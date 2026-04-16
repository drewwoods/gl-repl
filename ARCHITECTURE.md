# REPL Architecture

## Overview

The immediate-mode REPL is now split across focused translation units instead
of one monolithic `repl_core.c`.

- `repl_core.c`: parser, normalization, flattening, execution, replay, example
  loading orchestration, and OpenGL initialization.
- `repl_search.c`: search state, match navigation, and search-mode keyboard
  handling.
- `repl_export.c`: fixed scaffold strings, init bootstrap tables, import/export
  translation, save/load, and code-panel dump helpers.
- `repl_editor.c`: editor state, undo/redo, selection, clipboard, commit
  helpers, feed-line entrypoint, and GLUT input handlers.
- `repl_eval.c`: expression parsing and evaluation.
- `ui_panels.c`: 2D code/search/help/config/variable-panel rendering and panel
  hit-testing.
- `scene_render.c`: 3D scene rendering, overlays, and replay HUD.
- `sample.c`: application entrypoint and GLUT callback wiring.

The public API is still `repl_core.h`. Cross-module runtime/test helpers live in
`repl_core_internal.h`. Shared globals and UI-visible state still live in
`sample.h`.

## Data Flow

### Edit and commit path

1. GLUT keyboard/mouse callbacks enter through `repl_editor.c`.
2. Editor commit helpers convert input text into commands by calling
   `repl_parse_command*()` or `repl_parse_and_normalize()` in `repl_core.c`.
3. Parsed commands are stored in `g_cmds[]`.
4. `mark_normals_dirty()` and `g_flat_dirty` invalidate downstream derived
   state.

### Execution path

1. `flatten_commands()` in `repl_core.c` expands loops, functions, conditionals,
   and variable-driven commands into `g_flat_cmds[]`.
2. `scene_render.c` prepares the frame and calls `execute_commands()`.
3. `execute_commands()` issues fixed-function OpenGL calls against the flattened
   command stream.

### Search path

1. `repl_search.c` scans `g_cmds[].source`.
2. Search hit movement calls `navigate_to_line()` from `repl_editor.c`.
3. `ui_panels.c` renders search highlights from the shared search globals.

### Import/export path

1. `repl_export.c` owns the fixed scaffold shown in the code panel and emitted
   to exported C.
2. `save_output()` writes scaffold text plus translated command bodies.
3. `load_from_file()` converts exported C back into REPL lines and replays them
   through `feed_line()` from `repl_editor.c`, so import uses the same commit
   rules as interactive editing.

## Ownership

### `repl_core.c`

Owns the semantic model and execution pipeline.

- `g_cmds[]`, `g_num_cmds`
- `g_flat_cmds[]`, `g_num_flat_cmds`
- flattening helpers and scope/depth caches
- parser tables and command normalization
- replay engine and replay stepping
- example orchestration and GL init

### `repl_editor.c`

Owns transient editor and interaction state.

- `g_input`, `g_input_len`, `g_cursor_pos`
- `g_edit_line`, `g_inserting`, newline buffer
- undo/redo ring
- selection and clipboard state
- code-panel scroll and resize state
- variable-drag/config-menu interaction state
- feed-line and block-commit helpers
- keyboard, special-key, mouse, motion, and timer callbacks

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
- `save_output()`, `load_from_file()`
- import translation helpers
- code-panel dump helpers

## Shared State Rules

- `sample.h` remains the single shared runtime/UI header.
- `repl_core_internal.h` is the internal bridge for non-public helpers needed by
  tests or sibling `.c` files.
- No extra per-module headers are required for the split.
- Parser/execution internals stay in `repl_core.c`; editor/search/export call
  into them rather than duplicating logic.

## Key Pipelines

### Structured block commits

`repl_editor.c` handles user-facing block syntax:

- `for(...) { ... }`
- `funcN(...) { ... }`
- `if(...) { ... }`
- closing `}`

These helpers update `g_cmds[]` directly, but they still rely on parser and
scope helpers from `repl_core.c` for validation and normalization.

### Flattening

`flatten_range()` in `repl_core.c` is still the only place that expands:

- `CMD_FOR_BEGIN .. CMD_FOR_END`
- `CMD_FUNC_DEF .. CMD_FUNC_END`
- `CMD_IF_BEGIN .. CMD_IF_END`
- variable-dependent commands

That preserves a single semantic source of truth for execution, replay, and
export-derived behavior.

### Replay

Replay state and stepping remain in `repl_core.c`, but editor callbacks in
`repl_editor.c` drive it.

- editor input toggles replay modes and stepping
- replay helpers rebuild source-line highlighting state
- `scene_render.c` consumes replay state to draw the HUD and replay overlays

## Startup and Examples

`load_initial_commands()` remains in `repl_core.c`.

- If an import file is provided and `load_from_file()` succeeds, startup uses
  the imported session.
- Otherwise core loads a built-in example.
- Example lines are still committed through `feed_line()`, so examples,
  imported files, and interactive editing share the same commit pipeline.

## Extension Guide

### Add a new command

1. Add the `CmdType` in `sample.h`.
2. Add the matching name to the `cmd_type_name()` table in `repl_core.c` in
   the same order as the enum — `repl_debug_dump_editor()` and
   `repl_debug_dump_flat_commands()` index into it, so a missing or
   misordered entry silently mislabels every subsequent command in the
   debug dump (see commit `abccf5c` for `CMD_FRONT_FACE`).
3. Extend parser handling in `repl_core.c`.
4. Extend execution handling in `repl_core.c`.
5. Extend flattening if the command affects control flow or local scope.
6. Extend export/import in `repl_export.c` if the command must round-trip.
7. Extend autocomplete or editor affordances in `repl_core.c` /
   `repl_editor.c` if needed.

#### Impact of missing new command in `cmd_type_name()`
1. Every type name after CMD_VAR_DECLARE in the enum would be shifted by one.
   The names[] array is positional — indexed by the integer value of CmdType.
   With the entry missing, CMD_LABEL (enum value 27) would print as
   "CMD_VAR_DECLARE", CMD_GOTO as "CMD_LABEL", CMD_GLU_SPHERE as "CMD_GOTO",
   and so on down the line. Debug dumps would show the wrong type for ~18
   command types.
2. Out-of-bounds read on the last enum value. The bounds check at
   repl_core.c:751 is if (t >= 0 && t < CMD_TYPE_COUNT), which allows index
   CMD_TYPE_COUNT - 1 (i.e. CMD_CLEAR_COLOR). But the array only had
   CMD_TYPE_COUNT - 1 elements, so that access reads one past the end —
   undefined behavior, likely garbage or a crash.

No effect on rendering, parsing, or command execution — cmd_type_name is only
called from repl_debug_dump_editor() and repl_debug_dump_flat_commands(). So
the REPL would work fine until someone hit Ctrl+D to debug, at which point the
output would be misleading (wrong names) and potentially crash on the last
command type.

### Add a new editor interaction

1. Add state and handlers in `repl_editor.c`.
2. Add rendering or hit-testing in `ui_panels.c` if the feature is visible.
3. Only promote new helpers into `repl_core_internal.h` when another module
   truly needs them.

### Add a new exported scaffold feature

1. Add or update the scaffold strings in `repl_export.c`.
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
