# Immediate-Mode REPL Refactoring Cleanup Plan

## Status (2026-04-29)

Stages 1–11 of this plan have all landed at the strategic level:

- ✅ Stage 1 — baseline stable; all 2541 tests pass.
- ✅ Stage 2 — globals consolidated into `ReplRuntimeState` (one struct,
  one owner). Continued under `feature/gold-standard-state-ownership.md`.
- ✅ Stage 3 — `ReplCommandStore` owns array-level mutation; commit /
  clipboard / autonormal / import / examples / undo paths route through it.
- ✅ Stage 4 — parser ownership in `repl_parser.c`; command spec table in
  `repl_command_spec.c`; source scope/indentation in `repl_source_scope.c`.
- ✅ Stage 5 — `repl_flatten_program()` takes explicit options;
  `repl_execute_program()` runs through `FlatProgramView`.
- ✅ Stage 6 — editor input is a mode router with focused commit helpers.
- ✅ Stage 7 — UI layout and rendering split into focused modules
  (`ui_panels`, `ui_menu_bar`, `ui_color_picker`, `ui_help_overlay`,
  `ui_variable_panel`, `ui_autocomplete_panel`, `ui_profile_panel`,
  `ui_replay_hud`).
- ✅ Stage 8 — `SceneRenderConfig` / `FrameRenderContext` are explicit;
  scene rendering is split into `scene_grid`, `scene_axes`, `scene_backdrop`,
  `scene_lights`, `scene_overlays`, `scene_geometry_guides`,
  `scene_transform_guides`.
- ✅ Stage 9 — workspace header / import handlers / typed export scaffold
  refactor landed.
- ✅ Stage 10 — naming pass + comprehensive per-header documentation done.
- ✅ Stage 11 — live GL calls are isolated to `scene_*.c`, `ui_*.c`, and
  `repl_executor.c`; GLUT input is funnelled through `repl_editor.c`
  helpers; `make check-gl-boundaries` / `make check-layer-coupling` enforce
  the boundary.

Outstanding items now live in `feature/push-architecture-refinement.md`
(controller-first Phase 2 — R10 / R11 / R12 / R8) and in
`feature/gold-standard-state-ownership.md` (state-ownership Stages 4–8).
This document is retained as the high-level history; new strategic
direction should land in those active feature docs.

## Summary
The main cleanup target is not one bad file; it is the lack of clear ownership between state, command parsing, editor input, UI layout, rendering, replay, and import/export. The current hotspots are [sample.h](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/sample.h:338), [repl_core.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c:1595), [repl_editor.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_editor.c:2003), [ui_panels.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/ui_panels.c:2538), [scene_render.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/scene_render.c:23), and [repl_export.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_export.c:2457).

Refactor in small behavior-preserving stages. The first goal is to make responsibilities explicit and contain side effects; only then split large files, rename internals, and add comments around real invariants.

The controller-first architecture and Phase 2 dependency ordering are tracked in
[`feature/push-architecture-refinement.md`](./push-architecture-refinement.md).
Use that plan for R1-R12 boundaries, Makefile guard timing, and the deferred
`sample` → `imrepl` namespace work. This cleanup plan stays strategic and
should not diverge from the refinement plan's current ordering.

Baseline note: the historical pre-refactor failures in `test_repl_core_format`,
`test_repl_core_commit`, and `test_repl_core_examples` have been fixed on this
branch. `make test-stubs TEST_JOBS=4` is now expected to pass cleanly; any new
failure should be treated as a regression unless explicitly rebaselined.

## Key Interface Changes
- Keep the user-facing REPL language, file format, GLUT entrypoint, and sample-local structure unchanged.
- Shrink `sample.h` over time to shared types/constants plus small compatibility includes. Move broad `extern g_*` state into domain headers or state access APIs.
- Add internal ownership APIs:
  - `ReplCommandStore` / `repl_command_store_*` for `g_cmds`, `g_num_cmds`, edit line, insertion, deletion, replacement, dirty invalidation, and declaration bookkeeping.
  - `repl_parse_line()` and command descriptor metadata for parser behavior.
  - `repl_flatten_program()` with explicit `ReplFlattenOptions` /
    `ReplFlattenResult`.
  - `repl_execute_program()` with an explicit program range/options object.
  - `CodePanelLayout` for wrapping, hit-testing, rendering, and visual dump output.
  - `SceneRenderConfig` / `FrameRenderContext` for camera, toggles, replay limits, jitter, and render-time options.

## Staged Refactor
1. **Stabilize and document the baseline**
   - Confirm the current stub-test baseline first, or record any accepted failing cases explicitly.
   - Update `ARCHITECTURE.md` with the intended ownership map: command store, parser, flattener, executor, editor, UI layout, scene renderer, import/export.
   - Acceptance: every later stage can compare against a known test baseline.

2. **Contain global state without changing behavior**
   - Group current globals into domain-owned structs: command/document state, editor state, presentation config, render view, replay state, UI state, import/export state.
   - Move declarations out of `sample.h` incrementally into focused headers. Leave compatibility aliases temporarily so call sites can migrate in small steps.
   - Add reset/init helpers for each domain to replace scattered manual global initialization.
   - Header design: `feature/repl-state-phase2-sketch.md` captures the
     resolved endpoint for `sample.h` and `repl_state.h`, including final
     context ownership, focused state APIs, compatibility policy, and reset
     ordering.
   - Current progress: `repl_state.h` exposes document and flat-program
     accessors. `repl_command_store.c` now gets live command storage through
     `ReplDocumentState`, and `repl_flatten.c` writes flat count, lighting, and
     current-block metadata through `ReplFlatProgramState` helpers. The source
     command, flat-program, editor-input, selection, and clipboard buffer
     definitions, plus camera/pointer/viewport storage and mutable presentation
     config storage, have moved to `repl_state.c`; compatibility externs remain
     until production readers are migrated. Immutable descriptor/name tables
     stay outside runtime state.

3. **Centralize command mutation**
   - Introduce a command-store API for insert, replace, delete, range delete, load, clear, and reformat updates.
   - Convert commit handlers, clipboard, examples, import, workspace load, undo/redo, and reformat paths to use that API.
   - Remove scattered direct mutation patterns such as manual `memmove`, direct `g_num_cmds++`, ad hoc declaration unregister/register, and forgotten dirty flags.
   - Current progress: `ReplCommandStore` owns insertion, replacement, range
     deletion, clearing, and bulk source-array restore/load. Commit paths,
     clipboard, autonormal updates, import declaration insertion, examples,
     scene switching/workspace load, undo/redo restore, and global reset now
     route array-level mutations through that boundary. Remaining direct writes
     are field-level semantic rewrites inside existing commands, such as
     color-picker/variable-drag value sync and declaration assignment-slot
     repair.

4. **Separate command parsing from core state**
   - Done: parser logic now lives in `repl_parser.c` behind the existing `repl_parse_command*()` entrypoints plus explicit `ReplParseContext` for source-line-sensitive parsing.
   - Done: `repl_command_spec.c` owns the command descriptor table for simple fixed-arity GL commands, enum arguments, normalized source formatting, and basic command properties.
   - Done: source prefix-depth and indentation ownership now lives in `repl_source_scope.c`.
   - Keep complex forms custom but isolated: declarations, assignment, loops, functions, conditionals, tessellation, `glMaterialf`, point parameters, comments, labels, and goto/replay metadata.
   - Use command metadata later for autocomplete, export, display classification, and execution dispatch.

5. **Split flattening and execution**
   - Done: flattening lives in `repl_flatten.c`, and
     `repl_flatten_program()` now accepts explicit source input, destination
     flat buffer, local-var snapshot buffer, capacity, recursion limits, visit
     budget, and result/status output. `repl_flatten_commands()` remains the
     live-global compatibility wrapper.
   - Done: flattening no longer mutates `g_edit_line` as temporary parse context; it passes `ReplParseContext.source_line_idx`.
   - Done: GL execution lives in `repl_executor.c`; `repl_execute_program()`
     receives a `FlatProgramView` and explicit range/options object. Replay
     passes execution limits instead of temporarily mutating `g_num_flat_cmds`.

6. **Make editor input a mode router**
   - Split `repl_editor.c` into focused areas: keyboard/mouse dispatch, commit pipeline, undo/redo, clipboard, camera controls, and config actions.
   - Replace the long `keyboard_func()` flow with an ordered handler chain: rename, search, replay/help/menu, command commit, code editing.
   - Use a `CommitResult` style return value so handlers report consumed input, status text, cursor movement, insert/overwrite behavior, and command-store edits consistently.
   - Rename local variables as touched, especially terse control-flow names like `fb`, `fe`, `ib`, `ie`, `ind`, `p`, and ambiguous `cmd`.

7. **Untangle UI layout, rendering, and hit-testing**
   - Extract pure code-panel wrapping/layout into `CodePanelLayout`; use it for rendering, mouse hit-testing, search positions, and export visual dumps.
   - Split `ui_panels.c` into focused modules: code panel, menu bar, config menu, color picker, variable panel, help overlay, replay annotations, and inline rename.
   - Keep action execution outside layout/render code. Menus should produce actions; editor/core code should perform mutations.

8. **Clean up scene rendering side effects**
   - Introduce a per-frame render context containing camera, jitter, config toggles, replay execution limit, and derived view data.
   - Move focus-vertex calculation out of grid drawing; drawing helpers should render from prepared state rather than update global editor state.
   - Add small GL state guard helpers or strict documented push/pop conventions for blend, lighting, depth mask, polygon mode, matrices, and line/point sizes.
   - Split large render helpers into grid themes, axes themes, overlays, lights, backdrop, and geometry execution support.
   - Current progress: `SceneRenderConfig` now snapshots the per-frame scene
     rect, camera, jitter, quality toggles, grid/axes settings, overlay toggles,
     and replay-derived limits before the frame renders. `FrameRenderContext`
     carries that config plus prepared derived data such as Focus-grid vertex
     state and ocean-grid camera waterline classification. Grid rendering now
     lives in `scene_grid.c`, axes rendering in `scene_axes.c`, and helper
     passes have explicit GL attribute guards. Backdrop/cityscape rendering now
     lives in `scene_backdrop.c`, and light setup plus light indicators live in
     `scene_lights.c`. Polygon outline/current-block highlight rendering,
     vertex-number overlays, and normal-vector overlays now live in
     `scene_overlays.c`, sharing one flat-command traversal and cursor-block
     matcher. Scene-edit guides now live in `scene_geometry_guides.c`
     (vertex/normal guides) and `scene_transform_guides.c`
     (translate/rotate/scale planning + rendering) via a per-frame
     `SceneGuideSnapshot`.
   - GL call segmentation boundary: live OpenGL calls should stay in the
     scene/UI renderers plus `repl_executor.c`. The remaining non-scene/UI live
     calls are a small orchestration residue in `repl_core.c`, tessellation and
     GLU bootstrap wiring in `repl_state.c`, and a couple of inline helpers in
     `sample.h`. Export/example files may still emit GL command text because
     that text is part of the REPL language, not the live call surface.

9. **Refactor import/export around a shared scaffold model**
   - Represent the generated C scaffold as typed sections rather than duplicated string/layout logic.
   - Use the same code-panel layout/wrap engine for visual dumps and on-screen rendering.
   - Split import into ordered handlers: workspace headers, camera block, declarations, functions, display body commands, and fallback comments.
   - Preserve single-file and workspace round-trip behavior.
   - Current progress: workspace header parsing/emission now share a
     directive table, `load_from_file()` dispatches through ordered import
     handlers, visual dumps already use the shared code-panel wrap iterator,
     and `save_output()` drives the generated file through a typed top-level
     scaffold section table plus typed `display()` pass helpers.

10. **Final naming/comment pass**
   - Add comments only where they explain invariants, ordering, ownership, or non-obvious side effects: commit handler order, declaration placement, dirty flags, replay limits, import/export markers, and GL state assumptions.
   - Remove stale comments and vague TODOs; turn real defects, such as the ocean-grid camera TODO, into specific tracked fixes.
   - Normalize names: `*_idx` for indexes, `*_count` for counts, `source_line_idx`, `flat_cmd_idx`, `indent_cols`, `visible_line_count`, `command_store`, `render_config`, and `workspace_dir`.
   - Normalize module prefixes by ownership, not habit. `repl_*` is for REPL
     language/editor/source/replay model modules; `imrepl_*` is the app shell,
     controller, and app-level services after the deferred rename; `scene_*`
     and `ui_*` stay view-layer names; generic utilities keep neutral names
     such as `prof`.
   - Audit legacy names that no longer match ownership. `repl_audio` is the
     obvious candidate: it is an app-level playlist/persistence service, not
     REPL command-model code. Schedule any rename with
     `feature/push-architecture-refinement.md` R8/R11 so source files,
     function prefixes, tests, docs, and Makefile guards change together.

10a. **Comprehensive public API header documentation** ✅ DONE
   - Every module header (`*.h`) now documents its public API consistently with module overview, lifecycle notes, type definitions, and detailed function descriptions.
   - Headers are stand-alone references: read any header from top to bottom to understand the module's API without consulting other headers or implementation files.
   - Documentation covers all 26 `repl_*` headers, 7 `ui_*` headers, and 8 `scene_*` headers.
   - `MODULES.md` now includes a "Header Documentation Standard" section explaining the consistent structure applied across all modules.
   - `ARCHITECTURE.md` updated to emphasize that comprehensive header documentation is the current API reference until R12 consolidation.
   - All function names follow the module-name convention: `repl_<module>_<action>()`, `ui_<module>_<action>()`, `scene_<module>_<action>()`.
   - Follow-up R12 changes the long-term shape: truly public REPL APIs should
     consolidate into one concise public header, while verbose per-module prose
     moves into implementation sections or module docs.
   - Completed in current session; all 2541 tests passing.

11. **Segregate live GL calls to scene/UI modules plus `repl_executor.c`**
   - Cross-cutting tactical follow-up to stages 7-9: the structural splits
     are now in place, but live GL calls still leak from a few
     pipeline-layer files (`repl_core.c`, `repl_state.c`, `repl_replay.c`)
     and from inline helpers in `sample.h`. Tighten the boundary so the
     only `.c` files that issue OpenGL/GLU drawing calls are `scene_*.c`,
     `ui_*.c`, and `repl_executor.c`.
   - Establish UI/scene independence as a hard rule: `ui_*` and `scene_*`
     do not include each other's headers; generic 2D primitives move to
     `include/gl_2d.h`, a project-agnostic header-only library alongside
     `gl_includes.h`/`stb_image.h`/`utils.h`.
   - Funnel the remaining GLUT input/feedback calls in `repl_editor.c`
     and `repl_actions.c` through local helpers so `glutPostRedisplay`,
     `glutSetCursor`, and `glutGetModifiers` appear in one place per
     module.
   - Lock both boundaries with `make check-gl-boundaries` and
     `make check-layer-coupling` grep guards wired into `make test`.
   - Continue the guard work through
     [`feature/push-architecture-refinement.md`](./push-architecture-refinement.md)
     R11. The active Phase 2 tree may need transitional allowlists; strict
     no-exception state checks should only be required once their prerequisite
     R2/R4/R6 cleanup slices have landed.
   - Tactical breakdown: see [`feature/repl-cleanup-punch-list.md`](./repl-cleanup-punch-list.md)
     §11. The punch-list lays out the work as a precursor Phase 1 (prof
     module extraction, per-slice naming) followed by Phase 2 (six
     `refactor:` commits 11a-11f).

## Test Plan
- After baseline stabilization: run `make test-stubs TEST_JOBS=4` after every stage.
- For parser/command-store work: run `make test_repl_core_parse`, `make test_repl_core_commit`, `make test_repl_core_format`, `make test_repl_core_io`, and `make test_repl_core_examples`.
- For evaluator/flattening work: run `make test_eval`, `make test_format`, and focused flatten/reformat tests.
- For UI layout extraction: add or update tests that compare wrapping, hit-testing, visual dumps, search positions, and continuation indentation.
- For import/export work: add round-trip tests for declarations, functions, config metadata, camera metadata, workspace scene names, and single-file reload.
- For render/replay work: run `make sample USE_GL_STUBS=1`, `make sample`, and a manual smoke test for replay, grid/axes themes, overlays, accumulation AA toggle, and example cycling.
- For architecture-boundary work: run `make check-gl-boundaries`,
  `make check-layer-coupling`, `make check-controller-boundaries`,
  `make check-scene-no-repl-state-mut`, and `make check-state-boundaries`.
  During Phase 2, `check-state-boundaries` may rely on documented transitional
  allowlists; shrink those allowlists as the corresponding refinement-plan
  steps land.

## Assumptions
- Preserve the current C/OpenGL/GLUT stack and sample-local structure.
- Prefer behavior-preserving internal refactors over feature changes.
- Do not introduce a shared engine or cross-sample framework.
- Keep output file compatibility unless a specific migration is planned.
- Work in small reviewable stages; no stage should combine broad file movement with semantic behavior changes.
