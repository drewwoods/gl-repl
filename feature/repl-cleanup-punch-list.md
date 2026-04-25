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

#### 11a. Split the 2D helpers per layer; hoist accumulation-AA into `render_3d_scene()`; keep `repl_core.c` as orchestrator

**Layering check (verified):**

- No `ui_*.c` includes any `scene_*.h`.
- The only `scene_*` → `ui_*` coupling is `scene_render.c` reading
  `scene_rect()` and `ui_profile_panel.h` for layout coordinates — a
  read-only query, not render dispatch.
- `repl_core.c`'s `display_func()` is the single master that calls
  `render_3d_scene()` then the 2D `render_*` helpers in order; neither
  layer reaches sideways into the other.

That clean split is worth preserving. The 2D helpers
(`begin_2d`/`end_2d`/`draw_quad`/`draw_string`) are currently the *only*
shared GL code between `scene_*` and `ui_*` — and they're trivial. Rather
than moving them into one layer and forcing the other to depend on it, keep
them per-layer.

**Steps:**

1. **Duplicate / inline `begin_2d` / `end_2d`.** They have only two
   `scene_*` callers (`scene_render.c` replay HUD and `scene_grid.c` ruler
   labels). Copy the bodies verbatim into static helpers
   `scene_2d_begin()` / `scene_2d_end()` in `scene_render.c` (exposed via
   `scene_render.h` so `scene_grid.c` can share them within the scene
   layer). Move the public `begin_2d`/`end_2d` definitions from
   `repl_core.c:564-584` into `ui_panels.c` (or a new tiny `ui_2d.c` if
   you'd rather not enlarge `ui_panels.c`) and rename to `ui_2d_begin()` /
   `ui_2d_end()`. Update every `ui_*.c` caller. The two definitions are
   identical today; that's fine — the duplication makes it a *layer*
   convention rather than a shared utility, and either side can evolve
   independently (e.g. `scene_2d_*` may want to keep depth-test on for
   text-on-3D in future).

2. **Replace `draw_quad` with `glRectf`.** Every call site is a solid
   filled rectangle in current color — exactly `glRectf`'s job, no
   helper needed. Mechanical replace:
   `draw_quad(x,y,w,h)` → `glRectf((float)(x), (float)(y), (float)(x+w), (float)(y+h))`.
   ~50 sites across `scene_render.c` (replay HUD), `ui_color_picker.c`,
   `ui_help_overlay.c`, `ui_profile_panel.c`, `ui_variable_panel.c`,
   `ui_autocomplete_panel.c`, `ui_panels.c`, `ui_menu_bar.c`. Delete
   `draw_quad` from `repl_core.c:555-561` and the forward decl in
   `sample.h:355`. Confirm the local stub (`include/GL/gl.h`) declares
   `glRectf`; it almost certainly does, since fixed-function GL builds
   use it all over the project.

3. **Duplicate `draw_string` per layer.** The body is two GL calls
   (`glRasterPos2f` + `glutBitmapCharacter` loop) — same argument as
   `begin_2d`. Move to `scene_render.c` as static `scene_2d_draw_text()`
   (exposed in `scene_render.h` for `scene_grid.c` ruler labels) and to
   `ui_panels.c` (or `ui_2d.c`) as `ui_2d_draw_text()`. Update the three
   `scene_*` callers (`scene_render.c:375,383,409`) and every `ui_*`
   caller. Delete `draw_string` from `repl_core.c:549-552` and its
   forward decl in `sample.h:354`.

4. **Hoist accumulation-AA into `render_3d_scene()`.** `display_func()`
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
     readable.
   - After the move, `display_func()` calls `render_3d_scene()` once,
     period. No knowledge of accumulation.

5. **Trim `display_func()` to pure orchestration.** What remains in
   `repl_core.c` after step 4:

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
   - The 2D-side `glViewport` at `:677` belongs at the top of the
     overlay sequence — either as the first line of `render_code_panel()`
     or as a new `ui_panels_begin_overlays()` helper that
     `display_func()` calls before the overlay sequence.
   - `glutSwapBuffers()` is GLUT framework plumbing, not GL drawing.
     Move to `sample.c`'s `display_func()` GLUT wrapper, right after
     `repl_display_func()` returns. That keeps the buffer swap at the
     GLUT-callback boundary, which is its natural home.

6. **Move `init_gl`'s `glLightModelfv` into `scene_lights.c`.** Add
   `scene_lights_init_global_ambient()` (one-shot, called from
   `repl_init_gl()`); the existing per-frame `scene_lights_setup()` keeps
   its current responsibility. `repl_init_gl()` then has zero GL calls —
   it just calls `scene_resources_init()` (from 11b) and
   `scene_lights_init_global_ambient()`, plus `apply_init_bootstrap()`.

7. **Verification grep.** After 11a lands:
   `grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*\s*\(' repl_core.c`
   should be empty. The orchestration role of `repl_core.c`'s
   `display_func()` is preserved — it still drives the
   `render_3d_scene()` → 2D-overlay-sequence pipeline — but no GL call
   originates there.

8. **Update docs.** `ARCHITECTURE.md` `repl_core.c` ownership: drop the
   "GL init" and "display callback owns frame GL state" implications;
   add to `scene_render.c`: "owns 3D viewport, clear, accumulation-AA
   loop, and per-frame `glViewport`"; add to `ui_panels.c`: "owns the
   2D overlay viewport bracket"; add to `sample.c`: "owns the GLUT
   buffer swap." `MODULES.md` Layer 1/4/5 tables update in lockstep,
   and the per-layer 2D primitives convention (`scene_2d_*` /
   `ui_2d_*`) gets a one-line note in "Naming Notes".

**Verify:** `make test-stubs TEST_JOBS=4`; `make sample && ./sample`,
load examples 0-15, F12 cycle, replay a fade-heavy example with
accumulation AA on (default) and off (`--noaccum`) — the AA jitter
loop is the highest-risk piece. Confirm `glutSwapBuffers` still runs
exactly once per frame (a quick `printf` instrumented in
`sample.c`'s wrapper, removed before commit, is the easiest check).
After the move, A/B against the pre-refactor binary on the replay HUD
and ruler-grid labels — those are the two `scene_*` callers of
`begin_2d`/`draw_string` and the ones most likely to drift.

#### 11b. Push tessellation/GLU bootstrap behind a scene-owned module

`repl_state.c` is the typed runtime-state facade — it should own *storage*,
not GL resource lifetimes. The 5 tess callbacks and the `gluNewQuadric`/
`gluNewTess` bootstrap pair are render-side concerns.

Steps:

1. Create `scene_resources.c` / `scene_resources.h` (or fold into
   `scene_render.c`) that owns:
   - the static `g_quadric`, `g_tess`, `g_tess_verts[]`, `g_tess_vert_count`
     storage (move from `repl_state.c`),
   - the 5 tess callbacks `repl_render_tess_vtx_begin_cb`,
     `repl_render_tess_vtx_end_cb`, `repl_render_tess_vtx_cb`,
     `repl_render_tess_comb_cb`, `repl_render_tess_err_cb`
     (`repl_state.c:389-450ish`),
   - `repl_state_render_init_resources()` and
     `repl_state_render_destroy_resources()` (`repl_state.c:1066-1106`).
2. Rename the public entrypoints to match the scene module
   (`scene_resources_init()` / `scene_resources_destroy()`) and update the
   two callers (`repl_init_gl` in `repl_core.c` and the destroy site in
   `sample.c`). Keep one-line `repl_state_render_*_resources` shims if any
   test references them; otherwise delete the old names.
3. Confirm `repl_executor.c` accessors that read `g_quadric` / `g_tess`
   continue to compile — likely move those externs into
   `scene_resources.h`, or expose typed getters
   (`scene_resources_quadric()`, `scene_resources_tess()`) and migrate
   `repl_executor.c` to use them. Getter form is preferred since it keeps
   the pointer non-mutable from the caller side.
4. After the move, `repl_state.c` should have **zero** live GL/GLU calls.
   The string-literal hits at `repl_state.c:178-186` (the
   `g_render_state_lines` / camera scaffold strings) are not live calls and
   stay where they are.
5. Update the local `include/GL/glu.h` stub if needed — `gluNewQuadric`,
   `gluNewTess`, `gluTessCallback`, `gluTessProperty`, `gluTessBeginPolygon`,
   `gluTessBeginContour`, `gluTessVertex`, `gluTessEndContour`,
   `gluTessEndPolygon`, `gluDeleteQuadric`, `gluDeleteTess`,
   `gluQuadricNormals`, `gluQuadricTexture`, `gluSphere`, `gluCylinder`,
   `gluDisk`, `gluPartialDisk`, `gluOrtho2D` should all already be present
   for `USE_GL_STUBS=1`. Adding `#include "scene_resources.h"` to whatever
   stub-build object list previously included these symbols may be
   required.

**Verify:** `make test-stubs TEST_JOBS=4` (the tess callbacks must still
link in stub mode); `make sample && ./sample`; load an example that
exercises tessellation (the polygon-with-holes tess examples); confirm
the quadric is recreated on a window-context rebuild path if any. Also
explicitly run the leak/teardown path — exit cleanly via `q`/window
close — to make sure `scene_resources_destroy()` fires.

#### 11c. Replace inline GL helpers in `sample.h`

`sample.h` is supposed to be the shared *types* and compatibility header.
Inline helpers that expand to GL calls in every translation unit pull GL
symbols into every `.c` that includes `sample.h`, defeating the layering.

Steps:

1. Move `is_transform_cmd` (still pure, no GL) — keep where it is or move
   to `repl_executor.h` since it classifies commands. No behavior risk.
2. Move `apply_transform_cmd`, `apply_tracked_transform_cmd`, and
   `unwind_tracked_transform_stack` (`sample.h:296-333`) into
   `repl_executor.c` (or a new `repl_transform_apply.c` if both
   `repl_executor.c` and `scene_transform_guides.c` need them — check
   `grep -nE 'apply_(tracked_)?transform_cmd|unwind_tracked_transform_stack'`).
   Convert from `static inline` to ordinary `extern` functions exported
   from the new home's header. The current call sites are:
   - `repl_executor.c` (the `case CMD_TRANSLATE3F:` etc. dispatch) — already
     in the same TU, becomes a static helper local to `repl_executor.c`.
   - `scene_transform_guides.c` `compute_before_cursor_matrix` — calls
     `apply_tracked_transform_cmd`. Use the exported version from
     `repl_executor.h` (or `repl_transform_apply.h`).
   - `scene_overlays.c` walk — same call pattern.
3. Replace `_repl_point_size` (`sample.h:386-395`) with a real function
   in `repl_executor.c` (e.g. `repl_apply_point_size(GLfloat)`). Update
   the `glPointSize` macro re-definition: instead of overriding the GL
   symbol globally via `sample.h`, do the override only inside
   `repl_executor.c` for the `case CMD_POINT_SIZE:` path. All other
   `glPointSize` call sites (`scene_geometry_guides.c`, `scene_overlays.c`,
   `scene_axes.c`, `scene_lights.c`) call the real GL function and don't
   need the attenuation hack — confirm by re-reading the original intent
   in the `sample.h:386-395` comment block. If a *user-facing* point-size
   command is the only place that needs distance attenuation, scoping the
   shim to the executor is a behavior-preserving win.
4. Drop the `#include <gl_includes.h>` from `sample.h` if it survives only
   to make these inline GL helpers compile — keep it if struct types
   (`GLfloat`, `GLenum`, `GLCmd`) still require it.
5. `sample.h`'s GL-call grep should hit zero after the move.

**Verify:** `make test-stubs TEST_JOBS=4`; build both `make sample` and
`make sample USE_GL_STUBS=1` and `make sample NO_POINT_PARAMETER=1`
(the punch list lives at the intersection of these three flags). Run
the bench harness `./bench_repl fade_batches` if available — it
exercises tracked transforms via the executor.

#### 11d. Move replay-fade GL pass into `scene_render.c`

`repl_replay.c` should be the replay *model* (state machine, fade-batch
ring, alpha math, PC tracking). The actual GL pass at
`repl_replay.c:773-809` is a render owner.

Steps:

1. Split `execute_replay_fade_batches()` into two halves:
   - **Model side** (stays in `repl_replay.c`): the `skip_limits[]`
     computation loop at lines 735-771 — pure data, no GL. Expose as
     `replay_compute_fade_skip_limits(int *out_limits, int max_count)`
     returning the actual count. Also keep `replay_batch_alpha()`,
     `repl_execute_set_fade_context()`, and `replay_has_active_fades()`
     where they are.
   - **Render side** (moves to `scene_render.c` as a new
     `scene_render_replay_fades()` next to the existing fade-pass setup
     at `scene_render.c:512-530`): the `glPushAttrib` / per-batch
     `glDisable(GL_LIGHTING)` / `glEnable(GL_BLEND)` / `glBlendFunc` /
     `glPolygonMode` / `glColor4f` / `glPushMatrix` / `glPopMatrix` /
     `glPopAttrib` block plus the per-batch
     `repl_execute_program(&exec_options)` call.
2. The existing fade-pass setup in `scene_render.c:512-530` already
   primes lighting, blend, and polygon mode, then immediately calls
   `execute_replay_fade_batches()`. After the move, both blocks live
   in one place — collapse the duplicated setup so we only set blend/
   polygon mode once around the loop, not once per batch. This is the
   payoff for moving the code.
3. Rename or delete the now-thin `execute_replay_fade_batches()`
   wrapper in `sample.h:375` and `repl_replay.h:29` — callers in
   `bench_repl.c` need to migrate to the new entrypoint
   (`scene_render_replay_fades()`); the bench harness explicitly
   counts GL calls from this function (`bench_repl.c:531`,
   `bench_repl.c:564`), so its name appears in test output strings —
   update those too.
4. After the move, `repl_replay.c` should have **zero** live GL calls.
   `repl_replay.h` should drop any `gl_includes.h` include if no
   remaining declaration needs it.

**Verify:** `make test-stubs TEST_JOBS=4`; the bench harness
`./bench_repl fade_batches` GL-call count should be **strictly less than
or equal to** the pre-refactor number (the per-batch redundant
`glDisable(GL_LIGHTING)`/`glEnable(GL_BLEND)`/`glBlendFunc`/
`glPolygonMode` calls collapse to one). Capture the before/after counts
in the commit message. `make sample && ./sample`: load an example with
heavy geometry, Ctrl+G to start replay, hold Right arrow — the trailing
ghost should look identical.

#### 11e. Funnel `repl_editor.c` and `repl_actions.c` GLUT calls through helpers

This is the smallest sub-step but it's what makes the rule mechanically
enforceable: after this lands, the grep `grep -nE '\bgl[uU]?[A-Z]' repl_*.c`
(excluding `repl_executor.c`) should return nothing.

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
   `repl_editor.h`. `repl_actions.c` calls the helper instead of including
   GLUT headers directly. After this, `repl_actions.c` has zero
   `gl[uU][A-Z]` matches.
3. Update the "Naming Notes" exception list in `MODULES.md:359-365` to
   mention that `repl_editor.c` calls GLUT *input/feedback* APIs through
   the funnel helpers and that this is the only `repl_*` file allowed to
   include `<GL/freeglut.h>` for input — distinct from the
   `repl_executor.c` exception, which is allowed to call GL drawing APIs.

**Verify:** `make test-stubs TEST_JOBS=4`; `./sample` cursor changes
across the code-panel resize divider, undo/redo redraws, scrolling
redraws, and SHIFT-clicking the time toggle (Ctrl+T with SHIFT) all
behave as before.

#### 11f. Lock the boundary with a grep guard

After 11a-11e land, the live-call surface should match the rule: only
`scene_*.c`, `ui_*.c`, and `repl_executor.c` issue GL/GLU calls; only
`sample.c` and `repl_editor.c` issue GLUT input/feedback calls.

Steps:

1. Add a `make check-gl-boundaries` target that runs three greps and
   fails the build if any returns matches:
   - `grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*\s*\(' repl_*.c | grep -v '^repl_executor\.c:' | grep -v ':\s*[/*"]'`
     (exclude executor and exclude lines that look like comments/strings)
   - same grep against `sample.h` / `sample.c` minus `sample.c` GLUT
     callback wiring.
   - `grep -nE '\bglut[A-Z][A-Za-z0-9]*\s*\(' repl_*.c | grep -vE '^repl_(editor|executor)\.c:' | grep -v ':\s*[/*"]'`
     (only editor and executor may name GLUT symbols).
2. Wire `check-gl-boundaries` into the same recipe `make test` already
   uses, or call it from CI / the `make test-stubs` umbrella.
3. Document the rule in `MODULES.md` under "Naming Notes" so the next
   contributor sees the boundary plus the grep that enforces it. The
   single-line summary: *"`repl_*.c` files don't call GL or GLU. Only
   `repl_executor.c` (drawing) and `repl_editor.c` (GLUT input via local
   funnel helpers) are exceptions."*

**Verify:** the new `make check-gl-boundaries` passes on the cleaned
tree and fails if any of the prior `grep -E '\b(gl|glu)[A-Z]'` lines
are reintroduced — confirm by temporarily reverting one of the moves
in 11a and running the target. `make test-stubs TEST_JOBS=4` must stay
green.

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
