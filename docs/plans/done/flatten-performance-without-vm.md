# Flatten Performance Without a Control-Flow VM

## Status - DONE (2026-07-17)

Phases 0-3 landed on `main`: representative benchmarks, direct evaluation,
the slider transaction split, compiled-expression programs, dependency-aware
dirty routing, and in-place rebaking. Phase 3 was initially deferred because a
single-refresh benchmark showed only a modest gain. Subsequent Accum Blur
testing changed that conclusion: time blur refreshes at every accumulation
sample, making rebaking the practical enabler for most examples-especially in
the Emscripten build. The completed implementation keeps full flattening as
the correctness fallback for structurally dynamic programs.

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
guides, PLY export, and the `MAX_FLAT_COMMANDS` flat buffer remain unchanged. A
full flatten remains the correctness fallback for edits, cache misses/overflow,
and changes that can affect topology. “Topology” includes flat-stream
cardinality: a `t`-dependent condition inside an unrolled loop can add or remove
commands on every frame.

This plan supersedes **Phase A and Phase A.5** of
`docs/plans/not-started/rethinking-flattening-behaviour.md`. Its VM phase remains
separate future work and is explicitly out of scope here.

## Landing status (2026-07-17)

Phase 3 was developed and verified on `codex/improve-flatten-phase3`, then
landed on `main`. The earlier recommendation considered the roughly 0.2 ms
saving for one eligible refresh and undervalued the optimization. Accum Blur
performs 2/4/8/12/16 refreshes per displayed frame, so that saving compounds
with the sample count; Emscripten makes the avoided full-flatten work more
significant again. Structurally dynamic examples still require the full path,
but for most stable-topology examples Phase 3 is necessary to make time blur
practical.

## Implementation task list (2026-07-10)

- [x] Preserve the pre-rewrite Phase 3 tip on a backup branch.
- [x] Review Phase 3a/3b for routing, rollback, profiling, and cache-lifetime
  bugs.
- [x] Insert Phase 1C immediately after Phase 1 and replay Phases 2/3 over it.
- [x] Put compiled-cache mechanics and dependency propagation behind the
  internal `flatten_expr.c/.h` boundary.
- [x] Put full-vs-rebake selection, fallback, and profiling behind
  `repl_refresh_flat_program*()`.
- [x] Reject stale-cache and negative-count rebakes; test a true mid-walk
  rollback before full fallback.
- [x] Extend `make bench` with production refresh rows for Grass, Orrery, and
  Wave, including the selected route.
- [x] Document the expression-cache lifecycle, its narrow integration
  boundary, and all supported cache-disable/reference modes in
  `src/repl/ARCHITECTURE.md`.
- [x] Complete final verification of the cleaned Phase 2 landing branch.
- [x] Run the full sanitizer/stub/C99 verification matrix and capture final
  same-machine benchmark numbers.
- [x] Split Phase 3 runtime, benchmarks, tests, and documentation onto a
  branch based on the cleaned Phase 2 landing history.
- [x] Reassess Phase 3 against full Accum Blur frame cost and land it on
  `main`.

## Measured baseline and conclusions

Measurements were taken on `drew-macbook-air` (Darwin arm64), release `-O2`, GL
stubs, with predef values restored between samples. They are same-machine
comparison points, not portable thresholds.

Existing `make bench` results:

| Workload | Flat commands | Mean full flatten |
|---|---:|---:|
| Fixed wave fixture (`flatten_examples`) | 3579 | 10.37 ms |
| Grass, selected by current largest-flat spike | 4063 | 4.98-5.43 ms |

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

### Results after Phases 1C-3

Phase 1C supplies an intentionally cache-free comparison point. On the same
machine, its always-full direct evaluator reduced Grass from roughly 4.5 ms to
2.06 ms and Orrery from roughly 5.2 ms to 2.63 ms. Phase 2's warm compiled full
flatten then reduced the current scenes to roughly 0.63-0.67 ms for Grass and
0.55-0.62 ms for Orrery. These figures vary with host load; use the committed
bench rows, not the prose, for future comparisons.

`flatten_refresh` now measures the production route rather than presuming a
scene is rebakeable. A 15-iteration release/stub run measured:

| Scene | Production route for `t` | Mean refresh |
|---|---|---:|
| Grass | rebake | 0.487 ms |
| Orrery | full flatten | 0.578 ms |
| Wave | rebake | 0.303 ms |

Orrery legitimately remains structural: its live Gregorian date readout
threads `t` through `th`, `yr`, `leap`, and `doy` into a month-selecting
`if`/`else-if` chain. The selected source arm changes over time. An earlier
version of this plan described Orrery's function-call arguments but overlooked
that later branch. Whale likewise remains full-flatten because droplet
conditions change command count.

Phase 1C and Phase 2 still deliver most of the improvement to one full flatten.
Phase 3 is scene-dependent and cannot remove full flattening when source
topology changes, but describing it as a small ordinary-frame optimization is
misleading. Its saving is incurred once per time-blur sample, so it is a
practical requirement for multi-sample Accum Blur on most eligible examples
and has outsized value in the Emscripten build. It also remains useful for
value-only slider changes.

### Final verification

The rebased landing tip passed:

- `make check-c99`;
- the full stub suite: 61/61 binaries and 19,146/19,146 assertions;
- Phase 3 dependency, rebake, and optimized-versus-forced-full differential
  suites;
- documentation link/example and pre-push boundary checks.

Manual native and Emscripten testing established the load-bearing performance
result that the single-refresh benchmark missed: Accum Blur repeats the
refresh cost for every sample, and rebaking makes the effect practical for
most stable-topology examples.

The pre-split implementation tree was also checked at
`1918dc1e`:

- macOS stub pre-push suite: 61/61 binaries and 17,784/17,784 assertions;
- macOS ASan+UBSan suite at the preceding code boundary, followed by the final
  348/348 rebake corpus under ASan+UBSan after the constant-assignment fix;
- differential reference: 1,987/1,987 assertions, comparing the optimized
  paths with forced general-parser flattening;
- cache-disabled production refresh benchmark with
  `GLR_NO_FLATTEN_CACHE=1`: Grass, Orrery, and Wave all selected full flatten
  and completed successfully;
- `make check-c99` on macOS and on Ubuntu 24.04 with GCC 13;
- Linux stub suite: 61/61 binaries and 17,784/17,784 assertions;
- documentation links: 25 files and 1,641 local file/line links.

The Linux verification used a detached temporary worktree, leaving the
developer checkout's unrelated modified `packaging/web/shell.html` untouched.

### Phase 2 split verification

The cleaned always-full-flatten branch passed:

- `make check-c99`;
- the full stub suite: 59/59 binaries and 17,426/17,426 assertions;
- the optimized-versus-forced-parser differential under ASan+UBSan:
  1,987/1,987 assertions;
- cache-enabled and `GLR_NO_FLATTEN_CACHE=1` Grass/Orrery benchmark runs;
- documentation links: 25 files and 1,636 local file/line links.

## Goals and non-goals

Goals:

- materially reduce steady-state flatten CPU for Grass, Orrery, Wave, Ringed
  planet, and the rest of the corpus;
- keep variable-panel dragging on a warm cache and route each motion through
  value-dirty/full-dirty dependency masks instead of source invalidation;
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

## Phase 0 - Make the benchmark representative

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
5. After later phases exist, add warm-cache full-flatten and production-refresh
   rows for Grass and Orrery; do not assume the refresh is a rebake. Add Wave
   as a value-only real-scene control. Report the selected route and cold cache
   construction separately so an edit-time cost cannot disappear into the
   steady-state number.
6. Add a Whale dynamic-topology case sampled at several `t` values. Record the
   flat count with each timing and retain at least two samples with different
   counts. This case measures warm compiled **full flatten**, never rebake.
7. Add two slider-transaction cases: a value-only variable that should rebake
   on every motion and a structural variable that should warm-full-flatten.
   Time a 100-motion drag separately from the single release-time persistence
   edit; assert zero cache rebuilds during motion and exactly one on release.

The benchmark must restore values before every inner iteration, consume result
counts/status, use release `-O2`, and retain `USE_GL_STUBS=1` support. Default
`make bench` remains non-gating; saved before/after CSV from the same machine is
the review artifact.

## Phase 1 - Cheap fast paths and slider transaction split

### 1A. Remove needless parsing for literal commands

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

### 1B. Keep slider motion value-only

The variable-panel path currently calls `repl_compile_set_predef_value` for
every pointer-motion event. For a declared variable that produces both a
`SET_VALUE` op and `REPL_COMPILED_REPLACE_ONE` for the declaration initializer;
the replacement calls `repl_state_mark_source_dirty`. If left unchanged, Phase
2 would invalidate/rebuild the entire expression cache on every drag event and
Phase 3's value-change routing would be bypassed.

Split the drag transaction deliberately:

1. **Drag begin:** capture name/start value as today. Extend
   `VariablePanelDragState` with `value_changed` and `final_value`; initialize
   them to false/start value. Keep `undo_snapshot_pushed` as the independent
   undo-coalescing flag.
2. **Each motion:** compile/apply a **live-only** change containing
   `REPL_COMPILED_NO_CHANGE + REPL_PREDEF_OP_SET_VALUE`; never rewrite editor
   text or the command store. Capture the undo snapshot on the first successful
   motion exactly as today, notify tutorial REQUIRE_VAR through the existing
   external-change tail, and record the applied final value on the peer-owned
   drag state.
3. **Mouse release:** if `value_changed`, compile/apply one **source-only**
   persistence change. If the variable has a declaration, rewrite its
   initializer with `REPL_COMPILED_REPLACE_ONE` but emit no predef op (the live
   value is already final); if it has no declaration, return
   `REPL_COMPILED_NO_CHANGE`. Do not capture another undo snapshot and do not
   send a second tutorial variable notification. Then reset drag state.
4. **No-motion release:** perform no compile/apply work and create no undo
   entry.

Add two compile entry points rather than assembling changes in the controller:

```c
ReplCompileResult repl_compile_set_predef_value_live(...);    /* SET_VALUE only */
ReplCompileResult repl_compile_persist_predef_value(...);     /* source only */
```

Keep `repl_compile_set_predef_value` as the existing combined semantic for any
non-drag caller; implement the three entry points over shared lookup/rewrite
kernels so formatting cannot drift. Add a peer-owned
`variable_panel_drag_note_applied_value(float)` helper instead of mutating drag
fields from the app router.

The declaration shown in the code panel intentionally remains at its drag-start
text until mouse-up; the variable panel and rendered scene use the live value
throughout. On release, the one source replacement invalidates the cache and
forces one cold full flatten, preserving saved-source behavior. If release-time
persistence unexpectedly fails, report the error, retain the already-applied
live value (Undo still restores the drag-start snapshot), and end the drag.

This plan deliberately keeps whole-cache invalidation at the source-dirty seam.
Per-line cache invalidation is not needed to make slider motion warm and would
add cache-index shifting/generation complexity for insert/delete operations.

### 1C. Evaluate known command shapes directly

The source command already records the parsed `CmdType`, argument count, and
payload. For variable-bearing standard numeric commands, use that committed
shape to extract and evaluate only the argument list; do not redispatch,
revalidate, or canonicalize the entire line on each expanded instance. Preserve
command-specific postprocessing such as the dynamic `glClearColor` clamp, and
fall back to the general parser whenever the direct shape check misses.

Likewise, a committed scalar assignment's editor/import text is already
canonical REPL syntax. Extract its RHS with the existing assignment helper and
evaluate it directly instead of defensively translating it through C syntax on
every visit. `force_reparse` retains the old path as the differential reference.

This remains an always-full-flatten design: loops, calls, conditions, output
materialization, provenance, and local snapshots are rebuilt normally. It is
the understandable no-cache baseline against which Phase 2 must justify its
additional machinery.

## Phase 2 - Compile expressions once

### Representation and ownership

Add `src/repl/expr_program.c/.h`, an expression-only compiler/evaluator. This
is not a control-flow VM: flatten still owns loops, calls, branches,
assignments, output materialization, and resource budgets.

Keep its integration with flatten behind `src/repl/flatten_expr.c/.h`. That
internal engine owns cache-line build state, capture callbacks, warm program
evaluation, dependency accumulation, and compiled-only rebake lookups.
`flatten.c` asks for values by `(line, role, ordinal)` and never manipulates a
cache entry or expression-program handle. The cache-free/direct path therefore
remains readable and independently testable.

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
not need special cache logic. Variable-panel motion is no longer a source
mutation (Phase 1B); only its one mouse-up persistence edit invalidates the
cache.

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

## Phase 3 - Dependency-aware in-place rebake

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
cells have persistent dependency metadata; it does not block Grass (Orrery is
already structural for the independent date-branch reason below).
Missing text, an uncached structural expression, dependency overflow, or any
structural-analysis uncertainty sets `structural_dep_mask` to all bits. The
equivalent uncertainty on a value-only line sets `value_dep_mask` to all bits.

This propagation makes the following cases safe:

- Grass call args depend transitively on `bladeCount`/`field` and the outer
  iterator, but not on `t`; changing `t` can reuse its function snapshots.
- Orrery's planetary call args are constants, but the later Gregorian-date
  `if` chain depends transitively on `t`; the current scene therefore remains
  structural and full-flattens on time changes.
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

- `structural_dep_mask` - roots that can change topology or frozen locals;
- `value_dep_mask` - roots used by any flattened value/assignment;
- `args_dirty_mask` - accumulated roots changed since the last refresh.

Full dirty always subsumes/clears args dirty. Source edits and
declare/undeclare remain full-dirty. For a value change with mask `changed`:

```text
changed & structural_dep_mask != 0  -> full dirty
changed & value_dep_mask != 0       -> OR into args_dirty_mask
otherwise                           -> no flat work
```

Route `repl_state_time_advance`/`_time_set` through the `t` slot bit. Route
predef `SET_VALUE`-including every variable-panel motion-through the changed
slot bit; declaration-table reshaping never uses the bit fast path. Multiple
value changes accumulate, and a later structural change escalates to full
dirty. The mouse-up declaration rewrite is a real source edit and intentionally
performs one cache invalidation/full flatten after the drag.

### Rebake API and behavior

Keep the reusable buffer-level API, but hide live-state rebaking behind one
refresh boundary:

```c
int repl_flatten_rebake_program(const ReplRebakeOptions *options,
                                ReplRebakeResult *result);
ReplFlatRefreshKind repl_refresh_flat_program(int edit_line_idx);
ReplFlatRefreshKind repl_refresh_flat_program_for_deps(
    int edit_line_idx, ReplExprDepMask changed_deps);
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
evaluation failure returns failure. The private live rebake restores the
pre-rebake predef/scratch baseline; the public refresh boundary immediately
performs one full flatten, overwriting any command arguments changed earlier in
the failed walk before control returns to a consumer.

Add `PROF_REBAKE` as a top-level CPU section. The refresh boundary owns both
`PROF_REBAKE` and `PROF_FLATTEN`, so callers cannot double-nest a profiler
section. The frame controller, exports, debug dumps, benchmark drain, replay
freshness helper, and time-blur samples request freshness without duplicating
the routing policy. For time-blur subframes, reset the existing per-sample
predef/scratch baseline, then pass the `t` bit to the transient refresh entry.
Leave the final sample baked at the frame's true `t`, as today.

## Public/internal interface changes

- New REPL-internal expression program/cache API in `expr_program.h`, consumed
  by flatten only through `flatten_expr.c/.h`.
- `compile.h` gains separate live-only and persistence-only predef-value
  compile entry points; the variable-panel router uses one during motion and
  the other on release.
- `VariablePanelDragState` gains `value_changed`/`final_value` plus a peer-owned
  applied-value notifier.
- `ReplFlattenOptions` gains an optional cache view.
- `ReplFlattenResult` gains `structural_dep_mask` and `value_dep_mask` so
  temporary-buffer callers can inspect the same analysis.
- `ReplFlatProgramState` gains the two dependency masks and
  `args_dirty_mask`, exposed through the existing views/owners split.
- `state_notify.h` gains value-change notification by predef slot; existing
  full/source dirty entry points remain.
- `flatten.h` gains rebake options/result and the pure buffer-level entry
  point; `pipeline.h` exposes the one live refresh boundary.
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
   rand; the Grass assignment/call pattern; Orrery's constant planetary call
   args plus its `t`-dependent date branch; Whale's per-droplet `t` condition;
   and conservative structural scratch fallback.
4. **Rebake differential:** for every corpus example and each changed predef
   bit not intersecting its structural mask, compare rebake with a full flatten
   from the identical baseline. Compare all fields and runtime state, not only
   rendered args.
5. **Dirty routing:** value-only change sets args dirty, structural change
   escalates to full, unused value schedules nothing, edit/declare/undeclare
   force full, and full flatten clears both masks.
6. **Slider transaction:** repeated motion leaves declaration text and the
   expression-cache generation unchanged, updates live rendering, sends the
   changed slot through dependency routing, and captures one undo snapshot.
   Release rewrites an initialized or uninitialized declaration once, performs
   one cache invalidation/full dirty, and creates no second tutorial notify;
   no-declaration and no-motion releases are no-ops. Undo/redo restore both
   source and value across the whole drag.
7. **Failure rollback:** cache allocation/compile miss, parse fallback,
   scratch-range failure, and rebake failure restore the baseline before full
   fallback; no partially rebaked stream becomes visible.
8. **Consumer regressions:** replay seek/fade/annotations, flat-cost queries,
   current-block highlight, guides/edit overlays, autonormal, and PLY export
   behave identically after rebake.
9. **Blur:** 2/4/8/12/16 time-blur samples match forced-full-flatten output and
   do not compound assignment/scratch state between samples.
10. **End-to-end pixels:** OSMesa captures of Grass and Orrery at two distinct
   times must (a) differ across time and (b) match forced-full-flatten captures
   at the same time.
11. **Variable-length canary:** full-flatten Whale across a time grid spanning
    particle spawn/expiry boundaries; assert at least two distinct flat counts,
    exact command/provenance parity with cache disabled, and that the `t` dirty
    route always selects full flatten.

## Performance acceptance and rollout

Record Phase 0 CSV before each phase and compare on the same idle machine.

- Phase 1 ships only with no named/corpus regression over 5%.
- Warm compiled full flatten must improve both Grass and Orrery by at least 25%
  before Phase 2 is considered successful.
- Combined warm cache + production refresh must improve steady-state Grass and
  Orrery by at least 40% versus the original baseline. Orrery is expected to
  use the warm full path because its date branch is structural; the criterion
  is performance, not forcing an unsafe rebake route.
- The top-five corpus cases must not regress by more than 5%; report median and
  slowest-case ratios as well as named cases.
- Cold cache build + first full flatten should remain below 12 ms for every
  built-in on the baseline machine. Cache memory must stay below the 16 MiB
  cap and perform zero warm-path allocations.
- A 100-event slider drag must perform zero source/cache invalidations during
  motion, exactly one on release when a declaration exists, and no cold-cache
  work until release. Motion cost is compared separately for value-only rebake
  and structural warm-full-flatten cases.
- In the live detailed profile, stable-topology animation should show
  `Flatten` only after edits/structural value changes and `Rebake` during
  steady animation. Structurally animated scenes such as current Orrery and
  Whale continue to show `Flatten`; forced-full and optimized outputs must
  match.

Land in three reviewable changes (benchmark + Phase 1 fast paths/slider split,
expression cache, dependency masks/rebake). Run `make test`, `make test-stubs`,
`make check-state-ownership`, `make check-c99`, and the benchmark suite after
each. Because this touches C99 portability, state ownership, and build inputs,
finish with the documented real-GCC `check-c99` + `test-stubs` verification on
gracemont.
