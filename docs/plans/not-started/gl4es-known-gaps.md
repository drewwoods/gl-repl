# gl4es known gaps: closing the oracle failures

Status: **not started**
Date: 2026-10-04
Scope: gl4es, upstream (ptitSeb/gl4es, via the fork `~/src/drewwoods/gl4es`).
gl-repl changes are limited to oracle coverage, deleting closed known-gap
entries, and re-pinning.

[`packaging/web/gl4es-known-gaps.txt`](../../../packaging/web/gl4es-known-gaps.txt)
lists every real-GL oracle (`make test-gl-web`) that fails under gl4es, and
says it doubles as the gl4es to-do list. This plan sorts the to-do list into
the part with a known fix (phase A) and the part that needs a decision first,
mostly ptitSeb's (phase B).

Measured against the current pin (a444cc94 + 11 patches, `4713bbf2`):
`test_ui_gl_state` 223/243, `test_attrib_bits_gl` 89/90,
`test_tour_overlay_feedback` 12/25, `test_gl_state_inspector_gl` 479/531.

## How each patch goes out

The same path the #515 series used:

1. Branch in the fork off upstream `master`, one topic per branch.
2. Build that tree for wasm and run
   `make test-gl4es-ab ARGS="--b <fork tree>"`. The oracle lane must
   show the targeted entries as "fixed in B" and **no regressions**.
   Supplement the existing lane with targeted regression coverage for the
   behavior each patch introduces.
   For query/default-only changes, expect a 0 px catalog diff and flat
   render timings. State-restoration or emulation fixes can correct pixels;
   review those differences against the intended fix and require no
   unrelated rendering regressions.
   The fork tree has to ship the same `include/` as the pin (checked by the
   script), so build the branch on top of the pin + gl-repl's patches, or
   use the `gl-repl-upstream` stack.
3. Open the PR: the *why* first (which GL behavior, which oracle shows it),
   known gaps, AI use disclosed. At a measured pace, not all at once.
4. When it lands: re-pin gl-repl, delete the matching known-gap entries
   (the runner lists them as "no longer failing"), and log it in
   [`packaging/web/patches/README.md`](../../../packaging/web/patches/README.md).
   gl-repl does not carry these as local patches in the meantime: no web
   feature depends on them, so waiting costs nothing.

## Phase A: known fixes

Each one corrects queries, defaults, or restoration of state gl4es already
tracks. No new rendering emulation. A3 can change subsequent stencil draws
or clears by restoring the write mask correctly.

### A1. Answer the state queries gl4es already tracks

One PR in `src/gl/getter.c`: shared query cases in `gl4es_commonGet`,
plus changes to the `glGet*` wrappers so they handle the query's component
count and convert every component correctly.

The common getter currently handles scalar results. `glGetBooleanv` and
`glGetIntegerv` pass the address of a single `GLfloat` temporary
(`getter.c:862` / `:883`); a four-component case would overwrite stack
memory. `glGetDoublev` has a large enough temporary but copies only its
first component (`:1120`). Fix that contract before adding vector cases,
preserving scalar queries and applying GL's conversion rules per component.
The same handling will be needed for B1's two polygon modes and B2's four
raster-color components.

| Query | Source | Notes |
|---|---|---|
| `GL_ATTRIB_STACK_DEPTH` | `glstate->stack ? glstate->stack->len : 0` | `stack` stays NULL until the first push |
| `GL_CLIENT_ATTRIB_STACK_DEPTH` | `glstate->clientStack`, same shape | add push/pop query coverage alongside the getter |
| `GL_LINE_STIPPLE_REPEAT` | `glstate->linestipple.factor` | defaults already right (1, `glstate.c:203`) |
| `GL_LINE_STIPPLE_PATTERN` | `glstate->linestipple.pattern` | default `0xFFFF` |
| `GL_CURRENT_RASTER_POSITION` | `glstate->raster.rPos` x, y, z, plus clip w | `rasterpos_t` has no w: store `transl[3]` in `gl4es_glRasterPos3f`, 1 for `glWindowPos`, 1 at init |
| `GL_CURRENT_RASTER_POSITION_VALID` | `glstate->raster.rPos_valid` | resolves the `state.h:129` TODO left by #517 |

Today none of these has a getter case, so each falls through to the
driver, which has no such state. The oracles then read zeros, which is why
the default raster x, y, z "pass" and only w (spec: 1) fails.

Verification must query a raster position with distinct components through
`glGetFloatv`, `glGetDoublev`, `glGetIntegerv`, and `glGetBooleanv`, check
every converted component, and detect writes beyond the result buffer.
Include scalar queries and server/client attribute-stack depth before
push, after nested pushes, and after pops. The existing float-only raster
oracle does not cover the wrapper hazard.

Clears: attribute stack depth (2), line stipple (4), raster position
`[3]` and "inside the clip volume" (2).

### A2. Default diffuse/specular alpha for `GL_LIGHT1..7`

The spec default for lights 1..7 is (0, 0, 0, 1). `NewGLState`
(`glstate.c:263`) has a per-light loop that sets `ambient[3] = 1` but
never `diffuse[3]` or `specular[3]`, so they stay 0. This is two
assignments in that loop. It shows only in the query: fixed-function
lighting takes its alpha from the material, not the light.

Clears: `light* diffuse[3]`, `light* specular[3]` (all seven lights).

### A3. `GL_STENCIL_BUFFER_BIT` saves the stencil write mask

`stack.c:275` and `:607` are both `//TODO: Stencil buffer writemask`. The
getter already answers `GL_STENCIL_WRITEMASK` / `_BACK_WRITEMASK` from
`glstate->stencil.mask[0..1]` (`getter.c:612`). Fix: add a field to
`glstack_t`, save both faces on push, and restore with
`glStencilMaskSeparate` on pop. Front and back both, since the struct
already tracks both, but leave the separate-func/op TODOs next to it
alone (out of scope).

Clears: `STENCIL_BUFFER_BIT restores stencil write mask`.

A2 and A3 are separate PRs: they touch different files, and a small PR
is easier to review than a themed bundle.

### A4. `GL_MULTISAMPLE` initially enabled

Desktop GL's enable flag starts true independently of the framebuffer's
sample buffers ([OpenGL 2.1, table 6.15](https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf#page=294)).
gl4es's tracked `glstate->enable.multisample` starts at 0 and is reported
by `glIsEnabled` (`enable.c:396`). Initialize it to `GL_TRUE` in
`NewGLState`; do not derive it from `GL_SAMPLE_BUFFERS`.

An enabled flag does not promise a multisampled framebuffer: sample buffers
are a separate condition for multisample rasterization
([section 3.2.1](https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf#page=107)).
Whether GLES can honor subsequent enable/disable calls is the separate B3
question and does not block correcting this default.

Verify the initial flag on single-sample and multisampled targets, its
explicit enable/disable transitions, and restoration through
`GL_MULTISAMPLE_BIT` / `GL_ENABLE_BIT`. Keep the framebuffer sample-count
queries independent of the tracked flag.

Clears: `GL_MULTISAMPLE initially enabled`.

## Phase B: needs a decision first

Each item below needs a decision on the level of support to offer. Partial
implementations still have to satisfy the regression gate. The decision is
a question to put to ptitSeb (on #515 or a draft PR), not one to settle here.

### B1. `GL_POLYGON_MODE` per face

gl4es tracks one mode (`glstate->polygon_mode`, an int).
`gl4es_glPolygonMode` turns `GL_FRONT` into `GL_FRONT_AND_BACK` and
**returns early on `GL_BACK`** (`gl4es.c:1029`), and drawing applies the
one mode to every face.

- *Cheap:* a getter returning the tracked mode for both faces, mapping
  0 to `GL_FILL`. That fixes the defaults, but after
  `glPolygonMode(GL_FRONT, GL_LINE)` it reports back = `GL_LINE`, which
  is what gl4es draws but not what was asked for.
- *Faithful:* per-face state, and per-face drawing: split each primitive
  by winding and cull state before choosing fill, line or point. That is
  real emulation work in the polygon-line path that gl-repl's own patches
  already touch.

Question: is a getter that reports what gl4es draws acceptable, or does
he want per-face support or nothing?

Entries: `default polygon mode back`, `GL_POLYGON_MODE (front)` / `(back)`.

### B2. Raster color latched at `glRasterPos`

The spec latches the current color into `GL_CURRENT_RASTER_COLOR` at
`glRasterPos`, and `glBitmap` draws in that color, not the current one.
gl4es latches nothing and has no getter case.

- *Partial, subject to the regression gate:* initialize the raster color
  to (1, 1, 1, 1), latch the current color for unlit `glRasterPos`, and
  add the getter using A1's vector handling. `glWindowPos` bypasses
  lighting and latches the current color clamped to [0, 1]. This covers
  the default and unlit latch, but needs an explicit, tested policy for
  lit `glRasterPos` before it is a viable PR.
- *Faithful:* with `GL_LIGHTING` on, the raster color is the **lit**
  color of the raster position, which means evaluating fixed-function
  lighting on the CPU for one vertex. gl4es has no CPU lighting path.

Unconditionally copying the current color is not a regression-free partial
fix. `tests/test_ui_gl_state.c:311-316` enables lighting, sets red, and
asserts that the raster color differs from red. That assertion passes
today and is not in the known-gaps file; the unconditional copy would make
it a new failure. Preserve that check and the lit-color differential
cases. If no acceptable partial policy meets the gate, implement the lit
latch or defer B2; do not add the new failure to the allowlist to make a
partial fix pass. A partial fix also leaves the lit raster-color entries
on the list.

Also open: whether `glBitmap` should switch to the latched color. That
changes rendering (a `glColor` between `glRasterPos` and `glBitmap` stops
taking effect), which is correct but could surprise programs that rely on
gl4es's current behavior.

Entries: `default raster color[*]`, `raster color latched at glRasterPos
(*)`, `GL_CURRENT_RASTER_COLOR[*]`.

### B3. Controlling multisampling on GLES

The tracked-default fix is A4. The GLES path currently tracks
`GL_MULTISAMPLE` without switching rasterization (`enable.c:165`,
`proxy_GOFPE`), so faithfully honoring desktop GL's disable behavior is
separate emulation work.

Question: does ptitSeb want that emulation, or should the rendering
limitation remain explicitly unsupported? Keep the enable flag independent
of `GL_SAMPLE_BUFFERS` in either case.

No separate known-gap entry for rendering control today; A4 clears the
default-state oracle without claiming to implement this behavior.

### B4. Feedback mode

`glFeedbackBuffer` is a stub (`wrap/glstub.c:48`) and
`glRenderMode(GL_FEEDBACK)` returns `GL_INVALID_ENUM` (`render.c:47`,
`// missing GL_FEEDBACK`). gl4es already emulates `GL_SELECT` in
`render.c`, and feedback would follow that shape: transform on the CPU and
write tokens instead of drawing. This is a feature, not a fix: vertex
types (`GL_2D` .. `GL_4D_COLOR_TEXTURE`), polygon/line/point/bitmap
tokens, clipping, and pass-through.

Today nothing user-facing on the web needs it. gl-repl's PLY export
(`glr_mesh_export.c`) uses feedback, but the web build hides the File
menu. The cost is the 13 `test_tour_overlay_feedback` checks, and a future
web PLY export would need it.

Question: does ptitSeb want feedback in gl4es at all? If so, `GL_2D` alone
is a first cut for the overlay oracle. A `GL_2D` / `GL_3D` subset cannot serve PLY
export: `glr_mesh_export.c:87` requests `GL_3D_COLOR_TEXTURE` (x, y, z,
RGBA, and four texture coordinates), with the texture coordinates carrying
normals and `glPassThrough` markers framing the encoding.

Choose the scope explicitly: an overlay-only implementation, leaving PLY
unsupported, or an implementation that also supplies
`GL_3D_COLOR_TEXTURE` and pass-through for the exporter. The latter needs
targeted capture/parsing coverage for colors, encoded normals, and markers;
passing the `GL_2D` overlay oracle alone does not verify PLY support.

Entries: the 13 `test_tour_overlay_feedback` checks.

## Done when

- Phase A: A1-A4 merged upstream, targeted regression coverage passes,
  gl-repl re-pinned, their entries deleted from the known-gaps file, and
  `make test-gl4es-ab` clean.
- Phase B: each item has an answer. Implemented, or recorded in the
  known-gaps file as deliberately unsupported, with the reason replacing
  the to-do text. Record B3's rendering-control decision here (it has no
  separate oracle entry), and distinguish overlay feedback from PLY support.
