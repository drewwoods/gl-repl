# Smooth Autonormals Plan

## Status - NOT STARTED (2026-05-23 audit)

`src/repl/autonormal.c` still runs pre-flatten on source commands and
skips block heads (the existing behavior the conversation notes
diagnose). No `smooth_*` helpers, no capture/render two-pass exporter,
no crease-angle constants in the tree. Stays in `not-started/`.

## Summary
Implement smooth autonormals as a post-flatten, CPU-side geometry pass. Do not use OpenGL feedback and do not export fully flattened loops. Preserve compact user loops in export by generating a two-pass capture/render helper that computes normals at runtime from the same vertex expressions.

## Key Changes
- Add a smooth-normal sidecar owned by the REPL flat pipeline: scan flat `glBegin`/`glEnd` vertex streams, derive triangle faces, match duplicate positions within epsilon, and store one generated normal per flat vertex occurrence.
- Keep explicit `glNormal3f` authoritative unless smooth autonormals are explicitly enabled for that primitive.
- Support `GL_TRIANGLES`, `GL_TRIANGLE_STRIP`, `GL_TRIANGLE_FAN`, `GL_QUADS`, `GL_QUAD_STRIP`, and `GL_POLYGON`; unsupported modes get no generated normals.
- Use defaults: epsilon `1e-4f`, crease angle `60 degrees`, area-weighted averaging, degenerate faces ignored.
- Apply generated normals in `repl_executor.c` immediately before each matching `CMD_VERTEX3F`, keyed by flat vertex occurrence, without inserting visible source commands.

## Export
- Preserve source loops using the existing export path; do not emit fully flattened vertex arrays.
- When smooth autonormals are active, export helper functions: `smooth_reset`, `smooth_begin`, `smooth_vertex3f`, `smooth_end`, `smooth_finalize`, and `smooth_next_normal`.
- Generate a two-pass render function:
  - `PASS_CAPTURE`: run the same loops, record primitive modes and vertex positions, skip real GL geometry.
  - `PASS_RENDER`: run the same loops again, call `glNormal3fv(smooth_next_normal())` before each `glVertex3f`.
- Keep assignments and loop math running in both passes so vertex expressions match live render behavior.

## Tests
- Add focused smooth-normal tests for triangle strips, quad strips, duplicate torus-style seam vertices, crease cutoff behavior, degenerate triangles, and explicit user normals.
- Add export tests asserting loops are preserved and generated C contains the smooth helper/capture pass rather than fully flattened vertices.
- Run `make test-repl-autonormal`, `make test-repl-core-io`, `make test`, and `make gl-repl USE_GL_STUBS=1`.

## Assumptions
- Smooth autonormals are a rendering/export helper, not source rewriting.
- OpenGL feedback is not used because it is transformed-space, hard to map back to vertex calls, and weak for deterministic stub-mode tests.
- Analytic normal inference from arbitrary loop code is out of scope; a future `surface(...) { pos(...); }` primitive can provide a cleaner analytic or finite-difference path.

## Addendum: Conversation Notes
- User asked whether `ARCHITECTURE.md` suggests autonormals could be generated for vertices defined in a `for` loop, specifically `g_example_torus` and `g_example_waves`.
- Finding: current frame order rebuilds autonormals before flattening, then rebuilds the flat program. The architecture says flattened commands are where loops are expanded, functions are inlined, conditionals are resolved, and provenance is retained.
- Finding: current `recompute_autonormals()` operates on source commands and explicitly skips `CMD_FOR_BEGIN`, `CMD_FUNC_DEF`, and `CMD_IF_BEGIN` blocks. It only sees source-level `glBegin`/`glVertex3f` runs, so it cannot generate normals for loop-expanded vertices.
- Finding: tests currently lock in this skip behavior. `make test-repl-autonormal` and `./test_repl_autonormal` passed with `39/39`.
- Conclusion: loop-aware autonormals should run after flattening, because the evaluated loop vertices exist only in the flat stream.
- Caveat: `g_example_waves` already has explicit analytic normals, which are better than face-derived normals. `g_example_torus` is the stronger target for generated smooth normals.
- User asked about feasibility of normal smoothing by identifying adjacent polygons through duplicate `glVertex3f` matches with thresholds, and noted the export challenge: normals need to be computed inside or alongside the original loops, not from a permanently flattened copy.
- Evaluation: geometry-based smoothing after flattening is feasible. It can compute face normals, match duplicate positions with epsilon, average adjacent normals, and use a crease angle to avoid smoothing hard edges.
- Evaluation: inserting analytic normals back into source before flattening is high risk. Arbitrary REPL code can hide vertex formulas behind assignments, functions, conditionals, `rand`, time `t`, and reused variables, making symbolic normal inference brittle.
- Proposed natural future expression: a higher-level `surface(...) { pos(...); color(...); }` primitive, inspired by `GL_AUTO_NORMAL`/surface evaluation, where the REPL owns the parametric intent and can generate finite-difference or analytic normals predictably.
- User asked whether exported code would include fully flattened loops.
- Decision: no. Export should preserve compact loops. Fully flattened arrays are only suitable for snapshot export and would freeze dynamic behavior.
- Export approach: generate a two-pass helper. Capture pass runs the same source loops and records positions/primitive modes. Finalize computes normals. Render pass runs the same loops again and emits `glNormal3fv(smooth_next_normal())` before each `glVertex3f`.
- User asked whether OpenGL feedback could be used.
- Evaluation: possible as an experiment, but not the primary path. Feedback returns transformed vertices, not original object-space inputs or normals; mapping feedback data back to source vertex calls is brittle; `glPassThrough()` is awkward inside `glBegin`/`glEnd`; export would need a live GL context to compute normals; stubs do not currently expose feedback APIs.
- Decision: use “software feedback” instead: generated capture/render helpers in C, deterministic and testable without real GL feedback.
- User asked what `smooth_finalize()` would look like.
- Proposed implementation: build faces from captured primitive blocks, compute face normals and areas, then for each vertex occurrence find same-position vertices within epsilon and average incident face normals whose dot product passes the crease-angle threshold. Store one normal per vertex occurrence and reset the render cursor.

**See codex://threads/019ddabc-267c-7af0-88a1-ea20099a88ec**

### > what is the feasibility of doing normal smoothing, one way would be to identify adjacent polygons by matching the duplicate glVertex3f calls for adjacent polygons, with some threshold of vertex matches -- this would be a real challange for export where the normal would be needed to be computed in the for loop itself.  So the autonormal needs to be determined after flattening, but then inserted into the code prior to flattening if an analytic normal could be found.  If this seems very error prone or high failure rate, perhap propose a way for auto normals for surfaces to be more naturally expressed?  openGL itself has autonormals for bezier surfaces and glu for knots, perhaps we could base something off that but with less boiler plate and more visual guides?

### Response
Feasible, but I would split it into two tiers.

**1. Flat-program smooth autonormals: feasible**
The reliable version is geometry-based and happens after flattening:

- Flatten loops/functions first.
- Scan the flat `glBegin`/`glEnd` stream.
- Convert each primitive into faces.
- Compute face normals.
- Match duplicate vertex positions with an epsilon.
- Average adjacent face normals per vertex, with a crease-angle cutoff so cubes stay sharp.
- Apply normals as a sidecar during execution, instead of inserting visible `glNormal3f` lines.

This should work well for `g_example_torus`, because the nested loop creates duplicated seam/adjacent vertices after flattening. It can also work for wave grids as an approximation, though the current analytic normals in `g_example_waves` are better.

I would avoid inserting generated normals into the flat command array if possible. A sidecar like “normal for flat vertex index N” keeps replay/source mapping stable and avoids auto-normal lines changing command counts.

**2. Analytic source insertion: high risk**
Inferring compact analytic normals and inserting them back into the pre-flattened source is where this gets brittle.

For torus-like code, it might be possible sometimes. But arbitrary REPL code has assignments, reused variables, conditionals, `rand`, nested loops, functions, and time `t`. Once the vertex is `glVertex3f(x, y, z);`, the original parametric formula may already be hidden behind prior assignments. Deriving a symbolic normal from that reliably would be a high-failure-rate feature.

For export, I would not try to synthesize analytic `glNormal3f(...)` expressions from arbitrary loops.

**Better model**
I’d add a more natural REPL-level surface primitive, inspired by `GL_AUTO_NORMAL`, rather than trying to reverse-engineer loops:

```c
surface(i, 0, 24, j, 0, 24, GL_QUAD_STRIP) {
    pos((2+cos(j*TAU/24))*cos(i*TAU/24),
        (2+cos(j*TAU/24))*sin(i*TAU/24),
        sin(j*TAU/24));
    color(...);
}
```

Then the REPL owns the parametric intent:

- Live render can compute normals by finite differences or exact derivatives later.
- Export can emit compact helper code that evaluates the same surface and normals.
- Visual guides can show sampled normals, seam matches, and crease edges.
- Existing immediate-mode loops can still get flat-program smoothing as a best-effort preview.

So my recommendation is: implement geometry-based smooth autonormals after flattening first, as a render/export-helper feature, not source rewriting. Then add a surface-specific syntax for cases where compact, stable, high-quality normals and export matter.

### > Question: "implement geometry-based smooth autonormals after flattening first". but how would the exported code look like? would you include the full flattened loops?

### Response
No, I would not include the full flattened loops by default. That would make exports huge and would freeze anything time-dependent unless we re-flatten every frame into generated C, which is the wrong direction.

The better exported shape is a compact two-pass helper around the existing source loops. Current export already preserves REPL `for` blocks as C `for` loops in [repl_export.c](/Users/drew/src/code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_export.c:1379), so smooth autonormal export should preserve that.

Conceptually the generated C would look like:

```c
static void render_scene(int pass) {
  int _vid = 0;

  for (float i = 0; i < 24; i += 1.0f) {
    if (pass == PASS_RENDER) glBegin(GL_QUAD_STRIP);
    else smooth_begin(GL_QUAD_STRIP);

    for (float j = 0; j < 25; j += 1.0f) {
      float x0 = ...;
      float y0 = ...;
      float z0 = ...;

      if (pass == PASS_CAPTURE) smooth_vertex(_vid++, x0, y0, z0);
      else { glNormal3fv(smooth_normal(_vid++)); glVertex3f(x0, y0, z0); }

      float x1 = ...;
      float y1 = ...;
      float z1 = ...;

      if (pass == PASS_CAPTURE) smooth_vertex(_vid++, x1, y1, z1);
      else { glNormal3fv(smooth_normal(_vid++)); glVertex3f(x1, y1, z1); }
    }

    if (pass == PASS_RENDER) glEnd();
    else smooth_end();
  }
}

static void display(void) {
  reset_repl_vars();

  smooth_reset();
  render_scene(PASS_CAPTURE);
  smooth_finalize(/* epsilon */, /* crease angle */);

  render_scene(PASS_RENDER);
}
```

So export keeps the loops, but emits the vertex expressions through a capture/render wrapper. The capture pass builds evaluated vertices and primitive faces for the current frame, `smooth_finalize()` matches duplicate positions with an epsilon and averages compatible face normals, then the render pass emits `glNormal3fv(...)` before each `glVertex3f(...)`.

The alternative is baked export:

```c
static const float vertices[] = { ... fully flattened ... };
static const float normals[] = { ... fully flattened ... };
```

That is acceptable only as a snapshot export. It is bad for this REPL’s normal export because `t`, sliders, assignments, and procedural formulas would stop behaving like the live scene.

For longer-term ergonomics, I still think a surface primitive is cleaner than reverse-engineering arbitrary loops. Flat smooth autonormals can be a good compatibility layer; a future `surface(...) { pos(...); color(...); }` block would give compact export, better visual guides, and predictable finite-difference or analytic normals without trying to infer user intent from arbitrary immediate-mode code.

### > Question: "would you use OpenGL feedback for this or some other method?"

### > Response
I would not use OpenGL feedback for this, for several reasons:
- Feedback returns transformed vertices, not original object-space inputs or normals, so mapping feedback data back to source vertex calls is brittle.
- `glPassThrough()` is awkward to use inside `glBegin`/`glEnd` blocks, and may not be supported in all contexts.
- Export would require a live GL context to compute normals, which complicates testing and stubs.
- Stubs do not currently expose feedback APIs, so it would be hard to test this path without real GL.

###  > Question: "what would smooth_finalize() look like?"

`smooth_finalize()` would be pure CPU geometry processing. It would not call OpenGL. It would take the captured vertex stream and produce one normal per captured vertex occurrence.

A compact C sketch:

```c
static void smooth_finalize(float eps, float crease_degrees) {
  float cos_crease = cosf(crease_degrees * (float)M_PI / 180.0f);

  smooth_build_faces_from_blocks();

  for (int f = 0; f < g_face_count; f++) {
    SmoothFace *face = &g_faces[f];
    SmoothVertex *a = &g_vertices[face->v[0]];
    SmoothVertex *b = &g_vertices[face->v[1]];
    SmoothVertex *c = &g_vertices[face->v[2]];

    vec3 e1 = sub3(b->p, a->p);
    vec3 e2 = sub3(c->p, a->p);
    vec3 n = cross3(e1, e2);

    face->area = len3(n);
    face->valid = face->area > 1e-8f;
    face->n = face->valid ? scale3(n, 1.0f / face->area) : v3(0, 0, 1);
  }

  for (int v = 0; v < g_vertex_count; v++) {
    vec3 ref = smooth_incident_normal_average(v);

    vec3 sum = v3(0, 0, 0);

    for (int u = 0; u < g_vertex_count; u++) {
      if (!same_position(g_vertices[v].p, g_vertices[u].p, eps))
        continue;

      for (int fi = 0; fi < g_vertices[u].incident_count; fi++) {
        SmoothFace *face = &g_faces[g_vertices[u].incident_faces[fi]];
        if (!face->valid)
          continue;

        if (dot3(ref, face->n) >= cos_crease)
          sum = add3(sum, scale3(face->n, face->area));
      }
    }

    g_vertices[v].normal = safe_normalize3(sum, ref);
  }

  g_render_cursor = 0;
}
```

The key helpers:

```c
static int same_position(vec3 a, vec3 b, float eps) {
  return fabsf(a.x - b.x) <= eps &&
         fabsf(a.y - b.y) <= eps &&
         fabsf(a.z - b.z) <= eps;
}
```

`smooth_build_faces_from_blocks()` expands captured primitive ranges into triangle faces:

- `GL_TRIANGLES`: every 3 vertices.
- `GL_TRIANGLE_STRIP`: sliding triples, flip winding on odd triangles.
- `GL_TRIANGLE_FAN`: first vertex plus each adjacent pair.
- `GL_QUADS`: split each quad into two triangles.
- `GL_QUAD_STRIP`: each pair step becomes two triangles.
- `GL_POLYGON`: fan triangulation.

The important detail is that normals are stored per vertex occurrence, not just per unique position. Matching duplicate positions lets adjacent loop-generated polygons smooth together, while the crease angle prevents unrelated hard edges from blending.

In exported code, render would then do:

```c
smooth_reset();
render_scene(PASS_CAPTURE);
smooth_finalize(1e-4f, 60.0f);

render_scene(PASS_RENDER);
```

And each vertex call becomes:

```c
if (pass == PASS_CAPTURE) {
  smooth_vertex3f(x, y, z);
} else {
  glNormal3fv(smooth_next_normal());
  glVertex3f(x, y, z);
}
```

For a first implementation, I’d use the simple `O(vertex_count^2)` position matching above. It is much easier to verify. A spatial hash can replace `same_position` scanning later if profiling shows it matters.
