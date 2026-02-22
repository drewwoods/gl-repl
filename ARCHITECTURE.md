# Architecture Notes — OpenGL Immediate-Mode REPL

## Language Choice: C vs Go vs Lua

### C is the right call for the host

The project touches C APIs directly (OpenGL, GLUT) and has a hot render loop.

- **GC is your enemy in a render loop.** Go's GC can pause for 1-5ms. The frame
  budget at 60fps is 16ms. Visible hitches occur, especially during accumulation
  AA passes (multiple renders per frame).
- **No FFI layer.** OpenGL and GLUT are C APIs. In Go you pay CGO overhead on
  every `gl*` call, or use a pure-Go binding layer that adds its own complexity.
  In C you call them directly.
- **Static array architecture fits C naturally.** The `g_cmds[4096]` /
  `g_flat_cmds[4096]` model with no heap allocation is idiomatic C. Zero
  allocation in the hot path.

### Lua is the genuinely interesting alternative — but differently

Not "rewrite in Lua" — rather "embed Lua as the expression/scripting layer":

```
C host:        OpenGL, GLUT, windowing, camera, UI rendering
Lua inside:    expression evaluation, the REPL language itself
```

Instead of the custom `repl_eval.c` parser, expose `gl*` functions as Lua
globals and let users write actual Lua:

```lua
glVertex3f(math.sin(t), math.cos(t), 0)
for i = 0, 10 do
  glVertex3f(i * 0.1, math.sin(i + t), 0)
end
```

This is what LOVE2D / Defold do. Benefits:

- **Functions and goto** become free — Lua has `function`, `goto`, `::labels::`
- **Conditionals** — `if x > 0.5 then ... end`
- **Tables** as data structures for complex geometry
- Full math library already there
- `t % 1` works natively (Lua `%` is float modulo)
- The expression evaluator (`repl_eval.c`) goes away entirely

The cost: the command list concept would need rethinking. Currently `g_cmds[]`
is a structured list you can navigate, edit line-by-line, save, and reload. With
Lua you'd store Lua source text and re-execute it each frame — which is actually
fine for a REPL, and what things like Shadertoy do.

### Comparison

| | C (current) | Go | Lua embedded |
|---|---|---|---|
| Render loop perf | zero alloc | GC pauses | C host |
| GL binding cost | native | CGO overhead | native C |
| Expression power | custom DSL | n/a | full language |
| Functions/goto | manual work | n/a | free |
| `%` operator | added to eval | n/a | native |
| Code list editing | structured | - | harder |
| Binary size/deps | tiny | large runtime | +lua (~200KB) |

Go is the worst option for this use case. C stays right for the host. Lua as
the scripting backend is a real architectural direction — essentially building a
tiny LOVE2D.

---

## Current Architecture

### Data Model

```
GLCmd struct:
  type      CmdType enum (CMD_BEGIN, CMD_VERTEX3F, CMD_FOR_BEGIN, ...)
  mode      GLenum (for glBegin mode, glEnable cap, etc.)
  args[8]   float arguments (expanded from 4 to support future GLU commands)
  num_args  argument count
  source    char[256] — original REPL text for display and serialization
  valid     whether this command is active
  is_auto   auto-generated (e.g. auto-normals)
  has_vars  source references predefined variables, needs re-eval on flatten
```

### Two-Array Model

```
g_cmds[4096]       Raw command list (what the user types)
                    Contains for-loop blocks (CMD_FOR_BEGIN..CMD_FOR_END)
                    Contains var assignments, comments, etc.
                         |
                    flatten_range() — recursive expansion
                         |
                         v
g_flat_cmds[4096]  Flattened commands (loops expanded, vars evaluated)
                    Only executable GL commands remain
                         |
                    execute_commands() — while loop with program counter
                         |
                         v
                    OpenGL calls (glBegin, glVertex3f, glEnd, ...)
```

### Execution Model

`execute_commands()` uses a `while` loop with an explicit program counter (`pc`)
rather than a `for` loop. This enables future additions:

- `CMD_GOTO` / `CMD_LABEL` — set `pc` directly and `continue`
- `CMD_CALL` / `CMD_RETURN` — push/pop return address, jump to function body

### Expression Evaluator (repl_eval.c)

Recursive descent parser supporting:

- Arithmetic: `+`, `-`, `*`, `/`, `%` (modulo via `fmodf`)
- Functions: `sin`, `cos`, `tan`, `sqrt`, `abs`, `pow`, `min`, `max`,
  `floor`, `ceil`, `fmod`
- Constants: `PI`, `TAU`
- Variables: predefined (`x`, `y`, `z`, `i`, `j`, `k`, `n`, `t`) and
  loop-scoped variables from for-loops

### Predefined Variables

```
x, y, z    General-purpose (future: drag GUI)
i, j, k    Loop iterators / general-purpose
n          General-purpose counter
t          Time — auto-increments with elapsed time (Ctrl+T to pause)
```

`t` is driven by `g_anim_time` in `timer_func()`. When playing, `g_flat_dirty`
is set every frame so expressions referencing `t` re-evaluate automatically.

### For-Loop Representation

For-loops are stored inline in `g_cmds[]`:

```
[i]   CMD_FOR_BEGIN   source="  for(i, 0, 10) {"   args={0, 10, 1}
[i+1] CMD_VERTEX3F    source="    glVertex3f(i, 0, 0);"
[i+2] CMD_FOR_END     source="  }"
```

`flatten_range()` recursively expands these, binding the loop variable and
re-parsing `has_vars` commands with the current variable values.

### Auto-Normals

When enabled (F9), `recompute_autonormals()` scans `g_cmds[]` for
`glBegin`/`glEnd` blocks and inserts `CMD_NORMAL3F` commands (marked `is_auto`)
before vertices that lack explicit normals. Face normals are computed per
primitive mode (triangles, quads, etc.).

### Save/Load (Ctrl+S)

Export to `output.c` uses `repl_expr_to_c()` to translate REPL syntax to C
(`sin` -> `sinf`, `PI` -> `M_PI`, etc.). Import via `c_expr_to_repl()` does the
reverse. For-loops are exported as real C `for` statements. Snippet markers
(`// Snippet start` / `// Snippet end`) delimit the editable region.

---

## Planned Features and Their Architectural Impact

### Low Impact (no structural change)

- **More GL commands** (glRotatef, glScalef, glPushMatrix, glPopMatrix):
  new CmdType entries + parser cases + executor cases. Pattern is well
  established.
- **GLU quadric drawing** (gluSphere, gluCylinder, gluDisk): new CmdType
  entries, need a static `GLUquadric*`. gluCylinder needs 5 args (base, top,
  height, slices, stacks) — handled by `args[8]`.

### Moderate Impact

- **2D orthographic mode**: global `g_proj_mode`, `CMD_VERTEX2F`, different
  projection setup in render. Auto-normals skip 2D vertices.
- **Split viewports** (top/front/side): render loop calls `execute_commands()`
  multiple times via `glViewport()` with fixed orthographic cameras. Accum AA
  only on the perspective viewport.
- **Variable drag GUI**: new UI panel. `g_t_playing` distinguishes auto-animate
  vs drag. Modifying a predef var sets `g_flat_dirty = 1`.
- **Vertex entry plane preview**: parse partially-typed `g_input` during render,
  draw a faint guide plane at the fixed coordinate value.
- **GLU tessellator** (concave polygons): vertex accumulator in
  `execute_commands()` — when `CMD_BEGIN(GL_POLYGON)` is hit with tessellation
  enabled, accumulate vertices until `CMD_END`, then tessellate via
  `gluTessBeginContour` / `gluTessEndContour` callbacks.

### Significant Impact

- **Functions** (func0..funcN): `CMD_FUNC_DEF` + body + `CMD_FUNC_END` inline
  in `g_cmds[]`, analogous to for-loops. `CMD_CALL` looks up the function and
  recursively flattens its body. Function name stored in `source[]` field.
  Flatten may need two passes (collect function boundaries, then expand calls).
- **Goto / labels**: `CMD_LABEL` and `CMD_GOTO` survive flatten into
  `g_flat_cmds[]`. The `while (pc < ...)` executor handles `CMD_GOTO` by
  scanning for the matching `CMD_LABEL` and setting `pc`. Already prepared by
  the for-to-while refactor.
