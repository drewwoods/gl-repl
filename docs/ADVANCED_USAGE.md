# Advanced Usage

The power-user reference: command-line flags, environment variables,
headless rendering, recording, documentation media, app packaging, mesh export,
scene-file headers, music, and diagnostics. For day-to-day features (editing,
the language, the panels), see the [User Guide](USER_GUIDE.md).

## Synopsis

```
gl-repl [file.c | workspace/] [--example name|n] [--time secs]
        [--export-ply out.ply [--export-ply-srgb]] [--noaccum]
        [--assets dir] [--no-audio] [--dump-code] [--flat-histogram]
        [--detailed-prof] [--list-examples]
```

## Options

| Option | Effect |
|---|---|
| *file*.c | Reload a previously saved session from a single file. |
| *workspace*/ | Load every `*.c` under the directory as a separate scene. |
| `--example` *name*\|*n* | Start on a built-in example (case-insensitive name, or 0-based index). |
| `--list-examples` | Print the built-in examples and exit. |
| `--time` *secs* | Initial value of the animation variable `t` (applied after any `--example` load). |
| `--export-ply` *out*.ply | Capture the scene geometry to an ASCII PLY mesh on frame 1, then exit. |
| `--export-ply-srgb` | With the above, decode vertex colors sRGB → linear for color-managed viewers. |
| `--noaccum` | Disable the accumulation buffer (anti-aliasing + motion blur). |
| `--assets` *dir* | Scan *dir* for `*.mp3` instead of `./assets`. Beats `GLR_ASSETS_DIR`. |
| `--no-audio` | Skip audio initialization entirely (also isolates startup stalls). |
| `--dump-code` | Print the loaded buffer to stdout. |
| `--flat-histogram` | Print per-function / per-line flat-command budget costs. Honors `--example`. |
| `--detailed-prof` | Add fine-grained init-trace phases (see [Diagnostics](#diagnostics)). |

## Environment variables

The project uses environment variables in three places: runtime `gl-repl`
knobs, vendored freeglut runtime knobs, and developer build/test tooling.
When both a command-line flag and an env var exist, the flag wins.

### gl-repl runtime

| Variable | Values / default | Effect |
|---|---|---|
| `GLR_ASSETS_DIR` | Directory path; default `./assets`; `--assets` wins. | Primary music directory scanned for `*.mp3`. |
| `GLR_TIME` | Seconds; default `0`; `--time` wins. | Initial animation time `t`, applied after any example/file load. |
| `GLR_EDIT_LINE` | 0-based line index; clamped. | Parks the cursor after load so cursor-bound overlays render in captures. |
| `GLR_ACCUM_PASSES` | `1`, `2`, `4`, `8`, `12`, or `16`; default app setting. | Overrides accumulation-AA sample count, mainly for capture/media generation. |
| `GLR_VIEW_TOGGLE_AT` | Comma-separated capture-clock seconds. | Toggles 2D/3D view mode at deterministic times while recording. |
| `GLR_NO_POINT_PARAMETER` | Any non-empty value. | Forces the no-`glPointParameterfv` fallback path even on capable hardware. |
| `GLR_NO_GPU_PROF` | Any non-empty value. | Disables GPU timer-query profiling; the profile panel GPU column reads `--`. |
| `GLR_DETAILED_PROF` | Any non-empty value; same as `--detailed-prof`. | Enables the finer init-trace phases and first-two-frame timing triples. |
| `GLR_AUDIO_HITCH_MS` | Milliseconds; default `50`; `0` disables. | Threshold for logging audio-worker lifecycle hitches. |
| `GLR_MINIAUDIO_LOG` | Default `warning`; `debug`/`all`/`1`, `info`, `error`, `0`/`off`/`none`. | Forwards miniaudio backend logs to stderr, timestamped when the init clock is available. Debug/info are noisy; backend underrun/starvation messages are platform-specific. |
| `HOME` | User home directory. | Used for the macOS per-user music folder and as fallback for the XDG path. |
| `XDG_DATA_HOME` | Directory path; Linux/Unix fallback `~/.local/share`. | Selects the non-macOS per-user music folder: `$XDG_DATA_HOME/gl-repl/music`. |

### Vendored freeglut runtime

These are read by the vendored freeglut tree linked into native macOS and
OSMesa builds. Some are platform-specific and only matter on the backend that
implements them.

| Variable | Values / default | Effect |
|---|---|---|
| `FREEGLUT_CAPTURE_FILE` | Filename prefix; default `freeglut`. | Prefix for `SIGUSR1` screenshots and record-mode PPM frames. |
| `FREEGLUT_CAPTURE_FRAMES` | Positive frame count. | Record mode: capture N rendered frames as numbered PPMs, then exit. |
| `DISPLAY` | X display string. | X11 display selection for Linux/X11 freeglut. |
| `GLUT_FPS` | Millisecond interval. | Enables freeglut FPS statistics printing at the requested interval. |
| `FREEGLUT_NO_XRANDR` | Any non-empty value. | Disables XRandR use in X11 game-mode display changes. |
| `FREEGLUT_NO_XF86VM` | Any non-empty value. | Disables XF86VidMode use in X11 game-mode display changes. |
| `GLUT_DIALS_SERIAL` | Serial device path. | Enables the optional freeglut dials input device on supported X11 builds. |
| `ORIENTATION` | Integer. | BlackBerry backend orientation override; irrelevant to the supported desktop builds. |
| `HOME` | User home directory. | Also used by freeglut's X11 joystick code for `.joyNrc` files. |

### Build, test, and tooling

These are make variables or script/test env vars. Pass make variables as
`make target NAME=value`; export script vars when invoking the script directly.

| Variable | Where | Effect |
|---|---|---|
| `CC` | Makefile, `scripts/check/check-c99.sh`, export tests. | Compiler command. The test runner passes it to export-compile checks as `REPL_EXPORT_CC`. |
| `CFLAGS` | Makefile. | Extra user C flags appended to the selected build mode. |
| `CPPFLAGS` | Makefile compile rules. | Extra preprocessor flags, commonly `-DUI_THEME_DEFAULT=N` or `-DGLR_AUDIO_NO_THREAD=1`. |
| `BUILD` | Makefile. | Build mode: `release`, `debug`, or `coverage`. Tests default to debug; app/bench/demo targets default to release. |
| `SAN` | Makefile. | Debug sanitizer selector used by `make debug-msan` / `make test-msan`: `address` (default ASan+UBSan) or `memory` (MSan with origin tracking; uses `build/debug-msan*`). |
| `MSAN_CC` | Makefile. | Compiler used by `make debug-msan` / `make test-msan`; defaults to `clang`, override for versioned LLVM binaries. |
| `NO_SAN`, `NOSAN` | Makefile. | Disable debug-build sanitizers. |
| `USE_GL_STUBS` | Makefile. | `1` builds against bundled no-op GL/GLU/GLUT headers for non-rendering tests. |
| `GLR_AUDIO_NO_DEVICE` | Audio runtime, `make test-msan`. | Any non-empty value initializes miniaudio without opening a host audio device; `test-msan` sets this to avoid uninstrumented system audio backends. |
| `FREEGLUT_OSMESA` | Makefile. | `1` builds vendored freeglut with the headless OSMesa backend. |
| `FREEGLUT_VENDOR` | Makefile. | `0` skips the vendored freeglut static library, used by the `make glut` fallback. |
| `APP_ICON_SVG` | Makefile `make app`. | Source SVG for the generated macOS `.icns`. |
| `TEST_JOBS` | Makefile, `scripts/run-tests.sh`. | Limits parallel test binaries; empty/`0` means unbounded parallel runner behavior. |
| `TEST_LOG_DIR` | `scripts/run-tests.sh`. | Directory for per-test logs; default `build/test-logs/run-<pid>`. |
| `NO_COLOR` | Test runners. | Disables ANSI color output. |
| `FORCE_COLOR`, `CLICOLOR_FORCE` | Test runners. | Forces ANSI color output when `NO_COLOR` is unset. |
| `REPL_EXPORT_CC` | `tests/test_repl_core_examples`. | Compiler command for exported standalone C smoke tests. |
| `REPL_EXPORT_COMPILE_CFLAGS` | `tests/test_repl_core_examples`. | Extra C flags for exported standalone C smoke tests. |
| `REPL_EXPORT_VERBOSE` | `tests/test_repl_core_examples`. | `1` prints per-example export/compile details. |
| `REPL_EXPORT_KEEP_TEMP` | `tests/test_repl_core_examples`. | `1` keeps temporary export files for inspection. |
| `C99_SRCS` | `scripts/check/check-c99.sh`. | Source list passed by `make check-c99`; script fallback is used when unset. |
| `CHECK_BASE` | Makefile `check-trailing-whitespace`. | Git ref used as the diff base; default `origin/main`. |
| `COMPAT_REF` | `scripts/build-historical.sh`. | Git ref used to read compatibility headers; default `main`. |
| `FREEGLUT_REPO` | `scripts/vendor-freeglut.sh`. | Source repo or local clone to re-vendor from. |
| `BIN` | `scripts/docs-assets.sh`. | `gl-repl` binary for doc media generation; default `build/release-osmesa/gl-repl`. |
| `OUT` | `scripts/docs-assets.sh`. | Output directory for generated documentation media. |
| `BENCH_ARGS` | Makefile bench targets. | Extra arguments passed to every benchmark binary. |
| `SANITIZER_CHECKERS` | Makefile `analyze`. | Clang static-analyzer checker list. |
| `ANALYZE_EXCLUDE` | Makefile `analyze`. | Space-separated source list excluded from static analysis. |
| `OUT_DIR`, `SRC_DIR`, `TEST_DIR`, `TOOLS_DIR`, `BENCH_DIR` | `scripts/code-smells.sh`. | Input/output directories for the code-smell audit script. |
| `JOBS` | `scripts/code-smells.sh`. | Parallelism for clangd/clang-tidy scans. |
| `CLANGD_BIN`, `CLANG_TIDY_BIN` | `scripts/code-smells.sh`. | Tool paths for clangd and clang-tidy. |
| `CLANG_TIDY_CHECKS` | `scripts/code-smells.sh`. | clang-tidy check filter. |
| `MIN_TOKENS`, `PMD_IMAGE` | `scripts/code-smells.sh`. | PMD CPD duplicate-token threshold and Docker image. |
| `LIZARD_CCN`, `LIZARD_LEN` | `scripts/code-smells.sh`. | Lizard complexity and function-length thresholds. |

## Headless rendering (OSMesa)

For CI or any machine with no display, `make ... FREEGLUT_OSMESA=1` builds
gl-repl against a software, off-screen **OSMesa** backend — it renders into
memory with no window. The real-GL tests and `--export-ply` then run headless
(`make gl-tests FREEGLUT_OSMESA=1`), and you can grab a **screenshot of a
running headless process** by sending it `SIGUSR1`:

```bash
brew install mesa mesa-glu                       # macOS deps (Linux: apt-get install libosmesa6-dev)
make gl-repl FREEGLUT_OSMESA=1
FREEGLUT_CAPTURE_FILE=/tmp/shot ./build/release-osmesa/gl-repl --example 8 --no-audio &
kill -USR1 $!                                    # writes /tmp/shot-0000.ppm
magick /tmp/shot-0000.ppm shot.png               # PPM -> PNG to view
```

The same vendored freeglut capture hook also works in native Cocoa/X11 builds:
`kill -USR1 <pid>` posts a redisplay and captures `GL_BACK` just before swap,
so the image is from the real GPU path. Wait until after `glutInit` has run
before signaling, and give an actively animating scene a few frames to settle.
`FREEGLUT_CAPTURE_FILE` controls the filename prefix in both native and OSMesa
builds.

**Starting the animation later.** Animation plays by default, with `t`
advancing a fixed `1/60 s` per rendered frame from `0`. To capture from a
later point in the timeline, set the initial `t` with `--time <secs>` (or
`GLR_TIME`):

```bash
./build/release-osmesa/gl-repl --example 2 --time 5 --no-audio &
```

**Posing the cursor.** Cursor-bound overlays (transform guides, vertex
labels) need the cursor parked on the relevant line — `GLR_EDIT_LINE=<n>`
does that at startup, as if you had arrowed to source line *n*:

```bash
GLR_EDIT_LINE=4 ./build/release-osmesa/gl-repl scene.c --no-audio &
```

### Recording GIFs / MP4s

`FREEGLUT_CAPTURE_FRAMES=N` is the backend's record mode: it captures every
rendered frame to a numbered PPM and exits after N frames.
`scripts/record-gif.sh` wraps that and assembles the output via `ffmpeg`:

```bash
make gl-repl FREEGLUT_OSMESA=1
scripts/record-gif.sh --example 2 --duration 3 --out ring        # ring.gif + ring.mp4
scripts/record-gif.sh --example 8 --duration 4 --fps 30 --scale 600 --time 5 --out torus
```

`--duration <secs>` × `--fps` sets the frame count; `--scale <w>` downsizes;
`--time <t0>` starts later in the animation. The clock advances `1/60 s` per
frame, so playback is `~fps/60`× natural speed — use `--fps 60` for
real-time. Needs `ffmpeg`. (`scripts/record-gif.sh --help` for all flags.)

Native builds support the same `FREEGLUT_CAPTURE_FRAMES=N` contract too: they
open a real window, write N real-GPU frames, and exit.

`GLR_VIEW_TOGGLE_AT=0.5,2.0` is a recording affordance for the 2D/3D swatch in
the menu bar. It toggles view mode as the fixed capture clock crosses each
listed second and advances the transition one fixed tick per captured frame, so
UI transition clips are deterministic.

### Documentation media

The screenshots and GIFs in the README and User Guide are generated by
`scripts/docs-assets.sh`:

```bash
scripts/docs-assets.sh --list       # asset names
scripts/docs-assets.sh -j 4         # regenerate all, four workers
scripts/docs-assets.sh hero replay  # regenerate selected assets
```

Each asset is a staged snippet scene with optional `@cfg` headers, a `// camera`
block, and sometimes `GLR_EDIT_LINE` to pose cursor-bound overlays. Captures use
record mode and keep the last frame, so theme fades and other frame-based
settling are deterministic. Scene-only shots render at `--window 2400x1600` and
downscale 50% for 4x supersampling; full-UI and hairline grid/axes shots stay
1x and use `GLR_ACCUM_PASSES=16` so bitmap text and 1px lines keep full weight.

### Re-pinning vendored freeglut

The in-tree vendored freeglut already carries the OSMesa backend and the native
Cocoa/X11 capture hooks. No re-vendor is needed for `FREEGLUT_OSMESA=1`,
`SIGUSR1` screenshots, or `FREEGLUT_CAPTURE_FRAMES` recording. Re-pin only when
you want a newer fork commit:

```bash
FREEGLUT_REPO=https://github.com/drewwoods/freeglut \
  scripts/vendor-freeglut.sh capture-windowed-backends
make freeglut-clean
```

The resolved source/ref/SHA are pinned in `third_party/freeglut/VENDORED.txt`.
The current capture branch is stacked on the OSMesa backend branch, so one
vendored tree serves both native and headless builds. See *Headless Rendering &
Screenshots (OSMesa)* in
[`ARCHITECTURE.md`](ARCHITECTURE.md) for the full design. If you are changing
the vendored freeglut backend itself, the lower-level backend notes live in
`docs/plans/external/freeglut-osmesa-backend.md`.

## macOS app bundle

`make app` packages the binary as `gl-repl.app` with a Finder/Dock icon and a
bundled sample track:

```bash
make app
open gl-repl.app
```

The icon source is `packaging/macos/gl-repl-soft-cube.svg` by default
(`APP_ICON_SVG=...` selects another SVG). Building the `.icns` requires
`rsvg-convert` from `librsvg`; `iconutil` is provided by macOS. The bundle
copies only `assets/sample.mp3` into `Contents/Resources/assets/`; additional
tracks belong in the per-user music folder or a directory passed with
`--assets`. Generated bundle products (`gl-repl.app/`, `gl-repl.icns`,
`gl-repl.iconset/`) are ignored by git.

## Mesh export (PLY)

Press **F11** (or **File → Export .ply**) to capture the current scene as an
ASCII PLY mesh, named after the active scene (like Save Scene). The geometry
— your `glVertex` polygons, GLU-tessellated shapes, and the GLUT solids
(teapot/sphere/cube/cone/torus) — is captured through a single
`glRenderMode(GL_FEEDBACK)` pass, so everything on screen exports the same
way. Authored per-vertex normals are preserved; the rest are smoothly
synthesized.

Headless / scripted capture:

```bash
./gl-repl --example 8 --export-ply out.ply                     # capture on frame 1, then exit
./gl-repl --example 8 --export-ply out.ply --export-ply-srgb   # decode colors sRGB -> linear
```

Line primitives (`glBegin(GL_LINES/LINE_STRIP/LINE_LOOP)`) export as a PLY
`edge` element. See *Mesh Export (PLY via GL_FEEDBACK)* in
[`ARCHITECTURE.md`](ARCHITECTURE.md) for the capture/encode design.

## Scene-file headers

Saved scenes are standalone C files, but their leading comments carry REPL
metadata that round-trips on reload:

| Directive | Meaning |
|---|---|
| `// @scene-name <name>` | Names the scene slot the file loads into. |
| `// @workspace-dir <path>` | Re-binds the workspace directory. |
| `// @cfg <slug> = <value>` | Applies a scene-presentation setting (see the slug table below). |
| `// @declare name` | Reconstructs a `float name;` declaration on import. |
| `// @tune` | Marks a variable as a tunable knob in the exported program — see [User Guide → Tunable Variables](USER_GUIDE.md#tunable-variables--tune). |
| `// camera` block | A 5-line camera preset applied on load. |

Built-in examples use the same `@cfg` + `// camera` headers, so a saved
scene and an example are the same file format. Only *leading* directives
are metadata; the same text later in the file is an ordinary comment.

### `@cfg` scene-presentation slugs

A scene file (and a built-in example) carries one `// @cfg <slug> = <value>`
line per scene-presentation setting it overrides. The slug is the Config-menu
row label, lowercased with spaces → underscores (`Grid extent` → `grid_extent`).
Settings outside this scene subset (accumulation/MSAA, profiling panels, code
layout, syntax theme) are *not* scene metadata and don't round-trip through a
scene file.

**Enum slugs** take a symbolic value name; the bare integer index still loads
(legacy files), but the symbolic form is canonical on save and self-documents
which choice it selects:

| Slug | Symbolic values |
|---|---|
| `grid` | `GRID_THEME_OFF` `_CLASSIC` `_FOG` `_TRON` `_EMBER` `_FAINT` `_FOCUS` `_OCEAN` `_XZRULER` `_PLANES` `_RADAR` `_AURORA` `_SYNTHWAVE` `_FROZEN` `_SOIL` `_STARCHART` |
| `axes` | `AXES_THEME_OFF` `_CLASSIC` `_PULSE` `_NEON` `_COMPASS` `_GIZMO` `_RULER` |
| `backdrop` | `RENDER3D_BACKDROP_OFF` `_CITYSCAPE` `_STARS` `_CITY_AND_STARS` `_SUNSET` `_AURORA` `_NEBULA` `_POLAR_DAY` `_SNOWFALL` `_POLAR_DAY_SNOW` |
| `light_theme` | `LIGHT_THEME_DEFAULT` `_HEADLIGHT` `_SOLAR` `_STUDIO` `_NEON` |
| `grid_extent` | `GRID_EXTENT_CLOSE` `_MID` `_FAR` |
| `grid_major` | `GRID_MAJOR_1` `_2` `_5` `_10` (the major-tick spacing) |
| `view_mode` | `RENDER3D_VIEW_3D` `RENDER3D_VIEW_2D` (perspective vs. 2D ortho) |

**Integer slugs** carry a plain index: the toggles `wireframe`,
`normal_vectors`, `vertex_outlines`, `vertex_points`, `light_indicators`,
`camera_rotate`, `variable_panel` are `0`/`1`; `vertex_labels` and
`xform_guides` are small multi-state cycles saved as their index.

```c
// @cfg grid = GRID_THEME_OCEAN
// @cfg grid_extent = GRID_EXTENT_MID
// @cfg view_mode = RENDER3D_VIEW_2D
// @cfg light_indicators = 1
```

### `@cfg` backdrop/grid pairing

Some backdrops own a companion grid. The pairing defaults live in
[`src/app/glr_defaults.h`](../src/app/glr_defaults.h). Pairing is configured
in source code, not in an `.ini` file or saved-scene header.

To configure pairings, edit the defaults macro:

```c
#define GLR_BACKDROP_GRID_PAIR_DEFAULTS {                                  \
    { .backdrop = RENDER3D_BACKDROP_AURORA,         .grid = GRID_THEME_AURORA }, \
    { .backdrop = RENDER3D_BACKDROP_SUNSET,         .grid = GRID_THEME_SYNTHWAVE }, \
    { .backdrop = RENDER3D_BACKDROP_POLAR_DAY_SNOW, .grid = GRID_THEME_FROZEN }, \
    { .backdrop = RENDER3D_BACKDROP_NEBULA,         .grid = GRID_THEME_STARCHART }, \
}
```

Add one row per forced pair, using enum names from
[`src/render3d/themes.h`](../src/render3d/themes.h). For example, if a future
backdrop should own `GRID_THEME_NEON`, add another row such as
`{ RENDER3D_BACKDROP_..., GRID_THEME_NEON }`. No saved-scene schema changes
are needed: [`glr_config.c`](../src/app/glr_config.c) consumes the defaults macro as runtime config policy,
while `@cfg` continues to store ordinary `grid` and `backdrop` enum values.

Paired grid targets are valid enum/config values, but they are hidden from
direct Grid cycling. In practice:

- Selecting `RENDER3D_BACKDROP_NEBULA` forces `GRID_THEME_STARCHART`.
- Entering a paired backdrop saves the current grid; leaving paired backdrops
  restores that saved grid.
- While Nebula is active, user Grid cycling cannot switch away from Star Chart
  and posts a status warning.
- `GRID_THEME_STARCHART` can still appear in saved scenes and built-in examples.
- If both `grid` and `backdrop` appear in a header, Nebula forces Star Chart
  regardless of line order.

```c
// @cfg backdrop = RENDER3D_BACKDROP_NEBULA
// @cfg grid = GRID_THEME_STARCHART
```

The symbolic names come straight from the enums in [`src/render3d/themes.h`](../src/render3d/themes.h) /
[`src/render3d/view_mode.h`](../src/render3d/view_mode.h) via X-macros, so reordering an enum can't silently
shift which value a catalog literal selects. A typo'd or out-of-range
symbol is dropped with a `repl_cfg: dropping '…' (unknown symbolic value)`
note on stderr rather than landing at index 0.

## Music & assets

gl-repl plays background music: any `*.mp3` it finds at startup, in filename
order, from three combined sources:

1. **`./assets`** next to where you run it — overridden by `--assets <dir>`
   or `GLR_ASSETS_DIR` (flag beats env).
2. **Bundled with the app** — the macOS `gl-repl.app` (from `make app`)
   ships a sample track inside the bundle.
3. **Your music folder** — `~/Library/Application Support/gl-repl/Music` on
   macOS, `$XDG_DATA_HOME/gl-repl/music` on Linux. Created on first run;
   drop `.mp3`s there and they join the playlist.

The repository ships only one sample track to stay lightweight. The optional
**music pack** is attached to the GitHub release tagged `assets-v1` — fetch
it into your per-user music folder with:

```bash
scripts/fetch-music.sh             # downloads the pack into the user music folder
scripts/fetch-music.sh --dir ./assets   # or anywhere else
```

`./gl-repl --no-audio` starts silent. See *Music Asset Resolution* in
[`ARCHITECTURE.md`](ARCHITECTURE.md) for precedence details.

## Diagnostics

Stderr diagnostics help locate startup stalls, audio-worker hitches, and
backend audio-device issues:

- **Init trace** — `main()` logs a wall-clock line per startup phase
  (`[init +N.NNNs] <phase>`). A large gap names the slow phase;
  `--no-audio` isolates whether opening the OS audio device is the cause.
  `--detailed-prof` / `GLR_DETAILED_PROF=1` adds finer phases, including
  per-frame timing triples for the first two frames.
- **Audio-worker hitch detector** — any blocking audio lifecycle op (track
  load, stream teardown, advance) over the threshold logs
  `[init +N.NNNs] repl_audio: worker hitch: <op> took N ms`. Tune with
  `GLR_AUDIO_HITCH_MS` (default 50; `0` disables).
- **Miniaudio backend log** — `GLR_MINIAUDIO_LOG=debug` forwards miniaudio
  device/resource-manager logs to stderr with the same `[init +N.NNNs]`
  prefix when available. Leave it at the default warning/error level for
  normal runs; debug/info output is noisy and underrun messages depend on
  the active miniaudio backend.
- **GPU timer-query override** — `GLR_NO_GPU_PROF=1` leaves the CPU profile
  data on but disables GPU timings, useful when a driver advertises timer
  queries unreliably.

In-app, the CPU profiler overlay shows per-frame section timings and the
memory panel shows RSS history — see
[User Guide → Profiling & Diagnostics](USER_GUIDE.md#profiling--diagnostics).

## Keyboard map tooling

Shortcut bindings live in [`keymap.h`](../keymap.h) as `GLR_*` `(key, modifiers)` pairs.
Use `scripts/keymap.sh` when changing or auditing them:

```bash
scripts/keymap.sh check   # fail on duplicate (key, modifiers) bindings
scripts/keymap.sh list    # show bound, reserved, and available slots
```

`list` reports the current bindings, the byte-level reserved control aliases,
and the unbound Ctrl / Ctrl+Shift / F-key slots. `Ctrl+H`, `Ctrl+I`,
`Ctrl+J`, and `Ctrl+M` are reserved, including their Ctrl+Shift forms, because
the normal keyboard callback receives them as Backspace, Tab, LF/Enter, and
CR/Enter rather than distinct app shortcuts.

## Files

```
output.c        default save target (a standalone, compilable C file)
<workspace>/    a directory of *.c scenes, round-tripped via @-headers
audio_state.ini persisted audio state (track, position, volume)
~/Library/Application Support/gl-repl/Music    per-user music folder (macOS)
$XDG_DATA_HOME/gl-repl/music                   per-user music folder (Linux)
```

---

<sub>See also: [User Guide](USER_GUIDE.md) · [Contributing](CONTRIBUTING.md) · [Architecture](ARCHITECTURE.md)</sub>
