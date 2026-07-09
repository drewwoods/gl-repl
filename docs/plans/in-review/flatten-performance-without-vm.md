# Flatten Performance Without a Control-Flow VM

## Summary

Reduce animated-frame flatten cost without replacing the source/flat-command
model or introducing a control-flow VM. The work has two complementary parts:

1. compile source expressions once and evaluate the compiled expression
   programs during flatten/rebake, removing repeated command parsing and string
   processing;
2. when changed variables cannot affect loop counts, selected `if` arms, or
   function-parameter snapshots, keep the existing flat topology and rebake
   only its values in place.

The executor, replay PC model, provenance, flat-command queries, autonormal,
guides, PLY export, and the `MAX_COMMANDS` flat buffer remain unchanged. A full
flatten remains the correctness fallback for edits, cache misses/overflow, and
changes that can affect topology. “Topology” includes flat-stream cardinality:
a `t`-dependent condition inside an unrolled loop can add or remove commands on
every frame.

This plan supersedes **Phase A and Phase A.5** of
`docs/plans/not-started/rethinking-flattening-behaviour.md`. Its VM phase remains
separate future work and is explicitly out of scope here.

## Measured baseline and conclusions

Measurements were taken on `drew-macbook-air` (Darwin arm64), release `-O2`, GL
stubs, with predef values restored between samples. They are same-machine
comparison points, not portable thresholds.

Existing `make bench` results:

| Workload | Flat commands | Mean full flatten |
|---|---:|---:|
| Fixed wave fixture (`flatten_examples`) | 3579 | 10.37 ms |
| Grass, selected by current largest-flat spike | 4063 | 4.98–5.43 ms |

A temporary 16-sample corpus sweep found the slowest current built-ins:

| Built-in example | Flat commands | Mean full flatten |
|---|---:|---:|
| Orrery (labels track 3D orbits) | 3860 | 6.36 ms |
| Animated wave surface | 3639 | 6.20 ms |
| Ringed planet | 4003 | 5.40 ms |
| Dusk lighthouse atoll | 3739 | 4.98 ms |
| Swaying grass field | 4063 | 4.92 ms |

The current `spike_flatten_largest` benchmark chooses by flat-command count,
not elapsed cost, so it selected Grass and missed the more expensive Orrery.

The existing `PROF_FLATTEN_*` probes show where the time goes:

| Workload | Reparse + GL arg eval | Var assignment eval | Structural remainder |
|---|---:|---:|---:|
| Grass | 2.35 ms (48%) | 2.41 ms (49%) | 0.15 ms (3%) |
| Orrery | 4.28 ms (77%) | 1.13 ms (20%) | 0.12 ms (2%) |

Therefore:

- block scans, recursion, function-definition lookup, and loop mechanics are
  not the first optimization target;
- an in-place topology shortcut alone is insufficient because the existing
  reparse/assignment kernels still dominate;
- a one-bit rule that treats every runtime-valued call argument as dynamic
  would reject Grass: `blade(p, x, z, ...)` uses predefs, but those call values
  depend on deterministic `rand(p, slot)` and constants, not on `t`;
- `rand`/`rand2` are deterministic pure functions of their arguments and do
  not independently make topology dynamic.

## Goals and non-goals

Goals:

- materially reduce steady-state flatten CPU for Grass, Orrery, Wave, Ringed
  planet, and the rest of the corpus;
- preserve current results and side effects exactly, including sequential
  assignments such as `n = n + 1`, scratch writes, provenance, replay scopes,
  and blur subframe isolation;
- keep one full-flatten implementation as the fallback and reference result;
- add stable real-scene and corpus benchmarks before optimizing.

Non-goals:

- no source-level control-flow VM and no direct source-to-GL executor;
- no migration of flat-array consumers or replay PCs;
- no change to assignment timing or language semantics;
- no increase to `MAX_COMMANDS` and no change to overflow behavior;
- no persistent/cache data in undo snapshots, user scenes, or workspace files.

## Phase 0 — Make the benchmark representative

Extend `bench/bench_repl.c` before changing runtime code:

1. Keep the fixed wave fixture as a stable synthetic workload.
2. Add named real-scene cases for exactly:
   - `Swaying grass field (rand + t)`;
   - `Orrery (labels track 3D orbits)`.
   Resolve by exact display name at startup and fail the sub-benchmark clearly
   if a case disappears; do not hard-code catalog indices.
3. Add `flatten_corpus`, which loads every built-in, warms once, restores the
   post-load predef/scratch baseline before each sample, and times repeated full
   flattens. Human output prints all cases sorted by mean time; CSV emits one
   row per catalog index plus the display name as a comment line so existing
   CSV columns remain compatible.
4. Add phase rows for full flatten: total, reparse, scalar assignment, scratch
   assignment, and derived remainder. Use the existing profiler sections; do
   not add timers inside evaluator primitives.
5. After later phases exist, add warm-cache full-flatten and rebake rows for
   Grass and Orrery. Report cold cache construction separately so an edit-time
   cost cannot disappear into the steady-state number.
6. Add a Whale dynamic-topology case sampled at several `t` values. Record the
   flat count with each timing and retain at least two samples with different
   counts. This case measures warm compiled **full flatten**, never rebake.

The benchmark must restore values before every inner iteration, consume result
counts/status, use release `-O2`, and retain `USE_GL_STUBS=1` support. Default
`make bench` remains non-gating; saved before/after CSV from the same machine is
the review artifact.

## Phase 1 — Remove needless parsing for literal commands

In `flatten_reparse_line`, append the already-committed `src_cmd` directly when
`src_cmd->has_vars == 0`, even inside a loop/function scope. Still snapshot the
current locals for replay annotations, but do not invoke the line parser. The
source command array is replaced transactionally on every successful edit, so
its literal args and payload are already current.

Retain the text-parser fallback for commands marked `has_vars`. Add a
differential test over the full corpus proving that the fast path produces the
same flat commands, local snapshots, provenance, lighting result, status, and
post-flatten variable state as the old always-reparse path.

Ship this independently if it improves at least one named/corpus case and
causes no benchmark regression over 5%. Do not bundle speculative block-end or
function-signature caches: the measured structural remainder is too small to
justify them unless new phase data changes that conclusion.

## Phase 2 — Compile expressions once

### Representation and ownership

Add `src/repl/expr_program.c/.h`, an expression-only compiler/evaluator. This
is not a control-flow VM: flatten still owns loops, calls, branches,
assignments, output materialization, and resource budgets.

Use postfix instructions so evaluation is a single forward walk with the same
operator order as the current recursive-descent evaluator. Instructions cover:

- literal pushes;
- predef-slot pushes;
- local-name pushes;
- scratch reads;
- unary/binary/comparison/logical operators;
- calls into the existing builtin table (`sin`, `cos`, `rand`, `rand2`, etc.).

Each program records instruction range, maximum stack depth, and its source
line/role for diagnostics. Keep identifier names in one cache symbol arena;
instructions refer to symbol indices rather than embedding names. Predef slots
may be resolved at compile time because declare/undeclare invalidates the cache;
locals remain name-resolved against `FlatCmdLocalVars`/the active flatten
bindings.

The cache is ephemeral REPL-owned state, separate from `ReplRuntimeState` so it
is never copied into undo/redo, scene snapshots, or persistence. It owns
growable heap arenas for line entries, program descriptors, instructions, and
symbols. Cap total live cache allocation at 16 MiB; allocation/cap/compile
failure marks only that line uncached and uses the current text evaluator.
There must be no allocation on a warm full flatten or rebake.

Invalidate the live cache from the single source-mutation seam,
`repl_state_mark_source_dirty`. Rebuild lazily per source line on the next full
flatten. Undo/redo, scene/workspace/example loads, autonormal source rewrites,
and declaration-table reshapes already pass through that seam and therefore do
not need special cache logic.

### Do not duplicate the grammar

Refactor the current expression-evaluation boundaries to optionally capture
the exact expression span while they parse it:

- numeric GL/function argument lists;
- `for` start/end/step;
- `if`/`else if` conditions;
- function-call argument lists;
- scalar assignment RHS;
- scratch index and RHS.

Use one neutral capture contract shared by the parser/header/assignment
helpers:

```c
typedef int (*ReplExprCaptureFn)(void *user_data,
                                 ReplExprRole role, int ordinal,
                                 const char *begin, const char *end);

typedef struct {
    ReplExprCaptureFn fn;
    void *user_data;
} ReplExprCaptureSink;
```

`role` distinguishes command arg, loop start/end/step, condition, call arg,
assignment RHS, and scratch index/RHS; `ordinal` identifies repeated command or
call args. A null sink is today's behavior. The cache compiles the span during
the callback, so the pointers never outlive the source/helper buffer.

The same helpers must feed both text evaluation and expression-program
compilation. Do not build a second command parser in `expr_program.c`. Custom
commands (material arrays, point-parameter arrays, labels) either expose their
numeric spans through the same capture hook or remain line-local text fallbacks
until supported.

Add an optional expression-cache view to `ReplFlattenOptions`. A null/missing
entry preserves the current text path, keeping standalone tests/tools and
temporary-buffer callers source-compatible after initializing the new field to
zero. The live `repl_flatten_commands` wrapper supplies the live cache.

### Evaluation parity

The compiled evaluator returns both the float and a dependency mask:

```c
typedef unsigned int ReplExprDepMask; /* MAX_PREDEF_VARS <= 32 */

typedef struct {
    float value;
    ReplExprDepMask deps;
} ReplExprValue;
```

Constants have mask zero; a predef read contributes its current dependency
mask; a local read contributes the local binding's mask; scratch reads are
handled conservatively as described in Phase 3; operators and builtins union
their operand masks. `rand`/`rand2` add no dependency beyond their arguments.

Preserve evaluation order and float operations so cached and text evaluation
are bit-identical (allow both values to be NaN as the NaN parity case). On any
unsupported or invalid compiled program, evaluate the original text instead.

## Phase 3 — Dependency-aware in-place rebake

### Why dependency masks are required

The fast-path decision is about the variables that actually changed, not
whether a structural expression mentions any variable. During a full flatten,
propagate one bit per predef root through assignments and local bindings:

- initialize each predef's dependency to its own slot bit;
- scalar assignment replaces the destination dependency with the RHS union
  (`x=x+1` therefore retains `x`);
- a loop header unions start/end/step dependencies into the program's
  `structural_dep_mask`; its iterator local conservatively carries that union;
- an `if`/`else if` condition contributes to `structural_dep_mask`;
- function call args contribute to `structural_dep_mask` because their values
  are frozen into per-flat-command local snapshots; parameters inherit the
  respective arg masks inside the function;
- every emitted/assigned value expression contributes to `value_dep_mask`.

For v1, any scratch-array read in a loop header, condition, or call argument
sets `structural_dep_mask` to all bits. Scratch writes/reads in value-only
positions remain rebakeable. This is deliberately conservative until scratch
cells have persistent dependency metadata; it does not block Grass or Orrery.
Missing text, an uncached structural expression, dependency overflow, or any
structural-analysis uncertainty sets `structural_dep_mask` to all bits. The
equivalent uncertainty on a value-only line sets `value_dep_mask` to all bits.

This propagation makes the target cases safe:

- Grass call args depend transitively on `bladeCount`/`field` and the outer
  iterator, but not on `t`; changing `t` can reuse its function snapshots.
- Orrery's call args are constants while `t` is consumed inside function
  bodies; changing `t` can reuse its call topology/snapshots.
- `n=t*2; for(i,0,n)` and `x=t; func0(x)` include `t` in
  `structural_dep_mask` and retain full flattening.
- Whale's droplet `if((t-spawnDelay)>0 && ...)` contributes `t` to
  `structural_dep_mask`; as particles enter/leave the active interval, the
  emitted `glVertex3f` count changes. Every `t` update therefore performs a
  full flatten (accelerated by the expression cache), never an in-place
  rebake. Do not pad the stream with inactive commands: that would change
  replay PCs, flat-cost attribution, and current flat-program semantics.

### Dirty state and routing

Extend `ReplFlatProgramState` with:

- `structural_dep_mask` — roots that can change topology or frozen locals;
- `value_dep_mask` — roots used by any flattened value/assignment;
- `args_dirty_mask` — accumulated roots changed since the last refresh.

Full dirty always subsumes/clears args dirty. Source edits and
declare/undeclare remain full-dirty. For a value change with mask `changed`:

```text
changed & structural_dep_mask != 0  -> full dirty
changed & value_dep_mask != 0       -> OR into args_dirty_mask
otherwise                           -> no flat work
```

Route `repl_state_time_advance`/`_time_set` through the `t` slot bit. Route
predef `SET_VALUE` through the changed slot bit; declaration-table reshaping
never uses the bit fast path. Multiple value changes accumulate, and a later
structural change escalates to full dirty.

### Rebake API and behavior

Add a reusable API plus a live-state wrapper:

```c
int repl_flatten_rebake_program(const ReplRebakeOptions *options,
                                ReplRebakeResult *result);
int repl_flatten_rebake_commands(void);
```

`ReplRebakeOptions` contains mutable flat commands, const local snapshots,
flat count, source text, source commands, and the expression-cache view. It
does not accept or mutate topology/provenance.

Walk the existing flat array in order:

- scalar assignment: evaluate compiled RHS with that slot's local snapshot,
  update the live predef and baked `args[0]`;
- scratch assignment: evaluate index/RHS, apply the write, update baked args;
- other `has_vars` command: evaluate its compiled numeric expression bindings
  directly into the existing `args[]`/numeric payload;
- command without variables: skip;
- never change type, count, provenance, flags, local snapshots, cursor block,
  or lighting classification.

Sequential order is load-bearing: rebake must reproduce full flatten's
assignment threading and must not evaluate assignments a second time in the
executor. An out-of-range scratch index, missing required line/cache data, or
evaluation failure returns failure; the live wrapper restores the pre-rebake
predef/scratch baseline and performs one full flatten.

Add `PROF_REBAKE` as a top-level CPU section. In the normal frame gate: full
dirty wins, else args dirty rebakes, else no work. For time-blur subframes,
reset the existing per-sample predef/scratch baseline, then choose full flatten
or rebake using the `t` bit for every sample. Leave the final sample baked at
the frame's true `t`, as today.

## Public/internal interface changes

- New REPL-internal expression program/cache API in `expr_program.h`.
- `ReplFlattenOptions` gains an optional cache view.
- `ReplFlattenResult` gains `structural_dep_mask` and `value_dep_mask` so
  temporary-buffer callers can inspect the same analysis.
- `ReplFlatProgramState` gains the two dependency masks and
  `args_dirty_mask`, exposed through the existing views/owners split.
- `state_notify.h` gains value-change notification by predef slot; existing
  full/source dirty entry points remain.
- `flatten.h` gains rebake options/result and the pure/live entry points.
- Profiling gains `PROF_REBAKE`; no UI-visible configuration or file-format
  change is introduced.

All additions remain C99 and use the project `STATIC_ASSERT` shim to enforce
`MAX_PREDEF_VARS <= sizeof(ReplExprDepMask) * CHAR_BIT`.

## Correctness tests

1. **Expression differential:** compile every captured expression in the
   example corpus plus evaluator unit cases; compare compiled vs text values at
   multiple `t`, predef, local, and scratch bindings. Cover precedence, unary
   ops, comparisons/logicals, every builtin, C-to-REPL translations, NaN, and
   deterministic `rand`/`rand2`.
2. **Full-flatten differential:** run every example with cache disabled and
   enabled at multiple times. Compare result/status, flat count, every command
   field, local snapshots, provenance, lighting, and post-flatten
   predef/scratch state.
3. **Dependency masks:** cover direct/transitive `t` in loops, branches, and
   calls; self-referential assignments; nested constant loops; deterministic
   rand; the Grass assignment/call pattern; Orrery's constant call args with
   `t` inside bodies; Whale's per-droplet `t` condition; and conservative
   structural scratch fallback.
4. **Rebake differential:** for every corpus example and each changed predef
   bit not intersecting its structural mask, compare rebake with a full flatten
   from the identical baseline. Compare all fields and runtime state, not only
   rendered args.
5. **Dirty routing:** value-only change sets args dirty, structural change
   escalates to full, unused value schedules nothing, edit/declare/undeclare
   force full, and full flatten clears both masks.
6. **Failure rollback:** cache allocation/compile miss, parse fallback,
   scratch-range failure, and rebake failure restore the baseline before full
   fallback; no partially rebaked stream becomes visible.
7. **Consumer regressions:** replay seek/fade/annotations, flat-cost queries,
   current-block highlight, guides/edit overlays, autonormal, and PLY export
   behave identically after rebake.
8. **Blur:** 2/4/8/12/16 time-blur samples match forced-full-flatten output and
   do not compound assignment/scratch state between samples.
9. **End-to-end pixels:** OSMesa captures of Grass and Orrery at two distinct
   times must (a) differ across time and (b) match forced-full-flatten captures
   at the same time.
10. **Variable-length canary:** full-flatten Whale across a time grid spanning
    particle spawn/expiry boundaries; assert at least two distinct flat counts,
    exact command/provenance parity with cache disabled, and that the `t` dirty
    route always selects full flatten.

## Performance acceptance and rollout

Record Phase 0 CSV before each phase and compare on the same idle machine.

- Phase 1 ships only with no named/corpus regression over 5%.
- Warm compiled full flatten must improve both Grass and Orrery by at least 25%
  before Phase 2 is considered successful.
- Combined warm cache + rebake must improve steady-state Grass and Orrery by at
  least 40% versus the original baseline; target means are below 3.0 ms for
  Grass and below 3.8 ms for Orrery on the baseline machine.
- The top-five corpus cases must not regress by more than 5%; report median and
  slowest-case ratios as well as named cases.
- Cold cache build + first full flatten should remain below 12 ms for every
  built-in on the baseline machine. Cache memory must stay below the 16 MiB
  cap and perform zero warm-path allocations.
- In the live detailed profile, stable-topology animation should show
  `Flatten` only after edits/structural value changes and `Rebake` during
  steady animation; forced-full and optimized frame outputs must match.

Land in three reviewable changes (benchmark/literal fast path, expression
cache, dependency masks/rebake). Run `make test`, `make test-stubs`,
`make check-state-ownership`, `make check-c99`, and the benchmark suite after
each. Because this touches C99 portability, state ownership, and build inputs,
finish with the documented real-GCC `check-c99` + `test-stubs` verification on
gracemont.
