# REPL Architecture

> For a one-page layered overview of all modules, see
> [`MODULES.md`](MODULES.md). This document is the per-module reference.

## Overview

The immediate-mode REPL is now split across focused translation units instead
of one monolithic `repl_core.c`.

- `repl_core.c`: normalization, display callback, and init wrapper.
- `repl_parser.c`: source-line parser, expression validation against visible
  variables, explicit `ReplParseContext`, and canonical `GLCmd.source[]`
  generation for GL commands.
- `repl_source_scope.c`: source-command prefix-depth cache, indentation
  helpers, `find_block_end()`, and nearest-open-block queries.
- `repl_flatten.c`: source-to-flat command expansion and flat-command cursor
  matching.
- `repl_executor.c`: flat-command execution, GLU resource lifetimes,
  state-command dispatch, replay fade execution context, and
  predefined-variable snapshots.
- `repl_autocomplete.c`: input completions and parameter hints derived from
  parser command metadata plus user-defined function signatures.
- `repl_autonormal.c`: auto-generated `glNormal3f` command maintenance and
  feeding color/normal lookup for code-panel highlighting.
- `repl_scenes.c`: user-scene slots, example promotion, workspace save/load,
  LRU eviction, and scene rename state.
- `repl_example_loader.c`: built-in example loading, example metadata, camera
  presets, and active example tracking.
- `repl_search.c`: search state, match navigation, and search-mode keyboard
  handling.
- `repl_export.c`: typed export scaffold sections, init bootstrap tables,
  import/export translation, save/load, and code-panel dump helpers.
- `repl_commit.c`: float declarations, variable assignments, structured block
  commits, close-brace commits, and commit-order helpers.
- `repl_editor.c`: editor state, commit orchestration, feed-line entrypoint,
  and GLUT input handlers.
- `repl_clipboard.c`: line selection anchors, command clipboard buffer, and
  copy/cut/paste range behavior.
- `repl_undo.c`: undo/redo snapshots, history rings, and the mutation-time
  example promotion hook.
- `repl_camera_controls.c`: scene camera pointer state, orbit/pan/zoom drags,
  wheel zoom velocity, and per-frame momentum decay.
- `repl_actions.c`: config item table, config shortcut dispatch, menu item
  actions, startup config defaults, and side effects for toggles/cycles.
- `repl_code_panel_layout.c`: pure code-panel wrapping, row counts, row
  segment lookup, and cursor-row mapping shared by UI rendering, hit-testing,
  tests, and visual dumps.
- `repl_code_panel_document.c`: code-panel document row model: wrapped
  header/body/footer counts, replay annotation rows, cursor-follow scrolling,
  and document-line to command-line hit targets.
- `repl_replay_annotations.c`: code-panel replay annotations, including
  source-line to flat-command mapping, substituted variable comments, and
  evaluated command display text.
- `ui_menu_bar.c`: code-panel menu bar, dropdown rendering/hit-testing,
  config-menu right-click cycling, and inline search-slot rendering.
- `ui_color_picker.c`: floating color picker state, literal color swatches,
  and HSV/alpha mutation of color commands.
- `ui_help_overlay.c`: modal F1 help overlay rendering.
- `ui_variable_panel.c`: floating variable slider panel rendering, geometry,
  and hit-testing.
- `repl_var_drag.c`: variable slider drag transactions and writeback into
  predefined variables plus matching assignment source.
- `ui_autocomplete_panel.c`: floating autocomplete popup rendering; match
  building and selection state stay in `repl_autocomplete.c`.
- `repl_inline_rename.c`: inline scene-rename input buffer and key handling.
- `repl_eval.c`: expression parsing and evaluation.
- `ui_panels.c`: 2D code-panel row rendering, source search highlights, inline
  ghost/hint text, scene status banner, and top-level panel routing.
- `scene_render.c`: 3D frame orchestration, one-shot scene init,
  `SceneRenderConfig` / `FrameRenderContext` prep, orbit target, and replay HUD.
- `scene_render_types.h`: shared per-frame render snapshot types consumed by
  the scene helpers.
- `scene_guides_shared.h`: shared scene-guide snapshot and transform-guide
  planning types.
- `scene_geometry_guides.c`: vertex-input and normal-edit guide rendering from
  snapshot state.
- `scene_transform_guides.c`: transform-guide planning and rendering
  (`glTranslatef`/`glRotatef`/`glScalef`) from snapshot state.
- `scene_grid.c`: grid theme rendering, including focus/ocean/ruler/planes
  variants.
- `scene_axes.c`: axes theme rendering.
- `scene_backdrop.c`: backdrop mode dispatch and deterministic cityscape
  rendering.
- `scene_lights.c`: ambient init, per-pass light property setup, and light
  indicator overlay rendering.
- `scene_overlays.c`: polygon outline/current-block, vertex-number, and
  normal-vector overlay rendering plus shared flat-block cursor matching.
- `prof.c`: project-wide CPU timing instrumentation.
- `sample.c`: application entrypoint, GLUT callback wiring, and buffer swap.

The public API is still `repl_core.h`. Cross-module runtime/test helpers live in
`repl_core_internal.h`. Shared globals and UI-visible state still live in
`sample.h`.

For the layered overview and the editor-adjacent ownership diagram, see
[`MODULES.md`](MODULES.md).

## Two-Level Command Model

The REPL keeps **source commands** and **flattened commands** as separate
arrays. Everything else (execution, replay, normal recomputation, overlays)
reads from the flattened array.

- `g_cmds[MAX_COMMANDS]` — source-level. One entry per line visible in the
  code panel. Holds parsed type/args, the normalized `source[]` text, and
  flags (`has_vars`, `valid`, `is_auto`). Editor mutations touch only this
  array.
- `g_flat_cmds[MAX_COMMANDS]` — expanded. For-loops unrolled, function calls
  inlined, if-conditions resolved. Each flat cmd carries `src_cmd_idx`
  (owning source line), `call_src_cmd_idx` (immediate call site), and
  `func_scope_mask` (active function scopes) so the editor can highlight
  the right source line when the cursor lands on a flattened command.
- **Rebuild trigger:** every mutation sets `g_flat_dirty = 1` (via
  `mark_normals_dirty()`); `flatten_commands()` rebuilds the flat array on
  the next frame before rendering.
- `repl_flatten_program()` is the explicit builder underneath the live rebuild:
  callers provide source commands, destination flat buffers, local-variable
  snapshot storage, capacity, recursion limits, visit budget, and receive
  flat count/status output.

Keeping the two levels separate means edits are cheap, execution reads a
flat stream, and replay can step by flat-command without worrying about
loop/function structure.

## Command Lifecycle

A single REPL line progresses through five stages, owned by different
modules:

1. **Input** — `repl_editor.c` accumulates characters into `g_input`.
2. **Commit** — `;` (or Enter in overwrite mode, or programmatic
   `feed_line()`) runs the commit handler chain (see
   [Structured block commits](#structured-block-commits) below).
3. **Parse** — `repl_parse_command()` in `repl_parser.c` matches the line to
   a `CmdType`, evaluates argument expressions via `repl_eval.c`, and
   stores results into `GLCmd.args[]` / `GLCmd.source[]`.
4. **Flatten** — `repl_flatten_program()` recursively expands source commands
   through `flatten_range()`, capped at 100k visits and recursion depth 64 by
   the live wrapper.
5. **Execute** — `execute_commands()` / `repl_execute_program()` in
   `repl_executor.c` walk `g_flat_cmds[]` and emit GL calls. Commands flagged
   `has_vars` are re-evaluated each frame so animated expressions (e.g. `t`)
   stay live.

Stage 1 and commit orchestration live in `repl_editor.c`; commit handlers
live in `repl_commit.c`; parsing lives in `repl_parser.c`; flattening lives in
`repl_flatten.c`; execution lives in `repl_executor.c`. This is the
load-bearing boundary — outside code that needs to inject commands should
do so through `feed_line()` rather than poking `g_cmds[]` directly, so
every path shares the same parse/normalize/flatten guarantees.

## Data Flow

### Edit and commit path

1. GLUT keyboard/mouse callbacks enter through `repl_editor.c`.
2. Commit handlers in `repl_commit.c` convert input text into commands by calling
   `repl_parse_command*()` in `repl_parser.c` or
   `repl_parse_and_normalize()` in `repl_core.c`.
3. Parsed commands are stored in `g_cmds[]`.
4. `mark_normals_dirty()` and `g_flat_dirty` invalidate downstream derived
   state.

### Execution path

1. `flatten_commands()` in `repl_flatten.c` calls `repl_flatten_program()` to
   expand loops, functions, conditionals, and variable-driven commands into
   `g_flat_cmds[]`.
2. `scene_render.c` prepares the frame, delegates grid rendering to
   `scene_grid.c`, axes rendering to `scene_axes.c`, guide rendering to
   `scene_geometry_guides.c` / `scene_transform_guides.c`, light setup to
   `scene_lights.c`, and calls `repl_execute_program()` with an explicit
   `FlatProgramView`/flat-command limit for normal, replay, and fade passes.
3. `repl_executor.c` issues fixed-function OpenGL calls against the requested
   flat program view. `execute_commands()` remains a full-range compatibility
   wrapper around `repl_execute_program(NULL)`.

### Search path

1. `repl_search.c` scans `g_cmds[].source`.
2. Search hit movement calls `navigate_to_line()` from `repl_editor.c`.
3. `ui_panels.c` renders search highlights from the shared search globals.

### Import/export path

1. `repl_export.c` owns the scaffold shown in the code panel and emitted to
   exported C.
2. `save_output()` writes typed scaffold sections plus translated command
   bodies.
3. `load_from_file()` converts exported C back into REPL lines and replays them
   through `feed_line()` from `repl_editor.c`, so import uses the same commit
   rules as interactive editing.

## Ownership

### `repl_core.c`

Owns the semantic model and display infrastructure.

- `g_cmds[]`, `g_num_cmds`
- `g_flat_cmds[]`, `g_num_flat_cmds`
- normalization and reformat orchestration
- display callback
- init wrapper

### `repl_parser.c`

Owns the general GL command parser.

- `ReplParseContext` (`source_line_idx`, visible locals)
- `repl_parse_command()`
- `repl_parse_command_with_vars()`
- `repl_parse_command_ctx()`
- table-driven fixed-arity/enum command matching
- custom parser branches for material, point-parameter, tessellator,
  function-call, label, and goto syntax

### `repl_source_scope.c`

Owns source-command scope/depth lookups.

- prefix-depth cache invalidated through `depth_cache_invalidate()`
- `block_depth_at()`, `in_begin_block_at()`, `tess_scope_depth_at()`
- `cmd_indent()`, `cmd_tess_indent()`, `cmd_indent_chars()`
- `find_block_end()`, `nearest_open_block_at()`

### `repl_flatten.c`

Owns flattening and flat-command cursor matching.

- `flatten_commands()`
- `repl_flatten_program()` explicit source/destination builder
- recursive loop/function/if expansion
- flat-command provenance fields
- `g_current_block_begin`, `g_current_block_end`

### `repl_executor.c`

Owns flat-program execution and execution-time state.

- `repl_execute_program()`, `execute_commands()`
- `FlatProgramView` resolution and execution-limit clamping
- GLU quadric / tessellator resource lifetimes and tess callbacks
- `apply_state_cmd()`
- replay fade execution context
- predefined-variable snapshot/restore helpers

### `repl_autocomplete.c`

Owns editor-input completions and parameter hints.

- `g_ac_*` completion/ghost/hint state
- builtin command and enum completions
- user-defined `funcN(...)` parameter hints

### `repl_autonormal.c`

Owns auto-generated normals and feeding-state lookup.

- `recompute_autonormals()`
- `repl_find_feeding_normal_cmd()`
- `repl_find_feeding_color_cmd()`
- auto-normal command insertion/update through `ReplCommandStore`

### `repl_scenes.c`

Owns the multi-scene workspace model.

- `g_user_scenes[]` and active-scene tracking
- example-to-user-scene promotion
- workspace save/load and LRU scene eviction
- scene naming, slugging, and rename validation

### `repl_example_loader.c`

Owns built-in example loading and loader-only metadata.

- `g_example_idx`
- example `@cfg` metadata filtering and application
- example camera preset parsing
- example presentation-default reset before metadata application
- example source replay through `feed_line()`

### `repl_editor.c`

Owns transient editor and interaction state.

- `g_input`, `g_input_len`, `g_cursor_pos`
- `g_edit_line`, `g_inserting`, newline buffer
- code-panel scroll and resize state
- variable-drag/config-menu interaction state
- feed-line entrypoint and commit-attempt outcomes
- keyboard, special-key, mouse, motion, and timer callbacks

### `repl_actions.c`

Owns side-effecting actions that start from editor shortcuts or menu rows.

- `g_cfg_items[]`, `CFG_ITEM_COUNT`, and config state-label tables
- F-key and Ctrl-key config shortcut lookup
- `repl_cfg_cycle_row()` and per-item side effects
- menu item activation for File, Scene, and Config menus
- startup application of persisted audio config

### `repl_undo.c`

Owns editor history snapshots and the undo/redo rings.

- source-command, edit-line, and predefined-variable snapshots
- undo/redo ring heads and counts
- snapshot save/restore helpers used by commit-attempt rollback
- example-to-user-scene promotion before the first mutating snapshot

### `repl_camera_controls.c`

Owns scene-camera pointer and momentum behavior after editor/UI routing has
decided an event belongs to the viewport.

- `g_mouse_x`, `g_mouse_y`, `g_mouse_btn`
- active mouse-drag modifiers for scene panning
- orbit, ground-plane pan, vertical pan, and middle-button zoom math
- wheel zoom velocity and per-frame velocity decay
- camera auto-rotate and pan-target glow fade during the timer tick

### `repl_clipboard.c`

Owns line selection and command clipboard behavior.

- `g_sel_anchor`, `g_sel_end`
- `g_clipboard[]`, `g_clipboard_count`
- selected/current command-range resolution for copy and cut
- var-declaration copy/cut/paste guards
- paste insertion through `ReplCommandStore`

### `repl_commit.c`

Owns source-command mutations that happen before the general GL parser path.

- `float` declarations and predefined-variable registration
- predefined-variable assignments
- `for`, `func`, and `if` structural block commits
- explicit close-brace handling
- canonical commit-handler ordering helpers
- function-declaration resume state used when declarations move upward

### `repl_search.c`

Owns source-text search state.

- `g_search_*`
- row/text lookup helpers
- next/previous match navigation
- search-mode key handling

### `repl_export.c`

Owns generated scaffold and import/export plumbing.

- `g_header_pre`, `g_render_state_lines`, `g_lookat`, `g_header_post`
- init bootstrap tables and helpers
- `ExportScaffoldContext`, top-level scaffold sections, `ExportNeeds`, and
  display-pass specs for generated export sections
- `save_output()`, `load_from_file()`
- import translation helpers
- code-panel visual dump plumbing, using `repl_code_panel_layout.c` for wrap
  parity with the on-screen panel

### `repl_code_panel_layout.c`

Owns pure code-panel text wrapping and row lookup. It has no OpenGL, editor, or
export side effects.

- `CodePanelTextLayout`
- continuation-indent and wrap-break decisions
- `CodePanelWrapIter`
- row count, row segment, and cursor-row lookup helpers
- shared behavior for `ui_panels.c`, `repl_export.c`, and focused layout tests

### `repl_code_panel_document.c`

Owns the code-panel document row model that sits above pure text wrapping. It
has no drawing code; it turns current REPL/editor state into row counts and
targets consumed by rendering, scrolling tests, and mouse hit-testing.

- wrapped header/body/footer row totals
- per-command main rows plus replay annotation rows
- cursor and replay follow-line calculation
- scroll-follow clamping
- document-line to command-line target mapping

### `repl_replay_annotations.c`

Owns the code-panel replay text shown beside source commands during replay.
It reads replay/flat-command state, but keeps annotation caches and evaluated
display formatting out of `ui_panels.c`.

- source-command to latest flat-command mapping for the current replay PC
- predef-variable snapshots before annotated flat commands
- substituted variable comments and evaluated command strings
- assignment inline comments used by code-panel display text

### `ui_menu_bar.c`

Owns the menu bar and dropdown UI state. It renders the File/Scene/Config
menus, the Search/Replay pinned slots, and the menu-hosted search field.
Action execution still flows through `repl_actions.c`.

- menu/dropdown open state and hover state
- menu and pin hit-testing
- config dropdown right-click cycling
- `render_example_dropdown()` for the floating dropdown layer

### `ui_color_picker.c`

Owns the floating color editor opened from literal color-command swatches.
It mutates the selected command source/args directly today, but all picker
state, hit rectangles, and rendering live outside `ui_panels.c`.

- HSV/alpha picker state
- swatch rendering for literal color commands
- picker drag handling and command rewrite

## Layering Rules

Two hard rules govern where OpenGL calls and inter-module includes are
allowed. They keep the rendering surface mechanically auditable and
preserve the master/slave relationship between the orchestrator and the
two rendering layers.

### GL/GLUT isolation

- Live OpenGL/GLU drawing calls only appear in `scene_*.c`, `ui_*.c`,
  and `repl_executor.c`. The executor is the sole `repl_*` exception
  because it dispatches GL for flat commands.
- GLUT input/feedback APIs (`glutPostRedisplay`, `glutSetCursor`,
  `glutGetModifiers`, `glutSwapBuffers`) only appear in `sample.c` and
  `repl_editor.c`, and inside `repl_editor.c` they go through local
  funnel helpers (`editor_request_redraw()`, `editor_set_cursor()`,
  `repl_editor_active_modifiers()`) so the GLUT surface is reviewable
  in one place.
- All other `repl_*.c` files and `sample.h` do not call GL or GLU.
  Text emission of GL command *names* — `repl_export.c`,
  `repl_examples.c`, `repl_command_spec.c`,
  `repl_replay_annotations.c`, `repl_parser.c` — is REPL source, not
  a live call site.

### UI/scene independence

- `ui_*` and `scene_*` are sibling rendering layers. Neither includes
  the other's headers. They communicate only through `repl_*` models
  above them and through the orchestrator in `repl_core.c`'s
  `display_func()`, which is the sole master that dispatches
  `render_3d_scene()` and the 2D overlay sequence per frame.
- Both layers may freely include `repl_*` headers (pipeline plus
  domain models) and project-agnostic header libraries from
  `include/`.
- Generic 2D primitives live in `include/gl_2d.h`
  (`gl2d_begin`/`gl2d_end`/`gl2d_draw_string`) — a header-only
  library alongside `gl_includes.h`, `stb_image.h`, and `utils.h`.
  The criterion is **pure function of arguments, no project state**.
  Both `scene_*` and `ui_*` include this header; pure-model `repl_*`
  files do not (they don't draw). `glRectf` replaces the former
  `draw_quad` shared helper at every call site.

### Grandfathered exceptions

- `scene_render.c` includes `ui_panels.h` for `scene_rect()`. This is
  a read-only layout coordinate, not render dispatch. Tracked for
  removal by migrating `scene_rect()` into a `repl_*` layout model.

### Enforcement

`make check-gl-boundaries` greps `repl_*.c` (excluding the executor)
for GL/GLU drawing calls and `repl_*.c` (excluding the executor and
editor) for GLUT calls; both sets must be empty.
`make check-layer-coupling` greps `ui_*` for `#include "scene_*"` and
`scene_*` for `#include "ui_*"`, allowing only the two grandfathered
includes above. Both targets run inside the `make test` /
`make test-stubs` umbrella so a regression fails the build on first
push.

## Shared State Rules

- `sample.h` remains the single shared type and compatibility header, while
  `repl_config.h` now owns the keyed config descriptor API and broad runtime
  state declarations live behind `repl_state.h`.
- `repl_state.h` groups the historical `g_*` globals into typed live views
  (`ReplDocumentState`, `ReplFlatProgramState`, `ReplEditorState`,
  `ReplPresentationState`, `ReplRenderState`, etc.). Phase 2 is moving
  storage behind that facade domain by domain; the source command buffer,
  flat-program buffers, editor input, selection, clipboard storage, camera,
  pointer, viewport, and mutable presentation config state now live in
  `repl_state.c` while compatibility externs remain for unmigrated callers.
  Immutable descriptor/name tables remain outside runtime state.
- `repl_command_store.h` is the first ownership boundary around source-command
  array mechanics. Code that shifts, inserts, replaces, deletes, clears, or
  bulk-restores `g_cmds[]` should prefer `repl_command_store_*` helpers so
  capacity checks, edit-line adjustment, and depth-cache invalidation stay
  consistent. Undo, scene switching, example load, workspace import, and global
  reset all restore source-command arrays through `repl_command_store_load()`.
  Remaining direct writes are localized semantic edits to fields inside an
  existing command, such as color-picker/variable-drag rewrites and declaration
  assignment-slot repair.
- `repl_core_internal.h` is the internal bridge for non-public helpers needed by
  tests or sibling `.c` files.
- The `CFG_DEFAULT_*` macro block in `sample.h` is also the shared source of
  truth for example-owned scene-presentation defaults. Reuse those macros from
  `repl_core.c` initializers, example reset helpers, and focused tests instead
  of duplicating literals.
- New per-module headers are introduced only when they establish a real
  ownership boundary. Avoid adding headers for cosmetic splits.
- Parser internals live in `repl_parser.c`; editor/search/export/core pass an
  explicit `ReplParseContext` when the source-line index matters and otherwise
  call the compatibility wrappers.

## Refactoring Ownership Map

This is the target responsibility split for cleanup work. Refactors should move
one boundary at a time and keep behavior unchanged unless a test explicitly
captures the intended behavior change.

- **Command store:** owns source-command array mechanics, bulk snapshot loads,
  capacity checks, edit-line adjustment for raw insertions/restores, and
  depth-cache invalidation.
- **Parser:** owns line-to-`GLCmd` translation, command metadata, expression
  preservation, and normalized source text.
- **Commit pipeline:** owns user intent: where a parsed command lands, when
  undo snapshots are taken, and how variable declarations register names.
- **Flattener:** owns expansion of loops, functions, and conditionals into a
  flat program with source-line provenance. Recursive flattening state and
  destination buffers live in `FlattenContext` / `ReplFlattenOptions` rather
  than file-scope control globals.
- **Executor:** owns OpenGL calls for a flat command stream. Replay fill/fade
  passes use `ReplExecutionOptions` to supply explicit execution ranges.
- **Editor/input router:** owns modal dispatch, cursor/input buffers, and
  keyboard/mouse routing.
- **Clipboard/selection:** owns line-range selection state, clipboard storage,
  and copy/cut/paste command mutations.
- **UI layout:** owns pure code-panel wrapping, document rows, hit-testing, and
  visual dump coordinates. `repl_code_panel_layout.c` owns wrapping/segment
  lookup; `repl_code_panel_document.c` owns row totals, follow-scroll state, and
  document-line targets; `ui_panels.c` consumes those models while rendering.
- **UI overlays:** owns visible but non-core controls. `ui_menu_bar.c` owns
  menus/dropdowns/search slot, `ui_color_picker.c` owns the floating color
  editor, `ui_help_overlay.c` owns the modal F1 help overlay,
  `ui_variable_panel.c` owns the floating variable slider panel
  (rendering only — drag mutation lives in `repl_var_drag.c`),
  `ui_autocomplete_panel.c` owns the floating completion popup
  (rendering only — match/selection state lives in
  `repl_autocomplete.c`), and `repl_inline_rename.c` owns the inline
  scene-rename buffer and key handling. `ui_panels.c` now focuses on
  code-panel row rendering, the scene-status banner, and top-level
  hit routing.
- **Scene renderer:** owns camera/view setup, grid/axes/overlay drawing, and GL
  state discipline for a single frame. `SceneRenderConfig` snapshots the scene
  rectangle, camera, accumulation jitter, quality toggles, grid/axes choices,
  overlay toggles, and replay-derived limits once at frame start.
  `FrameRenderContext` carries that config plus prepared derived state such as
  the Focus-grid vertex and ocean-grid camera waterline classification before
  helper renderers run. Grid themes live in `scene_grid.c` and axes themes in
  `scene_axes.c`. Flattened geometry overlays live in `scene_overlays.c`,
  where outline, vertex-number, and normal-vector passes share cursor-block
  matching and transform traversal.
- **Import/export:** owns scaffold sections, workspace metadata, and
  translation between exported C and REPL command text.

### Current cleanup baseline

After the Phase 7 code-panel responsibility split, `make test-stubs TEST_JOBS=4`
builds all test binaries and passes 19 of 19 suites: 2437/2437 tests. The
latest slices extracted document rows, replay annotations, menu/dropdown
rendering, the color picker, help overlay, variable panel, autocomplete popup,
inline rename, and variable slider dragging while preserving behavior. Phase 8
then added grid theme rendering in `scene_grid.c`, axes theme rendering in
`scene_axes.c`, local helper-pass GL state guards, a shared vertex-overlay
traversal, explicit scene render config/frame context prep, Focus/ocean frame
prep, backdrop/light modules, and geometry overlay extraction. Phase 9 paired
workspace-header parsing/emission
through a directive table, split `load_from_file()` into ordered import
handlers, confirmed visual dumps use the shared code-panel wrap iterator, and
introduced typed top-level scaffold sections plus display/pass helpers for the
generated export file.

## Key Pipelines

### Structured block commits

`repl_commit.c` handles user-facing block syntax:

- `for(...) { ... }`
- `funcN(...) { ... }`
- `if(...) { ... }`
- closing `}`
- `float name[, ...];` declarations
- `name = expr;` assignments to predefined variables

Historically each dispatch site (`;` key, Enter in insert mode, Enter in
overwrite mode, `feed_line()`) open-coded the handler chain. That is now
consolidated into four helpers in `repl_commit.c`:

- `try_commit_var_statements()` — float decl, then assign
- `try_commit_block_structs()` — close-brace, for, func, if
- `try_commit_any()` — both groups in canonical order
- `try_commit_var_statements_then_insert()` — var variant that flips to
  insert mode on success, used by the overwrite-mode Enter key

Adding a new statement handler means extending the right helper, not
chasing the call sites. Ordering within a helper is load-bearing:
`try_commit_float_decl` must precede `try_assign_variable` so that
`float x;` is not misread as an assignment to an identifier named
`"float"`.

These helpers route source-array insert/replace/delete work through
`ReplCommandStore`, but they still rely on parser helpers from
`repl_parser.c` and scope helpers from `repl_source_scope.c` for validation and
normalization. Declaration bookkeeping may still repair assignment variable-slot
indices in place after the predef table changes; that is a semantic command
field update rather than command-array ownership.

### Float variable declarations

`float name[, ...];` lines commit to a dedicated `CMD_VAR_DECLARE` entry
rather than running through the general parser. Two structural choices
live here:

- **Placement rule:** new declarations are inserted at the top of
  non-decl code (index of the first non-`CMD_VAR_DECLARE` cmd), regardless
  of cursor position. That guarantees every reference in source order
  follows its declaration, so init expressions only see predef vars
  declared above them. Editing an existing decl still overwrites in
  place.
- **No-op at execution time:** `execute_commands()` and `flatten_range()`
  skip `CMD_VAR_DECLARE`. Registration into `g_predef_vars[]` happens at
  commit time via `declare_predef_var()` in `repl_eval.c`, so the
  evaluator sees the variable before the flat stream references it.

`delete_cmd_range()` in `repl_editor.c` refuses to remove a declaration
whose name is still referenced elsewhere (via `source_uses_ident()` in
`repl_eval.c`).

### Editing existing lines

Navigating onto an existing line loads `g_cmds[i].source` into `g_input`
with the trailing `;` and whitespace stripped, so re-committing goes
through the no-semicolon code path. Every commit handler that looks for
`;` must also accept end-of-string as a valid terminator. This invariant
is shared by all dispatch sites.

### Undo / redo

`repl_undo.c` owns fixed-size circular buffers of editor snapshots.

- `ReplUndoSnapshot` captures `g_cmds[]`, `g_num_cmds`, `g_edit_line`, and
  `g_predef_vars[]` values — enough to restore the full editor state.
- Any mutation (delete, paste, reformat, etc.) calls
  `push_undo_snapshot()` before changing state. Pushing clears the redo
  stack, which is the usual "diverged history" rule.
- Ctrl+Z pops the undo stack and moves the current state to the redo
  stack; Ctrl+Y does the reverse.
- Snapshot restore loads `g_cmds[]`, `g_num_cmds`, and `g_edit_line` through
  `repl_command_store_load()` so depth-cache invalidation and edit-line clamping
  match scene/example/reset restores.
- Rejected navigation commits use `ReplUndoRingState` to restore the history
  counters after rolling back the attempted command/predef mutation.

### Flattening

`repl_flatten_program()` / `flatten_range()` in `repl_flatten.c` are still the
only places that expand:

- `CMD_FOR_BEGIN .. CMD_FOR_END`
- `CMD_FUNC_DEF .. CMD_FUNC_END`
- `CMD_IF_BEGIN .. CMD_IF_END`
- variable-dependent commands

That preserves a single semantic source of truth for execution, replay, and
export-derived behavior.

### Transform edit guides

When the cursor sits on a committed `glTranslatef`, `glRotatef`, or
`glScalef` line, `scene_transform_guides.c` renders an overlay showing that
command's effect. `scene_render.c` builds one `SceneGuideSnapshot` per frame
and the guide module runs in the vertex-dots pass so it shares the flat-walk
matrix tracking.

Gating comes from `scene_transform_guides_prepare()`: `show_guides` is on,
`!replaying`, the edit-line source command is valid transform type, and the
input buffer still matches the normalized committed source (mirrors the
`unmodified` check in `repl_editor.c` so mid-keystroke edits suppress the
guide).

## Evaluator / translation learnings

- **C float-literal suffixes must remain evaluator-safe.** The importer can
  produce expressions containing literals like `1.0f`. The evaluator and
  identifier validator now treat `f`/`F` as a numeric suffix when it appears
  immediately after a parsed number literal, rather than as an identifier.
  This keeps imported expressions executable without requiring manual cleanup.
- **Harness gap that caught this:** translating `powf(1.0f,2.0f)` to REPL
  produced `pow(1.0f,2.0f)`, which previously failed evaluation because parsing
  stopped at the first `f`. Keep this pattern covered in `test_eval.c`.
- **String copy warning hygiene:** use explicit bounded formatting for internal
  translation buffers where practical to avoid `-Wstringop-truncation`
  false-positives from `strncpy`.
- **GL-stub parity matters for test-stubs.** The transform-guide renderer in
  `scene_transform_guides.c` uses `glLoadMatrixf` and `glVertex3fv`; missing
  these no-op declarations in `include/GL/gl.h` breaks `make test-stubs` at
  link time even though normal GL builds succeed. When adding fixed-function
  calls in rendering code, mirror them in the local stubs immediately.
- **Bounded copies still need explicit NUL termination.** In parser paths that
  trim/copy function identifiers into fixed buffers, always terminate manually
  after `strncpy(..., size-1)`; otherwise long unknown commands can leave
  unterminated stack strings and make command-dispatch comparisons undefined.

Flat-cmd scan: the matching flat cmd is found by flat execution order
— **not** by comparing `src_cmd_idx > g_edit_line`. Function-call
expansions carry the callee's `src_cmd_idx`, so a numeric comparison
would skip past them. Instead, locate the first flat cmd with
`src_cmd_idx == g_edit_line`, then take the next valid flat cmd whose
`src_cmd_idx` differs as the start of the post-cursor walk.

The guide's starting point is `p_after = M_after · origin`, where
`M_after` is the product of transforms that come after the cursor in
execution order, up to (but not including) the first geometry-emitting
command (`is_geometry_emit_cmd`: `CMD_BEGIN`, `CMD_GLU_*`,
`CMD_GLUT_TORUS`, `CMD_TESS_BEGIN_POLYGON`). Stopping at the first
emit prevents transforms following an intervening draw from bleeding
into the guide.

Two render modes, chosen via the `g_xform_guide_mode` config toggle
("Xform guide mode"):

- **World (0, default)** — render in world axes at world origin.
  Matches strict OpenGL reverse-order semantics: for cursor command
  `C_k`, vertices are computed as `M_1 · M_2 · ... · M_n · v`, so
  `C_k` acts on the point `M_after · origin`. Pre-cursor transforms
  wrap this sub-expression later and don't move the guide. The
  camera-view matrix is snapshotted before any user transforms and
  reloaded via `glLoadMatrixf(tg_cam_view)` when drawing the guide.

- **Frame (1)** — render at a scene-world frame derived from the
  **full pre-cursor modelview** (translations, rotations, and
  scales). `compute_before_cursor_matrix()` walks all flat cmds
  before the cursor in a fresh identity matrix (via
  `apply_tracked_transform_cmd` so push/pop scopes correctly).
  In Frame mode, the guide matrix is `camera * pre_cursor` and the
  command's acts-on point is the local origin, so translate/rotate/scale
  guides all inherit prior frame rotations consistently.

Per-command helpers:

Shared visual style: shafts and arcs use an **axes-pulse-style**
overlay — a dim solid base line (or arc) at `alpha≈0.30`, with a
bright `sin(π·phase)` dot sweeping `a→b` and a short trail behind
it, driven by snapshot `anim_time`. Helpers live alongside the per-command
helpers: `xform_axis_color()` maps `(|x|,|y|,|z|)/max` → RGB, and
`draw_pulse_segment()` renders the dim base + traveling dot + trail
for a straight segment. The rotate guide inlines an arc-based
variant that walks the pre-sampled Rodrigues arc points.

Per-command helpers:

- `draw_translate_guide` — pulse shaft from `p_after` to
  `p_after + (tx,ty,tz)` with a 4-fin arrowhead at the tip. Shaft
  color comes from the translation vector via `xform_axis_color`;
  arrowhead uses a brighter tint of the same color.
- `draw_rotate_guide` — axis stub through the local origin plus a
  48-segment arc swept by Rodrigues rotation. Arc, axis stub, and
  endpoint dots are tinted from the rotation axis
  (`xform_axis_color(ax,ay,az)`). The pulse dot sweeps along the
  arc; the trail is a short strip of arc samples between the
  trail-phase and dot-phase points so curvature is preserved. The
  on-axis degenerate case (where `p_after` lies on the rotation
  axis) is not specially handled — the arc simply collapses.
- `draw_scale_guide` — pulse shaft from `p_after` to
  `(sx·x, sy·y, sz·z)` with arrowhead. Shaft color comes from
  `xform_axis_color(sx-1, sy-1, sz-1)` so the color highlights the
  axes that deviate from identity. Degenerate case (`p_after` at
  origin) falls back to a 3-axis gizmo: gray unit reference segment
  plus a pulsing arrow per axis in that axis's own color (X=red,
  Y=green, Z=blue) to `(sx,0,0)`, `(0,sy,0)`, `(0,0,sz)`.

Adding a new transform-guide type: extend the command gate in
`scene_transform_guides_prepare()`, add a render branch in
`scene_transform_guides_render_if_due()`, and add a new
`draw_<name>_guide` helper in `scene_transform_guides.c`.

### Replay

Replay state and stepping live in `repl_replay.c`, while editor callbacks in
`repl_editor.c` drive it.

- editor input toggles replay modes and stepping
- replay helpers rebuild source-line highlighting state
- `scene_render.c` consumes replay state to draw the HUD and replay overlays

Under the hood, replay works by clamping how much of `g_flat_cmds[]`
`execute_commands()` will emit on each frame:

- `g_replay_state` is OFF / PLAYING / PAUSED / DONE; `g_replay_pc` is a
  program counter into `g_flat_cmds[]`; `g_replay_speed` is a playback
  multiplier.
- During playback, `replay_prepare_frame()` still computes the active replay
  PC, while scene rendering passes explicit `ReplExecutionOptions` limits so
  the fill/fade executor does not temporarily rewrite `g_num_flat_cmds`.
- `g_replay_fade_batches[]` is a circular buffer of recent geometry
  snapshots. Old batches fade out as new geometry appears and are drawn
  in a separate blended pass (`render_replay_fade_pass()` in
  `scene_render.c`) after the main fill. This is what produces the
  trailing-ghost look without changing how the main executor walks the
  flat array.

## Startup and Examples

`load_initial_commands()` remains in `repl_core.c`, while example metadata
and built-in example loading live in `repl_example_loader.c`.

- If an import file is provided and `load_from_file()` succeeds, startup uses
  the imported session.
- Otherwise core asks the example loader to load a built-in example.
- `load_example_lines()` performs a metadata pre-pass before feeding the
   remaining source through `feed_line()`.
- Built-in examples can begin with contiguous leading `// @cfg slug = value`
   lines, followed optionally by a 5-line `// camera` preset block.
- Leading example `@cfg` lines are parsed through
   `parse_workspace_header_line()` from `repl_export.c`, but only for
   scene-presentation slugs allowed by the example loader.
- That leading metadata is consumed as loader-only state and does not appear in
   the code panel.
- On each example load, the loader resets the allowed non-camera scene-presentation
   settings to the `CFG_DEFAULT_*` values from `sample.h` before applying any
   leading example `@cfg` metadata. This keeps unspecified settings from leaking
   between examples.
- Camera is intentionally excluded from that reset; examples keep the current
   `g_cam_*` state unless the explicit `// camera` block is present.
- `restore_user_scene()` still restores commands and predefined variables only,
   so leaving an example does not restore prior camera or presentation state.
- Example geometry lines are still committed through `feed_line()`, so
   examples, imported files, and interactive editing share the same commit
   pipeline once loader metadata has been consumed.

## Test Orchestration

The Makefile builds each standalone `test_*` binary normally, then delegates
suite execution to `scripts/run-tests.sh`.

- `TEST_BINS` is the canonical ordered list for the full suite.
- `CORE_TEST_BINS` is derived from `TEST_BINS` by filtering out the lightweight
  standalone tests (`test_eval`, `test_format`, and `test_repl_audio`).
- Per-test `*_OBJS`, `*_LDLIBS`, and `*_RUN` variables feed a single
  `.SECONDEXPANSION` link recipe for all test binaries.
- `TEST_RUNNER_CASES` is generated from `TEST_BINS` and each test's `*_RUN`
  command, so build output, replayed logs, and final summaries stay in one
  predictable order.
- `make test` and `make test-detailed` export shared environment needed by the
  example export/compile tests, then call the runner.
- `TEST_JOBS=N` limits concurrent test binaries. Leaving it empty runs all
  test binaries concurrently.

The runner prevents interleaved output by redirecting each test binary's
stdout/stderr into `build/test-logs/run-*/<name>.log`, recording its exit status
beside that log, and replaying logs in Makefile order after all launched jobs
finish. This keeps parallel execution fast while preserving the readable
per-suite transcript expected from the old serial runner.

Global stats are collected from the final `passed/total` line emitted by each
test binary. The runner reports both binary-level status and assertion-level
totals, for example:

- `test binaries: 14 total, 14 passed, 0 failed`
- `tests: 1501 total, 1501 passed, 0 failed`

If a binary exits nonzero, the runner still prints its buffered log and includes
the binary in the global failure count. If a binary lacks a parseable
`passed/total` summary, it is counted at the binary level and called out as an
unknown assertion-count source.

Coverage builds intentionally run tests serially via `make test BUILD=coverage
TEST_JOBS=1`. Do not parallelize coverage unless the coverage output paths are
made safe for concurrent `.gcda` writes.

## Extension Guide

### Add a new command

1. Add the `CmdType` in `sample.h`.
2. Add the matching `ReplCommandTypeSpec` entry in `repl_command_spec.c`.
   This table owns command debug names plus reformatter traits such as
   semicolon and block-indent behavior.
3. Extend parser handling.
   - Table-driven entries live in `repl_command_spec.c`; the generic loop in
     `parse_command()` (`repl_parser.c`) walks those tables so most commands
     need no changes to parser code itself.
   - Pure numeric calls usually belong in `k_std_command_specs`.
   - Enum-driven calls usually belong in `k_enum_command_specs`; add a dedicated
     `EnumEntry` table when the legal enum set differs from a similar command
     (for example `glColorMaterial` modes are not the same as all
     `glMaterialf` pnames). Reuse `k_bool_vals` for `GL_TRUE` / `GL_FALSE`
     arguments (see `glDepthMask`).
   - Special arity, vector/scalar alternatives, block commands, or commands
     with custom validation need an explicit parser branch in `repl_parser.c`
     (alongside `glMaterialf`, `glPointParameterfv`, `gluBegin`, `funcN`, etc.).
4. Extend execution handling in `repl_executor.c`.
   - State-only commands normally go through `apply_state_cmd()`, then are
     dispatched from `execute_commands()`.
   - Geometry-emitting commands must respect open `glBegin` / tessellation
     state and any replay/fade semantics.
5. Extend flattening if the command affects control flow, local scope,
   declarations, labels/gotos, or per-frame expression evaluation.
6. Extend export/import in `repl_export.c` if the command must round-trip.
   - Normal user commands should be emitted by `write_canonical_cmd_as_c()` /
     `format_cmd_source_as_c()`.
   - Commands that appear in generated scaffold, but are not user source,
     must be skipped or parsed as scaffold during `load_from_file()`.
   - If the command changes generated display setup, keep the code-panel
     preview and exporter sharing the same source of truth. Display-scaffold
     state belongs in `g_render_state_lines` plus `RENDER_STATE_LINE_COUNT`;
     one-time init scaffold belongs in `g_init_bootstrap_repl` or host-only
     init arrays.
7. Extend autocomplete and parameter hints.
   - Add the callable signature to `k_func_completions` in
     `repl_command_spec.c`. `repl_autocomplete.c` only consumes these tables;
     it rarely needs edits unless the command uses a bespoke completion mode
     (e.g. `AC_MODE_POINT_PARAM`).
   - Enum-argument completion is automatic once the command appears in
     `k_enum_command_specs` with non-NULL `enums1` / `enums2`.
   - Add focused coverage in `test_repl_autocomplete.c` for ambiguous enum
     sets and multi-argument completions.
8. Extend editor/UI affordances and docs if the command is user-facing.
   Common touch points are the `tab_commands[]` help overlay in
   `ui_help_overlay.c`, `color_for_type()` in `ui_panels.c`,
   `README.md`, `CLAUDE.md`, `AGENTS.md`,
   command formatting, search/highlight behavior, and any editor-specific
   validation in `repl_editor.c`.
9. Update local stub headers when a new GL/GLU/GLUT symbol, enum, or callback
   enters compiled code. Mirror symbols in `include/GL/`, `include/GLUT/`,
   `include/OpenGL/`, and `include/GL/gl_stub_counts.h` as needed so
   `USE_GL_STUBS=1` builds remain useful.
10. Add tests at the narrowest useful level, then run the relevant suite.
    Typical coverage:
    - `test_repl_core_parse.c` for parser acceptance/rejection.
    - `test_repl_autocomplete.c` for completion and hints.
    - `test_repl_core_io.c` for export/import and scaffold placement.
    - `test_repl_core_examples.c` and `testdata/repl_examples_ui/*.golden.txt`
      when code-panel scaffold output changes.
    - `make test-stubs` before considering the change complete.

#### Impact of missing new command in `repl_command_spec.c`

Missing command metadata does not affect execution dispatch directly, but it
does degrade debug dumps and can change reformatting defaults. Keep
`test_repl_core_extra` covering every `CmdType` so omitted metadata is caught
before debug output or source normalization starts returning `CMD_UNKNOWN`.

### Add a new editor interaction

1. Add state and handlers in `repl_editor.c`.
2. Add rendering or hit-testing in the owner for the visible feature:
   `ui_menu_bar.c` for menu/dropdown/search-slot UI,
   `ui_color_picker.c` for literal color editing, or `ui_panels.c` for the
   remaining code/help/autocomplete/variable-panel surfaces.
3. Only promote new helpers into `repl_core_internal.h` when another module
   truly needs them.

### Add a new exported scaffold feature

1. Add or update the scaffold strings and typed section entry in
   `repl_export.c`.
2. Keep the code-panel preview and file exporter using the same source of
   truth.
3. Preserve round-tripping through `load_from_file()` whenever possible.

### Add a new predefined variable

There are two variable classes in the REPL:

- predefined globals stored in `g_predef_vars[]`
- scoped locals introduced by `for(...)` loop indices and `funcN(...)`
  parameters

Top-level assignments like `foo = 1;` only target predefined globals. Loop
indices and function parameters are collected as local `ExprVar` scopes during
flattening; they are not added to the global variable table.

To add a new predefined global:

1. Increase `MAX_PREDEF_VARS` in `repl_eval.h` if you need more slots.
2. Add the new variable name to the `names[]` array in `init_predef_vars()` in
   `repl_eval.c`.
3. Keep names within the `ExprVar.name[16]` storage limit.
4. If you add many globals, also check `MAX_WORKSPACE_HEADER_LINES` in
   `sample.h`, because workspace save/load persists globals through `// @var`
   header lines.

Existing export/import and persistence paths already iterate
`g_predef_vars[]`, so new predefined globals automatically:

- participate in expression evaluation
- round-trip through workspace headers in `repl_export.c`
- emit as file-scope globals in exported C
- reset in exported `reset_repl_vars()`

Special case: `t` is wired into animation/time helpers in `repl_core.c`. New
variables are plain globals unless you add similar runtime plumbing.

## Current Split Intent

The split is structural, not behavioral.

- `repl_core.h` stays stable.
- Editor, search, export, and execution each have a clear home.
- Import/export and example loading still go through the same command-commit
  path used by live editing.
- Tests remain the behavioral contract for the refactor.

## test_eval Harness Learnings (2026-04-18)

- `make test_eval USE_GL_STUBS=1` is the fastest no-GL regression loop for
  evaluator/parser/translation behavior.
- Translation helpers should be treated like public utility functions: guard
  null pointers and `out_sz <= 0` explicitly before touching output buffers.
- Prefer bounded formatting (`snprintf`) over `strncpy` for internal staging
  buffers to avoid truncation warnings and make NUL-termination guarantees
  obvious during maintenance.
- Keep at least one tiny-buffer test in `test_eval.c` for translation helpers
  to ensure truncation behavior is deterministic and safe.
