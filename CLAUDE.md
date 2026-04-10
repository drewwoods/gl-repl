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
| `repl_core.c` | Parser, editor, command execution, flattening, keyboard handling, undo, save/load |
| `repl_core.h` | Public API for repl_core (parse, flatten, display, input callbacks) |
| `repl_core_internal.h` | Test-visible internals (normalize/commit pipeline) |
| `scene_render.c` | 3D scene: camera, grid, axes, lights, vertex overlays, outline pass |
| `scene_render.h` | Declares `render_3d_scene()` |
| `ui_panels.c` | Code panel, autocomplete popup, help overlay, variable slider panel, config menu |
| `ui_panels.h` | UI panel render + hit-test declarations |
| `repl_eval.c` | Expression evaluator (recursive descent), REPL<->C translators, for-loop parsers |
| `repl_eval.h` | Evaluator types (`ExprVar`, `ExprCtx`), function declarations |
| `cmd_format.c` | Pure indentation/depth computation (no GL dependency) |
| `cmd_format.h` | Formatting types (`FmtCmd`, `FmtType`), indent functions |

## Conventions

- Global variables prefixed `g_` (e.g., `g_num_cmds`, `g_cam_rx`)
- Static helpers are file-scoped; public API goes through `repl_core.h`
- Config toggles use the `CfgItem` pattern: add entry to `g_cfg_items[]` array
  in `repl_core.c` (line ~663), it auto-computes count via `sizeof`
- New GL commands: add to `CmdType` enum in `sample.h`, then handle in
  `parse_command()`, `execute_commands()`, and `flatten_range()` in `repl_core.c`
- Keyboard bindings: `keyboard_func()` for ASCII keys (Ctrl+X = key code X-64),
  `special_func()` for F-keys/arrows in `repl_core.c`
- Expression variables: `ExprVar` struct in `repl_eval.h`, predefined set in
  `g_predef_vars[]`

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
| F12 | Cycle examples |

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
glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, a, b, c)
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
