# OpenGL Immediate-Mode REPL

Interactive OpenGL command interpreter. Type GL commands, press `;` to execute,
and watch geometry render in real-time with a live code panel.

New to the tree? Start with [`MODULES.md`](MODULES.md) for the one-page
layered overview of the source files. This file is the agent-facing project
brief and goes deeper.

## Build

```bash
make sample          # Build main binary (freeglut)
make glut            # Build with system GLUT (macOS framework)
make test            # Build and run all tests
make clean           # Remove binaries
```

Requires: gcc with C2x support, OpenGL, GLUT/freeglut, AddressSanitizer enabled
by default in debug builds.

`include/gl_includes.h` is vendored alongside the source — the Makefile adds
`-Iinclude` to `COMMON_CFLAGS` so every translation unit can resolve it via
`#include <gl_includes.h>`. Source-backed modules keep paired `.c/.h` files at
the repo root; `include/` is for header-only helpers and vendored single-header
dependencies.

### Local GL Stub Headers

This sample ships no-op OpenGL, GLU, and GLUT headers under
`tests/gl-stubs/include/` so machines without system GL development packages
can still compile and run non-rendering tests.

```bash
make test-stubs
make sample USE_GL_STUBS=1
```

`USE_GL_STUBS=1` prepends `tests/gl-stubs/include/` and drops `-lGL`, `-lGLU`,
`-lglut` from the link flags. Stub-mode objects go to
`build/*-gl-stubs` so they don't mix with rendering builds.

Constraints:

- Stubs are for compilation and non-rendering tests only. No window, no pixels,
  no real GL context. Do not make stubs the default rendering path.
- If the sample starts calling a new GL/GLU/GLUT symbol, extend the matching
  stub in `tests/gl-stubs/include/GL/`, `tests/gl-stubs/include/GLUT/`, or
  `tests/gl-stubs/include/OpenGL/`.
- Keep stubs minimal and no-op — model types, constants, and callable
  signatures well enough for builds, not a fake renderer.
- After touching stubs, verify both paths: `make test-stubs`, `make sample
  USE_GL_STUBS=1`, `make sample`.

Header layout: `tests/gl-stubs/include/GL/gl.h` (fixed-function GL),
`tests/gl-stubs/include/GL/glu.h` (quadrics/projection/tessellator),
`tests/gl-stubs/include/GL/freeglut.h` (GLUT/freeglut callbacks + shapes);
`glext.h`, `glut.h`, `GLUT/glut.h`, `OpenGL/gl.h`, `OpenGL/glu.h` are
compatibility wrappers.

## Run

```bash
./sample                  # Fresh session
./sample output.c         # Reload saved session (single file)
./sample workspace/       # Load every *.c under workspace/ as a user scene
./sample --noaccum        # Disable accumulation buffer AA
./sample --dump-code      # Print loaded buffer to stdout
```

## Test

```bash
make test_eval             # Expression evaluator tests
make test_format           # Indentation/formatting tests
make test_repl_core_parse  # Command parser tests
make test_repl_core_format # Reformatter tests
make test_repl_core_commit # Commit pipeline tests
make test_repl_core_io     # Save/load round-trip tests
```

Run all: `make test`

Test sources live under `tests/` and shared test-only helpers live under
`tests/support/`. The Makefile still builds root-level test executables
(`./test_eval`, `./test_format`, etc.) so existing commands stay stable.

### Boundary Checks

`make check-state-ownership` runs the full inventory of ownership / contract guards
(e.g., input/REPL isolation, mutator placement, UI purity). See the Makefile for the full list.

## File Layout

| File | Responsibility |
|------|----------------|
| `sample.c` | GLUT callback registration, `main()`, window setup, buffer swap; forwards directly to `glr_ctrl_*` |
| `sample.h` | Shared types (`GLCmd`, `CmdType`, `SceneLight`), defaults, stateless helpers, compatibility includes |
| `glr_ctrl.c` | App-frame controller: `glr_ctrl_display_frame`, `glr_ctrl_reshape`, `glr_ctrl_init_gl`; builds `SceneRenderConfig`, calls scene/UI renderers |
| `glr_ctrl.h` | Controller public surface: display, reshape, init-GL entrypoints |
| `glr_config.c` | Config key implementation and descriptor table helpers |
| `glr_config.h` | `ReplConfigKey` / `ReplConfigItem` descriptor API for keyed config access |
| `repl_core.c` | Normalization pipeline (`repl_parse_and_normalize*`), reformatter, startup helpers |
| `repl_parser.c` | REPL source-line parser, expression validation, canonical `GLCmd.source[]` generation |
| `repl_parser.h` | Parser entrypoints (`repl_parser_parse_command*`, `repl_parser_parse_command_ctx`) and `ReplParseContext` |
| `repl_source_scope.c` | Source prefix-depth cache, indentation helpers, block lookup |
| `repl_source_scope.h` | Source-scope query API (`repl_source_scope_block_depth_at`, `repl_source_scope_find_block_end`, indent helpers) |
| `repl_command_spec.c` | Command type metadata and specifications (parsing, formatting, completion requirements) |
| `repl_command_spec.h` | Command spec query API |
| `repl_command_store.c` | Source-command array mutations: insert, delete, replace, bulk-load |
| `repl_command_store.h` | Command-store public API (`repl_command_store_insert_one`, etc.) |
| `repl_core.h` | Public API (parse, flatten, user scene + workspace); GLUT input-dispatch declarations |
| `repl_core_internal.h` | Test-visible internals (normalize/commit pipeline, `feed_line`, `load_line_to_input`, `repl_promote_example_if_needed`) |
| `repl_state.c` | Owns `g_repl_state`, lifecycle, snapshot assembly (`repl_state_capture` / `repl_state_restore`) |
| `repl_state.h` | Typed runtime-state facade, reset helpers, and focused accessors over the live REPL state |
| `repl_state_views.h` | Read-only (by-value) state getters; safe to include from `scene_*` and `ui_*` |
| `repl_state_owners.h` | Mutable `_mut()` accessors; owner modules and controller only |
| `src/editor/input.c` | GLUT-callback dispatch (`editor_handle_key/special/mouse/motion/passive_motion/mousewheel`), 19 keyboard route helpers + 9 special route helpers, router stubs for non-editor concerns, commit orchestration, `feed_line` |
| `src/editor/input.h` | Editor input dispatch entry points + `editor_input_router_*` router stubs + `ReplInputDispatchEffects` typedef + `editor_input_active_modifiers` test seam |
| `repl_keys.h` | ASCII and control-key code constants (Ctrl+A=1 … Ctrl+Z=26, F-key names) |
| `src/editor/clipboard.c` | Line selection anchors, command clipboard buffer, copy/cut/paste behavior |
| `src/editor/clipboard.h` | Clipboard public API |
| `src/editor/undo.c` | Undo/redo snapshots, history rings, example auto-promote hook before mutation |
| `src/editor/undo.h` | Undo public API (`editor_undo_push_snapshot`, `editor_undo_pop_snapshot`, `editor_undo_do_redo`) |
| `repl_camera_controls.c` | Scene camera pointer state, orbit/pan/zoom drags, wheel zoom velocity, momentum tick |
| `glr_actions.c` | Config descriptor table, config shortcuts, menu actions, startup config defaults |
| `glr_actions.h` | Actions public API (`glr_action_menu_item_activate`, cursor-pixel setter, etc.) |
| `src/ui/code_panel_layout.c` | Pure code-panel wrapping, row counts, segment lookup, cursor-row mapping |
| `src/ui/code_panel_layout.h` | `CodePanelTextLayout` / `CodePanelWrapIter` API shared by UI, export dumps, tests |
| `src/editor/code_panel_document.c` | Code-panel document row model, scroll-follow calculation, hit-test targets |
| `src/editor/code_panel_document.h` | `CodePanelDocumentLayout` API consumed by UI and scrolling tests |
| `repl_executor.c` | Narrow live-GL dispatch: walks the flat command array emitting OpenGL calls |
| `repl_executor.h` | Executor public API (`repl_execute_program`, transform helpers) |
| `repl_flatten.c` | Source-to-flat program builder: unrolls loops, inlines functions, resolves if-blocks |
| `repl_flatten.h` | Flatten public API (`repl_flatten_program`, cursor-highlight refresh) |
| `repl_pipeline.h` | Pipeline and lifecycle surface for frame orchestration (flatten, autonormal, replay snapshots) |
| `repl_autonormal.c` | Auto-generated `glNormal3f` maintenance for source commands |
| `replay.c` | Replay state machine: PC, mode (OFF/PLAYING/PAUSED/DONE), speed, fade-batch ring |
| `replay.h` | Replay public API (`repl_replay_start`, `repl_replay_toggle_play_pause`, etc.) |
| `src/editor/search.c` | Case-insensitive substring search state and match navigation |
| `src/editor/search.h` | Search query helpers and input routing API |
| `repl_autocomplete.c` | REPL-side completion provider: walks command spec / predef vars / `CMD_FUNC_DEF` for matches, ghost text, parameter hints. Registered via `EditorCompletionProvider`. |
| `src/ui/layout.c` | Pure window layout geometry: scene rect and code-panel rect derivation |
| `src/ui/layout.h` | Layout geometry API (`ui_layout_scene_rect`, `ui_layout_code_panel_rect`) |
| `repl_scenes.c` | User-scene slots, LRU eviction, workspace save/load, workspace dir binding |
| `repl_example_loader.c` | Built-in example loading and active-example tracking |
| `glr_debug.c` | Diagnostic dumps for CLI flags and tests |
| `glr_debug.h` | Debug dump public API |
| `repl_replay_annotations.c` | Replay-time source annotations, variable substitution, evaluated command display text |
| `repl_replay_annotations.h` | Code-panel replay annotation API |
| `src/ui/snapshot.h` | `UiRenderSnapshot` — frame-frozen bundle built once per frame by `glr_ctrl_build_ui_snapshot()` |
| `src/ui/editor.h` | Per-frame editor-overlay snapshots (swatches, sliders, highlights) pushed by the controller |
| `replay_ui_hud.c` | 2D replay status HUD (feature-UI under the `replay_ui_*` prefix; reads replay peer snapshot) |
| `replay_ui_hud.h` | Replay HUD render entrypoint |
| `src/ui/profile_panel.c` | CPU profiling overlay panel (per-frame section timings) |
| `src/ui/profile_panel.h` | Profile panel render entrypoint |
| `src/ui/menu_bar.c` | Code-panel menu bar, dropdowns, config right-click handling, search slot |
| `src/ui/menu_bar.h` | Menu/pin hit-test and dropdown state API |
| `color_picker_state.c` | Floating color picker peer: state, lifecycle, slider input handlers, source-line writeback through editor commit |
| `color_picker_state.h` | Peer API (`ColorPickerView`, `ColorPickerInputResult`, `color_picker_open/close/handle_*`, `color_picker_hsv_to_rgb`) |
| `src/ui/color_picker.c` | Floating color picker renderer + hit-test (pure, takes `ColorPickerView *`) |
| `src/ui/color_picker.h` | Picker UI render/hit-test API + `UI_COLOR_SWATCH_W` |
| `src/ui/tabbed_overlay.c` | Generic modal tabbed text overlay renderer (the F1 help overlay's UI shell) |
| `src/ui/tabbed_overlay.h` | Tabbed-overlay render API (`UiOverlayState`, `UiOverlayContent`) |
| `repl_help_text.c` | Builds the F1 help overlay's content table (commands, key bindings) consumed by `tabbed_overlay` |
| `repl_help_text.h` | Help-content public API |
| `src/ui/variable_panel.c` | Floating variable slider panel rendering, geometry, and hit-test |
| `src/ui/variable_panel.h` | Variable panel render/rect/hit API |
| `src/ui/autocomplete_panel.c` | Floating autocomplete popup renderer (reads autocomplete state populated by `repl_autocomplete.c`) |
| `src/ui/autocomplete_panel.h` | Autocomplete popup render entrypoint |
| `src/editor/inline_rename.c` | Inline scene-rename input buffer and key handling (status-bar overlay) |
| `src/editor/inline_rename.h` | Rename begin/active/cancel/key/special API |
| `variable_panel_drag.c` | Variable slider drag transaction: begin/motion/reset, linear/log value writeback |
| `variable_panel_drag.h` | Drag state accessors + begin/motion/reset API |
| `variable_panel_state.c` | Variable-panel peer subsystem: owns visibility flag + drag-state storage |
| `variable_panel_state.h` | Peer-subsystem facade (`VariablePanelState`, capture/restore/reset, view/drag accessors) |
| `replay_state.c` | Replay peer subsystem: owns `ReplReplayRuntimeState` storage |
| `replay_state.h` | Peer-subsystem facade (`replay_state_capture/restore/reset/view/mut`) |
| `src/editor/help_session.c` | Read-only editor session for the help overlay (tab_idx + scroll) |
| `src/editor/help_session.h` | `EditorHelpSession` API (capture/restore/reset, narrow accessors) |
| `src/editor/completion.c` | Completion-provider registry: editor input invokes registered provider for autocomplete |
| `src/editor/completion.h` | `EditorCompletionProvider` struct + `editor_completion_register/update/clear` API |
| `repl_examples.c` | Predefined example data (`g_examples[]`, `g_example_names[]`) |
| `repl_examples.h` | Example query API (`repl_examples_count/name/lines`) |
| `repl_export.c` | `repl_export_save_output` / `repl_export_load_from_file`, workspace header directives, `@scene-name` / `@workspace-dir` markers |
| `repl_export.h` | Export/import public API and workspace-header pending-state types |
| `prof.c` | CPU wall-time profiling instrumentation (per-section accumulators, frame tick) |
| `prof.h` | Profiling API (`prof_begin`, `prof_end`, `prof_frame_tick`, etc.); no UI dependency |
| `src/scene/render_types.h` | Shared `SceneRgba` / `SceneRenderConfig` / `FrameRenderContext` types for scene helpers |
| `guides_shared.h` | Shared guide snapshot and planning types for REPL-aware 3D overlay passes (lives at root because no `src/scene/*.c` consumes it; see also `transform_utils.h`) |
| `geometry_guides.c` | Vertex/primitive guide rendering (input context at cursor) from `SceneGuideSnapshot` |
| `geometry_guides.h` | Geometry guides render entrypoint |
| `transform_guides.c` | Transform guide rendering (pending matrix ops during replay) |
| `transform_guides.h` | Transform guides render entrypoint |
| `transform_utils.h` | Header-only GL matrix helpers (`apply_tracked_transform`, `unwind_transform_stack`) mirroring executor transforms without requiring `repl_executor.h` |
| `src/scene/render.c` | 3D scene frame orchestration, one-shot init, scene config/frame prep, edit guides, orbit target, replay fade pass orchestration |
| `src/scene/grid.c` | Grid theme rendering and custom focus/ocean/ruler/planes passes |
| `src/scene/grid.h` | Grid render entrypoint |
| `src/scene/axes.c` | Axes theme rendering |
| `src/scene/axes.h` | Axes render entrypoint |
| `src/scene/render.h` | Declares `scene_render_3d_scene(const SceneRenderConfig *)` and `scene_apply_camera(...)` |
| `src/scene/backdrop.c` | Backdrop mode dispatch and deterministic cityscape renderer |
| `src/scene/backdrop.h` | Backdrop render entrypoint |
| `src/scene/lights.c` | Ambient init, light setup/reset, and visible light indicator overlay |
| `src/scene/lights.h` | Scene light setup/render entrypoints |
| `src/scene/overlays.c` | Tiny per-vertex GL primitives the controller calls (vertex-number labels, normal arrows). Outline / vertex-point passes moved to `glr_ctrl.c` |
| `src/scene/overlays.h` | Scene overlay primitive API |
| `src/ui/panels.c` | Code-panel row rendering (incl. inline ghost/hint text), scene status banner, top-level panel hit routing |
| `src/ui/panels.h` | Code-panel geometry, render, hit-test, and panel input bridge declarations |
| `repl_eval.c` | Expression evaluator (recursive descent), REPL<->C translators, for-loop parsers |
| `repl_eval.h` | Evaluator types (`ExprVar`, `ExprCtx`), function declarations |
| `cmd_format.c` | Pure indentation/depth computation (no GL dependency) |
| `cmd_format.h` | Formatting types (`FmtCmd`, `FmtType`), indent functions |
| `include/gl_2d.h` | Header-only 2D OpenGL helper functions |
| `tests/support/` | Shared test harness/setup helpers |
| `tests/gl-stubs/` | No-op GL/GLU/GLUT headers used by `USE_GL_STUBS=1` builds |
| `MODULES.md` | One-page layered overview, ownership diagram, current boundaries, open edges |

## Conventions

- File-private statics use `g_` prefix (e.g., `g_cfg_items[]`, `g_user_scenes[]`).
  Runtime state that crosses module boundaries is accessed through the typed
  facade in `repl_state.h` (e.g., `repl_state_render()`, `repl_state_search()`).
- Static helpers are file-scoped; public API goes through `repl_core.h`
- Prefixes express ownership. Use `repl_*` for REPL language/editor/source/
  replay model modules, `glr_*` for app shell/controller/app-service code,
  `scene_*` for 3D rendering, `ui_*` for 2D editor/view rendering, and neutral
  names such as `prof` for generic utilities. `repl_audio` is legacy-named and
  should be revisited with the deferred app-shell namespace work rather than
  copied as a pattern.
- Config toggles use the `ReplConfigItem` / `ReplConfigKey` pattern: add a
  descriptor entry to `g_cfg_items[]` in `glr_actions.c`; `CFG_ITEM_COUNT`
  auto-computes via `sizeof`
- New GL commands: add to the `CmdType` enum in `repl_command.h`, then
  handle in `repl_parser_parse_command_ctx()` in `repl_parser.c`,
  `repl_execute_program()` in `repl_executor.c`, and `flatten_range()`
  (static, inside `repl_flatten.c`). Add a `g_command_type_specs[]`
  entry in `repl_command_spec.c` with the right `CmdSyntaxCategory`
  so the new command picks up its code-panel highlight color
  automatically; if you need a `glEnable`-shaped enum-arg spec or a
  standard float-arg spec, append a row to `k_enum_command_specs[]`
  or `k_std_command_specs[]` in the same file.
- Keyboard bindings: `editor_handle_key()` for ASCII keys (Ctrl+X
  produces ASCII X & 0x1F via standard GLUT), `editor_handle_special()`
  for F-keys/arrows. Cross-subsystem routing (replay / save / config /
  audio / camera) lives in `glr_ctrl.c::glr_ctrl_router_*`
  helpers, called from `glr_ctrl_keyboard` before delegating to
  `editor_handle_key`. macOS Cmd+letter is normalized to its
  control-character form by `editor_input_normalize_super_to_ctrl`,
  called at the top of `glr_ctrl_keyboard` so every downstream
  dispatcher sees Cmd+B identically to Ctrl+B.
- Expression variables: `ExprVar` struct in `repl_eval.h`, predefined set
  accessible via `repl_state_variables()` and managed by `declare_predef_var()`

## Architecture

### Rendering Pipeline

`glr_ctrl_display_frame()` in `glr_ctrl.c` drives each frame.
`sample.c` registers the GLUT display callback and forwards directly — there
is no shim layer.
1. Rebuild autonormals and flat program if dirty; save predef var values;
   prepare replay frame if active; update export/camera strings
2. Build `SceneRenderConfig` from REPL state and call `scene_apply_camera(...)` then
   `scene_render_3d_scene(&cfg)` once per jitter sample (if accumulation-buffer AA is enabled).
   Jitter is applied as a scene-local frustum shift inside the scene function.
3. `scene_render_3d_scene(&cfg)` in `src/scene/render.c`: viewport/clear setup
   → projection → execute user geometry via `SceneExecuteProgramFn`
   callback → replay fade batches → grid/axes/backdrop/orbit-target →
   polygon-outline, vertex, normal, and guide overlays → 2D replay HUD
   (renders via `replay_ui_hud_render` from `replay_ui_hud.c`)
4. 2D overlays: code panel, autocomplete popup, example dropdown,
   variable slider panel, config menu, help overlay, search overlay

### Two-Level Command Model

The core data flow is **source commands → flat commands → GL calls**:

- **Source array** (`repl_state_document_cmds()`, count via
  `repl_state_document_count()`) — each `GLCmd` holds parsed type/args,
  normalized `source[]` text, and flags (`has_vars`, `valid`, `is_auto`).
  Edited directly by the user via the code panel.
- **Flat array** (`repl_state_flat_cmds()`) — expanded copy. For-loops are
  unrolled, function calls are inlined, if-blocks are resolved.
  Each flat cmd records `src_cmd_idx` (owning source line),
  `call_src_cmd_idx` (immediate call site), and `func_scope_mask`
  (active function scopes) for cursor highlighting.
- **Trigger:** any edit marks the flat array dirty (via `mark_normals_dirty()`);
  `flatten_commands()` rebuilds it on the next frame before rendering.

### Command Lifecycle

1. **Input** — user types into the input buffer (`repl_state_editor_input()->input`,
   max 1024 chars)
2. **Commit** — pressing `;` calls the commit dispatch chain in
   `keyboard_func()` in `src/editor/input.c`. There are TWO distinct paths:
   - **Interactive `;` key** (`src/editor/input.c`, `key == ';'` block):
     the input buffer does NOT include the `;` — the keystroke triggers the
     commit but is not appended. Commit handlers must accept input
     without a trailing `;`.
   - **`feed_line()`** (`src/editor/input.c`): copies the full line
     (including `;`) into the input buffer, then runs the same dispatch chain.
     Used by file loading and example loading.
   - **Enter key** (insert mode): input may or may not have `;`
     depending on what the user typed.
   The dispatch chain calls the consolidated `try_commit_*()` helpers
   in `src/editor/commit.c` (`try_commit_var_statements`,
   `try_commit_block_structs`, `try_commit_any`, plus the var-then-
   insert variant). Internally those run, in canonical order:
   `try_commit_float_decl` → `try_assign_variable` → `try_commit_close_brace`
   → `try_commit_for_loop` → `try_commit_func_def` → `try_commit_if_block`
   → `repl_parse_and_normalize()` (general GL commands).
   **Ordering matters**: `try_commit_float_decl` MUST run before
   `try_assign_variable`, otherwise `float x` is misread as an
   assignment. Each handler returns 1 if it consumed the input
   (success or error with status message), 0 if it didn't match.
   If all handlers return 0, `parse_command()` in `repl_parser.c`
   sets the per-context error buffer.
3. **Parse** — `parse_command()` in `repl_parser.c` matches the line to a
   `CmdType`, evaluates argument expressions via `eval_expr()`, stores
   result in `GLCmd.args[]`. Per-line canonical text lives in
   `ReplEditorBuffer.lines[]` (not on `GLCmd`); the parser returns it as
   `ReplParsedLine.text` for the commit path to write into the editor
   buffer. Internal call sites pass `ReplParseContext.source_line_idx`
   instead of temporarily changing the edit-line cursor.
4. **Flatten** — `flatten_range()` recursively expands the source array:
   for-loops iterate (capped at `MAX_FLATTEN_VISIT_BUDGET = 200000`
   visits), function calls inline the body with actual args, if-blocks
   evaluate conditions. Recursion depth limited to
   `MAX_FLATTEN_CALL_DEPTH = 64`.
5. **Execute** — `repl_execute_program()` walks the flat command array emitting GL
   calls. Re-evaluates expressions with `has_vars` flag each frame
   (for animated `t`, etc.)

### Commit Dispatch Sites

The `try_commit_*` handler chain is consolidated into four helpers in
`src/editor/commit.c`:
- `try_commit_var_statements()` — float decl, then assign
- `try_commit_block_structs()` — close-brace, for, func, if
- `try_commit_any()` — both groups in canonical order
- `try_commit_var_statements_then_insert()` — var variant used by the
  overwrite-mode Enter key, which must flip to insert mode on success

Dispatch sites then call these helpers instead of open-coding the chain:
1. **`;` key handler** — `key == ';'` block in `keyboard_func()` calls
   `try_commit_any()`
2. **Enter key, insert mode** — calls `try_commit_var_statements()` and
   `try_commit_block_structs()` to maintain the insert-mode behavior
3. **Enter key, overwrite mode** — uses
   `try_commit_var_statements_then_insert()` plus
   `try_commit_block_structs()`
4. **`feed_line()`** — the programmatic entry point calls
   `try_commit_any()`

When adding a new handler, add it to the right helper rather than all
call sites. Ordering inside each helper is load-bearing:
`try_commit_float_decl` MUST run before `try_assign_variable`, otherwise
`float x;` is misread as an assignment to an identifier named "float".

### Editing Existing Lines

When the user navigates to an existing line, `load_line_to_input()`
strips the trailing `;` and whitespace from `cmd.source` before
loading into the input buffer. This means re-committing the line goes
through the no-semicolon path. Commit handlers that check for `;`
must also accept end-of-string as a valid terminator.

### Float Variable Declarations (`CMD_VAR_DECLARE`)

`try_commit_float_decl()` in `src/editor/commit.c` handles `float name;`
syntax. Current implementation supports multi-name (`float a, b, c;`)
and initializers (`float x = 1;`), but there is an open design
question about simplifying to single-name, no-initializer only.

Key details:
- **Placement rule:** new `CMD_VAR_DECLARE` lines are inserted at the
  top of non-decl code (index of first non-`CMD_VAR_DECLARE` cmd),
  regardless of cursor position. This guarantees every reference
  follows its declaration (no `n = tmp` before `float tmp`). Editing
  an existing decl still overwrites in place. Init expressions can
  therefore only reference already-declared predef vars — no scope
  locals are visible at block depth 0.
- `CMD_VAR_DECLARE` is a no-op in `repl_execute_program()` and
  `flatten_range()` — registration into the predefined-variable table happens at
  commit time via `declare_predef_var()`
- `GLCmd` fields: `var_names[MAX_NAMES_PER_DECL][16]`, `var_decl_count`
- Editing an existing `CMD_VAR_DECLARE` line works: the overwrite
  detection runs before the "already declared" validation loop, and
  names carried over from the old decl are exempted from the duplicate
  check (they get undeclared before the new registration runs).
- `delete_cmd_range()` guards against deleting a declaration whose
  variable is still referenced outside the deleted range (uses
  `repl_eval_source_uses_ident()` against every line not in the range).
  Deleting a decl together with all its uses is allowed; deleting an
  unreferenced decl by itself is allowed. Cut/copy/paste of decl rows
  remain blocked outright (clipboard semantics — see commit 72be1dd).
- C export writes `// @declare name` markers; import via
  `import_parse_declare_marker()` in `repl_export.c` reconstructs
  the `CMD_VAR_DECLARE` commands, bypassing `try_commit_float_decl`
- `repl_examples.c` has multi-name declarations (e.g. `"float n, x, y, z, j, k;"`)
  — if simplifying to single-name, these must be split into separate lines
- Related helpers in `repl_eval.c`: `declare_predef_var()`,
  `undeclare_predef_var()`, `find_predef_var_idx()`,
  `is_reserved_ident()`, `source_uses_ident()`,
  `validate_expression_idents()`

### Save/Load (output.c)

`repl_export.c` handles bidirectional text format:
- **Export** (`repl_export_save_output()`): writes a standalone C file with header
  comments embedding workspace state (`@var name=value`,
  `@cfg setting=value`, `@scene-name <name>`, `@workspace-dir <path>`),
  camera state as the raw `glTranslatef`/`glRotatef` sequence the REPL
  uses internally, predefined vars plus fixed scratch arrays `A/B/C[8]` as
  globals, REPL functions as C
  functions, and `display()` body containing the user's geometry commands.
  The workspace iterator in `repl_core.c` sets the export scene-name hint
  in import/export state before each slot's save so the hint wins over the
  active user scene index.
- **Import** (`repl_export_load_from_file()`): line-by-line scan parses camera state
  and workspace directives, detects function definitions (converts C
  syntax back to REPL), and feeds geometry lines through `feed_line()`.
  Pending scene-name and workspace-dir directives are read by the caller
  after `repl_export_load_from_file` returns so the importer can name the new slot
  and remember the workspace dir.

### Replay System

Step-by-step execution visualization in `replay.c`:
- `ReplReplayRuntimeState` (via `repl_state_replay()`) tracks state
  (OFF/PLAYING/PAUSED/DONE), program counter, and speed multiplier
- During playback, the flat command count is clamped to `replay_exec_limit()`
  so only commands up to the PC render
- Fade batch ring buffer — fading geometry snapshots; old geometry fades out
  as new geometry appears, rendered in a separate blended pass after the
  main fill pass
- Toggled via Ctrl+G or the Replay header button

### Undo/Redo

Circular snapshot buffers in `src/editor/undo.c`:
- `ReplUndoSnapshot` captures the full editor state: source commands,
  command count, cursor position, predefined variable values
- Undo and redo rings (32 slots each) with head/count tracking
- `editor_undo_push_snapshot()` called before any mutation (delete, paste,
  reformat, etc.); `editor_undo_pop_snapshot()` on Ctrl+Z; `editor_undo_do_redo()` on
  Ctrl+Y. Also the hook where `repl_promote_example_if_needed()` fires
  so editing an example auto-creates a user scene.
- Pushing clears the redo stack; undo moves current state to redo

### Autocomplete

Symbol matching and function parameter hints in `repl_autocomplete.c` (registered as the editor's `EditorCompletionProvider`):
- `repl_state_autocomplete()->matches` — matched completions from GL command/constant tables
- `repl_state_autocomplete()->ghost` — suffix to append to input on Tab accept
- `repl_state_autocomplete()->hint` — parameter list hint shown below cursor
- Modes: `AC_MODE_FUNC_PREFIX` (after `foo(` → param hints),
  `AC_MODE_ENUM_ARG1/2` (GL constant completion),
  `AC_MODE_POINT_PARAM` (3D point coordinates)

### Search

Case-insensitive text search in `src/editor/search.c`:
- Activated by Ctrl+F; query and state accessed via `repl_state_search()`
- `repl_search_find_next_in_text()` finds substring matches across
  all visible lines (header, user code, footer)
- `hit_line_idx`/`hit_char_idx` in `ReplSearchState` track current match position
- Integrated with code panel rendering for match highlighting

### Config Menu

Declarative toggle system in `glr_actions.c`:
- `g_cfg_items[]` array of `ReplConfigItem` descriptors: `{ label, key_code,
  is_special, key, state_count, state_names[], section_header }`
- Each item is a toggle (2 states, default OFF/ON) or cycle (>2 states
  with named entries, e.g. grid themes)
- Rendered by the Config dropdown in `src/ui/menu_bar.c`; menu clicks and
  F-key/Ctrl-key shortcuts dispatch through `glr_actions.c`
- Adding a config item: append to `g_cfg_items[]` — count is
  auto-computed via `sizeof`

## Key Controls

| Key | Action |
|-----|--------|
| `;` | Execute/commit current line |
| Enter | Insert new line |
| Up/Down | Navigate lines |
| Tab | Autocomplete |
| Ctrl+S | Save to output.c |
| Ctrl+Z | Undo |
| Ctrl+R | Reformat all lines |
| Ctrl+T | Toggle time variable `t` |
| F1 | Help overlay |
| F2-F11 | Toggle visual overlays |
| F12 | Cycle examples and user scenes |

## Supported Commands

```
glBegin(MODE), glEnd()
glVertex3f(x,y,z), glVertex2f(x,y)
glNormal3f(x,y,z)
glColor3f(r,g,b), glColor4f(r,g,b,a)
glTranslatef(x,y,z), glScalef(sx,sy,sz), glRotatef(deg,x,y,z)
glPushMatrix(), glPopMatrix()
glEnable(CAP), glDisable(CAP)
  CAP: GL_DEPTH_TEST, GL_LIGHTING, GL_COLOR_MATERIAL, GL_NORMALIZE,
       GL_LINE_SMOOTH, GL_POINT_SMOOTH, GL_BLEND, GL_CULL_FACE,
       GL_LIGHT0, GL_LIGHT1, GL_LIGHT2, GL_LIGHT3
glShadeModel(MODE)
glPointSize(size)
glLineWidth(width)
glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, const, linear, quadratic)
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA|GL_ONE)
glColorMaterial(face, mode), glMaterialf(face, pname, value)
  glColorMaterial mode: GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR, GL_EMISSION, GL_AMBIENT_AND_DIFFUSE
glLightModeli(pname, param), glFrontFace(mode)
glDepthMask(GL_TRUE|GL_FALSE)
GLUT Solid Shapes:
  glutSolidTorus(inner, outer, nsides, rings)
  glutSolidCube(size)
  glutSolidSphere(radius, slices, stacks)
  glutSolidTeapot(size)
  glutSolidCone(base, height, slices, stacks)
glRasterPos3f(x, y, z)
  - Sets the current raster position; transforms (x, y, z) through
    the active modelview/projection. Pair with `label(...)` to draw
    bitmap text.
Bitmap Text:
  label("fmt", a, b, c, d)
    - Renders text at the current raster position (set by a
      preceding glRasterPos3f). Does not modify GL state itself.
      Font is fixed to GLUT_BITMAP_9_BY_15.
    - "fmt" supports %f (substitution from a/b/c/d) and %% (literal '%').
    - Up to 4 substitution args; format-string limit is 64 chars.
    - Forbidden inside the string: '//', '(', ')', ',' and any
      backslash. The parser rejects with a status error if any
      appear (graceful — line is not committed).
    - REPL-specific primitive; not a real GL/GLUT symbol. The
      exporter emits a self-contained static `label(...)` helper
      in the file's prologue (gated on `needs_label`) using
      vsnprintf + glutBitmapCharacter, so exported files compile
      standalone against vanilla freeglut.

    Distinct from the goto-label syntax `:name` / `name:` — those use
    a colon and live on CMD_LABEL. `label(...)` is a function call.
for(var, start, end[, step]) { body }
func0..func9(params) { body }   (parens always required, even for zero args)
NAME(params) { body }     (alias: NAME -> next free funcN slot, 10 max)
if(expr) { body }
// comment
float name[, name2, ...];  (variable declaration)
var = expr;
A[index] = expr;           (fixed scratch arrays: A/B/C, index 0..7)
```

## Math

Functions: `sin`, `cos`, `tan`, `sqrt`, `abs`, `pow`, `min`, `max`, `floor`, `ceil`, `fmod`, `rem`, `rand(seed[, iter])`, `rand2(seed[, iter])`

`rand` returns a value in `[0, 1]`. `rand2` is the same hash mapped
to `[-1, 1]` — useful for centered jitter, signed offsets, etc. Both
are deterministic for a given (seed, iter) pair.
Constants: `PI`, `TAU`
Variables: declared via `float name;` — only `t` is predefined (Ctrl+T toggles animation).
Scratch arrays: `A[8]`, `B[8]`, `C[8]` are fixed global runtime arrays for recursive/loop algorithms.
Reads and writes use normal expression syntax; indices are truncated with `(int)` and must stay in `0..7`.
Other names (`x`, `y`, `z`, etc.) must be declared before use.
`MAX_PREDEF_VARS` = 24 (1 reserved for `t`, 23 user-declarable slots). The
float-decl handler rejects new declarations once the table is full with
`"variable table full (max 24)"`.

Example:

```c
A[0] = 0;
A[1] = 1;
A[0] = A[0] + (A[1] - A[0]) * 0.25;
glVertex3f(A[0], 0, 0);
```
