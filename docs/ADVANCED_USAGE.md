# Advanced Usage (Draft)

The power-user reference: command-line flags, environment variables,
headless rendering, recording, documentation media, app packaging, mesh export,
scene-file headers, scratch block assignment, music, and diagnostics. For
day-to-day features (editing, the language, the panels), see the
[User Guide](USER_GUIDE.md).

## Synopsis

```
gl-repl [file.c | workspace/ | -] [--example name|n] [--tutorial name|n]
        [--tour name|n] [--tour-stop checkpoint]
        [--time secs] [--window WxH]
        [--export-c out.c] [--export-glr out.glr]
        [--export-ply out.ply [--export-ply-srgb]]
        [--accum | --no-accum]
        [--assets dir] [--examples-dir dir] [--no-audio] [--watch]
        [--dump-code] [--dump-flat] [--flat-histogram] [--call-tree]
        [--dump-state-layout] [--detailed-prof]
        [--list-examples] [--list-tutorials] [--list-tours] [--list-config]
        [--lint-scenes dir]
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
| `--tutorial` *name*\|*n* | Start a built-in interactive tutorial on launch (case-insensitive name, or 1-based index). |
| `--list-tutorials` | Print the built-in tutorials and exit. |
| `--examples-dir` *dir* | Load `catalog.ini` + its scene files from *dir* at runtime instead of the compiled-in examples. The tree ships one such directory: [`tests/scenes/stress/`](../tests/scenes/stress/README.md). |
| `--lint-scenes` *dir* | Validate every `.glr` in *dir* against the canonical document order and `@camera` tags, print every violation, and exit (no window). See [Scene-file headers](#scene-file-headers). |
| `--tour` *name*\|*n* | Start and play a built-in guided tour on launch (case-insensitive name, or 1-based index). Space play/pause, arrows step, Esc exit. |
| `--tour-stop` *id* | With `--tour`, fast-forward to `# @checkpoint id` and pause there. Checkpoints are authoring markers and are ignored during normal playback. |
| `--list-tours` | Print the built-in guided tours and exit. |
| `--list-config` | Print the tab-separated config labels and stable `@cfg` slugs and exit. |
| `--time` *secs* | Initial value of the animation variable `t` (applied after any `--example` load). Overrides `GLR_TIME`. |
| `--window` *WxH* | Initial window size (default 1200x800). |
| `--export-c` *out*.c | Load the session, write it out as a standalone C program (the same file Ctrl+S / File → Save writes), then exit. Runs GL-free - no window, no display - so it works over ssh and in CI. Honors `--example`, a positional session file, and `--window` (which sets the exported program's window size). |
| `--export-glr` *out*.glr | The same, in the `.glr` authoring format built-in examples ship in (File → Save Scene as .glr). Combines with `--export-c`: the session loads once and is written in both formats. |
| `--export-ply` *out*.ply | Capture the scene geometry to an ASCII PLY mesh on frame 1, then exit. Needs a display. |
| `--export-ply-srgb` | With the above, decode vertex colors sRGB → linear for color-managed viewers. |
| `--no-accum` | Disable the accumulation buffer (anti-aliasing + motion blur). |
| `--accum` | Force the accumulation buffer on. Without either flag the feature auto-disables on renderers that emulate `glAccum` on the CPU - anything reporting Mesa / llvmpipe / softpipe / swrast, where each pass costs a full scene re-render *plus* a host-side read-add-write of the color buffer. Hardware renderers and the web build (gl4es accumulates in a real FBO - see [`packaging/web/patches/gl4es-accum-fbo.patch`](../packaging/web/patches/gl4es-accum-fbo.patch)) stay on by default. Needed for accum-AA captures under the headless OSMesa build, which is Mesa by construction. |
| `--assets` *dir* | Scan *dir* for `*.mp3` instead of `./assets`. Beats `GLR_ASSETS_DIR`. |
| `--no-audio` | Skip audio initialization entirely (also isolates startup stalls). |
| `--watch` | Follow the positional file: whenever an external editor saves it, gl-repl re-reads it and the scene updates. See [Bring your own editor](#bring-your-own-editor). Requires a positional file - `--watch` on its own is an error. |
| `--dump-code` | Load the session and print the editor buffer to stdout, then exit. |
| `--dump-flat` | Load the session and print the flattened command list, then exit. Each row now includes `frame=N` (or `frame=-1` at top level / after table overflow). |
| `--flat-histogram` | Print per-function / per-line flat-command budget costs. Honors `--example`. |
| `--call-tree` | Print the interned per-invocation call tree (function name, captured arguments, source site, flat range). Honors `--example`. A `NOTE` line appears if the table overflowed. |
| `--dump-state-layout` | Print the `ReplRuntimeState` field layout and exit. |
| `--detailed-prof` | Add fine-grained init-trace phases (see [Diagnostics](#diagnostics)). Also via `GLR_DETAILED_PROF`. |

Unrecognized flags are not rejected - the first non-option argument is taken as
the input file, so a typo lands as `Error: cannot open --typo`. `--help` is the
authoritative list; the completions in
[Shell completion](#shell-completion) offer exactly these flags.

## Shell completion

`scripts/completions/` carries bash and zsh completions for `gl-repl` and for
`scripts/docs-assets.sh`. Nothing needs generating or regenerating: the
candidate lists come from the commands' own `--list-examples`,
`--list-tutorials`, `--list-tours`
and `--list` output, so the example/tutorial/tour catalogs and the doc-asset
arrays stay the single source of truth. These list paths print and exit before
any GL, window, audio, or `magick`/`ffmpeg` work, so completion is instant and
works on a headless machine with no capture tools installed.

To add both zsh completions to `~/.zshrc` (without duplicating them on later
runs):

```bash
make install-completions
```

The generated block uses absolute paths to this checkout. Set `ZSHRC=path` to
install into a different startup file.

```bash
# bash - source what you want (a shell rc, or per-session)
source scripts/completions/gl-repl.bash
source scripts/completions/docs-assets.bash
```

```zsh
# zsh - either put the directory on fpath before compinit…
fpath=("$PWD/scripts/completions" $fpath)
autoload -Uz compinit && compinit

# …or just source the files any time after compinit has run
source scripts/completions/_gl-repl
source scripts/completions/_docs-assets.sh
```

`gl-repl` completes every flag and catalog names for `--example` /
`--tutorial` / `--tour`, directories for `--examples-dir` / `--assets` /
`--lint-scenes`,
`*.c` for `--export-c`, `*.glr` for `--export-glr`, `*.ply` for `--export-ply`,
common sizes for `--window`, and `*.c` scenes or a workspace directory
positionally. Catalog names contain spaces and parentheses; they come
back correctly escaped (or bare inside an open quote), so
`--example Par<Tab>` yields a single usable argument.

The flag list is **not** generated, so `make check-completions`
([`scripts/check/check-completions.sh`](../scripts/check/check-completions.sh),
also part of `make check-state-ownership`) hard-fails unless four lists hold
exactly the same options: the `print_usage()` text and the `strcmp(argv[i], …)`
arms in [`src/app/boot/glr_cli.c`](../src/app/boot/glr_cli.c), the `_arguments`
spec in `_gl-repl`, and `$_gl_repl_opts` in `gl-repl.bash`. It reads sources,
never a built binary, so it runs in an unbuilt tree. Adding a flag is four
edits - five when it takes an argument, since the bash side needs a `case` arm
for the argument's own candidates.

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

Every hook that names a source row takes the line number the **code panel's
gutter shows** - 1-based, so the number in the script is the number on screen.
A value below 1 is refused with a note on stderr rather than acted on.

| Variable | Values / default | Effect |
|---|---|---|
| `GLR_ASSETS_DIR` | Directory path; default `./assets`; `--assets` wins. | Primary music directory scanned for `*.mp3`. |
| `GLR_TIME` | Seconds; default `0`; `--time` wins. | Initial animation time `t`, applied after any example/file load. |
| `GLR_EDIT_LINE` | Line number as the code panel shows it (1-based); clamped. | Parks the cursor after load and scrolls it into view so cursor-bound overlays render in captures. |
| `GLR_TYPE_KEYS` | Text fed through the keyboard dispatch after load. | Poses mid-typing states (partial-input vertex guides, autocomplete ghost/popup) for captures. |
| `GLR_CODE_FOCUS` | `0` / `1`; default is the app's own state. | Sets code focus as a level, not a toggle. `0` reveals the generated C the focused view hides - `init()`, the `display()` prologue - which is the only place a light rig's positions and colors are written down. |
| `GLR_CODE_SCROLL` | Code-panel row to park at the top of the panel. | Scrolls the panel and stops follow-scroll from pulling it back. Rows here are panel rows: with code focus off the generated C fills rows that belong to no document line, so a cursor cannot reach them. |
| `GLR_OPEN_COLOR_PICKER` | Line number of an editable color command. | Opens the floating color picker on that line for captures - it otherwise needs a swatch click. |
| `GLR_OPEN_GL_STATE` | Line number of a blank source row. | Routes a synthetic right-click to that visible row and opens the GL-state popup; retries after `GLR_EDIT_LINE` follow-scroll. |
| `GLR_OPEN_ASSIGN_PLOT` | Comma-separated line numbers of assignment rows. | Routes a synthetic right-click to the first (visible) row and opens its value plot; same retry-until-on-screen behavior. Further rows are added as extra series, up to four, as Shift+right-click would. No-op if a row is not a `var = expr;` / `A[i] = expr;`, or if its X axis cannot match the first row's. |
| `GLR_OPEN_COMMAND_HELP` | `<line>[,<dx>]` - line number of a committed GL-family row, plus an optional horizontal offset in screen px (right positive). | Right-clicks that row to raise its authored help card; same retry-until-on-screen behavior. No-op on a row with no description record. `dx` slides the opened card along x, since the click must land on the row being explained while the card may need to sit clear of something else; the renderer clamps it into the window. Pose it *after* `GLR_OPEN_ASSIGN_PLOT` - a right-click on an assignment row closes the card. |
| `GLR_ASSIGN_PLOT_EXPANDED` | Any non-empty value; default off. | Opens the assignment plot in its doubled size (the `2x` zoom chip, otherwise mouse-only). |
| `GLR_ASSIGN_PLOT_LOG` | Any non-empty value; default off. | Requests the log₁₀ Y axis (the `log` chip). Ignored only when the trace is pinned at exactly zero; signed data lands on the symmetric log axis. |
| `GLR_ASSIGN_PLOT_RATE` | `once`, `1hz`, `frame`; default `1hz`. | Sets the capture-rate chip (mouse-only otherwise). `frame` is what makes a plotted capture deterministic: at `1hz` the samples land on wall-clock, so the shot depends on how fast the machine renders. |
| `GLR_ACCUM_PASSES` | `1`, `2`, `4`, `8`, `12`, or `16`; default app setting. | Overrides accumulation-AA sample count, mainly for capture/media generation. |
| `GLR_ACCUM_EFFECT` | `off`, `aa`, `blur`, `blur cam` (case/space-insensitive); default app setting. | Picks the Config menu's *Accum effect* state by name. Not scene metadata (no `@cfg` slug), and its key binding carries a Shift that `GLR_TYPE_KEYS` cannot deliver, so this is a capture's only route to motion blur. |
| `GLR_SYNTAX_HIGHLIGHT` | `off`, `on`, `on+shadow` (case/separator-insensitive); default is renderer-dependent (see below). | Picks the Config menu's *Syntax highlight* state by name. Applied after the file/example load, so it beats both the renderer default and a scene's own `@cfg syntax_highlight` - which is what keeps a doc capture looking the same on a Mesa box as on a native one. |
| `GLR_TICK_PER_FRAME` | Any non-empty value; default off. | Advances the complete fixed-dt simulation once per rendered frame for deterministic offline capture. |
| `GLR_VIEW_TOGGLE_AT` | Comma-separated capture-clock seconds. | Toggles 2D/3D view mode at deterministic times while recording. |
| `GLR_POINTER_SCRIPT` | Path to a pointer script; implies `GLR_TICK_PER_FRAME`. | Drives scripted synthetic mouse/keyboard input (menu glides, clicks, highlight rings) with a visible cursor overlay - the video-capture hook behind `scripts/record-video.sh`. Grammar in [`src/app/glr_pointer_script.h`](../src/app/glr_pointer_script.h). |
| `GLR_NO_SPLASH` | Any non-empty value. | Skips the startup splash banner (captures that should not open on the splash band). |
| `GLR_NO_INPUT` | Any value other than `0`; default off. | Makes the window ignore real keyboard and mouse events. A windowed capture takes focus, so anything typed at the launching terminal - or a stray click, or the pointer merely crossing the window - lands in the editor and corrupts the shot. Scripted input is unaffected: `GLR_TYPE_KEYS` and `GLR_POINTER_SCRIPT` drive `glr_ctrl_scripted_*`, not the gated GLUT callbacks. |
| `GLR_NO_POINT_PARAMETER` | Any non-empty value. | Forces the no-`glPointParameterfv` fallback path even on capable hardware. |
| `GLR_NO_GPU_PROF` | Any non-empty value. | Disables GPU timer-query profiling; the profile panel GPU column reads `--`. |
| `GLR_DETAILED_PROF` | Any non-empty value; same as `--detailed-prof`. | Enables the finer init-trace phases and first-two-frame timing triples. |
| `GLR_PROF_DUMP` | Frame interval; default off. | Prints the profile panel's rows (running averages, ms) to stderr every N frames, for A/B measurement without a window. See [Diagnostics](#diagnostics) for how to read them. |
| `GLR_PROF_DUMP_MIN_MS` | Milliseconds; default `0.02`. | Row cutoff for `GLR_PROF_DUMP`; `0` prints every section. |
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
| `FREEGLUT_CAPTURE_STREAM` | Path or `-` (stdout). | Redirects captured frames into one concatenated PPM stream instead of numbered files. Point it at a fifo and an encoder (`ffmpeg -f image2pipe -vcodec ppm`) consumes frames as they render - no on-disk framebuffer dumps. |
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

`make help-vars` prints the public Make variables straight from their
declarations - it is generated from the Makefile, so it cannot drift. The
table below is the wider set, including the script and test env vars Make
never sees.

| Variable | Where | Effect |
|---|---|---|
| `CC` | Makefile, `scripts/check/check-c99.sh`, export tests. | Compiler command. The test runner passes it to export-compile checks as `REPL_EXPORT_CC`. |
| `CFLAGS` | Makefile. | Extra user C flags appended to the selected build mode - **the hook for the compile-time defines**, e.g. `make gl-repl CFLAGS=-DUI_THEME_DEFAULT=1` or `CFLAGS=-DGLR_AUDIO_NO_THREAD=1`. |
| `CPPFLAGS` | Makefile compile rules. | Also honored, for toolchains that inject preprocessor flags through the environment (`emmake` forwards it into the web build). Prefer `CFLAGS` when passing defines by hand; this is a C project and the `CPP` reads as C++. |
| `BUILD` | Makefile. | Build mode: `release`, `debug`, `coverage`, or `quick` (`-O0 -g0`, no sanitizers). Tests default to debug; app/bench/demo targets default to release. |
| `V`, `VERBOSE` | Makefile. | `1` restores verbose compiler/linker commands and diagnostics; the default prints one timed line per compiled C file and per linked binary, plus a longest-build-step summary (compile and link steps both count - the per-binary link step is often the dominant cost across the ~80 test binaries). |
| `DEBUG_INFO_CFLAGS` | Makefile. | Debug-info flags. Defaults to `-ggdb -g3` everywhere except the `WEB=1` release link, which defaults to `-g0`: emcc keeps DWARF inside the `.wasm` and drops to limited binaryen optimizations when it is present (5.4 MB vs 1.8 MB `index.wasm`, with no measurable runtime difference). Override to force either way, e.g. `make web DEBUG_INFO_CFLAGS=-g2` for named frames in a browser profile. |
| `EXAMPLES_CATALOG` | Makefile. | Catalog compiled into the example table. `make web` defaults to `examples/catalog-emscripten.ini`; pass another catalog, e.g. `make web EXAMPLES_CATALOG=tests/scenes/general/catalog.ini`, to compile that catalog instead. Explicit overrides accept runtime-style flat scene paths and free-form tags. |
| `SAN` | Makefile. | Debug sanitizer selector used by `make debug-msan` / `make test-msan`: `address` (default ASan+UBSan) or `memory` (MSan with origin tracking; uses `build/debug-msan*`). |
| `MSAN_CC` | Makefile. | Compiler used by `make debug-msan` / `make test-msan`; defaults to `clang`, override for versioned LLVM binaries. |
| `NO_SAN`, `NOSAN` | Makefile. | Disable debug-build sanitizers. |
| `ASAN` | Makefile. | Positive-polarity spelling of the sanitizer toggle: `ASAN=0` is equivalent to `NO_SAN=1`; `ASAN=1` is the explicit affirmative (also the default). |
| `USE_GL_STUBS` | Makefile. | `1` builds against bundled no-op GL/GLU/GLUT headers for non-rendering tests. |
| `GLR_AUDIO_NO_DEVICE` | Audio runtime, `make test-msan`. | Any non-empty value initializes miniaudio without opening a host audio device; `test-msan` sets this to avoid uninstrumented system audio backends. |
| `FREEGLUT_OSMESA` | Makefile. | `1` builds vendored freeglut with the headless OSMesa backend. |
| `FREEGLUT_VENDOR` | Makefile. | `0` skips the vendored freeglut static library, used by the `make glut` fallback. |
| `APP_ICON_SVG` | Makefile `make app`. | Source SVG for the generated macOS `.icns`. |
| `SKIP_CHECKS` | Makefile. | `1` drops the `check` prerequisite from the test lanes: `make test SKIP_CHECKS=1` is the suite with no guard gate. |
| `FORMAT` | Makefile: `bench`, `bench-web`, `bench-render`. | Report shape: `human` (default) or `csv` - the lanes whose binaries implement `--csv`. Under `csv` the lane drops its progress echoes and command echo, so its own output is exactly the machine-readable rows (a *cold* build still prints compile lines first - build once, then pipe). The windowed benches (`bench-glut-bitmap*`, `bench-code-panel-*`, `bench-vertex-labels`) have no CSV mode and **refuse** `FORMAT=csv` rather than silently returning human output; the two build-only lanes (`bench-glut-bitmap-build`, `bench-web-gl4es`) take neither `FORMAT` nor `ARGS`. |
| `TEST_JOBS` | Makefile, `scripts/run-tests.sh`. | Limits parallel test binaries; empty/`0` means unbounded parallel runner behavior. |
| `ARGS` | Makefile `run-test-*` and bench targets. | Extra arguments passed to the binaries run by the target, e.g. `make run-test-repl-core-examples ARGS='--show-mismatch'` or `make bench ARGS='--iters 20'`. |
| `<BENCH>_BENCH_ARGS` | Makefile. | Per-bench default arguments (`GLUT_BITMAP_BENCH_ARGS`, `CODE_PANEL_TEXT_BENCH_ARGS`, `CODE_PANEL_STENCIL_BENCH_ARGS`, `VERTEX_LABEL_BENCH_ARGS`); `ARGS` is appended after them. |
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
| `SANITIZER_CHECKERS` | Makefile `analyze`. | Clang static-analyzer checker list. |
| `ANALYZE_EXCLUDE` | Makefile `analyze`. | Space-separated source list excluded from static analysis. |
| `OUT_DIR`, `SRC_DIR`, `TEST_DIR`, `TOOLS_DIR`, `BENCH_DIR` | `scripts/code-smells.sh`. | Input/output directories for the code-smell audit script. |
| `JOBS` | `scripts/code-smells.sh`. | Parallelism for clangd/clang-tidy scans. |
| `CLANGD_BIN`, `CLANG_TIDY_BIN` | `scripts/code-smells.sh`. | Tool paths for clangd and clang-tidy. |
| `CLANG_TIDY_CHECKS` | `scripts/code-smells.sh`. | clang-tidy check filter. |
| `MIN_TOKENS`, `PMD_IMAGE` | `scripts/code-smells.sh`. | PMD CPD duplicate-token threshold and Docker image. |
| `LIZARD_CCN`, `LIZARD_LEN` | `scripts/code-smells.sh`. | Lizard complexity and function-length thresholds. |

### Compile-time defines

These are `-D` defines passed through `CFLAGS` (or `CPPFLAGS`).

| Define | Values / default | Effect |
|---|---|---|
| `UI_THEME_DEFAULT` | `0` green (default), `1` warm, `2` cyan, `3` amber, `4` violet, `5` mono. | Compile-time UI color scheme, e.g. `make gl-repl CFLAGS=-DUI_THEME_DEFAULT=1`. Defined in [`config.h`](../config.h), range-checked in [`src/ui/core/theme.h`](../src/ui/core/theme.h). See [ARCHITECTURE.md > UI Color Theming](ARCHITECTURE.md#ui-color-theming). |
| `GLR_CODE_PANEL_SCROLLOFF` | Integer $\ge 0$, default `2`. | Minimum number of screen lines kept exposed above and below the cursor when the code panel follows cursor navigation (like Vim's `scrolloff`), e.g. `make gl-repl CFLAGS=-DGLR_CODE_PANEL_SCROLLOFF=4`. Defined in [`config.h`](../config.h). |
| `GLR_VIEW_CAMERA_TO_2D_DECAY` | Float in $(0, 1)$, default `0.75f`. | Damping decay factor for camera easing during 3D &rarr; 2D view flattening, applied before projection blending begins, e.g. `make gl-repl CFLAGS=-DGLR_VIEW_CAMERA_TO_2D_DECAY=0.85f`. Defined in [`config.h`](../config.h). |
| `GLR_AUDIO_NO_THREAD` | `1` drops the worker; `0` forces it on. Auto-enabled on Emscripten (no `-pthread`). | Runs the playlist lifecycle ops (file open/uninit, state save) synchronously, drained from `glr_audio_tick()` on the caller, e.g. `make gl-repl CFLAGS=-DGLR_AUDIO_NO_THREAD=1`. Contained entirely in [`src/app/glr_audio.c`](../src/app/glr_audio.c). |

## Headless rendering (OSMesa)

For CI or any machine with no display, `make ... FREEGLUT_OSMESA=1` builds
gl-repl against a software, off-screen **OSMesa** backend - it renders into
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

### Rendering benchmark and web/gl4es regression lane

The shared [`bench/bench_render.c`](../bench/bench_render.c) harness runs
fixed-function draw workloads and reports timing plus framebuffer metrics.
Use the native build for a local baseline; use OSMesa when no display is
available:

```bash
make bench-render
make bench-render FREEGLUT_OSMESA=1 ARGS="--csv --reps 20"
```

Native runs report oracle failures as warnings by default because some native
contexts lack features that web gl4es emulates, such as an accumulation
buffer. Add `--strict` when those feature-invariant checks should make the
benchmark exit non-zero; the browser build enables strict mode automatically.
The checks use coverage and color probes, not exact framebuffer hashes.

The web-driven path is the regression lane for gl4es patches:

```bash
make bench-web-gl4es
python3 scripts/web-serve.py build/release-web
```

Open `gl4es-render.html` from that server. It exercises triangles, batched
points, wide/polygon lines, attrib-stack restoration, `glAccum`, perspective
bitmap raster positions, and front-face isolation after a GL_BACK
color-material selection. The page sets
`document.title` to `PASS gl4es render bench` or `FAIL gl4es render bench` and
publishes per-case timing/coverage/probe data in `window.gl4esRenderBench`.
The checks use feature invariants rather than exact pixels, since WebGL
antialiasing and color output can differ between browsers and GPUs. The other
pages built by `bench-web-gl4es` remain useful for narrower line/display-list
investigations.

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
labels) need the cursor parked on the relevant line - `GLR_EDIT_LINE=<n>`
does that at startup, as if you had arrowed to the line the code panel numbers
*n*:

```bash
GLR_EDIT_LINE=5 ./build/release-osmesa/gl-repl scene.c --no-audio &
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
frame, so playback is `~fps/60`× natural speed - use `--fps 60` for
real-time. Needs `ffmpeg`. (`scripts/record-gif.sh --help` for all flags.)

Native builds support the same `FREEGLUT_CAPTURE_FRAMES=N` contract too: they
open a real window, write N real-GPU frames, and exit.

`GLR_VIEW_TOGGLE_AT=0.5,2.0` is a recording affordance for the 2D/3D swatch in
the menu bar. It toggles view mode as the fixed capture clock crosses each
listed second and implicitly enables `GLR_TICK_PER_FRAME`, so UI transition
clips are deterministic.

### Recording videos with scripted interaction

`scripts/record-video.sh` records a session to an MP4 (H.264 + AAC music),
optionally driven by a **pointer script** (`GLR_POINTER_SCRIPT`) - synthetic
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
- **Colorspace** is declared at encode time - the captured frames are
  display-referred RGB (the app is not color-managed). `--colorspace srgb`
  (default) tags standard sRGB; `--colorspace p3` tags Display P3 (P3-D65
  primaries + sRGB transfer), reproducing how the app looks on a wide-gamut
  Mac display. Either way the RGB→YCbCr conversion uses the BT.709 matrix
  explicitly (ffmpeg's RGB default is BT.601, which players mis-assume for
  HD), and the tags are set on the frames via `setparams` - on ffmpeg ≥ 7
  the encoder takes color properties from frames, so the old
  `-color_primaries`/`-color_trc` encoder flags no longer stick.
- **Scripted input** covers the whole input surface: `move`/`glide` pointer
  motion (hover opens menu flyouts exactly as a real mouse), `click` /
  `rightclick` / `down` / `up`, `wheel`, `ring` (pulsing highlight around a
  UI element), `key` (typed text, incl. `\cX` control bytes like Ctrl+T;
  `key@<cps>` paces the text at N characters per second so it types out like
  a person instead of appearing at once) and
  `skey` (F-keys, arrows) - so whole typed demos are re-recordable from a
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
captured from the standalone demo binaries rather than from `gl-repl` - they are
their own category so `--pngs` still needs only `make gl-repl`. Build the demos
first (`make render3d-demo repl-live-demo editor-demo variable-panel-demo
color-picker-demo cpuprof-demo memprof-demo`); `DEMO_BIN_DIR` overrides where
the script looks for them. Demos with nothing on screen cold are staged through
the `GLR_DEMO_*` hooks above.

Tab-completion for the flags and the ~60 asset names ships in
`scripts/completions/` - see [Shell completion](#shell-completion).

GIF generation uses `GLR_TICK_PER_FRAME`, so machine load changes generation
time without dropping animation states. The `profile-panels` screenshot still
reflects live rendering performance and should be regenerated on an otherwise
unloaded machine.

**Animated clips: GIF or APNG, per clip.** A clip's format is not a property
of its call site - it is whichever file is already on disk (`<name>.gif` or
`<name>.png`; a brand-new clip defaults to GIF). **A plain regeneration
reproduces each clip in the format it is already in**, so
`scripts/docs-assets.sh` with no options rebuilds an APNG clip as an APNG and
a GIF clip as a GIF, and formats and doc links never move on their own. Only
`--to-apng` / `--to-gif` change a format.

`--to-apng` and `--to-gif` migrate the selected clips:

```bash
scripts/docs-assets.sh --formats              # what each clip is now
scripts/docs-assets.sh --to-apng view-mode-2d # one clip
scripts/docs-assets.sh --to-apng sc-whale replay
scripts/docs-assets.sh --to-apng              # every clip
```

`--formats` is the companion to those: it lists the selected clips with their
current and target format, size and path, so a subset can be chosen from what
is actually on disk rather than guessed. Target equals current unless a
migration flag is present, which makes `--formats --to-apng <clips>` a **dry
run** - it prints what a real run would write and changes nothing. It reports clips only - a still has no format to
convert - and it reads each clip's path out of its own `clip` call site, so it
cannot drift from what the script would actually write. `--list` deliberately
stays bare names: the shell completions parse it.

Either flag re-encodes, **repoints every Markdown reference** at the new
extension, and deletes the superseded file. It is symmetric, so a migration
you don't like is undone by running the other flag. Because it rewrites
shared docs it forces `-j 1`.

The two formats are tuned separately, because they fail differently:

| | palette | dither | post |
|---|---|---|---|
| GIF | 128 | none | `magick -fuzz 4% -layers Optimize` |
| APNG | 192, `stats_mode=diff` | `atkinson`, `diff_mode=rectangle` | `oxipng -o max` |

GIF gets no dither because dithering *and* the lossy `-fuzz` delta together
speckle the flat blacks. APNG can afford both more colours and a dither
precisely because its deltas are exact - there is no lossy step downstream to
turn the dither into permanent speckle - and `oxipng` then takes ~8% back
losslessly. APNG clips need `apngasm` **and** `oxipng`
(`brew install apngasm oxipng`); a run that touches no APNG clip needs
neither. Note that `ffmpeg`'s own APNG encoder is not a substitute for
`apngasm`: it barely deltas between frames (3x the size).

An APNG is a `.png` to every consumer, so nothing in the Markdown marks it as
animated: say "Animation:" in the alt text. The script prints a reminder on
each migration.

Each asset is a staged snippet scene with optional `@cfg` headers, `@camera`-tagged
rows, and sometimes `GLR_EDIT_LINE` to pose cursor-bound overlays. Captures use
record mode and keep the last frame, so theme fades and other frame-based
settling are deterministic.

Every gl-repl asset renders into the same 1x `--window 1200x800`. There is no
oversize-and-downscale supersampling path: a native window is clamped to the
visible screen, so an over-size request comes back the wrong size. Antialiasing
comes from the context's MSAA, and stills raise `GLR_ACCUM_PASSES` on top of it
- the scene is re-rendered per pass with a sub-pixel frustum jitter and
accumulated, which the 2D UI draws *outside* of, so bitmap text stays crisp
while 3D edges and 1px grid/axes hairlines smooth out at full weight. GIFs keep
stock MSAA only; raising the pass count would multiply the cost of every
recorded frame. The `demo-*` stills take none of this: each demo binary opens at
its own compiled-in size and takes no gl-repl flags.

### Windowed capture on Linux (`FREEGLUT_VENDOR_LINUX=1`)

A default windowed Linux build links the **distro's** `-lglut`, which has none
of the fork's capture hooks - so `SIGUSR1` screenshots,
`FREEGLUT_CAPTURE_FRAMES` and `FREEGLUT_CAPTURE_STREAM` silently produce
nothing there, and `scripts/docs-assets.sh` / `record-gif.sh` /
`record-video.sh` come back empty. Build against the vendored static
freeglut (X11/GLX backend) instead:

```bash
make gl-repl FREEGLUT_VENDOR_LINUX=1
```

Needs `cmake` plus the X11/GL dev headers (`libx11-dev`, `libxi-dev`,
`libxrandr-dev`, `libxxf86vm-dev`, `libgl1-mesa-dev`, `libglu1-mesa-dev`).
GL/GLU/X11 still come from the system; only freeglut is vendored, linked by
archive path with no `-lglut`. Objects land in a separate
`build/<cfg>-fgvendor/` tree, so this build and the default one coexist
without recompiling each other. macOS always vendors and needs no flag; the
Linux **OSMesa** build (`FREEGLUT_OSMESA=1`) vendors unconditionally, since
system freeglut has no OSMesa backend.

### External freeglut (`FREEGLUT_LIB_PATH`)

To try a freeglut that is *not* the vendored one - a local fork, another
branch, a distro build - point the build at its static archive instead of
re-vendoring:

```bash
make gl-repl \
  FREEGLUT_LIB_PATH=~/src/freeglut/build/lib/libglut.a \
  FREEGLUT_INCLUDE_DIR=~/src/freeglut/include
```

The path replaces the vendored archive everywhere it is used (the binary, the
tests, the GL benches, `make render3d-hot`), and nothing under
`third_party/freeglut/` is built or consulted for it - if the file is missing,
the build stops with that message rather than running CMake. `make
freeglut-clean` still cleans the vendored build dir.

**Headers do not follow the archive.** `FREEGLUT_INCLUDE_DIR` defaults to the
vendored `third_party/freeglut/include`, so set it whenever the external
build's `<GL/freeglut.h>` differs - upstream freeglut is exactly that case,
since its header lacks the fork's capture declarations. Objects go to a
separate `build/<cfg>-fgext/` tree so they never mix with vendored-header
objects.

Everything the archive does not carry itself still comes from the platform link
line (macOS frameworks, X11 libs on Linux), so the external build has to be a
**static** freeglut for the same backend as the arm you are building - Cocoa on
macOS, X11/GLX for a windowed Linux build, OSMesa under `FREEGLUT_OSMESA=1`. On
Linux the flag implies `FREEGLUT_VENDOR_LINUX=1` (archive by path, no
`-lglut`).

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
- your `glVertex` polygons, GLU-tessellated shapes, and the GLUT solids
(teapot/sphere/cube/cone/torus) - is captured through a single
`glRenderMode(GL_FEEDBACK)` pass, so everything on screen exports the same
way. Authored per-vertex normals are preserved; the rest are smoothly
synthesized.

Scripted capture (the normal build needs a display; on a machine without one,
use the [OSMesa build](#headless-rendering-osmesa)):

```bash
./gl-repl --example "Parametric torus (nested for)" --export-ply out.ply                     # capture on frame 1, then exit
./gl-repl --example "Parametric torus (nested for)" --export-ply out.ply --export-ply-srgb   # decode colors sRGB -> linear
```

Point and line primitives are preserved as PLY loose vertices / `edge`
elements. Because mesh-only viewers such as Xcode ignore those records, the
app export also adds small octahedron / capped-tube triangle proxies whose
radius scales with the scene bounds. See *Mesh Export (PLY via GL_FEEDBACK)* in
[`ARCHITECTURE.md`](ARCHITECTURE.md) for the capture/encode design.

## `gl-repl-unchained` — raised capacity, experimentation only

```bash
make gl-repl-unchained                    # ./gl-repl-unchained
make gl-repl-unchained FREEGLUT_OSMESA=1  # headless variant
```

The same application with the document ceilings lifted, for machine-generated
scenes — a `glprobe` extraction, a mesh conversion — that blow straight past
limits sized for hand-typed input. **Nothing in the test suite runs against it,
and it is not a supported configuration.**

| Constant | Default | Unchained |
|---|---|---|
| `MAX_EDITOR_COMMANDS` | 1024 | 32768 |
| `MAX_FLAT_COMMANDS` | 8192 | 65536 |
| `MAX_FLATTEN_VISIT_BUDGET` | 200000 | 2000000 |
| `MAX_CALL_FRAMES` | 16384 | 131072 |
| `REPL_UNDO_DEPTH` | 32 | 8 |

Two of those are not capacity increases, and both are load-bearing:

- **`REPL_UNDO_DEPTH` goes *down*.** `MAX_EDITOR_COMMANDS` costs ~33 KB per
  unit (`make show-capacity`) because every source command is duplicated
  across the 32×2 undo/redo rings and 8 user-scene slots — and that memory is
  resident, not lazily-faulted. Shortening the history is what keeps a 32×
  document cap from costing over a gigabyte; measured resident set is ~630 MB.
- **`MAX_FLATTEN_VISIT_BUDGET` goes up**, or the target would just trade the
  editor ceiling for the flatten one on the first large document.
- **`MAX_CALL_FRAMES` tracks the 8× flat-command raise** (the argument
  arena is `MAX_CALL_FRAMES * 8` and follows). Leaving it at 16384 would
  make PATH 8× more likely to latch and fall back on the one build that
  exists for oversized scenes.

### The stack

The controller and editor take document-sized snapshots as ordinary locals.
At the default cap that is 267 KB a frame — unremarkable. At 32× it is 8.5 MB,
and `glr_ctrl_display_frame` alone overflows the 8 MB macOS main-thread stack
on the *first* frame, with a `___chkstk_darwin` segfault and no usable
backtrace.

The build therefore links with `-Wl,-stack_size,0x8000000` (128 MB), visible as
a non-zero `stacksize` in `LC_MAIN`:

```bash
otool -l ./gl-repl-unchained | grep -A3 LC_MAIN     # stacksize 134217728
```

On Linux the main-thread stack comes from `RLIMIT_STACK`, not the executable,
so raise it in the shell instead — the default there is the same 8 MB
(Ubuntu 24.04 `ulimit -s` = 8192 KB):

```bash
ulimit -s 131072 && ./gl-repl-unchained scene.glr
```

There is no link-flag equivalent. GNU `ld` accepts `-z stacksize=N` and then
prints `warning: -z stacksize=... ignored`; the binary still dies at 8 MB,
because Linux sizes the main stack from the rlimit and ignores `PT_GNU_STACK`'s
size field. The build therefore emits no flag there, and prints the `ulimit`
reminder after linking.

Moving those ~20 snapshots off the stack is the real fix; forcing it on the
shipping build for the sake of an experimental target is not. If you raise the
caps further, re-check with
`-Wframe-larger-than=131072` — that is how the overflow was found.

## Bring your own editor

```bash
./gl-repl --watch scene.glr     # then edit scene.glr in vim, VS Code, anything
```

`--watch` makes an external editor a peer author of the live scene. gl-repl
re-reads the bound file whenever it is saved, so the scene updates without
leaving the editor. Outbound is the same path in reverse: Ctrl+S writes **that**
file - the one the watcher follows, not a name-derived copy somewhere else -
and so does the automatic sync below.

**Edits made in gl-repl are written back.** Not everything gl-repl changes was
typed: dragging a value in the variable panel rewrites the declaration row, and
the color picker rewrites a `glColor` row. Whenever the document moves away from
what the file last gave it, the file is rewritten and re-stamped, so the two
never drift apart silently - the failure that costs you work is the editor's
next save landing on top of a change gl-repl never wrote down.

The trigger is the document *text* plus the scene settings the file's leading
`@cfg` block carries - between them, everything the writer derives from live
state. That is also why a drag costs one write rather than sixty: the value is
applied live on every pointer event, but the source row is rewritten once, when
you let go. Live state that is neither a row nor a scene setting is deliberately
not a reason to rewrite the file: camera moves, `t`, and the
session-inspection settings (profilers, depth/stencil viz, replay) - and neither
is an empty document.

Four situations hold the write back until they resolve, all of them cases where
the document is not what the file should hold: a tutorial or tour owns the
document; a save is already waiting to be applied (inbound wins - a local write
there would erase it); a live WIP sidecar session is running, in which case the
editor's unsaved buffer is the truth and the file is meant to be behind it; or a
reload has parked an incomplete final row in the input and you are still on it
(an implicit write would drop that row from the file).

Editing locally ends a WIP session anyway, and the sync follows on the next
frame - it does **not** wait for the editor to do anything. An editor sitting
open and idle publishes nothing for as long as you leave it alone, and a change
made in gl-repl must not sit unwritten until you happen to touch vim again.

A half-typed line of your own is **not** one of them. It holds an inbound
reload - which would overwrite it - but not the write: a line you are still
typing is not in the document, so publishing the committed document without it
takes nothing away from a file that never had it. Only the parked row is
different, because that row came from the file in the first place.

`--watch` is a boolean over the existing positional argument, and it is a
session mode rather than scene configuration: there is no config toggle and no
`@cfg` slug, so a scene file cannot switch its own watcher on.

**What a save replaces, and what it leaves alone.** A watched reload replaces
the program text and its variables. Live config and the camera are preserved
even if the file carries `@cfg` or `@camera` rows - an external *text* edit is
geometry, not a presentation reset. `@scene-name` and `@workspace-dir` are read
and discarded too: a watched reload changes the program, never the slot's
identity or which file is being watched.

**The loader stays strictly atomic.** One rejected line fails the whole import
and the live document is untouched, with the offending row named in the status
bar. A file that will not parse is attempted once, not once per frame; fixing
it resumes the loop.

**When a save has to wait.** A reload is a wholesale document replacement, and
undo does not carry the in-progress input row - so a save arriving while you
are halfway through typing a line is held, and the status bar says a newer
version is waiting. What happens next depends on what you do with the line:

| You... | The waiting version |
|---|---|
| abandon the line | is applied |
| commit the line | is discarded - your commit wins until the editor saves again |

A tutorial or a guided tour holds saves the same way, and discards them when
the lesson ends: a version computed against the pre-lesson document has no
business landing on the document the lesson left behind.

**Undo.** On a user scene, a reload is one undoable clobber - Ctrl+Z gets the
gl-repl version back. On an unedited example or a transient document there is
no undo entry and no scene slot is created: the file is the source of truth and
the editor's own undo is the undo.

**Cost.** One `stat()` per frame. The file is read only when its
mtime/inode/size moves, and re-parsed only when the bytes actually differ; the
profile panel's **External Edit** row reads ~0 in steady state. What a save
itself costs is measurable - `make bench-extedit` times real content updates
against the smallest, median and largest catalog scenes and prints p50/p95/max
for the watcher section alone and for the whole frame. On this machine that is
~1 ms for a typical scene and ~5.5 ms p95 for the largest one, which is nearly
all document re-parse: reading and hashing the file is under 1% of it.

### Following the editor live

`--watch` alone reacts to `:w`. With one plugin it reacts to every keystroke
instead:

```vim
" ~/.vim/plugin/glr-wip.vim  (copy it, or :source it from the checkout)
```

[`packaging/editor/glr-wip.vim`](../packaging/editor/glr-wip.vim) publishes the
unsaved buffer to `<file>.wip` on every change and every cursor move, and
gl-repl mirrors it: the geometry updates as you type, the caret follows the row
you are on, and a half-typed command lands in gl-repl's own input line where
the edit guides and autocomplete can see it. Nothing writes the scene file
except `:w` and Ctrl+S; the sidecar is a shadow of the buffer, and it is
deleted when the buffer unloads or vim exits.

By default only `*.glr` is published - set `g:glr_wip_patterns` to widen it.

| What happens | What gl-repl does |
|---|---|
| you type | the scene follows, with no save |
| you move the cursor | the caret follows, with **no** re-parse |
| the row you are on is half-typed | it is parked in the input line, wherever it sits in the file |
| you press Ctrl+Z in gl-repl | live follow pauses and your version stands, until you type in the editor again |
| you edit in gl-repl instead | same: the local edit wins until the editor publishes again |
| vim exits | a scene you own keeps the unsaved text; a catalog scene goes back to the file |
| a `.wip` is already there when gl-repl starts | vim died holding unsaved work: gl-repl offers to follow it, and never applies it unasked |

Two things worth knowing about the shape of this:

- **A cursor move is deliberately cheap.** gl-repl hashes the buffer *without*
  its `// @cursor` line, so scrolling around costs a `stat` and a hash instead
  of a document re-parse. Following the caret across a re-parse boundary then
  needs a real physical-row-to-document-row map, because a `.glr` header row or
  an exported `.c` wrapper occupies a file row the document does not have; that
  map is built once per content update and indexed for free afterwards.
- **Undo is per session, not per keystroke.** One gl-repl undo entry when a
  session starts, then none - so Ctrl+Z exits live follow back to the last
  gl-repl document rather than stepping backwards through your typing, which
  vim's own undo already owns. On an unedited example gl-repl pushes nothing
  and creates no scene slot: typing in vim must not silently promote a catalog
  scene into one of your eight.

Any editor can drive this - the sidecar is a plain text file whose last line is
`// @cursor <line> <col>`, 1-based row and 1-based byte column. Write it
atomically (temp file in the same directory, then `rename`), or gl-repl may
read it half-written.

**The one friction that does not go away.** gl-repl rewrites the text it saves:
canonical spacing, indentation re-derived from block scope, and C float
suffixes stripped (`1.0f` becomes `1.0`). The first Ctrl+S after an external
edit therefore reformats the file under the editor. `.glr` survives best;
expressions that reference variables keep their verbatim text.

## Scene-file headers

Saved scenes are standalone C files, but their leading comments carry REPL
metadata that round-trips on reload:

| Directive | Meaning |
|---|---|
| `// @scene-name <name>` | Names the scene slot the file loads into. |
| `// @workspace-dir <path>` | Re-binds the workspace directory. |
| `// @cfg <slug> = <value>` | Applies a scene-presentation setting (see the slug table below). |
| `// @declare name` | Reconstructs a `float name;` declaration on import. |
| `// @tune` | Marks a variable as a tunable knob in the exported program - see [User Guide → Tunable Variables](USER_GUIDE.md#tunable-variables--tune). |
| `// @config` | Marks an assigned variable as config so the variable panel doesn't dim it (bounds-keeping writes like clamps) - see [Config Variables](#config-variables--config) below. |
| `// @bool` | Presents a variable as a two-state toggle: a checkbox in the variable panel, `[x]`/`[ ]` in an exported knob's HUD - see [Toggle Variables](#toggle-variables--bool) below. |
| `// @plot` | On an assignment row: opens the [assignment value plot](USER_GUIDE.md#plotting-an-assignments-values) on that row when the document loads. Tag up to four rows to overlay them. |
| `// @camera <role>` | Tags a transform row as camera state. Five roles: `dist`, `rx`, `ry`, `spin`, `pan`. |

Built-in examples use the same `@cfg` header and `@camera` tags, so a saved
scene and an example are the same file format. The `@cfg` directives are
metadata only where they *lead* the file; the same text later on is an
ordinary comment.

The camera tags are different, and deliberately so: a row is a camera row
because of its tag, never because of where it sits, so `@camera` is reserved
syntax wherever it appears. A tagged row in a place the format does not allow
- inside a function body, inside the geometry snippet, after body code, or out
of the canonical `dist, rx, ry, spin, pan` order - is **rejected with a
diagnostic naming the file, the line and the rule**, and is never passed
through into the document as geometry. "Inside the geometry snippet" means
between the `// Snippet start` / `// Snippet end` markers described in
[`examples/README.md`](../examples/README.md) - that region is geometry-only,
so a `@camera` row placed there is dropped and the host's current/default
camera stands.

`// camera` itself carries no meaning the tags do not already carry: it is an
ordinary comment, and a file with tags and no marker is fully canonical.

Each pose row's arguments must be plain float literals - `dist` with x = y = 0,
`rx` on axis 1,0,0, `ry` on 0,1,0, `pan` free - and nothing may follow the
call. `spin` is the exported C's `g_angle` animation hook: it appears only in
a `.c`, its argument must be the bare `g_angle` token, and it is accepted and
discarded. Roles the file omits keep their current value and produce a note.

Exported C rewrites `//` into C89 block comments, so the same block reads
`/* @camera dist */` there; both spellings are read identically.

`--lint-scenes <dir>` validates every `.glr` in a directory against these
rules and the document order below, reporting every violation in one pass
without opening a window.

### Authoring an example catalog

**File → Save Scene as .glr** writes the same file shape the built-in examples
ship in - the `@cfg` header, the tagged `@camera` rows, and the commands in
the canonical order (declarations, then function definitions, then camera and
body), with no C scaffold (see [User Guide → Scene export](USER_GUIDE.md#scene-export-glr) for
what the format drops). To turn one into an example:

When a `.glr` scene is loaded as a file, **File → Save Scene** writes back to
that authoring file. Use **File → Save Scene as .c** when you want the
standalone C export instead.

1. Drop the `.glr` file into `examples/scenes/`.
2. Add a section for it to `examples/catalog.ini`.

`--export-glr <file>` writes the same thing without opening the app, so an
existing scene can be re-canonicalized from a script:

```bash
./gl-repl scene.c --export-glr examples/scenes/scene.glr   # C session -> authoring format
./gl-repl --example "Parametric torus (nested for)" --export-glr copy.glr
```

It then loads as a built-in. Runtime example directories take the same layout
as-is, so `--examples-dir <dir>` will load `<dir>/catalog.ini` without a
rebuild:

```bash
./gl-repl --examples-dir examples --example <name-or-idx>
```

Each entry's `file` is any relative path under the directory - the built-in
catalog's `scenes/` subdirectory is a convention, not a requirement, and a flat
directory of `.glr` files beside its `catalog.ini` works too.

Under a runtime catalog, **File → Save Scene as .glr writes back to the file
the catalog names**, in place - the loaded entry, or the user scene the first
edit promoted it into. That is what makes `--examples-dir` an editing loop
rather than an export-and-copy-back chore: tweak the scene, save, reload. The
binding survives a rename (it tracks the file you opened, not the display
name) and takes precedence over any bound workspace. It applies to `.glr`
entries only; a `.c` catalog entry is a different format, so Save as .glr
falls back to the usual workspace export rather than overwriting it. Built-in
examples are compiled-in strings with no file behind them and behave the same
way.

Catalog entry fields, size budgets, and the other files an example must be
edited alongside are covered by the `gl-repl-scene-authoring` skill.

**A runtime catalog is not a built-in catalog**, and the difference is what the
directory is for. `examples/` is compiled into the binary by
`scripts/gen_examples.py`, so its `tags` are restricted to the four
`EXAMPLE_TAG_*` values (`2D`, `3D`, `Polygons`, `Lines`) and every entry ships
to users in the Scene menu. A `--examples-dir` catalog registers arbitrary tag
names at runtime and ships nothing, which makes it the home for scenes that
exist to be *exercised* rather than shown.

[`tests/scenes/stress/`](../tests/scenes/stress/README.md) is the one such
catalog in the tree - targeted scenes covering parser, flattener, evaluator and
GL-state corner cases, tagged by the corner case each one probes:

```bash
./gl-repl --examples-dir tests/scenes/stress --list-examples
./gl-repl tests/scenes/stress/matrix-stack-recursion-stress.glr
```

### `@cfg` scene-presentation slugs

A scene file (and a built-in example) carries one `// @cfg <slug> = <value>`
line per scene-presentation setting it overrides. Slugs are explicit stable
fields on the Config-menu descriptors; they are not generated from labels, so
a UI relabel does not rewrite saved scenes. `./gl-repl --list-config` (or
`make config-list`) prints the authoritative tab-separated slug/label/state
table. `make check-config-slugs` consumes that table and checks every `.glr`
under `examples/scenes/` and `tests/scenes/` for unknown slugs. Settings outside
this scene subset (accumulation/MSAA, profiling panels, code layout, syntax
theme) are *not* scene metadata and don't round-trip through a scene file.

**Enum slugs** take a symbolic value name; the bare integer index still loads
(legacy files), but the symbolic form is canonical on save and self-documents
which choice it selects:

| Slug | Symbolic values |
|---|---|
| `grid` | `GRID_THEME_OFF` `_CLASSIC` `_FOG` `_TRON` `_EMBER` `_FAINT` `_FOCUS` `_OCEAN` `_XZRULER` `_PLANES` `_RADAR` `_AURORA` `_SYNTHWAVE` `_FROZEN` `_SOIL` `_STARCHART` |
| `axes` | `AXES_THEME_OFF` `_CLASSIC` `_PULSE` `_NEON` `_COMPASS` `_GIZMO` `_RULER` |
| `backdrop` | `RENDER3D_BACKDROP_OFF` `_STARS` `_CITYSCAPE` `_SUNSET` `_AURORA` `_NEBULA` `_POLAR_DAY` `_DRONES` `_FAIRIES` |
| `light_theme` | `LIGHT_THEME_DEFAULT` `_HEADLIGHT` `_SOLAR` `_STUDIO` `_NEON` |
| `grid_extent` | `GRID_EXTENT_CLOSE` `_MID` `_FAR` |
| `grid_major` | `GRID_MAJOR_1` `_2` `_5` `_10` (the major-tick spacing) |
| `grid_brightness` | `GRID_BRIGHTNESS_DIM` `_NORMAL` `_BRIGHT` `_BOLD` |
| `view_mode` | `RENDER3D_VIEW_3D` `RENDER3D_VIEW_2D` (perspective vs. 2D ortho) |
| `vertex_labels` | `OVERLAY_VERTEX_LABEL_OFF` `_INDEX` `_INDEX_POS` `_INDEX_WORLD` `_INDEX_WORLD_FINE` |
| `vertex_outline_style` | `VERTEX_OUTLINE_STYLE_DEFAULT` `_BOLD` `_INVERTED` `_BOLD_INVERTED` |
| `syntax_highlight` | `SYNTAX_HIGHLIGHT_OFF` `_ON` `_ON_SHADOW` (default is renderer-dependent - see below) |

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

### Renderer-dependent syntax-highlight default

`syntax_highlight` is the one setting whose default is decided at runtime
rather than compiled in. `glr_ctrl_init_gl` reads `GL_RENDERER` / `GL_VENDOR`
and installs `SYNTAX_HIGHLIGHT_OFF` on a Mesa-family context (`mesa`,
`llvmpipe`, `softpipe`, `swrast`, `lavapipe`), because highlighting is
disproportionately expensive there and its drop shadow renders wrong:

- Highlighting doesn't change the glyph count, it changes how many colored
  spans those glyphs are split across, and each span issues its own `glColor`
  before `glRasterPos`. Mesa re-validates raster state on a dirty color -
  98.8 ns for the pair against 61.6 ns when the color is unchanged - so a
  code panel spends roughly 2 ms of its ~4 ms on highlighting alone.
  Measured by [`bench/bench_code_panel_text.c`](../bench/bench_code_panel_text.c)
  (`make bench-code-panel-text`, Linux + windowed).
- `On+Shadow` draws an offset dark copy behind each constant span, which does
  not composite correctly on Mesa.

Only the *default* moves. The **Config > Syntax highlight** row still cycles
freely, a scene's own `@cfg syntax_highlight` still wins (the probe runs
before the initial load), and `GLR_SYNTAX_HIGHLIGHT` overrides both for a
capture. The web build is exempt - gl4es draws bitmap text through its own
WebGL2 translation, so a browser renderer string that happens to name Mesa is
not describing the path the glyphs take. The boot trace records which branch
ran:

```
[init +0.412s] syntax highlighting off by default: llvmpipe (LLVM 17.0.6, 256 bits) ...
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
    { .backdrop = RENDER3D_BACKDROP_POLAR_DAY, .grid = GRID_THEME_FROZEN }, \
    { .backdrop = RENDER3D_BACKDROP_NEBULA,         .grid = GRID_THEME_STARCHART }, \
}
```

Add one row per forced pair, using enum names from
[`src/render3d/themes.h`](../src/render3d/themes.h). For example, if a future
backdrop should own `GRID_THEME_AURORA`, add another row such as
`{ RENDER3D_BACKDROP_..., GRID_THEME_AURORA }`. No saved-scene schema changes
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

## Scratch block assignment

`A[base] = {e0, ..., eN-1};` fills a contiguous run of scratch cells from one
source row. [USER_GUIDE.md](USER_GUIDE.md#writing-several-cells-at-once)
introduces the form; this is the rest of the rules.

The reason to reach for it is a 4x4 for `glMultMatrixf`: four rows of four
read as the matrix they are, and each row stays inside the 256-character
`MAX_LINE_LEN` budget that sixteen expressions on one line would blow past.

Three limits, each of which buys something concrete:

- **The base index must be a plain number**, not an expression. `A[n] = {…}`
  is rejected; write those cells one at a time with `A[n] = expr;`. The
  literal is what keeps the exported C a plain `A[4] = …; A[5] = …;` run on a
  single line - foldable back on import without a temporary, and with no
  chance of evaluating a base expression once per cell.
- **The run must fit.** `A[14] = {1, 2, 3}` runs off the end of the array and
  is rejected, as is a list of more than sixteen values.
- **A list needs at least two values.** `A[3] = {5}` is `A[3] = 5;` written
  the long way. It exports as the scalar form and would come back from a
  save/load as `CMD_SCRATCH_ASSIGN`, so it is rejected rather than shipped as
  a spelling that silently rewrites itself.
- **The row must fit its own exported C.** The *row* is short; the expansion
  need not be. Each cell becomes a `A[k] = …;` store, and a token like `TAU`
  lowers to a parenthesised float cast several times its source width - so
  sixteen `TAU` cells reach ~440 characters, past the 255 the importer will
  read back. Commit computes the exported width and refuses with
  `block is too long to export`; the remedy is the documented idiom, four
  cells to a row.

A fourth rule is about the target rather than the values: **the subscript is
required.** `A = {…}` is rejected. It would be the only place in the language
where an array name is itself assignable - `A = 5;` is an error everywhere
else - and export writes `A[0] = …` regardless, so import could only ever fold
it back subscripted. Requiring it also keeps the bases lined up when four rows
stack into a 4x4.

The committed row keeps the expressions you typed but standardises the
separators to `, `, so what you see is what a save and reload returns: import
rebuilds the list that way, and a row committed as `{1,2}` would otherwise
come back as `{1, 2}`.

### A cell that reads its own array

Don't write one. `A[0] = {t, A[0] + 100}` - a cell referring to the array the
row is writing - is the one construct where the three legs disagree. The full
flatten evaluates every cell before applying any of them, so `A[0]` reads its
pre-row value; the in-place rebake and the exported C both write as they go,
so the second cell sees what the first just stored. Same row, 101 or 105
depending on which path ran.

There is no reason to write it - the cells of one row are written together by
construction, and a genuinely sequential dependency is what the single-cell
`A[k] = expr;` form is for. The divergence is pinned as an XFAIL case
(`scratch_block_self_ref` in
[`tests/test_export_trace_parity.c`](../tests/test_export_trace_parity.c)),
whose annotation carries the full reasoning and why each available fix costs
more than the bug.

### Export shape

An array is not assignable in C, so the row lowers to the cell stores it
means - one C line per source row:

```c
A[4] = {c, s, 0, 0};   ->   A[4] = c; A[5] = s; A[6] = 0; A[7] = 0;
```

Import recognises a run of consecutive literal subscripts on one line and
folds it back. A lone `A[4] = c;` is left alone, which is what the two-value
minimum above protects.

The `static float A[16];` global the run needs is detected from the command
type rather than from a scan of the source text - the same route
`glMultMatrixf(A)` takes, since a row that only ever *writes* an array is as
much a reason to declare it as one that reads it.

### Why a block row cannot be plotted

Right-clicking a block row does nothing, and a `// @plot` tag on one is
ignored. The [assignment value
plot](USER_GUIDE.md#plotting-an-assignments-values) identifies a series by its
document row and nothing else, so a row producing
N values per execution has no single series to draw - plotting it would
interleave unrelated cells into one trace and label the result as one row's
history. Plot the cell you care about as its own `A[k] = expr;` row instead.

Replay annotation declines the row for the same reason: both the Expanded and
Verbose expansions resolve a source row to one flat command, and annotating a
block from the first of its N would describe a single cell as though it were
the whole row. The row renders unannotated rather than misdescribed.

## Config Variables (`// @config`)

The variable panel dims rows for variables the program assigns - a useful
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
match - `// @configured` does not count), it applies to every name on the
declaration line, extra comment text after the tag is fine, and it survives
export/import round trips via a marker on the exported `@declare` comment.
The two tags are independent and can be combined (`// @tune @config`):
`@tune` adds the exported-knob badge and keyboard controls, `@config` only
affects panel brightness inside gl-repl.

## Toggle Variables (`// @bool`)

Some variables are switches, not dials - `showVolume`, `wireframe`, a debug
overlay's on/off. The REPL has one numeric type, so a scene writes them as a
float and tests them with `> 0.5`:

```c
static float showVolume = 1; // draw the extruded hull itself @tune @bool
...
if(showVolume > 0.5) {
```

Dragging a slider to pick between two values is the wrong control for that.
Tagging the declaration `// @bool` says so:

- **In the variable panel** the row draws a checkbox in place of the numeric
  slider, and the value column reads `YES` / `NO`. Clicking the row toggles
  it - one press writes 1 or 0 to both the live value and the declaration,
  and there is no scrub. Right-click does the same thing: a flip has no
  coarse version.
- **In exported C**, a knob that is also `@tune`-tagged renders in the HUD as
  `q  showVolume [x]` rather than a number. It uses only the first-column key
  as a toggle; the paired lower key is omitted. The HUD keeps `[x]` / `[ ]` as
  its state indicator, and Shift/Ctrl step scaling does not apply.

The variable stays an ordinary `float` throughout: `@bool` is presentation,
not a type. "On" is the same `> 0.5` test the scene writes itself, so a value
the program leaves at 0.3 reads as off rather than as a third state.

Like `// @tune` and `// @config`, the tag is a bare trailing comment token
(whole-token match - `// @boolean` does not count), it applies to every name
on the declaration line, extra comment text is fine, and it survives
export/import round trips via a marker on the exported `@declare` comment.
All three tags are independent and can be combined. `@bool` on its own gives
the panel checkbox with no exported knob; a local (`float x;` inside a
function body) is rejected, since a local has no variable-panel row.

## Music & assets

gl-repl plays background music: any `*.mp3` it finds at startup, in filename
order, from three combined sources:

1. **`./assets`** next to where you run it - overridden by `--assets <dir>`
   or `GLR_ASSETS_DIR` (flag beats env).
2. **Bundled with the app** - the macOS `gl-repl.app` (from `make app`)
   ships a sample track inside the bundle.
3. **Your music folder** - `~/Library/Application Support/gl-repl/Music` on
   macOS, `$XDG_DATA_HOME/gl-repl/music` on Linux. Created on first run;
   drop `.mp3`s there and they join the playlist.

The repository ships only one sample track to stay lightweight. The optional
**music pack** is attached to the GitHub release tagged `assets-v1` - fetch
it into your per-user music folder with:

```bash
scripts/fetch-music.sh             # downloads the pack into the user music folder
scripts/fetch-music.sh --dir ./assets   # or anywhere else
make fetch-music                   # the same, into MUSIC_DEST (default ./assets)
make fetch-music MUSIC_TAG=assets-v2    # a different release
```

### The build-time prompt

`make gl-repl` offers to pull the pack the first time, defaulting to yes:

```
Download the optional gl-repl music pack (~94 MB) from release assets-v1? [Y/n]
```

The music is not part of the program, so this hook is written to be
unnoticeable rather than helpful-but-intrusive - it can neither stall nor
fail a build:

| Situation | What happens |
|---|---|
| You answer `n` | Remembered; you are never asked again |
| Pack already installed for that tag | Skipped without touching the network |
| No controlling terminal (CI, a piped build) | Skipped with a one-line note on stderr, and *not* remembered - a later interactive build still asks |
| `NO_MUSIC=1` | Skipped, no prompt |
| Download fails (offline, rate-limited) | Warns and continues; the build still succeeds |

The answer and the identity of the installed pack live in `.music.ini` at the
repo root (gitignored):

```ini
consent = yes
tag = assets-v1
tracks = 18
manifest = 8c96...        # sha256 of the sorted track list
```

`tag` + `manifest` together are what "already downloaded" means. The check is
deliberately **local-only** - it never asks GitHub what the release holds, so
a build that is already up to date costs no network at all. What that buys and
what it gives up:

- A track deleted, renamed, or added *on your disk* changes the manifest, so
  the next build re-fetches (`--skip-existing`, so only the missing files
  move).
- A release **re-uploaded under the same tag** is invisible: the local
  manifest still matches, and the pack stays as it is. Release tags are
  treated as immutable - publish a new pack as `assets-v2` rather than
  editing `assets-v1`, and point at it with `MUSIC_TAG`.

To force a re-check either way, change the tag or drop the cache:

```bash
make music-forget                  # or: scripts/fetch-music.sh --forget
```

`make fetch-music` ignores all of this - it always contacts the release, and
a failure there is a real error.

`./gl-repl --no-audio` starts silent. See *Music Asset Resolution* in
[`ARCHITECTURE.md`](ARCHITECTURE.md) for precedence details.

## Diagnostics

Stderr diagnostics help locate startup stalls, audio-worker hitches, and
backend audio-device issues:

- **Init trace** - `main()` logs a wall-clock line per startup phase
  (`[init +N.NNNs] <phase>`). A large gap names the slow phase;
  `--no-audio` isolates whether opening the OS audio device is the cause.
  `--detailed-prof` / `GLR_DETAILED_PROF=1` adds finer phases, including
  per-frame timing triples for the first two frames.
- **Audio-worker hitch detector** - any blocking audio lifecycle op (track
  load, stream teardown, advance) over the threshold logs
  `[init +N.NNNs] repl_audio: worker hitch: <op> took N ms`. Tune with
  `GLR_AUDIO_HITCH_MS` (default 50; `0` disables).
- **Miniaudio backend log** - `GLR_MINIAUDIO_LOG=debug` forwards miniaudio
  device/resource-manager logs to stderr with the same `[init +N.NNNs]`
  prefix when available. Leave it at the default warning/error level for
  normal runs; debug/info output is noisy and underrun messages depend on
  the active miniaudio backend.
- **GPU timer-query override** - `GLR_NO_GPU_PROF=1` leaves the CPU profile
  data on but disables GPU timings, useful when a driver advertises timer
  queries unreliably.
- **Profile dump to stderr** - `GLR_PROF_DUMP=N` prints the profile panel's
  rows every `N` frames: the catalog's labels, indented by nesting depth,
  carrying each section's running average in ms. Same data as the panel, but
  in a form a shell loop can diff - which is what makes A/B measurement
  (feature on vs. off, two machines, two drivers) practical without a window,
  a mouse and a screenshot. `GLR_PROF_DUMP_MIN_MS` sets the row cutoff
  (default `0.02`; `0` prints the whole catalog).

  The summary distinguishes callback time from cadence. `Frame Time` is the
  completed display callback; `Browser Wait` on WebAssembly (`Frame Wait`
  natively) is recorded on the next callback as start-to-start interval minus
  that Frame Time. Add those two rows to compare with the FPS interval. A large
  Browser Wait can include rAF/event-loop pacing or queued GPU back-pressure
  even when every CPU section and `Present` are small.

  **Read a dump as attribution, not as work.** A section containing a
  synchronous GL readback absorbs whatever pipeline drain the driver owes at
  that point, and on a render-ahead driver with vsync on that drain is most of
  a refresh interval. Vertex numbering is the worked example: `vertex nums`
  reports ~14 ms on NVIDIA, but ~12 ms of it is the driver's swap queue
  draining into the overlay's depth readback, not overlay work - with labels
  off the identical wait sits in `Present` instead. When a row's cost lands
  suspiciously close to the vsync period, re-run with `__GL_SYNC_TO_VBLANK=0`
  (or `__GL_MaxFramesAllowed=1`) before believing it; `make bench-vertex-labels`
  reproduces both sides of that comparison in isolation.

- **GL state dump** - `GL_STATE_DUMP=<prefix>` writes the live OpenGL 1.1
  state around the first main fill: `<prefix>.before` is what the frame hands
  the user program, `<prefix>.after` is what the program left. One
  `NAME=value` row per state variable in a fixed order, so `diff` is the
  reading tool:

  ```bash
  GL_STATE_DUMP=/tmp/app ./gl-repl scene.glr    # /tmp/app.before, /tmp/app.after
  diff /tmp/app.after /tmp/export.after
  ```

  The probe point is deliberately the boundary
  [`gl_state_inspector`](../src/repl/gl_state_inspector.c) predicts state for
  and the exported C program's `display()` reaches, so a scene that renders
  differently in the app than as exported C - or differently than the GL-state
  popup claims - can be settled by diffing two dumps instead of by guesswork.
  Any GLUT program can produce a comparable file by linking
  [`src/support/gl_state_dump.c`](../src/support/gl_state_dump.c) and calling
  `gl_state_dump_write_path()` at the matching point. Diagnostic only: a dump
  is ~250 glGet* round trips, each a pipeline sync. The controller hook still
  calls `getenv("GL_STATE_DUMP")` on every main fill when the variable is
  unset; `bench_repl --only getenv` prices that leftover scan against the
  one-time cached alternative the other frame-path env hooks use.

In-app, the CPU profiler overlay shows per-frame section timings and the
memory panel shows RSS history - see
[User Guide → Profiling](USER_GUIDE.md#profiling).

## Keyboard map tooling

Shortcut bindings live in [`keymap.h`](../keymap.h) as `GLR_*` `(key, modifiers)` pairs.
Use `scripts/keymap.sh` when changing or auditing them:

```bash
scripts/keymap.sh check   # fail on duplicate (key, modifiers) bindings
scripts/keymap.sh list    # show bound, reserved, and available slots
```

`list` reports the current bindings, the byte-level reserved control aliases,
and the unbound Ctrl / Ctrl+Shift / F-key slots. `Ctrl+H`, `Ctrl+I`,
`Ctrl+J`, and `Ctrl+M` arrive as Backspace, Tab, LF/Enter, and CR/Enter rather
than distinct app-shortcut bytes, so their plain forms remain reserved. Their
Shift forms are reported as assignable because a pre-editor route can claim
the modifier pair, with the tradeoff that it also consumes the corresponding
Shift+editing-key alias. Ctrl+Shift+H is claimed by Syntax highlight. The
report also derives the `GlrConfigKey` rows without shortcuts from the config
descriptor table plus the marked multi-key exceptions in `keymap.h`.

## Files

```
output.c        default save target (a standalone, compilable C file)
<workspace>/    managed scenes plus the authoritative .glr-workspace manifest
audio_state.ini persisted audio state (track, position, volume) - in the
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
