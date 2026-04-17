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

## Run

```bash
./sample                  # Fresh session
./sample output.c         # Reload saved session
./sample --noaccum        # Disable accumulation buffer AA
./sample --dump-code      # Print loaded buffer to stdout
```

## Test

```bash
make test_eval            # Expression evaluator tests
make test_format          # Indentation/formatting tests
make test_repl_core_parse # Command parser tests
make test_repl_core_format # Reformatter tests
make test_repl_core_commit # Commit pipeline tests
make test_repl_core_io    # Save/load round-trip tests
```

Run all: `make test`

## File Layout

| File | Responsibility |
|------|----------------|
| `sample.c` | GLUT callback wrappers, `main()`, window setup |
| `sample.h` | Shared types (`GLCmd`, `CmdType`, `SceneLight`, `CfgItem`), extern globals, utility declarations |
| `repl_core.c` | Parser, command execution, flattening, save/load, user scene storage |
| `repl_core.h` | Public API for repl_core (parse, flatten, display, input callbacks, user scene) |
| `repl_core_internal.h` | Test-visible internals (normalize/commit pipeline, `feed_line`, `load_line_to_input`) |
| `repl_editor.c` | Keyboard/mouse handling, undo/redo (`UndoSnapshot`), `CfgItem` array, F-key dispatch |
| `repl_examples.c` | Predefined example data (`g_examples[]`, `g_example_names[]`) |
| `repl_examples.h` | Example query API (`repl_examples_count/name/lines`) |
| `scene_render.c` | 3D scene: camera, grid themes, axes themes, lights, vertex overlays, outline pass |
| `scene_render.h` | Declares `render_3d_scene()` |
| `ui_panels.c` | Code panel, header buttons, example/scene dropdown, autocomplete, help overlay, config menu |
| `ui_panels.h` | UI panel render + hit-test declarations |
| `repl_eval.c` | Expression evaluator (recursive descent), REPL<->C translators, for-loop parsers |
| `repl_eval.h` | Evaluator types (`ExprVar`, `ExprCtx`), function declarations |
| `cmd_format.c` | Pure indentation/depth computation (no GL dependency) |
| `cmd_format.h` | Formatting types (`FmtCmd`, `FmtType`), indent functions |

## Conventions

- Global variables prefixed `g_` (e.g., `g_num_cmds`, `g_cam_rx`)
- Static helpers are file-scoped; public API goes through `repl_core.h`
- Config toggles use the `CfgItem` pattern: add entry to `g_cfg_items[]` array
  in `repl_editor.c`, it auto-computes count via `sizeof`
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

## Adding Header Buttons

Header buttons live in `ui_panels.c`:
1. Increment `NUM_HEADER_BTNS`
2. Add label to `g_header_btn_labels[]`
3. Add active-state condition in the render loop (`int active = ...`)
4. Add `case N:` in `handle_code_panel_press()` switch

## User Scene System

User scenes are stored independently from predefined examples (`repl_core.c`):
- `UserScene` struct holds `GLCmd` array + `num_cmds` + `edit_line` + variable values
- `save_user_scene()` — called automatically before first example load
- `restore_user_scene()` — called from Scene button, F12 cycle, or dropdown
- Public API: `repl_user_scene_valid()`, `repl_load_user_scene()` (in `repl_core.h`)
- Example loading path: `repl_load_example()` → `load_example()` → saves user scene
  if not already saved → `load_example_lines()` which calls `feed_line()` per line
- F12 cycles: examples → user scene (if saved) → back to first example
- Example dropdown in `ui_panels.c` appends user scene entry when saved
- Designed for future expansion to multiple scene slots

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

The `try_commit_*` handler chain is duplicated at 5 sites in
`repl_editor.c`. When adding or reordering handlers, ALL sites must
be updated consistently:
1. **`;` key handler** — `key == ';'` block in `keyboard_func()`
2. **Enter key, insert mode, with visible vars** — `g_inserting` +
   `dnv > 0` path
3. **Enter key, insert mode, no visible vars** — `g_inserting` +
   `dnv == 0` path
4. **Enter key, overwrite mode** — `!g_inserting` path with the
   for/func/if special-case checks first
5. **`feed_line()`** — the programmatic entry point

### Editing Existing Lines

When the user navigates to an existing line, `load_line_to_input()`
strips the trailing `;` and whitespace from `cmd.source` before
loading into `g_input`. This means re-committing the line goes
through the no-semicolon path. Commit handlers that check for `;`
must also accept end-of-string as a valid terminator.

### Float Variable Declarations (`CMD_VAR_DECLARE`)

`try_commit_float_decl()` in `repl_editor.c` handles `float name;`
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
- The function has overwrite logic for editing an existing
  `CMD_VAR_DECLARE` line, but the "already declared" validation
  currently fires BEFORE the overwrite check — this means editing
  `float tmp;` back to `float tmp;` (or changing its value) fails
  with "'tmp' already declared". The fix is to detect same-line
  overwrites before the duplicate check.
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
  `@cfg setting=value`), camera state as the raw `glTranslatef`/`glRotatef`
  sequence the REPL uses internally, predefined vars as globals, REPL
  functions as C functions, and `display()` body containing the user's
  geometry commands
- **Import** (`load_from_file()`): line-by-line scan parses camera
  state, detects function definitions (converts C syntax back to REPL),
  and feeds geometry lines through `feed_line()`. The text format is
  human-editable and round-trips cleanly

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

Circular snapshot buffers in `repl_editor.c`:
- `UndoSnapshot` captures `cmds[]`, `num_cmds`, `edit_line`,
  `predef_vals[]` — full editor state
- `g_undo_buf[32]` and `g_redo_buf[32]` with head/count tracking
- `push_undo_snapshot()` called before any mutation (delete, paste,
  reformat, etc.); `pop_undo_snapshot()` on Ctrl+Z; `do_redo()` on
  Ctrl+Y
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

Declarative toggle system in `repl_editor.c`:
- `g_cfg_items[]` array of `CfgItem` structs: `{ label, key_hint,
  int *value, n_states, state_names[] }`
- Each item is a toggle (2 states, default OFF/ON) or cycle (>2 states
  with named entries, e.g. grid themes)
- Rendered by `render_config_menu()` in `ui_panels.c`; toggled via
  `g_show_config` (Config header button)
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
| F12 | Cycle examples (includes saved user scene) |

## Supported Commands

```
glBegin(MODE), glEnd()
glVertex3f(x,y,z), glVertex2f(x,y)
glNormal3f(x,y,z)
glColor3f(r,g,b), glColor4f(r,g,b,a)
glTranslatef(x,y,z), glScalef(sx,sy,sz), glRotatef(deg,x,y,z)
glPushMatrix(), glPopMatrix()
glEnable(CAP), glDisable(CAP)
glShadeModel(MODE)
glPointSize(size)
glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, const, linear, quadratic)
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA|GL_ONE)
glColorMaterial(face, mode), glMaterialf(face, pname, value)
glLightModeli(pname, param), glFrontFace(mode)
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
