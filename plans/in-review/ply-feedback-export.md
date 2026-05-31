# Mesh export to PLY via a single GL_FEEDBACK capture path

> Line numbers are **approximate**; match by symbol/content.

## Context

The REPL's design goal is "Export/import is first class… take what you build and
use it in your own engine or tool" (`README.md`), and "no textures, just geometry
**and color**." We already export standalone C source (`src/repl/export.c`); the
natural next target is a **mesh** format so scenes drop into Blender / MeshLab /
an engine.

**Format: PLY (ASCII), not OBJ.** Per-vertex color is the defining data of this
tool, and PLY carries per-vertex RGBA natively; OBJ cannot (its `.mtl` companion
is per-*material*, the wrong granularity, and needs a second file). PLY is just
as easy to write and is widely imported.

**One capture path: `glRenderMode(GL_FEEDBACK)`.** Instead of a CPU walk for user
geometry plus a separate tessellation path for the GLUT solids (teapot/sphere/
cube/cone/torus, whose triangles live *inside* freeglut), we run the **existing
executor** with GL in feedback mode and parse the captured primitive stream. This
gives transformed geometry **for everything — user `glVertex`, GLU-tessellated
polygons, and the GLUT solids including the teapot — through one path**, with no
per-primitive or solid-meshing code. This is the explicit design constraint:
**one path only.**

## Goal / non-goals

- **Goal:** a "File → Export .ply" action (and F11 shortcut) that writes the
  current scene's geometry — positions, per-vertex colors, synthesized normals,
  triangular faces — to a `.ply` file, capturing all drawable geometry via one
  GL_FEEDBACK pass.
- **Non-goals:** textures/UVs (the project has none); animation (PLY is a static
  snapshot at the current `t`); exporting grid/axes/backdrop/HUD chrome (only the
  user's geometry is captured); OBJ (the format collector is format-agnostic, but
  this plan ships PLY only).

## Key design decisions (the GL_FEEDBACK specifics)

These are the load-bearing facts; they shape the whole implementation.

1. **Feedback type `GL_3D_COLOR` → position + color, but NO normals.** Feedback
   returns, per vertex, `x y z` (window coords) + 4 RGBA floats. Normals are *not*
   in the stream. **We synthesize normals geometrically** (per-face cross product,
   averaged across welded vertices for smooth shading). This is standard for mesh
   export and keeps the single-path goal; the user's `glNormal3f` data is not used
   for export. PLY stores the computed `nx ny nz`.

2. **Recovering world coordinates.** Feedback values are *window* coordinates
   (after modelview × projection × viewport/depth-range). To get world space:
   - Run the pass with **modelview = identity baseline** (do **not** load the
     camera — we want world space, not eye space). The user's
     translate/rotate/scale then build world coords, exactly as the executor does.
   - Set a **known containing orthographic projection** `glOrtho(-R,R, -R,R, -R,R)`
     with `R = 1000`, and a known `glViewport` + depth range. Then invert that
     fixed transform analytically per vertex on the CPU:
     `world = R · (2·(win−vp_origin)/vp_size − 1)` per axis (z negated, per
     glOrtho's z mapping; depth range `[0,1]` → `ndc_z = 2·win_z − 1`).
   - **Why ortho, not identity projection:** feedback **clips to the view
     frustum**. Identity projection clips to the eye-space `[-1,1]` cube — which
     would truncate normal scenes (vertices at 2, 3, …). The `±1000` ortho cube
     contains any hand-typed scene; at `R=1000`, float feedback precision is
     ~1e-4 (fine for unit geometry). **Caveat:** geometry beyond `±1000` units is
     clipped. Make `R` a named constant; an optional later refinement is a cheap
     bounding-box pre-pass to fit `R` exactly (deferred — keeps one path for now).

3. **Color = raw `glColor`, lighting OFF.** Disable `GL_LIGHTING` for the capture
   so the returned RGBA is the user's immediate color, not scene-lit shading
   (re-usable base colors). RGBA mode (the app's mode) → 4 color floats/vertex.

4. **What is captured:** only `repl_execute_program(...)` (the user's flat program,
   incl. GLUT solids + tess). Grid/axes/backdrop/HUD are *separate* scene passes we
   never call, so they're naturally excluded.

5. **Buffer sizing / overflow.** `glRenderMode(GL_RENDER)` returns the float count
   written, or **negative on overflow**. Start with a generous buffer
   (e.g. `1<<20` floats), and on overflow **grow ×2 and retry** (cap ~`64<<20` with
   an error status). The flatten budget allows large scenes, so this matters.

6. **No GL state leak / no visible disturbance.** Feedback produces no fragments,
   so the pass can run directly in the menu/key handler on the GLUT thread without
   touching the visible frame. Wrap it in `glPushAttrib(GL_ALL_ATTRIB_BITS)` +
   push/pop **both** matrix stacks + restore render mode, mirroring the
   `glPushAttrib`/`glPopAttrib` bracket already around the executor call in
   `src/app/glr_ctrl.c` (~L548).

7. **Snapshot semantics.** Export uses the live flat program
   (`repl_state_flat_program_view()`), already evaluated at the current `t`/vars —
   so it's a faithful snapshot of what's on screen this frame.

## Architecture — two modules (capture is GL-coupled; the writer is pure/testable)

- **`src/app/glr_mesh_export.{c,h}`** — GL-coupled orchestration (app layer; may
  use GL + repl). Public:
  ```c
  int glr_export_mesh_ply(const char *path);   /* returns triangle count, or <0 on error */
  ```
  Saves GL state; sets identity modelview, the `±R` ortho, viewport, depth range;
  `glDisable(GL_LIGHTING)`; `glFeedbackBuffer(n, GL_3D_COLOR, buf)`;
  `glRenderMode(GL_FEEDBACK)`; calls `repl_execute_program(&(ReplExecutionOptions){
  .flat_cmd_count=…, .program=repl_state_flat_program_view(), .text=… })`
  (same shape as the render call site); `glRenderMode(GL_RENDER)` → count (grow/
  retry on overflow); restores state; hands the raw buffer + capture params to the
  pure module; sets the status message.

- **`src/support/mesh_ply.{c,h}`** — **pure** (neutral `mesh_ply_` prefix; uses the
  GL token *macros* from `<GL/gl.h>` — which exist in the stubs — but calls **no**
  GL functions). Public:
  ```c
  typedef struct { float ortho_r; int vp_x, vp_y, vp_w, vp_h; /* depth range */ } MeshPlyCapture;
  typedef struct { int weld; float weld_eps; int smooth_normals; int triangulate; } MeshPlyOptions;
  int mesh_ply_write(FILE *out, const float *feedback, int float_count,
                     const MeshPlyCapture *cap, const MeshPlyOptions *opts); /* -> triangle count */
  ```
  Does: parse the token stream (`GL_POLYGON_TOKEN` → vertex count `n` → `n`×(3 pos
  + 4 color) floats; skip `GL_POINT_TOKEN`/`GL_LINE_TOKEN`/`GL_PASS_THROUGH_TOKEN`/
  bitmap/pixel tokens); invert ortho+viewport+depth-range → world coords; fan-
  triangulate `n>3` faces; weld vertices by (quantized position, color) → index;
  compute per-face normals (cross product, winding-consistent) and average across
  shared welded vertices; write the PLY ASCII document. Fully unit-testable with
  synthetic buffers under the GL stubs.

  **Layering:** `src/support/` is the neutral low-level tier (like `cpuprof`); the
  pure writer belongs there. The GL/repl-coupled capture is `glr_*` in `src/app/`.

PLY header emitted:
```
ply
format ascii 1.0
comment generated by gl-repl
element vertex N
property float x / y / z
property float nx / ny / nz
property uchar red / green / blue / alpha
element face M
property list uchar int vertex_indices
end_header
```
(colors: feedback float `[0,1]` × 255 → `uchar`.)

## Phases

**Phase 1 — pure `mesh_ply` writer + unit tests (no GL context needed).**
- Implement `src/support/mesh_ply.{c,h}` (parse → invert → triangulate → weld →
  normals → PLY text).
- `tests/test_mesh_ply.c`: synthetic `GL_3D_COLOR` feedback buffers — a single
  `GL_POLYGON_TOKEN` triangle; a quad (`n=4` → 2 faces); two triangles sharing an
  edge (weld → 4 verts, averaged normal); interleaved `GL_POINT_TOKEN`/
  `GL_LINE_TOKEN` (skipped); empty buffer (0/0). Assert vertex/face counts, that
  a known window-coord input under a known `MeshPlyCapture` inverts to the expected
  world coords, color 0–1→0–255, and normal direction/winding. Register in
  `Makefile`: add `test_mesh_ply` to `TEST_BINS`/`CORE_TEST_BINS` with
  `test_mesh_ply_OBJS = $(OBJDIR)/$(TEST_DIR)/test_mesh_ply.o $(OBJDIR)/src/support/mesh_ply.o`
  and `test_mesh_ply_LDLIBS = -lm` (minimal, like `test_format`). Token macros are
  in `tests/gl-stubs/include/GL/gl.h`, so it builds stub-mode.

**Phase 2 — GL_FEEDBACK capture (`glr_mesh_export`).**
- Implement `src/app/glr_mesh_export.{c,h}` per the design above (state save/
  restore, ortho/viewport/depth-range, lighting off, buffer grow/retry, run the
  executor, hand off to `mesh_ply_write`).
- Add `src/support/mesh_ply.c` + `src/app/glr_mesh_export.c` to the gl-repl object
  set (they're under `src/`, picked up by the build's `find src -name '*.c'`).

**Phase 3 — triggers + status.**
- **Menu:** add `GLR_FILE_ITEM_EXPORT_PLY` to the File-menu enum in
  `src/app/glr_actions.h` (after `GLR_FILE_ITEM_RENAME_SCENE`; bump
  `GLR_FILE_ITEM_COUNT`), its label in the File-menu label table, and a `case` in
  `glr_action_menu_item_activate()` (`src/app/glr_actions.c`, ~L720) that calls
  `glr_export_mesh_ply("output.ply")` and reports status. Mirror the
  `GLR_FILE_ITEM_SAVE_SCENE` (Ctrl+S) wiring.
- **Key:** bind **F11** (freed by the recent "Shift+F12 → previous example"
  commit; confirm with `make keymap-list`). Add `#define GLR_EXPORT_PLY GLUT_KEY_F11, 0`
  to `keymap.h` and route it in the special-key handler in `src/app/glr_ctrl_router.c`
  to the same export call. `make check-keymap-no-dup` must stay green.
- **Status:** on success `repl_set_status("Exported N triangles to output.ply")`;
  on failure `repl_set_status_error(...)` (e.g. feedback overflow at cap, fopen
  failure, empty scene → "nothing to export"). Default filename `output.ply`
  (mirrors `output.c`); optionally derive `<scene-name>.ply`.

**Phase 4 — CLI `--export-ply <file>` (optional, deferred).** All current flags
run *pre-context* (`gl_repl.c` parses before `glutInit`), but feedback needs a
live context. So a flag would set a deferred request performed in the **first
`display_func`** (after `glr_ctrl_init_gl`), then exit. Useful for scripting;
not required for the core feature. Keep out of Phases 1–3.

## Files to create / modify

| Path | Change |
|---|---|
| `src/support/mesh_ply.{c,h}` | **new** — pure: feedback-token → PLY (parse, invert, triangulate, weld, normals) |
| `src/app/glr_mesh_export.{c,h}` | **new** — GL_FEEDBACK capture + orchestration + status |
| `tests/test_mesh_ply.c` | **new** — pure unit tests with synthetic feedback buffers |
| `Makefile` | register `test_mesh_ply` (TEST_BINS + `_OBJS`/`_LDLIBS`) |
| `src/app/glr_actions.{c,h}` | `GLR_FILE_ITEM_EXPORT_PLY` enum + label + dispatch case |
| `keymap.h` | `GLR_EXPORT_PLY GLUT_KEY_F11, 0` |
| `src/app/glr_ctrl_router.c` | route F11 → `glr_export_mesh_ply` |
| `CLAUDE.md`, `MODULES.md` | document the two new modules + the export action |

## Verification

1. **Unit (`Phase 1`):** `make test_mesh_ply` (and it runs under `make test-stubs`,
   no GL) — triangle/quad/shared-edge/skip-points/empty cases pass; coordinate
   inversion and normal winding assertions hold.
2. **Build:** `make gl-repl` (and `make check-c99`, `make test-stubs`) green;
   `make check-keymap-no-dup` green after the F11 binding.
3. **Runtime (needs a display):**
   - Triangle + quad-loop scene → File → Export .ply → open in Blender/MeshLab:
     geometry matches, **per-vertex colors present**.
   - A `glutSolidTeapot(1)` scene → exported `.ply` **contains the teapot mesh**
     (the headline proof that the single feedback path captures GLUT solids). Also
     verify sphere/cone/torus/cube. *(Verify freeglut's solids route through the
     fixed-function pipeline so feedback captures them — this is the one
     assumption to confirm early in Phase 2; the teapot uses evaluators and the
     solids use client vertex arrays, both of which feedback captures.)*
   - Confirm the visible window is **unchanged** after export (no GL state leak):
     scene still renders, camera/lighting intact.
4. **Overflow path:** a large loop scene exercises buffer grow/retry; status shows
   the triangle count, file opens correctly.

## Risks / caveats

- **Normals are synthesized, not from `glNormal3f`** (feedback omits them) — flat
  per-face by default, smoothed across welded verts. Acceptable and standard;
  noted so reviewers don't expect authored normals.
- **`±R` clipping** — geometry beyond ±1000 world units is clipped. `R` is a named
  constant; bbox-fit is the deferred refinement if it ever bites.
- **GL_FEEDBACK is fixed-function/deprecated** — fine here (the whole app is
  fixed-function freeglut), but it's the reason this approach works at all; it
  would not port to a core-profile rewrite.
- **freeglut solid capture** — must confirm freeglut's solids/teapot go through the
  fixed-function transform path (verified early in Phase 2). If a future freeglut
  used shaders/VBOs they'd bypass feedback — currently they don't.
