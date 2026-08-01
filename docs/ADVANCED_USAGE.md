# Advanced Usage (Draft)

The power-user reference: command-line flags, environment variables,
headless rendering, recording, documentation media, app packaging, mesh export,
scene-file headers, music, and diagnostics. For day-to-day features (editing,
the language, the panels), see the [User Guide](USER_GUIDE.md).

## Synopsis

```
gl-repl [file.c | workspace/ | -] [--example name|n] [--tour name|n]
        [--time secs] [--window WxH]
        [--export-ply out.ply [--export-ply-srgb]] [--accum | --no-accum]
        [--assets dir] [--examples-dir dir] [--no-audio]
        [--dump-code] [--dump-flat] [--flat-histogram]
        [--dump-state-layout] [--detailed-prof]
        [--list-examples] [--list-tours]
```

## Options

| Option | Effect |
|---|---|
| *file*.c | Reload a previously saved session from a single file. |
| *workspace*/ | Load the ordered scenes named by the directory's required `.glr-workspace` manifest. Manifest-less directories are rejected. |
| `-` | Read a saved session or REPL snippet from standard input. Input is buffered to an anonymous temporary file so the normal multi-pass importer is preserved. |
| `-h`, `--help` | Print the full flag + environment reference and exit. |
| `--example` *name*\|*n* | Start on a built-in example (case-insensitive name, or 1-based index). |
| `--list-examples` | Print the built-in examples and exit. |
| `--examples-dir` *dir* | Load `catalog.ini` + `scenes/` from *dir* at runtime instead of the compiled-in examples. |
| `--tour` *name*\|*n* | Start and play a built-in guided tour on launch (case-insensitive name, or 1-based index). Space play/pause, arrows step, Esc exit. |
| `--list-tours` | Print the built-in guided tours and exit. |
| `--time` *secs* | Initial value of the animation variable `t` (applied after any `--example` load). Overrides `GLR_TIME`. |
| `--window` *WxH* | Initial window size (default 1200x800). |
| `--export-ply` *out*.ply | Capture the scene geometry to an ASCII PLY mesh on frame 1, then exit. Needs a display. |
| `--export-ply-srgb` | With the above, decode vertex colors sRGB → linear for color-managed viewers. |
| `--no-accum` | Disable the accumulation buffer (anti-aliasing + motion blur). |
| `--accum` | Force the accumulation buffer on. Without either flag the feature auto-disables on renderers that emulate `glAccum` on the CPU — anything reporting Mesa / llvmpipe / softpipe / swrast, where each pass costs a full scene re-render *plus* a host-side read-add-write of the color buffer. Hardware renderers and the web build (gl4es accumulates in a real FBO — see [`packaging/web/patches/gl4es-accum-fbo.patch`](../packaging/web/patches/gl4es-accum-fbo.patch)) stay on by default. Needed for accum-AA captures under the headless OSMesa build, which is Mesa by construction. |
| `--assets` *dir* | Scan *dir* for `*.mp3` instead of `./assets`. Beats `GLR_ASSETS_DIR`. |
| `--no-audio` | Skip audio initialization entirely (also isolates startup stalls). |
| `--dump-code` | Load the session and print the editor buffer to stdout, then exit. |
| `--dump-flat` | Load the session and print the flattened command list, then exit. |
| `--flat-histogram` | Print per-function / per-line flat-command budget costs. Honors `--example`. |
| `--dump-state-layout` | Print the `ReplRuntimeState` field layout and exit. |
| `--detailed-prof` | Add fine-grained init-trace phases (see [Diagnostics](#diagnostics)). Also via `GLR_DETAILED_PROF`. |

Unrecognized flags are not rejected — the first non-option argument is taken as
the input file, so a typo lands as `Error: cannot open --typo`. `--help` is the
authoritative list; the completions in
[Shell completion](#shell-completion) offer exactly these flags.

## Shell completion

`scripts/completions/` carries bash and zsh completions for `gl-repl` and for
`scripts/docs-assets.sh`. Nothing needs generating or regenerating: the
candidate lists come from the commands' own `--list-examples`, `--list-tours`
and `--list` output, so the example/tour catalogs and the doc-asset arrays stay
the single source of truth. All three list paths print and exit before any GL,
window, audio, or `magick`/`ffmpeg` work, so completion is instant and works on
a headless machine with no capture tools installed.

To add both zsh completions to `~/.zshrc` (without duplicating them on later
runs):

```bash
make install-completions
```

The generated block uses absolute paths to this checkout. Set `ZSHRC=path` to
install into a different startup file.

```bash
# bash — source what you want (a shell rc, or per-session)
source scripts/completions/gl-repl.bash
source scripts/completions/docs-assets.bash
```

```zsh
# zsh — either put the directory on fpath before compinit…
fpath=("$PWD/scripts/completions" $fpath)
autoload -Uz compinit && compinit

# …or just source the files any time after compinit has run
source scripts/completions/_gl-repl
source scripts/completions/_docs-assets.sh
```

`gl-repl` completes every flag, example and tour names for `--example` /
`--tour`, directories for `--examples-dir` / `--assets`, `*.ply` for
`--export-ply`, common sizes for `--window`, and `*.c` scenes or a workspace
directory positionally. Catalog names contain spaces and parentheses; they come
back correctly escaped (or bare inside an open quote), so
`--example Par<Tab>` yields a single usable argument.

`docs-assets.sh` completes its flags and asset names, narrows to the category
already on the line (`--gifs sc-<Tab>` offers only GIF assets), drops names
already typed, and offers nothing for the `--jobs` count.

The completions are registered on the command's basename, so `gl-repl`,
`./gl-repl` and `build/release/gl-repl` all complete.

## Environment variables

The project uses environment variables in three places: runtime `gl-repl`
knobs, vendored freeglut runtime knobs, and developer build/test tooling.
When both a command-line flag and an env var exist, the flag wins.

### gl-repl runtime

| Variable | Values / default | Effect |
|---|---|---|
| `GLR_ASSETS_DIR` | Directory path; default `./assets`; `--assets` wins. | Primary music directory scanned for `*.mp3`. |
| `GLR_TIME` | Seconds; default `0`; `--time` wins. | Initial animation time `t`, applied after any example/file load. |
| `GLR_EDIT_LINE` | 0-based line index; clamped. | Parks the cursor after load and scrolls it into view so cursor-bound overlays render in captures. |
| `GLR_TYPE_KEYS` | Text fed through the keyboard dispatch after load. | Poses mid-typing states (partial-input vertex guides, autocomplete ghost/popup) for captures. |
| `GLR_OPEN_COLOR_PICKER` | 0-based line index of an editable color command. | Opens the floating color picker on that line for captures — it otherwise needs a swatch click. |
| `GLR_OPEN_GL_STATE` | 0-based line index of a blank source row. | Routes a synthetic right-click to that visible row and opens the GL-state popup; retries after `GLR_EDIT_LINE` follow-scroll. |
| `GLR_OPEN_ASSIGN_PLOT` | Comma-separated 0-based line indices of assignment rows. | Routes a synthetic right-click to the first (visible) row and opens its value plot; same retry-until-on-screen behavior. Further rows are added as extra series, up to four, as Shift+right-click would. No-op if a row is not a `var = expr;` / `A[i] = expr;`, or if its X axis cannot match the first row's. |
| `GLR_ASSIGN_PLOT_EXPANDED` | Any non-empty value; default off. | Opens the assignment plot in its doubled size (the `2x` zoom chip, otherwise mouse-only). |
| `GLR_ASSIGN_PLOT_LOG` | Any non-empty value; default off. | Requests the log₁₀ Y axis (the `log` chip). Ignored only when the trace is pinned at exactly zero; signed data lands on the symmetric log axis. |
| `GLR_ACCUM_PASSES` | `1`, `2`, `4`, `8`, `12`, or `16`; default app setting. | Overrides accumulation-AA sample count, mainly for capture/media generation. |
| `GLR_TICK_PER_FRAME` | Any non-empty value; default off. | Advances the complete fixed-dt simulation once per rendered frame for deterministic offline capture. |
| `GLR_VIEW_TOGGLE_AT` | Comma-separated capture-clock seconds. | Toggles 2D/3D view mode at deterministic times while recording. |
| `GLR_POINTER_SCRIPT` | Path to a pointer script; implies `GLR_TICK_PER_FRAME`. | Drives scripted synthetic mouse/keyboard input (menu glides, clicks, highlight rings) with a visible cursor overlay — the video-capture hook behind `scripts/record-video.sh`. Grammar in [`src/app/glr_pointer_script.h`](../src/app/glr_pointer_script.h). |
| `GLR_NO_SPLASH` | Any non-empty value. | Skips the startup splash banner (captures that should not open on the splash band). |
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
| `FREEGLUT_CAPTURE_STREAM` | Path or `-` (stdout). | Redirects captured frames into one concatenated PPM stream instead of numbered files. Point it at a fifo and an encoder (`ffmpeg -f image2pipe -vcodec ppm`) consumes frames as they render — no on-disk framebuffer dumps. |
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
| `BUILD` | Makefile. | Build mode: `release`, `debug`, `coverage`, or `quick` (`-O0 -g0`, no sanitizers). Tests default to debug; app/bench/demo targets default to release. |
| `DEBUG_INFO_CFLAGS` | Makefile. | Debug-info flags. Defaults to `-ggdb -g3` everywhere except the `WEB=1` release link, which defaults to `-g0`: emcc keeps DWARF inside the `.wasm` and drops to limited binaryen optimizations when it is present (5.4 MB vs 1.8 MB `index.wasm`, with no measurable runtime difference). Override to force either way, e.g. `make web DEBUG_INFO_CFLAGS=-g2` for named frames in a browser profile. |
| `SAN` | Makefile. | Debug sanitizer selector used by `make debug-msan` / `make test-msan`: `address` (default ASan+UBSan) or `memory` (MSan with origin tracking; uses `build/debug-msan*`). |
| `MSAN_CC` | Makefile. | Compiler used by `make debug-msan` / `make test-msan`; defaults to `clang`, override for versioned LLVM binaries. |
| `NO_SAN`, `NOSAN` | Makefile. | Disable debug-build sanitizers. |
| `USE_GL_STUBS` | Makefile. | `1` builds against bundled no-op GL/GLU/GLUT headers for non-rendering tests. |
| `GLR_AUDIO_NO_DEVICE` | Audio runtime, `make test-msan`. | Any non-empty value initializes miniaudio without opening a host audio device; `test-msan` sets this to avoid uninstrumented system audio backends. |
| `FREEGLUT_OSMESA` | Makefile. | `1` builds vendored freeglut with the headless OSMesa backend. |
| `FREEGLUT_VENDOR` | Makefile. | `0` skips the vendored freeglut static library, used by the `make glut` fallback. |
| `APP_ICON_SVG` | Makefile `make app`. | Source SVG for the generated macOS `.icns`. |
| `TEST_JOBS` | Makefile, `scripts/run-tests.sh`. | Limits parallel test binaries; empty/`0` means unbounded parallel runner behavior. |
| `TEST_ARGS` | Makefile `run-test-*` targets. | Extra arguments passed to one test binary, e.g. `make run-test-repl-core-examples TEST_ARGS='--show-mismatch'`. |
| `TEST_LOG_DIR` | `scripts/run-tests.sh`. | Directory for per-test logs; default `build/test-logs/run-<pid>`. |
| `NO_COLOR` | Test runners. | Disables ANSI color output. |
| `FORCE_COLOR`, `CLICOLOR_FORCE` | Test runners. | Forces ANSI color output when `NO_COLOR` is unset. |
| `GLR_DEMO_TEXT` | `editor_demo`. | Path to a text file typed into the document at startup through the demo's own dispatcher; stages a populated editor for a headless capture. |
| `GLR_DEMO_OPEN_PICKER` | `color_picker_demo`. | 0-based shape index; opens the floating picker on it at startup (it otherwise only opens on a click). |
| `GLR_DEMO_ALLOC` | `memprof_demo`. | Number of ~4 MB blocks to allocate, standing in for that many presses of `a`. Fires after memprof's first ring sample (it defers its baseline to that moment), and keeps the blocks page-resident so the delta survives to the capture. |
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
FREEGLUT_CAPTURE_FILE=/tmp/shot ./build/release-osmesa/gl-repl --example "Parametric torus (nested for)" --no-audio &
kill -USR1 $!                                    # writes /tmp/shot-0000.ppm
magick /tmp/shot-0000.ppm shot.png               # PPM -> PNG to view
```

The same vendored freeglut capture hook also works in native Cocoa/X11 builds:
`kill -USR1 <pid>` posts a redisplay and captures `GL_BACK` just before swap,
so the image is from the real GPU path. Wait until after `glutInit` has run
before signaling, and give an actively animating scene a few frames to settle.
`FREEGLUT_CAPTURE_FILE` controls the filename prefix in both native and OSMesa
builds.

**Starting the animation later.** Animation plays by default, with each
simulation tick advancing `t` by a fixed `1/60 s` from `0`. Recording scripts
set `GLR_TICK_PER_FRAME`, making that exactly one tick per rendered frame. To
capture from a later point in the timeline, set the initial `t` with
`--time <secs>` (or `GLR_TIME`):

```bash
./build/release-osmesa/gl-repl --example "Animated ring (for + t)" --time 5 --no-audio &
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
scripts/record-gif.sh --example "Animated ring (for + t)" --duration 3 --out ring        # ring.gif + ring.mp4
scripts/record-gif.sh --example "Parametric torus (nested for)" --duration 4 --fps 30 --scale 600 --time 5 --out torus
```

`--duration <secs>` × `--fps` sets the frame count; `--scale <w>` downsizes;
`--time <t0>` starts later in the animation. The clock advances `1/60 s` per
frame, so playback is `~fps/60`× natural speed — use `--fps 60` for
real-time. Needs `ffmpeg`. (`scripts/record-gif.sh --help` for all flags.)

Native builds support the same `FREEGLUT_CAPTURE_FRAMES=N` contract too: they
open a real window, write N real-GPU frames, and exit.

`GLR_VIEW_TOGGLE_AT=0.5,2.0` is a recording affordance for the 2D/3D swatch in
the menu bar. It toggles view mode as the fixed capture clock crosses each
listed second and implicitly enables `GLR_TICK_PER_FRAME`, so UI transition
clips are deterministic.

### Recording videos with scripted interaction

`scripts/record-video.sh` records a session to an MP4 (H.264 + AAC music),
optionally driven by a **pointer script** (`GLR_POINTER_SCRIPT`) — synthetic
mouse and keyboard events with a visible cursor overlay, click ripples, and
highlight rings. Scripts can use absolute rendered-frame timestamps for a
fixed recording schedule, or omit timestamps to start each step when the
previous one completes. That is how the
User Guide's menu-tour video is made:

```bash
scripts/record-video.sh --script scripts/video/menu-tour.pointer \
    --example "gl-repl logo" --duration 36 --out menu-tour     # -> menu-tour.mp4
```

- **No intermediate frame dumps.** Frames stream from the app straight into
  `ffmpeg` through a fifo (`FREEGLUT_CAPTURE_STREAM`), so encoding starts on
  frame 1 and nothing raw lands on disk. Recorded videos stay out of the
  repo (gitignored under `docs/images/`).
- **Native backend by default** (same rationale as `docs-assets.sh`): real
  GPU colors and MSAA; a window opens for the duration of the recording.
  Point `--bin` at an OSMesa build for fully headless capture.
- **Music** is muxed at encode time (`--music`, `--music-seek` to scrub into
  the track, fade-out over the last 1.5 s; default `assets/sample.mp3`). A
  pointer script can pin its own soundtrack with `# music:` / `# music-seek:`
  header comments.
- **Colorspace** is declared at encode time — the captured frames are
  display-referred RGB (the app is not color-managed). `--colorspace srgb`
  (default) tags standard sRGB; `--colorspace p3` tags Display P3 (P3-D65
  primaries + sRGB transfer), reproducing how the app looks on a wide-gamut
  Mac display. Either way the RGB→YCbCr conversion uses the BT.709 matrix
  explicitly (ffmpeg's RGB default is BT.601, which players mis-assume for
  HD), and the tags are set on the frames via `setparams` — on ffmpeg ≥ 7
  the encoder takes color properties from frames, so the old
  `-color_primaries`/`-color_trc` encoder flags no longer stick.
- **Scripted input** covers the whole input surface: `move`/`glide` pointer
  motion (hover opens menu flyouts exactly as a real mouse), `click` /
  `rightclick` / `down` / `up`, `wheel`, `ring` (pulsing highlight around a
  UI element), `key` (typed text, incl. `\cX` control bytes like Ctrl+T;
  `key@<cps>` paces the text at N characters per second so it types out like
  a person instead of appearing at once) and
  `skey` (F-keys, arrows) — so whole typed demos are re-recordable from a
  script. Untimed scripts are completion-driven; `pause <seconds>` adds an
  intentional delay before the next step. Grammar and examples:
  [`src/app/glr_pointer_script.h`](../src/app/glr_pointer_script.h).
- A malformed script line fails the run with a file:line message rather than
  silently recording the wrong interaction.

### Documentation media

Raster doc media (`docs/images/**` `.png`/`.gif`/`.jpg`) lives in **Git LFS**
(see `.gitattributes`; SVGs stay in normal git). Without `git-lfs` installed
those paths check out as ~130-byte pointer files, and a regeneration run would
commit real binaries over the pointers. Install it before touching them:

```bash
# macOS: brew install git-lfs
# Linux: sudo apt install git-lfs
git lfs install        # one-time, per user
git lfs pull           # backfill an existing clone
```

The screenshots and GIFs in the README and User Guide are generated by
`scripts/docs-assets.sh`:

```bash
scripts/docs-assets.sh --list       # asset names
scripts/docs-assets.sh --gifs       # regenerate every GIF
scripts/docs-assets.sh --pngs       # regenerate every PNG
scripts/docs-assets.sh --demos      # regenerate the standalone-demo stills
scripts/docs-assets.sh -j 4         # regenerate all, four workers
scripts/docs-assets.sh hero replay  # regenerate selected assets
scripts/docs-assets.sh --help       # full CLI reference
```

The `demo-*` assets are the screenshots in [`tools/README.md`](../tools/README.md),
captured from the standalone demo binaries rather than from `gl-repl` — they are
their own category so `--pngs` still needs only `make gl-repl`. Build the demos
first (`make render3d-demo repl-live-demo editor-demo variable-panel-demo
color-picker-demo cpuprof-demo memprof-demo`); `DEMO_BIN_DIR` overrides where
the script looks for them. Demos with nothing on screen cold are staged through
the `GLR_DEMO_*` hooks above.

Tab-completion for the flags and the ~60 asset names ships in
`scripts/completions/` — see [Shell completion](#shell-completion).

GIF generation uses `GLR_TICK_PER_FRAME`, so machine load changes generation
time without dropping animation states. The `profile-panels` screenshot still
reflects live rendering performance and should be regenerated on an otherwise
unloaded machine.

Each asset is a staged snippet scene with optional `@cfg` headers, a `// camera`
block, and sometimes `GLR_EDIT_LINE` to pose cursor-bound overlays. Captures use
record mode and keep the last frame, so theme fades and other frame-based
settling are deterministic.

Every gl-repl asset renders into the same 1x `--window 1200x800`. There is no
oversize-and-downscale supersampling path: a native window is clamped to the
visible screen, so an over-size request comes back the wrong size. Antialiasing
comes from the context's MSAA, and stills raise `GLR_ACCUM_PASSES` on top of it
— the scene is re-rendered per pass with a sub-pixel frustum jitter and
accumulated, which the 2D UI draws *outside* of, so bitmap text stays crisp
while 3D edges and 1px grid/axes hairlines smooth out at full weight. GIFs keep
stock MSAA only; raising the pass count would multiply the cost of every
recorded frame. The `demo-*` stills take none of this: each demo binary opens at
its own compiled-in size and takes no gl-repl flags.

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

Use **File → Export .ply** to capture the current scene as an
ASCII PLY mesh, named after the active scene (like Save Scene). The geometry
— your `glVertex` polygons, GLU-tessellated shapes, and the GLUT solids
(teapot/sphere/cube/cone/torus) — is captured through a single
`glRenderMode(GL_FEEDBACK)` pass, so everything on screen exports the same
way. Authored per-vertex normals are preserved; the rest are smoothly
synthesized.

Headless / scripted capture:

```bash
./gl-repl --example "Parametric torus (nested for)" --export-ply out.ply                     # capture on frame 1, then exit
./gl-repl --example "Parametric torus (nested for)" --export-ply out.ply --export-ply-srgb   # decode colors sRGB -> linear
```

Point and line primitives are preserved as PLY loose vertices / `edge`
elements. Because mesh-only viewers such as Xcode ignore those records, the
app export also adds small octahedron / capped-tube triangle proxies whose
radius scales with the scene bounds. See *Mesh Export (PLY via GL_FEEDBACK)* in
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
| `// @config` | Marks an assigned variable as config so the variable panel doesn't dim it (bounds-keeping writes like clamps) — see [Config Variables](#config-variables--config) below. |
| `// camera` block | A 5-line camera preset applied on load. |

Built-in examples use the same `@cfg` + `// camera` headers, so a saved
scene and an example are the same file format. Only *leading* directives
are metadata; the same text later in the file is an ordinary comment.

### Authoring an example catalog

**File → Save Scene as .glr** writes the same file shape the built-in examples
ship in — `@cfg` + `// camera` headers and the commands verbatim, with no C
scaffold (see [User Guide → Scene export](USER_GUIDE.md#scene-export-glr) for
what the format drops). To turn one into an example:

1. Drop the `.glr` file into `examples/scenes/`.
2. Add a section for it to `examples/catalog.ini`.

It then loads as a built-in. Runtime example directories take the same layout
as-is, so `--examples-dir <dir>` will load `<dir>/catalog.ini` +
`<dir>/scenes/` without a rebuild:

```bash
./gl-repl --examples-dir examples --example <name-or-idx>
```

Catalog entry fields, size budgets, and the other files an example must be
edited alongside are covered by the `gl-repl-scene-authoring` skill.

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
| `grid_brightness` | `GRID_BRIGHTNESS_DIM` `_NORMAL` `_BRIGHT` `_BOLD` |
| `view_mode` | `RENDER3D_VIEW_3D` `RENDER3D_VIEW_2D` (perspective vs. 2D ortho) |
| `vertex_labels` | `OVERLAY_VERTEX_LABEL_OFF` `_INDEX` `_INDEX_POS` `_INDEX_WORLD` `_INDEX_WORLD_FINE` |
| `vertex_outline_style` | `VERTEX_OUTLINE_STYLE_DEFAULT` `_BOLD` `_INVERTED` `_BOLD_INVERTED` |
| `syntax_highlight` | `SYNTAX_HIGHLIGHT_OFF` `_ON` `_ON_SHADOW` |

**Integer slugs** carry a plain index: the toggles `wireframe`,
`normal_vectors`, `vertex_outlines`, `vertex_points`, `light_indicators`, `camera_rotate`, `variable_panel`
are `0`/`1`; `transform_guides` is a small multi-state cycle saved as its
index. Legacy numeric `vertex_labels` values are still accepted, but saves
now use the symbolic enum names above.

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

## Config Variables (`// @config`)

The variable panel dims rows for variables the program assigns — a useful
cue separating *state* the scene computes from *config* you are meant to
tweak. But some config variables are assigned only to keep them in bounds:

```c
float n = 20; // @config
n = floor(n);           // keep n an integer
n = min(20, max(n, 3)); // clamp to a safe range
```

Without a tag, those clamp lines would dim `n` as if it were program state.
Tagging the declaration `// @config` tells the panel the writes are
bounds-keeping, not state updates, so the row stays bright and reads as the
slider-friendly knob it is.

Like `// @tune`, the tag is a bare trailing comment token (whole-token
match — `// @configured` does not count), it applies to every name on the
declaration line, extra comment text after the tag is fine, and it survives
export/import round trips via a marker on the exported `@declare` comment.
The two tags are independent and can be combined (`// @tune @config`):
`@tune` adds the exported-knob badge and keyboard controls, `@config` only
affects panel brightness inside gl-repl.

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
<workspace>/    managed scenes plus the authoritative .glr-workspace manifest
audio_state.ini persisted audio state (track, position, volume) — in the
                working directory, or in the app state dir when the working
                directory is not writable (the macOS .app, whose working
                directory is `/`)
./workspaces/   named workspace root for writable-directory development runs
~/Library/Application Support/gl-repl/workspaces  packaged macOS workspace root
~/Library/Application Support/gl-repl/state       packaged macOS app state dir
~/Library/Application Support/gl-repl/Music    per-user music folder (macOS)
$XDG_DATA_HOME/gl-repl/music                   per-user music folder (Linux)
```

---

<sub>See also: [User Guide](USER_GUIDE.md) · [Contributing](CONTRIBUTING.md) · [Architecture](ARCHITECTURE.md)</sub>
