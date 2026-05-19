# OpenGL Immediate-Mode REPL

Interactive OpenGL command interpreter. Type GL commands, press `;` to execute,
and watch geometry render in real-time with a live code panel.

New to the tree? Start with [`MODULES.md`](MODULES.md) for the one-page
layered overview of the source files. This file is the agent-facing project
brief and goes deeper.

## GNU Sed

GNU sed is available as `gsed` on macOS via Homebrew (`brew install gnu-sed`).

## Linux / real-gcc verification (gracemont)

Environment-specific to this dev setup (local: `drew` on macOS, the
repo at `~/src/code/openGL/samples/gen-ai/gl-repl`; the macOS
toolchain's `gcc` is Apple clang). The project targets old-gcc / Linux
portability (`-std=c99`, the `make check-c99` ratchet), so changes that
touch the build, sanitizer flags, or anything portability-sensitive
should be cross-checked under real GCC on Ubuntu.

- Host: `ssh gracemont` — Ubuntu 24.04, real `gcc` (13.x), GNU Make 4.x.
- Repo path there: `~/code/openGL/samples/gen-ai/gl-repl` — i.e. the
  same path **without the `src/` segment** the macOS checkout has.
- Sync + verify:

  ```bash
  ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && \
    git pull --ff-only origin main && \
    make check-c99 && make test-stubs'
  ```

  `check-c99` is the real-gcc C99 ratchet; `test-stubs` builds and runs
  the suite with the bundled GL stubs, so it needs **no GL dev libs**
  on the headless box (and picks up the debug-default ASan+UBSan). Use
  `make test` there only if GL/GLUT dev packages are installed.

## Build

```bash
make sample          # Build main binary (freeglut)
make glut            # Build with system GLUT (macOS framework)
make test            # Build and run all tests (debug: ASan + UBSan)
make check-c99       # C99 ratchet (sample + demos + bench)
make clean           # Remove binaries
```

Requires: gcc with C99 support, OpenGL, GLUT/freeglut.

The test targets (`test`, `test-detailed`, `test-stubs`, `test-full`)
default to `BUILD=debug`, which compiles with **AddressSanitizer +
UndefinedBehaviorSanitizer** (UB aborts: `-fno-sanitize-recover`).
`make sample`/`bench`/the demos stay `BUILD=release`. An explicit
`BUILD=...` on the command line or in the environment always wins, so
`make coverage` (BUILD=coverage) and `make test BUILD=release` (a fast
unsanitized run) keep working.

### C99 standard

**The whole project compiles `-std=c99`, project-wide, no exceptions**
(sample, tests, demos, bench, CI) so it runs on old machines / old
GCC. It is *non-pedantic* by default — GNU extensions GCC accepts in
`-std=c99` are fine; the goal is "old gcc compiles it", not pure ISO
C99. There is no C2x build and no `STD` knob.

**`make check-c99` is a build guard** (also run inside
`make check-state-ownership`, so it's in the standard gate): it
syntax-checks the *shipped/real* sources — the sample object set
(`$(SRCS)`) plus the demo drivers (`tools/`) and bench harness
(`bench/`) — under `gcc -std=c99 -fsyntax-only`, **non-pedantic**.
Our code dirs are `-I`; the real GL/GLU/GLUT/freeglut headers are
`-isystem` so a vendored header's own old-style decl (e.g.
`freeglut_ext.h`) can't fail it. It still has teeth: C99 makes
implicit function declarations a hard error even non-pedantic, and
unknown symbols fail. Tests are excluded (not shipped); they build
under plain `-std=c99` too.

Why non-pedantic: a real-GCC check found the *only*
`-std=c99 -pedantic-errors` failures across the whole shipped set
were 22 hits of one benign rule — C99 forbids the implicit
pointer-to-array `const`-qualifier conversion (`T (*)[N]` →
`const T (*)[N]`) that **C2x explicitly re-allows**. The code is
correct under C99/C2x/Clang; clearing it would mean de-`const`-ing
~7 files of legitimate const-correctness (and there is no granular
`-Wno-` for it — it's pure `-Wpedantic`). Not worth it; the guard
stays non-pedantic.

Coding conventions for genuinely-old-GCC portability (not all
machine-enforced by the non-pedantic guard, but follow them):

- Compile-time asserts: `STATIC_ASSERT(expr, msg)` from
  `include/c_compat.h` — **never raw `_Static_assert`** (early-2000s
  GCC predates it; the shim falls back to a negative-array typedef
  under C99).
- Keep every TU non-empty even when its body is `#ifdef`-gated off
  (a token/`typedef` outside the guard).
- Prototyped function-pointer typedefs (e.g. `ReplGluCallback` in
  `src/repl/executor.c`), not old-style `void (*)()`.
- Plain `, __VA_ARGS__` (no GNU `, ##__VA_ARGS__`); one `typedef`
  per type name across headers.

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
./sample --no-audio       # Skip audio init entirely (isolates startup stalls)
GLR_NO_POINT_PARAMETER=1 ./sample   # Force the no-glPointParameterfv path
GLR_AUDIO_HITCH_MS=10 ./sample      # Lower the audio-worker hitch threshold
```

### Startup & audio-worker diagnostics

Two always-on stderr diagnostics help locate startup stalls and
audio-thread hitches (the kind seen on slow Linux disks):

- **Init trace.** `main()` in `sample.c` logs a wall-clock line per
  startup phase (`[init +N.NNNs] <phase>`: window create, GL init,
  REPL bootstrap, audio_init, playlist start, main loop) via
  `gettimeofday`. A large gap names the slow phase; `--no-audio`
  isolates whether `ma_engine_init()` (the one synchronous audio call
  on the `main()` path — it opens the OS audio device) is the cause.
- **Worker hitch detector.** The audio worker thread
  (`audio_worker_main` in `src/app/glr_audio.c`) wakes from `pthread_cond_wait`,
  runs exactly one blocking lifecycle op, then sleeps again. The
  dispatch span is timed with `clock_gettime(CLOCK_MONOTONIC)` (after
  the mutex is released, so only the blocking work counts); any op
  over the threshold logs `repl_audio: worker hitch: <op>[+save] took
  N ms`. `<op>` is `load` (`ma_sound_init_from_file`), `uninit`
  (`ma_sound_uninit` stream page-flush), `advance`, or `save-only`.
  Threshold via `GLR_AUDIO_HITCH_MS` (default 50; `0` disables; read
  once and cached). `AWR_QUIT` (shutdown) is intentionally not timed.
  These stalls delay track changes / resume, not the miniaudio device
  callback (a thread the REPL does not own).

### `GLR_NO_POINT_PARAMETER`

Runtime env var (any non-empty value). `glr_ctrl_init_gl()` auto-detects
`glPointParameterfv` support from the live GL context
(`GL_VERSION >= 1.4 || GL_ARB/EXT_point_parameters`); this var forces the
unsupported path on capable hardware so the fallback stays testable.
There is **no build flag** — it replaced the old compile-time
`NO_POINT_PARAMETER` macro. When point attenuation is off the binary
logs one stderr line distinguishing the env override from a GL context
that genuinely lacks the entry point. Unsupported → `CMD_POINT_PARAMETER_FV`
is a silent no-op (executor falls back to a camera-distance `glPointSize`
approximation), the injected `point_attenuation` init bootstrap entry is
skipped in apply *and* export, and the star backdrop's direct call is
gated via `SceneRenderConfig.point_parameter_supported`. User-typed
`glPointParameterfv(...)` is still kept verbatim in exported standalone C
(it may target other hardware). See *Runtime GL Capability Detection* in
`ARCHITECTURE.md`.

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
| `sample.h` | Minimal legacy header: standard includes and `M_PI`; types/defaults moved out to dedicated headers |
| `src/app/glr_ctrl.c` | App-frame controller: `glr_ctrl_display_frame`, `glr_ctrl_reshape`, `glr_ctrl_init_gl`; builds `SceneRenderConfig`, calls scene/UI renderers |
| `src/app/glr_ctrl.h` | Controller public surface: display, reshape, init-GL entrypoints |
| `src/app/glr_config.c` | Config key implementation and descriptor table helpers |
| `src/app/glr_config.h` | `ReplConfigKey` / `ReplConfigItem` descriptor API for keyed config access |
| `src/repl/command.h` | Core command model types: `CmdType` enum, `GLCmd` struct (pure parse-result: type, args, flags, provenance — no `source[]` field) |
| `src/repl/compile.c` | Pure source-text validators that produce `ReplCompiledChange` descriptors; never mutates state |
| `src/repl/compile.h` | `ReplCompiledChange`, `ReplCompileResult`, `ReplCompileContext`, compile entry points |
| `src/repl/apply.c` | Applies a `ReplCompiledChange` to `ReplState` command arrays |
| `src/repl/apply.h` | Apply public API (`repl_apply_compiled_change`, `repl_apply_predef_ops`) |
| `src/repl/core.c` | Normalization pipeline (`repl_parse_and_normalize*`), reformatter, startup helpers |
| `src/repl/parser.c` | REPL source-line parser, expression validation, canonical line text emission via `ReplParsedLine.text` (the per-line text lives in `EditorState`'s editor buffer, not on `GLCmd`) |
| `src/repl/parser.h` | Parser entrypoints (`repl_parser_parse_command*`, `repl_parser_parse_command_ctx`), `ReplParseContext`, `ReplParsedLine` |
| `src/repl/source_scope.c` | Source prefix-depth cache, indentation helpers, block lookup |
| `src/repl/source_scope.h` | Source-scope query API (`repl_source_scope_block_depth_at`, `repl_source_scope_find_block_end`, indent helpers) |
| `src/repl/command_spec.c` | Command type metadata and specifications (parsing, formatting, completion requirements) |
| `src/repl/command_spec.h` | Command spec query API |
| `src/repl/command_store.c` | Low-level `GLCmd` array mechanics: insert, delete, replace, bulk-load (no text-buffer writes) |
| `src/repl/command_store.h` | Command-store public API (`repl_command_store_insert_one`, etc.) |
| `src/repl/core.h` | Public API (parse, flatten, user scene + workspace); GLUT input-dispatch declarations |
| `src/repl/core_internal.h` | Test-visible internals (normalize/commit pipeline, `editor_feed_line`, `editor_load_line_to_input`, `repl_promote_example_if_needed`) |
| `src/repl/state.c` | Owns `g_repl_state`, lifecycle, snapshot assembly (`repl_state_capture` / `repl_state_restore`) |
| `src/repl/state.h` | Typed runtime-state facade, reset helpers, and focused accessors over the live REPL state |
| `src/repl/state_views.h` | Read-only (by-value) state getters; safe to include from `scene_*` and `ui_*` |
| `src/repl/state_owners.h` | Mutable `_mut()` accessors; owner modules and controller only |
| `src/editor/input.c` | Editor's text-document controller: keyboard/mouse dispatch, cursor/scroll/selection/search/autocomplete navigation, clipboard, undo, commit orchestration, `editor_feed_line`. Non-editor routing (replay, audio, config, save, camera) lives in `src/app/glr_ctrl.c` |
| `src/editor/input.h` | Editor input dispatch entry points + `EditorInputDispatchEffects` typedef + `editor_input_active_modifiers` test seam |
| `src/editor/commit.c` | Editor-side commit transaction boundary: compile via `repl_compile`, undo snapshot, text-buffer write, REPL apply, dirty-state updates |
| `src/editor/commit.h` | Commit orchestration API (`editor_commit_apply_external_change`, `editor_try_commit_*` helpers) |
| `src/editor/state.c` | Owns `EditorState`: editor buffer, cursor, selection, search, autocomplete, scroll, undo/redo, transformers, highlights, virtual lines |
| `src/editor/state.h` | `EditorState` typed facade, `EditorBufferView`, `editor_state_input/search/autocomplete` accessors |
| `src/editor/services.c` | Default `EditorServices` bound to live REPL (compile/apply seam used by commit code) |
| `src/editor/services.h` | `EditorServices` dispatch table for REPL semantics |
| `src/editor/limits.h` | Shared editor input and autocomplete capacity constants |
| `keys.h` | ASCII and control-key code constants (Ctrl+A=1 … Ctrl+Z=26, F-key names) |
| `src/editor/clipboard.c` | Line selection anchors, command clipboard buffer, copy/cut/paste behavior |
| `src/editor/clipboard.h` | Clipboard public API |
| `src/editor/undo.c` | Undo/redo snapshots, history rings, example auto-promote hook before mutation |
| `src/editor/undo.h` | Undo public API (`editor_undo_push_snapshot`, `editor_undo_pop_snapshot`, `editor_undo_do_redo`) |
| `src/app/glr_camera.c` | Scene camera pointer state, orbit/pan/zoom drags, wheel zoom velocity, momentum tick |
| `src/app/glr_camera.h` | Camera state + setters (`glr_camera`, `glr_camera_set_*`, `glr_camera_controls_reset`) |
| `src/app/glr_actions.c` | Config descriptor table, config shortcuts, menu actions |
| `src/app/glr_actions.h` | Actions public API (`glr_action_menu_item_activate`, etc.) |
| `config.h` | Project-wide compile-time configuration constants |
| `src/app/glr_defaults.h` | Controller-side scene/presentation defaults (`CFG_DEFAULT_*` macros) |
| `src/ui/text_layout.c` | Pure code-panel wrapping, row counts, segment lookup, cursor-row mapping |
| `src/ui/text_layout.h` | `CodeLayout` / `CodeWrapIter` API shared by UI, export dumps, tests |
| `src/ui/repl_code_panel.c` | REPL-specific code-panel adapter: row building, scroll-follow layout, render/hit bridging |
| `src/ui/repl_code_panel.h` | `UiReplCodePanelLayout` plus REPL adapter render/hit/layout entrypoints |
| `src/repl/executor.c` | Narrow live-GL dispatch: walks the flat command array emitting OpenGL calls |
| `src/repl/executor.h` | Executor public API (`repl_execute_program`, transform helpers) |
| `src/repl/flatten.c` | Source-to-flat program builder: unrolls loops, inlines functions, resolves if-blocks |
| `src/repl/flatten.h` | Flatten public API (`repl_flatten_program`, cursor-highlight refresh) |
| `src/repl/pipeline.h` | Pipeline and lifecycle surface for frame orchestration (flatten, autonormal, replay snapshots) |
| `src/repl/autonormal.c` | Auto-generated `glNormal3f` maintenance for source commands |
| `src/widgets/replay.c` | Replay state machine: PC, mode (OFF/PLAYING/PAUSED/DONE), speed, fade-batch ring |
| `src/widgets/replay.h` | Replay public API (`replay_start`, `replay_toggle_play_pause`, etc.) |
| `src/editor/search.c` | Case-insensitive substring search state and match navigation |
| `src/editor/search.h` | Search query helpers and input routing API |
| `src/app/glr_completion.c` | REPL-side completion provider: walks command spec / predef vars / `CMD_FUNC_DEF` for matches, ghost text, parameter hints. Registered via `EditorCompletionProvider`. |
| `src/ui/layout.c` | Pure window layout geometry: scene rect and code-panel rect derivation |
| `src/ui/layout.h` | Layout geometry API (`ui_layout_scene_rect`, `ui_layout_code_panel_rect`) |
| `src/repl/scenes.c` | User-scene slots, LRU eviction, workspace save/load, workspace dir binding |
| `src/repl/example_loader.c` | Built-in example loading and active-example tracking |
| `src/app/glr_debug.c` | Diagnostic dumps for CLI flags and tests |
| `src/app/glr_debug.h` | Debug dump public API |
| `src/repl/replay_annotations.c` | Replay-time source annotations, variable substitution, evaluated command display text |
| `src/repl/replay_annotations.h` | Code-panel replay annotation API |
| `src/ui/snapshot.h` | `UiRenderSnapshot` — frame-frozen bundle built once per frame by `glr_ctrl_build_ui_snapshot()` |
| `src/ui/editor.h` | Per-frame editor-overlay snapshots (swatches, sliders, highlights) pushed by the controller |
| `src/ui/replay_hud.c` | 2D replay status HUD (feature-UI under the `replay_ui_*` prefix; reads replay peer snapshot) |
| `src/ui/replay_hud.h` | Replay HUD render entrypoint |
| `src/ui/profile_panel.c` | CPU profiling overlay panel (per-frame section timings) |
| `src/ui/profile_panel.h` | Profile panel render entrypoint |
| `src/ui/menu_bar.c` | Code-panel menu bar, dropdowns, config right-click handling, search slot |
| `src/ui/menu_bar.h` | Menu/pin hit-test and dropdown state API |
| `src/ui/scene_tabs.c` | Scene tab strip below the menu bar: snapshot-pure render + whole-band hit-test (TAB / inert CHROME); derived each frame, no persistent model |
| `src/ui/scene_tabs.h` | Scene tab strip render/hit/`band_h` API |
| `src/widgets/color_picker_state.c` | Floating color picker peer: state, lifecycle, slider input handlers, source-line writeback through editor commit |
| `src/widgets/color_picker_state.h` | Peer API (`ColorPickerView`, `ColorPickerInputResult`, `color_picker_open/close/handle_*`, `color_picker_hsv_to_rgb`) |
| `src/ui/color_picker.c` | Floating color picker renderer + hit-test (pure, takes `ColorPickerView *`) |
| `src/ui/color_picker.h` | Picker UI render/hit-test API + `UI_COLOR_SWATCH_W` |
| `src/ui/tabbed_overlay.c` | Generic modal tabbed text overlay renderer (the F1 help overlay's UI shell) |
| `src/ui/tabbed_overlay.h` | Tabbed-overlay render API (`UiOverlayState`, `UiOverlayContent`) |
| `src/repl/help_text.c` | Builds neutral F1 help text tables (commands, key bindings); `glr_ctrl` adapts them to `UiOverlayContent` |
| `src/repl/help_text.h` | Help-content public API |
| `src/ui/variable_panel.c` | Floating variable slider panel rendering, geometry, and hit-test |
| `src/ui/variable_panel.h` | Variable panel render/rect/hit API |
| `src/ui/autocomplete_panel.c` | Floating autocomplete popup renderer (reads autocomplete state populated by `src/app/glr_completion.c`) |
| `src/ui/autocomplete_panel.h` | Autocomplete popup render entrypoint |
| `src/editor/inline_rename.c` | Inline scene-rename input buffer and key handling (status-bar overlay) |
| `src/editor/inline_rename.h` | Rename begin/active/cancel/key/special API |
| `src/widgets/variable_panel_drag.c` | Variable slider drag transaction: begin/motion/reset, linear/log value writeback |
| `src/widgets/variable_panel_drag.h` | Drag state accessors + begin/motion/reset API |
| `src/widgets/variable_panel_state.c` | Variable-panel peer subsystem: owns visibility flag + drag-state storage |
| `src/widgets/variable_panel_state.h` | Peer-subsystem facade (`VariablePanelState`, capture/restore/reset, view/drag accessors) |
| `src/widgets/replay_state.c` | Replay peer subsystem: owns `ReplReplayRuntimeState` storage |
| `src/widgets/replay_state.h` | Peer-subsystem facade (`replay_state_capture/restore/reset/view/mut`) |
| `src/editor/help_session.c` | Read-only editor session for the help overlay (tab_idx + scroll) |
| `src/editor/help_session.h` | `EditorHelpSession` API (capture/restore/reset, narrow accessors) |
| `src/editor/completion.c` | Completion-provider registry: editor input invokes registered provider for autocomplete |
| `src/editor/completion.h` | `EditorCompletionProvider` struct + `editor_completion_register/update/clear` API |
| `src/repl/examples.c` | Predefined example data (`g_examples[]`, `g_example_names[]`) |
| `src/repl/examples.h` | Example query API (`repl_examples_count/name/lines`) |
| `src/repl/tutorials.c` | Built-in tutorial catalog: per-tutorial null-terminated `comments[]` / `expected[]` parallel arrays + name. Starter set: "First Triangle", "Color & Transform" |
| `src/repl/tutorials.h` | Catalog query API (`repl_tutorial_count/name/step_count/step_comment/step_expected`) + `TutorialEntry` typedef |
| `src/widgets/tutorial_state.c` | Tutorial peer subsystem: owns `TutorialRuntimeState` (active flag, step, locked_lines, fade timing, last match result) |
| `src/widgets/tutorial_state.h` | Peer-subsystem facade (`tutorial_state_view/_mut/_reset`, `tutorial_active`), `TutorialMatchKind/Result` types |
| `src/widgets/tutorial.c` | Tutorial runner: starts/exits/advances, emits instruction comments via `repl_load_apply_line`, whitespace-tolerant match, locked-line guard, fade-alpha math |
| `src/widgets/tutorial.h` | Runner API (`tutorial_start/_exit/_handle_commit_attempt/_advance_after_successful_commit/_current_expected_text/_line_is_locked/_line_is_fading/_step_fade_alpha/_guard_source_change/_match`) |
| `src/repl/export.c` | `repl_export_save_output` / `repl_export_load_from_file`, workspace header directives, `@scene-name` / `@workspace-dir` markers |
| `src/repl/export.h` | Export/import public API and workspace-header pending-state types |
| `src/repl/export_state.h` | Shared dimensions for import/export state text |
| `src/app/glr_audio.c` | App-level playlist engine and persisted audio config |
| `src/app/glr_audio.h` | Audio playback API (`glr_audio_*`) |
| `prof.c` | CPU wall-time profiling instrumentation (per-section accumulators, frame tick) |
| `prof.h` | Profiling API (`prof_begin`, `prof_end`, `prof_frame_tick`, etc.); no UI dependency |
| `src/scene/render_types.h` | Shared `SceneRgba` / `SceneRenderConfig` / `FrameRenderContext` types for scene helpers |
| `src/scene/guides/guides_shared.h` | Shared guide snapshot and planning types for REPL-aware 3D overlay passes |
| `src/scene/guides/geometry_guides.c` | Vertex/primitive guide rendering (input context at cursor) from `SceneGuideSnapshot` |
| `src/scene/guides/geometry_guides.h` | Geometry guides render entrypoint |
| `src/scene/guides/transform_guides.c` | Transform guide rendering (pending matrix ops during replay) |
| `src/scene/guides/transform_guides.h` | Transform guides render entrypoint |
| `transform_utils.h` | Header-only GL matrix helpers (`apply_tracked_transform`, `unwind_transform_stack`) mirroring executor transforms without requiring `src/repl/executor.h` |
| `src/scene/render.c` | 3D scene frame orchestration, one-shot init, scene config/frame prep, edit guides, orbit target, replay fade pass orchestration |
| `src/scene/grid.c` | Grid theme rendering and custom focus/ocean/ruler/planes passes |
| `src/scene/grid.h` | Grid render entrypoint |
| `src/scene/axes.c` | Axes theme rendering |
| `src/scene/axes.h` | Axes render entrypoint |
| `src/scene/scene_transition.c` | Pure grid/axes show↔hide fade state machine (`scene_xn_init/set/show/tick`); no GL, one instance per overlay |
| `src/scene/scene_transition.h` | Transition machine API: `SceneXnState`, `SceneXnPhase`, entry points |
| `src/scene/render.h` | Declares `scene_render_3d_scene(const SceneRenderConfig *)` and `scene_apply_camera(...)` |
| `src/scene/backdrop.c` | Backdrop mode dispatch and deterministic cityscape renderer |
| `src/scene/backdrop.h` | Backdrop render entrypoint |
| `src/scene/lights.c` | Ambient init, light setup/reset, and visible light indicator overlay |
| `src/scene/lights.h` | Scene light setup/render entrypoints |
| `src/scene/overlays.c` | Tiny per-vertex GL primitives the controller calls (vertex-number labels, normal arrows). Outline / vertex-point passes moved to `src/app/glr_ctrl.c` |
| `src/scene/overlays.h` | Scene overlay primitive API |
| `src/ui/state.c` | Owns `UiState`: viewport, pointer, status text TTL, panel visibility, panel-divider geometry |
| `src/ui/hit.h` | `UiHitKind` + `UiHit` — neutral hit-test result returned by UI input handlers to `glr_ctrl` |
| `src/ui/panels.c` | Code-panel row rendering (incl. inline ghost/hint text), scene status banner, hit-test (returns `UiHit`) |
| `src/ui/panels.h` | Code-panel geometry, render, and hit-test declarations |
| `src/repl/eval.c` | Expression evaluator (recursive descent), REPL<->C translators, for-loop parsers |
| `src/repl/eval.h` | Evaluator types (`ExprVar`, `ExprCtx`), function declarations |
| `src/repl/format.c` | Pure indentation/depth computation (no GL dependency) |
| `src/repl/format.h` | Formatting types (`ReplFmtCmd`, `ReplFmtType`), `repl_format_*` indent functions |
| `include/gl_2d.h` | Header-only 2D OpenGL helper functions |
| `tests/support/` | Shared test harness/setup helpers |
| `tests/gl-stubs/` | No-op GL/GLU/GLUT headers used by `USE_GL_STUBS=1` builds |
| `MODULES.md` | One-page layered overview, ownership diagram, current boundaries, open edges |

## Conventions

- File-private statics use `g_` prefix (e.g., `g_cfg_items[]`, `g_user_scenes[]`).
  Runtime state that crosses module boundaries is accessed through typed
  facades: `src/repl/state.h` for REPL program state (e.g., `repl_state_render()`,
  `repl_state_variables()`), `src/editor/state.h` for editor session state
  (e.g., `editor_state_input()`, `editor_state_search()`), and peer-subsystem
  accessors for replay (`replay_state_view()`) and variable panel
  (`variable_panel_state_view()`).
- Static helpers are file-scoped; public API goes through module headers
- Prefixes express ownership. Use `repl_*` for REPL language/source/program
  model modules, `editor_*` for text-document model/controller (under
  `src/editor/`), `glr_*` for app shell/controller/app-service code,
  `scene_*` for 3D rendering, `ui_*` for 2D view rendering, and neutral
  names such as `prof` for generic utilities. The app-level audio
  service lives at `src/app/glr_audio.c` with the `glr_audio_*` API
  (resolved from the former neutral `audio.c`). Don't introduce new
  top-level prefixes without a plan.
- Config toggles use the `ReplConfigItem` / `ReplConfigKey` pattern: add a
  descriptor entry to `g_cfg_items[]` in `src/app/glr_actions.c`; `CFG_ITEM_COUNT`
  auto-computes via `sizeof`
- New GL commands: add to the `CmdType` enum in `src/repl/command.h`, then
  handle in `repl_parser_parse_command_ctx()` in `src/repl/parser.c`,
  `repl_execute_program()` in `src/repl/executor.c`, and `flatten_range()`
  (static, inside `src/repl/flatten.c`). Add a `g_command_type_specs[]`
  entry in `src/repl/command_spec.c` with the right `CmdSyntaxCategory`
  so the new command picks up its code-panel highlight color
  automatically; if you need a `glEnable`-shaped enum-arg spec or a
  standard float-arg spec, append a row to `k_enum_command_specs[]`
  or `k_std_command_specs[]` in the same file.
- Enum-backed commands use **one uniform storage convention**: every
  parsed enum argument lives in `GLCmd.args[]` (with `num_args` set),
  N args wide, resolved by the generalized loop in `parser.c` from
  each row's positional `ReplEnumArgSpec args[]`. There is **no
  `GLCmd.mode` field** — it was deleted; the absence is the
  compiler-enforced "enum args go through `args[]`" invariant (so no
  `check-state-ownership` guard is needed for it). Each slot declares a
  `ReplEnumSlotKind`: `ENUM_ONLY` (strict token; the behavior-neutral
  default for non-bool slots), `ENUM_OR_CONST_VALUE` (token or a
  constant 0/1 reverse-mapped — the bool-mask policy for `glDepthMask`
  / `glColorMask`), or `ENUM_OR_EXPR` (token or a full expression —
  only `glLightModeli` slot 1). `k_enum_command_specs[]` /
  `k_std_command_specs[]` stay alphabetically sorted by GL name.
- `CmdType` set tests go through the inline predicates in
  `src/repl/command.h`, not ad-hoc `||` chains: `repl_cmd_is_transform`,
  `repl_cmd_emits_vertex` (VERTEX3F/VERTEX2F/TESS_VERTEX),
  `repl_cmd_is_block_head` (FOR_BEGIN/FUNC_DEF/IF_BEGIN),
  `repl_cmd_is_block_end` (FOR_END/FUNC_END/IF_END). These are the
  *control-flow* taxonomy; `CmdSyntaxCategory` in `command_spec.h` is
  the separate *visual* (syntax-highlight) taxonomy — don't fold one
  through the other (it would invert the header layering). A drift
  test in `tests/test_replay_walk.c` asserts predicate↔category
  agreement for the pairs that have a category twin. When a subset is
  *intentionally* narrower (e.g. autonormal's gl-vertex-vs-tess split),
  spell it out inline with a comment rather than adding a predicate.
- Splitting a function call's comma-separated args: use
  `repl_scan_next_arg_delim()` from `src/repl/eval.h`, never a bare
  `strchr(s, ',')` or `*s != ',' && *s != ')'` loop — those are
  paren-naive and stop at the first inner `)` of e.g.
  `cos(i + phase)`, silently truncating the slot.
- Keyboard bindings: `editor_handle_key()` for ASCII keys (Ctrl+X
  produces ASCII X & 0x1F via standard GLUT), `editor_handle_special()`
  for F-keys/arrows. Cross-subsystem routing (replay / save / config /
  audio / camera) lives in `src/app/glr_ctrl.c::glr_ctrl_router_*`
  helpers, called from `glr_ctrl_keyboard` before delegating to
  `editor_handle_key`. macOS Cmd+letter is normalized to its
  control-character form by `editor_input_normalize_super_to_ctrl`,
  called at the top of `glr_ctrl_keyboard` so every downstream
  dispatcher sees Cmd+B identically to Ctrl+B.
- Expression variables: `ExprVar` struct in `src/repl/eval.h`, predefined set
  accessible via `repl_state_variables()` and managed by `repl_eval_declare_predef_var()`

## Architecture

### Rendering Pipeline

`glr_ctrl_display_frame()` in `src/app/glr_ctrl.c` drives each frame.
`sample.c` registers the GLUT display callback and forwards directly — there
is no shim layer.
1. Rebuild autonormals and flat program if dirty; save predef var values;
   prepare replay frame if active; update export/camera strings
2. Build `SceneRenderConfig` from REPL state and call `scene_apply_camera(...)` then
   `scene_render_3d_scene(&cfg)` once per jitter sample (if accumulation-buffer AA is enabled).
   The camera modelview transform is the controller's responsibility —
   `src/scene/render.c` does not touch the modelview except for sub-renderer
   push/pop bracketing. Jitter is applied as a scene-local frustum shift
   inside the scene function.
3. `scene_render_3d_scene(&cfg)` in `src/scene/render.c`: viewport/clear setup
   → projection → execute user geometry via `SceneExecuteProgramFn`
   callback → replay fade batches → grid/axes/backdrop/orbit-target →
   polygon-outline, vertex, normal, and guide overlays → 2D replay HUD
   (renders via `replay_ui_hud_render` from `src/ui/replay_hud.c`)
4. 2D overlays: code panel, autocomplete popup, example dropdown,
   variable slider panel, config menu, help overlay, search overlay

The standalone `make scene_demo` binary (sources in `tools/scene_demo/`)
exercises the scene contract with a non-REPL geometry callback — it builds
without dragging in the REPL editor / controller, which is the load-bearing
proof that `src/scene/` has no hard dependency on REPL code.

### Two-Level Command Model

The core data flow is **source commands → flat commands → GL calls**:

- **Source array** (`repl_state_document_cmds()`, count via
  `repl_state_document_count()`) — each `GLCmd` holds parsed type/args
  and flags (`has_vars`, `valid`, `is_auto`). Per-line canonical text is
  *not* on `GLCmd`; it lives in `EditorState`'s editor buffer (accessed via
  `editor_buffer_view_line()`) and is the editor's writable model.
- **Flat array** (`repl_state_flat_cmds()`) — expanded copy. For-loops are
  unrolled, function calls are inlined, if-blocks are resolved.
  Each flat cmd records `src_cmd_idx` (owning source line),
  `call_src_cmd_idx` (immediate call site), and `func_scope_mask`
  (active function scopes) for cursor highlighting.
- **Trigger:** any edit marks the flat array dirty (via `mark_normals_dirty()`);
  `flatten_commands()` rebuilds it on the next frame before rendering.

### Command Lifecycle

1. **Input** — user types into the input buffer (`editor_state_input().input`,
   max 1024 chars)
2. **Commit** — pressing `;` calls the commit dispatch chain in
   `editor_handle_key()` in `src/editor/input.c`. There are TWO distinct paths:
   - **Interactive `;` key** (`src/editor/input.c`, `key == ';'` block):
     the input buffer does NOT include the `;` — the keystroke triggers the
     commit but is not appended. Commit handlers must accept input
     without a trailing `;`.
   - **`editor_feed_line()`** (`src/editor/input.c`): copies the full line
     (including `;`) into the input buffer, then runs the same dispatch chain.
     Used by file loading and example loading.
   - **Enter key** (insert mode): input may or may not have `;`
     depending on what the user typed.
   The dispatch chain calls the consolidated `editor_try_commit_*()` helpers
   in `src/editor/commit.c` (`editor_try_commit_var_statements`,
   `editor_try_commit_block_structs`, `editor_try_commit_any`, plus the var-then-
   insert variant). Internally those run, in canonical order:
   `editor_try_commit_float_decl` → `editor_try_assign_variable` → `editor_try_commit_close_brace`
   → `editor_try_commit_for_loop` → `editor_try_commit_func_def` → `editor_try_commit_if_block`
   → `repl_parse_and_normalize()` (general GL commands).
   **Ordering matters**: `editor_try_commit_float_decl` MUST run before
   `editor_try_assign_variable`, otherwise `float x` is misread as an
   assignment. Each handler returns 1 if it consumed the input
   (success or error with status message), 0 if it didn't match.
   If all handlers return 0, `parse_command()` in `src/repl/parser.c`
   sets the per-context error buffer.
3. **Parse** — `parse_command()` in `src/repl/parser.c` matches the line to a
   `CmdType`, evaluates argument expressions via `eval_expr()`, stores
   result in `GLCmd.args[]`. Per-line canonical text lives in
   `EditorState`'s editor buffer (not on `GLCmd`); the parser returns it as
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

The `editor_try_commit_*` handler chain is consolidated into four helpers in
`src/editor/commit.c`:
- `editor_try_commit_var_statements()` — float decl, then assign
- `editor_try_commit_block_structs()` — close-brace, for, func, if
- `editor_try_commit_any()` — both groups in canonical order
- `editor_try_commit_var_statements_then_insert()` — var variant used by the
  overwrite-mode Enter key, which must flip to insert mode on success

Dispatch sites then call these helpers instead of open-coding the chain:
1. **`;` key handler** — `key == ';'` block in `editor_handle_key()` calls
   `editor_try_commit_any()`
2. **Enter key, insert mode** — calls `editor_try_commit_var_statements()` and
   `editor_try_commit_block_structs()` to maintain the insert-mode behavior
3. **Enter key, overwrite mode** — uses
   `editor_try_commit_var_statements_then_insert()` plus
   `editor_try_commit_block_structs()`
4. **`editor_feed_line()`** — the programmatic entry point calls
   `editor_try_commit_any()`

When adding a new handler, add it to the right helper rather than all
call sites. Ordering inside each helper is load-bearing:
`editor_try_commit_float_decl` MUST run before `editor_try_assign_variable`, otherwise
`float x;` is misread as an assignment to an identifier named "float".

### Editing Existing Lines

When the user navigates to an existing line, `editor_load_line_to_input()` reads
the line text from the editor buffer view, strips the trailing `;` and
whitespace, and loads it into the input buffer. This means re-committing
the line goes through the no-semicolon path. Commit handlers that check
for `;` must also accept end-of-string as a valid terminator.

### Float Variable Declarations (`CMD_VAR_DECLARE`)

`editor_try_commit_float_decl()` in `src/editor/commit.c` handles `float name;`
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
  commit time via `repl_eval_declare_predef_var()`
- `GLCmd` fields: `var_names[MAX_NAMES_PER_DECL][16]`, `var_decl_count`
- Editing an existing `CMD_VAR_DECLARE` line works: the overwrite
  detection runs before the "already declared" validation loop, and
  names carried over from the old decl are exempted from the duplicate
  check (they get undeclared before the new registration runs).
- Deleting a declaration range goes through `repl_compile_delete_range()`,
  which validates that no variable in the range is still referenced
  outside it (uses `repl_eval_source_uses_ident()`). Deleting a decl
  together with all its uses is allowed; deleting an unreferenced decl
  by itself is allowed. Cut/copy/paste of decl rows remain blocked
  outright (clipboard semantics — see commit 72be1dd).
- C export writes `// @declare name` markers; import via
  `import_parse_declare_marker()` in `src/repl/export.c` reconstructs
  the `CMD_VAR_DECLARE` commands, bypassing `editor_try_commit_float_decl`
- `src/repl/examples.c` has multi-name declarations (e.g. `"float n, x, y, z, j, k;"`)
  — if simplifying to single-name, these must be split into separate lines
- Related helpers in `src/repl/eval.c`: `repl_eval_declare_predef_var()`,
  `repl_eval_undeclare_predef_var()`, `repl_eval_find_predef_var_idx()`,
  `repl_eval_is_reserved_ident()`, `repl_eval_source_uses_ident()`,
  `repl_eval_validate_expression_idents()`

### User Scenes & Auto-Promotion

The REPL keeps up to `MAX_USER_SCENES = 8` independent scenes in
`g_user_scenes[]` (`src/repl/scenes.c`). Slot 0 is the pinned "home" scene —
the pre-example editor state captured on first example load, never
auto-evicted. Each `UserScene` stores command array + count + edit_line
+ predef variable values + scene `name` + `last_touch` tick.

- **Active slot.** `repl_active_user_scene()` returns the current slot
  index, or `-1` when an example or fresh empty workspace is loaded.
- **Auto-promote on first edit.** `editor_undo_push_snapshot()` calls
  `repl_promote_example_if_needed()` before every mutation. When the
  user is editing an example, that allocates a fresh slot, copies state
  into it, inherits the example's name (de-duplicated by
  `derive_unique_scene_name`), and sets the active slot. The user never
  sees the promotion directly — subsequent edits accumulate into the
  new user scene.
- **LRU eviction.** When every non-home slot is full *and* a workspace
  directory is bound, the next promotion evicts the LRU non-pinned,
  non-active slot to `<workspace_dir>/<slug>.c` and reuses the index.
  With no workspace bound, promotion is rejected with a status message
  (the user must save a workspace first).
- **F12 cycle.** `examples → user scenes (in slot order) → back to first
  example`. Handles both "active example" and "active scene" starting
  states.
- **Inline rename.** `editor_inline_rename_*` in
  `src/editor/inline_rename.c`; triggered by Scene → "Rename active
  scene"; commits via `repl_user_scene_rename` (Enter), Esc cancels.
  Path-unsafe chars (`/`, `\`, `:`) and non-printables are filtered at
  input time since names become filesystem slugs on workspace export.
- **Workspace I/O.** `repl_save_workspace(dir)` mkdirs `dir` and
  iterates every occupied slot, setting the export scene-name hint per
  slot. `repl_load_workspace(dir)` loads each `*.c` into a fresh slot;
  scene names come from `@scene-name` headers (filename stem as
  fallback). Single-file save/load still works — files round-trip
  between modes via the `@scene-name` / `@workspace-dir` headers.

### Example Metadata

Built-in examples in `src/repl/examples.c` can prefix their command list
with:

1. Contiguous `// @cfg <slug> = <value>` lines.
2. An optional 5-line `// camera` preset block.

Leading metadata is consumed before lines feed through the commit
pipeline, so it stays hidden from the code panel. `@cfg` parsing reuses
`parse_workspace_header_line()` from `src/repl/export.c`, restricted to
these scene-presentation slugs:

`wireframe`, `grid`, `grid_major`, `grid_extent`, `axes`,
`vertex_labels`, `normal_vectors`, `vertex_outlines`, `vertex_points`,
`vertex_guides`, `light_indicators`, `backdrop`, `camera_rotate`, `variable_panel`.

Non-leading `@cfg` lines are not metadata — they stay as ordinary
comments.

**Reset and restore rules:**

- Every example load resets the allowed non-camera scene-presentation
  settings to built-in defaults *before* applying the example's leading
  `@cfg` metadata. Prevents stale state leaking across examples.
- Camera is intentionally excluded from that reset. Examples inherit
  the current camera unless they supply an explicit `// camera` header.
- `restore_user_scene()` restores commands and predefined variables
  only. Leaving an example does not restore camera or other
  presentation state.

The single source of truth for example-owned presentation defaults is
the `CFG_DEFAULT_*` macro block in `src/app/glr_defaults.h`. Initializers, example
reset helpers, and tests reuse those macros instead of duplicating
literals. `make test_repl_core_examples` is the focused regression
suite; touch `src/repl/core.c`, `src/repl/export.c`, `src/repl/examples.c`,
`src/app/glr_defaults.h`, and `tests/test_repl_core_examples.c` together when
changing example-metadata behavior.

### Save/Load (output.c)

`src/repl/export.c` handles bidirectional text format:
- **Export** (`repl_export_save_output()`): writes a standalone C file with header
  comments embedding workspace state (`@var name=value`,
  `@cfg setting=value`, `@scene-name <name>`, `@workspace-dir <path>`),
  camera state as the raw `glTranslatef`/`glRotatef` sequence the REPL
  uses internally, predefined vars plus fixed scratch arrays `A/B/C[8]` as
  globals, REPL functions as C
  functions, and `display()` body containing the user's geometry commands.
  The workspace iterator in `src/repl/core.c` sets the export scene-name hint
  in import/export state before each slot's save so the hint wins over the
  active user scene index.
- **Import** (`repl_export_load_from_file()`): line-by-line scan parses camera state
  and workspace directives, detects function definitions (converts C
  syntax back to REPL), and feeds geometry lines through `editor_feed_line()`.
  Pending scene-name and workspace-dir directives are read by the caller
  after `repl_export_load_from_file` returns so the importer can name the new slot
  and remember the workspace dir.

### Replay System

Step-by-step execution visualization in `src/widgets/replay.c`:
- `ReplReplayRuntimeState` (via `replay_state_view()`) tracks state
  (OFF/PLAYING/PAUSED/DONE), program counter, and speed multiplier
- During playback, the flat command count is clamped to `replay_exec_limit()`
  so only commands up to the PC render
- Fade batch ring buffer — fading geometry snapshots; old geometry fades out
  as new geometry appears, rendered in a separate blended pass after the
  main fill pass
- Toggled via Ctrl+G or the Replay header button

### Undo/Redo

Circular snapshot buffers in `src/editor/undo.c`:
- `EditorUndoSnapshot` captures the full editor state: source commands,
  command count, cursor position, predefined variable values
- Undo and redo rings (32 slots each) with head/count tracking
- `editor_undo_push_snapshot()` called before any mutation (delete, paste,
  reformat, etc.); `editor_undo_pop_snapshot()` on Ctrl+Z; `editor_undo_do_redo()` on
  Ctrl+Y. Also the hook where `repl_promote_example_if_needed()` fires
  so editing an example auto-creates a user scene.
- Pushing clears the redo stack; undo moves current state to redo
- The rings are global, not per-scene. Any wholesale replacement of
  the live REPL document **must** call `editor_undo_clear()` first or a
  post-switch Ctrl+Z restores the previous scene's snapshot into the
  new one. Call sites: `glr_app_reset_all`, the F12 cycle
  (`cycle_example_or_user_scene`), and the load-example /
  load-user-scene / load-workspace menu actions in
  `src/app/glr_actions.c`. The clear lives in `src/editor/` (the ring's
  owner); callers sit in `src/app/` to preserve the
  editor-depends-on-repl layering (repl/scenes.c can't reach editor/).

### Cursor Edit Guides

The vertex/normal guides drawn at the cursor line
(`src/scene/guides/geometry_guides.c`) are fed by a `SceneGuideSnapshot`.
The non-obvious data-flow gotcha: `glr_ctrl_build_guide_snapshot()`
fills `snapshot.vertex_args` / `normal_args` by text-parsing the input
line with a **predef-only** evaluator. That can't resolve funcN-local
params (`scale`, `phase`) or loop-assigned vars, so for a cursor inside
a funcN body those args silently evaluate to 0 and the guide lands at
the object's local origin.

The fix path (`src/app/glr_ctrl.c`):
- `glr_ctrl_render_cursor_guides` always walks the flat program via
  `replay_walk_user_vertices` (no fast-path skip — that broke modelview
  tracking inside funcN frames). `ctx.stop_flag` makes the walk bail
  out once both guides have rendered, keeping big loops cheap.
- At the cursor's first flat-cmd, `cursor_guide_snapshot_with_flat_args`
  overrides `vertex_args` / `normal_args` from the **flat** cmd's args
  (flatten already substituted funcN params and re-evaluates every
  frame for `has_vars` cmds, so they track animation). For a normal
  cursor it also walks forward in the flat program to set
  `normal_base_pos` — the live anchor point — since
  `draw_normal_guides`'s own source-cmd search is parse-time-frozen.
- `parse_vertex_arg_slots` uses `repl_scan_next_arg_delim` so nested
  parens (`cos(i + phase)`) don't truncate a slot and drop the guide
  into its wrong-arg-count branch.

### Autocomplete

Symbol matching and function parameter hints in `src/app/glr_completion.c` (registered as the editor's `EditorCompletionProvider`):
- `editor_state_autocomplete()->matches` — matched completions from GL command/constant tables
- `editor_state_autocomplete()->ghost` — suffix to append to input on Tab accept
- `editor_state_autocomplete()->hint` — parameter list hint shown below cursor
- Modes: `AC_MODE_FUNC_PREFIX` (after `foo(` → param hints),
  `AC_MODE_ENUM_SLOT` (slot-indexed GL constant completion over
  `def->args[slot].enums`; the active slot is the top-level comma
  count),
  `AC_MODE_POINT_PARAM` (3D point coordinates)

### Search

Case-insensitive text search in `src/editor/search.c`:
- Activated by Ctrl+F; query and state accessed via `editor_state_search()`
- `editor_search_find_next_in_text()` finds substring matches across
  all visible lines (header, user code, footer)
- `hit_line_idx`/`hit_char_idx` in `EditorSearchState` track current match position
- Integrated with code panel rendering for match highlighting

### Config Menu

Declarative toggle system in `src/app/glr_actions.c`:
- `g_cfg_items[]` array of `ReplConfigItem` descriptors: `{ label, key_code,
  is_special, key, state_count, state_names[], section_header }`
- Each item is a toggle (2 states, default OFF/ON) or cycle (>2 states
  with named entries, e.g. grid themes)
- **Section flyout menu.** `### ` rows in `g_cfg_items[]` define
  sections; the Config dropdown shows one **parent row per section**
  (label with `### ` stripped) plus a synthetic trailing **All** row,
  each hover-opening a flyout of its items. The flyout engine is the
  generic one shared with the Scene example submenu (one
  `(menu_id, parent_row)` provider in `src/ui/menu_bar.c`:
  `submenu_row_count/_label/_abs_index/_kind/_is_active`,
  `menu_row_has_submenu`, `submenu_rect/_hit_test`,
  `render_active_submenu`). The pure section model lives in
  `src/app/glr_config.c` (`glr_config_section_count/_label/_range`,
  `glr_config_row_kind`); it counts only real `### ` headers — the
  `All` row is owned in the menu layer (`config_all_parent_row`), never
  double-counted. The `All` flyout spans the whole table 1:1 with
  `### `/`---` rows rendered as inert chrome (`GlrConfigRowKind`).
- **Click semantics.** Section/All parent rows are inert on click
  (hover-open only): the `GLR_MENU_CONFIG` branch of
  `glr_action_menu_item_activate` is a no-op returning 0, mirroring the
  `MENU_SCENE` tag-row guard. A flyout item click
  (`UI_HIT_SUBMENU_ITEM`, `cmd_idx == GLR_MENU_CONFIG`,
  `item_idx == absolute g_cfg_items[] index`) routes via
  `route_submenu_item_hit` → `glr_cfg_cycle_row(idx, +1)` and keeps the
  dropdown open; right-press over a flyout item cycles backward
  (`ui_menu_bar_handle_config_right_press` → `submenu_hit_test`).
  F-key/Ctrl-key shortcuts dispatch through `src/app/glr_actions.c`
  unchanged.
- Adding a config item: append to `g_cfg_items[]` (under the right
  `### ` section) — count is auto-computed via `sizeof`; it joins its
  section's flyout automatically

## Key Controls

| Key | Action |
|-----|--------|
| `;` | Execute/commit current line |
| Enter | Insert new line |
| Up/Down | Navigate lines |
| Tab | Autocomplete |
| Shift+Left/Right | Extend input-buffer selection by one character |
| Shift+Home/End | Extend input-buffer selection to row start / end |
| Double-click | Select the word under the cursor (input-buffer selection) |
| Click + drag | Per-character selection inside the active input row |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut / paste — input selection wins over line-range |
| Ctrl+S | Save to output.c |
| Ctrl+Z | Undo |
| Ctrl+R | Reformat all lines |
| Ctrl+T | Toggle time variable `t` |
| Ctrl+Shift+F | Toggle code focus (hide boilerplate chrome) — also the statusbar "focus" keycap |
| Ctrl+Shift+O | Focus origin — ease the orbit target to (0,0,0) |
| Ctrl+Shift+C | Reset camera to default (eased) |
| Ctrl+Shift+V | Toggle View mode (2D / 3D) |
| F1 | Help overlay — also the clickable statusbar "F1 help" keycap |
| F2-F11 | Toggle visual overlays |
| F12 | Cycle examples and user scenes |

When an input-buffer (character-range) selection is active,
`Ctrl+C` / `Ctrl+X` copy or cut the substring into a separate
`INPUT_TEXT` clipboard slot — they do **not** copy the whole command
line. `Ctrl+V` then inserts the substring at the cursor (replacing
any active destination selection). With no input selection, the
existing line-range clipboard path runs unchanged. See
[`done/editor-input-selection.md`](done/editor-input-selection.md)
for the full model.

## Supported Commands

```
glBegin(MODE), glEnd()
glVertex3f(x,y,z), glVertex2f(x,y)
glNormal3f(x,y,z)
glColor3f(r,g,b), glColor4f(r,g,b,a)
glTranslatef(x,y,z), glScalef(sx,sy,sz), glRotatef(deg,x,y,z)
glPushMatrix(), glPopMatrix(), glLoadIdentity()
glEnable(CAP), glDisable(CAP)
  CAP: GL_DEPTH_TEST, GL_LIGHTING, GL_COLOR_MATERIAL, GL_NORMALIZE,
       GL_LINE_SMOOTH, GL_POINT_SMOOTH, GL_BLEND, GL_CULL_FACE,
       GL_LIGHT0, GL_LIGHT1, GL_LIGHT2, GL_LIGHT3
glShadeModel(MODE)
glPointSize(size)
glLineWidth(width)
glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, const, linear, quadratic)
  - Runtime-gated: silent no-op when the GL context lacks
    glPointParameterfv or GLR_NO_POINT_PARAMETER is set (see the
    GLR_NO_POINT_PARAMETER section under Run).
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA|GL_ONE)
glColorMaterial(face, mode), glMaterialf(face, pname, value)
  glColorMaterial mode: GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR, GL_EMISSION, GL_AMBIENT_AND_DIFFUSE
glLightModeli(pname, param), glFrontFace(mode)
glDepthFunc(func)
  func: GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER,
        GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS
glDepthMask(GL_TRUE|GL_FALSE)
glColorMask(red, green, blue, alpha)
  Each channel is GL_TRUE/GL_FALSE (or 0/1, canonicalized to the
  symbolic token). glDepthMask accepts 0/1 the same way.
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
