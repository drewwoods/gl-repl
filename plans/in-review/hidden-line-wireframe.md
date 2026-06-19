# Hidden-Line Wireframe (depth-primed, dimmed occluded edges)

## Context

Wireframe mode (`Ctrl+G`) currently draws **every** edge — front and back —
because `glPolygonMode(GL_LINE)` renders the geometry as lines with no filled
faces in the depth buffer to occlude the back edges ("see-through" wireframe).

The user wants hidden-line rendering for wireframe, using the **last technique**
from the SIGGRAPH '98 Advanced OpenGL notes (`.../node248.html`) — the
*dual-color hidden/visible line* method: render all edges, prime the depth
buffer with the polygons (color writes off), then redraw edges so visible and
occluded edges get different styles.

**Decisions (confirmed with user):**
- **Fold into Wireframe** as a 3-state cycle on `Ctrl+G`: `Off → Wireframe →
  Hidden-line → Off`.
- Occluded edges drawn **dimmed** (the literal technique), not "fully removed".
- **Fixed** line colors — bright for visible, dim for occluded — deliberately
  **ignoring the program's per-vertex `glColor`**. (Plain Wireframe still shows
  user colors.) This is the desired dual-color look.
- **Pipeline approach** for edges: re-run the program through the real executor
  in line mode (no hand-walkers, no special tess/glut casing). GLU-tessellated
  polygons therefore show their triangulation — same as plain Wireframe today,
  so the two modes stay consistent.

**Outcome:** in Hidden-line mode the object reads as a solid that occludes the
grid and its own back faces; visible edges draw bright, occluded edges draw as
faint lines.

## How the technique maps to this codebase

The object is drawn **three times** per frame (all opt-in, Hidden-line only):

1. **Depth prime** — the existing main-fill hook (`scene_execute_adapter`) runs
   the program *filled* with `glColorMask(0,0,0,0)` and depth writes on. Runs
   *before* the grid/axes helpers, so the solid hull occludes the grid.
2. **Hidden edges** — re-run the program in `glPolygonMode(GL_LINE)` with
   `glDepthFunc(GL_GREATER)` and the dim color: only edges *behind* the primed
   surfaces pass.
3. **Visible edges** — re-run again with `glDepthFunc(GL_LEQUAL)` and the bright
   color: only edges at/in front pass. Polygon offset (reused
   `REPL_OUTLINE_POLYGON_OFFSET_*`) biases coincident edges to "visible".

All three runs go through the same `repl_execute_program` pipeline that existing
wireframe mode uses: `render.c` sets `glPolygonMode(GL_LINE)` in scene setup,
the normal executor emits the program, then the scene restores `GL_FILL` before
helpers. Hidden-line keeps that shape instead of adding a hand-walker: the app
just adds one filled depth-prime run plus two app-owned `GL_LINE` edge runs.

The only executor change is a narrow **`fixed_raster_state`** option (below), so
the caller can own color, depth, blend, cull, lighting, line width, and masks
while the executor still runs the program's transforms, geometry, assignments,
and execute-time control flow normally. The orchestration is app code
(`glr_ctrl.c`); `src/scene/` and the `edit_overlays` subsystem are untouched.

### Why the `fixed_raster_state` flag is needed (state interference)

A plain re-run would replay the program's own `glColorMask` / `glDepthMask` /
`glDepthFunc` / `glColor` / `glEnable(GL_LIGHTING|GL_CULL_FACE)` *after* the
forced pass state, breaking it — e.g. a user `glColorMask(GL_TRUE…)` would draw
solid colored faces in the prime; `glDepthMask(GL_FALSE)` would write no depth
(no occlusion); a user `glColor` would override the fixed line color;
`glEnable(GL_CULL_FACE)` would cull the back edges we need for the dim pass.

Under `fixed_raster_state` the executor skips only commands that would change the
pass's raster/appearance state, so the caller fully owns the forced hidden-line
state. It still executes `CMD_VAR_ASSIGN`, `CMD_SCRATCH_ASSIGN`,
`CMD_IF_BEGIN`/`CMD_IF_END`, `CMD_GOTO`/labels, transforms, `glBegin` blocks,
GLU tess, and `glutSolid*` exactly like the existing wireframe path. This
mirrors the spirit of `encode_feedback_normals` (mesh export), which already
suppresses selected user state while leaving geometry execution intact.

## Changes

### 1. Config: Wireframe becomes a 3-state cycle (app)

- `src/app/glr_actions.c`: add `static const char *wireframe_mode_names[] = {
  "Off", "Wireframe", "Hidden-line" };` and change the Wireframe row
  (≈`glr_actions.c:164`) to `.state_count = 3, .state_names =
  wireframe_mode_names`. Binding stays `GLR_WIREFRAME` — a `state_count > 2` row
  cycles forward on each press, so `Ctrl+G` now cycles three states. **No
  keymap.h change.**
- **No other config plumbing.** `presentation.wireframe` is already a plain
  `int` (`glr_config.c:117/171`, passthrough get/set), default
  `CFG_DEFAULT_WIREFRAME = 0` (Off) stays valid; already in
  `cfg_key_in_scene_subset` (`glr_actions.c:288`) so it persists per scene.
- **Serialization is automatic.** `cfg_symbol_table_for_slug` has *no*
  `wireframe` entry, so it emits the integer (`@cfg wireframe = 0|1|2`) and reads
  it back via the integer-literal fallback. Old `wireframe = 0/1` scenes load
  unchanged; examples can opt into hidden-line with `// @cfg wireframe = 2` (slug
  already allowed). The new `state_names` only affect menu/status display.

### 2. Executor `fixed_raster_state` option (the one repl-side change)

- `src/repl/executor.h`: add these fields to `ReplExecutionOptions` (≈line 97,
  beside `encode_feedback_normals`), documented:
  ```c
  int   fixed_raster_state;
  int   fixed_color_enabled;
  float fixed_color[4];
  ```
- `src/repl/executor.c`: when `opts->fixed_raster_state`, preserve the normal
  executor walk but suppress only commands that would overwrite the caller's
  forced pass state:
  - Skip appearance/raster state: `CMD_COLOR3F`, `CMD_COLOR4F`,
    `CMD_TESS_COLOR`, `CMD_ENABLE`, `CMD_DISABLE`, `CMD_SHADE_MODEL`,
    `CMD_COLOR_MATERIAL`, `CMD_MATERIALFV`, `CMD_MATERIALF`,
    `CMD_LIGHT_MODEL_I`, `CMD_FRONT_FACE`, `CMD_DEPTH_FUNC`,
    `CMD_DEPTH_MASK`, `CMD_COLOR_MASK`, `CMD_POINT_PARAMETER_FV`,
    `CMD_BLEND_FUNC`, `CMD_CLEAR_COLOR`, `CMD_POINT_SIZE`, `CMD_LINE_WIDTH`,
    `CMD_RASTER_POS3F`, and `CMD_LABEL`.
  - Keep side effects and control flow: `CMD_VAR_ASSIGN`,
    `CMD_SCRATCH_ASSIGN`, `CMD_IF_BEGIN`/`CMD_IF_END`, `CMD_GOTO`/labels.
  - Keep geometry and transforms: `repl_cmd_is_transform(...)`,
    `CMD_BEGIN`/`CMD_END`, `CMD_VERTEX2F`/`CMD_VERTEX3F`, tess begin/contour/end
    and vertices, `CMD_NORMAL3F`/`CMD_TESS_NORMAL`, and `repl_cmd_is_glut_solid`.
- Fixed color for tess: GLU tess callbacks currently emit `glColor4dv(v->color)`
  from each `TessVertex`, so simply skipping `CMD_TESS_COLOR` would turn tess
  output white/opaque instead of using the hidden-line pass color. When
  `fixed_color_enabled` is set, initialize the local tess color to
  `fixed_color`, ignore user `CMD_TESS_COLOR`, and copy `fixed_color` into every
  tess vertex (including combine-generated vertices). Plain `glBegin` and
  `glutSolid*` geometry rely on the caller's current `glColor4f`, just like the
  existing pipeline wireframe redraws rely on current GL state.

### 3. Orchestration: prime + two edge passes (app)

All in `src/app/glr_ctrl.c`; `src/scene/` and `edit_overlays` untouched.

- `glr_ctrl_build_scene_config` tail: set a per-frame file-static
  `g_hidden_line_active = (presentation.wireframe == 2) && !replay_active();`
  (replay falls back to plain see-through wireframe — avoids clashing with the
  replay fade `post_fill` pass). Keep `config->wireframe =
  (presentation.wireframe != 0)` so the scene's truthy checks
  (`render.c:481/640`) keep `GL_LINE` semantics — scene stays a bool, no
  `render_types.h` change.
- Factor a small helper `glr_ctrl_run_geometry_only(int count)` →
  `repl_execute_program(&(ReplExecutionOptions){ .geometry_only = 1,
  .flat_cmd_count = count, .program = repl_state_flat_program_view(), .text =
  source_document_view(), .status_out = … })`, reused by all three runs.
- `scene_execute_adapter` (`glr_ctrl.c:743`), `SCENE_EXEC_MAIN_FILL` when
  `g_hidden_line_active`: before running, force the prime state —
  `glPolygonMode(FILL)`, `glColorMask(0,0,0,0)`, `glDepthMask(GL_TRUE)`,
  `glEnable(GL_DEPTH_TEST)`, `glDepthFunc(GL_LEQUAL)`, `glDisable(GL_CULL_FACE)`,
  `glDisable(GL_LIGHTING)` — then `glr_ctrl_run_geometry_only(count)`. Already
  wrapped in `glPushAttrib(GL_ALL_ATTRIB_BITS)`/`glPopAttrib`
  (`glr_ctrl.c:764/773`); side-effect save/restore unchanged (the prime *is* the
  main fill — applies accumulating-program effects once).
- New `glr_ctrl_post_overlays(void *ud)` wrapper; set
  `config->post_overlays_fn = glr_ctrl_post_overlays` (instead of
  `edit_overlays_post_overlays` directly, `glr_ctrl.c:841`). Body: if
  `g_hidden_line_active`, call `glr_ctrl_render_hidden_line_edges()`, then always
  `edit_overlays_post_overlays(ud)`.
- `glr_ctrl_render_hidden_line_edges()`:
  - **Guard side effects** like the adapter's non-MAIN_FILL path
    (`glr_ctrl.c:758-781`): save/restore predef vars, scratch arrays, and
    `ReplRenderState` around the two runs, so the extra passes don't re-advance
    accumulating programs (`t=t+1`, `A[0]=A[0]+1`).
  - `glPushAttrib(GL_ALL_ATTRIB_BITS)` + `glPushMatrix()`; common edge state:
    lighting off, cull off, `glEnable(GL_DEPTH_TEST)`, `glDepthMask(GL_FALSE)`
    (don't disturb primed depth), `glColorMask` all true, blend SRC_ALPHA/
    ONE_MINUS, `glEnable(GL_POLYGON_OFFSET_LINE)` + `glPolygonOffset(REPL_OUTLINE_
    POLYGON_OFFSET_FACTOR, _UNITS)`, `glPolygonMode(GL_LINE)`, MSAA/line-smooth
    from `glr_state_render()`.
  - Hidden pass: `glDepthFunc(GL_GREATER)`,
    `glColor4f(REPL_HIDDEN_LINE_HIDDEN_RGBA)`,
    `glLineWidth(REPL_HIDDEN_LINE_HIDDEN_WIDTH)`, `glr_ctrl_run_geometry_only(count)`.
  - Visible pass: `glDepthFunc(GL_LEQUAL)`,
    `glColor4f(REPL_HIDDEN_LINE_VISIBLE_RGBA)`,
    `glLineWidth(REPL_HIDDEN_LINE_VISIBLE_WIDTH)`, run again.
  - `glPopMatrix()` + `glPopAttrib()`. The own push/pop keeps the leftover
    `glDepthFunc` from leaking into the following `edit_overlays` overlays.
  - `count = repl_state_flat_program_count()` (not replay-clamped — hidden-line
    is off during replay).

### 4. Named colors/widths — no magic numbers (config.h)

Define beside `REPL_OUTLINE_POLYGON_OFFSET_*` (`config.h:186`), comma-list form
so each feeds `glColor4f`/`glLineWidth` directly and is retuned in one place
(`config.h` is force-included everywhere):
```c
#define REPL_HIDDEN_LINE_VISIBLE_RGBA  0.82f, 0.86f, 0.92f, 0.95f
#define REPL_HIDDEN_LINE_HIDDEN_RGBA   0.82f, 0.86f, 0.92f, 0.16f
#define REPL_HIDDEN_LINE_VISIBLE_WIDTH 1.4f
#define REPL_HIDDEN_LINE_HIDDEN_WIDTH  1.0f
```
Visible/hidden are independent — can differ in hue, not just alpha.

## Notes / edge cases

- **Tess / strips show triangulation.** Re-running through the pipeline draws the
  *rendered* polygons' edges, so GLU-tess polygons and triangle strips show their
  internal edges — identical to today's plain Wireframe mode (intended,
  consistent). Clean contour-only outlines would require the hand-walk approach
  that was rejected.
- **Replay:** `g_hidden_line_active` is false while replaying → prime and edge
  passes skipped → hidden-line degrades to plain see-through wireframe.
- **2D / ortho:** harmless — prime writes ~constant depth, all edges read
  visible (looks like normal wireframe).
- **Cost:** Hidden-line runs the executor 3× (prime + 2 edges), re-evaluating
  animated expressions each time — acceptable for an opt-in mode; zero cost when
  off (`g_hidden_line_active` gates everything).
- **GL stubs:** every symbol used (`glColorMask`, `glDepthFunc`, `GL_GREATER`,
  `GL_LEQUAL`, `glPolygonOffset`, `GL_POLYGON_OFFSET_LINE`) is already exercised
  by the executor / outline pass; stub headers need no change.

## Files to modify

- `src/app/glr_actions.c` — Wireframe row → 3-state + names array.
- `src/repl/executor.h` / `src/repl/executor.c` — `geometry_only` option + the
  skip gate.
- `src/app/glr_ctrl.c` — `g_hidden_line_active`, `config->wireframe` bool
  coerce, prime branch in `scene_execute_adapter`, `glr_ctrl_run_geometry_only`,
  `glr_ctrl_post_overlays` wrapper + `glr_ctrl_render_hidden_line_edges`.
- `config.h` — named `REPL_HIDDEN_LINE_*` color/width constants.
- `tests/test_glr_actions.c` — update the `g_cfg_items` runtime twin if it
  asserts Wireframe is 2-state (grep `WIREFRAME`).
- `CLAUDE.md` Key Controls — `Ctrl+G` is now a 3-state cycle (Off / Wireframe /
  Hidden-line); note `@cfg wireframe = 2`. (And `README.md`/`USER_GUIDE.md` if
  they call wireframe a toggle.)

## Verification

1. **Build:** `make gl-repl`, `make test-stubs`, `make gl-repl USE_GL_STUBS=1`,
   `make check-c99`, `make check-state-ownership`.
2. **Tests:** `make test` (debug ASan+UBSan); `make test_repl_core_examples` if
   an example gains `@cfg wireframe = 2`.
3. **Interactive:** `./gl-repl --example teapot` (or torus / a `glutSolid*`),
   press `Ctrl+G` twice → Hidden-line. Confirm back edges faint, front edges
   bright, grid hidden behind the solid; one more `Ctrl+G` → Off. Save
   (`Ctrl+S`), reload (`./gl-repl output.c`) → mode persists (`@cfg wireframe =
   2`).
   - **State-interference check (the `geometry_only` flag):** in Hidden-line
     mode add `glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);` and, separately,
     `glDepthMask(GL_FALSE);` and a `glColor3f(1,0,0);` to the program —
     occlusion and the fixed line colors must stay correct (no solid colored
     faces, no red edges, hidden edges still dimmed).
4. **Headless screenshot (OSMesa):** `make gl-repl FREEGLUT_OSMESA=1`; run a
   staged scene with `// @cfg wireframe = 2`, capture via
   `FREEGLUT_CAPTURE_FRAMES`/`SIGUSR1`, convert the PPM; eyeball dim-vs-bright
   edges vs. `wireframe = 1`.
5. **Real-gcc / Linux:** `ssh gracemont` → `git pull --ff-only origin main &&
   make check-c99 && make test-stubs`.
