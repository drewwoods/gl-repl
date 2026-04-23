# Phase 2 Step 3: Storage Migration Substep Plan

## Summary
Migrate one runtime domain at a time from broad `g_*` access to `ReplRuntimeState` ownership. Each domain follows the same pattern: add focused accessors, convert production callers, update tests, move storage into `repl_state.c`, then delete that domain’s compat externs from `repl_state_compat.h`.

Current status: the storage moves for 3.1-3.4 are underway. `repl_state.c` already owns the document, flat-program, editor-input, selection, clipboard, camera, pointer, and viewport storage blocks, but the compat bridge and several direct callers are still in place. The render-resource slice is already landed. The next implementation slice is 3.3, editor input / selection / clipboard, followed by 3.4 camera / pointer / viewport cleanup.

## Migration Pattern For Every Slice
1. Add the smallest missing `repl_state_*` helper API needed by the domain.
2. Convert production code from direct `g_*` reads/writes to the helper API or an explicit snapshot/view.
3. Convert tests away from direct `g_*` access where the compat symbol is being removed.
4. Move the actual storage into private domain storage in `repl_state.c`, with `ReplRuntimeState` pointing at that storage.
5. Remove the domain’s `extern g_*` declarations and legacy live-bundle fields from `repl_state_compat.h`.
6. Run focused tests plus `make test-stubs TEST_JOBS=4`.

## Slices

### 3.1 Document + Command Store Hooks
- Add document helpers for source command count, command array/view, edit-line get/set/clamp, normals-dirty get/clear, and command-store dirty invalidation.
- Convert `repl_command_store.c` off `repl_command_state_live()` and onto `repl_state_document_mut()` plus `repl_state_mark_flat_dirty()` / `repl_state_mark_normals_dirty()`.
- Convert source-command mutation paths first: commit, undo/redo restore, clipboard paste/cut, examples, scene load/restore, import declaration insertion, reformat/autonormal replacements.
- Convert read-only production consumers after mutation paths: UI rows, search rows, scene focus lookup, export, flatten inputs, autocomplete function scan.
- Move `g_cmds`, `g_num_cmds`, `g_edit_line`, and `g_normals_dirty` storage under the document storage block in `repl_state.c`.
- Remove document fields from `ReplCommandState` / `ReplEditorState`, then delete document externs from `repl_state_compat.h`.
- Current progress: the command-store boundary is in place and `repl_state.c` already owns the source-command storage, but many production callers still read `g_cmds`, `g_num_cmds`, and `g_edit_line` directly. This slice remains in progress until those callers move off the bridge and the compat externs can go away.

### 3.2 Flat Program + Dirty Flags
- Add flat-program helpers for live `FlatProgramView`, mutable output buffer for flattening, flat count set/clear, dirty get/set/clear, current-block highlight set/clear, and user-lighting flag access.
- Convert `repl_flatten.c` to write through the flat-program state API instead of mutating `g_flat_cmds`, `g_num_flat_cmds`, `g_flat_dirty`, and current-block globals directly.
- Convert executor/render/replay/overlay readers to accept `FlatProgramView` or call `repl_state_flat_program_view()`.
- Move `g_flat_cmds`, `g_flat_cmd_local_vars`, `g_num_flat_cmds`, `g_flat_dirty`, `g_user_lighting_enabled`, `g_current_block_begin`, `g_current_block_end`, and `g_current_block_line` storage into `repl_state.c`.
- Remove the remaining flat fields from `ReplCommandState`; if no callers remain, delete `repl_command_state_live()` entirely.
- Current progress: `repl_state.c` now builds the live flat-program view directly from its owned storage, `repl_flatten.c` writes current-block and dirty state through the facade, and `repl_core.c`, `repl_executor.c`, `repl_replay.c`, `scene_render.c`, `scene_lights.c`, `scene_overlays.c`, and the internal state tests read the flat stream through the new accessor path. The compat externs remain only for the still-unmigrated legacy bridge, so this slice is partially complete but not retired yet.

### 3.3 Editor Input, Selection, Clipboard
- Add editor-input helpers for clear, set text, load source line, cursor move/set, insert/delete character, pending-newline save/restore, and insert-mode get/set.
- Convert keyboard/feed-line/autocomplete callers away from raw `g_input`, `g_input_len`, `g_cursor_pos`, `g_newline_buf`, `g_newline_len`, and `g_inserting`.
- Convert selection and clipboard modules to use `ReplSelectionState` / `ReplClipboardState` helpers, not direct `g_sel_anchor`, `g_sel_end`, `g_clipboard`, or `g_clipboard_count`.
- Move input storage from editor, clipboard storage from clipboard module, and selection storage into `repl_state.c`.
- Remove `ReplEditorState`, `repl_editor_state_live()`, and the editor/selection/clipboard compat externs once no production users remain.
- Current progress: the typed editor-input, selection, and clipboard accessors already exist in `repl_state.h` and are backed by storage in `repl_state.c`, but `repl_editor.c`, `repl_clipboard.c`, `repl_search.c`, `repl_autocomplete.c`, and the editor tests still reach the compat globals in many places. This slice is started, not finished.

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
