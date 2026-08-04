# State Ownership: Finalize Headers, Helpers, and Capture Docs

## Status - LANDED (2026-08-04)

The "Provenance" stages 0-5 landed earlier (that section is below). The three
residual items are now closed:

- **A - `state_views.h` / `state_owners.h` split and header cleanup.**
  - **Status:** **LANDED**
  - `src/repl/state.h` includes only `src/repl/state_views.h` (the read-side
    surface) plus the `ReplRuntimeState` / checkpoint capture-restore
    declarations. Callers that mutate state include `src/repl/state_owners.h`
    explicitly, so a read-only reader cannot pull in the write surface by
    including the facade.
- **B - Domain-helper audit.**
  - **Status:** **DONE - premise partly overtaken; one helper changed.**
    See "B - Domain-helper audit (as built)" below for the finding and the
    evidence. Nothing was removed; `repl_state_document_count_set()` gained
    the clamp its sibling already had.
- **C - Capture/restore boundary docs.**
  - **Status:** **DONE**
  - `docs/ARCHITECTURE.md` has a "Capture/restore boundaries" subsection under
    "Decoupling and Link Boundaries", covering the three overlapping pairs, why
    undo spans both owners, why its exclusion set is a rule, and the three test
    suites that pin it.

## Provenance

This is the residual content from `feature/gold-standard-state-ownership.md`,
a 9-stage state-ownership migration drafted 2026-04-29. Between then
and 2026-05-11 the codebase moved files into `src/repl/`,
`src/editor/`, and `src/ui/`, renamed several modules (`imrepl_ctrl`
→ `glr_ctrl`, `repl_actions` → `glr_actions`, `repl_undo` →
`src/editor/undo`), and shipped or overtook Stages 0-7. The 990-line
plan was retired because its file paths, status table, and Stage-1+
bookkeeping no longer match the code shape. This document keeps only
what is still genuinely actionable.

If you need the full design rationale or the original stage
inventory, see the git history for `feature/gold-standard-state-ownership.md`
on or before commit `edc682e`.

### What landed (Stages 0-5)

- **Stage 0/1** - `check-state-ownership` ratchets wired into
  `make test`. `ReplRuntimeState` is the real aggregate;
  `repl_state_capture` / `repl_state_restore` round-trip in
  `test_repl_state.c`.
- **Stages 2/3** - Every UI-visible slice has a by-value getter
  declared in `src/repl/state_views.h`. UI files contain zero
  `_mut()` calls; mutations route through `glr_actions`,
  `repl_command_store`, `variable_panel_drag`, and the replay
  helpers. `check-ui-no-repl-state-mut` enforces it.
- **Stage 4** - Cursor pixel write went through the
  `UiCodePanelOutput` pattern with controller actualization, exactly
  as the plan's target shape. `src/ui/panels.c` writes
  `out->cursor_px` / `out->cursor_py`; the interim
  `repl_action_set_cursor_pixel` helper and the
  `cursor-px-encapsulated.txt` allowlist are gone.
- **Stage 5** - Medium slices return by-value or read-only views.
  `src/ui/panels.c` no longer calls `repl_state_presentation()`,
  `repl_state_replay()`, or `repl_state_render()` directly - those
  arrive through `UiRenderSnapshot`.

### What was abandoned

- **Stage 6 - rebuild `repl_undo.c` on `repl_state_capture()`.**
  Premise overtaken. Undo lives at `src/editor/undo.c` and
  *deliberately* does not snapshot input bytes or clipboard state.
  See [`done/editor-input-selection.md`](editor-input-selection.md)
  Phase A item 6: input is rebuilt on undo via `load_line_to_input`,
  and the input-text clipboard payload intentionally survives
  unchanged across undo/redo. Folding all state into one
  `repl_state_capture` would silently reverse those decisions, so
  the stage as written no longer makes sense. The
  "Capture/restore boundaries" section added by item C is the durable
  record of that reasoning.

- **Stage 7 input-bridge cleanup (Phase C of
  [`done/push-architecture-ui.md`](push-architecture-ui.md)).**
  Explicitly optional in the original plan; not pursued. UI input
  handlers stay as direct action calls rather than returning
  `UiAction` lists. The trade-off (15-25 new variants for limited
  testability gain on synchronous handlers) hasn't shifted, so this
  is parked indefinitely. The render boundary side of Stage 7 *is*
  complete and is the load-bearing half.

## A - Long-term shape of `state_views.h` / `state_owners.h`

**Decision:** Keep the split and leave filenames as-is, but refine the decoupling.

Rather than collapsing the headers or renaming them, the split was preserved and
strengthened:

- `src/repl/state.h` includes only `src/repl/state_views.h` (read-only
  views/accessors) plus the `ReplRuntimeState` capture/restore API declarations.
- It no longer includes `src/repl/state_owners.h`.
- Calls to `_mut()` functions or other mutating API entry points must explicitly
  include `src/repl/state_owners.h`.
- Files that only read (formatters, view code, most tests) can include
  `repl/state.h` without pulling in the mutating API surface, so the boundary is
  compiler-enforced rather than convention.

## B - Domain-helper audit (as built)

**The audit ran; nothing qualified for removal.** The half of the original
target shape that read "write paths should be raw `_mut()->field` (visible
mutation)" was superseded by a later landed decision, and the pass-through
setters this item proposed deleting are the write surface that decision
requires.

### Why the premise is overtaken

`scripts/check/check-repl-no-mut-reads.sh` holds `src/repl/*.c` - every file
outside the owner set (`state.c`, `apply.c`, `command_store.c`, `eval.c`) - at
**zero** `_mut()` call sites (`scripts/baselines/repl-no-mut-reads.txt`,
`mut_call_count: 0`). That guard and its baseline landed 2026-06-08 in commit
`a5aa4557` (plan: [`done/remove-remaining-repl-mut-reads.md`](remove-remaining-repl-mut-reads.md)),
and the same commit *introduced* the targeted setters - `repl_state_render_*`,
`repl_state_scenes_set_active_example_idx`, the flat-program writers - precisely
to replace the raw `_mut()->field` writes that item B proposed restoring.

Deleting those setters would push raw `_mut()` writes back into `flatten.c`,
`executor.c`, `scenes.c`, and `example_loader.c` and break the guard. They earn
their existence as the write surface that lets it hold at zero.

### Findings per named target

| Target | Finding |
|---|---|
| `glr_state.c` | No per-field setters exist at all - only `_mut()` accessors plus reset / capture / restore. Nothing to remove. |
| `glr_actions.c` | No pass-through wrapper. Every `glr_action_*` / `glr_cfg_*` helper sequences several calls, resolves a lookup, or sets status alongside the write. Its single raw `_mut()->` write is inline inside `glr_cfg_cycle_row`, not a wrapper. |
| `repl_state_*_set_*` | Ten members are literal `field = value` forwards, but each with a production caller is that caller's only legal write door (see above). The rest normalize to 0/1, clamp, or write several related fields. |

A mechanical scan of all of `src/` for functions whose entire body is one
`_mut()->field = value` statement found exactly two, both outside this plan's
scope and both peer-encapsulation doors ratcheted by their own guards:
`replay_clear_fade_batches()` (`check-replay-forwarders`) and
`variable_panel_drag_mark_undo_snapshot_pushed()`
(`check-variable-panel-forwarders`). Callers outside those peers cannot reach
the `_mut()` accessor at all, so the wrapper *is* the boundary.

### The one change made

`repl_state_document_count_set()` was the only family member that neither
enforced an invariant nor had a production caller. It now clamps to
`[0, MAX_EDITOR_COMMANDS]`, matching its sibling
`repl_state_flat_program_set_count()`; every reader walks `cmds[0, cmd_count)`
with no second bound, so an out-of-range count read past the array. Pinned by
`test_command_count_setters_clamp` in `tests/test_repl_state.c`, which asserts
both setters clamp in both directions.

### Left in place deliberately

- `repl_state_render_mut()`, `repl_state_variables_mut()`, and
  `repl_state_render_reset_defaults()` have zero production callers and are
  reached only from tests. They are the fixture doors; there is no narrower
  alternative to move test setup to, and removing them would buy nothing.
- `repl_state_flat_program_clear_current_block()` is declared in
  `state_owners.h` but called only from inside `state.c`. Kept as the symmetric
  half of the set/clear pair rather than made static for a zero-caller
  technicality - `check-public-api-usage` is informational by design.

### Incidental finding (not acted on)

`scripts/check/check-mut-accessor-count.sh` and its baseline
`scripts/baselines/mut-count.txt` (384) are **orphaned**: no Makefile target and
no entry in `scripts/check/run-state-ownership.sh` invokes them, so that ratchet
is not enforced. Wiring it up or deleting it is a separate decision - note that
its regex matches `_mut(` only, so the `_writable()` accessors introduced by the
2026-06-08 sweep would not be counted either way.

## C - The three capture/restore boundaries

Documented in `docs/ARCHITECTURE.md` → "Decoupling and Link Boundaries" →
**"Capture/restore boundaries"**. The section records:

| Pair | Owns | Excludes (intentionally) |
|---|---|---|
| `repl_state_capture` / `_restore` | REPL slices: source document, flat program, predef vars + scratch arrays, executor-mutated render tail, scene/workspace identity, import/export buffers. | Editor session state; the user-scene catalog slots (`repl_scenes_snapshot_capture`). |
| `editor_state_capture` / `_restore` | Editor session: line buffer, input buffer, edit-line cursor, anchor, clipboard, search, autocomplete, scroll, cursor blink, per-frame overlay lists. | The REPL document. |
| `editor_undo_snapshot_save` / `_restore` | One undo ring entry: source commands, editor-buffer text, edit_line, predef names + values, scratch arrays, funcN aliases. | Input-buffer bytes, anchor, clipboard, search, autocomplete, scroll. Restore rebuilds the input row via `editor_load_line_to_input()`. |

plus the two rules that make the table load-bearing:

1. **Undo spans both owners on purpose** - a document edit is atomically REPL
   commands *and* editor text, so a snapshot holding one half would restore a
   document and buffer that disagree.
2. **The undo subset is a rule, not an omission** - `editor_undo_push_snapshot()`
   is the auto-promotion hook, so anything it snapshots becomes a thing that
   promotes an example into a user scene. Because the snapshot is document-only,
   input-buffer-only edits (typed characters, partial-line cut/paste) never push
   and never promote.

The section also covers the lean `repl_state_checkpoint_capture` /
`editor_state_session_capture` variants the tour baseline composes, and links
the three suites that pin the boundaries: `tests/test_repl_state.c`,
`tests/test_editor_input_selection.c`, and `tests/test_repl_editor.c`.

## Verification

- `make check-state-ownership` green.
- `make check-doc-links` green after the ARCHITECTURE.md section (`make
  fix-doc-links` repointed four line anchors that the `state_owners.h` comment
  addition drifted).
- `make test-repl-state` green, including the new
  `test_command_count_setters_clamp`.
