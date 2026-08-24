# gl4es patch notes

This directory contains local patches applied to the pinned gl4es checkout by
[`scripts/web-deps.sh`](../../../scripts/web-deps.sh). This file is the running
investigation log for those patches: why a patch exists, what evidence led to
it, which tempting approaches failed, and how its behavior was verified.

The patch files themselves remain the executable source of truth. New patches
should add an entry here while the measurements and failed hypotheses are
still available. Older patches predate this log; their inventory below is
derived from their header comments rather than reconstructed history.

The web build currently pins gl4es commit
`17f0894e19d1553e4176276c759915dab44c08e2`. Patches are applied in the order
listed in `GL4ES_PATCHES` in `scripts/web-deps.sh`. That script stamps the pin
SHA plus a hash of every patch file; a mismatch resets the managed
`third_party/web/gl4es` clone and rebuilds it. A failed `git apply --check` is
not treated as “already applied”.

## Patch inventory

| Patch | Purpose |
|---|---|
| `gl4es-rasterpos-perspective-divide.patch` | Perspective-divide clip coordinates before deriving the raster position; invalidate on `w<=0` / out of clip. |
| `gl4es-bitmap-dirty-clear.patch` | Clear only the dirty CPU bitmap rectangle between glyph batches. |
| `gl4es-getter-client-state.patch` | Answer tracked `glGet*` state locally instead of synchronously draining WebGL. |
| `gl4es-color-material-face.patch` | Track front/back `GL_COLOR_MATERIAL` independently in the fixed-pipeline emulator. |
| `gl4es-pushattrib-gaps.patch` | Fill polygon, line, point, and transform state gaps in `glPushAttrib`/`glPopAttrib`. |
| `gl4es-pushattrib-texenv.patch` | Preserve texture environment mode and color per texture unit. |
| `gl4es-accum-fbo.patch` | Implement the accumulation buffer with an internal FBO. |
| `gl4es-accum-deferred-return.patch` | Cache LOAD/ACCUM snapshots and reduce their weights once at RETURN. |
| `gl4es-accum-deferred-scissor.patch` | Size and copy deferred samples to the WebGL scene scissor. |
| `gl4es-point-smooth.patch` | Emulate round antialiased points in the GLES2 fixed-pipeline shader. |
| `gl4es-polygon-line-drawarrays.patch` | Avoid Emscripten's client-index upload and scan for polygon-mode lines. |
| `gl4es-point-size-batch.patch` | Apply `glPointSize` to the batch it was called on, not to whatever is still pending. |
| `gl4es-polygon-offset-line.patch` | Shadow `GL_POLYGON_OFFSET_LINE` and apply a projection-row depth bias around polygon-mode line draws. |
| `gl4es-line-width-quads.patch` | Expand `glLineWidth` > 1 into screen-space quads on `[1, 1]` line-width stacks. |
| `gl4es-edge-flag.patch` | Implement `glEdgeFlag`: build polygon-mode line edges from the original primitive topology and drop the suppressed ones. |
| `gl4es-polygon-line-quad-edges.patch` | Draw quad and quad-strip boundary edges under polygon-mode lines instead of the triangulation's spokes and diagonals. |
| `gl4es-getbooleanv-local-state.patch` | Answer `glGetBooleanv` from tracked state instead of a synchronous WebGL `getParameter`. |

See each older patch's leading prose for the detail that is available.

## 2026-08-24: `glEdgeFlag` under polygon-mode lines

Patch: [`gl4es-edge-flag.patch`](gl4es-edge-flag.patch)

### Premise

gl4es lists `glEdgeFlag` in `src/gl/wrap/glstub.c`, so it did nothing, and
`globals4es.silentstub` defaults to 1 - the call was discarded without a word.
Under `glPolygonMode(..., GL_LINE)` that means every edge is drawn, including
the boundary edges edge flags exist to hide.

`glGetBooleanv(GL_EDGE_FLAG)` was unhandled as well. It is not part of the
shared `gl4es_commonGet()` switch that answers `glGetIntegerv`/`glGetFloatv`,
and `glGetBooleanv` had no gl4es-side implementation at all, so the query fell
straight through to GLES and raised `GL_INVALID_ENUM` without writing the
parameter.

The reach is the wireframe, hidden-line, vertex-outline and polygon-highlight
views, and the GLU tessellator: `executor.c` and `hidden_lines.c` both wire
`GLU_TESS_EDGE_FLAG` directly to `glEdgeFlag`, which is what keeps a
tessellated concave polygon's outline clean instead of showing every interior
diagonal. `tests/scenes/general/polygon-edge-flags.glr` is the scene that
demonstrates it; before this patch its middle panel rendered identically to
its left one on the web.

### The filter that did not work

The first implementation was a compaction over `fill_lineIndices()`'s output:
that function emits every edge as an index pair, so dropping the pairs whose
first index carried a cleared flag looked like the whole job, and it left the
intricate per-mode generation untouched.

The coverage oracle rejected it. Suppressing a quad's vertex 0 removed 879
pixels where one edge is worth 320, and suppressing vertex 3 removed nothing.
Dumping the generated pairs explained both numbers at once - a four-vertex
`GL_QUADS` block comes out of `fill_lineIndices()` as:

```
(0,1) (0,2) (1,2) (0,3) (2,3)
```

That is the *triangulated* quad: five edges including the interior diagonal
`(0,2)`, with the pairs unoriented, so the boundary edge `3->0` appears as
`(0,3)`. Filtering on the first index therefore drops three edges for vertex 0
and none for vertex 3. Neither "which vertex starts this edge" nor "is this
edge a boundary at all" survives triangulation, so no filter over those pairs
can be correct.

### What works

The vertex array is still in specification order - triangulation reorders the
*indices*, not the vertices - so flagged geometry gets its edges built from the
original primitive topology instead, in `build_lineIndices_edgeflag()`.
`modeinit_t` gains a `vlen` field recording each block's cumulative vertex
span, because `ilen` is in index space once a list carries indices and no
longer marks the primitive grouping.

`GL_TRIANGLES`, `GL_QUADS`, `GL_QUAD_STRIP` and `GL_POLYGON` are
reconstructed - each emits at most 2 indices per vertex, inside the caller's
4-per-vertex allocation. Triangle strips and fans return -1 and fall back to
`fill_lineIndices()` with the flags ignored, as before: their edge count is not
bounded by that allocation, and they are not where edge flags are used.

Storage is a `GLubyte *edgeflag` on `renderlist_t`, one byte per vertex. NULL
means "every vertex is `GL_TRUE`", so geometry that never clears a flag
allocates nothing and pays one predictable null check per vertex in
`rlVertexCommon()`. It stays out of the interleaved `merger_master` layout,
which is exactly full at 20 floats per vertex and would otherwise grow for
every vertex in the process.

### Oracle

`make bench-web-gl4es` builds `gl4es-edge-flag.html`. Each case calibrates
itself - every edge under test is first drawn alone as a `GL_LINES` segment -
so the expectations come from the rasterizer rather than a hard-coded count,
and the assertions are absolute rather than deltas.

Headless Chrome/SwiftShader (640x480):

```
q0=799/800 q1=880/880 q2=800/800 q3=879/880
t0=576/576 t1=672/672 t2=672/672
default=1119/1119 lone=1119/1120 nodiag=1468/1468/1472
qstrip=1620/1624 qstripflag=1500/1504 pervertex=1280/1024
fill=76800/76800 pushattrib=1/1 getter=1/1 displaylist=1/1 merged=2170/2176
```

The `fill-capacity` case also emits a 96-vertex `GL_TRIANGLES` FILL block
with `glEdgeFlag(GL_FALSE)` set before `glBegin`, immediately after a merger
batch has grown the shared capacity.  Its coverage must match the all-TRUE
control block and it must leave `GL_NO_ERROR` set.  Because FILL does not
consume the edge array, the patch also exposes a test-only capacity hook; the
oracle directly rejects a requested capacity below 96 instead of relying on
a silent heap overrun to disturb rendering.

Each of the quad's four and the triangle's three suppressions lands within a
pixel of the calibrated edge it should remove. `gl4es-line-width` reproduces
its documented pre-patch numbers exactly (`w1 20480 w1.5 32768 w3 61440
w6 122880 loop 2672 strip 2104 zero 16`) and `gl4es-render` still reports
PASS, which is the evidence that the unflagged path is untouched - as it must
be, since `list->edgeflag` stays NULL there and the generator is never called.

The scene itself was checked end to end by rendering its geometry on native GL
(Mesa 4.6 compat / llvmpipe through surfaceless EGL) and on gl4es, counting
covered pixels the same way: the boundary star matches at **498 px on both**,
and the full mesh star differs by 4 px in ~1394 (rasterizer, not topology).

### Odds and ends

`glEdgeFlag` needs no packed-call entry for display lists: a compiling list
picks the flag up through its own `lastEdgeFlag`, and a freshly extended one
copies the previous list's `lastEdgeFlag`. A separate explicit-state bit
ensures that executing a list only changes the caller's flag when the list
actually contains `glEdgeFlag`; inherited compile-time state is not a command.
The per-vertex array is materialized only when a vertex is emitted, after the
list's merger capacity is known.
`glEdgeFlagv` is promoted out of the stub table alongside it. `GL_CURRENT_BIT`
now saves and restores the flag.

The query side needs both halves. `GL_EDGE_FLAG` joins `gl4es_commonGet()`,
which serves `glGetIntegerv`/`glGetFloatv`/`glGetDoublev`; `glGetBooleanv` had
no gl4es-side implementation at all, so it gets one (`skip_glGetBooleanv`)
that answers `GL_EDGE_FLAG` and passes every other pname straight through,
exactly as before. Answering the *rest* of gl4es's tracked state locally is
worth doing but is a separate decision, and a separate patch.

The client-array path (`glEdgeFlagPointer`) stays stubbed - gl-repl draws in
immediate mode, so renderlists cover it.

### A pre-existing bug left alone

`fill_lineIndices()` reads `modes[k].mode_init` and `modes[k].ilen`, where `k`
is the *output write cursor*, not `m`, the block loop counter. For a single
block `m == k == 0` so it works by accident; for a multi-block merged list it
indexes `modes[]` by the number of indices written so far.
`build_lineIndices_edgeflag()` uses `modes[m]` correctly, but fixing the
original changes behavior on merged lists well beyond this work, so it is
noted rather than touched.

## 2026-08-24: quad and quad-strip boundary edges under polygon-mode lines

Patch: [`gl4es-polygon-line-quad-edges.patch`](gl4es-polygon-line-quad-edges.patch)

Found while reviewing the edge-flag patch above, and independent of it: this
one changes *unflagged* rendering, so it is kept separate to stay revertable
on its own.

### Premise

Under `glPolygonMode(..., GL_LINE)`, gl4es drew a different edge set for the
same quad depending only on how many quads shared a `glBegin`/`glEnd`:

| geometry | gl4es | native | perimeter |
|---|---:|---:|---:|
| one quad | 485 | 367 | 368 |
| four quads, one `glBegin` | 1468 | 1468 | 1472 |
| four quads, four `glBegin` | 1939 | 1468 | 1472 |

Two or more quads per block go through `renderlist_quads2triangles()` to
`GL_TRIANGLES`, where `fill_lineIndices()` recovers the topology from
`mode_init == GL_QUADS`. That path was always right.

A lone quad is relabelled `GL_TRIANGLE_FAN` by `end_renderlist()` - a
4-vertex fan *is* the quad, so nothing is expanded - but `case
GL_TRIANGLE_FAN` only special-cased `GL_QUAD_STRIP` and `GL_POLYGON`. With no
`GL_QUADS` arm it fell through to the generic fan reconstruction, which emits
the spokes: `(0,1) (0,2) (1,2) (0,3) (2,3)` - five edges, the extra `(0,2)`
being the diagonal that splits the quad.

`GL_QUAD_STRIP` had the same shape of bug and is fixed here too.
`end_renderlist()` relabels it to `GL_TRIANGLE_STRIP`, and that arm had **no
`mode_init` check at all**, so a quad strip came out with the strip's diagonal
across every quad - 2124 px on a 4-quad ladder against a 1624 px outline,
where native draws 1620.

### Fix

For the lone quad, the ring the `GL_POLYGON` arm already walks over `[z, len)`
is exactly the four boundary edges of a 4-vertex block, so `GL_QUADS` joins
that arm. For the strip, quad q is bounded by
`v(2q) -> v(2q+1) -> v(2q+3) -> v(2q+2)`, so those four edges are emitted;
adjacent quads share a rung and emit it twice, coincident, which is what
native does when it draws each polygon's boundary.

The pre-existing `mode_init == GL_QUAD_STRIP` arms under `GL_TRIANGLE_FAN` and
`GL_TRIANGLES` are **not** reused, because they are wrong: for a 10-vertex
strip they emit the rungs plus *both* diagonals of each quad and no rails at
all. They are unreachable, which is why that was never visible; this patch
leaves them alone rather than widening its scope. The `len==4` sub-branch
under `case GL_TRIANGLES` / `case GL_QUADS` is unreachable for the same
reason - a 4-vertex quad list never has `mode == GL_TRIANGLES`.

### Native reference

Mesa 4.6 (Compatibility Profile) / llvmpipe through surfaceless EGL, same
geometry and pixel counting as the browser oracle. Native is insensitive to
batching. After this patch gl4es reports 1119 / 1468 / 1468 for the quad cases
(the oracle's lone quad is the larger one, 1120 px perimeter) and 1620 for the
quad strip - native to the pixel in every case.

### Scope

This changes *unflagged* rendering wherever single quads or quad strips are
drawn in wireframe, hidden-line, vertex-outline or polygon-highlight views -
the diagonals go away. That is the fix working: they were never in native GL's
output.

## 2026-08-24: `glGetBooleanv` from tracked state

Patch: [`gl4es-getbooleanv-local-state.patch`](gl4es-getbooleanv-local-state.patch)

Split out of the edge-flag patch, which needed only `GL_EDGE_FLAG` answered
locally. Widening that to every tracked pname is a separate decision with a
separate risk - and this is the patch to drop first if a boolean query is ever
seen returning stale state.

### Premise

`gl4es-getter-client-state.patch` already routes `glGetIntegerv` /
`glGetFloatv` / `glGetDoublev` through `gl4es_commonGet()`, because a `glGet*`
that reaches WebGL is a synchronous `getParameter` and drains the command
queue. `glGetBooleanv` was left out - it had no gl4es-side implementation at
all, so every boolean query went to the driver.

### Measurement

The same benchmark linked against gl4es built with and without this patch,
headless Chrome / SwiftShader, 20000 iterations of
`glGetBooleanv(GL_DEPTH_WRITEMASK)` + `glGetIntegerv(GL_SHADE_MODEL)`, five
runs per build:

| build | median | range |
|---|---:|---|
| without | 37.2 us/pair | 34.8 - 57.1 |
| with | 0.14 us/pair | 0.135 - 0.185 |

The ranges do not overlap. `glGetIntegerv` was already local in both builds,
so the whole difference is the `glGetBooleanv` round trip.

For contrast, the same A/B over the **draw** path - immediate-mode vertex
submission, polygon-mode lines, quads and quad strips - produced no resolvable
difference: medians within a few percent with run-to-run ranges overlapping
almost entirely (`fill` varied 8.7-17.0 ms on *both* builds). Note also that
`--virtual-time-budget` fast-forwards timers, so in-page `performance.now()`
is not wall-clock; one run reported all zeros and was discarded. Treat the
draw-path numbers as "below the noise floor", not as evidence either way.

### Tradeoff

The one the existing getter patch already accepted: a pname whose tracked value
is stale now returns the stale value rather than the driver's. Checked against
native GL (Mesa 4.6 compat / llvmpipe through surfaceless EGL) on
`GL_DEPTH_WRITEMASK`, `GL_DEPTH_TEST`, `GL_BLEND`, `GL_CULL_FACE` and
`GL_EDGE_FLAG`, each set and cleared - all ten agree with native. That is ten
pnames, not the whole enum space.

## 2026-08-24: deferred accumulation performance

The two deferred-accumulation patches were measured separately rather than
assigning their combined result to both. Three browser builds used the same
full patch stack and differed only at the accumulation stage: the original
`gl4es-accum-fbo.patch`, that baseline plus deferred RETURN, and both deferred
patches. The workload was the default gl-repl logo at 1280x676, 16x
accumulation AA, and the browser-selected 4x canvas MSAA in the in-app
Chromium/WebGL2 browser. Each number combines two interleaved 300-frame
windows after a 120-frame warm-up (600 measured frames per variant).

| Accumulation stage | FPS | Mean callback | Incremental result |
|---|---:|---:|---:|
| Original float-FBO accumulation | 33.962 | 29.444 ms | baseline |
| + deferred RETURN reduction | 37.035 | 27.002 ms | **+9.05%**, -2.443 ms |
| + scissored snapshot textures/copies | 38.545 | 25.944 ms | **+4.08%**, -1.058 ms |

Together the two patches gained **13.49%** and saved **3.500 ms per frame**
against the original float-FBO path in this workload. These are attribution
numbers for one browser, viewport, and scene, not universal multipliers:
scene complexity, canvas layout, pass count, GPU, and thermal state change the
absolute result. The important boundary is that the second percentage is
incremental over deferred RETURN, not another comparison against the original
baseline.

## 2026-08-24: review follow-ups folded into existing patches

No new patch file. Fixes from a review of the whole set were folded into
the patch that owns the code:

- Raster position: `w<=0` / non-finite / NDC outside [-1, 1] sets
  `rPos_valid` so `glBitmap` does not keep drawing at the previous
  on-screen position.
- Bitmap realloc: always zero the new buffer (including realloc-while-drawing)
  and treat `malloc` failure as a no-op.
- Getters: `GL_DEPTH_CLEAR_VALUE` from `glstate->depth.clear`; scalars in
  `gl4es_commonGet`; viewport/scissor/clear-color in all three typed getters;
  a `viewport_known` / `scissor_known` flag so width 0 is not "unset".
  `glClearColorx` / `glClearDepthx` update the same client-side mirrors
  (and `glstate->depth.clear`) as their float setters.
- `GL_LIGHTING_BIT` saves/restores the `GL_COLOR_MATERIAL` enable.
- Accum: probe a blended draw before keeping RGBA16F; immediate `GL_ACCUM`
  forces `GL_FUNC_ADD`; `GL_ACCUM_CLEAR_VALUE`; `DeleteGLState` frees GL
  objects; reduce-shader failure is sticky. The format probe restores
  driver blend enable/func/equation, the ARRAY_BUFFER bind, and the
  tracked `gleshard` program/attribs after the raw GLES draw, so later
  state-aware `glBlendFunc` calls cannot keep `GL_ONE,GL_ONE`. Immediate
  `GL_ACCUM` still forces `GL_FUNC_ADD`; `GL_COLOR_BUFFER_BIT` does not
  save the blend equation, so the pre-call equation is restored after
  `glPopAttrib`. The `accum-probe-state` case in `bench/bench_render.c`
  is the state-hygiene regression oracle: it queries `GL_ACCUM_RED_BITS`
  while alpha blending is live, then checks that a translucent draw still
  uses the caller's enable and factors.
- `gl4es_probe_line_width_and_depth()` after a real context exists (first
  `glViewport` / offset draw / wide-line query), so Emscripten notest no
  longer leaves `depthbits` at the pre-window default forever.

## 2026-08-19: `GL_POLYGON_OFFSET_LINE` and `glLineWidth` on WebGL

Two patches, in this order.

`gl4es-polygon-offset-line.patch` is a pre-existing web bug: `GL_POLYGON_OFFSET_LINE` is not a GLES enum, so `glEnable` raised `GL_INVALID_ENUM` and changed nothing. GLES fill-offset does not apply to the `GL_LINES` draw that polygon-mode already lowered to, so wrapping that draw in `GL_POLYGON_OFFSET_FILL` is also a no-op. The fix shadows the LINE enable, mirrors factor/units (including `glPolygonOffsetx`) onto `GL_POLYGON_BIT`, and pokes `P_row2 += d · P_row3` around the line draw — `d = 2 · units · 2^{-depth_bits} / (Far − Near)`, clamped to `[-1, 1]`. Cached object-space `line_arrays` are never written. Independently testable at width 1: a vertex-outline pass over a solid face should stop speckling. Genuine `GL_LINE_LOOP` (the tessellation overlay) is out of scope; native `_LINE` never applied to line primitives. `maxlinewidth` and `depthbits` are probed on the notest path as well as the full test — Emscripten always calls `GetHardwareExtensions` with notest, and a skipped probe left both at 0.

`gl4es-line-width-quads.patch` is the width emulator. ANGLE reports `ALIASED_LINE_WIDTH_RANGE` `[1, 1]`; every `glLineWidth` was a no-op. Each segment is expanded to a clip-space quad and drawn under a temporary identity MVP (the helper from the first patch). The hook sits *before* `list2VBO` and `line_arrays_to_vbo` so a named display list does not upload an unused object-space VBO, and the object-space cache is not bound as if it were the quads. An unset tracked viewport (`width` or `height` `< 1`) skips the expander rather than clamping those to 1. Polygon-mode edges with `_LINE` on get GLES `FILL` offset on the generated triangles; genuine `GL_LINES` / `STRIP` / `LOOP` force FILL off. Width 1 stays on the cheap `DrawArrays(GL_LINES)` path. Getters advertise `[1, 64]` when the driver cannot widen a line.

Compiled `STAGE_GLCALL` `glLineWidth` already replays before geometry on the same node (`listdraw.c` packed-call walk), so there is no `linewidth_op`. The same-width early-out stays after `PUSH_IF_COMPILING`.

Known deviations, by design: fog and lighting are off for the expanded draw; a live clip plane skips emulation; tess `GL_LINE_LOOP` overlays stay unoffset.

Coverage oracle: `make bench-web-gl4es` builds `gl4es-line-width.html`. After fixing the extra `* 0.5` on the NDC half-extent, headless Chrome/SwiftShader (640×480) reports `line-width w1 20480 w1.5 32768 w3 61440 w6 122880 loop 2672 strip 2104 zero 16`. w6 is exactly `40×512×6`. The w6 floor is now `N_SEGS * 512 * 6 * 0.8` so a half-width implementation fails; loop − strip must be about one edge. Details: `docs/plans/active/gl4es-line-width-emulation.md`.

The same target also builds `gl4es-line-width-cases.html`, a state and
compiled-list regression page. Its current browser result is recorded in the
plan; it covers trailing resets, culling, near-plane clipping, stipple,
client-array loop closure, named-list copy/delete, polygon-mode list caching,
fill-offset isolation, width-1 polygon offset, and the wide path's
`instanceCount` replay. The page intentionally reports the known fog and
live-clip fallbacks instead of treating them as failures.

## 2026-08-02: the point-smooth workarounds come out

`gl4es-point-smooth.patch` is what the two chrome vertex-marker sites were
waiting on. Both had forked by target to avoid gl4es' square points:
`draw_vertex_point_overlay()` (edit_overlays.c) drew a scaled eight-triangle
octahedron per authored vertex, and `draw_vertex_point_marker()`
(geometry_guides.c) drew a `glutSolidSphere` halo/core pair. Commit 9b284167
then hoisted `glEnable(GL_POINT_SMOOTH)` out of the native-only arm at both
sites - the enable had never fired on the target that needs it.

With the patch in and the enable reaching gl4es, both fallbacks are gone: every
target draws the same attenuated, point-smoothed `GL_POINTS`. gl4es carries the
other half of the parity too - its fixed-pipeline vertex shader implements
`GL_POINT_DISTANCE_ATTENUATION` (`fpe_shader.c`, the `gl_PointSize = clamp(...
inversesqrt(...))` line), so web point markers shrink with camera distance the
way native ones do. The triangle proxies only approximated that: their world
radii were tuned to match at one camera distance and drifted either side of it.

Native GL is untouched by construction - the native arm became the only arm at
both sites, and the enable/disable keep their relative order.

### The second bug the swap uncovered: point size is applied at flush

Flipping the markers to `GL_POINTS` first produced markers that were drawn but
**exactly one pixel wide**, on every vertex, at every camera distance. Ruled out
by direct experiment in headless Chrome (SwiftShader reports
`ALIASED_POINT_SIZE_RANGE` `[1, 1023]`, so no driver cap was involved):

| Hypothesis | Test | Result |
|---|---|---|
| `GL_POINT_SMOOTH` discards everything | scene with `glPointSize(16)` + smooth | round 16px points - works |
| gl4es drops single-vertex point batches | one `glBegin/glEnd` per point vs. two points in one | both render |
| `glPushAttrib(GL_ALL_ATTRIB_BITS)` loses the size | same scene wrapped in push/pop | still 16px |
| point *attenuation* shrinks them | `@cfg point_attenuation = 0` | still 1px |
| a later `glPointSize` retro-applies | same scene + a trailing `glPointSize(1)` | **all four points collapse to 1px** |

gl4es takes the point size from its state at *flush* time, not at the time the
primitive is recorded, so one `glPointSize(1)` at the end of a pass shrinks
every point that pass already drew. Both overlay passes ended with exactly that
reset (`edit_overlays_render_vertex_points`, its glut `GL_POINT` polygon-mode
sibling, and `draw_vertex_point_marker`), which is why the first swap looked
like "gl4es still can't draw points".

The resets were redundant anyway - all three sites sit inside a
`glPushAttrib(GL_ALL_ATTRIB_BITS)` / `glPopAttrib()` bracket that restores the
size - so they are gone. `glPopAttrib()` restoring the size does **not**
retro-shrink the batch (verified by the push/pop row above); only an explicit
`glPointSize` call did.

Dropping the resets fixed the two marker sites, but not the bug: the same trap
was set at 18 other `glPointSize(1.0f)` calls in `axes.c`, `render.c`,
`overlays.c` and `transform_guides.c`, and at every user scene that restores a
point size. So the underlying gl4es behavior is now patched too -
`gl4es-point-size-batch.patch`, below.

Fallout worth knowing: `test_edit_overlays` leaves `WEB_TEST_EXCLUDE` (it was
excluded *only* because the octahedron emitted unit-corner coords instead of
the marker's world coordinates), so `make test-web` covers 74 of 76 binaries
instead of 73, and `test_render3d_guides` / `test_render3d_render` drop their
`glutSolidSphere` arms.

## 2026-08-02: `glPointSize` applies to the batch it was called on

Patch: [`gl4es-point-size-batch.patch`](gl4es-point-size-batch.patch)

The finding above, fixed at the source instead of routed around.

gl4es defers immediate-mode geometry into a renderlist and draws it later; the
point size reaches the GLES2 fixed-pipeline shader as the `gl_Point.size`
uniform, uploaded from `glstate` at draw time (`fpe.c`'s
`builtin_pointsprite.size`). `gl4es_glPointSize` only wrote `glstate`, so the
last size set before the flush applied to every point in the pending batch -
a conformance break with desktop GL, where the size is fixed per primitive when
the primitive is issued.

`gl4es_glPointParameterfv`, ten lines up in the same file, already gets this
right, so the patch copies it rather than inventing a mechanism:

- compiling a display list -> record the size as a renderlist op
  (`pointsize_op` / `pointsize_val` via `rlPointSizeOp`, replayed in
  `listdraw.c` next to the pointparam op) and return;
- otherwise -> `gl4es_flush()` the pending batch before the state changes.

The op reuses `STAGE_POINTPARAM`, an exclusive stage, so a second size within
one list starts a new renderlist instead of overwriting the first, and
`list.c`'s merge-compatibility test refuses to merge a list carrying the op -
both mirroring the pointparam op exactly.

Oracle: a scene drawing four 16px `GL_POINTS` and then calling
`glPointSize(1)`. One-pixel dots before, four full-size round points after,
with `GL_POINT_SMOOTH` on or off. The patch reverse-applies against the built
tree and forward-applies against the pinned checkout with the earlier patches
on it (it is last in `GL4ES_PATCHES`).

Not fixed here, and worth knowing: `glLineWidth` is not wrapped by gl4es at all
(it goes straight to GLES), so it has the same deferral exposure with no
client-side state to flush on. No symptom chased down yet.

## 2026-08-02: polygon-mode lines via `glDrawArrays`

Patch: [`gl4es-polygon-line-drawarrays.patch`](gl4es-polygon-line-drawarrays.patch)

### Premise

The Emscripten build became dramatically slower whenever a feature redrew
triangles as lines. Affected features included:

- plain wireframe mode;
- hidden-line wireframe mode;
- vertex outlines;
- polygon highlight; and
- other overlays which temporarily select `glPolygonMode(..., GL_LINE)`.

The slowdown was specific to the line conversion path. Reducing the canvas to
200x150 did not improve it, and `GL_POINT` polygon mode was unaffected. That
made fill rate and the amount of rasterized coverage poor explanations.

gl4es implements desktop polygon line mode by calling `fill_lineIndices()` to
turn each triangle primitive into edge indices, then drawing those edges with
client-side `glDrawElements(GL_LINES, ...)`. The web build uses Emscripten's
`FULL_ES2` emulation because gl4es supplies desktop-style client arrays on top
of WebGL2.

For a client-indexed draw, Emscripten must do two things before WebGL can draw:

1. upload the client index array into an element buffer; and
2. scan the indices in JavaScript to discover the vertex range that must be
   copied from the client vertex arrays.

That fixed per-draw work is especially costly for gl-repl's immediate-mode
programs, which produce many small, short-lived renderlists.

### Trustworthy baseline

The first requirement was a rendering oracle. Timing a build which silently
drew no geometry had already produced convincing but false speedups.

The confirmed-correct pristine build produced these coverage counts in the
original fixed-size 3D viewport harness:

| Rendering feature | Covered pixels |
|---|---:|
| Off | 6,980 |
| Plain wireframe | 867 |
| Hidden-line | 29,338 |
| Vertex outlines | 5,217 |

The absolute values depend on the viewport and crop, but they established a
hard rule: a candidate had to retain substantial, mode-appropriate coverage.
Visual inspection alone was not accepted as proof.

Profiling the unmodified, visibly correct build showed:

- plain wireframe took approximately 35 ms per frame;
- `bufferSubData` accounted for 28.71 ms of that frame;
- there were about 128 `ELEMENT_ARRAY_BUFFER` uploads per frame;
- each index upload was only 0.2-0.4 KiB, yet cost roughly 90-380 us;
- ordinary client vertex-array uploads were much cheaper: about 1 us each in
  the fill baseline and about 11 us each in wireframe; and
- changing the output resolution did not remove the cost.

The important asymmetry was therefore not the number of bytes. It was the
Emscripten client-index path and its per-draw synchronization/scanning work.

### Misleading timing symptoms

Several observations looked contradictory until the browser queue was taken
into account:

- A Sierpinski sponge could render at roughly 5.5 FPS in plain wireframe while
  gl-repl's C-side profiler reported only about 3.25 ms of work.
- A smaller parametric torus reported roughly 13 ms with outlines and managed
  40-50 FPS, while scenes with many tiny renderlists were far worse.
- The same scene at 200x150 was nearly as slow as at the normal viewport size.
- Tiny 0.2-0.4 KiB element uploads sometimes took hundreds of microseconds,
  whereas larger vertex-array traffic appeared inexpensive.

The C profiler measures where the application submits work, not necessarily
where the browser drains queued WebGL work or performs Emscripten's internal
client-array copies. Consequently, its section totals can substantially
under-report the user-visible frame interval for this class of problem. FPS,
browser-side call measurements, and pixel coverage all had to be considered
together.

### Failed approach 1: put only generated indices in a VBO

The first attempted optimization uploaded `ind_lines` into an element VBO and
passed a null/offset index pointer to `glDrawElements`. It appeared spectacular:
plain and hidden-line timings fell to approximately 3.4 and 3.8 ms.

Those results were invalid because almost nothing rendered. Coverage fell:

| Mode | Correct coverage | Broken element-VBO coverage |
|---|---:|---:|
| Plain wireframe | 867 | 61 |
| Hidden-line | 29,338 | 1,112 |

The failure mechanism is important. The vertex attributes were still client
arrays, but the CPU-visible indices had been replaced with a GPU element-buffer
offset. Emscripten's `FULL_ES2` path could no longer inspect the indices to
determine which client vertices to upload. Moving only the indices therefore
breaks the mixed client-vertex/buffer-index case. The apparently excellent
numbers measured a mostly blank frame.

### Failed approach 2: move vertices and indices into fresh VBOs

The second version also called `list2VBO()` so both sides of the draw lived in
buffers. It rendered correctly, but performance was approximately 38.4 ms for
plain wireframe and 70.5 ms for hidden-line: no improvement.

`list2VBO()` was not silently declining the conversion. A renderlist with
vertex data takes its VBO path; it returns the non-VBO result only when there
is no vertex base to upload. The stall had simply moved:

- `list2VBO()` generated a buffer, allocated it with `glBufferData`, and filled
  it with `glBufferSubData`;
- the element fix created a second fresh buffer for the generated indices;
- immediate-mode lists are discarded after the frame; and
- roughly 128 lists therefore meant roughly 256 new buffers per frame, with
  no useful lifetime over which to amortize them.

Buffer placement alone cannot help while every small renderlist creates fresh
GPU objects every frame.

### Why renderlist merging was not the main lever

gl4es has two relevant merging gates in `listrl.c` at the pinned revision:

- the recycle/merge path rejects an active polygon mode; and
- the shared `use_glstate` merger is disabled for `GL_LINE` and `GL_POINT`.

Removing only the first gate changed total draws from about 233 to 229, which
was initially read as evidence that merging had little value. In fact, merging
had barely engaged. There is a deeper structural guard:
`ispurerender_renderlist()` returns false once `list->ind_lines` exists. Every
wireframe renderlist therefore becomes ineligible after generating its edge
indices. The Aurora stress scene also changes color and transforms between
many blocks, which limits legal merging independently of those gates.

Reducing draw count remains potentially useful for other workloads, but it
was not a reliable or sufficiently general fix for this path.

### Implemented approach

The successful patch retains gl4es's existing edge topology but removes the
client-indexed draw:

1. `fill_lineIndices()` generates the same line endpoints as before.
2. For GLES2+, every enabled client attribute is expanded in that index order:
   vertices, normals, primary and secondary colors, fog coordinates, and all
   texture-coordinate sets.
3. The expanded arrays are submitted with `glDrawArrays(GL_LINES, ...)`.
4. GLES1 retains the original indexed path because it may synthesize final
   colors and texture coordinates later in `draw_renderlist()`.
5. A missing vertex array or any allocation failure falls back to the original
   indexed path.
6. Immediate-mode lists use temporary client arrays and do not overwrite a
   display list's existing VBO placement decision.
7. Compiled display lists cache the expanded arrays and a matching VBO. Their
   expansion and GPU upload happen once, while later frames use VBO offsets
   with `glDrawArrays`.

This trades duplicated edge vertices and small CPU copies for the client-array
upload class that profiling showed to be cheap. More importantly, the generated
polygon-line path no longer makes an element upload or JavaScript index-range
scan per renderlist.

### Results

An apples-to-apples test used a fresh navigation for each build, the same
paused Aurora observatory scene at `t = 0`, and gl-repl's FPS panel:

| Mode or feature | Pristine gl4es | Patched gl4es |
|---|---:|---:|
| Plain wireframe | 28.5 FPS | 60.0 FPS |
| Hidden-line | bottlenecked | 60.0 FPS |
| Vertex outlines | bottlenecked | 60.0 FPS |
| Polygon highlight | bottlenecked | 60.2 FPS |

The patched cases reached the display's 60 FPS cap; the table does not claim
their unconstrained maximum. A separate user test of the finished patch also
reported correct rendering at 60 FPS.

The candidate was also compared with pristine screenshots in the same
1280x720 browser viewport. Thresholded coverage stayed within about 4% across
useful thresholds; hidden-line stayed within 1.5%. The immediate-mode gl-repl
logo and the mixed immediate-mode/GLUT Aurora scene both retained their
geometry. This specifically guards against the blank-frame failure of the
element-only VBO attempt.

### Upstream hardening follow-up

An end-to-end review after the first gl-repl fix identified four non-blocking
concerns worth addressing before proposing the change upstream:

1. named display lists re-expanded their line arrays on every frame;
2. immediate lists can perform as many as `5 + maxtex` allocation/free pairs
   per renderlist;
3. `build_line_arrays()` could report success without a vertex array; and
4. expanded attributes use substantially more transient memory than the old
   index-only representation for a pathological large shape.

The final patch handles the display-list and missing-vertex cases directly.
`glEndList` marks the stored renderlist nodes as cacheable. Their first
polygon-line draw builds the CPU expansion and uploads it to a dedicated VBO;
subsequent draws reuse both. Deleting or recompiling the list releases the
expanded arrays and buffer alongside `ind_lines`.

`append_calllist()` deserves special treatment because it begins with a
shallow `memcpy` of the stored renderlist. The base vertex/index arrays have
their own sharing protocol, but the derived line caches do not. The copy now
clears `ind_lines`, the expanded-array pointer, and the cacheable flag. It
therefore derives its own transient data and cannot retain or double-free the
source display list's cache. This also keeps a temporary called-list copy from
creating a fresh persistent VBO just because it inherited a non-zero list
name.

`build_line_arrays()` now rejects a missing vertex array before allocating.
Triangle renderlists with a non-zero length should already have vertices, but
the helper's contract no longer relies on that external invariant.

The immediate-mode allocation behavior is deliberately unchanged. Aurora
still reaches the 60 FPS cap despite roughly 700-1000 allocation/free pairs
per frame, so replacing them with `gl4es_scratch()` would be speculative. The
memory tradeoff is documented rather than hidden: each generated line endpoint
stores 64 bytes when all five non-texture attributes are present, plus 16 bytes
for every texture-coordinate set. A near-60,000-vertex triangle list can
therefore create several MiB-and with many texture units, tens of MiB-of CPU
data, with a compiled list retaining a similarly sized VBO. That is acceptable
for the measured workloads but is the limit to investigate if a pathological
scene appears.

Two separate browser workloads verified the two lifetime strategies. The
reproducible microbenchmarks live in
[`../bench/gl4es_polygon_line.c`](../bench/gl4es_polygon_line.c) and build with
`make bench-web-gl4es`. One binary emits 128 short-lived immediate renderlists
per frame. The other warms a source display list, compiles a second list by
calling it, deletes the source, and measures the surviving copy. Both read back
their first frame and change the page title to `FAIL` below 1,000 covered
pixels, so an empty frame cannot produce an accepted timing.

| Workload | Result |
|---|---:|
| Aurora immediate renderlists, plain wireframe | 60.0 FPS |
| 16,000 triangles / 128 immediate lists, first draw | 11.60 ms |
| Same immediate workload, subsequent draws | 5.071 ms average |
| 16,000-triangle source display-list warm-up | 9.40 ms |
| Copied display list after source deletion, first draw | 1.10 ms |
| Same copied display list, subsequent draws | 0.032 ms average |

Both microbenchmarks reported 130,500 covered pixels in the 640x480 canvas.
The display-list result exercises the exact shallow-copy and source-deletion
boundary which would otherwise risk stale pointers, stale VBO names, or a
double free. The timings are browser/machine-specific; the stable contract is
substantial coverage, a one-time compiled-list expansion/upload, and no
per-frame element-index uploads.

### Verification procedure and harness lessons

The implementation was verified with:

```sh
. "$HOME/src/emsdk/emsdk_env.sh"
emmake make -C third_party/web/gl4es/build_wasm -j4
make web
make bench-web-gl4es
make web-serve
```

The browser checks froze simulation time, reset `t` to zero, and exercised
fill, plain wireframe, hidden-line, vertex outlines, and an actually visible
polygon highlight. The dependency script was checked for idempotent patch
application, the patch was reverse-checked against the tested gl4es tree, and
`make check-c99` plus `git diff --check` passed.

Two harness details are now considered mandatory for this class of work:

1. **Assert rendering.** Record coverage from the pristine build and reject a
   candidate whose mode-specific geometry disappears. FPS without coverage is
   not a performance result.
2. **Force a genuinely new build in the browser.** Navigating to the same URL
   can preserve the existing application or apply only a hash change, leaving
   the old Wasm module and config state alive. The matched runs used distinct
   cache-busting query strings. Build identity should be verified in the same
   workflow that launches the measurement; an earlier file-restore race had
   also contaminated before/after comparisons.

Finally, do not use gl-repl's C profiler alone to judge WebGL submission
changes. A browser-side stall can appear in frame cadence or presentation
without being attributed to the C section which caused it.
