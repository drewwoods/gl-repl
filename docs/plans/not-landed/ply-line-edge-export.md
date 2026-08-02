# PLY line / edge export

> Line numbers are **approximate**; match by symbol/content.
> Builds on the completed [`ply-feedback-export.md`](ply-feedback-export.md).

## ⚠️ NOT LANDED - parked in `not-landed/`

The work on branch `feature/ply-line-edge-export` is implemented and its
tests pass, but it is **not merged to `main`** and is not considered a
supported path. Why it was parked:

- I could not see the exported line/edge geometry in any viewer I tried -
  **3D Viewer (macOS)** and **Xcode**'s preview both rendered the mesh
  without the lines.
- The exported `.ply` with `--export-ply-edges` *did* contain additional
  geometry (the `edge` element + extra vertices), so the writer is emitting
  something - but it's unclear whether these viewers simply ignore PLY
  `edge` elements, expect a different encoding, or whether the data is
  malformed in a way I haven't diagnosed.
- PLY export is **not a main support path** for this project, so I'm
  deliberately not spending more time investigating right now.

If this is revisited: validate the emitted `edge` element against a viewer
known to render PLY edges (or a parser), and confirm whether the issue is
the file or viewer-side support. Until then, treat the status below as
"implemented but unverified end-to-end in a real viewer."

## Status - implemented, UNVERIFIED in viewers (branch `feature/ply-line-edge-export`)

- **Tier A - capture line primitives:** ✅ done. `mesh_ply.c` collects line
  endpoints as deferred edges in pass 1 and resolves/welds them in pass 2
  (`resolve_edge_endpoint`, the always-weld edge table); `mesh_ply_write` gained
  the `out_edges` out-param; the edge element is omitted when empty.
  `test_skip_alignment` rewritten + `test_line_edges` added; `parse_ply` reads
  the edge element. Verified live: example 11 → 400 edges, example 13 → 102.
- **Tier B - wireframe edges:** ✅ done. `MeshPlyOptions.wireframe_edges` (CLI
  `--export-ply-edges`) pushes polygon-perimeter edges (from `poly[]`, not the
  fan-triangulation) into the same deferred-edge path. `test_wireframe_edges`
  proves the no-diagonal property + dedup. Verified live: lit cube → 12-edge
  wireframe (shared edges deduped).
- **Resolved semantic (worth recording):** Tier B is a **full mesh wireframe of
  the whole captured mesh** (user polygons + GLUT solids + GLU tess) - a
  *superset* of the on-screen vertex-outline overlay, which only outlines user
  `glBegin` primitives. Matching the overlay exactly would need executor-side
  tagging of user polygons (a passthrough marker like the normals one); that was
  judged out of scope for a pure-writer change and left as a possible follow-up.
- Full stub suite green (48 binaries / 8274 tests); `mesh_ply` 120/120.

## Original brief

Drafted after the `--export-ply-srgb` color work. Two distinct "line" sources
with different value and effort.

## Context - where lines live in the pipeline

The `.ply` export captures the live flat program through one
`glRenderMode(GL_FEEDBACK)` pass in `src/app/glr_mesh_export.c`, then
hands the raw float stream to the pure writer `src/support/mesh_ply.c`.
Two facts about lines:

1. **User line primitives already flow through, and are silently
   dropped.** `glBegin(GL_LINES / GL_LINE_STRIP / GL_LINE_LOOP)` is
   accepted by the parser (`src/repl/command_spec.c`) and executed like
   any other primitive, so feedback returns `GL_LINE_TOKEN` /
   `GL_LINE_RESET_TOKEN` records (2 vertices each). The writer's parse
   loop currently **skips** them (`mesh_ply.c`, the
   `MESH_PLY_TOK_LINE || MESH_PLY_TOK_LINE_RESET` branch → `i += 2 *
   stride`). This is real authored geometry the PLY does not represent.

2. **Vertex outlines are NOT in the export path.** They are a
   controller overlay (`src/subsystems/edit_overlays/edit_overlays.c::render_outlines_glbegin_pass`),
   drawn *after* `repl_execute_program`. The capture pass forces
   `glPolygonMode(GL_FRONT_AND_BACK, GL_FILL)` (`glr_mesh_export.c:83`)
   and runs only the flat program, so toggling the `vertex_outlines`
   config changes nothing about what the exporter sees. "Export lines
   when outlines are on" therefore cannot mean "capture the outline
   pass" without re-plumbing the overlay into the capture - instead it
   means *synthesize* edges (Tier B below).

## PLY mechanism

PLY represents lines with an edge element, written after the face
element:

```
element edge <N>
property int vertex1
property int vertex2
property uchar red          # optional, mirrors the vertex color block
property uchar green
property uchar blue
property uchar alpha
```

`vertex1` / `vertex2` index into the **same** `element vertex` block the
faces use. So every edge endpoint must be welded into the existing
`opos` / `ocol` output-vertex pool (`mesh_ply.c`), not a separate array,
or the indices are invalid.

## Tier A - capture the user's actual line primitives (recommended first)

Stop dropping `GL_LINE` tokens. Pure-writer change, no GL/capture
change, fully unit-testable with synthetic buffers (no GL context).

- **Defer-and-resolve, not weld-inline** (see wrinkle #1). In the
  `MESH_PLY_TOK_LINE` / `_RESET` branch, `invert_vertex()` the 2
  endpoints (and run the `opts->srgb_decode` color path on them, same as
  polygon corners) and append them to a **deferred edge-endpoint list**
  - a parallel `RawVert` array plus `(idx_a, idx_b)` pairs. You cannot
  weld here: this is Pass 1, and the weld `table`/`okey` are not
  allocated until Pass 2 (`mesh_ply.c`, after `do_weld` is computed).
- In Pass 2, after the vertex pool exists, resolve each deferred
  endpoint against the weld table to an output vertex index, then record
  an undirected edge `(vidx_a, vidx_b)`. Endpoints that coincide with
  face corners collapse onto them; endpoints touching no face append new
  vertices (valid - a PLY may carry vertices referenced only by edges).
- **The flat (`!do_weld`) path has no table** - `do_weld = opts->weld &&
  opts->smooth_normals`. Decide that **edge endpoints always weld** (edges
  have no flat-vs-smooth notion): build a minimal weld table for endpoint
  resolution even when faces take the 1:1 flat path. The alternative
  (edges only on the weld path) is worse - it silently drops edges for
  `OPT_FLAT` exports.
- Dedup edges as **undirected** pairs (normalize `min,max`); store in a
  small set (reuse the FNV hash-set pattern, or a sorted-then-unique
  pass since edge counts are modest).
- Emit `element edge N` + the write loop after the face loop. Return
  value stays the **triangle** count (callers/tests rely on it); expose
  the edge count via the status string in `glr_mesh_export.c` if useful
  (`"Exported %d triangles, %d edges to %s"`).
- `GL_LINE_RESET` vs `GL_LINE`: both are a 2-vertex segment for our
  purposes; the RESET flag only signals stipple-counter reset and needs
  no special handling here.

**Effort: ~2-3 hrs.** The defer-and-resolve plumbing across both shading
paths (not "stop skipping, weld inline") is the bulk of it, plus the
existing-test rewrite below. Includes new `test_mesh_ply.c` cases (a
`GL_LINES` segment between two non-face points → 1 edge, correct
endpoints/indices; a segment sharing a face vertex → welds, no duplicate
vertex; an edge case under `OPT_FLAT` to prove edges still weld).

## Tier B - synthesize wireframe edges from polygon perimeters

Mirror the vertex-outline overlay look. Also pure-writer.

- While parsing each polygon (the `poly[]` loop, **before**
  fan-triangulation), record perimeter edges `(k, k+1)` for
  `k in 0..n-1` plus the closing `(n-1, 0)` - but as **deferred
  endpoint references**, same mechanism as Tier A. `poly[]` is *reused
  per polygon* (`mesh_ply.c`), so the perimeter adjacency is destroyed
  by the next polygon and is gone entirely by Pass 2; you must capture
  the endpoints (or their not-yet-known output indices) during Pass 1,
  not read them back from `poly[]` later.
  - Critical: take edges from the **polygon loop, not the triangles.**
    Deriving from the fan-triangulated corners would add spurious
    diagonals across quads / n-gons (e.g. a quad → triangles (0,1,2),
    (0,2,3) → bogus diagonal 0-2).
- Resolve + dedup into the same Pass-2 edge set as Tier A (and the same
  "edges always weld" decision applies).
- Gate it: a `MeshPlyOptions.emit_edges` (or `edge_mode`) flag, exposed
  as `--export-ply-edges`, and/or read the live `vertex_outlines` config
  in `glr_mesh_export.c` so the file mirrors the screen. (Keep the
  *pure writer* config-free; the controller decides and sets the flag -
  same split as `srgb_decode`.)

**Effort: ~1-2 hrs** once Tier A's defer-and-resolve plumbing exists
(Tier B only adds the perimeter-capture source). A + B together ≈ **4
hrs.**

## Wrinkles / decisions to settle before implementing

1. **Two-pass structure - the load-bearing constraint.** The writer is
   two-pass: Pass 1 (`mesh_ply.c`, the token loop) parses into transient
   `corners[]` / `poly[]`; Pass 2 builds `opos`/`ocol`/`corner_vidx` and
   *only then* allocates the weld `table`/`okey`. The LINE branch and the
   polygon loop both live in Pass 1, where **no weld table exists yet** -
   so edge endpoints (Tier A) and perimeter edges (Tier B) must be
   *collected as deferred references in Pass 1 and resolved against the
   weld table in Pass 2*, not welded inline. This is the real shape of
   the change, more so than the "shared vertex pool" framing: edge
   endpoints still end up indexing the same `opos`/`ocol` block faces
   use, but the indices can't be known until Pass 2. Plus the flat path
   has no table at all (`do_weld = opts->weld && opts->smooth_normals`) -
   **decide that edges always weld** (build a minimal endpoint-weld table
   even when faces take the 1:1 flat path).
2. **Fan diagonals** (Tier B) - perimeter edges come from the `poly[]`
   loop, not the triangle corners (and `poly[]` is reused per polygon, so
   capture them during Pass 1).
3. **Edge color** - copy from the endpoint vertex color (after the
   optional sRGB decode). Decide whether the edge block even carries
   color or just indices; some downstream tools ignore edge color.
4. **Edge-only vertices carry a degenerate normal.** The vertex element
   has mandatory `nx/ny/nz`. An endpoint touching no face contributes to
   no `fnorm`, so its accumulated normal is `(0,0,0)` and stays that way
   (`mesh_ply.c`, the `len > 0` guard leaves it zero). That's the only
   option without restructuring normals - acceptable, but make it a
   conscious decision, not a surprise. (Most viewers ignore normals on
   edge-referenced verts anyway.)
4. **Viewer support is uneven.** MeshLab renders PLY edges; Blender's
   PLY importer has historically dropped the edge element entirely. So
   this is mostly useful for tools that consume edges or for
   round-tripping - set expectations in the status/docs, don't promise
   it shows up everywhere.
5. **Flag surface** - Tier A could be unconditional (it stops discarding
   real geometry) or behind a flag. Tier B should be behind a flag /
   config. Mirror the `--export-ply-srgb` precedent: bool field on
   `MeshPlyOptions`, threaded through `glr_export_mesh_ply(...)`,
   interactive callers default off.

## Touch list

- `src/support/mesh_ply.c` - LINE branch + deferred endpoints (Tier A),
  polygon perimeter capture (Tier B), Pass-2 endpoint resolution
  (always-weld), edge dedup, `element edge` header + write loop.
- `src/support/mesh_ply.h` - `MeshPlyOptions` flag(s); doc the edge
  element. **Surfacing an edge count means changing the
  `mesh_ply_write` contract** (it currently returns `ntris` and callers
  rely on that) - add an `int *out_edges` out-param or a small result
  struct rather than overloading the return.
- `src/app/glr_mesh_export.c` / `.h` - thread the flag(s); if the status
  string reports edges (`"Exported %d triangles, %d edges to %s"`),
  consume the new out-param.
- `gl_repl.c` - `--export-ply-edges` CLI flag (+ usage), like
  `--export-ply-srgb`.
- `tests/test_mesh_ply.c` - **`test_skip_alignment` must be rewritten**:
  it currently encodes the drop-the-lines contract (two LINE segments →
  asserts `nverts==3, nfaces==1`); with Tier A those 4 endpoints weld in
  (→ `nverts==7`, a new `element edge` block, 2 edges), so its
  assertions flip. Keep its skip-alignment intent via the POINT /
  PASS_THROUGH tokens (still skipped) and add explicit edge assertions.
  Also **extend the `parse_ply` test helper** - it only reads `element
  vertex` / `element face` today; it needs to parse `element edge N` +
  the edge lines before any edge assertion is possible. Plus the new
  line-token / perimeter-edge / `OPT_FLAT`-edge cases.
- `CLAUDE.md` - Run-section flag(s) + the `mesh_ply.c` file-layout row.

## Recommendation

Do **Tier A first** - small, and it stops the exporter from silently
discarding line geometry the user actually drew. Add **Tier B behind
`--export-ply-edges`** only if a wireframe matching the outline overlay
is specifically wanted.
