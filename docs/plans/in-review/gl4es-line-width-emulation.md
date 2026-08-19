# gl4es `glLineWidth` emulation via screen-space quads

Status: **in-review** (review folded 2026-08-19)
Date: 2026-08-19
Scope: Emscripten / WebGL2 build only (local gl4es patch). Native
Cocoa / GLX / OSMesa are unchanged — they already rasterize wide
lines.

This is the line-width twin of
[`gl4es-point-smooth.patch`](../../../packaging/web/patches/gl4es-point-smooth.patch):
fix the translator, keep one C path in gl-repl. Do not add
`#ifdef __EMSCRIPTEN__` geometry proxies in `grid.c`, overlays, or
guides.

Reviews are folded into the design below rather than left as a
changelog. The ones that changed the shape of the work are called
out where they sit (`PUSH_IF_COMPILING` already flushes, the
identity-MVP bracket must force `GL_FILL` / disable cull, getter
tokens collide, transient quads must unbind any active VBO before
pointing at CPU arrays).

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

Immediate-mode batching is already handled. The generated wrapper
opens with `PUSH_IF_COMPILING(glLineWidth)` (`loader.h:119–128`):

- compiling a display list (`list.active && !list.pending`) → record
  a `STAGE_GLCALL` packed call and return
- a pending immediate batch (`list.active && list.pending`) →
  `gl4es_flush()` and fall through

`pending` implies `active`, so the later
`if (hardext.maxlinewidth > 1) FLUSH_BEGINEND` is dead code.
Immediate-mode batches already flush on every width change, WebGL
included. Deleting the skip changes nothing. The point-size patch
is the wrong template here: `gl4es_glPointSize` lives in
`pointsprite.c` with no `PUSH_IF_COMPILING` at all, which is why
that wrapper had to grow its own compile/flush path.

The remaining compile-list question is whether a `STAGE_GLCALL`
`glLineWidth` replays in the right order relative to the geometry
that follows it inside `glNewList`. That is the thing to establish
(see Implementation order, step 1). It is smaller than “copy
`gl4es-point-size-batch.patch`.”

`stack.c:546` restores width via `gl4es_glLineWidth`, so
`glPopAttrib(GL_LINE_BIT)` — which gl-repl does every frame —
picks up whatever mechanism lands. Load-bearing, and the reason
the wrapper (not a side door) has to stay the single writer of
the mirror.

The mirror-equality early-out currently sits **after**
`PUSH_IF_COMPILING`, so a redundant same-width call still flushes
a pending batch. Minor cost. Keep the early-out **ahead** of any
flush the wrapper adds.

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
  perspective-divides, and maps to NDC×`(vp_w/2, vp_h/2)` to build
  stipple texcoords (`gen_stipple_tex_coords`). The viewport origin
  cancels in any difference, which is why that function never reads
  `viewport.x` / `viewport.y`. Width expansion is the same
  projection plus a perpendicular offset, and should stay in NDC
  for the same reason.
- `src/gl/listdraw.c` is every immediate-mode and display-list draw,
  including the polygon-line expansion that already emits
  `gles_glDrawArrays(GL_LINES, …)`.
- `src/gl/drawing.c` `should_intercept_render()` special-cases
  `mode == GL_LINES && line_stipple` only. The wide-line intercept
  has to name `GL_LINES`, `GL_LINE_STRIP`, and `GL_LINE_LOOP`, or
  client-array strips and loops slip past the same way. Stipple
  stays `GL_LINES`-only; do not silently inherit that predicate.
  The existing `polygon_mode == GL_LINE && mode >= GL_TRIANGLES`
  intercept (line 260) is why the identity-MVP bracket must force
  `GL_FILL` — see Identity-MVP draw.
- `gen_stipple_tex_coords` (`line.c:135–136`) walks `GL_LINE_LOOP`
  as a strip and skips the closing last→first segment on purpose.
  The width expander must emit that edge; the stipple helper will
  not.
- `src/glx/hardext.c` already records `hardext.maxlinewidth` from
  `GL_ALIASED_LINE_WIDTH_RANGE`. That is the trigger, not a new
  probe. It is an `int`.
- `0x0B22` (`GL_LINE_WIDTH_RANGE` in `const.h` /
  `GL_SMOOTH_LINE_WIDTH_RANGE` in `gles.h`) and
  `GL_ALIASED_LINE_WIDTH_RANGE` have **no** getter cases. They fall
  through to WebGL and report `[1, 1]`. After emulation the getter
  must tell the truth. The two names for `0x0B22` are both in
  scope in `getter.c` — they are one `case`, not two.
- Renderlist merge (`islistscompatible_renderlist` in `list.c`) does
  not know about line width. Immediate mode already flushes on a
  width change (`PUSH_IF_COMPILING`), so two pending batches at
  different widths do not sit together. A compiled list that
  contains a `STAGE_GLCALL` `glLineWidth` between two geometry
  nodes is the case that still needs an ordering check.

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
2. **4D clip against the near plane**, not a `w > ε` test.
   Under a standard perspective, `w_clip = −z_eye`. A vertex
   between the eye and the near plane has `w > 0` but
   `z_clip + w_clip < 0`; perspective divide then produces
   huge window coordinates and the 2D perpendicular blows up.
   A vertex behind the eye (`w ≤ 0`) flips the NDC direction
   and draws a spurious inverted line.

   Parametric clip against `z + w = 0`:

   ```
   C(t) = C0 + t (C1 − C0)
   t     = −(z0 + w0) / ((z1 + w1) − (z0 + w0))
   ```

   Interpolate **every duplicated attribute** (color RGBA, secondary,
   fog coord, every texcoord set including stipple `s`) at `t`.
   Discard the segment if both endpoints have `z + w < 0`. After
   this clip every surviving endpoint has `w > 0` and is in front
   of the near plane. Far / side planes are out of v1 — the GPU
   still clips the expanded triangles.
3. Perspective-divide to NDC. Do **not** go through window pixels
   and back: the viewport origin cancels in the perpendicular
   (that is why `line.c` only ever uses `w * 0.5f` / `h * 0.5f`).
4. 2D perpendicular of the NDC direction, then scale to a
   constant **pixel** half-width:

   ```
   n     = normalize(perp(ndc1.xy − ndc0.xy))
   Δndc  = (n.x * width / vp_width, n.y * width / vp_height)
   ```

   Degenerate / ~0-length segments (`|ndc1 − ndc0| ≈ 0`, including
   `p0 == p1`) become an axis-aligned NDC square of side
   `width / vp_*`, or are skipped. No division by a zero-length
   normal.
5. Four NDC corners → clip space, **keeping each endpoint’s
   original `clip.z` and `clip.w`** (post-step-2 values, so a
   clipped endpoint carries interpolated `z`/`w`):

   ```
   clip.xy = (ndc.xy + Δndc * side) * clip.w
   clip.z  = parent clip.z
   clip.w  = parent clip.w
   ```

6. Emit two triangles. Broadcast each parent endpoint’s attributes
   to both corners of that end (see Attribute policy). Run
   `gen_stipple_tex_coords()` on the line endpoints first (or
   fold it into the expander loop) and broadcast each `s` to
   both corners; `t = 0` samples the 16×1 `GL_REPEAT` stipple
   texture safely across the quad. `s` then interpolates along
   the line and stays constant across the width.

Caps are square. No miters, no round joins. Desktop aliased
`glLineWidth` is independent square-capped segments; `GL_LINE_STRIP`
/ `LOOP` just overlap those square ends. Matching that is the v1
contract.

### Strip / loop walk, including the closing edge

`GL_LINE_STRIP` and `GL_LINE_LOOP` are expanded pair-wise. Do **not**
inherit `gen_stipple_tex_coords`’s walk for the loop close:
`line.c:135–136` treats `GL_LINE_LOOP` as a strip and admits
“the last segment, the loop one, will look strange, but I will
not add a vertex for that.” Width emulation has to emit that
last→first segment or a `GL_LINE_LOOP` of N vertices is missing
an edge.

For stipple on that closing edge, compute `s` from the
window-space length of last→first ourselves. Do not ask
`gen_stipple_tex_coords` for a coordinate it never wrote.

`rlEnd` already fans merged strips/loops into indexed `GL_LINES`
pairs (including the loop close). Those arrive as `GL_LINES` and
need no extra walk. The explicit close is for a still-packed
`mode_init == GL_LINE_LOOP` list and for the `drawing.c`
client-array intercept.

Polygon-mode edges arrive as `GL_LINES` from `fill_lineIndices` /
the existing line-array expansion; they pick the emulator up for
free if the current width needs it, and stay on the cheap
`DrawArrays(GL_LINES)` path when it does not.

### Attribute policy

Duplicate every attribute the source list (or, on the
`drawing.c` path, the enabled VAO) actually carries — the same
set `build_line_arrays` already expands for polygon-mode lines:

| Attribute | Action |
|---|---|
| position | replaced by the four clip-space corners |
| color | duplicated per endpoint; interpolated at a near-plane clip |
| secondary color | same |
| fog coord | same (the draw still disables fog; keep the values so a later fog pass is not reading garbage) |
| every `tex[a]` | same; the stipple TMU’s `s` is the generated length coord |
| normal | duplicated even though lighting is off, so a leftover FPE lighting path cannot read an unbound pointer |

Presence of secondary / tex / fog / normal does **not** skip
emulation. If any expansion allocation fails, fall back to the
native 1 px `GL_LINES` draw — the same missing-array fallback
`build_line_arrays` already has. Do not emit a half-expanded
quad.

### Identity-MVP draw

Around the triangle draw only. Do **not** call public
`glPushMatrix` / `glPopMatrix`: a scene that is already at
`GL_MAX_MODELVIEW_STACK_DEPTH` (or the projection twin) would
`GL_STACK_OVERFLOW` and silently fail the draw. Save the top 16
floats of each stack into locals, poke identity into
`glstate->modelview_matrix` / `glstate->projection_matrix`, set
`glstate->mvp_matrix_dirty = 1`, draw, write the 16 floats back,
mark dirty again. FPE then does `gl_Position = I * clip` for the
duration.

Raster / enable state the bracket must force, and restore:

| State | Why |
|---|---|
| `glPolygonMode(GL_FRONT_AND_BACK, GL_FILL)` | `drawing.c:260` intercepts `polygon_mode == GL_LINE && mode >= GL_TRIANGLES`. Expanded quads drawn as `GL_TRIANGLES` under wireframe/hidden-line would be re-expanded into 1 px outlines of themselves. This is the same path verification case 6 is there to protect. |
| `glDisable(GL_CULL_FACE)` | Quad winding follows the window-space segment direction. With culling on, roughly half the segments vanish. |
| `glDisable(GL_POLYGON_OFFSET_FILL)` (and the line/point offset enables if they can affect the fill draw) | Offset is in primitive-type units; leaving it on shifts the fake quads relative to the geometry they were meant to sit on. |
| lighting / texgen off | Lighting the clip-space corners is nonsense; chrome is unlit `glColor`. |
| fog off | Fog would use the same wrong eye position. **Known deviation** — see below. |

Clip planes: skip emulation while a plane is enabled, rather than
trying to clip the object-space segment in v1. Also a known
deviation.

Do not introduce a new FPE program key. The point of the identity
MVP is to reuse the shader that is already bound.

### When to activate

```
requested_width > (GLfloat)hardext.maxlinewidth
```

`maxlinewidth` is an `int`. There is no separate tolerance
constant; the cast is just so the comparison is not int-vs-float
by accident. On WebGL `maxlinewidth` is 1, so every width greater
than 1 takes the quad path. On a GLES driver that can do 8 px
natively, leave it alone — this patch must not change those
pixels.

Treat `width <= 1` as native 1 px in v1. The grid uses `0.9f` on a
faded rank; desktop `GL_LINE_SMOOTH` can actually draw that, we
cannot until the follow-up AA step. Emitting a 0.9 px quad without
coverage AA is just a 1 px quad with more math.

### Compiled-list width, not a new immediate-mode flush

Immediate mode already flushes (see Problem). Do not add another
`FLUSH_BEGINEND`, and do not copy `rlPointSizeOp`.

What remains:

- **Trailing-reset oracle against the current tree first.**
  `glLineWidth(8); draw; glLineWidth(1)` should already keep the
  first batch at whatever width the flush-time mirror held — which
  is 8, because `PUSH_IF_COMPILING` flushes *before* the wrapper
  updates the mirror. Run this before writing any flush code. If
  it fails, the failure is somewhere other than “we forgot to
  flush,” and the fix is not the point-size patch.
- **`linewidth_op` only if compiled `glNewList` needs it.**
  `PUSH_IF_COMPILING` records a `STAGE_GLCALL`. Establish whether
  that packed `glLineWidth` replays before the geometry that
  follows it in the same list. If it does, leave it; the wrapper
  updates the mirror on replay and the expander reads the mirror
  at draw. If it does not (geometry node drawn with the
  *pre-call* width), add a dedicated `linewidth_op` /
  `linewidth_val` exclusive stage, replayed in `listdraw.c` next
  to the point-size op — and only then.
- A list with no in-list width op uses the current mirror at draw
  time. That is the polygon-mode case: wireframe uses whatever
  `glLineWidth` is in force, almost always 1.
- `islistscompatible_renderlist` only needs a width check if two
  geometry nodes with different stamped widths can actually merge.
  Immediate mode will not produce that. A compiled list that
  splits on `STAGE_GLCALL` will not either. Do not add the check
  speculatively.

Keep the mirror-equality early-out **ahead** of
`PUSH_IF_COMPILING` (move it up). A same-width call must not
flush, must not record a packed call, and must not hit GLES.

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

### Draw-time VBO / client-array contract

`listdraw.c:894–902` can call `listActiveVBO` on the source list
or on the polygon-line `line_arrays` cache *before* the GLES
draw. That binds `GL_ARRAY_BUFFER` and rewrites every enabled
attrib’s `real_pointer` to a **VBO offset**. A naïve
`gles_glVertexPointer(..., cpu_quads)` after that interprets the
CPU pointer as an offset into the still-bound buffer — empty
frame, or a GPU page fault.

The wide-line path therefore **does not share that draw**. Hook
**before** `listActiveVBO`:

1. Source the expansion from the **CPU** arrays only
   (`list->vert` / `list->color` / …, or `line_arrays->vert` if
   the polygon-line expansion already ran). Never from
   `list->vbo_vert` — those are offsets, not addresses.
2. Skip `listActiveVBO` / `line_arrays_to_vbo` for this draw.
   The object-space VBO cache can stay allocated for the width-1
   path; this frame just does not bind it.
3. `bindBuffer(GL_ARRAY_BUFFER, 0)` and
   `bindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)` (or
   `wantBufferIndex(0)`) so the subsequent pointer calls are
   genuine client arrays.
4. Point the enabled attribs at the transient expanded buffers
   (`gles_glVertexPointer` / `ColorPointer` / …).
5. Identity-MVP + `GL_FILL` + cull/offset off, then
   `gles_glDrawArrays(GL_TRIANGLES, 0, expanded_count)`.
6. Restore the previous ARRAY/ELEMENT bindings. No
   `listInactiveVBO` is needed if step 2 skipped the activate.

If a future hook has to sit *after* `listActiveVBO`, it must
`listInactiveVBO` and unbind first. Do not upload the transient
quads into the list’s existing `vbo_array` — that buffer is the
object-space cache and is the wrong lifetime.

On the `drawing.c` intercept the arrays are already client-side
(that is why the intercept exists). Same unbind-before-pointer
rule if a user VBO happens to be bound.

### Advertise a real range

Add getter cases for **two** pnames, not three:

- `0x0B22` — `GL_LINE_WIDTH_RANGE` / `GL_SMOOTH_LINE_WIDTH_RANGE`
  (same token; one `case` label, using whichever name is convenient,
  with a comment that the other is an alias)
- `GL_ALIASED_LINE_WIDTH_RANGE`

Plus granularity:

- `GL_LINE_WIDTH_GRANULARITY` (`0x0B23`; `GL_SMOOTH_LINE_WIDTH_GRANULARITY`
  is the same token if that name is in scope — do not write both
  as `case` labels)

Values:

- if `hardext.maxlinewidth > 1`, report the driver’s range
  (unchanged) and its granularity
- else report `[1, 64]` (or another documented cap the expander
  actually clamps to) and granularity `1.0f`

Both `glGetFloatv` and `glGetIntegerv` need the cases (two elements
for a range, one for granularity). `gl4es_commonGet` is the usual
place so both getters share them.

`glGet(GL_LINE_WIDTH)` already returns the mirror. The expander
clamps the requested width to that advertised max so a scene that
asks for 200 px cannot allocate a pathological quad.

`gl2d_begin()` already forces `glLineWidth(1)` and disables
`GL_LINE_SMOOTH`. 2D chrome that sets 1.5–2 px *inside* that
bracket (color-picker accents) will start working; that is
intended.

## Known deviations (v1)

Defensible, not accidental. Measure them in verification case 2
rather than discovering them in a fogged or clipped scene later.

| Deviation | What the user sees | Why v1 accepts it |
|---|---|---|
| Fog disabled for the expanded draw | Wide lines render unfogged against a fogged native reference | Fog distance is computed from `MV * object`; the vertices are clip-space corners. Doing it right means either CPU fog into the vertex color or an FPE program key. |
| Lighting disabled for the expanded draw | Lit wide lines lose per-vertex lighting | Same reason. gl-repl chrome is unlit `glColor`. |
| Clip plane enabled → skip emulation | Wide lines in a clipped scene stay 1 px | Object-space clip of the segment is real work; v1 will not pretend it did it. |
| `width <= 1` stays native | `0.9f` grid minors are 1 px | Coverage AA is the smooth follow-up. |

A later FPE expander, or a CPU fog/clip pass, can close these
without changing the expansion math.

## Patch shape

New file
[`packaging/web/patches/gl4es-line-width-quads.patch`](../../../packaging/web/patches/gl4es-line-width-quads.patch),
**last** in `GL4ES_PATCHES` in `scripts/web-deps.sh`. It will
retouch `wrap/gles.c` that getter-client-state already patched, so
it cannot sit earlier.

Likely hunks:

| File | Change |
|---|---|
| `src/gl/line.c` / `line.h` | `expand_wide_lines` + 4D near clip + identity-MVP draw (local matrix save, `GL_FILL`, cull/offset off) |
| `src/gl/listdraw.c` | call it for line modes when width needs it, **before** `listActiveVBO`; skip VBO activate on the wide path; replay `linewidth_op` only if step 1 showed `STAGE_GLCALL` is not enough |
| `src/gl/drawing.c` | intercept `glDrawArrays` / `glDrawElements` of `GL_LINES` / `GL_LINE_STRIP` / `GL_LINE_LOOP` when `gl4es_mirror_line_width > (GLfloat)hardext.maxlinewidth` (stipple stays `GL_LINES`-only) |
| `src/gl/wrap/gles.c` | move the mirror-equality early-out **ahead** of `PUSH_IF_COMPILING`; do not add another flush |
| `src/gl/list.h` / `list.c` | `linewidth_op` / `linewidth_val` **only if** the compiled-list oracle needs it |
| `src/gl/getter.c` | `0x0B22` and `GL_ALIASED_LINE_WIDTH_RANGE` (and `0x0B23` granularity) in `gl4es_commonGet` |

Header comment on the patch follows the existing house style
(problem, what was tried, what landed). Add an inventory row and a
dated section to
[`packaging/web/patches/README.md`](../../../packaging/web/patches/README.md)
while the measurements are still available.

No gl-repl C changes in v1. Native and stub builds never see the
patch. A bench under `packaging/web/bench/` is a new file in that
tree, not a gl-repl C change, and is not in `$(SRCS)`.

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
   cannot report a speedup. Also a **zero-length** segment
   (`p0 == p1`, width > 1): no NaN, either an axis-aligned square
   or a clean skip.
2. **Headful screenshots** of: default grid + axes, orbit gizmo
   (6 px halo), a live transform guide, Bold vertex outlines, the
   Points & Lines tutorial. Compare against native, not against
   “looks thicker.” **Also a fogged scene and a clip-plane scene**
   so the Known deviations (unfogged wide lines, 1 px under a
   clip plane) are measured rather than discovered.
3. **Culling on.** `glEnable(GL_CULL_FACE)` + `glFrontFace` both
   windings, a handful of wide segments whose window-space
   direction flips. Every segment stays visible; the bracket
   disabled cull. A missing disable shows up as “every other
   line vanished.”
4. **Trailing-reset oracle, against the current tree first.**
   Immediate mode only: `glLineWidth(8); draw; glLineWidth(1)`
   must keep the first batch at 8 px. This is how gl-repl
   actually writes overlays, and `PUSH_IF_COMPILING` is supposed
   to already make it true. If it fails before the patch, that
   is a finding, not a reason to copy the point-size patch.
   Named-list replay is case 9, not this case.
5. **Near-plane.** Orbit around a long axis line until it passes
   through the camera **and** until an endpoint sits between the
   eye and the near plane (`w > 0` but `z + w < 0`). No giant
   slivers, no inverted lines, no NaN uploads. A stippled wide
   line clipped this way must keep a continuous dash pattern
   (attributes interpolated at `t`, not snapped to an endpoint).
6. **Stipple + width 2.** Occluded-axis ghost (`GL_LINE_STIPPLE` +
   a non-1 width) still dashes. The expander must broadcast the
   stipple `s` that `gen_stipple_tex_coords` already produced.
7. **`GL_LINE_LOOP` / `GL_LINE_STRIP`, both paths.** A 4-vertex
   loop at width 4, once via `glBegin`/`glEnd` and once via
   `glDrawArrays(GL_LINE_LOOP, …)`. Coverage must include the
   closing last→first edge (the one `gen_stipple_tex_coords`
   skips). A strip of the same verts is missing that edge, so
   the two are distinguishable — use that as the oracle, not
   “it looks closed.”
8. **Wireframe / hidden-line stay at 60 FPS, and stay 1 px.**
   Those are width 1 and must remain on the existing
   `DrawArrays(GL_LINES)` path. This is also the path that would
   re-expand expanded quads if the bracket forgot `GL_FILL`. A
   regression here — slower *or* “wireframe of the halo quads” —
   is a failed patch even if wide chrome looks perfect.
9. **Named display list** — not covered by the trailing-reset
   case. All of:

   - One compiled list containing a 1 px segment, a
     `glLineWidth(4)` op, and a 4 px segment, played back
     across a camera orbit. This is the `STAGE_GLCALL` /
     `linewidth_op` replay oracle from step 1.
   - `glCallList` of a list that recorded width 4 while the
     current mirror is 1, and the reverse (unstamped list
     drawn while the current width is 4). Stamped lists keep
     their recorded width; unstamped lists follow the mirror.
   - `glNewList` / `glCallList` of a source list, then
     `glDeleteLists` on the source (the `append_calllist`
     shallow-copy case from the polygon-line patch). The
     caller must not share or double-free transient expanded
     storage, and must not inherit a VBO of object-space
     verts as if it were the quad cache.
   - A compiled width-4 list whose `list2VBO` path has
     already filled `vbo_array`. First draw and later draws
     both produce the wide quad, not an empty frame from
     treating CPU pointers as VBO offsets.

10. **Patch mechanics.** `git apply --check` clean on the pristine
   pin and on the pin with the earlier patches applied;
   reverse-apply against the tested tree. `make check-c99` does
   **not** cover this: it syntax-checks `$(SRCS)`
   (`Makefile:2390`), and a new file under `packaging/web/bench/`
   is not in that list. `check-trailing-whitespace` explicitly
   excludes `packaging/web/patches/` (`Makefile:2406`). Instead:
   `grep -n ' $'` on the new patch, `web-deps.sh`, this plan, and
   the bench; and an `emcc -fsyntax-only` (or the existing
   `make bench-web-gl4es` compile) of the bench.

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
- **Closing the Known deviations** (fog, lighting, clip-plane skip)
  in this patch.

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

1. **Oracles on the current tree, no visual change.** Run the
   trailing-reset scene against unpatched-for-width gl4es. It
   should already flush. Then compile a mixed-width display list
   and dump / single-step replay order of the `STAGE_GLCALL`
   `glLineWidth` against the geometry nodes. Only if that order
   is wrong, add `linewidth_op`. Move the mirror-equality
   early-out ahead of `PUSH_IF_COMPILING` in the same step
   (batching; no pixel change).
2. `expand_wide_lines` in `line.c` and the `listdraw.c` hook
   **before** `listActiveVBO`, `width > (GLfloat)hardext.maxlinewidth`
   only. Identity-MVP draw via local matrix save, 4D near clip,
   `GL_FILL` + cull and offset off, square caps, unbind
   ARRAY/ELEMENT, expand from CPU arrays only. Emit the
   `GL_LINE_LOOP` close.
3. `drawing.c` intercept for `GL_LINES` / `GL_LINE_STRIP` /
   `GL_LINE_LOOP` so client-array draws cannot sneak past.
4. Getter range + granularity + clamp.
5. Bench + the verification cases above, including the cull-on
   scene, the loop-close coverage oracle, the fogged and
   clip-plane screenshots, and the named-list suite.
6. Patch header, `GL4ES_PATCHES` append, README inventory + dated
   section.

Do not start by adding a flush. The flush is already there.

## Rejected approaches

Recorded so they are not rediscovered during implementation.

- **Geometry shader.** WebGL 2 still has none.
- **Pass-through to GLES `glLineWidth`.** Already what we do; ANGLE
  ignores it.
- **Literal window-space `glOrtho`.** Works, but Z and
  perspective-correct interpolation become a second problem. Clip
  space + identity MVP is the same 2D expansion without that remap.
- **Public `glPushMatrix` / `glPopMatrix` for the identity MVP.**
  Uses the application's matrix stack and can
  `GL_STACK_OVERFLOW`. Save 16 floats, poke identity, restore.
- **Copy `gl4es-point-size-batch.patch`.** That wrapper has no
  `PUSH_IF_COMPILING`. This one does, and it already flushes.
- **Separate `case GL_LINE_WIDTH_RANGE` and
  `case GL_SMOOTH_LINE_WIDTH_RANGE`.** Same token (`0x0B22`);
  that is a duplicate-case compile error.
- **Only hook `listdraw.c`.** Misses `glDrawArrays` of line
  primitives from `drawing.c`. The stipple intercept is
  `GL_LINES`-only; the wide-line condition has to name strip
  and loop too.
- **Reuse `gen_stipple_tex_coords` as the segment walker.**
  It skips the `GL_LINE_LOOP` close on purpose. Width
  emulation cannot.
- **Expand in the FPE vertex shader first.** See Non-goals. The
  CPU path reuses the stipple projector and does not fragment the
  shader cache.
- **Cache expanded geometry on `glEndList`.** Camera-dependent.
  The polygon-line cache is the wrong precedent here; it caches
  object-space edge topology, not window-space offsets.
- **`w > ε` as the only clip.** Misses the between-eye-and-near
  case (`w > 0`, `z + w < 0`) and still inverted-projects
  `w ≤ 0` if the test is on the wrong side of the divide.
- **Expand after `listActiveVBO` and point at CPU arrays.**
  The bound `GL_ARRAY_BUFFER` makes those pointers VBO
  offsets. Hook before the activate, or unbind first.
