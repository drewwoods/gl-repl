# Third-Party Licenses (Draft)

This project bundles the following third-party components. Their licenses are
reproduced (or pointed to) below; all are permissive.

---

## freeglut

- **Upstream:** <https://github.com/freeglut/freeglut> (currently vendored from
  a fork that adds the headless OSMesa backend, env/signal-gated frame
  capture on the windowed backends, an Emscripten/WebAssembly platform
  backend, a high-resolution stroke font, and clipboard access
  (`glutSetClipboardString` / `glutGetClipboardString`, Cocoa); see
  `VENDORED.txt`).
- **Vendored at:** `third_party/freeglut/` (built as a static library on macOS -
  Cocoa backend by default, the headless OSMesa backend under
  `make ... FREEGLUT_OSMESA=1`, or the Emscripten backend under
  `make ... WEB=1`). See `third_party/freeglut/VENDORED.txt` for the
  exact pinned source + commit; at time of writing it is
  `e1a7d181e135c2e6efde03ca49fc127be61ee0a1`.
- **License:** X-Consortium / MIT-style (the freeglut license).

The full contributor list lives in `third_party/freeglut/AUTHORS` (current
maintainers: John F. Fay, Diederick C. Niehorster, John Tsiombikas). The
license text, reproduced verbatim from `third_party/freeglut/COPYING`:

```
  Freeglut Copyright
  ------------------

  Freeglut code without an explicit copyright is covered by the following
  copyright:

  Copyright (c) 1999-2000 Pawel W. Olszta. All Rights Reserved.
  Permission is hereby granted, free of charge,  to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction,  including without limitation the rights
  to use, copy,  modify, merge,  publish, distribute,  sublicense,  and/or sell
  copies or substantial portions of the Software.

  The above  copyright notice  and this permission notice  shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE  IS PROVIDED "AS IS",  WITHOUT WARRANTY OF ANY KIND,  EXPRESS OR
  IMPLIED,  INCLUDING  BUT  NOT LIMITED  TO THE WARRANTIES  OF MERCHANTABILITY,
  FITNESS  FOR  A PARTICULAR PURPOSE  AND NONINFRINGEMENT.  IN  NO EVENT  SHALL
  PAWEL W. OLSZTA BE LIABLE FOR ANY CLAIM,  DAMAGES OR OTHER LIABILITY, WHETHER
  IN  AN ACTION  OF CONTRACT,  TORT OR OTHERWISE,  ARISING FROM,  OUT OF  OR IN
  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

  Except as contained in this notice,  the name of Pawel W. Olszta shall not be
  used  in advertising  or otherwise to promote the sale, use or other dealings
  in this Software without prior written authorization from Pawel W. Olszta.
```

---

## gl4es

- **Upstream:** <https://github.com/ptitSeb/gl4es>
- **Fetched/built by:** `scripts/web-deps.sh`, into gitignored
  `third_party/web/gl4es/` (not vendored in-tree - the build is
  toolchain-specific to the pinned Emscripten SDK). Pinned SHA recorded in
  `third_party/web/PINNED.txt` after a build; default pin at time of writing
  is `a444cc94b17c672c66c3e6ce07428dc603034db1`.
- **Local patches:** applied by `scripts/web-deps.sh` after cloning, before
  building, in this order. Six earlier ones are upstream since #517-#520
  (listed in `packaging/web/patches/README.md`); these are the rest, also
  carried on the `drewwoods/gl4es` fork while they go upstream:
  - `packaging/web/patches/gl4es-color-material-face.patch` - track the
    single active `GL_COLOR_MATERIAL_FACE` so `glColorMaterial(GL_FRONT, …)`
    / `(GL_BACK, …)` updates only the selected side's material in the
    generated shaders (two-sided scenes had back-face colors leaking onto
    front faces). `glPushAttrib(GL_LIGHTING_BIT)` also restores the
    `GL_COLOR_MATERIAL` enable.
  - `packaging/web/patches/gl4es-point-smooth.patch` - emulate round
    antialiased points in the GLES2 fixed-pipeline shader.
  - `packaging/web/patches/gl4es-point-size-batch.patch` - apply
    `glPointSize` to the batch it was called on.
  - `packaging/web/patches/gl4es-accum-fbo.patch` - implement the
    accumulation buffer with an internal FBO (new `src/gl/accum.c`)
    instead of stubbing `glAccum`/`glClearAccum`: `GL_ACCUM`/`GL_LOAD`
    copy the read framebuffer into a scratch texture and blend it into
    an RGBA16F (RGBA8 fallback) accum texture scaled by the weight;
    `GL_RETURN` draws it back. The getter reports `GL_ACCUM_*_BITS`
    (16/8/0) and `GL_ACCUM_CLEAR_VALUE`, so gl-repl's runtime accum
    detection re-enables the accumulation effects (Accum AA, Blur, Blur
    Cam) in the browser. 16-bit targets are kept only if a blended draw
    succeeds (otherwise RGBA8). The blended-format probe restores driver
    and tracked blend/program state afterward. `DeleteGLState` frees the
    FBO objects.
  - `packaging/web/patches/gl4es-accum-deferred-return.patch` - cache
    LOAD/ACCUM snapshots and reduce their weights once at RETURN.
  - `packaging/web/patches/gl4es-accum-deferred-scissor.patch` - size and
    copy deferred samples to the WebGL scene scissor.
  - `packaging/web/patches/gl4es-polygon-line-drawarrays.patch` - expand
    polygon-mode edges to `glDrawArrays` instead of client-indexed
    `glDrawElements`.
  - `packaging/web/patches/gl4es-polygon-line-quad-edges.patch` - draw a
    quad's (and quad strip's) boundary edges under polygon-mode lines
    instead of the triangulation's spokes and diagonals.
  - `packaging/web/patches/gl4es-edge-flag.patch` - implement `glEdgeFlag`
    (upstream a stub): polygon-mode line edges are built from the original
    primitive topology and suppressed edges are dropped, so tessellated
    outlines no longer show every interior diagonal. `GL_EDGE_FLAG` is
    answered by the getter.
  - `packaging/web/patches/gl4es-polygon-offset-line.patch` - shadow
    `GL_POLYGON_OFFSET_LINE` and apply a projection-row depth bias
    around polygon-mode line draws so vertex outlines are not a
    silent `GL_INVALID_ENUM` on WebGL.
  - `packaging/web/patches/gl4es-line-width-quads.patch` - expand
    `glLineWidth` > 1 into screen-space quads on stacks whose
    `ALIASED_LINE_WIDTH_RANGE` is `[1, 1]`. `gl4es_probe_line_width_and_depth`
    fills `maxlinewidth` / `GL_DEPTH_BITS` once a context exists.
- **License:** MIT. Reproduced verbatim from gl4es's `LICENSE`:

```
Copyright (c) 2016-2018 Sebastien Chevalier
Copyright (c) 2013-2016 Ryan Hileman

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

---

## GLU

- **Upstream:** <https://github.com/ptitSeb/GLU> - a standalone-buildable
  extraction of Mesa's GLU (tessellator, quadrics, NURBS, projection
  helpers), used here only for its Emscripten static build.
- **Fetched/built by:** `scripts/web-deps.sh`, into gitignored
  `third_party/web/GLU/` (same rationale as gl4es above). Pinned SHA
  recorded in `third_party/web/PINNED.txt`; default pin at time of writing
  is `2fed2bda2b725d2b9e32c435b48d5141cc95827f`.
- **License:** SGI Free Software License B (the license Mesa's GLU
  implementation ships under, which this fork is derived from) - permissive,
  no attribution required in binary distributions. The checkout carries no
  bundled `LICENSE`/`COPYING` file; see the upstream repo or
  <https://www.khronos.org/legal/free_and_open_source_notices> for the exact
  license text.

---

## miniaudio

- **Upstream:** <https://github.com/mackron/miniaudio>
- **Vendored at:** [`include/miniaudio.h`](../include/miniaudio.h) (single-header library).
- **Author:** David Reid (mackron@gmail.com).
- **License:** dual-licensed - your choice of **public domain (Unlicense)** or
  **MIT No Attribution (MIT-0)**. Neither option legally requires attribution;
  it is acknowledged here as a courtesy. The full text of both license options
  is at the end of [`include/miniaudio.h`](../include/miniaudio.h).
