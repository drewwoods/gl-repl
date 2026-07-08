# Web build (Emscripten)

Files owned by this directory, plus their build-system entry points:

| File | Role |
|------|------|
| [`gl4es_bootstrap.c`](gl4es_bootstrap.c) | Compiled at **link time** (listed in `GL_LDFLAGS`, not `$(SRCS)`), so it never joins the `-std=c99` ratchet. Provides `glutExtensionSupported` (Emscripten's built-in JS GLUT lacks it; gl-repl's runtime GL capability detection calls it) and gl4es init/config. |
| `shell.html` | The themed Emscripten shell (`--shell-file`) — dark + amber, monospace, loading overlay, collapsible console drawer, canvas resize handling, browser scene open/download/fullscreen controls. |
| `patches/*.patch` | Local fixes to gl4es/GLU not yet upstream (or in a public fork). Applied by `scripts/web-deps.sh` after cloning, before building — see each patch's header comment for what it fixes and why. |

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
- **GL**: every TU force-includes gl4es's `<GL/gl.h>` (`-DUSE_MGL_NAMESPACE`),
  which maps `gl*` calls to `gl4es_gl*` over WebGL2. `glGetString` etc. resolve
  through gl4es, so `glutExtensionSupported` above reads the real live
  extension string.
- **Audio**: no `-pthread` — `pthread_create` fails, so `glr_audio_init`
  returns -1 gracefully and music stays off. Enabling it would need
  `-pthread` plus COOP/COEP response headers from whatever serves the page.
- **Browser input shims** (see the replayed commit history in
  `git log -- packaging/web/shell.html` for the specific fixes): mouse wheel
  is neutralized in JS GLUT's own handler so 3D-scene zoom works instead of
  page scroll; backspace is delivered as ASCII 8; Ctrl+letter is delivered as
  control codes so editor shortcuts fire; shifted digit keys are translated
  correctly.
- **Canvas sizing**: the canvas framebuffer is synced 1:1 to element size via
  `Browser.setCanvasSize` plus a `ResizeObserver`, polling until GLUT's
  reshape listener registers (emscripten's `GLUT.onResize` covers window
  resizes but not the initial `glutInitWindowSize` or element-only reflows
  like the console drawer toggling).
- **Color**: the WebGL drawing buffer is tagged Display-P3 so browser color
  management doesn't wash out saturated scene colors.
- **Scene import/export**: the shell's Open/Download/New controls call into
  [`src/app/glr_web_io.c`](../../src/app/glr_web_io.c)'s `EMSCRIPTEN_KEEPALIVE` exports
  (`glr_web_new_scene`, `glr_web_load_scene_text`, `glr_web_export_scene`) —
  see `-sEXPORTED_FUNCTIONS` in the Makefile's `WEB=1` block.

## Building

See the repo root `CLAUDE.md` "Web build (Emscripten)" section:
`scripts/build-web.sh` (cold start, no emsdk in the shell) or `make web` /
`make web-serve` (emsdk already activated).

## Headless verification

The 40MB+ `.wasm`/`.data` payload makes `--screenshot`-style headless capture
unreliable (it stalls waiting for the full download). Drive the page instead
with a small Chrome DevTools Protocol script over a native WebSocket that
waits for `#overlay.hidden` before capturing — see the `shot.js` pattern from
the 2026-07-07 session (not checked in; a throwaway script, not a build
artifact).
