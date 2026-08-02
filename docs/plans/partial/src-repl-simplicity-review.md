# `src/repl/` Architectural Review - Findings & Followups

> **Superseded as the active map (2026-06-20).** The live cleanup map for
> `src/repl` is now `plans/done/repl-structure-readability-audit.md`,
> which subsumes the brittle-spots / smaller-risks lists below and carries
> the "durable spine" section forward. Two still-live items from here - the
> `compile.c` verb-boundary split and the `apply.c` `num_args`-cascade helper
> - are tracked there explicitly. This document is retained for provenance and
> its dated currency pass; schedule remaining work from the audit, not here.

## Provenance

Captured 2026-05-14 from an arch-simplicity-reviewer pass over `src/repl/`.
This document records both what the reviewer found durable (so we don't
churn it) and the localized brittle spots worth scheduling work against.
No rewrite is proposed; every recommended fix uses an idiom already
present in the tree.

## Currency pass (2026-06-20)

Re-verified against the tree ~5 weeks after capture; most actionable
items have shipped. Per-item STATUS lines below carry the detail; the
short version:

- **Brittle #2 (ReplHostEffects)** - DONE (already marked).
- **Brittle #3 (export.c)** - premise outdated: audit #69 split the
  reader half into `import.c`, so it is no longer one TU. The
  "still large?" question survives; the description doesn't.
- **Smaller risks now resolved:** the func-alias compile pre-step +
  `repl_compiled_change_rollback_alias` (removed by `4e0a6b87`, which
  moved alias publish to apply-time), the `core.h` `editor_navigate_to_line`
  / `editor_feed_line` exports (off the facade), and the `eval.h`
  comment drift (rewritten to reference the macros symbolically).
- **Live residual (genuinely still open):** the `compile.c` split
  (Brittle #1 - line numbers refreshed below), the `apply.c`
  `num_args`-cascade `command_store` helper, and the `compile.h` /
  `ReplCompileContext` "promote predef-reads off the global" ratchet.

## Durable spine - leave it alone

These are the structural commitments the reviewer flagged as load-bearing
and well-paid-for. Future changes should preserve them.

- **Two-level command model (source → flat → GL).** `command.h`'s `GLCmd`
  is a clean parse-result record - type/args/flags + provenance
  (`src_cmd_idx`, `call_src_cmd_idx`, `func_scope_mask`) - with **no
  source text field**. The fact that per-line text lives on
  `EditorState` (not on `GLCmd`) is what lets `flatten.c`/`executor.c`/
  `replay_annotations.c` take a `SourceTextView` and what lets the
  `tools/repl_demo` binary link without the editor.
- **Descriptor-table pattern is fully internalized.** `command_spec.c`
  has three coherent static arrays - `k_enum_command_specs[]`,
  `k_std_command_specs[]`, and `g_command_type_specs[CMD_TYPE_COUNT]`
  via the `CMD_TYPE_SPEC` designated-initializer macro. Parser,
  formatter, autocomplete provider, code-panel renderer, F1 help
  builder, and the "valid in begin block" guard all read from it.
  Adding `CmdSyntaxCategory` and `valid_in_begin` to the one struct
  rather than scattering switches is the canonical extension point for
  new commands.
- **compile/apply purity split is enforced.** `compile.c` produces
  `ReplCompiledChange` values; predef-var/scratch side effects are
  represented as data (`ReplPredefOp[]`, `ReplScratchOp[]`) rather
  than mutation. `apply.c` is strictly the dual - touches command
  arrays only, with an explicit preflight
  (`repl_apply_can_apply_compiled_change`) so capacity failure can't
  half-commit. The `delete_pos`/`delete_count` + `INSERT_MANY`
  compound-plan shape expresses "replace block" atomically without a
  new kind enum.
- **`command_store.c` is small (~151 lines) and stays parse-free.**
  Pure array mechanics; the seam is honest.
- **Owner-vs-view header split.** `state.h` / `state_views.h` /
  `state_owners.h` plus the `check-views-no-owners` guard is the
  cheapest possible structural barrier between read-by-value (UI,
  scene, tests) and mutate (controller, owners). The `_mut()` suffix
  reads cleanly.
- **`source_scope.c` (~201 lines) is right-sized.** Prefix-depth cache,
  indent helpers, block-end finder, plus small predicates exposed to
  the editor (`repl_line_is_block_head`, `repl_line_is_label`,
  `repl_range_contains_var_decl`) so the editor doesn't pattern-match
  on `CmdType`.

## Brittle spots - actionable followups

Each item below is localized. None requires redesign; all three have the
right idiom already in-tree.

### 1. Split `compile.c` at the verb boundary - DEFERRED (2026-05-15)

**Status.** Deferred. Pure file-boundary refactor with no behavior or
API change; no triggering feature pending. Revisit when `compile.c`
growth or a compile-facing feature makes the split pay for itself.
STILL OPEN (2026-06-20) - and the file kept growing: **2137 lines** now
(was 1914 at capture). The split-landing line numbers below are
stale; re-derive them before acting.

**Problem.** `compile.c` is **1914 lines** and growing. The static
helpers around float-decl parsing alone - `parse_float_name_list`,
`validate_decl_names`, `format_decl_text`, `build_decl_predef_ops`,
`build_decl_commit_message` - total ~250 lines for one command form.
`repl_compile_toggle_comment` is at lines 1289-1473. The four
structured-block validators (`close_brace`, `if_block`, `func_def`,
`for_loop`) are each ~140 lines.

**Proposed split.**
- `compile_var.c` - float-decl, var-assign, set-predef.
- `compile_block.c` - close-brace, if, func, for.
- `compile.c` keeps the dispatcher + range/comment/empty +
  `ReplCompiledChange` helpers.

**Contract.** No public API changes. File-boundary refactor at a natural
seam. Pure mechanical move; `make check-state-ownership` and the full
test suite should pass unchanged.

**Suggested split landings (line numbers approximate as of review):**
~717, 816, 1289, 1487, 1540, 1648, 1794.

**Scope note (2026-05-15, added when deferring).** The split is worth
doing, but this plan understates the work. "Pure mechanical move"
(line 75 of the original review) is true for the *function bodies*
only - the surrounding build/guard plumbing also has to move with
them:

- **Makefile.** The compile TU is hardcoded in source lists, e.g.
  `Makefile:217` (and the parallel stub/test object lists). Two new
  TUs (`compile_var.c`, `compile_block.c`) must be added everywhere
  `src/repl/compile.c` appears, or the new files won't link / won't
  build under `USE_GL_STUBS=1`.
- **Guard scripts hardcode the filename.**
  `scripts/check/check-no-set-status-in-compile-apply.sh:21` greps a literal
  `src/repl/compile.c src/repl/apply.c` pair; the new TUs inherit the
  same "compile is a pure validator, never calls `set_status`"
  contract and must be added to that grep (and any sibling guards that
  name `compile.c`) or the invariant silently stops being enforced on
  the moved code.
- **Shared private helpers.** `compile_set_err` (and any other
  `static` helper used by both the var and block validators -
  `compile.c` has ~20 file-statics) currently rely on single-TU
  visibility. Splitting forces either a small private
  `compile_internal.h` exposing those helpers, or local duplication.
  A private header is the cleaner choice and is itself a new file the
  Makefile/guards don't need to know about but the split must create.

So: low *behavioral* risk, but it is a multi-file change touching the
build and the guard layer, not a single-file cut. Budget for the
Makefile + guard-script + private-header edits, not just the function
moves.

### 2. Consolidate `core.c` sink installers into one `ReplHostEffects` bridge - DONE (2026-05-15)

**Status.** Done. The six `repl_install_*_sink` installers and their
`g_*_sink` statics collapsed into one `ReplHostEffects` struct +
`repl_install_host_effects()` / `repl_host_effects()`, mirroring
`ReplExportConfigBridge`. `core.c` keeps a single
`g_host_effects` pointer; the `repl_set_status` / `repl_dispatch_*`
emit functions are unchanged for callers and now read the struct.
`glr_ctrl.c` installs one file-static `g_glr_host_effects`. Guardrail
honored: no individual installers remain (a 7th effect extends the
struct). Verified: `make sample`, `make sample USE_GL_STUBS=1`,
`make repl_demo` (bridge unset → dispatches no-op, link isolation
intact), `make test` (4298/4298), `make check-state-ownership` all
green.

**Problem.** `core.c` currently exposes six function-pointer sink
installers (`repl_install_status_sink`,
`repl_install_example_presentation_reset_sink`,
`repl_install_input_reset_sink`,
`repl_install_insert_mode_off_sink`,
`repl_install_scroll_to_line_sink`,
`repl_install_follow_cursor_sink`) plus matching `g_*_sink` statics and
dispatchers. Each is individually justified by demo-link isolation, but
**collectively** they read as a callback bus - the only pattern-soup
risk in this directory.

**Proposed shape.** Collect the sinks into one `ReplHostEffects` struct
installed in a single call, mirroring `ReplExportConfigBridge` /
`ReplExportCameraBridge` (already in `export.h`, lines ~91-105). This
is the **already-present** idiom for "controller installs vtable into
pipeline" - the sinks should match it rather than invent their own
shape.

**Guardrail.** Resist adding a 7th individual `repl_install_*_sink`. If
a 7th effect is needed, the bridge-struct consolidation becomes
mandatory rather than optional.

### 3. `export.c` split (R9 in MODULES.md) - DEFERRED (2026-05-15)

**Status.** Deferred (already self-described as "Not urgent" below).
The `ReplExportConfigBridge` / `ReplExportCameraBridge` pattern absorbs
new persisted keys without code change, so the split is comfort, not
necessity. Revisit on trigger (a third bridge, or editing friction).

**Status.** Already an acknowledged open edge. PREMISE OUTDATED
(2026-06-20): audit #69 has since split the **reader** half out into
`src/repl/import.c`, so this is no longer one TU. The two files are now
`export.c` (writer, 3048 lines) + `import.c` (reader, 2039 lines) - each
still large, sharing the duplicated `IMPORT_EXPORT_STATE` macro block.
The "split export.c further?" question survives only as a size concern;
the import/export separation it asked for already landed.

Original framing (pre-split): `export.c` is **3633 lines** - the largest
file in the directory by a wide margin. Single TU does import + export +
workspace headers + camera-line refresh + bootstrap init for two
orchestration modes.

**Not urgent.** The `ReplExportConfigBridge` / `ReplExportCameraBridge`
pattern inside already absorbs new persisted keys without code change,
so this split is comfort, not necessity. Defer until either (a) a third
bridge wants to appear, or (b) editing the file becomes a friction
point.

## Smaller risks - note and watch

- **`repl_compile_func_def` mutates `g_func_aliases` as a pre-step.**
  RESOLVED (2026-06-20). `4e0a6b87` reworked this: compile now emits a
  pending `ReplFuncAliasOp` and the parser resolves the new name through
  it (`parse_repl_func_signature_with_pending_alias`); `apply.c`
  publishes the alias only after the command-store mutation succeeds.
  `repl_compiled_change_rollback_alias` and its bookkeeping fields are
  gone, and the redundant `repl_func_alias_clear` it called was removed
  too. The bent purity seam this item watched no longer exists - compile
  no longer touches the alias table.
- **`apply.c` reaches into `repl_state_document_cmds_mut()`**
  to cascade `var_idx` decrements during `repl_apply_predef_ops`.
  RESOLVED (2026-07-31). The broad command-array mutator was retired;
  `apply.c` now performs this owner-only cascade through its existing
  `ReplCommandStore`, while read-only callers use
  `repl_state_document_cmds()`.
- **`core.h` still publishes `editor_navigate_to_line` /
  `editor_feed_line`** on the REPL public facade. RESOLVED
  (2026-06-20) - neither symbol is on `core.h` anymore (R10 work).
- **`eval.h:38-88` comment drift.** RESOLVED (2026-06-20) - the
  header comment was rewritten to reference `MAX_PREDEF_VARS` /
  `MAX_EXPR_VARS` symbolically; the stale "silent truncation at 16"
  literal is gone and the truncation is now documented as intended
  behavior, not drift.
- **`compile.h` is 401 lines, much of it a doc block.** STILL OPEN
  (2026-06-20) - now **530 lines**. `ReplCompileContext` is half real
  fields, half admission that compile still reads predef vars through a
  global. The transitional state should be explicitly time-bounded -
  pick a "promote to context" ratchet before the next compile-facing
  feature lands.

## Verdict

The spine ages well. New GL commands cost one `CmdType` row + one
`g_command_type_specs[]` row + one `k_*_command_specs[]` row + the
executor/flatten cases. New editor modes don't touch `src/repl/` at all.
New export formats pay the `export.c` size tax but the existing
bridge-struct pattern absorbs new keys cleanly.

The brittle spots (`compile.c` size, `core.c` sink soup, `export.c`
size) are all localized and the project already has the right idioms
in-tree to fix them. As of the 2026-06-20 currency pass the `core.c`
sink soup (`ReplHostEffects`) is done and the `export.c` reader/writer
split has landed (`import.c`); the one scheduled item still open is the
`compile.c` verb-boundary split, plus the two smaller-risk followups
(the `apply.c` `num_args` cascade helper and the `compile.h` /
`ReplCompileContext` global-predef ratchet). Leave the rest unless a
triggering change arrives.

## Files cited (review snapshot, 2026-05-14)

- `src/repl/command.h`
- `src/repl/command_spec.c` (lines 313-376 - `CMD_TYPE_SPEC` macro and
  `g_command_type_specs[]`)
- `src/repl/compile.c` (1914 lines; split candidates ~717, 816, 1289,
  1487, 1540, 1648, 1794)
- `src/repl/compile.h` (401 lines)
- `src/repl/apply.c` (lines 121-124 - `num_args` cascade)
- `src/repl/command_store.c`
- `src/repl/state_views.h` / `state_owners.h` / `state.h`
- `src/repl/core.c` (six sink installers; flagged for dissolution under
  R10)
- `src/repl/core.h` (`editor_*` symbols still on this facade)
- `src/repl/export.h` (lines ~91-105 - bridge pattern to mirror in
  `core.c`)
- `src/repl/export.c` (3633 lines; R9 split deferred)
- `src/repl/eval.h` (lines 38-88 - `MAX_EXPR_VARS` / `MAX_PREDEF_VARS`
  comment drift)
