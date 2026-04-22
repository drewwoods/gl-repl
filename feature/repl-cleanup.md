# Immediate-Mode REPL Refactoring Cleanup Plan

## Summary
The main cleanup target is not one bad file; it is the lack of clear ownership between state, command parsing, editor input, UI layout, rendering, replay, and import/export. The current hotspots are [sample.h](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/sample.h:338), [repl_core.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c:1595), [repl_editor.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_editor.c:2003), [ui_panels.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/ui_panels.c:2538), [scene_render.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/scene_render.c:23), and [repl_export.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_export.c:2457).

Refactor in small behavior-preserving stages. The first goal is to make responsibilities explicit and contain side effects; only then split large files, rename internals, and add comments around real invariants.

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
   - Triage the existing stub-test failures first, or record them in a short baseline note with exact failing cases.
   - Update `ARCHITECTURE.md` with the intended ownership map: command store, parser, flattener, executor, editor, UI layout, scene renderer, import/export.
   - Acceptance: every later stage can compare against a known test baseline.

2. **Contain global state without changing behavior**
   - Group current globals into domain-owned structs: command/document state, editor state, presentation config, render view, replay state, UI state, import/export state.
   - Move declarations out of `sample.h` incrementally into focused headers. Leave compatibility aliases temporarily so call sites can migrate in small steps.
   - Add reset/init helpers for each domain to replace scattered manual global initialization.

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
   - Current progress: standard grid/axes themes now use local specs and
     shared draw helpers, helper passes have explicit GL attribute guards,
     vertex-number/normal overlays share one flat-command traversal helper, and
     Focus-grid vertex selection now happens in `FrameRenderContext`
     preparation instead of inside `draw_grid()`. Ocean-grid camera
     height/waterline classification is also prepared there instead of being
     recomputed inside the theme renderer. Backdrop/cityscape rendering now
     lives in `scene_backdrop.c`, and light setup plus light indicators live in
     `scene_lights.c`. Polygon outline/current-block highlight rendering now
     lives in `scene_overlays.c`, with a shared flat-block cursor matcher for
     outline and vertex-overlay paths.

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

## Test Plan
- After baseline stabilization: run `make test-stubs TEST_JOBS=4` after every stage.
- For parser/command-store work: run `make test_repl_core_parse`, `make test_repl_core_commit`, `make test_repl_core_format`, `make test_repl_core_io`, and `make test_repl_core_examples`.
- For evaluator/flattening work: run `make test_eval`, `make test_format`, and focused flatten/reformat tests.
- For UI layout extraction: add or update tests that compare wrapping, hit-testing, visual dumps, search positions, and continuation indentation.
- For import/export work: add round-trip tests for declarations, functions, config metadata, camera metadata, workspace scene names, and single-file reload.
- For render/replay work: run `make sample USE_GL_STUBS=1`, `make sample`, and a manual smoke test for replay, grid/axes themes, overlays, accumulation AA toggle, and example cycling.

## Assumptions
- Preserve the current C/OpenGL/GLUT stack and sample-local structure.
- Prefer behavior-preserving internal refactors over feature changes.
- Do not introduce a shared engine or cross-sample framework.
- Keep output file compatibility unless a specific migration is planned.
- Work in small reviewable stages; no stage should combine broad file movement with semantic behavior changes.
