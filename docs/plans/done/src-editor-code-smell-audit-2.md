# `src/editor/` — Code-Smell Audit (Follow-Up)

> Audit produced 2026-05-26 by four parallel reviewers, scoped to
> `src/editor/`. Findings extend the closed first-round audit
> (`plans/done/src-editor-code-smell-audit.md`, last revision
> 2026-05-25) — reviewers were given the closed audit's resolved
> list and instructed **not** to re-flag items already ✅ done.
>
> The slice split mirrored the prior audit:
>
> - `input.c` (~1913 lines — heaviest file in `src/editor/`) +
>   `input.h` + `edit_ops.{c,h}`
> - `commit.c` (~1392 lines) + `commit.h` + `services.{c,h}` +
>   `reformat.{c,h}`
> - `state.{c,h}` + `clipboard.{c,h}` + `undo.{c,h}`
> - `search.{c,h}` + `inline_rename.{c,h}` +
>   `inline_file_prompt.{c,h}` + `completion.{c,h}` +
>   `help_session.{c,h}`
>
> File:line references are exact at the time of writing; re-verify
> with the cited file before acting if this doc has aged.
>
> **Prior closures verified to still hold:** #1, #2, #5, #6, #8,
> #9, #14, #17, #21-#24, #25 (kept), #30, #31, #35, #36, #44, #46,
> #48-#50. No regressions on closed items. Slice 4 reports that
> `help_session.c` and `completion.c` are still appropriately thin —
> no logic creep since 2026-05-25.

## Status update — 2026-05-27 final pass (`editor-smells-2` complete)

The final pass closed the remaining Tier A/B residual items under the approved implementation plan:

- **#1 (asymmetric modal capture)** — resolved by introducing missing modal checks (`editor_input_file_prompt_capture_key` and `_special`) in the editor's keyboard/special handlers, securing the hard modal contracts during direct dispatches.
- **#5 / #21 (status unified & opt-in status publishing)** — standardized on a single `commit_message` inside `ReplCompiledChange`, removing duplicate fields from `EditorCommitPlan`, and implemented `publish_status` opt-in flag to keep framerate-hot dragging actions silent.
- **#6 (preflight transactional safety)** — added `repl_apply_can_apply_compiled_change` preflight guard at the top of `apply_compiled_change_full` in `commit.c` to prevent partial commits from corrupting state boundaries, and clamped indices in `state.c` to avoid uninitialized memory reads.
- **#8 / #35 (clean layering Escape routing)** — routed the Escape key through the controller router (`glr_ctrl_router_handle_escape_key`), completely decoupling editor input dispatch from peer subsystems and UI help visible states.
- **#11 (shared tail extraction)** — factored out the identical document reset tail in `editor_clear_all_cmds` and `editor_reset_for_new_scene` into `editor_reset_document_to_empty` in `input.c`.
- **#26 (merged alias pre-step)** — merged speculatively duplicated function alias registration logic into the unified `repl_compile_func_def_resolve_alias` helper in `src/repl/compile.c`. Added robust `rejected_keyword` signaling to prevent premature early exits for bare predefined functions (like `func0() {`).
- **#28 / #29 (undo ring generation safety)** — introduced `generation` tracking to `EditorUndoRingState` and `EditorUndoSnapshot` to prevent cross-scene or cross-workspace undo leakage, and clarified side-effects in `undo.h`.
- **#32 / #43 (clipboard clear canonicalization)** — standardized on the canonical `editor_state_clipboard_clear()` API.
- **#34 (inline rename re-entry)** — aligned inline rename modals under the `g_rename_active` conservative re-entry check.
- **#37 (reset scene docstring)** — documented the programmatic scene reset logic cleanly in `input.h`.
- **#42 / #45 / #46 / #47 (trimmed public surface)** — deleted completely callerless getters, mutators, and structural views (`editor_state_buffer_mut`, `editor_state_virtual_lines_count_for`, `editor_state_document`, `editor_state_document_mut`, `editor_state_document_reset`, `EditorDocumentView`, and `editor_help_session_mut`), and clearly documented test-only scaffolding APIs as such.

With all planned Tier A and Tier B residual items successfully completed, **the entire audit has graduated to `plans/done/`**. The remaining open items are all Tier C structural work.

Validated for the final complete slice:
- `make check-c99 && make test-stubs` → `7606 / 7606 passed`
- `make test` → `6492 / 6492 passed`

## Status update — 2026-05-26 follow-up (`editor-smells-2`, residual pass)

A second commit on this branch closed the remaining Tier A items
plus a P3 docstring inconsistency a reviewer caught in `commit.h`:

- **P3 (commit.h docstring drift)** — `commit.h` previously
  documented `editor_commit_apply_plan` as
  *"preflight → undo capture → REPL apply → editor-buffer apply
  → post-effects"* and called the apply sequence
  *"all three halves (predef-ops, editor buffer, cmd store)"*.
  Neither matched the implementation: apply does no undo capture
  (the dispatch site owns it) and runs the four-step
  predef/scratch/editor-buffer/cmd-store sequence via
  `apply_compiled_change_full`. File header, the
  `editor_commit_apply_external_change` "returns 1 if..." block,
  and the `editor_commit_apply_plan` overview were all rewritten
  to match the implementation; the undo-policy contract is now
  spelled out in the file header so future readers don't
  re-introduce the drift.
- `#3` — `commit_current_input` gained a `needs_commit_hint`
  parameter; `commit_before_navigation` passes `1` (we already
  verified above) so the predicate isn't double-evaluated on the
  navigation path. The Enter path passes `-1` (compute) to keep
  the call site neutral.
- `#9` — `parse_for_overwrite_enter` renamed to
  `parse_input_for_enter_commit` (matches its two actual paths —
  overwrite-Enter and append-at-end Enter — instead of lying
  about the second). Internal docstring tightened to spell out
  the two-path coverage.
- `#13` — `enter_parse_err` and `editor_parse_err` (two local
  buffers serving the same parse-err role) standardized on
  `parse_err_buf`.
- `#36` — `inline_rename.c` now uses an explicit `g_rename_active`
  flag instead of overloading `g_rename_slot = -1` as the
  active sentinel. Matches the `g_prompt_active` shape in
  `inline_file_prompt.c`; both modules now read the predicate the
  same way, which simplifies the eventual `EditorInlineModal`
  peer extraction (closed-audit #57).
- `#38` — `editor_take_input_effects` renamed to
  `editor_take_and_reset_input_effects`. The "take" verb hid the
  load-bearing reset side effect; many callers don't manually
  reset between dispatches, so dropping the inner reset would
  have been a behavior change. The rename makes the side effect
  explicit in the name and keeps the call sites unchanged.
- `#40` — `editor_commit_func_decl_resume_take` and `_set` made
  `static` to `commit.c` (only `_peek` had a cross-TU consumer —
  the test harness). The publish writer and read-and-clear
  consumer are called *above* their definitions in `commit.c`
  (apply-plan at L171, close-brace compile at L275), so
  file-local forward declarations were added near the top of
  the file to keep `make check-c99` green (under `-std=c99`,
  implicit declarations are a hard error).

The branch additionally documented the new `plans/` state
machine (`active/`, `not-started/`, `partial/`, `in-review/`,
`done/`) in `CLAUDE.md` so future contributors know where to
file new audit / implementation docs.

After this residual pass, every Tier A item from the audit
classification block has landed; **the audit is ready to
graduate to `plans/done/`**. The remaining open items are all
Tier B / Tier C work that should be tracked in their own future
plans (notably `#34` inline-overlay re-entry semantics, `#48–#57`
structural work).

## Status update — 2026-05-26 (`editor-smells-2`)

The branch has already landed a substantial part of the high-ROI
follow-up work. Completed findings from this audit:

- `#2` — `commit_progressed_since()` now uses the const document
  accessor for its read-only `memcmp`.
- `#4` — `editor_try_commit_var_statements_then_insert()` now uses
  `editor_input_clear()` instead of open-coding the input reset.
- `#7` — `tests/test_editor_completion.c` now exercises
  `editor_completion_accept()` directly and covers both the
  registered-provider and NULL/partial-provider paths.
- `#10` — the `parse_for_overwrite_enter` documentation was moved so
  it describes the actual helper it belongs to.
- `#12` — the `editor_committed_line_text` wrapper was removed and
  its callers now read directly through the buffer-view helper.
- `#14` — insert-mode parse failures now route through
  `repl_set_status_error()` instead of the plain status sink.
- `#15` — `EditorUndoRingState` documentation now records the
  production rollback path in `commit_before_navigation()`.
- `#16` — `editor_feed_line()` now goes through
  `editor_input_set_text()`.
- `#17` — `edit_op_buffer_delete_left_of_cursor()` now documents
  the same selection-preservation contract as the sibling edit ops.
- `#19` — the `commit.c` file header now reflects the current direct
  apply flow rather than the pre-`EditorServices`/pre-scratch-op
  description.
- `#20` — `apply_compiled_three_halves()` was renamed to
  `apply_compiled_change_full()`.
- `#22` — `_then_insert` no longer duplicates its epilogue, no
  longer clobbers the inner success status with a generic
  "Insert mode", and no longer issues a redundant outer
  `repl_mark_source_dirty()`.
- `#23` — the `end_type` docstring no longer claims a nonexistent
  status-formatting consumer.
- `#24` — the `commit.h` structured-compile doc now correctly says
  the helpers write `EditorCommitPlan` through the `out` pointer.
- `#25` — the four `editor_compile_*` wrappers now share a single
  err-buffer policy: clear provided buffers on entry, tolerate
  `NULL`/zero-sized buffers, and format failures through one helper.
- `#30` / `#31` — `editor_state_clipboard()` and
  `editor_state_autocomplete()` now return const pointers and the
  production/test callers that only read them were updated.
- `#39` — `EditorServices` is gone: the four live parse dispatches in
  `input.c` now call `repl_parser_parse_command_ctx()` directly,
  `services.{c,h}` were deleted, the Makefile/test manifests were
  cleaned up, and the supporting docs/guards/baselines were updated.
- `#41` — the redundant `newly_aliased_slot` success-path assignments
  in `editor_compile_func_def()` were removed.

Validated for the landed slice:

- `make test_repl_editor USE_GL_STUBS=1 && build/release-gl-stubs/test_repl_editor`
  → `867 / 867 passed`
- `make test_repl_compile && build/release/test_repl_compile`
  → `211 / 211 passed`
- `make test_editor_completion && build/release/test_editor_completion`
  → `11 / 11 passed`
- `make test_repl_state && build/release/test_repl_state`
  → `153 / 153 passed`
- `make test_tutorial_runner && build/release/test_tutorial_runner`
  → `453 / 453 passed`
- `make test_glr_actions USE_GL_STUBS=1 && build/release-gl-stubs/test_glr_actions`
  → `343 / 343 passed`
- `./scripts/check-editor-repl-surface.sh`
  → `input.c=24/24, commit.c=35/35`

Findings that no longer need work as written in the current tree:

- `#27` — the contradictory `editor_commit_func_decl_resume_set`
  comment cited in the audit is already gone from `src/editor/commit.c`.
- `#44` — the cited forward-declaration block is already gone from
  `src/editor/state.c`.

## Suggested Commit Message

```text
editor: land audit-driven cleanup across input, commit, state, and services

Address the highest-ROI follow-ups from
plans/active/src-editor-code-smell-audit-2.md and fold in the
adjacent doc/test/build cleanup that shares the same touched
surfaces.

Input dispatch cleanup:
- switch commit_progressed_since() to the const document accessor
  for its read-only memcmp path (#2)
- move the parse_for_overwrite_enter() documentation so it
  describes the right helper and remove the stale
  editor_committed_line_text() wrapper in favor of direct
  editor_buffer_view_line() reads (#10/#12)
- report insert-mode parse failures with repl_set_status_error()
  so the insert and overwrite parse paths use the same error sink
  (#14)
- route editor_feed_line() through editor_input_set_text() instead
  of open-coding the input-buffer write (#16)

Commit-path cleanup:
- replace the raw input clear in
  editor_try_commit_var_statements_then_insert() with
  editor_input_clear(), factor the shared insert-mode epilogue,
  preserve the inner success status, and drop the redundant outer
  repl_mark_source_dirty() (#4/#22)
- rename apply_compiled_three_halves() to
  apply_compiled_change_full() and refresh the surrounding
  commit.c/commit.h docs so they describe the current four-step
  apply flow and the real EditorCommitPlan out-parameter contract
  (#19/#20/#23/#24)
- standardize editor_compile_close_brace(),
  editor_compile_if_block(), editor_compile_func_def(), and
  editor_compile_for_loop() on one err-buffer policy: clear
  provided buffers on entry, tolerate NULL/zero-sized buffers, and
  format failures through a shared helper (#25)
- remove redundant newly_aliased_slot success-path assignments from
  editor_compile_func_def() now that out->change.newly_aliased_slot
  is the only live sink (#41)

State/accessor cleanup:
- update EditorUndoRingState docs to mention the production
  rollback path in commit_before_navigation() (#15)
- add the missing "Pure with respect to selection" contract to
  edit_op_buffer_delete_left_of_cursor() in edit_ops.h (#17)
- convert editor_state_clipboard() and
  editor_state_autocomplete() to const-pointer accessors and
  update the production/test callers that only read those
  structures (#30/#31)

Completion/test coverage:
- extend tests/test_editor_completion.c with accept-dispatch
  coverage and verify editor_completion_accept() is safe with both
  a NULL provider and a provider whose hooks are partially NULL
  (#7)
- extend tests/test_repl_editor.c to pin the _then_insert status
  behavior, extend tests/test_repl_compile.c to pin the new
  err-buffer semantics, and refresh the state/tutorial/actions
  tests for the const-pointer accessor shape (#22/#25/#30/#31)

Remove EditorServices completely:
- replace the last four svc.parse_command_ctx(...) calls in
  src/editor/input.c with direct repl_parser_parse_command_ctx()
  calls and delete the now-empty EditorServices setup sites (#39)
- delete src/editor/services.c and src/editor/services.h, remove
  them from the Makefile, drop the stale forward declaration in
  commit.h, and remove the obsolete include from
  tests/test_repl_compile.c (#39)
- update MODULES.md, AGENTS.md, CLAUDE.md, ARCHITECTURE.md,
  src/editor/README.md, tools/editor_demo/editor_demo.c,
  scripts/check-editor-repl-surface.sh,
  scripts/baselines/editor-repl-surface.txt,
  scripts/check-module-prefixes.sh,
  scripts/check-glr-ctrl-not-editor-mirror.sh, and
  scripts/callgraph_file_groups.json so the docs and guardrails
  match the deleted service shim (#39)
- update plans/active/src-editor-code-smell-audit-2.md with the
  landed findings, current validation results, and this commit
  message template

Validation:
- make test_repl_editor USE_GL_STUBS=1 && build/release-gl-stubs/test_repl_editor
- make test_repl_compile && build/release/test_repl_compile
- make test_editor_completion && build/release/test_editor_completion
- make test_repl_state && build/release/test_repl_state
- make test_tutorial_runner && build/release/test_tutorial_runner
- make test_glr_actions USE_GL_STUBS=1 && build/release-gl-stubs/test_glr_actions
- ./scripts/check-editor-repl-surface.sh
```

## Status summary (verified 2026-05-27)

| # | Sev | Tier | Status | Finding (short) |
|---|---|---|---|---|
| 1 | 🔴 | B | ✅ closed | Asymmetric modal-capture (rename vs file-prompt) |
| 2 | 🔴 | A | ✅ closed | `_mut()` for read-only `memcmp` |
| 3 | 🔴 | A | ✅ closed | Double-eval `needs_navigation_commit` |
| 4 | 🔴 | A | ✅ closed | Open-coded input clear in `_then_insert` |
| 5 | 🔴 | B | ✅ closed | `apply_external_change` callers must publish status |
| 6 | 🔴 | B | ✅ closed | `apply_compiled_change_full` partial commit on bad input |
| 7 | 🔴 | B | ✅ closed | Missing `accept` test on completion provider |
| 8 | 🟡 | B | ✅ closed | Editor reaches into peer subsystems from input dispatch |
| 9 | 🟡 | A | ✅ closed | `parse_for_overwrite_enter` name mismatch |
| 10 | 🟡 | A | ✅ closed | Doc-comment detached from its function |
| 11 | 🟡 | B | ✅ closed | `clear_all_cmds` / `reset_for_new_scene` shared tail dup |
| 12 | 🟡 | A | ✅ closed | `editor_committed_line_text` no-op wrapper |
| 13 | 🟡 | A | ✅ closed | `enter_parse_err` vs `editor_parse_err` naming |
| 14 | 🟡 | A | ✅ closed | Insert-mode parse error uses wrong status sink |
| 15 | 🟡 | A | ✅ closed | `EditorUndoRingState` docstring claims test-only |
| 16 | 🟡 | A | ✅ closed | `editor_feed_line` open-codes `editor_input_set_text` |
| 17 | 🟡 | A | ✅ closed | `edit_ops.h` docstring drift on `_delete_left` |
| 18 | 🟡 | B | open | Sixth `collect_visible_vars` site in `compile_for_loop` |
| 19 | 🟡 | A | ✅ closed | `commit.c` file header describes pre-services flow |
| 20 | 🟡 | A | ✅ closed | `apply_compiled_three_halves` misnamed (4 ops) |
| 21 | 🟡 | B | ✅ closed | Two `commit_message` fields, no layering doc |
| 22 | 🟡 | B | ✅ closed | `_then_insert` asymmetric arms undocumented |
| 23 | 🟡 | A | ✅ closed | `end_type` docstring claims nonexistent consumer |
| 24 | 🟡 | A | ✅ closed | `commit.h` return-shape doc literally wrong |
| 25 | 🟡 | B | ✅ closed | `editor_compile_*` err-buffer null-check diverges |
| 26 | 🟡 | B | ✅ closed | Func-def alias pre-step duplicated editor/repl |
| 27 | 🟡 | A | ✅ closed | Stale "no cross-TU wrapper" comment |
| 28 | 🟡 | B | ✅ closed | `editor_undo_snapshot_restore` bypasses generation check |
| 29 | 🟡 | B | ✅ closed | `editor_undo_snapshot_restore` undocumented side effects |
| 30 | 🟡 | B | ✅ closed | `editor_state_clipboard()` returns ~1 MB by value |
| 31 | 🟡 | B | ✅ closed | `editor_state_autocomplete()` returns ~2 KB by value |
| 32 | 🟡 | B | ✅ closed | `_count_set(0)` and `_clear()` are aliases |
| 33 | 🟡 | C | open | `editor → ui` layering inversion via UI typedefs |
| 34 | 🟡 | B | ✅ closed | Inline-modal `begin` re-entry semantics diverge |
| 35 | 🟡 | B | ✅ closed | Help-overlay-close logic duplicated + layering trap |
| 36 | 🟡 | A | ✅ closed | Inline-overlay active predicates inconsistent sentinels |
| 37 | 🟢 | A | ✅ closed | `editor_reset_for_new_scene` exported with no docstring |
| 38 | 🟢 | A | ✅ closed | `editor_take_input_effects` hidden reset side effect |
| 39 | 🟢 | B | ✅ closed | `EditorServices` down to 1 method on 4 sites |
| 40 | 🟢 | A | ✅ closed | `_take` / `_set` could be static to `commit.c` |
| 41 | 🟢 | A | ✅ closed | Redundant `newly_aliased_slot` assignment |
| 42 | 🟢 | B | ✅ closed | 8 `editor_state_*` pairs with zero production callers |
| 43 | 🟢 | B | ✅ closed | `editor_state_clipboard_clear()` zero production callers |
| 44 | 🟢 | A | ✅ closed | Vestigial forward-decl block in `state.c` |
| 45 | 🟢 | B | ✅ closed | `editor_help_session_mut()` zero callers |
| 46 | 🟢 | B | ✅ closed | `help_session_capture/_restore` test-only |
| 47 | 🟢 | B | ✅ closed | `editor_completion_provider()` test-only |
| 48 | 🔵 | C | open | `commit_current_input` god-function (219 lines) |
| 49 | 🔵 | C | open | `parse_for_overwrite_enter` 50% structural dup |
| 50 | 🔵 | C | open | `compile_func_def` (267L) and `compile_for_loop` (249L) |
| 51 | 🔵 | C | open | `apply_swatch_change` wrong neighborhood |
| 52 | 🔵 | D | withdrawn | `editor_state_input()` not a 1 KB copy (factually wrong) |
| 53 | 🔵 | C | open | `state.h` 466-line junk drawer |
| 54 | 🔵 | C | open | Five different slice-getter shapes |
| 55 | 🔵 | C | open | `editor_state_capture/restore` test-only 3.2 MB |
| 56 | 🔵 | C | open | Dead O(N) per-line override lookup |
| 57 | 🔵 | C | open | `inline_rename.c` / `inline_file_prompt.c` 90% dup |

**Totals:** 45 closed, 1 withdrawn, 11 open.

By severity (open only): 0 🔴, 2 🟡, 0 🟢, 9 🔵.

By tier (open only): 0 Tier A, 1 Tier B (#18), 10 Tier C, 0 Tier D.

All Tier A items have landed. The remaining open items are Tier B / Tier C work.

---

## Headline take

57 findings total; #52 withdrawn (factually wrong — see finding),
so 56 live. Of those: 7 🔴 (real bugs / hazards), 29 🟡, 11 🟢,
9 🔵 (one of the 🔵 count is the withdrawn #52). Most reds are
tightly bounded. The dominant theme is **partial generalizations**:
closed audit items that were fixed for one slice but missed
identical patterns elsewhere. Examples:

- The `editor_state_search()` "return-pointer-not-by-value" fix
  (#35) was missed for `editor_state_clipboard()` (1 MB by-value)
  and `editor_state_autocomplete()` (2 KB by-value, hot path).
- The `editor_input_clear()` helper sweep (#44) was missed in
  `commit.c::editor_try_commit_var_statements_then_insert` — the
  open-coded pattern landed *after* the closeout.
- The `editor_input_set_text()` helper sweep (#46) was missed for
  `editor_feed_line`.
- The `editor_compile_close_brace` `err`-guard standard (#3 closed)
  was missed for the sibling `editor_compile_if_block` /
  `editor_compile_func_def` / `editor_compile_for_loop`.

The closed audit's #18 (Tier C — `collect_visible_vars → parse →
place` duplication) now has a sixth site (`editor_compile_for_loop`).
The closed #28 (Tier C — `EditorServices` dismantling) has shrunk
from a multi-step refactor to a single method on four call sites
— much more attractive to land now.

## Tier classification (this audit's recommendations)

(Mirrors the system the prior audit landed; see `plans/done/src-editor-code-smell-audit.md`
"Tier system" for the full definitions. Membership below matches
the Sequencing section at the end of this doc — Tier A == afternoon
pass; Tier B == week pass; Tier C == deferred; Tier D == kept.)

- **Tier A (small, near-zero risk, 5-30 LOC each):** #2, #3, #4
  (regression-class — straight helper replace), #9, #10, #12,
  #13, #14, #15, #16, #17, #19, #20, #23, #24, #27, #36, #37,
  #38, #40, #41, #44.
- **Tier B (moderate, focused pass, 50-200 LOC each):** #1, #5,
  #6, #7, #8, #11, #18, #21, #22, #25, #26, #28, #29, #30, #31,
  #32, #34, #35, #39 (now far smaller — closed #28 dismantling),
  #42, #43, #45, #46, #47.
- **Tier C (high cost or cross-cutting):** #33 (overlay-list
  typedef hoist — touches every UI snapshot consumer; promoted
  from week-pass after sizing review), #48, #49, #50, #53, #54,
  #55, #56, #57.
- **Tier D (kept on purpose / withdrawn):** #52 (withdrawn —
  factually wrong, see finding). The closed audit's #25 (search
  pass-through wrappers) and #12 (cursor blink ownership) also
  remain Tier D.

**Notes on placement (where this audit differs from a naive
red-tier-A rule):**

- **#1 (asymmetric modal capture) → Tier B**, not Tier A: although
  the one-line "add the missing capture call" option exists, the
  cleaner alternative routes through `glr_ctrl_router_handle_escape_key`
  alongside findings #8 and #35 (editor-reaches-into-peer / UI
  state). Bundling matters more than the size delta — see the
  one-week pass.
- **#6 (preflight in `apply_compiled_three_halves`) → Tier B**, not
  Tier A: the diff is small, but it's a *semantic* change (turning
  a partial-commit into a hard fail) that wants paired test
  coverage. Plays well with #28 + #29 (undo-restore semantics) as
  a single transactional-contract PR.
- **#33 (editor → ui layering inversion) → Tier C**, not Tier B:
  earlier drafts kept this in the week pass with a "may slip to
  Tier C if scope grows" caveat. The caveat was the answer —
  hoisting four typedefs out of `ui/app/editor.h` touches every UI
  snapshot consumer (controller + every UI renderer + the editor
  demo's link surface), which is unambiguously cross-cutting.
  Promoted to Tier C from the start so the sequencing isn't
  ambiguous.
- **#37 (`editor_reset_for_new_scene` docstring) → Tier A**, not
  Tier C: earlier drafts inherited the closed audit's #14
  conv.-narrowing framing, but this specific finding is a ~5-line
  docstring addition with one caller. Tier A sizing fits.
- **#52 → Tier D (withdrawn)**: see finding for the factual
  correction.

---

## 🔴 Actual bugs / hazards (verified)

### 1. Asymmetric defensive modal-capture between rename and file-prompt

**Where:** `src/editor/input.c:1471` (`keyboard_func`), `src/editor/input.c:1735` (`special_func`).

**Smell:** Both dispatchers call `editor_input_rename_capture_key(key)` /
`_special(key)` as a defensive duplicate of the controller's
hard-modal capture at `src/app/glr_ctrl.c:3813, 3852`. The matching
file-prompt capture (`editor_input_file_prompt_capture_key` /
`_special`, defined right next to rename at `input.c:992, 1577`)
is **not** called from either editor dispatcher — only the
controller calls it. The comment at L1461-1467 explicitly states
that the defensive translation + capture exists for tests that
call `editor_handle_key` directly. The file-prompt capture is
missing the same defense.

**Why it matters:** Any test that opens the inline file prompt and
then calls `editor_handle_key`/`editor_handle_special` without
going through `glr_ctrl_keyboard` will leak the keystroke past the
modal into the regular editor dispatch chain (search → escape →
undo → cut/paste). The hard-modal contract documented at
`input.h:135-139` is violated for that path. The two modals are
documented as having the "same hard-modal contract" but the editor
enforces only one of them defensively.

**Fix:** Add `if (editor_input_file_prompt_capture_key(key)) return;`
directly after the rename capture at `input.c:1471`, and the
`_special` analog at `input.c:1735`. Alternative: drop the
duplicate rename guard from the dispatchers entirely and rely
solely on controller-side capture, fixing the affected tests to go
through `glr_ctrl_keyboard`.

### 2. `repl_state_document_cmds_mut()` called for a read-only `memcmp`

**Where:** `src/editor/input.c:618` (`commit_progressed_since`).

**Smell:** `memcmp(repl_state_document_cmds_mut(), s->undo.cmds, …)`
invokes the **mutable** accessor to obtain a pointer it only reads.
The const accessor `repl_state_document_cmds()` exists at
`src/repl/state_views.h:136` (and `state_owners.h:17` re-exports it).
The mutable variant is for owner modules that intend to write.

**Why it matters:** Reads through `_mut()` defeat the const/mutable
distinction the typed-facade encodes. New readers cargo-cult the
pattern. If `repl_state_document_cmds_mut()` ever grows side
effects (e.g., bumps a dirty counter), this read-only diff would
silently flip the dirty bit on every navigation commit attempt.

**Fix:** Replace with `repl_state_document_cmds()`. Add an include
for `repl/state_views.h` if not already pulled in (it's not —
`input.c:59` only includes `state_owners.h`).

### 3. `editor_navigate_to_line` evaluates `current_input_needs_navigation_commit` twice

**Where:** `src/editor/input.c:885` (`commit_before_navigation`) →
calls `commit_current_input(0)` → `src/editor/input.c:661`
evaluates the same predicate.

**Smell:** `commit_before_navigation` returns `COMMIT_UNCHANGED`
early if `current_input_needs_navigation_commit()` is false. It
then enters `commit_current_input(0)`, which at L661-662 evaluates
`!enter_mode && !current_input_needs_navigation_commit()` and
returns `COMMIT_UNCHANGED` a second time. Not a bug today — the
predicate is pure and idempotent — but
`current_input_needs_navigation_commit` calls
`input_matches_committed_line()` which calls
`repl_canonical_input_view` on the live committed line. If that
ever becomes side-effecting or expensive (line cache, byte-counted
span), the double call surfaces immediately.

**Why it matters:** Coupling fragile and invisible: a maintainer
adds caching/instrumentation to `input_matches_committed_line` and
the navigation path runs it twice without warning.

**Fix:** Pass the precomputed result into `commit_current_input`
(e.g., add an `int input_needs_commit_hint` parameter, or split
the early-exit branch out of `commit_current_input`).

### 4. Open-coded input clear in `editor_try_commit_var_statements_then_insert` re-introduces what closed #44 fixed

**Where:** `src/editor/commit.c:1308-1312, 1319-1323`.

**Smell:** The function open-codes the input-buffer clear *twice*
in the same function body:

```c
EditorInputState *inp = editor_state_input_mut();
inp->input[0] = '\0';
inp->input_len = 0;
```

The canonical helper `editor_input_clear()` (`state.h:300`) does
exactly this and is already used at `commit.c:139` inside
`apply_post_effects`. Closed audit #44 caught this exact pattern in
`input.c` and replaced it with the helper; the same open-coded
form has now landed in `commit.c` after that closeout.

**Why it matters:** Inconsistent storage discipline. If
`EditorInputState` layout ever gains a third field (e.g., a `dirty`
flag, a `last_committed_len`), the helper updates would silently
miss these two sites. Duplication obscures what's actually
different between the two arms — the trailing status /
`mark_source_dirty` calls (see #22).

**Fix:** Replace each open-coded clear with `editor_input_clear();`
and drop the `editor_cursor_pos_set(0)` call that follows (verify
via the source first whether `editor_input_clear` already resets
the cursor). Add a structural guard `check-no-raw-input-clear` if
there's appetite, mirroring `check-no-raw-undo-clear` that closed
#23 introduced.

### 5. `editor_commit_apply_external_change` callers must each remember to publish `change.commit_message` — silent contract drift vs. `_apply_plan`

**Where:** `src/editor/commit.c:76-92` (`editor_commit_apply_external_change`)
vs. `src/editor/commit.c:183-184` (`editor_commit_apply_plan`).

**Smell:** `editor_commit_apply_plan` ends with
`if (plan->commit_message_valid && plan->commit_message[0]) repl_set_status(plan->commit_message);`.
The sibling helper `editor_commit_apply_external_change` does
**not** publish `change->commit_message` — even though
`ReplCompiledChange` carries its own `commit_message` field
(`compile.h:172`) that compile entries populate. Every production
caller of `editor_commit_apply_external_change` must remember to
call `repl_set_status(change.commit_message)` separately. The
pattern is visible at `src/editor/input.c:245-260`. Closed audit
#15 (Tier C deferred) flagged "error reporting drift" between the
two paths; this is the *success-side* drift, complementary to the
error-side asymmetry.

**Why it matters:** Each new `editor_commit_apply_external_change`
caller has to know to publish status manually. The closest sibling
(`editor_commit_apply_plan`) does it automatically; the asymmetry
is a near-100% chance of forgotten-status bugs in new call sites.
The header doc at `commit.h:45` says "Does not call set_status;
callers surface diagnostics" — but the function's `change`
argument *is* the diagnostic carrier, and the choice to make
callers double-handle it isn't motivated.

**Fix (must be opt-in — see counter-example below):** Add an
explicit publish flag to `editor_commit_apply_external_change`
rather than always publishing. A plain `int publish_status`
parameter (0 = caller surfaces diagnostics, 1 = call
`repl_set_status(change->commit_message)` on success) is enough;
no enum needed unless a third mode appears:

```c
int editor_commit_apply_external_change(const ReplCompiledChange *change,
                                        int capture_undo,
                                        int publish_status);
```

Then the four current "set status manually" call sites in
`src/editor/input.c` and `src/app/glr_ctrl.c` pass `1` and drop
their explicit `repl_set_status(change.commit_message)` line; the
variable-drag site keeps `0`.

**Why a blanket "always publish" is wrong** (counter-example):
the variable-panel drag path at `src/app/glr_ctrl.c:3110-3121`
compiles via `repl_compile_set_predef_value`, which always fills
`commit_message` with `"name = value"` at `src/repl/compile.c:1086`.
The drag intentionally suppresses status on every motion (it would
spam the status bar with the current drag value at frame rate). An
unconditional publish-on-success would change the drag UX,
surfacing the per-motion message on every drag event. With the
opt-in flag, the drag path stays silent by passing `0`.

Coordinates well with #21 (the two-`commit_message`-fields
refactor); the publish-flag plumbing can land alongside the
storage unification.

### 6. `editor_buffer_apply_compiled_change` runs without preflight; `apply_compiled_three_halves` partially commits on malformed input

*Severity note: kept 🔴 because the failure mode is concrete and
the existing in-file `MAX_COMMIT_CMDS` clamp at `state.c:204` is
asymmetric with the unclamped `change->count` at `state.c:222`
(a real one-edit-away misbehavior). However, both current public
callers preflight first — see the "Why it matters" section — so
no production code path triggers the partial-commit window today.
Reasonable to read as "🔴 latent" or "🟡 defense-in-depth" depending
on house convention; this audit leaves it at 🔴 because the
asymmetric clamp by itself is wrong regardless of caller
behavior.*

**Where:** `src/editor/state.c:198-229` (no preflight + ignored
return); `src/editor/commit.c:61-68` (`apply_compiled_three_halves`).

**Smell:** `editor_buffer_apply_compiled_change` performs the
editor-buffer mutation immediately on entry — no validation of
`change->kind`/`pos`/`count` against `MAX_COMMIT_CMDS` and
document bounds. The call sequence in `apply_compiled_three_halves`
is:

```c
repl_apply_predef_ops(change);
repl_apply_scratch_ops(change);
editor_buffer_apply_compiled_change(change);  /* return DROPPED */
int edit_line = editor_state_edit_line();
if (repl_apply_compiled_change(change, &edit_line))  /* has internal preflight; can fail */
    editor_state_edit_line_set(edit_line);
```

`repl_apply_compiled_change` defensively preflights and bails
(`src/repl/apply.c:80`). The editor side does not. If a malformed
change reaches `apply_compiled_three_halves`, predefs + scratch +
editor buffer mutate while REPL bails — half-committed
transaction. The defensive `i < MAX_COMMIT_CMDS` clamp inside
`editor_buffer_apply_compiled_change`'s pointer-build loop
(state.c:204) is asymmetric with the *un*clamped `change->count`
passed to `editor_buffer_insert_lines` on line 222 — if
`change->count > MAX_COMMIT_CMDS` (legal only for `DELETE_RANGE`
per `compile.c:130` comment), `INSERT_MANY` would read
uninitialized `line_ptrs[MAX_COMMIT_CMDS..]`.

**Why it matters:** The two public callers
(`editor_commit_apply_external_change`, `editor_commit_apply_plan`)
preflight first, so a malformed change never reaches
`apply_compiled_three_halves` in production. But: (a)
defense-in-depth is missing exactly where it would catch a
regression — adding a third caller would silently break the
transaction guarantee; (b) the editor-side helper is documented as
"Mutates EditorState text only" (`compile.h:15`) but actually
mutates without checking that the change is well-formed.

**Fix:** Either (a) lift the
`repl_apply_can_apply_compiled_change(change)` preflight into
`apply_compiled_three_halves` so it bails before any of the three
sides mutate; or (b) make `editor_buffer_apply_compiled_change`
preflight internally and have `apply_compiled_three_halves` check
its return — then mirror the predef/scratch failure path. Either
way, the loop-clamp at `state.c:204` should match the bound passed
to `editor_buffer_insert_lines` (clamp the count too, or drop the
loop-bound clamp).

### 7. New `accept` field on `EditorCompletionProvider` has no test coverage (regression risk from closed #9)

**Where:** `tests/test_editor_completion.c:23-27` (provider literal
omits `accept`); `tests/test_editor_completion.c:80-91` (test 7
"Partial provider (NULL functions)" never calls
`editor_completion_accept`); `src/editor/completion.c:35-38` (the
dispatch wrapper added by closed #9).

**Smell:** Closed #9 added `void (*accept)(void);` to
`EditorCompletionProvider` and the matching
`editor_completion_accept()` dispatch wrapper. The dedicated test
file for this seam was not updated — `g_test_provider` is a
3-field initializer that *implicitly* zero-inits `accept` to NULL,
but no test ever calls `editor_completion_accept()` to verify
either (a) it forwards to the registered hook, or (b) it is
NULL-safe with a partial provider. The 19
`editor_completion_*` references in `tests/test_repl_autocomplete.c`
use the real REPL provider end-to-end and never construct a custom
provider for the seam itself.

**Why it matters:** Production calls `editor_completion_accept()`
from two sites (`input.c:1239`, `input.c:1326`). If a future
contributor regresses the NULL guard at `completion.c:36` (or
moves `accept` after the body of an inline NULL check), the
partial-provider Tab/Enter path will crash on systems with no
completion provider installed (e.g., `editor_demo`, future
alternate REPL configurations). The other three dispatch wrappers
(`_update`, `_update_selected_preview`, `_clear`) are protected by
test 7's "should not crash" coverage; `_accept` is the only one
without it.

**Fix:** In `tests/test_editor_completion.c`, (a) add
`.accept = test_accept,` to `g_test_provider` with a counter;
(b) add a "Dispatch: accept" test block mirroring tests 3-5;
(c) extend test 7 to also call `editor_completion_accept()`.
Roughly 15 LOC; all three are direct copies of existing test
patterns.

---

## 🟡 Drift / boundary hazards

### 8. Editor reaches into peer subsystems and UI state from input dispatch — new layering inversion since #8 closed

**Where:** `src/editor/input.c:52`
(`#include "subsystems/color_picker/color_picker_state.h"`);
calls at `input.c:1013` (`color_picker_stop()`), `input.c:1017-1020`
(writes `ui_state_help_mut()->visible`, `editor_help_session_set_*`),
`input.c:905-913` (writes `ui_state_status_mut()` directly to
save/restore status text across a rollback).

**Smell:** The closed audit's #8 hoisted four `glr_*` includes out
of `input.c`. The current code has the same shape of inversion in
three new directions:
- `color_picker_stop()` from the Escape route reaches into a peer
  subsystem (`src/subsystems/color_picker/`) — peer subsystems are
  routed by the controller, per `MODULES.md` L350-364, not by the
  editor.
- `ui_state_help_mut()->visible = 0` is editor-side mutation of
  UI chrome state. The matching mutator path through
  `src/app/glr_ctrl.c` (similar at L2900) is the canonical one.
- `ui_state_status_mut()` save/restore (L905-913) is an
  opportunistic hack: the editor manually preserves UI status
  across a rollback because there is no `repl_status_capture/restore`
  API. A future field on `UiStatusState` (color, icon, severity)
  silently won't roll back.

**Why it matters:** The `check-editor-no-app` ratchet caught the
`app/glr_*` includes but doesn't see `subsystems/*` or `ui/app/*`
includes. The architectural promise of "editor talks to the world
through `EditorInputDispatchEffects`" (`input.c:30-34`) is
partially false today.

**Fix:** Route Escape's color-picker dismissal through a
`glr_ctrl_router_handle_escape_key` before `editor_handle_key`,
in the same shape as the existing tutorial-ack router. Hoist the
help-overlay close into `glr_ctrl_router_handle_escape_key` too
(or expose a `glr_ctrl_close_help_overlay` editor effect). For the
status save/restore, add a narrow `repl_status_capture_state` /
`_restore_state` API or extend `EditorInputDispatchEffects` with a
`clear_status_after_rollback` flag handled controller-side.

### 9. `parse_for_overwrite_enter` name no longer matches its two call sites

**Where:** `src/editor/input.c:448` (def), `input.c:810`
(overwrite-Enter site), `input.c:858` (append-Enter site,
edit_line ≥ doc_count).

**Smell:** The function name says `for_overwrite_enter`, but the
docstring at L426-434 admits the function is also used by the
"append-at-end Enter path." The second call site at L858 isn't
an overwrite — it's append. The name is a documentation lie.

**Why it matters:** Future readers who see
`parse_for_overwrite_enter(repl_state_document_count(), ...)`
(an append, not an overwrite) will assume the call is wrong. The
closed audit's #18 explicitly chose not to consolidate the
strict/permissive parse APIs (deferred Tier C); the name should at
least describe what survives.

**Fix:** Rename to `parse_for_enter_with_visible_vars` or
`editor_parse_input_for_enter`, matching the docstring's actual
two-path description.

### 10. Doc-comment for `parse_for_overwrite_enter` is detached from its function by `warn_if_scope_truncated`

**Where:** `src/editor/input.c:426-446`.

**Smell:** The block comment at L426-434 documents
`parse_for_overwrite_enter`. Immediately below at L435 is a
*second* block comment for `warn_if_scope_truncated`, whose
definition starts at L439. The first comment is now 22 lines above
the function it describes. A reader scanning by eye attaches
L426-434 to the immediately-following symbol, which is
`warn_if_scope_truncated` — wrong function.

**Why it matters:** Documentation drift that misattributes a
docstring to a sibling helper. Trivially confusing.

**Fix:** Move the L426-434 block down to immediately above the
`parse_for_overwrite_enter` definition at L448.

### 11. `editor_clear_all_cmds` and `editor_reset_for_new_scene` share a 7-line tail with diverging intent

**Where:** `src/editor/input.c:268-285` vs. `src/editor/input.c:287-297`.

**Smell:** Both functions issue the same sequence:
`repl_command_store_clear → editor_buffer_clear → edit_line=0 →
insert_mode=0 → editor_input_clear → editor_pending_newline_clear
→ repl_eval_init_predef_vars → repl_mark_source_dirty`.
`editor_clear_all_cmds` wraps it with
`tutorial_guard_source_change_or_status` +
`editor_undo_push_snapshot` + a status message;
`editor_reset_for_new_scene` is the bare reset. The 7-line
clear-step is duplicated verbatim.

**Why it matters:** Any future "wholesale reset" step (e.g.,
clearing the autocomplete state, dropping the search query) needs
to be added in two places. Easy to update one and miss the other;
closed audit's #50 (reformat save set) flagged the same pattern.

**Fix:** Extract `static void editor_reset_document_to_empty(void)`
containing the 7-line tail; both wrappers call it. Both surfaces
keep their own pre/post effects.

### 12. `editor_committed_line_text` is a defensive wrapper equivalent to `editor_buffer_line`

**Where:** `src/editor/input.c:299-302` (definition); L314/L353/L595
(callers).

**Smell:** `editor_committed_line_text(idx)` returns
`(text && text[0]) ? text : ""`. But `editor_buffer_line` (its
sole source, at `state.c:86-90`) returns `""` for invalid idx and
never returns NULL (the buffer is `char[][MAX_LINE_LEN]`). The
`text &&` branch is dead; the `text[0] ? text : ""` returns `""`
when the buffer is already empty (semantically identical to
returning a pointer to a NUL byte). The wrapper does nothing the
underlying call doesn't do.

**Why it matters:** Three callers go through this wrapper for no
reason; new readers think there's something special about
"committed" line text vs. raw buffer line text. There isn't.

**Fix:** Delete the wrapper; replace the three callers with
`editor_buffer_line(idx)` directly.

### 13. `enter_parse_err` vs. `editor_parse_err` — same buffer, two names

**Where:** `src/editor/input.c:458` (`editor_parse_err`),
`input.c:729` (`enter_parse_err`).

**Smell:** Two parsing helpers (`parse_for_overwrite_enter` and
the inline insert-mode parser in `commit_current_input`) each
declare a `char[REPL_STATUS_TEXT_MAX]` for the parser err_buf.
They use different names for the same semantic slot.

**Why it matters:** A grep for "parse error capture" returns two
hits that look unrelated. A future cleanup pass that promotes the
parse path into a single helper has to reconcile both names anyway.

**Fix:** Standardize on one name (`parse_err_buf` or
`editor_parse_err`).

### 14. Insert-mode Enter parse error uses `repl_set_status`, sibling `parse_for_overwrite_enter` uses `repl_set_status_error` — new instance of closed #15

**Where:** `src/editor/input.c:494`
(`repl_set_status_error(editor_parse_err)` in
`parse_for_overwrite_enter`) vs. `input.c:762`
(`repl_set_status(enter_parse_err)` in the insert-mode branch of
`commit_current_input`).

**Smell:** Two structurally-identical parse paths report errors
through different status sinks. The closed #15 (Tier C deferred)
flagged "error reporting drifts across handlers"; this is a new
instance of the same pattern in the same module.

**Why it matters:** Red-status semantics (introduced for
`repl_set_status_error`) distinguish errors from informational
status. Whether a parse failure under Enter shows red depends on
which dispatch path you took (overwrite vs. insert mode), not on
the kind of error. End-user-facing inconsistency.

**Fix:** Pick one — almost certainly `repl_set_status_error` since
both are reporting a parse failure. One-line change at L762.

### 15. `EditorUndoRingState` docstring claims test-only usage; production code uses it

**Where:** `src/editor/undo.h:70-74` (docstring), `src/editor/input.c:897, 909`
(production callers).

**Smell:** `undo.h:70-74` says ring-state capture/restore is "Used
by tests that verify undo/redo ordering" and L33-34 says "allow
tests and tools to query/restore." But `commit_before_navigation`
in `input.c` uses both to roll back the undo ring after a rejected
navigation commit — this is production code, not test.

**Why it matters:** A future cleanup that "deletes the test-only
API" because the docstring says it's test-only will break
navigation rollback silently. The doc is wrong about scope.

**Fix:** Update `undo.h:33-34` and L70-74 to mention the production
rollback caller in `src/editor/input.c::commit_before_navigation`.
Better: name the production-friendly intent: "Used to
capture/restore the ring head/count across an aborted transaction
that pushed snapshots which must be rolled back."

### 16. `editor_input_set_text` exists; `editor_feed_line` open-codes the same write (closed #46 miss)

**Where:** `src/editor/input.c:1506-1513` (`editor_feed_line` opens
with a 4-line strncpy + len/cursor block); helper
`editor_input_set_text` at `src/editor/state.c:352-357`.

**Smell:** Closed #46 replaced the Tutorial-Tab branch's open-coded
buffer write with `editor_input_set_text(text)`. The exact same
open-coded pattern lives at the head of `editor_feed_line`. The
audit fix touched the Tutorial-Tab site but not this one.

**Why it matters:** Mechanical regression risk — the next
contributor reading L1506-1513 won't know `editor_input_set_text`
exists. The audit's principle of "use the existing helper" applies
identically here.

**Fix:** Replace the four-line block at L1507-1513 with
`editor_input_set_text(line);`.

### 17. `edit_ops.h` docstring drift: `_insert_char_at_cursor` and `_delete_right_of_cursor` claim "Pure with respect to selection"; `_delete_left_of_cursor` is silent

**Where:** `src/editor/edit_ops.h:37-52`.

**Smell:** Three buffer-only primitives. Two of three have a "Pure
with respect to selection: the anchor (if any) is left untouched"
sentence in their docstring. `edit_op_buffer_delete_left_of_cursor`
(L43-46) has no such claim, even though its implementation at
`edit_ops.c:60` uses `editor_cursor_pos_set_keep_anchor` — exactly
the same anchor-preserving move as `_insert_char_at_cursor`. The
contract is identical; only one docstring states it.

**Why it matters:** A reader can't tell from the docstring alone
whether `_delete_left` is supposed to preserve the anchor or not.
The implementation comment at `edit_ops.c:57-59` refers them to
"see comment in edit_op_buffer_insert_char_at_cursor" — but the
header is the public contract. If a future maintainer changes the
implementation to `editor_cursor_pos_set` (without `_keep_anchor`),
they will not realize they're changing a documented promise.

**Fix:** Add the missing sentence to L43-46. Or move the
"buffer-only primitives leave the anchor untouched" sentence up
into the L35 section header so it applies to all three.

### 18. New instance of closed #18 in `editor_compile_for_loop`

**Where:** `src/editor/commit.c:835` (`collect_visible_vars` call)
+ `src/editor/commit.c:1012-1028` (body parse with
`repl_parser_parse_command_ctx`).

**Smell:** Closed #18 (Tier C deferred) listed 5 sites of the
canonical `collect_visible_vars(insert_idx, ...) → parse_command_ctx
→ editor_place_parsed_command → status` sequence.
`editor_compile_for_loop` is now a *sixth* instance — line 835
collects visible vars; lines 1000-1028 build a `dv[]` array from
loop-var + visible vars, then call `repl_parser_parse_command_ctx`
with strict-refs on. The audit's planned helper
`parse_for_commit(insert_idx, ..., int strict_refs, int preserve_var_exprs)`
would absorb this site too.

**Why it matters:** The deferred helper extraction now has 6+ call
sites and gains weight. Each additional instance adds to the
refactor surface; the for-loop variant in particular adds the
"loop scope" (var_name + start value) dimension that the helper
signature must accommodate.

**Fix:** When extracting `parse_for_commit` per closed #18's plan,
add a `const ExprVar *extra_scope, int extra_scope_count`
parameter (or a small `ExprVar scope[]` builder) so
`editor_compile_for_loop`'s loop-scope prefix can be passed in.
Update the closed audit's afternoon-sequencing item for #18 to
name this sixth site.

### 19. File-top docstring at `commit.c:6-23` is stale — describes the pre-`apply_compiled_three_halves` flow

**Where:** `src/editor/commit.c:1-30` (file header).

**Smell:** The file header reads:

```
editor_commit_apply_external_change(change, capture_undo)
    preflight repl_apply_can_apply_compiled_change(change)
    services.apply_predef_ops(change)            // predef-var cascade
    editor_buffer_apply_compiled_change(change)  // editor text buffer
    services.apply_repl_change(change)           // ReplState only

The mutating halves go through the EditorServices table so the
editor doesn't reach into `repl_apply_*` directly.
```

Wrong twice over: (1) `commit.c` no longer goes through
`EditorServices` at all — `apply_compiled_three_halves`
(`commit.c:61-68`) calls `repl_apply_predef_ops`,
`repl_apply_scratch_ops`, `editor_buffer_apply_compiled_change`,
`repl_apply_compiled_change` directly. Closed #28 step 1 landed.
(2) The docstring lists 3 mutation steps but the real sequence is
4 (adds `apply_scratch_ops`).

**Why it matters:** The first paragraph a new contributor reads
when opening `commit.c` is now lying about both the call path
(services vs. direct) and the operation count. Future reads will
hunt for the `EditorServices` indirection that no longer exists in
this file.

**Fix:** Rewrite the file header to reflect the current shape —
direct `repl_apply_*` calls via `apply_compiled_three_halves`,
the four-step sequence, and the fact that `EditorServices` is only
consulted from `input.c` (and that closed #28 plans its
dismantling). One-paragraph fix; can ride alongside the #39
(EditorServices dismantling) work.

### 20. `apply_compiled_three_halves` is misnamed (does 4 ops, not 3)

**Where:** `src/editor/commit.c:53-68`.

**Smell:** Function name says "three_halves" but the body executes
4 distinct ops:
1. `repl_apply_predef_ops(change)`
2. `repl_apply_scratch_ops(change)`
3. `editor_buffer_apply_compiled_change(change)`
4. `repl_apply_compiled_change(change, &edit_line)`

The doc-comment at L53-60 *correctly* lists "predef-ops +
scratch-ops + editor-buffer + cmd-store halves" — four items.

**Why it matters:** Grep-archaeology footgun. The name encodes a
count that's wrong; future contributors who add an additional
apply step will look at the name and assume it's already at
capacity.

**Fix:** Rename to `apply_compiled_change_all_halves` or
`apply_compiled_change_full` (drop the count). Mechanical — single
static function, one TU.

### 21. Two `commit_message` fields with no documented layering

**Where:** `src/editor/commit.h:132` and `src/repl/compile.h:172`.

**Smell:** Both structs carry a
`char commit_message[REPL_STATUS_TEXT_MAX]`. The `editor_compile_*`
structured-block compile functions (e.g., `editor_compile_close_brace`
at `commit.c:302-304`) write the *outer*
`EditorCommitPlan.commit_message`. The pure-REPL
`repl_compile_float_decl` / `repl_compile_var_assign` write the
*inner* `ReplCompiledChange.commit_message`. The thin editor
wrappers in `editor_try_commit_float_decl` (`commit.c:1167-1171`)
and `editor_try_commit_assign_variable` (`commit.c:1210-1213`)
manually `snprintf` from the inner to the outer because only
`EditorCommitPlan.commit_message` is what `editor_commit_apply_plan`
publishes.

**Why it matters:** Two storage locations for the same datum with
no documented contract. When extending — e.g., a future "block
commit publishes a richer status with line range" — the
contributor has to know which of the two fields wins, and that
they need to copy between them.

**Fix:** Pick one canonical location. Easiest: have
`editor_commit_apply_plan` publish `plan->change.commit_message`
instead of `plan->commit_message`; drop the outer field; update
structured-block compile functions to write `out->change.commit_message`
(and `out->change.commit_message_valid` — or replace `commit_message_valid`
with non-empty-string check). Coordinates well with #5's fix.

### 22. Asymmetric arms of `editor_try_commit_var_statements_then_insert` are undocumented load-bearing

**Where:** `src/editor/commit.c:1305-1331`.

**Smell:** The two arms (float-decl, assign) differ in **three**
ways:
- Assign arm calls `repl_set_status("Insert mode")`; float-decl arm
  does not.
- Assign arm calls `repl_mark_source_dirty()`; float-decl arm does
  not (but both `editor_try_commit_float_decl` and
  `editor_try_commit_assign_variable` already call it internally —
  L1177 and L1220, so the *outer* call on the assign arm is
  redundant *and* the float-decl arm gets dirty marking anyway).
- The trailing `repl_set_status` on the assign arm overwrites the
  per-assign message from `editor_commit_apply_plan` with the
  generic "Insert mode"; the float-decl arm leaves the per-decl
  message ("Declared variable …") visible.

The single comment at L1303-1304 acknowledges the asymmetry but
doesn't explain **why** — intentional UX, or oversight from the
`_then_insert` extraction?

**Why it matters:** Closed audit's #14 renamed
`editor_try_assign_variable` → `editor_try_commit_assign_variable`
for naming consistency; this finding is the *behavior* equivalent.
The `repl_mark_source_dirty()` on the assign arm is **redundant**
(called twice — once inside `editor_try_commit_assign_variable` at
L1220, once at L1327). A reader sees both calls and assumes both
are required.

**Fix:** (a) Extract a shared post-`_then_insert` epilogue
(`set_insert_mode_clear_input_clear_completion`) the two arms share
— fixes #4 too. (b) Decide whether "Insert mode" should clobber
both arms (consistent UX), only assign (current), or neither;
document the rationale at the function's docstring. (c) Drop the
redundant `repl_mark_source_dirty()` at L1327. Add a tiny test in
`tests/test_repl_editor.c` pinning the per-arm status text.

### 23. `EditorCommitPostEffects.end_type` docstring claims a use that doesn't exist

**Where:** `src/editor/commit.h:108-111` vs. `src/editor/commit.c:127`.

**Smell:** The field docstring reads:
> CmdType-shaped value identifying the block kind. Used by the
> func-decl-resume guard ("only fire on CMD_FUNC_END") and by
> status-message formatting.

But grep shows `end_type` is *only* read at `commit.c:127` for the
resume-guard check. It is not consulted by any status-message
formatter. `editor_compile_close_brace` writes its own status text
(L302-303) using a local `label` variable — not `end_type`.

**Why it matters:** The docstring is a contract lie. A future
contributor who needs the "block kind" for some formatting purpose
will assume the field is the source of truth, write code that
depends on it, and then discover that no formatter actually reads
it.

**Fix:** Strike "and by status-message formatting" from
`commit.h:108-111`. Or, if it's intentional aspiration: add a
status-formatter helper that uses `end_type` and document the
policy.

### 24. Header docstring at `commit.h:167-170` describes a return-shape contract that's literally wrong

**Where:** `src/editor/commit.h:167-170`.

**Smell:**
> Editor-side compile entry points. These are the structured-block
> counterparts to repl_compile_*; **they return EditorCommitPlan**
> (REPL change + editor effects) rather than bare ReplCompiledChange.

But the function signatures one line below return `ReplCompileResult`
(a status enum) and **write** `EditorCommitPlan` via the `out`
pointer.

**Why it matters:** Reading the docstring before reading the
signature gives the wrong mental model. Trivial drift but
live-bait for the next grep-driven rename.

**Fix:** "…they write `EditorCommitPlan` (REPL change + editor
effects) via the `out` pointer rather than bare `ReplCompiledChange`."

### 25. `editor_compile_*` err-buffer null-check policy diverges across the four entries

**Where:** `src/editor/commit.c:246-247` (close_brace) vs.
`commit.c:344, 360, 376, 394` (if_block), `commit.c:616, 627, 668, 723`
(func_def), `commit.c:845, 903, 912-927, 995, 1024-1026` (for_loop).

**Smell:** `editor_compile_close_brace` is the only one that guards:
`if (err && err_size > 0) snprintf(err, ...)`. The other three call
`snprintf(err, (size_t)err_size, ...)` unconditionally — passing
NULL would crash. Today every production caller is
`editor_try_commit_block` (`commit.c:1240-1244`) which passes a
real buffer, so no crash, but the contract is silent and
inconsistent.

**Why it matters:** If a test or future caller passes NULL err,
three of four entries crash and one returns cleanly. The header
`commit.h:172-224` declarations do not document `err` as
required-non-NULL.

**Fix:** Either (a) document `err` as non-NULL in the header for
all four and drop the close-brace guard; or (b) add
`if (err && err_size > 0)` guards uniformly to the other three.
Either is fine — picking one resolves the drift.

### 26. Func-def alias-aware pre-step duplicated between `editor_compile_func_def` and `repl_compile_func_def`

**Where:** `src/editor/commit.c:576-643` vs.
`src/repl/compile.c:1705-1755`.

**Smell:** ~67 lines of nearly-identical "look at trimmed input,
walk identifier chars, look up alias, validate name, pick target
slot, register, publish into change" exist in two TUs. They
subtly diverge:

| Aspect | editor side | repl side |
|---|---|---|
| Identifier walk | `repl_eval_is_ident_continue`/`_start` | open-codes `isalpha`/`isalnum` (the predicate closed #45 fixed only on the editor side) |
| Slot picking | reuses existing slot on overwrite (L608-612) | always picks "first free" (L1727) |
| Error context | takes `ReplCompileContext` for cursor position | doesn't have cursor — used from `load_try_block` |

Closed #11 (Tier C — done with doc-fix only) noted the broader
"duplicate compile/apply" issue across the editor/repl pair; this
finding identifies a specific large duplication block that wasn't
called out separately.

**Why it matters:** A bug-fix in alias rejection (e.g., new
reserved-name policy, name-length increase) requires touching both
sites; the predicate-helper divergence is already evidence that
drift has happened. The editor side's overwrite-slot-reuse logic
also looks like it should be the canonical behavior — the repl
side may be silently dropping that capability when reached from
load.

**Fix:** Extract the shared core into a single helper in
`src/repl/compile.c`:
`repl_compile_func_def_resolve_alias(const ReplCompileContext *ctx, const char *trimmed, ReplCompiledChange *out, char *err, int err_size)`.
Both `editor_compile_func_def` and `repl_compile_func_def` call
it. The `ReplCompileContext` carries enough to support the
editor's slot-reuse policy (the cursor lives in `ctx->edit_line`);
the loader path passes the same ctx with `insert_mode=0` and gets
the same behavior. Cross-references closed #11 — they're
complementary.

### 27. Stale comment claims `editor_commit_func_decl_resume_set` doesn't need a cross-TU wrapper (it's the cross-TU wrapper)

**Where:** `src/editor/commit.c:156-159`.

**Smell:**
```c
/* editor_commit_func_decl_resume_set: defined below alongside the
 * file-private g_func_decl_resume_delta storage. The resume
 * bookkeeping is local to this file, so no cross-TU setter wrapper is
 * needed. */
```

But `editor_commit_func_decl_resume_set` is publicly declared at
`commit.h:145` with its own docstring "Encapsulates the shared
resume bookkeeping so callers stop poking at storage directly." It
**is** the cross-TU setter wrapper. The internal comment was once
accurate (when the function was private) and is now wrong.

**Why it matters:** Contradictory contracts between the .c and .h
files. A reader gets one story from the .c saying "local TU, no
need for wrapper" and another from the .h saying "encapsulates
shared bookkeeping for cross-TU callers."

**Fix:** Delete the L156-159 comment block entirely —
`commit.h:140-145` already documents the function.

### 28. `editor_undo_snapshot_restore` bypasses the cross-generation safety net

**Where:** `src/editor/undo.c:62-85` (`editor_undo_snapshot_restore`);
`src/editor/input.c:586` (production caller).

**Smell:** The generation safety net (closed #23) is implemented at
the **ring level** — `editor_undo_pop_snapshot` (undo.c:142-149)
and `editor_undo_do_redo` (undo.c:174-181) check
`snapshot->generation != g_undo_generation` before restoring. The
**primitive** `editor_undo_snapshot_restore` is publicly exported
and called directly by `input.c:586` (the commit-attempt rollback),
and it does *not* check generation. Callers who hold a snapshot
through code paths that could trigger a wholesale replacement
(scene load, workspace load, full reset) will restore foreign-world
state — exactly the bug closed #23 was added to prevent.

**Why it matters:** Today the `input.c` call pair (capture at L570,
restore at L586) is bounded within a single keystroke and no
wholesale replacement can intervene, so this is latent. But the
docstring in `undo.h:33-37` advertises
`editor_undo_snapshot_save/restore` as a general-purpose primitive
for "import/export to preserve full state" — a future caller that
holds a snapshot across a frame boundary, or across a menu action,
will hit the cross-generation bug the safety net was designed to
catch.

**Fix:** Have `editor_undo_snapshot_restore` consult
`snapshot->generation` against `g_undo_generation` and refuse with
a status / no-op if they differ. The ring-level callers
(`pop_snapshot` / `do_redo`) already check before calling, so the
primitive's check is redundant for them but closes the hole for
everyone else.

**Note on `editor_undo_ring_state_restore` (a related hole, not the
same fix):** the ring-state restorer at `undo.c:94-99` has the same
"resurrects cross-generation snapshots" failure mode, but the
fix shape is different — `EditorUndoRingState` at `undo.h:75-80`
only stores `undo_head`, `undo_count`, `redo_head`, `redo_count`.
There is no captured generation field to compare against. Closing
that hole requires either:

1. **Add a `generation` field to `EditorUndoRingState`**, populated
   at capture time and checked at restore. Touches the struct
   layout (and any test that initializes the struct positionally),
   so it's a separate, slightly larger Tier B change. The capture
   site at `undo.c:88-93` would set
   `state->generation = g_undo_generation;`; the restore at
   `undo.c:94-99` rejects when `state->generation != g_undo_generation`.
2. **Narrow `editor_undo_ring_state_restore` to test-only scope**
   (it's already test-scaffolding per closed audit's `undo.h:70-74`
   docstring; finding #15 in this audit notes that
   `EditorUndoSnapshot` capture/restore *also* has a production
   caller, but the ring-state pair only has test callers — verify
   with `rg "editor_undo_ring_state_(capture|restore)" src/`).
   If test-only, the cross-generation hazard is bounded and the
   docstring can simply warn.

This finding closes only the `editor_undo_snapshot_restore` half;
the ring-state half should be tracked as a separate Tier B item
once the snapshot fix lands.

### 29. `editor_undo_snapshot_restore` performs side effects the header doesn't admit

**Where:** `src/editor/undo.c:62-85` (body); `src/editor/undo.h:14-18, 86-87`
(contract).

**Smell:** The header docstring at `undo.h:14-18` explicitly
enumerates what `editor_undo_snapshot_restore` does **not** restore:
"input-buffer text, selection, clipboard, search, autocomplete, and
scroll position. Those are transient view / editing state; an undo
restores the document, not the cursor's in-progress typing." But
the body (lines 82-84) performs three side effects that are *not*
restoration of snapshotted state:

```c
editor_insert_mode_set(0);                              /* mutates input state */
editor_load_line_to_input(editor_state_edit_line());    /* mutates input state */
repl_mark_source_dirty();                               /* flatten/render side effect */
```

These are not symmetric "restore the saved value" operations —
they're *resets* paired with the restore.

**Why it matters:** A test calling `editor_undo_snapshot_save`
then `editor_undo_snapshot_restore` expecting a perfect round-trip
will find `insert_mode=0` and the input buffer reloaded — not what
they captured. The function name `snapshot_restore` implies
symmetry it doesn't deliver.

**Fix:** Either (a) split the primitive into
`editor_undo_snapshot_restore_for_undo()` (with the post-restore
resets) for the ring path, plus a clean
`editor_undo_snapshot_restore_strict()` for the snapshot-only path
used by tests/import-export; or (b) expand the `undo.h:14-18`
docstring to acknowledge the post-restore reset of insert_mode +
input buffer + dirty flag.

### 30. `editor_state_clipboard()` returns ~1 MB by value but only tests use it; production reads via `_mut()`-as-`const`

**Where:** `src/editor/state.h:377` (declaration); `src/editor/state.c:524-526`
(impl); `src/editor/clipboard.c:386` (production usage pattern).

**Smell:** `editor_state_clipboard()` returns the whole
`EditorClipboardState` by value — that's
`lines[MAX_COMMANDS][MAX_LINE_LEN]` (~1 MB) + input_text + ints.
No production caller uses it; only `tests/test_repl_state.c:277-279`.
Meanwhile, production code at `clipboard.c:386` needs a const read
of the clipboard payload and does:

```c
const EditorClipboardState *cb = editor_state_clipboard_mut();
```

— using `_mut()` to obtain a pointer it treats as `const`, because
the slice has no const-pointer getter. The API exposes two
equally-wrong shapes: a 1 MB-by-value getter and a mut-only
pointer getter.

**Why it matters:** Closed #35 fixed the same problem for
`editor_state_search()` (now returns `const EditorSearchState *`).
The clipboard slice missed the same treatment despite carrying
~1000× more state.

**Fix:** Mirror the search-slice fix. Change
`editor_state_clipboard()` to return `const EditorClipboardState *`;
have `clipboard.c:386` use that instead of `_mut()`. Tests that
consume by-value can dereference.

### 31. `editor_state_autocomplete()` returns ~2 KB by value, called for `match_count > 0` checks (closed #35 miss)

**Where:** `src/editor/state.h:403`; `src/editor/state.c:614-616`;
hot callers at `src/editor/input.c:1021, 1238, 1325` and
`src/app/glr_ctrl.c:1607`.

**Smell:** `EditorAutocompleteState` size ≈ 10 × 8 + 10 × 8 + 8 +
`ghost[MAX_LINE_LEN=1024]` + `hint[MAX_LINE_LEN=1024]` = ~2.2 KB.
The four hot callers above only read `match_count`:

```c
if (editor_state_autocomplete().match_count > 0)
```

Each call copies 2 KB to read 4 bytes. Same pattern as closed #35.

**Why it matters:** Per-keystroke hot path. `input.c:1238/1325`
are inside the Tab/Enter dispatch — fired on every keystroke.
Multiply by frame rate for `glr_ctrl.c:1607` which runs in
`glr_ctrl_router_*` dispatch.

**Fix:** Change `editor_state_autocomplete()` to return
`const EditorAutocompleteState *`, mirroring the closed search fix.
Hot callers become `editor_state_autocomplete()->match_count > 0`.

### 32. `_count_set(0)` and `editor_state_clipboard_clear()` are aliases; clipboard.c uses both

**Where:** `src/editor/state.c:559-562` (`_count_set(0)` calls
`_clear()`); `src/editor/clipboard.c:300, 323` use `_count_set(0)`;
tests use `_clear()`.

**Smell:** The two functions do exactly the same thing —
`_count_set(0)` immediately delegates to `_clear()`. clipboard.c
picks `_count_set(0)`; tests use `_clear()`. Neither name clearly
says "reset the entire clipboard regardless of what kind it
currently holds." A contributor reading `clipboard.c:300`
(`editor_state_clipboard_count_set(0)`) would reasonably assume it
only touches the line-payload, not the input-text payload — and
would be wrong (`state.c:559-562`'s `_count_set(0)` resets BOTH
payloads).

**Why it matters:** Naming-vs-behavior footgun similar to closed
#6.

**Fix:** Either (a) replace the two `_count_set(0)` calls in
clipboard.c with `editor_state_clipboard_clear()` so the intent
("nuke everything") is obvious at the call site; or (b) delete
`editor_state_clipboard_clear()` (it has zero production callers
besides being delegated to from `_count_set(0)` and tests), make
`_count_set(0)` the single canonical reset, and rename it
`editor_state_clipboard_reset()` to match the behavior.

### 33. `editor → ui` layering inversion via UI typedefs embedded in `EditorState`

**Where:** `src/editor/state.h:6-8` (include); `state.h:190-193` (fields).

**Smell:** `editor/state.h` includes `ui/app/editor.h` to obtain
`UiTransformerList`, `UiHighlightList`, `UiVirtualLineList`,
`UiLineOverrideList`. The editor (layer 2) thereby depends on UI
(layer 3) for the structural definition of its own state. The
four fields embedded in `EditorState` are sized at ~1.2 MB total
(closed #48 documented this); they are per-frame transients
populated by the controller and consumed by UI renderers — the
editor itself doesn't read or write them in any non-trivial way.

**Why it matters:** The `MODULES.md` four-role contract is "editor
uses UI as its view" (i.e., editor depends on UI for *rendering
services*) — but here the editor depends on UI for *data structure
layouts*. Closed #48 fixed the docstring; the structural issue
remains: types named `Ui*` are embedded in `EditorState`. Future
readers see `EditorState.transformers` (named with a UI prefix) and
may assume the editor owns rendering. Also, the `editor_demo` tool
links all of `EditorState` even though it doesn't use any overlay
system — paying ~1.2 MB BSS for unused capability.

**Fix:** Two options. (a) Hoist the overlay-list typedefs out of
`ui/app/editor.h` into a neutral header (e.g., `src/editor/overlays.h`
or a header in `src/ui/core/`) and rename them away from the `Ui*`
prefix to reflect what they actually are — controller-pushed
per-frame snapshots, not UI state. UI keeps consuming them; editor
stops needing to include from `ui/app/`. (b) Move the four fields
off `EditorState` into a separate per-frame transient struct the
controller owns directly.

### 34. `inline_file_prompt_begin` and `inline_rename_begin` have opposite "already active" semantics — and the header lies about it

**Where:** `src/editor/inline_rename.c:31-42` vs.
`src/editor/inline_file_prompt.c:51-70`;
`src/editor/inline_file_prompt.h:43-46` (the documentation lie).

**Smell:** `inline_file_prompt_begin` returns 0 immediately when
`g_prompt_active` is true (conservative "no-op while open").
`inline_rename_begin` has **no equivalent guard** — calling it
with a valid slot while rename is already active silently re-seeds
the rename buffer with the new slot's name and switches
`g_rename_slot`. The header `inline_file_prompt.h:43-46` documents
the file_prompt's "begin while open is a no-op" as "*mirroring the
conservative rename idiom*" — but the rename idiom does the
opposite: it overwrites mid-rename state.

**Why it matters:** Two failure modes. (a) A future caller reading
the file_prompt header will wrongly assume rename rejects re-entry
the same way and skip a guard against double-begin in their own
logic. (b) Mid-rename, if any code path (tutorial step, scene-tab
interaction, hotkey) calls `editor_inline_rename_begin(other_slot)`,
the user silently loses their in-progress rename. Production
callers happen to avoid this today (`glr_ctrl.c:3572` only calls
on double-click, `glr_actions.c:616` only from a menu), but the
contract is silent on the hazard.

**Fix:** Decide one shape and document it once. Either (a) add
`if (g_rename_slot >= 0) return 0;` to `inline_rename_begin`
(matching file_prompt's conservative idiom and making the header
accurate), or (b) drop the "conservative" wording from
`inline_file_prompt.h:43-46` and document re-entry policy
explicitly per module. (a) is the smaller change and matches
existing reality.

### 35. Help-overlay-close logic duplicated across editor sites with a layering trapdoor

**Where:** `src/editor/search.c:30-35, 408-410` (forward declaration
of `ui_state_help_mut`, then `search_open` closes help);
`src/editor/input.c:1017-1020` (Esc dismisses help with the same
3-line sequence); `src/app/glr_ctrl.c:2898-2904`
(`glr_ctrl_toggle_help`, the canonical helper, performs a 3-line
variant — toggle vs. set-to-0).

**Smell:** Two editor-side sites (`search.c` and `input.c`) write
```c
ui_state_help_mut()->visible = 0;
editor_help_session_set_tab(0);
editor_help_session_set_scroll(0);
```
verbatim. Both reach `ui_state_help_mut` by **forward-declaring it
locally** rather than including `ui/app/state.h`. The comment at
`search.c:30-34` justifies the forward declaration with
"*ui_state_help_mut is forward-declared here because `repl_*.c` is
not allowed to include ui_state.h per `check-controller-boundaries`*"
— but `search.c` is `editor_*.c`, not `repl_*.c`. The actual guard
only restricts `REPL_SRCS` plus a controller allowlist; it does
not constrain `src/editor/`. So the forward declaration is
justified by a comment that cites the wrong guard, and serves only
as a stylistic escape hatch.

**Why it matters:** (a) The shared 3-line "close help" sequence
has no helper; the next contributor will copy-paste a third site
and forget the `set_scroll(0)` or `set_tab(0)`. (b) The
forward-declaration trick relies on `UiHelpState *` being
layout-stable across editor and UI modules; a header
reorganization of `UiHelpState` would break only at link time,
never at compile time. (c) The stale comment lies about the
layering rules.

**Fix:** Add `glr_ctrl_close_help(void)` next to
`glr_ctrl_toggle_help` in `src/app/glr_ctrl.c`. Have
`search.c::search_open` and `input.c`'s Esc branch call it through
an editor-callable seam (either through a thin
`editor_request_close_help()` indirection or by extending
`EditorInputDispatchEffects` with a flag). Delete the forward
declaration and the stale comment from `search.c`. (Coordinates
with finding #8 — both are editor-reaching-into-UI-state.)

### 36. Inline-overlay "active" predicates use inconsistent sentinels (slot vs. flag)

**Where:** `src/editor/inline_rename.c:19-25` (`g_rename_slot >= 0`
is the active predicate, sentinel of -1);
`src/editor/inline_file_prompt.c:30, 39-41` (`g_prompt_active != 0`
is the active predicate, plain int flag).

**Smell:** Two near-identical modules (per #57's deduplication
candidate) chose different state encodings for the same concept.
`inline_rename` overloads `g_rename_slot` as both "which slot" and
"are we active". `inline_file_prompt` carries a separate
`g_prompt_active` flag *and* a `g_prompt_len` field. Micro-drift
that makes a future "extract a shared `EditorInlineModalState`"
refactor harder.

**Why it matters:** No bug today — but codifies the inconsistency
in the next-to-touch path. Each future read site has to remember
which module's "am I active" idiom is which.

**Fix:** Pick one. The flag-plus-sentinel-slot encoding
(`int active`, `int slot`) is more self-documenting and survives
extension. Lower priority; pair with #57.

---

## 🟢 Dead code / dead fields

### 37. `editor_reset_for_new_scene` is publicly exported with no docstring

**Where:** `src/editor/input.h:155-156`.

**Smell:** `editor_clear_all_cmds` has the comment "/* Clear ALL
commands unconditionally (same behavior as Ctrl+L). */";
`editor_reset_for_new_scene` immediately below has no comment at
all. The only caller is `src/app/glr_actions.c:597` (the New Scene
action). The function differs from `editor_clear_all_cmds` in
three load-bearing ways (no tutorial guard, no undo push, no
status text) but the header doesn't say so.

**Why it matters:** A caller looking at the two API entries can't
tell which to pick.

**Fix:** Add `/* Programmatic reset for scene/workspace load: drops
the document state without taking an undo snapshot, surfacing a
status message, or applying the tutorial guard. Used by code paths
that bracket the load with their own undo/status policy (see
src/app/glr_actions.c::New Scene). For interactive Ctrl+L, use
editor_clear_all_cmds(). */`.

### 38. `editor_take_input_effects` redundantly calls `editor_reset_input_effects`

**Where:** `src/editor/input.c:107-111`.

**Smell:** `editor_take_input_effects()` copies the global effects
struct, then calls `editor_reset_input_effects()` to zero it. But
every caller in `src/app/glr_ctrl.c` follows the pattern
`editor_reset_input_effects(); … ; editor_take_input_effects();`
— they reset *before* invoking dispatch and then *take* (which
resets *again*) afterward. The second reset is redundant.

**Why it matters:** Two resets per dispatch cycle is one
byte-zero-fill more than needed. Not a perf issue but a contract
surprise: `take` has a side effect (clearing) that the name
doesn't suggest. A test that builds effects, calls `take`, and
expects the effects struct to be re-readable will find it zeroed.

**Fix:** Make `editor_take_input_effects` a pure read (return a
copy without resetting); rely on callers' explicit
`editor_reset_input_effects` before next dispatch. Or, rename to
`editor_take_and_reset_input_effects` so the side effect is in the
name.

### 39. `EditorServices` is now used by only ONE method on FOUR call sites; six of seven struct fields are dead code from the editor's perspective

**Where:** `src/editor/services.h:28-74`,
`src/editor/services.c:32-63`.

**Smell:** Closed #28 step 1 landed (✅). Step 2 (the
`commit.c:872` `svc.parse_command_ctx` migration via #26)
**also landed** — `commit.c` no longer references `EditorServices`.
The only remaining production users are in `input.c`:

- `input.c:450` and `input.c:660` are *setup* sites
  (`EditorServices svc = editor_services_default();`) in
  `parse_for_overwrite_enter` and `commit_current_input`
  respectively.
- The *dispatch* sites — actual `svc.parse_command_ctx(...)`
  calls — are at `input.c:468, 480` (the two branches of
  `parse_for_overwrite_enter`'s `num_vis_vars > 0 / else` switch)
  and `input.c:742, 755` (the matching branches inside
  `commit_current_input`). Four dispatches total.

That is the *only* method consumed from any caller in `src/` or
`tests/`. The other six fields (`context`, `compile`,
`apply_repl_change`, `apply_predef_ops`, `apply_scratch_ops`,
`user`) have zero readers. `editor_services_default` constructs
all 7 fields including the dead 6 every time it's called.

This is now substantially worse than the prior audit framed it —
the abstraction is functionally non-existent. All four dispatches
pass `svc.user` (always NULL) into `parse_command_ctx` which
ignores it. The struct definition + default factory + cross-TU
header inclusion (~88 lines of code in `services.{c,h}`) exists to
serve a one-method, four-site interface to the same underlying
function.

**Why it matters:** Closed #28 was planned as a multi-step
dismantling. Steps 1 and 2 have landed organically; the remaining
state is "one struct field, two call sites" — at this point the
struct is *empty wrapping* around `repl_parser_parse_command_ctx`.

**Fix:** Closed #28 step 3 was "migrate the `svc.parse_command_ctx`
sites in `input.c` together with the #18 helper extraction." That
migration is now even more attractive because there are no other
live consumers of any other field. Replace all four
`svc.parse_command_ctx(...)` dispatches with direct
`repl_parser_parse_command_ctx(...)`. Then delete `services.{c,h}`
entirely (step 4).

**Complete deletion checklist** (the file-removal alone is not
build-clean — multiple manifest sites still reference the module;
re-verify line numbers with `rg "svc\.parse_command_ctx|editor_services_default" src/editor/input.c`
before editing, as they drift):

1. Replace `svc.parse_command_ctx(...)` with
   `repl_parser_parse_command_ctx(...)` at all four dispatch sites
   in `src/editor/input.c` — at time of writing: L468, L480 (the
   two branches of `parse_for_overwrite_enter`'s
   `num_vis_vars > 0 / else` switch) and L742, L755 (the matching
   branches in `commit_current_input`). Drop the two
   `EditorServices svc = editor_services_default();` setup sites
   at L450 and L660. The `svc.user` argument is always NULL today
   — pass NULL directly to the new `repl_parser_parse_command_ctx`
   calls.
2. Delete `src/editor/services.c` and `src/editor/services.h`.
3. Delete `struct EditorServices_s` forward-declaration at
   `src/editor/commit.h:26` (now dead).
4. Remove `src/editor/services.c` from the Makefile build lists at
   `Makefile:272` and `Makefile:443`.
5. Remove `src/editor/services.h` from the header dependency list
   at `Makefile:358`.
6. Remove `#include "editor/services.h"` from
   `tests/test_repl_compile.c:19`.
7. Run `make check-c99 && make test-stubs && make test` — first
   verifies all manifests align, second + third catch any straggler
   include path.
8. Update `scripts/check-editor-repl-surface.sh` /
   `scripts/baselines/editor-repl-surface.txt` if the direct
   `repl_parser_*` calls change the ratchet's surface baseline.

**Alternative: do step 3 + step 4 as a self-contained pass NOW
(without waiting for #18's full helper extraction)** — the two
`parse_command_ctx` sites are minimal mechanical replacements; the
deletion checklist above is the complete pass scope.

### 40. `editor_commit_func_decl_resume_take` and `_set` could be `static` to `commit.c`

**Where:** `src/editor/commit.h:145, 242` (declarations);
`commit.c:1089, 1095` (definitions).

**Smell:** All three resume bookkeeping accessors are publicly
declared:
- `editor_commit_func_decl_resume_peek` — 2 readers (`commit.c:254`,
  `test_repl_compile.c:937,946`). Public use justified.
- `editor_commit_func_decl_resume_take` — 1 reader: `commit.c:263`
  (inside the same TU)
- `editor_commit_func_decl_resume_set` — 1 reader: `commit.c:153`
  (inside the same TU)

**Why it matters:** Two public functions with zero cross-TU readers.
Each adds to the surface that closed #28 step 4 must account for.

**Fix:** Make `_take` and `_set` `static` in `commit.c`; remove
the declarations from `commit.h:145, 242`. Keep `_peek` public
for the test caller.

**Implementation note:** the call sites at `commit.c:153` and
`commit.c:263` are *above* the definitions at `commit.c:1089`
(`_take`) and `commit.c:1095` (`_set`). Today the public
prototypes in `commit.h` (which `commit.c` includes at the top)
supply the necessary forward declarations. After making both
functions `static`, the include no longer covers the prototype,
and `check-c99` will fail at the upstream call sites with
"implicit declaration of function" (which is a hard error under
`-std=c99` — see CLAUDE.md "C99 standard" section). Pick one of:

- **Add file-local static prototypes** near the top of `commit.c`,
  alongside the file's other forward declarations. Minimal
  footprint:
  ```c
  static int  editor_commit_func_decl_resume_take(void);
  static void editor_commit_func_decl_resume_set(int delta);
  ```
- **Move the definitions above their first use** — `_set` above
  L153 and `_take` above L263. The trio (`_peek` stays public,
  declared in `commit.h`) lives near the top of the file. Less
  noise but bigger diff.

Either works. The static-prototype option keeps the diff small and
locally-readable; the definition-move option puts the resume
bookkeeping together. Verify by running `make check-c99` after
the change — that's the load-bearing guard.

### 41. Redundant `newly_aliased_slot` assignment on success paths of `editor_compile_func_def`

**Where:** `src/editor/commit.c:702, 800` vs. `commit.c:632`.

**Smell:** The alias-aware pre-step (L632) writes
`out->change.newly_aliased_slot = target_slot;` when a new alias
is registered. Both success branches (L702 for header-overwrite,
L800 for new-def) then write
`out->change.newly_aliased_slot = newly_aliased_slot;`. Two cases:
- The pre-step fired and registered an alias → L632 wrote
  `target_slot`; L702/800 writes the same value (the local
  `newly_aliased_slot = target_slot`). Redundant.
- The pre-step didn't fire (bare `funcN`, or alias already
  registered) → `newly_aliased_slot` stays at -1; L702/800 writes
  -1; but `editor_commit_plan_init` (L96) →
  `repl_compiled_change_init` (`compile.c:99`) already sets it to
  -1. Redundant.

In every reachable case, the assignment is a no-op.

**Why it matters:** Code that looks load-bearing isn't.

**Fix:** Delete L702 and L800
(`out->change.newly_aliased_slot = newly_aliased_slot;`). Confirm
with `make test` (the alias-rollback tests at
`test_repl_compile.c:1054-1139` exercise this code path).

### 42. Eight `editor_state_*` accessor/typedef pairs have zero production callers

**Where:** `src/editor/state.h` / `state.c`.

**Smell:** Verified by `grep -rn` across `src/` and `tools/`:

| Symbol | Location | Production callers |
|---|---|---|
| `editor_state_capture` | `state.c:60` | 0 (tests only) |
| `editor_state_restore` | `state.c:68` | 0 (tests only) |
| `editor_state_buffer` | `state.c:78` | 0 |
| `editor_state_buffer_mut` | `state.c:82` | 0 |
| `editor_state_input_reset` | `state.c:317` | 0 (tests only) |
| `editor_state_document` | `state.c:286` | 0 |
| `editor_state_document_mut` | `state.c:292` | 0 |
| `editor_state_document_reset` | `state.c:296` | 0 |
| `editor_state_selection_mut` | `state.c:502` | 0 (tests use by-value variant) |
| `editor_state_line_override_for` | `state.c:733` | 0 |
| `editor_state_virtual_lines_count_for` | `state.c:697` | 0 |

`editor_input_buffer_mut` and `editor_pending_newline_buffer_mut`
also have zero production callers (tests only). `EditorDocumentView`
exists solely to back `editor_state_document()`, which has zero
callers.

**Why it matters:** The "typed facade" pattern was the design
intent, but for half the slices nobody actually uses the slice —
the underlying primitives are what production code reaches. The
header advertises an API surface that's mostly unwired.

**Fix:** Tier-by-tier: (a) Truly callerless
(`editor_state_buffer_mut`, `_line_override_for`,
`_virtual_lines_count_for`) — delete. (b) Tests-only
(`editor_state_capture/_restore`, `editor_state_input_reset`,
`_state_selection_mut`, `editor_input_buffer_mut`,
`editor_pending_newline_buffer_mut`, `editor_state_buffer`) — move
to a `_test.h` header or mark as test scaffolding so the
production header doesn't advertise them. (c) Document-slice trio
(`editor_state_document`, `_document_mut`, `_document_reset`) —
delete the trio; `editor_state_edit_line*` is the actual surface
used.

### 43. `editor_state_clipboard_clear()` has zero production callers

**Where:** `src/editor/state.h:379`, `state.c:532-537`.

**Smell:** Only tests call `editor_state_clipboard_clear()`
directly. Production uses `_count_set(0)` (clipboard.c:300, 323),
which delegates internally to `_clear()`. Two public names for one
operation, with production picking the less-obvious one.

**Fix:** See #32 — pick one as canonical and delete the other.

### 44. Vestigial forward-decl block in `state.c` after closed #36

**Where:** `src/editor/state.c:245-250`.

**Smell:** The forward declarations triplet (`editor_input_clear`,
`editor_pending_newline_clear`, `editor_cursor_pos_set`) is still
present, with a comment explaining why. But all three are declared
in `state.h` (lines 300, 339, 306 respectively), which the file
includes at line 1. Closed #36 in the prior audit flagged the
*first* such block; this comment-justified block is the same
pattern. The justification reads "Forward decls cover entry points
editor_state.c implements that get called from sibling impls in
this file" — but `state.h` already provides those declarations to
every TU that includes it, including `state.c` itself.

**Fix:** Delete the three forward decls and their justifying
comment.

### 45. `editor_help_session_mut()` has zero callers

**Where:** `src/editor/help_session.c:29-31`,
`src/editor/help_session.h:26`.

**Smell:** Declared and defined as the mutable counterpart to
`editor_help_session_view()`. **Nothing in `src/`, `tests/`, or
`tools/` calls it.** Every external mutation routes through the
narrow setters: `_set_tab`, `_set_scroll`, `_scroll_by`.

**Why it matters:** Adds API surface that suggests a "grab the
whole struct mutably" idiom that isn't used and isn't compatible
with the codebase's pattern.

**Fix:** Delete `editor_help_session_mut()` from both files. If a
future caller genuinely needs whole-struct mutation, it can be
re-added with a real call site.

### 46. `editor_help_session_capture` / `_restore` are test-only

**Where:** `src/editor/help_session.c:11-19`,
`src/editor/help_session.h:21-22`. Only callers:
`tests/test_repl_state.c:240, 249, 257`.

**Smell:** No production code captures or restores help-session
state. The wider state snapshot/restore mechanism (in
`repl_state_capture`/`_restore`) doesn't include it; the
controller uses `_view` + setters for runtime mutation.

**Why it matters:** API surface implies a public capture/restore
contract that production doesn't honor.

**Fix:** Either (a) integrate help-session capture into the normal
snapshot/restore pipeline if there's a UX reason (e.g., the help
overlay scroll position should survive undo/redo or example-switch
— arguably nice), or (b) move both functions to test-only support.
Leaning toward (a) — there's no reason help-state shouldn't be
part of the regular session snapshot if it costs 8 bytes.

### 47. `editor_completion_provider()` is test-only

**Where:** `src/editor/completion.c:15-17`,
`src/editor/completion.h:40`. Only callers:
`tests/test_editor_completion.c:35, 41`.

**Smell:** Public getter returning the registered provider pointer.
Production never reads it — `glr_completion.c::glr_completion_register_provider`
only writes via `editor_completion_register`. The two test
references are pointer-equality checks asserting "the thing I
registered is the thing I see registered" — a round-trip property
of the setter rather than a thing production needs.

**Fix:** Move the getter to a `_internal.h` test header or replace
the test assertions with a counter on `register` calls.

---

## 🔵 Structural concerns

### 48. `commit_current_input` is 219 lines of nested branches encoding 5 distinct dispatch paths

**Where:** `src/editor/input.c:659-877`.

**Smell:** Longest function in the file. Five logical entry
conditions (insert mode + empty input; insert mode + non-empty
input; overwrite mode + edit_line < doc_count; overwrite + at-end
+ enter + empty; overwrite + non-empty + at-end). Each has its own
commit policy. The cyclomatic complexity hides:
- the early "unmodified line + Enter" branch (L664-679) silently
  skips `editor_undo_push_snapshot()`;
- the L805 `editor_try_commit_var_statements_then_insert()` branch
  is reached from one site, the L739 plain
  `editor_try_commit_var_statements()` from another, with the
  difference being "do the post-effects flip into insert mode";
- `editor_resolve_insert_idx()` (L505) encapsulates the insert-idx
  semantics, but `commit_current_input` re-derives the same value
  inline three times (L723, L859, plus the L789 implicit case).

**Why it matters:** Closed #10 explicitly chose to document rather
than refactor; the tests pin both ordering invariants. But the
function continues to accrete complexity (the Enter route's
special-case at L664-679 is invisible from the dispatcher; #14's
status drift between L494 and L762 sits inside this function's
parsing branches). Without an extraction, every new dispatch
concern lands as another nested `if` here.

**Fix:** Split into three roughly-parallel functions matching the
three modal columns: `commit_current_input_insert_mode(enter_mode)`,
`commit_current_input_overwrite_replace(enter_mode)`,
`commit_current_input_append(enter_mode)`. Each is ~50 lines. The
dispatcher (`commit_current_input`) becomes a 20-line router. Use
`editor_resolve_insert_idx()` inside each branch instead of
re-deriving the index.

### 49. `parse_for_overwrite_enter` vs. `commit_current_input`'s insert-mode-with-vars branch: 50% structural duplication

**Where:** `src/editor/input.c:448-497` (function) vs.
`input.c:720-778` (inline).

**Smell:** Both blocks:
1. `collect_visible_vars(insert_idx, vis_vars, MAX_EXPR_VARS, &vis_total)`
2. Declare a parse-err char buffer
3. Branch on `num_vis_vars > 0` vs. else
4. Build a `ReplParseContext` per branch
5. Call `svc.parse_command_ctx(...)`
6. If parsed: store cmd, build canonical text (with-vars-indent or
   copy pl.text)
7. Surface parse error
8. Warn if scope truncated

The insert-mode branch *additionally* calls
`editor_try_commit_var_statements()` between steps 3 and 5 (line
739) and uses `repl_set_status` instead of `repl_set_status_error`
(line 762). Two parallel encodings, one extracted helper and one
inline, of the same parse-and-place sequence — exactly closed #18
(deferred Tier C). New observation: the gap between them is now
**3 lines + 1 status-sink choice** rather than a full
reimplementation.

**Why it matters:** Whenever closed #18 lands, this is the easy
half to consolidate; the policy difference (insert-mode
short-circuits via `try_commit_var_statements` first) is the only
real divergence. Today they drift independently (status sink
choice already drifted per #14).

**Fix:** Add a `try_commit_var_statements_first` parameter to
`parse_for_overwrite_enter` (renamed per #9) and a sink-choice
parameter; replace the L720-778 inline block with a call. Keep
#14's status fix orthogonal — once consolidated, only one sink
choice survives.

### 50. `editor_compile_func_def` (267 lines) and `editor_compile_for_loop` (249 lines) are god-functions

**Where:** `src/editor/commit.c:537-803` (func_def, 267 lines),
`commit.c:815-1063` (for_loop, 249 lines).

**Smell:** Both functions exceed 250 lines and mix multiple
responsibilities (alias pre-step, parse, duplicate-rejection,
overwrite-header, new-def + comment-relocation for func_def; quick-
reject, parse + visible-vars, for-begin construction, for-end,
empty-body, one-liner-body for for_loop). Roughly 60% of `commit.c`
(~1392 lines) lives in these two functions. Each branch is
logically distinct and individually testable.

**Why it matters:** When trying to *understand* a single branch,
the reader has to scroll past unrelated branches. Bug-fix locality
is poor — e.g., fixing the for-loop overwrite-header behavior
requires reading the empty-body fast-path that wraps it. The
for-loop's three `repl_format_fits` branches (visible-vars, step!=1,
default) duplicate the err-on-overflow plumbing 3×.

**Fix:** Extract two static helpers per function — one per major
branch — into `commit.c`:
- `compile_func_def_overwrite_header(...)` and
  `compile_func_def_new_def(...)`; the alias pre-step extraction
  lands as a separate helper (`compile_func_def_resolve_alias`)
  per finding #26.
- `compile_for_loop_empty_body(...)`,
  `compile_for_loop_one_liner(...)`, and a `format_for_begin(...)`
  helper that owns the three format-fits branches.

Each becomes ~40-60 lines instead of 250+. Mechanical, build-safe
per-function pass.

### 51. `editor_commit_apply_swatch_change` is a feature-specific action, not a commit primitive — wrong neighborhood

**Where:** `src/editor/commit.c:1333-1392`.

**Smell:** The only `commit.c` function that is not a commit
primitive or wrapper. It is a *color-swatch arrow-cycle action*
that happens to use the commit infrastructure: reads numeric arg
at cursor, formats a new value, builds a `REPLACE_ONE` change,
calls `editor_commit_apply_external_change`, then post-processes
(reload input, reposition cursor, update completion, request
redraw). Its 60-line body has more in common with
`src/subsystems/color_picker/color_picker_state.c` or an editor
action helper than with the `editor_try_commit_*` /
`editor_commit_apply_*` family.

It's also the only `commit.c` caller of `editor_request_redraw`
and `editor_completion_update`. Those calls indicate it's an
interactive-action handler, not a transaction primitive.

**Why it matters:** Mixing transaction primitives (atomicity,
undo, status policy) with feature-specific actions (cursor
reposition, completion update, redraw) makes both harder to
understand. The function's prefix `editor_commit_apply_*` makes it
grep-look like a sibling of `_apply_external_change` / `_apply_plan`,
suggesting it's part of the same family — but it does notably more
than they do.

**Fix:** Move to `src/editor/swatch.c` (new file) or
`src/subsystems/color_picker/color_picker_state.c`. Rename to
`editor_swatch_cycle_value(int edit_line, int direction)` since
it's not really a "commit" entry point, it's a swatch-action that
consumes the commit API. Keep `editor_commit_apply_external_change`
as the public crossing point. This also brings `commit.c` down by
~60 lines.

### 52. *(withdrawn — factually wrong)* `editor_state_input()` by-value is **not** a 1 KB hot-path copy

**Status:** Withdrawn. An earlier draft of this finding claimed
`editor_state_input()` copies a 1 KB struct because
`EditorInputView` "includes `char input[MAX_INPUT_LEN]`". That is
wrong. The struct at `src/editor/state.h:68-76` holds:

```c
typedef struct {
    const char *input;            /* borrowed pointer into live storage */
    int         input_len;
    int         cursor_pos;
    int         anchor_pos;
    const char *pending_newline;  /* borrowed pointer into live storage */
    int         pending_newline_len;
    int         insert_mode;
} EditorInputView;
```

— two borrowed pointers + five ints, total ~40 bytes. The
`input` field is **already** a pointer; the by-value return copies
a pointer, not 1 KB of buffer.

There is no hot-path copy savings to be had by switching to a
pointer-returning API; the current shape is consistent with closed
audit #22's documented "live-aliased pointers" convention for
`EditorInputView` (and is what made that documentation contract
non-trivial in the first place — the borrowed pointers track
mutations made through the mutable API, so the *liveness* matters,
not the size).

If a future maintainer wants narrower-getter ergonomics
(`editor_input_text()`, `editor_input_len()`, etc.) for readability
or to avoid the per-call snapshot of the int fields, that's a
style choice — not a performance fix. No change recommended.

### 53. `state.h` is a 466-line junk drawer hosting 13 typedefs and ~100 prototypes for five disjoint slices

**Where:** `src/editor/state.h` (entire file).

**Smell:** The header bundles the whole `EditorState` struct +
capture/restore/reset, buffer mutation primitives, input slice +
25+ getters/setters/policy helpers, selection slice, clipboard
typedefs + slice API (note that `clipboard.h` has zero clipboard
typedefs, they all live here), search slice, autocomplete slice,
cursor blink slice, scroll slice, document slice, four UI-typed
overlay lists + 12+ prototypes, line-comment prefix. Five distinct
subsystems with their own .c files (`clipboard.c`, `search.c`,
`completion.c`, plus the inline rename overlays) reach into this
single header for their storage typedefs. The pattern is
"EditorState is the union of every editor-owned slice; one header
declares everything." The first-round audit flagged the size
without proposing a split.

**Why it matters:** (a) Any change to one slice's typedef forces a
rebuild of every TU that includes `state.h` (every file in
`src/editor/`, much of `src/app/`, all UI snapshot code, and the
demo). (b) The contract for each slice — what's owned, who can
mutate, what `_clear` does — is buried among unrelated slices,
making it hard to write the kind of focused docstring closed #22,
#49, #50 needed. (c) Naming inconsistency hides — five different
return shapes for the slice-getters (see #54) are easy to miss
when they're 200 lines apart.

**Fix:** Split into slice-owned headers, keeping `state.h` as the
umbrella:

- `state.h` — `EditorState` struct, `editor_state_capture/restore/reset`,
  slice-getter declarations
- `buffer.h` — `EditorBuffer`, `EditorBufferView`, `editor_buffer_*`
  primitives
- `input_state.h` — `EditorInputState`, `EditorInputView`,
  `editor_input_*` / `editor_cursor_*` API
- `clipboard.h` (existing) — `EditorClipboardState`,
  `EditorClipboardKind`, `editor_clipboard_*` + the storage
  accessors currently in `state.h:377-391`
- `search.h` (existing) — `EditorSearchState` + slice accessors
- `autocomplete.h` (new) — `EditorAutocompleteState` + slice accessors
- `overlays.h` — the four UI-list slice accessors (deals with #33's
  layering issue too)

The `EditorState` struct itself can be one header forward-declaring
the slice types and including them. Mechanical move; preserves all
existing call sites.

### 54. Five different slice-getter shapes; no single convention

**Where:** `src/editor/state.h:213-446`.

**Smell:** The slice-accessor API has at least five different
shapes:

| Slice | Return | Size |
|---|---|---|
| `editor_state_buffer()` | `const EditorBuffer *` | pointer |
| `editor_state_input()` | `EditorInputView` (by value) | ~40 B |
| `editor_state_selection()` | `EditorSelectionState` (by value) | 8 B |
| `editor_state_clipboard()` | `EditorClipboardState` (by value) | ~1 MB |
| `editor_state_search()` | `const EditorSearchState *` | pointer |
| `editor_state_autocomplete()` | `EditorAutocompleteState` (by value) | ~2 KB |
| `editor_state_scroll()` | `EditorScrollState` (by value) | 8 B |
| `editor_state_document()` | `EditorDocumentView` (by value) | 4 B |
| `editor_state_transformers()` | `const UiTransformerList *` | pointer |

The "view" typedef is used in two places (`EditorInputView`,
`EditorDocumentView`) and not in others. The "by value" shape is
used for slices whose payloads range from 4 bytes to 1 MB. The
"const pointer" shape is used for two slices but not the seven
others. Closed items #22 and #35 each addressed one slice's
variant; the underlying pattern was never made consistent.

**Why it matters:** Inconsistent surface invites bugs like #30 and
#31 (huge by-value copies for trivial reads) and inconsistent
locking discipline (the `EditorInputView` aliasing-vs-copying
convention spelled out in closed #22 isn't generalized).

**Fix:** Pick one convention and apply it. Recommended: every read
accessor returns `const SliceType *`; every write accessor returns
`SliceType *` (named `_mut`). View typedefs only when the slice
has fields that *should* be by-value (e.g., live-aliased pointers
like `EditorInputView.input`). Once the slice headers split per
#53, the per-slice consistency story is local and reviewable.

### 55. `editor_state_capture/restore` exist solely to back tests; size and ownership model don't fit

**Where:** `src/editor/state.h:206-208` (declarations);
`src/editor/state.c:60-72` (impls).

**Smell:** `editor_state_capture(&snap)` does `*snap = g_editor_state`
— a 3.2 MB struct copy. It captures everything: per-frame overlay
lists (1.2 MB of which is refilled every frame, per closed #48),
clipboard (1 MB), buffer (1 MB), and slice state. But undo never
uses this — undo has its own `EditorUndoSnapshot` (undo.h:57-68)
that intentionally captures *less* (undo.h:14-18 documents
exclusions). Production code has zero callers of `_capture/_restore`.
The only callers are `tests/test_repl_state.c:236, 245, 253` and
`tests/test_editor_input_selection.c:255, 267`.

**Why it matters:** The header advertises a 3.2 MB copy-by-pointer
API that production code never uses. Worse, the API gives tests
false confidence — capturing 3.2 MB and restoring it makes the
test look exhaustive, but the per-frame overlay lists are
immediately refilled by the next frame, so any test "verifying
overlay restore" is testing a value the controller will overwrite.
The undo subsystem's own `EditorUndoSnapshot` already captures the
right subset.

**Fix:** Move these to a test-only header
(`tests/editor_state_test_helpers.h`) so the production header
stops advertising the 3.2 MB copy as a real API. If "full-struct
round-trip" is genuinely useful for tests, that's the right place
for it. The `repl_state_capture/restore` symmetry argument in the
existing docstring (`state.h:205`) is a holdover from when the
editor's session was sliced across REPL state.

### 56. `editor_state_line_override_for` is O(N) linear scan over `MAX_LINE_OVERRIDES = MAX_COMMANDS = 4096`

**Where:** `src/editor/state.c:733-742`.

**Smell:** Per-call linear walk over up to 4096 entries. The
function has zero callers (see #42), so this is dead — but if it
gets resurrected as the obvious "get the override for line N"
lookup, calling it from a per-source-line render loop would be
O(N²) over the document. The neighboring
`editor_state_virtual_lines_count_for` (state.c:697-707) has the
same shape with `MAX_VIRTUAL_LINES = 512`.

**Why it matters:** This is the kind of accidentally-quadratic
helper that lurks until someone uses it. The same architectural
choice (sparse list keyed by line_idx, linear scan to find) was
made for both overlay slices.

**Fix:** Either (a) delete both (per #42 they're dead); or (b) if
either is resurrected, document the O(N) cost and require callers
to iterate the list directly when looping over many lines, rather
than calling `_for_line` per line. The `glr_ctrl` controller
already walks `transformers` / `highlights` / `virtual_lines` /
`line_overrides` linearly each frame — that's the right pattern
for these sparse lists; the per-line lookup is the antipattern.

### 57. `inline_rename.c` and `inline_file_prompt.c` are 90% duplicated by structure

**Where:** Whole files: `src/editor/inline_rename.c` (98 lines) vs.
`src/editor/inline_file_prompt.c` (205 lines).

**Smell:** Both modules implement the same shape:
- module-local `char buf[N]`, `int len`, active sentinel/flag;
- `_begin(...)` seeds and activates, cancels the other modal;
- `_cancel()` zeros buffer + len + active;
- `_handle_key()` switch on Esc / Enter / Backspace+Delete /
  printable+char_ok;
- `_handle_special()` swallow-all;
- module-local `char_ok(unsigned char c)` filter.

The differences are: capacity (`USER_SCENE_NAME_MAX` vs.
`FILE_PROMPT_BUF_MAX`), the char-allowed set, the Enter-commit
action (rename vs. load), and whether errors are also surfaced
in-prompt (file_prompt has `g_prompt_err`; rename doesn't). About
80 lines of nearly-identical key-dispatch logic.

A second observation: **search.c handles arrow keys (left/right/
home/end) inside its query buffer for inline cursor positioning,
but neither inline modal does.** Both inline modals' `_handle_special`
just swallow everything. UX inconsistency: users can edit search
queries with the cursor but can only append/backspace at end
inside rename/file-prompt. Either intentional UX (modal is short,
just retype) or a feature gap; not documented either way.

**Why it matters:** Any policy change to either modal (e.g.,
adding cursor-key support — see UX note above; adding paste
support; supporting Cmd+A select-all) has to be done twice and
stays in sync only by convention. Today the two modules are
already drifting on small things (predicate shape per #36,
no-op-while-active behavior per #34, error-in-prompt support).

**Fix:** Extract a generic `EditorInlineModal` peer (e.g.,
`src/editor/inline_modal.{c,h}`):
```c
typedef struct {
    char buf[INLINE_MODAL_BUF_MAX];
    int  len;
    int  active;
    int  capacity;                    /* per-instance, set at begin */
    int  (*char_ok)(unsigned char);
    void (*on_commit)(const char *buf);
    void (*on_cancel)(void);
} EditorInlineModal;
```
Each of `inline_rename` and `inline_file_prompt` becomes a thin
wrapper that owns its `EditorInlineModal` instance, supplies the
`char_ok` and `on_commit` callbacks, and exposes the existing
public API verbatim. Then add cursor-key support **once** if
desired and it lands in both. Roughly 100 lines net reduction;
preserves all existing public symbols and test surface. Tier C —
defer until a second nearby modal is added, but flagged here so
the duplication doesn't become 3-way.

---

## Sequencing

### One-afternoon pass — focused on bugs + closed-#18/#35/#44/#46 generalizations

The Tier A items below convert the "we forgot to apply the closed
fix elsewhere" findings into mechanical commits. **18 numbered
list items map to ~15 commits** because two list items bundle
multiple findings (#19+#20+#23+#24 lands as one
docstring-sweep commit; #40+#41 lands as one resume-bookkeeping
commit). Each commit is build-safe in isolation; net ~250 LOC
reduction. List matches the Tier A membership in the classification
block above.

1. **#2** — `_mut()` for read-only `memcmp` in `input.c:618`.
   One-line const-accessor switch.
2. **#3** — Pass a precomputed `current_input_needs_navigation_commit`
   hint into `commit_current_input` to avoid the double evaluation
   in the navigation path.
3. **#4** — Open-coded input clear regression in
   `editor_try_commit_var_statements_then_insert`. Helper
   substitution.
4. **#9** — Rename `parse_for_overwrite_enter` to match its two
   call sites (overwrite + append-at-end). Mechanical rename + ~5
   call-site updates.
5. **#10** — Move misattributed doc-comment in `input.c:426-446`
   next to `parse_for_overwrite_enter`.
6. **#12** — Delete `editor_committed_line_text` wrapper + inline
   the 3 callers.
7. **#13** — Standardize `enter_parse_err` / `editor_parse_err`
   naming.
8. **#14** — Switch `input.c:762` parse-error to
   `repl_set_status_error`.
9. **#15** — Update `EditorUndoRingState` docstring to admit the
   production rollback caller.
10. **#16** — `editor_feed_line` open-codes `editor_input_set_text`.
    Helper substitution.
11. **#17** — Add the missing "Pure with respect to selection"
    sentence to `edit_op_buffer_delete_left_of_cursor` docstring in
    `edit_ops.h:43-46`.
12. **#19** + **#20** + **#23** + **#24** — File-header and
    docstring corrections in `commit.{c,h}` (stale file-header,
    `apply_compiled_three_halves` rename, `end_type` docstring,
    `commit.h:167-170` return-shape doc).
13. **#27** — Delete stale "no cross-TU wrapper" comment in
    `commit.c:156-159`.
14. **#36** — Pick one active-predicate encoding for the
    inline-overlay pair (rename + flag).
15. **#38** — Make `editor_take_input_effects` a pure read (or
    rename to `_take_and_reset_input_effects`).
16. **#40** + **#41** — Make `_take` / `_set` `static` in
    `commit.c` (add file-local static prototypes per #40's
    implementation note); drop the redundant
    `newly_aliased_slot` assignments at L702, L800.
17. **#37** — Add a docstring to `editor_reset_for_new_scene`
    explaining the contract difference vs. `editor_clear_all_cmds`
    (no tutorial guard, no undo push, no status message). Five
    lines of header comment.
18. **#44** — Delete vestigial `state.c:245-250` forward-decls.

### One-week pass — the Tier B cluster work

1. **#39** — Land closed #28's steps 3 + 4 (`EditorServices`
   dismantling) as a single self-contained pass. With `commit.c`
   already off services, the remaining surface is **four**
   `parse_command_ctx` dispatch sites in `input.c` (plus two
   setup sites that disappear with them). ~30 LOC change; delete
   `services.{c,h}` (~163 LOC). Follow the **complete deletion
   checklist** in finding #39 — the file removal is not
   build-clean without the Makefile and test-include cleanup.
2. **#7** — Add the missing `accept` test coverage for the
   completion seam.
3. **#1** + **#8** + **#35** — Route the editor-modal-capture,
   color-picker dismissal, and help-overlay close through the
   controller. Coordinates the three "editor reaches across
   layers" findings into one PR with a
   `glr_ctrl_router_handle_escape_key` helper.
4. **#6** + **#28** + **#29** — Transactional-contract pass:
   add preflight to `apply_compiled_three_halves` (closes the
   partial-commit hole); add the generation check to
   `editor_undo_snapshot_restore`; split the post-restore
   side-effect documentation. Bundling because each is a
   single-helper semantic change with paired test coverage; the
   PR reviewer sees one transactional story.
5. **#30** + **#31** — Generalize closed #35 to the clipboard
   (~1 MB by-value) and autocomplete (~2 KB hot path) slices.
6. **#22** — Decide and document
   `editor_try_commit_var_statements_then_insert` arm semantics;
   extract shared epilogue; drop redundant `mark_source_dirty`.
7. **#5** + **#21** — Unify `commit_message` storage; add the
   `EditorCommitPublishMode` flag to
   `editor_commit_apply_external_change` (must be opt-in — see
   #5 for the variable-drag counter-example).
8. **#34** — Pick one re-entry semantics for the inline-modal
   pair (#36 is folded into the afternoon pass; #34 is the
   re-entry behavior decision specifically).
9. **#11** — Extract `editor_reset_document_to_empty` for the
   shared 7-line tail between `editor_clear_all_cmds` and
   `editor_reset_for_new_scene`.
10. **#18** — Add `editor_compile_for_loop` to closed #18's
    `parse_for_commit` extraction (sixth site).
11. **#25** — Standardize `editor_compile_*` `err`-null policy
    across the four entries.
12. **#26** — Extract `repl_compile_func_def_resolve_alias`
    shared core.
13. **#32** + **#43** — Pick canonical clipboard-reset name and
    delete the alias. Bundle with #30.
14. **#42** + **#45** + **#46** + **#47** — Trim the public
    state.h / help_session / completion surface; move test-only
    accessors out of the production headers.

(`#33` was previously item 15 here with a "may slip to Tier C"
caveat. It's now classified Tier C from the start — see the
"Notes on placement" block and the Tier C list below.)

### Tier C — defer (high cost or cross-cutting)

- **#33** — Move overlay-list typedefs (`UiTransformerList`,
  `UiHighlightList`, `UiVirtualLineList`, `UiLineOverrideList`)
  out of `ui/app/editor.h` to break the editor → ui inversion.
  Touches every UI snapshot consumer (controller + UI renderers +
  the `editor_demo` link surface). Promoted from Tier B after
  sizing review — the conditional "may slip" framing was
  ambiguous; calling it Tier C from the start is more honest.
- **#48** — `commit_current_input` god-function extraction.
  Touches the heart of the dispatch chain.
- **#49** — `parse_for_overwrite_enter` consolidation with the
  insert-mode-with-vars branch — bundle with closed #18's
  one-week extraction.
- **#50** — `editor_compile_func_def` + `_for_loop` god-function
  extraction.
- **#51** — Move `editor_commit_apply_swatch_change` to its
  proper neighborhood.
- **#53** — Split `state.h` into slice-owned headers.
- **#54** — Pick one slice-getter convention and converge.
- **#55** — Move `editor_state_capture/restore` to test-only.
- **#56** — Delete dead O(N) overlay-list per-line lookups (or
  document the iteration pattern).
- **#57** — Extract shared `EditorInlineModal` peer.

(Items moved up to Tier B since the original Tier-C draft: #32 +
#43 — bundled with #30 in the one-week pass; #42 + #45 + #46 + #47
— bundled into the public-surface trim in the one-week pass; #33
— overlay-list layering inversion, kept in the one-week pass with
a "may slip to Tier C if scope grows" caveat.)

### Tier D — withdrawn

- **#52** — Withdrawn (factually wrong; see finding for the
  correction). `EditorInputView` is ~40 B of pointers + ints, not
  a 1 KB struct.

### Out of scope

- The closed audit's #13 (parallel encodings of `float_decl →
  var_assign` order) and #18 (collect_visible_vars → parse → place
  sequences) remain Tier C as the prior audit landed; this audit
  adds finding #18 to the closed #18's call-site list, and #49 +
  #18 (this audit) describe the shape of the eventual fix.
- The `editor_demo` link surface — still working as designed
  (`state.c` + `edit_ops.c` only).
- The `EditorClipboardKind` tagged-union — keep the kind for the
  future `BLOCK` use case.
- The 32-deep undo ring depth — long-standing constant.

## Method note

This audit was produced by four parallel review agents:

- `input.c` (1913 lines — the heaviest file in `src/editor/`) +
  `edit_ops.{c,h}`
- `commit.c` (1392 lines) + `commit.h` + `services.{c,h}` +
  `reformat.{c,h}`
- `state.{c,h}` (state.h 466 lines) + `clipboard.{c,h}` +
  `undo.{c,h}`
- `search.{c,h}` + `inline_rename.{c,h}` +
  `inline_file_prompt.{c,h}` + `completion.{c,h}` +
  `help_session.{c,h}`

Each agent was given the prior audit's resolved list plus the
still-deferred Tier C items (#13, #15, #18, #28, #41) and
instructed **not to re-flag items already ✅ done**, but to flag
regressions, new smells introduced by cleanup, or items the prior
audit closed only partially.

The 🔴 findings (#1-#7) were spot-verified by the consolidator
against the cited source; the 🟡 / 🟢 / 🔵 findings are reported
as the agents framed them. Spot-check before acting on the more
mechanical ones.

Cross-references to other in-progress audits:

- `plans/in-review/src-repl-code-smell-audit-2.md` #14 (`_mut()`
  for reads in `src/repl/`) is the upstream sibling of finding #2
  here. Both should land alongside the missing
  `repl_eval_predef_vars()` const accessor that #14 also depends on.
- `plans/in-review/src-app-code-smell-audit.md` — coordinate
  finding #8 + #35 (editor reaches into peer/UI state) with the
  `glr_ctrl_router_*` extraction work proposed there.
- `plans/done/src-editor-code-smell-audit.md` (this audit's
  predecessor) — all closed-finding references in this doc are to
  that one.
