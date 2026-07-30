# `glprobe` — GL_FEEDBACK geometry probe for standalone samples

Debugging for *"I drew something and it isn't there."*

`glprobe` runs a draw callback through `glRenderMode(GL_FEEDBACK)`, which
returns the post-transform vertex stream **without rasterizing anything**, and
reports what the pipeline actually received. No pixels, no screenshot squinting,
no bisecting the source by commenting things out.

It is not part of `gl-repl` — it is a two-file drop-in
(`glprobe.c` + the project's pure PLY writer `src/support/mesh_ply.c`) for the
loose fixed-function samples in this tree.

## The one idea

Every "why can't I see it?" is really two questions, and the tool answers them
separately:

| Capture | Transform / state | Answers |
|---|---|---|
| `glprobe_geometry()` | identity modelview + containing ortho, lighting **off** | Does the mesh exist? Where is it? Is it degenerate? |
| `glprobe_shading()` | the caller's **live** matrices, lights and materials | Is it lit? Is it on screen? Is it inside the depth range? |

Feedback colors are *post-lighting*, which is the whole trick: a mesh that
comes back with 888 healthy triangles in geometry mode and luminance `0.02` in
shading mode is a **lighting** bug, and the tool says so rather than leaving you
to infer it from a dark screenshot.

`glprobe_diagnose()` runs both over the same callback and prints a verdict.

## Using it

```c
#include "glprobe_glut.h"   /* GL + GLU + GLUT, headless-capable (see below) */
#include "glprobe.h"

/* glprobe passes a void* through; scene draw functions usually take none. */
static void probe_torch(void *u) { (void)u; drawTorch(); }

/* Call from inside display(), AFTER the camera and lights are loaded, so the
 * shading capture describes the frame on screen. */
GlProbeOptions o;
memset(&o, 0, sizeof o);
o.dump_primitives = 8;          /* optional: print the first 8 primitives */
glprobe_diagnose(probe_torch, NULL, "drawTorch", &o, "torch.ply", stderr);
```

Build it:

```bash
make glprobe SAMPLE=flame-torch.c                     # native window
make glprobe SAMPLE=flame-torch.c FREEGLUT_OSMESA=1   # headless
```

The binary lands at `build/glprobe/<basename>`. The headless form needs no
window at all, so it composes with the freeglut capture hooks:

```bash
cd /tmp && GLPROBE=1 FREEGLUT_CAPTURE_FRAMES=1 \
    ~/…/build/glprobe/flame-torch          # report on stderr + a PPM frame
```

### `glprobe_glut.h`

A sample written with the usual `#ifdef __APPLE__ / <GLUT/glut.h>` block cannot
build against the vendored OSMesa freeglut — on macOS that block always picks
the Apple framework, which has no headless backend. Replacing that block with
`#include "glprobe_glut.h"` is the only source change needed to gain
`FREEGLUT_OSMESA=1`. It is deliberately *not* `include/gl_includes.h`: that one
is the app's shim (forces freeglut on macOS, pulls in glext for the GPU
profiler, defines `M_PI`), and samples here should need nothing but a C
compiler and a GL.

## Reading the report

```
== glprobe: drawTorch ==
   mode: geometry (known ortho, lighting off)
   prims: 888 polygons (888 tris), 0 lines, 0 points; 2664 verts
   bbox:  x [-0.1600,  0.1600]  y [ 0.0000,  1.4250]  z [-0.1600,  0.1600]
   size:   0.3200 x  1.4250 x  0.3200
   color: luminance min 1.0000  mean 1.0000  max 1.0000; alpha [1.000, 1.000]
!! 24 of 888 polygons are degenerate (zero area) -- duplicated corners or a collapsed ring.
   wrote: torch.ply
```

Every line that indicates a problem is prefixed `!!`, so a long log greps
cleanly. What each signal usually means:

| Signal | Usual cause |
|---|---|
| `NOTHING CAPTURED` | early return, zero loop bound, `glBegin/glEnd` never reached, or geometry outside the capture cube |
| bbox is wrong / tiny / huge | the geometry function itself, not the camera — geometry mode has no camera |
| `degenerate` polygons | duplicated corners, or a ring that collapses to a point (a sphere's polar stack does this legitimately) |
| `NaN/Inf` vertices | divide-by-zero or an uninitialized value reaching `glVertex`/`glColor` |
| `OUTSIDE the viewport` (shading) | camera problem, not a mesh problem |
| `outside the depth range` (shading) | near/far planes are eating the geometry |
| `EVERY vertex is darker than 0.05` (shading) | lighting: check the `N·L` sign, `GL_LIGHTING`/`GL_LIGHTn`, a non-zero material diffuse, and that the light position is set *after* the camera matrix |
| alpha 0 everywhere | fully transparent under a blended draw |

Geometry-mode coordinates are in the callback's **own emitted space** (model
space plus whatever transforms it applies itself). Shading-mode coordinates are
unprojected back through the live matrices, so they read in whatever space was
current at probe entry — world space when probed after the camera is loaded.

## PLY dumps

Passing a path to `glprobe_geometry()` / `glprobe_diagnose()` also writes the
capture as an ASCII PLY (welded, smooth-normalled, with triangle proxies for
points and lines so mesh-only viewers show them). Open it in Blender, Quick Look
or MeshLab to inspect a mesh interactively.

PLY output is **geometry-mode only** — it depends on the known ortho inverse
that shading mode deliberately does not install.

## Implementation notes

- Both captures bracket the callback in `glPushAttrib(GL_ALL_ATTRIB_BITS)` plus
  a projection/modelview push/pop, and feedback generates no fragments, so
  probing from inside `display()` leaves the visible frame untouched.
- Geometry mode captures **twice**: once inside a 1000-unit cube to find the
  extent, then again inside a cube sized to the mesh. Window coordinates are
  floats, so capturing a 0.16-unit torch inside a 1000-unit cube quantizes away
  most of the precision a report printed to four decimals implies.
- The feedback buffer starts at 1M floats and doubles on overflow up to 64M. A
  capture that still overflows is reported with `overflow = 1` and truncated
  counts rather than failing outright — a lower bound still answers "did
  anything come out?".
- `GL_3D_COLOR` (7 floats/vertex) is used, not the app's
  `GL_3D_COLOR_TEXTURE`: `glr_mesh_export.c` encodes authored normals into the
  texcoord channel because the REPL knows them, whereas an arbitrary sample does
  not cooperate, so `mesh_ply.c` synthesizes face normals instead.

## Related

- `src/app/glr_mesh_export.c` — the app's own feedback→PLY path (REPL-coupled).
- `src/support/mesh_ply.h` — the pure writer both share.
