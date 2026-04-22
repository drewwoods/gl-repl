# OpenGL Immediate-Mode REPL

Interactive OpenGL command interpreter. Type GL commands, press `;` to execute,
and watch geometry render in real-time with a live code panel.

## Build

```bash
make sample          # Build main binary (freeglut)
make glut            # Build with system GLUT (macOS framework)
make test            # Build and run all tests
make clean           # Remove binaries
```

Requires: gcc with C2x support, OpenGL, GLUT/freeglut, AddressSanitizer enabled
by default in debug builds.

Include path must reach `../../include` (the project-wide `gl_includes.h`).

### Local GL Stub Headers

This sample ships no-op OpenGL, GLU, and GLUT headers under `include/` so
machines without system GL development packages can still compile and run
non-rendering tests.

```bash
make test-stubs
make sample USE_GL_STUBS=1
```

`USE_GL_STUBS=1` prefers this sample's local `include/` directory and drops
`-lGL`, `-lGLU`, `-lglut` from the link flags. Stub-mode objects go to
`build/*-gl-stubs` so they don't mix with rendering builds.

Constraints:

- Stubs are for compilation and non-rendering tests only. No window, no pixels,
  no real GL context. Do not make stubs the default rendering path.
- If the sample starts calling a new GL/GLU/GLUT symbol, extend the matching
  stub in `include/GL/`, `include/GLUT/`, or `include/OpenGL/`.
- Keep stubs minimal and no-op — model types, constants, and callable
  signatures well enough for builds, not a fake renderer.
- After touching stubs, verify both paths: `make test-stubs`, `make sample
  USE_GL_STUBS=1`, `make sample`.

Header layout: `include/GL/gl.h` (fixed-function GL), `include/GL/glu.h`
(quadrics/projection/tessellator), `include/GL/freeglut.h` (GLUT/freeglut
callbacks + shapes); `glext.h`, `glut.h`, `GLUT/glut.h`, `OpenGL/gl.h`,
`OpenGL/glu.h` are compatibility wrappers.

## Run

```bash
./sample                  # Fresh session
./sample output.c         # Reload saved session (single file)
./sample workspace/       # Load every *.c under workspace/ as a user scene
./sample --noaccum        # Disable accumulation buffer AA
./sample --dump-code      # Print loaded buffer to stdout
```

## Test

```bash
make test_eval             # Expression evaluator tests
make test_format           # Indentation/formatting tests
make test_repl_core_parse  # Command parser tests
make test_repl_core_format # Reformatter tests
make test_repl_core_commit # Commit pipeline tests
make test_repl_core_io     # Save/load round-trip tests
```

Run all: `make test`

## File Layout

| File | Responsibility |
|------|----------------|
| `sample.c` | GLUT callback wrappers, `main()`, window setup |
| `sample.h` | Shared types (`GLCmd`, `CmdType`, `SceneLight`, `CfgItem`), extern globals, utility declarations, `CFG_DEFAULT_*` macros |
| `repl_core.c` | Parser, normalization, display callback, GL init, source/depth queries |
| `repl_commit.c` | Float declarations, variable assignments, structured block commits, close-brace commits |
| `repl_core.h` | Public API (parse, flatten, display, input callbacks, user scene + workspace) |
| `repl_core_internal.h` | Test-visible internals (normalize/commit pipeline, `feed_line`, `load_line_to_input`, `repl_promote_example_if_needed`) |
| `repl_editor.c` | Keyboard/mouse routing, commit orchestration, feed-line entrypoint |
| `repl_clipboard.c` | Line selection anchors, command clipboard buffer, copy/cut/paste behavior |
| `repl_undo.c` | Undo/redo snapshots, history rings, example auto-promote hook before mutation |
| `repl_camera_controls.c` | Scene camera pointer state, orbit/pan/zoom drags, wheel zoom velocity, momentum tick |
| `repl_actions.c` | Config item table, config shortcuts, menu actions, startup config defaults |
| `repl_code_panel_layout.c` | Pure code-panel wrapping, row counts, segment lookup, cursor-row mapping |
| `repl_code_panel_layout.h` | `CodePanelTextLayout` / `CodePanelWrapIter` API shared by UI, export dumps, tests |
| `repl_code_panel_document.c` | Code-panel document row model, scroll-follow calculation, hit-test targets |
| `repl_code_panel_document.h` | `CodePanelDocumentLayout` API consumed by UI and scrolling tests |
| `repl_replay_annotations.c` | Replay-time source annotations, variable substitution, evaluated command display text |
| `repl_replay_annotations.h` | Code-panel replay annotation API |
| `repl_menu_bar.c` | Code-panel menu bar, dropdowns, config right-click handling, search slot |
| `repl_menu_bar.h` | Menu/pin hit-test and dropdown state API |
| `repl_color_picker.c` | Floating color picker and literal color swatch rendering/mutation |
| `repl_color_picker.h` | Color-picker input/render bridge API |
| `repl_help_overlay.c` | Modal F1 help overlay (Commands / Keys tabs, dynamic F-key bindings) |
| `repl_help_overlay.h` | Help overlay render entrypoint |
| `repl_examples.c` | Predefined example data (`g_examples[]`, `g_example_names[]`) |
| `repl_examples.h` | Example query API (`repl_examples_count/name/lines`) |
| `repl_export.c` | `save_output` / `load_from_file`, workspace header directives, `@scene-name` / `@workspace-dir` markers |
| `scene_render.c` | 3D scene: camera, grid themes, axes themes, lights, vertex overlays, outline pass |
| `scene_render.h` | Declares `render_3d_scene()` |
| `ui_panels.c` | Code-panel row rendering, autocomplete, variable panel, panel hit routing, inline rename state |
| `ui_panels.h` | UI panel render + hit-test declarations, compatibility declarations, rename state API |
| `repl_eval.c` | Expression evaluator (recursive descent), REPL<->C translators, for-loop parsers |
| `repl_eval.h` | Evaluator types (`ExprVar`, `ExprCtx`), function declarations |
| `cmd_format.c` | Pure indentation/depth computation (no GL dependency) |
| `cmd_format.h` | Formatting types (`FmtCmd`, `FmtType`), indent functions |
| `REPL_REFACTOR_MAP.md` | Mermaid ownership map for editor-adjacent refactor slices |

## Conventions

- Global variables prefixed `g_` (e.g., `g_num_cmds`, `g_cam_rx`)
- Static helpers are file-scoped; public API goes through `repl_core.h`
- Config toggles use the `CfgItem` pattern: add entries to `g_cfg_items[]` in
  `repl_actions.c`; `CFG_ITEM_COUNT` auto-computes via `sizeof`
- New GL commands: add to `CmdType` enum in `sample.h`, then handle in
  `parse_command()`, `execute_commands()`, and `flatten_range()` in `repl_core.c`
- Keyboard bindings: `keyboard_func()` for ASCII keys (Ctrl+X = key code X-64),
  `special_func()` for F-keys/arrows in `repl_editor.c`
- Expression variables: `ExprVar` struct in `repl_eval.h`, predefined set in
  `g_predef_vars[]`

## Adding Grid/Axes Themes

Grids and axes are themeable via a `switch` in `scene_render.c`:
1. Add a new entry to the `GridTheme` (or `AxesTheme`) enum in `sample.h`
   before the trailing `_COUNT` sentinel
2. Add the name string at that enum's index in `g_grid_names[]`
   (or `g_axes_names[]`) in `repl_core.c` — both arrays use designated
   initializers keyed on the enum, so order is validated at compile time
3. Add a new `case GRID_THEME_XXX:` in `draw_grid()` (or `draw_axes()`)
   in `scene_render.c`
4. The theme cycles via F3 (grid) / F4 (axes) in `repl_editor.c`

## Adding Menu Bar Items

The top row is a menu bar in `repl_menu_bar.c` styled after the Header
Wireframes v2 mock. Left side has top-level menus (File / Scene / Config);
right side has pinned buttons (Search / Replay).

To add an **item** to an existing top-level menu:
1. Extend the per-menu enum (e.g. `FILE_ITEM_*` or the `SCENE_OFF_*` block) and
   bump the trailing `*_COUNT`
2. Add the label in `menu_item_label()` and shortcut (if any) in
   `menu_item_shortcut()` in `repl_menu_bar.c`
3. Add the action branch in `repl_action_menu_item_activate()` in
   `repl_actions.c`; return `1` for action items (menu closes), `0` for
   cycle/toggle items (menu stays open; click-outside dismisses)

To add a **new top-level menu**: extend the `MENU_*` enum (before
`NUM_MENUS`), add a label in `g_menu_labels[]`, and handle the new id in
`menu_item_count` / `menu_item_label` / `menu_item_shortcut` in
`repl_menu_bar.c`, plus `repl_action_menu_item_activate()` in
`repl_actions.c` for side effects.

To add a **pinned right-side button**: extend `PIN_*` enum, append a label
to `g_pin_btn_labels[]` in `repl_menu_bar.c`, and add a `case` in
`handle_code_panel_press()` inside the `repl_menu_bar_pin_hit` block.

## User Scene System

The REPL keeps up to `MAX_USER_SCENES` (= 8) independent scenes in
`g_user_scenes[]` (in `repl_core.c`). Slot 0 is the pinned "home" scene — the
pre-example editor state captured on first example load, never auto-evicted.
Each `UserScene` stores `GLCmd` array + `num_cmds` + `edit_line` + predefined
variable values + a scene `name` + `last_touch` tick for LRU.

### Active slot and auto-promotion

- `g_active_user_scene` (`-1` means an example or a fresh empty workspace is
  loaded instead of a user scene).
- `push_undo_snapshot()` in `repl_editor.c` calls
  `repl_promote_example_if_needed()` before every mutation. If the user is
  editing an example, that call allocates a fresh slot, copies the current
  state into it, inherits the example's name (de-duplicated via
  `derive_unique_scene_name`), and sets `g_active_user_scene`. The editor
  keeps going — the user never sees the promotion directly, but subsequent
  edits now accumulate into a user scene.

### LRU eviction

When every non-home slot is full *and* a workspace directory is bound, a 9th
promotion picks the LRU non-pinned, non-active slot, flushes it to
`<workspace_dir>/<slug>.c` via `evict_scene_to_workspace()`, and reuses the
freed index. With no workspace bound the promotion is rejected with a status
message (user has to save workspace first to unlock eviction).

### Inline rename

- `ui_panels_begin_rename(slot)` / `ui_panels_handle_rename_key(...)` /
  `ui_panels_cancel_rename()` in `ui_panels.c`.
- Triggered by the Scene → "Rename active scene" menu item; typing updates a
  status-bar prompt; Enter commits via `repl_user_scene_rename` (which trims,
  de-duplicates, and guards against an empty name), Esc cancels.
- Path-unsafe chars (`/`, `\`, `:`) and non-printables are filtered at input
  time since names become filesystem slugs on workspace export.
- The key dispatcher in `repl_editor.c` forwards keys to
  `ui_panels_handle_rename_key` right after `handle_search_key`, so rename
  mode swallows input even when other overlays aren't open.

### Workspace I/O

- `repl_save_workspace(dir)` mkdirs `dir` (idempotent), flushes the active
  slot, then iterates every occupied slot: `install_scene_into_live` + a
  stash/restore pattern wraps each slot so `save_output()` sees that scene's
  live state. `g_export_scene_name_hint` is set per-slot so the exported
  header's `// @scene-name` reflects the correct name. The bound dir is
  remembered in `g_workspace_dir` and stamped into every single-file export
  as `// @workspace-dir <path>`.
- `repl_load_workspace(dir)` opens `dir`, loads each `*.c` into a fresh slot
  via `load_scene_file_into_slot`, and restores live editor state around the
  iteration. Names come from `@scene-name` headers (or the filename stem as
  fallback).
- Single-file save/load still works unchanged. Files round-trip between
  workspace and single-file modes because the header encodes both
  `@scene-name` and `@workspace-dir`.

### Scene menu layout

`SCENE_OFF_*` offsets in `repl_menu_bar.c` place fixed rows above the user-scene
list (`New empty scene`, `Save to output.c`, `Rename active scene`). User
scenes follow at `SCENE_OFF_SCENES`; rows are dense (unused slots skipped via
`repl_scene_menu_slot_for_dense_index`). The active scene row is drawn with
accent color.

### Public API touch points (in `repl_core.h`)

`repl_user_scene_count`, `repl_user_scene_slot_used`, `repl_user_scene_name`,
`repl_user_scene_rename`, `repl_load_user_scene_idx`, `repl_active_user_scene`,
`repl_save_workspace`, `repl_load_workspace`, `repl_workspace_dir`,
`repl_set_workspace_dir`. Back-compat helpers: `repl_user_scene_valid()` still
reports "any slot occupied?", `repl_load_user_scene()` still loads slot 0.

### F12 cycle

`examples → user scenes (in slot order) → back to first example`. Handles both
"active example" and "active scene" starting states.

## Example Metadata

Built-in examples in `repl_examples.c` can prefix their command list with:
1. Contiguous `// @cfg <slug> = <value>` lines.
2. An optional 5-line `// camera` preset block.

`repl_core.c` consumes leading metadata before feeding remaining lines through
the commit pipeline, so metadata stays hidden from the code panel. `@cfg`
parsing reuses `parse_workspace_header_line()` from `repl_export.c`, restricted
to these scene-presentation slugs:

`wireframe`, `grid`, `grid_major`, `grid_extent`, `axes`, `vertex_labels`,
`normal_vectors`, `vertex_outlines`, `vertex_points`, `vertex_guides`,
`light_indicators`, `backdrop`, `camera_rotate`.

Non-leading `@cfg` lines are not metadata — they stay as ordinary comments.

### Reset and restore rules

- Every example load resets the allowed non-camera scene-presentation settings
  to built-in defaults *before* applying the example's leading `@cfg`
  metadata. This prevents stale grid/axes/overlay/backdrop state from leaking
  across examples.
- Camera is intentionally excluded from that reset. Examples inherit the
  current `g_cam_*` state unless they supply the explicit leading `// camera`
  header.
- `restore_user_scene()` restores commands and predefined variables only.
  Leaving an example does not restore camera or other presentation state.

### Shared defaults

Keep the single source of truth for example-owned presentation defaults in
the `CFG_DEFAULT_*` macro block in `sample.h`. `repl_core.c` initializers,
example reset helpers, and focused example tests should reuse those macros
instead of duplicating literals.

When changing example-metadata behavior, inspect `repl_core.c`,
`repl_export.c`, `repl_examples.c`, `sample.h`, and
`test_repl_core_examples.c` together. `make test_repl_core_examples` is the
focused regression suite for this area; `make test` for broader REPL state.

### Open: desired vs. inherited @cfg

`@cfg` settings currently can't distinguish "desired" (always apply) from
"inherited" (only if unset). A desired/inherited split would let user scenes
save their own presentation config without being overwritten on the next
example switch. Deferred — see `feature/multi-user-scenes.md`.

## Architecture

### Rendering Pipeline

`display_func()` in `repl_core.c` drives each frame:
1. Clear color/depth/accum buffers, save predef var values
2. If accumulation-buffer AA is enabled (`g_accum_aa_enabled`), loop
   `g_accum_samples` times with sub-pixel jitter offsets from
   `g_jitter_table[]`, calling `render_3d_scene()` per pass and
   accumulating with `glAccum(GL_ACCUM, 1/samples)`; final
   `glAccum(GL_RETURN, 1.0)` averages into framebuffer. The jitter is
   applied as a frustum shift in `render_3d_scene()` via
   `g_accum_jitter_x/y` → adjusted `glFrustum()` bounds
3. `render_3d_scene()` in `scene_render.c`: projection setup → camera
   transforms → `execute_commands()` (user geometry fill pass) → replay
   fade batches → grid/axes/orbit-target (depth-masked, blended) →
   polygon outline overlays → vertex number/normal/guide overlays
4. 2D overlays: code panel, autocomplete popup, example dropdown,
   variable slider panel, config menu, help overlay, search overlay

### Two-Level Command Model

The core data flow is **source commands → flat commands → GL calls**:

- **`g_cmds[MAX_COMMANDS]`** — source-level array. Each `GLCmd` holds
  parsed type/args, normalized `source[]` text, and flags (`has_vars`,
  `valid`, `is_auto`). Edited directly by the user via the code panel.
- **`g_flat_cmds[MAX_COMMANDS]`** — expanded array. For-loops are
  unrolled, function calls are inlined, if-blocks are resolved.
  Each flat cmd records `src_cmd_idx` (owning source line),
  `call_src_cmd_idx` (immediate call site), and `func_scope_mask`
  (active function scopes) for cursor highlighting.
- **Trigger:** any edit sets `g_flat_dirty = 1` (via
  `mark_normals_dirty()`); `flatten_commands()` rebuilds
  `g_flat_cmds[]` on the next frame before rendering.

### Command Lifecycle

1. **Input** — user types into `g_input[]` (max 1024 chars)
2. **Commit** — pressing `;` calls the commit dispatch chain in
   `keyboard_func()` in `repl_editor.c`. There are TWO distinct paths:
   - **Interactive `;` key** (`repl_editor.c`, `key == ';'` block):
     `g_input` does NOT include the `;` — the keystroke triggers the
     commit but is not appended. Commit handlers must accept input
     without a trailing `;`.
   - **`feed_line()`** (`repl_editor.c`): copies the full line
     (including `;`) into `g_input`, then runs the same dispatch chain.
     Used by file loading and example loading.
   - **Enter key** (insert mode): `g_input` may or may not have `;`
     depending on what the user typed.
   The dispatch chain calls `try_commit_*()` handlers in order:
   `try_commit_float_decl` → `try_assign_variable` → `try_commit_close_brace`
   → `try_commit_for_loop` → `try_commit_func_def` → `try_commit_if_block`
   → `repl_parse_and_normalize()` (general GL commands).
   **Ordering matters**: `try_commit_float_decl` MUST run before
   `try_assign_variable`, otherwise `float x` is misread as an
   assignment. Each handler returns 1 if it consumed the input
   (success or error with status message), 0 if it didn't match.
   If all handlers return 0, `parse_command()` in `repl_core.c`
   sets `"Unknown cmd."` status (`repl_core.c`, end of
   `parse_command`).
3. **Parse** — `parse_command()` in `repl_core.c` matches the line to a
   `CmdType`, evaluates argument expressions via `eval_expr()`, stores
   result in `GLCmd.args[]` and normalized text in `GLCmd.source[]`
4. **Flatten** — `flatten_range()` recursively expands the source array:
   for-loops iterate (capped at 100k visits), function calls inline the
   body with actual args, if-blocks evaluate conditions. Recursion
   depth limited to `MAX_FLATTEN_CALL_DEPTH=32`
5. **Execute** — `execute_commands()` walks `g_flat_cmds[]` emitting GL
   calls. Re-evaluates expressions with `has_vars` flag each frame
   (for animated `t`, etc.)

### Commit Dispatch Sites

The `try_commit_*` handler chain is consolidated into four helpers in
`repl_commit.c`:
- `try_commit_var_statements()` — float decl, then assign
- `try_commit_block_structs()` — close-brace, for, func, if
- `try_commit_any()` — both groups in canonical order
- `try_commit_var_statements_then_insert()` — var variant used by the
  overwrite-mode Enter key, which must flip to insert mode on success

Dispatch sites then call these helpers instead of open-coding the chain:
1. **`;` key handler** — `key == ';'` block in `keyboard_func()` calls
   `try_commit_any()`
2. **Enter key, insert mode** — calls `try_commit_var_statements()` and
   `try_commit_block_structs()` to maintain the insert-mode behavior
3. **Enter key, overwrite mode** — uses
   `try_commit_var_statements_then_insert()` plus
   `try_commit_block_structs()`
4. **`feed_line()`** — the programmatic entry point calls
   `try_commit_any()`

When adding a new handler, add it to the right helper rather than all
call sites. Ordering inside each helper is load-bearing:
`try_commit_float_decl` MUST run before `try_assign_variable`, otherwise
`float x;` is misread as an assignment to an identifier named "float".

### Editing Existing Lines

When the user navigates to an existing line, `load_line_to_input()`
strips the trailing `;` and whitespace from `cmd.source` before
loading into `g_input`. This means re-committing the line goes
through the no-semicolon path. Commit handlers that check for `;`
must also accept end-of-string as a valid terminator.

### Float Variable Declarations (`CMD_VAR_DECLARE`)

`try_commit_float_decl()` in `repl_commit.c` handles `float name;`
syntax. Current implementation supports multi-name (`float a, b, c;`)
and initializers (`float x = 1;`), but there is an open design
question about simplifying to single-name, no-initializer only.

Key details:
- **Placement rule:** new `CMD_VAR_DECLARE` lines are inserted at the
  top of non-decl code (index of first non-`CMD_VAR_DECLARE` cmd),
  regardless of cursor position. This guarantees every reference
  follows its declaration (no `n = tmp` before `float tmp`). Editing
  an existing decl still overwrites in place. Init expressions can
  therefore only reference already-declared predef vars — no scope
  locals are visible at block depth 0.
- `CMD_VAR_DECLARE` is a no-op in `execute_commands()` and
  `flatten_range()` — registration into `g_predef_vars[]` happens at
  commit time via `declare_predef_var()`
- `GLCmd` fields: `var_names[MAX_NAMES_PER_DECL][16]`, `var_decl_count`
- Editing an existing `CMD_VAR_DECLARE` line works: the overwrite
  detection runs before the "already declared" validation loop, and
  names carried over from the old decl are exempted from the duplicate
  check (they get undeclared before the new registration runs).
- `delete_cmd_range()` guards against deleting a declaration whose
  variable is still referenced elsewhere (uses `source_uses_ident()`)
- C export writes `// @declare name` markers; import via
  `import_parse_declare_marker()` in `repl_export.c` reconstructs
  the `CMD_VAR_DECLARE` commands, bypassing `try_commit_float_decl`
- `repl_examples.c` has multi-name declarations (e.g. `"float n, x, y, z, j, k;"`)
  — if simplifying to single-name, these must be split into separate lines
- Related helpers in `repl_eval.c`: `declare_predef_var()`,
  `undeclare_predef_var()`, `find_predef_var_idx()`,
  `is_reserved_ident()`, `source_uses_ident()`,
  `validate_expression_idents()`

### Save/Load (output.c)

`repl_export.c` handles bidirectional text format:
- **Export** (`save_output()`): writes a standalone C file with header
  comments embedding workspace state (`@var name=value`,
  `@cfg setting=value`, `@scene-name <name>`, `@workspace-dir <path>`),
  camera state as the raw `glTranslatef`/`glRotatef` sequence the REPL
  uses internally, predefined vars as globals, REPL functions as C
  functions, and `display()` body containing the user's geometry commands.
  The workspace iterator in `repl_core.c` sets `g_export_scene_name_hint`
  before each slot's save so the hint wins over `g_active_user_scene`.
- **Import** (`load_from_file()`): line-by-line scan parses camera state
  and workspace directives, detects function definitions (converts C
  syntax back to REPL), and feeds geometry lines through `feed_line()`.
  Pending directives (`g_pending_scene_name`, `g_pending_workspace_dir`)
  are read by the caller after `load_from_file` returns so the importer
  can name the new slot and remember the workspace dir.

### Replay System

Step-by-step execution visualization in `repl_core.c`:
- `g_replay_state` (OFF/PLAYING/PAUSED/DONE), `g_replay_pc` (program
  counter into `g_flat_cmds[]`), `g_replay_speed` (multiplier)
- During playback, `g_num_flat_cmds` is clamped to `replay_exec_limit()`
  so only commands up to the PC render
- `g_replay_fade_batches[]` — circular buffer of fading geometry
  snapshots; old geometry fades out as new geometry appears, rendered
  in a separate blended pass after the main fill pass
- Toggled via Ctrl+G or the Replay header button

### Undo/Redo

Circular snapshot buffers in `repl_undo.c`:
- `ReplUndoSnapshot` captures `cmds[]`, `num_cmds`, `edit_line`,
  `predef_vals[]` — full editor state
- `g_undo_buf[32]` and `g_redo_buf[32]` with head/count tracking
- `push_undo_snapshot()` called before any mutation (delete, paste,
  reformat, etc.); `pop_undo_snapshot()` on Ctrl+Z; `do_redo()` on
  Ctrl+Y. Also the hook where `repl_promote_example_if_needed()` fires
  so editing an example auto-creates a user scene.
- Pushing clears the redo stack; undo moves current state to redo

### Autocomplete

Symbol matching and function parameter hints in `repl_core.c`:
- `g_ac_matches[]` — matched completions from GL command/constant tables
- `g_ac_ghost[]` — suffix to append to input on Tab accept
- `g_ac_hint[]` — parameter list hint shown below cursor
- Modes: `AC_MODE_FUNC_PREFIX` (after `foo(` → param hints),
  `AC_MODE_ENUM_ARG1/2` (GL constant completion),
  `AC_MODE_POINT_PARAM` (3D point coordinates)

### Search

Case-insensitive text search in `repl_search.c`:
- Activated by Ctrl+F; query stored in `g_search_query[]`
- `repl_search_find_next_in_text()` finds substring matches across
  all visible lines (header, user code, footer)
- `g_search_hit_line`/`g_search_hit_char` track current match position
- Integrated with code panel rendering for match highlighting

### Config Menu

Declarative toggle system in `repl_actions.c`:
- `g_cfg_items[]` array of `CfgItem` structs: `{ label, key_hint,
  int *value, n_states, state_names[] }`
- Each item is a toggle (2 states, default OFF/ON) or cycle (>2 states
  with named entries, e.g. grid themes)
- Rendered by the Config dropdown in `repl_menu_bar.c`; menu clicks and
  F-key/Ctrl-key shortcuts dispatch through `repl_actions.c`
- Adding a config item: append to `g_cfg_items[]` — count is
  auto-computed via `sizeof`

## Key Controls

| Key | Action |
|-----|--------|
| `;` | Execute/commit current line |
| Enter | Insert new line |
| Up/Down | Navigate lines |
| Tab | Autocomplete |
| Ctrl+S | Save to output.c |
| Ctrl+Z | Undo |
| Ctrl+R | Reformat all lines |
| Ctrl+T | Toggle time variable `t` |
| F1 | Help overlay |
| F2-F11 | Toggle visual overlays |
| F12 | Cycle examples and user scenes |

## Supported Commands

```
glBegin(MODE), glEnd()
glVertex3f(x,y,z), glVertex2f(x,y)
glNormal3f(x,y,z)
glColor3f(r,g,b), glColor4f(r,g,b,a)
glTranslatef(x,y,z), glScalef(sx,sy,sz), glRotatef(deg,x,y,z)
glPushMatrix(), glPopMatrix()
glEnable(CAP), glDisable(CAP)
  CAP: GL_DEPTH_TEST, GL_LIGHTING, GL_COLOR_MATERIAL, GL_NORMALIZE,
       GL_LINE_SMOOTH, GL_POINT_SMOOTH, GL_BLEND, GL_CULL_FACE,
       GL_LIGHT0, GL_LIGHT1, GL_LIGHT2, GL_LIGHT3
glShadeModel(MODE)
glPointSize(size)
glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, const, linear, quadratic)
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA|GL_ONE)
glColorMaterial(face, mode), glMaterialf(face, pname, value)
  glColorMaterial mode: GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR, GL_EMISSION, GL_AMBIENT_AND_DIFFUSE
glLightModeli(pname, param), glFrontFace(mode)
glDepthMask(GL_TRUE|GL_FALSE)
gluSphere(r, slices, stacks)
gluCylinder(base, top, height, slices, stacks)
gluDisk(inner, outer, slices, loops)
gluPartialDisk(inner, outer, slices, loops, start, sweep)
glutSolidTorus(inner, outer, nsides, rings)
for(var, start, end[, step]) { body }
func0..func9[(params)] { body }
if(expr) { body }
// comment
float name[, name2, ...];  (variable declaration)
var = expr;
```

## Math

Functions: `sin`, `cos`, `tan`, `sqrt`, `abs`, `pow`, `min`, `max`, `floor`, `ceil`, `fmod`, `rand(seed[, iter])`
Constants: `PI`, `TAU`
Variables: declared via `float name;` — only `t` is predefined (Ctrl+T toggles animation).
Other names (`x`, `y`, `z`, etc.) must be declared before use.
`MAX_PREDEF_VARS` = 16 (1 reserved for `t`, 15 user-declarable slots).
