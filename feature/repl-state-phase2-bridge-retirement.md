# Phase 2 Bridge Retirement

## Status: COMPLETE

Phase 2 storage migration and bridge retirement are both done.
`repl_state_compat.h` has been deleted. No production or test source file
includes or depends on it. All 2551 tests pass.

## What Landed

### Storage migration (slices 3.1–3.10)
- `ReplEditorState` and `ReplViewState` bundle helpers removed.
- Immutable export scaffold declarations moved to `repl_export.h`.
- Render-resource bridge backed by `repl_state_render()`.
- Old config scratch buffer local to `repl_actions.c`.
- Grid and axes label tables local to `repl_actions.c`.

### Bridge retirement (this branch)
1. `ui_autocomplete_panel.c` - converted to typed code-panel and autocomplete
   accessors.
2. `repl_search.c` - 68 compat macro uses replaced with typed search accessors.
3. `repl_autocomplete.c` - compat dependence removed.
4. `repl_editor.c` - 84 compat macro uses replaced across 19 functions.
5. Scene files (`scene_render.c`, `scene_grid.c`, `scene_axes.c`,
   `scene_lights.c`, `scene_backdrop.c`) - `g_anim_time` replaced.
6. `repl_replay.c` - `g_scroll_follow_cursor` and `g_t_playing` replaced.
7. `repl_code_panel_document.c` - scroll block converted.
8. `repl_executor.c` - local `EXEC_RENDER` macro block covers remaining render
   state reads (quadric, tess, lights, clear_color).
9. `repl_export.c` - `g_lights`, `g_multisample_enabled`, `g_line_smooth_enabled`
   replaced with typed accessor calls.
10. `repl_core.c`, `ui_panels.c`, `repl_example_loader.c` - old function names
    `refresh_workspace_header_lines` / `parse_workspace_header_line` updated to
    `repl_state_refresh_workspace_header_lines` / `repl_state_parse_workspace_header_line`.
11. `repl_replay.c` - `g_t_playing` replaced with `repl_state_variables_mut()->time_playing`.
12. All test files - local `#define` alias blocks added for the handful of compat
    names each test uses, pointing to typed accessor calls.
13. `repl_state_compat.h` deleted.

## Final Acceptance

- `make test-stubs TEST_JOBS=4`: 2551/2551 passed, 19/19 suites ✓
- `make sample USE_GL_STUBS=1`: clean link ✓
- `grep -rn repl_state_compat src/`: zero hits in any `.c` or `.h` file ✓
