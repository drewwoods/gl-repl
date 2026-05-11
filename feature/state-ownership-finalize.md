# State Ownership: Finalize Headers, Helpers, and Capture Docs

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
  See [`feature/done/editor-input-selection.md`](done/editor-input-selection.md)
  Phase A item 6: input is rebuilt on undo via `load_line_to_input`,
  and the input-text clipboard payload intentionally survives
  unchanged across undo/redo. Folding all state into one
  `repl_state_capture` would silently reverse those decisions, so
  the stage as written no longer makes sense.

- **Stage 7 input-bridge cleanup (Phase C of
  [`feature/done/push-architecture-ui.md`](done/push-architecture-ui.md)).**
  Explicitly optional in the original plan; not pursued. UI input
  handlers stay as direct action calls rather than returning
  `UiAction` lists. The trade-off (15–25 new variants for limited
  testability gain on synchronous handlers) hasn't shifted, so this
  is parked indefinitely. The render boundary side of Stage 7 *is*
  complete and is the load-bearing half.

## Remaining work

### A — Decide the long-term shape of `state_views.h` / `state_owners.h`

The original Stage 8 framed these as transitional headers to collapse
back into a single `state.h`. In the live code they are not
transitional — they're the read-side and write-side public APIs.
`src/repl/state.h` is a 25-line shim that pulls both in:

- `src/repl/state_views.h` (~200 lines) — by-value getters, view structs.
  14 files include it. UI files include only this header, by
  convention.
- `src/repl/state_owners.h` (~130 lines) — `_mut()` accessors and
  domain helpers. 28 files include it. Controllers and editors only.

The split serves a real purpose: UI cannot mutate via the include
graph because it never asks for the writable surface. Three reasonable
directions:

1. **Keep the split, rename for clarity.** `state_views.h` →
   `state_read.h`, `state_owners.h` → `state_write.h`. Makes the
   role obvious at the include line. Mechanical: ~42 includer
   updates.
2. **Collapse both into `state.h`** (the original Stage 8 target).
   Removes include-time enforcement; the `check-ui-no-repl-state-mut`
   grep becomes the only gate against UI mutations. Acceptable only
   if the grep guard is treated as load-bearing forever.
3. **Leave as-is.** Names are technical-history baggage but the
   split is right.

Recommend (1) or (3). Don't pick (2) unless someone wants to retire
the include-graph enforcement.

Either rename change is independent of the other items below.

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
6 of `feature/done/editor-input-selection.md`). Future migrations
must not silently fold one boundary into another.

Add a short section to `ARCHITECTURE.md` documenting this — currently
the rules live in code comments and one item of a now-archived
feature plan.

## Verification

- `make check-state-ownership` stays green.
- After A: `grep -rln state_views.h | wc -l` and the equivalent for
  `state_owners.h` reflect the new names (or are unchanged if option
  3); no surprise UI mutations get through `check-ui-no-repl-state-mut`.
- After B: `git log -p glr_actions.c glr_state.c src/repl/state.c`
  shows removed helpers each have a one-line rationale.
- After C: `ARCHITECTURE.md` has a short "Capture/restore boundaries"
  subsection; the three test suites that exercise these
  (`test_repl_state`, `test_editor_input_selection`, undo cases in
  `test_repl_editor`) are linked from it.
