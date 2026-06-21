# OpenGL Immediate-Mode REPL

Interactive OpenGL command interpreter. Type GL commands, press `;` to execute,
and watch geometry render in real-time with a live code panel.

New to the tree? Start with [`MODULES.md`](MODULES.md) for the one-page
layered overview of the source files. This file is the agent-facing project
brief and goes deeper. The `src/repl` language pipeline (parse → compile →
apply → flatten → execute) has its own module-local docs:
[`src/repl/README.md`](src/repl/README.md) for orientation plus the
standalone `repl_demo`, and [`src/repl/ARCHITECTURE.md`](src/repl/ARCHITECTURE.md)
for the deep dive (data model, edit/frame flows, state ownership, host-effects
bridge, with a worked `repl_demo --trace`).

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
make gl-repl          # Build main binary (vendored static freeglut, macOS Cocoa)
make glut            # Build with system GLUT (macOS Apple framework fallback)
make test            # Build and run all tests (debug: ASan + UBSan)
make check-c99       # C99 ratchet (sample + demos + bench)
make freeglut-clean  # Drop the vendored freeglut CMake build (forces a rebuild)
make app             # (macOS) Bundle gl-repl.app with icon + sample.mp3
make clean           # Remove binaries
```

Requires: gcc with C99 support, OpenGL, GLUT/freeglut. On macOS the build also
needs **cmake** — `make gl-repl` (and the non-stub `make test`) builds the
vendored freeglut into a static `libglut.a` on first use.

### Vendored freeglut

On macOS, freeglut is vendored in-tree under `third_party/freeglut/` (full
upstream source) and built as a **static library with the native Cocoa backend**
(`-DFREEGLUT_COCOA=ON`), linked by archive path — no out-of-tree checkout, no
`libglut.dylib` rpath. The CMake build lands in `third_party/freeglut/build/`
(gitignored; survives `make clean`, so `make freeglut-clean` forces a rebuild).
`make glut` is the fallback that links Apple's system GLUT framework instead
(passes `FREEGLUT_VENDOR=0`). On **Linux** nothing is vendored — the build keeps
using the system `-lglut -lGL -lGLU`.

Re-pin the vendored version with `scripts/vendor-freeglut.sh [<ref>]` (default
`master`; the resolved SHA is recorded in `third_party/freeglut/VENDORED.txt`),
then `make freeglut-clean`. The source repo defaults to upstream but is
overridable with `FREEGLUT_REPO=<url-or-path>` (accepts a local fork clone) —
e.g. `FREEGLUT_REPO=~/src/freeglut-fork scripts/vendor-freeglut.sh
osmesa-backend` re-vendors from a fork carrying the OSMesa backend. Licenses for
freeglut and miniaudio are acknowledged in `THIRD_PARTY_LICENSES.md`.

### Headless OSMesa build (`make ... FREEGLUT_OSMESA=1`)

`FREEGLUT_OSMESA=1` builds the vendored freeglut with its **headless OSMesa**
(off-screen software/swrast) backend instead of Cocoa, and links the GL binaries
against Homebrew Mesa's `libGL`/`libGLU` + `libOSMesa` (rpaths baked, so no
`DYLD_LIBRARY_PATH` needed) rather than Apple's OpenGL framework. This renders
with **no window/display** — for headless geometry/feedback tests:

```bash
brew install mesa mesa-glu                 # one-time: OSMesa + GLU
make gl-repl FREEGLUT_OSMESA=1             # headless gl-repl
make gl-tests FREEGLUT_OSMESA=1           # the real-GL tests, headless
./build/release-osmesa/gl-repl --example 8 --export-ply out.ply --no-audio
```

It needs an OSMesa-capable vendored tree (re-vendor from a fork that has the
backend, as above). The OSMesa static lib and objects use distinct
`build-osmesa/` and `build/<cfg>-osmesa/` dirs, so the OSMesa and Cocoa builds
coexist without stale-binary collisions. Compatibility GL only (fixed-function +
GLU tess + `glRenderMode(GL_FEEDBACK)` all work). Pure build-system change — the
`-std=c99` sources are untouched; `gl_includes.h` already resolves the Mesa
headers on the non-Apple-GLUT path.

**Screenshots on `SIGUSR1` (OSMesa *and* native).** The vendored freeglut
captures the current frame to a PPM on `SIGUSR1` — no app code. On the OSMesa
backend it works even when the app is idle (it reads the last completed colour
buffer directly via `OSMesaGetColorBuffer`); on the **windowed backends**
(Cocoa/X11, fork branch `capture-windowed-backends`, vendored) the same
signal posts a redisplay and grabs `GL_BACK` pre-swap on the next frame —
real-GPU pixels (MSAA, true driver timings), so wait a few frames after
signaling, and don't signal before `glutInit` has run (the handler isn't
installed yet; default disposition kills the process). `kill -USR1 <pid>`
writes `<prefix>-NNNN.ppm` (`FREEGLUT_CAPTURE_FILE` env sets the prefix,
default `freeglut`); convert with `magick shot-0000.ppm shot.png`. This
capture support lives in the freeglut fork, and the in-tree vendored freeglut
**already carries it** — the pinned `capture-windowed-backends` branch is
stacked on `osmesa-backend`, so one vendored tree holds both the native
windowed *and* OSMesa capture paths. **No re-vendor is needed:** plain `make
gl-repl` (native Cocoa → the windowed `GL_BACK` path) and `make gl-repl
FREEGLUT_OSMESA=1` (headless → the `OSMesaGetColorBuffer` path) both get it
out of the box. You'd only re-pin (`scripts/vendor-freeglut.sh`) to upstream a
newer fork commit.

**Doc media regeneration (`scripts/docs-assets.sh`).** Regenerates every
screenshot/GIF under `docs/images/` (the media embedded in `README.md` /
`USER_GUIDE.md`); takes asset names as args (`--list`), default all;
`-j N` runs N assets in parallel (self-reexec via `xargs -P`, one
process per asset). Each asset is a staged snippet scene (`@cfg` headers
+ optional `// camera` block + `GLR_EDIT_LINE` for cursor-bound
overlays) captured via the record mode and keeping the **last frame**,
so frame-based settling (theme cross-fades ≈ 80 frames) is deterministic
— no sleeps. Antialiasing (no MSAA in the software rasterizer):
scene-only shots render at `--window 2400x1600` and downscale 50% (4x
supersampling); full-UI and hairline (grid/axes) shots stay 1x with
`GLR_ACCUM_PASSES=16` accumulation AA instead — bitmap fonts don't
scale (downscaling halves code-panel text) and supersampling dims 1px
grid lines to half intensity, while the 2D UI renders outside the
accumulation loop so jitter AA leaves both at full weight.

**Headless animations → GIF/MP4 (`scripts/record-gif.sh`).** `FREEGLUT_CAPTURE_FRAMES=N`
is the backend's record mode: it captures every rendered frame to a numbered PPM
and `exit(0)`s after N (serviced from `fgPlatformProcessSingleEvent`, the
backend's per-frame main-loop hook — the swap path is unreachable on a
single-buffered window). `scripts/record-gif.sh --example 2 --duration 3 --out ring`
records that headlessly and assembles `ring.gif` + `ring.mp4` via `ffmpeg`; the
knob is duration (clip length, fps-invariant). On the OSMesa backend the
capture lives in the backend files; the windowed backends share one core
`src/fg_capture.c` + three hooks (`glutInit` → init, `glutMainLoopEvent` →
tick, `glutSwapBuffers` → pre-swap grab), compiled to stubs on OSMesa builds.
Record mode works natively too: `FREEGLUT_CAPTURE_FRAMES=3 ./gl-repl ...`
opens a real window, writes N real-GPU frames, and exits.

### macOS app bundle (`make app`)

`make app` packages the binary into `gl-repl.app` so the Dock/Finder show
a real icon instead of the launching terminal's, and so a Finder launch
has music. It assembles `Contents/{MacOS/gl-repl, Resources/, Info.plist}`:

- **Icon** — `APP_ICON_SVG` (default `packaging/macos/gl-repl-soft-cube.svg`)
  is rasterized via `rsvg-convert` (needs `brew install librsvg`) into an
  `.iconset` and packed with `iconutil` into `gl-repl.icns`. Source SVGs +
  their generators live in `packaging/macos/` (`gen_soft_cube.py` →
  `make icon-cube` / `icon-cube-strong`; `gen_retro_a.py` → `make
  icon-regen`). Swap icons by repointing `APP_ICON_SVG`.
- **Music** — copies `assets/sample.mp3` into `Contents/Resources/assets/`.
  The binary finds it via source 2 above (`<exe>/../Resources/assets`),
  so the bundle ships with a track even though the Finder cwd is `/`. Only
  `sample.mp3` is bundled (small download; avoids shipping the full
  playlist); the user folder (source 3) is for adding more.

Build products (`gl-repl.app/`, `gl-repl.icns`, `gl-repl.iconset/`) are
gitignored; the committed `.svg`s are the source of truth. Pure packaging
— no source changes — so the `-std=c99` / Linux build is untouched.

The test targets (`test`, `test-detailed`, `test-stubs`, `test-full`)
default to `BUILD=debug`, which compiles with **AddressSanitizer +
UndefinedBehaviorSanitizer** (UB aborts: `-fno-sanitize-recover`).
`make gl-repl`/`bench`/the demos stay `BUILD=release`. An explicit
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
`#include "gl_includes.h"`. Source-backed modules keep paired `.c/.h` files at
the repo root; `include/` is for header-only helpers and vendored single-header
dependencies.

#### `#include` style

Project-local headers use quoted form; system / vendored headers use angle
brackets. `make check-include-style` (in `check-state-ownership`) enforces
this for the project-local set:

```c
#include <stdio.h>          // system / standard library  — angle
#include <GL/gl.h>           // system GL                  — angle
#include <miniaudio.h>       // vendored third-party       — angle
#include "keys.h"            // project-local              — quoted
#include "gl_includes.h"     // project-local (in include/) — quoted
#include "c_compat.h"        // project-local (in include/) — quoted
#include "support/cpuprof.h"    // project-local subdir       — quoted
```

The rule is mechanical: anything that lives in this tree uses `""`. The
guard tracks the bare-name set (`c_compat.h`, `gl_includes.h`, `keys.h`,
`gl_2d.h`) because those are the ones that ambiguously could be resolved
either way (they sit on `-I.` / `-Iinclude`).

### Local GL Stub Headers

This sample ships no-op OpenGL, GLU, and GLUT headers under
`tests/gl-stubs/include/` so machines without system GL development packages
can still compile and run non-rendering tests.

```bash
make test-stubs
make gl-repl USE_GL_STUBS=1
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
- After touching stubs, verify both paths: `make test-stubs`, `make gl-repl
  USE_GL_STUBS=1`, `make gl-repl`.

Header layout: `tests/gl-stubs/include/GL/gl.h` (fixed-function GL),
`tests/gl-stubs/include/GL/glu.h` (quadrics/projection/tessellator),
`tests/gl-stubs/include/GL/freeglut.h` (GLUT/freeglut callbacks + shapes);
`glext.h`, `glut.h`, `GLUT/glut.h`, `OpenGL/gl.h`, `OpenGL/glu.h` are
compatibility wrappers.

## Run

```bash
./gl-repl                  # Fresh session
./gl-repl output.c         # Reload saved session (single file)
./gl-repl workspace/       # Load every *.c under workspace/ as a user scene
./gl-repl --noaccum        # Disable the accumulation buffer (AA + motion blur)
./gl-repl --dump-code      # Print loaded buffer to stdout
./gl-repl --example 28 --flat-histogram   # Per-function / per-line flat-command budget breakdown (the --dump-* family honors --example)
./gl-repl --no-audio       # Skip audio init entirely (isolates startup stalls)
./gl-repl --assets ~/Music/glr  # Scan this dir for *.mp3 instead of ./assets (also GLR_ASSETS_DIR)
./gl-repl --detailed-prof  # Add fine-grained init-trace phases (default off)
./gl-repl --example torus  # Start on a built-in example (name, case-insensitive, or 0-based index)
./gl-repl --list-examples  # Print the built-in examples and exit
./gl-repl --time 5         # Set the initial animation time t at startup (also GLR_TIME; --time wins)
./gl-repl --example 8 --export-ply out.ply   # Capture an example's geometry to PLY on frame 1, then exit
./gl-repl --example 8 --export-ply out.ply --export-ply-srgb  # ...decoding vertex colors sRGB -> linear (color-managed viewers)
GLR_NO_POINT_PARAMETER=1 ./gl-repl   # Force the no-glPointParameterfv path
GLR_NO_GPU_PROF=1 ./gl-repl          # Disable GPU timer-query profiling (profile panel's GPU column reads "--")
GLR_AUDIO_HITCH_MS=10 ./gl-repl      # Lower the audio-worker hitch threshold
GLR_DETAILED_PROF=1 ./gl-repl        # Same as --detailed-prof, via env
GLR_ASSETS_DIR=/path/to/music ./gl-repl   # Same as --assets, via env (--assets wins)
GLR_TIME=5 ./gl-repl                 # Initial animation time t in seconds (--time wins)
GLR_EDIT_LINE=4 ./gl-repl scene.c    # Park the cursor on source line 4 (0-based, clamped) after load
GLR_ACCUM_PASSES=16 ./gl-repl        # Accumulation AA sample count (1/2/4/8/12/16; capture hook)
GLR_VIEW_TOGGLE_AT=0.5,2.0 ./gl-repl # Toggle 2D/3D view mode at t=0.5s and t=2.0s (records the swatch transition headlessly)
```

`GLR_EDIT_LINE` sets the cursor exactly as arrowing to the line would
(edit line + input buffer, via `glr_ctrl_set_edit_line`), so
cursor-bound overlays — transform guides, vertex labels — render on a
headless OSMesa capture with no keyboard input. Applied after the
file/example load, alongside `GLR_TIME`, in `main()` (`gl_repl.c`).

The animation variable `t` starts at `0` and (with animation playing, the
default) advances by a fixed `1/60 s` per rendered frame. `--time <secs>` /
`GLR_TIME` set its initial value at startup — applied after any `--example`
load (which resets `t`), so the override sticks. Useful headless: start an
animation capture from a later point in its timeline instead of always from
`t = 0`. Implemented as `repl_set_time()` (`src/repl/time.c` → `repl_state.c`),
read in `main()` (`gl_repl.c`).

`GLR_VIEW_TOGGLE_AT=<t1,t2,...>` is the capture affordance for the
**menu-bar 2D/3D swatch** transition (the pin left of Replay). It toggles
the view mode (`glr_action_toggle_view_mode`) once as the rendered-frame
clock crosses each listed second (frame `N` ↔ `t = N/60`). Because the
2D↔3D transition (`src/app/glr_ctrl_view_transition.c`) advances on the
animation *timer*, which the record/headless main loop doesn't fire per
captured frame, the hook also drives **one fixed-dt `glr_ctrl_tick()` per
captured frame** — so the transition (and the swatch's cross-fade / lit-cube
animation) advances deterministically, one frame of motion per frame
written. Lives in `display_func` (`gl_repl.c`), gated on the env var (no-op
when unset, so production timing is unchanged). Native record mode
(`FREEGLUT_CAPTURE_FRAMES`) shows the full UI; pair them to record the
swatch headlessly.

### Music assets & playlist sources

Music is `*.mp3` files discovered at startup; they play in filename
order. `build_mp3_playlist()` in `gl_repl.c` scans **three sources** and
concatenates them (each sorted independently, so each source keeps
filename order):

1. **Primary assets dir** — `./assets` relative to the working directory
   by default (the dev/CLI case). Overridden by `--assets <dir>` (highest
   precedence) or the `GLR_ASSETS_DIR` env var. `--assets` beats the env
   var beats the default.
2. **Bundled assets beside the executable** — `<exe>/../Resources/assets`,
   resolved via `_NSGetExecutablePath` (macOS) / `/proc/self/exe`
   (Linux). This is the macOS `.app` case: `make app` copies
   `assets/sample.mp3` into `Contents/Resources/assets/`, so a
   Finder-launched bundle (whose cwd is `/`, where source 1 finds
   nothing) still has music. The bundle subfolder name is fixed
   (`AUDIO_ASSETS_DIR`); `--assets` does not change it.
3. **Per-user music folder** — `user_music_dir()`:
   `~/Library/Application Support/gl-repl/Music` on macOS,
   `$XDG_DATA_HOME/gl-repl/music` (or `~/.local/share/gl-repl/music`)
   elsewhere. Created on first run (`ensure_dir`) and announced once on
   stderr (`repl_audio: add more music in <dir>`) so users have a place
   to drop their own tracks.

If all three turn up zero `.mp3`s, it falls back to the single-file
`AUDIO_DEFAULT_MUSIC` (`assets/song.mp3`). `--no-audio` skips audio
entirely. Adding a new candidate source or changing the per-user path is
a `gl_repl.c` change (all of this lives in that one TU's statics); the
platform branches in `executable_dir` / `user_music_dir` are
`#ifdef`-guarded and **must stay C99 / portable** (see *Windows port*
plan for the Windows branches).

### Startup & audio-worker diagnostics

Two always-on stderr diagnostics help locate startup stalls and
audio-thread hitches (the kind seen on slow Linux disks):

- **Init trace.** `main()` in `gl_repl.c` logs a wall-clock line per
  startup phase (`[init +N.NNNs] <phase>`) via `gettimeofday`. Two
  granularity levels share one stream:

  *Baseline* (always emitted): `start`, `glutInit begin`, `window
  created`, `GL init done`, `REPL bootstrap done`, `glr_audio_init
  begin/done`, `audio playlist started`, `entering main loop`. A
  large gap names the slow phase; `--no-audio` isolates whether
  `ma_engine_init()` (the one synchronous audio call on the `main()`
  path — it opens the OS audio device) is the cause.

  *Detailed* (gated on `--detailed-prof` or `GLR_DETAILED_PROF=1` env;
  default off): `glutInit done` (splits glutInit runtime), `playlist
  scan done (N tracks)` (opendir/readdir on `assets/`), `playlist
  start requested` (after the synchronous `load_state` read of
  `audio_state.ini` and the worker poke), and the post-loop `frame N
  display callback` / `frame N render done` / `frame N swap done`
  triples from `display_func` for the first two frames. The frame-1
  triple splits the otherwise-invisible time between `glutMainLoop()`
  and the OS showing pixels (GLUT solid-shape display-list
  compilation, macOS first-drawable wait, GL stack lazy init); the
  frame-2 triple is the side-by-side control that reveals whether
  spending was first-frame-only (the expected case) or a steady-state
  regression.
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
Two hard-failing guards run *outside* that aggregate: `check-duplicate-api-decls`
(no duplicate function declarations across public headers) and
`check-trailing-whitespace` (commits since `origin/main` carry no trailing
whitespace; also wired into `test-stubs` and the pre-push hook).

## Plans & audits

Long-form audit / implementation docs live under `plans/` and move
between subdirectories as their state changes. Five buckets:

| Directory | Meaning |
|---|---|
| `plans/not-started/` | Drafted but no commits yet — green-field plans waiting on a slot. |
| `plans/in-review/` | A reviewer is actively reading the doc; no implementation has begun. |
| `plans/active/` | Implementation is in flight. Some commits have landed; the doc's status table tracks what's done vs. deferred. |
| `plans/partial/` | Implementation stalled mid-way (intentional pause) and the residual scope is still meaningful. |
| `plans/done/` | Fully completed (the bulk of the directory — closed audits, finished refactors). |

The transitions go `not-started → in-review → active → done` for the
happy path, with `partial` as the side branch when a plan ships some
of its scope but defers the rest. Don't graduate a doc to `done/` if
its status table or deferred-follow-ups block still describes
unfinished work — keep it in `active/` (or move to `partial/` if the
gap is intentional) so future readers see the residual scope at the
state-machine level, not buried in the doc body.

## File Layout

| File | Responsibility |
|------|----------------|
| `gl_repl.c` | GLUT callback registration, `main()`, window setup, buffer swap; forwards directly to `glr_ctrl_*` |
| `gl_repl.h` | Minimal legacy header: standard includes and `M_PI`; types/defaults moved out to dedicated headers |
| `src/app/glr_ctrl.c` | App-frame controller: `glr_ctrl_display_frame`, `glr_ctrl_reshape`, `glr_ctrl_init_gl`; builds `SceneRenderConfig`, calls scene/UI renderers |
| `src/app/glr_ctrl.h` | Controller public surface: display, reshape, init-GL entrypoints |
| `src/app/glr_config.c` | Config key implementation and descriptor table helpers. The tail of `glr_config_set` notifies the tutorial runner (`tutorial_notify_state_changed`) so REQUIRE steps observe every write path — direct setters (e.g. accum-passes Ctrl+=/-), `glr_cfg_cycle_row`'s early-return branches, and the bridge's `apply` during `@cfg` / example / workspace load |
| `src/app/glr_config.h` | `ReplConfigKey` / `ReplConfigItem` descriptor API for keyed config access |
| `src/repl/command.h` | Core command model types: `CmdType` enum, `GLCmd` struct (pure parse-result: type, args, flags, provenance — no `source[]` field) |
| `src/repl/compile.c` | Pure source-text validators that produce `ReplCompiledChange` descriptors; never mutates state |
| `src/repl/compile.h` | `ReplCompiledChange`, `ReplCompileResult`, `ReplCompileContext`, compile entry points |
| `src/repl/apply.c` | Applies a `ReplCompiledChange` to `ReplState` command arrays |
| `src/repl/apply.h` | Apply public API (`repl_apply_compiled_change`, `repl_apply_predef_ops`) |
| `src/repl/normalize.c` | Parse-and-normalize pipeline (`repl_parse_and_normalize*`) |
| `src/repl/reformat.c` | Source reformatter (`repl_reformat_program`) |
| `src/repl/bootstrap.c` | Startup loading helpers (`repl_load_initial_commands`) |
| `src/repl/parser.c` | REPL source-line parser, expression validation, canonical line text emission via `ReplParsedLine.text` (the per-line text lives in `EditorState`'s editor buffer, not on `GLCmd`) |
| `src/repl/parser.h` | Parser entrypoints (`repl_parser_parse_command*`, `repl_parser_parse_command_ctx`), `ReplParseContext`, `ReplParsedLine` |
| `src/repl/source_scope.c` | Source prefix-depth cache, indentation helpers, block lookup |
| `src/repl/source_scope.h` | Source-scope query API (`repl_source_scope_block_depth_at`, `repl_source_scope_find_block_end`, indent helpers) |
| `src/repl/command_spec.c` | Command type metadata and specifications (parsing, formatting, completion requirements) |
| `src/repl/command_spec.h` | Command spec query API |
| `src/repl/command_store.c` | Low-level `GLCmd` array mechanics: insert, delete, replace, bulk-load (no text-buffer writes) |
| `src/repl/command_store.h` | Command-store public API (`repl_command_store_insert_one`, etc.) |
| `src/repl/state.c` | Owns `g_repl_state`, lifecycle, snapshot assembly (`repl_state_capture` / `repl_state_restore`) |
| `src/repl/state.h` | Typed runtime-state facade, reset helpers, and focused accessors over the live REPL state |
| `src/repl/state_views.h` | Read-only (by-value) state getters; safe to include from `scene_*` and `ui_*` |
| `src/repl/state_owners.h` | Mutable `_mut()` accessors; owner modules and controller only. |
| `src/repl/cfg_baseline.h` | Config bag API plus typed live-cfg helpers (`repl_cfg_get_int` / `_set_int` / `_known` / `_set_text` / `_resolve_text`) used by `src/subsystems/tutorial/tutorial_runner.c` for SET / REQUIRE handling and cfg baseline restore |
| `src/editor/input.c` | **REPL editor input dispatcher**: REPL key bindings (`;` commit, Tab autocomplete, Ctrl+R reformat, tutorial guards, comment toggle) + REPL-flavored orchestration on top of `edit_ops` primitives. Non-editor routing (replay, audio, config, save, camera) lives in `src/app/glr_ctrl.c`. The generic counterpart for `editor_demo` is `tools/editor_demo/input.c`. |
| `src/editor/input.h` | Editor input dispatch entry points + `EditorInputDispatchEffects` typedef + `editor_input_active_modifiers` test seam |
| `src/editor/edit_ops.c` | Generic text-editing primitives shared by `src/editor/input.c` (REPL dispatcher) and `tools/editor_demo/input.c` (generic dispatcher): char insert/delete at cursor, input-selection consume, type-char and backspace (selection-aware). REPL-free; locked by `check-edit-ops-pure`. |
| `src/editor/edit_ops.h` | `edit_op_*` primitive declarations |
| `src/editor/commit.c` | Editor-side commit transaction boundary: compile via `repl_compile`, undo snapshot, text-buffer write, REPL apply, dirty-state updates |
| `src/editor/commit.h` | Commit orchestration API (`editor_commit_apply_external_change`, `editor_try_commit_*` helpers) |
| `src/editor/state.c` | Owns `EditorState`: editor buffer, cursor, selection, search, autocomplete, scroll, undo/redo, transformers, highlights, virtual lines |
| `src/editor/state.h` | `EditorState` typed facade, `EditorBufferView`, `editor_state_input/search/autocomplete` accessors |
| `src/editor/limits.h` | Shared editor input and autocomplete capacity constants |
| `keys.h` | Physical ASCII / control-key byte constants (Ctrl+A=1 … Ctrl+Z=26, `KEY_ESC`, etc.) in `include/` (project-agnostic). The action→key bindings on top live in `keymap.h` |
| `src/editor/clipboard.c` | Line selection anchors, command clipboard buffer, copy/cut/paste behavior |
| `src/editor/clipboard.h` | Clipboard public API |
| `src/editor/undo.c` | Undo/redo snapshots, history rings, example auto-promote hook before mutation |
| `src/editor/undo.h` | Undo public API (`editor_undo_push_snapshot`, `editor_undo_pop_snapshot`, `editor_undo_do_redo`) |
| `src/app/glr_camera.c` | Scene camera pointer state, orbit/pan/zoom drags, wheel zoom velocity, momentum tick |
| `src/app/glr_camera.h` | Camera state + setters (`glr_camera`, `glr_camera_set_*`, `glr_camera_controls_reset`) |
| `src/app/glr_actions.c` | Config descriptor table, config shortcuts, menu actions |
| `src/app/glr_actions.h` | Actions public API (`glr_action_menu_item_activate`, etc.) |
| `config.h` | Project-wide compile-time configuration constants (force-included into every TU via `-include config.h`). Also `#include`s `keymap.h` so the key bindings reach every TU |
| `keymap.h` | Keyboard shortcut bindings: one `#define GLR_<ACTION>  <key>, <mods>` pair per action — the single place to reassign a shortcut. Matched via `keymap_event_is(key, GLR_X)` (call sites never spell out modifiers); `KM_KEY`/`KM_MODS` extract one element for `case` labels / struct fields. Zero includes (tokens resolve lazily at the dispatch site, like `config.h`'s `FONT_*`); consumed by `g_cfg_items[]` (via `KM_KEY`/`KM_MODS` designated initializers), the `glr_ctrl_router_*` handlers, and the editor input dispatcher. Guarded by `make check-keymap-no-dup`; `make keymap-list` prints bindings + free slots (`scripts/keymap.sh`). Sits at root (project-specific config), not `include/` (project-agnostic) |
| `prof_sections.h` | CPU-profile section catalog: the `ProfSection` enum + `PROF_SECTION_COUNT`, force-included via `-include prof_sections.h`. Keeps `src/support/cpuprof.{c,h}` host-agnostic (they fall back to `typedef int ProfSection` when it's absent). Per-section *labels* are not here — see `src/app/glr_prof.c` |
| `src/app/glr_defaults.h` | Controller-side scene/presentation defaults (`CFG_DEFAULT_*` macros) |
| `src/ui/core/text_layout.c` | Pure code-panel wrapping, row counts, segment lookup, cursor-row mapping |
| `src/ui/core/text_layout.h` | `CodeLayout` / `CodeWrapIter` API shared by UI, export dumps, tests |
| `src/ui/app/repl_code_panel.c` | REPL-specific code-panel adapter: row building, scroll-follow layout, render/hit bridging |
| `src/ui/app/repl_code_panel.h` | `UiReplCodePanelLayout` plus REPL adapter render/hit/layout entrypoints |
| `src/repl/executor.c` | Narrow live-GL dispatch: walks the flat command array emitting OpenGL calls |
| `src/repl/executor.h` | Executor public API (`repl_execute_program`, transform helpers) |
| `src/repl/flatten.c` | Source-to-flat program builder: unrolls loops, inlines functions, resolves if-blocks |
| `src/repl/flatten.h` | Flatten public API (`repl_flatten_program`, `repl_flatten_commands`) |
| `src/repl/flatten_query.c` | Live flat-program query helpers: cursor matching, current-block highlight refresh, and per-line flat-cost attribution |
| `src/repl/flatten_query.h` | Flatten query public API (`repl_flat_cmd_matches_cursor`, `repl_flatten_cost_at_line`, `repl_flatten_refresh_current_block_highlight`) |
| `src/repl/pipeline.h` | Pipeline and lifecycle surface for frame orchestration (flatten, autonormal, replay snapshots) |
| `src/repl/autonormal.c` | Auto-generated `glNormal3f` maintenance for source commands |
| `src/subsystems/replay/replay.c` | Replay-side walkers for tess preview and user-vertex traversal |
| `src/subsystems/replay/replay_fade.c` | Fade batch ring ownership, skip-limit planning, and fade lifecycle |
| `src/subsystems/replay/replay_input.c` | Replay keyboard and special-key routing |
| `src/subsystems/replay/replay_internal.h` | Replay-private shared constants and helpers for the split translation units |
| `src/subsystems/replay/replay_playback.c` | Replay state machine: start/stop, seek/advance, speed, and baseline snapshots |
| `src/subsystems/replay/replay.h` | Replay public API (`replay_start`, `replay_toggle_play_pause`, etc.) |
| `src/editor/search.c` | Case-insensitive substring search state and match navigation |
| `src/editor/search.h` | Search query helpers and input routing API |
| `src/app/glr_completion.c` | REPL-side completion provider: walks command spec / predef vars / `CMD_FUNC_DEF` for matches, ghost text, parameter hints. Registered via `EditorCompletionProvider`. |
| `src/ui/app/layout.c` | Pure window layout geometry: scene/code-panel rect derivation plus shared menu-bar anchoring |
| `src/ui/app/layout.h` | Layout geometry API (`ui_layout_scene_rect`, `ui_layout_code_panel_rect`, `ui_layout_menu_bar_rect`) |
| `src/ui/app/overlay_layout.c` | Layout engine for the floating scene-overlay panels (variable / FPS plot / profile / memory): pure bottom-up right-column stacking solve above the statusbar + replay-HUD band with column spill (panels can't overlap), plus the eased positions every panel glides on (the old variable-panel-only `replay_lift_px`, generalized). Controller ticks it once per frame; view builders read resolved positions, falling back to pure solve targets when unticked (tests/headless). Anchor-bound popups (autocomplete, color picker, dropdowns) stay out by design |
| `src/ui/app/overlay_layout.h` | Engine API: `UiOverlayPanelId`/`UiOverlayLayoutIn`, `ui_overlay_layout_inputs/_solve/_tick/_panel_pos/_last_band_h/_reset` |
| `src/ui/core/layout_utils.h` | Header-only pure layout helpers shared across UI layers (`ui_clamp_panel_y`) |
| `src/repl/scenes.c` | User-scene slots, LRU eviction, workspace save/load orchestration, workspace dir binding |
| `src/repl/scene_snapshot.c` | Copy/apply helpers for scene snapshots: commands/text, cursor, variables, scratch arrays, function aliases, scene-local cfg, and camera text |
| `src/repl/workspace_io.c` | Workspace filesystem + scene-file naming mechanics: recursive `mkdir -p`, `.c`-extension test, basename→scene-name, slug derivation + collision suffixes. Pure (no `g_user_scenes`/live state); `scenes.c` drives it |
| `src/repl/example_loader.c` | Built-in example loading and active-example tracking |
| `src/app/glr_debug.c` | Diagnostic dumps for CLI flags and tests |
| `src/app/glr_debug.h` | Debug dump public API |
| `src/subsystems/replay/replay_annotations.c` | Replay-time source annotations, variable substitution, evaluated command display text |
| `src/subsystems/replay/replay_annotations.h` | Code-panel replay annotation API |
| `src/ui/app/snapshot.h` | `UiRenderSnapshot` — frame-frozen bundle built once per frame by `glr_ctrl_build_ui_snapshot()` |
| `src/ui/app/editor.h` | Per-frame editor-overlay snapshots (swatches, sliders, highlights) pushed by the controller |
| `src/ui/subsystems/replay_hud.c` | 2D replay status HUD (feature-UI under the `replay_ui_*` prefix; reads replay peer snapshot) |
| `src/ui/subsystems/replay_hud.h` | Replay HUD render entrypoint |
| `src/ui/support/cpuprof.c` | Compute-profile overlay surfaces: the section listing panel (per-section CPU, GPU, and Max columns; Max = worse of the two EMAs — deliberately not paired per-frame samples; see the NOTE at its computation) and the separate FPS plot panel (10s/1m/10m series from the `prof_fps_*` history). Ctrl+W cycles Off / Plot / Sections / Details |
| `src/ui/support/cpuprof.h` | Profile panel render entrypoint |
| `src/ui/support/memprof.c` | Memory profiling overlay panel (RSS history / baseline / delta) |
| `src/ui/support/memprof.h` | Memory panel render entrypoint |
| `src/ui/app/menu_bar.c` | Code-panel menu bar, dropdowns, config right-click handling, search slot; mouse-wheel scroll for flyouts taller than the viewport |
| `src/ui/app/menu_bar.h` | Menu/pin hit-test and dropdown state API |
| `src/ui/app/scene_tabs.c` | Scene tab strip below the menu bar: snapshot-pure render + whole-band hit-test (TAB / inert CHROME); derived each frame, no persistent model |
| `src/ui/app/scene_tabs.h` | Scene tab strip render/hit/`band_h` API |
| `src/subsystems/color_picker/color_picker_state.c` | Floating color picker peer: state, lifecycle, slider input handlers, source-line writeback through editor commit |
| `src/subsystems/color_picker/color_picker_state.h` | Peer API (`ColorPickerView`, `ColorPickerInputResult`, `color_picker_open/close/handle_*`, `color_picker_hsv_to_rgb`) |
| `src/ui/subsystems/color_picker.c` | Floating color picker renderer + hit-test (pure, takes `ColorPickerView *`) |
| `src/ui/subsystems/color_picker.h` | Picker UI render/hit-test API + `UI_COLOR_SWATCH_W` |
| `src/ui/core/tabbed_overlay.c` | Generic modal tabbed text overlay renderer (the F1 help overlay's UI shell) |
| `src/ui/core/tabbed_overlay.h` | Tabbed-overlay render API (`UiOverlayState`, `UiOverlayContent`) |
| `src/repl/help_text.c` | Builds neutral F1 help text tables (commands, key bindings); `glr_ctrl` adapts them to `UiOverlayContent` |
| `src/repl/help_text.h` | Help-content public API |
| `src/ui/subsystems/variable_panel.c` | Floating variable slider panel rendering, geometry, and hit-test |
| `src/ui/subsystems/variable_panel.h` | Variable panel render/rect/hit API |
| `src/ui/app/autocomplete_panel.c` | Floating autocomplete popup renderer (reads autocomplete state populated by `src/app/glr_completion.c`) |
| `src/ui/app/autocomplete_panel.h` | Autocomplete popup render entrypoint |
| `src/editor/inline_rename.c` | Inline scene-rename input buffer and key handling (status-bar overlay) |
| `src/editor/inline_rename.h` | Rename begin/active/cancel/key/special API |
| `src/subsystems/variable_panel/variable_panel_drag.c` | Variable slider drag transaction: begin/motion/reset, linear/log value writeback |
| `src/subsystems/variable_panel/variable_panel_drag.h` | Drag state accessors + begin/motion/reset API |
| `src/subsystems/variable_panel/variable_panel_state.c` | Variable-panel peer subsystem: owns visibility flag + drag-state storage |
| `src/subsystems/variable_panel/variable_panel_state.h` | Peer-subsystem facade (`VariablePanelState`, capture/restore/reset, view/drag accessors) |
| `src/subsystems/replay/replay_state.c` | Replay peer subsystem: owns `ReplayRuntimeState` storage |
| `src/subsystems/replay/replay_render.c` | Replay fade-batch GL rendering pass (`glPushAttrib`/`glMaterialfv`/`glBegin`), extracted out of `src/scene/render.c` |
| `src/subsystems/edit_overlays/edit_overlays.c` | Cursor edit-guide + vertex/normal overlay orchestration: owns the cursor-guide snapshot and the flat-program walk that calls the scene overlay primitives; extracted out of `src/app/glr_ctrl.c` |
| `src/subsystems/replay/replay_state.h` | Peer-subsystem facade (`replay_state_capture/restore/reset/view/mut`) |
| `src/editor/help_session.c` | Read-only editor session for the help overlay (tab_idx + scroll) |
| `src/editor/help_session.h` | `EditorHelpSession` API (capture/restore/reset, narrow accessors) |
| `src/editor/completion.c` | Completion-provider registry: editor input invokes registered provider for autocomplete |
| `src/editor/completion.h` | `EditorCompletionProvider` struct + `editor_completion_register/update/clear` API |
| `src/repl/examples.c` | Predefined example data (`g_examples[]`, `g_example_names[]`) |
| `src/repl/examples.h` | Example query API (`repl_example_count/name/lines`) |
| `src/repl/tutorials.c` | Built-in tutorial catalog: per-tutorial null-terminated `TutorialStep[]` array + name + optional leading `@cfg` line array (same slug vocabulary as example `@cfg`) + `tags` bitmask (private `TUTORIAL_TAG_*` macros + `g_tutorial_tag_labels[]`) + optional `subheading` string (free-form in-flyout section label). Each step carries a `TutorialStepKind`: COMMAND (type the expected GL call), SET (apply `cfg_slug = cfg_value` on entry, advance on Enter/Tab/Space), REQUIRE (advance when the user sets the slug to the target value), or REQUIRE_VAR (advance when the predef variable `var_name` reaches `var_target`, via a typed commit or a variable-panel slider drag). A REQUIRE_VAR step whose variable does not exist yet is a *declaration* step: the satisfying `float n = 1;` relocates to the document top (above any comment), so the runner skips the separate locked instruction comment and instead rides the instruction on the autocomplete ghost as a TRAILING comment on the decl line (`float n = 1; // ...`, synthesized in `tutorial_shadow_suffix`) — the comment commits as part of the decl and travels with it. See `tutorial_enter_step`; the tutorial does not pre-declare variables. The commit-side advance is gated on `tutorial_note_expected_commit_applied()` returning a pending COMMAND attempt, so a declaration commit (whose notify already advanced onto the next COMMAND step) does not double-advance and skip that step. Sentinel is `comment == NULL`. Starter set: First Triangle (`@cfg view_mode = 1`; tag `GEOMETRY`; subheading `Beginner`), Color & Transform (`COLOR_TRANSFORMS`; `Beginner`), Feature Tour (`GEOMETRY`; `Beginner`; mixes COMMAND + REQUIRE + SET), Variable Slider (`COLOR_TRANSFORMS`; `Beginner`; declare `n` via REQUIRE_VAR, draw a triangle whose vertices use `n` via COMMAND steps, then a final REQUIRE_VAR drags the `n` slider to grow it), Depth Test Triangle (`GEOMETRY \| DEPTH_LIGHTING`; `Intermediate`) |
| `src/repl/tutorials.h` | Catalog query API (`repl_tutorial_count/name/step_count/step_comment/step_expected/step_kind/step_cfg_slug/step_cfg_value/step_var_name/step_var_target/cfg_lines/subheading`) + tag query API (`repl_tutorial_tag_count/_label/_mask/_has_tag/_count_for_tag/_index_for_tag/_visible_tag_count/_visible_tag_at`, inline `_tag_bit`) + `TutorialEntry` + `TutorialStepKind` + `ReplTutorialTagMask` typedefs + `REPL_TUTORIAL_TAG_*` enum (synthetic `ALL` folded into every entry's mask) |
| `src/subsystems/tutorial/tutorial_state.c` | Tutorial peer subsystem: owns `TutorialRuntimeState` (active flag, step, locked_lines, fade timing, pending commit, instruction-line map, last match result). `fade_duration` is set per-line at emit time from `TUTORIAL_FADE_CHARS_PER_SEC`, not held as a constant |
| `src/subsystems/tutorial/tutorial_state.h` | Peer-subsystem facade (`tutorial_state_view/_mut/_reset`, `tutorial_active`), `TutorialMatchKind/Result` types |
| `src/subsystems/tutorial/tutorial_runner.c` | Tutorial runner: starts/exits/advances, emits instruction comments via `repl_load_apply_line`, captures/restores cfg baselines, enforces locked-line/source guards, and handles SET/REQUIRE/REQUIRE_VAR progression without recursion (a REQUIRE_VAR step for a not-yet-declared var is a declaration step: no separate locked comment, instruction rides the ghost as a trailing comment on the decl line) |
| `src/subsystems/tutorial/tutorial_animation.h` | Pure fade helper surface: `TutorialFadeView` plus `tutorial_fade_*` queries over frozen timing data (shared by the snapshot/render path and tests) |
| `src/subsystems/tutorial/tutorial_animation.c` | Implementation of the pure tutorial fade helpers; no live tutorial-state reads |
| `src/subsystems/tutorial/tutorial_match.c` | Tutorial command matching, normalization, expected-message formatting, and ghost-text shadow suffix helpers |
| `src/subsystems/tutorial/tutorial_internal.h` | Tutorial-private shared declarations for the split runner / animation / match files |
| `src/subsystems/tutorial/tutorial.h` | Runner API: `tutorial_start/_exit/_teardown/_handle_commit_attempt/_advance_after_successful_commit/_current_expected_text/_current_step_kind/_notify_state_changed/_handle_ack_key/_block_noncommand_commit/_line_is_locked/_guard_source_change/_match`. Knobs: `TUTORIAL_FADE_CHARS_PER_SEC` (reveal rate), `TUTORIAL_FADE_SETTLE_CHARS` (settle-wave width) |
| `src/repl/export.c` | Writer half (audit #69 split). `repl_export_save_output`, `repl_dump_code_panel_text`, workspace header emit dispatcher `repl_state_refresh_workspace_header_lines`, scaffold sections, render-state/cam refresh, init-bootstrap apply, light/render text generators. Also implements the typed live-cfg wrappers `repl_cfg_get_int` / `_set_int` / `_known` over the installed config bridge (bridge-only — no `scene_*`/`glr_*` calls; `check-repl-export-via-bridge` stays green) |
| `src/repl/import.c` | Reader half (audit #69 split). `repl_export_load_from_file`, the pending-`@cfg` accumulator + `repl_export_apply_pending_cfg`, deferred-`@var` table, workspace directive readers (`parse_workspace_dir` / `_scene_name` / `_var` / `_func_alias` / `_cfg`) and the `repl_state_parse_workspace_header_line` dispatcher, snippet directive table (`@declare`), C-to-REPL line translators (for-headers, function headers, tess lines, `glPointParameterfv`, `label()`), and the line-by-line `ImportState` machine. The `IMPORT_EXPORT_STATE` macro block is duplicated verbatim with `src/repl/export.c`; both TUs reach the same state-owner facade. |
| `src/repl/export.h` | Export/import public API and workspace-header pending-state types |
| `src/repl/export_state.h` | Shared dimensions for import/export state text |
| `src/app/glr_audio.c` | App-level playlist engine and persisted audio config |
| `src/app/glr_audio.h` | Audio playback API (`glr_audio_*`) |
| `src/app/glr_mesh_export.c` | PLY mesh export: one `glRenderMode(GL_FEEDBACK, GL_3D_COLOR_TEXTURE)` capture of the live flat program under a fixed identity-modelview + ortho + viewport + identity-texture transform (lighting/cull off, fill mode), buffer grow/retry, then hands the raw stream to `mesh_ply_write`. Captures user `glVertex`, GLU tess, and the GLUT solids through one path. Runs the executor with `encode_feedback_normals` so authored per-vertex normals ride the texcoord channel. State saved/restored so the visible frame is undisturbed |
| `src/app/glr_mesh_export.h` | `glr_export_mesh_ply(path)` — capture + write, returns triangle count or `<0`; sets the status message |
| `src/support/mesh_ply.c` | **Pure** (no-GL) PLY writer: parses a `GL_3D_COLOR[_TEXTURE]` feedback float stream, inverts ortho+viewport+depth-range → world coords, fan-triangulates, welds, emits ASCII PLY. Per vertex uses the **authored** world-space normal recovered from the texcoord channel when the out-of-band `glPassThrough` marker says so (else synthesizes + smooths a geometric normal — GLUT solids, GLU tess). Feedback token markers + `MESH_PLY_PASS_*` sentinels defined locally (no GL header); `glr_mesh_export.c` `STATIC_ASSERT`s them against `GL_*_TOKEN`. Optional `MeshPlyOptions.srgb_decode` (off by default; CLI `--export-ply-srgb`) decodes the captured RGB sRGB→linear before quantizing — the REPL has no color management so glColor values are display-referred; color-managed viewers that read PLY colors as linear otherwise render them washed out (alpha stays linear) |
| `src/support/mesh_ply.h` | `MeshPlyCapture` (incl. `floats_per_vertex` = 7/11) / `MeshPlyOptions` + `mesh_ply_write()`; local `MESH_PLY_TOK_*` token enum + `MESH_PLY_PASS_NORMALS`/`_NO_NORMALS` markers |
| `src/support/cpuprof.c` | CPU wall-time profiling instrumentation (per-section accumulators, frame tick); host-agnostic — sections are opaque ints, catalog injected via `prof_sections.h`. Carries the optional per-section begin/end hook seam (`prof_install_section_hooks`) the app uses to bracket GPU sections without touching call sites |
| `src/support/cpuprof.h` | Profiling API (`prof_begin`, `prof_end`, `prof_frame_tick`, etc.); no UI/app dependency — `#ifndef PROF_SECTIONS_PROVIDED` fallback makes it compile with no catalog (`check-cpuprof-standalone`). Declares `prof_section_info()` (per-section label/depth/is_total), implemented per-binary |
| `src/support/gpuprof.c` | GPU time profiling via GL timer queries, keyed on the same `ProfSection` ids. Two modes picked at init: **timestamp** (`glQueryCounter(GL_TIMESTAMP)`, ARB_timer_query / GL 3.3 — one marker per transition, interval deltas tile the GPU timeline so sums are additive; the Linux/Mesa path) and **elapsed** (`GL_TIME_ELAPSED_EXT` brackets, the EXT_timer_query fallback — Apple GL 2.1; windows overlap on tile-deferred GPUs, see the NOTE in gpuprof.h). Asynchronous by design: per-frame query slots in a 4-deep ring, polled with `GL_QUERY_RESULT_AVAILABLE` and harvested 1-3 frames later — never a glFinish. Begin/end slice the frame into non-overlapping segments tagged with the open-section mask. GL-free TU: entry points injected as function pointers at init (loaded in `glr_ctrl_init_gl`, same proc-loader pattern as glPointParameterfv; `GLR_NO_GPU_PROF=1` forces off), GL tokens defined locally (mesh_ply precedent). See *Runtime GL Capability Detection* in `ARCHITECTURE.md` |
| `src/support/gpuprof.h` | GPU profiler API (`gpu_prof_init/frame_begin/begin/end`, `gpu_prof_section_avg_us/_has_data`, `gpu_prof_uses_timestamps`) + the injected `GpuProfGlFns` table (`query_counter` optional → timestamp mode) |
| `src/app/glr_prof.c` | gl-repl's `prof_section_info()` table: per-section `{ label, depth, is_total }` (bare label + explicit nesting depth — the panel derives indentation from depth). Also the GPU-bracketing policy: `k_gpu_sections[]` (which sections get timer queries — GL-emitting ones only; per-fade-batch subsections excluded for query-budget reasons), the cpuprof hook pair that routes them into gpuprof, and the per-frame capture mode (Off/Plot → no queries, Sections → depth-0 rows only, Details → full subset; set by the controller at frame top, since query boundaries cost real GPU time on GL-on-Metal). The demos implement their own `prof_section_info` |
| `src/app/glr_prof.h` | GPU-section policy API (`glr_prof_section_is_gpu`, `glr_prof_install_gpu_section_hooks`, `glr_prof_set_gpu_capture_mode`) |
| `src/scene/render_types.h` | Shared `SceneRgba` / `SceneRenderConfig` / `FrameRenderContext` types for scene helpers |
| `src/scene/guides/guides_shared.h` | Shared guide snapshot and planning types for REPL-aware 3D overlay passes |
| `src/scene/guides/geometry_guides.c` | Vertex/primitive guide rendering (input context at cursor) from `SceneGuideSnapshot` |
| `src/scene/guides/geometry_guides.h` | Geometry guides render entrypoint |
| `src/scene/guides/transform_guides.c` | Transform guide rendering (pending matrix ops during replay) |
| `src/scene/guides/transform_guides.h` | Transform guides render entrypoint |
| `src/repl/transform_utils.h` | Header-only GL matrix helpers (`apply_tracked_transform`, `unwind_transform_stack`) mirroring executor transforms without requiring (or linking) `src/repl/executor.h`; shared by transform guides, edit overlays, and the replay walkers |
| `src/scene/render.c` | 3D scene frame orchestration, one-shot init, scene config/frame prep, edit guides, orbit target, replay fade pass orchestration |
| `src/scene/grid.c` | Grid theme rendering and custom focus/ocean/ruler/planes passes |
| `src/scene/grid.h` | Grid render entrypoint |
| `src/scene/axes.c` | Axes theme rendering |
| `src/scene/axes.h` | Axes render entrypoint |
| `src/scene/scene_transition.c` | Pure grid/axes show↔hide fade state machine (`scene_xn_init/set/show/tick`); no GL, one instance per overlay |
| `src/scene/scene_transition.h` | Transition machine API: `SceneXnState`, `SceneXnPhase`, entry points |
| `src/scene/render.h` | Declares `scene_render_3d_scene(SceneRendererState *, const SceneRenderConfig *)`, `scene_renderer_state_init(...)`, `scene_get_active_projection(...)`. Camera transform is set by the caller (e.g. `glr_camera_load_modelview` from `src/app/glr_camera.h`) before invoking — the scene module owns no camera type or apply helper |
| `src/scene/backdrop.c` | Backdrop mode dispatch and deterministic cityscape renderer |
| `src/scene/backdrop.h` | Backdrop render entrypoint |
| `src/scene/lights.c` | Ambient init, light setup/reset, and visible light indicator overlay |
| `src/scene/lights.h` | Scene light setup/render entrypoints |
| `src/scene/overlays.c` | Tiny per-vertex GL primitives the controller calls (vertex-number labels, normal arrows). Outline / vertex-point passes moved to `src/app/glr_ctrl.c` |
| `src/scene/overlays.h` | Scene overlay primitive API |
| `src/ui/app/state.c` | Owns `UiState`: viewport, pointer, status text TTL, recent-message history ring (`ui_state_status_history*`, pushed from the single `ui_state_status_set_kind` chokepoint with consecutive-dup collapse), panel visibility, panel-divider geometry |
| `src/ui/core/hit.h` | `UiHitKind` + `UiHit` — neutral hit-test result returned by UI input handlers to `glr_ctrl` |
| `src/ui/app/panels.c` | Code-panel row rendering (incl. inline ghost/hint text), scene status banner, persistent bottom "messages" button + toggled recent-message history list (`UI_HIT_STATUS_HISTORY`; routed in `glr_ctrl_router.c`), hit-test (returns `UiHit`) |
| `src/ui/app/panels.h` | Code-panel geometry, render, and hit-test declarations |
| `src/repl/eval.c` | Expression evaluator (recursive descent), REPL<->C translators, for-loop parsers |
| `src/repl/eval.h` | Evaluator types (`ExprVar`, `ExprCtx`), function declarations |
| `src/repl/format.c` | Pure indentation/depth computation (no GL dependency) |
| `src/repl/format.h` | Formatting types (`ReplFmtCmd`, `ReplFmtType`), `repl_format_*` indent functions |
| `src/ui/core/gl_2d.h` | Header-only 2D OpenGL helper functions |
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
- Keyboard bindings: each action is one `keymap.h` pair macro
  `#define GLR_<ACTION>  <key>, <mods>` — the single place to reassign a
  shortcut. Call sites pass the whole pair to the matcher and never spell
  out modifiers: `keymap_event_is(key, GLR_X)` (it folds in the live
  modifiers in one place; implemented in `src/editor/input.c` next to the
  modifier accessor, declared in `keymap.h`). The cfg table splits the pair
  into its `.key_code` / `.modifiers` designated initializers via
  `KM_KEY()` / `KM_MODS()`, as do the two sites that need a bare key
  (a `case` label, the search pin's synthetic dispatch).
  `keys.h` underneath is the physical byte/special-code layer
  (`KEY_CTRL_*`); `keymap.h` is the action→key map on top. Guard:
  `make check-keymap-no-dup` (in the `check-state-ownership` gate) fails on
  any two bindings sharing a `(key, mods)`; `make keymap-list` prints the
  bindings and the free Ctrl / Ctrl+Shift / F-key slots (both via
  `scripts/keymap.sh`, also symlinked at `tools/keymap.sh`). A
  `g_cfg_items[]` runtime twin lives in `tests/test_glr_actions.c`.
  `editor_handle_key()` handles ASCII keys (Ctrl+X
  produces ASCII X & 0x1F via standard GLUT), `editor_handle_special()`
  for F-keys/arrows. Cross-subsystem routing (replay / save / config /
  audio / camera / tutorial-ack) lives in `src/app/glr_ctrl.c::glr_ctrl_router_*`
  helpers, called from `glr_ctrl_keyboard` before delegating to
  `editor_handle_key`. `glr_ctrl_router_handle_tutorial_ack_key` consumes
  Enter / Tab / Space during a tutorial SET (showcase) step and is a
  no-op for COMMAND / REQUIRE / inactive — scope it strictly there so it
  doesn't swallow keys outside that window. macOS Cmd+letter is normalized
  to its control-character form by `editor_input_normalize_super_to_ctrl`,
  called at the top of `glr_ctrl_keyboard` so every downstream
  dispatcher sees Cmd+B identically to Ctrl+B.
- Expression variables: `ExprVar` struct in `src/repl/eval.h`, predefined set
  accessible via `repl_state_variables()` and managed by `repl_eval_declare_predef_var()`

## Architecture

### Rendering Pipeline

`glr_ctrl_display_frame()` in `src/app/glr_ctrl.c` drives each frame.
`gl_repl.c` registers the GLUT display callback and forwards directly — there
is no shim layer.
1. Rebuild autonormals and flat program if dirty; save predef var values;
   prepare replay frame if active; update export/camera strings
2. Build `SceneRenderConfig` from REPL state. Load the camera via
   `glr_camera_load_modelview(&pose)` (from `src/app/glr_camera.h`),
   then call `scene_render_3d_scene(&g_scene_renderer, &cfg)` once. The
   accumulation loop (when `accum_effect` is AA or Blur with
   `accum_passes > 1`) lives inside that call. The camera modelview
   transform is the controller's responsibility — `src/scene/render.c`
   does not touch the modelview except for sub-renderer push/pop
   bracketing, and owns no camera type. For AA, jitter is applied as a
   scene-local frustum shift inside the scene function. For Blur, the
   controller installs a per-sample `setup_subframe_fn` on the config
   (`glr_ctrl_resolve_blur_subframe`) that the scene calls before each
   accumulation pass to interpolate the camera pose or advance an
   animation-time sub-step; see *Accumulation Motion Blur* below.
3. `scene_render_3d_scene(&cfg)` in `src/scene/render.c`: viewport/clear setup
   → projection → execute user geometry via `SceneExecuteProgramFn`
   callback → replay fade batches → grid/axes/backdrop/orbit-target →
   polygon-outline, vertex, normal, and guide overlays → 2D replay HUD
  (renders via `replay_ui_hud_render` from `src/ui/subsystems/replay_hud.c`)
4. 2D overlays: code panel, autocomplete popup, example dropdown,
   variable slider panel, config menu, help overlay, search overlay

The standalone `make scene_demo` binary (sources in `tools/scene_demo/`)
exercises the scene contract with a non-REPL geometry callback — it builds
without dragging in the REPL editor / controller, which is the load-bearing
proof that `src/scene/` has no hard dependency on REPL code.

### Accumulation Motion Blur

The accumulation buffer drives several effects, selected by the **Accum
effect** config (`GLR_CONFIG_ACCUM_EFFECT`: Off / AA / Blur / Blur Cam, F2)
over **Accum passes** samples (`GLR_CONFIG_ACCUM_PASSES`: 1/2/4/8/12/16,
Ctrl+=/Ctrl+−). Backing fields are `GlrRenderState.accum_effect/accum_passes`
→ `SceneRenderConfig`. AA (the historic default, 2 passes) jitters the frustum
per sample. **Blur** and **Blur Cam** are opt-in (expensive: they re-render the
scene per sample with no temporal reuse). The two blur modes differ only in the
no-camera-motion case: **Blur** also blurs the animation time, **Blur Cam**
falls back to AA. **Blur and AA jitter are never combined** — a blur sample
always renders with jitter 0, so a frame is exactly `accum_passes` renders
either way (no doubling).

The accum loop lives in `scene_render_3d_scene()` (`src/scene/render.c`); a
sample is a "blur sample" when `SCENE_ACCUM_EFFECT_IS_BLUR(accum_effect)` and a
hook is installed. For blur the scene makes a per-sample `SceneRenderConfig`
copy and calls `config->setup_subframe_fn(ud, pass_idx, pass_count, &pass_cfg)`
— a hook the **controller** installs (`glr_ctrl_resolve_blur_subframe` →
`glr_ctrl_setup_subframe` in `src/app/glr_ctrl.c`), so the scene stays
camera/REPL-agnostic. Per frame the controller picks:

- **Camera blur** (both Blur and Blur Cam) — if the camera pose changed since
  last frame (`glr_camera_pose_changed`), interpolate `prev`↔`cur` pose across
  the samples (`glr_camera_pose_lerp`). The hook loads the interpolated
  modelview *and* writes the pose into `pass_cfg->cam_*` so
  grid/axes/orbit-target/lights and the ortho projection (recomputed per
  sample) blur with the camera too.
- **Time blur** (Blur only) — else if `t` is playing, sample the trailing
  window `[t−dt, t]` (`dt = GLR_FRAME_DT_SECS`): the hook calls
  `repl_state_time_set_transient()` then `repl_flatten_commands()` to re-bake
  geometry at the sub-step `t`. (Re-baking is required — the executor consumes
  baked `flat_cmds[].args`; animation is reflatten-per-frame, not execute-time
  re-eval.)
- **Fallback** — camera still (and not time-blurring) ⇒ the hook stays NULL and
  the scene takes the AA jitter path. **Replay** also forces this (NULL hook): a
  per-sample reflatten would clobber the replay-narrowed flat count.

Each sample first resets predef/scratch/render to a frame baseline captured in
`g_subframe_ctx`, so accumulating programs (`A[0]=A[0]+1`, `t=t+1`) don't
compound across samples. The frame-level predef/scratch restore at the end of
`glr_ctrl_display_frame` puts the true `t` back; the last sample (f=1) bakes at
exactly `t_end`, so the flat program is left at the true frame time.

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
   `editor_try_commit_float_decl` → `editor_try_commit_assign_variable` → `editor_try_commit_close_brace`
   → `editor_try_commit_for_loop` → `editor_try_commit_func_def` → `editor_try_commit_if_block`
   → `repl_parse_and_normalize()` (general GL commands).
   **Ordering matters**: `editor_try_commit_float_decl` MUST run before
   `editor_try_commit_assign_variable`, otherwise `float x` is misread as an
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
`editor_try_commit_float_decl` MUST run before `editor_try_commit_assign_variable`, otherwise
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
- `GLCmd` fields (tagged-union payload, keyed on `type`):
  `payload.decl.names[MAX_NAMES_PER_DECL][16]`, `payload.decl.count`
  (active for `CMD_VAR_DECLARE`); `payload.label.fmt[GLUT_BITMAP_FMT_MAX]`
  (active for `CMD_LABEL`). Other types must not read the payload.
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

`wireframe`, `grid`, `grid_major`, `grid_extent`, `grid_brightness`, `axes`,
`vertex_labels`, `normal_vectors`, `vertex_outlines`, `vertex_points`,
`xform_guides`, `light_indicators`, `light_theme`, `backdrop`,
`view_mode`, `camera_rotate`, `variable_panel`.

`view_mode` is the 2D/3D projection toggle (slug for the "View mode"
config row → `GLR_CONFIG_ORTHO_MODE`): `0` = 3D perspective, `1` = 2D
ortho. It is reset per example load like the other scene-presentation
toggles (not sticky), so a 3D example loaded after a 2D one snaps back
to 3D unless it declares otherwise.

Non-leading `@cfg` lines are not metadata — they stay as ordinary
comments.

**Reset and restore rules:**

- Every example load resets the allowed non-camera scene-presentation
  settings (including `view_mode`) to built-in defaults *before*
  applying the example's leading `@cfg` metadata. Prevents stale state
  leaking across examples.
- Camera is intentionally excluded from that reset. Examples inherit
  the current camera unless they supply an explicit `// camera` header.
  (`view_mode` used to share this carve-out because it lives under the
  CAMERA menu section; it was moved into the reset set so the F12 cycle
  doesn't strand 3D examples in ortho after visiting a 2D one.)
- `restore_user_scene()` restores commands and predefined variables
  only. Leaving an example does not restore camera or other
  presentation state.

The single source of truth for example-owned presentation defaults is
the `CFG_DEFAULT_*` macro block in `src/app/glr_defaults.h`. Initializers, example
reset helpers, and tests reuse those macros instead of duplicating
literals. `make test_repl_core_examples` is the focused regression
suite; touch `src/repl/example_loader.c`, `src/repl/export.c`,
`src/repl/examples.c`, `src/app/glr_defaults.h`, and
`tests/test_repl_core_examples.c` together when changing example-metadata
behavior.

### Save/Load (output.c)

`src/repl/export.c` handles bidirectional text format:
- **Export** (`repl_export_save_output()`): writes a standalone C file with header
  comments embedding workspace state (`@var name=value`,
  `@cfg setting=value`, `@scene-name <name>`, `@workspace-dir <path>`),
  camera state as the raw `glTranslatef`/`glRotatef` sequence the REPL
  uses internally, predefined vars plus fixed scratch arrays `A/B/C[8]` as
  globals, REPL functions as C
  functions, and `display()` body containing the user's geometry commands.
  The workspace iterator in `src/repl/scenes.c` sets the export scene-name hint
  in import/export state before each slot's save so the hint wins over the
  active user scene index.
- **Import** (`repl_export_load_from_file()`): line-by-line scan parses camera state
  and workspace directives, detects function definitions (converts C
  syntax back to REPL), and feeds geometry lines through `editor_feed_line()`.
  Pending scene-name and workspace-dir directives are read by the caller
  after `repl_export_load_from_file` returns so the importer can name the new slot
  and remember the workspace dir.

### Replay System

Step-by-step execution visualization in `src/subsystems/replay/`:
- `ReplayRuntimeState` (via `replay_state_view()`) tracks state
  (OFF/PLAYING/PAUSED/DONE), program counter, and speed multiplier
- `replay_playback.c` owns start/stop, seek, advance, and speed control
- `replay_fade.c` owns the fade batch ring, skip limits, and fade decay
- `replay_input.c` routes replay-specific keyboard handling
- `replay.c` keeps the tess-preview and user-vertex walkers used by render paths
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
  new one. Call sites: `glr_ctrl_reset_all`, the F12 cycle
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
  `(menu_id, parent_row)` provider in `src/ui/app/menu_bar.c`:
  `submenu_row_count/_label/_abs_index/_kind/_is_active`,
  `menu_row_has_submenu`, `submenu_rect/_hit_test`,
  `render_active_submenu`). The pure section model lives in
  `src/app/glr_config.c` (`glr_config_section_count/_label/_range`,
  `glr_config_row_kind`); it counts only real `### ` headers — the
  `All` row is owned in the menu layer (`config_all_parent_row`), never
  double-counted. The `All` flyout spans the whole table 1:1 with
  `### `/`---` rows rendered as inert chrome (`GlrConfigRowKind`).
- **Scrolling long flyouts.** A flyout taller than the viewport (the
  synthetic **All** flyout is ~47 rows, taller than an 800px window) is
  clamped to fit by `submenu_rect`, and the mouse wheel pages through the
  hidden rows via a persistent `g_submenu_scroll` row offset
  (`ui_menu_bar_handle_wheel_scroll`, called first in both wheel paths of
  `src/app/glr_ctrl_router.c` so it never leaks to the code panel / camera
  behind the menu). `render_active_submenu` draws only the visible
  `[scroll, scroll+visible_rows)` window plus a thin right-edge scrollbar
  hint; the hit-test/hover paths add the same offset. The offset resets to
  0 when the flyout closes or its parent row changes. Lives in the shared
  flyout engine, so Scene / Tutorials flyouts get it too.
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

### Tutorials Menu

Tag-grouped flyouts driven by the same submenu engine as Scene and
Config. The tag system mirrors examples one-for-one
(`REPL_TUTORIAL_TAG_*` enum + `ReplTutorialTagMask` + private
`TUTORIAL_TAG_*` macros + `g_tutorial_tag_labels[]`, all in
`src/repl/tutorials.{c,h}`); the synthetic `ALL` is folded into every
entry's mask by `repl_tutorial_tag_mask`, so entry literals stay free
of it. Each tutorial declares a `.tags = TUTORIAL_TAG_X | …` mask
alongside `.cfg` (see the file-layout table for the shipped catalog).

- **Top-level layout.** `[0..t-1]` tag rows
  (`t = repl_tutorial_visible_tag_count()` — unused tags are hidden,
  matching how `ANIMATION` is currently filtered out). When a tutorial
  is active, `[t]` = `---`, `[t+1]` = "Restart Tutorial", `[t+2]` =
  "Exit Tutorial". No leading `### TUTORIALS` chrome header — it is a
  single-section menu.
- **Click semantics.** Tag rows are inert hover-only, mirroring the
  Scene tag-row guard: the `GLR_MENU_TUTORIALS` branch of
  `glr_action_menu_item_activate` returns 0 for any `item_idx <
  tag_count`. Restart/Exit are handled there at the trailing indices.
  Tutorial activation flows through `route_submenu_item_hit` in
  `src/app/glr_ctrl.c`: a `UI_HIT_SUBMENU_ITEM` with
  `cmd_idx == GLR_MENU_TUTORIALS` and `item_idx == absolute tutorial
  index` calls `tutorial_start(item_idx)` directly and dismisses the
  menu — symmetric with how Scene flyout hits call
  `glr_scene_load_example`.
- **Tagging a new tutorial.** Add the entry to `g_tutorials[]` with a
  `.tags = TUTORIAL_TAG_X | …` mask. The metadata test
  (`test_catalog_tag_metadata` in `tests/test_tutorial_runner.c`)
  enforces `mask != 0` and "only known bits", so a forgotten `tags`
  initializer fails CI rather than silently dropping the tutorial out
  of every flyout.
- **In-flyout subheading grouping.** A tutorial may declare an optional
  `.subheading = "…"` free-form string. Per-tag flyouts render
  consecutive tutorials sharing the same subheading under a single
  `### subheading` chrome row (HEADER kind, inert hover-only — same
  styling as Config's "All" flyout section headers). Tutorials with
  `NULL` subheading render without a header. Labels are catalog-author
  choices, not a fixed vocabulary — the current shipped subheadings
  happen to be `Beginner` / `Intermediate`, but a future REPL-vs-OpenGL
  tag layout could use `Grids` / `Lighting` / etc. within each tag's
  flyout. **Catalog convention:** entries sharing a subheading must be
  contiguous in the catalog so each header is emitted exactly once per
  flyout — `test_catalog_subheading_metadata` enforces this per tag and
  fails on interleaving (e.g., catalog order "Beginner, Intermediate,
  Beginner" would render two "Beginner" headers in the Geometry
  flyout). The walker is the generic `src/ui/app/menu_bar.c::catalog_flyout_row_at`
  + `CatalogFlyoutOps` vtable, shared with the Scene example flyout
  (`ReplExampleEntry` carries the same `.subheading` field — see
  `test_example_subheading_metadata`).

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
| Shift+click | Extend a selection from the cursor to the click — same row → per-character input-buffer selection; different row → whole-line range (the shift+Up/Down model) |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut / paste — input selection wins over line-range |
| Ctrl+S | Save to output.c |
| Ctrl+Z | Undo (Ctrl+Y or Ctrl+Shift+Z to redo) |
| Ctrl+\ | Reformat all lines |
| Ctrl+Shift+S | Split the multi-variable declaration under the cursor into one per line (also File → Split Declaration) |
| Ctrl+R | Start / stop replay |
| Ctrl+T | Toggle time variable `t` (Ctrl+Shift+T resets it to 0) |
| Ctrl+G | Toggle wireframe |
| Ctrl+Shift+N | Toggle normal vectors |
| Ctrl+Shift+E | Toggle vertex outlines |
| Ctrl+Shift+L | Toggle light indicators |
| Ctrl+Shift+F | Toggle code focus (hide boilerplate chrome) — also the statusbar "focus" keycap |
| Ctrl+Shift+O | Focus origin — ease the orbit target to (0,0,0) |
| Ctrl+Shift+C | Reset camera to default (eased) |
| Ctrl+Shift+R | Toggle camera auto-rotate |
| Ctrl+Shift+V | Toggle View mode (2D / 3D) |
| F1 | Help overlay — also the clickable statusbar "F1 help" keycap |
| F2-F10 | Cycle the bound config forward. Each drives a multi-state cycle: F2 Accum effect (Off/AA/Blur), F3 Grid, F4 Axes, F5 Vertex labels, F6 Backdrop, F7 Grid extent, F8 Xform guides, F9 Light theme, F10 Syntax highlight |
| Ctrl+= / Ctrl+− | Step Accum passes up/down (1/2/4/8/12/16; active when Accum effect ≠ Off) |
| Shift+F2-F10 | Step the bound cycle backward |
| F11 | Export scene geometry to PLY (also File → Export .ply). File is named after the active scene like Save Scene — `<scene>.ply` (in the workspace dir if bound), else `output.ply`. On macOS F11 may be claimed by "Show Desktop" — use the menu item then |
| F12 | Next example / scene |
| Shift+F12 | Previous example / scene |

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
glClearColor(r,g,b,a)   (sets the background clear color; channels clamped to >= 0.15)
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
glColorMaterial(face, mode), glMaterialfv(face, pname, (GLfloat[]){r, g, b, a})
  glColorMaterial mode: GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR, GL_EMISSION, GL_AMBIENT_AND_DIFFUSE
  glMaterialfv accepts either the (GLfloat[]){...} compound literal (the
  canonical form, with 1 element for GL_SHININESS or 4 for RGBA) or
  the flat input shorthand "face, pname, r, g, b, a" / "face, pname,
  shininess", which the parser rewrites to the compound-literal form.
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
:name  or  name:           (goto label — CMD_GOTO_LABEL; colon syntax, distinct from the label(...) call)
goto name                  (jump to a label — CMD_GOTO)
// comment
float name[, name2, ...];  (variable declaration)
var = expr;
A[index] = expr;           (fixed scratch arrays: A/B/C, index 0..7)
```

## Math

Functions: `sin`, `cos`, `tan`, `sqrt`, `abs`, `pow`, `log`, `ln`, `min`, `max`, `floor`, `ceil`, `fmod`, `rem`, `rand(seed[, iter])`, `rand2(seed[, iter])`

`log(x)` returns base-10 logarithm; `ln(x)` returns natural logarithm (base e).
`rand` returns a value in `[0, 1]`. `rand2` is the same hash mapped
to `[-1, 1]` — useful for centered jitter, signed offsets, etc. Both
are deterministic for a given (seed, iter) pair.
Constants: `PI`, `TAU`, `e`
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
