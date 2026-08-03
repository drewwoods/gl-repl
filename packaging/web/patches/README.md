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
listed in `GL4ES_PATCHES` in `scripts/web-deps.sh`.

## Patch inventory

| Patch | Purpose |
|---|---|
| `gl4es-rasterpos-perspective-divide.patch` | Perspective-divide clip coordinates before deriving the raster position. |
| `gl4es-bitmap-dirty-clear.patch` | Clear only the dirty CPU bitmap rectangle between glyph batches. |
| `gl4es-getter-client-state.patch` | Answer tracked `glGet*` state locally instead of synchronously draining WebGL. |
| `gl4es-color-material-face.patch` | Track front/back `GL_COLOR_MATERIAL` independently in the fixed-pipeline emulator. |
| `gl4es-pushattrib-gaps.patch` | Fill polygon, line, point, and transform state gaps in `glPushAttrib`/`glPopAttrib`. |
| `gl4es-pushattrib-texenv.patch` | Preserve texture environment mode and color per texture unit. |
| `gl4es-accum-fbo.patch` | Implement the accumulation buffer with an internal FBO. |
| `gl4es-point-smooth.patch` | Emulate round antialiased points in the GLES2 fixed-pipeline shader. |
| `gl4es-polygon-line-drawarrays.patch` | Avoid Emscripten's client-index upload and scan for polygon-mode lines. |
| `gl4es-point-size-batch.patch` | Apply `glPointSize` to the batch it was called on, not to whatever is still pending. |
| `gl4es-line-width-batch.patch` | Same for `glLineWidth`, gated on a driver that can actually widen a line; skip repeated widths. |

See each older patch's leading prose for the detail that is available.

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

## 2026-08-03: the `glLineWidth` half of the same gap

Patch: [`gl4es-line-width-batch.patch`](gl4es-line-width-batch.patch)

(The entry above originally claimed `glLineWidth` was not wrapped by gl4es at
all. It is - `wrap/gles.c` generates `gl4es_glLineWidth`, and
`PUSH_IF_COMPILING` there already covers display lists. The real gap was
narrower: no flush of the pending immediate-mode batch before the state
change.)

So the same deferred-state shape as point size, with one difference that
decides the design: **it is almost never observable.** GLES and WebGL stacks
commonly report `GL_ALIASED_LINE_WIDTH_RANGE` as `[1, 1]` and rasterize every
line one pixel wide regardless. Measured here under ANGLE/SwiftShader through
WebGL2:

| Check | Result |
|---|---|
| `getParameter(ALIASED_LINE_WIDTH_RANGE)` | `[1, 1]` |
| Scene drawing one line at width 9 and one at width 1 | two identical one-pixel lines |

An unconditional `FLUSH_BEGINEND` would therefore break batching to fix pixels
this driver cannot draw differently - the opposite trade from the polygon-line
patch, which exists because per-draw cost dominates on this target. The patch
gates on capability instead: `GetHardwareExtensions` records
`hardext.maxlinewidth` from `GL_ALIASED_LINE_WIDTH_RANGE` (one query, beside
the other max queries), and `gl4es_glLineWidth` flushes only when that is
greater than 1. Correct where wide lines exist, free where they do not.

The part that does something everywhere is the early-out for a repeated width:
`gl4es_mirror_line_width` (from `gl4es-getter-client-state.patch`) is
authoritative - `glPopAttrib` and display-list replay both come back through
`gl4es_glLineWidth`, and nothing calls `gles_glLineWidth` directly - so a
no-op set can return instead of crossing into JS. gl-repl's overlay passes
restore `glLineWidth(1.0f)` repeatedly per frame, so those calls are now free.

Verified: the web build renders **pixel-identically** to the build before the
patch on a scene carrying line loops, vertex outlines and point markers (0
differing pixels), which is the expected result on a `[1, 1]` stack. The full
patch stack applies in order to a pristine pinned checkout and reproduces the
built tree exactly.

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
