---
name: gl-repl-scene-authoring
description: Write or edit gl-repl scenes and built-in examples — the full REPL command/expression language reference, per-command parser policies, @cfg + camera headers, the presentation-reset rule, size budgets, and the four files examples must be edited together with. Use when asked to write a scene, add or modify a built-in example, author a .c scene file, or when touching examples.c / example_loader.c.
---

# Scene & example authoring

## The language

```
glBegin(MODE), glEnd()
glVertex3f(x,y,z), glVertex2f(x,y), glNormal3f(x,y,z)
glColor3f(r,g,b), glColor4f(r,g,b,a)
glClearColor(r,g,b,a)          (channels clamped >= 0.15)
glClear(mask)                  (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)
glClearDepth(depth)            (depth value glClear writes; GL clamps to 0..1)
glTranslatef/glScalef/glRotatef, glPushMatrix/glPopMatrix/glLoadIdentity
glPushAttrib(mask), glPopAttrib()  (attribute-stack save/restore; GL_*_BIT tokens)
glEnable(CAP), glDisable(CAP)  (depth/lighting/blend/cull/fog/lights 0-3,
                                GL_CLIP_PLANE0..5, line/point smooth, ...)
glFogi/glFogf/glFogfv          (fog mode/scalar/color)
glClipPlane(plane, (GLdouble[]){a,b,c,d})
glShadeModel(MODE), glPointSize(s), glLineWidth(w)
glLineStipple(factor, pattern) (pattern is plain int — no hex literals)
glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, c, l, q)
glBlendFunc(sfactor, dfactor)
glColorMaterial(face, mode)
glMaterialfv(face, pname, (GLfloat[]){...})
glLightModeli(pname, param), glFrontFace(mode), glCullFace(mode)
glDepthFunc(func), glDepthMask(GL_TRUE|GL_FALSE), glColorMask(r,g,b,a)
glutSolidTorus/Cube/Sphere/Teapot/Cone(...)
glRasterPos3f(x,y,z)
label("fmt", a, b, c, d)       (bitmap text at raster pos; REPL primitive)
for(var, start, end[, step]) { body }
func0..func9(params) { body }  (parens required; NAME(params) aliases a slot)
if(expr) { body }
:name / name:  and  goto name  (goto labels — colon syntax, not label())
// comment
float name[, name2, ...];      (declaration)
var = expr;   A[i] = expr;     (scratch arrays A/B/C, index 0..15)
```

Semantics in depth: `docs/USER_GUIDE.md`.

## Math

Functions: `sin cos tan asin acos atan atan2(y,x) sqrt abs pow log ln min max
floor ceil fmod rem rand(seed[,iter]) rand2(seed[,iter])` — `log` is base-10, `ln`
natural; `asin`/`acos` clamp to [-1,1] before the call, `atan2` returns
[-PI, PI] (the aim-at / polar-angle primitive); `rand` ∈ [0,1], `rand2` ∈
[-1,1], both deterministic per (seed, iter). Constants: `PI`, `TAU`, `e`.

Only `t` is predefined; everything else needs `float name;`. `t` starts at 0 and
advances 1/60 s per simulation tick while playing.

Decl-line tags: `// @tune` (tunable knob badge + exported-C controls),
`// @config` (keeps a source-assigned var bright in the panel); both round-trip
via `@declare`.

Scratch arrays `A`/`B`/`C[16]`: fixed globals, indices truncated with `(int)`,
must stay 0..15.

`MAX_PREDEF_VARS` = 32 (31 user slots); a full table rejects with
"variable table full (max 32)".

**Reserved names** — `A`, `B`, `C`, `t`, `PI`, `TAU`, `float`, `var` all reject
a `float ...;` declaration. Use `X`/`Y`/`Z` in tests and scratch scenes.

## Per-command policies that bite

- **`glClear` is load-bearing.** Nothing clears the scene rect on the program's
  behalf (identical to the exported C); deleting the line smears the frame. Its
  mask slot is an ENUM_BITFIELD: `|`-joined tokens only, emitted in table order,
  deduped. Replay fade batches skip `CMD_CLEAR`, and replay clamps never cut
  below the leading clear.
- **Flat-shorthand canonicalization.** `glMaterialfv`, `glFogfv`, and
  `glClipPlane` accept flat args (`face, pname, r, g, b, a`) and are rewritten
  to the compound-literal form. `glDepthMask`/`glColorMask` accept 0/1,
  canonicalized to `GL_TRUE`/`GL_FALSE`.
- **`glPushAttrib`/`glPopAttrib`.** Mask is `|`-joined `GL_*_BIT` tokens (same
  bitfield policy as glClear); 10 supported bits — GL_CURRENT / POINT / LINE /
  POLYGON / LIGHTING / FOG / DEPTH_BUFFER / TRANSFORM / ENABLE /
  COLOR_BUFFER_BIT. GL_FOG_BIT scopes the glFog* parameters; the GL_FOG enable
  flag rides both GL_FOG_BIT and GL_ENABLE_BIT, matching real GL.
  `GL_ALL_ATTRIB_BITS` aliases the union of those 10 modeled groups (rather than
  the platform's broader GL value); canonical text retains the alias, and its
  REPL meaning grows when another supported group is added. The alias token
  itself has no per-bit colour; its covered setter lines still receive per-bit
  markers. The executor keeps a real GL stack only `REPL_ATTRIB_STACK_CAP` (8)
  deep — virtual push depth is unbounded, an orphan `glPopAttrib` is a silent
  no-op, and unmatched pushes unwind at frame end, so only balanced pairs reach
  GL. Unbalanced pairs get the red gutter warning. Opens no indentation scope
  (unlike `glPushMatrix`).
- **`glPointParameterfv`** is runtime-gated (`GLR_NO_POINT_PARAMETER`, or a
  context without the entry point → silent no-op with a `glPointSize`
  fallback); user-typed lines are still exported verbatim.
- **`label()`** format string: `%f` and `%%` only, ≤ 4 args, ≤ 64 chars; no
  `//`, parens, commas, or backslashes inside the string. The exporter emits a
  self-contained `label()` helper. Distinct from the goto-label `:name`
  (`CMD_GOTO_LABEL`).

## Example headers: `@cfg` and camera

Examples may lead with `// @cfg <slug> = <value>` lines plus an optional 5-line
`// camera` block. Both are consumed before commit and hidden from the panel.
Slug list: `docs/ADVANCED_USAGE.md` (scene-presentation only; `projection` =
ortho projection at the free camera, `view_mode` = locked 2D/3D toggle).

**Every example load resets the non-camera presentation settings (including
`view_mode`) to `CFG_DEFAULT_*` before applying the example's `@cfg`.**
`CFG_DEFAULT_*` lives in `src/app/glr_defaults.h` and is the single source of
truth — reuse the macros, never duplicate literals.

Camera is deliberately excluded from that reset: it is inherited unless a
`// camera` header is present. `restore_user_scene()` restores commands and
predef vars only.

## Files that move together

Editing an example means touching all of these in one change:

- `src/repl/example_loader.c`
- `src/repl/export.c`
- `src/repl/examples.c`
- `src/app/glr_defaults.h`
- `tests/test_repl_core_examples.c`

`make test_repl_core_examples` is the focused suite.

**Index-keyed goldens shift when you insert mid-catalog.** Appending is cheap;
inserting is not.

## Budgets

| Cap | Value | Meaning |
|---|---|---|
| `MAX_FLAT_COMMANDS` | 8192 | after loop unrolling / func inlining |
| `MAX_EDITOR_COMMANDS` | 1024 | source lines |
| `MAX_FLATTEN_VISIT_BUDGET` | 200000 | flatten visit budget |
| `MAX_FLATTEN_CALL_DEPTH` | 64 | func recursion depth |

Hoist loop-invariant assignments out of `for` bodies — they multiply against the
flat cap, not the source cap.

## Authoring gotchas

- **Const-fold vs flatten differential** — an expression that constant-folds at
  parse time and one that flattens per-frame can disagree; verify the animated
  path, not just the `t = 0` frame.
- **Palette**: use accent anchors or expressions, not raw literals invented per
  scene (`accent_palette.h`, `make check-palette`).
- **Eased example cameras** need ~240-frame captures to settle.

## Visual check

Headless, no window required:

```bash
make gl-repl FREEGLUT_OSMESA=1
```

Then use the capture hooks — see the `gl-repl-capture` skill. Vertex outlines
default ON and pollute reference shots; theme fades need ~12 s to settle.
