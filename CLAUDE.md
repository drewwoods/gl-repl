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
1. Increment `GRID_THEME_COUNT` (or `AXES_THEME_COUNT`) in `sample.h`
2. Add the name string to `g_grid_names[]` (or `g_axes_names[]`) in `repl_core.c`
3. Add a new `case N:` in `draw_grid()` (or `draw_axes()`) in `scene_render.c`
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
2. **Commit** — pressing `;` calls `feed_line()` in `repl_editor.c`,
   which dispatches to `try_commit_*()` handlers for special forms
   (for-loops, functions, if-blocks, assignments, labels), or
   `repl_parse_and_normalize()` for GL commands
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

### Save/Load (output.c)

`repl_export.c` handles bidirectional text format:
- **Export** (`save_output()`): writes a standalone C file with header
  comments embedding workspace state (`@var name=value`,
  `@cfg setting=value`), camera position as `gluLookAt()`, predefined
  vars as globals, REPL functions as C functions, and `display()` body
  containing the user's geometry commands
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
var = expr;
```

## Math

Functions: `sin`, `cos`, `tan`, `sqrt`, `abs`, `pow`, `min`, `max`, `floor`, `ceil`, `fmod`, `rand(seed[, iter])`
Constants: `PI`, `TAU`
Variables: `x`, `y`, `z`, `i`, `j`, `k`, `n`, `t`
