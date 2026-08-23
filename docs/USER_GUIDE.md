# gl-repl User Guide

gl-repl is an interactive OpenGL interpreter. You type classic immediate-mode
GL commands and see geometry render live as you type, with interactive overlays
guiding each edit. Every command stays in an editable code panel, so a scene is
a readable list of GL calls you can revisit, tweak, animate, replay
step-by-step, and export as a standalone C program.

![gl-repl rendering the Whale example](images/hero.png)

**The code is the interface.** You shape the scene by editing commands - there
is no click-to-select, drag-a-vertex, or gizmo-in-the-scene editing. Mouse
input inside the 3D viewport moves the *camera* and nothing else (orbit, pan,
zoom). Everything else you can point at - menus, the code panel, the variable
panel sliders, the status bar - lives in the 2D chrome around the viewport, and
the edits it makes show up as text in your program.

This guide follows the shape of a session: you [start the app](#getting-started),
get your bearings from the [built-in examples](#built-in-examples) and guided
[tutorials](#tutorials), then [write some code](#writing-code) in
[the REPL language](#the-repl-language),
[make it move](#making-it-move), lean on the
[visual feedback](#seeing-what-youre-doing) and
[diagnostic views](#diagnostic-views) to understand and debug what you built,
and finally [keep and ship](#scenes--workspaces) the
result. Reference material - the common [CLI](#command-line-options) and
[keyboard](#keyboard--mouse-reference) listings - sits at the end.

This guide describes what gl-repl *is*. For the OpenGL techniques you can
build with it - clip planes, stencil masks, decals, attribute scoping - and
worked explanations of the math functions, see
[`TUTORIAL.md`](TUTORIAL.md). For headless rendering, recording, and every
environment variable, see [`ADVANCED_USAGE.md`](ADVANCED_USAGE.md); for project
internals, [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Contents

**Start here**

- [Getting Started](#getting-started)
- [Built-in Examples](#built-in-examples)
- [Tutorials](#tutorials)
- [Guided Tours](#guided-tours)

**Write and animate**

- [Writing Code](#writing-code)
- [The REPL Language](#the-repl-language)
- [Making It Move](#making-it-move)
- [Camera & Views](#camera--views)

**See and debug**

- [Seeing What You're Doing](#seeing-what-youre-doing)
- [Diagnostic Views](#diagnostic-views)
- [The Config Menu](#the-config-menu)
- [Scene Appearance](#scene-appearance)
- [Replay](#replay)

**Keep and export**

- [Scenes & Workspaces](#scenes--workspaces)
- [Exporting & Importing](#exporting--importing)
- [Tunable Variables (`// @tune`)](#tunable-variables--tune)
- [Performance](#performance)
- [Limitations](#limitations)
- [Profiling](#profiling)
- [Music](#music)

**Reference**

- [Command-Line Options](#command-line-options)
- [Keyboard & Mouse Reference](#keyboard--mouse-reference)

---

## Getting Started

Run a fresh session, or reload earlier work:

```bash
./gl-repl                  # fresh session
./gl-repl output.c         # reload a saved scene
./gl-repl workspace/       # load a managed .glr-workspace directory
printf 'glutSolidCube(1);\n' | ./gl-repl -  # load a snippet from stdin
```

### The window

![The window: scene tabs, code panel with a command help card, status bar, viewport, variable panel and an assignment plot, over a motion-blurred spinning torus](images/window-tour.png)

Top to bottom:

- **Menu bar** - *File*, *Scene*, *Tutorials*, *Tours*, *Config*, *Audio*
  dropdowns, a *search...* slot (same as Ctrl+F), and the *Replay* button at
  the far right.
- **Scene tabs** - one tab per open scene (here *Spinning Torus* and *Ring
  Sketch*), behind the name of the managed workspace they came from (*Tour*).
  Click to switch.
- **Code panel** - the live, editable list of GL commands. By default it sits
  above the viewport; cycle its position (Left / Top / Bottom / Hidden) with
  **Ctrl+B** or the *Code panel* config item. When a document is taller than
  the panel, a scrollbar appears on the right: wheel to scroll, drag the thumb,
  or click the track to jump.
- **Status bar** - command count, current line, and the accumulation indicator
  (`AA 1x` / `Blur 16x`). The shot above is under motion blur, which is why the
  torus smears. The bar also carries clickable controls: undo/redo, copy/cut,
  a reset to the six display defaults, *focus*
  ([code focus](#keeping-the-buffer-tidy)), and *F1 help*.
- **3D viewport** - your geometry, rendered every frame. Drag to orbit,
  scroll to zoom.
- **Variable panel** - bottom-right overlay listing `t` and every program-wide
  variable with a draggable slider (see [The Variable
  Panel](#the-variable-panel)). Function locals do not appear there.
- **Floating panels** - what right-clicking a row opens. Above: the
  [assignment value plot](#plotting-an-assignments-values) tracking both angle
  rows that drive the torus, and the help card for `glutSolidSphere`.
- **Message line** - the bottom row shows the most recent status message.
  Click the small button at its right end to pop up the recent-message
  history. The bell stays red while that history holds an unread error,
  and returns to muted once you open the list.

For a guided flythrough of the menus without leaving the app, run the *Menus &
Examples* entry from the [Tours](#guided-tours) menu.

### Your first triangle

Fresh launches open on the default built-in example. Choose **File → New
Scene** first so the triangle has a clean scene of its own. New Scene already
seeds the first line below (see [Display default
commands](#display-default-commands)); keep it so it is obvious that *your
program* owns the frame. Type the remaining lines after the defaults, then
press **`;`** to commit each line, or **Enter** to commit and insert a new one:

```c
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glColor3f(0.98, 0.76, 0.36);
glBegin(GL_TRIANGLES);
glVertex3f(0, 1, 0);
glVertex3f(-1, -1, 0);
glVertex3f(1, -1, 0);
glEnd();
```

![A first triangle](images/first-triangle.png)

> [!NOTE]
> Depth testing is not enabled yet, so the grid behind the triangle remains
> visible. Add `glEnable(GL_DEPTH_TEST)` before `glBegin(GL_TRIANGLES)` to turn
> on depth testing.

The triangle appears as soon as the vertices commit. Now:

- Drag in the viewport to orbit, scroll to zoom.
- Press **Ctrl+T** and change a vertex to `glVertex3f(sin(t), 1, 0)` to
  animate it.
- Press **F12** to flip through the built-in examples for ideas.
- Press **F1** for the built-in help overlay - its *Commands* tab lists every
  supported command, while *Editor* and *Scene* split the keyboard and mouse
  reference. Esc or a click outside dismisses it.

Deleting `glClear` smears the frame: nothing else clears the scene rectangle
for you. That is the same rule the [exported C](#exporting--importing) follows.

---

## Built-in Examples

**F12** cycles forward through the 40 built-in examples, then any saved
scenes, wrapping to the start; **Shift+F12** cycles backward. The Scene menu
lists them grouped by tag. `./gl-repl --list-examples` prints the compiled-in
set.

The **&lt; Prev** / **Next &gt;** buttons in the menu bar (left of the 2D/3D
swatch) do the same stepping with the mouse - and while a tutorial is
running they step the *lesson* catalog instead, matching F11 / Shift+F11.
Hovering either one names where it lands - `Next Example | Torus knot |
F12` - so you can see what is one click away before taking it.
They hide themselves when the code panel is too narrow to fit them
alongside the menus and the search field.

Examples may carry their own presentation presets (grid theme, backdrop,
camera, 2D mode...). Loading an example resets the scene-presentation
settings to defaults first, then applies the example's presets - so examples
always look as authored, and your camera carries over unless the example
sets its own.

Changing an example's document automatically [promotes it to a user
scene](#scenes--workspaces). Moving the camera or flipping a config toggle
does not.

---

## Tutorials

The **Tutorials** menu offers guided, step-by-step lessons rendered directly
in the code panel - instruction comments appear with a typewriter reveal, and
each step either asks you to type a command (autocomplete ghost text shows
the expected call), acknowledge a short note, change a setting, or drag a
variable slider to a target.

Run `./gl-repl --list-tutorials` to print the catalog, or start one directly
with `./gl-repl --tutorial "First Triangle"` (a 1-based index also works).

- Tutorials are grouped by tag (hover a tag row for its flyout); flyouts
  group entries under difficulty subheadings.
- While a tutorial is active, the menu gains **Restart Tutorial** and
  **Exit Tutorial** entries.
- Instruction lines are locked - the tutorial guards them against edits
  until you finish or exit.
- **Beginner:** *First Triangle*, *Color & Transform*, *Points & Lines*,
  *GLUT Solids Tour*, *Scene Chrome & Overlays*, *First Animation*,
  *Expressions & Motion*, *Variable Slider*, and *First Loop*.
- **Intermediate:** *Lighting Basics*, *Normals*, *Flat & Smooth Shading*,
  *Materials & Shininess*, *Culling & Winding*, *Two-Sided Lighting*,
  *Color Interpolation*, *Blending & Transparency*, *Depth Mask & Draw Order*,
  *Fog*, *Clip Planes*, *Line Stipple*, *Bitmap Text*, *Console Output*,
  *Transform Stacks & Hierarchy*, *Watching a Program Run*, and
  *Keeping Your Work*.
- **Advanced:** *Functions*, *If & Conditionals*, *Scratch Arrays*, and
  *Loops Beyond the Ring*.

---

## Guided Tours

Where a tutorial asks *you* to type, the **Tours** menu drives the app for
you: pick an entry and a synthetic pointer takes over - gliding through
menus, hovering flyouts, clicking, and typing, with an on-screen cursor,
click ripples, spotlight rings, and captions narrating each step.

Tours have replay-style transport:

| Key | Action |
|---|---|
| **Space** | Play / pause (from the end, restarts) |
| **Left** / **Right** | Step back / forward |
| **+** / **-** | Faster / slower (0.25× .. 16×) |
| **Esc** | Exit the tour (keeps its result) |

Any other physical input - a key that is not transport, a click, a wheel
tick - cancels the tour and hands control back. A finished tour closes on
its own after the last caption.

Start one from the menu, or from the command line:

```bash
./gl-repl --list-tours
./gl-repl --tour "Menus & Examples"
./gl-repl --tour editing-basics --tour-stop before-torus
```

- *Menus & Examples* - browses the Scene example flyouts, loads a showcase
  scene, peeks at Tutorials, toggles a Config overlay, and runs a replay.
- *Editing Basics* - creates a fresh scene and types a spinning triangle
  plus a torus, line by line, showing commits and the autocomplete ghost.
- *Camera & Views* - orbit drags, wheel zoom, and the focus-origin ease.
- *Getting Help* - opens the F1 overlay and clicks through its Overview /
  Commands / Keys / About tabs, then right-clicks a command in the code panel
  to pop that command's own help card.

Tours aim at the UI elements themselves rather than at fixed screen positions,
so they play correctly at any window size, and each step waits for the last one
to finish rather than running on a stopwatch. The web build ships a slightly
different catalog, since it replaces the native File menu with its own
controls.

---

## Writing Code

The code panel works like a text editor: write and modify your scene's source
there. Unlike a normal editor, gl-repl renders the scene live as you type.

### Committing lines

New and edited lines are checked before they become part of the scene.

- Press **`;`** to commit the current line. If it is valid, gl-repl applies
  it. **Enter** commits *and* inserts a new line after it. If the line is
  invalid, the error is highlighted and the line stays active so you can fix
  it.
- Moving to another line with **Up/Down** or by clicking attempts to commit
  your edit automatically. Valid edits are kept; invalid edits are discarded
  and the previous committed version is restored. This keeps every inactive
  line valid.
- Press **Esc** to discard an edit yourself.

### Autocomplete

Autocomplete appears as you type:

![Typing glEnable(GL_LI: the popup lists the matching constants, ghost text completes inline](images/autocomplete.png)

Your own functions complete too. Once a slot is defined, both spellings are
offered - the bare `func3(` and any name you gave it (`drawCube(`) - and the
parameter hint after the open paren names that definition's parameters.

### Editing what's there

Selection, clipboard (**Ctrl+C / Ctrl+X / Ctrl+V**), and undo/redo
(**Ctrl+Z / Ctrl+Shift+Z**, with Ctrl+Y as an alternate) work like a normal
editor. Copy and cut also put the text on the **system clipboard**, and
Ctrl+V pastes from anywhere else - multi-line text arrives as source lines, a
single line lands in the input row when you are editing one. Right-click a GL
command for a short description, or right-click an assignment for [a plot of
its values](#plotting-an-assignments-values). **Ctrl+D** deletes the current
line or selection. The status-bar **select-all** button (the dashed marquee,
left of copy) highlights every line at once, ready to copy or cut; the trash
button resets the whole scene to the
[six editable display defaults](#display-default-commands).

**Ctrl+F** opens case-insensitive search over the buffer; **Up / Down** move
to the previous / next match. Press **Enter** for the next match, or **Esc**
to close it. If text is highlighted in the edit line when you press Ctrl+F,
that text seeds the find field; with nothing highlighted the previous query
is kept.

### Replace

**Tab** in the find bar opens a replace row. Type a replacement and press
**Enter** for **Replace All**, or click *Replace* for a single match. The
**word** chip restricts both search and replace to whole identifiers - turn it
on before renaming something short. Replace All rewrites the entire document
in one transaction and is undoable with **Ctrl+Z**.

### Display default commands

A new user scene is not empty. **File → New Scene** starts one with these six
lines already committed, and the status-bar trash button resets the current
scene to the same six lines. A fresh app launch is different: it opens the
default built-in example, as described under [Scenes &
Workspaces](#scenes--workspaces).

```c
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_COLOR_MATERIAL);
glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, (GLfloat[]){0.4, 0.4, 0.4, 1.0});
glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 30);
```

These defaults reduce boilerplate for a lit scene. They are ordinary editable
commands, so feel free to modify, remove, or replace them. Keep `glClear`
unless you want the previous frame to smear.

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

- HSV color controls with a hex readout.
- Palette tabs: **Basic**, **Full**, **Neon** (the curated accent set the
  built-in examples are coloured from), and **Harmony**.
- Changes write back to the source line in real time.

### Plotting an assignment's values

Right-click a `var = expr;` (or `A[i] = expr;`) row to plot what it actually
computed. The plot costs nothing while closed.

**X follows the row.** Inside a `for` loop it is the execution number within
one frame; for a top-level row that runs once per frame it is successive
captures over time.

![Left: a loop row, X is the execution index within one frame. Right: a top-level row, X is successive captures](images/assign-plot.png)

The caption names the mode and reports executions per frame. **Y** fits the
data without forcing a zero baseline; when the range crosses zero, that line
is brighter. The `log` chip uses ordinary log₁₀ for positive-only data and a
symmetric log axis when negative values or a zero crossing are present.

![The same two traces on a linear axis and on the symmetric log axis](images/assign-plot-log.png)

Log spacing separates traces of different magnitudes while keeping their
signs. Near-zero values collapse onto the center line; an all-zero trace has no
logarithmic range, so the chip is disabled. **Min**, **max**, **mean**, and
**sd** use every captured value for the selected series - hover its legend
entry, or move away to select the first.

| Control | Action |
|---|---|
| `once` / `1 Hz` / `frame` | Cycle capture rate; right-click cycles backward |
| `lin` / `log` | Switch Y between linear and logarithmic spacing |
| `1x` / `2x` | Toggle normal and double-size panels |
| `[reset]` | Clear samples and statistics, keeping the plotted rows and rate |
| `[x]` | Close the plot |

**once** freezes one comparable frame, **1 Hz** is the default live view, and
**frame** captures every frame. With several rows, **once** waits for a frame
in which they all execute.

**During [replay](#replay)** a vertical rule marks how far the replay has run
through each plotted row this frame, with the value printed beside it. Replay
captures every frame so the rule sits on real values; **once** stays frozen
and gets no rule. A top-level row plotted against successive captures has no
within-frame position to mark.

**Comparing several rows.** **Shift**+right-click adds an assignment to the
open plot instead of replacing it, up to four at once; Shift+right-click a
plotted row again to remove it.

![Three assignments from one loop body overlaid, with the legend naming them](images/assign-plot-series.png)

X comes from the first row, so another row must have the same executions per
frame or it is refused. Y spans every series - use `log` for widely different
magnitudes. Plain right-click retargets the plot to one row. Editing a plotted
row keeps tracking it; deleting it removes that series.

**Opening the plot from the scene: `// @plot`.** A trailing `// @plot` comment
on an assignment row plots that row as soon as the document loads:

```c
static float wobble;
wobble = sin(t * 3) * 0.4;   // @plot
```

Tag several rows to overlay them. The first tagged row fixes the X axis; rows
past the four-series limit are ignored. The tag rides the row's text, so it
survives save, reload, and export. It is re-read when a document is loaded,
never mid-session, so it cannot undo a retarget you just made by hand.

### Inspecting OpenGL state

Right-click a visually blank row in the code panel to inspect OpenGL state at
that point in the program. The row may already be committed or may be the
current, still-uncommitted empty input row.

![OpenGL state inspector opened on a blank source row](images/gl-state-inspector.png)

The popup opens on the state **your program** wrote before that row. Setup
writes from generated `init()` / `display()` start folded behind the
**[+] N from setup** chip. Click it to expand them; they draw muted. Explicit
writes that happen to match the OpenGL default are kept either way.

The current value is always visible; click the **[+] default/source** chip in
the column header to add the initial default and the source of the latest
write. The line number it quotes is the one in the code panel's left margin.

**Shift+right-click a second blank row** while the popup is open to pin that
row as the comparison basis: the default column becomes **value at L*n***, and
rows are coloured by whether they changed between the two lines. Shift+right-
click the pinned row again to unpin. The basis is a live line, so the
comparison stays correct as `t` advances.

A light's rows appear only while it can affect the frame. Modelview matrices
use four aligned rows; light positions show in both world and eye coordinates
when available. Scroll with the mouse wheel; click elsewhere or type in the
editor to dismiss.

`label(...)` draws with the raster color latched at `glRasterPos3f`, not a
later `glColor3f`. See [`label()`](#bitmap-text---label).

### Keeping the buffer tidy

| Key | Action |
|---|---|
| Ctrl+\ | Reformat all lines (re-indent blocks) |
| Ctrl+/ | Toggle `//` comment on the selection, the enclosing block, or the current line |
| Ctrl+Shift+F | Toggle code focus - on (the first-run view) shows just your code, off also shows the generated C and workspace chrome (also the *focus* keycap) |
| Ctrl+B | Cycle code panel layout: Left / Top / Bottom / Hidden |
| PgUp / PgDn | Scroll the active panel or overlay |

More code-panel toggles live in the Config menu: **Wrap at commas**
(long calls wrap at argument boundaries), **Syntax highlight**
(Ctrl+Shift+H; Off / On / On+Shadow), and **Paren match** / **Paren scope**
(highlight the matching bracket under the caret, and the span of the
enclosing parens).

**Syntax highlight** starts at On+Shadow, except on a Mesa renderer (most
Linux setups, and any software rasterizer) where it starts Off: drawing the
panel in many colors costs milliseconds a frame there, and the shadow mode
composites incorrectly. Only the starting point changes - Ctrl+Shift+H still
cycles all three, and the choice is saved with the scene.

---

## The REPL Language

The language is immediate-mode OpenGL with just enough structure around it -
variables, loops, functions, conditionals - to build real scenes. Every
numeric argument everywhere is a full math expression.

### Supported GL commands

The complete command list is below; worked techniques live in
[`TUTORIAL.md`](TUTORIAL.md).

- [`glBegin(MODE)`](https://docs.gl/gl2/glBegin), [`glEnd()`](https://docs.gl/gl2/glEnd)
- [`glVertex3f(x,y,z)`](https://docs.gl/gl2/glVertex), [`glVertex4f(x,y,z,w)`](https://docs.gl/gl2/glVertex), [`glVertex2f(x,y)`](https://docs.gl/gl2/glVertex)
  - `glVertex4f` submits homogeneous coordinates; OpenGL retains `w` for perspective division
- [`glNormal3f(x,y,z)`](https://docs.gl/gl2/glNormal)
- [`glColor3f(r,g,b)`](https://docs.gl/gl2/glColor), [`glColor4f(r,g,b,a)`](https://docs.gl/gl2/glColor)
- [`glClearColor(r,g,b,a)`](https://docs.gl/gl2/glClearColor) - background clear color; channels clamp to >= 0.15
- [`glTranslatef(x,y,z)`](https://docs.gl/gl2/glTranslate), [`glScalef(sx,sy,sz)`](https://docs.gl/gl2/glScale), [`glRotatef(deg,x,y,z)`](https://docs.gl/gl2/glRotate)
- [`glPushMatrix()`](https://docs.gl/gl2/glPushMatrix), [`glPopMatrix()`](https://docs.gl/gl2/glPushMatrix), [`glLoadIdentity()`](https://docs.gl/gl2/glLoadIdentity)
- [`glMultMatrixf((GLfloat[]){m0, ..., m15})`](https://docs.gl/gl2/glMultMatrix) - column-major 4x4, inline or a scratch array (`glMultMatrixf(A)`); see [Arbitrary matrices](#arbitrary-matrices)
- [`glPolygonMode(face,mode)`](https://docs.gl/gl2/glPolygonMode) - `GL_FILL`, `GL_LINE`, or `GL_POINT`, per face
- [`glPolygonOffset(factor,units)`](https://docs.gl/gl2/glPolygonOffset) - depth nudge; needs `glEnable(GL_POLYGON_OFFSET_FILL)`
- [`glPushAttrib(mask)`](https://docs.gl/gl2/glPushAttrib), [`glPopAttrib()`](https://docs.gl/gl2/glPushAttrib) - save/restore a group of GL state ([tutorial](TUTORIAL.md#scoping-state-with-glpushattrib); the editor draws the scope, see [Attribute scope](#attribute-scope)). `mask` is one or more of `GL_CURRENT_BIT`, `GL_POINT_BIT`, `GL_LINE_BIT`, `GL_POLYGON_BIT`, `GL_LIGHTING_BIT`, `GL_FOG_BIT`, `GL_DEPTH_BUFFER_BIT`, `GL_STENCIL_BUFFER_BIT`, `GL_TRANSFORM_BIT`, `GL_ENABLE_BIT`, `GL_COLOR_BUFFER_BIT`, or `GL_ALL_ATTRIB_BITS` for all eleven supported groups
- [`glEnable(CAP)`](https://docs.gl/gl2/glEnable), [`glDisable(CAP)`](https://docs.gl/gl2/glEnable)
  - CAP: `GL_DEPTH_TEST`, `GL_LIGHTING`, `GL_COLOR_MATERIAL`, `GL_NORMALIZE`,
    `GL_LINE_SMOOTH`, `GL_POINT_SMOOTH`, `GL_BLEND`, `GL_CULL_FACE`, `GL_FOG`,
    `GL_LINE_STIPPLE`, `GL_MULTISAMPLE`, `GL_STENCIL_TEST`,
    `GL_POLYGON_OFFSET_FILL`, `GL_POLYGON_OFFSET_LINE`, `GL_POLYGON_OFFSET_POINT`,
    `GL_LIGHT0`..`GL_LIGHT3`, `GL_CLIP_PLANE0`..`GL_CLIP_PLANE5`
- [`glShadeModel(MODE)`](https://docs.gl/gl2/glShadeModel)
- [`glPointSize(size)`](https://docs.gl/gl2/glPointSize), [`glLineWidth(width)`](https://docs.gl/gl2/glLineWidth)
- [`glLineStipple(factor, pattern)`](https://docs.gl/gl2/glLineStipple) - 16-bit pattern while `GL_LINE_STIPPLE` is on; hex stays hex
- [`glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, const, linear, quadratic)`](https://docs.gl/gl2/glPointParameter)
- [`glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA | GL_ONE)`](https://docs.gl/gl2/glBlendFunc)
- [`glColorMaterial(face, mode)`](https://docs.gl/gl2/glColorMaterial)
- [`glMaterialfv(face, pname, (GLfloat[]){r, g, b, a})`](https://docs.gl/gl2/glMaterial), [`glMaterialf(face, GL_SHININESS, value)`](https://docs.gl/gl2/glMaterial)
- [`glLightModeli(pname, param)`](https://docs.gl/gl2/glLightModel), [`glFrontFace(mode)`](https://docs.gl/gl2/glFrontFace), [`glCullFace(mode)`](https://docs.gl/gl2/glCullFace)
- [`glDepthFunc(func)`](https://docs.gl/gl2/glDepthFunc), [`glDepthMask(GL_TRUE|GL_FALSE)`](https://docs.gl/gl2/glDepthMask)
- [`glStencilFunc(func, ref, mask)`](https://docs.gl/gl2/glStencilFunc), [`glStencilOp(sfail, dpfail, dppass)`](https://docs.gl/gl2/glStencilOp), [`glStencilMask(mask)`](https://docs.gl/gl2/glStencilMask) - [tutorial](TUTORIAL.md#stencil-masks). `ref` is a full expression (it can animate); `mask` must be a literal. A literal out of `0..255` is rejected at commit; an animated one is clamped per frame. `glClearStencil` shares that policy
- [`glColorMask(r, g, b, a)`](https://docs.gl/gl2/glColorMask) - each channel `GL_TRUE`/`GL_FALSE` or 0/1
- [`glClear(mask)`](https://docs.gl/gl2/glClear) - **your program's own frame setup**. Nothing clears the scene rectangle on its behalf, so deleting the line smears the frame. A second one part-way down starts a later pass on a fresh buffer ([tutorial](TUTORIAL.md#clearing-mid-scene)). `mask` combines `GL_COLOR_BUFFER_BIT`, `GL_DEPTH_BUFFER_BIT`, and `GL_STENCIL_BUFFER_BIT`
- [`glClearDepth(depth)`](https://docs.gl/gl2/glClearDepth) - value a depth clear writes; GL clamps to 0..1
- [`glClearStencil(value)`](https://docs.gl/gl2/glClearStencil) - value a stencil clear writes, 0..255
- [`glEdgeFlag(GL_TRUE|GL_FALSE)`](https://docs.gl/gl2/glEdgeFlag)
- [`glClipPlane(plane, (GLdouble[]){a, b, c, d})`](https://docs.gl/gl2/glClipPlane) - user clip plane, gated by `glEnable(GL_CLIP_PLANE0..5)`; coefficients are expressions ([the clip-plane guide](#the-clip-plane-guide), [tutorial](TUTORIAL.md#clip-planes))
- [`glFogi(GL_FOG_MODE, GL_LINEAR|GL_EXP|GL_EXP2)`](https://docs.gl/gl2/glFog), [`glFogf(pname, value)`](https://docs.gl/gl2/glFog), [`glFogfv(GL_FOG_COLOR, (GLfloat[]){r, g, b, a})`](https://docs.gl/gl2/glFog) - enable with `glEnable(GL_FOG)`
- [`glRasterPos3f(x, y, z)`](https://docs.gl/gl2/glRasterPos) - position for bitmap text (see `label` below)

[`glMaterialfv`](https://docs.gl/gl2/glMaterial), [`glClipPlane`](https://docs.gl/gl2/glClipPlane), and [`glFogfv`](https://docs.gl/gl2/glFog) also accept a flat shorthand (`glMaterialfv(face, pname, r, g, b, a)`) that the parser rewrites to the compound-literal form.

#### Enum and mask arguments

An enum slot takes one token from a fixed list - no expression, no variable.
A mask slot (`glClear`, `glPushAttrib`) takes one or more of those tokens
joined with `|`. Tab completion offers the legal set for the slot the cursor
is in.

Masks are **canonicalized on commit**: duplicates collapse and tokens come
back in a fixed order. `GL_ALL_ATTRIB_BITS` is kept as itself.

Three slots bend the rule:

- **Boolean slots take `0`/`1` too.** `glDepthMask`, `glColorMask`, and
  `glEdgeFlag` canonicalize to `GL_TRUE` / `GL_FALSE`.
- **`glStencilFunc`'s `ref` is a real expression** while its `mask` stays a
  literal.
- **`glLightModeli`'s parameter slot is an expression**, since it carries a
  value rather than a mode.

### GLUT solid shapes

- [`glutSolidTorus(inner, outer, nsides, rings)`](https://github.com/freeglut/freeglut/blob/master/doc/api.md#152-glutwiretorus-glutsolidtorus)
- [`glutSolidCube(size)`](https://github.com/freeglut/freeglut/blob/master/doc/api.md#155-glutwirecube-glutsolidcube)
- [`glutSolidSphere(radius, slices, stacks)`](https://github.com/freeglut/freeglut/blob/master/doc/api.md#151--glutwiresphere-glutsolidsphere)
- [`glutSolidTeapot(size)`](https://github.com/freeglut/freeglut/blob/master/doc/api.md#1511-glutwireteapot-glutsolidteapot-glutwireteacup)
- [`glutSolidCone(base, height, slices, stacks)`](https://github.com/freeglut/freeglut/blob/master/doc/api.md#154-glutwirecone-glutsolidcone)

### GLU tessellator (concave / complex polygons)

![The contour rows of the GLU concave arrow example above the shape they tessellate into](images/glu-tess.png)

`gluTess` polygons handle concave outlines; a second contour punches a hole
(odd winding rule):

```c
gluBegin(GLU_POLYGON);
  gluNormal(0, 0, 1);
  gluBegin(GLU_CONTOUR);
    gluColor(0.98, 0.76, 0.36);
    gluVertex(-1, -1, 0);
    gluVertex(1, -1, 0);
    gluVertex(0, 1, 0);
  gluEnd();                      // close the contour
gluEnd();                        // close the polygon
```

The built-in examples *GLU concave arrow*, *GLU concave arrow cutout*, and
*GLU concave arrow extrusion* build up the three cases.

- `gluBegin(GLU_POLYGON)` - start a tessellated polygon
- `gluBegin(GLU_CONTOUR)` - start a contour within the polygon
- `gluEnd()` - end the current contour or polygon
- `gluNormal(nx, ny, nz)` - per-vertex normal
- `gluColor(r, g, b[, a])` - per-vertex color; alpha defaults to 1.0
- `gluVertex(x, y, z)` - add a vertex to the current contour

### Bitmap text - `label()`

![The Orrery example uses labels that track 3D orbits](images/labels-orrery.png)

```c
glRasterPos3f(x, y, z);      // place the text anchor (transforms apply)
label("Earth phase = %f", t);
```

- `label("fmt", a, b, c, d)` draws bitmap text at the current raster
  position. Up to 4 substitution args; `%f` substitutes a 6-character field
  (` 1.250`, `-0.017`) with a leading minus when negative, `%%` is a literal
  percent. The string literal
  is limited to 64 characters and may not contain `//`, `(`, `)`, `,`, or
  backslashes.
- This is a REPL convenience, not a real GL call - exported C files include
  a self-contained `label()` helper so they still compile standalone.

With lighting on, the text colour is the *lit* result latched at the
`glRasterPos` - a later `glColor3f` cannot change it.

> **macOS:** if `GL_COLOR_MATERIAL` is enabled when the `glRasterPos3f` runs,
> Apple's GL lights the tracked components as **zero** and the text comes out
> black. It is a driver deviation, not a REPL one - the same scene is correct
> on Mesa and NVIDIA, and the exported C behaves identically to the REPL
> either way. `glDisable(GL_COLOR_MATERIAL);` immediately before the
> `glRasterPos3f` works around it.

### Console output - `console()`

![The console panel: formatted debug output auto-indented by call depth](images/console-panel.png)

- `console("fmt", a, b, c, ...)` outputs formatted debug strings to a floating
  **Console panel** in the scene overlay. Up to 8 substitution arguments;
  `%f` substitutes a 6-character field (` 1.250`, `-0.017`) with a leading
  minus when negative so a live line does not shift as values change, and
  `%%` produces a literal percent sign. The format string is limited to 64
  characters and may not contain `//`, `(`, `)`, `,`, or backslashes.
- Lines emitted from functions are auto-indented by `2 * call_depth` spaces to
  visually mirror the execution tree.
- Legal anywhere in a scene, including inside `glBegin` / `glEnd` blocks.
- In the REPL: When frame replay is active, console output scrubs with the
  replay position.
- In standalone C export: Emits a `console()` helper that prints unindented
  lines directly to `stdout`, still using `%g` for each `%f`.

### Math expressions

Every numeric argument is a full expression, evaluated when the line runs:

- **Operators:** `+ - * / %` and parentheses; comparisons
  `> < >= <= == !=`; logical `&& || !`.
- **Functions:** `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(y, x)`,
  `sqrt`, `abs`, `pow`, `log`, `ln`, `min`, `max`, `clamp(x, lo, hi)`,
  `lerp(a, b, s)`, `smoothstep(e0, e1, x)`, `sign`, `floor`, `ceil`, `round`,
  `fmod`, `rem`, `rand(seed[, iter])`, `rand2(seed[, iter])`.
- **Constants:** `PI`, `TAU`, `e`, plus `NAN` and `INFINITY` (also spelled
  `nan`, `inf`, `infinity`).

`rand` returns a deterministic value in `[0, 1]` for a given (seed, iter) pair,
and `rand2` maps the same hash to `[-1, 1]`. There is no per-frame random
state: the same pair always returns the same number. See [Working without
state](#working-without-state).

That list is the whole vocabulary - a `funcN` emits geometry and does not
return a value. An unknown name is rejected at commit (`unknown function
'fabs'`). Export translates each name to its C twin (`abs` becomes `fabsf`).

What each function is *for*, and how to shape animation with `clamp` / `lerp` /
`smoothstep` / `atan2`, is worked through in
[TUTORIAL.md](TUTORIAL.md#math-expressions).

#### Where expressions differ from C

The syntax is C's, but this is a float evaluator with no integer type, and
the domain errors you are most likely to hit while editing are **guarded**:
division and `%` by zero yield `0`, `sqrt` takes the absolute value,
`asin`/`acos` clamp to `[-1, 1]`, and a zero-width `smoothstep` degenerates
to a step. Guarded is not the same as total - `log`/`ln`/`pow` can still
produce NaN. Worked examples of `fmod` vs GLSL `mod`, `rem`, unclamped
`lerp`, and NaN propagation live in
[TUTORIAL.md](TUTORIAL.md#where-expressions-differ-from-c).

- **`%` is float modulo** (`fmod`), not C's integer `%` and not GLSL's floored `mod`.
- **Dividing by zero yields `0`**, not infinity. So does `%` by zero (threshold `1e-12`).
- **`==` and `!=` compare within a `1e-6` epsilon**, not bitwise.
- **`&&` and `||` do not short-circuit** - nothing here has side effects.
- **`asin` and `acos` clamp their argument to `[-1, 1]`** first.
- **`abs` is float absolute value** (C's `fabsf`). C's `abs(-1.5)` is `1`.
- **The libm spellings are not accepted** - no `fabs`, no `sinf`.
- **`log` is base 10 and `ln` is base e** - the reverse of C's `log`/`log10`.
- **`fmod` truncates toward zero**; GLSL `mod` floors. `rem` is IEEE remainder.
- **`lerp` is unclamped**; `smoothstep` accepts `e0 > e1`.

There is no ternary `?:`, no bitwise operators (`|` appears only inside a
[mask argument](#enum-and-mask-arguments)), no compound assignment, and no
assignment-as-expression. A `1.5f` literal is accepted on input but the suffix
is dropped from the committed line.

### Variables

```c
float x, y, z;          // declare before use
x = 1.5;                // assign (any expression)
glVertex3f(x, y, z);    // use anywhere a number is expected
```

- Program-wide declarations are hoisted to the top automatically.
- Up to 31 program-wide user variables; the predefined `t` occupies a separate
  reserved slot.
- Program-wide values persist across commits and are saved/loaded with the
  scene. Initializers are allowed: `float n = 1;`.
- A name is at most 15 characters, and one declaration line may introduce at
  most 8 of them. `t`, `PI`, `TAU`, `e`, `float`, `var`, and the scratch-array
  names `A`, `B`, `C` are reserved.

A committed line holds at most **255 characters**. The line you are typing is
not held to that (the input buffer is 1024 bytes). A Replace All that would
push any line past 255 fails the whole operation rather than truncating.

That is what `float` means at the top level; `static float` is also
program-wide even when typed inside a function. A plain `float` inside a
function instead declares a [function-scoped local](#function-scoped-locals).

### Scratch arrays

`A[16]`, `B[16]`, and `C[16]` are three fixed global arrays:

```c
A[0] = 0;
A[1] = 1;
A[0] = A[0] + (A[1] - A[0]) * 0.25;
glVertex3f(A[0], 0, 0);
```

The index is a full expression, truncated to int, and must land in `0..15`. A
bare `A` is an error everywhere except [`glMultMatrixf(A)`](#from-a-scratch-array).
Scratch arrays persist and round-trip through save/load.

#### Writing several cells at once

A braced list fills a contiguous run of cells from one row:

```c
A[0] = {1, 0, 0, 0};      // writes A[0], A[1], A[2], A[3]
A[4] = {0, c, s, 0};
```

The cells are ordinary expressions, re-evaluated every frame. Four rows of
four is how a 4x4 for [`glMultMatrixf`](#from-a-scratch-array) is usually
written. Exact rules - base index, list length, and why a block row cannot
be plotted - are in
[ADVANCED_USAGE.md](ADVANCED_USAGE.md#scratch-block-assignment).

### Arbitrary matrices

`glTranslatef`, `glRotatef`, and `glScalef` cover almost everything.
`glMultMatrixf` is for what they cannot express - a shear, a mirror, a planar
shadow projection. It post-multiplies the current matrix by a column-major
4x4; cells 12, 13, 14 hold the translation:

```c
glMultMatrixf((GLfloat[]){1, 0, 0, 0, 0.4, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
```

The flat shorthand `glMultMatrixf(m0, ..., m15)` rewrites itself to the
compound-literal form. Cells are ordinary expressions, so a matrix built from
`t` animates. The classic planar-shadow projection is the *Planar shadows
(glMultMatrixf)* example; the walkthrough is in
[TUTORIAL.md](TUTORIAL.md#planar-shadows).

#### From a scratch array

Sixteen cells is a 4x4, so `glMultMatrixf` accepts a bare scratch array name:

```c
A[0]  = {cos(t), sin(t), 0, 0};
A[4]  = {-sin(t), cos(t), 0, 0};
A[8]  = {0, 0, 1, 0};
A[12] = {0, 0, 0, 1};
glMultMatrixf(A);         // a rotation about z, animated by t
```

This form is worth it when the matrix is built up over several lines or reused
by more than one call. Cells you never assign are zero - write every one your
matrix needs non-zero.

### For-loops

```c
for(i, 0, 24) glVertex3f(cos(i*TAU/24), sin(i*TAU/24), 0);

for(i, 0, n, 2) {        // optional step argument; multi-line body
    glVertex3f(i, 0, 0);
}
```

The parser accepts up to 64 nested blocks across `for`, `if`, and function
bodies. Loop bounds can be expressions (and can animate with `t`).

#### Leaving a loop early - `break` and `continue`

```c
for(i, 0, 64) {
    if(i*step > limit) {
        break;                        // stop the loop here
    }
    if(fmod(i, 3) == 0) {
        continue;                     // skip to the next iteration
    }
    glVertex3f(i*step, 0, 0);
}
```

Both mean exactly what they mean in C, and export as themselves. `break` and
`continue` address the **innermost enclosing** loop.

### Functions

```c
func1(radius, sides) {            // func0..func9, up to 10 params
    for(i, 0, sides) {
        glVertex3f(radius*cos(i*TAU/sides), radius*sin(i*TAU/sides), 0);
    }
}
drawCube() {                      // or any name - aliases to a free funcN slot
    glutSolidCube(1);
}

func1(1.5, 6);                    // call (parens always required)
drawCube();
```

Ten function slots are available. Recursion works when paired with an
`if(...)` guard - see the *3D tree* example.

#### Function-scoped locals

A `float` declaration *inside* a function body declares a **local**: it belongs
to that function, gets a fresh binding on every call, and takes none of the 31
program-wide variable slots and no variable-panel row.

```c
blade(x0, z0, height) {
    float bend, u;
    bend = sin(t*2.2 + x0)*height*0.26;
    for(s, 0, 6) {
        u = s/4;
        glVertex3f(x0 + bend*u*u, height*u, z0);
    }
}
```

- **`static` chooses storage.** `static float x;` is program-wide wherever you
  type it. Plain `float x;` is a local inside a function and program-wide at
  the top level.
- Every local starts at `0` on entry, and takes **no initializer**.
  `float x = 1;` inside a body is rejected: declare, then assign.
- Locals hoist to the top of the *function body*. They live for the whole
  call, not the block you typed them in.
- Recursion is safe: two frames of the same function never share a local.
- **Names follow C's scope rules.** A local may not collide with a parameter
  or another local of the same body. Shadowing a program-wide variable is
  fine; a loop iterator may shadow a local for the length of its loop.
  Parameters and loop iterators stay read-only.
- `// @tune` and `// @config` need a variable-panel row, so they require a
  program-wide declaration (`static float`).
- Capacity is `params + locals + deepest loop nesting <= 32`.

A local is not a return value. When the caller needs a result, keep that one
variable program-wide and let the function write it.

### Conditionals

```c
if(t > 2) {
    glColor3f(1, 0, 0);
} else if(t > 1) {
    glColor3f(0, 1, 0);
} else {
    glColor3f(0, 0, 1);
}
```

Continuation lines are matched as a unit, so both braces must sit on the same
line as the keyword - `} else if(t > 1) {` and `} else {`. Splitting the brace
onto its own line does not parse.

### Comments

Type `// text` directly to add a comment line, or press `Ctrl+/` to toggle a
comment on an existing one.

`Ctrl+/` works on a range, and picks it for you:

- **With a selection**, the range is the selection. If every selected line is
  already commented the press restores them all; otherwise it comments them
  all. A mix of code and comments comments the rest, so the next press puts
  the mix back exactly as it was.
- **Without one**, the cursor line decides. On a `for`, `if`, or function
  header - or on its closing `}` - the whole block toggles in one press,
  commented block included: `Ctrl+/` on `// triangle() {` brings the entire
  function back. On any other line it is just that line.

Comment and uncomment are exact inverses, including function names, parameter
lists, and expressions like `cos(ph + t)` that would otherwise come back as
the numbers they evaluated to.

A range is refused when it would not survive the trip back:

- a range that opens a block it does not close (or closes one it did not
  open);
- a range holding a `float` declaration whose variable is still read outside
  it;
- more than 16 lines at once.

A refused toggle changes nothing, leaves no undo step, and says why in the
status line.

Alternatively you can wrap lines in `if(0) { … }` - the body emits nothing
but the lines stay visible.

A trailing comment on a `float` declaration can also carry a tag:

- `// @tune` makes it a knob in the exported program - see [Tunable
  Variables](#tunable-variables--tune).
- `// @config` marks it as a parameter the program assigns on purpose, which
  keeps its variable-panel row bright instead of dimmed - see [Config
  Variables](ADVANCED_USAGE.md#config-variables--config).

---

## Making It Move

![The Animated ring example](images/animated-ring.png)

`t` is the one predefined variable - it exists in every session without a
declaration, starts at `0`, and while playing advances a fixed 1/60 s per
simulation tick. That is *not* the same as per rendered frame: a replay
backstep reconstructs its frame over several rendered frames, and none of
them advance `t`. Use it in any expression:

```c
glRotatef(t*45, 0, 1, 0);
glVertex3f(sin(t), cos(t), 0);
```

This is the whole animation model: there is no keyframe data and no
accumulated state. The scene re-evaluates every frame, so anything written
in terms of `t` animates, and a frame is a *pure function of `t`*. The same
`t` always produces the same picture.

- **Ctrl+T** plays/pauses time (the *Auto time* config item is the same
  toggle).
- **Ctrl+Shift+T** resets time `t` to zero.
- `--time <secs>` (or the `GLR_TIME` env var) sets the starting `t` when
  launching.

### Working without state

A frame being a pure function of `t` means you cannot write `pos = pos + vel`
and expect `pos` to remember where it was. Scenes that would normally keep
state use one of three patterns instead: deterministic `rand` keyed on an
index (*Snowfall particles*, *Swaying grass field*), closed-form integrals of
velocity (*Whale*), or replaying an algorithm from the start each frame
(*Bubble sort*). The same `t` always rebuilds the same picture, which is what
makes scrubbing, [replay](#replay), and `--time`-anchored capture
reproducible. Worked versions of all three live in
[TUTORIAL.md](TUTORIAL.md#working-without-state).

### The Variable Panel

![The variable panel: t plus three program-wide variables, each with its value and slider](images/variable-panel.png)

The variable panel (bottom-right) lists `t` plus every program-wide variable
declared by the scene, with its current value and a slider. Function locals
have no panel row.

- **Left-click drag** on a row scrubs the value linearly (0.1 units/px).
- **Right-click drag** is the *fast* scrub: 10× (1.0 units/px).
- **Shift + click drag** is the *slow* scrub: 1/5 speed (0.02 units/px).
- Toggle the panel with **Backquote** (`` ` ``) or the *Variable panel* config
  item.

Row brightness distinguishes knobs from storage:

- **Bright** - read-only parameter/config variables; best for sliders.
- **Dim** - variables written by committed `name = expr;` lines. You can
  drag them, but a later assignment may overwrite the value.
- A [`// @config`](ADVANCED_USAGE.md#config-variables--config) tag on the
  declaration keeps a row bright even though the program assigns it.
- The [`// @tune`](#tunable-variables--tune) accent is separate from
  brightness. `t` only dims if your source explicitly assigns `t = ...;`.

Slider edits are undoable and go through the normal commit pipeline; when a
declaration exists, dragging rewrites its initializer.

Because `t` is just a variable, dragging that row is a timeline scrubber.
Pause with **Ctrl+T**, drag `t`, then **Ctrl+T** again to resume from wherever
you left it.

#### Stepping One Frame At A Time

A drag scrubs, it does not step: at 0.1 units/px even the slow scrub moves
`t` more than a frame per pixel. So the `t` row carries a small **frame
stepper** at its right edge - the same two-arrow control the code panel puts
on a number and the find bar puts on the match count.

- **Up** advances `t` by exactly one simulation tick (1/60 s), **down** goes
  back one. Hold **right-click** on an arrow to jump ten frames instead.
- It is live only while time is paused. With *Auto time* on, the arrows are
  drawn greyed - a frame you step forward would be overwritten by the next
  tick anyway.
- Stepping backwards runs `t` below zero, exactly as dragging the slider
  does. Reset to a clean origin with **Ctrl+Shift+T**.

Stepping is a transport control, not an edit: it moves the clock, leaves your
source untouched, and does not enter the undo history.

---

## Camera & Views

Mouse input in the viewport is *camera-only* - there is nothing to click on
in the scene, and no drag can change your geometry. To move a vertex, edit
its `glVertex3f` line (or the variable feeding it); see
[Adjusting values without retyping](#adjusting-values-without-retyping).

| Input | Action |
|---|---|
| Left-drag (viewport) | Orbit around the target |
| Right-drag | Pan in the XZ plane |
| Shift+Right-drag | Pan vertically (Y) |
| Scroll wheel | Zoom, with momentum |
| Ctrl+O | Focus origin - ease the orbit target back to (0,0,0) |
| Ctrl+Shift+C | Reset the camera to its default pose (eased) |
| Ctrl+Shift+R | Toggle camera auto-rotate |
| Ctrl+Shift+V | Toggle View mode: 3D perspective / 2D ortho |
| Ctrl+Shift+E | Toggle Projection: Perspective / Ortho (free camera) |
| Ctrl+Shift+I | Look down Z |

**2D mode.** *View mode* (Ctrl+Shift+V, or the CAMERA section of the Config
menu) switches between the 3D perspective camera and a flat 2D orthographic
projection - useful for plots, sketches, and UI-like drawings. Examples that
declare `@cfg view_mode = RENDER3D_VIEW_2D` start in 2D automatically.

![Animation: toggling View mode between 3D perspective and 2D ortho on the wave surface](images/view-mode-2d.gif)

**Projection.** *Projection* (Ctrl+Shift+E) toggles between perspective and
orthographic projection while keeping the free, interactive camera. Unlike
*View mode* (which flattens and locks the camera to a top-down 2D view), it
keeps the current orbit angle, so you can navigate the scene
orthographically. Examples can declare `@cfg projection = PROJ_ORTHO`.

---

## Seeing What You're Doing

Immediate-mode GL is invisible state: the current color, the current matrix,
the winding of the polygon you just typed. A family of guides and overlays
exists to make that state visible while you edit. The [diagnostic
views](#diagnostic-views) that follow work the other way round: they re-render
the whole scene to answer one question.

### Vertex entry guides

While a `glVertex3f(` or `glVertex4f(` line is still being typed, the scene
shows where the vertex *could* still land. Each coordinate you type removes a
degree of freedom, and the guide collapses to match - a surface, then a line,
then a point:

![The guide narrowing as coordinates are typed: sheet at x = 1.2, then a line along z, then a point, then a fresh sheet for the next vertex](images/vertex-guides.png)

- **One coordinate** → a translucent sheet spanning the two open axes.
- **Two coordinates** → a line along the one open axis.
- **All three** → a point marker at the exact position. The same marker
  appears when the cursor sits on a committed vertex line.

The guide is colored by the axis you pinned (X red, Y green, Z blue).
`glVertex2f` pins `z = 0` implicitly, so its first coordinate already narrows
the guide to a line. Guides follow the cursor's transform context. `glVertex4f`
uses its first three coordinates for the same guide; `w` is passed through to
OpenGL.

### Cursor guides & vertex overlays

Once lines are committed, the cursor keeps pointing into the scene: whatever
geometry the line under your cursor draws is outlined and labelled, and moving
the cursor moves the highlight.

![Cursor on each of two tri() calls in turn; the highlight and vertex labels move to the triangle that call draws](images/cursor-highlight.png)

This works through function calls and loop iterations, not just on literal
`glVertex3f` lines. On a `glVertex3f` line the guide narrows to that one
vertex. Arrow the cursor down through a `glBegin` block and the crosshair
steps from vertex to vertex.

The overlay toggles annotate geometry scene-wide:

![The cursor on the glNormal3f row, and the quad below it carrying vertex labels, points, outlines, normals and the polygon highlight](images/vertex-overlays.png)

- **Vertex labels** (F7): Off / Index / Index+Pos / Index+World /
  Index+World Fine - numbers each vertex of the primitive at the cursor.
  The cursor has to be resting on a committed `glVertex3f` line, unless
  **Overlay scope** is *Whole scene*.
- **Overlay scope** (F8): Cursor block - last instance / all instances /
  single polygon - plus **Whole scene**. Controls how broadly the cursor-bound
  overlays are shown. *Last instance* picks the final unrolled copy, so they
  sit on the copy whose loop-body values the **variable panel** is showing.
  *Whole scene* takes every vertex the program emits. Vertex labels are culled
  against the depth buffer when the context can read it; the web build shows
  every in-scope label instead.
- **Vertex label placement**: Decluttered / At vertex - *where* each label
  sits. Decluttered numbers globally and floats labels onto clear rows; At
  vertex numbers per primitive and pins each label to its vertex. See the
  figure under [Vertex label placement](#vertex-label-placement).
- **Normal vectors** (Ctrl+N): draws each vertex's normal as an arrow,
  within the **Overlay scope** above.
- **Vertex outlines** (Ctrl+Shift+O) and **Vertex points** (Ctrl+Shift+P):
  outline polygons and mark vertices *(both on by default)*.
- **Polygon highlight** (Ctrl+P): highlights the polygon under the cursor
  line. Cycles Off / On / **Clipped & culled**. On draws the cursor's shape
  **as authored**; Clipped & culled draws only what the frame draws.

Vertex outlines and vertex points always report the geometry the frame
actually shows (clip planes, culling, color mask). The cursor highlight is
the deliberate exception: it draws through a color mask, and under Polygon
highlight = On it ignores clipping and culling too.

### Vertex label placement

![Label placement: one quad drawn twice by a loop, shown decluttered (v0..v7, globally unique) and at vertex (v0..v3, repeated per copy)](images/label-placement.png)

**Decluttered** is global (`v0`..`v7` across both copies). **At vertex** is
per-primitive (`v0`..`v3`, repeated). *Single polygon* scope narrows labels
and the highlight to the one primitive your cursor is building inside a
multi-face `glBegin`.

![Single polygon scope: cursor on the second quad's second vertex highlights and labels only that quad](images/single-polygon-scope.png)

### Transform guides

With **Transform guides** on (Ctrl+Shift+X), placing the cursor on a committed
`glTranslatef` / `glRotatef` / `glScalef` line draws an overlay showing what
that line does:

![Four transform guides: a translate arrow, a rotate dial, and two scale gizmos, each with the program that produced it](images/xform-guide-montage.png)

- **Translate** - an arrow along the argument vector, tip where the geometry
  lands.
- **Rotate** - a dial in the plane of the rotation, labeled with the angle.
- **Scale** - a 3-axis gizmo with a pulsing arrow only on axes whose factor
  is not 1.

Guides only appear when the line parsed cleanly and your current input
matches the committed source.

**Transform guides** is a three-state setting - Ctrl+Shift+X cycles Off →
World → Frame:

![The same glTranslatef line under World mode and under Frame mode: World anchors the arrow at the world origin, Frame at the first cube](images/xform-guide-mode.png)

- **World** - drawn in world axes from the point the commands *after* the
  cursor have already placed (accumulation stops at the first draw call).
- **Frame** *(default)* - anchored where the modelview *before* the cursor
  has carried the origin. The same vector now lines up with the geometry as
  rendered.

The *Transform stress* example exercises all three guides at once.

### The clip-plane guide

Park the cursor on a committed `glClipPlane` line and the plane draws
itself: a translucent gridded disc, a dashed ghost rim, and an arrow pointing
into the *kept* half-space. If the program never enables that plane's cap,
the guide dims and the readout appends `(off)`.

![Cursor on the glClipPlane line: the guide draws the plane as a gridded disc with a kept-side arrow](images/clip-plane.png)

### Attribute scope

`glPushAttrib` / `glPopAttrib` protect a group of GL state
([tutorial](TUTORIAL.md#scoping-state-with-glpushattrib)). Park the cursor on
a `glPushAttrib` line and each mask token lights up, while every earlier line
whose value the push *saves* and every scoped line whose change the matching
pop *reverts* gets a matching gutter marker. Park it on the `glPopAttrib` to
focus on just the lines whose changes it reverts.
A push or pop with no partner gets the same red gutter warning as an unmatched
`glPushMatrix`.

### Auto-normals

**Auto-normals** does not draw anything - it writes `glNormal3f` lines into
your program. It is an experimental helper for pasted static geometry with no
normals: **Face** (hard edges) or **Smooth** (area-weighted). **The pass only
touches a `glBegin` block whose vertex coordinates are all literal numbers.**
Expression-driven vertices - a `for` loop, anything using `t` - are skipped.

Generated lines are tagged `auto` and drawn dimmer. Switching to **Off**
removes them (undoable). Editing a generated line makes it yours. Exported
`.c` files mark generated normals with `/* @auto */`.

---

## Diagnostic Views

Each of these re-renders the whole scene to answer one question. They are
independent of the cursor, and they stack with the overlays above.

### Winding & face diagnosis

**Winding** (Ctrl+Shift+W) re-renders the scene with front-facing polygons
in green and back-facing ones in red (as decided by the active
`glFrontFace`):

![Winding view: the left triangle is wound counter-clockwise (front, green), the right clockwise (back, red)](images/winding-view.png)

When culling or lighting misbehaves, this view usually names the culprit in
one glance.

### Wireframe & hidden-line

**Ctrl+W** cycles Off / Wireframe / Hidden-line.

![Wireframe (left) and hidden-line (right), on a torus (top) and a Sierpinski sponge (bottom)](images/wireframe-hidden-line.png)

Wireframe draws polygon edges over the scene. Hidden-line draws all edges
muted, seeds the depth buffer with filled polygons, then redraws visible
edges bright - so the silhouette reads clearly while occluded structure stays
faint. Vertex outlines/points are on by default and draw over the wires; turn
them off for a clean look.

### Depth view

**Ctrl+Shift+D** cycles the Depth view: **Off / Linear / Scene / Split** - a
grayscale rendering of the depth buffer. Near surfaces are bright, far ones
dark, empty background is black:

![Depth view: Scene mode (left) normalizes the grayscale to the geometry's own depth range; Split (right) overlays depth on the right half of the normal render](images/depth-view.png)

- **Linear** - gray ramp across the whole near/far range. Use it for absolute
  depth (how close geometry sits to the near plane).
- **Scene** - ramp stretched over *your geometry's* actual depth extent.
- **Split** - normal render with scene-normalized depth on the right half.

The snapshot is taken right after your geometry draws, before the grid,
backdrop, and axes. It works during replay. On the web build the row is
inert - WebGL cannot read the depth buffer back.

### Stencil view

**Stencil view** (Ctrl+Shift+S) cycles **Off / Palette / Ramp / Split** - a
false-color overlay of the stencil buffer, with a legend in the scene's
top-left. It is the companion to [stencil masks](TUTORIAL.md#stencil-masks):
a mask changes what draws without ever showing itself.

Zero is fully transparent. **Palette** gives each value a fixed swatch,
**Ramp** normalizes the non-zero values, and **Split** restricts the overlay
to the right half.

The web build cannot read the stencil buffer; the row refuses there (and on
a native context that granted no stencil planes). The commands themselves
work everywhere - only the visualization is native/OSMesa-only.

A few things the overlay will not tell you: it shows zero versus non-zero,
not write history; the palette repeats every 16 values; a program that omits
`GL_STENCIL_BUFFER_BIT` from its `glClear` accumulates stencil across frames
(the status line points that out). If Depth view is on at the same time,
depth wins wherever it paints.

### Call depth

**Call depth** colours your geometry by how deep in `funcN` calls it was
drawn. Depth 0 is the top level of `display()`; each nested call is one
deeper. A recursive scene renders as one pile of triangles and nothing on
screen says which came from the outer call and which from four frames down -
this says it.

The colour is a ramp: **azure at the top level, warming through teal and
gold to coral at the deepest call**, and it is stretched over the depth
range *your program actually reaches*. A tree that recurses three deep gets
four well-separated colours; one that recurses twenty deep gets a smooth
gradient. That is deliberate - depth is an ordering, so the key to it has to
be an ordering too, which a fixed per-depth palette would not be. It also
means the colours re-scale if your recursion gets deeper, because it did.

The legend in the scene's top-left lists each occupied depth with the number
of flat commands at it - so it answers "which nesting level is eating my
8192-command budget" as well as "which colour is which depth".

While the view is on, the tint replaces your `glColor*` and material
commands. Lighting is left alone: the geometry stays lit and shaded, so
shape still reads - only the hue changes. Wireframe modes draw through a
different path and are not tinted, and if Stencil view is also on it takes
the legend corner.

---

## The Config Menu

Nearly every toggle in this guide has a home here, whether or not it also has
a key. Open the **Config** dropdown (or press **Ctrl+Shift+K**). Items are
grouped into sections - hovering one opens a flyout of its items, and the
trailing **All** row shows the entire table at once (the mouse wheel scrolls
flyouts taller than the window):

- **RENDERING** - MSAA, Line smooth, Accum effect, Accum passes, Point attenuation,
  Post FX Scope, Post FX Effect
- **TIME & REPLAY** - Auto time, Replay, Replay mode, Replay expand
- **SCENE** - Grid, Grid major, Grid extent, Grid brightness, Axes, Backdrop, Light theme,
  Light indicators
- **CAMERA** - View mode, Projection, Camera rotate, Focus origin, Look down Z
  (swings the camera head-on down the Z axis: orbit angles and pan ease back to
  zero, the current zoom distance is kept), Reset camera
  (returns to the scene's authored `// camera` pose - from a built-in example
  or a loaded file - and to the built-in defaults only when the scene has no
  camera header)
- **GEOMETRY** - Wireframe, Winding, Depth view, Stencil view, Call depth,
  Auto-normals
- **OVERLAYS** - Overlay scope, Vertex labels, Vertex label placement, Vertex
  points, Vertex outlines, Vertex outline style, Normal vectors, Polygon
  highlight, Transform guides
- **INTERFACE** - Variable panel, Compute profile, Memory profile, Code panel,
  Wrap at commas, Syntax highlight, Paren match, Paren scope

**Left-click** a flyout item to cycle it forward, **right-click** to cycle
backward. Multi-state items show their current state name. Items shown without
a shortcut are available from the menu; examples include Point attenuation,
Post FX Effect, Auto-normals, and Vertex label placement.

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
| F8 | Overlay scope |
| F9 | Light theme |
| F10 | Post FX Scope |

---

## Scene Appearance

The stage your geometry stands on, and the quality settings that render it.
None of this is part of your program - it is not exported, and switching any
of it changes nothing about the commands in the code panel.

### Grid & axes

![Grid themes: Sketchbook, Radar, Adaptive Planes, Ocean](images/grid-themes.png)

Thirteen directly-selectable grid themes (**F2**): Off, Classic, Tron, Ember,
Ocean, XZ Ruler *(default)*, Adaptive Planes, Radar, Tilled Field, Sketchbook,
Neon Graph, Graph Planes, Checkerboard. Checkerboard is the odd one out: a
translucent solid floor lit by the scene's own lights rather than a line
drawing, with each cell labelled with its `x,z` coordinate - the labels fade
into the background with distance and stop entirely past a camera-relative
radius, so the far floor stays clean. Some backdrops enable hidden companion
grids; see
[Advanced Usage](ADVANCED_USAGE.md#cfg-backdropgrid-pairing).
**Grid major** (Ctrl+Shift+G) cycles the major-tick spacing (1/2/5/10),
**Grid extent** (F3) the grid's reach (Close / Mid / Far), and **Grid
brightness** (F4) the line weight (Dim / Normal / Bright / Bold). Theme
changes cross-fade.

![Grid brightness against a bright cube - Dim, Normal (top), Bright, Bold (bottom)](images/grid-brightness.png)

Reach for **Bold** (or Bright) when the grid crosses pale geometry - those
two levels give every line a dark contrast casing. Dim all but drops the
lines there.

Nine axes themes (**F6**): Off *(default)*, Classic, Pulse, Neon, Compass,
Gizmo, Ruler, Arrow, Fountain.

![Compass axes](images/axes-compass.png)

### Backdrops

The primordial cube enjoying the view:
![Backdrops: Polar Day+Snow, Nebula, Sunset, Aurora](images/backdrops.png)

**F5** cycles the scene backdrop: Off *(default)*, Cityscape, Stars,
City+Stars, Sunset, Aurora, Nebula, Polar Day, Polar Day+Snow.

Some backdrops enable a hidden companion grid. Nebula selects Star Chart;
see [Advanced Usage](ADVANCED_USAGE.md#cfg-backdropgrid-pairing) for the
`@cfg` details.

### Lighting

![The light rig on a teapot: indicators at each light, L1 haloed because the cursor is on its glEnable line](images/light-theme-studio.png)

**Light themes** (**F9**) are preset light rigs: Default (three colored
keys), Headlight (light 0 rides the camera), Solar (light 0 at the world
origin - for orbit/planet scenes), Studio (warm key / cool rim / warm fill),
Neon (saturated magenta/cyan/lime triad).

> [!NOTE]
> A theme only positions and colors
> the four light slots - your program still chooses which ones are on via
> `glEnable(GL_LIGHT0..3)`.

**Themes are the in-app control for light positions and colors.** The REPL
command set does not include `glLightfv`, so you choose a preset rig and use
`glEnable` / `glDisable` to select its slots rather than editing each light.
See [Limitations](#limitations).

**Light indicators** (Ctrl+L) draw a marker at each light's position
(labelled `L0..L3`, with *off* noted for disabled lights). Park the cursor on
a light's `glEnable`/`glDisable(GL_LIGHTn)` line and that light's indicator
is wrapped in a soft halo of its own color.

**Reading the numbers.** Turn code focus off (**Ctrl+Shift+F**) to show the
generated C around your program. Each light's colors appear in `init()`;
positions are re-issued in `display()` every frame. Switching themes updates
both blocks. The exported program carries the same lines.

![The generated light-position block in display(), and the per-light color block in init()](images/light-theme-inspect.png)

### Rendering quality

- **MSAA** (Ctrl+U) - hardware multisampling on/off.
- **Line smooth** (Ctrl+Shift+L) - GL line antialiasing.
- **Accum effect** (Ctrl+Shift+U) + **Accum passes** (Ctrl+= / Ctrl+-) - the
  accumulation buffer drives antialiasing and motion blur:
  - **AA** *(default, 1 pass)* - jitters the camera frustum per pass.
  - **Blur** - re-renders the scene per pass across the frame's
    animation-time window, so spinning geometry smears realistically.
    Scenes that don't use `t` fall back to AA jitter.
  - **Blur Cam** - blurs camera motion only; falls back to AA when still.
  - Passes: 1/2/4/6/8/10/12/14/16. The status bar shows the active mode
    (`AA 1x`, `Blur 16x`). Blur is expensive - every pass is a full scene
    render. `--no-accum` disables the accumulation buffer entirely;
    `--accum` forces it on where it would otherwise auto-disable (Mesa
    emulates the accumulation buffer on the CPU).
  - The web build also resolves and copies the antialiased canvas into its
    accumulation FBO once per pass. That is genuine GPU bandwidth work even
    when the CPU-side Render 3D row stays small; it can appear as Browser Wait
    because the browser delays the next animation callback while draining it.

  ![Motion blur on a spinning cube (Blur 16x)](images/motion-blur.png)

- **Point attenuation** - distance-attenuated point sprites
  (`glPointParameterfv`); used by the glow/particle examples. On hardware
  without the entry point the REPL falls back to a distance-based
  `glPointSize` approximation.

  ![The point-size and attenuation setup rows above the sprite cloud they draw](images/glow-sprites.png)

- **Post FX Scope** (F10 forward / Shift+F10 backward) - where the selected
  effect applies: Off / 3D View / Frame.
- **Post FX Effect** - the selected operation: Chromatic aberration, Vignette,
  Scanlines, or Film grain.

---

## Replay

![Replay stepping through a scene](images/replay.gif)

Replay executes your program one command at a time so you can watch the
scene assemble - geometry appears incrementally, older geometry fades in a
ghosted pass, and the code panel highlights each line as it runs, with
loop-variable values substituted into the displayed text.

| Key | Action |
|---|---|
| **Ctrl+R** (or the Replay button) | Start / stop replay |
| **Space** | Pause / resume |
| **+** / **-** | Faster / slower |
| **Left** / **Right** | Step backward / forward (while paused) |
| **Ctrl+K** | Jump the replay to the cursor line (first geometry at/after it) |
| **m** / **M** | Toggle replay mode: Polygon / Vertex granularity |
| **e** / **E** | Cycle Replay expand: Off / Expanded / Verbose |
| **n** / **N** | Cycle replay normals: off / vector / vector + direction |
| **v** / **V** | Toggle the replay focused-vertex label |
| **Esc** | Stop replay |

The HUD at the bottom of the viewport shows play state, position, and speed.
When a replay has finished, **Space** restarts it from the beginning.

An open [assignment value plot](#plotting-an-assignments-values) follows the
replay: it marks how far each plotted row has run this frame and prints the
value at that point.

Two related config items: **Replay mode** (Polygon steps a primitive at a
time, Vertex steps a vertex at a time) and **Replay expand**. **Expanded**
annotates lines in place as a `//` comment. Assignment readouts follow the
whole live call chain, not just the innermost function: standing on a draw
inside `func4`, the `a = x * 0.9;` row of the `func3` invocation that called
it still shows that invocation's own values. A call that has already
*returned* is not on the chain and stays unannotated, so a readout is never
left over from finished work. **Verbose** is the only mode
that splits a source row, adding a call-path breadcrumb (how the focused
vertex was reached: loop iterators and each `funcN` invocation's arguments)
then substituted and evaluated rows beneath it. The breadcrumb is vertex
replay only.

---

## Scenes & Workspaces

gl-repl keeps up to 8 scenes in memory, shown as tabs below the menu bar.

- **Examples first** - a fresh launch opens on the default example. A user
  scene is created when you choose File → New Scene, load a scene file or
  workspace, or edit an example.
- **Auto-promotion** - editing a built-in example, or saving it into a managed
  workspace, forks it into a fresh scene slot named after the example.
  Subsequent edits accumulate there. Promotion needs a free scene slot: with
  all eight occupied your edit still applies to the working copy, but it has
  nowhere to be parked, so the status line says *All user scene slots full*
  and the document stays unparked until you delete a scene - the next edit
  retries and carries along everything typed in the meantime.
- **Rename** - File → Rename Scene opens an inline prompt in the status bar
  (Enter commits, Esc cancels). In a managed workspace this changes the tab
  name while retaining the scene's stable persisted filename. The action is
  disabled while an example or tutorial is active.
- **Switching** - click a scene tab, or cycle with F12 / Shift+F12.
- **Capacity** - all eight tabs are explicit. New/load/promotion operations
  fail without mutation when every slot is occupied; gl-repl never evicts a
  tab implicitly.

### Saving and loading

| Menu | Action |
|---|---|
| File → New Scene | Start a fresh scene |
| File → Save Scene (Ctrl+S) | Save the active scene in the format it was loaded from; a visible example is promoted into the bound workspace first |
| File → Save Scene as .c | Always export the active scene as a standalone C program |
| File → Save Scene as .glr | Write the active scene in the built-in-example authoring format |
| File → Load Scene | Load a `.c` or `.glr` file into a new scene slot |
| File → New Workspace… | Create and open a named managed workspace; scenes from a session with no workspace bound come along into it, while a workspace created from an already-bound one starts empty |
| File → Save Workspace | Save every open tab at once, the visible example included |
| File → Save Workspace As… | Save the whole set of tabs into a new named workspace |
| File → Open Workspace → *name* | Switch to a managed workspace |
| File → Open Workspace → Other folder… | Open a managed workspace outside the normal workspace root |
| File → Reveal Workspace Folder | Reveal the bound managed workspace; disabled while no workspace is loaded |
| File → Delete Workspace Scene | Confirm and remove the active managed scene from its workspace; disabled for examples, tutorials, and unbound scenes |

Which workspace those rows act on is named in three places: the chip leading
the scene tab strip (`Demo > [scene][scene]`), the **WORKSPACE:** header
inside the File menu, and the window title (`gl-repl - Workspace:
<workspace> | <scene>`, or `gl-repl - <scene>` when nothing is bound).

A workspace is a directory holding your scene files plus a `.glr-workspace`
manifest that lists them in tab order. gl-repl opens exactly the files the
manifest names, in that order, and leaves any other `.c` file in the folder
alone. A directory without a manifest is declined; use **Load Scene** for a
loose file.

Opening a workspace is all-or-nothing: every scene is read and checked before
anything replaces what you have. Switching to a different workspace saves the
one you are leaving first - and if the tabs you are leaving were never bound
to a workspace at all, they are copied somewhere safe rather than dropped.

Where a save lands depends on how you launched. Run gl-repl from a directory
you can write to and it keeps using that directory. The packaged macOS app
saves into your gl-repl data folder instead, and it will ask you to name a
scene the first time.

---

## Exporting & Importing

### Standalone C export

**File → Save Scene as .c** writes the active scene as a complete, compilable
GLUT/OpenGL C program. Where the file lands depends on whether you have a
workspace open: with one bound it becomes a scene file inside that workspace,
and without one it is the scene-named `.c` file next to the binary (or
`output.c` for an unnamed scene). This explicit action is useful after loading
a `.glr`: **Save Scene** preserves that file's authoring format, while **Save
Scene as .c** produces the standalone program.

**Ctrl+S** (File → Save Scene) saves the active scene in the format it was
loaded from. For ordinary workspace scenes and built-in examples, that is the
standalone C export described above. If what you are looking at is still a
built-in example, saving forks it into a scene of your own first.

Header comments carry the REPL state (variables, config, camera), your
functions become C functions, and your commands become the `display()` body.
The generated file is **C89-compliant**:

```bash
cc -std=c89 -Wall -o scene output.c -lglut -lGL -lGLU -lm     # Linux
cc -std=c89 -Wall -o scene output.c -framework OpenGL -framework GLUT -lm  # macOS
```

Everything round-trips: `./gl-repl output.c` reloads the file with
variables, settings, camera, scene name, and `// @tune` tags intact.

The same export is available without opening the app at all:

```bash
./gl-repl --example "Parametric torus (nested for)" --export-c torus.c
./gl-repl saved.c --export-c rewritten.c        # load a session, re-export it
```

`--export-c` loads the session, writes the file, and exits. It never creates a
window or touches GL, so it works over ssh and in CI; `--window WxH` sets the
window size the exported program opens with.

`Ctrl+Q` quits and saves a recovery copy to a temp file - an unedited built-in
example is skipped instead (any unsaved scenes go to `recovery-workspace/`).

The exported program is your scene with the interpreter removed. No code
panel, no grid, no menu bar, and **no flat-command budget**: your `for` loops
stay loops instead of being unrolled into the 8192-command flat program. That
is why a scene that is pressed against the ceiling in the REPL can grow
dramatically once compiled - see [Performance](#performance).

### Editing exported code & reimporting

The exported file is meant to be worked on. You can extend it by hand in C
and load it back - on reload, the commands between the `// Snippet start` /
`// Snippet end` markers are imported. Two rules keep the round trip clean:

- **Stay inside the REPL language** for the snippet section. Lines the
  importer doesn't recognize are skipped with a warning. (Anything goes
  *outside* the snippet markers, but those edits live only in the C file.)
- **Stay within the command budget** - the source document holds 1024
  lines and its flattened program 8192 commands (the status bar shows
  flat usage, e.g. `1345/8192 cmds`). Loops count once as source but
  every unrolled iteration lands in the flat program.

  To see *where* the budget goes, watch the last left-aligned status-bar
  segment as you move the cursor: `fn cmds 2480` on or inside a function
  definition, `scope cmds 230` inside a `for`/`if` block, `call cmds 96` on
  a call line, `line cmds 12` on a plain line that expands more than once.
  For an offline breakdown,
  `./gl-repl --example "bubble sort (scratch arrays)" --flat-histogram`
  prints per-function and per-line costs sorted by spend.

If you need a GL call the REPL does not yet support, see
[*Adding A New Command*](ARCHITECTURE.md#adding-a-new-command).

### Scene export (`.glr`)

**File → Save Scene as .glr** writes the same file shape the built-in
examples ship in - no C scaffold. Reach for it when you are **authoring or
modifying an example**. `--export-glr <path>` writes the same file from the
command line, without opening a window. Phase order, the `@cfg` subset, and
catalog authoring live in
[ADVANCED_USAGE.md](ADVANCED_USAGE.md#authoring-an-example-catalog). For a
file that carries *everything* back, use Save Scene.

### Mesh export (PLY)

**File → Export .ply** captures the current scene as an ASCII PLY mesh
named after the active scene - `glVertex` polygons, GLU-tessellated shapes,
and GLUT solids all export through one GL feedback pass. Line primitives
export as PLY edges; point primitives export as loose vertices.

Scripted capture (the normal build needs a display; for a machine with no
window, use the [OSMesa build](ADVANCED_USAGE.md#headless-rendering-osmesa)):

```bash
./gl-repl --example "2d assignment sketch (vars only)" --export-ply out.ply
```

`--export-ply-srgb` decodes colors sRGB→linear for color-managed viewers.
Feedback-pass and color-management detail:
[ADVANCED_USAGE.md](ADVANCED_USAGE.md#mesh-export-ply).

---

## Tunable Variables (`// @tune`)

Use `// @tune` on a `float` declaration when a scene parameter should become
a keyboard-adjustable knob in the exported standalone C program.

```c
float amp = 1.5; // @tune
float freq = 2;  // @tune
```

The tag is a bare trailing comment token. If a declaration line contains
multiple names, every name on that line is tagged. A knob needs a
variable-panel row, so the tag requires a program-wide declaration. Inside a
function body, write `static float amp = 1; // @tune`.

Tagged variables are still normal REPL variables while you are authoring. In
the variable panel, tagged rows get an accent mark so you can see which
values will export as knobs.

### Exported controls

When you save/export C, each tagged variable becomes a keyboard knob in the
standalone program. The generated program also draws a small HUD listing each
knob, its current value, and its keys.

![The exported grass program's knob HUD: bladeCount as exported, and after holding q](images/export-c-knobs.png)

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
the export keeps the first 9 and writes a note in the generated C.

### Step size

The exported knobs use the same step size as the in-app numeric stepper:

| Current value magnitude | Step |
|---:|---:|
| `< 10` | `0.05` |
| `10..99.999` | `0.5` |
| `100..999.999` | `5` |

The pattern continues by decade. The same modifier keys apply in the exported
program: Shift is fine (`×0.2`), Ctrl is coarse (`×10`), Shift+Ctrl is `×2`.
There is no range clamp.

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

`// @tune` survives export/import round trips. The generated keyboard
controls live only in the standalone exported C. Inside gl-repl, adjust
values through the variable panel or inline numeric steppers.

---

## Performance

The REPL is an interpreter. Every frame it re-evaluates expressions, and
while anything is animating it re-flattens the whole program - loops
unrolled, functions inlined, every argument expression parsed and evaluated
again. That is what makes the live experience possible, but it costs real
CPU. Scenes built on the [stateless patterns](#working-without-state) add
their own recompute cost on top.

The exported C program has none of that machinery - it is the same scene as
plain compiled GL calls, roughly **100× lighter on the CPU**. The workflow
for pushing limits is:

1. Sketch and tune the scene in the REPL until it looks right.
2. Tag the parameters you still want to play with as `// @tune`.
3. Export, compile, and push the numbers in the standalone program.

The *Swaying grass field* example is the clearest case. Each blade costs 60
flat commands, so its 135 blades flatten to **8113 of 8192** - set
`bladeCount` to 137 and the program exceeds the budget and stops rendering.
Exported, `bladeCount` is nothing but a loop bound:

![The exported grass program at 9600 blades, with its generated @tune HUD in the corner](images/export-c-grass.png)

That is the same scene, exported and compiled with nothing changed but two
numbers - 9600 blades (71× the example's count) over a wider field. Those
extra blades cost about 2.4 ms of draw time per frame, well inside a 60 fps
budget, because a compiled loop has no per-frame flatten to pay for.

The REPL is a launchpad, debugging aid, and educational environment. The
export is the product.

---

## Limitations

gl-repl focuses on fixed-function, immediate-mode geometry. Intentionally
outside that scope:

- **Geometry is edited through code.** Viewport input moves only the camera.
- **Textures are not supported.** No `glTexCoord`, texture binding, or image
  loading.
- **Shaders and buffered drawing are not supported.** Immediate-mode
  `glBegin`/`glEnd` keeps individual source lines visible while editing and
  replaying.
- **Individual lights use presets.** A light theme supplies the four slots'
  positions and colors; scene code enables or disables the slots it needs.
  The generated and exported C still exposes the underlying `glLightfv`
  calls; see [Lighting](#lighting).

These points describe the current release, not commitments about future
versions.

---

## Profiling

When a scene starts feeling heavy, the built-in profilers show where the
frame goes before you reach for the export.

### The compute profile (Ctrl+G)

**Ctrl+G** cycles the compute profile through Off / FPS / Sections /
Histogram. *FPS* shows only the frame-rate graph. *Sections* adds the full
collapsible per-section timing tree. *Histogram* adds the section-distribution
graph.

![Histogram mode: the section listing (CPU/GPU/Max), the log-log section histograms, and the FPS plot](images/profile-panels.png)

Three floating panels work together:

- **The section listing** (right) breaks the display callback into named
  sections. *Frame Time* is the whole callback; it splits into *Frame Work*
  (producing the image) and *Present* (the callback tail containing finish and
  swap). *Frame Wait* on native builds, or *Browser Wait* on the web build,
  accounts for the gap from that callback's end to the next callback's start.
  Previous Frame Time + following Wait is therefore the interval represented
  by the FPS graph. Browser Wait includes animation-frame scheduling and any
  queued WebGL/GPU back-pressure the browser resolves between callbacks; it is
  not evidence of a CPU stall inside a particular GL call. **Frame Work is the
  number to watch for callback cost; use Wait plus the FPS graph for pacing.**
  The **CPU** column is a running average of wall-clock time; the **GPU** column
  comes from asynchronous GL timer queries and reads `--` where the driver
  lacks them. **Max** shows the worse of the two averages.
- **The section histograms** (center) overlay every top-level section's
  timing distribution on one log-log graph. Click a legend entry to hide that
  series; rest the pointer on one to see its sample count, fastest, mean,
  slowest, and standard deviation.
- **The FPS plot** (bottom-right) graphs frame rate over the last 10
  seconds, minute, and 10 minutes.

Histogram mode is the tool for *hitches*: a scene that averages 60 fps but
stutters once a second shows a clean main hump plus a second bump at the
stall duration. The histograms accumulate from load (both reset when you
switch examples, or on the panel's `[reset]` control).

GPU timing needs timer-query support (GL 3.3 / `ARB_timer_query`, or the
`EXT_timer_query` fallback); `GLR_NO_GPU_PROF=1` disables it explicitly.
Many WebGL drivers expose no compatible timer query, so Browser Wait is the
cadence cross-check when that column reads `--`.

### Memory, messages, and startup

- **Memory profile** (Ctrl+Shift+B) - a floating panel with the process RSS
  history, a session baseline, and the delta since baseline.
- **Message history** - click the button at the right end of the bottom
  message line to review recent status messages.
- **Ctrl+Shift+N** - dump debug state to stdout.
- Startup prints an init trace (`[init +N.NNNs] <phase>`) to stderr;
  `--detailed-prof` adds finer phases.

---

## Music

gl-repl plays background `.mp3`s found at startup, in filename order.

| Key | Action |
|---|---|
| Ctrl+Shift+A | Play / pause |
| Ctrl+Left / Ctrl+Right | Previous / next track |

`--no-audio` skips audio entirely. The play/pause state persists across
runs. Search paths (`./assets`, the app bundle, the per-user music folder)
and the optional music pack are in
[ADVANCED_USAGE.md](ADVANCED_USAGE.md#music--assets).

---

## Command-Line Options

This is the common, day-to-day set. The complete flag reference, including
`--examples-dir`, `--lint-scenes`, `--dump-flat`, `--call-tree`, and every `GLR_*`
environment variable, is in
[`ADVANCED_USAGE.md`](ADVANCED_USAGE.md#options).

```
./gl-repl [file.c | workspace-dir | -]   load a file, directory, or stdin

--example <name|idx>   start on a built-in example (case-insensitive name or 1-based index)
--list-examples        print the built-in examples and exit
--tutorial <name|idx>  start an interactive tutorial (case-insensitive name or 1-based index)
--list-tutorials       print the built-in tutorials and exit
--tour <name|idx>      start a guided tour (case-insensitive name or 1-based index)
--tour-stop <id>       with --tour, fast-forward to # @checkpoint <id> and pause
--list-tours           print the built-in tours and exit
--list-config           print config labels and stable @cfg slugs and exit
--time <secs>          initial animation time t (also GLR_TIME; --time wins)
--window <WxH>         initial window size (default 1200x800)
--export-c <path>      write the session out as standalone C, then exit (no window)
--export-glr <path>    write it in the .glr authoring format instead (no window)
--export-ply <path>    capture frame 1 geometry to PLY, then exit (needs a display)
--export-ply-srgb      decode vertex colors sRGB -> linear during PLY export
--assets <dir>         scan this dir for *.mp3 (also GLR_ASSETS_DIR)
--no-audio             skip audio init entirely
--no-accum             disable the accumulation buffer (AA + motion blur)
--accum                force it on (default: off on software-accum renderers)
--dump-code            print the loaded buffer to stdout
--dump-flat            print the flattened command list (includes frame=N)
--flat-histogram       print per-function / per-line flat-command costs
                       (where the 8192 budget goes; works with --example)
--call-tree            print the interned per-invocation call tree
--detailed-prof        verbose startup timing trace (also GLR_DETAILED_PROF=1)
-h, --help             usage
```

`GLR_TIME` and `GLR_ASSETS_DIR` set the same values as `--time` and
`--assets`; `GLR_NO_GPU_PROF=1` turns off GPU timer-query profiling.

A second family of `GLR_*` variables exists to pose the app for scripted
screenshots and recordings. Those, and headless (no-window) rendering, are
in [`ADVANCED_USAGE.md`](ADVANCED_USAGE.md#environment-variables).

---

<a id="keyboard-mouse"></a>
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
| Enter | Commit the current line and move to a new one |
| Backspace | Delete character or selected lines |
| Tab | Autocomplete (Tab/Enter accepts) |
| Up / Down | Navigate lines |
| Ctrl+Up / Ctrl+Down | Previous / next function. Inside a function: Up jumps to the declaration, Down to the closing `}`. Still jumps functions during replay and with help open (plain Up/Down keep changing replay speed / scrolling help) |
| Left / Right | Move cursor within line |
| Home / Ctrl+A | Start of line |
| End / Ctrl+E | End of line |
| Shift+Arrows / Home / End | Extend selection |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut / paste |
| Ctrl+Z / Ctrl+Shift+Z (Ctrl+Y alternate) | Undo / redo |
| Ctrl+D | Delete line or selection |
| Ctrl+F | Search (seeded from highlighted text) |
| Tab (find bar) | Cycle find field / replace field / whole-word chip |
| Enter (replace field) | Replace all matches |
| Ctrl+\ | Reformat buffer |
| Ctrl+/ | Toggle comment on the selection / block / line |
| Ctrl+Shift+F | Toggle code focus |
| Ctrl+B | Cycle code panel layout |
| Ctrl+Shift+H | Cycle syntax highlight |
| Ctrl+click / Cmd+click a `funcN` name | Go to that function's definition - from a call, an expression, or a mention in a comment |
| Right-click GL command | Show a short description of that command |
| Right-click `var = expr;` | Toggle the [assignment value plot](#plotting-an-assignments-values) for that row |
| Shift+right-click `var = expr;` | Add (or remove) that row as an extra series on the open plot |
| Right-click empty line | Toggle the [OpenGL state inspector](#inspecting-opengl-state) at that point |
| Shift+right-click empty line | Pin (or unpin) that row as the inspector's comparison basis |
| Esc | Clear input / close overlay |

### Scene & rendering

| Key | Action |
|---|---|
| Ctrl+T | Play/pause time |
| Ctrl+Shift+T | Reset time to 0 |
| Ctrl+Shift+W | Winding |
| Ctrl+R | Start/stop replay |
| Space / + / - / arrows | Pause, speed, step replay (see [Replay](#replay)) |
| m / e / n / v | Replay mode / expand / normals / focused-vertex label |
| Ctrl+K | Jump replay to the cursor line |
| Ctrl+W | Wireframe |
| Ctrl+Shift+D | Depth view (Off / Linear / Scene / Split) |
| Ctrl+Shift+S | Stencil view (Off / Palette / Ramp / Split) |
| Ctrl+Shift+J | Call depth tint |
| Ctrl+L | Light indicators |
| Ctrl+Shift+L | Line smooth |
| Ctrl+U | MSAA |
| Ctrl+Shift+U | Accum effect |
| Ctrl+Shift+G | Grid major spacing |
| Ctrl+= / Ctrl+- | Accum passes up/down |
| Ctrl+Shift+X | Transform guides |
| Ctrl+N | Normal vectors |
| Ctrl+Shift+O | Vertex outlines |
| Ctrl+Shift+P | Vertex points |
| Ctrl+P | Polygon highlight (Off / On / Clipped & culled) |
| Ctrl+Shift+K | Open Config menu |
| Ctrl+G / Ctrl+Shift+B | Compute / memory profile panel |
| F2-F10 | Config cycles (Shift steps backward) - see [The Config menu](#the-config-menu) |
| F11 / Shift+F11 | Next / previous tutorial |
| F12 / Shift+F12 | Next / previous example or scene |
| F1 | Help overlay |
| Backquote (`` ` ``) | Variable panel |

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
| Ctrl+Shift+I | Look down Z |

### Tours (while a Tours-menu tour runs)

| Key | Action |
|---|---|
| Space | Play / pause (restart from the end) |
| Left / Right | Step back / forward |
| + / - | Change tour speed |
| Esc | Exit tour |

### Session & audio

| Key | Action |
|---|---|
| Ctrl+S | Save scene |
| Ctrl+Q | Quit (saves recovery file) |
| Ctrl+Shift+N | Debug state dump |
| Ctrl+Shift+A | Play / pause |
| Ctrl+Left / Ctrl+Right | Previous / next track |

> **macOS note:** Cmd+letter works the same as Ctrl+letter. F11 may be
> claimed by the system's "Show Desktop" - use the Tutorials menu instead.
