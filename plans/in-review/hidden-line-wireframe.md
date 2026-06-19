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
- **Existing-pattern feature, not a new rendering model.** Existing Wireframe
  already uses `glPolygonMode(GL_LINE)` around the real executor, and existing
  polygon highlight / vertex outlines already do app-owned line redraws with
  depth test, polygon offset, blend, and fixed overlay colors. Hidden-line is
  the same family: the normal all-geometry wireframe pass plus one depth-tested
  visible-edge overlay.

**Outcome:** in Hidden-line mode the object reads as a solid that occludes the
grid and its own back faces; visible edges draw bright, occluded edges draw as
faint lines.

## How the technique maps to this codebase

The object is drawn **three times** per frame (all opt-in, Hidden-line only),
but the first pass is not new:

1. **Dim all edges** — use the existing Wireframe path: `render.c` sets
   `glPolygonMode(GL_LINE)` in scene setup, then the normal executor emits the
   program in `scene_execute_adapter`. Hidden-line only forces app-owned raster
   state for this pass: fixed dim color, lighting/cull off, depth writes off
   (so this pass does not seed the depth buffer), and user appearance/raster
   state suppressed. This is the literal "draw all edges dim first" pass.
2. **Depth prime** — in `post_fill_fn`, before `render.c` restores `GL_FILL`
   and before grid/axes/helpers render, re-run the program filled with
   `glColorMask(0,0,0,0)` and depth writes on. This silent fill gives the
   object a solid depth hull, so later helpers/grid are occluded by the object.
3. **Visible edges** — in `post_overlays_fn`, after helpers, re-run the program
   in `glPolygonMode(GL_LINE)` with `glDepthFunc(GL_LEQUAL)` and the bright
   color. Polygon offset (reused `REPL_OUTLINE_POLYGON_OFFSET_*`) biases
   coincident edges to "visible".

All three runs go through the same `repl_execute_program` pipeline that existing
Wireframe uses. Hidden-line keeps that shape instead of adding a hand-walker:
it reuses the main wireframe pass for dim all-edges, then adds a silent fill
prime and one bright visible-edge redraw in the app hooks.

The REPL-side change is a small **post-flatten fixed-raster filter** plus a
tess fixed-color option. The filter builds a pass-local `FlatProgramView` from
the already-flattened program, marking commands that would overwrite the
pass-owned raster state by setting an "ignored" bit in `cmd.type`. That makes
ignored commands distinct type values for naive `cmd.type == CMD_COLOR3F` /
`switch (cmd.type)` checks, while helper APIs can still recover the base command
type when they intentionally need it. The executor still runs the program's
transforms, geometry, assignments, and execute-time control flow normally. The
app controller remains the REPL execution adapter, but the new GL passes live
with the existing edit-overlay rendering code rather than as controller-local
drawing helpers.

### Why the fixed-raster filter is needed (state interference)

A plain run would replay the program's own `glColorMask` / `glDepthMask` /
`glDepthFunc` / `glColor` / `glEnable(GL_LIGHTING|GL_CULL_FACE)` *after* the
forced pass state, breaking it — e.g. a user `glColorMask(GL_TRUE…)` would draw
solid colored faces in the prime; `glDepthMask(GL_FALSE)` would write no depth
(no occlusion); a user `glColor` would override the fixed line color;
`glEnable(GL_CULL_FACE)` would cull the back edges we need for the dim pass.

The fixed-raster filter suppresses only commands that would change the pass's
raster/appearance state, so the caller fully owns the forced hidden-line state.
It does not delete/compact commands or rewrite their type: flat indices remain
stable, `local_vars` stay aligned by index, `CMD_GOTO` label scans still see the
same stream shape, `CMD_IF_BEGIN` false-branch skipping keeps the same block
structure, and pass-specific code can still inspect the original command through
the base-type helper. This mirrors the spirit of `encode_feedback_normals` (mesh
export), which already suppresses selected user state while leaving geometry
execution intact.

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

### 2. Post-flatten fixed-raster filter + tess color option

- `src/repl/command.h`: reserve an ignored-command bit in `CmdType`, e.g.
  ```c
  #define CMD_TYPE_IGNORED_BIT 0x1000u

  static inline CmdType repl_cmd_type_base(CmdType type);
  static inline int repl_cmd_type_is_ignored(CmdType type);
  static inline CmdType repl_cmd_type_ignored(CmdType type);
  ```
  Add a `STATIC_ASSERT(CMD_TYPE_COUNT < CMD_TYPE_IGNORED_BIT, ...)` so the bit
  cannot collide with real enum values. This is not source semantics and must
  not be set on the live document/flat program. It is only for scratch
  `FlatProgramView` copies built for special execution passes. Any table lookup
  or display helper that intentionally accepts ignored values must mask through
  `repl_cmd_type_base(type)` first; raw comparisons intentionally do not match.
- Add a small reusable flat-program filter helper (for example under
  `src/repl/flatten_filter.[ch]` or near the executor if that fits local
  ownership better):
  ```c
  typedef int (*ReplFlatSuppressFn)(const GLCmd *cmd, void *user_data);

  int repl_flat_program_filter_mark_suppressed(FlatProgramView src,
                                               int src_count,
                                               GLCmd *scratch_cmds,
                                               int scratch_capacity,
                                               ReplFlatSuppressFn suppress_fn,
                                               void *suppress_user_data,
                                               FlatProgramView *out);
  ```
  The helper copies `src.cmds[0..src_count)` into `scratch_cmds`; when
  `suppress_fn` returns true, it writes `cmd.type =
  repl_cmd_type_ignored(cmd.type)` on that scratch command while preserving the
  original args, payload, and provenance. The output view reuses
  `src.local_vars` because indices are unchanged. It must not mutate the live
  flat program.
- Prefer the pass-local ignored type bit over deletion/compaction or `CMD_EMPTY`
  replacement for this feature. Physical deletion would require rebuilding the
  parallel `FlatCmdLocalVars` table and auditing every flat-index-sensitive path
  (`CMD_GOTO`, `CMD_IF_BEGIN` skipping, replay clamps, cursor/source mapping).
  Rewriting to `CMD_EMPTY` avoids those index problems but loses the original
  command identity. A separate `exec_flags` field preserves identity but leaves
  raw type matches live; the ignored type bit keeps stream shape and command
  information while making ignored commands non-matching by default.
- `src/repl/executor.h`: add the tess fixed-color fields to
  `ReplExecutionOptions` (beside `encode_feedback_normals`), documented:
  ```c
  int   fixed_tess_color_enabled;
  float fixed_tess_color[4];
  ```
- Add a blacklist predicate for the filter, e.g.
  `repl_cmd_is_fixed_raster_suppressed(CmdType type)`. Do not implement a
  geometry/control-flow whitelist: every command remains in the filtered program
  unless this predicate identifies it as raster/appearance/text state that the
  hidden-line pass owns. New geometry/control commands should therefore keep
  working by default; only new state/text/appearance commands should be added to
  the predicate.
  - Mark appearance/state commands: `CMD_COLOR3F`, `CMD_COLOR4F`,
    `CMD_TESS_COLOR`, `CMD_ENABLE`, `CMD_DISABLE`, `CMD_SHADE_MODEL`,
    `CMD_COLOR_MATERIAL`, `CMD_MATERIALFV`, `CMD_MATERIALF`,
    `CMD_LIGHT_MODEL_I`, `CMD_FRONT_FACE`, `CMD_DEPTH_FUNC`,
    `CMD_DEPTH_MASK`, `CMD_COLOR_MASK`, `CMD_POINT_PARAMETER_FV`,
    `CMD_BLEND_FUNC`, `CMD_CLEAR_COLOR`, `CMD_POINT_SIZE`, and
    `CMD_LINE_WIDTH`.
  - Mark bitmap text rendering: `CMD_RASTER_POS3F` and `CMD_LABEL`. These are
    not polygon raster state, but once fixed-raster passes suppress text labels,
    the raster-position setup is also irrelevant and should not leak text into
    hidden-line passes.
  - Everything else falls through normally. In particular, side effects and
    control flow must not be blacklisted: `CMD_VAR_ASSIGN`,
    `CMD_SCRATCH_ASSIGN`, `CMD_IF_BEGIN`/`CMD_IF_END`,
    `CMD_FOR_BEGIN`/`CMD_FOR_END`, `CMD_FUNC_DEF`/`CMD_FUNC_END`, `CMD_CALL`,
    `CMD_GOTO`, and `CMD_GOTO_LABEL`. `CMD_COMMENT`, `CMD_EMPTY`, and
    `CMD_VAR_DECLARE` are harmless no-ops if present.
  - Geometry and transforms must also fall through normally:
    `repl_cmd_is_transform(...)`,
    `CMD_BEGIN`/`CMD_END`, `CMD_VERTEX2F`/`CMD_VERTEX3F`,
    `CMD_TESS_BEGIN_POLYGON`, `CMD_TESS_BEGIN_CONTOUR`, `CMD_TESS_END`,
    `CMD_TESS_VERTEX`, `CMD_NORMAL3F`/`CMD_TESS_NORMAL`, and
    `repl_cmd_is_glut_solid`.
- For the persistent hidden-line main pass, be careful not to lose the tiny
  REPL render-model updates that currently happen as a side effect of GL state
  commands. The concrete one that matters is
  `ReplRenderState.light_enabled_mask`: `repl_execute_program()` clears it at
  the start of each walk, and `repl_apply_state_cmd()` repopulates it from
  `CMD_ENABLE` / `CMD_DISABLE` for `GL_LIGHTn`; the light-indicator overlay reads
  that mask through `SceneRenderConfig.lights[].enabled`. Because ignored types
  preserve their base type, the executor can update the light mask for ignored
  `GL_LIGHTn` enable/disable commands by checking `repl_cmd_type_base(cmd.type)`
  without issuing `glEnable` / `glDisable`. This is a runtime overlay concern,
  not an export concern: export emits user `glEnable` lines and light-property
  setup from the source/app bridges, not from this live bitmask.
  `CMD_CLEAR_COLOR` needs no extra handling here because both runtime scene
  config and export already resolve clear color by scanning the command stream.
  The depth-prime and visible-edge overlay passes remain non-persistent and
  restore their snapshots.
- Fixed color for tess: GLU tess callbacks currently emit `glColor4dv(v->color)`
  from each `TessVertex`, so simply skipping `CMD_TESS_COLOR` would turn tess
  output white/opaque instead of using the hidden-line pass color. Filtering
  marking user `CMD_TESS_COLOR` as suppressed is necessary but not sufficient
  for vertices that appear before any tess color command. When
  `fixed_tess_color_enabled` is set, initialize the local tess color to
  `fixed_tess_color` and copy that color into every tess vertex (including
  combine-generated vertices). Plain `glBegin` and `glutSolid*` geometry rely on
  the caller's current `glColor4f`, just like the existing pipeline wireframe
  redraws rely on current GL state.

### 3. Scene execution purposes + overlay pass placement

This should follow existing boundaries:

- `src/scene/render.c` keeps owning pass order: setup → main fill →
  `post_fill_fn` → helpers/grid/axes → `post_overlays_fn`.
- `src/app/glr_ctrl.c` owns REPL execution adaptation and config wiring.
- `src/subsystems/edit_overlays/` owns overlay-style GL state brackets and the
  hidden-line depth-prime / visible-line passes.

Do **not** land the hidden-line GL pass bodies in `glr_ctrl.c`; that file should
only set flags, install hooks, build the filtered execution view, and translate
`SceneExecutePurpose` into `ReplExecutionOptions`.

### 3a. Scene execution purposes

- `src/scene/render_types.h`: add hidden-line execution purposes:
  ```c
  SCENE_EXEC_HIDDEN_LINE_DEPTH_PRIME,
  SCENE_EXEC_HIDDEN_LINE_VISIBLE,
  ```
  The dim all-edges pass still arrives as `SCENE_EXEC_MAIN_FILL`; hidden-line
  mode changes the raster state for that normal main-fill execution.
- `src/app/glr_ctrl.c`, `scene_execute_adapter`: handle these purposes by
  filtering the live `FlatProgramView` into a scratch suppression-marked view
  and selecting the fixed tess color option:
  - `SCENE_EXEC_MAIN_FILL` + `g_hidden_line_active`: capture the hidden-line
    baseline, run the existing wireframe execution against the filtered view
    with fixed dim color, apply the small non-GL `ReplRenderState` bookkeeping
    for suppressed state commands once, then capture the post-main-fill state.
  - `SCENE_EXEC_HIDDEN_LINE_DEPTH_PRIME`: restore the captured baseline, run
    the filtered view with no fixed tess color (color writes are disabled), then
    restore the saved post-main-fill state.
  - `SCENE_EXEC_HIDDEN_LINE_VISIBLE`: restore the captured baseline, run
    the filtered view with fixed visible tess color, then restore the saved
    post-prime state.
  This is the controller's legitimate role: it bridges scene purposes to REPL
  state snapshots and executor options. It should not also contain the GL
  overlay drawing bodies.

### 3b. Overlay snapshot / hook wiring

- `src/subsystems/edit_overlays/edit_overlays.h`: extend `OverlaySnapshotPack`
  with hidden-line execution inputs:
  ```c
  int hidden_line;
  SceneExecuteProgramFn execute_fn;
  void *execute_user_data;
  ```
  Include the narrow scene render type header as needed. This keeps the overlay
  subsystem able to ask the scene/app execution adapter to emit user geometry
  without reaching directly into `glr_ctrl.c`.
- `src/app/glr_ctrl.c`, `glr_ctrl_build_overlay_pack`: populate
  `pack->hidden_line = g_hidden_line_active`, `pack->execute_fn =
  scene_execute_adapter`, and `pack->execute_user_data = NULL` (or the config's
  execute user data if that ever stops being NULL).
- `src/app/glr_ctrl.c`, `glr_ctrl_build_scene_config`:
  - Keep `config->post_overlays_fn = edit_overlays_post_overlays`; no controller
    wrapper.
  - When hidden-line is active, install `config->post_fill_fn =
    edit_overlays_hidden_line_depth_prime` with `post_fill_user_data =
    &g_overlay_pack`, unless replay fade/tess preview owns the hook (hidden-line
    is already disabled during replay).

### 3c. Hidden-line passes in `edit_overlays`

- Add `edit_overlays_hidden_line_depth_prime(void *user_data)`. It runs before
  helpers/grid/axes and before `render.c` restores `GL_FILL`, so it must bracket
  its own state:
  - Use `glPushAttrib(GL_ALL_ATTRIB_BITS)` and `glPushMatrix()`; unlike attribs,
    the modelview stack is not restored by `glPushAttrib`, and this hook runs
    outside the main fill pass's execute `glPushMatrix()` / `glPopMatrix()`.
  - Force prime GL state: `glPolygonMode(FILL)`,
    `glColorMask(0,0,0,0)`, `glDepthMask(GL_TRUE)`,
    `glEnable(GL_DEPTH_TEST)`, `glDepthFunc(GL_LEQUAL)`,
    `glDisable(GL_CULL_FACE)`, `glDisable(GL_LIGHTING)`.
  - Call `pack->execute_fn` with purpose `SCENE_EXEC_HIDDEN_LINE_DEPTH_PRIME`.
  - Restore with `glPopMatrix()` + `glPopAttrib()`. REPL baseline restore/save happens inside
    `scene_execute_adapter`, because only the controller should touch REPL
    mutable state.
  - If replay fade/tess preview is active, `g_hidden_line_active` is already
    false, so the existing replay `post_fill_fn` remains the only hook.
- In `edit_overlays_post_overlays`: if `pack->hidden_line`, call
  `edit_overlays_render_hidden_line_visible_edges(pack)` before the existing
  outline/point/guide overlays.
- `edit_overlays_render_hidden_line_visible_edges()`:
  - State bracket mirrors the existing polygon-highlight / vertex-outline
    overlay pattern: `glPushAttrib(GL_ALL_ATTRIB_BITS)` + `glPushMatrix()`;
    lighting off, cull off, `glEnable(GL_DEPTH_TEST)`, `glDepthFunc(GL_LEQUAL)`,
    `glDepthMask(GL_FALSE)` (don't disturb primed depth), `glColorMask` all true,
    blend SRC_ALPHA/ONE_MINUS, `glEnable(GL_POLYGON_OFFSET_LINE)` +
    `glPolygonOffset(REPL_OUTLINE_POLYGON_OFFSET_FACTOR, _UNITS)`,
    `glPolygonMode(GL_LINE)`, MSAA/line-smooth from `glr_state_render()`,
    `glColor4f(REPL_HIDDEN_LINE_VISIBLE_RGBA)`, and
    `glLineWidth(REPL_HIDDEN_LINE_VISIBLE_WIDTH)`.
  - Call `pack->execute_fn` with purpose `SCENE_EXEC_HIDDEN_LINE_VISIBLE`.
  - `glPopMatrix()` + `glPopAttrib()`. REPL baseline restore/save happens inside
    `scene_execute_adapter`; the overlay only owns GL state.

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
- **Replay:** `g_hidden_line_active` is false while replaying → hidden-line
  extras are skipped → hidden-line degrades to plain see-through wireframe.
- **2D / ortho:** harmless — prime writes ~constant depth, all edges read
  visible (looks like normal wireframe).
- **Cost:** Hidden-line runs the executor 3× per scene pass (dim wireframe +
  depth prime + visible redraw). With accumulation AA/motion blur enabled, that
  becomes `3 × accum_passes` executor walks because each jittered sub-frame needs
  its own primed depth buffer and visible-edge redraw. The extra fill/visible
  passes restore the captured main-fill baseline before each run and restore the
  post-prime state afterward, so execute-time assignments/control flow are
  evaluated consistently without persistent re-advance. Acceptable for an opt-in
  mode; zero cost when off (`g_hidden_line_active` gates everything).
- **GL stubs:** every symbol used (`glColorMask`, `glDepthFunc`, `GL_LEQUAL`,
  `glPolygonOffset`, `GL_POLYGON_OFFSET_LINE`) is already exercised by the
  executor / outline pass; stub headers need no change.

## Files to modify

- `src/app/glr_actions.c` — Wireframe row → 3-state + names array.
- `src/repl/command.h` — add the pass-local `CMD_TYPE_IGNORED_BIT` helpers for
  ignored `CmdType` values.
- `src/repl/flatten_filter.h` / `src/repl/flatten_filter.c` (or an equivalent
  repl-owned helper location) — reusable post-flatten filter that copies a flat
  program and marks blacklisted commands with `CMD_TYPE_IGNORED_BIT`.
- `src/repl/executor.h` / `src/repl/executor.c` — treat ignored command types as
  no-GL-emission commands without losing allowed non-GL bookkeeping, plus fixed
  tess color handling for GLU tess callbacks.
- `src/scene/render_types.h` — add the hidden-line `SceneExecutePurpose` values
  used by overlay hooks to ask the app execution adapter for a depth-prime or
  visible-edge geometry replay.
- `src/app/glr_ctrl.c` — `g_hidden_line_active`, `config->wireframe` bool
  coerce, overlay-pack execution callback wiring, `post_fill_fn` installation,
  hidden-line main-fill fixed-raster branch in `scene_execute_adapter`, and
  hidden-line REPL baseline/post-pass state snapshots. No controller-local GL
  pass bodies or `post_overlays_fn` wrapper.
- `src/subsystems/edit_overlays/edit_overlays.h` /
  `src/subsystems/edit_overlays/edit_overlays.c` — extend `OverlaySnapshotPack`
  with hidden-line execution inputs, add `edit_overlays_hidden_line_depth_prime`,
  and render the visible-edge overlay from `edit_overlays_post_overlays`.
- `config.h` — named `REPL_HIDDEN_LINE_*` color/width constants.
- `tests/test_glr_actions.c` — update the `g_cfg_items` runtime twin if it
  asserts Wireframe is 2-state (grep `WIREFRAME`), and update
  `test_cfg_cycling()`'s status assertion from `"Wireframe: ON"` to the named
  state string (then add/adjust a second cycle assertion for `"Hidden-line"`).
- `tests/test_repl_executor.c` — cover the fixed-raster suppression filter:
  raster / appearance commands become ignored type values whose base type still
  resolves correctly, raw type matches do not fire, assignments/control
  flow/geometry keep their slots and still run, and fixed tess color prevents
  `CMD_TESS_COLOR` / tess callbacks from overriding the pass color.
- `src/repl/help_text.c` — `Ctrl+G` help text changes from "Toggle wireframe" to
  "Cycle wireframe (Off / Wire / Hidden-line)" or similarly explicit wording.
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
   - **State-interference check (the fixed-raster suppression filter):** in Hidden-line
     mode add `glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);` and, separately,
     `glDepthMask(GL_FALSE);` and a `glColor3f(1,0,0);` to the program —
     occlusion and the fixed line colors must stay correct (no solid colored
     faces, no red edges, hidden edges still dimmed).
   - **Light-background check:** set a bright `glClearColor` and confirm the
     hidden-line colors still read acceptably; retune `REPL_HIDDEN_LINE_*` if the
     dim pass disappears completely on light scenes.
   - **Side-effect baseline check:** add a self-updating assignment before
     geometry (`t = t + 1;` or `A[0] = A[0] + 1;`) and confirm the dim and
     visible wireframe edges overlap exactly rather than drifting apart.
4. **Headless screenshot (OSMesa):** `make gl-repl FREEGLUT_OSMESA=1`; run a
   staged scene with `// @cfg wireframe = 2`, capture via
   `FREEGLUT_CAPTURE_FRAMES`/`SIGUSR1`, convert the PPM; eyeball dim-vs-bright
   edges vs. `wireframe = 1`.
5. **Real-gcc / Linux:** `ssh gracemont` → `git pull --ff-only origin main &&
   make check-c99 && make test-stubs`.
