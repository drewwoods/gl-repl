## Single-Source Export for Outlines and Vertex Points

### Summary
Refactor the exporter so generated `output.c` defines the REPL geometry only once per top-level snippet and once per user-defined function, while still supporting fill, outline, and vertex-point rendering.

The chosen approach is **macro remap**: generate one shared geometry/function body written in near-original GL syntax, but temporarily `#define` exported GL/GLU calls to pass-aware helper emitters. `display()` will keep separate fill/outline/point setup blocks, but each block will call the **same** generated geometry entrypoint instead of three duplicated geometry variants.

### Implementation Changes
- Replace the current duplicated export modes (`canonical`, `outline`, `vpoints`) with a **single exported geometry body** plus a small pass-aware emitter layer in generated C.
- Emit a generated internal pass enum and context, e.g. fill / outline / vertex-points, with helpers that implement pass-specific behavior.
- Scope the macro remap tightly:
  - `#define` the GL/GLU calls immediately before generated geometry/user-function definitions.
  - `#undef` them immediately after.
  - Do not let the remap affect `display()`, `init()`, tess helpers, or other support code.
- Keep generated user function signatures unchanged. Do **not** add a pass parameter to `func0(...)` etc.; the active pass comes from the generated internal emitter context.
- The shared remap layer must cover every GL/GLU command the exporter currently emits inside snippet/function bodies, not just vertex calls, so overlay passes do not accidentally re-run fill geometry. That includes:
  - immediate-mode calls: `glBegin`, `glEnd`, `glVertex2f`, `glVertex3f`, `glNormal3f`, `glColor3f`, `glColor4f`
  - state/transform calls that can appear in snippet bodies: `glEnable`, `glDisable`, `glShadeModel`, `glBlendFunc`, `glPointSize`, `glPointParameterfv`, `glTranslatef`, `glScalef`, `glRotatef`, `glPushMatrix`, `glPopMatrix`, `glColorMaterial`, `glLightModeli`, `glFrontFace`, `glMaterialf` / `glMaterialfv`
  - geometry helpers: `gluSphere`, `gluCylinder`, `gluDisk`, `gluPartialDisk`, `glutSolidTorus`
  - tess commands: `gluBegin`, `gluEnd`, `gluNormal`, `gluColor`, `gluVertex`
- Pass behavior:
  - **Fill pass**: preserve current exported behavior.
  - **Outline pass**:
    - preserve current polygon-only overlay policy for explicit `glBegin(...)` geometry: skip source `GL_POINTS`, `GL_LINES`, `GL_LINE_STRIP`, `GL_LINE_LOOP`
    - preserve current tess contour outline behavior: emit one `GL_LINE_LOOP` per contour
    - keep source color/normal/material/state commands as **no-op** in overlay passes so the existing black overlay styling from `display()` remains in control
    - keep quadric / torus overlay behavior unchanged for now: **fill-only**, no generated outline overlay for them
  - **Vertex-point pass**:
    - do not wrap every vertex in its own `glBegin(GL_POINTS)` / `glEnd()`
    - instead, lazily open one points primitive when the first explicit vertex or tess vertex is emitted, and flush it before any wrapped command that cannot legally occur inside `glBegin(GL_POINTS)` / `glEnd()` (notably transforms, matrix stack ops, begin/end boundaries, tess contour boundaries, and any fill-only geometry helper)
    - keep quadric / torus behavior unchanged for now: **fill-only**, no generated point overlay for them
- `display()` in generated C should still have separate state setup blocks for fill, outline, and points, but should call the same top-level geometry function for each enabled pass.

### Generated Output Shape
- Remove duplicated generated helpers like:
  - `render_repl_outline_overlay`
  - `render_repl_vertex_points_overlay`
  - `render_repl_outline_funcN`
  - `render_repl_vpoints_funcN`
- Replace them with:
  - one shared top-level geometry helper, e.g. `render_repl_geometry(repl_pass)`
  - one shared definition for each exported user function
  - one generated internal emitter context + helper layer
- The geometry text should appear once in `output.c`; pass-specific behavior should live in the helper/macros, not in duplicated geometry bodies.

### Test Plan
- Update export IO tests to assert the new shape when outlines / vertex points are enabled:
  - exported C no longer contains per-pass duplicated geometry helpers or per-pass duplicated `funcN` variants
  - representative vertex lines from a function body appear once, not three times
  - generated C still compiles with outlines and vertex points enabled
- Add targeted coverage for a tess example and a function example so both shared top-level geometry and shared user functions are exercised under the new exporter.
- Keep the existing example export compile sweep and ensure all exported examples still compile.
- Keep the existing round-trip example suite; it does not need to become exact for this refactor, but it must still pass.

### Assumptions and Defaults
- The goal is **single-source geometry in exported C**, not single-pass runtime execution. `display()` may still invoke the same shared geometry body multiple times with different internal passes.
- Overlay visual behavior should stay as close as possible to current output:
  - black outlines and black points remain the default
  - source color / normal / material commands are ignored in overlay passes by default
- Quadric / torus overlays remain out of scope for this refactor; they should stay fill-only unless separately requested later.
- Macro remap is the chosen export style and should be kept tightly scoped so the generated file remains readable and safe to edit.

