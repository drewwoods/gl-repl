# `repl_live_demo` - external-editor, file-watching REPL demo

## Context

The repo already has a minimal `repl_demo` that drives the REPL language
pipeline from hard-coded static text. The goal here is a **new, "live"
standalone demo** that stresses the same components as *independent, composable
parts* under a realistic workflow:

- **The editor is external** (vim, or anything). The demo never edits text.
- **Scenes are separate `.c` files** on disk, listed in an INI config.
- A **demo-specific controller** bootstraps the REPL, imports a scene file,
  applies its camera, runs the executor each frame, and logs parse/import
  errors to the terminal.
- The demo **watches the active scene file's mtime** and re-imports it on
  change, warning on errors (the swap/temp-file idea is dropped per
  clarification - plain mtime polling on the canonical `.c`).
- The demo links **only the REPL pipeline + the variable panel subsystem** (no
  editor, no `glr_ctrl`, no `src/ui/app`, no `src/render3d`), proving those
  pieces compose without the app shell. Predefined variables surface in the
  floating slider panel; dragging a slider drives the geometry live.

This is the inverse-composition counterpart to the existing isolation demos: it
asserts the REPL pipeline and the variable-panel peer can be wired together by a
~single-file host controller with a manual camera and a static source-document
backend.

## Decisions (confirmed with user)

- **Render stack:** minimal - REPL executor + a manual orbit camera. A
  *demo-local* `ReplExportCameraBridge` applies the imported `// camera` block.
  No grid/axes/lights/backdrop, no `src/render3d`.
- **File watching:** poll the active scene file's `mtime`; on change re-import
  and warn on errors. No `.swp`/`.swo` candidate logic.
- **Name:** `repl_live_demo` (`tools/repl_live_demo/`, `./repl_live_demo`).

## Reload semantics (non-transactional, by design)

Reload is **not** transactional, and the demo does not pretend otherwise. The
import sequence resets live state *first*
(`repl_state_reset_program()` + `source_document_clear()`), and
`repl_export_load_from_file` records per-line parse failures as *warnings* and
still returns success as long as the file opened - it only returns 0 on
`fopen` failure (`src/repl/import.c`). So a malformed save can replace a good
scene with a **partial or empty** one; there is no rollback to the previous
frame.

This is acceptable (confirmed with user): the source of truth is the file, and
the user fixes/undoes in their own editor (vim, etc.). The demo's
responsibility is therefore **diagnostic clarity, not state preservation**:
every per-line failure prints a clear, prefixed stderr line that names the
scene file path (and the import line number / offending text when the importer
surfaces it), so the cause is obvious. Route the host-effects `status` /
`status_error` hooks to stderr and bracket each reload with a
`[repl_live_demo] reloading <path>` / `… N warning(s)` banner so a partial load
is never silent.

## Reused APIs / patterns (do not reinvent)

- Import: `repl_export_load_from_file(path, &ReplImportResult)`
  (`src/repl/export.h`) - streams lines through `repl_load_apply_line`, no
  editor. **Does not reset state**, so the caller must
  `repl_state_reset_program()` + `source_document_clear()` +
  `repl_dispatch_edit_line_set(0)` before each import (see `src/repl/import.c`
  `repl_export_load_from_file`).
- Host effects: `ReplHostEffects` / `repl_install_host_effects`
  (`src/repl/host_effects.h`) - back `edit_line_get/_set` with a file-static int
  (as `repl_demo` does) and route `status`/`status_error` to stderr to surface
  per-line import diagnostics.
- Camera bridge: `ReplExportCameraBridge` / `repl_export_install_camera_bridge`
  (`src/repl/export.h`). Import only calls `reset_import()` +
  `try_consume_import_line()`; implement just those (parse `glTranslatef`/
  `glRotatef`/`static float g_angle =` lines via `sscanf` into demo cam vars),
  leave the save/example callbacks `NULL`.
- Frame loop: flatten + execute exactly like `repl_demo`'s `--render` /
  `tick_and_execute` - `repl_state_mark_flat_dirty` →
  `repl_flatten_commands(repl_dispatch_edit_line_get())` →
  `repl_execute_program(&ReplExecutionOptions{ flat_cmd_count, program,
  text=source_document_view() })`.
- Manual camera + mouse orbit/pan/wheel: copy the small pattern from
  `tools/render3d_demo/render3d_demo.c` (`apply_camera_modelview`, drag state).
- Point-parameter proc install: copy `render_install_point_parameter_proc` from
  `tools/repl_demo/repl_demo.c`.
- Variable panel: `ui_variable_panel_render/_hit_test/_size`, `UiVariable`,
  `UiVariablePanelView` (`src/ui/subsystems/variable_panel.h`);
  `variable_panel_handle_drag_begin/_motion/_reset`, `variable_panel_visible/
  _set_visible/_state_reset` (`.../variable_panel_state.h`);
  `VariablePanelValueSource` / `variable_panel_install_value_source`
  (`.../variable_panel_drag.h`). Build the view directly like
  `tools/variable_panel_demo/variable_panel_demo.c`.
- Variable enumeration: `repl_state_variables()` (names+values) +
  `repl_eval_find_predef_var_idx(name)` → row `value` pointers into
  `g_predef_vars_mut[idx].value`. Writeback = set predef value by name + mark
  flat dirty (transient; a reload resets it).
- Source-document backend: **reuse** `tools/repl_demo/source_document.c` (already
  editor-free, implements the `source_document_*` contract). Link it directly.

## Files to create

- `tools/repl_live_demo/repl_live_demo.c` - the whole demo controller:
  - arg/INI parse → list of scene file paths + options (window size, poll ms,
    panel on/off);
  - install host effects (edit-line int + stderr status), camera bridge,
    variable-panel value source;
  - GLUT bootstrap (real window) + point-parameter proc;
  - `import_active_scene()`: reset → `repl_export_load_from_file` → mark dirty →
    rebuild variable-panel rows → seed camera from bridge; record `mtime`;
  - `idle`: advance `t` when playing; every `poll_ms` re-`stat` the active file
    and re-import on mtime change;
  - `display`: flatten + execute under manual camera, draw variable panel + a
    small bitmap HUD (scene name/file/`t`);
  - input: mouse orbit/pan/wheel zoom; slider drag (left=linear/right=log);
    keys - `[`/`]` cycle scene, `r` force reload, `v` toggle panel, space
    pause/resume `t`, `q`/Esc quit.
- `tools/repl_live_demo/repl_live_demo.ini` - sample config (flat `key=value`,
  repeatable `scene=`):
  ```
  poll_ms=250
  window=960x720
  panel=on
  scene=scenes/triangle.c
  scene=scenes/ring.c
  scene=scenes/torus.c
  ```
  CLI: `./repl_live_demo [config.ini]` (default `repl_live_demo.ini`); also
  `./repl_live_demo file1.c file2.c …` to bypass the INI.
- `tools/repl_live_demo/scenes/{triangle,ring,torus}.c` - example scenes in the
  **export `.c` format** (`repl_export_load_from_file` is the reader). At least
  one carries a `// camera` block to exercise the camera bridge. Generate/verify
  by importing them in the demo during implementation; adjust until clean.
- `tools/repl_live_demo/README.md` - workflow (open a `scene=` file in vim, save,
  watch it reload; drag sliders; cycle scenes), INI format, link-set note.

## Files to modify

- `Makefile`:
  - `REPL_LIVE_DEMO_DEP_SRCS = $(REPL_DEMO_DEP_SRCS)` **plus**
    `src/subsystems/variable_panel/variable_panel_state.c`,
    `src/subsystems/variable_panel/variable_panel_drag.c`,
    `src/ui/subsystems/variable_panel.c`, `src/ui/core/theme.c`.
    (`REPL_DEMO_DEP_SRCS` already supplies the pipeline +
    `tools/repl_demo/source_document.c` + `gl_stub_counts` + `cpuprof`.)
  - `REPL_LIVE_DEMO_BIN`, objs, link rule, phony `repl_live_demo:` + symlink -
    mirror the `REPL_DEMO_BIN` block; add to `ROOT_BIN_LINKS`, `demos:`, and the
    `clean` binary list.
  - Wire `check-repl-live-demo-no-editor` into the guard aggregate next to
    `check-repl-demo-no-editor`.
- `scripts/check-repl-demo-no-editor.sh` - generalize: accept the Makefile
  dep-srcs var name as `$2` (default `REPL_DEMO_DEP_SRCS`) so the same script
  guards `repl_live_demo` via `check-repl-demo-no-editor.sh repl_live_demo
  REPL_LIVE_DEMO_DEP_SRCS`. The forbidden patterns (`src/editor/`,
  `glr_source_document.c`, editor nm symbols) are satisfied - the variable-panel
  TUs live under `src/subsystems` / `src/ui`, not `src/editor`.
- `docs/MODULES.md` - add a `repl_live_demo` bullet under "Standalone Demo
  Binaries"; note it is the *composition* proof (REPL pipeline + variable panel,
  no editor/app/render3d).

## Constraints to honor

- `-std=c99`, non-pedantic, portable: `STATIC_ASSERT` not `_Static_assert`;
  keep `mtime`/`stat` and any platform branches localized and portable
  (`struct stat` + `st_mtime` is fine on macOS+Linux); quoted includes for
  project-local headers, angle for system/vendored.
- No trailing whitespace (pre-push + `check-trailing-whitespace`).
- The demo links **no** `src/editor/*`, `src/app/*`, `src/render3d/*`, or
  `src/ui/app/*` - only the REPL pipeline + the four variable-panel TUs +
  `src/ui/core/theme.c` + the reused `source_document.c`.

## Verification

1. **Headless link + isolation (no GL libs needed):**
   `make repl_live_demo USE_GL_STUBS=1` then
   `bash scripts/check-repl-demo-no-editor.sh repl_live_demo REPL_LIVE_DEMO_DEP_SRCS`
   → no editor sources / symbols.
2. **Real GL run:** `make repl_live_demo && ./repl_live_demo` → window shows the
   first scene; the slider panel lists its predefined vars; dragging reshapes
   the geometry live; `[`/`]` cycle scenes; the `// camera` scene loads with the
   saved view.
3. **Live reload:** with the demo running, `vim tools/repl_live_demo/scenes/ring.c`,
   change a vertex, `:w` → the window updates within `poll_ms`. Then introduce a
   parse error and `:w`: confirm a clear stderr warning naming the file (and line
   when available). **Reload is non-transactional** - the bad save may blank or
   partially replace the scene (see "Reload semantics" above); the test is that
   the *diagnostic* makes the cause obvious, not that the prior frame survives.
4. **C99 ratchet:** `make check-c99` (the demo driver joins the project source
   set guard) and `make test-stubs`.
5. **Cross-check under real GCC** per CLAUDE.md (gracemont): `make check-c99 &&
   make test-stubs`.

## Open implementation detail (resolve while coding, not blocking)

- Exact `sscanf` patterns for the camera block + whether `import.c` NULL-checks
  the unused bridge callbacks (`fill_*`, `apply_*`) - confirm and guard the demo
  bridge accordingly. If `// camera` parsing proves fiddly, the manual orbit
  camera still gives a usable default; camera-from-file is the enhancement the
  bridge seam is there to prove.
