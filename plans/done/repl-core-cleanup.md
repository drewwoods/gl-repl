# Plan: phased split of repl_core.c

## Context

`repl_core.c` is ~8750 lines and mixes unrelated concerns: command
parsing/execution, expression plumbing, editor input handling, search,
export/codegen to `output.c`, and `init_gl`. The export subsystem alone is
~2000 lines, self-contained, and the file we edit most often. Splitting it
gives faster incremental builds, clearer ownership, and a smaller surface to
re-read on each change.

Goal: carve out three sibling translation units (`repl_search.c`,
`repl_export.c`, `repl_editor.c`) in that order, keeping the public
`repl_core.h` API untouched. Globals that become cross-unit visible are
promoted to `extern` in the already-existing `repl_core_internal.h` (which is
test-visible and exactly the right place for internal API). Every phase
leaves the tree building and all currently-green tests green - pre-existing
failures in `test_repl_core_commit` (mouse/backspace, 7 assertions) are
captured as a baseline in Phase 0 and must not change across any phase.

## Files involved

- `src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c` (source of moves)
- `src/immediate-mode-repl/claude4.6-opus-thinking/repl_core_internal.h` (extern promotions)
- `src/immediate-mode-repl/claude4.6-opus-thinking/Makefile` (add new `.c` files to every `SRCS`/target rule)
- New: `repl_search.c`, `repl_export.c`, `repl_editor.c`

## Phase 0 - prep (behavior-preserving)

1. Capture the baseline failing assertions in `test_repl_core_commit` (7
   mouse/backspace failures) into a note file so later phases can diff
   against it. Any new failure at any phase boundary is a regression.
2. Audit `repl_core.c` and list every `g_*` global and every static helper
   each of the upcoming phases will need across a file boundary. Add
   `extern` declarations for those globals to `repl_core_internal.h` now
   - definitions still live in `repl_core.c`, so this is purely a
   header change.
3. Build the sample + every test binary and run the full suite. No
   behavior change expected; this confirms the header prep didn't break
   includes.

Commit: "immediate-mode-repl: promote repl_core globals to internal header".

## Phase 1 - extract `repl_search.c`

Why first: smallest unit, dedicated test binary (`test_repl_core_search`,
38 assertions), validates the extract-and-wire pattern before we touch
2000 lines of codegen.

1. Move the search-state globals (Ctrl+F buffer, match cursor), match/next/
   prev helpers, and search-mode keyboard handling into `repl_search.c`.
2. Add a thin `repl_search.h` *only* if sample.c or ui_panels.c needs to
   peek at search state for rendering - otherwise keep the entry points
   in `repl_core_internal.h`.
3. Update `Makefile`: every target that compiles `repl_core.c` must also
   compile `repl_search.c` (sample + each test binary).
4. Grep every moved function name across the tree - any that are only
   called from inside the new file become `static` immediately.
5. Build. Run `make test_repl_core_search` first, then the full suite.
   Diff `test_repl_core_commit` failures against the Phase 0 baseline.

Commit: "immediate-mode-repl: extract repl_search.c".

## Phase 2 - extract `repl_export.c`

Biggest payoff (~2000 lines). Covered by `test_repl_core_io` (75
assertions) and `test_repl_core_examples` (184 assertions, incl. compile
and roundtrip).

Move these out of `repl_core.c`:

- `write_canonical_cmd_as_c`, `write_render_body_range_as_c`,
  `write_render_helper_as_c`, `write_func_defs_as_c`,
  `find_export_block_end`
- `write_tess_preamble`, `write_tess_source_as_c`, `split_top_level_args`,
  `quadric_source_to_c`, `format_cmd_source_as_c`,
  `write_cmd_source_as_c`, `write_for_begin_as_c`
- `write_predef_var_globals`, `write_predef_var_reset_func`,
  `write_rand_helper`, `write_light_setup`
- `save_output` itself
- The init/header tables: `g_init_bootstrap_repl`, `g_init_host_only_c`,
  `g_header_pre`, `g_header_post`, `g_footer_pre_init`,
  `g_footer_post_init`, `g_init_bootstrap_cmds`, `g_init_bootstrap_ready`,
  `g_init_attenuate_points`, `apply_init_bootstrap`,
  `emit_init_section_to_file`, `init_section_line_count`,
  `init_section_line`
- Render-state and lookat string builders: `update_render_state_strings`,
  `update_lookat_strings`, `g_render_state_lines`, `g_lookat`

Constraints:

- Public API in `repl_core.h` stays identical - only
  `repl_save_output()` and the init-section query functions are
  exposed, and they already are.
- Anything that's only called inside `repl_export.c` becomes `static`
  there.
- Things the export unit needs to *read* (`g_cmds[]`, `g_num_cmds`,
  `g_predef_vars[]`, `g_num_predef_vars`, `g_multisample_enabled`,
  `g_line_smooth_enabled`, `g_show_outlines`, `g_show_vpoints`,
  `g_cam_*`, `g_lights[]`, etc.) were already `extern`'d in Phase 0.
- `test_repl_core_io.c` calls `init_section_line` / `init_section_line_count`
  - those must remain accessible via `repl_core_internal.h`.

Update `Makefile`, rebuild, run **`test_repl_core_io`** and
**`test_repl_core_examples`** first (these are the export-specific
regressions), then the full suite. Diff `test_repl_core_commit`
failures against the Phase 0 baseline. Also run `glut_compile` on a
generated `output.c` as a final smoke check.

Commit: "immediate-mode-repl: extract repl_export.c".

## Phase 3 - extract `repl_editor.c`

Most tangled unit, done last so everything around it has already
quieted down.

Move:

- `keyboard_func`, `special_func`, mouse handlers
- `g_input`, `g_input_len`, `g_cursor_pos`, `g_edit_line`, `g_inserting`
- Selection state, `load_line_to_input`, `commit_input_line`
- Undo stack + `Ctrl+Z` handling
- Tab-complete glue if it's file-local to the editor (parser stays in
  `repl_core.c` / wherever it currently lives)

What stays in `repl_core.c`: command parsing (`parse_command`, the
cmd-spec table `g_cmd_specs[]`), `execute_commands`, `flatten_range`,
`init_gl`, normalize/commit pipeline internals, and anything that doesn't
cleanly separate from them yet.

Test gates: `test_repl_core_parse`, `test_repl_core_format`, and
critically `test_repl_core_commit` - capture its failure set and
confirm it matches the Phase 0 baseline exactly. Zero new failures, zero
accidental "fixes".

Commit: "immediate-mode-repl: extract repl_editor.c".

## Phase 4 - cleanup

1. Grep `repl_core.c` for now-unused statics, dead prototypes, and
   forward declarations that no longer point at anything. Delete.
2. Scan `repl_core_internal.h` for externs that are only referenced
   from inside `repl_core.c` after the moves - demote them back to
   file-local.
3. Run the full suite one more time and diff vs. Phase 0 baseline.

Commit: "immediate-mode-repl: tighten internal header after split".

## Guardrails (apply every phase)

- **Never move a function without grepping its name tree-wide first.** If
  the only remaining callers are inside the new file, make it `static`
  in the same commit that moves it.
- **Never add a new `extern` without checking the symbol isn't already
  static-only to one file after the move.** Don't leak API surface.
- **Don't fix unrelated bugs during a move.** Pre-existing mouse/backspace
  failures stay pre-existing until their own dedicated fix.
- **One phase per commit.** If a phase's test run goes red in a way the
  previous phase didn't predict, revert the WIP for that phase, don't
  pile a fix on top.

## Verification

After each phase:

```bash
cd src/immediate-mode-repl/claude4.6-opus-thinking
make clean && make sample
make test_repl_core_parse test_repl_core_format \
     test_repl_core_io test_repl_core_examples \
     test_repl_core_search test_repl_core_commit
./test_repl_core_io       # must be 75/75
./test_repl_core_examples # must be 184/184
./test_repl_core_search   # must be 38/38
./test_repl_core_parse    # must be 32/32
./test_repl_core_format   # must be 58/58
./test_repl_core_commit   # must match Phase 0 baseline (134/141)
```

After Phase 2, additionally:

```bash
./sample                     # load an example, Ctrl+S
glut_compile output.c        # must compile clean
```

After Phase 4, a `wc -l` check: `repl_core.c` should be roughly half its
current size, and no single sibling unit should exceed ~3000 lines.
