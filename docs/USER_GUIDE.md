# gl-repl User Guide

gl-repl is an interactive OpenGL interpreter. You type classic immediate-mode
GL commands, press `;` to run each one, and watch the geometry build up live
in a 3D viewport. Every command stays in an editable code panel, so a scene is
a readable list of GL calls you can revisit, tweak, animate, replay
step-by-step, and export as a standalone C program.

![gl-repl rendering the Whale example](images/hero.png)

This guide follows the shape of a session: you [start the app](#getting-started)
and [write some code](#writing-code) in [the REPL language](#the-repl-language),
[make it move](#making-it-move), lean on the [visual feedback](#seeing-what-youre-doing)
to understand and debug what you built, and finally [keep and ship](#scenes--workspaces)
the result. Reference material — the full [CLI](#command-line-options) and
[keyboard](#keyboard--mouse-reference) listings — sits at the end. For headless
rendering, recording, and every environment variable, see
[`ADVANCED_USAGE.md`](ADVANCED_USAGE.md); for project internals,
[`ARCHITECTURE.md`](ARCHITECTURE.md).

## Contents

- [Getting Started](#getting-started)
- [Writing Code](#writing-code)
- [The REPL Language](#the-repl-language)
- [Making It Move](#making-it-move)
- [Seeing What You're Doing](#seeing-what-youre-doing)
- [Replay](#replay)
- [Tutorials](#tutorials)
- [Built-in Examples](#built-in-examples)
- [Scenes & Workspaces](#scenes--workspaces)
- [Exporting & Importing](#exporting--importing)
- [Tunable Variables (`// @tune`)](#tunable-variables--tune)
- [Performance & Scope](#performance--scope)
- [Profiling & Diagnostics](#profiling--diagnostics)
- [Music](#music)
- [Command-Line Options](#command-line-options)
- [Keyboard & Mouse Reference](#keyboard--mouse-reference)

---

## Getting Started

Run a fresh session, or reload earlier work:

```bash
./gl-repl                  # fresh session
./gl-repl output.c         # reload a saved scene
./gl-repl workspace/       # load every *.c in a directory as scenes
```

### The window

![The window: scene tabs, code panel, status bar, viewport, variable panel](images/window-tour.png)

Top to bottom:

- **Menu bar** — *File*, *Scene*, *Tutorials*, *Config*, *Audio* dropdowns, a
  *search...* slot (same as Ctrl+F), and the *Replay* button at the far
  right.
- **Scene tabs** — one tab per open scene (here *First Triangle* and *Ring
  Sketch*). Click to switch.
- **Code panel** — the live, editable list of GL commands. By default it sits
  above the viewport; cycle its position (Left / Top / Bottom / Hidden) with
  **Ctrl+B** or the *Code panel* config item.
- **Status bar** — command count, current line, the accumulation indicator
  (`AA 1x` / `Blur 8x`), and clickable controls for undo/redo, copy/cut,
  clearing all commands, *focus* ([code focus](#keeping-the-buffer-tidy)),
  and *F1 help*.
- **3D viewport** — your geometry, rendered every frame. Drag to orbit,
  scroll to zoom.
- **Variable panel** — bottom-right overlay listing every declared variable
  with a draggable slider (see [The Variable Panel](#the-variable-panel)).
- **Message line** — the bottom row shows the most recent status message.
  Click the small button at its right end to pop up the recent-message
  history.

### Your first triangle

Type each line and press `;` to commit it — the `;` keystroke runs the line,
you don't type a literal semicolon at the end:

```c
glColor3f(0.98, 0.76, 0.36)
glBegin(GL_TRIANGLES)
glVertex3f(0, 1, 0)
glVertex3f(-1, -1, 0)
glVertex3f(1, -1, 0)
glEnd()
```

![A first triangle](images/first-triangle.png)

The triangle appears as soon as the vertices commit. Now:

- Drag in the viewport to orbit, scroll to zoom.
- Press **Ctrl+T** and change a vertex to `glVertex3f(sin(t), 1, 0)` to
  animate it.
- Press **F12** to flip through the built-in examples for ideas.
- Press **F1** for the built-in help overlay — its *Commands* tab lists every
  supported command, and its *Keys* tab is the full keyboard reference. Esc
  or a click outside dismisses it.

---

## Writing Code

Everything in gl-repl happens through one input row in the code panel. You
type there, commit lines into the scene, arrow back up to change your mind,
and the viewport answers every keystroke. This section is that loop.

### Committing lines

- **`;`** commits the current input line (the semicolon is the trigger key,
  not part of the text).
- **Enter** normally inserts a blank line below the cursor. However,
  variable declarations and block openers like `for(...) {` commit
  immediately on Enter.
- **Up/Down** move between lines; loading an existing line into the input
  lets you edit and re-commit it in place.
- **Esc** clears the input line (or closes whichever overlay is open).
- A line that fails to parse is *not* committed — the status line explains
  why, and your input stays put for fixing.

### Autocomplete

You rarely have to type a GL name (or constant) to the end. Press **Tab**
while typing:

![Typing glEnable(GL_LI: the popup lists the matching constants, ghost text completes inline](images/autocomplete.png)

- A unique match completes in place; multiple matches pop up a list (Tab or
  Enter accepts, arrows move).
- Ghost text shows the pending completion inline — in the shot above the
  dimmed `GHTO)` after the typed `glEnable(GL_LI` is what Tab will accept.
- After typing `foo(`, a parameter hint appears for GL commands and your own
  functions.
- Inside an enum argument slot (e.g. `glEnable(`), completion offers the GL
  constants valid for that slot — this works mid-line too, when the cursor
  sits at the end of the token being completed.

### Editing what's there

Navigation plus a full selection model, so reshaping a committed scene feels
like a normal editor:

- **Shift+Left/Right** extends a character selection inside the input row;
  **Shift+Home/End** extends to the row start/end.
- **Shift+Up/Down** selects whole lines.
- **Click+drag** over the active row selects characters; dragging in the
  line-number gutter selects whole lines.
- **Double-click** selects the word under the cursor.
- **Shift+click** extends a selection from the cursor to the click point —
  same row gives a character selection, a different row gives a line range.
- **Ctrl+C / Ctrl+X / Ctrl+V** — copy / cut / paste. A character selection in
  the input row wins over a line selection: the substring is copied/cut and
  pasted at the cursor. With no character selection, whole command lines are
  copied/cut/pasted.
- **Ctrl+Z** undo, **Ctrl+Y** (or Ctrl+Shift+Z) redo. Undo covers source
  mutations — deletes, pastes, reformat, commits.
- **Ctrl+D** deletes the current line or selection; **Ctrl+L** clears the
  scene and restores five editable display defaults:
  `glEnable(GL_COLOR_MATERIAL)`,
  `glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE)`,
  `glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE)`,
  a gray (0.4) specular color, and shininess 30.
  Delete or comment those lines explicitly when a scene needs different
  state.

These five defaults are real commands, not hidden setup. They remain visible
in code-focus mode and run with the rest of the scene every frame.

**Ctrl+F** (or the *search...* menu slot) opens case-insensitive substring
search over the whole buffer. Type to refine, **Enter** jumps to the next
match, **Esc** closes. Matches are highlighted in the code panel.

### Adjusting values without retyping

Two in-panel widgets close the loop between "committed line" and "number I
want to nudge" without a retype-and-recommit round trip.

**The inline numeric stepper.** Put the cursor on any number in a committed
line and a small up/down stepper appears at the panel's right edge:

![Cursor on the initializer: the stepper appears at the right edge of the row](images/numeric-stepper.png)

- **Click** the arrows to nudge the value (step scales with the value's
  magnitude).
- **Right-click** steps coarse (×10); **Shift+click** steps fine (×1/5).

The line re-commits automatically, so the scene updates as you click.

**The color picker.** Committed `glColor3f` / `glColor4f` / `glClearColor` /
`glMaterialfv` lines show a small color swatch at the right edge of the code
panel. Click it to open the floating picker:

![The color picker open on a glColor3f line, with the teapot tracking it live](images/color-picker.png)

- An HSV square plus hue strip (and an alpha strip for 4-component colors),
  with the current value as a hex readout.
- Four palette tabs: **Basic** (common named colors), **Full** (hue ×
  tint/shade grid with a greyscale ramp), **Neon** (the curated accent set
  the built-in examples share; the tab is named after the active palette
  in [`accent_palette.h`](../accent_palette.h)), **Harmony** (the current color plus a
  tetradic set derived live from it).
- Every change writes straight back to the source line, so the scene follows
  the picker in real time. Click outside to close.

### Keeping the buffer tidy

| Key | Action |
|---|---|
| Ctrl+\ | Reformat all lines (re-indent blocks) |
| Ctrl+/ | Toggle `//` comment on the current line |
| Ctrl+Shift+S | Split a multi-name `float` declaration into one decl per line (also File → Split Declaration) |
| Ctrl+Shift+F | Toggle code focus — the first-run view shows just your code; turn it off to show generated C/workspace chrome (also the *focus* keycap) |
| Ctrl+B | Cycle code panel layout: Left / Top / Bottom / Hidden |
| PgUp / PgDn | Scroll the active panel or overlay |

More code-panel toggles live in the Config menu: **Wrap at commas**
(long calls wrap at argument boundaries), **Syntax highlight**
(Ctrl+Shift+Y; Off / On / On+Shadow), and **Paren match** / **Paren scope**
(highlight the matching bracket under the caret, and the span of the
enclosing parens).

---

## The REPL Language

The language is immediate-mode OpenGL with just enough structure around it —
variables, loops, functions, conditionals — to build real scenes. Every
numeric argument everywhere is a full math expression.

### Supported GL commands

```
glBegin(MODE), glEnd()
glVertex3f(x,y,z), glVertex2f(x,y)
glNormal3f(x,y,z)
glColor3f(r,g,b), glColor4f(r,g,b,a)
glClearColor(r,g,b,a)        background clear color; channels clamp to >= 0.15
                             (prevents a fully black background from hiding geometry)
glTranslatef(x,y,z), glScalef(sx,sy,sz), glRotatef(deg,x,y,z)
glPushMatrix(), glPopMatrix(), glLoadIdentity()
glEnable(CAP), glDisable(CAP)
  CAP: GL_DEPTH_TEST, GL_LIGHTING, GL_COLOR_MATERIAL, GL_NORMALIZE,
       GL_LINE_SMOOTH, GL_POINT_SMOOTH, GL_BLEND, GL_CULL_FACE,
       GL_LIGHT0..GL_LIGHT3, GL_CLIP_PLANE0..GL_CLIP_PLANE5
glShadeModel(MODE)
glPointSize(size), glLineWidth(width)
glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, const, linear, quadratic)
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA | GL_ONE)
glColorMaterial(face, mode)
glMaterialfv(face, pname, (GLfloat[]){r, g, b, a})
glLightModeli(pname, param), glFrontFace(mode)
glDepthFunc(func), glDepthMask(GL_TRUE|GL_FALSE)
glColorMask(r, g, b, a)      each channel GL_TRUE/GL_FALSE or 0/1
glEdgeFlag(GL_TRUE|GL_FALSE) scalar boundary-edge flag; 0/1 accepted
glClipPlane(plane, (GLdouble[]){a, b, c, d})   user clip plane (see Clip planes)
glRasterPos3f(x, y, z)       position for bitmap text (see label below)
```

`glMaterialfv` also accepts the flat shorthand
`glMaterialfv(face, pname, r, g, b, a)` — the parser rewrites it to the
compound-literal form. `glClipPlane` accepts the same flat shorthand
(`glClipPlane(plane, a, b, c, d)`).

### GLUT solid shapes

```
glutSolidTorus(inner, outer, nsides, rings)
glutSolidCube(size)
glutSolidSphere(radius, slices, stacks)
glutSolidTeapot(size)
glutSolidCone(base, height, slices, stacks)
```

### GLU tessellator (concave / complex polygons)

![GLU tessellated concave arrow with a cutout](images/glu-tess.png)

`gluTess` polygons handle concave outlines, and multiple contours in one
polygon create holes (odd winding rule). See built-in examples *GLU
concave arrow*, *GLU concave arrow cutout*, and *GLU concave arrow
extrusion* for the syntax in action.

### Bitmap text — `label()`

![The Orrery example uses labels that track 3D orbits](images/labels-orrery.png)

```c
glRasterPos3f(x, y, z);      // place the text anchor (transforms apply)
label("Earth phase = %f", t);
```

- `label("fmt", a, b, c, d)` draws bitmap text at the current raster
  position. Up to 4 substitution args; `%f` substitutes a value, `%%` is a
  literal percent. The string literal between the quotes is limited to 64
  characters and may not contain `//`, `(`, `)`, `,`, or backslashes.
- This is a REPL convenience, not a real GL call — exported C files include
  a self-contained `label()` helper so they still compile standalone.

### Clip planes

```c
glClipPlane(GL_CLIP_PLANE0, (GLdouble[]){0.2, 1, 0.3, 0.4});
glEnable(GL_CLIP_PLANE0);
```

`glClipPlane` sets the plane equation `a*x + b*y + c*z + d >= 0` — GL keeps
the half-space the inequality selects and clips everything on the other
side. Six planes are available (`GL_CLIP_PLANE0..5`); each does nothing
until its cap is enabled. The equation is interpreted in the coordinate
frame active at the call, so transforms before the line position the plane
just like they position geometry.

Coefficients are full expressions, so a plane can animate — a `d` driven by
`t` sweeps a live cross-section through the scene:

![An animated clip plane sweeping a torus](images/clip-plane-sweep.gif)

A plane equation is hard to picture from four numbers, so the cursor draws
it for you — see [the clip-plane guide](#the-clip-plane-guide). Exporting
keeps clip planes standalone-compilable: the C file routes the equation
through a small `repl_gldouble4` helper (compound literals are C99; the
export targets C89) and reload converts it back.

The *Clip planes carve solids (glClipPlane)* example walks the three core
moves — one plane (a sphere becomes a dome), two planes meeting at an angle
(a 120° wedge), and an animated `d` (a cutaway sweeping through a torus).

### Math expressions

Every numeric argument is a full expression, evaluated when the line runs:

- **Operators:** `+ - * / %` and parentheses; comparisons
  `> < >= <= == !=`; logical `&& || !`.
- **Functions:** `sin`, `cos`, `tan`, `sqrt`, `abs`, `pow`, `log` (base 10),
  `ln` (base e), `min`, `max`, `floor`, `ceil`, `fmod`, `rem`,
  `rand(seed[, iter])`, `rand2(seed[, iter])`. `fmod` is the C `fmodf`
  (result takes the sign of the dividend); `rem` is the IEEE remainder via
  `remainderf` (rounds the quotient to nearest, so the result can differ in
  sign).
- **Constants:** `PI`, `TAU`, `e`.

`rand` returns a deterministic value in `[0, 1]` for a given (seed, iter)
pair; `rand2` is the same hash mapped to `[-1, 1]` — useful for centered
jitter. Determinism means particle systems look the same every frame and
every run — it is the stateless substitute for storing random values, a
pattern covered in [Working without state](#working-without-state).

### Variables

```c
float x, y, z;          // declare before use
x = 1.5;                // assign (any expression)
glVertex3f(x, y, z);    // use anywhere a number is expected
```

- Declarations are hoisted to the top of the program automatically, so every
  reference follows its declaration.
- Up to 31 user variables (plus the predefined `t`).
- Variables persist across commits and are saved/loaded with the scene.
- Initializers are allowed: `float n = 1;`.

### Scratch arrays

`A[16]`, `B[16]`, and `C[16]` are three fixed global arrays for loop and
recursive algorithms:

```c
A[0] = 0;
A[1] = 1;
A[0] = A[0] + (A[1] - A[0]) * 0.25;
glVertex3f(A[0], 0, 0);
```

Indices truncate to int and must stay in `0..15`. Like variables, scratch
arrays persist and round-trip through save/load.

### For-loops

```c
for(i, 0, 24) glVertex3f(cos(i*TAU/24), sin(i*TAU/24), 0);

for(i, 0, n, 2) {        // optional step argument; multi-line body
    glVertex3f(i, 0, 0);
}
```

The parser accepts up to 64 nested blocks. In practice, useful nesting is
limited first by the flat-command budget and the number of loop-iterator
variables in scope. Loop bounds can be expressions (and can animate with
`t`).

### Functions

```c
func1(radius, sides) {            // func0..func9, up to 10 params
    for(i, 0, sides) {
        glVertex3f(radius*cos(i*TAU/sides), radius*sin(i*TAU/sides), 0);
    }
}
drawCube() {                      // or any name — aliases to a free funcN slot
    glutSolidCube(1);
}

func1(1.5, 6);                    // call (parens always required)
drawCube();
```

Ten function slots are available. Recursion works when paired with an
`if(...)` guard — see the *Recursive triangle tree* example.

### Conditionals

```c
if(t > 1) {
    glColor3f(1, 0, 0);
}
```

### Labels & goto (experimental, top-level only)

```c
:loop                    // declare a jump target (colon syntax)
goto loop                // jump back; pair with if(...) to exit
```

(Note the distinction: `:name` / `name:` is a goto target; `label("...")` is
the bitmap-text call.)

### Comments

Type `// text` directly to add a comment line. `Ctrl+/` toggles a comment on
an existing line. A `// @tune` tag on a `float` declaration marks it as a
tunable knob (see [Tunable Variables](#tunable-variables--tune)).

---

## Making It Move

![The Animated ring example](images/animated-ring.gif)

`t` is the one predefined variable — it exists in every session without a
declaration, starts at `0`, and while playing advances a fixed 1/60 s per
rendered frame. Use it in any expression:

```c
glRotatef(t*45, 0, 1, 0);
glVertex3f(sin(t), cos(t), 0);
```

This is the whole animation model: there is no keyframe data and no
accumulated state. The scene re-evaluates every frame, so anything written
in terms of `t` animates — loop bounds, colors, transforms, vertex
positions — and a frame is a *pure function of `t`*. The same `t` always
produces the same picture, which is what makes the scrubbing below (and
headless capture) exactly reproducible.

- **Ctrl+T** plays/pauses time (the *Auto time* config item is the same
  toggle).
- **Ctrl+Shift+T** resets time `t` to zero.
- `--time <secs>` (or the `GLR_TIME` env var) sets the starting `t` when
  launching — handy for headless captures of a later moment.

### Working without state

A frame being a pure function of `t` cuts both ways: there is nowhere to
*accumulate* anything between frames. You cannot write `pos = pos + vel`
once per frame and expect `pos` to remember where it was — next frame the
scene re-evaluates from source, not from last frame's values. Scenes that
would normally keep state use one of three patterns instead. They read a
little differently from typical game-loop code, but they are not harder
to write — and they are *easier to debug*, because any moment can be
reproduced exactly by setting `t`, without replaying history to get there.

**Deterministic randomness instead of stored random state.** Where a
game loop would roll a particle's attributes once and store them, here
each particle recomputes them every frame from `rand(seed, iter)` /
`rand2` — the same (seed, iter) pair always returns the same value, so a
particle's "random" drift, size, or tint is a stable per-particle
constant keyed on its index. The *Snowfall particles* example
derives every flake's drift, fall speed, and depth from `rand(p, slot)`
with the particle index `p` as the seed; *Swaying grass field (rand + t)*
does the same per blade (position, height, sway phase, tint), with a
comment documenting its seed-slot convention so the RNG streams stay
independent.

**Integrate velocity in closed form.** Physics normally accumulates:
`vel += accel*dt; pos += vel*dt`. Statelessly, position must instead be
the *integral* of the velocity function, evaluated at the particle's age.
For constant acceleration that is the familiar projectile polynomial; for
dampened (decaying) velocity it is the integral of the decay curve. The
*Whale (particle system + lit model)* example does both, with the math
worked out in its comments: `computeVerticalMotion` integrates gravity
(`launchY + gravityY/2*age^2 + launchVelY*age`), and
`computeDriftX`/`computeDriftZ` integrate an exponentially decaying
horizontal velocity (`velX(s) = launchVelX * dragDecay^(dragRate*s)`) to
get drift-with-drag. Looping lifetimes fall out of `fmod`:
`age = fmod(t - spawnDelay, particleLife)` respawns each particle forever
with no bookkeeping.

**Replay the algorithm from the start.** Some scenes really are
history-dependent — a sort's array order depends on every swap before it.
The stateless version recomputes that history each frame: start from the
initial state and re-run the algorithm's steps up to the count implied by
`t`. The *Bubble sort (scratch arrays)* example re-seeds `A[0..15]` with
a deterministic shuffle and re-runs its compare-and-swap loops every
frame, gating each compare on `p*15 + j < steps` where `steps` derives
from `t` — the bars freeze mid-sort at exactly the right compare, and
dragging the time slider scrubs the sort forwards *and backwards*.

The payoff of all three is the same: reproducibility. A glitch spotted at
`t ≈ 7.3` is inspected by pausing and dragging `t` to 7.3 — no waiting,
no lucky re-run, no divergent state. It is also what makes
[replay](#replay), timeline scrubbing, and `--time`-anchored headless
captures possible at all. The cost is recomputation — the replay pattern
in particular redoes work proportional to what `t` implies, every frame —
see [Performance & Scope](#performance--scope) for where that ceiling
sits and how export lifts it.

### The Variable Panel

![Variable panel with sliders](images/variable-panel.png)

The variable panel (bottom-right) lists `t` plus every declared variable with
its current value and a slider:

- **Left-click drag** on a row scrubs the value linearly.
- **Right-click drag** is the *fast* scrub: it scales the value
  multiplicatively (a decade per ~200 px of drag), covering large ranges
  quickly — and staying fine near zero.
- **Shift + click drag** is the *slow* scrub: linear deltas at 1/5 speed,
  for dialing in a precise value.
- Toggle the panel with **`** backquote or the *Variable panel* config item.

Row brightness distinguishes knobs from storage:

- **Bright** — read-only parameter/config variables; best for sliders.
- **Dim** — variables written by committed `name = expr;` lines. You can
  drag them, but a later assignment may overwrite the value.
- The [`// @tune`](#tunable-variables--tune) accent is separate from
  brightness. `t` only dims if your source explicitly assigns `t = ...;`.

Slider edits are undoable and go through the normal commit pipeline; when a
declaration exists, dragging rewrites its initializer.

Because `t` is just a variable, it gets a row in the variable panel like any
other — and dragging that row is a timeline scrubber. Pause with **Ctrl+T**,
then drag the `t` row to move the whole animation back and forth; release
and press **Ctrl+T** again to resume playing from wherever you left it. The
usual drag speeds apply: plain drag scrubs linearly, **right-click drag** is
the fast scrub, **Shift+drag** the slow one.

---

## Seeing What You're Doing

Immediate-mode GL is invisible state: the current color, the current
matrix, the winding of the polygon you just typed. Most of gl-repl's
surface area exists to make that state *visible* — guides that draw what
the cursor line means, overlays that annotate the geometry, and display
options that dress the stage. This section walks them roughly in the order
you meet them: camera first, then the cursor-following guides, then the
scene-wide diagnostics and looks.

### Camera & views

| Input | Action |
|---|---|
| Left-drag (viewport) | Orbit around the target |
| Right-drag | Pan in the XZ plane |
| Shift+Right-drag | Pan vertically (Y) |
| Scroll wheel | Zoom, with momentum |
| Ctrl+O | Focus origin — ease the orbit target back to (0,0,0) |
| Ctrl+Shift+C | Reset the camera to its default pose (eased) |
| Ctrl+Shift+R | Toggle camera auto-rotate |
| Ctrl+Shift+V | Toggle View mode: 3D perspective / 2D ortho |
| Ctrl+Shift+E | Toggle Projection: Perspective / Ortho (free camera) |

**2D mode.** *View mode* (Ctrl+Shift+V, or the CAMERA section of the Config
menu) switches between the 3D perspective camera and a flat 2D orthographic
projection — useful for plots, sketches, and UI-like drawings. Examples that
declare `@cfg view_mode = RENDER3D_VIEW_2D` start in 2D automatically.

![Toggling View mode between 3D perspective and 2D ortho on the wave surface](images/view-mode-2d.gif)

**Projection.** *Projection* (Ctrl+Shift+E) toggles between perspective and
orthographic projection while keeping the free, interactive camera. Unlike
*View mode* (which flattens and locks the camera to a top-down 2D view), it
keeps the current orbit angle, so you can navigate the scene
orthographically. Examples can declare `@cfg projection = PROJ_ORTHO`.

### Vertex entry guides

While a `glVertex3f(` line is still being typed, the scene shows where the
vertex *could* land given what's entered so far — one guide per remaining
degree of freedom, colored by the axis you typed (X red, Y green, Z blue):

- **One coordinate typed** → a translucent graph-paper sheet spanning the
  two open axes, with integer grid lines (the zero lines brighter), a
  dashed ghost rim visible through geometry, and an `x = 1.2` readout
  naming the pinned coordinate:

![Typing glVertex3f(1.2 — the two open coordinates span a graph-paper sheet at x = 1.2](images/vertex-guide-plane.png)

- **Two coordinates typed** → the locus collapses to a line along the one
  open axis, with integer tick dots (the 0 tick larger), end fades, a
  dashed ghost pass through occluders, and the still-free axis named at
  the positive end:

![Typing glVertex3f(1.2, 0.8 — one open coordinate leaves a tick-marked line along z](images/vertex-guide-line.png)

- **All three typed** → a pulsing point marker at the exact position (also
  shown when the cursor sits on a committed vertex line).

`glVertex2f` pins `z = 0` implicitly, so its first coordinate already
narrows the guide to a line. Guides follow the cursor's transform context —
inside a `glPushMatrix`/`glTranslatef` frame the sheet and line render in
that frame, matching where the vertex will actually land.

### Cursor guides & vertex overlays

Once lines are committed, the cursor keeps pointing into the scene. A
crosshair guide marks the vertex your cursor line refers to, with its
position label — move the cursor through a `glBegin` block and the guide
follows:

![Cursor guide and Tron grid in the Transform stress example](images/transform-stress.png)

The overlay toggles annotate geometry scene-wide:

![Normal vectors, vertex points and outlines on a quad](images/vertex-overlays.png)

- **Vertex labels** (F7): Off / Index / Index+Pos / Index+World /
  Index+World Fine — numbers each vertex of the primitive at the cursor,
  optionally with its coordinates.
- **Label & highlight scope** (F8): First instance / All instances / At vertex /
  Visible only / Single polygon — controls how broadly cursor-bound overlays
  are shown around repeated function/loop instances.
- **Normal vectors** (Ctrl+Shift+N): draws each vertex's normal as an arrow.
- **Vertex outlines** (Ctrl+Shift+O) and **Vertex points** (Ctrl+Shift+P):
  outline polygons and mark vertices *(both on by default)*.
- **Polygon highlight** (Ctrl+P): highlights the polygon under the cursor line.
- **Auto-normals**: maintains generated `glNormal3f` lines for your
  geometry so lighting works without hand-written normals.

Decluttered label scopes give active edit-guide text priority: vertex labels
move to a nearby row, or are omitted when the bounded layout has no clear row,
rather than covering partial-vertex, normal, clip-plane, translate, or rotate
labels. **At vertex** remains the explicit exact-position mode and bypasses
this decluttering.

**Single polygon** scope narrows labels and the polygon highlight down to
just the one primitive your cursor is building, instead of the whole
`glBegin`/`glEnd` block — handy for a multi-face batch like a cube drawn as
one `glBegin(GL_QUADS)` with several faces packed into it. The cursor's
position between vertex lines picks the primitive: any line up through a
primitive's last vertex belongs to it, and the next line starts the next
one.

![Single polygon scope: cursor on the second quad's second vertex highlights and labels only that quad](images/single-polygon-scope.png)

Here the cursor sits on the second quad's `glVertex3f(1.6, -0.8, 0)` line;
only that quad outlines and labels (`v4`..`v7`) — the first quad is left
alone even though both share the same `glBegin`/`glEnd` pair.

### Transform guides

With **Transform guides** on (Ctrl+Shift+X), placing the cursor on a committed
`glTranslatef` / `glRotatef` / `glScalef` line draws an overlay arrow or arc
showing what that line does — color-coded by axis (X=red, Y=green, Z=blue
blends), with a pulse traveling along the path:

![Cursor on a glTranslatef line: the guide shows the displacement](images/xform-guide-still.png)

Guides only appear when the line parsed cleanly and your current input
matches the committed source — partial or mid-edit lines are skipped.

All guides share an "axes pulse" visual language: a dim solid base line or
arc with a bright dot traveling along it and a short fading trail behind.
The color is derived from the command's vector so the shape of the motion
reads at a glance:

- **Translate** — shaft color is `(|tx|, |ty|, |tz|)` normalized by its max
  component, mapped to RGB. A pure-axis translation reads as a pure axis
  color (`glTranslatef(2, 0, 0)` → red, `glTranslatef(0, 0, -3)` → blue);
  diagonals blend. A 4-fin pyramid arrowhead marks the tip.
- **Rotate** — shaft color is `(|ax|, |ay|, |az|)` normalized, so a Y-axis
  rotation reads green. The axis stub through the rotation pivot shares the
  color; the pulse dot sweeps along the arc so direction is unambiguous.
  (When the "before" point lies on the rotation axis the arc collapses to a
  point — hover a rotate line whose pivot is off-axis to see the sweep.)
- **Scale** — shaft color is `(|sx-1|, |sy-1|, |sz-1|)` normalized, so the
  color highlights which axes deviate from identity (`glScalef(2, 1, 1)`
  reads red). The arrow runs from the "before" point to the component-wise
  scaled result. If the "before" point is at the origin, a 3-axis gizmo is
  drawn instead: a gray unit reference segment and a pulsing arrow per axis
  in that axis's color.

#### What is the "before" point?

OpenGL applies transforms in reverse source order when computing a vertex:
`M_1 · M_2 · ... · M_n · v`. That means the cursor's command `C_k` operates
on the point that the *later* commands `C_{k+1..n}` have already placed.
The guide starts at that point. Accumulation of the post-cursor transforms
stops at the first draw call (`glBegin`, the `glutSolid*` shapes, a tess
polygon) — transforms after an intervening draw don't factor in.

#### Guide mode

The Config menu has an **Xform guide mode** toggle with two options:

- **World** — the guide is rendered in world axes at the world origin: the
  strict OpenGL reverse-order reading of your line, independent of
  surrounding transforms. Cursor on the last line of

  ```
  glTranslatef(0, 0, 2);
  glRotatef(45, 0, 1, 0);
  glTranslatef(0, 0, -2);   // cursor here
  ```

  shows an arrow from `(0, 0, 0)` to `(0, 0, -2)` along world Z.

- **Frame** *(default)* — the guide anchors at the scene-world position the
  full pre-cursor modelview has carried the origin to, so it lines up
  visually with geometry drawn by earlier `func0()` / `glBegin` blocks.
  Only the anchor comes from the pre-cursor matrix — the guide itself still
  draws with world-axis orientation. Cursor on the second translate in

  ```
  glTranslatef(2, 0, 0);
  func0();
  glTranslatef(-4, 0, 0);   // cursor here
  func0();
  ```

  anchors the guide at `x = 2`, so the arrow runs `(2, 0, 0)` →
  `(-2, 0, 0)`, matching the rendered triangles. World mode would draw
  `(0, 0, 0)` → `(-4, 0, 0)`.

The *Transform stress* example (F12 to cycle to it) is built to exercise
all three transform guides (translate, rotate, scale) at once.

### The clip-plane guide

Park the cursor on a committed `glClipPlane` line and the plane draws
itself: a translucent gridded disc lying in the plane, a dashed ghost rim
that reads through occluding geometry, and an arrow pointing into the
*kept* half-space with a `P0 =(a, b, c, d)` readout. If the program never
enables that plane's cap, the guide dims and the readout appends `(off)`.

![Cursor on the glClipPlane line: the guide draws the plane as a gridded disc with a kept-side arrow](images/clip-plane.png)

Like the vertex guides, the disc renders in the coordinate frame active at
the call, so it sits exactly where the plane cuts.

### Winding & face diagnosis

**Winding** (Ctrl+Shift+B) re-renders the scene with front-facing polygons
in green and back-facing ones in red (as decided by the active
`glFrontFace`), so flipped or inside-out faces stand out immediately:

![Winding view: the left triangle is wound counter-clockwise (front, green), the right clockwise (back, red)](images/winding-view.png)

Both triangles here list three vertices; only the order differs. When
culling or lighting misbehaves, this view usually names the culprit in one
glance.

### Wireframe & hidden-line

**Ctrl+G** cycles Off / Wireframe / Hidden-line.

![Wireframe (left) and hidden-line (right) on a torus](images/wireframe-hidden-line.png)

Wireframe draws polygon edges over the scene. Hidden-line goes further: it
draws all edges first in a muted color, seeds the depth buffer with filled
polygons, then redraws visible edges bright — so the silhouette reads
clearly while occluded structure stays faint. Tip: vertex outlines/points
are on by default and draw over the wires; turn them off for a clean
wireframe look.

### The Config menu

Everything above — and the stage dressing below — has a home menu. Open the
**Config** dropdown (or press **Ctrl+Shift+K**). Items are grouped into
sections — hovering a section opens a flyout of its items, and the trailing
**All** row shows the entire table at once (the mouse wheel scrolls flyouts
taller than the window):

- **RENDERING** — MSAA, Line smooth, Accum effect, Accum passes, Point attenuation,
  Post FX Scope, Post FX Effect
- **TIME & REPLAY** — Auto time, Replay, Replay mode, Replay expand
- **SCENE** — Grid, Grid major, Grid extent, Grid brightness, Axes, Backdrop, Light theme,
  Light indicators
- **CAMERA** — View mode, Projection, Camera rotate, Focus origin, Reset camera
- **GEOMETRY** — Wireframe, Winding, Auto-normals
- **OVERLAYS** — Label & highlight scope, Vertex labels, Vertex points, Vertex outlines,
  Vertex outline style, Normal vectors, Polygon highlight, Transform guides
- **INTERFACE** — Variable panel, Compute profile, Memory profile, Code panel,
  Wrap at commas, Syntax highlight, Paren match, Paren scope

**Left-click** a flyout item to cycle it forward, **right-click** to cycle
backward. Multi-state items show their current state name.

Function keys drive the most common cycles directly (**Shift+F*n*** steps
backward):

| Key | Cycles |
|---|---|
| F2 | Grid theme |
| F3 | Grid extent |
| F4 | Grid brightness |
| F5 | Backdrop |
| F6 | Axes theme |
| F7 | Vertex labels |
| F8 | Label & highlight scope |
| F9 | Light theme |
| F10 | Post FX Scope |

### Grid & axes

![Grid themes: Sketchbook, Radar, Adaptive Planes, Ocean](images/grid-themes.png)

Twelve directly-selectable grid themes (**F2**): Off, Classic, Tron, Ember,
Ocean, XZ Ruler *(default)*, Adaptive Planes, Radar, Tilled Field, Sketchbook,
Neon Graph, Graph Planes. Some backdrops enable hidden companion grids; see
[Advanced Usage](ADVANCED_USAGE.md#cfg-backdropgrid-pairing).
**Grid major** (Ctrl+Shift+G) cycles the major-tick spacing (1/2/5/10),
**Grid extent** (F3) the grid's reach (Close / Mid / Far), and **Grid
brightness** (F4) the line weight (Dim / Normal / Bright / Bold). Theme
changes cross-fade, so a newly chosen grid takes a few seconds to fully appear.

Seven axes themes (**F6**): Off *(default)*, Classic, Pulse, Neon, Compass,
Gizmo, Ruler.

![Compass axes](images/axes-compass.png)

### Backdrops

![Backdrops: Polar Day+Snow, Nebula, Sunset, Aurora](images/backdrops.png)

**F5** cycles the scene backdrop: Off *(default)*, Cityscape, Stars,
City+Stars, Sunset, Aurora, Nebula, Polar Day, Polar Day+Snow.

Some backdrops enable a hidden companion grid. Nebula selects Star Chart;
see [Advanced Usage](ADVANCED_USAGE.md#cfg-backdropgrid-pairing) for the
`@cfg` details.

### Lighting

![Studio light theme on the teapot, with a light indicator](images/light-theme-studio.png)

**Light themes** (**F9**) are preset light rigs: Default (three colored
keys), Headlight (light 0 rides the camera), Solar (light 0 at the world
origin — for orbit/planet scenes), Studio (warm key / cool rim / warm fill),
Neon (saturated magenta/cyan/lime triad).

> [!NOTE]
> A theme only positions and colors
> the four light slots — your program still chooses which ones are on via
> `glEnable(GL_LIGHT0..3)`.

**Light indicators** (Ctrl+Shift+L) draw a marker at each light's position
(labelled `L0..L3`, with *off* noted for disabled lights), so you can see
where the rig sits.

### Rendering quality

- **MSAA** (Ctrl+U) — hardware multisampling on/off.
- **Line smooth** — GL line antialiasing.
- **Accum effect** (Ctrl+Shift+U) + **Accum passes** (Ctrl+= / Ctrl+−) — the
  accumulation buffer drives antialiasing and motion blur:
  - **AA** *(default, 1 pass)* — jitters the camera frustum per pass.
  - **Blur** — re-renders the scene per pass across the frame's
    animation-time window, so spinning geometry smears realistically —
    even while the camera moves (the camera itself stays crisp). Scenes
    that don't use `t` fall back to AA jitter.
  - **Blur Cam** — blurs camera motion only; falls back to AA when still.
  - Passes: 1/2/4/8/12/16. The status bar shows the active mode
    (`AA 1x`, `Blur 16x`). Blur is expensive — every pass is a full scene
    render. `--noaccum` disables the accumulation buffer entirely.

  ![Motion blur on a spinning cube (Blur 16x)](images/motion-blur.png)

- **Point attenuation** — distance-attenuated point sprites
  (`glPointParameterfv`); used by the glow/particle examples. On hardware
  without the entry point the REPL falls back to a distance-based
  `glPointSize` approximation.

  ![Glow sprites example: blending + point attenuation](images/glow-sprites.png)

- **Post FX Scope** (F10 forward / Shift+F10 backward) — where the selected
  effect applies: Off / 3D View / Frame.
- **Post FX Effect** — the selected operation: Chromatic aberration, Vignette,
  Scanlines, or Film grain.

---

## Replay

![Replay stepping through a scene](images/replay.gif)

Replay executes your program one command at a time so you can watch the
scene assemble — geometry appears incrementally, older geometry fades in a
ghosted pass, and the code panel highlights each line as it runs, with
loop-variable values substituted into the displayed text.

| Key | Action |
|---|---|
| **Ctrl+R** (or the Replay button) | Start / stop replay |
| **Space** | Pause / resume |
| **+** / **−** | Faster / slower |
| **Left** / **Right** | Step backward / forward (while paused) |
| **Ctrl+K** | Jump the replay to the cursor line (first geometry at/after it) |
| **m** / **M** | Toggle replay mode: Polygon / Vertex granularity |
| **e** / **E** | Toggle Replay expand while playback is live |
| **n** / **N** | Cycle replay normals: off / vector / vector + direction |
| **v** / **V** | Toggle the replay focused-vertex label |
| **Esc** | Stop replay |

The HUD at the bottom of the viewport shows play state, position, and speed.
When a replay has finished, **Space** restarts it from the beginning.
Two related config items: **Replay mode** (Polygon steps a primitive at a
time, Vertex steps a vertex at a time) and **Replay expand** (whether loops
expand iteration-by-iteration).

---

## Tutorials

The **Tutorials** menu offers guided, step-by-step lessons rendered directly
in the code panel — instruction comments appear with a typewriter reveal, and
each step either asks you to type a command (autocomplete ghost text shows
the expected call), acknowledge a short note, change a setting, or drag a
variable slider to a target.

- Tutorials are grouped by tag (hover a tag row for its flyout); flyouts
  group entries under difficulty subheadings.
- While a tutorial is active, the menu gains **Restart Tutorial** and
  **Exit Tutorial** entries.
- Instruction lines are locked — the tutorial guards them against edits
  until you finish or exit.
- The starter set: *First Triangle*, *Color & Transform*, *Feature Tour*,
  *Variable Slider*, *First Animation*, *Depth Test Triangle*, *Lighting
  Basics*, and *Color Interpolation*.

---

## Built-in Examples

**F12** cycles forward through the 32 built-in examples, then any saved
scenes, wrapping to the start; **Shift+F12** cycles backward. The Scene menu
lists them grouped by tag. `./gl-repl --list-examples` prints the compiled-in
set.
Developers can point the app at an editable catalog with
`./gl-repl --examples-dir examples --example <name-or-idx>`:

```
 1  gl-repl logo                                        17  Annotated orbit plot (labels)
 2  Rotating cube                                       18  GLU concave arrow
 3  Animated ring (for + t)                             19  GLU concave arrow cutout
 4  Conditional colors (if + t)                         20  GLU concave arrow extrusion
 5  Transform stress (translate/rotate/scale guides)    21  Glow sprites (blend + point attenuation)
 6  Parametric torus (nested for)                       22  Snowfall particles
 7  Animated wave surface (analytic normals)            23  Swaying grass field (rand + t)
 8  Torus knot (animated)                               24  Jellyfish (glDepthMask translucency)
 9  2D assignment sketch (vars only)                    25  Dusk lighthouse atoll (stress test)
10  Function demo (named func)                          26  Orrery (labels track 3D orbits)
11  Function polygons (args + for)                      27  Whale (particle system + lit model)
12  Function branching (args + if)                      28  Teapot carousel (transform stacks + glow points)
13  Recursive triangle tree (func + recursion)          29  Ringed planet (nebula skies)
14  Animated spirograph curve                           30  Aurora observatory (dish tracks the sky)
15  Traveling ripple ring                               31  Bubble sort (scratch arrays)
16  Bezier curve with guides                            32  Clip planes carve solids (glClipPlane)
```

Examples may carry their own presentation presets (grid theme, backdrop,
camera, 2D mode...). Loading an example resets the scene-presentation
settings to defaults first, then applies the example's presets — so examples
always look as authored, and your camera carries over unless the example
sets its own.

Editing an example automatically promotes it to a user scene (see the next
section) — you never modify the built-ins themselves.

---

## Scenes & Workspaces

gl-repl keeps up to 8 scenes in memory, shown as tabs below the menu bar.

- **Examples first** — a fresh launch opens on the default example. A user
  scene is created when you choose File -> New Scene, load a scene file or
  workspace, or edit an example.
- **Auto-promotion** — editing a built-in example forks it into a fresh
  scene slot named after the example. Subsequent edits accumulate there.
- **Rename** — File → Rename Scene opens an inline prompt in the status bar
  (Enter commits, Esc cancels). Names become filenames on export, so `/`,
  `\` and `:` are filtered.
- **Switching** — click a scene tab, or cycle with F12 / Shift+F12.

### Saving and loading

| Menu | Action |
|---|---|
| File → New Scene | Start a fresh scene |
| File → Save Scene (Ctrl+S) | Export the active scene as standalone C |
| File → Load Scene | Load a `.c` file into a new scene slot |
| File → Load Scene from Clipboard (macOS) | Load clipboard text, or the first Markdown fenced code block, into a new scene slot |
| File → Save Workspace | Write every open scene as `<name>.c` in a directory |
| File → Load Workspace | Load every `*.c` in a directory as scenes |

A workspace is just a directory of exported scene files. Once one is bound
and more than 8 scenes are open, the least-recently-used slot is
automatically saved to the workspace directory before being replaced.
Single files and workspaces round-trip freely — scene names and the
workspace binding are carried in `@scene-name` / `@workspace-dir` header
comments.

---

## Exporting & Importing

### Standalone C export

**Ctrl+S** (File → Save Scene) writes a complete, compilable GLUT/OpenGL C
program — header comments carry the REPL state (variables, config, camera),
your functions become C functions, and your commands become the `display()`
body. The generated file is **C89-compliant**, so it builds anywhere a GL/GLUT
toolchain exists, old machines included:

```bash
cc -std=c89 -Wall -o scene output.c -lglut -lGL -lGLU -lm     # Linux
cc -std=c89 -Wall -o scene output.c -framework OpenGL -framework GLUT -lm  # macOS
```

Everything round-trips: `./gl-repl output.c` reloads the file with
variables, settings, camera, scene name, and `// @tune` tags intact.

`Ctrl+Q` quits and saves a recovery copy to a temp file.

### Editing exported code & reimporting

The exported file is meant to be worked on. You can extend it by hand in C
and load it back — on reload, the commands between the `// Snippet start` /
`// Snippet end` markers are imported, so edits inside that span come home to
the REPL. Two rules keep the round trip clean:

- **Stay inside the REPL language** for the snippet section — the supported
  commands and syntax in [The REPL Language](#the-repl-language). Lines the
  importer doesn't recognize are skipped with a warning. (Anything goes
  *outside* the snippet markers, but those edits live only in the C file.)
- **Stay within the command budget** — the source document holds 1024
  lines and its flattened program 8192 commands (the status bar shows
  flat usage, e.g. `1345/8192 cmds`). Loops count once as source but
  every unrolled iteration lands in the flat program, so a heavy
  particle loop reaches the flat cap long before line 1024.

  To see *where* the budget goes, watch the last left-aligned status-bar
  segment as you move the cursor: `fn cmds 2480` on or inside a function
  definition (its total expansion across every call site), `scope cmds 230`
  inside a `for`/`if` block (the block's per-frame cost), `call cmds 96` on
  a call line (that call's inclusive expansion), `line cmds 12` on a plain
  line that expands more than once. For an offline breakdown,
  `./gl-repl --example "bubble sort (scratch arrays)" --flat-histogram`
  prints per-function and per-line costs sorted by spend.

> **Advanced — extending the REPL itself.** If you want a GL call the REPL
> doesn't speak yet, the interpreter is built to be extended: see
> [*Adding A New Command*](ARCHITECTURE.md#adding-a-new-command) in
> `ARCHITECTURE.md` for the full recipe (command type, spec-table row,
> executor case, replay annotation, help text, save/load round-trip). To
> raise the flat command budget, bump `MAX_FLAT_COMMANDS` in
> [`config.h`](../config.h) — it is `#ifndef`-guarded, so
> `-DMAX_FLAT_COMMANDS=16384` on the compiler command line works without
> editing the file (`MAX_EDITOR_COMMANDS` is the separate source-line cap).
> Expect proportionally more per-frame work: the flattened program
> re-executes every frame.

### Mesh export (PLY)

**F11** (File → Export .ply) captures the current scene as an ASCII PLY mesh
named after the active scene — `glVertex` polygons, GLU-tessellated shapes,
and GLUT solids all export through one GL feedback pass. Authored per-vertex
normals are preserved; the rest are synthesized and smoothed. Line
primitives export as PLY edges; point primitives export as loose vertices
(a point cloud).

Headless / scripted capture:

```bash
./gl-repl --example "2d assignment sketch (vars only)" --export-ply out.ply    # capture frame 1, exit
./gl-repl --example "2d assignment sketch (vars only)" --export-ply out.ply --export-ply-srgb   # decode colors sRGB->linear
```

Use `--export-ply-srgb` when the viewer is color-managed and treats PLY
colors as linear (otherwise the mesh looks washed out).

---

## Tunable Variables (`// @tune`)

Use `// @tune` on a `float` declaration when a scene parameter should become
a keyboard-adjustable knob in the exported standalone C program.

```c
float amp = 1.5; // @tune
float freq = 2;  // @tune

glBegin(GL_TRIANGLES);
glVertex3f(amp, 0, 0);
glVertex3f(0, freq, 0);
glVertex3f(0, 0, amp);
glEnd();
```

The tag is a bare trailing comment token. It matches `// @tune`, not names
like `// @tuned=5`. If a declaration line contains multiple names, every name
on that line is tagged:

```c
float amp = 1, freq = 2; // @tune
```

Tagged variables are still normal REPL variables while you are authoring. In
the variable panel, tagged rows get an accent mark so you can see which
values will export as knobs:

![Tagged rows get an accent mark](images/tune-badges.png)

### Exported controls

When you save/export C, each tagged variable becomes a keyboard knob in the
standalone program. The generated program also draws a small HUD listing each
knob, its current value, and its keys.

Knobs are assigned in declaration order:

| Knob | Raise | Lower |
|---|---:|---:|
| 1 | `q` | `a` |
| 2 | `w` | `s` |
| 3 | `e` | `d` |
| 4 | `r` | `f` |
| 5 | `t` | `g` |
| 6 | `y` | `h` |
| 7 | `u` | `j` |
| 8 | `i` | `k` |
| 9 | `o` | `l` |

Only the first 9 tagged variables get keyboard controls. If more are tagged,
the export keeps the first 9 and writes a note in the generated C that the
rest were capped.

### Step size

The exported knobs use the same step size as the in-app numeric stepper:

| Current value magnitude | Step |
|---:|---:|
| `< 10` | `0.05` |
| `10..99.999` | `0.5` |
| `100..999.999` | `5` |

The pattern continues by decade — values in the thousands step by `50`,
and so on (the step is `0.05` scaled by the value's power of ten).

The same modifier keys apply in the exported program:

| Modifier | Effect |
|---|---:|
| Shift | fine step, `x0.2` |
| Ctrl | coarse step, `x10` |
| Shift+Ctrl | fine then coarse, `x2` total |

There is no range clamp. A tunable can go negative or very large if you keep
pressing its keys.

### Values that stick

A tunable only persists if your display body does not overwrite it every
frame. This works:

```c
float radius = 2; // @tune

glutSolidSphere(radius, 32, 16);
```

This appears inert in the exported program because the assignment runs again
on every display call:

```c
float radius = 2; // @tune

radius = 2;
glutSolidSphere(radius, 32, 16);
```

Use tunables for parameter-style values that the scene reads, not values the
scene recomputes unconditionally each frame.

### Export and reload

`// @tune` survives export/import round trips. The exported C carries a
marker for each tagged declaration, and reloading that file reconstructs the
original declaration with `// @tune` so the variable-panel badge and future
exports keep working.

The generated keyboard controls live only in the standalone exported C.
Inside gl-repl, adjust values through the variable panel or inline numeric
steppers.

---

## Performance & Scope

The REPL is an interpreter. Every frame it re-evaluates expressions, and
while anything is animating it re-flattens the whole program — loops
unrolled, functions inlined, every argument expression parsed and evaluated
again. That is what makes the live experience possible (edit any line, drag
any slider, scrub time backwards), but it costs real CPU. Scenes built on
the [stateless patterns](#working-without-state) — especially the
replay-from-the-start kind — add their own recompute cost on top, since
each frame redoes all the work `t` implies.

The exported C program has none of that machinery — it is the same scene as
plain compiled GL calls, roughly **100× lighter on the CPU**. So the
workflow for pushing limits is:

1. Sketch and tune the scene in the REPL until it looks right.
2. Tag the parameters you still want to play with as `// @tune`.
3. Export, compile, and push the numbers (particle counts, tessellation,
   iteration depth) in the standalone program — via the exported tune knobs
   or by editing the C directly.

A scene that drops frames in the REPL at 500 particles will typically run
thousands in the export without effort.

That division of labor is by design: the REPL is a **launchpad, debugging
aid, and educational environment** — a place to see immediate-mode GL
respond line by line — not a platform to build a complete application on.
The export is the product; the REPL is where it is born.

> **Honest footnote on interpreter speed.** There is plenty of low-hanging
> fruit in the interpreter — identifier lookup is a series of string
> compares where a trie would do, and the per-frame flatten is a complete
> pass where a partial recompile of only the dirty range would suffice.
> These are deliberate omissions: optimizing further heads toward building
> a virtual machine, and the project's complexity has already grown well
> past its original intention. If the REPL feels slow, that's the signal to
> export.

---

## Profiling & Diagnostics

When a scene starts feeling heavy, the built-in profilers show where the
frame goes before you reach for the export.

### The compute profile (Ctrl+W)

**Ctrl+W** cycles the compute profile through Off / FPS / Sections /
Histogram. *FPS* shows only the frame-rate graph. *Sections* adds the full
collapsible per-section timing tree. *Histogram* adds the section-distribution
graph as the final, more expensive diagnostic surface.

![Histogram mode: the section listing (CPU/GPU/Max), the log-log section histograms, and the FPS plot](images/profile-panels.png)

Three floating panels work together:

- **The section listing** (right) breaks the frame into named sections —
  *Render 3D*, *Code Panel*, *Flatten*, and so on, down to *Frame Total*.
  The **CPU** column is a running average of wall-clock time per frame; the
  **GPU** column comes from asynchronous GL timer queries (no stalls — the
  numbers arrive a few frames late), and reads `--` where the driver lacks
  timer queries or a section emits no GL. **Max** shows the worse of the
  two averages — the number to watch when deciding what to trim. Sections
  that did nothing this frame dim out.
- **The section histograms** (center) overlay every top-level section's
  timing distribution on one graph. Both axes are logarithmic: microsecond
  sections and a 100 ms stall fit on the same plot, so a rare spike shows
  up as a small bump far to the right instead of vanishing into an average.
  Each section keeps its listing color; the legend below maps them.
- **The FPS plot** (bottom-right) graphs frame rate over the last 10
  seconds, minute, and 10 minutes as three overlaid series, with the
  current rate in the corner.

Histogram mode is the tool for *hitches*: a scene that averages 60 fps but
stutters once a second shows a clean main hump plus a second bump at the
stall duration — and the bump's color names the section responsible. The
histograms accumulate from load (they reset when you switch examples), so
leave the panel up while you reproduce the hiccup.

GPU timing needs timer-query support (GL 3.3 / `ARB_timer_query`, or the
`EXT_timer_query` fallback); `GLR_NO_GPU_PROF=1` disables it explicitly, and
the GPU column then reads `--`.

### Memory, messages, and startup

- **Memory profile** (Ctrl+Shift+W) — a floating panel with the process RSS
  history, a session baseline, and the delta since baseline. Useful for
  confirming a long editing session isn't growing without bound.
- **Message history** — click the button at the right end of the bottom
  message line to review recent status messages (parse errors you dismissed,
  save confirmations, budget warnings).
- **Ctrl+Shift+D** — dump debug state to stdout.
- Startup prints an init trace (`[init +N.NNNs] <phase>`) to stderr — useful
  for locating slow startup phases; `--detailed-prof` adds finer phases.

---

## Music

gl-repl plays background `.mp3`s found at startup, in filename order, from
three places combined:

1. **`./assets`** next to where you run it — override with `--assets <dir>`
   or `GLR_ASSETS_DIR`.
2. **Bundled with the app** — the macOS `gl-repl.app` ships a sample track.
3. **Your music folder** — `~/Library/Application Support/gl-repl/Music` on
   macOS (XDG data dir on Linux), created on first run; drop `.mp3`s there.

| Key | Action |
|---|---|
| Ctrl+Shift+A | Play / pause |
| Ctrl+Left / Ctrl+Right | Previous / next track |

`--no-audio` skips audio entirely. The play/pause state persists across
runs.

---

## Command-Line Options

```
./gl-repl [file.c | workspace-dir]   load a saved scene or a directory of scenes

--example <name|idx>   start on a built-in example (case-insensitive name or 1-based index)
--examples-dir <dir>   load examples from <dir>/catalog.ini and <dir>/scenes/
--list-examples        print the built-in examples and exit
--time <secs>          initial animation time t (also GLR_TIME; --time wins)
--window <WxH>         initial window size (default 1200x800)
--export-ply <path>    capture frame 1 geometry to PLY, then exit
--export-ply-srgb      decode vertex colors sRGB -> linear during PLY export
--assets <dir>         scan this dir for *.mp3 (also GLR_ASSETS_DIR)
--no-audio             skip audio init entirely
--noaccum              disable the accumulation buffer (AA + motion blur)
--dump-code            print the loaded buffer to stdout
--flat-histogram       print per-function / per-line flat-command costs
                       (where the 8192 budget goes; works with --example)
--detailed-prof        verbose startup timing trace (also GLR_DETAILED_PROF=1)
-h, --help             usage
```

Useful environment variables: `GLR_TIME`, `GLR_ASSETS_DIR`,
`GLR_EDIT_LINE=<n>` (park the cursor on source line *n* after load — poses
cursor-bound overlays like transform guides for headless captures),
`GLR_TYPE_KEYS=<text>` (feed keystrokes through the keyboard dispatch after
load — poses mid-typing states like the vertex entry guides and the
autocomplete popup),
`GLR_OPEN_COLOR_PICKER=<n>` (open the color picker on source line *n* —
poses the picker, which otherwise needs a swatch click),
`GLR_ACCUM_PASSES=<n>` (accumulation AA sample count, 1/2/4/8/12/16 — lets
headless captures smooth 3D edges at full UI text size),
`GLR_TICK_PER_FRAME=1` (advance the complete fixed-dt simulation once per
rendered frame for deterministic offline recordings),
`GLR_VIEW_TOGGLE_AT=<secs,...>` (toggle 2D/3D mode during deterministic
captures; implicitly enables `GLR_TICK_PER_FRAME`),
`GLR_NO_POINT_PARAMETER=1` (force the no-`glPointParameterfv` fallback),
`GLR_NO_GPU_PROF=1` (disable GPU timer-query profiling),
`GLR_AUDIO_HITCH_MS` (audio worker stall-warning threshold).

For scripted screenshots and GIF/MP4 recordings, the deterministic
frame-record mode captures exactly N rendered frames and exits — every
screenshot and GIF in this guide was generated that way
(`scripts/docs-assets.sh`). For fully *headless* rendering with no window at
all, build with `FREEGLUT_OSMESA=1`; both are covered in
[*Headless rendering (OSMesa)* in `ADVANCED_USAGE.md`](ADVANCED_USAGE.md#headless-rendering-osmesa).

---

## Keyboard & Mouse Reference

Press **F1** in-app for the always-current version of this list (the Keys
tab), plus the full command reference (the Commands tab).
For shortcut-maintenance details, reserved control-key aliases, and the
`scripts/keymap.sh` audit helper, see
[Advanced Usage -> Keyboard map tooling](ADVANCED_USAGE.md#keyboard-map-tooling).

### Editing

| Key | Action |
|---|---|
| `;` | Commit current line |
| Enter | Insert new line |
| Backspace | Delete character or selected lines |
| Tab | Autocomplete (Tab/Enter accepts) |
| Up / Down | Navigate lines |
| Left / Right | Move cursor within line |
| Home / Ctrl+A | Start of line |
| End / Ctrl+E | End of line |
| Shift+Arrows / Home / End | Extend selection |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut / paste |
| Ctrl+Z / Ctrl+Y | Undo / redo |
| Ctrl+D | Delete line or selection |
| Ctrl+L | Clear all commands |
| Ctrl+F | Search |
| Ctrl+\ | Reformat buffer |
| Ctrl+/ | Toggle comment |
| Ctrl+Shift+S | Split multi-variable declaration |
| Ctrl+Shift+F | Toggle code focus |
| Ctrl+B | Cycle code panel layout |
| Ctrl+Shift+Y | Cycle syntax highlight |
| Right-click empty line | Toggle OpenGL state touched before that line, compared with initial defaults |
| Esc | Clear input / close overlay |

### Scene & rendering

| Key | Action |
|---|---|
| Ctrl+T | Play/pause time |
| Ctrl+Shift+T | Reset time to 0 |
| Ctrl+Shift+B | Winding |
| Ctrl+R | Start/stop replay (Ctrl+K jump to cursor) |
| Ctrl+G | Wireframe |
| Ctrl+U | MSAA |
| Ctrl+Shift+U | Accum effect |
| Ctrl+Shift+G | Grid major spacing |
| Ctrl+= / Ctrl+− | Accum passes up/down |
| Ctrl+Shift+X | Transform guides |
| Ctrl+Shift+N | Normal vectors |
| Ctrl+Shift+O | Vertex outlines |
| Ctrl+Shift+L | Light indicators |
| Ctrl+Shift+P | Vertex points |
| Ctrl+P | Polygon highlight |
| Ctrl+Shift+K | Open Config menu |
| Ctrl+W / Ctrl+Shift+W | CPU / memory profile panel |
| F2–F10 | Config cycles (Shift steps backward) — see [The Config menu](#the-config-menu) |
| F11 | Export .ply |
| F12 / Shift+F12 | Next / previous example or scene |
| F1 | Help overlay |
| ` | Variable panel |

### Camera

| Key | Action |
|---|---|
| Left-drag | Orbit |
| Right-drag | Pan XZ (Shift+Right-drag: pan Y) |
| Scroll | Zoom (with momentum) |
| Ctrl+O | Focus origin |
| Ctrl+Shift+C | Reset camera |
| Ctrl+Shift+R | Auto-rotate |
| Ctrl+Shift+V | 2D / 3D view mode |
| Ctrl+Shift+E | Projection (Perspective / Ortho) |

### Session & audio

| Key | Action |
|---|---|
| Ctrl+S | Save scene |
| Ctrl+Q | Quit (saves recovery file) |
| Ctrl+Shift+D | Debug state dump |
| Ctrl+Shift+A | Play / pause |
| Ctrl+Left / Ctrl+Right | Previous / next track |

> **macOS note:** Cmd+letter works the same as Ctrl+letter. F11 may be
> claimed by the system's "Show Desktop" — use File → Export .ply instead.
