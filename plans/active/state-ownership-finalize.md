# State Ownership: Finalize Headers, Helpers, and Capture Docs

## Status — ACTIVE (2026-06-08)

The "Provenance" stages 0–5 have all landed (that's the section
below). The remaining work is tracked below:

- **A — `state_views.h` / `state_owners.h` split and header cleanup.**
  - **Status:** **IN PROGRESS (Staged)**
  - Decoupled `src/repl/state_owners.h` from `src/repl/state.h`.
  - Updated `src/repl/state.h` to only include `src/repl/state_views.h` (the read-side surface) and declare the `ReplRuntimeState` and capture/restore APIs.
  - Callers mutating state now explicitly include `src/repl/state_owners.h`.
  - Updated all tests and source files to match this new structure.
- **B — Domain-helper audit.**
  - **Status:** **NOT STARTED**
  - No changes yet. Pass-through wrappers still exist in `src/app/glr_actions.c`, `src/app/glr_state.c`, and the `repl_state_*_set_*` family.
- **C — Capture/restore boundary docs.**
  - **Status:** **NOT STARTED**
  - No "Capture/restore boundaries" subsection in `ARCHITECTURE.md` yet.

## Provenance

This is the residual content from `feature/gold-standard-state-ownership.md`,
a 9-stage state-ownership migration drafted 2026-04-29. Between then
and 2026-05-11 the codebase moved files into `src/repl/`,
`src/editor/`, and `src/ui/`, renamed several modules (`imrepl_ctrl`
→ `glr_ctrl`, `repl_actions` → `glr_actions`, `repl_undo` →
`src/editor/undo`), and shipped or overtook Stages 0–7. The 990-line
plan was retired because its file paths, status table, and Stage-1+
bookkeeping no longer match the code shape. This document keeps only
what is still genuinely actionable.

If you need the full design rationale or the original stage
inventory, see the git history for `feature/gold-standard-state-ownership.md`
on or before commit `edc682e`.

### What landed (Stages 0–5)

- **Stage 0/1** — `check-state-ownership` ratchets wired into
  `make test`. `ReplRuntimeState` is the real aggregate;
  `repl_state_capture` / `repl_state_restore` round-trip in
  `test_repl_state.c`.
- **Stages 2/3** — Every UI-visible slice has a by-value getter
  declared in `src/repl/state_views.h`. UI files contain zero
  `_mut()` calls; mutations route through `glr_actions`,
  `repl_command_store`, `variable_panel_drag`, and the replay
  helpers. `check-ui-no-repl-state-mut` enforces it.
- **Stage 4** — Cursor pixel write went through the
  `UiCodePanelOutput` pattern with controller actualization, exactly
  as the plan's target shape. `src/ui/panels.c` writes
  `out->cursor_px` / `out->cursor_py`; the interim
  `repl_action_set_cursor_pixel` helper and the
  `cursor-px-encapsulated.txt` allowlist are gone.
- **Stage 5** — Medium slices return by-value or read-only views.
  `src/ui/panels.c` no longer calls `repl_state_presentation()`,
  `repl_state_replay()`, or `repl_state_render()` directly — those
  arrive through `UiRenderSnapshot`.

### What was abandoned

- **Stage 6 — rebuild `repl_undo.c` on `repl_state_capture()`.**
  Premise overtaken. Undo lives at `src/editor/undo.c` and
  *deliberately* does not snapshot input bytes or clipboard state.
  See [`done/editor-input-selection.md`](../done/editor-input-selection.md)
  Phase A item 6: input is rebuilt on undo via `load_line_to_input`,
  and the input-text clipboard payload intentionally survives
  unchanged across undo/redo. Folding all state into one
  `repl_state_capture` would silently reverse those decisions, so
  the stage as written no longer makes sense.

- **Stage 7 input-bridge cleanup (Phase C of
  [`done/push-architecture-ui.md`](../done/push-architecture-ui.md)).**
  Explicitly optional in the original plan; not pursued. UI input
  handlers stay as direct action calls rather than returning
  `UiAction` lists. The trade-off (15–25 new variants for limited
  testability gain on synchronous handlers) hasn't shifted, so this
  is parked indefinitely. The render boundary side of Stage 7 *is*
  complete and is the load-bearing half.

## Remaining work

### A — Decide the long-term shape of `state_views.h` / `state_owners.h`

**Decision:** Keep the split and leave filenames as-is, but refine the decoupling.

Rather than collapsing the headers or renaming them, the split was preserved and strengthened:
- `src/repl/state.h` was changed to only include `src/repl/state_views.h` (read-only views/accessors) plus the `ReplRuntimeState` capture/restore API declarations.
- It no longer includes `src/repl/state_owners.h`.
- Calls to `_mut()` functions or other mutating API entry points must now explicitly include `src/repl/state_owners.h`.
- This ensures that files (e.g. read-only readers, tests, formatters, etc.) can include `repl/state.h` without pulling in the mutating API surface, maintaining strict compiler-enforced state boundaries.

Staged changes implement this change across the codebase, updating all references and tests.

### B — Domain-helper audit

(Stage 9 leftover, narrowed.)

Audit `_mut()`-backed domain helpers in `glr_actions.c`,
`glr_state.c`, and the `repl_state_*_set_*` family. Remove any that
just forward `_mut()->field = value` without enforcing an invariant.
Keep helpers that clamp, invalidate a derived cache, or update
related state.

The point: write paths should be either raw `_mut()->field` (visible
mutation) or domain helpers that earn their existence. Pass-through
wrappers blur the surface without adding safety.

Suitable for a single focused commit. Likely small.

### C — Document the three capture/restore boundaries

The codebase has three independent capture/restore pairs, and the
boundary between them is load-bearing:

| Pair | Owns | Excludes (intentionally) |
|---|---|---|
| `repl_state_capture` / `_restore` | REPL document: commands, predef vars, scratch arrays, scene metadata. | Editor session state. |
| `editor_state_capture` / `_restore` | Editor session: input buffer, cursor, anchor, clipboard, selection, search, autocomplete, scroll. | REPL document. |
| `editor_undo_snapshot_save` / `_restore` | Undo ring: source commands, editor-buffer text, edit_line, predef vars, scratch arrays, func aliases. | Active input buffer bytes, anchor, clipboard. Rebuilt at restore via `load_line_to_input`. |

The undo subset is *not* a bug; it's the rule that lets typed-char
insertion, partial-line cut, and partial-line paste avoid
spuriously promoting an example to a custom scene (see Phase A item
6 of `done/editor-input-selection.md`). Future migrations
must not silently fold one boundary into another.

Add a short section to `ARCHITECTURE.md` documenting this — currently
the rules live in code comments and one item of a now-archived
feature plan.

## Verification

- `make check-state-ownership` stays green.
- After A: `make test` builds and all tests run successfully with the new header layout. Decoupled includes are verified: files mutating state explicitly include `repl/state_owners.h`, and `repl/state.h` does not leak write access.
- After B: `git log -p glr_actions.c glr_state.c src/repl/state.c`
  shows removed helpers each have a one-line rationale.
- After C: `ARCHITECTURE.md` has a short "Capture/restore boundaries"
  subsection; the three test suites that exercise these
  (`test_repl_state`, `test_editor_input_selection`, undo cases in
  `test_repl_editor`) are linked from it.
