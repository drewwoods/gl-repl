# Rethinking Flattening Behaviour

> **Update:** Phase A and Phase A.5 here are superseded by the measured,
> dependency-aware non-VM plan in
> [`../done/flatten-performance-without-vm.md`](../done/flatten-performance-without-vm.md).
> In particular, the one-bit classifier below would conservatively reject the
> Swaying Grass workload, and measurements show expression/assignment parsing
> dominates structural traversal. Phase B remains the separate long-range VM
> proposal.

Planned future work, in two phases. **Phase A** removes the per-frame
re-flatten for the common animation case (a structure-stability classifier
paired with an in-place arg re-bake pass). **Phase B** is the longer-range
control-flow-interpreting VM executor sketched in `ARCHITECTURE.md` §13.6,
staged so each flat-array consumer migrates independently. Neither phase is
started; this document is the design they should follow when picked up.

## Context

`ARCHITECTURE.md` §13 explains that the flat program is rebuilt every frame
whenever an animated `t` (or a slider change) touches the source — flatten +
assignment evaluation can run ~8 ms/frame for a large program, over half the
60 Hz budget. §13.5 sketches a "structure-stability" fast path and §13.6 a
control-flow-interpreting VM; this plan turns both sketches into designs.

**The finding that shaped Phase A (verified):** the executor performs **no
expression evaluation**. `src/repl/executor.c` consumes flatten-baked
`cmd->args[]` only — there is no `repl_eval_expr` call in the file, and the
comment at `executor.c:677-679` states it outright: "The flatten pass keeps
args[] in sync with current variable values for has_vars commands, so we use
them directly." The per-frame flatten **is** the animation mechanism.
(ARCHITECTURE.md previously claimed the executor re-evaluated `has_vars` args
each frame; §3.4, §5.3, §13.3–13.5 and the same stale claim in the
`executor.h` / `flatten.h` header comments have been corrected to match the
code, in the same commit that added this plan.)

Consequence: any scheme that skips the re-flatten must supply a replacement
arg re-bake, or animations silently freeze. That is why Phase A pairs the
classifier with a re-bake pass rather than merely suppressing the dirty flag.

## Verified current behavior (exploration snapshot, 2026-07)

Line numbers are anchors from the exploration that produced this plan; expect
drift.

- Dirty gates today: `repl_state_time_advance`/`_time_set`
  (`src/repl/state.c:340-369`) set `flat_program.dirty` gated on the lazy
  `source_uses_time` cache (`scan_source_uses_time` `state.c:122-148`,
  invalidated only in `repl_state_mark_source_dirty` `state.c:289-303`).
  Slider drags → `repl_apply_predef_ops` SET_VALUE (`src/repl/apply.c:160-167`)
  mark flat dirty **unconditionally** on any value change — no structure or
  even uses-this-var check.
- Controller flatten gate: `src/app/glr_ctrl.c:1701-1708` under `PROF_FLATTEN`.
  Blur time-mode reflattens **once per accumulation sample** (up to 16/frame):
  `glr_ctrl_setup_subframe` `glr_ctrl.c:~1597-1604`
  (`repl_state_time_set_transient` + `repl_flatten_commands`), after restoring
  baseline predef/scratch state.
- Flatten (`src/repl/flatten.c`): loops materially unrolled (`flatten_for_loop`
  :158-217), if-arms selected (`flatten_if_block` :335-372), funcN inlined with
  params frozen into per-flat-cmd `local_vars[]` snapshots (`flatten_call`
  :220-310, `flatten_append_cmd` :122-151). Assignments evaluated **at flatten
  time**, writing the live tables (`flatten_var_assign` :444-478,
  `flatten_scratch_assign` :483-543); the executor deliberately re-applies only
  the baked values (`executor.c:793-813`) to avoid double-applying `x = x + 1`.
- Expressions are text, re-parsed by recursive descent on every evaluation —
  no AST/bytecode cache anywhere. goto is already execute-time interpreted
  (`executor.c:754-790`, `REPL_GOTO_LOOP_LIMIT`); `replay_annotations.c:603-676`
  carries a second, independent symbolic if/goto walker — an accidental VM
  prototype, currently duplicated logic.
- The `local_vars[]` snapshots are read today only by replay annotations, not
  the executor. The rebake pass below becomes their second reader.
- Flat-array consumers that must keep working through any change: flatten_query
  cursor match / current-block highlight / cost histogram (+ `glr_debug.c`
  `--flat-histogram`), replay (PC = flat array index everywhere), fade batches,
  edit overlays + cursor guides, transform guides, autonormal (filters flat by
  `src_cmd_idx`), PLY export (executes flat under feedback), the executor
  itself (`ReplExecutionOptions.flat_cmd_count` replay narrowing).

---

## Phase A — Structure-stability gate + in-place arg re-bake

### Design

Two new lazily-cached properties plus one new pass:

1. **Structure classifier** (whole-program single bit, v1): the program's
   structure is *dynamic* iff any **structural expression** — a `for(...)`
   header, an `if` / `else if` condition, or a `funcN(...)` call-arg list —
   references any predef var, scratch array (`A`/`B`/`C`), or `rand`/`rand2`.
   Call args are structural because their values are frozen into `local_vars[]`
   snapshots that a rebake cannot refresh; `rand` in a structural position must
   classify dynamic because today's per-frame reflatten re-rolls it.

   No dataflow closure is needed for v1. By induction: a loop counter or func
   param can only vary frame-to-frame if its *binder* expression (an enclosing
   for-bound or a call arg — both themselves structural positions) references a
   predef/scratch/rand, which already sets the bit at the binder. So
   `n=t*2; for(i,0,n)`, `A[0]=t; for(i,0,A[0])`, `func1(t)`, and `if(t>1)` all
   correctly classify dynamic with zero analysis machinery, while
   `for(i,0,10){ for(j,0,i){ ... } }` with `glVertex3f(sin(t), i, j)` stays
   static. Conservative fallbacks everywhere (missing source text ⇒ dynamic).

   A per-variable bit vector with transitive closure through assignments is a
   possible v2 — it only adds value for programs mixing a structural slider
   var with a leaf-only `t`, and its scratch-array closure is exactly the kind
   of analysis that silently freezes an animation when wrong. Defer.

2. **New dirty level** `flat_program.args_dirty`: "structure unchanged; only
   baked args / assignment values are stale." Full-dirty subsumes it; a full
   flatten clears both.

3. **Rebake pass** `repl_flatten_rebake_*`: walk the **existing** flat array in
   program order:

   ```
   for each flat cmd i in [0, count):
       CMD_VAR_ASSIGN      → re-eval RHS from source line text (src_cmd_idx)
                             against local_vars[i] + live predefs;
                             write predef table + args[0].  Always, even when
                             has_vars == 0 (sequential state threading).
       CMD_SCRATCH_ASSIGN  → same via the scratch kernel; index out of range
                             → return 0 (caller falls back to full flatten,
                             reproducing today's error status).
       other, has_vars     → re-parse the line with the snapshot scope; copy
                             only args[]/num_args/payload values into the
                             existing slot — never provenance/flags.
                             Parse failure → return 0 (defensive).
       other, !has_vars    → skip (literal args can't change).  This is where
                             rebake beats full flatten even for loop-light
                             programs — flatten re-parses every line today.
   ```

   Structure, provenance, snapshots, counts, current-block range: untouched ⇒
   every flat-array consumer keeps working by construction; the executor is
   untouched. goto needs nothing special: flatten evaluates assignments in
   linear source order ignoring goto, and rebake replicates linear order.

**Correctness invariant (what the tests pin):** when the classifier says
"static", a rebake is bit-identical to a full flatten at the new variable
values — both evaluate the same expression texts against the same local scopes
and the same sequentially-threaded predef/scratch state. Three things could
break sequence stability — loop trip counts, if-arm selection, call-arg
snapshot values — and all three are exactly the structural-expression set the
classifier checks.

**Known limitation (document when shipped):** programs passing a runtime var
through `funcN(...)` args still full-reflatten every frame. Refreshing
call-bound snapshot entries in place is possible in principle (snapshots carry
names; provenance carries `call_src_cmd_idx`) but is effectively a mini-VM —
Phase B territory, not worth half-building here.

### Steps

**A1. Classifier + caches** — `src/repl/eval.c/h`, `src/repl/state.c`,
`src/repl/state_views.h`, `src/repl/state_owners.h`:

- New pure text predicate `repl_eval_line_has_runtime_values(const char *src)`
  in eval.c (clone the comment-skipping token-scan shape of
  `repl_eval_source_uses_ident`, `eval.c:964-988`): matches any predef var
  name, scratch array ident, or `rand`/`rand2`. Do **not** widen the existing
  `repl_eval_input_has_predef_vars` — other callers depend on its semantics.
- `ReplDocumentState` gains `source_structure_dynamic` + `_dirty` (beside the
  `source_uses_time` pair, `state_views.h:42-48`); `ReplFlatProgramState` gains
  `args_dirty`. New `scan_source_structure_dynamic()` in state.c walks document
  cmds, running the predicate on lines whose type ∈ {CMD_FOR_BEGIN,
  CMD_IF_BEGIN, CMD_ELSE_IF, CMD_CALL}. Lazy accessor
  `repl_state_source_structure_dynamic()` mirroring
  `repl_state_source_uses_time()`; invalidate in `repl_state_mark_source_dirty`
  (the documented single invalidation point).
- Accessors: `repl_state_flat_program_args_dirty/_clear_args_dirty`,
  `repl_state_mark_flat_args_dirty` (declared beside `repl_state_mark_flat_dirty`
  in `state_notify.h`); keep full-dirty ⊇ args-dirty coherent in mark/reset/
  clear. Views vs owners split per convention (`check-state-ownership`
  polices it).

**A2. Dirty routing** — `state.c`, `apply.c`:

- `repl_state_time_advance` / `repl_state_time_set`: three-way — source doesn't
  use `t` → nothing (today); uses `t` && structure-dynamic → full dirty
  (today); uses `t` && static → `args_dirty`.
- `apply.c` SET_VALUE (`:160-167`): same three-way per changed var — source
  doesn't mention the name at all (a `repl_eval_source_uses_ident`-style scan)
  → mark nothing (a new free win; today sliders always full-dirty); else
  structure-dynamic → full; else args-dirty. DECLARE/UNDECLARE keep full dirty
  (var-table reshape; UNDECLARE shifts `var_idx` slots — never rebake across
  that).

**A3. Rebake pass** — `src/repl/flatten.c/h`:

- Extract shared evaluation kernels from the existing leaves so flatten and
  rebake cannot drift (most of the diff, and the main Phase A risk): (a)
  per-line arg re-parse from `flatten_reparse_line` (:389-437) —
  `repl_parser_parse_command_ctx` with `.skip_text=1`, leaving append/
  provenance to the caller; (b) assign-RHS eval from `flatten_var_assign`;
  (c) scratch index+RHS eval from `flatten_scratch_assign`.
- New public API: `repl_flatten_rebake_program(options, status, sz)` (pure,
  testable; options carry mutable flat cmds + `const` local_vars + count +
  `SourceTextView` + func aliases) returning 1 on success / 0 = caller must
  fall back to full flatten, plus a live-state wrapper
  `repl_flatten_rebake_commands(void)`.
- Add a `PROF_REBAKE` section (`prof_sections.h` + label in
  `src/app/glr_prof.c`).

**A4. Controller gates** — `src/app/glr_ctrl.c`:

- Frame gate (:1701): `if (full dirty) flatten; else if (args_dirty) { rebake
  under PROF_REBAKE; on failure fall back to full flatten; clear; refresh flat
  view; }`. Current-block highlight refresh is *not* needed on the rebake path
  — block boundaries are structural.
- Blur time subframe (:1597-1604): rebake per sample when structure is static,
  else full reflatten as today. With 16 accum passes this is the single
  biggest win (16 reflattens/frame → 16 rebakes/frame).
- All other direct `repl_flatten_commands` callers (loads, tests, export)
  unchanged — full flatten always remains correct.

**A5. Tests** — new `tests/test_repl_flatten_rebake.c` (+ Makefile wiring):

1. **Differential equivalence over the full example corpus** (the freeze-bug
   defense, load-bearing): for every built-in example, flatten at t=0; advance
   t; if classified static, rebake into buffer A and full-flatten into scratch
   buffer B; assert identical counts/types/provenance/has_vars/args and
   identical post-run predef/scratch tables. Any future classifier bug fails
   loudly here instead of freezing pixels.
2. Classifier unit table: `for(i,0,floor(t))`, `n=t*2; for(i,0,n)`,
   `A[0]=t; … for(i,0,A[0])`, `func1(t)`, `if(t>1)`, `for(i,0,rand(0,5))` →
   dynamic; nested constant-bound loops with `glVertex3f(sin(t),i,j)` and
   `t`-only-in-comments → static.
3. Sequential assignment semantics: `n = n + 1` in a loop + a scratch
   accumulator; rebake twice ≡ two full flattens, frame for frame.
4. Fallback: scratch index driven out of range by the new value → rebake
   returns 0, full flatten reproduces the error status.
5. Dirty routing: t-tick on static source sets args_dirty not dirty; a
   subsequent edit escalates to full; one flatten clears both.
6. Replay + rebake regression (seek mid-replay, rebake, compare annotations/
   fade inputs against a full-flatten baseline).

**A6. Doc fixes:** `ARCHITECTURE.md` §3.4/§5.3/§13.3-13.5 and the
`executor.h` / `flatten.h` header comments were already corrected when this
plan was written (the executor-re-eval claim). Remaining when Phase A ships:
rewrite §13.5 from "planned" to "implemented" wording, and note in
`flatten.h` that the rebake pass is the local-var snapshots' second reader
(alongside replay annotations).

**A7 (optional follow-up, separate change):** slider drags on vars with a
`float k = 1;` decl line also rewrite the decl initializer text per drag frame
(`repl_compile_set_predef_value`, `compile.c:~1323` → REPLACE_ONE →
source-dirty → full reflatten regardless of A2). Defer the text rewrite to
drag-release; the SET_VALUE op alone carries the live value during the drag.
Touches editor/app layers + undo semantics — keep out of the core Phase A
diff.

**Effort:** ~2-4 days including tests; risk concentrated in the kernel
extraction (mitigated by test 1).

---

## Phase A.5 — Compiled-expression cache (assessed, deferred to B0)

Text re-parse (recursive descent over strings, on every evaluation) is likely
the dominant flatten cost for big unrolled programs. A per-source-line
compiled RPN/postfix representation — compiled once at commit, lazily; stored
as a parallel document cache invalidated in `repl_state_mark_source_dirty`;
falling back to text parse on miss/overflow — would speed **both** full
flatten (the structure-dynamic programs Phase A cannot help) and the rebake
pass, and is exactly the expression representation the Phase B VM needs for
execute-time evaluation.

Sketch: new `src/repl/expr_program.c/h` (`repl_` prefix); token stream of
{push-const, push-named-ident (resolved at eval against locals then predefs,
surviving slot shifts), call-builtin(n), binary/unary ops}; compiled slots for
per-arg expressions, for-header bounds, if conditions, call args, assignment
RHS/index.

**Decision rule: defer until after Phase A lands, then measure.** Profile the
reparse share on `--example 28`; build it as Phase B milestone 0 (where the
Phase A differential harness gives it a free correctness net) only if the
share is still material. Effort ~1 week.

---

## Phase B — Control-flow-interpreting VM executor (staged design)

### Shape decision: the VM is the one flattener

The VM interprets the *source command array* — which already is the symbolic
program: for/if/call/goto markers all present, ≤ MAX_COMMANDS lines — and
reports each resolved emission through an observer callback:

```c
typedef struct {
    int src_cmd_idx, call_src_cmd_idx, root_call_src_cmd_idx;
    unsigned func_scope_mask;
    int call_depth;
    int emission_ordinal;              /* the future replay PC */
} ReplVmProvenance;

typedef int (*ReplVmEmitFn)(const GLCmd *resolved,
                            const ReplVmProvenance *prov,
                            const ExprVar *locals, int nlocals, void *ud);
```

A **materializing observer** writes `flat_cmds[]` / `local_vars[]` /
provenance bit-identically to today's `repl_flatten_program`. Consequences:

- **One control-flow interpreter forever.** The executor's goto scanner
  (`executor.c:754-790`) and replay_annotations' independent if/goto simulator
  (`replay_annotations.c:603-676`) eventually fold into it — today those are
  three interpreters' worth of drift surface.
- **Every consumer keeps an escape hatch**: on-demand materialization
  reconstructs today's flat array for consumers not yet migrated, so migration
  is per-consumer and independently shippable.
- **Phase A's classifier stays as the permanent dispatcher**: static structure
  → materialize once + today's dumb executor (the cheapest path); args-only
  dynamic → rebake; structure-dynamic → VM per frame. Static programs never
  pay VM interpretation.

### Milestones

- **B0 — expression cache** (= A.5). Flatten output unchanged (differential
  test); reparse cost ≈ 0.
- **B1 — VM core, offline parity mode.** New `src/repl/vm.c/h` (`repl_vm_*`
  prefix — the one new prefix, documented as repl-layer). Loop frames (counter
  binding, end/step, body range via the `source_scope` block-end helpers),
  call frames (param bindings from call-arg exprs, return index, scope mask,
  depth ≤ MAX_FLATTEN_CALL_DEPTH), if/else-if skip-scan, execute-time
  assignment evaluation threading the live predef/scratch tables, goto over
  source indices (top-level only, same restriction as flatten today). Two
  budgets, both `REPL_GOTO_LOOP_LIMIT`-shaped with clamp + status text:
  `visit_budget` (interpretation work; replaces MAX_FLATTEN_VISIT_BUDGET's
  role) and `emission_budget` (output volume — the runtime answer to
  `for(i,0,1000*t)`, which grows without any source edit and must be clamped
  with a diagnostic *mid-frame*). Deliverable: a **differential harness** —
  old flatten vs VM+materializing-observer over the full example corpus plus
  adversarial snippets (recursion, goto loops, budget overflows, invalid
  lines) asserting bit-identical flat arrays and identical status text. That
  harness is the safety net for everything after.
- **B2 — swap the engine.** `repl_flatten_program` becomes a thin
  VM+materializer wrapper; delete the recursive `flatten_range` family after a
  parity soak; keep the harness as a permanent golden regression. No consumer
  changes; per-frame cost unchanged; the Phase A rebake keeps working (or is
  re-expressed as a VM mode that re-emits into existing slots).
- **B3 — direct execution for the live frame.** The emit callback drives
  `ReplExecCursor` per resolved command (it already steps per-command); the
  flat-array goto scan is deleted (the VM owns goto). Materialization becomes
  on-demand behind `repl_state_flat_program_view()` with a generation counter
  so consumers can tell whether their view is stale. `MAX_COMMANDS` stays as
  the source-line cap and the materialized-view buffer size (with a
  "materialized view truncated" status when a symbolic program exceeds it);
  the runtime emission budget starts at the 4096-equivalent for behavior
  parity, relaxing later via a config knob.
- **B4 — per-consumer migration**, each an independent, small change:

  | Consumer | Today | Migration story |
  |---|---|---|
  | replay playback (PC/seek/exec_limit) | PC = flat index | PC = **emission ordinal**; seek/advance = VM run with `emission_budget = N` cutoff (deterministic — replay baselines vars/time). Alternatively keep replay materializing forever: it freezes `t`, so it's one materialization per replay start; cheap, low-risk. Migrate last. |
  | replay fade batches | walks `flat_cmds` between old/new PC | batches of emission ordinals collected by an observer during the seek run |
  | replay annotations | own if/goto simulator | delete; VM in a no-GL "state-observe" mode |
  | flatten_query cursor match | **backward** state scan over flat | reformulate as a single forward observer pass recording the best match — backward scans don't survive symbolic walks |
  | flat-cost histogram (`--flat-histogram`) | counts flat entries per line | observer counting emissions per `src_cmd_idx` — arguably better: counts true per-frame work |
  | current-block highlight | flat range scan | provenance events filtered by src range, or stay on the materializer (UI cadence) |
  | edit overlays / cursor guides / transform guides | index flat by `src_cmd_idx` | event filter by `src_cmd_idx` subscribed during the live VM run |
  | autonormal | reads flat on source-dirty only | on-demand materialization is fine indefinitely |
  | PLY export | executes flat under feedback | VM → executor emission under feedback, same as the live path |

- **B5 — relax the limit + docs.** Raise the emission-budget default, expose
  the knob, rewrite §13.1's "the cap is the lever" story to "the budget is the
  lever".

### Phase B risks

- **Assignment-timing semantics change.** Today assignments evaluate once per
  flatten and the executor re-applies baked values; under the VM they evaluate
  per execution. `n = n + 1` accumulates per-flatten today, per-frame after
  B3 — a deliberate, more intuitive semantics, but it must be documented and
  the differential harness must compare *observable per-frame state*, not just
  emission streams.
- **Mid-frame budget clamp** must not corrupt GL state: unwind open
  glBegin/tess/matrix via the existing `repl_exec_cursor_end` path.
- **Ownership guards**: vm.c is repl-layer; observers for UI consumers are
  installed from the controller (no editor/app includes from `repl_*`;
  `check-state-ownership` stays green).
- **Static-program regression risk**: avoided by the Phase A dispatcher —
  static programs keep the materialize-once + dumb-executor path.
- **Effort**: B0 ~1 wk, B1 ~2-3 wk, B2 ~1 wk, B3 ~2 wk, B4 spread across many
  small changes, opportunistically.

---

## Files touched (Phase A)

- `src/repl/eval.c/h` — runtime-values line predicate
- `src/repl/state.c`, `src/repl/state_views.h`, `src/repl/state_owners.h`,
  `src/repl/state_notify.h` — classifier cache, `args_dirty`, time-tick routing
- `src/repl/apply.c` — slider SET_VALUE three-way routing
- `src/repl/flatten.c/h` — kernel extraction + rebake pass
- `src/app/glr_ctrl.c` — frame gate + blur subframe gate
- `prof_sections.h`, `src/app/glr_prof.c` — `PROF_REBAKE`
- `tests/test_repl_flatten_rebake.c` (new), `Makefile`
- `src/repl/ARCHITECTURE.md` §13.5 (planned → implemented wording),
  `src/repl/flatten.h` (note the rebake pass as the snapshots' second reader)

## Verification (when implemented)

- `make test`, `make test-stubs`, `make check-state-ownership`,
  `make check-c99`; cross-check on gracemont
  (`ssh gracemont '… make check-c99 && make test-stubs'`).
- Profiling: `./gl-repl --example 28` profile panel — PROF_FLATTEN ≈ 0 in
  animation steady state for static-structure examples, PROF_REBAKE ≪ old
  PROF_FLATTEN; `--flat-histogram` output unchanged.
- Animation-freeze canary (end-to-end): OSMesa headless build, capture two
  frames of a `t`-driven static-structure example at different `t`
  (`GLR_TIME` + SIGUSR1 capture or `FREEGLUT_CAPTURE_FRAMES`), assert the
  framebuffers differ; plus a blur-mode capture to exercise the subframe
  rebake path.

## Conventions

C99 throughout; no new top-level prefixes beyond the documented `repl_vm_*`
(Phase B); new state fields behind `state.c` accessors with the views/owners
split; `repl_*` modules never reach editor/app layers.
