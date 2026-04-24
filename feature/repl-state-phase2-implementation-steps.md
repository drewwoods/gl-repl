# Phase 2 Step 3: Storage Migration Substep Plan

## Summary
Migrate one runtime domain at a time from broad `g_*` access to `ReplRuntimeState` ownership. Each domain follows the same pattern: add focused accessors, convert production callers, update tests, move storage into `repl_state.c`, then delete that domain’s compat externs from `repl_state_compat.h`.

Current status: slices 3.1, 3.2, 3.3, 3.4, and 3.6 are complete. The compat bridge still carries externs for 3.5+ (presentation, replay, scenes, search, autocomplete, status). Next up is 3.5 (presentation/config).

## Migration Pattern For Every Slice
1. Add the smallest missing `repl_state_*` helper API needed by the domain.
2. Convert production code from direct `g_*` reads/writes to the helper API or an explicit snapshot/view.
3. Convert tests away from direct `g_*` access where the compat symbol is being removed.
4. Move the actual storage into private domain storage in `repl_state.c`, with `ReplRuntimeState` pointing at that storage.
5. Remove the domain’s `extern g_*` declarations and legacy live-bundle fields from `repl_state_compat.h`.
6. Run focused tests plus `make test-stubs TEST_JOBS=4`.

## Slices

### 3.1 Document + Command Store Hooks ✅ DONE
All document globals (`g_cmds`, `g_num_cmds`, `g_edit_line`, `g_normals_dirty`) now route
exclusively through `repl_state` typed APIs (`repl_state_document_cmds_mut()`,
`repl_state_document_count()`, `repl_state_document_count_set()`,
`repl_state_edit_line()`, `repl_state_edit_line_set()`, `repl_state_edit_line_clamp()`,
`repl_state_mark_normals_dirty()`, `repl_state_normals_dirty_clear()`). Converted 32
source files. Removed 4 compat externs from `repl_state_compat.h`. All 2557 tests pass.

### 3.2 Flat Program + Dirty Flags ✅ DONE
All flat-program globals (`g_flat_cmds`, `g_num_flat_cmds`, `g_flat_dirty`,
`g_flat_cmd_local_vars`, `g_user_lighting_enabled`, `g_current_block_begin`,
`g_current_block_end`, `g_current_block_line`) now route exclusively through `repl_state`
typed APIs (`repl_state_flat_program_view()`, `repl_state_flat_program_cmds_mut()`,
`repl_state_flat_program_count()`, `repl_state_flat_program_set_count()`,
`repl_state_flat_program_dirty()`, `repl_state_mark_flat_dirty()`,
`repl_state_flat_program_clear_dirty()`, `repl_state_flat_program_user_lighting_enabled()`,
`repl_state_flat_program_set_user_lighting_enabled()`,
`repl_state_flat_program_set_current_block()`, `repl_state_flat_program_clear_current_block()`).
Converted 16 source files. Fixed `repl_replay.c`'s `replay_advance()` which was missing the
`REPLAY_FLAT_STATE` macro. Removed 8 compat externs from `repl_state_compat.h`. All 2557 tests pass.

### 3.3 Editor Input, Selection, Clipboard ✅ DONE
All editor-input globals (`g_input`, `g_input_len`, `g_cursor_pos`, `g_newline_buf`,
`g_newline_len`, `g_inserting`) now route exclusively through `repl_state` typed APIs
(`repl_state_editor_input()`, `repl_state_editor_input_mut()`, `repl_state_cursor_pos()`,
`repl_state_cursor_pos_set()`, `repl_state_insert_mode()`, `repl_state_insert_mode_set()`,
`repl_state_pending_newline_*`). Converted `repl_editor.c` (133 refs), `repl_commit.c`
(78 refs), `ui_panels.c` (16 refs), `repl_autocomplete.c` (20 refs), and all 7 affected
test files. Removed 6 compat externs from `repl_state_compat.h`. All 2557 tests pass.

### 3.4 Camera, Pointer, Viewport ✅ DONE
All camera (rx/ry/dist/tx/ty/tz/motion_glow/auto_rotate), pointer (mouse_x/y/button),
and viewport (win_w/h) access now routes through `repl_state` typed APIs. Removed 8
compat externs (`g_cam_rx`, `g_cam_ry`, `g_cam_dist`, `g_cam_tx`, `g_cam_ty`,
`g_cam_tz`, `g_cam_motion_glow`, `g_mouse_x`, `g_mouse_y`, `g_mouse_btn`, `g_win_w`,
`g_win_h`, `g_cam_rotate`) from `repl_state_compat.h`. Converted 24 source files
including all production modules and all 7 affected test files. All 2557 tests pass.

### 3.5 Presentation + Config
- Treat `repl_config_get/set/cycle()` as the only mutation API for user-facing presentation config.
- Convert remaining direct config reads in UI/render/export to either `repl_config_get()` or a `repl_state_presentation_snapshot()` captured at frame/layout boundaries.
- Keep immutable descriptor/name tables as `static const` or descriptor accessors; do not put them in runtime state.
- Move presentation storage into `repl_state.c`: grid/axes modes, overlays, wrapping/layout toggles, backdrop, camera rotate, auto normals, highlight, ortho.
- Remove presentation compat externs after all production callers use config/state accessors.

### 3.6 Render Resources + Derived Render State ✅ DONE
Landed as the render-resource slice: `repl_state.c` now owns the GL resource storage and the derived focus state, bootstrap/teardown run through `repl_state_render_init_resources()` and `repl_state_render_destroy_resources()`, and the scene helper passes read the facade instead of writing those globals directly.

### 3.7 Replay
- Keep replay behavior owned by `repl_replay.c`; expose state through replay APIs and `ReplReplayRuntimeState` only where cross-module access is still needed.
- Convert direct replay reads/writes in UI, editor actions, render, annotations, and tests to replay helpers or `repl_state_replay()`.
- Move replay control storage into `repl_state.c`: active, state, pc, mode, speed, accum, fade speed, source line, total flat count, expand args.
- Keep fade batch internals private to `repl_replay.c` unless a later render slice needs them in the state facade.
- Remove replay compat externs.

### 3.8 Scenes, Workspace, Import/Export Metadata
- Keep `g_user_scenes[]` and active scene internals private to `repl_scenes.c`; they are already encapsulated domain storage.
- Move cross-module scene/workspace state into `repl_state.c`: active example index, workspace dir, pending scene/workspace names, export scene-name hint, workspace header lines, render-state lines, camera metadata lines.
- Convert import/export and scene modules to `repl_state_workspace_*`, `repl_state_import_export_*`, and existing public scene APIs.
- Remove import/export and workspace compat externs; leave public scene APIs in `repl_core.h` unchanged for callers.

### 3.9 Search, Autocomplete, Status
- Add status read accessor if needed; keep mutation through `repl_status_set/clear/tick`.
- Convert search UI/editor paths to `ReplSearchState` helpers rather than raw search globals.
- Convert autocomplete UI/editor paths to `ReplAutocompleteState` helpers. Keep private autocomplete matcher internals in `repl_autocomplete.c`.
- Move status, search, and public autocomplete display storage into `repl_state.c`.
- Remove status/search/autocomplete compat externs.

### 3.10 Variable Panel, Profile Panel, Variable Drag, Code Panel Runtime
- Move small UI runtime states last because they have low behavior risk but many incidental reads.
- Convert variable panel/profile visibility through state/config helpers.
- Convert variable drag to `ReplVariableDragState` helpers and keep the drag transaction logic in `repl_var_drag.c`.
- Convert code-panel runtime fields: panel fraction, resizing flag, scroll, follow-cursor, cursor blink, cursor pixel position.
- Move storage into `repl_state.c` and remove the final UI runtime compat externs.

## API And Compatibility Rules
- Public command language, save/load format, GLUT entrypoint, and existing high-level REPL APIs stay unchanged.
- `repl_state_compat.h` is deleted only after the last compat extern and live-bundle accessor is gone.
- Tests may use focused state helpers, command-store APIs, or high-level REPL APIs; do not preserve direct `g_*` test access just for convenience.
- Descriptor/name arrays that are immutable are not runtime state; keep them module-local or behind descriptor accessors.

## Test Plan
- After every slice: `make test-stubs TEST_JOBS=4` and `git diff --check`.
- Document/flat/editor slices: also run `make test_repl_core_internal`, `make test_repl_core_commit`, `make test_repl_core_format`, `make test_repl_core_io`, and `make test_repl_editor`.
- Presentation/render/replay slices: also run `make test_ui`, `make test_scene_guides`, `make sample USE_GL_STUBS=1`, and `make sample`.
- Scene/import/export slices: also run `make test_repl_core_examples`, `make test_repl_core_extra`, and `make test_repl_core_io`.
- Final Step 3 acceptance: no production `.c` file includes or depends on `repl_state_compat.h`; `repl_state_compat.h` can be removed; all 19 stub suites pass.

## Assumptions
- This is still behavior-preserving Phase 2 work: no command-language, file-format, UI, or render behavior changes.
- Storage migration means private state-owned storage in `repl_state.c` plus focused accessors, not a broad rewrite of every module to pass context pointers through every call.
- Slice commits should stay reviewable; if a domain is too large, split it into caller-conversion commits first, but remove compat only in the final commit for that domain.
