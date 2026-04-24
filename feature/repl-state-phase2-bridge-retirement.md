# Phase 2 Bridge Retirement

## Status
Phase 2 storage migration is done. The remaining Phase 2 work is bridge retirement: remove the last production dependence on `repl_state_compat.h`, keep the behavior unchanged, and delete the compatibility header once no production source needs it.

This doc is the incremental landing zone for that remaining work. Keep it current and narrow: one or two small slices at a time, then update the progress section with what landed and what is still open.

## Done So Far
- Storage migration slices 3.1 through 3.10 are complete.
- `ReplEditorState` and `ReplViewState` bundle helpers are gone.
- The immutable export scaffold declarations now live in `repl_export.h`.
- The render-resource bridge is macro-backed through `repl_state_render()`.
- The old config scratch buffer is local to `repl_actions.c`.
- The grid and axes label tables are local to `repl_actions.c`.
- `repl_actions.c`, `ui_variable_panel.c`, `ui_help_overlay.c`, and `ui_menu_bar.c` now use typed state accessors instead of the bridge for the state they still read.
- The branch still passes `make test-stubs TEST_JOBS=4`, `make sample USE_GL_STUBS=1`, and `make sample` after those slices.

## Remaining Slices
1. `ui_autocomplete_panel.c`
   - Replace compat-backed cursor and autocomplete reads with typed code-panel and autocomplete accessors.
   - Keep popup geometry, highlighting, and placement unchanged.
2. `repl_search.c`
   - Convert the search overlay and navigation paths to typed search/state accessors.
   - Preserve the current search semantics and focus behavior.
3. `repl_autocomplete.c`
   - Remove the remaining compat dependence from autocomplete model reads and writes.
   - Keep matching, ghost text, and hint text behavior unchanged.
4. `repl_editor.c`
   - Peel off the last high-touch compat reads in smaller slices.
   - Prefer keyboard/mouse routing, overlay dispatch, and input state cleanup one path at a time.
5. `repl_state_compat.h`
   - Delete the header only after no production `.c` file includes it.
   - Leave only module-local `static const` descriptor tables and direct typed state accessors.

## Possible Follow-Ons
- If `rg` still finds compat reads in `ui_profile_panel.c`, `repl_var_drag.c`, or another small renderer, peel those off as tiny follow-on slices before the final header deletion.
- Keep any final cleanup commit narrow: no behavior changes, no API reshaping, just bridge removal.

## Slice Rules
- Keep each commit reviewable.
- Do not combine broad bridge deletion with behavior changes.
- Update this doc after each slice so the next target is obvious.

## Test Plan
- After each slice: `make test-stubs TEST_JOBS=4` and `git diff --check`.
- Search/autocomplete/editor slices: also run `make test_repl_core_search`, `make test_repl_core_search_extra`, `make test_repl_autocomplete`, `make test_repl_editor`, `make test_ui`, `make sample USE_GL_STUBS=1`, and `make sample`.
- Final bridge deletion: run `make test-stubs TEST_JOBS=4`, `make sample USE_GL_STUBS=1`, and `make sample`.
