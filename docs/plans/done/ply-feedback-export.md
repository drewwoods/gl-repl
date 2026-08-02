# Mesh export to PLY via a single GL_FEEDBACK capture path

> Line numbers are **approximate**; match by symbol/content.

## Status (branch `feature/ply-mesh-export`) - COMPLETE

- **Phase 1 - pure `mesh_ply` writer + tests:** ✅ done (`src/support/mesh_ply.{c,h}`,
  `tests/test_mesh_ply.c`, 50 assertions; runs under `make test` and stubs).
- **Phase 2 - `glr_mesh_export` GL_FEEDBACK capture:** ✅ done
  (`src/app/glr_mesh_export.{c,h}`; `glDepthRange` stub; `STATIC_ASSERT` token
  drift guards).
- **Phase 3 - triggers + status:** ✅ done (File → Export .ply, F11 binding +
  router, status messages; CLAUDE.md / MODULES.md updated).
- **Phase 4 - CLI `--export-ply <file>`:** ✅ done (`gl_repl.c`: a deferred request
  fires from the first `display_func` after a full frame, then `exit()`s - needs a
  display, post-context unlike `--dump-*`).
- **Runtime verification (Verification §3): ✅ passed.** `./gl-repl <triangle+teapot
  scene> --export-ply out.ply` against real freeglut captured **3137 triangles
  through the single feedback path - the GLUT teapot included** (the headline
  proof). Per-vertex colors correct (red/green/blue triangle; `0.2/0.7/1.0`→`51 179
  255` teapot), world-coord inversion to ~1e-4 (the predicted `R·2⁻²³` precision),
  +Z winding normal on the triangle and smooth-shaded teapot normals, and a
  structurally valid PLY (1600 verts / 3137 faces, all indices in range). Full
  `make test-stubs` green (48 binaries / 8190 tests); `check-c99` /
  `check-keymap-no-dup` / `check-gl-boundaries` / `check-state-ownership` green.
- **Deferred (optional, future):** bbox-fit of the ortho `R` (decision 2). The
  interactive "visible frame unchanged after export" no-leak property is correct by
  construction (`glPushAttrib(GL_ALL_ATTRIB_BITS)` + both matrix stacks); worth a
  one-time manual eyeball but not blocking.

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
gives transformed geometry **for everything - user `glVertex`, GLU-tessellated
polygons, and the GLUT solids including the teapot - through one path**, with no
per-primitive or solid-meshing code. This is the explicit design constraint:
**one path only.**

## Goal / non-goals

- **Goal:** a "File → Export .ply" action (and F11 shortcut) that writes the
  current scene's geometry - positions, per-vertex colors, synthesized normals,
  triangular faces - to a `.ply` file, capturing all drawable geometry via one
  GL_FEEDBACK pass.
- **Non-goals:** textures/UVs (the project has none); animation (PLY is a static
  snapshot at the current `t`); exporting grid/axes/backdrop/HUD chrome (only the
  user's geometry is captured); **non-polygon primitives - `GL_POINTS` and
  `GL_LINES`/strips/loops feed back as point/line tokens and are dropped, so a
  wireframe- or point-only scene exports an empty mesh** (PLY here = faces); OBJ
  (the format collector is format-agnostic, but this plan ships PLY only).

## Key design decisions (the GL_FEEDBACK specifics)

These are the load-bearing facts; they shape the whole implementation.

1. **Feedback type `GL_3D_COLOR` → position + color, but NO normals.** Feedback
   returns, per vertex, `x y z` (window coords) + 4 RGBA floats =
   **7 floats/vertex** (name it `FB_FLOATS_PER_VERTEX = 7`; nearly every formula
   and the skip table below reference it). Normals are *not* in the stream. **We
   synthesize normals geometrically** (per-face cross product, averaged across
   welded vertices for smooth shading). This is standard for mesh export and keeps
   the single-path goal; the user's `glNormal3f` data is not used for export. PLY
   stores the computed `nx ny nz`.

2. **Recovering world coordinates.** Feedback values are *window* coordinates
   (after modelview × projection × viewport/depth-range). To get world space:
   - Run the pass with **modelview = identity baseline** (do **not** load the
     camera - we want world space, not eye space). The user's
     translate/rotate/scale then build world coords, exactly as the executor does.
   - Set a **known containing orthographic projection** `glOrtho(-R,R, -R,R, -R,R)`
     with `R = 1000`, and a known `glViewport` + depth range. Then invert that
     fixed transform analytically per vertex on the CPU:
     `world = R · (2·(win-vp_origin)/vp_size - 1)` per axis (z negated, per
     glOrtho's z mapping; depth range `[0,1]` → `ndc_z = 2·win_z - 1`).
   - **Why ortho, not identity projection:** feedback **clips to the view
     frustum**. Identity projection clips to the eye-space `[-1,1]` cube - which
     would truncate normal scenes (vertices at 2, 3, …). The `±1000` ortho cube
     contains any hand-typed scene; at `R=1000`, float feedback precision is
     ~1e-4 (fine for unit geometry). **Caveat:** geometry beyond `±1000` units is
     clipped. Make `R` a named constant; an optional later refinement is a cheap
     bounding-box pre-pass to fit `R` exactly (deferred - keeps one path for now).

3. **Capture render state: raw `glColor`, lighting OFF, fill mode, cull OFF.**
   Feedback honors lighting, polygon mode, *and* face culling (all pre-rasterization
   stages), so the capture forces a known minimal state before running the executor
   (under the decision-6 `glPushAttrib` bracket, restored after):
   - `glDisable(GL_LIGHTING)` → returned RGBA is the user's immediate `glColor`, not
     scene-lit shading (re-usable base colors). RGBA mode (the app's mode) → 4 color
     floats/vertex.
   - `glPolygonMode(GL_FRONT_AND_BACK, GL_FILL)` → guarantees polygons feed back as
     `GL_POLYGON_TOKEN`. Otherwise ambient wireframe state (`GL_LINE`) would make
     them feed back as *line* tokens and get dropped. `src/scene/render.c` resets to
     `GL_FILL` after each frame so it is safe today, but forcing it makes the pass
     self-contained.
   - `glDisable(GL_CULL_FACE)` for the baseline. **Caveat:** `glEnable(GL_CULL_FACE)`
     is a supported REPL command; because we run the executor unchanged (decision 4),
     a user program that enables culling re-enables it mid-capture and feedback omits
     the culled faces. Inherent to the single-path design - documented as a known
     limitation, not worked around (see Risks).

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

7. **Snapshot semantics - fresh flat program, respect replay clamp.** Export uses
   the live flat program (`repl_state_flat_program_view()`), already evaluated at the
   current `t`/vars. Two caveats:
   - **Freshness:** the per-frame flatten-if-dirty runs *inside*
     `glr_ctrl_display_frame` (the `repl_state_flat_program_dirty()` →
     `repl_flatten_commands()` guard), which the export path (a key/menu handler)
     bypasses. In practice a frame always renders before the user triggers export so
     the flat program is current - but the capture should defensively rebuild it if
     dirty, and the Phase 4 CLI path must run *after* that block in `display_func`.
   - **Replay clamp:** to match "what's on screen", when replay is **active** the
     visible render clamps the flat count to `replay_exec_limit()`
     (`src/subsystems/replay/replay.h:77`); the capture must use the **same clamped
     count** for `flat_cmd_count`, not the full `program.cmd_count`. So
     `flat_cmd_count = replay_active() ? replay_exec_limit() : program.cmd_count`.

8. **Known, asserted depth range.** The window→world inversion assumes a fixed
   depth range. The capture sets `glDepthRange(0, 1)` (saved/restored with the rest
   of GL state) and passes `depth_near=0, depth_far=1` to the writer, so the
   inversion is exact and self-consistent regardless of the app's live depth range.

## Feedback token stream - parser skip table (load-bearing)

The buffer is a flat float array of token records. The parser must consume each
record's exact length or every subsequent polygon is silently misaligned (the
classic feedback-parsing bug). For `GL_3D_COLOR` (`FB_FLOATS_PER_VERTEX = 7`):

| Token | Floats *after* the token marker |
|---|---|
| `GL_POLYGON_TOKEN` | `1` (vertex count `n`), then `n × 7` - **the only token we emit faces from** |
| `GL_POINT_TOKEN` | `7` (1 vertex) - skip |
| `GL_LINE_TOKEN` | `14` (2 vertices) - skip |
| `GL_LINE_RESET_TOKEN` | `14` (2 vertices) - skip (**emitted for `GL_LINE_STRIP`/`GL_LINE_LOOP`; easy to miss**) |
| `GL_BITMAP_TOKEN` | `7` (1 vertex) - skip |
| `GL_DRAW_PIXEL_TOKEN` | `7` (1 vertex) - skip |
| `GL_COPY_PIXEL_TOKEN` | `7` (1 vertex) - skip |
| `GL_PASS_THROUGH_TOKEN` | `1` (passthrough value) - skip |

Notes:
- A `GL_POLYGON_TOKEN` with `n` vertices is fan-triangulated to `n-2` faces; for
  `n=3` (the GLUT-solid / triangle case) that's one face.
- An unrecognized token marker is a hard parse error (don't guess a length - bail
  with an error status), so a corrupt/misaligned stream fails loudly.
- **All these token defines (and `GL_FEEDBACK`/`GL_3D_COLOR`) already exist in
  `tests/gl-stubs/include/GL/gl.h`** - confirmed - so the pure parser compiles and
  is fully testable in stub mode with synthetic buffers.

## Architecture - two modules (capture is GL-coupled; the writer is pure/testable)

- **`src/app/glr_mesh_export.{c,h}`** - GL-coupled orchestration (app layer; may
  use GL + repl). Public:
  ```c
  int glr_export_mesh_ply(const char *path);   /* returns triangle count, or <0 on error */
  ```
  Saves GL state; sets identity modelview (no camera), the `±R` ortho, viewport,
  and `glDepthRange(0,1)`; `glDisable(GL_LIGHTING)`; `glFeedbackBuffer(n, GL_3D_COLOR, buf)`;
  `glRenderMode(GL_FEEDBACK)`; calls `repl_execute_program(&(ReplExecutionOptions){
  .flat_cmd_count = replay_active() ? replay_exec_limit() : program.cmd_count,
  .program = repl_state_flat_program_view(), .text = … })` (same shape as the
  render call site, but with the replay-clamped count per decision 7);
  `glRenderMode(GL_RENDER)` → count (grow/retry on overflow); restores state; hands
  the raw buffer + `MeshPlyCapture` params (`ortho_r`, viewport, `depth_near=0`,
  `depth_far=1`) to the pure module; sets the status message.

- **`src/support/mesh_ply.{c,h}`** - **pure** (neutral `mesh_ply_` prefix; uses the
  GL token *macros* from `<GL/gl.h>` - which exist in the stubs - but calls **no**
  GL functions). Public:
  ```c
  typedef struct {
      float ortho_r;                  /* glOrtho half-extent R (see decision 2) */
      int   vp_x, vp_y, vp_w, vp_h;   /* glViewport */
      float depth_near, depth_far;    /* glDepthRange; capture passes 0,1 (decision 8) */
  } MeshPlyCapture;
  typedef struct {
      int   weld;            /* dedup vertices by (quantized pos, color) */
      float weld_eps;        /* position quantization grid; ~1e-4 (matches ortho
                                precision at R=1000); color matched exact after the
                                8-bit uchar quantization */
      int   smooth_normals;  /* average face normals across welded verts (else flat) */
      int   triangulate;     /* fan-triangulate n-gon faces (recommended on) */
  } MeshPlyOptions;
  /* Returns the triangle count on success, or NEGATIVE on parse error
   * (unknown/misaligned token) or I/O error (fwrite/fprintf failure) - same
   * <0-is-error convention as glr_export_mesh_ply. Partial writes are possible
   * on mid-stream I/O failure; the caller surfaces an error status. */
  int mesh_ply_write(FILE *out, const float *feedback, int float_count,
                     const MeshPlyCapture *cap, const MeshPlyOptions *opts);
  ```
  Does: parse the token stream **per the skip table above** (faces only from
  `GL_POLYGON_TOKEN`); invert ortho + viewport + depth-range → world coords;
  fan-triangulate `n>3` faces; weld vertices by (quantized position, color) →
  index; compute per-face normals (cross product, winding-consistent) and average
  across shared welded vertices; write the PLY ASCII document. Fully unit-testable
  with synthetic buffers under the GL stubs.

  **Layering:** `src/support/` is the neutral low-level tier (like `cpuprof`); the
  pure writer belongs there. The GL/repl-coupled capture is `glr_*` in `src/app/`.
  (The writer pulls in `<GL/gl.h>` only for the ~8 token *macros*; if keeping the
  support tier strictly GL-header-free matters, define those integer constants
  locally instead - they are stable OpenGL enum values - which also drops the test's
  stub dependency.)

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

**Phase 1 - pure `mesh_ply` writer + unit tests (no GL context needed).**
- Implement `src/support/mesh_ply.{c,h}` (parse → invert → triangulate → weld →
  normals → PLY text).
- `tests/test_mesh_ply.c`: synthetic `GL_3D_COLOR` feedback buffers - a single
  `GL_POLYGON_TOKEN` triangle; a quad (`n=4` → 2 faces); two triangles sharing an
  edge (weld → 4 verts, averaged normal); **a polygon preceded by skipped
  `GL_POINT_TOKEN` / `GL_LINE_TOKEN` / `GL_LINE_RESET_TOKEN` / `GL_PASS_THROUGH_TOKEN`
  records (the alignment test - the trailing polygon must still parse correctly,
  catching off-by-N skip bugs)**; an unknown/misaligned token (→ negative return);
  empty buffer (0/0). Assert vertex/face counts, that a known window-coord input
  under a known `MeshPlyCapture` (including `depth_near/far`) inverts to the
  expected world coords, color 0-1→0-255, and normal direction/winding. Register in
  `Makefile`: add `test_mesh_ply` to `TEST_BINS`/`CORE_TEST_BINS` with
  `test_mesh_ply_OBJS = $(OBJDIR)/$(TEST_DIR)/test_mesh_ply.o $(OBJDIR)/src/support/mesh_ply.o`
  and `test_mesh_ply_LDLIBS = -lm` (minimal, like `test_format`). Token macros are
  in `tests/gl-stubs/include/GL/gl.h`, so it builds stub-mode.

**Phase 2 - GL_FEEDBACK capture (`glr_mesh_export`).**
- Implement `src/app/glr_mesh_export.{c,h}` per the design above (state save/
  restore, identity modelview + `±R` ortho + viewport + `glDepthRange(0,1)`,
  lighting off + `GL_FILL` polygon mode + cull off, buffer grow/retry that
  **re-runs the full executor pass** each iteration - the buffer is only populated
  during rendering - then hand off to `mesh_ply_write`).
- Register the new files **explicitly**: the Makefile `SRCS` is a hand-maintained
  list, *not* a `find`/wildcard - add both `src/support/mesh_ply.c` and
  `src/app/glr_mesh_export.c` to `SRCS` (`mesh_ply.c` is already added for the test
  in Phase 1).
- **Extend the GL stubs:** `glr_mesh_export.c` is in the gl-repl object set and
  builds under `make gl-repl USE_GL_STUBS=1` / `make test-stubs`. Every feedback
  symbol it needs is already stubbed *except* **`glDepthRange`, which is MISSING** -
  add it to `tests/gl-stubs/include/GL/gl.h`. (The stub `glRenderMode` returns 0, so
  the capture gets compile-coverage only under stubs; the pure writer carries the
  functional tests - consistent with Phase 1.)
- **Confirm the freeglut solid-capture assumption early** (the headline risk): in
  the vendored freeglut 3.8 build the teapot is **CPU-tessellated in C** (not
  evaluators) and every solid draws via `fghDrawGeometrySolid11` - fixed-function
  client-side vertex arrays (`glVertexPointer`/`glDrawArrays`), no VBO/shader - so
  feedback captures them. (See Risks for the latent constraint.)

**Phase 3 - triggers + status.**
- **Menu:** add `GLR_FILE_ITEM_EXPORT_PLY` to the File-menu enum in
  `src/app/glr_actions.h` (after `GLR_FILE_ITEM_RENAME_SCENE`; bump
  `GLR_FILE_ITEM_COUNT`); add its label string to `menu_item_label()` in
  **`src/ui/app/menu_bar.c`** (the `MENU_FILE` branch - *not* `glr_actions.c`,
  which has no label table); and add a `case` in `glr_action_menu_item_activate()`
  (`src/app/glr_actions.c`) that calls `glr_export_mesh_ply("output.ply")` and
  reports status. Mirror the `GLR_FILE_ITEM_SAVE_SCENE` (Ctrl+S) wiring.
- **Key:** bind **F11** (freed by commit `425c36f8`, "Shift+F12 → previous
  example"; confirm with `make keymap-list`). Add `#define GLR_EXPORT_PLY GLUT_KEY_F11, 0`
  to `keymap.h` and route it in `src/app/glr_ctrl_router.c` (e.g. alongside
  `glr_ctrl_router_handle_scene_cycle_special`, matched via
  `keymap_event_is(key, GLR_EXPORT_PLY)`) to the same export call.
  `make check-keymap-no-dup` must stay green. **Note:** on macOS F11 defaults to
  "Show Desktop" and may be swallowed by the OS - the menu item is the reliable
  trigger; F11 is a convenience.
- **Status:** `repl_set_status` / `repl_set_status_error` take a **plain `const char *`
  (no printf formatting)** - `snprintf` into a local buffer first, then pass it. On
  success `"Exported N triangles to output.ply"`; on failure `repl_set_status_error(...)`
  (feedback overflow at cap, fopen failure, empty scene → "nothing to export").
  Default filename `output.ply` (mirrors `output.c`); optionally derive
  `<scene-name>.ply`.

**Phase 4 - CLI `--export-ply <file>` (optional, deferred).** All current flags
run *pre-context* (`gl_repl.c` parses before `glutInit`), but feedback needs a
live context. So a flag would set a deferred request performed in the **first
`display_func`** (after `glr_ctrl_init_gl`), then exit. Useful for scripting;
not required for the core feature. Keep out of Phases 1-3.

## Files to create / modify

| Path | Change |
|---|---|
| `src/support/mesh_ply.{c,h}` | **new** - pure: feedback-token → PLY (parse, invert, triangulate, weld, normals) |
| `src/app/glr_mesh_export.{c,h}` | **new** - GL_FEEDBACK capture + orchestration + status |
| `tests/test_mesh_ply.c` | **new** - pure unit tests with synthetic feedback buffers |
| `Makefile` | add `src/support/mesh_ply.c` + `src/app/glr_mesh_export.c` to the explicit `SRCS` list; register `test_mesh_ply` (TEST_BINS/CORE_TEST_BINS + `_OBJS`/`_LDLIBS`) |
| `tests/gl-stubs/include/GL/gl.h` | **add `glDepthRange`** (the only missing feedback-path symbol) so `USE_GL_STUBS` / `test-stubs` builds compile |
| `src/app/glr_actions.{c,h}` | `GLR_FILE_ITEM_EXPORT_PLY` enum (bump `_COUNT`) + dispatch `case` |
| `src/ui/app/menu_bar.c` | add the "Export .ply" label string in `menu_item_label()` (`MENU_FILE` branch) |
| `keymap.h` | `GLR_EXPORT_PLY GLUT_KEY_F11, 0` |
| `src/app/glr_ctrl_router.c` | route F11 → `glr_export_mesh_ply` |
| `CLAUDE.md`, `MODULES.md` | document the two new modules + the export action |

## Verification

1. **Unit (`Phase 1`):** `make test_mesh_ply` (and it runs under `make test-stubs`,
   no GL) - triangle/quad/shared-edge/skip-points/empty cases pass; coordinate
   inversion and normal winding assertions hold.
2. **Build:** `make gl-repl` (and `make check-c99`, `make test-stubs`) green;
   `make check-keymap-no-dup` green after the F11 binding.
3. **Runtime (needs a display):**
   - Triangle + quad-loop scene → File → Export .ply → open in Blender/MeshLab:
     geometry matches, **per-vertex colors present**.
   - A `glutSolidTeapot(1)` scene → exported `.ply` **contains the teapot mesh**
     (the headline proof that the single feedback path captures GLUT solids). Also
     verify sphere/cone/torus/cube. *(Confirmed against the vendored freeglut 3.8
     build: the teapot is **CPU-tessellated in C** - not evaluators - and every
     solid draws through `fghDrawGeometrySolid11`, i.e. fixed-function client-side
     vertex arrays with no VBO/shader, all of which feedback captures. Re-confirm if
     freeglut is ever revendored.)*
   - Confirm the visible window is **unchanged** after export (no GL state leak):
     scene still renders, camera/lighting intact.
4. **Overflow path:** a large loop scene exercises buffer grow/retry; status shows
   the triangle count, file opens correctly.

## Risks / caveats

- **Normals are synthesized, not from `glNormal3f`** (feedback omits them) - flat
  per-face by default, smoothed across welded verts. Acceptable and standard;
  noted so reviewers don't expect authored normals.
- **User-enabled face culling drops back faces.** `glEnable(GL_CULL_FACE)` is a
  supported REPL command; since we run the executor unchanged (one path), a scene
  that culls re-enables it mid-capture and feedback omits the culled faces. Inherent
  to the single-path design - documented, not worked around.
- **Only polygon primitives are exported** - `GL_POINTS`/`GL_LINES` feed back as
  point/line tokens and are skipped, so wireframe- or point-only scenes export an
  empty mesh.
- **`±R` clipping** - geometry beyond ±1000 world units is clipped. `R` is a named
  constant; bbox-fit is the deferred refinement if it ever bites.
- **GL_FEEDBACK is fixed-function/deprecated** - fine here (the whole app is
  fixed-function freeglut), but it's the reason this approach works at all; it
  would not port to a core-profile rewrite.
- **freeglut solid capture is verified, with a latent constraint.** Confirmed in the
  vendored freeglut 3.8 build (teapot CPU-tessellated in C; solids via fixed-function
  client vertex arrays `fghDrawGeometrySolid11`, no VBO/shader). It would break only
  if (a) freeglut is revendored onto a shader/VBO solid path, or (b) anyone calls
  `glutSetVertexAttribCoord3/Normal/TexCoord2`, which flips freeglut to its
  `glVertexAttribPointer` path - the REPL never does today.
