# Web build (Emscripten)

Files owned by this directory, plus their build-system entry points:

| File | Role |
|------|------|
| [`gl4es_bootstrap.c`](gl4es_bootstrap.c) | Compiled at **link time** (listed in `GL_LDFLAGS`, not `$(SRCS)`), so it never joins the `-std=c99` ratchet. Provides `glutExtensionSupported` (Emscripten's built-in JS GLUT lacks it; gl-repl's runtime GL capability detection calls it) and gl4es init/config. |
| `shell.html` | The themed Emscripten shell (`--shell-file`) - dark + amber, monospace, loading overlay, collapsible console drawer, canvas resize handling, browser scene open/download/fullscreen controls. |
| [`patches/*.patch`](patches/) | Local fixes to gl4es/GLU not yet upstream (or in a public fork). Applied by `scripts/web-deps.sh` after cloning, before building - see each patch's header comment and the running [`patches/README.md`](patches/README.md) investigation log. |

Both [`gl4es_bootstrap.c`](gl4es_bootstrap.c) and `shell.html` carry their full history from the
original `OpenGL-Vibe/emscripten/` prototyping tree (`git log -- packaging/web/*`).

## How the pieces fit together

- **Windowing**: Emscripten's built-in JS GLUT (`library_glut.js`) supplies
  windowing/events. The vendored freeglut's Emscripten backend
  (`third_party/freeglut/src/emscripten/`, built under `make ... WEB=1`)
  renames its own windowing entry points to `fg_glut*` via
  [`include/GL/emscripten_hide_glut.h`](../../third_party/freeglut/include/GL/emscripten_hide_glut.h), so the JS implementation wins and
  freeglut only supplies solids, stroke/bitmap fonts, and the geometry
  primitives gl-repl calls directly.
  That JS implementation answers a **fixed set of `glutGet` types and
  `abort()`s on every other one** - not "returns 0", but kills the page with
  `Aborted(glutGet(N) not implemented yet)` and a bare "Exception thrown"
  dialog. A probe between `glutInit()` and `glutCreateWindow()` therefore takes
  the app down before a window ever opens, in every browser (that is what
  `glutGet(GLUT_DISPLAY_MODE_POSSIBLE)`, a native Mesa accum-visual
  workaround, did). `make check-web-glut-get` guards the enum set; native-only
  queries belong in `#if !defined(__EMSCRIPTEN__)`. Nothing else catches this -
  `make test-web` never calls `main()`.
- **GL**: every TU force-includes gl4es's `<GL/gl.h>` (`-DUSE_MGL_NAMESPACE`),
  which maps `gl*` calls to `gl4es_gl*` over WebGL2. `glGetString` etc. resolve
  through gl4es, so `glutExtensionSupported` above reads the real live
  extension string.
- **Accumulation cost**: gl4es emulates each `GL_LOAD` / `GL_ACCUM` by
  snapshotting the antialiased canvas with `glCopyTexSubImage2D` (an implicit
  MSAA resolve), then drawing that texture into an RGBA16F accumulation FBO.
  A 10-pass AA frame is therefore ten scene renders, ten full-buffer
  resolve/copies, ten full-buffer blends, and one return draw. There is no
  per-pass `glFinish` or CPU fence; the passes are ordered on the GPU because
  each snapshot consumes the preceding render and each blend updates the same
  accumulation target. The first app sample uses `GL_LOAD`, so it replaces the
  target without a separate accumulation clear. On WebGL, queued cost can
  surface primarily as delayed delivery of the next animation frame; the
  compute profile accounts for that outside-callback gap as `Browser Wait`.
- **Console output**: `shell.html` overrides **both** `Module.print` (fd 1)
  and `Module.printErr` (fd 2), and feeds both to the in-page console drawer.
  Leaving `printErr` unset does not mean "same as print" - it means
  Emscripten's runtime default `console.error`, so stderr misses the drawer
  and lands red with a synthesized stack trace. That matters here because
  gl-repl's always-on startup trace is a *stderr* trace (on native that keeps
  stdout clean for `--dump-*` piping), so the drawer was missing exactly the
  output you would open it to read.

  Everything gl-repl prints on this page carries a single `GLREPL: ` tag, to
  sit alongside gl4es's own `LIBGL: ` lines instead of adding a third and
  fourth prefix style to one scrolling log. The tag is spelled once, in
  [`src/app/glr_log_prefix.h`](../../src/app/glr_log_prefix.h) (`GLR_LOG_TAG`),
  which is where the native/web difference lives - native keeps the
  historic `[init +N.NNNs]` / `[gl-repl]` stamps, web uses
  `GLREPL: +N.NNNs `. `printErr` routes by that tag: `GLREPL:` lines are the
  informational trace and go to `console.log`, everything else on stderr is
  a real diagnostic and keeps `console.error`. **Keep the shell's
  `glrIsTraceLine()` in step with `GLR_LOG_TAG`.** A new one-shot
  capability report should go through `glr_ctrl_init_log()` so it inherits
  the tag rather than reading as an error on every load.

  Carrying the stamp is not a claim of being informational -
  [`src/app/glr_audio.c`](../../src/app/glr_audio.c) stamps its miniaudio-log
  and worker-hitch lines so they line up against the startup timeline, and
  those are diagnostics. They keep their own `repl_audio: ` sub-tag right
  after the stamp, and `glrIsTraceLine()` reads a lowercase `word: ` there as
  "diagnostic" (trace messages never open that way - `accum buffer: 16 bits`
  has a space before its colon). So a stamped line may open with a
  `module: ` sub-tag only if it really is one.
  `make check-log-prefix-single-source` keeps the stamp from being
  hand-rolled somewhere that bypasses all of this.
- **Audio**: web builds use the browser media stack instead of miniaudio's
  file-backed decoder. `scripts/web-audio-assets.sh` copies MP3s beside the
  built page and writes `assets/music.json`; [`src/app/glr_audio.c`](../../src/app/glr_audio.c) fetches that
  manifest, registers the tracks through the normal `glr_audio_*` facade, and
  streams the selected URL with `HTMLAudioElement`. The native miniaudio path is
  unchanged, and music is no longer put in an Emscripten `.data` preload
  package. Browser autoplay policy is still honored: startup queues playback,
  and the first keyboard/mouse gesture begins the stream.
- **Browser input shims** (see the replayed commit history in
  `git log -- packaging/web/shell.html` for the specific fixes): mouse wheel
  is neutralized in JS GLUT's own handler so 3D-scene zoom works instead of
  page scroll; backspace is delivered as ASCII 8; Ctrl+letter is delivered as
  control codes so editor shortcuts fire; shifted digit keys are translated
  correctly; Ctrl+= / Ctrl+- (and their numpad twins) are delivered as the
  plain ASCII punctuation with CTRL in the modifiers so the accum-passes
  step works (delivering them also `preventDefault()`s the browser's
  Ctrl+=/- page zoom where that binding exists).
- **OS clipboard**: plain Ctrl/Cmd+C/X/V bypass JS GLUT entirely - gl4es_bootstrap.c's
  `GLUT.getASCIIKey` override returns `null` for them so neither `keyboardFunc`
  (gl-repl's own Ctrl+C/X/V path) nor `preventDefault()` fire, leaving the
  browser free to synthesize its native `copy`/`cut`/`paste` events, which
  shell.html's listeners bridge to [`src/app/glr_web_io.c`](../../src/app/glr_web_io.c)'s clipboard exports
  (reusing the existing editor clipboard copy/cut/paste machinery). Ctrl+Shift+C
  (reset camera) and Ctrl+Shift+V (view mode toggle) are excluded and still
  flow through GLUT unchanged. `text/plain` carries the snippet for
  interop with other apps; a custom `application/x-gl-repl-clipboard-kind`
  MIME type round-trips the exact gl-repl clipboard kind (input-buffer text
  vs. source lines) when the clipboard comes back into gl-repl itself, with a
  plain-text heuristic (multiline → source lines; single-line → replaces an
  active input selection / inserts in insert mode, else becomes one source
  line) when it doesn't survive (external apps, or a browser that drops
  custom clipboard types).
- **Canvas sizing**: the canvas framebuffer is synced 1:1 to element size via
  `Browser.setCanvasSize` plus a `ResizeObserver`, polling until GLUT's
  reshape listener registers (emscripten's `GLUT.onResize` covers window
  resizes but not the initial `glutInitWindowSize` or element-only reflows
  like the console drawer toggling).
- **Color**: the WebGL drawing buffer is tagged Display-P3 so browser color
  management doesn't wash out saturated scene colors.
- **Scene import/export**: the shell's Open/Download/New controls call into
  [`src/app/glr_web_io.c`](../../src/app/glr_web_io.c)'s `EMSCRIPTEN_KEEPALIVE` exports
  (`glr_web_new_scene`, `glr_web_load_scene_text`, `glr_web_export_scene`) -
  see `-sEXPORTED_FUNCTIONS` in the Makefile's `WEB=1` block.
- **Guided tours**: the in-canvas Tours menu uses
  [`tours/catalog-emscripten.ini`](../../tours/catalog-emscripten.ini). Its
  shared Editing Basics script selects a `shell:new` branch; the pointer-script
  bridge resolves that symbolic target against the real DOM New button and
  queues its existing click handler instead of relying on the hidden native
  File menu. The same conditional source remains usable for native tours and
  direct `GLR_POINTER_SCRIPT` recordings.
- **URL sharing**: the *share* button encodes the whole scene into
  `location.hash` so a copy-pasted link reproduces it - no server involved,
  works on static hosting. Payload format is
  `#s1=<base64url(deflate-raw(scene text))>` where the text is exactly what
  the download button saves (so `@cfg` presentation settings, the camera
  block, variables, and code all ride along); shift-click shares a
  settings-only `#c1=<base64url(deflate-raw("// @cfg slug = value" lines))>`
  link that applies to the receiver's current scene instead of replacing it.
  The `s0`/`c0` variants are uncompressed fallbacks for browsers without
  `CompressionStream`. On boot (postRun) the shell decodes the hash and
  routes it through `glr_web_load_scene_text` (scene) or the
  `glr_web_apply_cfg_text` export (settings; its counterpart
  `glr_web_cfg_share_text` generates the payload from the live config).
  Pasting a share hash into an already-open tab applies it via
  `hashchange`; the share button itself uses `history.replaceState`, which
  doesn't fire that event, so it never re-applies its own link. A default
  example encodes to a ~4.8k-char URL (from ~10 KB of scene text).

## Building

See the repo root `CLAUDE.md` "Web build (Emscripten)" section:
`scripts/build-web.sh` (cold start, no emsdk in the shell) or `make web` /
`make web-serve` (emsdk already activated).

The release web link builds with `-g0` (`DEBUG_INFO_CFLAGS`, see
`docs/ADVANCED_USAGE.md`), unlike every native build. emcc stores DWARF inside
the `.wasm` and falls back to limited binaryen optimizations while it is
there: 5.4 MB vs 1.8 MB of `index.wasm` for no measurable runtime difference,
so it is purely a fetch-and-compile cost. Pass
`make web DEBUG_INFO_CFLAGS=-g2` when a browser profile needs named frames.

The shipping build uses `examples/catalog-emscripten.ini`. To compile a
different catalog into the web app, pass `EXAMPLES_CATALOG` to `make web`; an
explicit override accepts the same flat-file and free-form-tag catalog shape
as `--examples-dir`:

```bash
make web EXAMPLES_CATALOG=tests/scenes/general/catalog.ini
```

## Benchmarking the web build

`make bench-web` compiles `bench/bench_repl.c` to wasm and runs it headless
under node. It exists because wasm cost is not a fixed multiple of native
cost - measured per-op ratios ran from ~1.2x to ~2.2x across sub-benchmarks,
which is enough to reorder what looks expensive - so `make bench` alone can
point at the wrong hot spot for this target.

It measures the C pipeline only. node has no GPU and no WebGL context, so
`fade_batches` skips itself and nothing here observes the
gl4es -> WebGL2 -> browser-GL cost of real draw calls. A regression in the
draw path needs an in-browser harness instead; note that `scripts/web-serve.py`
sends no COOP/COEP headers, so a page it serves has `performance.now()`
clamped to 100 µs and cannot resolve sub-100 µs work. Read the header comment
in `bench/bench_repl.c` before comparing web numbers against native ones.

## Testing the web build (`make test-web`)

`make test-web` builds the ordinary test binaries with `WEB=1 USE_GL_STUBS=1`
and runs them as wasm under node, through the same `scripts/run-tests.sh`
runner and the same per-test arguments as `make test-stubs`.

The `WEB=1 USE_GL_STUBS=1` combination is the whole trick. `make test` is
already a stubs build, so the wasm twin needs no GL stack at all: no gl4es, no
`scripts/web-deps.sh`, no `third_party/web/` checkout, no browser, and none of
`packaging/web/gl4es_bootstrap.c` - whose constructor calls
`document.querySelector` and throws before `main()` outside a browser. Only
`emcc` and `node` are required. From clean it takes about 35 s.

Link differences from `BENCH_WEB_LDFLAGS` live in `WEB_TEST_LDFLAGS`:

- `-sNODERAWFS=1`. Tests `fopen()` real paths - `build/*_trace.txt` stub
  traces, `tests/testdata/` fixtures, `/tmp` workspaces - and MEMFS has none
  of them. Without it, `fopen()` fails silently and roughly 150 assertions
  fail as "expected 6, got 0" rather than as an error. It also makes the
  `system()` calls in `test_export_trace_parity` work.
- `-sEXIT_RUNTIME=1` on **every** binary. Without it `main()`'s non-zero
  return never reaches node and a failing test exits 0.

### What it does and does not cover

It covers what the native suite structurally cannot reach: the
`__EMSCRIPTEN__` branch of every `#ifdef` in the tree (`menu_bar.c`,
`edit_overlays.c`, `geometry_guides.c`, `glr_audio.c`, `glr_clipboard.c`,
`glr_web_io.c`, `memprof.c`, `help_text.c`, …), plus wasm's 32-bit pointers
and stricter alignment.

It does **not** cover gl4es -> WebGL2. This lane links the GL stubs, so no GL
call goes anywhere - the same blind spot as `bench-web`, and the reason the
recent gl4es polygon-line and vertex-label regressions would not have been
caught here. That still needs a browser lane.

For the gl4es draw path, `make bench-web-gl4es` builds browser-side coverage
oracles under `build/release-web/`, including
`gl4es-line-width-cases.html`. The cases page exercises state transitions and
compiled-list lifetimes that the ordinary wasm test lane cannot observe.

The same target also builds `gl4es-render.html` from the shared
[`bench/bench_render.c`](../../bench/bench_render.c) harness. It runs the
native-compatible fixed-function workload through gl4es and covers batched
triangles, point-size capture, wide and polygon-mode lines, attrib-stack
restoration, accumulation, perspective bitmap raster positions, and
front-face isolation after a GL_BACK color-material selection. Each case reports
draw time plus coverage and
probe metrics; the browser page publishes `window.gl4esRenderBench` and sets
`document.title` to `PASS gl4es render bench` or `FAIL gl4es render bench`.
This deliberately uses feature invariants instead of an exact screenshot, so
the result remains meaningful across WebGL drivers and antialiasing modes.

For a native baseline, run `make bench-render` (or
`make bench-render FREEGLUT_OSMESA=1` on a headless machine). The native run
prints the same metrics and timing rows. Native oracle failures are warnings
unless `--strict` is passed through `ARGS`; strict mode makes a
failed coverage/color invariant return non-zero. The browser build enables
strict mode automatically, and does not compare exact framebuffer hashes.

For in-app WebGL measurements, compare the FPS interval with the compute
profile's callback summary: the previous `Frame Time` plus the following
`Browser Wait` is one callback-start interval. The wait row includes browser
animation-frame scheduling and queued GPU back-pressure that cannot be charged
to a C section; it is especially important when the WebGL driver exposes no
timer-query extension and the GPU column reads `--`.

On fanless Apple Silicon, log the GPU operating point with the run so thermal
drift does not masquerade as a code change:

```bash
sudo powermetrics --samplers gpu_power,thermal \
  --sample-rate 1000 --sample-count 30 --show-plimits
```

Keep the reported GPU active-frequency/power samples and thermal-pressure
state beside the benchmark, and interleave before/after runs after the same
cool-down. The available fields vary by macOS and SoC; actual die temperature
is not guaranteed. `pmset -g therm` can confirm recorded pressure warnings
without a sampling trace, but neither it nor the AGX `ioreg` utilization
counter substitutes for average GPU frequency. `powermetrics` requires root.

### Web-aware tests, and the exclusion list

74 of the 76 binaries run under node. Where a test asserted behavior the web
build deliberately does not have, the fix is a `__EMSCRIPTEN__` arm around the
affected assertions rather than dropping the binary - `test_ui`,
`test_ui_scene_tabs`, `test_glr_ctrl` (the File menu `menu_visible()` hides)
and `test_repl_core_extra` (a `dup2`'d stdin pipe, which does not reach wasm's
stdio) each carry one. Better still, assert the web form where one exists;
reach for a skip only when the web build genuinely does nothing.

Two binaries remain in `WEB_TEST_EXCLUDE`; see the Makefile for the
per-binary reason. Neither is a wasm defect:

- `test_audio` - the whole binary is the native miniaudio device backend, down
  to the hitch-threshold accessor that returns `0.0` on web. Guarding it out
  assertion-by-assertion leaves an empty binary.
- `test_ui_menu_bar` - 25 assertions across ~8 sites drive the hidden File
  menu; guarding each would gut the binary's File-menu coverage for no web
  gain. A browser lane should cover the shell's replacement chrome instead.

`test_edit_overlays` and `test_render3d_guides` used to carry marker-form
forks (an Emscripten octahedron and a `glutSolidSphere` pair standing in for
GL_POINTS). Both markers are plain attenuated `GL_POINTS` on every target now
that `gl4es-point-smooth.patch` makes gl4es honour `GL_POINT_SMOOTH`, so the
world-coordinate trace assertions hold under wasm and the exclusion is gone.

Re-check the list whenever an `__EMSCRIPTEN__` branch is added or removed.

## Headless verification

The 40MB+ `.wasm`/`.data` payload makes `--screenshot`-style headless capture
unreliable (it stalls waiting for the full download). Drive the page instead
with a small Chrome DevTools Protocol script over a native WebSocket that
waits for `#overlay.hidden` before capturing - see the `shot.js` pattern from
the 2026-07-07 session (not checked in; a throwaway script, not a build
artifact).
