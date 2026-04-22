# REPL Refactor Punch List

## Context

The companion doc `feature/repl-cleanup.md` is the strategic 10-stage
ownership-reorganization plan. This punch list is the tactical
counterpart: a prioritized set of concrete, behavior-preserving
extractions that can each land as a single reviewable commit *today*,
without committing to the larger context-object rewrite.

Four files still dominate the codebase: `scene_render.c` (~2586, now with
frame prep/theme specs/state guards/overlay visitor helpers while delegating
backdrop, light, and outline helpers), `repl_export.c` (2541),
`repl_core.c` (~1964), and `repl_editor.c` (~1653). `ui_panels.c`
dropped to 1237 LoC (from 4452) after Phase-7
extractions: document rows, replay annotations, menu/dropdowns, color
picker, help overlay, variable panel, autocomplete popup, inline rename,
and variable dragging. The highest-value remaining refactors fall into
two camps:

1. **Mechanical extractions** — self-contained features still living in
   the wrong file. Low risk, immediate LoC reduction, no behavior
   change.
2. **Pattern consolidation** — repeated boilerplate (per-theme switch
   cases, per-overlay traversal loops, repeated GL-pass emit blocks)
   that hurts every time someone adds a new theme/overlay.

This list is ordered by impact-per-effort. Pick one and execute it
in isolation; each item is sized to land as one `refactor:` commit.

> **Baseline note:** `make test-stubs TEST_JOBS=4` currently passes
> all 17 suites / 2380 tests cleanly. Any new failure introduced by a
> punch-list item is a real regression unless the team explicitly
> rebaselines it.

---

## Tier 1 — Mechanical extractions (hours each, near-zero risk)

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
dedicated render pass — the buffer is surfaced through
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

## Tier 2 — Pattern consolidation (1–2 days, medium impact)

### 4. Data-drive grid + axes themes in `scene_render.c` ✅ DONE

Implemented in the Phase-8 render cleanup slice. Standard grid themes now
flow through `GridThemeSpec` entries plus a shared line/origin-axis renderer;
focus, ocean, ruler extras, and adaptive planes remain custom where they
draw extra geometry. Axes now have `AxesThemeSpec` entries and shared
triplet helpers for X/Y/Z lines, tips, and labels, so adding a normal theme
starts with table data instead of a copied switch body.

The Focus grid no longer chooses or writes its focus vertex while drawing.
`FrameRenderContext` prepares the sticky focus vertex before helper rendering,
so the theme renderer consumes prepared state like the standard table-driven
themes. The Ocean grid also consumes prepared camera waterline state; the old
inline camera-height TODO was replaced with a single frame-prep derivation of
the orbit camera's world-space Y.

### 5. Vertex overlay visitor in `scene_render.c` ✅ DONE

Implemented in the Phase-8 render cleanup slice. `walk_vertex_overlay()`
centralizes transform replay, begin/end/tessellation block tracking, current
normal state, and cursor-selected block filtering. `draw_vertex_numbers()`
and `draw_normal_vectors()` are now small callbacks over that traversal.
The partial-input guides still have their own parsing/search logic because
they are driven by live `g_input`, not by the flattened command stream.

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
explicit `scene_overlay_flat_block_matches_cursor()` helper shared by the
outline pass and the remaining vertex-number/normal overlay visitor in
`scene_render.c`.

### 5f. Widen render config/frame context ✅ DONE

Implemented in the Phase-8 residual render cleanup slice. `scene_render.c`
now builds a private `SceneRenderConfig` at the start of `render_3d_scene()`
for scene rectangle, camera, accumulation jitter, quality toggles, grid/axes
choices, guide/vertex overlay toggles, replay mode, replay tess preview, and
replay fill-base limit. `FrameRenderContext` carries that config plus prepared
derived state such as Focus-grid vertex and ocean-grid waterline facts. The
projection/camera setup, grid/axes helpers, orbit target, replay outlines,
replay fade pass, guide drawing, vertex-point overlay, and replay HUD now
consume the explicit config instead of independently sampling those globals.

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

## Tier 3 — Remaining structural extraction

The editor-adjacent Phase 7 mechanical extractions are now complete.
`MODULES.md` (Open Edges) still lists smaller residual ownership edges in
`ui_panels.c` and `repl_editor.c`, but they are mostly routing or inline
row-rendering concerns. Phase 9 now has its main scaffold/import slices in
place. The next large mechanical extraction, parser ownership, is now done.

### 8. Extract `parse_command()` → `repl_parser.c` — done

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

---

## Not recommended for now

- **Global state clustering** (group `repl_state.h`'s 127 externs into
  context structs): high long-term value but enormous churn surface.
  This is what stage 2 of `repl-cleanup.md` plans more carefully —
  defer to that effort rather than attempting it as a punch-list
  item.
- **Keyboard dispatch table** (collapse the 22 `handle_*_key_route`
  functions in `repl_editor.c` into a table): the chain *is* the
  ordering policy and reading it sequentially is fairly clear today.
  Lower payoff than the active Tier 2/3 cleanup items.

---

## Verification umbrella

For any item picked:

1. `make test-stubs TEST_JOBS=4` — all 17 suites should pass cleanly.
2. `make sample && ./sample` — load an example, exercise the
   touched feature, confirm no visual or interactive regression.
3. For Tier 2 rendering changes (items 4 & 5), do an explicit A/B
   against the pre-refactor binary on every theme/overlay — diff
   review alone is not enough for pixel-level equivalence.
4. Single commit per item, conventional `refactor:` prefix per
   project CLAUDE.md.
