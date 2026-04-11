# REPL Architecture

## Data Flow

```
User Input
    │
    ▼
keyboard_func()          ← repl_core.c:4580+
    │
    ▼
parse_command(line)      ← repl_core.c:6398  (text → GLCmd)
    │
    ▼
g_cmds[MAX_COMMANDS]     ← sample.h:158      (source command array)
    │
    ▼
flatten_commands()       ← repl_core.c        (expand loops/funcs/ifs)
    │
    ▼
g_flat_cmds[MAX_COMMANDS] ← sample.h:161     (flat executable array)
    │
    ▼
execute_commands()       ← repl_core.c:3547   (issue GL calls)
    │
    ▼
OpenGL Pipeline → Screen
```

## Core Data Structures

### GLCmd (sample.h:98-111)

The fundamental command representation. Every parsed line becomes one `GLCmd`.

```c
typedef struct {
    CmdType  type;           // CMD_BEGIN, CMD_VERTEX3F, CMD_FOR_BEGIN, etc.
    GLenum   mode;           // GL_TRIANGLES, GL_LIGHTING, etc.
    float    args[8];        // Numeric arguments (evaluated expressions)
    int      num_args;       // How many args are valid
    char     source[256];    // Original/reformatted source text
    int      valid;          // 0 = deleted/skipped
    int      is_auto;        // Auto-generated (e.g., auto-normals)
    int      has_vars;       // Contains unevaluated variable references
    int      src_cmd_idx;    // Index into g_cmds[] (for flat→source mapping)
    int      call_src_cmd_idx;    // Source of the func call that produced this
    int      root_call_src_cmd_idx; // Outermost call in nested expansion
    unsigned int func_scope_mask;  // Bitmask of active function scopes
} GLCmd;
```

### CmdType Enum (sample.h:60-91)

All recognized command types. To add a new command:
1. Add enum value here
2. Handle in `parse_command()` (repl_core.c)
3. Handle in `execute_commands()` (repl_core.c:3547)
4. Handle in `flatten_range()` if it affects control flow

### ExprVar (repl_eval.h:36-39)

Variable binding for the expression evaluator.

```c
typedef struct {
    char  name[16];
    float value;
} ExprVar;
```

Predefined variables (`g_predef_vars[]`): x, y, z, i, j, k, n, t

### CfgItem (sample.h:122-128)

Config menu toggle entry. Adding one to `g_cfg_items[]` (repl_core.c:663)
automatically adds it to the config menu UI.

```c
typedef struct {
    const char  *label;       // Display name
    const char  *key_hint;    // Keyboard shortcut hint
    int         *value;       // Pointer to the global toggle
    int          n_states;    // 2=on/off, >2=cycle
    const char **state_names; // NULL → "OFF"/"ON"
} CfgItem;
```

## Rendering Pipeline

### display_func() (repl_core.c:3832)

Main GLUT display callback. Called every frame (~60fps via timer).

```
display_func()
├── recompute_autonormals()    (if dirty)
├── flatten_commands()         (if dirty)
├── update_render_state_strings()
├── update_lookat_strings()
│
├── [Optional: accumulation buffer AA loop]
│   └── render_3d_scene()      (multiple jittered passes)
│
├── render_3d_scene()          (single pass if no AA)
│   ├── setup projection + modelview
│   ├── setup_lights()
│   ├── draw_grid()
│   ├── draw_axes()
│   ├── execute_commands()     ← THE CORE RENDER CALL
│   ├── outline overlay pass
│   ├── draw_vertex_numbers()  (if enabled)
│   ├── draw_normal_vectors()  (if enabled)
│   └── draw_vertex_guides()   (if enabled)
│
├── render_code_panel()        (2D overlay)
├── render_autocomplete()
├── render_var_panel()
├── render_config_menu()
├── render_help()
└── glutSwapBuffers()
```

### execute_commands() (repl_core.c:3547)

Iterates `g_flat_cmds[]` and issues corresponding GL calls. Tracks:
- `in_begin` flag (inside glBegin/glEnd block)
- `tess_depth` (tessellator nesting)
- `pc` program counter through flat commands
- Goto loop safety counter

Transform commands (translate/scale/rotate/push/pop) are handled by
`apply_transform_cmd()` (sample.h:141).

## Flattening System

`flatten_commands()` expands high-level constructs into a flat array of
executable GL commands:

- **For-loops**: `CMD_FOR_BEGIN..CMD_FOR_END` → N copies of body with
  loop variable bound to successive values
- **Functions**: `CMD_CALL` → inline expansion of `CMD_FUNC_DEF..CMD_FUNC_END`
  body with parameters bound as local variables
- **Conditionals**: `CMD_IF_BEGIN..CMD_IF_END` → body included only if
  expression evaluates to non-zero
- **Variable assignments**: `CMD_VAR_ASSIGN` → updates `ExprVar` table,
  subsequent expressions re-evaluated

Each flat command preserves `src_cmd_idx` pointing back to its origin in
`g_cmds[]`, enabling source-line highlighting and debugging.

### Goto / Label Limitations

`CMD_LABEL` / `CMD_GOTO` exist, but they are only partially supported:

- Top-level only. `flatten_range()` rejects labels/gotos inside functions.
- Normal execution can jump between flat commands and re-apply
  `CMD_VAR_ASSIGN` and `CMD_IF_BEGIN` using current predefined-variable
  values.
- Variable-driven GL commands inside a goto loop are **not** re-evaluated per
  jump. Their numeric args are still the values baked into `g_flat_cmds[]`
  during flattening, so goto loops are only reliable for control flow and
  variable state, not for dynamic geometry generation.
- Replay does not support goto/label traces. Replay stepping operates on the
  static flat command list, so it cannot follow dynamic jumps.

Because of those constraints, goto/label coverage lives in tests and internal
docs rather than the shipped F12 example list.

### flatten_range() (repl_core.c)

Recursive worker that processes a range of `g_cmds[]`. Handles nested
blocks by recursing into sub-ranges. Expansion budget prevents infinite
recursion (function call depth cap).

## Editor System

### Command Buffer

- `g_cmds[MAX_COMMANDS]` — ordered array of parsed commands
- `g_num_cmds` — current count
- `g_edit_line` — cursor position (which command is being edited)
- `g_input[]` / `g_cursor_pos` — current input line text and cursor

### Editing Flow

1. User types into `g_input[]`
2. Press `;` → `commit_line()` parses and inserts/replaces at `g_edit_line`
3. `g_flat_dirty = 1` triggers re-flatten on next display
4. Enter inserts a blank line; Up/Down navigates; Ctrl+D deletes

### Undo System

Snapshot-based: `push_undo_snapshot()` saves entire `g_cmds[]` + editor state.
Ctrl+Z restores the previous snapshot.

## Depth Cache (repl_core.c)

O(1) nesting-depth lookup via prefix-sum arrays:

- `g_for_depth_prefix[]` — for-loop nesting at each position
- `g_block_depth_prefix[]` — total block nesting (for + func + if)
- `g_begin_depth_prefix[]` — glBegin/glEnd nesting
- `g_tess_depth_prefix[]` — tessellator nesting

Invalidated by `depth_cache_invalidate()` on any command edit.
Rebuilt lazily by `depth_cache_rebuild()`.

## Expression Evaluator (repl_eval.c)

Recursive descent parser supporting:

- Binary operators: `+`, `-`, `*`, `/`, `%`
- Comparison: `>`, `<`, `>=`, `<=`, `==`, `!=`
- Logical: `&&`, `||`, `!`
- Unary: `-`, `!`
- Functions: `sin()`, `cos()`, `tan()`, `sqrt()`, `abs()`, `pow()`, `min()`, `max()`
- Constants: `PI`, `TAU`
- Variables: lookup in `ExprVar` table (local scope first, then `g_predef_vars`)

### Scoping

- `g_predef_vars[]` — global predefined variables (x, y, z, i, j, k, n, t)
- Function parameters injected as local `ExprVar` entries during flatten
- For-loop variables bound during `flatten_range()` iteration

## UI Panels (ui_panels.c)

All 2D overlays rendered in `display_func()` after the 3D scene:

| Panel | Function | Toggle |
|-------|----------|--------|
| Code panel | `render_code_panel()` | Always visible (resizable) |
| Autocomplete | `render_autocomplete()` | Auto-shown during typing |
| Variable panel | `render_var_panel()` | Backtick key |
| Config menu | `render_config_menu()` | Right-click |
| Help | `render_help()` | F1 |

### Code Panel Features

- Syntax-colored command display
- Current edit line highlighted
- Selection highlighting (Shift+Up/Down)
- Scrollable with PgUp/PgDn or mouse wheel
- Resizable left edge via drag
- Click-to-navigate to source lines

### Fixed Code Scaffold Generation

The dimmed, non-editable C shown in the REPL code panel is not synthesized
from `g_cmds[]` or `g_flat_cmds[]`. It is assembled from a small set of global
string buffers in `repl_core.c`, then rendered ahead of the editable REPL
commands.

#### Variables involved

- `g_header_pre[]` — static preamble lines: includes, `M_PI` guard, generated
  globals such as `g_angle`, `g_rotating`, `g_quadric`, and the opening
  `display()` boilerplate through `glPushAttrib(...)`.
- `g_render_state_lines[RENDER_STATE_LINE_COUNT][64]` — dynamic fixed lines for
  current render toggles. Today these are the `GL_MULTISAMPLE` and
  `GL_LINE_SMOOTH` enable/disable lines.
- `g_lookat[LOOKAT_LINE_COUNT][128]` — dynamic fixed `gluLookAt(...)` lines.
- `g_header_post[]` — static fixed lines after `gluLookAt`, currently the
  `glRotatef(g_angle, ...)` line.
- `g_cmds[i].source` — the user-editable lines appended after the fixed
  scaffold.

#### Functions involved

1. `display_func()` calls `update_render_state_strings()` and
   `update_lookat_strings()` before drawing the UI.
2. `update_render_state_strings()` formats `g_render_state_lines[]` from
   `g_multisample_enabled` and `g_line_smooth_enabled`.
3. `update_lookat_strings()` formats `g_lookat[]` from the live camera state:
   `g_cam_rx`, `g_cam_ry`, `g_cam_dist`, `g_cam_px`, and `g_cam_py`.
4. `render_code_panel()` iterates `g_header_pre[]`,
   `g_render_state_lines[]`, `g_lookat[]`, `g_header_post[]`, and then the
   editable command sources in `g_cmds[]`.
5. `code_panel_header_row_count()` uses the same fixed arrays to compute scroll
   height and layout for the panel.
6. `repl_dump_code_panel_text()` and `repl_dump_code_panel_visual_text()` dump
   the same scaffold for tests/debugging, so their output matches what the code
   panel is built from.

## Lighting System

- `g_lights[MAX_LIGHTS]` (MAX_LIGHTS=4) — `SceneLight` structs
- `setup_lights()` in scene_render.c configures GL light state
- User enables via `glEnable(GL_LIGHTING)` + `glEnable(GL_LIGHT0..3)`
- Light indicators (position markers) rendered when F10 toggle is on
- `g_user_lighting_enabled` tracks whether user has lighting active

## Save/Load (repl_core.c)

### Export (Ctrl+S → output.c)

Generates standalone C code:
- Header includes + GLUT boilerplate
- User commands translated to C (REPL expressions → C math)
- For-loops preserved as C `for()` statements
- Snippet markers for re-import

The exporter reuses the same fixed-code sources that the REPL panel shows:

- `save_output()` first calls `update_render_state_strings()` and
  `update_lookat_strings()` so the generated file matches the current camera and
  render toggles.
- It writes the pre-`display()` part of `g_header_pre[]`, then inserts helper
  code via `write_predef_var_globals()`, `write_rand_helper()`,
  `write_tess_preamble()`, `write_predef_var_reset_func()`,
  `write_func_defs_as_c()`, and `write_render_helper_as_c()`.
- It then emits a fresh `display()` function body using
  `g_render_state_lines[]`, `g_lookat[]`, `g_header_post[]`, and
  `write_light_setup()`, followed by calls into the generated geometry helpers.

### Import (./sample file.c)

Parses C code back into REPL commands:
- Extracts code between snippet markers
- C `for()` → REPL `for(var, start, end, step)`
- C math → REPL expression syntax

## Extension Points

### Adding a New GL Command

1. `sample.h`: Add `CMD_NEW_THING` to `CmdType` enum
2. `repl_core.c` `parse_command()`: Add string matching and argument parsing
3. `repl_core.c` `execute_commands()`: Add case to issue the GL call
4. `repl_core.c` `flatten_range()`: Handle if it's a block command
5. `repl_core.c` `g_func_completions[]`: Add for autocomplete
6. `cmd_format.c/h`: Add `FmtType` if it affects indentation

### Adding a UI Overlay

1. `ui_panels.h`: Declare `render_my_panel()`
2. `ui_panels.c`: Implement it
3. `sample.h`: Add `extern int g_show_my_panel;`
4. `repl_core.c`: Add global definition, keyboard binding, and `CfgItem` entry
5. `repl_core.c` `display_func()`: Call `render_my_panel()`

### Adding a Keyboard Binding

- ASCII keys: `keyboard_func()` in repl_core.c (~line 4580)
  - Ctrl+X = key code `X - 64` (e.g., Ctrl+G = 7)
- Special keys: `special_func()` in repl_core.c (~line 5450)
  - Uses `GLUT_KEY_F1..F12`, `GLUT_KEY_UP/DOWN/LEFT/RIGHT`, etc.
