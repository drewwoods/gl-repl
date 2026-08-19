# gl4es `glLineWidth` emulation via screen-space quads

Status: **in-review**
Date: 2026-08-19
Scope: Emscripten / WebGL2 build only (local gl4es patch). Native
Cocoa / GLX / OSMesa are unchanged — they already rasterize wide
lines.

This is the line-width twin of
[`gl4es-point-smooth.patch`](../../../packaging/web/patches/gl4es-point-smooth.patch)
and
[`gl4es-point-size-batch.patch`](../../../packaging/web/patches/gl4es-point-size-batch.patch):
fix the translator, keep one C path in gl-repl. Do not add
`#ifdef __EMSCRIPTEN__` geometry proxies in `grid.c`, overlays, or
guides.

## Problem

WebGL's `lineWidth` does not widen a line. ANGLE (Chrome, Safari,
Firefox on most machines) reports `ALIASED_LINE_WIDTH_RANGE` of
`[1, 1]`; every `glLineWidth` is a no-op and every `GL_LINES` /
`GL_LINE_STRIP` / `GL_LINE_LOOP` rasterizes one pixel wide.

gl4es does not emulate the missing width. `gl4es_glLineWidth` is the
generated GLES wrapper in `src/gl/wrap/gles.c`. It forwards to
`gles_glLineWidth` and, after
[`gl4es-getter-client-state.patch`](../../../packaging/web/patches/gl4es-getter-client-state.patch),
mirrors the value in `gl4es_mirror_line_width` so `glGet(GL_LINE_WIDTH)`
and `glPushAttrib(GL_LINE_BIT)` stay client-side. That is enough for
state round-trip. It is not enough to change a pixel.

The same wrapper currently flushes a pending immediate-mode batch on
width change **only when** `hardext.maxlinewidth > 1`. On WebGL that
skip is correct *today* — every width draws identically, so a flush
would only break batching. Once width is emulated, the skip is the
`glPointSize` trap all over again.

## What gl-repl is missing on web

Chrome that is authored as “thick” is 1 px in the browser. The
payoff list, not an exhaustive inventory:

| Surface | Typical widths |
|---|---|
| Grid casing / checker ink | 1.3–3.4, plus 2 px casing (`GRID_CASING_WIDTH_ADD`) |
| Axes | 2.5 / 3.0 |
| Orbit gizmo halo / core | **6.0** over 1.5 |
| Focused-normal overlay | 6.0 halo, 2.2 core |
| Transform-guide shafts | 5.5 halo, 2.5 core (`TG_SHAFT_*`) |
| Active vertex outline | 3.0 (×2.5 if Bold) |
| Tutorials “Points & Lines” / “Line Stipple” | 3 and 2 |
| User `glLineWidth` in scenes | whatever the document sets |

`GL_LINE_SMOOTH` is a config toggle and is also a no-op on this
backend. It is **out of v1** (see Non-goals); width alone is the
gap this plan closes.

Width 1.0 must stay a native `GL_LINES` draw. That is the
polygon-mode / wireframe path
[`gl4es-polygon-line-drawarrays.patch`](../../../packaging/web/patches/gl4es-polygon-line-drawarrays.patch)
already made cheap.

## Current gl4es surfaces this will touch

Verified against the pinned checkout
`17f0894e19d1553e4176276c759915dab44c08e2` plus the existing local
patches (the tree under `third_party/web/gl4es` is that pin with the
patches applied):

- `src/gl/line.c` already projects endpoints through `getMVPMat()`,
  perspective-divides, and maps to window pixels to build stipple
  texcoords (`gen_stipple_tex_coords`). Width expansion is the same
  projection plus a perpendicular offset.
- `src/gl/listdraw.c` is every immediate-mode and display-list draw,
  including the polygon-line expansion that already emits
  `gles_glDrawArrays(GL_LINES, …)`.
- `src/gl/drawing.c` `should_intercept_render()` already special-cases
  `mode == GL_LINES && line_stipple`. A raw `glDrawArrays(GL_LINES)`
  with a wide width has to take the same intercept, or GLUT-solid
  wireframe and any client-array line draw miss the emulation.
- `src/glx/hardext.c` already records `hardext.maxlinewidth` from
  `GL_ALIASED_LINE_WIDTH_RANGE`. That is the trigger, not a new
  probe.
- `GL_LINE_WIDTH_RANGE` / `GL_ALIASED_LINE_WIDTH_RANGE` have **no**
  getter cases. They fall through to WebGL and report `[1, 1]`. After
  emulation the getter must tell the truth.
- Renderlist merge (`islistscompatible_renderlist` in `list.c`) does
  not know about line width. Two consecutive `glBegin(GL_LINES)`
  batches at different widths are legal to merge today because the
  driver cannot tell them apart.

`line.c` is the natural home. Do not start in `fpe_shader.c`.

## Design

Expand each wide line segment into a screen-space quad (two
triangles) at draw time, then draw those triangles with a temporary
identity MVP so the existing FPE vertex line

```
gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
```

becomes a passthrough.

That is the “draw it in ortho” idea, cleaned up so depth stays
perspective-correct. A literal `glOrtho(0, vp_w, 0, vp_h, …)` with
window XY works in 2D but forces a hand-rolled Z remap through
`glDepthRange`. The clip-space sibling does not.

### Per-segment expansion

For each pair of object-space endpoints `(p0, p1)`:

1. `c0 = MVP * p0`, `c1 = MVP * p1` (same `getMVPMat()` /
   `vector_matrix` as stipple).
2. Homogeneous-clip the segment to `w > ε`. A line through the
   camera (orbit gizmo, a transform shaft, any axis the eye sits on)
   otherwise produces `w <= 0` and the quad goes to infinity.
3. Perspective-divide, map to window pixels via
   `glstate->raster.viewport`.
4. 2D perpendicular of the window-space direction, scaled to
   `width / 2`. Degenerate / ~0-length segments become a
   pixel-aligned square of side `width`, or are skipped.
5. Four window-space corners → back to clip space, **keeping each
   endpoint’s original `clip.z` and `clip.w`**:

   ```
   ndc.xy  = window_to_ndc(corner)
   clip.xy = ndc.xy * clip.w     // clip.w from the parent endpoint
   clip.z  = parent clip.z
   clip.w  = parent clip.w
   ```

6. Emit two triangles. Copy the parent endpoint’s color (and stipple
   `s` if present) to both corners of that end.

Caps are square. No miters, no round joins. Desktop aliased
`glLineWidth` is independent square-capped segments; `GL_LINE_STRIP`
/ `LOOP` just overlap those square ends. Matching that is the v1
contract.

`GL_LINE_STRIP` and `GL_LINE_LOOP` are expanded pair-wise the same
way `gen_stipple_tex_coords` already walks them (and the way
`rlEnd` already fans them into indexed `GL_LINES` pairs when lists
merge). Polygon-mode edges arrive as `GL_LINES` from
`fill_lineIndices` / the existing line-array expansion; they pick
the emulator up for free if the current width needs it, and stay on
the cheap `DrawArrays(GL_LINES)` path when it does not.

### Identity-MVP draw

Around the triangle draw only:

1. Push projection and modelview, `glLoadIdentity()` both.
   FPE then does `gl_Position = I * clip`.
2. Disable lighting, texgen, and fog for the draw. Lighting the
   clip-space corners is nonsense; chrome is unlit `glColor`
   anyway. Fog would use the same wrong eye position. Clip planes:
   clip the **object-space segment** first, or skip emulation while
   a plane is enabled.
3. Draw `GL_TRIANGLES` with the expanded client arrays.
4. Pop both matrices and restore enables.

Do not introduce a new FPE program key for this. The point of the
identity MVP is to reuse the shader that is already bound.

### When to activate

```
requested_width > hardext.maxlinewidth + epsilon
```

On WebGL `maxlinewidth` is 1, so every width other than 1 takes the
quad path. On a GLES driver that can do 8 px natively, leave it
alone — this patch must not change those pixels.

Treat `width < 1` as native 1 px in v1. The grid uses `0.9f` on a
faded rank; desktop `GL_LINE_SMOOTH` can actually draw that, we
cannot until the follow-up AA step. Emitting a 0.9 px quad without
coverage AA is just a 1 px quad with more math.

### Stamp width on the list; flush on change

`glLineWidth(3); glBegin/End; glLineWidth(1);` is everywhere
(axes, overlays, grid casing). Immediate lists are drawn later. If
expansion reads `gl4es_mirror_line_width` at flush, the 3 px batch
becomes 1 px.

Copy `gl4es-point-size-batch.patch` exactly rather than inventing a
mechanism:

- compiling a display list → record a `linewidth_op` /
  `linewidth_val` (exclusive stage, same pattern as
  `STAGE_POINTPARAM` / `rlPointSizeOp`), replayed in `listdraw.c`
  next to the point-size op
- otherwise `gl4es_flush()` **before** the width changes — including
  when `hardext.maxlinewidth == 1`. The current skip in
  `wrap/gles.c` is deleted
- `islistscompatible_renderlist` refuses to merge two lists whose
  stamped widths disagree
- a list with no stamp (never saw a `glLineWidth` inside it) uses
  the current mirror at draw time, which is the polygon-mode case:
  wireframe uses whatever `glLineWidth` is in force, almost always 1

`glLineWidth` is invalid inside `glBegin`/`glEnd`, so a single list
has one width.

### Do not cache the expanded quads

They depend on MVP and viewport. A display-list VBO of expanded
corners would freeze width in camera space and look wrong the
moment the user orbits. Rebuild every draw, like stipple texcoords.

The polygon-line `line_arrays` VBO cache stays. It caches **topology
in object space**, which is camera-invariant. Width expansion is a
separate, transient buffer sitting on top of that (or on top of the
original `GL_LINES` verts). `clear_line_arrays` / `append_calllist`
do not grow a new persistent cache for it.

Immediate lists already allocate per frame for stipple; the same
budget applies. Use `gl4es_scratch()` if a single scratch region is
enough for the expanded verts + colors of one list, otherwise a
file-local reallocating buffer. Do not add 5+ malloc/free pairs per
list the way the first polygon-line cut did.

### Advertise a real range

Add getter cases for `GL_LINE_WIDTH_RANGE`,
`GL_SMOOTH_LINE_WIDTH_RANGE`, and `GL_ALIASED_LINE_WIDTH_RANGE`:

- if `hardext.maxlinewidth > 1`, report the driver’s range
  (unchanged)
- else report `[1, 64]` (or another documented cap the expander
  actually clamps to)

`glGet(GL_LINE_WIDTH)` already returns the mirror. The expander
clamps the requested width to that advertised max so a scene that
asks for 200 px cannot allocate a pathological quad.

`gl2d_begin()` already forces `glLineWidth(1)` and disables
`GL_LINE_SMOOTH`. 2D chrome that sets 1.5–2 px *inside* that
bracket (color-picker accents) will start working; that is
intended.

## Patch shape

New file
[`packaging/web/patches/gl4es-line-width-quads.patch`](../../../packaging/web/patches/gl4es-line-width-quads.patch),
**last** in `GL4ES_PATCHES` in `scripts/web-deps.sh`. It will
retouch `wrap/gles.c` that getter-client-state already patched, so
it cannot sit earlier.

Likely hunks:

| File | Change |
|---|---|
| `src/gl/line.c` / `line.h` | `expand_wide_lines` + clip + identity-MVP draw |
| `src/gl/listdraw.c` | call it for line modes when width needs it; replay `linewidth_op` |
| `src/gl/drawing.c` | intercept `glDrawArrays` / `glDrawElements` of wide lines |
| `src/gl/wrap/gles.c` | always flush / record op; drop the `maxlinewidth` skip |
| `src/gl/list.h` / `list.c` | `linewidth_op` / `linewidth_val`; merge incompatibility |
| `src/gl/getter.c` | fake `LINE_WIDTH_RANGE` when emulating |
| `src/glx/hardext.c` | already has `maxlinewidth`; no new probe |

Header comment on the patch follows the existing house style
(problem, what was tried, what landed). Add an inventory row and a
dated section to
[`packaging/web/patches/README.md`](../../../packaging/web/patches/README.md)
while the measurements are still available.

No gl-repl C changes in v1. Native and stub builds never see the
patch.

## Verification

Same discipline as the polygon-line work: FPS without coverage is
not a result, and a reused browser tab can keep the old wasm
module. Fresh navigation, coverage oracle, then timing.

1. **Microbenchmark** in the style of
   [`packaging/web/bench/gl4es_polygon_line.c`](../../../packaging/web/bench/gl4es_polygon_line.c):
   a few dozen segments at widths 1 / 1.5 / 3 / 6, `glReadPixels`
   coverage. Width 1 must match the pristine build to the pixel.
   Width 6 must cover on the order of 6× a 1 px line of the same
   screen length (not 6× the *pixel count of the whole canvas*).
   Title the page `FAIL` below the coverage floor so an empty frame
   cannot report a speedup.
2. **Headful screenshots** of: default grid + axes, orbit gizmo
   (6 px halo), a live transform guide, Bold vertex outlines, the
   Points & Lines tutorial. Compare against native, not against
   “looks thicker.”
3. **Trailing-reset oracle.** A scene that does
   `glLineWidth(8); draw; glLineWidth(1)` must keep the first batch
   at 8 px. This is the point-size-batch bug, and it is how gl-repl
   actually writes overlays.
4. **Near-plane.** Orbit around a long axis line until it passes
   through the camera. No giant slivers, no NaN uploads.
5. **Stipple + width 2.** Occluded-axis ghost (`GL_LINE_STIPPLE` +
   a non-1 width) still dashes. The expander must copy the stipple
   `s` that `gen_stipple_tex_coords` already produced.
6. **Wireframe / hidden-line stay at 60 FPS.** Those are width 1
   and must remain on the existing `DrawArrays(GL_LINES)` path. A
   regression here is a failed patch even if wide chrome looks
   perfect.
7. **Patch mechanics.** `git apply --check` clean on the pristine
   pin and on the pin with the earlier patches applied; reverse-apply
   against the tested tree; `make check-c99` and
   `git diff --check` on the gl-repl side (the patch file itself,
   `web-deps.sh`, this plan, the bench).

`make test-web` will not catch this. It links the GL stubs, not
gl4es. The browser lane *is* the test.

## Non-goals (v1)

- **App-side workarounds.** No octahedron-for-points redux. The
  point-smooth lesson: fix gl4es, delete the forks, keep one C path.
- **World-space expansion.** Desktop `glLineWidth` is pixels. A
  world-space ribbon would thicken under the camera.
- **Cached expanded VBOs** on named display lists.
- **Miter / round joins.** Different look from native aliased
  lines, and a second implementation.
- **`GL_LINE_SMOOTH`.** Follow-up patch, same directory, same
  relationship `gl4es-point-smooth` had to points: a 1 px alpha
  falloff across the quad (a varying, or a 1D gradient) folded into
  alpha after the alpha test. Do not block width on AA.
- **FPE vertex-shader expander.** Correct for lit lines (lighting
  stays on the original object-space position) and for clip planes,
  but it is a new program key, new attributes (other endpoint +
  side), and a much larger review. gl-repl’s wide lines are almost
  all unlit chrome. Revisit only if a lit user scene is actually
  wrong.
- **Sub-pixel widths** (`0.9f` grid minors) without coverage AA.
- **Changing native or stub builds.**

## Follow-up (not this patch)

`gl4es-line-smooth.patch`: when `GL_LINE_SMOOTH` is enabled, emit
the same quad with a 1 px feather on each long edge and multiply
coverage into alpha after the alpha test — the point-smooth
pattern. Width emulation must already be in so there is a quad to
feather; native 1 px `GL_LINES` cannot AA on WebGL either, so the
smooth follow-up should take the quad path for width == 1 as well
when the enable is on. That is a deliberate extra cost, gated on
the enable, and is why it is not folded into v1.

## Implementation order

1. Stamp + flush (`wrap/gles.c`, `list.h` / `list.c` / `listdraw.c`)
   with no visual change. The trailing-reset oracle still draws 1 px
   everywhere, but the lists now carry the width they were recorded
   at.
2. `expand_wide_lines` in `line.c` and the `listdraw.c` hook, width
   > `maxlinewidth` only. Identity-MVP draw, near-plane clip,
   square caps.
3. `drawing.c` intercept so client-array `glDrawArrays(GL_LINES)`
   cannot sneak past.
4. Getter range + clamp.
5. Bench + the six verification cases above.
6. Patch header, `GL4ES_PATCHES` append, README inventory + dated
   section.

Step 1 is independently reviewable and is the same shape as
`gl4es-point-size-batch.patch`. Do not skip it and read the mirror
at flush — that will ship a patch that looks right in a one-width
microbench and wrong on every overlay.

## Rejected approaches

Recorded so they are not rediscovered during implementation.

- **Geometry shader.** WebGL 2 still has none.
- **Pass-through to GLES `glLineWidth`.** Already what we do; ANGLE
  ignores it.
- **Literal window-space `glOrtho`.** Works, but Z and
  perspective-correct interpolation become a second problem. Clip
  space + identity MVP is the same 2D expansion without that remap.
- **Only hook `listdraw.c`.** Misses `glDrawArrays(GL_LINES)` from
  `drawing.c`, which is the same class of hole the stipple intercept
  already exists to close.
- **Expand in the FPE vertex shader first.** See Non-goals. The
  CPU path reuses the stipple projector and does not fragment the
  shader cache.
- **Cache expanded geometry on `glEndList`.** Camera-dependent.
  The polygon-line cache is the wrong precedent here; it caches
  object-space edge topology, not window-space offsets.
