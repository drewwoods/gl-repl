# REPL Refactor Punch List

## Status (2026-04-29)

Everything in Tiers 1, 2, and 3 of this punch list is complete:

- ✅ Tier 1 - Mechanical extractions (help overlay, variable panel,
  autocomplete popup, inline rename, variable dragging, color picker,
  menu/dropdown).
- ✅ Tier 2 - Pattern consolidation (data-driven grid/axes themes,
  vertex overlay visitor, scene helper GL state guards, backdrop /
  light setup / polygon outline / scene-edit guides extraction,
  widened render config / frame context, workspace header table,
  ordered importer handlers, typed export scaffold).
- ✅ Tier 3 - Structural extractions (parse_command → repl_parser,
  repl_flatten_program API, command-store escape hatches, prof
  module split, GL/GLUT segregation 11a–11f, layer-coupling guards).
- ✅ Strategic completions - function naming consistency, comprehensive
  per-header documentation.

The active follow-up tracks are now:

- `feature/push-architecture-refinement.md` - R10 phases 2–5 (dissolve
  `repl_core.c`), R11 allowlist shrink, R12 (single public REPL header),
  R8 (sample → imrepl rename).
- `feature/gold-standard-state-ownership.md` - Stage 4 cursor-pixel
  output actualization, Stage 6 undo on `repl_state_capture()`, Stage 7
  UI snapshot purity.

This punch list is preserved as historical context; new tactical work
should land against the active feature docs above.

## Context

The companion doc `feature/repl-cleanup.md` is the strategic 10-stage
ownership-reorganization plan. This punch list is the tactical
counterpart: a prioritized set of concrete, behavior-preserving
extractions that can each land as a single reviewable commit *today*,
without committing to the larger context-object rewrite.

The active architecture direction and ordering constraints live in
[`feature/push-architecture-refinement.md`](./push-architecture-refinement.md).
Use that file as the controller-first source of truth for Phase 2 boundaries;
this punch list should cross-link to it whenever a cleanup item depends on
R1-R12 rather than repeating a divergent local plan.

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
> all 18 suites / 2503 tests cleanly. Any new failure introduced by a
> punch-list item is a real regression unless the team explicitly
> rebaselines it.

---

## Strategic Completions

### Phase 10 Step 1: Function Name Consistency + Comprehensive Header Documentation ✅ DONE

Function naming across public headers now follows a module-prefix pattern, and
all module headers have comprehensive public API documentation. For
REPL-owned modules the public prefix is `repl_<module>_<action>()`; the
architecture refinement plan now calls out that `repl_` is not a catch-all
sample prefix.

**Function Naming (Waves 1-2):**
- **repl_replay.h:** 18 functions renamed (replay_start → replay_start, etc.)
- **repl_eval.h:** 16 functions renamed (init_predef_vars → repl_eval_init_predef_vars, etc.)
- **repl_parser.h:** 3 functions renamed (repl_parse_command → repl_parser_parse_command, etc.)
- **repl_source_scope.h:** 10 functions renamed (depth_cache_invalidate → repl_source_scope_depth_cache_invalidate, etc.)
- **repl_undo.h:** 3 functions renamed (push_undo_snapshot → repl_undo_push_snapshot, etc.)
- **repl_clipboard.h:** 4 functions renamed (clear_selection → repl_clipboard_clear_selection, etc.)
- **repl_executor.h:** 3 functions renamed (apply_transform_cmd → repl_executor_apply_transform_cmd, etc.)
- **repl_export.h:** 2 functions renamed (save_output → repl_export_save_output, load_from_file → repl_export_load_from_file)
- **repl_audio.h:** Syntactically compliant with `repl_audio_*`, but
  semantically a namespace-audit candidate because it is an app-level service,
  not REPL language/editor state. See R11 / the namespace notes in
  `feature/push-architecture-refinement.md`.

**Total naming scope:** 59 functions renamed across 9 header modules, 25+ implementation files updated, all 2503 tests passing.

**Header Documentation (Comprehensive Phase):**
All 26 repl_* headers, 7 ui_* headers, and 8 scene_* headers now include:
1. **Module overview** - Role, key abstractions, primary use cases
2. **Lifecycle documentation** - When/where/how initialized, per-frame updates, state maintenance
3. **Type definitions** - Shared structs and enums (if any)
4. **Function documentation** - Purpose, return value, parameters, frame phase, integration points

Headers are stand-alone references: read any header from top to bottom without consulting other headers or implementation files.

**Documentation coverage:**
- **repl_* (26 headers):** repl_audio, repl_autocomplete, repl_autonormal, repl_camera_controls, repl_clipboard, repl_code_panel_document, repl_code_panel_layout, repl_command_spec, repl_command_store, repl_config, repl_core_internal, repl_editor, repl_eval, repl_example_loader, repl_examples, repl_export, repl_flatten, repl_inline_rename, repl_keys, repl_parser, repl_replay_annotations, repl_replay, repl_scenes, repl_search, repl_source_scope, repl_var_drag
- **ui_* (7 headers):** ui_autocomplete_panel, ui_color_picker, ui_help_overlay, ui_menu_bar, ui_panels, ui_profile_panel, ui_variable_panel
- **scene_* (8 headers):** scene_axes, scene_backdrop, scene_geometry_guides, scene_grid, scene_lights, scene_overlays, scene_render, scene_transform_guides

**Test fixes:** test_ui.c updated to use new ui_* function names. All 2541 tests passing (includes new test_ui fixes).

**Documentation updates:** MODULES.md and ARCHITECTURE.md updated to reference new "Header Documentation Standard" explaining the consistent documentation structure and conventions. New section in MODULES.md: "Header Documentation Standard" with full details on module overview, lifecycle, types, and function documentation.

Implementations updated across repl_*.c files; all call sites including tests updated. Landed as multiple commits with focused naming waves, header documentation batches, test fixes, and doc updates.

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

### 10b. Namespace audit after `imrepl_ctrl` ✅ TODO

`repl_` has become too broad as a historical prefix. With `imrepl_ctrl` now
owning app-frame wiring, the naming pass in
[`feature/repl-cleanup.md`](./repl-cleanup.md) §10 should distinguish module
ownership before more renames land:

- `repl_*` - REPL language, source/editor model, command pipeline, replay model,
  workspace/user-scene state, and focused REPL actions.
- `imrepl_*` - app shell, controller, app-level services, and lifecycle glue
  after the deferred `sample.c` / `sample.h` rename.
- `scene_*` - 3D stage/render helpers.
- `ui_*` - 2D editor/view renderers and UI hit surfaces.
- neutral names such as `prof`, `cmd_format`, and `gl_stub_counts` - generic
  utilities that should not import REPL state.

`repl_audio` is the concrete case to revisit. It is a playlist/persistence
service used by the sample app and config layer, not the REPL command model.
Do not rename it as drive-by churn in an unrelated cleanup; schedule it with
the app-shell namespace work in
[`feature/push-architecture-refinement.md`](./push-architecture-refinement.md)
R8/R11 so file moves, public function prefixes, tests, and docs change together.

### 11. Segregate live GL calls to scene/UI modules plus `repl_executor.c`

**Goal:** the only `.c` files that issue live GL/GLU calls should be `scene_*.c`,
`ui_*.c`, and `repl_executor.c`. `repl_*.c` files (other than the executor) and
`sample.h` should stop calling GL directly. GLUT *input/feedback* APIs
(`glutPostRedisplay`, `glutSetCursor`, `glutGetModifiers`, `glutSwapBuffers`)
are framework plumbing, not drawing - they are allowed to remain in `sample.c`
and the editor input router, but should be funnelled through one or two
named wrappers so the rule is mechanically checkable.

**Out of scope** (text emission, not live GL):

- `repl_export.c`, `repl_examples.c`, `repl_replay_annotations.c`,
  `repl_command_spec.c`, `repl_parser.c` - these only handle GL command
  strings as REPL source. Grep hits inside string literals don't count.

**Current live-call residue outside the allowed set** now consists of
GLUT input/feedback only (verified by `grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*\s*\('` minus comment/string lines):

| File | Calls | Nature |
|------|-------|--------|
| `repl_actions.c` | 1 | `glutGetModifiers()` SHIFT check on the time-toggle config row |
| `repl_editor.c` | ~23 | All `glutPostRedisplay`/`glutSetCursor`/`glutGetModifiers` - GLUT input/feedback, not GL drawing |

The work below lands as a sequence of small `refactor:` commits.
Each sub-step is **self-contained**: it states the problem, the
files it edits, the move, the verification, and a per-slice naming
pass that lands as a separate `refactor:` commit immediately
before the move. Read any sub-step in isolation; it tells you
everything you need to execute it.

After each commit, `make test-stubs TEST_JOBS=4` must still pass
and `make sample` must still launch.

The sub-steps are organised in two phases:

- **Phase 1 - precursor cleanup** (11.0a). One structural
  prerequisite: extract the prof primitives out of
  `ui_profile_panel.*` into their own module so the GL-isolation
  slices in Phase 2 don't trip over it.
- **Phase 2 - GL isolation** (11a-11f). The mechanical moves that
  segregate live GL calls.

**Per-slice naming pass convention.** The broader naming/comment
pass described in [`feature/repl-cleanup.md` §10](./repl-cleanup.md)
is a Phase 10 strategic concern; doing the full pass before Phase 2
here would inflate the slice. But landing each move on top of
inconsistent local names produces diffs that are harder than
necessary to review. So each sub-step below opens with a "Naming
pass" subsection that lists the files it touches and asks for a
focused rename pass on those files only, applying §10 conventions
(`*_idx` for indexes, `*_count` for counts, canonical names like
`source_line_idx` / `flat_cmd_idx` / `indent_cols` /
`visible_line_count` / `command_store` / `render_config` /
`workspace_dir`, plus removing stale comments and vague TODOs in
the touched files). The naming-pass commit must be a pure rename
(no logic changes). The move commit that follows must be a pure
move (no rename creep). The full project-wide sweep stays a
Phase 10 concern.

**Sub-step ordering.** 11.0a is a hard prerequisite for 11f's
grep guard. Within Phase 2: 11b is independent of 11a; 11c is
cleanest after 11b; 11d/11e are independent of 11a-11c; 11f
requires 11.0a + 11a-11e all landed. Recommended order: 11.0a →
11a → 11b → 11c → 11d → 11e → 11f. Each move commit is preceded
by its naming-pass commit.

---

#### 11.0a. Extract prof primitives into a `prof` module

**Problem.** `prof_begin`, `prof_end`, `prof_accum_reset`,
`prof_accum_end`, `prof_accum_commit`, `prof_frame_tick`,
`prof_code_panel_details_enabled`, and the `ProfSection` enum live
in `ui_profile_panel.h` and `ui_profile_panel.c`. They are called
from `repl_core.c`, `repl_replay.c`, every `scene_*.c` that times
sub-passes, and the code-panel renderers - every layer in the
project transitively `#include`s a UI header to instrument itself.
Profiling primitives are generic instrumentation, not UI.

This is a Phase 1 prerequisite for 11f. Once 11f's
`make check-layer-coupling` grep guard lands, every
`scene_*.c → ui_profile_panel.h` include would show up as a
violation. Extracting prof first means that guard can be
unconditional rather than carrying a per-file exception list.

**Files this slice touches:** `ui_profile_panel.{c,h}`,
`prof.{c,h}` (new), `repl_core.c`, `repl_replay.c`, every
`scene_*.c` that uses prof brackets, every `ui_*.c` that uses prof
brackets, `Makefile`, `MODULES.md`, `ARCHITECTURE.md`.

**Naming pass (separate `refactor:` commit, lands before the
move).** Run a focused rename pass on the files listed above,
applying §10 conventions to touched lines. Pure rename, no logic
changes. `make test-stubs TEST_JOBS=4` stays green. Commit as
`refactor: normalize names for §11.0a prep`.

**Move (the second `refactor:` commit):**

1. Create `prof.h` and `prof.c`. Move out of
   `ui_profile_panel.{h,c}`:
   - the `ProfSection` enum (`ui_profile_panel.h:13-46`),
   - the `PROF_SECTION_COUNT` sentinel,
   - `prof_begin`, `prof_end`,
   - `prof_accum_reset`, `prof_accum_end`, `prof_accum_commit`,
   - `prof_frame_tick`,
   - `prof_code_panel_details_enabled`,
   - any internal `static` storage (per-section accumulators,
     frame-tick counters) - keep file-static in `prof.c`.
2. `ui_profile_panel.{h,c}` now own only the *rendering* of the
   prof HUD: `render_profile_panel()` plus its layout helpers. The
   HUD reads measured timings via a small read-only API exported
   from `prof.h` (e.g. `prof_section_avg_ms(ProfSection)` /
   `prof_section_last_ms(ProfSection)`). Define just enough
   getters to cover what the HUD currently displays - no
   speculative API.
3. Update every caller's include: replace
   `#include "ui_profile_panel.h"` with `#include "prof.h"` in
   `repl_core.c`, `repl_replay.c`, each `scene_*.c` using prof
   brackets, `ui_panels.c`, `ui_*` overlay renderers. The HUD
   call site (`render_profile_panel()` in `repl_core.c:690`)
   keeps `#include "ui_profile_panel.h"` because that's the
   renderer.
4. Add `prof.{h,c}` to the Makefile object lists (sample,
   test-stubs, bench).
5. Update `MODULES.md`: add `prof` to the layer table - it's a
   project-wide instrumentation module that sits beside the
   layered groups (similar to `gl_stub_counts`). Update
   `ARCHITECTURE.md` ownership list with a `prof.c` bullet.

**Verify:** `make test-stubs TEST_JOBS=4` (every TU using prof
brackets must still link); `make sample && ./sample`; toggle the
profile HUD and confirm timings still populate.

#### 11a. Extract generic 2D helpers into a project-agnostic header library; hoist accumulation-AA into `render_3d_scene()`; keep `repl_core.c` as orchestrator ✅ DONE

**Status.** This slice landed as a focused render-orchestration cleanup.
`include/gl_2d.h` now owns the generic 2D helpers, `scene_render.c`
owns the 3D viewport/clear/accumulation-AA loop and clear-color setup,
`ui_panels.c` owns the overlay viewport bracket, and `sample.c` owns the
GLUT buffer swap. `repl_core.c` is now orchestration only; step 7's
ambient-light setup has moved into `scene_render.c`/`scene_lights.c`.

**Layering check (verified):**

- No `ui_*.c` includes any `scene_*.h`.
- After 11.0a, the only `scene_*` → `ui_*` coupling is
  `scene_render.c` reading `scene_rect()` from `ui_panels.h` (a
  read-only layout query, not render dispatch).
- `repl_core.c`'s `display_func()` is the single master; neither
  rendering layer reaches sideways into the other.

The 2D helpers are *generic fixed-function GL utilities* once
`end_2d`'s lighting query is replaced with a plain
`glPushAttrib`/`glPopAttrib` bracket. The right home is neither
"duplicate per layer" nor "move into one layer and force a
dependency"; it's **extract into `include/gl_2d.h`**, a
project-agnostic header-only library alongside `gl_includes.h`,
`stb_image.h`, and `utils.h`. CLAUDE.md endorses extending that
library set. Both `scene_*` and `ui_*` `#include <gl_2d.h>`;
pure-model `repl_*` files don't include it (they don't draw).

**Files this slice touches:** `include/gl_2d.h` (new),
`repl_core.c`, `sample.c`, `sample.h`, `scene_render.c`,
`scene_grid.c`, `scene_lights.c`, `ui_panels.c`, `ui_menu_bar.c`,
`ui_color_picker.c`, `ui_help_overlay.c`, `ui_variable_panel.c`,
`ui_autocomplete_panel.c`, `ui_profile_panel.c`, `MODULES.md`,
`ARCHITECTURE.md`, `CLAUDE.md`.

**Naming pass (separate `refactor:` commit, lands before the
move).** Focused rename on the files listed above, applying §10
conventions to touched lines. Pure rename, no logic changes.
`make test-stubs TEST_JOBS=4` stays green. Commit as
`refactor: normalize names for §11a prep`.

**Implementation notes:**

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
   No need to put it in `gl_2d.h` - `glRectf(x, y, x+w, y+h)` is
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
     loop at the same nesting - keep it inside the inner per-sample step
     so behavior is unchanged.
   - **Profiling layout stays as-is.** Do *not* move any prof
     brackets. `prof_begin(PROF_SCENE_3D)` /
     `prof_end(PROF_SCENE_3D)` keep their frame-level position in
     `display_func()`; the inner `PROF_SCENE_3D_SETUP..HUD`
     reset/commit pair stays where it currently sits. This slice is
     about GL isolation, not profiling layout - keeping prof
     untouched preserves the existing flame chart and limits the
     diff. (The prof primitives themselves were extracted into
     `prof.h` in 11.0a; this step only moves GL calls.)
   - After the move, `display_func()` calls `render_3d_scene()` once,
     period. No knowledge of accumulation.

6. **Trim `display_func()` to pure orchestration.** What stays in
   `repl_core.c` after step 5:

   - autonormal/flatten dirty-bit handling (model bookkeeping -
     stays),
   - replay PC clamp via `repl_state_flat_program_set_count(...)`
     (model - stays),
   - `update_render_state_strings()` / `update_cam_lines()` calls
     (string scaffold - **stays where it is**; these live in
     `repl_export.c`, produce text scaffold not GL, and have other
     non-display callers. Not a GL-refactor concern; flagged so the
     implementer doesn't get distracted),
   - per-frame predef-value snapshot/restore (model - stays),
   - one call to `render_3d_scene()`,
   - the 2D overlay sequence (`render_code_panel`,
     `ui_autocomplete_panel_render`, `render_example_dropdown`,
     `render_var_panel`, `render_scene_status`, `render_help`,
     `render_profile_panel`).

   What moves out:

   - The 3D-side `glViewport(0, 0, window_w, window_h)` at
     `repl_core.c:624` becomes the first line of
     `render_3d_scene()`.
   - The 2D-side `glViewport` at `repl_core.c:677` becomes the
     first line of `render_code_panel()`. This mirrors
     `render_3d_scene()` setting its own viewport - each renderer
     states explicitly what viewport it draws into. Subsequent 2D
     overlays (autocomplete, dropdown, var panel, help, profile)
     inherit that viewport, which matches today's behavior. No new
     `ui_panels_begin_overlays()` helper is introduced. Document
     the convention in `render_code_panel()`'s top-of-function
     comment.
   - `glutSwapBuffers()` at `repl_core.c:697` is GLUT framework
     plumbing, not GL drawing. Move to `sample.c`'s GLUT
     `display_func()` wrapper, right after `repl_display_func()`
     returns. That keeps the buffer swap at the GLUT-callback
     boundary, which is its natural home.

   **Keep the `repl_display_func()` wrapper.** After this slice,
   `repl_display_func()` is a one-liner that just dispatches to
   the orchestrator body. Keep the wrapper anyway - it preserves
   `repl_core.c`'s public-API role (`sample.c` only knows about
   `repl_*`, not `scene_render.h`).

7. **Move `init_gl`'s `glLightModelfv` into `scene_render.c → scene_lights.c`.**
   Add a `scene_render_init_gl()` (one-shot) in `scene_render.c`,
   called from `repl_init_gl()`. `scene_render_init_gl()` delegates
   the ambient-light setup to a new
   `scene_lights_init_global_ambient()` in `scene_lights.c`; the
   existing per-frame `scene_lights_setup()` keeps its current
   responsibility. After the move:
   - `repl_init_gl()` in `repl_core.c` has zero GL calls - it calls
     `repl_executor_init_resources()` (from 11b),
     `scene_render_init_gl()`, and `apply_init_bootstrap()`.
   - `scene_render.c` becomes the owner of one-shot scene-side GL
     setup, mirroring its existing ownership of per-frame scene
     orchestration. Future one-shot scene init goes there too.
   - The `{0.15, 0.15, 0.20, 1.0}` constants live next to the rest
     of light state in `scene_lights.c`, not in `repl_core.c`.

8. **Verification grep.** After 11a lands:
   `grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*\s*\(' repl_core.c`
   should be empty. The orchestration role of `repl_core.c`'s
   `display_func()` is preserved - it still drives the
   `render_3d_scene()` → 2D-overlay-sequence pipeline - but no GL call
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
accumulation AA on (default) and off (`--noaccum`) - the AA jitter
loop is the highest-risk piece. Confirm `glutSwapBuffers` still
runs exactly once per frame (a quick `printf` instrumented in
`sample.c`'s wrapper, removed before commit, is the easiest check).
A/B against the pre-refactor binary on the replay HUD and
ruler-grid labels - those are the two `scene_*` callers of the old
`begin_2d`/`draw_string` and the most likely places visual output
could drift. `make test_ui` should pass unchanged (it exercises
`render_help()` and other public `ui_*` renderers; the `gl2d_*`
rename is internal to those renderers).

#### 11b. Move tessellation/GLU resources into `repl_executor.c` ✅ DONE

**Problem.** `repl_state.c` owns the static `g_quadric` / `g_tess`
storage, the 5 GLU tess callbacks, and the
`gluNewQuadric`/`gluNewTess` bootstrap pair. `repl_state.c` is the
typed runtime-state facade - it should own *storage*, not GL
resource lifetimes.

**Layering check.** `gluSphere`, `gluCylinder`, `gluDisk`,
`gluPartialDisk`, `gluTessBeginPolygon`, `gluTessVertex`, etc. are
all called from `repl_executor.c` - the executor is the *only*
consumer of `g_quadric` / `g_tess`. No `scene_*` or `ui_*` module
reads these resources. The right home is `repl_executor.c`
itself: putting them in a hypothetical `scene_resources.c` would
force `repl_executor.c` to include `scene_*.h`, inverting the
layering (the executor is the pipeline layer below scene
rendering - scene calls executor, not the other way around).

**Status.** The move landed. `repl_executor.c` now owns the quadric/
tessellator storage, callback wiring, and public init/destroy API;
`repl_state.c` is back to facade-only runtime state.

**Files this slice touches:** `repl_state.{c,h}`,
`repl_executor.{c,h}`, `repl_core.c` (one caller), `sample.c`
(one teardown caller), `MODULES.md`, `ARCHITECTURE.md`.

**Naming pass (separate `refactor:` commit, lands before the
move).** Focused rename on the files listed above, applying §10
conventions to touched lines. Pure rename, no logic changes.
`make test-stubs TEST_JOBS=4` stays green. Commit as
`refactor: normalize names for §11b prep`.

**Move (the second `refactor:` commit):**

1. Move from `repl_state.c` into `repl_executor.c`:
   - the static `g_quadric`, `g_tess`, `g_tess_verts[]`,
     `g_tess_vert_count` storage (and corresponding externs in
     `repl_state.h`),
   - the 5 tess callbacks `repl_render_tess_vtx_begin_cb`,
     `repl_render_tess_vtx_end_cb`, `repl_render_tess_vtx_cb`,
     `repl_render_tess_comb_cb`, `repl_render_tess_err_cb`
     (`repl_state.c:389-450ish`),
   - `repl_state_render_init_resources()` and
     `repl_state_render_destroy_resources()`
     (`repl_state.c:1066-1106`).
2. **Hard rename, no compat shim.** Rename the public entrypoints
   to `repl_executor_init_resources()` /
   `repl_executor_destroy_resources()` in `repl_executor.h`.
   Update both callers in the same commit: `repl_init_gl()` in
   `repl_core.c` and the teardown site in `sample.c`. No alias
   layer - two callers is too few to justify the overhead.
3. Internal access to `g_quadric` / `g_tess` is now file-static in
   `repl_executor.c`. No header export needed. If anything outside
   the executor ever needs them in future, expose typed getters
   rather than re-introducing externs.
4. **Leave `TessVertex` in `repl_state.h` for now.** The typedef
   is referenced by `repl_executor.c` (the dispatch path at
   `repl_executor.c:328-334`) and emitted as a string by
   `repl_export.c:1529-1543`. Moving it is unnecessary churn for
   this slice - revisit only if a later state-clustering refactor
   wants the type local to its consumer.
5. After the move, `repl_state.c` should have **zero** live GL/GLU
   calls. The string-literal hits at `repl_state.c:178-186` (the
   `g_render_state_lines` / camera scaffold strings) are not live
   calls and stay where they are.
6. `repl_state.h` loses the `g_quadric` / `g_tess` / `g_tess_verts`
   / `g_tess_vert_count` externs (but keeps `TessVertex`, per
   step 4).
7. Stubs in `include/GL/glu.h` are unchanged - the GLU symbols
   already linked in `repl_state.c`'s build now link in
   `repl_executor.c`'s instead. The `USE_GL_STUBS=1` build needs
   no stub additions, just the moved object file.

**Verify:** `make test-stubs TEST_JOBS=4` (the tess callbacks must
still link in stub mode); `make sample && ./sample`; load an
example that exercises tessellation (the polygon-with-holes tess
examples) and one that exercises quadrics
(`gluSphere`/`gluCylinder` calls); exit cleanly via window close
so `repl_executor_destroy_resources()` fires.

#### 11c. Replace inline GL helpers in `sample.h` with `repl_executor.h` exports ✅ DONE

**Status.** This slice landed in the 11c cleanup commit. `repl_executor.c` now owns the transform helper bodies and `sample.h` is back to types-and-compatibility only, with the point-size shim moved out of the shared header.

**Validation.** `make test-stubs TEST_JOBS=4` passed, along with the requested build matrix (`make sample`, `make sample USE_GL_STUBS=1`, `make sample NO_POINT_PARAMETER=1`) and `./bench_repl fade_batches`.

**Problem.** `sample.h` is the shared *types* and compatibility
header, but it currently defines `static inline` helpers that
expand to GL calls in every translation unit:
`apply_transform_cmd`, `apply_tracked_transform_cmd`,
`unwind_tracked_transform_stack` (`sample.h:296-333`), and the
`_repl_point_size` `NO_POINT_PARAMETER` shim (`sample.h:386-395`).
Every `.c` including `sample.h` then drags GL symbols into its TU,
defeating the layering rule.

**Layering check.** `apply_transform_cmd` and
`apply_tracked_transform_cmd` are called by `repl_executor.c`
(dispatch) and by two `scene_*` modules
(`scene_transform_guides.c`'s `compute_before_cursor_matrix`,
`scene_overlays.c`'s flat-cmd walk). `scene_*` already includes
`repl_executor.h` (because `scene_render.c` calls
`repl_execute_program`), so the dependency direction is
**scene → executor** - pipeline-below-scene, which matches the
layering. No new coupling is introduced by exporting these from
`repl_executor.h`. There is no `ui_*` consumer.

**Files this slice touches:** `sample.h`, `repl_executor.{c,h}`,
`scene_transform_guides.c`, `scene_overlays.c`, `MODULES.md`,
`ARCHITECTURE.md`.

**Naming pass (separate `refactor:` commit, lands before the
move).** Focused rename on the files listed above, applying §10
conventions to touched lines. Pure rename, no logic changes.
`make test-stubs TEST_JOBS=4` stays green. Commit as
`refactor: normalize names for §11c prep`.

**Move (the second `refactor:` commit):**

1. Keep `is_transform_cmd` where it is - it's a pure
   command-classifier with no GL, so it can stay in `sample.h` or
   move to `repl_executor.h` if you prefer. No behavior risk.
2. Move `apply_transform_cmd`, `apply_tracked_transform_cmd`, and
   `unwind_tracked_transform_stack` (`sample.h:296-333`) into
   `repl_executor.c` and export from `repl_executor.h`. Convert
   from `static inline` to ordinary functions. Call sites:
   - `repl_executor.c`'s own dispatch - was inline-via-include,
     becomes a direct call within the same TU.
   - `scene_transform_guides.c` `compute_before_cursor_matrix` -
     already includes `repl_executor.h` transitively; no new
     coupling.
   - `scene_overlays.c` flat-walk - same.
3. Replace `_repl_point_size` (`sample.h:386-395`) with a static
   helper inside `repl_executor.c` and scope the `glPointSize`
   macro override to that TU only:
   - Move the `#ifdef NO_POINT_PARAMETER` block out of `sample.h`
     and into `repl_executor.c`. The override is for the
     `case CMD_POINT_SIZE:` dispatch path only - that's the only
     *user-facing* `glPointSize` issued by the REPL language.
   - All other `glPointSize` call sites in `scene_*` modules
     (`scene_geometry_guides.c`, `scene_overlays.c`,
     `scene_axes.c`, `scene_lights.c`) are render-internal
     helpers that already chose their own size and don't want
     the attenuation hack - confirmed by the comment at
     `sample.h:386-395`. Scoping the shim to the executor is a
     behavior-preserving win.
4. **Leave `gl_includes.h` in `sample.h`.** It is required -
   `sample.h` declares `GLfloat`, `GLenum`, and `GLCmd` (which
   contains GL types). The point of 11c is to keep
   `static inline` GL *function calls* from expanding into every
   TU; type declarations are fine. Do not attempt trial removal -
   the include is load-bearing.
5. After the move, `sample.h`'s GL-call grep should hit zero
   (only type uses remain). `sample.h`'s line count drops by the
   ~50 lines of inline helpers + macro shim.

**Verify:** `make test-stubs TEST_JOBS=4`; build all three flag
combinations: `make sample`, `make sample USE_GL_STUBS=1`, and
`make sample NO_POINT_PARAMETER=1` (the punch-list intersection).
Run `./bench_repl fade_batches` - it exercises tracked transforms
via the executor and the scene_* walkers.

#### 11d. Move replay-fade GL pass into `scene_render.c` ✅ DONE

**Problem.** `repl_replay.c` should be the replay *model* (state
machine, fade-batch ring, alpha math, PC tracking). The actual GL
pass at `repl_replay.c:773-809` (push/pop attrib, lighting/blend
setup, `glColor4f`, push/pop matrix wrapping the executor call) is
render-side work in the wrong file.

**Layering check.** The fade pass is pure 3D scene rendering - no
`ui_*` involvement. `scene_render.c` already calls
`execute_replay_fade_batches()` from inside its frame, so moving
the GL pass there preserves the layering: `scene_render.c` reads
model data from `repl_replay.c`. Direction stays
**scene → replay model** (read-only).

**Files this slice touches:** `repl_replay.{c,h}`,
`scene_render.{c,h}`, `bench_repl.c`, `sample.h`, `MODULES.md`,
`ARCHITECTURE.md`.

**Naming pass (separate `refactor:` commit, lands before the
move).** Focused rename on the files listed above, applying §10
conventions to touched lines. Pure rename, no logic changes.
`make test-stubs TEST_JOBS=4` stays green. Commit as
`refactor: normalize names for §11d prep`.

**Move (the second `refactor:` commit):**

1. Split `execute_replay_fade_batches()` into two halves:
   - **Model side** (stays in `repl_replay.c`): the `skip_limits[]`
     computation loop at lines 735-771 - pure data, no GL. Expose
     as `replay_compute_fade_skip_limits(int *out_limits, int max_count)`
     returning the actual count. Also keep `replay_batch_alpha()`,
     `repl_execute_set_fade_context()`, and
     `replay_has_active_fades()` where they are.
   - **Render side** (moves to `scene_render.c` as a new
     `render_replay_fade_pass()` adjacent to the existing fade-pass
     setup at `scene_render.c:512-530`): the `glPushAttrib` /
     per-batch `glDisable(GL_LIGHTING)` / `glEnable(GL_BLEND)` /
     `glBlendFunc` / `glPolygonMode` / `glColor4f` / `glPushMatrix`
     / `glPopMatrix` / `glPopAttrib` block plus the per-batch
     `repl_execute_program(&exec_options)` call.
2. The existing fade-pass setup in `scene_render.c:512-530`
   already primes lighting, blend, and polygon mode, then
   immediately calls `execute_replay_fade_batches()`. After the
   move, both blocks live in one place - collapse the duplicated
   setup so we only set blend/polygon mode once around the loop,
   not once per batch. This is the payoff.
3. Delete the now-empty `execute_replay_fade_batches()` wrapper
   from `sample.h:375` and `repl_replay.h:29`. Callers:
   - `scene_render.c` calls `render_replay_fade_pass()` directly.
   - `bench_repl.c:549` calls `execute_replay_fade_batches()` for
     its GL-call count harness - it needs to drive the fade pass
     without the surrounding frame setup. **Decision: expose
     `render_replay_fade_pass()` from `scene_render.h`
     unconditionally.** Do not gate it behind
     `#ifdef BENCH_REPL_HARNESS` or any other compile flag. The
     bench is part of the source tree, just a different
     entrypoint that drives one render pass. Conditional public
     declarations split readers between two interpretations of
     the same header. The upward-shim alternative (a thin
     `execute_replay_fade_batches()` shim in `repl_replay.c`
     delegating to scene) is rejected because it re-introduces
     `replay → scene_render` upward coupling.
   - The bench's stdout strings at `bench_repl.c:531,564`
     reference `execute_replay_fade_batches` - update them to
     name the new entrypoint so log readers can find the symbol.
4. After the move, `repl_replay.c` should have **zero** live GL
   calls. `repl_replay.h` should drop any `gl_includes.h` include
   if no remaining declaration needs it.

**Verify:** `make test-stubs TEST_JOBS=4`; the bench harness
`./bench_repl fade_batches` GL-call count should be **strictly
less than or equal to** the pre-refactor number (the per-batch
redundant `glDisable(GL_LIGHTING)`/`glEnable(GL_BLEND)`/
`glBlendFunc`/`glPolygonMode` calls collapse to one). Capture the
before/after counts in the commit message.
`make sample && ./sample`: load an example with heavy geometry,
Ctrl+G to start replay, hold Right arrow - the trailing ghost
should look identical.

**Status.** This slice landed in commit `d513dd9` as a replay-model/render
split. `repl_replay.c` now owns the fade-batch ring, alpha math, and
skip-limit helpers; `scene_render.c` owns `render_replay_fade_pass()`;
`bench_repl.c` drives the scene-side helper directly; and the architecture
docs now describe the blended replay pass as scene-owned.

**Validation.** `make test-stubs TEST_JOBS=4` passed, and
`make bench USE_GL_STUBS=1 BENCH_ARGS="--only fade_batches"` passed with the
new helper name in the benchmark output.

#### 11e. Funnel `repl_editor.c` and `repl_actions.c` GLUT calls through helpers ✅ DONE

**Problem.** `repl_editor.c` makes ~22 direct GLUT calls
(`glutPostRedisplay`, `glutSetCursor`, `glutGetModifiers`), and
`repl_actions.c:169` makes one (`glutGetModifiers` for a SHIFT
check). GLUT input/feedback isn't GL drawing, but the call sites
should still be funnelled through one helper per primitive so the
GLUT surface is reviewable in one place and `repl_actions.c` stops
including `<GL/freeglut.h>` directly.

**Layering check.** GLUT input/feedback (post-redisplay, cursor,
modifiers) is editor/input layer, not rendering. No
`scene_*`/`ui_*` coupling involved.

**Files this slice touches:** `repl_editor.{c,h}`, `repl_actions.c`,
`MODULES.md`, `ARCHITECTURE.md`.

**Naming pass (separate `refactor:` commit, lands before the
move).** Focused rename on the files listed above, applying §10
conventions to touched lines. Pure rename, no logic changes.
`make test-stubs TEST_JOBS=4` stays green. Commit as
`refactor: normalize names for §11e prep`.

**Move (the second `refactor:` commit):**

1. Add two helpers to `repl_editor.c` (private - just `static`):
   - `static void editor_request_redraw(void)` wrapping
     `glutPostRedisplay()`.
   - `static void editor_set_cursor(int cursor)` wrapping
     `glutSetCursor()`.
   Replace all 21 `glutPostRedisplay()` / `glutSetCursor(...)`
   call sites inside `repl_editor.c` with the helpers. There is
   no cross-module API change; this is purely a single-funnel
   refactor.
2. The two `glutGetModifiers()` queries - one in
   `repl_editor.c:75`, one in `repl_actions.c:169` - are GLUT
   inputs, not drawing. Move them behind a single helper
   `repl_editor_active_modifiers()` exported from `repl_editor.h`.
   `repl_actions.c` calls the helper instead of including GLUT
   headers directly. After this, `repl_actions.c` has zero
   `gl[uU][A-Z]` matches.
3. Update the "Naming Notes" exception list in `MODULES.md:359-365`
   to mention that `repl_editor.c` calls GLUT *input/feedback*
   APIs through the funnel helpers and that this is the only
   `repl_*` file allowed to include `<GL/freeglut.h>` for input -
   distinct from the `repl_executor.c` exception, which is
   allowed to call GL drawing APIs.

**Verify:** `make test-stubs TEST_JOBS=4`; `./sample` cursor
changes across the code-panel resize divider, undo/redo redraws,
scrolling redraws, and SHIFT-clicking the time toggle (Ctrl+T
with SHIFT) all behave as before.

**Completion.** Landed as `refactor: funnel GLUT calls through helpers in repl_editor`.
- Added `repl_editor.h` with public `repl_editor_active_modifiers()` function
- Created static helpers `editor_request_redraw()` and `editor_set_cursor()` in repl_editor.c
- Replaced ~23 direct GLUT calls with funnel helpers across repl_editor.c
- Updated repl_actions.c to call `repl_editor_active_modifiers()` instead of glutGetModifiers()
- All 2503 tests pass. Visual cursor and redraw behavior verified.

#### 11f. Lock both boundaries with grep guards ✅ DONE

**Problem.** Without an enforcement mechanism, the rules from
11.0a-11e will erode the next time someone adds a quick GL call
in the wrong file. Two rules are now enforceable:

- **GL/GLUT layer rule:** only `scene_*.c`, `ui_*.c`, and
  `repl_executor.c` issue GL/GLU drawing calls; only `sample.c`
  and `repl_editor.c` issue GLUT input/feedback calls.
- **Cross-layer coupling rule:** `ui_*` and `scene_*` do not
  include each other's headers, with one grandfathered exception
  (see step 2 below).

**Files this slice touches:** `Makefile`, `MODULES.md`,
`ARCHITECTURE.md`. No `.c` source code is modified - this is the
guard slice.

**Naming pass:** none. This slice doesn't touch `.c` files, so
there's no per-slice rename pass. The accompanying doc edits in
`MODULES.md` and `ARCHITECTURE.md` are themselves the
documentation update.

**Move (a single `refactor:` commit):**

1. Add a `make check-gl-boundaries` target that runs three greps
   and fails if any returns matches:
   - **GL/GLU drawing:**
     `grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*\s*\(' repl_*.c | grep -v '^repl_executor\.c:' | grep -v ':\s*[/*"]'`
     (exclude executor and lines that look like
     comments/strings).
   - **GL in sample.h:** same grep against `sample.h` should be
     empty (transform helpers / point-size shim moved out in 11c).
   - **GLUT input/feedback:**
     `grep -nE '\bglut[A-Z][A-Za-z0-9]*\s*\(' repl_*.c | grep -vE '^repl_(editor|executor)\.c:' | grep -v ':\s*[/*"]'`
     (only editor and executor may name GLUT symbols).
2. Add a `make check-layer-coupling` target:
   - `! grep -nE '#include\s+"scene_' ui_*.c ui_*.h` - **UI must
     not include any scene header.** Hard rule, no exceptions.
   - `! grep -nE '#include\s+"ui_' scene_*.c scene_*.h | grep -vE 'scene_render\.c:.*ui_panels\.h'` -
     scene must not include UI headers, with one grandfathered
     `scene_render.c → ui_panels.h` coupling for `scene_rect()`.
     (The previous `scene_render.c → ui_profile_panel.h` coupling
     went away after 11.0a.) The `scene_rect()` exception is
     **left in place indefinitely**; see step 4 below.
3. Wire both targets into `make test` and the `make test-stubs`
   umbrella so a regression is caught on first push.
4. **Document the `scene_rect()` exception as a known limitation,
   not a near-term TODO.** The deeper fix is structural: scene
   and UI layers should ideally have **no runtime configuration
   of their own** - they should render based on config provided
   by `repl_*`. `scene_rect()` is one symptom of UI owning layout
   that scene also needs to read. Resolving it properly means
   moving viewport/scene-rect derivation into a `repl_*` config
   model that both layers consume. That's a *separate* later
   refactor, not part of step 11 and not blocked on. Add a single
   line to `MODULES.md` Open Edges noting the long-term direction
   ("scene/UI should consume layout config from `repl_*`, not own
   it"), but do not assign a date or block this slice on it.
5. Document the rules in `MODULES.md` under "Naming Notes".
   One-line summaries:
   - *"`repl_*.c` files don't call GL or GLU. Only
     `repl_executor.c` (drawing) and `repl_editor.c` (GLUT input
     via local funnel helpers) are exceptions."*
   - *"`ui_*` and `scene_*` are independent layers. Neither
     includes the other's headers. The
     `scene_render.c → ui_panels.h` include for `scene_rect()`
     is a grandfathered exception; eliminating it is a future
     refactor that moves layout config into a `repl_*` model."*

**Verify:** both new make targets pass on the cleaned tree and
fail if any boundary line from 11.0a-11e is reintroduced -
confirm by temporarily reverting one move from each sub-step and
running the targets. `make test-stubs TEST_JOBS=4` must stay
green.

**Completion.** Landed as `refactor: add GL/GLUT boundary and layer-coupling enforcement targets`.
- Added `make check-gl-boundaries` target: grep guards verify GL/GLU calls only in scene_*.c, ui_*.c, repl_executor.c; GLUT input calls only in sample.c, repl_editor.c
- Added `make check-layer-coupling` target: grep guards verify ui_* doesn't include scene_* headers and vice versa (grandfathered exception: scene_render.c → ui_panels.h)
- Integrated both targets into `.PHONY` and `test` target for continuous verification
- Grep patterns filter out string literals and comments to avoid false positives
- All 2503 tests pass and both guard targets report OK

**Follow-up.** R11 in
[`feature/push-architecture-refinement.md`](./push-architecture-refinement.md)
continues this work. It hardens the Makefile grep targets, adds
state-boundary checks, and explicitly allows transitional leaks only while the
dependent Phase 2 slices remain open. Do not treat those allowlists as
completion criteria; shrink them in the same commits that finish R2/R4/R6.

---

**Cumulative verification umbrella for item 11.** Each commit lands
independently and is verifiable, but the full sequence should also be
re-checked at the end:

- `make test-stubs TEST_JOBS=4` - all 19 suites green.
- `make sample && ./sample` - load the cube, orbit/pan/zoom, F12 cycle
  through every example, replay (Ctrl+G) a heavy example, accumulation
  AA on and off, multi-scene workspace save/load, Ctrl+T time toggle
  with and without SHIFT.
- `make sample USE_GL_STUBS=1` - link must succeed without any new
  stub gaps.
- `make sample NO_POINT_PARAMETER=1` - the relocated point-size shim
  still applies the attenuation only to the user-facing
  `glPointSize` command.
- `bench_repl fade_batches` GL-call count: equal or lower than before.
- `make check-gl-boundaries` - empty.
- `make check-layer-coupling` - empty modulo grandfathered exceptions.
- `make check-state-boundaries` - passes with only documented transitional
  allowlists while Phase 2 is in progress.


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
