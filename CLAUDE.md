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

`make check-state-ownership` runs the full inventory of
ownership / contract guards. Highlights:

- `check-no-repl-commit` — `repl_commit.c/h` cannot reappear
  (commit dispatch lives in `editor_commit.c`).
- `check-no-repl-editor-input-shim` — `editor_input.c` cannot include
  the deleted `repl_editor.h` or call legacy `repl_*_func` dispatch
  bodies (Phase J1 closed the input boundary; `repl_editor.{c,h}`
  is gone).
- `check-no-set-status-in-repl-parser` — parser core never calls
  `set_status` (baseline **0/0** after Phase J5 retired the legacy
  no-ctx wrappers `repl_parser_parse_command` / `_with_vars` and the
  `parser_legacy_surface_to_status` bridge; tests now own status
  surfacing locally via their own `parse_for_test` helper).
- `check-no-set-status-in-compile-apply` — `repl_compile.c` and
  `repl_apply.c` are status-free (Phase C purity).
- `check-no-test-default-output` — hard guard. Tests may not call
  `repl_save_default_output()`, which writes to the hardcoded relative
  path `./output.c` (the repo root when tests run). Tests that need to
  verify save behavior must call `repl_export_save_output()` with an
  explicit `/tmp/` path instead.
- `check-no-store-text-api` — `repl_command_store_*_with_line[s]`
  text-aware overloads stay deleted.
- `check-repl-no-direct-buffer-read` — REPL files read text via
  `EditorBufferView`, not directly into editor buffer.
- `check-ui-returns-hits-only` — `ui_*.c` is mutator-free in input
  + render paths. Baseline reached **0/0** (Phase J4 routed the
  cursor-pixel publish through `UiCodePanelOutput` and deleted the
  `cursor_px/cursor_py` state slice along with the
  `check-cursor-px-encapsulated` migration guard). Any new mutator
  fails the build.
- `check-ui-panels-no-mutators` — Phase J2.2 hard guard. `ui_panels.c`
  is hit-test only: zero matches for code-panel press / click / drag /
  release / scene-press / motion / mouse-release / escape forwarders
  and color-picker open/close/press/motion/release / replay-pin /
  search / menu open-close-activate calls. No allowlist.
- `check-replay-ui-isolation` — feature-UI prefix discipline.
  `replay_ui_*.c` may render replay HUD/buttons, hit-test replay
  controls, route hits via `replay_handle_*`, and read replay
  snapshots, but must not mutate editor/REPL state, call
  parser/compile/apply, or grow generic `ui_*` responsibilities.
- `check-color-picker-ui-isolation` — stricter feature-UI guard.
  `color_picker_ui*.c` is a pure renderer + hit-test over
  `ColorPickerView`; no mutators, no live REPL/editor state reads,
  no parser/compile/apply, no `set_status`. Lifecycle and writeback
  belong on the peer (`color_picker.c`).
- `check-variable-panel-forwarders` — baseline **0/0** after Phase
  J6 migrated every fixture site onto `variable_panel_drag` /
  `variable_panel_view` / `variable_panel_handle_drag_*`, and Phase
  J7 deleted the legacy `editor_state_variable_drag*` /
  `ui_state_variable_panel*` / `repl_var_drag_*` shims. The
  canonical peer accessors are the only entry points.
- `check-replay-forwarders` — baseline **0/0** after Phase J5
  migrated every `bench/` + `tests/` site onto `replay_state_view` /
  `replay_state_mut` / `replay_handle_*`, and Phase J7 deleted the
  legacy `repl_state_replay` / `_mut` / `_reset` forwarders.
- `check-imrepl-not-editor-mirror` — `imrepl_ctrl_editor_*` per-field
  wrappers are forbidden.
- `check-editor-ownership-budget` — UI-slice forwarder + facade
  include counts only shrink.

## File Layout

| File | Responsibility |
|------|----------------|
| `sample.c` | GLUT callback registration, `main()`, window setup, buffer swap; forwards directly to `imrepl_ctrl_*` |
| `sample.h` | Shared types (`GLCmd`, `CmdType`, `SceneLight`), defaults, stateless helpers, compatibility includes |
| `imrepl_ctrl.c` | App-frame controller: `imrepl_ctrl_display_frame`, `imrepl_ctrl_reshape`, `imrepl_ctrl_init_gl`; builds `SceneRenderConfig`, calls scene/UI renderers |
| `imrepl_ctrl.h` | Controller public surface: display, reshape, init-GL entrypoints |
| `repl_config.c` | Config key implementation and descriptor table helpers |
| `repl_config.h` | `ReplConfigKey` / `ReplConfigItem` descriptor API for keyed config access |
| `repl_core.c` | Normalization pipeline (`repl_parse_and_normalize*`), reformatter, startup helpers; being dissolved into natural owners (R10) |
| `repl_parser.c` | REPL source-line parser, expression validation, canonical `GLCmd.source[]` generation |
| `repl_parser.h` | Parser entrypoints (`repl_parser_parse_command*`, `repl_parser_parse_command_ctx`) and `ReplParseContext` |
| `repl_source_scope.c` | Source prefix-depth cache, indentation helpers, block lookup |
| `repl_source_scope.h` | Source-scope query API (`repl_source_scope_block_depth_at`, `repl_source_scope_find_block_end`, indent helpers) |
| `repl_command_spec.c` | Command type metadata and specifications (parsing, formatting, completion requirements) |
| `repl_command_spec.h` | Command spec query API |
| `repl_command_store.c` | Source-command array mutations: insert, delete, replace, bulk-load |
| `repl_command_store.h` | Command-store public API (`repl_command_store_insert_one`, etc.) |
| `repl_core.h` | Public API (parse, flatten, user scene + workspace); GLUT input-dispatch declarations (`repl_keyboard_func` etc.) are live — called from `imrepl_ctrl.c` — pending R10-phase1 re-evaluation |
| `repl_core_internal.h` | Test-visible internals (normalize/commit pipeline, `feed_line`, `load_line_to_input`, `repl_promote_example_if_needed`) |
| `repl_state.c` | Owns `g_repl_state`, lifecycle, snapshot assembly (`repl_state_capture` / `repl_state_restore`) |
| `repl_state.h` | Typed runtime-state facade, reset helpers, and focused accessors over the live REPL state |
| `repl_state_views.h` | Read-only (by-value) state getters; safe to include from `scene_*` and `ui_*` |
| `repl_state_owners.h` | Mutable `_mut()` accessors; owner modules and controller only |
| `editor_input.c` | GLUT-callback dispatch (`editor_handle_key/special/mouse/motion/passive_motion/mousewheel`), 19 keyboard route helpers + 9 special route helpers, router stubs for non-editor concerns, commit orchestration, `feed_line` |
| `editor_input.h` | Editor input dispatch entry points + `editor_input_router_*` router stubs + `ReplInputDispatchEffects` typedef + `editor_input_active_modifiers` test seam |
| `repl_keys.h` | ASCII and control-key code constants (Ctrl+A=1 … Ctrl+Z=26, F-key names) |
| `editor_clipboard.c` | Line selection anchors, command clipboard buffer, copy/cut/paste behavior |
| `editor_clipboard.h` | Clipboard public API |
| `editor_undo.c` | Undo/redo snapshots, history rings, example auto-promote hook before mutation |
| `editor_undo.h` | Undo public API (`editor_undo_push_snapshot`, `editor_undo_pop_snapshot`, `editor_undo_do_redo`) |
| `repl_camera_controls.c` | Scene camera pointer state, orbit/pan/zoom drags, wheel zoom velocity, momentum tick |
| `repl_actions.c` | Config descriptor table, config shortcuts, menu actions, startup config defaults |
| `repl_actions.h` | Actions public API (`repl_action_menu_item_activate`, cursor-pixel setter, etc.) |
| `ui_code_panel_layout.c` | Pure code-panel wrapping, row counts, segment lookup, cursor-row mapping |
| `ui_code_panel_layout.h` | `CodePanelTextLayout` / `CodePanelWrapIter` API shared by UI, export dumps, tests |
| `editor_code_panel_document.c` | Code-panel document row model, scroll-follow calculation, hit-test targets |
| `editor_code_panel_document.h` | `CodePanelDocumentLayout` API consumed by UI and scrolling tests |
| `repl_executor.c` | Narrow live-GL dispatch: walks the flat command array emitting OpenGL calls |
| `repl_executor.h` | Executor public API (`repl_execute_program`, transform helpers) |
| `repl_flatten.c` | Source-to-flat program builder: unrolls loops, inlines functions, resolves if-blocks |
| `repl_flatten.h` | Flatten public API (`repl_flatten_program`, cursor-highlight refresh) |
| `repl_pipeline.h` | Pipeline and lifecycle surface for frame orchestration (flatten, autonormal, replay snapshots) |
| `repl_autonormal.c` | Auto-generated `glNormal3f` maintenance for source commands |
| `replay.c` | Replay state machine: PC, mode (OFF/PLAYING/PAUSED/DONE), speed, fade-batch ring |
| `replay.h` | Replay public API (`repl_replay_start`, `repl_replay_toggle_play_pause`, etc.) |
| `editor_search.c` | Case-insensitive substring search state and match navigation |
| `editor_search.h` | Search query helpers and input routing API |
| `editor_autocomplete.c` | Completion model: symbol matching, ghost text, parameter hints |
| `ui_layout.c` | Pure window layout geometry: scene rect and code-panel rect derivation |
| `ui_layout.h` | Layout geometry API (`repl_layout_scene_rect`, `repl_layout_code_panel_rect`) |
| `repl_scenes.c` | User-scene slots, LRU eviction, workspace save/load, workspace dir binding |
| `repl_example_loader.c` | Built-in example loading and active-example tracking |
| `repl_debug.c` | Diagnostic dumps for CLI flags and tests |
| `repl_debug.h` | Debug dump public API |
| `repl_replay_annotations.c` | Replay-time source annotations, variable substitution, evaluated command display text |
| `repl_replay_annotations.h` | Code-panel replay annotation API |
| `ui_snapshot.h` | `UiRenderSnapshot` — frame-frozen bundle built once per frame by `imrepl_ctrl_build_ui_snapshot()` |
| `ui_editor.h` | Per-frame editor-overlay snapshots (swatches, sliders, highlights) pushed by the controller |
| `replay_ui_hud.c` | 2D replay status HUD (feature-UI under the `replay_ui_*` prefix; reads replay peer snapshot) |
| `replay_ui_hud.h` | Replay HUD render entrypoint |
| `ui_profile_panel.c` | CPU profiling overlay panel (per-frame section timings) |
| `ui_profile_panel.h` | Profile panel render entrypoint |
| `ui_menu_bar.c` | Code-panel menu bar, dropdowns, config right-click handling, search slot |
| `ui_menu_bar.h` | Menu/pin hit-test and dropdown state API |
| `color_picker.c` | Floating color picker peer: state, lifecycle, slider input handlers, source-line writeback through editor commit |
| `color_picker.h` | Peer API (`ColorPickerView`, `ColorPickerInputResult`, `color_picker_open/close/handle_*`, `color_picker_hsv_to_rgb`) |
| `color_picker_ui.c` | Floating color picker renderer + hit-test (pure, takes `ColorPickerView *`) |
| `color_picker_ui.h` | Picker UI render/hit-test API + `UI_COLOR_SWATCH_W` |
| `ui_help_overlay.c` | Modal F1 help overlay (Commands / Keys tabs, dynamic F-key bindings) |
| `ui_help_overlay.h` | Help overlay render entrypoint |
| `ui_variable_panel.c` | Floating variable slider panel rendering, geometry, and hit-test |
| `ui_variable_panel.h` | Variable panel render/rect/hit API |
| `ui_autocomplete_panel.c` | Floating autocomplete popup renderer (reads `editor_autocomplete.c` model) |
| `ui_autocomplete_panel.h` | Autocomplete popup render entrypoint |
| `editor_inline_rename.c` | Inline scene-rename input buffer and key handling (status-bar overlay) |
| `editor_inline_rename.h` | Rename begin/active/cancel/key/special API |
| `variable_panel_drag.c` | Variable slider drag transaction: begin/motion/reset, linear/log value writeback |
| `variable_panel_drag.h` | Drag state accessors + begin/motion/reset API |
| `variable_panel.c` | Variable-panel peer subsystem: owns visibility flag + drag-state storage (Phase F) |
| `variable_panel.h` | Peer-subsystem facade (`VariablePanelState`, capture/restore/reset, view/drag accessors) |
| `replay_state.c` | Replay peer subsystem: owns `ReplReplayRuntimeState` storage (Phase F commit 33) |
| `replay_state.h` | Peer-subsystem facade (`replay_state_capture/restore/reset/view/mut`) |
| `editor_help_session.c` | Read-only editor session for the help overlay (tab_idx + scroll; Phase G commit 35) |
| `editor_help_session.h` | `EditorHelpSession` API (capture/restore/reset, narrow accessors) |
| `editor_completion.c` | Completion-provider registry: editor input invokes registered provider for autocomplete (Phase G commit 36) |
| `editor_completion.h` | `EditorCompletionProvider` struct + `editor_completion_register/update/clear` API |
| `repl_examples.c` | Predefined example data (`g_examples[]`, `g_example_names[]`) |
| `repl_examples.h` | Example query API (`repl_examples_count/name/lines`) |
| `repl_export.c` | `repl_export_save_output` / `repl_export_load_from_file`, workspace header directives, `@scene-name` / `@workspace-dir` markers |
| `repl_export.h` | Export/import public API and workspace-header pending-state types |
| `prof.c` | CPU wall-time profiling instrumentation (per-section accumulators, frame tick) |
| `prof.h` | Profiling API (`prof_begin`, `prof_end`, `prof_frame_tick`, etc.); no UI dependency |
| `scene_render_types.h` | Shared `SceneRgba` / `SceneRenderConfig` / `FrameRenderContext` types for scene helpers |
| `scene_guides_shared.h` | Shared guide snapshot and planning types for REPL-aware 3D overlay passes |
| `scene_geometry_guides.c` | Vertex/primitive guide rendering (input context at cursor) from `SceneGuideSnapshot` |
| `scene_geometry_guides.h` | Geometry guides render entrypoint |
| `scene_transform_guides.c` | Transform guide rendering (pending matrix ops during replay) |
| `scene_transform_guides.h` | Transform guides render entrypoint |
| `scene_transform_utils.h` | Header-only GL matrix helpers mirroring executor transforms without requiring `repl_executor.h` |
| `scene_render.c` | 3D scene frame orchestration, one-shot init, scene config/frame prep, edit guides, orbit target, replay fade pass orchestration |
| `scene_grid.c` | Grid theme rendering and custom focus/ocean/ruler/planes passes |
| `scene_grid.h` | Grid render entrypoint |
| `scene_axes.c` | Axes theme rendering |
| `scene_axes.h` | Axes render entrypoint |
| `scene_render.h` | Declares `scene_render_3d_scene(const SceneRenderConfig *)` |
| `scene_backdrop.c` | Backdrop mode dispatch and deterministic cityscape renderer |
| `scene_backdrop.h` | Backdrop render entrypoint |
| `scene_lights.c` | Ambient init, light setup/reset, and visible light indicator overlay |
| `scene_lights.h` | Scene light setup/render entrypoints |
| `scene_overlays.c` | Polygon outline/current-block, vertex-number, and normal-vector overlays |
| `scene_overlays.h` | Scene overlay render/helper API |
| `ui_panels.c` | Code-panel row rendering (incl. inline ghost/hint text), scene status banner, top-level panel hit routing |
| `ui_panels.h` | Code-panel geometry, render, hit-test, and panel input bridge declarations |
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
  replay model modules, `imrepl_*` for app shell/controller/app-service code,
  `scene_*` for 3D rendering, `ui_*` for 2D editor/view rendering, and neutral
  names such as `prof` for generic utilities. `repl_audio` is legacy-named and
  should be revisited with the deferred app-shell namespace work rather than
  copied as a pattern.
- Config toggles use the `ReplConfigItem` / `ReplConfigKey` pattern: add a
  descriptor entry to `g_cfg_items[]` in `repl_actions.c`; `CFG_ITEM_COUNT`
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
  audio / camera) lives in `imrepl_ctrl.c::imrepl_ctrl_router_*`
  helpers, called from `imrepl_ctrl_keyboard` before delegating to
  `editor_handle_key`. macOS Cmd+letter is normalized to its
  control-character form by `editor_input_normalize_super_to_ctrl`,
  called at the top of `imrepl_ctrl_keyboard` so every downstream
  dispatcher sees Cmd+B identically to Ctrl+B.
- Expression variables: `ExprVar` struct in `repl_eval.h`, predefined set
  accessible via `repl_state_variables()` and managed by `declare_predef_var()`

## Adding Or Migrating An Owner Module

When a module starts owning mutable REPL state, follow the Stage-1 template:

1. Put the live bytes in `ReplRuntimeState` unless the state is intentionally a
   sidecar such as undo rings or user-scene slots. If it is a sidecar, call
   that out explicitly instead of describing it as runtime-state migration.
2. Add a named runtime slice in `repl_state.h`, wire it into
   `static ReplRuntimeState g_repl_state;`, and say whether the read path is
   currently `facade-backed`, `direct-runtime`, or `value-getter`.
3. Keep mutations on the owner side. Scene/UI renderers read snapshots only;
   render-time discoveries return through output structs that the controller
   actualizes back into state.
4. Extend the ownership tests in the same change: keep
   `repl_state_capture()`, `repl_state_restore()`, and `repl_state_reset_all()`
   current for runtime slices, and add focused behavior coverage in the
   module's own tests.

## Adding Grid/Axes Themes

Grids and axes are themeable through small specs in `scene_grid.c` and
`scene_axes.c`:
1. Add a new entry to the `GridTheme` (or `AxesTheme`) enum in `sample.h`
   before the trailing `_COUNT` sentinel
2. Add the name string at that enum's index in `g_grid_names[]`
   (or `g_axes_names[]`) in `repl_core.c` — both arrays use designated
   initializers keyed on the enum, so order is validated at compile time
3. Add a matching `GridThemeSpec` entry in `scene_grid.c` for standard grid
   line/color themes and an `AxesThemeSpec` entry in `scene_axes.c` for
   standard axes themes. Keep custom geometry-heavy grid cases in
   `scene_grid.c`.
4. The theme cycles via F3 (grid) / F4 (axes); the special-key route
   in `editor_input.c` calls `repl_cfg_handle_special_shortcut`, which
   walks `g_cfg_items[]` in `repl_actions.c` and cycles the matching
   config row.

## Adding Menu Bar Items

The top row is a menu bar in `ui_menu_bar.c` styled after the Header
Wireframes v2 mock. Left side has top-level menus (File / Scene / Config);
right side has pinned buttons (Search / Replay).

To add an **item** to an existing top-level menu:
1. Extend the per-menu enum (e.g. `FILE_ITEM_*` or the `SCENE_OFF_*` block) and
   bump the trailing `*_COUNT`
2. Add the label in `menu_item_label()` and shortcut (if any) in
   `menu_item_shortcut()` in `ui_menu_bar.c`
3. Add the action branch in `repl_action_menu_item_activate()` in
   `repl_actions.c`; return `1` for action items (menu closes), `0` for
   cycle/toggle items (menu stays open; click-outside dismisses)

To add a **new top-level menu**: extend the `MENU_*` enum (before
`NUM_MENUS`), add a label in `g_menu_labels[]`, and handle the new id in
`menu_item_count` / `menu_item_label` / `menu_item_shortcut` in
`ui_menu_bar.c`, plus `repl_action_menu_item_activate()` in
`repl_actions.c` for side effects.

To add a **pinned right-side button**: extend `PIN_*` enum, append a label
to `g_pin_btn_labels[]` in `ui_menu_bar.c`. Activation routing lives in
`imrepl_ctrl.c::route_pin_button_hit()` — add the new pin id to its
switch.

## User Scene System

The REPL keeps up to `MAX_USER_SCENES` (= 8) independent scenes in
`g_user_scenes[]` (in `repl_core.c`). Slot 0 is the pinned "home" scene — the
pre-example editor state captured on first example load, never auto-evicted.
Each `UserScene` stores `GLCmd` array + `num_cmds` + `edit_line` + predefined
variable values + a scene `name` + `last_touch` tick for LRU.

### Active slot and auto-promotion

- `g_active_user_scene` (`-1` means an example or a fresh empty workspace is
  loaded instead of a user scene).
- `repl_undo_push_snapshot()` (in `editor_undo.c`, called from
  `editor_input.c` and `editor_commit.c` before mutations) calls
  `repl_promote_example_if_needed()` before every mutation. If the user is
  editing an example, that call allocates a fresh slot, copies the current
  state into it, inherits the example's name (de-duplicated via
  `derive_unique_scene_name`), and sets `g_active_user_scene`. The editor
  keeps going — the user never sees the promotion directly, but subsequent
  edits now accumulate into a user scene.

### LRU eviction

When every non-home slot is full *and* a workspace directory is bound, a 9th
promotion picks the LRU non-pinned, non-active slot, flushes it to
`<workspace_dir>/<slug>.c` via `evict_scene_to_workspace()`, and reuses the
freed index. With no workspace bound the promotion is rejected with a status
message (user has to save workspace first to unlock eviction).

### Inline rename

- `repl_inline_rename_begin(slot)` / `repl_inline_rename_handle_key(...)` /
  `repl_inline_rename_cancel()` in `editor_inline_rename.c`.
- Triggered by the Scene → "Rename active scene" menu item; typing updates a
  status-bar prompt; Enter commits via `repl_user_scene_rename` (which trims,
  de-duplicates, and guards against an empty name), Esc cancels.
- Path-unsafe chars (`/`, `\`, `:`) and non-printables are filtered at input
  time since names become filesystem slugs on workspace export.
- The key dispatcher in `editor_input.c::keyboard_func` forwards keys
  to `editor_input_rename_capture_key` at the very top, so rename mode
  swallows input even when other overlays aren't open.

### Workspace I/O

- `repl_save_workspace(dir)` mkdirs `dir` (idempotent), flushes the active
  slot, then iterates every occupied slot: `install_scene_into_live` + a
  stash/restore pattern wraps each slot so `repl_export_save_output()` sees that scene's
  live state. The export scene-name hint is set per-slot so the exported
  header's `// @scene-name` reflects the correct name. The bound dir is
  remembered in import/export state and stamped into every single-file export
  as `// @workspace-dir <path>`.
- `repl_load_workspace(dir)` opens `dir`, loads each `*.c` into a fresh slot
  via `load_scene_file_into_slot`, and restores live editor state around the
  iteration. Names come from `@scene-name` headers (or the filename stem as
  fallback).
- Single-file save/load still works unchanged. Files round-trip between
  workspace and single-file modes because the header encodes both
  `@scene-name` and `@workspace-dir`.

### Scene menu layout

`SCENE_OFF_*` offsets in `ui_menu_bar.c` place fixed rows above the user-scene
list (`New empty scene`, `Save to output.c`, `Rename active scene`). User
scenes follow at `SCENE_OFF_SCENES`; rows are dense (unused slots skipped via
`repl_scene_menu_slot_for_dense_index`). The active scene row is drawn with
accent color.

### Public API touch points (in `repl_core.h`)

`repl_user_scene_count`, `repl_user_scene_slot_used`, `repl_user_scene_name`,
`repl_user_scene_rename`, `repl_load_user_scene_idx`, `repl_active_user_scene`,
`repl_save_workspace`, `repl_load_workspace`, `repl_workspace_dir`,
`repl_set_workspace_dir`. Back-compat helpers: `repl_user_scene_valid()` still
reports "any slot occupied?", `repl_load_user_scene()` still loads slot 0.

### F12 cycle

`examples → user scenes (in slot order) → back to first example`. Handles both
"active example" and "active scene" starting states.

## Example Metadata

Built-in examples in `repl_examples.c` can prefix their command list with:
1. Contiguous `// @cfg <slug> = <value>` lines.
2. An optional 5-line `// camera` preset block.

`repl_core.c` consumes leading metadata before feeding remaining lines through
the commit pipeline, so metadata stays hidden from the code panel. `@cfg`
parsing reuses `parse_workspace_header_line()` from `repl_export.c`, restricted
to these scene-presentation slugs:

`wireframe`, `grid`, `grid_major`, `grid_extent`, `axes`, `vertex_labels`,
`normal_vectors`, `vertex_outlines`, `vertex_points`, `vertex_guides`,
`light_indicators`, `backdrop`, `camera_rotate`.

Non-leading `@cfg` lines are not metadata — they stay as ordinary comments.

### Reset and restore rules

- Every example load resets the allowed non-camera scene-presentation settings
  to built-in defaults *before* applying the example's leading `@cfg`
  metadata. This prevents stale grid/axes/overlay/backdrop state from leaking
  across examples.
- Camera is intentionally excluded from that reset. Examples inherit the
  current `g_cam_*` state unless they supply the explicit leading `// camera`
  header.
- `restore_user_scene()` restores commands and predefined variables only.
  Leaving an example does not restore camera or other presentation state.

### Shared defaults

Keep the single source of truth for example-owned presentation defaults in
the `CFG_DEFAULT_*` macro block in `sample.h`. `repl_core.c` initializers,
example reset helpers, and focused example tests should reuse those macros
instead of duplicating literals.

When changing example-metadata behavior, inspect `repl_core.c`,
`repl_export.c`, `repl_examples.c`, `sample.h`, and
`test_repl_core_examples.c` together. `make test_repl_core_examples` is the
focused regression suite for this area; `make test` for broader REPL state.

### Open: desired vs. inherited @cfg

The "inherited (scene-set) is ephemeral" half of this is now in place via
the example sandbox in `repl_scenes.c`: entering an example from
non-example state captures the 14 presentation keys into
`g_pre_example_cfg[]`, and the next user-scene / home transition restores
them before applying the destination's saved `scene_cfg[]`. Today the
restore is observably overwritten by full `scene_cfg` coverage, but the
sandbox is now the canonical seam where any sparse-`scene_cfg` /
inherited-aware future change attaches.

Still open: a continuous "desired" mirror that survives toggles BETWEEN
example F12 cycles (Option B in the plan). Today, toggling cfg while
inside example A and then F12-cycling to example B drops A's toggles
on the floor — which matches the "ephemeral" intent for in-example
toggles. A future Option B would let users distinguish "I'm tweaking
A" from "this is my new preference" via a `repl_config_apply_inherited`
sibling to `repl_config_set`.

## Architecture

### Rendering Pipeline

`imrepl_ctrl_display_frame()` in `imrepl_ctrl.c` drives each frame.
`sample.c` registers the GLUT display callback and forwards directly — there
is no shim layer.
1. Rebuild autonormals and flat program if dirty; save predef var values;
   prepare replay frame if active; update export/camera strings
2. Build `SceneRenderConfig` from REPL state; if accumulation-buffer AA is
   enabled (currently read from `repl_state_render()`; R1a moves this to
   config fields), call `scene_render_3d_scene(&cfg)` once per jitter sample.
   Jitter is applied as a scene-local frustum shift inside the scene function —
   it is no longer a config field
3. `scene_render_3d_scene(&cfg)` in `scene_render.c`: viewport/clear setup →
   projection → camera → execute user geometry via `SceneExecuteProgramFn`
   callback → replay fade batches (transitional; R1b replaces with
   `ReplayFadePlan` snapshot iteration) → grid/axes/backdrop/orbit-target →
   polygon-outline, vertex, normal, and guide overlays → 2D replay HUD
   (renders via `replay_ui_hud_render` from `replay_ui_hud.c` —
   feature-UI under the `replay_ui_*` prefix)
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
   `keyboard_func()` in `editor_input.c`. There are TWO distinct paths:
   - **Interactive `;` key** (`editor_input.c`, `key == ';'` block):
     the input buffer does NOT include the `;` — the keystroke triggers the
     commit but is not appended. Commit handlers must accept input
     without a trailing `;`.
   - **`feed_line()`** (`editor_input.c`): copies the full line
     (including `;`) into the input buffer, then runs the same dispatch chain.
     Used by file loading and example loading.
   - **Enter key** (insert mode): input may or may not have `;`
     depending on what the user typed.
   The dispatch chain calls the consolidated `try_commit_*()` helpers
   in `editor_commit.c` (`try_commit_var_statements`,
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
   sets the per-context error buffer (no `set_status` from the parser
   core — the bridge was retired in Phase J5).
3. **Parse** — `parse_command()` in `repl_parser.c` matches the line to a
   `CmdType`, evaluates argument expressions via `eval_expr()`, stores
   result in `GLCmd.args[]` and normalized text in `GLCmd.source[]`.
   Internal call sites pass `ReplParseContext.source_line_idx` instead of
   temporarily changing the edit-line cursor.
4. **Flatten** — `flatten_range()` recursively expands the source array:
   for-loops iterate (capped at 100k visits), function calls inline the
   body with actual args, if-blocks evaluate conditions. Recursion
   depth limited to `MAX_FLATTEN_CALL_DEPTH=32`
5. **Execute** — `repl_execute_program()` walks the flat command array emitting GL
   calls. Re-evaluates expressions with `has_vars` flag each frame
   (for animated `t`, etc.)

### Commit Dispatch Sites

The `try_commit_*` handler chain is consolidated into four helpers in
`editor_commit.c`:
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

`try_commit_float_decl()` in `editor_commit.c` handles `float name;`
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

Circular snapshot buffers in `editor_undo.c`:
- `ReplUndoSnapshot` captures the full editor state: source commands,
  command count, cursor position, predefined variable values
- Undo and redo rings (32 slots each) with head/count tracking
- `repl_undo_push_snapshot()` called before any mutation (delete, paste,
  reformat, etc.); `repl_undo_pop_snapshot()` on Ctrl+Z; `repl_undo_do_redo()` on
  Ctrl+Y. Also the hook where `repl_promote_example_if_needed()` fires
  so editing an example auto-creates a user scene.
- Pushing clears the redo stack; undo moves current state to redo

### Autocomplete

Symbol matching and function parameter hints in `editor_autocomplete.c`:
- `repl_state_autocomplete()->matches` — matched completions from GL command/constant tables
- `repl_state_autocomplete()->ghost` — suffix to append to input on Tab accept
- `repl_state_autocomplete()->hint` — parameter list hint shown below cursor
- Modes: `AC_MODE_FUNC_PREFIX` (after `foo(` → param hints),
  `AC_MODE_ENUM_ARG1/2` (GL constant completion),
  `AC_MODE_POINT_PARAM` (3D point coordinates)

### Search

Case-insensitive text search in `editor_search.c`:
- Activated by Ctrl+F; query and state accessed via `repl_state_search()`
- `repl_search_find_next_in_text()` finds substring matches across
  all visible lines (header, user code, footer)
- `hit_line_idx`/`hit_char_idx` in `ReplSearchState` track current match position
- Integrated with code panel rendering for match highlighting

### Config Menu

Declarative toggle system in `repl_actions.c`:
- `g_cfg_items[]` array of `ReplConfigItem` descriptors: `{ label, key_code,
  is_special, key, state_count, state_names[], section_header }`
- Each item is a toggle (2 states, default OFF/ON) or cycle (>2 states
  with named entries, e.g. grid themes)
- Rendered by the Config dropdown in `ui_menu_bar.c`; menu clicks and
  F-key/Ctrl-key shortcuts dispatch through `repl_actions.c`
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

Functions: `sin`, `cos`, `tan`, `sqrt`, `abs`, `pow`, `min`, `max`, `floor`, `ceil`, `fmod`, `rand(seed[, iter])`
Constants: `PI`, `TAU`
Variables: declared via `float name;` — only `t` is predefined (Ctrl+T toggles animation).
Scratch arrays: `A[8]`, `B[8]`, `C[8]` are fixed global runtime arrays for recursive/loop algorithms.
Reads and writes use normal expression syntax; indices are truncated with `(int)` and must stay in `0..7`.
Other names (`x`, `y`, `z`, etc.) must be declared before use.
`MAX_PREDEF_VARS` = 16 (1 reserved for `t`, 15 user-declarable slots).

Example:

```c
A[0] = 0;
A[1] = 1;
A[0] = A[0] + (A[1] - A[0]) * 0.25;
glVertex3f(A[0], 0, 0);
```
