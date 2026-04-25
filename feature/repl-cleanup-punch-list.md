# REPL Refactor Punch List

## Context

The companion doc `feature/repl-cleanup.md` is the strategic 10-stage
ownership-reorganization plan. This punch list is the tactical
counterpart: a prioritized set of concrete, behavior-preserving
extractions that can each land as a single reviewable commit *today*,
without committing to the larger context-object rewrite.

Four files still dominate the codebase: `scene_render.c` (frame prep,
replay HUD), `scene_grid.c` (grid themes), `scene_axes.c`
(axes themes), `repl_export.c` (2541), `repl_core.c` (~1964), and
`repl_editor.c` (~1653). `ui_panels.c`
dropped to 1237 LoC (from 4452) after Phase-7
extractions: document rows, replay annotations, menu/dropdowns, color
picker, help overlay, variable panel, autocomplete popup, inline rename,
and variable dragging. The live GL call surface is already mostly in the
scene/UI renderers plus `repl_executor.c`; the remaining out-of-band call
sites are frame orchestration in `repl_core.c`, tessellation/bootstrap
wiring in `repl_state.c`, a couple of inline helpers in `sample.h`, and
text-emitting export/example scaffolds that intentionally print GL source
rather than execute it. The highest-value remaining refactors fall into
two camps:

1. **Mechanical extractions** - self-contained features still living in
   the wrong file. Low risk, immediate LoC reduction, no behavior
   change.
2. **Pattern consolidation** - repeated boilerplate (per-theme switch
   cases, per-overlay traversal loops, repeated GL-pass emit blocks)
   that hurts every time someone adds a new theme/overlay.

This list is ordered by impact-per-effort. Pick one and execute it
in isolation; each item is sized to land as one `refactor:` commit.

> **Baseline note:** `make test-stubs TEST_JOBS=4` currently passes
> all 19 suites / 2437 tests cleanly. Any new failure introduced by a
> punch-list item is a real regression unless the team explicitly
> rebaselines it.

---

## Tier 1 - Mechanical extractions (hours each, near-zero risk)

### 1. Extract help overlay → `ui_help_overlay.c` ✅ DONE

Landed as `refactor: extract help overlay` on branch
`immediate-mode-repl/repl-cleanup-2`. `render_help()` plus its
`_HELP_STR`/`_HELP_STR2` macros moved out of `ui_panels.c` (which
shrunk from 2051 → 1610 LoC). State globals (`g_show_help`,
`g_help_tab`, `g_help_scroll`) stayed in `repl_state.h` because they
are mutated from `repl_editor.c` and `repl_search.c`.

### 1b. Extract variable panel → `ui_variable_panel.c` ✅ DONE

Landed as `refactor: extract variable slider panel`. `render_var_panel()`,
`var_panel_rect()`, `var_panel_hit()`, the asinh slider math, and the
replay-lift easing state moved out of `ui_panels.c` (1610 → 1399 LoC).
The renderer is read-only on `g_predef_vars`; mutation stays with the
editor's drag handler (`g_drag_var` in `repl_editor.c`), satisfying
the "keep variable mutation outside the renderer" constraint.
`render_scene_status()` got its own section header in `ui_panels.c`
since the var-panel section that previously housed it is gone.

### 1c. Extract autocomplete popup → `ui_autocomplete_panel.c` ✅ DONE

Landed as `refactor: extract autocomplete popup renderer`. Pairs with
the existing model-only `repl_autocomplete.c`: the new module owns
*only* the floating popup render path; `repl_autocomplete.c` keeps
match building, selection state, ghost text, and parameter hints.
`ui_panels.c` 1399 → 1327 LoC. Inline ghost/hint text drawn next to
the input line stays in `ui_panels.c` (it needs the surrounding
code-panel row layout). Public entrypoint uses the newer module-
prefix convention: `ui_autocomplete_panel_render()` (replacing
the bare `render_autocomplete()`).

### 1d. Extract inline rename → `repl_inline_rename.c` ✅ DONE

Landed as `refactor: extract inline scene rename`. Rename state
(`g_rename_slot`, `g_rename_buf`, `g_rename_len`), the filesystem-
safe char filter, and the Enter/Esc/backspace/printable key handlers
moved out of `ui_panels.c` (1327 → 1237 LoC). The five public
entrypoints were renamed `ui_panels_*` → `repl_inline_rename_*` to
match the newer module-prefix convention (`_active`, `_begin`,
`_cancel`, `_handle_key`, `_handle_special`). Callers updated in
`repl_editor.c`, `repl_actions.c`, and two test files
(`test_repl_editor.c`, `test_repl_core_extra.c`). Rename has no
dedicated render pass - the buffer is surfaced through
`set_status()` into the existing status strip, so this is an input
module, not a panel; that matches the model/render split rule
(no OpenGL calls in the new file).

### 1e. Extract variable dragging → `repl_var_drag.c` ✅ DONE

Landed as `refactor: extract variable slider drag`. Closes the
MODULES.md Open Edge *"repl_editor.c still owns variable
slider dragging."* The four `g_drag_*` globals (storage) move to
the new file; the externs in `repl_state.h` and the `ReplUiState`
catalog entries in `repl_state.c` are unchanged, so the state-
access contract is preserved. Public API uses the newer prefix
convention: `repl_var_drag_begin(row, log_mode, x)`,
`repl_var_drag_motion(x)`, `repl_var_drag_reset()`,
`repl_var_drag_active()`, `repl_var_drag_active_var()`,
`repl_var_drag_log_mode()`. `repl_editor.c`'s mouse_func and
motion_func now call the API instead of touching state directly;
`ui_variable_panel.c` reads drag state through the accessors.
The value-writeback logic (linear/log mapping, `g_predef_vars`
write, matching `CMD_VAR_ASSIGN` source sync, `g_flat_dirty = 1`)
is now in one place instead of split across mouse-begin and
motion handlers.

### 2. Extract color picker → `ui_color_picker.c` ✅ DONE

Landed as `refactor: split code panel UI responsibilities`. The floating
HSV/alpha picker, literal color swatches, hit rectangles, and command
rewrite logic live in `ui_color_picker.c`; `ui_panels.c` only invokes
the public input/render bridge.

### 3. Extract menu/dropdown rendering → `ui_menu_bar.c` ✅ DONE

Landed as `refactor: split code panel UI responsibilities`. Top-level
menu/dropdown state, menu hit-testing, right-click config cycling, pinned
button hit-testing, the inline search slot, and example dropdown rendering
live in `ui_menu_bar.c`. Side effects remain in `repl_actions.c`.

---

## Tier 2 - Pattern consolidation (1–2 days, medium impact)

### 4. Data-drive grid + axes themes into `scene_grid.c` / `scene_axes.c` ✅ DONE

Implemented in the Phase-8 render cleanup slice. Standard grid themes now
flow through `GridThemeSpec` entries in `scene_grid.c` plus a shared
line/origin-axis renderer; focus, ocean, ruler extras, and adaptive planes
remain custom where they draw extra geometry. Axes now have `AxesThemeSpec`
entries and shared triplet helpers in `scene_axes.c` for X/Y/Z lines, tips,
and labels, so adding a normal theme starts with table data instead of a
copied switch body.

The Focus grid no longer chooses or writes its focus vertex while drawing.
`FrameRenderContext` prepares the sticky focus vertex before helper rendering,
so the theme renderer consumes prepared state like the standard table-driven
themes. The Ocean grid also consumes prepared camera waterline state; the old
inline camera-height TODO was replaced with a single frame-prep derivation of
the orbit camera's world-space Y.

### 5. Vertex overlay visitor → `scene_overlays.c` ✅ DONE

Implemented in the Phase-8 render cleanup slices. `walk_vertex_overlay()`
centralizes transform replay, begin/end/tessellation block tracking, current
normal state, and cursor-selected block filtering. Ownership now lives in
`scene_overlays.c` alongside polygon outlines, with public entrypoints for
vertex-number and normal-vector rendering.

### 5b. Guard scene helper GL state ✅ DONE

Implemented in the Phase-8 render cleanup slice. Scene helpers that toggle
blend, lighting, depth, fog, line stipple, line width, point size, or viewport
state now wrap their work in the local `scene_render_push_state()` /
`scene_render_pop_state()` convention. The full frame still has its
top-level `glPushAttrib(GL_ALL_ATTRIB_BITS)`, but helper guards keep
side effects contained at the source.

### 5c. Extract backdrop rendering → `scene_backdrop.c` ✅ DONE

Implemented in the Phase-8 render cleanup slice. The backdrop mode dispatcher,
deterministic city hash, day/night window timing, and cityscape geometry moved
out of `scene_render.c` into `scene_backdrop.c`. The frame still invokes the
backdrop at the same helper-pass point, but backdrop-specific GL state is now
guarded inside the backdrop module.

### 5d. Extract light setup/indicators → `scene_lights.c` ✅ DONE

Implemented in the Phase-8 render cleanup slice. Light property reset/setup and
the visible light indicator overlay moved out of `scene_render.c`. The normal
fill and replay fade passes now call `scene_lights_setup()`, and the final HUD
pass calls `scene_lights_render()`, preserving the old render order while
making light side effects explicit.

### 5e. Extract polygon outline overlay → `scene_overlays.c` ✅ DONE

Implemented in the Phase-8 render cleanup slice. The polygon outline/current-
block highlight pass moved out of `render_3d_scene()` into
`scene_overlays_render_outlines()`. The flat-block cursor matcher is now an
explicit `scene_overlay_flat_block_matches_cursor()` helper shared by outline,
vertex-number, and normal-vector overlay paths inside `scene_overlays.c`.

### 5f. Widen render config/frame context ✅ DONE

Implemented in the Phase-8 residual render cleanup slice. `scene_render.c`
now builds a private `SceneRenderConfig` at the start of `render_3d_scene()`
for scene rectangle, camera, accumulation jitter, quality toggles, grid/axes
choices, guide/vertex overlay toggles, replay mode, replay tess preview, and
replay fill-base limit. `FrameRenderContext` carries that config plus prepared
derived state such as Focus-grid vertex and ocean-grid waterline facts. The
projection/camera setup, grid renderer in `scene_grid.c`, axes renderer in
`scene_axes.c`, orbit target, replay outlines, replay fade pass, guide
drawing, vertex-point overlay, and replay HUD now consume the explicit config
instead of independently sampling those globals.

### 5g. Extract scene-edit guides into snapshot-driven modules ✅ DONE

Implemented in the Phase-8 residual render cleanup slice. Scene-edit guides no
longer read globals directly while drawing. `scene_render.c` now builds one
`SceneGuideSnapshot` per frame/per-input state, then delegates:

- vertex-input and normal-edit guides to `scene_geometry_guides.c`
- transform guide planning/rendering to `scene_transform_guides.c`

`scene_transform_guides_prepare()` keeps the existing committed+unmodified gate,
flat source-index matching, and `after_flat_idx` fallback behavior; rendering
still uses live flattened args and one-shot consumption in the vertex-dots pass.

### 6. Workspace header read/write symmetry in `repl_export.c` ✅ DONE

Landed as `refactor: drive workspace header I/O from a directive table`.
`parse_workspace_header_line()` and `refresh_workspace_header_lines()` now
share one `WorkspaceDirective` table for `@scene-name`, `@workspace-dir`,
`@var`, and `@cfg`, so a directive's parser and emitter stay paired.

### 6b. Ordered importer handlers in `repl_export.c` ✅ DONE

Landed as `refactor: split load_from_file into ordered import handlers`.
`load_from_file()` now drives an `ImportState` through focused
`import_try_*` handlers. Dispatch order encodes the original priority:
camera/workspace/function/snippet/comment handling before snippet body
handling, then snippet-end/blank/predef-decl/feed-line inside the snippet.
Deferred `@var` reapply and the final status message remain in the driver.

### 6c. Visual dump wraps through `repl_code_panel_layout.c` ✅ ALREADY DONE

`repl_dump_code_panel_visual_text()` already calls `CodePanelWrapIter` via
`dump_code_panel_wrapped_line()`, and `test_repl_core_format.c` covers visual
dump wrapping, continuation rows, overflow commas, point-parameter wrapping,
and bottom-layout panel width. There is no separate Phase-9 extraction needed
for this unless a narrower parity bug is found.

### 6d. Typed export scaffold/pass model in `repl_export.c` ✅ DONE

`save_output()` now collects export prerequisites in `ExportScaffoldContext`
and emits the generated file through `ExportScaffoldSectionSpec` entries:
workspace metadata, header, globals, optional rand/tess helpers, variable
reset, user functions, render helper, and `display()`. The `display()` body
then uses display begin/geometry/tail helpers and `ExportDisplayPassSpec`
entries for fill/outline/vertex-point passes. The generated C text and pass
order stay unchanged, but top-level scaffold, conditional helpers, and render
passes are now explicit enough for future export changes to review locally.

---

## Tier 3 - Remaining structural extraction

The editor-adjacent Phase 7 mechanical extractions are now complete.
`MODULES.md` (Open Edges) still lists smaller residual ownership edges in
`ui_panels.c` and `repl_editor.c`, but they are mostly routing or inline
row-rendering concerns. Phase 9 now has its main scaffold/import slices in
place. The next large mechanical extraction, parser ownership, is now done.

### 8. Extract `parse_command()` → `repl_parser.c` - done

- `repl_parser.c` now owns the parser grammar and the public
  `repl_parse_command*()` entrypoints.
- `ReplParseContext` now carries the source-line index and visible locals for
  parser calls that need source-sensitive indentation.
- `repl_source_scope.c` owns the source-command prefix-depth cache, indentation
  helpers, `find_block_end()`, and nearest-open-block queries.
- `repl_core.c` keeps normalization, display, and GL init.
- `Makefile`, `MODULES.md`, `ARCHITECTURE.md`, and the local agent docs now
  list parser ownership explicitly.
- **Verify:** `make test_repl_core_parse`, `make test_repl_core_commit`,
  `make test_eval`, `make test-stubs TEST_JOBS=4`, `make sample
  USE_GL_STUBS=1`, and `make sample`.

### 9. Finish Phase 5 flatten API ✅ DONE

`repl_flatten.c` now exposes `repl_flatten_program()` with explicit source
input, destination flat buffer, local-var snapshot buffer, capacity, recursion
depth, visit budget, and result/status output. `repl_flatten_commands()` is
now the compatibility wrapper that passes the live `g_cmds[]` / `g_flat_cmds[]`
arrays, installs the returned flat count, updates lighting state, and refreshes
cursor highlighting. Focused internal coverage proves a temporary flat
destination can be built without changing the live `g_flat_cmds[]` /
`g_num_flat_cmds` arrays, including the capacity-failure path.

### 10. Audit command-store escape hatches ✅ DONE

Array-level source-command restores now go through
`repl_command_store_load()`, which centralizes command-buffer copying,
`g_num_cmds` replacement, edit-line clamping, and depth-cache invalidation.
Converted paths include undo/redo snapshot restore, scene switching,
workspace save/load stashing, workspace file load reset, built-in example
load reset, and full REPL reset. Focused internal coverage exercises successful
bulk load, rejected invalid loads, edit-line clamping, empty loads, and
depth-cache invalidation.

Remaining direct `g_cmds[]` writes are not command-array escape hatches:
`ui_color_picker.c` and `repl_var_drag.c` rewrite literal values inside an
existing command, and declaration commit paths repair assignment variable-slot
indices after predef declarations change. Those should only move if we add a
future per-command semantic mutation API.

### 11. Segregate live GL calls to scene/UI modules plus `repl_executor.c`

**Goal:** the only `.c` files that issue live GL/GLU calls should be `scene_*.c`,
`ui_*.c`, and `repl_executor.c`. `repl_*.c` files (other than the executor) and
`sample.h` should stop calling GL directly. GLUT *input/feedback* APIs
(`glutPostRedisplay`, `glutSetCursor`, `glutGetModifiers`, `glutSwapBuffers`)
are framework plumbing, not drawing — they are allowed to remain in `sample.c`
and the editor input router, but should be funnelled through one or two
named wrappers so the rule is mechanically checkable.

**Out of scope** (text emission, not live GL):

- `repl_export.c`, `repl_examples.c`, `repl_replay_annotations.c`,
  `repl_command_spec.c`, `repl_parser.c` — these only handle GL command
  strings as REPL source. Grep hits inside string literals don't count.

**Current live-call residue outside the allowed set** (verified by
`grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*\s*\('` minus comment/string lines):

| File | Calls | Nature |
|------|-------|--------|
| `repl_core.c` | ~34 | `draw_string`/`draw_quad`/`begin_2d`/`end_2d` 2D helpers, `display_func()` viewport+clear+accum-AA loop+`glutSwapBuffers`, `init_gl()` `glLightModelfv` |
| `repl_state.c` | ~19 | 5 tess callbacks (`glBegin`/`glEnd`/`glNormal3dv`/`glColor4dv`/`glVertex3dv`) + `repl_state_render_init_resources`/`_destroy_resources` (`gluNewQuadric`, `gluQuadric*`, `gluNewTess`, `gluTessCallback`, `gluDelete*`) |
| `repl_replay.c` | 9 | `execute_replay_fade_batches()` push/pop attrib, lighting/blend setup, `glColor4f`, `glPushMatrix`/`glPopMatrix` around the executor call |
| `sample.h` | 9 | inline `apply_transform_cmd`, `apply_tracked_transform_cmd`, `unwind_tracked_transform_stack`, `_repl_point_size` (`NO_POINT_PARAMETER` shim) |
| `repl_actions.c` | 1 | `glutGetModifiers()` SHIFT check on the time-toggle config row |
| `repl_editor.c` | ~23 | All `glutPostRedisplay`/`glutSetCursor`/`glutGetModifiers` — GLUT input/feedback, not GL drawing |

The work below lands as a sequence of small `refactor:` commits, one
sub-step per commit. After each commit, `make test-stubs TEST_JOBS=4`
must still pass and `make sample` must still launch.

The sub-steps are organised in two phases:

- **Phase 1 — precursor cleanup** (11.0). Two structural prerequisites
  that aren't strictly GL-isolation work, but that the GL-isolation
  slices in Phase 2 will trip over if they aren't addressed first:
  extracting the prof primitives out of `ui_profile_panel.*` into
  their own module, and a focused naming pass on the files about to be
  edited.
- **Phase 2 — GL isolation** (11a-11f). The mechanical moves that
  segregate live GL calls.

---

#### 11.0 Phase 1 — precursor cleanup

These two slices unblock the GL-isolation work and should land first.
Each is one `refactor:` commit.

##### 11.0a. Extract prof primitives into a `prof` module

**Problem:** `prof_begin`, `prof_end`, `prof_accum_reset`,
`prof_accum_end`, `prof_accum_commit`, `prof_frame_tick`,
`prof_code_panel_details_enabled`, and the `ProfSection` enum live
in `ui_profile_panel.h` and `ui_profile_panel.c`. They are called
from `repl_core.c`, `repl_replay.c`, every `scene_*.c` that times
sub-passes, and the code-panel renderers — i.e. every layer in the
project transitively `#include`s a UI header to instrument itself.
That violates the layering rule before the GL-isolation work even
starts. Profiling primitives are generic instrumentation, not UI.

**Steps:**

1. Create `prof.h` and `prof.c`. Move the following out of
   `ui_profile_panel.{h,c}`:
   - the `ProfSection` enum (`ui_profile_panel.h:13-46`),
   - the `PROF_SECTION_COUNT` sentinel,
   - `prof_begin`, `prof_end`,
   - `prof_accum_reset`, `prof_accum_end`, `prof_accum_commit`,
   - `prof_frame_tick`,
   - `prof_code_panel_details_enabled`,
   - any internal `static` storage (per-section accumulators,
     frame-tick counters) — keep file-static in `prof.c`.
2. `ui_profile_panel.{h,c}` now own only the *rendering* of the prof
   HUD: `render_profile_panel()` plus its layout helpers. The HUD
   reads measured timings via a small read-only API exported from
   `prof.h` (e.g. `prof_section_avg_ms(ProfSection)` /
   `prof_section_last_ms(ProfSection)`). Define just enough getters
   to cover what the HUD currently displays — no speculative API.
3. Update every caller's include: `repl_core.c`, `repl_replay.c`,
   each `scene_*.c` using prof brackets, `ui_panels.c`,
   `ui_*` overlay renderers — replace
   `#include "ui_profile_panel.h"` with `#include "prof.h"`. The HUD
   call site (`render_profile_panel()` in `repl_core.c:690`) keeps
   `#include "ui_profile_panel.h"` because that's the renderer.
4. Add `prof.{h,c}` to the Makefile object lists (sample,
   test-stubs, bench).
5. Update `MODULES.md`: add `prof` to the layer table — it's a
   project-wide instrumentation module that sits beside the
   layered groups (similar to `gl_stub_counts`). Update
   `ARCHITECTURE.md` ownership list with a `prof.c` bullet.

**Why this is a Phase 1 prerequisite:** once 11f's
`make check-layer-coupling` grep guard lands, every
`scene_*.c → ui_profile_panel.h` include shows up as a violation.
Extracting prof first means the guard can be unconditional rather
than carrying a per-file exception list for each prof-bracketed
helper.

**Verify:** `make test-stubs TEST_JOBS=4` (every TU using prof
brackets must still link); `make sample && ./sample`; toggle the
profile HUD and confirm timings still populate.

##### 11.0b. Naming pass on the files about to be edited

**Decision:** the broader naming/comment pass described in
[`feature/repl-cleanup.md` §10](./repl-cleanup.md) ("Final
naming/comment pass") is a Phase 10 concern in the strategic plan.
Doing the *full* pass before Phase 2 here would inflate the slice.
But each Phase 2 sub-step (11a-11f) edits a small set of files, and
landing those moves on top of inconsistent local names produces
diffs that are harder than necessary to review.

**Approach:** before each Phase 2 sub-step, run a focused naming
pass on **only the files that sub-step touches**, following the
conventions from `feature/repl-cleanup.md` §10:

- `*_idx` for indexes, `*_count` for counts (e.g. rename `i`,
  `n`, `cnt` style locals when the rename clarifies intent at
  function-call sites).
- `source_line_idx`, `flat_cmd_idx`, `indent_cols`,
  `visible_line_count`, `command_store`, `render_config`,
  `workspace_dir` are the canonical names — adopt them where
  encountered.
- Remove stale comments and vague TODOs in the touched files.
  Real defects become tracked items; speculative comments are
  deleted.
- Do **not** rename across files outside the sub-step's edit scope.
  The full project-wide naming sweep stays at Phase 10.

**Why this is a Phase 1 prerequisite:** the GL-isolation moves in
Phase 2 will rewrite call sites and signatures of helpers like
`apply_transform_cmd`, `_repl_point_size`, `begin_2d`,
`execute_replay_fade_batches`. Doing the rename in the same commit
muddies the diff; doing it after means re-touching the same lines
twice. A pre-slice rename pass on the touched files is the cheapest
ordering.

**Steps for each Phase 2 sub-step (executed inline before the move
itself, but split into a separate `refactor:` commit):**

1. List the files the sub-step will edit (grep for callers of the
   functions being moved).
2. Apply the §10 naming conventions to those files only.
3. Run `make test-stubs TEST_JOBS=4` — must stay green.
4. Commit as `refactor: normalize names in <file list> for §11<x>
   prep`.
5. Land the GL-isolation move in the next commit.

**Verify per slice:** `make test-stubs TEST_JOBS=4` after the
naming commit and again after the move commit; the rename commit
should be a pure rename (no logic changes), and the move commit
should be a pure move (no rename creep).

---

#### Phase 2 — GL-isolation moves

11a through 11f below are the GL-isolation sub-steps that depend on
Phase 1.

#### 11a. Extract generic 2D helpers into a project-agnostic header library; hoist accumulation-AA into `render_3d_scene()`; keep `repl_core.c` as orchestrator

**Layering check (verified):**

- No `ui_*.c` includes any `scene_*.h`.
- The only `scene_*` → `ui_*` coupling is `scene_render.c` reading
  `scene_rect()` and `ui_profile_panel.h` for layout coordinates — a
  read-only query, not render dispatch.
- `repl_core.c`'s `display_func()` is the single master that calls
  `render_3d_scene()` then the 2D `render_*` helpers in order; neither
  layer reaches sideways into the other.

The 2D helpers (`begin_2d`/`end_2d`/`draw_quad`/`draw_string`) are
*generic fixed-function GL utilities* — they don't carry any REPL
domain knowledge once `end_2d`'s lighting query is replaced with a
plain `glPushAttrib`/`glPopAttrib` bracket. The right answer is
neither "duplicate per layer" nor "move into one layer and force a
dependency"; it's **extract into a project-agnostic header-only
library under `include/`** alongside `gl_includes.h`, `stb_image.h`,
and `utils.h`. The project's CLAUDE.md explicitly endorses extending
that library set ("Extending these libraries is allowed").

Both `scene_*` and `ui_*` then `#include <gl_2d.h>` from the shared
include path. There's no cross-layer coupling because the include
target is a third-party-style utility, not a sibling layer's header.
Pure-model `repl_*` files don't include it (they don't draw), which
preserves the GL-isolation rule.

**Steps:**

1. **Create `include/gl_2d.h`.** Header-only, `static inline`, no
   project state. Public API:

   ```c
   #ifndef GL_2D_H
   #define GL_2D_H
   #include <gl_includes.h>

   /* Push a 2D ortho projection sized to (0,0)-(w,h) and disable depth +
    * lighting via glPushAttrib so the corresponding gl2d_end() restores
    * prior state without project-side lighting queries. */
   static inline void gl2d_begin(int w, int h) {
       glMatrixMode(GL_PROJECTION);
       glPushMatrix();
       glLoadIdentity();
       gluOrtho2D(0, w, 0, h);
       glMatrixMode(GL_MODELVIEW);
       glPushMatrix();
       glLoadIdentity();
       glPushAttrib(GL_DEPTH_BUFFER_BIT | GL_LIGHTING_BIT);
       glDisable(GL_DEPTH_TEST);
       glDisable(GL_LIGHTING);
   }

   static inline void gl2d_end(void) {
       glPopAttrib();
       glMatrixMode(GL_PROJECTION);
       glPopMatrix();
       glMatrixMode(GL_MODELVIEW);
       glPopMatrix();
   }

   /* Render a NUL-terminated string at (x,y) in the current GL color
    * using the given GLUT bitmap font. */
   static inline void gl2d_draw_string(float x, float y, const char *s,
                                       void *font) {
       glRasterPos2f(x, y);
       for (; *s; s++) glutBitmapCharacter(font, (unsigned char)*s);
   }

   #endif /* GL_2D_H */
   ```

   The `end_2d` lighting-restore is now a `glPushAttrib`/`glPopAttrib`
   pair. That removes the only project-state coupling
   (`repl_state_flat_program_user_lighting_enabled()`) and is
   behaviorally a strict improvement: the GL attribute stack restores
   *exact* prior state regardless of how lighting was configured,
   instead of restoring a binary on/off flag.

2. **Migrate callers to the new API.** Mechanical rename:
   - `begin_2d()` → `gl2d_begin(*repl_state_viewport()->window_w, *repl_state_viewport()->window_h)`.
     Each caller passes the viewport size explicitly. ~10 sites across
     `ui_panels.c`, `ui_menu_bar.c`, `ui_color_picker.c`,
     `ui_help_overlay.c`, `ui_variable_panel.c`,
     `ui_autocomplete_panel.c`, `ui_profile_panel.c`, `scene_render.c`,
     `scene_grid.c`.
   - `end_2d()` → `gl2d_end()`. Same caller set.
   - `draw_string(x, y, s, font)` → `gl2d_draw_string(x, y, s, font)`.
     Same caller set.
   - Add `#include <gl_2d.h>` to each caller; drop the
     `sample.h:354,356,357` forward declarations of the old names.
   - Delete the old definitions from `repl_core.c:549-584`.

3. **Replace `draw_quad` with `glRectf` directly at every call site.**
   No need to put it in `gl_2d.h` — `glRectf(x, y, x+w, y+h)` is
   already a built-in fixed-function primitive that does exactly the
   same work as the old helper. Mechanical replace at ~50 sites in
   `scene_render.c`, `ui_color_picker.c`, `ui_help_overlay.c`,
   `ui_profile_panel.c`, `ui_variable_panel.c`,
   `ui_autocomplete_panel.c`, `ui_panels.c`, `ui_menu_bar.c`:
   `draw_quad(x,y,w,h)` → `glRectf((float)(x), (float)(y), (float)(x)+(float)(w), (float)(y)+(float)(h))`.
   Delete `draw_quad` from `repl_core.c:555-561` and the forward decl
   in `sample.h:355`. Confirm `include/GL/gl.h` (the local stub)
   declares `glRectf`; if not, add a no-op stub line.

4. **Stub-header parity.** Confirm the new `gl_2d.h` symbols are all
   already covered by the local stubs:
   `glPushAttrib`/`glPopAttrib`/`GL_DEPTH_BUFFER_BIT`/`GL_LIGHTING_BIT`,
   `glRasterPos2f`, `glutBitmapCharacter`, `gluOrtho2D`,
   `glMatrixMode`/`glPushMatrix`/`glPopMatrix`/`glLoadIdentity`,
   `glDisable(GL_DEPTH_TEST/GL_LIGHTING)`. All are heavily used
   already; no stub additions expected. `gl_2d.h`'s `#include
   <gl_includes.h>` lets `USE_GL_STUBS=1` builds pick up the local
   stub headers via the same include path.

5. **Hoist accumulation-AA into `render_3d_scene()`.** `display_func()`
   in `repl_core.c:644-670` currently owns the per-sample
   `glClear`/`glAccum(GL_ACCUM)` jitter loop and calls
   `render_3d_scene()` once per sample. That's a render-internal concern
   leaking into the orchestrator. Push it inside:

   - Move the `if (use_accum && accum_aa_enabled && accum_samples > 1)`
     branch into `render_3d_scene()` (or a dedicated
     `render_3d_scene_accum()` it dispatches to).
   - `render_3d_scene()` becomes responsible for: clearing color/depth
     (and accum if AA is on), running the inner pass(es), writing
     `accum_jitter_x/y` into `ReplRenderState`, and final
     `glAccum(GL_RETURN)` if AA was used.
   - The replay-baseline restore call
     (`replay_restore_baseline_predef_values()`) currently sits inside the
     loop at the same nesting — keep it inside the inner per-sample step
     so behavior is unchanged.
   - The PROF section bracketing (`PROF_SCENE_3D` plus the
     `prof_accum_reset`/`prof_accum_commit` for `PROF_SCENE_3D_SETUP..HUD`)
     is render-internal too — move the reset/commit pair inside
     `render_3d_scene()` along with the loop. Keep `PROF_SCENE_3D`
     begin/end in `display_func()` if you want one frame-level entry, or
     move it inside too — pick whichever keeps the existing flame chart
     readable (decision: keep frame-level entry in `display_func()`,
     see decision #5 below).
   - After the move, `display_func()` calls `render_3d_scene()` once,
     period. No knowledge of accumulation.

6. **Trim `display_func()` to pure orchestration.** What remains in
   `repl_core.c` after step 5:

   - autonormal/flatten dirty-bit handling (model bookkeeping — stays),
   - replay PC clamp via `repl_state_flat_program_set_count(...)` (model —
     stays),
   - `update_render_state_strings()` / `update_cam_lines()` (string
     scaffold — stays),
   - per-frame predef-value snapshot/restore (model — stays),
   - one call to `render_3d_scene()`,
   - the 2D overlay sequence (`render_code_panel`,
     `ui_autocomplete_panel_render`, `render_example_dropdown`,
     `render_var_panel`, `render_scene_status`, `render_help`,
     `render_profile_panel`),
   - `glutSwapBuffers()` and the bracket `glViewport` calls.

   Move the two `glViewport(0, 0, window_w, window_h)` calls
   (`repl_core.c:624,677`) and `glutSwapBuffers()` (`:697`) out:

   - The 3D-side `glViewport` at `:624` becomes the first line of
     `render_3d_scene()`.
   - The 2D-side `glViewport` at `:677` becomes the first line of
     `render_code_panel()`. This mirrors `render_3d_scene()` setting
     its own viewport, so each renderer states explicitly what
     viewport it draws into. Subsequent 2D overlays (autocomplete,
     dropdown, var panel, help, profile) inherit that viewport,
     which matches today's behavior. Document the convention in
     `render_code_panel()`'s top-of-function comment.
   - `glutSwapBuffers()` is GLUT framework plumbing, not GL drawing.
     Move to `sample.c`'s `display_func()` GLUT wrapper, right after
     `repl_display_func()` returns. That keeps the buffer swap at the
     GLUT-callback boundary, which is its natural home.

7. **Move `init_gl`'s `glLightModelfv` into `scene_lights.c`.** Add
   `scene_lights_init_global_ambient()` (one-shot, called from
   `repl_init_gl()`); the existing per-frame `scene_lights_setup()` keeps
   its current responsibility. `repl_init_gl()` then has zero GL calls —
   it just calls `repl_executor_init_resources()` (from 11b) and
   `scene_lights_init_global_ambient()`, plus `apply_init_bootstrap()`.

8. **Verification grep.** After 11a lands:
   `grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*\s*\(' repl_core.c`
   should be empty. The orchestration role of `repl_core.c`'s
   `display_func()` is preserved — it still drives the
   `render_3d_scene()` → 2D-overlay-sequence pipeline — but no GL call
   originates there.

9. **Update docs.** `ARCHITECTURE.md` `repl_core.c` ownership: drop the
   "GL init" and "display callback owns frame GL state" implications;
   add to `scene_render.c`: "owns 3D viewport, clear, accumulation-AA
   loop, and per-frame `glViewport`"; add to `ui_panels.c`: "owns the
   2D overlay viewport bracket"; add to `sample.c`: "owns the GLUT
   buffer swap." Document the new `include/gl_2d.h` utility in
   `MODULES.md` Layer 6 (or in a "Project-agnostic libraries"
   sub-section under "Shared State Rules") and in CLAUDE.md's
   "Shared Libraries" list. `MODULES.md` Layer 1/4/5 tables update in
   lockstep.

**Verify:** `make test-stubs TEST_JOBS=4`; `make sample && ./sample`,
load examples 0-15, F12 cycle, replay a fade-heavy example with
accumulation AA on (default) and off (`--noaccum`) — the AA jitter
loop is the highest-risk piece. Confirm `glutSwapBuffers` still runs
exactly once per frame (a quick `printf` instrumented in
`sample.c`'s wrapper, removed before commit, is the easiest check).
After the move, A/B against the pre-refactor binary on the replay HUD
and ruler-grid labels — those are the two `scene_*` callers of
`begin_2d`/`draw_string` and the ones most likely to drift.

#### 11b. Move tessellation/GLU resources into `repl_executor.c`

**Layering check:** `gluSphere`, `gluCylinder`, `gluDisk`, `gluPartialDisk`,
`gluTessBeginPolygon`, `gluTessVertex`, etc. are all called from
`repl_executor.c` — the executor is the *only* consumer of `g_quadric` /
`g_tess`. No `scene_*` or `ui_*` module reads these resources. Putting them
in a `scene_resources.c` would force `repl_executor.c` to include
`scene_*.h`, which inverts the layering (executor is the pipeline layer
*below* scene rendering — scene calls executor, not the other way around).

The right home is `repl_executor.c` itself: it's the sole caller of GLU
drawing APIs, so it should own the GLU resources too.

Steps:

1. Move from `repl_state.c` into `repl_executor.c`:
   - the static `g_quadric`, `g_tess`, `g_tess_verts[]`,
     `g_tess_vert_count` storage (`repl_state.c` definitions and the
     externs in `repl_state.h`),
   - the 5 tess callbacks `repl_render_tess_vtx_begin_cb`,
     `repl_render_tess_vtx_end_cb`, `repl_render_tess_vtx_cb`,
     `repl_render_tess_comb_cb`, `repl_render_tess_err_cb`
     (`repl_state.c:389-450ish`),
   - `repl_state_render_init_resources()` and
     `repl_state_render_destroy_resources()` (`repl_state.c:1066-1106`).
2. Rename the public entrypoints to match the executor module
   (`repl_executor_init_resources()` /
   `repl_executor_destroy_resources()`) in `repl_executor.h`. Update the
   two callers: `repl_init_gl()` in `repl_core.c` (now calls
   `repl_executor_init_resources()`) and the destroy site in `sample.c`.
3. Internal access to `g_quadric` / `g_tess` is now file-static in
   `repl_executor.c` — no header export needed. If anything outside the
   executor ever needs them in future, expose typed getters rather than
   re-introducing externs.
4. After the move, `repl_state.c` should have **zero** live GL/GLU calls.
   The string-literal hits at `repl_state.c:178-186` (the
   `g_render_state_lines` / camera scaffold strings) are not live calls
   and stay where they are.
5. `repl_state.h` loses the `g_quadric` / `g_tess` / `g_tess_verts` /
   `g_tess_vert_count` externs and the `TessVertex` typedef if no other
   module references it (grep first — `TessVertex` may be referenced in
   tests).
6. Stubs in `include/GL/glu.h` are unchanged — the GLU symbols already
   linked in `repl_state.c`'s build now link in `repl_executor.c`'s
   instead. The `USE_GL_STUBS=1` build needs no stub additions, just
   the moved object file.

**Verify:** `make test-stubs TEST_JOBS=4` (the tess callbacks must still
link in stub mode); `make sample && ./sample`; load an example that
exercises tessellation (the polygon-with-holes tess examples) and one
that exercises quadrics (`gluSphere`/`gluCylinder` calls); confirm exit
cleanly via window close so `repl_executor_destroy_resources()` fires.

#### 11c. Replace inline GL helpers in `sample.h` with `repl_executor.h` exports

`sample.h` is supposed to be the shared *types* and compatibility header.
Inline helpers that expand to GL calls in every translation unit pull GL
symbols into every `.c` that includes `sample.h`, defeating the layering.

**Layering check:** `apply_transform_cmd` and `apply_tracked_transform_cmd`
are called by `repl_executor.c` (dispatch) and by two `scene_*` modules
(`scene_transform_guides.c`'s `compute_before_cursor_matrix`,
`scene_overlays.c`'s flat-cmd walk). `scene_*` already includes
`repl_executor.h` (because `scene_render.c` calls `repl_execute_program`),
so the dependency direction is **scene → executor** — pipeline-below-scene,
which matches the layering. No new coupling is introduced by exporting
these from `repl_executor.h`. There is no `ui_*` consumer.

Steps:

1. Keep `is_transform_cmd` where it is — it's a pure command-classifier
   with no GL, so it can stay in `sample.h` or move to `repl_executor.h`
   if you prefer. No behavior risk.
2. Move `apply_transform_cmd`, `apply_tracked_transform_cmd`, and
   `unwind_tracked_transform_stack` (`sample.h:296-333`) into
   `repl_executor.c` and export from `repl_executor.h`. Convert from
   `static inline` to ordinary functions. Call sites:
   - `repl_executor.c`'s own dispatch — was inline-via-include, becomes a
     direct call within the same TU.
   - `scene_transform_guides.c` `compute_before_cursor_matrix` — already
     includes `repl_executor.h` transitively; no new coupling.
   - `scene_overlays.c` flat-walk — same.
3. Replace `_repl_point_size` (`sample.h:386-395`) with a static helper
   inside `repl_executor.c` and scope the `glPointSize` macro override to
   that TU only:
   - Move the `#ifdef NO_POINT_PARAMETER` block out of `sample.h` and
     into `repl_executor.c`. The override is for the `case CMD_POINT_SIZE:`
     dispatch path only — that's the only *user-facing* `glPointSize`
     issued by the REPL language.
   - All other `glPointSize` call sites in `scene_*` modules
     (`scene_geometry_guides.c`, `scene_overlays.c`, `scene_axes.c`,
     `scene_lights.c`) are render-internal helpers that already chose
     their own size and don't want the attenuation hack — confirmed by
     the comment at `sample.h:386-395`. Scoping the shim to the executor
     is a behavior-preserving win.
4. Drop `#include <gl_includes.h>` from `sample.h` if it survives only to
   make these inline GL helpers compile. Keep it if `GLfloat`/`GLenum`
   still appear in struct types (`GLCmd`). Verify by trial removal +
   `make sample USE_GL_STUBS=1` link.
5. `sample.h`'s GL-call grep should hit zero after the move.

**Verify:** `make test-stubs TEST_JOBS=4`; build all three flag
combinations: `make sample`, `make sample USE_GL_STUBS=1`, and
`make sample NO_POINT_PARAMETER=1` (the punch-list intersection). Run
`./bench_repl fade_batches` — it exercises tracked transforms via the
executor and the scene_* walkers.

#### 11d. Move replay-fade GL pass into `scene_render.c`

**Layering check:** the fade pass is pure 3D scene rendering — no `ui_*`
involvement. `scene_render.c` already calls `execute_replay_fade_batches()`
from inside its frame, and the layering rule (master `repl_*` calls down
into `scene_*`/`ui_*` renderers) is preserved by having `repl_replay.c`
expose model data that `scene_render.c` reads. Direction stays:
**scene → replay model** (read-only, fade-batch ring + alpha math).

Steps:

1. Split `execute_replay_fade_batches()` into two halves:
   - **Model side** (stays in `repl_replay.c`): the `skip_limits[]`
     computation loop at lines 735-771 — pure data, no GL. Expose as
     `replay_compute_fade_skip_limits(int *out_limits, int max_count)`
     returning the actual count. Also keep `replay_batch_alpha()`,
     `repl_execute_set_fade_context()`, and `replay_has_active_fades()`
     where they are.
   - **Render side** (moves to `scene_render.c` as a new static
     `render_replay_fade_pass()` adjacent to the existing fade-pass
     setup at `scene_render.c:512-530`): the `glPushAttrib` / per-batch
     `glDisable(GL_LIGHTING)` / `glEnable(GL_BLEND)` / `glBlendFunc` /
     `glPolygonMode` / `glColor4f` / `glPushMatrix` / `glPopMatrix` /
     `glPopAttrib` block plus the per-batch
     `repl_execute_program(&exec_options)` call.
2. The existing fade-pass setup in `scene_render.c:512-530` already
   primes lighting, blend, and polygon mode, then immediately calls
   `execute_replay_fade_batches()`. After the move, both blocks live in
   one place — collapse the duplicated setup so we only set blend/
   polygon mode once around the loop, not once per batch. This is the
   payoff for moving the code.
3. Delete the now-empty `execute_replay_fade_batches()` wrapper from
   `sample.h:375` and `repl_replay.h:29`. Callers:
   - `scene_render.c` calls the new static `render_replay_fade_pass()`
     directly — no public API needed.
   - `bench_repl.c:549` calls `execute_replay_fade_batches()` for its
     GL-call count harness. The bench is a *test harness* that needs to
     drive the GL pass without `scene_render.c`'s frame setup. Two
     options:
     - **Preferred:** move the bench's fade-batch driver to call the new
       `render_replay_fade_pass()` directly — exposed via
       `scene_render.h` solely for the bench. Keep the function `static`
       within `scene_render.c` for normal builds, expose only behind
       `#ifdef BENCH_REPL_HARNESS` (or via a separate
       `scene_render_test.h` linked only by the bench). This preserves
       the master/slave rule for normal builds.
     - **Fallback:** keep a thin `execute_replay_fade_batches()` in
       `repl_replay.c` that delegates to the new
       `render_replay_fade_pass()` — but that re-introduces the upward
       call (replay → scene render). Avoid unless the bench-only
       exposure proves intractable.
   - The bench's stdout strings at `bench_repl.c:531,564` reference
     `execute_replay_fade_batches` — update them to name the new
     entrypoint so log readers can find the symbol.
4. After the move, `repl_replay.c` should have **zero** live GL calls.
   `repl_replay.h` should drop any `gl_includes.h` include if no
   remaining declaration needs it.

**Verify:** `make test-stubs TEST_JOBS=4`; the bench harness
`./bench_repl fade_batches` GL-call count should be **strictly less
than or equal to** the pre-refactor number (the per-batch redundant
`glDisable(GL_LIGHTING)`/`glEnable(GL_BLEND)`/`glBlendFunc`/
`glPolygonMode` calls collapse to one). Capture the before/after counts
in the commit message. `make sample && ./sample`: load an example with
heavy geometry, Ctrl+G to start replay, hold Right arrow — the trailing
ghost should look identical.

#### 11e. Funnel `repl_editor.c` and `repl_actions.c` GLUT calls through helpers

**Layering note:** GLUT input/feedback (post-redisplay, cursor, modifiers)
is editor/input layer, not rendering. No `scene_*`/`ui_*` coupling
involved. This step is unchanged by the layering rule — included here
for completeness.

Steps:

1. Add two helpers to `repl_editor.c` (private — just `static`):
   - `static void editor_request_redraw(void)` wrapping
     `glutPostRedisplay()`.
   - `static void editor_set_cursor(int cursor)` wrapping
     `glutSetCursor()`.
   Replace all 21 `glutPostRedisplay()` / `glutSetCursor(...)` call sites
   inside `repl_editor.c` with the helpers. There is no cross-module API
   change; this is purely a single-funnel refactor.
2. The two `glutGetModifiers()` queries — one in `repl_editor.c:75`, one
   in `repl_actions.c:169` — are GLUT inputs, not drawing. Move them
   behind a single helper `repl_editor_active_modifiers()` exported from
   `repl_editor.h`. `repl_actions.c` calls the helper instead of
   including GLUT headers directly. After this, `repl_actions.c` has
   zero `gl[uU][A-Z]` matches.
3. Update the "Naming Notes" exception list in `MODULES.md:359-365` to
   mention that `repl_editor.c` calls GLUT *input/feedback* APIs
   through the funnel helpers and that this is the only `repl_*` file
   allowed to include `<GL/freeglut.h>` for input — distinct from the
   `repl_executor.c` exception, which is allowed to call GL drawing APIs.

**Verify:** `make test-stubs TEST_JOBS=4`; `./sample` cursor changes
across the code-panel resize divider, undo/redo redraws, scrolling
redraws, and SHIFT-clicking the time toggle (Ctrl+T with SHIFT) all
behave as before.

#### 11f. Lock both boundaries with grep guards

After 11a-11e land, two rules are enforceable:

- **GL/GLUT layer rule:** only `scene_*.c`, `ui_*.c`, and
  `repl_executor.c` issue GL/GLU drawing calls; only `sample.c` and
  `repl_editor.c` issue GLUT input/feedback calls.
- **Cross-layer coupling rule:** `ui_*` and `scene_*` do not include
  each other's headers (with one grandfathered exception — see below).

Steps:

1. Add a `make check-gl-boundaries` target that runs four greps and
   fails the build if any returns matches:
   - **GL/GLU drawing:**
     `grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*\s*\(' repl_*.c | grep -v '^repl_executor\.c:' | grep -v ':\s*[/*"]'`
     (exclude executor and lines that look like comments/strings).
   - **GL in sample.h:** same grep against `sample.h` should be empty
     (transform helpers / point-size shim moved out in 11c).
   - **GLUT input/feedback:**
     `grep -nE '\bglut[A-Z][A-Za-z0-9]*\s*\(' repl_*.c | grep -vE '^repl_(editor|executor)\.c:' | grep -v ':\s*[/*"]'`
     (only editor and executor may name GLUT symbols).
2. Add a `make check-layer-coupling` target enforcing layer
   independence:
   - `! grep -nE '#include\s+"scene_' ui_*.c ui_*.h` —
     **UI must not include any scene header.** Hard rule, no exceptions.
   - `! grep -nE '#include\s+"ui_' scene_*.c scene_*.h | grep -vE 'scene_render\.c:.*ui_panels\.h'` —
     scene must not include UI headers, with one grandfathered
     `scene_render.c → ui_panels.h` coupling for `scene_rect()`.
     (The previous `scene_render.c → ui_profile_panel.h` coupling
     disappears after the Phase 1 prof-module extraction —
     `prof_begin`/`prof_end` come from the new `prof.h` instead.)
     Track the remaining `scene_rect()` exception for removal — see
     follow-up below.
3. Wire both targets into `make test` and the `make test-stubs`
   umbrella so a regression is caught on first push.
4. Document the rules in `MODULES.md` under "Naming Notes". One-line
   summaries:
   - *"`repl_*.c` files don't call GL or GLU. Only `repl_executor.c`
     (drawing) and `repl_editor.c` (GLUT input via local funnel helpers)
     are exceptions."*
   - *"`ui_*` and `scene_*` are independent layers. Neither includes
     the other's headers. The remaining `scene_render.c → ui_panels.h`
     include for `scene_rect()` is grandfathered until layout
     coordinates migrate to a shared `repl_*` model module."*
5. **Optional follow-up (separate punch-list item, not part of 11):**
   move `scene_rect()` out of `ui_panels.c` into a `repl_*` layout
   model so the grandfathered `scene_render.c → ui_panels.h` include
   can be deleted. Same for profile-panel layout. Track as a
   TODO in `MODULES.md` Open Edges; do not block 11 on it.

**Verify:** both new make targets pass on the cleaned tree and fail
if any boundary line from 11a-11e is reintroduced — confirm by
temporarily reverting one move from each sub-step and running the
targets. `make test-stubs TEST_JOBS=4` must stay green.

---

**Cumulative verification umbrella for item 11.** Each commit lands
independently and is verifiable, but the full sequence should also be
re-checked at the end:

- `make test-stubs TEST_JOBS=4` — all 19 suites green.
- `make sample && ./sample` — load the cube, orbit/pan/zoom, F12 cycle
  through every example, replay (Ctrl+G) a heavy example, accumulation
  AA on and off, multi-scene workspace save/load, Ctrl+T time toggle
  with and without SHIFT.
- `make sample USE_GL_STUBS=1` — link must succeed without any new
  stub gaps.
- `make sample NO_POINT_PARAMETER=1` — the relocated point-size shim
  still applies the attenuation only to the user-facing
  `glPointSize` command.
- `bench_repl fade_batches` GL-call count: equal or lower than before.
- `make check-gl-boundaries` — empty.
- `make check-layer-coupling` — empty modulo grandfathered exceptions.

---

#### Decisions / open questions explicitly settled before starting 11

These are the architectural calls the punch list would otherwise let the
implementer make ad-hoc on the fly. Decide and record before opening
the first commit so each sub-step is mechanical.

1. **2D helpers go into a project-agnostic header library, not
   per-layer duplication.** Earlier drafts considered duplicating
   `begin_2d`/`draw_string` per layer (`scene_2d_*` and `ui_2d_*`).
   That trades cross-layer coupling for code duplication. The
   better answer is **extract into `include/gl_2d.h`**, a header-only
   library alongside the existing `gl_includes.h` / `stb_image.h` /
   `utils.h` set. Both `scene_*` and `ui_*` `#include <gl_2d.h>`;
   neither depends on a sibling layer's header. Pure-model `repl_*`
   files don't include it (they don't draw). The criterion for what
   belongs in the header library is **purely a function of arguments
   with no project state** — `gl2d_begin(int w, int h)`,
   `gl2d_draw_string(float x, float y, const char *s, void *font)`.
   Project-state-dependent helpers (`apply_transform_cmd`,
   `_repl_point_size`, the replay fade pass) stay project-side
   because they read `GLCmd`, REPL state, or replay state. See 11a
   for the proposed `gl_2d.h` API and 11c for the project-side
   helpers that cannot move. ✅ DONE

2. **`repl_display_func()` thin wrapper retention.** `sample.c:96`
   currently calls `repl_display_func()` from its GLUT wrapper. After
   11a, that wrapper becomes a one-liner — just dispatch + swap.
   **Decision: keep the wrapper.** `sample.c` stays the GLUT-only
   file (it only knows about `repl_*`, not `scene_render.h`), and
   `repl_core.c`'s orchestrator role is preserved at the public API
   boundary. The buffer-swap call (`glutSwapBuffers()`) moves into
   `sample.c`'s wrapper after `repl_display_func()` returns, since
   that's GLUT plumbing, not GL drawing.

3. **`update_render_state_strings()` / `update_cam_lines()`
   ownership.** Both live in `repl_export.c` and are called from
   `display_func()`, three call sites inside `repl_export.c`, and
   `repl_state.c`. They produce text scaffold (no GL).
   **Decision: leave as-is.** This is not a GL-refactor concern;
   moving them would mix string-formatter ownership churn into a
   GL-isolation slice. Flagged here only so the implementer doesn't
   get distracted by their non-GL footprint while editing
   `repl_core.c`'s `display_func()`.

4. **2D viewport `glViewport` placement (11a-step-6).**
   **Decision: keep viewport configuration internal to
   `render_code_panel()`.** This mirrors `render_3d_scene()`, which
   sets its own viewport at the top of its body — symmetric across
   the two render layers. `display_func()` does not own viewport
   policy for either side; each renderer states explicitly what
   viewport it draws into. No new `ui_panels_begin_overlays()`
   helper is introduced. The other 2D overlays (autocomplete,
   dropdown, var panel, help, profile) run after
   `render_code_panel()` and inherit the viewport it set, which
   matches today's behavior. Document the convention in
   `render_code_panel()`'s top-of-function comment so the dependency
   is explicit.

5. **PROF_SCENE_3D bracketing (11a-step-5).**
   **Decision: leave the entire profiling structure as-is.** Keep
   `prof_begin(PROF_SCENE_3D)` / `prof_end(PROF_SCENE_3D)` in
   `display_func()`, keep the inner `PROF_SCENE_3D_SETUP..HUD`
   reset/commit pair where it currently sits. The accumulation-AA
   hoist (11a-step-5) does *not* move any prof brackets — it only
   moves the `glClear`/`glAccum` jitter loop. That keeps this slice
   focused on GL-isolation; profiling layout stays untouched.

   **However** — the prof system itself is structurally
   misplaced. `prof_begin`/`prof_end`/`prof_accum_*`/`prof_frame_tick`
   and the `ProfSection` enum currently live in `ui_profile_panel.h` /
   `ui_profile_panel.c`, but they are called from `repl_core.c`,
   `repl_replay.c`, every `scene_*.c`, and the code-panel renderers.
   That means every pipeline and scene-rendering file effectively
   includes a UI header for instrumentation, which violates the
   layering rule before we even start applying it. The prof
   primitives are generic instrumentation, not UI.

   **Mitigation:** extract the prof primitives into a separate
   `prof` module *before* 11a-11f. See the new "11. Phase 1
   Cleanup" stage below; that stage is a precursor to the
   GL-isolation work.

6. **`bench_repl.c` access path for `render_replay_fade_pass()`
   (11d-step-3).** The plan lists a "preferred" and "fallback".
   **Decide up front:** the preferred path — expose
   `render_replay_fade_pass()` from `scene_render.h` unconditionally,
   not behind `#ifdef BENCH_REPL_HARNESS`. Conditional exports
   complicate the public header and the bench is part of the source
   tree, not a separate consumer. The bench is simply a different
   entrypoint that drives one render pass; that's fine. Flagging
   this as the canonical answer prevents the implementer from
   reaching for the upward-shim fallback.

7. **`gl_includes.h` retention in `sample.h` (11c-step-4).** The
   plan says "trial removal." **Concrete answer:** `sample.h`
   declares `GLfloat`, `GLenum`, and the `GLCmd` struct (which
   contains GL types), so `gl_includes.h` must remain in `sample.h`
   even after 11c. The point of 11c is to ensure no `static inline`
   GL *function calls* expand into every TU; type declarations are
   fine. Don't waste a sub-commit chasing this.

8. **`TessVertex` typedef location (11b-step-5).** Currently in
   `repl_state.h`; consumed by `repl_executor.c` (the dispatch path
   at `repl_executor.c:328-334`) and emitted as a string by
   `repl_export.c:1529-1543`. After 11b, the only live consumer is
   `repl_executor.c`. **Decide:** move the typedef into
   `repl_executor.h` (or keep as file-private in `repl_executor.c`
   with a forward declaration if no header export is needed — check
   tests first via `grep -n 'TessVertex' test_*.c`).

9. **`repl_state_render_init_resources()` rename strategy
   (11b-step-2).** No callers currently reference the old name except
   `repl_init_gl()` and `sample.c`'s teardown path. **Decide:** hard
   rename, no compat shim. Two callers update in the same commit;
   leaving an alias is overhead with no payoff.

10. **`test_ui.c` impact.** `test_ui.c` exercises `render_help()` and
    other `ui_*` renderers via the GL-stub-counter harness. It
    includes `sample.h` plus the `ui_*.h` headers it tests. **Decide:**
    `test_ui.c` is unaffected by 11a-11e because all its calls are
    already through the `ui_*` public renderers. The 2D primitive
    rename (`ui_2d_begin/end/draw_text`) is purely internal to those
    renderers. Confirm by re-running `make test_ui` after each
    sub-step; no test edits expected.

11. **`scene_lights_init_global_ambient()` vs. fold into
    `scene_lights_setup()` (11a-step-6).** The current
    `glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ...)` runs once at GL init
    with constants `{0.15, 0.15, 0.20, 1.0}`. `scene_lights_setup()`
    runs every frame. **Decide:** add a separate one-shot
    `scene_lights_init_global_ambient()` (or
    `scene_lights_init_static_state()` if more such one-shots
    accumulate later). The global ambient is invariant; folding it
    into `scene_lights_setup()` would re-set it on every frame for
    no benefit and would make per-frame light state harder to audit.

12. **Sub-step ordering and dependencies.** Phase 1 (11.0a, 11.0b)
    must land before Phase 2. Within Phase 2:
    - 11.0a (prof module extraction) is a hard prerequisite for 11f's
      `make check-layer-coupling` grep guard — without it, every
      `scene_*.c → ui_profile_panel.h` include shows up as a
      violation.
    - 11.0b (per-slice naming pass) lands as a separate
      `refactor:` commit immediately before each Phase 2 sub-step,
      scoped to the files that sub-step touches.
    - 11b (executor owns tess/quadric) is independent of 11a.
    - 11c (transform helpers move to `repl_executor.h`) depends on
      nothing in 11a/11b but is cleanest if 11b lands first so
      `repl_executor.c` is already the established home for
      executor-owned resources.
    - 11d (replay fade pass) is independent of 11a-11c.
    - 11e (GLUT funnel) is independent of all preceding sub-steps.
    - 11f (grep guards) requires 11.0a + 11a-11e all landed.

    **Decide:** land in the order 11.0a → 11.0b(11a) → 11a →
    11.0b(11b) → 11b → … → 11f. The full project-wide naming sweep
    from `feature/repl-cleanup.md` §10 stays a separate Phase 10
    item, not a Phase 2 prerequisite. Document the intended landing
    order in the first commit message of the series.

13. **"Hard rule" on `ui_*` ↔ `scene_*` separation vs. read-only
    layout queries.** 11f-step-2 grandfathers
    `scene_render.c → ui_panels.h` for `scene_rect()`. The deeper
    fix (move `scene_rect()` into a `repl_*` layout model) is listed
    as an *optional* follow-up. **Decide:** confirm the optional
    follow-up is genuinely deferrable. `scene_rect()` is a query
    against `repl_state_viewport()` plus the code-panel layout
    fraction — pure derivation, no GL. Moving it into a new
    `repl_layout.c` (sibling to `repl_code_panel_layout.c`) is a
    half-day refactor with no runtime risk. **Recommendation:** do
    the optional follow-up *before* item 11f's grep guard so the
    guard can be unconditional (no grandfathered list to maintain).
    Alternatively, document a hard date / next-tier-cycle commitment
    so the exception doesn't ossify.

---

## Not recommended for now

- **Broad, multi-domain state clustering in one commit:** high long-term value
  but enormous churn surface. Phase 2 is now active and should continue as
  narrow storage-migration slices: document, flat program, editor input,
  camera/view, presentation, render resources, replay, scenes/import-export,
  search/autocomplete/status, then remaining UI runtime state.
- **Keyboard dispatch table** (collapse the 22 `handle_*_key_route`
  functions in `repl_editor.c` into a table): the chain *is* the
  ordering policy and reading it sequentially is fairly clear today.
  Lower payoff than the active Tier 2/3 cleanup items.

---

## Verification umbrella

For any item picked:

1. `make test-stubs TEST_JOBS=4` - all 19 suites should pass cleanly.
2. `make sample && ./sample` - load an example, exercise the
   touched feature, confirm no visual or interactive regression.
3. For Tier 2 rendering changes (items 4 & 5), do an explicit A/B
   against the pre-refactor binary on every theme/overlay - diff
   review alone is not enough for pixel-level equivalence.
4. Single commit per item, conventional `refactor:` prefix per
   project CLAUDE.md.
