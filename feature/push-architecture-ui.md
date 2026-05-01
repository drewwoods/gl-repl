# Plan: Make `ui_*` snapshot-driven (push model)

## Context

Today the `scene_*` layer is snapshot-driven: the controller
(`imrepl_ctrl.c`) builds a `SceneRenderConfig` once per frame, pushes it to
`scene_render_3d_scene(&cfg)`, and the scene reads only from the config.
The `ui_*` layer is half-converted:

- `ui_replay_hud.c` already takes a `UiReplayHudState` snapshot
  (`ui_replay_hud.h:10–25`). Zero live state reads. This is the proven pattern.
- Every other `ui_*.c` file still pulls live state through
  `repl_state_views.h` — 127 read sites across 8 files
  (~70 in `ui_panels.c`, ~26 in `ui_menu_bar.c`).
- A single render-time mutation remains: `ui_panels.c:250–251` writes the
  computed cursor pixel position back into REPL state from inside the line
  draw loop. Color picker render also caches hit rects in file-statics
  (`ui_color_picker.c:151,187,207`) — same shape, different storage.
- Input-handler mutations are already routed through `repl_action_*`,
  `repl_command_store_*`, and `repl_undo_*` (R2 in
  `feature/push-architecture-refinement.md`). That is the existing
  "output channel" — the user's "outputs from UI" rule is mostly a
  re-framing of those calls, not a new mechanism.

The goal is to extend the `UiReplayHudState` pattern to every `ui_*` render
entry point, eliminate the render-time write-backs, and make the
snapshot-in / outputs-out shape symmetric with `SceneRenderConfig` /
`SceneFocusVertex` (controller actualizes scene discoveries the same way it
should actualize UI discoveries).

## Recommended Approach

Three phases, in order. Each phase is independently shippable; later phases
are optional once the earlier ones land.

### Phase A — Eliminate render-time write-backs (small, ~1 commit)

The named violation and its siblings are the load-bearing change. Without
this, no snapshot scheme can be enforced — UI render functions still need
write access.

**A1. Move cursor pixel computation out of render.**
`code_panel_draw_line()` in `ui_panels.c` currently writes `*cp->cursor_px`
and `*cp->cursor_py` while drawing. Two options:
- **Preferred:** add a `cursor_px/cursor_py` field to a new
  `UiCodePanelOutput` struct passed to `ui_panels_render_code_panel()` as
  an `out` param. Controller reads it after render and calls a
  `repl_action_set_cursor_pixel(int px, int py)` (new) or writes through
  the code-panel store. This keeps the render pure and makes the fact that
  cursor px is a frame output explicit.
- **Alternative:** add a layout pass `repl_code_panel_compute_cursor_pixel()`
  in `repl_code_panel_document.c` that the controller runs before render;
  render only consumes the result. Cleaner separation, more code to write.

Recommend the first option — symmetric with how scene returns
`SceneFocusVertex` discoveries.

**A2. Extract color-picker hit rects.**
`ui_color_picker_render()` writes `g_cp_sv_x/y/sz`, `g_cp_hue_*`,
`g_cp_alp_*` for use by later mouse handlers. Replace with a
`UiColorPickerHitRects` struct returned from render (or computed once by a
new `ui_color_picker_layout()` helper called by both render and input).
File-local statics aren't REPL state, but the render-then-read coupling
is the same shape and worth fixing in the same pass.

**A3. Verify the full set.** Grep for `*[a-z_]\+ =` inside every render
function in `ui_*.c`. Anything writing through a pointer that came from
`repl_state_*_mut()` is in scope. The other render-time mutation
(`ui_variable_panel.c:81–87`, replay-lift easing animation) writes to
file-local statics for a UI-local effect; leave it alone but document.

Exit criterion: every `ui_*` render function takes only `const`
inputs and (where needed) an `out` struct. No `_mut()` accessors used
inside any `*_render()` function.

### Phase B — Build `UiRenderSnapshot`, route every UI render through it ✅ Done

Status (2026-05-01): Phase B has landed. Every UI render entry point now
takes `const UiRenderSnapshot *snap`; the controller builds the snapshot
once per frame in `imrepl_ctrl_build_ui_snapshot()` and passes it to each
renderer. The `check-ui-no-repl-state-read` guard runs as part of
`make test` / `make check-state-ownership`.

What landed:

- **`ui_snapshot.h`** — `UiRenderSnapshot` bundles the by-value
  `Repl*State` slices, the pointer-shaped read-only views
  (`ReplVariableView`, `ReplEditorInputView`, `ReplImportExportView`,
  `FlatProgramView`, `ReplPredefView`), document/flat-program metadata,
  user-scene names, and grid arrays.
- **`imrepl_ctrl_build_ui_snapshot()`** — single per-frame builder; the
  only place that reads `repl_state_*` for UI rendering. Refreshes
  workspace header lines before populating the import/export view so
  renderers stop calling `repl_state_refresh_workspace_header_lines()`
  mid-frame.
- **Renderer signatures** — every entry point now matches one of the
  canonical shapes:
  - `void ui_X_render(const UiRenderSnapshot *snap)`
  - `void ui_X_render(const UiRenderSnapshot *snap, ...extra layout args)`
- **Allowlists** — `scripts/allowlists/ui-renderers-signature.txt`
  enumerates the audited render functions; `ui_snapshot.h` is allowed
  to include `repl_state_views.h` because it *is* the UI read boundary.
- **Tests** — `tests/test_imrepl_ctrl.c` stubs were updated to the new
  signatures; full suite stays green (2974/2974).

Residual scope (intentional, Phase C territory):

- `ui_*` files keep `#include "repl_state_views.h"` for *non-render*
  paths: input bridges (`ui_color_picker_press/motion`,
  `ui_menu_bar_*_hit`, `ui_variable_panel_rect/hit`,
  `ui_panels_handle_*`) still query live state because they run from
  GLUT input handlers without a snapshot in scope.
- `repl_state_status_set()` and similar invariant-bearing setters are
  still called from input handlers; the Phase B render-side rule does
  not touch them.

The historical plan follows below; Phase A and Phase C remain as written.

#### Original plan (medium, ~5–8 commits)

Mirror what `SceneRenderConfig` did for `scene_*`. The controller builds
one per frame from REPL state; UI render functions consume it; UI files
stop including `repl_state_views.h` for read access.

**B1. Define `UiRenderSnapshot` in a new `ui_snapshot.h`.**
Sized from the inventory below (127 read sites, 8 categories). Carry
*values*, not pointers, to make the snapshot a true frame-frozen view.

```text
viewport      : window_w/h, code_panel_layout, scene_x/y/w/h, panel_frac
presentation  : show_vertex_indices, grid_*, axes_*, backdrop_mode, ...
document      : cmd_count, edit_line_idx, insert_mode, cursor_pos,
                cursor_visible, blink_tick, scroll
input         : input_text[1024], input_len, ghost text
selection     : anchor, end_idx
clipboard     : count, summary
status        : text, ticks_remaining
search        : active, query, hit_line/char_idx
autocomplete  : matches[N], selected_idx, ghost, hint, mode
replay        : already covered by UiReplayHudState — fold it in
help          : visible, active_tab
variable_pnl  : visible, drag_state snapshot
profile_pnl   : mode
inline_rename : active, buffer, target_slot
user_scenes   : active_idx, names[MAX_USER_SCENES], slot_used[MAX_USER_SCENES]
examples      : active_example_idx, names[]
variables     : anim_time, predef view (already exists via ReplPredefView)
pointer       : mouse_x/y (for menu-bar hover hit-test)
flat_program  : FlatProgramView (for replay annotations)
```

A few of these are already snapshot-shaped (`UiReplayHudState`,
`ReplPredefView`, `FlatProgramView`) — embed them by value rather than
re-inventing.

**B2. Add `imrepl_ctrl_build_ui_snapshot()` in `imrepl_ctrl.c`.**
Same shape as `imrepl_ctrl_build_scene_config()`
(`imrepl_ctrl.c:145–246`). Built once per frame after the scene config,
before any `ui_*_render()` call. This is the only function that reads
`repl_state_*` for UI rendering.

**B3. Convert UI render entry points one file at a time.**
Order by leverage: `ui_replay_hud.c` (already done — extend signature to
take the unified snapshot), `ui_help_overlay.c` (5 reads, trivial),
`ui_profile_panel.c` (5 reads), `ui_autocomplete_panel.c` (5 reads),
`ui_variable_panel.c` (8 reads), `ui_color_picker.c` (6 reads),
`ui_menu_bar.c` (26 reads), `ui_panels.c` (70 reads — last and largest).

Each file: change render signature to
`void ui_X_render(const UiRenderSnapshot *snap[, UiXOutput *out])`,
replace every `repl_state_X()` call with `snap->X`, drop the
`#include "repl_state_views.h"`. Build green between each file.

**B4. Add `check-ui-no-repl-state-read` Makefile guard.**
After B3 completes, every `ui_*.c` file should no longer include
`repl_state_views.h`. Grep guard:

```makefile
check-ui-no-repl-state-read:
    @bad=$$(grep -lE '#include\s+"repl_state' ui_*.c) ; \
    if [ -n "$$bad" ]; then echo "ERROR: $$bad"; exit 1; fi
```

Wire into `make test` alongside the existing `check-views-no-owners`
(R6c) and `check-ui-no-repl-state-mut` (R7) guards.

Exit criterion: `grep -l 'repl_state_' ui_*.c` returns only files that
still call `repl_state_status_set()` from input handlers (Phase C scope).

### Phase C — `UiActionList` model for input handlers (in progress, two halves)

Status (2026-05-01): Phase C is being landed in two halves. Splitting it
keeps the diff reviewable and lets the smaller, self-contained handler
chain prove the pattern before the bigger ones are touched.

#### C-1 ✅ Done — infrastructure + color-picker chain

What landed:

- **`ui_action.h`** — `UiActionKind` enum (19 variants covering every
  observed input-handler mutation), `UiAction` tagged union, fixed-size
  `UiActionList` (cap 16, with overflow flag), and the `ui_action_list_*`
  append/init helpers.
- **`ui_action.c`** — `ui_action_dispatch_all()` switches over the kind
  and forwards each entry to the matching `repl_action_*` /
  `repl_command_store_*` / `repl_undo_*` / domain helper. Centralizing
  the include set here keeps `ui_*.c` files free of action-API includes
  for the actions they emit.
- **Color-picker chain** —
  `ui_color_picker_press(UiActionList *out, ...)`,
  `ui_color_picker_motion(UiActionList *out, ...)`, and
  `ui_color_picker_open_actions(UiActionList *out, ...)` append
  `UI_ACTION_COLOR_SET` / `UI_ACTION_CLEAR_COLOR_SET` /
  `UI_ACTION_UNDO_PUSH_SNAPSHOT` instead of calling
  `repl_command_store_*` / `repl_undo_*` synchronously. Passing `NULL`
  preserves the legacy synchronous path so unmigrated callers keep
  working.
- **`ui_panels.c` glue** — the three sites that drive the color picker
  (`ui_panels_handle_code_panel_press`, `ui_panels_handle_scene_press`,
  `ui_panels_handle_motion`) construct a stack-local `UiActionList`,
  thread it down, and dispatch via `ui_action_dispatch_all()` before
  returning. The public `ui_panels_handle_*` API is unchanged, so
  unmigrated callers in `repl_editor.c` keep working.
- **Tests** — `tests/test_ui.c` updated to the new color-picker
  signatures (passes `NULL` to retain legacy semantics for those
  fixtures). Full suites green: `make test` 24/24 binaries (2974 tests),
  `make test-stubs` 27/27 binaries (3118 tests).

What this proves:

- The action-vocabulary split (kind + tagged args) covers real call
  sites without contortions.
- The two-style coexistence (`out` parameter optional, NULL falls
  through to the legacy path) lets handlers migrate independently.
- Stack-local `UiActionList` is the right buffer shape — no allocator,
  no thread-safety questions, naturally scoped to one input event.

#### C-2 ✅ Done — `ui_panels` + `ui_menu_bar` handler chains internalized

Status (2026-05-01): C-2 lands the deferred-dispatch pattern across the
remaining `ui_panels` and `ui_menu_bar` input handlers. Each top-level
handler builds a stack-local `UiActionList` and drains it on its way
out. Public `ui_panels_handle_*` and `ui_menu_bar_*` signatures are
unchanged so `repl_editor.c` continues to work without modification —
moving the dispatch up to `repl_editor.c` is now explicitly C-3 work.

What landed:

- **`ui_menu_bar`** —
  `ui_menu_bar_handle_config_right_press_actions(UiActionList *out, ...)`
  emits `UI_ACTION_CFG_CYCLE_ROW`;
  `ui_menu_bar_activate_dropdown_item_actions(UiActionList *out, ...)`
  emits `UI_ACTION_MENU_ITEM_ACTIVATE`. The dispatch handler for
  `MENU_ITEM_ACTIVATE` mirrors the legacy semantics by closing the
  menu when `repl_action_menu_item_activate` reports the item is
  terminal. Legacy synchronous variants kept as thin NULL-out wrappers.
  Other menu mutations (`ui_menu_bar_set_open_menu`,
  `ui_menu_bar_close`, `ui_menu_bar_note_search_opened`) only touch
  file-static state, not REPL state, so they stay synchronous.
- **`ui_panels` press chain** —
  `ui_panels_handle_code_panel_press_inner(UiActionList *out, ...)`
  threads one list through pin-button activation (replay toggle, search
  key, search-opened note), dropdown-item activation, and the swatch
  open path. Public `ui_panels_handle_code_panel_press()` is now a thin
  wrapper that builds the list and dispatches once at the end.
- **`ui_panels` click chain** —
  `ui_panels_handle_code_panel_click_inner(UiActionList *out, ...)`
  emits `UI_ACTION_NAVIGATE_TO_LINE` (when a line was hit),
  `UI_ACTION_CURSOR_BLINK_RESET`, `UI_ACTION_CLEAR_AUTOCOMPLETE`,
  `UI_ACTION_CLIPBOARD_CLEAR_SELECTION`. The press path now reuses the
  inner click directly, so a single list spans the whole press event.
- **`ui_panels` drag** — emits
  `UI_ACTION_SELECTION_START` + `UI_ACTION_SELECTION_SET_END` +
  `UI_ACTION_NAVIGATE_TO_LINE` + `UI_ACTION_CURSOR_BLINK_RESET` per
  drag-target change.
- **`ui_panels_handle_right_press`** — wraps
  `ui_menu_bar_handle_config_right_press_actions` with init/dispatch.

`make sample` and `make sample USE_GL_STUBS=1` clean. `make test` 24/24
binaries (2974 tests) and `make test-stubs` 27/27 binaries (3118 tests)
green — no test changes were needed because the public handler API is
unchanged and the legacy synchronous variants of the menu-bar bridges
remain available for callers that pass `NULL`.

#### C-3 ❌ Not started — top-level dispatch + allowlist tightening

Concrete checklist:

1. **`repl_editor.c` top-level dispatch** — make
   `repl_keyboard_func`/`repl_mouse_func`/etc. allocate one
   `UiActionList` per GLUT event, pass it to the `ui_panels_handle_*`
   chain (signatures must be widened to take `out`), and call
   `ui_action_dispatch_all()` once before returning. This collapses the
   per-handler dispatches C-1/C-2 introduced into a single dispatch per
   event.
2. **Allowlist tightening** — once the leaf handlers stop calling
   action APIs directly through their wrappers, remove
   `repl_actions.h` / `repl_command_store.h` / `repl_clipboard.h` /
   `repl_undo.h` / `repl_search.h` / `repl_replay.h` includes from the
   migrated `ui_*.c` files (currently still pulled in for the legacy
   synchronous code paths). Add a `check-ui-no-action-call` Makefile
   guard.
3. **Renderer-side cursor-pixel actualization (Stage 4 link)** —
   `ui_panels.c:249` still calls `repl_action_set_cursor_pixel()` from
   inside the line-draw loop. Replace with a
   `UiPanelsOutput { int cursor_px, cursor_py; int cursor_pixel_valid; }`
   returned from `ui_panels_render_code_panel()` and actualized by
   `imrepl_ctrl_display_frame()`. Closes Stage 4 of
   `feature/gold-standard-state-ownership.md`.

#### Original Phase C plan (kept for reference)



This converts UI input handlers from "synchronously call action APIs" to
"return a list of intended actions; controller dispatches them."
The user's framing literally asks for this; whether it's worth doing
depends on appetite. Concrete trade-off:

- **Pro:** UI becomes a pure function `(snapshot, event) -> output`.
  Testing without a live REPL becomes trivial. Replay/record of UI
  events becomes possible.
- **Con:** The action APIs (`repl_action_*`, `repl_command_store_*`)
  already centralise the mutation surface; converting to a deferred
  output adds indirection without changing what state actually moves.
  Likely 15–25 distinct `UiAction` variants given the current handler
  surface.

If pursued: define a `UiAction` tagged union, change every
`ui_*_handle_*()` signature to return `UiActionList`, controller
dispatches in `imrepl_ctrl_keyboard()/_mouse()` after each handler.
Phase A and Phase B can ship without Phase C.

## Critical Files

| File | Phase | Role |
|------|-------|------|
| `ui_panels.c:250–251` | A | The named cursor-px write-back |
| `ui_color_picker.c:151,187,207` | A | Render-time hit-rect cache |
| `ui_replay_hud.h` | B | Existing snapshot precedent — extend, don't re-invent |
| `imrepl_ctrl.c:145–246` | B | `imrepl_ctrl_build_scene_config()` shape to mirror |
| `imrepl_ctrl.c:248–333` | B | `imrepl_ctrl_display_frame()` — add snapshot build before UI calls |
| `repl_state_views.h` | B | Source of every field the snapshot needs |
| `ui_snapshot.h` (new) | B | `UiRenderSnapshot` struct |
| `Makefile` | B | New `check-ui-no-repl-state-read` guard |
| `repl_actions.h` | A, C | Where new `repl_action_set_cursor_pixel()` lands; potential `UiAction` dispatch home |
| All `ui_*.c` | B | One commit per file, render signature change + include drop |

## Existing patterns to reuse

- `UiReplayHudState` (`ui_replay_hud.h`) — proven snapshot pattern; the new
  `UiRenderSnapshot` should subsume it.
- `SceneRenderConfig` build sequence in
  `imrepl_ctrl_build_scene_config()` — mix of by-value copies, struct
  memcpy, and lazy snapshots (e.g. `imrepl_ctrl_build_guide_snapshot()`).
  Same template applies.
- `SceneFocusVertex` — example of a render-discovered value handed back
  through config rather than written to live state. Same shape as the
  Phase A cursor-px output struct.
- `repl_action_cursor_blink_reset()`, `repl_replay_toggle_play_pause()`
  (R2 outputs) — proven way to expose a one-line action API for what
  used to be an inline `_mut()` write.
- `ReplPredefView` (`repl_eval.h`, R4b) — example of a by-value snapshot
  helper that hides global access.

## Verification

For each phase:

1. Build matrix: `make clean && make sample`, `make sample USE_GL_STUBS=1`,
   `make test`.
2. Phase A: `grep -nE '\\*[a-z_]+\\s*=' $(grep -l '_render(' ui_*.c)`
   should show no writes through pointers reaching into REPL state.
   Manual smoke: cursor still blinks, color picker hit-tests still
   land on the right swatch.
3. Phase B: after each file conversion, `make test` plus targeted UI
   smoke (open help overlay, open color picker, type into search,
   open autocomplete, open variable panel, switch examples via F12,
   open scene rename). End state: `grep -l 'repl_state_' ui_*.c`
   returns only files that still mutate via action APIs (which Phase C
   would clean up); `make check-ui-no-repl-state-read` passes.
4. Phase C: every input handler returns `UiActionList`; controller
   dispatches; tests can drive UI without a live REPL.

## Sizing note

Phase A is half a day. Phase B is the bulk — call it 2–4 days,
front-loaded by `ui_panels.c` (largest file, owns the code panel).
Phase C is open-ended; defer until A+B prove the pattern works at scale.

The right place to stop is after Phase B with the guard wired in. That
is the architecture parity with `scene_*`. Phase C is the "pure UI"
extension and only worth doing if the testability win lands on a
concrete need.
