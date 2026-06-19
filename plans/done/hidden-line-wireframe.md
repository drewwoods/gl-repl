# Hidden-Line Wireframe

## Status

Done. Implemented on branch `hidden-lines`.

Key commits:

- `b7167e5d` Restore plain wireframe mode
- `3eedafef` Expose REPL execution cursor
- `0898fc75` Drive hidden lines with REPL cursor
- `7c13b80d` Add direct REPL cursor tests
- `b756befb` Centralize REPL state bookkeeping in the executor
- `195e3238` Move hidden lines into subsystem

This supersedes the earlier reviewed draft in `plans/in-review/`, which proposed
an edit-overlays-owned pass and an ignored-command type/filter. The landed
implementation took the simpler cursor-driven approach discussed during review.

## Outcome

Wireframe is now a three-state mode:

1. `SCENE_WIREFRAME_OFF`
2. `SCENE_WIREFRAME_PLAIN`
3. `SCENE_WIREFRAME_HIDDEN`

Plain wireframe restores the original main-branch behavior: the scene renders
the user program with `glPolygonMode(GL_LINE)`, preserving authored lines,
points, colors, and widths.

Hidden-line wireframe renders three passes:

1. Hidden/dim line pass
2. Depth-fill pass
3. Visible/bright line pass

The result is the intended dual-color hidden-line style: visible polygon edges
draw bright, occluded edges remain faint, and polygon depth hides grid/helper
geometry behind the object.

## Landed Architecture

### Scene owns pass sequencing

`src/scene/render.c` owns the hidden-line pass recipe:

- set fixed GL state for hidden edges
- execute user geometry for `SCENE_EXEC_WIREFRAME_HIDDEN_LINES`
- prime depth with filled polygons and color writes disabled
- execute user geometry for `SCENE_EXEC_WIREFRAME_DEPTH_FILL`
- draw visible edges with depth test enabled and depth writes disabled
- execute user geometry for `SCENE_EXEC_WIREFRAME_VISIBLE_LINES`

This keeps the rendering order close to the scene pass that already owns setup,
fill, helper drawing, and overlay hooks.

### REPL cursor owns execution semantics

`src/repl/executor.h` exposes a stack-owned `ReplExecCursor`:

- `repl_exec_cursor_begin`
- `repl_exec_cursor_step`
- `repl_exec_cursor_end`
- `repl_exec_cursor_done`
- `repl_exec_cursor_peek`
- `repl_exec_cursor_advance`

`repl_execute_program()` now uses the same cursor internally, so whole-program
execution and selective pass execution share one implementation for transforms,
begin/end state, tessellation state, assignments, `if`, and `goto`.

`advance()` is intentionally a skip-without-execute primitive. Callers that skip
structural commands own the matching state consequences.

### Hidden-line command filtering lives in a subsystem

`src/subsystems/hidden_lines/hidden_lines.c` drives the REPL cursor for the
hidden-line scene purposes.

It lets the cursor execute:

- transforms
- `glBegin` / `glEnd`
- vertices
- GLUT solids
- assignments
- execute-time control flow

It skips pass-owned raster/appearance state such as color, material, line width,
point size, text/raster position, depth/color masks, and blend/depth functions.

For depth-fill, authored line/point primitives are skipped because they do not
contribute to a filled depth hull. That preserves plain wireframe behavior for
line/point-driven scenes while keeping hidden-line depth meaningful for polygon
geometry.

The subsystem keeps a colorless tessellator because the normal executor's GLU
tess callbacks emit per-tess-vertex colors; hidden-line passes need fixed pass
colors rather than authored tess colors.

### Bookkeeping is centralized in the executor

`repl_apply_state_bookkeeping()` records the non-GL REPL render bookkeeping
needed by skipped state commands:

- light enable mask updates for `GL_LIGHTn`
- clear-color runtime state updates

Both normal execution and hidden-line selective execution use the same helper,
so the subsystem does not duplicate light/clear-color state rules.

## Files Changed

- `src/scene/render_types.h`
  - Added `SceneWireframeMode`.
  - Added hidden-line scene execute purposes.
- `src/scene/render.c`
  - Restored plain wireframe via `glPolygonMode(GL_LINE)`.
  - Added hidden-line three-pass scene rendering.
- `src/app/glr_state.h`
  - `presentation.wireframe` is now `SceneWireframeMode`.
- `src/app/glr_actions.c`
  - Wireframe config row cycles through Off / Wireframe / Hidden-line.
- `src/app/glr_config.c`
  - Bridges the enum-backed wireframe setting through the existing integer config API.
- `src/app/glr_ctrl.c`
  - Routes wireframe scene execute purposes to the hidden-lines subsystem.
  - Initializes and destroys hidden-lines resources.
- `src/subsystems/hidden_lines/hidden_lines.c`
  - Cursor-driven hidden-line pass executor.
- `src/subsystems/hidden_lines/hidden_lines.h`
  - Neutral subsystem API: `HiddenLinesRenderContext`, `hidden_lines_*`.
- `src/repl/executor.h` / `src/repl/executor.c`
  - Added `ReplExecCursor`.
  - Converted `repl_execute_program()` to cursor execution.
  - Added `repl_apply_state_bookkeeping()`.
- `src/repl/help_text.c`
  - Updated `Ctrl+G` help text for the three-state cycle.
- `src/subsystems/README.md`
  - Added the hidden-lines subsystem to the subsystem map.
- Tests updated in:
  - `tests/test_glr_actions.c`
  - `tests/test_scene_render.c`
  - `tests/test_repl_core_io.c`
  - `tests/test_repl_executor.c`

## Review Feedback Addressed

- **Non-polygon scenes:** plain wireframe remains available as the original
  behavior. Hidden-line mode keeps line/point primitives from polluting the
  depth-fill hull.
- **Controller bloat:** hidden-line execution moved out of `src/app/glr_ctrl.c`
  into `src/subsystems/hidden_lines/`.
- **Executor duplication:** hidden-line execution uses `ReplExecCursor` instead
  of duplicating the main executor's control-flow and transform semantics.
- **No generic callbacks/flags:** the cursor API is explicit and stack-owned.
  It exposes types directly and avoids dynamic allocation.
- **No ignored command type bit:** pass-local skipping is execution-side via
  `peek`/`advance`, so temporary hidden-line state does not enter the command
  type system.
- **Bookkeeping clarity:** light-mask and clear-color bookkeeping is centralized
  in `repl_apply_state_bookkeeping()`.
- **Cursor test coverage:** direct tests assert both `step()` state mutation and
  `advance()` skip-without-execute behavior.

## Verification

`make test-stubs` was run after each implementation slice requested during the
branch work.

Latest relevant full run after moving hidden-lines into `src/subsystems/`:

- test binaries: `57 total`, `57 passed`, `0 failed`
- tests: `10283 total`, `10283 passed`, `0 failed`

Additional focused cursor check:

- `test_repl_executor` covers direct cursor behavior, including `peek`,
  `advance`, `done`, and `step`.
