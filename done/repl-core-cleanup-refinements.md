# REPL Core Split Plan (Refined)

## Summary
- Split `repl_core.c` (currently about 8501 lines) into `repl_search.c`, `repl_export.c`, and `repl_editor.c`.
- Keep `repl_core.h` unchanged and preserve behavior.
- Use the current green baseline as the contract: `test_repl_core_search 38/38`, `test_repl_core_parse 32/32`, `test_repl_core_format 58/58`, `test_repl_core_io 75/75`, `test_repl_core_examples 184/184`, and `test_repl_core_commit 141/141`.
- Keep the existing header split instead of inventing new public surface: `sample.h` remains the shared runtime/UI/test header, and `repl_core_internal.h` remains the internal/test helper header.

## Interface And Structure Changes
- Update `Makefile` so `SRCS` and `CORE_TEST_SRCS` both include `repl_search.c`, `repl_export.c`, and `repl_editor.c`.
- Keep search state and UI-facing search helpers in `sample.h`: `g_search_*`, `repl_search_row_count`, `repl_search_row_text`, `repl_search_row_for_cmd_index`, `repl_search_find_next_in_text`, and `repl_search_find_prev_in_text`.
- Keep scaffold/export state already consumed outside core in `sample.h`: `g_header_pre`, `g_render_state_lines`, `g_lookat`, `g_header_post`, `init_section_line_count`, `init_section_line`, and `update_lookat_strings`.
- Add only true cross-module internals to `repl_core_internal.h`, such as any non-public helper that `repl_export.c` needs to call into the editor/core commit path.
- Do not add new module headers unless an include cycle forces it.

## Implementation Phases
- Phase 0: inventory symbols before moving anything. Classify each symbol as `static`, `sample.h`, or `repl_core_internal.h`. Record that the real baseline is fully green, not the stale failing-baseline described in the current markdown.
- Phase 1: extract `repl_search.c`. Move search state ownership, row/text helpers, case-insensitive matching, query refresh, hit navigation, and search-mode key/special handling. Leave `ui_panels.c` and tests using the same names through `sample.h`.
- Phase 2: extract `repl_export.c` as the full scaffold and I/O module. Move scaffold tables and builders, init bootstrap tables and emitters, render-state/look-at string generation, all `write_*_as_c` export helpers, `save_output` / `repl_save_output`, `load_from_file` / `repl_load_from_file`, the `import_*` translators, and the code-panel dump helpers.
- Phase 2 also keeps `load_initial_commands` in `repl_core.c`; it should continue to orchestrate “load file if present, otherwise load example” using the extracted load helper.
- Phase 3: extract `repl_editor.c`. Move editor state, selection, clipboard, undo snapshots, input buffer loading, commit/delete/paste helpers, autocomplete glue, and the non-search keyboard/special/mouse/motion/passive/timer editor handling.
- Phase 3 keeps parse/normalize/flatten/execute/replay logic in `repl_core.c`; the editor module calls into that logic rather than owning copies of it.
- Phase 4: cleanup. Remove dead statics, stale forward declarations, and declarations that no longer cross file boundaries. Re-audit `sample.h` and `repl_core_internal.h` and demote anything that can become file-local again.
- Phase 5: update `ARCHITECTURE.md` after the code split lands. Rewrite ownership and data-flow sections so they describe `repl_core.c`, `repl_search.c`, `repl_export.c`, and `repl_editor.c` accurately, and replace brittle single-file assumptions with module-level descriptions.

## Test Plan
- After each phase, run `make clean && make all` and then `make test`.
- Treat any change from the current passing counts as a regression.
- After Phase 2, do a manual export smoke test by launching `./sample`, saving `output.c`, and compiling that file with `glut_compile output.c` or the equivalent repo compile command.
- After Phase 5, verify `ARCHITECTURE.md` no longer describes search, export/import, and editor handling as all living inside one monolithic `repl_core.c`.

## Assumptions And Defaults
- This is a structural refactor only; no user-visible behavior changes are part of scope.
- `sample.c`, `scene_render.c`, `ui_panels.c`, and the current tests should keep compiling with minimal include/declaration churn.
- The existing `feature/repl-core-cleanup.md` baseline is stale and should be replaced with the green baseline above.
