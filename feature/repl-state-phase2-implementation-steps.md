# Phase 2 Step 3: Storage Migration Substep Plan

## Summary
Migrate one runtime domain at a time from broad `g_*` access to `ReplRuntimeState` ownership. Each domain follows the same pattern: add focused accessors, convert production callers, update tests, move storage into `repl_state.c`, then delete that domain’s compat externs from `repl_state_compat.h`.

Current status: slices 3.1 through 3.10 are complete. Scene/workspace runtime, the import/export metadata bundle, the search/autocomplete/status bundle, and the remaining UI/time runtime scalars now route through `repl_state` accessors. The remaining Phase 2 work is bridge retirement, not more state migration.

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

### 3.5 Presentation + Config ✅ DONE
All presentation globals (`g_wireframe`, `g_grid_theme`, `g_grid_major_idx`, `g_grid_extent_idx`,
`g_axes_theme`, `g_show_vnums`, `g_show_normals`, `g_show_indices`, `g_show_outlines`,
`g_show_vpoints`, `g_show_guides`, `g_xform_guide_mode`, `g_autonormal`, `g_show_lights`,
`g_backdrop_mode`, `g_highlight_current_poly`, `g_ortho_mode`, `g_wrap_at_comma`,
`g_code_panel_layout`, `g_focus_vtx`, `g_focus_vtx_valid`) now route exclusively through
`repl_state_presentation()` / `repl_state_presentation_mut()` typed accessors.
`repl_config.c`'s `config_value_ptr()` now returns state-struct field pointers directly.
`repl_example_loader.c`'s reset block collapsed to `repl_state_presentation_reset_example_defaults()`.
`g_grid_major_steps` and `g_grid_extents` moved from `repl_core.c` to `repl_state.c`.
Pointer-equality tests in `test_repl_core_internal.c` updated to non-NULL checks.
Removed 23 compat externs from `repl_state_compat.h`. All 2557 tests pass.

### 3.6 Render Resources + Derived Render State ✅ DONE
Landed as the render-resource slice: `repl_state.c` now owns the GL resource storage and the derived focus state, bootstrap/teardown run through `repl_state_render_init_resources()` and `repl_state_render_destroy_resources()`, and the scene helper passes read the facade instead of writing those globals directly.

### 3.7 Replay ✅ DONE
Replay behavior stays owned by `repl_replay.c`, but replay control storage now lives in `repl_state.c`: active, state, pc, mode, speed, accum, fade speed, source line, total flat count, and expand args. The main production consumers are on `repl_state_replay()` for replay reads/writes: `repl_core.c`, `scene_render.c`, `repl_actions.c`, `ui_panels.c`, `repl_editor.c`, `repl_replay_annotations.c`, `repl_config.c`, `ui_menu_bar.c`, `ui_variable_panel.c`, `repl_code_panel_document.c`, `repl_executor.c`, and `bench_repl.c`. Replay tests now use `repl_state_replay_mut()` accessors, and the replay compat externs have been removed.

### 3.8 Scenes, Workspace, Import/Export Metadata ✅ DONE
`g_example_idx` and `g_workspace_dir` now live behind `repl_state_scenes()`, and the workspace/import-export metadata bundle now lives behind `repl_state_import_export()`: workspace header lines, render-state lines, camera metadata lines, pending scene/workspace names, and the export scene-name hint. `repl_scenes.c`, `repl_example_loader.c`, `repl_actions.c`, `repl_editor.c`, `ui_menu_bar.c`, `repl_core.c`, `repl_export.c`, and the affected tests now use the state facade, and the remaining workspace/import-export compat externs have been removed.

### 3.9 Search, Autocomplete, Status ✅ DONE
Status, search, and autocomplete storage now live in `repl_state.c` and are accessed through the typed state facade. `repl_state_compat.h` no longer exports the raw status/search/autocomplete externs; the remaining code paths read and mutate those domains through the accessor-backed compatibility bridge while the final UI runtime slice still has broader global cleanup to do. All 2557 tests pass.

### 3.10 Variable Panel, Profile Panel, Variable Drag, Code Panel Runtime, Time Scalars ✅ DONE
The last state-migration slice landed in `repl_state.c`: panel fraction, resizing flag, scroll, follow-cursor, cursor blink, cursor pixel position, help overlay state, variable-panel visibility, profile-panel mode, variable-drag transaction state, and the `t` animation bookkeeping now all route through the typed runtime facade. `repl_state_compat.h` no longer exports raw externs for those domains. All 2557 tests pass.

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
