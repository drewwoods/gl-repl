# OpenGL Immediate-Mode REPL

Interactive OpenGL command interpreter. Type GL commands, press `;` to execute,
and watch geometry render in real-time with a live code panel.

This file is the compact agent brief: commands, conventions, and gotchas.
Deep detail lives in the docs — consult them instead of guessing:

- [`docs/MODULES.md`](docs/MODULES.md) — layered module overview, ownership
  diagram, boundary rules, guard summary, where new code goes.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — frame pipeline, controller /
  render3d / UI layers, state ownership, host bridges, runtime GL capability
  detection, theming, replay, keymap dispatch order.
- [`src/repl/README.md`](src/repl/README.md) + [`src/repl/ARCHITECTURE.md`](src/repl/ARCHITECTURE.md)
  — the language pipeline (parse → compile → apply → flatten → execute),
  standalone `repl_demo`, rebake/dep-routing model.
- [`docs/ADVANCED_USAGE.md`](docs/ADVANCED_USAGE.md) — full CLI + env-var
  reference, headless OSMesa builds, screenshot/GIF/video capture, doc-media
  regeneration, `@cfg` slug list, music/assets, diagnostics, keymap tooling.
- [`docs/USER_GUIDE.md`](docs/USER_GUIDE.md) — user-facing command semantics
  (fog, clip planes, blending, materials, labels, …).
- [`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md) — guard suite, how to extend
  the REPL.
- [`docs/RELEASE.md`](docs/RELEASE.md) — `make release` orchestration (build
  plan, SHA pinning, staging/upload) and `make app` bundle/signing detail.
- [`packaging/web/README.md`](packaging/web/README.md) — Emscripten/gl4es web
  build shims.

GNU sed is available as `gsed` on macOS (`brew install gnu-sed`).

## Linux / real-gcc verification (gracemont)

Local dev is macOS (`gcc` = Apple clang). Portability-sensitive changes
(build, sanitizer flags, C99) should be cross-checked under real GCC:

```bash
ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && \
  git pull --ff-only origin main && \
  make check-c99 && make test-stubs'
```

Host: Ubuntu 24.04, gcc 13.x. Repo path there has **no `src/` segment**
(`~/code/openGL/samples/gen-ai/gl-repl`). `test-stubs` needs no GL dev libs
(headless-safe, picks up ASan+UBSan); `make test` only if GL/GLUT dev
packages are installed; `make debug-msan` for MemorySanitizer.

## Build

```bash
make gl-repl          # Main binary (vendored static freeglut, macOS Cocoa)
make glut             # Fallback: system GLUT (Apple framework)
make test             # Build + run all tests (debug: ASan + UBSan)
make test-stubs       # Tests against bundled no-op GL stubs (no GL libs needed)
make test-msan        # Stubbed tests under MemorySanitizer
make debug-msan       # Everything under MSan (Clang/runtime permitting)
make check-c99        # C99 ratchet (sample + demos + bench)
make check-state-ownership  # Full boundary/contract guard suite
make freeglut-clean   # Drop vendored freeglut CMake build (forces rebuild)
make app              # (macOS) Bundle gl-repl.app — see docs/RELEASE.md
make release          # Build macOS+Linux artifacts, confirm, upload — see docs/RELEASE.md
make fetch-music      # Download music pack from GitHub release into ./assets
make web              # Emscripten build (needs emcc; see below)
make clean            # Remove binaries
```

Requires gcc (C99), OpenGL, GLUT/freeglut; macOS also needs **cmake** (first
`make gl-repl` builds the vendored freeglut in `third_party/freeglut/build/`,
gitignored, survives `make clean`).

- **Vendored freeglut** (`third_party/freeglut/`): static lib, native Cocoa
  backend, linked by archive path. Linux uses system `-lglut -lGL -lGLU`.
  Re-pin with `scripts/vendor-freeglut.sh [<ref>]` (SHA recorded in
  `VENDORED.txt`; `FREEGLUT_REPO=<url-or-path>` accepts a local fork), then
  `make freeglut-clean`. The pinned tree carries the fork's OSMesa backend
  *and* the SIGUSR1/record capture support on all backends — no re-vendor
  needed for capture work.
- **Headless OSMesa**: `make gl-repl FREEGLUT_OSMESA=1` (after
  `brew install mesa mesa-glu`) renders with no window — for headless
  geometry/feedback tests and captures. Separate `build-osmesa/` dirs, so
  Cocoa and OSMesa builds coexist. Compatibility GL only.
- **Capture/recording**: `kill -USR1 <pid>` writes a PPM frame (native +
  OSMesa); `FREEGLUT_CAPTURE_FRAMES=N` records N frames then exits;
  `FREEGLUT_CAPTURE_STREAM` pipes frames to ffmpeg. Scripts:
  `scripts/docs-assets.sh` (regenerates `docs/images/`; deterministic
  frame-based settling), `scripts/record-gif.sh`, `scripts/record-video.sh`
  (scripted interaction via `GLR_POINTER_SCRIPT` pointer scripts with
  symbolic targets — same engine powers the in-app **Tours** menu:
  `tours/*.pointer` + `tours/catalog.ini`, compiled by
  `scripts/gen_tours.py`, validated by `make check-tours-catalog`).
  Full detail: `docs/ADVANCED_USAGE.md`.
- **Web build**: `make web` needs `emcc` on `PATH` — per-shell
  (`source <emsdk>/emsdk_env.sh`); `scripts/build-web.sh` is self-contained
  (sources emsdk itself; checkout via `EMSDK`, default `~/src/emsdk`).
  `make web-serve` serves at `http://localhost:8000/`. gl4es → WebGL2;
  deps fetched by `scripts/web-deps.sh` into gitignored `third_party/web/`.
- **Sanitizers**: test targets default `BUILD=debug` = ASan + UBSan
  (`-fno-sanitize-recover`). `make debug-msan`/`test-msan` = MSan + origin
  tracking (Clang; test run sets `GLR_AUDIO_NO_DEVICE=1`). `NO_SAN=1`
  disables debug sanitizers. `gl-repl`/`bench`/demos stay `BUILD=release`;
  an explicit `BUILD=...` always wins (`make coverage`, `make test
  BUILD=release`).

### C99 standard

**Everything compiles `-std=c99`, project-wide, no exceptions** (old-GCC /
Linux portability). Non-pedantic by default — GNU extensions GCC accepts in
`-std=c99` are fine; there is no C2x build and no `STD` knob.
`make check-c99` syntax-checks the sample + demos + bench under
`gcc -std=c99 -fsyntax-only` (project dirs `-I`, GL headers `-isystem`;
tests excluded but build under plain `-std=c99` too).

Conventions for genuinely-old-GCC portability:

- `STATIC_ASSERT(expr, msg)` from [`include/c_compat.h`](include/c_compat.h)
  — **never raw `_Static_assert`**.
- Keep every TU non-empty even when `#ifdef`-gated off.
- Prototyped function-pointer typedefs, not old-style `void (*)()`.
- Plain `, __VA_ARGS__` (no GNU `##`); one `typedef` per type name.

#### `#include` style

Anything in this tree uses quoted `""`; system/vendored headers use `<>`.
Enforced by `make check-include-style` (tracks the ambiguous bare names
[`c_compat.h`](include/c_compat.h), `gl_includes.h`, [`keys.h`](include/keys.h), [`gl_2d.h`](src/ui/core/gl_2d.h)).

```c
#include <stdio.h>           // system — angle
#include <GL/gl.h>           // system GL — angle
#include <miniaudio.h>       // vendored third-party — angle
#include "gl_includes.h"     // project-local — quoted
#include "support/cpuprof.h" // project-local subdir — quoted
```

### GL stub headers

`tests/gl-stubs/include/` ships no-op GL/GLU/GLUT headers so machines
without GL dev packages can compile + run non-rendering tests
(`make test-stubs`, `make gl-repl USE_GL_STUBS=1`; objects go to
`build/*-gl-stubs`). Stubs are compile-only — no window, no pixels. If the
sample calls a new GL/GLU/GLUT symbol, extend the matching stub header;
keep stubs minimal no-op. After touching stubs verify: `make test-stubs`,
`make gl-repl USE_GL_STUBS=1`, `make gl-repl`.

## Run

```bash
./gl-repl                  # Fresh session
./gl-repl output.c         # Reload saved session
./gl-repl workspace/       # Load every *.c as a user scene
./gl-repl --example torus  # Start on a built-in example (name or 1-based index)
./gl-repl --list-examples
./gl-repl --no-audio       # Skip audio init
./gl-repl --assets <dir>   # Music dir override (also GLR_ASSETS_DIR)
./gl-repl --time 5         # Initial animation t (also GLR_TIME; --time wins)
./gl-repl --example 9 --export-ply out.ply [--export-ply-srgb]
./gl-repl --dump-code      # Print loaded buffer; --dump-* family honors --example
./gl-repl --noaccum        # Disable accumulation buffer (AA + blur)
./gl-repl --detailed-prof  # Fine-grained init-trace phases (also GLR_DETAILED_PROF)
```

Capture/headless hooks (env): `GLR_EDIT_LINE=<n>` (park cursor → cursor-bound
overlays render headlessly), `GLR_TYPE_KEYS='...'` (feed keystrokes after
load), `GLR_OPEN_COLOR_PICKER=<line>` / `GLR_OPEN_GL_STATE=<line>` (open
floating popups on frame 1), `GLR_ACCUM_PASSES=1/2/4/8/12/16`,
`GLR_TICK_PER_FRAME=1` (fixed-dt sim advances per rendered frame —
deterministic offline capture), `GLR_VIEW_TOGGLE_AT=<t1,t2,...>` (2D/3D
swatch transition; implies tick-per-frame), `GLR_POINTER_SCRIPT=<file>`
(scripted pointer/keyboard + cursor overlay), `GLR_NO_SPLASH=1`.
Other env: `GLR_NO_POINT_PARAMETER=1` (force the no-`glPointParameterfv`
fallback path — runtime-gated, no build flag; see docs/ADVANCED_USAGE.md),
`GLR_NO_GPU_PROF=1`, `GLR_AUDIO_HITCH_MS=<ms>` (worker hitch log threshold,
default 50).

- `t` starts at 0 and advances 1/60 s per simulation tick while playing;
  `--time`/`GLR_TIME` applies after `--example` load so the override sticks.
- **Music**: `glr_audio_bootstrap()` in [`src/app/glr_audio.c`](src/app/glr_audio.c) concatenates
  three sources (each sorted by filename): primary assets dir
  (`./assets`, overridden by `--assets` > `GLR_ASSETS_DIR`), bundled
  `<exe>/../Resources/assets` (the `.app` case), and the per-user folder
  (`~/Library/Application Support/gl-repl/Music` on macOS,
  `$XDG_DATA_HOME/gl-repl/music` elsewhere; created on first run). Zero
  mp3s → fallback `assets/song.mp3`. All in [`src/app/glr_audio.c`](src/app/glr_audio.c) statics; the
  `#ifdef` platform branches must stay C99/portable and localized.
- **Startup diagnostics**: always-on `[init +N.NNNs] <phase>` stderr trace in
  `main()`; `--no-audio` isolates `ma_engine_init()` stalls;
  `--detailed-prof` adds glutInit/playlist/first-two-frames phases. The
  audio worker logs `worker hitch: <op> took N ms` for blocking ops over
  `GLR_AUDIO_HITCH_MS`.

## Test

```bash
make test                  # All tests
make test_eval             # Expression evaluator
make test_format           # Indentation/formatting
make test_repl_core_parse | test_repl_core_format | test_repl_core_commit | test_repl_core_io
```

Test sources under `tests/`, shared helpers `tests/support/`; binaries land
at the repo root (`./test_eval`, …).

**Guards**: `make check-state-ownership` runs the full ownership/contract
inventory (includes `check-c99`, `check-include-style`, `check-keymap-no-dup`,
`check-palette`, …; see the Makefile). Outside that aggregate, two
hard-failing guards: `check-duplicate-api-decls` and
`check-trailing-whitespace` (commits since `origin/main`; also in
`test-stubs` and the pre-push hook).

Design/audit rationale too big for a commit message goes in `docs/plans/`;
root docs describe the current design first and link plans for background.

## File Layout

Terse map; per-file responsibilities in depth: `docs/MODULES.md`.

| Path | Responsibility |
|------|----------------|
| [`gl_repl.c`](gl_repl.c)/`.h` | `main()`, GLUT callback registration, window/timer scheduling, playlist sources, init trace; forwards to `glr_ctrl_*` |
| [`config.h`](config.h) | Compile-time constants, force-included into every TU; includes [`keymap.h`](keymap.h) |
| [`keymap.h`](keymap.h) | Action→key bindings: one `#define GLR_<ACTION> <key>, <mods>` per action; matched via `keymap_event_is`; `KM_KEY`/`KM_MODS` split for case labels/initializers |
| [`accent_palette.h`](accent_palette.h) | Shared accent palette anchors + semantic roles + brand-mark list; `make check-palette` ratchet, `make palette-list` |
| [`prof_sections.h`](prof_sections.h) | `ProfSection` enum catalog, force-included (labels live in [`glr_prof.c`](src/app/glr_prof.c)) |
| `include/` | Project-agnostic headers: [`keys.h`](include/keys.h) (physical key bytes), [`c_compat.h`](include/c_compat.h), [`gl_includes.h`](include/gl_includes.h) |
| **src/app/** | |
| `glr_ctrl.{c,h}` | App-frame controller: display/reshape/init-GL, builds [`Render3dRenderConfig`](src/render3d/render_types.h#L135), UI snapshot, chrome clear, camera load |
| `glr_frame_pacer.{c,h}` | Pure absolute-deadline 60 Hz timer-delay calculator used by the GLUT host |
| [`glr_ctrl_router.c`](src/app/glr_ctrl_router.c) | GLUT input dispatch shims, [`UiHit`](src/ui/core/hit.h#L51) routing, wheel, SIGINT-quit |
| `glr_config.{c,h}` | Config-key impl + section model; `glr_config_set` tail notifies the tutorial runner |
| `glr_actions.{c,h}` | `g_cfg_items[]` descriptor table, config shortcuts, menu actions |
| `glr_camera.{c,h}` | Camera state, orbit/pan/zoom drags, momentum |
| [`glr_completion.c`](src/app/glr_completion.c) | Autocomplete provider (specs/predefs/`CMD_FUNC_DEF`; ghost + hints) |
| `glr_tours.{c,h}` | Tours-menu catalog (file-backed `tours/*.pointer`) |
| `glr_audio.{c,h}` | Playlist engine + persisted audio config (worker thread) |
| `glr_mesh_export.{c,h}` | PLY export via one `GL_FEEDBACK` capture of the live flat program |
| `glr_debug.{c,h}` | Diagnostic dumps for CLI flags/tests |
| `glr_prof.{c,h}` | `prof_section_info` table + GPU-bracketing policy |
| [`glr_defaults.h`](src/app/glr_defaults.h) | `CFG_DEFAULT_*` scene/presentation defaults (single source of truth) |
| [`glr_pointer_script.c`](src/app/glr_pointer_script.c) | Synthetic pointer/keyboard script engine (captures + tours) |
| **src/repl/** | Language pipeline — see `src/repl/ARCHITECTURE.md` |
| [`command.h`](src/repl/command.h) | [`CmdType`](src/repl/command.h#L37) enum + [`GLCmd`](src/repl/command.h#L95) (pure parse result, no `source[]`) + set predicates |
| `parser.{c,h}` | Source-line parser; canonical text via `ReplParsedLine.text` |
| `command_spec.{c,h}` | Command metadata; `k_enum_command_specs[]`/`k_std_command_specs[]` (alphabetical by GL name); owns `k_attrib_bits[]` (the glPushAttrib `GL_*_BIT` groups, canonical order) + `repl_attrib_bit_entries()` |
| `attrib_bits.{c,h}` | Pure (no-GL) glPushAttrib/glPopAttrib mapping: command → bit mask, per-cell state writes (flow-sensitive color-material), masked-LIFO-fold collectors for the editor per-bit highlighting; also consumed by [`gl_state_inspector.c`](src/repl/gl_state_inspector.c) so the two can't drift |
| `command_store.{c,h}` | Low-level [`GLCmd`](src/repl/command.h#L95) array mechanics |
| `compile.{c,h}` / `apply.{c,h}` | Pure validators → [`ReplCompiledChange`](src/repl/compile.h#L130) descriptors; apply mutates runtime arrays |
| [`normalize.c`](src/repl/normalize.c) / `reformat.c` / `format.{c,h}` / `source_scope.{c,h}` | Parse-and-normalize, reformatter, indentation, block-depth cache |
| `flatten.{c,h}` / `flatten_expr.{c,h}` / `flatten_query.{c,h}` / `expr_program.{c,h}` | Source→flat expansion, dep masks + value-only rebake, compiled-expression cache, cursor/cost queries |
| `executor.{c,h}` | Flat-array walk emitting GL calls |
| `eval.{c,h}` | Expression evaluator, translators, [`ExprVar`](src/repl/eval.h#L135)/[`ExprCtx`](src/repl/eval.h#L142), predef-var management |
| `state.{c,h}` + [`state_views.h`](src/repl/state_views.h) / [`state_owners.h`](src/repl/state_owners.h) | `g_repl_state` owner; read-only views (safe for render3d/ui) vs `_mut()` accessors (owners/controller only) |
| [`cfg_baseline.h`](src/repl/cfg_baseline.h) / [`pipeline.h`](src/repl/pipeline.h) | Config bag + typed live-cfg helpers; frame-orchestration surface |
| [`autonormal.c`](src/repl/autonormal.c) | Auto `glNormal3f` maintenance |
| [`scenes.c`](src/repl/scenes.c) / [`scene_snapshot.c`](src/repl/scene_snapshot.c) / [`workspace_io.c`](src/repl/workspace_io.c) | User-scene slots + LRU + workspace orchestration; snapshot copy/apply; pure filesystem/naming mechanics |
| [`example_loader.c`](src/repl/example_loader.c) / `examples.{c,h}` | Example loading + built-in example data |
| `tutorials.{c,h}` | Tutorial catalog (steps, tags, `@cfg`, setup scaffolds, subheadings): 23 lessons in contiguous Beginner/Intermediate/Advanced runs across six tag groups; Phase C adds block-language lessons plus effects, materials, culling, and bitmap-text coverage |
| `export.{c,h}` / [`import.c`](src/repl/import.c) / [`export_state.h`](src/repl/export_state.h) | C export writer / import reader (workspace headers, `@declare`, C↔REPL translators) |
| `help_text.{c,h}` | F1 help content tables |
| `time.{c,h}` / [`transform_utils.h`](src/repl/transform_utils.h) / [`program_query.c`](src/repl/program_query.c) | `repl_set_time`; header-only matrix helpers; decl-tag collectors |
| **src/editor/** | |
| `input.{c,h}` | REPL key dispatcher (`;` commit, Tab, Ctrl+R, tutorial guards); generic twin in [`tools/editor_demo/input.c`](tools/editor_demo/input.c) |
| `edit_ops.{c,h}` | Generic text-edit primitives, REPL-free (`check-edit-ops-pure`) |
| `commit.{c,h}` | Commit transaction boundary: compile → undo snapshot → buffer write → apply |
| `state.{c,h}` | [`EditorState`](src/editor/state.h#L175): buffer, cursor, selection, search, autocomplete, scroll, undo rings |
| `clipboard.{c,h}` / `undo.{c,h}` / `search.{c,h}` | Selection+clipboard; snapshot rings (+example auto-promote hook); Ctrl+F search |
| `inline_rename.{c,h}` / `completion.{c,h}` / `help_session.{c,h}` / [`limits.h`](src/editor/limits.h) | Scene rename, provider registry, help-overlay session, capacity constants |
| **src/subsystems/** | Peer subsystems (editor/UI-independent) |
| `replay/` | Replay state machine, fade-batch ring, input routing, GL fade pass, annotations (`replay_*.c`) |
| `tutorial/` | Runner / state / match / fade-animation split (`tutorial_*.c`) |
| `color_picker/` / `variable_panel/` | Floating-picker peer (writeback via editor commit); slider-panel peer + drag (persists once at release) |
| `edit_overlays/` | Cursor guide snapshot + flat-walk overlay orchestration (replays clip/cull state as it walks) |
| **src/render3d/** | 3D scene renderer — no REPL dependency (proof: `render3d_demo`) |
| `render.{c,h}` | Frame orchestration + accum loop; camera transform is the **caller's** job |
| [`grid.c`](src/render3d/grid.c) / [`axes.c`](src/render3d/axes.c) / [`backdrop.c`](src/render3d/backdrop.c) / [`lights.c`](src/render3d/lights.c) / [`overlays.c`](src/render3d/overlays.c) / `depth_viz.{c,h}` / `render3d_transition.{c,h}` | Grid/axes themes, backdrop, lights, per-vertex overlay primitives, depth visualization, fade state machine |
| `guides/` | Geometry + transform guide passes ([`Render3dGuideSnapshot`](src/render3d/guides/guides_shared.h#L44)) |
| [`render_types.h`](src/render3d/render_types.h) / [`palette.h`](src/render3d/palette.h) | Shared config/context types; overlay/guide color tokens |
| **src/ui/** | 2D view rendering + hit-test (pure over snapshots) |
| `core/` | [`gl_2d.h`](src/ui/core/gl_2d.h), `text_layout.{c,h}`, `hit.h` ([`UiHit`](src/ui/core/hit.h#L51)), `tabbed_overlay.{c,h}`, [`layout_utils.h`](src/ui/core/layout_utils.h) |
| `app/` | `state.c` (UiState + status history ring), `panels.{c,h}` (code panel + hit-test), `menu_bar.{c,h}` (dropdowns + shared flyout engine), `scene_tabs.{c,h}`, `layout.{c,h}`, `overlay_layout.{c,h}` (floating-panel stacking solve), `repl_code_panel.{c,h}`, `autocomplete_panel.{c,h}`, `gl_state_panel.{c,h}`, [`snapshot.h`](src/ui/app/snapshot.h) ([`UiRenderSnapshot`](src/ui/app/snapshot.h#L70)), [`editor.h`](src/ui/app/editor.h) |
| `subsystems/` | `replay_hud`, `color_picker`, `variable_panel` renderers |
| `support/` | `cpuprof.{c,h}` (profile/FPS/histogram panels — log/log axes), `memprof.{c,h}` |
| **src/support/** | Neutral: `cpuprof` (host-agnostic CPU prof, log-binned histograms), `gpuprof` (GL timer queries, injected fn table, GL-free TU), `mesh_ply` (pure PLY writer) |
| `tests/` + `tests/support/` + `tests/gl-stubs/` | Tests, shared harness, no-op GL headers |

## Conventions

- File-private statics use `g_` prefix. Cross-module runtime state goes
  through typed facades: [`src/repl/state.h`](src/repl/state.h) (REPL),
  [`src/editor/state.h`](src/editor/state.h) (editor), peer accessors
  ([`replay_state_view()`](src/subsystems/replay/replay_state.h#L126), `variable_panel_state_view()`). Static helpers are
  file-scoped; public API through module headers.
- Prefixes express ownership: `repl_*` (language/program model), `editor_*`
  (text document), `glr_*` (app shell/controller/services), `render3d_*`
  (3D), `ui_*` (2D view), neutral (`prof`) for generic utilities. Don't
  introduce new top-level prefixes without documenting the boundary.
- **Config toggles**: append a [`ReplConfigItem`](src/repl/cfg_baseline.h#L29) descriptor to `g_cfg_items[]`
  in [`src/app/glr_actions.c`](src/app/glr_actions.c) (under the right `### `
  section); count auto-computes, flyout membership is automatic.
- **New GL commands**: add to [`CmdType`](src/repl/command.h#L37) ([`src/repl/command.h`](src/repl/command.h)),
  handle in [`repl_parser_parse_command_ctx()`](src/repl/parser.h#L100), [`repl_execute_program()`](src/repl/executor.h#L199), and
  `flatten_range()` (static in flatten.c); add a `g_command_type_specs[]`
  entry with the right [`CmdSyntaxCategory`](src/repl/command_spec.h#L153). Enum-arg / float-arg commands:
  append a row to `k_enum_command_specs[]` / `k_std_command_specs[]`
  (**keep alphabetical by GL name**).
- Enum args live in `GLCmd.args[]` (there is **no `GLCmd.mode` field** — its
  absence is the invariant). Per-slot [`ReplEnumSlotKind`](src/repl/command_spec.h#L77): `ENUM_ONLY`
  (default), `ENUM_OR_CONST_VALUE` (bool masks, 0/1 reverse-mapped),
  `ENUM_OR_EXPR` (only `glLightModeli` slot 1).
- [`CmdType`](src/repl/command.h#L37) set tests use the inline predicates in [`command.h`](src/repl/command.h)
  (`repl_cmd_is_transform`, `repl_cmd_emits_vertex`, `repl_cmd_is_block_head`
  / `_end`), never ad-hoc `||` chains. That's the *control-flow* taxonomy;
  [`CmdSyntaxCategory`](src/repl/command_spec.h#L153) is the separate *visual* one — don't fold one through
  the other. A drift test in [`tests/test_replay_walk.c`](tests/test_replay_walk.c) asserts agreement.
- Splitting comma-separated call args: [`repl_scan_next_arg_delim()`](src/repl/eval.h#L428) from
  [`src/repl/eval.h`](src/repl/eval.h), never bare `strchr(s, ',')` — those
  are paren-naive and truncate `cos(i + phase)`.
- **Keyboard bindings**: one [`keymap.h`](keymap.h) pair per action; call
  sites use `keymap_event_is(key, GLR_X)` and never spell out modifiers;
  `KM_KEY`/`KM_MODS` for case labels / initializers. Guard:
  `make check-keymap-no-dup`; `make keymap-list` shows bindings + free
  slots. [`editor_handle_key()`](src/editor/input.h#L56) takes ASCII (Ctrl+X arrives as X & 0x1F),
  [`editor_handle_special()`](src/editor/input.h#L57) F-keys/arrows. Cross-subsystem routing (replay /
  save / config / audio / camera / tutorial-ack) lives in the
  `glr_ctrl_router_*` helpers, dispatched before `editor_handle_key`.
  macOS Cmd+letter is normalized to Ctrl form at the top of
  `glr_ctrl_keyboard`.
- Expression variables: [`ExprVar`](src/repl/eval.h#L135) in eval.h; predef set via
  `repl_state_variables()` + [`repl_eval_declare_predef_var()`](src/repl/eval.h#L309).

## Architecture — agent gotchas

Full prose: `docs/ARCHITECTURE.md` and `src/repl/ARCHITECTURE.md`. What
follows is the trip-wire list.

### Frame & rendering

[`glr_ctrl_display_frame()`](src/app/glr_ctrl.h#L147) drives each frame: rebuild autonormals + flat
program if dirty → build [`Render3dRenderConfig`](src/render3d/render_types.h#L135) → clear chrome + load camera
+ scissor (all **controller** policy — render3d owns no camera type, sets no
scissor, clears no color/depth) → [`render3d_draw_scene()`](src/render3d/render.h#L136) (projection → user
geometry callback → replay fades → grid/axes/backdrop → overlays → replay
HUD) → 2D overlays. `render3d_demo` is the load-bearing proof that
`src/render3d/` has no REPL dependency; `make render3d-hot` is its
dlopen-based live-reload variant (state lives in the host TU; a
[`Render3dState`](src/render3d/render.h#L96) layout change still needs a relaunch).

### Accumulation effects

Accum effect Off/AA/Blur/Blur Cam × accum passes (1..16). The **effect picks
the blur axis** — camera motion never overrides it: Blur = animation-time
blur (re-bakes the flat program at sub-step `t` via
`repl_state_time_set_transient` + `repl_refresh_flat_program_for_deps`;
works while paused), Blur Cam = camera-pose lerp only. Blur and AA jitter
are never combined; an axis with nothing to blur falls back to AA jitter.
Replay forces the fallback (a per-sample reflatten would clobber the
replay-narrowed flat count). Each sample resets predef/scratch/render to a
frame baseline so accumulating programs don't compound.

### Two-level command model

Source `GLCmd[]` (per-line canonical **text lives in [`EditorState`](src/editor/state.h#L175)'s editor
buffer, not on [`GLCmd`](src/repl/command.h#L95)**) → flat array (loops unrolled, funcs inlined, ifs
resolved; each flat cmd records `src_cmd_idx` / `call_src_cmd_idx` /
`func_scope_mask`) → executor emits GL. Any edit marks the flat array dirty;
rebuilt next frame. Budgets: `MAX_FLATTEN_VISIT_BUDGET` = 200000,
`MAX_FLATTEN_CALL_DEPTH` = 64. Animation is reflatten-per-frame, not
execute-time re-eval.

### Commit paths (two, and they differ)

- **Interactive `;` key**: the input buffer does **not** contain the `;` —
  handlers must accept input without a trailing `;`.
- **[`editor_feed_line()`](src/editor/input.h#L188)** (file/example loading): copies the full line
  *including* `;`, then runs the same chain.
- Enter may or may not have `;`. [`editor_load_line_to_input()`](src/editor/input.h#L182) strips the
  trailing `;`, so re-committing an existing line takes the no-semicolon
  path — handlers checking for `;` must also accept end-of-string.

The chain is consolidated in [`src/editor/commit.c`](src/editor/commit.c):
`editor_try_commit_var_statements` / `_block_structs` / `_any` /
`_var_statements_then_insert` (used by overwrite-mode Enter). Add new
handlers to the right helper, not to call sites. **Ordering is
load-bearing**: `editor_try_commit_float_decl` MUST run before
`editor_try_commit_assign_variable` or `float x;` parses as an assignment.
Handlers return 1 if consumed (success or error), 0 if no match; fallthrough
is [`repl_parse_and_normalize()`](src/repl/normalize.h#L20) → `parse_command()`.

### Float declarations (`CMD_VAR_DECLARE`)

- New decls insert at the **top of non-decl code** regardless of cursor (so
  every reference follows its declaration); editing an existing decl
  overwrites in place (carried-over names are exempt from the dup check).
- No-op in executor/flatten — registration happens at commit time via
  [`repl_eval_declare_predef_var()`](src/repl/eval.h#L309).
- [`GLCmd`](src/repl/command.h#L95) payload is a tagged union keyed on `type` (`payload.decl.*`,
  `payload.label.fmt`); other types must not read it.
- Deleting a decl range goes through [`repl_compile_delete_range()`](src/repl/compile.h#L534) which
  validates no variable is still referenced outside the range. Cut/copy/
  paste of decl rows is blocked outright.
- Export writes `// @declare` markers; import reconstructs decls bypassing
  the commit handler. `MAX_PREDEF_VARS` = 32 (1 reserved for `t`).

### User scenes & auto-promotion

Up to `MAX_USER_SCENES` = 8 slots in `g_user_scenes[]`
([`src/repl/scenes.c`](src/repl/scenes.c)); no automatic startup scene.
Editing an example auto-promotes it into a fresh slot — the hook is
[`editor_undo_push_snapshot()`](src/editor/undo.h#L123) → [`repl_promote_example_if_needed()`](src/repl/scenes.h#L46) before
every mutation. LRU eviction to `<workspace_dir>/<slug>.c` only when a
workspace is bound (else promotion is rejected with a status message).
F12 cycles examples → user scenes → back. Inline rename filters
path-unsafe chars. Workspace round-trips via `@scene-name` /
`@workspace-dir` headers.

### Example metadata & presentation reset

Examples may lead with `// @cfg <slug> = <value>` lines + an optional 5-line
`// camera` block — consumed before commit, hidden from the panel; slug list
in `docs/ADVANCED_USAGE.md` (scene-presentation only; `projection` = ortho
projection at the free camera; `view_mode` = locked 2D/3D toggle). **Every
example load resets the non-camera presentation settings (incl. `view_mode`)
to `CFG_DEFAULT_*` (in [`src/app/glr_defaults.h`](src/app/glr_defaults.h) —
the single source of truth; reuse the macros, never duplicate literals)
before applying the example's `@cfg`.** Camera is deliberately excluded
(inherited unless a `// camera` header is present).
`restore_user_scene()` restores commands + predef vars only. Touch
[`example_loader.c`](src/repl/example_loader.c) / [`export.c`](src/repl/export.c) / [`examples.c`](src/repl/examples.c) / [`glr_defaults.h`](src/app/glr_defaults.h) /
[`tests/test_repl_core_examples.c`](tests/test_repl_core_examples.c) together; `make test_repl_core_examples`
is the focused suite.

### Save / load

Export ([`src/repl/export.c`](src/repl/export.c)) writes standalone C:
header directives (`@cfg`, `@scene-name`, `@workspace-dir`), camera as raw
transforms, predefs + scratch arrays as globals, funcs, `display()` body.
Import ([`src/repl/import.c`](src/repl/import.c)) reverses it line-by-line,
feeding geometry through [`editor_feed_line()`](src/editor/input.h#L188). The `IMPORT_EXPORT_STATE`
macro block is deliberately duplicated verbatim across the two TUs.
`repl_cfg_get_int`/`_set_int` etc. go through the installed config bridge
only (`check-repl-export-via-bridge`).

### Replay

State in [`replay_state_view()`](src/subsystems/replay/replay_state.h#L126); playback/fade/input split across
`src/subsystems/replay/`. During playback the flat count is clamped to
[`replay_exec_limit()`](src/subsystems/replay/replay.h#L79); fade-batch ring renders old geometry in a blended
pass. Fade replays skip `CMD_CLEAR`, and clamps never cut below the
program's leading `glClear` (`replay_frame_setup_limit`).

### Undo

32-slot global rings (not per-scene). **Any wholesale replacement of the
live document must call [`editor_undo_clear()`](src/editor/undo.h#L145) first** or post-switch Ctrl+Z
restores the previous scene into the new one (call sites:
`glr_ctrl_reset_all`, F12 cycle, load-example/scene/workspace actions in
[`glr_actions.c`](src/app/glr_actions.c)). Push clears the redo stack; push is also the
auto-promotion hook.

### Cursor edit guides

`glr_ctrl_build_guide_snapshot()` text-parses the input line with a
**predef-only** evaluator — funcN-locals/loop vars evaluate to 0. Fix path:
the render always walks the flat program (`replay_walk_user_vertices`, with
`ctx.stop_flag` for early-out) and overrides `vertex_args`/`normal_args`
from the flat cmd at the cursor. Vertex-arg slot parsing uses
`repl_scan_next_arg_delim` (nested parens).

### Autocomplete / search

Modes: `AC_MODE_FUNC_PREFIX`, `AC_MODE_ENUM_SLOT` (active slot = top-level
commas before cursor), `AC_MODE_POINT_PARAM`. Enum modes also fire mid-line
when the cursor ends the token being completed and the tail is only
trailing args (Tab splices, keeps tail); the inline ghost stays
end-of-input-only; function-name completion/hints too. Search: Ctrl+F,
state via [`editor_state_search()`](src/editor/state.h#L405).

### Config / Tutorials menus (shared flyout engine)

`### ` rows in `g_cfg_items[]` define sections → one parent row each +
synthetic trailing **All** row, hover-opening flyouts (generic engine in
[`src/ui/app/menu_bar.c`](src/ui/app/menu_bar.c), shared with Scene +
Tutorials; wheel-scroll for tall flyouts). Parent/tag rows are **inert on
click** (hover-open only — the activate branches return 0); flyout item
clicks route via `route_submenu_item_hit` (`glr_cfg_cycle_row` for config,
`tutorial_start` for tutorials); right-press cycles backward. Tutorials
mirror the example tag system (`REPL_TUTORIAL_TAG_*` mask; `.tags` required
— metadata test fails on 0/unknown bits); optional `.subheading` renders
`### ` group headers — entries sharing one must be contiguous per tag
(enforced by `test_catalog_subheading_metadata`). Tutorial step kinds
(COMMAND / NOTE / SET / REQUIRE / REQUIRE_VAR incl. declaration steps and
setup scaffolds): see `src/repl/tutorials.{c,h}` headers and
`src/subsystems/tutorial/`.

## Key Controls

| Key | Action |
|-----|--------|
| `;` | Execute/commit current line |
| Enter | Insert new line |
| Up/Down | Navigate lines |
| Tab | Autocomplete |
| Shift+Left/Right, Shift+Home/End | Extend input-buffer selection |
| Double-click / drag / Shift+click | Word select / char select / extend (same row = chars, other row = line range) |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut / paste — input selection wins over line-range; input-text goes to a separate clipboard slot |
| Ctrl+S | Save to output.c |
| Ctrl+Z | Undo (Ctrl+Y / Ctrl+Shift+Z redo) |
| Ctrl+Shift+Y | Cycle syntax highlight (Off / On / On+Shadow) |
| Ctrl+\ | Reformat all lines |
| Ctrl+Shift+S | Split multi-var declaration at cursor |
| Ctrl+R | Start/stop replay |
| Ctrl+T / Ctrl+Shift+T | Toggle time `t` / reset `t` to 0 |
| Ctrl+G | Toggle wireframe |
| Ctrl+Shift+B | Toggle winding view |
| Ctrl+N | Cycle Depth view (Off / Linear / Scene / Split) |
| Ctrl+O | Focus origin (ease orbit target to 0,0,0) |
| Ctrl+Shift+N / O / L | Toggle normal vectors / vertex outlines / light indicators |
| Ctrl+Shift+E | Toggle Projection (Perspective / Ortho) |
| Ctrl+Shift+F | Toggle code focus |
| Ctrl+Shift+C / R / V | Reset camera / auto-rotate / View mode (2D/3D) |
| F1 | Help overlay |
| F2–F10 | Cycle bound config forward (F2 Grid, F3 Grid extent, F4 Grid brightness, F5 Backdrop, F6 Axes, F7 Vertex labels, F8 Label scope, F9 Light theme, F10 Post FX Scope); Shift+F = backward |
| Ctrl+= / Ctrl+− | Step Accum passes (1/2/4/8/12/16) |
| F11 | Export PLY (`<scene>.ply`; may be claimed by macOS Show Desktop — use File menu) |
| F12 / Shift+F12 | Next / previous example or scene |

## Supported Commands

Semantics detail: `docs/USER_GUIDE.md`. Parser-policy notes inline below.

```
glBegin(MODE), glEnd()
glVertex3f(x,y,z), glVertex2f(x,y), glNormal3f(x,y,z)
glColor3f(r,g,b), glColor4f(r,g,b,a)
glClearColor(r,g,b,a)          (channels clamped >= 0.15)
glClear(mask)                  (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)
glTranslatef/glScalef/glRotatef, glPushMatrix/glPopMatrix/glLoadIdentity
glPushAttrib(mask), glPopAttrib()  (attribute-stack save/restore; GL_*_BIT tokens)
glEnable(CAP), glDisable(CAP)  (depth/lighting/blend/cull/fog/lights 0-3,
                                GL_CLIP_PLANE0..5, line/point smooth, ...)
glFogi/glFogf/glFogfv          (fog mode/scalar/color)
glClipPlane(plane, (GLdouble[]){a,b,c,d})
glShadeModel(MODE), glPointSize(s), glLineWidth(w)
glLineStipple(factor, pattern) (pattern is plain int — no hex literals)
glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, c, l, q)
glBlendFunc(sfactor, dfactor)
glColorMaterial(face, mode)
glMaterialfv(face, pname, (GLfloat[]){...})
glLightModeli(pname, param), glFrontFace(mode), glCullFace(mode)
glDepthFunc(func), glDepthMask(GL_TRUE|GL_FALSE), glColorMask(r,g,b,a)
glutSolidTorus/Cube/Sphere/Teapot/Cone(...)
glRasterPos3f(x,y,z)
label("fmt", a, b, c, d)       (bitmap text at raster pos; REPL primitive)
for(var, start, end[, step]) { body }
func0..func9(params) { body }  (parens required; NAME(params) aliases a slot)
if(expr) { body }
:name / name:  and  goto name  (goto labels — colon syntax, not label())
// comment
float name[, name2, ...];      (declaration)
var = expr;   A[i] = expr;     (scratch arrays A/B/C, index 0..15)
```

Agent-relevant policies:

- **`glClear` is load-bearing** — nothing clears the scene rect on the
  program's behalf (identical to the exported C); deleting the line smears
  the frame. Its mask slot is the one ENUM_BITFIELD: `|`-joined tokens only,
  emitted in table order deduped. Replay fade batches skip `CMD_CLEAR`, and
  replay clamps never cut below the leading clear.
- **Flat-shorthand canonicalization**: `glMaterialfv`, `glFogfv`, and
  `glClipPlane` accept flat args (`face, pname, r, g, b, a`) and are
  rewritten to the compound-literal form. `glDepthMask`/`glColorMask`
  accept 0/1, canonicalized to `GL_TRUE`/`GL_FALSE`.
- **`glPushAttrib`/`glPopAttrib`**: mask is `|`-joined `GL_*_BIT` tokens
  (same bitfield policy as glClear); 10 supported bits — GL_CURRENT/POINT/
  LINE/POLYGON/LIGHTING/FOG/DEPTH_BUFFER/TRANSFORM/ENABLE/COLOR_BUFFER_BIT.
  GL_FOG_BIT scopes the glFog* parameters; the GL_FOG enable flag rides both
  GL_FOG_BIT and GL_ENABLE_BIT, matching real GL.
  GL_ALL_ATTRIB_BITS aliases the union of those 10 modeled groups (rather than
  storing the platform's broader GL value), canonical text retains the alias,
  and its REPL meaning grows when another supported group is added. The alias
  token itself has no
  per-bit colour; its covered setter lines still receive per-bit markers. The
  executor keeps a real GL stack only REPL_ATTRIB_STACK_CAP (8) deep — virtual
  push depth is unbounded, an orphan glPopAttrib is a silent no-op, and unmatched
  pushes unwind at frame end, so only balanced pairs reach GL. Cursor on the
  push line highlights the prior setter lines it saves (per-token colours);
  cursor on the pop highlights what its restore reverts. Unbalanced pairs
  get the red gutter warning. Opens no indentation scope (unlike
  glPushMatrix).
- **`glPointParameterfv`** is runtime-gated (`GLR_NO_POINT_PARAMETER` or a
  context without the entry point → silent no-op with `glPointSize`
  fallback); user-typed lines are still exported verbatim.
- **`label()`** format string: `%f` + `%%` only, ≤ 4 args, ≤ 64 chars; no
  `//`, parens, commas, or backslashes inside the string. The exporter
  emits a self-contained `label()` helper. Distinct from goto-label
  `:name` (CMD_GOTO_LABEL).
- **Overlay passes** replay clip/cull/`glFrontFace` state as they walk
  (`overlay_gl_track_cmd` in edit_overlays.c) so outlines/points match the
  frame; color-mask gates those walks instead of being replayed. The
  Polygon-highlight On state suspends clip planes + culling for the cursor
  highlight only.

## Math

Functions: `sin cos tan sqrt abs pow log ln min max floor ceil fmod rem
rand(seed[,iter]) rand2(seed[,iter])` — `log` is base-10, `ln` natural;
`rand` ∈ [0,1], `rand2` ∈ [-1,1], both deterministic per (seed, iter).
Constants: `PI`, `TAU`, `e`. Only `t` is predefined; others need
`float name;`. Decl-line tags: `// @tune` (tunable knob badge + exported-C
controls), `// @config` (keeps a source-assigned var bright in the panel);
both round-trip via `@declare`. Scratch arrays `A/B/C[16]`: fixed globals,
indices truncated with `(int)`, must stay 0..15. `MAX_PREDEF_VARS` = 32
(31 user slots); a full table rejects with "variable table full (max 32)".
