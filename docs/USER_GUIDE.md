# gl-repl User Guide (Draft)

gl-repl is an interactive OpenGL interpreter. You type classic immediate-mode
GL commands and see geometry render live as you type, with interactive overlays
guiding each edit. Every command stays in an editable code panel, so a scene is
a readable list of GL calls you can revisit, tweak, animate, replay
step-by-step, and export as a standalone C program.

![gl-repl rendering the Whale example](images/hero.png)

**The code is the interface.** By design, you shape the scene by editing
commands - there is no click-to-select, drag-a-vertex, or gizmo-in-the-scene
editing. Mouse input inside the 3D viewport moves the *camera* and nothing
else (orbit, pan, zoom), so you can never nudge your geometry by accident
while looking around, and the code panel always says exactly what the scene
is. Everything else you can point at - menus, the code panel, the variable
panel sliders, the status bar - lives in the 2D chrome around the viewport,
and the edits it makes show up as text in your program.

This guide follows the shape of a session: you [start the app](#getting-started),
get your bearings from the [built-in examples](#built-in-examples) and guided
[tutorials](#tutorials), then [write some code](#writing-code) in
[the REPL language](#the-repl-language),
[make it move](#making-it-move), lean on the
[visual feedback](#seeing-what-youre-doing) and
[diagnostic views](#diagnostic-views) to understand and debug what you built,
and finally [keep and ship](#scenes--workspaces) the
result. Reference material - the full [CLI](#command-line-options) and
[keyboard](#keyboard--mouse-reference) listings - sits at the end.

This guide describes what gl-repl *is*. For the OpenGL techniques you can
build with it - clip planes, stencil masks, decals, attribute scoping - and
worked explanations of the math functions, see
[`TUTORIAL.md`](TUTORIAL.md). For headless rendering, recording, and every
environment variable, see [`ADVANCED_USAGE.md`](ADVANCED_USAGE.md); for project
internals, [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Contents

- [Getting Started](#getting-started)
- [Built-in Examples](#built-in-examples)
- [Tutorials](#tutorials)
- [Guided Tours](#guided-tours)
- [Writing Code](#writing-code)
- [The REPL Language](#the-repl-language)
- [Making It Move](#making-it-move)
- [Camera & Views](#camera--views)
- [Seeing What You're Doing](#seeing-what-youre-doing)
- [Diagnostic Views](#diagnostic-views)
- [The Config Menu](#the-config-menu)
- [Scene Appearance](#scene-appearance)
- [Replay](#replay)
- [Scenes & Workspaces](#scenes--workspaces)
- [Exporting & Importing](#exporting--importing)
- [Tunable Variables (`// @tune`)](#tunable-variables--tune)
- [Performance & Scope](#performance--scope)
- [Fidelity to OpenGL](#fidelity-to-opengl)
- [Scope & Current Limitations](#scope--current-limitations)
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
  the panel, a scrollbar appears along its right edge. The mouse wheel scrolls
  as usual. Dragging the thumb moves through a long document in one gesture,
  and clicking anywhere on the track jumps the thumb there and keeps dragging
  from that point.
- **Status bar** - command count, current line, and the accumulation indicator
  (`AA 1x` / `Blur 16x`). The shot above is under motion blur, which is why the
  torus smears. The bar also carries clickable controls: undo/redo, copy/cut,
  a reset to the five display defaults, *focus*
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
  history.

For a guided flythrough of the menus without leaving the app, run the *Menus &
Examples* entry from the [Tours](#guided-tours) menu.

### Your first triangle

Fresh launches open on the default built-in example. Choose **File → New
Scene** first so the triangle has a clean scene of its own, then type each
line and press `;` or Enter to commit it:

```c
glColor3f(0.98, 0.76, 0.36)
glBegin(GL_TRIANGLES)
glVertex3f(0, 1, 0)
glVertex3f(-1, -1, 0)
glVertex3f(1, -1, 0)
glEnd()
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
  supported command, and its *Keys* tab is the full keyboard reference. Esc
  or a click outside dismisses it.

---

## Built-in Examples

**F12** cycles forward through the 40 built-in examples, then any saved
scenes, wrapping to the start; **Shift+F12** cycles backward. The Scene menu
lists them grouped by tag. `./gl-repl --list-examples` prints the compiled-in
set.

The **&lt; Prev** / **Next &gt;** buttons in the menu bar (left of the 2D/3D
swatch) do the same stepping with the mouse - and while a tutorial is
running they step the *lesson* catalog instead, matching F11 / Shift+F11.
Hovering either one names where it lands - `Next Example | Torus knot
(animated) | F12` - so you can see what is one click away before taking it.
They hide themselves when the code panel is too narrow to fit them
alongside the menus and the search field.

```
 1  gl-repl logo                                        21  Torus knot (animated)
 2  Rotating cube                                       22  GLU concave arrow
 3  Animated ring (for + t)                             23  GLU concave arrow cutout
 4  Conditional colors (if + t)                         24  GLU concave arrow extrusion
 5  Transform stress (translate/rotate/scale guides)    25  Glow sprites (blend + point attenuation)
 6  Function demo (named func)                          26  Snowfall particles
 7  Function polygons (args + for)                      27  Swaying grass field (rand + t)
 8  Function branching (args + if)                      28  Clip planes carve solids (glClipPlane)
 9  2D assignment sketch (vars only)                    29  Fog ring tunnel (glFog)
10  Animated spirograph curve                           30  Stencil mask window (glStencilOp)
11  Traveling ripple ring                               31  Planar shadows (glMultMatrixf)
12  Bezier curve with guides                            32  Jellyfish (glDepthMask translucency)
13  Annotated orbit plot (labels)                       33  Lantern festival (additive glow + reflections)
14  Bubble sort (scratch arrays)                        34  Whale (particle system + lit model)
15  Sierpinski carpet (2D recursion)                    35  Teapot carousel (transform stacks + glow points)
16  Sierpinski sponge (3D recursion)                    36  Ringed planet (nebula skies)
17  Recursive 3D tree (func + recursion)                37  Aurora observatory (dish tracks the sky)
18  Parametric torus (nested for)                       38  Orrery (labels track 3D orbits)
19  Animated wave surface                               39  Dusk lighthouse atoll (stress test)
20  Animated wave surface (analytic normals)            40  Pulse bars (easing)
```

Examples may carry their own presentation presets (grid theme, backdrop,
camera, 2D mode...). Loading an example resets the scene-presentation
settings to defaults first, then applies the example's presets - so examples
always look as authored, and your camera carries over unless the example
sets its own.

Changing an example's document - typing, deleting, pasting, or any other edit
that would go on the undo stack - automatically promotes it to a user scene
(see [Scenes & Workspaces](#scenes--workspaces)), so you never modify the
built-ins themselves. Moving the camera or flipping a config toggle is not a
document change and does not promote. Promotion needs a free scene slot: with
all eight occupied your edit still applies to the working copy, but it has
nowhere to be parked, so the status line says *All user scene slots full* and
the document stays unparked until you delete a scene - the next edit retries
and carries along everything typed in the meantime.

---

## Tutorials

The **Tutorials** menu offers guided, step-by-step lessons rendered directly
in the code panel - instruction comments appear with a typewriter reveal, and
each step either asks you to type a command (autocomplete ghost text shows
the expected call), acknowledge a short note, change a setting, or drag a
variable slider to a target.

- Tutorials are grouped by tag (hover a tag row for its flyout); flyouts
  group entries under difficulty subheadings.
- While a tutorial is active, the menu gains **Restart Tutorial** and
  **Exit Tutorial** entries.
- Instruction lines are locked - the tutorial guards them against edits
  until you finish or exit.
- **Beginner:** *First Triangle*, *Color & Transform*, *Feature Tour*,
  *Variable Slider*, *First Animation*, *Points & Lines*, *GLUT Solids Tour*,
  and *First Loop*.
- **Intermediate:** *Depth Test Triangle*, *Lighting Basics*, *Color
  Interpolation*, *Line Stipple*, *Blending & Transparency*, *Depth Mask &
  Draw Order*, *Fog*, *Clip Planes*, *Materials & Shininess*, *Flat & Smooth
  Shading*, *Normals*, *Culling & Winding*, and *Bitmap Text*.
- **Advanced:** *Functions*, *If & Conditionals*, and *Scratch Arrays*.

---

## Guided Tours

Where a tutorial asks *you* to type, the **Tours** menu drives the app for
you: pick an entry and a synthetic pointer takes over - gliding through
menus, hovering flyouts, clicking, and typing, with an on-screen cursor,
click ripples, spotlight rings, and captions narrating each step. Press any
key or click anywhere to stop a tour and take back control (a finished tour
hands control back by itself).

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

- Press **`;`** or **Enter** to commit the current line. If it is valid,
  gl-repl applies it and moves to the next line. If not, the error is
  highlighted and the line stays active so you can fix it.
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
(**Ctrl+Z / Ctrl+Shift+Z**, with Ctrl+Y as an alternate) work like a normal editor. Copy and cut also put the text
on the **system clipboard**, so it pastes into any other app, and Ctrl+V takes
text copied from anywhere else - multi-line text arrives as source lines, a
single line lands in the input row when you are editing one. Right-click a GL
command for a short description, or right-click an assignment for [a plot of
its values](#plotting-an-assignments-values). **Ctrl+D** deletes the current
line or selection. The status-bar trash button resets the whole scene to the
[five editable display defaults](#display-default-commands) - it replaces the
document rather than emptying it.

**Ctrl+F** opens case-insensitive search over the buffer; **Up / Down** move
to the previous / next match. Press **Enter** for the next match, or **Esc**
to close it. If text is highlighted in the edit line when you press Ctrl+F,
that text seeds the find field and searching starts from it; with nothing
highlighted the previous query is kept.

### Replace

**Tab** in the find bar opens a replace row. Type a replacement and press
**Enter** for **Replace All**, or click *Replace* for a single match. The
**word** chip restricts both search and replace to whole identifiers - turn it
on before renaming something short. Replace All rewrites the entire document
in one transaction and is undoable with **Ctrl+Z**.

### Display default commands

A new user scene is not empty. **File → New Scene** starts one with these five
lines already committed, and the status-bar trash button resets the current scene to the same
five lines. A fresh app launch is different: it opens the default built-in
example, as described under [Scenes & Workspaces](#scenes--workspaces).

```c
glEnable(GL_COLOR_MATERIAL);
glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, (GLfloat[]){0.4, 0.4, 0.4, 1});
glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 30);
```

These defaults reduce boilerplate for a lit scene. They are ordinary editable
commands, so feel free to modify, remove, or replace them.

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
computed. The plot is a debugging aid rather than sample-accurate capture, and
costs nothing while closed.

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
**sd** use every captured value for the selected series - hover its legend entry,
or move away to select the first. Visual decimation affects only the curve, not
these statistics.

Everything is mouse-driven. A setting appears as its current bare value in a
sunken box; a bracketed word is a one-shot button.

| Control | Action |
|---|---|
| `once` / `1 Hz` / `frame` | Cycle capture rate; right-click cycles backward |
| `lin` / `log` | Switch Y between linear and logarithmic spacing |
| `1x` / `2x` | Toggle normal and double-size panels |
| `[reset]` | Clear samples and statistics, keeping the plotted rows and rate |
| `[x]` | Close the plot |

**once** freezes one comparable frame, **1 Hz** is the default live view, and
**frame** captures every frame. With several rows, **once** waits for a frame
in which they all execute; changing the rows or rate starts a new collection.

**During [replay](#replay)** a vertical rule marks how far the
replay has run through each plotted row's executions this frame, with a dot on
the trace at the value that execution produced and the number printed beside
it - one row per plotted assignment, in its own color, so stepping the replay
reads the numbers off directly instead of off the axis.

The rule has to sit over the values the replay is actually stepping through, so
while replay is active the plot captures every frame regardless of the rate -
the rate chip greys out to show it is not in force. **once** is the exception: it stays
frozen, and a frozen snapshot gets no rule, since the replay is not walking the
frame it holds. A top-level row plotted against successive captures gets no
rule either - there is no position within a frame to mark.

**Comparing several rows.** **Shift**+right-click adds an assignment to the open
plot instead of replacing what is there, up to four at once; Shift+right-click a
plotted row again to remove it.

![Three assignments from one loop body overlaid, with the legend naming them](images/assign-plot-series.png)

The legend identifies each color. X comes from the first row, so another row
must have the same executions per frame or it is refused; a row that skips a
later capture leaves a gap. Y spans every series - use `log` for widely different
magnitudes and `2x` for more drawing room.

Plain right-click retargets the plot to one row. Editing a plotted row keeps
tracking it; deleting it removes that series and closes the panel when none
remain.

**Opening the plot from the scene: `// @plot`.** A trailing `// @plot` comment
on an assignment row plots that row as soon as the document loads, so a saved
scene can arrive with its plot already open on the interesting values:

```c
static float wobble;
wobble = sin(t * 3) * 0.4;   // @plot
```

Tag several rows to overlay them, exactly as Shift+right-click would; the first
tagged row is the primary that fixes the X axis, rows past the four-series limit
are ignored, and a row whose executions per frame disagree with the primary is
skipped with a status message. The tag rides the row's text, so it survives
save, reload, reformat and export to C - and it is document state, not a
setting: it is re-read whenever a document is loaded, never in the middle of an
editing session, so it cannot undo a retarget you just made by hand. Loading a
document with no `// @plot` in it closes the plot.

### Inspecting OpenGL state

Right-click a visually blank row in the code panel to inspect OpenGL state at
that point in the program. The row may already be committed or may be the
current, still-uncommitted empty input row.

![OpenGL state inspector opened on a blank source row](images/gl-state-inspector.png)

The popup opens on the state **your program** wrote before that row. The
generated `init()` and `display()` setup writes far more state than a typical
scene does - often by a factor of ten - so those rows start folded behind the
**[+] N from setup** chip on the title row. Click it to expand them into the
listing; they draw in a muted tone so the two groups stay distinguishable. Explicit writes that
happen to match the initial OpenGL default are kept either way, so touched-ness
stays visible.

The current value is always visible; click the **[+] default/source** chip in
the column header to add the initial default and the source of the latest
write - `init()`, generated `display()`, or a user `display()` line. The line
number it quotes is the one in the code panel's left margin, so it stays
right whether or not code focus is hiding the derived C boilerplate (those
hidden rows are counted in the margin numbering).

#### Comparing two probe points

**Shift+right-click a second blank row** while the popup is open to pin that
row as the comparison basis: the default column becomes **value at L*n***, and
rows are coloured by whether they *changed between the two lines* instead of
how far they sit from the OpenGL default - the question that actually matters
when a block of drawing code misbehaves, and it quiets the report since
identical setup rows stop reading as differences. Shift+right-click the
pinned row again to unpin; closing the popup also clears it.

The basis is a live line, not a snapshot, so the comparison stays correct as
`t` advances. Untouched rows still compare against the OpenGL default. Pinning
a basis *after* the anchor row shows only the state both lines share.

A light's rows appear only while it can affect the frame (enabled, or touched
by your program) - otherwise four disabled lights alone would add twenty rows
of unreachable state.

`glRasterPos3f` contributes two rows - position and the
`GL_CURRENT_RASTER_COLOR` it latches from the current color, which is what
`label(...)` actually draws with and a later `glColor3f` can't change. Neither
row appears before the first `glRasterPos3f`. With lighting on, the raster
position is lit like a vertex, so the shown color is that lit result, not the
raw `glColor3f` above it. (Not shown: undefined values from a clipped raster
position, or the rare driver that computes this cell wrong.)

Modelview matrices use four aligned rows; light positions show in both world
and eye coordinates when available. Scroll with the mouse wheel; click
elsewhere or type in the editor to dismiss.

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
(Ctrl+Shift+Y; Off / On / On+Shadow), and **Paren match** / **Paren scope**
(highlight the matching bracket under the caret, and the span of the
enclosing parens).

**Syntax highlight** starts at On+Shadow, except on a Mesa renderer (most
Linux setups, and any software rasterizer) where it starts Off: drawing the
panel in many colors costs milliseconds a frame there, and the shadow mode
composites incorrectly. Only the starting point changes - Ctrl+Shift+Y still
cycles all three, and the choice is saved with the scene.

---

## The REPL Language

The language is immediate-mode OpenGL with just enough structure around it -
variables, loops, functions, conditionals - to build real scenes. Every
numeric argument everywhere is a full math expression.

### Supported GL commands

- [`glBegin(MODE)`](https://docs.gl/gl2/glBegin), [`glEnd()`](https://docs.gl/gl2/glEnd)
- [`glVertex3f(x,y,z)`](https://docs.gl/gl2/glVertex), [`glVertex2f(x,y)`](https://docs.gl/gl2/glVertex)
- [`glNormal3f(x,y,z)`](https://docs.gl/gl2/glNormal)
- [`glColor3f(r,g,b)`](https://docs.gl/gl2/glColor), [`glColor4f(r,g,b,a)`](https://docs.gl/gl2/glColor)
- [`glClearColor(r,g,b,a)`](https://docs.gl/gl2/glClearColor) - background clear color; channels
  clamp to >= 0.15 (prevents a fully black background from hiding geometry)
- [`glTranslatef(x,y,z)`](https://docs.gl/gl2/glTranslate), [`glScalef(sx,sy,sz)`](https://docs.gl/gl2/glScale),
  [`glRotatef(deg,x,y,z)`](https://docs.gl/gl2/glRotate)
- [`glPushMatrix()`](https://docs.gl/gl2/glPushMatrix), [`glPopMatrix()`](https://docs.gl/gl2/glPushMatrix),
  [`glLoadIdentity()`](https://docs.gl/gl2/glLoadIdentity)
- [`glMultMatrixf((GLfloat[]){m0, ..., m15})`](https://docs.gl/gl2/glMultMatrix) -
  post-multiply by a column-major 4x4, given inline or as a scratch array name
  (`glMultMatrixf(A)`); see [Arbitrary matrices](#arbitrary-matrices)
- [`glPolygonMode(face,mode)`](https://docs.gl/gl2/glPolygonMode) - `GL_FILL`,
  `GL_LINE`, or `GL_POINT` rasterization, per face
- [`glPolygonOffset(factor,units)`](https://docs.gl/gl2/glPolygonOffset) - depth
  nudge for coplanar passes; needs `glEnable(GL_POLYGON_OFFSET_FILL)`
- [`glPushAttrib(mask)`](https://docs.gl/gl2/glPushAttrib), [`glPopAttrib()`](https://docs.gl/gl2/glPushAttrib) -
  save/restore a group of GL state ([tutorial](TUTORIAL.md#scoping-state-with-glpushattrib);
  the editor draws the scope, see [Attribute scope](#attribute-scope)). `mask`
  is one or more of `GL_CURRENT_BIT`, `GL_POINT_BIT`, `GL_LINE_BIT`, `GL_POLYGON_BIT`,
  `GL_LIGHTING_BIT`, `GL_FOG_BIT`, `GL_DEPTH_BUFFER_BIT`,
  `GL_STENCIL_BUFFER_BIT`, `GL_TRANSFORM_BIT`,
  `GL_ENABLE_BIT`, `GL_COLOR_BUFFER_BIT`, OR'd with `|`, or
  `GL_ALL_ATTRIB_BITS` for all eleven supported groups. `GL_FOG_BIT` scopes the
  `glFog*` parameters; the `GL_FOG` and `GL_STENCIL_TEST`
  enable flags ride both their group bit and `GL_ENABLE_BIT`
- [`glEnable(CAP)`](https://docs.gl/gl2/glEnable), [`glDisable(CAP)`](https://docs.gl/gl2/glEnable)
  - CAP: `GL_DEPTH_TEST`, `GL_LIGHTING`, `GL_COLOR_MATERIAL`, `GL_NORMALIZE`,
    `GL_LINE_SMOOTH`, `GL_POINT_SMOOTH`, `GL_BLEND`, `GL_CULL_FACE`, `GL_FOG`,
    `GL_LINE_STIPPLE`, `GL_MULTISAMPLE`, `GL_STENCIL_TEST`,
    `GL_POLYGON_OFFSET_FILL`, `GL_POLYGON_OFFSET_LINE`, `GL_POLYGON_OFFSET_POINT`,
    `GL_LIGHT0..GL_LIGHT3`, `GL_CLIP_PLANE0..GL_CLIP_PLANE5`
- [`glShadeModel(MODE)`](https://docs.gl/gl2/glShadeModel)
- [`glPointSize(size)`](https://docs.gl/gl2/glPointSize), [`glLineWidth(width)`](https://docs.gl/gl2/glLineWidth)
- [`glLineStipple(factor, pattern)`](https://docs.gl/gl2/glLineStipple) - repeat
  a 16-bit line pattern while `GL_LINE_STIPPLE` is enabled. `pattern` is an
  expression, or a `0..65535` decimal or `0xNNNN` literal; a hex pattern is
  kept in hex (`0xAAAA` = dots, `0x00FF` = dashes), a decimal one stays
  decimal
- [`glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, const, linear, quadratic)`](https://docs.gl/gl2/glPointParameter)
- [`glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA | GL_ONE)`](https://docs.gl/gl2/glBlendFunc)
- [`glColorMaterial(face, mode)`](https://docs.gl/gl2/glColorMaterial)
- [`glMaterialfv(face, pname, (GLfloat[]){r, g, b, a})`](https://docs.gl/gl2/glMaterial),
  [`glMaterialf(face, GL_SHININESS, value)`](https://docs.gl/gl2/glMaterial)
- [`glLightModeli(pname, param)`](https://docs.gl/gl2/glLightModel),
  [`glFrontFace(mode)`](https://docs.gl/gl2/glFrontFace),
  [`glCullFace(mode)`](https://docs.gl/gl2/glCullFace) with mode `GL_FRONT`,
  `GL_BACK`, or `GL_FRONT_AND_BACK`
- [`glDepthFunc(func)`](https://docs.gl/gl2/glDepthFunc), [`glDepthMask(GL_TRUE|GL_FALSE)`](https://docs.gl/gl2/glDepthMask)
- [`glStencilFunc(func, ref, mask)`](https://docs.gl/gl2/glStencilFunc),
  [`glStencilOp(stencil-fail, depth-fail, depth-pass)`](https://docs.gl/gl2/glStencilOp),
  [`glStencilMask(mask)`](https://docs.gl/gl2/glStencilMask) - stencil-mask
  setup ([tutorial](TUTORIAL.md#stencil-masks)), scoped by
  `GL_STENCIL_BUFFER_BIT` on the attribute stack. `ref` and `mask` both take
  decimal or `0xNN` in `0..255`, but differ in kind: `ref` is a full
  expression, so it can animate, while `mask` must be a literal - a mask names
  bits, not a quantity to sweep. Fractional references truncate toward zero,
  and the two ways to leave `0..255` are handled differently on purpose: a
  **literal** out of range is rejected at commit, where you are right there to
  be told, while an **animated** one is clamped per frame, because a parse
  error that fires on frame 900 is not a usable failure mode. `glClearStencil`
  shares that policy
- [`glColorMask(r, g, b, a)`](https://docs.gl/gl2/glColorMask) - each channel GL_TRUE/GL_FALSE or 0/1
- [`glClear(mask)`](https://docs.gl/gl2/glClear) - **your program's own frame
  setup**: nothing clears the scene rectangle on its behalf, exactly as in the
  exported C, so deleting the line smears the frame. A second one part-way down
  a scene starts a later pass on a fresh buffer
  ([tutorial](TUTORIAL.md#clearing-mid-scene)). `mask` combines
  `GL_COLOR_BUFFER_BIT`, `GL_DEPTH_BUFFER_BIT`, and `GL_STENCIL_BUFFER_BIT`
  with `|`; the accumulation bit is not offered
- [`glClearDepth(depth)`](https://docs.gl/gl2/glClearDepth) - the depth value a
  `GL_DEPTH_BUFFER_BIT` clear writes. GL clamps it to 0..1 and defaults to 1
  (the far plane); a lower value makes the cleared buffer reject geometry
  further away than that depth. Scoped by `GL_DEPTH_BUFFER_BIT` on the
  attribute stack, alongside `glDepthFunc` / `glDepthMask`
- [`glClearStencil(value)`](https://docs.gl/gl2/glClearStencil) - the value a
  `GL_STENCIL_BUFFER_BIT` clear writes, 0..255 (GL starts at 0). Takes an
  expression, truncated toward zero; scoped by `GL_STENCIL_BUFFER_BIT`
- [`glEdgeFlag(GL_TRUE|GL_FALSE)`](https://docs.gl/gl2/glEdgeFlag) - scalar boundary-edge flag; 0/1 accepted
- [`glClipPlane(plane, (GLdouble[]){a, b, c, d})`](https://docs.gl/gl2/glClipPlane) - user clip
  plane, gated by `glEnable(GL_CLIP_PLANE0..5)`; coefficients are expressions,
  so a plane can animate, and the cursor draws the one you are editing (see
  [the clip-plane guide](#the-clip-plane-guide) and
  [tutorial](TUTORIAL.md#clip-planes)). Export routes the equation through a
  small `repl_gldouble4` helper so the C file stays C89-compilable, and reload
  converts it back
- [`glFogi(GL_FOG_MODE, GL_LINEAR|GL_EXP|GL_EXP2)`](https://docs.gl/gl2/glFog),
  [`glFogf(pname, value)`](https://docs.gl/gl2/glFog) with pname `GL_FOG_DENSITY`,
  `GL_FOG_START`, or `GL_FOG_END`, and
  [`glFogfv(GL_FOG_COLOR, (GLfloat[]){r, g, b, a})`](https://docs.gl/gl2/glFog) -
  distance fog; enable with `glEnable(GL_FOG)` (see built-in example *Fog ring
  tunnel*)
- [`glRasterPos3f(x, y, z)`](https://docs.gl/gl2/glRasterPos) - position for bitmap text (see label below)

[`glMaterialfv`](https://docs.gl/gl2/glMaterial) also accepts the flat shorthand
`glMaterialfv(face, pname, r, g, b, a)` - the parser rewrites it to the
compound-literal form. [`glClipPlane`](https://docs.gl/gl2/glClipPlane) and
[`glFogfv`](https://docs.gl/gl2/glFog) accept the same flat shorthand
(`glClipPlane(plane, a, b, c, d)`, `glFogfv(GL_FOG_COLOR, r, g, b, a)`).

#### Enum and mask arguments

"Every numeric argument is an expression" stops at the slots that name a GL
constant. An enum slot takes one token from a fixed list and nothing else -
no expression, no variable, no arithmetic - and a mask slot (`glClear`,
`glPushAttrib`) takes one or more of those tokens joined with `|`. Tab
completion offers the legal set for the slot the cursor is in, which is the
quickest way to see what a command will accept.

Mask spellings are **canonicalized on commit**: duplicates collapse and the
tokens come back in a fixed order regardless of how you typed them, so
`GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT` commits as `GL_COLOR_BUFFER_BIT |
GL_DEPTH_BUFFER_BIT`. `GL_ALL_ATTRIB_BITS` is kept as itself rather than
expanded, and means the union of the groups the REPL can currently change -
so its scope grows if a later release models another group.

Three slots bend the rule, each in a documented direction:

- **Boolean slots take `0`/`1` too.** `glDepthMask`, `glColorMask`, and
  `glEdgeFlag` accept either spelling and canonicalize to `GL_TRUE` /
  `GL_FALSE`.
- **`glStencilFunc`'s `ref` is a real expression** while its `mask` stays a
  literal - see the bullet above.
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

`gluTess` polygons handle concave outlines, and multiple contours in one
polygon create holes (odd winding rule). A polygon wraps one or more contours,
and each contour is a run of vertices:

```c
gluBegin(GLU_POLYGON);
  gluNormal(0, 0, 1);
  gluBegin(GLU_CONTOUR);         // outer outline, may be concave
    gluColor(0.98, 0.76, 0.36);
    gluVertex(-1, -1, 0);
    gluVertex(1, -1, 0);
    gluVertex(0, 1, 0);
  gluEnd();                      // close the contour
gluEnd();                        // close the polygon
```

Add a second `gluBegin(GLU_CONTOUR)` block inside the same polygon to punch a
hole through it. The built-in examples *GLU concave arrow*, *GLU concave arrow
cutout*, and *GLU concave arrow extrusion* build up all three cases.

- `gluBegin(GLU_POLYGON)` - start a tessellated polygon (REPL syntax over
  [`gluTessBeginPolygon`](https://registry.khronos.org/OpenGL-Refpages/gl2.1/xhtml/gluTessBeginPolygon.xml))
- `gluBegin(GLU_CONTOUR)` - start a contour within the polygon (REPL syntax
  over [`gluTessBeginContour`](https://registry.khronos.org/OpenGL-Refpages/gl2.1/xhtml/gluTessBeginContour.xml))
- `gluEnd()` - end the current contour or polygon, tracked by nesting depth
  (REPL syntax over [`gluTessEndContour`](https://registry.khronos.org/OpenGL-Refpages/gl2.1/xhtml/gluTessEndContour.xml) /
  [`gluTessEndPolygon`](https://registry.khronos.org/OpenGL-Refpages/gl2.1/xhtml/gluTessEndPolygon.xml))
- `gluNormal(nx, ny, nz)` - set the per-vertex normal (REPL syntax over
  [`gluTessNormal`](https://registry.khronos.org/OpenGL-Refpages/gl2.1/xhtml/gluTessNormal.xml))
- `gluColor(r, g, b[, a])` - set the per-vertex color; alpha defaults to 1.0
  when omitted (REPL-only convenience, no direct GLU equivalent)
- `gluVertex(x, y, z)` - add a vertex to the current contour (REPL syntax
  over [`gluTessVertex`](https://registry.khronos.org/OpenGL-Refpages/gl2.1/xhtml/gluTessVertex.xml))

### Bitmap text - `label()`

![The Orrery example uses labels that track 3D orbits](images/labels-orrery.png)

```c
glRasterPos3f(x, y, z);      // place the text anchor (transforms apply)
label("Earth phase = %f", t);
```

- `label("fmt", a, b, c, d)` draws bitmap text at the current raster
  position. Up to 4 substitution args; `%f` substitutes a value, `%%` is a
  literal percent. The string literal between the quotes is limited to 64
  characters and may not contain `//`, `(`, `)`, `,`, or backslashes.
- This is a REPL convenience, not a real GL call - exported C files include
  a self-contained `label()` helper so they still compile standalone.

With lighting on, the text colour is the *lit* result latched at the
`glRasterPos` - a later `glColor3f` cannot change it. That path is where
drivers disagree, and one disagreement bites in practice:

> **macOS:** if `GL_COLOR_MATERIAL` is enabled when the `glRasterPos3f` runs,
> Apple's GL lights the tracked components as **zero** and the text comes out
> black. It is a driver deviation, not a REPL one - the same scene is correct
> on Mesa and NVIDIA, and the exported C behaves identically to the REPL
> either way. `glDisable(GL_COLOR_MATERIAL);` immediately before the
> `glRasterPos3f` works around this case: in the tested scene, the text then
> matches the lit vertex colour. Details:
> [Fidelity to OpenGL](#fidelity-to-opengl).

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
and `rand2` is the same hash mapped to `[-1, 1]`. There is no per-frame random
state anywhere: the same pair always returns the same number, which is what
makes `rand` the stateless substitute for stored random values - see [Working
without state](#working-without-state).

That list is the whole vocabulary - there is no way to add to it, because a
`funcN` emits geometry and does not return a value. A name outside the list is
rejected when the line commits (`unknown function 'fabs'`) rather than quietly
evaluating to zero, so a typo surfaces as an error on the line that has it.
Export translates each name to its C twin (`abs` becomes `fabsf`), which is
what makes a scene that commits a scene whose exported C computes the same
numbers.

What each function is *for*, and how to shape animation with `clamp` / `lerp` /
`smoothstep` / `atan2`, is worked through in
[TUTORIAL.md](TUTORIAL.md#math-expressions).

#### Where expressions differ from C

The syntax is C's, but this is a float evaluator with no integer type, and the
domain errors you are most likely to hit while editing are **guarded** rather
than left to poison the frame: division and `%` by zero yield `0`, `sqrt` takes
the absolute value of its argument, `asin`/`acos` clamp to `[-1, 1]`, and a
zero-width `smoothstep` degenerates to the step function it is the limit of.
Guarded is not the same as total - see [NaN and infinity](#nan-and-infinity)
below for what still gets through.

That guarding, plus a value model with no integers, costs a handful of
deviations, and they are the ones that surprise people:

- **`%` is float modulo** - `5.5 % 2` is `1.5`, where C's `%` is integer-only
  and would not compile that. It is exactly `fmod`, including the truncation
  described below, so it is *not* GLSL's floored `mod` either.
- **Dividing by zero yields `0`**, not infinity or NaN. So does `%` by zero.
  (The threshold is a magnitude of `1e-12`, so a denominator merely *near*
  zero collapses too, rather than exploding.)
- **`==` and `!=` compare within a `1e-6` epsilon**, not bitwise. Two values
  that differ only in the last few bits count as equal, which is what makes
  `if(fmod(i, 3) == 0)` usable at all.
- **`&&` and `||` do not short-circuit** - both sides always evaluate. Nothing
  in the language has side effects, so this only ever costs time.
- **`asin` and `acos` clamp their argument to `[-1, 1]`** first, so a dot
  product that drifts a hair past 1 returns a number rather than a NaN.
- **`abs` is float absolute value**, C's `fabsf`. In C, `abs` is
  `int abs(int)` from `<stdlib.h>`, so the same call there truncates:
  `abs(-1.5)` is `1.5` here and `1` in C. The trap runs both directions, and
  it is the one spelling worth double-checking when you move an expression
  between the two.
- **The libm spellings are not accepted.** `fabs` does not exist here, and
  neither do the `f`-suffixed forms - `sinf` is as unknown as a typo. You
  cannot supply the missing name either: a `funcN` draws, it does not return a
  value, so the function list above is the entire vocabulary.
- **`log` is base 10 and `ln` is base e** - the reverse of C's `log`/`log10`
  naming, and the reason a `log` copied out of C code comes back wrong.
- **`fmod` is truncated, not floored** - it is C's `fmodf`, and the `f` stands
  for *floating-point* (the prefix in `fabs`, `fmin`, `fdim`), not for
  *floored*. It is in fact the non-floored one. GLSL's `mod` differs from it
  in a single operation:

  ```c
  fmod(x, y)  ==  x - y*trunc(x/y)   // here and in C - quotient toward zero
  mod(x, y)   ==  x - y*floor(x/y)   // GLSL - quotient toward -infinity
  ```

  `trunc` and `floor` agree whenever `x/y` is positive, so the two are the
  same function until an operand goes negative. Then `fmod` takes the sign of
  the **dividend** and `mod` the sign of the **divisor**: `fmod(-1, 3)` is
  `-1`, where GLSL's `mod(-1, 3)` is `2`. There is no `mod` here (nor a
  `trunc` - `fmod` is already the truncating one), so when you want the
  floored version - wrapping an index or an angle, where a negative input
  should land back inside `[0, y)` - spell out the `floor` form above.
- `rem` is the IEEE remainder via `remainderf`, which rounds the quotient to
  nearest rather than toward zero and so can differ in sign from both:
  `rem(5, 3)` is `-1` where `fmod(5, 3)` is `2`.
- `lerp` is deliberately **not** clamped (`s` outside `[0, 1]` overshoots), and
  `smoothstep` accepts `e0 > e1`, ramping from 1 down to 0.

And a few things C has that simply are not here: no ternary `?:`, no bitwise
operators (`|` appears only inside a [mask
argument](#enum-and-mask-arguments)), no compound assignment, and no
assignment-as-expression - `var = expr;` is a statement of its own. A `1.5f`
literal is accepted on input but the suffix is dropped from the committed line.

#### NaN and infinity

The guards listed above cover the cases that come up while editing, not every
domain error, so both values remain reachable:

- `log` and `ln` of a negative number return NaN, and of `0` return `-inf` -
  the guarded `sqrt` has no equivalent here, because a negative logarithm has
  no sensible substitute the way `sqrt(-4)` has `2`.
- `pow` with a negative base and a fractional exponent returns NaN, matching
  C's `powf`.
- `NAN` and `INFINITY` are constants you can type outright.

A NaN coordinate generally means the geometry using it fails to draw, so a
shape that vanishes right after an edit to a `log`, `ln`, or `pow` argument is
worth suspecting first.

Tracking one down is fiddlier than it looks, because the functions differ on
whether they pass a NaN along:

- Arithmetic propagates it, and so does `clamp` - its comparisons are both
  false against a NaN, so the value falls through untouched.
- `min` and `max` **discard** it. They are C's `fminf`/`fmaxf`, which return
  the other operand when one side is NaN, so `min(x, 1)` quietly yields `1`
  and the NaN disappears somewhere upstream of the symptom.
- `sign` returns `0` for a NaN, the same as it does for exactly zero.

### Variables

```c
float x, y, z;          // declare before use
x = 1.5;                // assign (any expression)
glVertex3f(x, y, z);    // use anywhere a number is expected
```

- Program-wide declarations are hoisted to the top automatically, so every
  reference follows its declaration.
- Up to 31 program-wide user variables; the predefined `t` occupies a separate
  reserved slot.
- Program-wide variable values persist across commits and are saved/loaded
  with the scene.
- Program-wide initializers are allowed: `float n = 1;`.
- A name is at most 15 characters, and one declaration line may introduce at
  most 8 of them - split a wider list across lines. `t`, `PI`, `TAU`, `e`,
  `float`, `var`, and the scratch-array names `A`, `B`, `C` are reserved and
  refuse a declaration.

A committed line holds at most **255 characters**. The line you are typing is
not held to that - the input buffer is 1024 bytes - so a long line is rejected
when you commit it, not while you compose it. It is also the one limit an edit
can hit indirectly: a Replace All that would push any line past it fails the
whole operation rather than truncating.

That is what `float` means at the top level; `static float` is also
program-wide even when typed inside a function. A plain `float` inside a
function instead declares a value private to each call. Locals consume none of
the 31 program-wide slots and use a separate 32-binding frame shared with that
function's parameters and deepest active loop nesting; see [Function-scoped
locals](#function-scoped-locals).

### Scratch arrays

`A[16]`, `B[16]`, and `C[16]` are three fixed global arrays for loop and
recursive algorithms:

```c
A[0] = 0;
A[1] = 1;
A[0] = A[0] + (A[1] - A[0]) * 0.25;
glVertex3f(A[0], 0, 0);
```

The index is itself a full expression - `A[i]`, `A[i + 1]`, `A[floor(u*4)]`
all work - truncated to int, and it must land in `0..15`. A bare `A` with no
subscript is an error everywhere except [`glMultMatrixf(A)`](#from-a-scratch-array),
which reads the whole array as a matrix. Like variables, scratch arrays persist
and round-trip through save/load.

#### Writing several cells at once

A braced list fills a contiguous run of cells from one row:

```c
A[0] = {1, 0, 0, 0};      // writes A[0], A[1], A[2], A[3]
A[4] = {0, c, s, 0};
A[8] = {0, 0, 1, 0};
```

The cells are ordinary expressions, re-evaluated every frame just like a
single `A[k] = ...` row, and anything the list does not cover keeps its
previous value. Four rows of four is how a 4x4 for
[`glMultMatrixf`](#from-a-scratch-array) is usually written - it reads as the
matrix it is.

The exact rules - what the base index may be, how long a list can get, and
why a block row is the one assignment the value plot will not take - are in
[ADVANCED_USAGE.md](ADVANCED_USAGE.md#scratch-block-assignment).

### Arbitrary matrices

`glTranslatef`, `glRotatef`, and `glScalef` cover almost everything, and they
read far better than sixteen numbers. `glMultMatrixf` is for what they cannot
express between them - a shear, a mirror, a planar shadow projection. It
post-multiplies the current matrix by a 4x4 you supply, written inline:

```c
glMultMatrixf((GLfloat[]){1, 0, 0, 0, 0.4, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
```

The flat shorthand `glMultMatrixf(m0, ..., m15)` is accepted too and rewrites
itself to the compound-literal form, the same as `glMaterialfv`, `glFogfv`, and
`glClipPlane`.

The layout is OpenGL's **column-major** order, the same one `glGetFloatv`
returns: the first four values are the first column, the next four the second,
and - the one worth memorizing - cells 12, 13, 14 hold the translation. The
example above shears x by 0.4 of y.

Cells are ordinary expressions, re-evaluated every frame like any other
argument, so a matrix built from `t` animates. The transform guides, replay,
and overlays all follow it, because they see the same matrix the frame did.

The classic use is the planar shadow projection - the matrix that squashes
geometry onto a plane as seen from a light, so drawing the shape a second time
through it draws its shadow (the *Planar shadows (glMultMatrixf)* example is
this, animated):

```c
float lx, ly, lz;         // light position, over a floor at y = 0
lx = 2*cos(t);
ly = 4;
lz = 2*sin(t);
glPushMatrix();
glMultMatrixf((GLfloat[]){ly, 0, 0, 0, -lx, 0, -lz, -1, 0, 0, ly, 0, 0, 0, 0, ly});
glColor3f(0.1, 0.1, 0.12);   // draw the geometry again, flattened and dark
glutSolidTeapot(1);
glPopMatrix();
```

#### From a scratch array

Sixteen cells is also exactly a 4x4, so `glMultMatrixf` accepts a bare scratch
array name instead - `A`, `B`, or `C`:

```c
A[0] = 1;                 // the identity matrix: 1s down the diagonal
A[5] = 1;
A[10] = 1;
A[15] = 1;
glMultMatrixf(A);         // post-multiply the current matrix by A
```

Or a [block assignment](#writing-several-cells-at-once) per column, which
lays the matrix out the way it is usually written down:

```c
A[0]  = {cos(t), sin(t), 0, 0};
A[4]  = {-sin(t), cos(t), 0, 0};
A[8]  = {0, 0, 1, 0};
A[12] = {0, 0, 0, 1};
glMultMatrixf(A);         // a rotation about z, animated by t
```

This form is worth it when the matrix is built up over several lines - in a
loop, or by a `funcN` - or reused by more than one call. The name is not an
expression and not a subscript: the array *is* the matrix, filled by ordinary
`A[k] = ...` lines above the call, in the same block. Cells you never assign
are zero, so write every one your matrix needs non-zero - an unassigned
`A[15]` leaves the matrix singular and your geometry gone.

The array is read when the frame is built, at that point in the program, so
cells fed by `t` animate exactly as inline expressions do.

### For-loops

```c
for(i, 0, 24) glVertex3f(cos(i*TAU/24), sin(i*TAU/24), 0);

for(i, 0, n, 2) {        // optional step argument; multi-line body
    glVertex3f(i, 0, 0);
}
```

The parser accepts up to 64 nested blocks, counted across every kind that owns
a brace pair - `for`, `if`, and function bodies alike, not loops alone. In
practice useful nesting runs out well before that: first against the
flat-command budget, then against the number of loop-iterator variables in
scope. Loop bounds can be expressions (and can animate with `t`).

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

Both mean exactly what they mean in C, and export as themselves. `break`
ends the **innermost enclosing** loop; a jump inside a nested loop never
touches the outer one. `continue` abandons the rest of the current
iteration and starts the next.

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
`if(...)` guard - see the *Recursive 3D tree* example.

#### Function-scoped locals

A `float` declaration *inside* a function body declares a **local**: it belongs
to that function, gets a fresh binding on every call, and takes none of the 31
program-wide variable slots and no variable-panel row.

```c
blade(x0, z0, height) {
    float bend, u;                    // one fresh pair per call
    bend = sin(t*2.2 + x0)*height*0.26;
    for(s, 0, 6) {
        u = s/4;
        glVertex3f(x0 + bend*u*u, height*u, z0);
    }
}
```

- **`static` chooses storage, and it beats where the cursor is.**
  `static float x;` is program-wide wherever you type it - including from
  inside a function body, which is how you declare one without leaving the
  function first. Plain `float x;` is a local inside a function and
  program-wide at the top level, so existing scenes behave exactly as before.
- Every local starts at `0` on entry to the call, and takes **no initializer**.
  `float x = 1;` inside a body is rejected: declare it, then assign on the next
  line.
- Locals hoist to the top of the *function body* the same way top-level
  declarations hoist to the top of the *program*, so you can type one at the
  moment you realize you need it - including inside a `for` or an `if` nested in
  the body. It lives for the whole call either way: it is not scoped to the
  block you typed it in, and it is not reset per iteration.
- Recursion is safe. Two frames of the same function never share a local, which
  is what a program-wide scratch variable could never manage.
- **Names follow C's scope rules.** A local may not collide with a parameter of
  the same function, or with another local of the same body - those share one
  scope, and C calls that a redefinition rather than shadowing. Shadowing an
  *outer* name is fine and resolves innermost-first: a local may shadow a
  program-wide variable, and a loop iterator may shadow a local for the length
  of its loop. Parameters and loop iterators stay read-only either way.
- `// @tune` and `// @config` need a variable-panel row, so they require a
  program-wide declaration (`static float`).
- A function's parameters, its locals, and its loop iterators at the deepest
  nesting level share one 32-binding frame:
  `params + locals + deepest loop nesting <= 32`.

A local is not a return value - the caller cannot read it. When the caller does
need a result, keep that one variable program-wide and let the function write
it. The *Orrery* example does exactly that: `px/py/pz` stay program-wide so the
caller can hang a label off the body it just drew, while the fifteen Kepler
intermediates that used to crowd the variable panel are now locals.

### Conditionals

Both simple `if` blocks and multi-branch `if` / `else if` / `else` chains
are supported:

```c
if(t > 2) {
    glColor3f(1, 0, 0);      // Red if t > 2
} else if(t > 1) {
    glColor3f(0, 1, 0);      // Green if 1 < t <= 2
} else {
    glColor3f(0, 0, 1);      // Blue otherwise
}
```

Continuation lines are matched as a unit, so both braces must sit on the same
line as the keyword - `} else if(t > 1) {` and `} else {`, with whitespace
between the pieces optional. Splitting the brace onto its own line does not
parse.

### Disabling a block

Place the cursor on the header line of a `for`, `if`, or function block - or
on its closing `}` - and press `Ctrl+/`. Every line in the block is commented
out in one press:

```c
for(i, 0, 5) {                 // ← cursor here, or on the closing brace…
    glTranslatef(1.2, 0, 0);
    glutSolidCube(0.6);
}
```

```c
// for(i, 0, 5) {              // …and one Ctrl+/ later
//     glTranslatef(1.2, 0, 0);
//     glutSolidCube(0.6);
// }
```

Pressing it again on either brace line restores the block. The toggle works
off the block structure the REPL parsed, so it applies to `for`, `if` /
`else if` / `else`, and function definitions - the constructs that own a brace
pair.

Alternatively you can wrap lines in `if(0) { … }` - the body emits nothing
but the lines stay visible. This is less convenient in practice, since it
means adding and later removing the wrapper.

### Comments

Type `// text` directly to add a comment line, or press `Ctrl+/` to toggle a
comment on an existing one.

`Ctrl+/` works on a range, and picks it for you:

- **With a selection**, the range is the selection. If every selected line is
  already commented the press restores them all; otherwise it comments them
  all. A mix of code and comments comments the rest, so the next press puts
  the mix back exactly as it was. The selection stays put afterwards, which is
  what lets a second press undo the first.
- **Without one**, the cursor line decides. On a `for`, `if`, or function
  header - or on its closing `}` - the whole block toggles in one press,
  commented block included: `Ctrl+/` on `// triangle() {` brings the entire
  function back, not just that line. On any other line it is just that line.

Comment and uncomment are exact inverses: whatever a press comments, the next
press restores character for character, including function names, parameter
lists, and expressions like `cos(ph + t)` that would otherwise come back as
the numbers they evaluated to.

That reversibility is why the toggle is picky about what it will comment.
Commented-out code still has to be legal code, so a range is refused when it
would not survive the trip back:

- a range that opens a block it does not close (or closes one it did not
  open) - select whole blocks, not halves;
- a range holding a `float` declaration whose variable is still read outside
  it - delete the readers first;
- more than 16 lines at once.

A refused toggle changes nothing, leaves no undo step, and says why in the
status line.

A trailing comment on a `float` declaration can also carry a tag that changes
how the variable is treated:

- `// @tune` makes it a knob in the exported program - see [Tunable
  Variables](#tunable-variables--tune).
- `// @config` marks it as a parameter the program assigns on purpose, which
  keeps its variable-panel row bright instead of dimmed - see [Config
  Variables](ADVANCED_USAGE.md#config-variables--config).

---

## Making It Move

![The Animated ring example](images/animated-ring.gif)

`t` is the one predefined variable - it exists in every session without a
declaration, starts at `0`, and while playing advances a fixed 1/60 s per
simulation tick. That is *not* the same as per rendered frame: a replay
backstep, for instance, reconstructs its frame over as many as eight rendered
frames, and none of them advance `t`. Use it in any expression:

```c
glRotatef(t*45, 0, 1, 0);
glVertex3f(sin(t), cos(t), 0);
```

This is the whole animation model: there is no keyframe data and no
accumulated state. The scene re-evaluates every frame, so anything written
in terms of `t` animates - loop bounds, colors, transforms, vertex
positions - and a frame is a *pure function of `t`*. The same `t` always
produces the same picture, which is what makes the scrubbing below (and
headless capture) exactly reproducible.

- **Ctrl+T** plays/pauses time (the *Auto time* config item is the same
  toggle).
- **Ctrl+Shift+T** resets time `t` to zero.
- `--time <secs>` (or the `GLR_TIME` env var) sets the starting `t` when
  launching - handy for headless captures of a later moment.

### Working without state

A frame being a pure function of `t` cuts both ways: there is nowhere to
*accumulate* anything between frames. You cannot write `pos = pos + vel`
once per frame and expect `pos` to remember where it was - next frame the
scene re-evaluates from source, not from last frame's values. Scenes that
would normally keep state use one of three patterns instead. They read a
little differently from typical game-loop code, but they are not harder
to write - and they are *easier to debug*, because any moment can be
reproduced exactly by setting `t`, without replaying history to get there.

**Deterministic randomness instead of stored random state.** Where a
game loop would roll a particle's attributes once and store them, here
each particle recomputes them every frame from `rand(seed, iter)` /
`rand2` - the same (seed, iter) pair always returns the same value, so a
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
history-dependent - a sort's array order depends on every swap before it.
The stateless version recomputes that history each frame: start from the
initial state and re-run the algorithm's steps up to the count implied by
`t`. The *Bubble sort (scratch arrays)* example re-seeds `A[0..15]` with
a deterministic shuffle and re-runs its compare-and-swap loops every
frame, gating each compare on `p*15 + j < steps` where `steps` derives
from `t` - the bars freeze mid-sort at exactly the right compare, and
dragging the time slider scrubs the sort forwards *and backwards*.

The payoff of all three is the same: reproducibility. A glitch spotted at
`t ≈ 7.3` is inspected by pausing and dragging `t` to 7.3 - no waiting,
no lucky re-run, no divergent state. It is also what makes
[replay](#replay), timeline scrubbing, and `--time`-anchored headless
captures possible at all. The cost is recomputation - the replay pattern
in particular redoes work proportional to what `t` implies, every frame -
see [Performance & Scope](#performance--scope) for where that ceiling
sits and how export lifts it.

### The Variable Panel

![The variable panel: t plus three program-wide variables, each with its value and slider](images/variable-panel.png)

The variable panel (bottom-right) lists `t` plus every program-wide variable
declared by the scene, with its current value and a slider. Function locals
have no panel row.

- **Left-click drag** on a row scrubs the value linearly (0.1 units/px).
- **Right-click drag** is the *fast* scrub: the same linear scrub at 10×
  the rate (1.0 units/px), for covering large ranges quickly.
- **Shift + click drag** is the *slow* scrub: linear deltas at 1/5 speed
  (0.02 units/px), for dialing in a precise value.
- Toggle the panel with **Backquote** (`` ` ``) or the *Variable panel* config
  item.

Row brightness distinguishes knobs from storage:

- **Bright** - read-only parameter/config variables; best for sliders.
- **Dim** - variables written by committed `name = expr;` lines. You can
  drag them, but a later assignment may overwrite the value.
- A [`// @config`](ADVANCED_USAGE.md#config-variables--config) tag on the
  declaration keeps a row bright even though the program assigns it - for
  bounds-keeping writes like `n = min(20, max(n, 3));` that clamp a knob
  rather than compute state.
- The [`// @tune`](#tunable-variables--tune) accent is separate from
  brightness. `t` only dims if your source explicitly assigns `t = ...;`.

Slider edits are undoable and go through the normal commit pipeline; when a
declaration exists, dragging rewrites its initializer.

Because `t` is just a variable, it gets a row in the variable panel like any
other - and dragging that row is a timeline scrubber. Pause with **Ctrl+T**,
then drag the `t` row to move the whole animation back and forth; release
and press **Ctrl+T** again to resume playing from wherever you left it. The
usual drag speeds apply: plain drag scrubs linearly, **right-click drag** is
the fast scrub, **Shift+drag** the slow one.

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

**2D mode.** *View mode* (Ctrl+Shift+V, or the CAMERA section of the Config
menu) switches between the 3D perspective camera and a flat 2D orthographic
projection - useful for plots, sketches, and UI-like drawings. Examples that
declare `@cfg view_mode = RENDER3D_VIEW_2D` start in 2D automatically.

![Toggling View mode between 3D perspective and 2D ortho on the wave surface](images/view-mode-2d.gif)

**Projection.** *Projection* (Ctrl+Shift+E) toggles between perspective and
orthographic projection while keeping the free, interactive camera. Unlike
*View mode* (which flattens and locks the camera to a top-down 2D view), it
keeps the current orbit angle, so you can navigate the scene
orthographically. Examples can declare `@cfg projection = PROJ_ORTHO`.

---

## Seeing What You're Doing

Immediate-mode GL is invisible state: the current color, the current matrix,
the winding of the polygon you just typed. A family of guides and overlays
exists to make that state visible while you edit - they follow your cursor and
annotate the geometry the line under it produces. The [diagnostic
views](#diagnostic-views) that follow work the other way round: they re-render
the whole scene to answer one question.

### Vertex entry guides

While a `glVertex3f(` line is still being typed, the scene shows where the
vertex *could* still land given what you have entered so far. Each coordinate
you type removes a degree of freedom, and the guide collapses to match - a
surface, then a line, then a point:

![The guide narrowing as coordinates are typed: sheet at x = 1.2, then a line along z, then a point, then a fresh sheet for the next vertex](images/vertex-guides.png)

Reading in order, and the typed line above each panel is what produced it:

- **One coordinate** → a translucent graph-paper sheet spanning the two open
  axes, with integer grid lines (the zero lines brighter), a dashed ghost rim
  visible through geometry, and an `x = 1.2` readout naming the coordinate you
  pinned.
- **Two coordinates** → the locus collapses to a line along the one open axis,
  with integer tick dots (the 0 tick larger), end fades, a dashed ghost pass
  through occluders, and the still-free axis named at the positive end.
- **All three** → a point marker at the exact position, pulsing so it stays
  findable against geometry. The same marker appears when the cursor sits on a
  committed vertex line.
- **The next vertex** starts the ladder over. In the last panel the first
  vertex has been committed - it is the one labelled `v0` - and a second is
  under way, pinned on `y` this time, so the sheet is horizontal and green
  where the first was vertical and red.

The guide is colored by the axis you pinned (X red, Y green, Z blue), which is
why the sheet changes color between the first and last panels.

`glVertex2f` pins `z = 0` implicitly, so its first coordinate already narrows
the guide to a line. Guides follow the cursor's transform context - inside a
`glPushMatrix`/`glTranslatef` frame the sheet and line render in that frame,
matching where the vertex will actually land.

### Cursor guides & vertex overlays

Once lines are committed, the cursor keeps pointing into the scene: whatever
geometry the line under your cursor draws is outlined and labelled out in the
viewport, and moving the cursor moves the highlight.

![Cursor on each of two tri() calls in turn; the highlight and vertex labels move to the triangle that call draws](images/cursor-highlight.png)

Both halves are the same scene and the same program - only the cursor has
moved. The *Transform stress* example calls one `tri()` function from two
different transform stacks, so line 54 draws the magenta triangle up top and
line 60 draws the yellow one spinning at the origin. Park the cursor on either
call and the highlight, the `v0`/`v1`/`v2` labels, and the vertex outline land
on *that* call's triangle. This works through function calls and loop
iterations, not just on literal `glVertex3f` lines. That is what makes it
useful for finding out which of forty identical-looking shapes a line is
responsible for.

On a `glVertex3f` line the guide narrows to that one vertex: a crosshair at its
position, with the coordinates spelled out beside it. Arrow the cursor down
through a `glBegin` block and the crosshair steps from vertex to vertex.

The overlay toggles annotate geometry scene-wide:

![The cursor on the glNormal3f row, and the quad below it carrying vertex labels, points, outlines, normals and the polygon highlight](images/vertex-overlays.png)

- **Vertex labels** (F7): Off / Index / Index+Pos / Index+World /
  Index+World Fine - numbers each vertex of the primitive at the cursor,
  optionally with its coordinates. "At the cursor" is literal: the cursor has
  to be resting on a committed `glVertex3f` line. Park it anywhere else - a
  blank row, a `glBegin`, a vertex you are still typing - and there is no
  primitive to number, so nothing is labelled. **Whole scene** is the scope
  that lifts that restriction.
- **Overlay scope** (F8): Cursor block - last instance / all instances /
  single polygon - plus **Whole scene**. Controls how broadly the cursor-bound
  overlays are shown around repeated function/loop instances: the vertex
  labels, the polygon highlight, the **normal vectors** and the cursor guide
  all obey it. *Last instance* picks the final unrolled copy, so they all sit
  on the copy whose loop-body variable values the **variable panel** is
  showing - and on the last polygon a replay draws. *Whole scene* is the one
  scope not anchored to the cursor: it takes **every** vertex the program
  emits, not just the block you are editing. The highlight stays on the
  cursor's block there.

  Vertex labels are culled against the depth buffer when the GL context
  supports reading it - a vertex hidden behind solid geometry gets no label,
  in every scope. WebGL cannot read the default framebuffer's depth, so the web
  build shows every in-scope label instead. The polygon highlight is exempt and
  still draws through, so the cursor's shape stays findable.
- **Vertex label placement**: Decluttered / At vertex - *where* each label
  sits, independent of which vertices get one. Decluttered floats labels onto
  clear rows with leader lines; At vertex pins each to its own vertex, exact
  position and overlaps included.
- **Normal vectors** (Ctrl+N): draws each vertex's normal as an arrow,
  within the **Overlay scope** above - so on a dense surface you can narrow
  the arrows to the block you are editing instead of reading a pincushion.
  *Whole scene* is the scope that arrows everything.
- **Vertex outlines** (Ctrl+Shift+O) and **Vertex points** (Ctrl+Shift+P):
  outline polygons and mark vertices *(both on by default)*.
- **Polygon highlight** (Ctrl+P): highlights the polygon under the cursor line.
  Cycles Off / On / **Clipped & culled**. Both draw the highlight; they differ
  over your `glClipPlane` and `glEnable(GL_CULL_FACE)` calls. On draws the
  cursor's shape **as authored** - the whole sphere a plane cuts into a dome,
  both sides of a culled shell - so you can see the geometry you wrote. Clipped
  & culled draws only what the frame draws, like every other overlay.

Vertex outlines and vertex points always report the geometry the frame
actually shows:

- **Clip planes** - an outline stops where the shape does, and no point is
  drawn on a clipped-away face.
- **Culling** - back-face outlines vanish under `glEnable(GL_CULL_FACE)`,
  following your own `glCullFace` / `glFrontFace` rule. Authored vertex points
  are the exception: GL never culls point primitives, and a vertex shared by a
  front and a back face still sits on a face you can see, so the dots stay.
  Points on `glutSolid*` meshes do cull, since those draw as polygons.
- **Color mask** - geometry drawn under a `glColorMask` with no live color
  channel gets no outline and no dots: a depth-only seed pass puts nothing on
  screen to outline.

The cursor highlight is the deliberate exception to all of it. It draws through
a color mask (finding the line you're editing matters even when its geometry is
an invisible depth seed), and under Polygon highlight = On it ignores clipping
and culling too - so you can get a cut, culled outline with a whole-shape
highlight over it.

### Vertex label placement & numbering

Decluttered placement gives active edit-guide text priority: vertex labels
move to a nearby row, or are omitted when the bounded layout has no clear row,
rather than covering partial-vertex, normal, clip-plane, translate, or rotate
labels. **At vertex** is the explicit exact-position placement and bypasses
this decluttering.

The placement also decides **how vertices are numbered**, because the two
choices need different numbers to stay readable:

![Label placement: one quad drawn twice by a loop, shown decluttered (v0..v7, globally unique) and at vertex (v0..v3, repeated per copy)](images/label-placement.png)

The program is the code panel at the top; both scenes below it ran exactly
that, under *Cursor block, all instances* scope with the cursor on line 9.
The `for` loop draws one quad twice, so the flat program holds two unrolled
copies of a single four-vertex `glBegin` block. Only the placement differs
between the two scenes.

- **Decluttered** (left) numbers **globally**: `v0`..`v7`. These labels float
  off their vertex onto a clear row, so a repeated `v0`..`v3` per copy would
  leave two labels reading `v2` sitting near each other with nothing but
  crossing leader lines to say which quad each belongs to.
- **At vertex** (right) numbers by **index within the primitive**: `v0`..`v3`,
  once per copy. The label is drawn on the vertex it names, so there is
  nothing to disambiguate, and the in-block ordinal is the more useful reading
  - it tells you which corner of the quad you are on, and stays short. Global
  numbering on a dense surface would put four-digit numbers on every vertex.

*Last instance* and *Single polygon* keep only one copy, so both rules produce
the same numbers there.

**Single polygon** scope narrows labels and the polygon highlight down to
just the one primitive your cursor is building, instead of the whole
`glBegin`/`glEnd` block - handy for a multi-face batch like a cube drawn as
one `glBegin(GL_QUADS)` with several faces packed into it. The cursor's
position between vertex lines picks the primitive: any line up through a
primitive's last vertex belongs to it, and the next line starts the next
one.

![Single polygon scope: cursor on the second quad's second vertex highlights and labels only that quad](images/single-polygon-scope.png)

Here the cursor sits on the second quad's `glVertex3f(1.6, -0.8, 0)` line;
only that quad outlines and labels (`v4`..`v7`) - the first quad is left
alone even though both share the same `glBegin`/`glEnd` pair.

### Transform guides

With **Transform guides** on (Ctrl+Shift+X), placing the cursor on a committed
`glTranslatef` / `glRotatef` / `glScalef` line draws an overlay showing what
that line does. Every guide is drawn in the same "axes pulse" language - a dim
base shape with a bright dot traveling along it - colored from the command's
own vector, so a pure-axis transform reads as a pure axis color (X=red,
Y=green, Z=blue) and diagonals blend:

![Four transform guides: a translate arrow, a rotate dial, and two scale gizmos, each with the program that produced it](images/xform-guide-montage.png)

Each kind draws its own shape, labeled with its own readout:

- **Translate** - an arrow along the argument vector, tip where the geometry
  lands, labeled with the endpoint position.
- **Rotate** - a dial in the plane of the rotation, swept by the pulse in the
  direction of the turn and labeled with the angle.
- **Scale** - a 3-axis gizmo: a gray unit reference with a tick at `1.0` on
  each axis, and a pulsing arrow only on the axes whose factor is not 1 -
  outward for `> 1`, inward for `< 1` (the green Y arrow in the last tile).

Guides only appear when the line parsed cleanly and your current input
matches the committed source - partial or mid-edit lines are skipped.

#### Guide mode

**Transform guides** is a three-state setting - Ctrl+Shift+X (or the Config
menu row) cycles Off → World → Frame - and the two on-states differ in *where*
the guide is anchored. It is worth knowing which one you are reading: OpenGL
applies transforms in reverse source order when computing a vertex
(`M_1 · M_2 · ... · M_n · v`), so the literal reading of a transform line is
often not where you see it act.

![The same glTranslatef line under World mode and under Frame mode: World anchors the arrow at the world origin, Frame at the first cube](images/xform-guide-mode.png)

Both captures above run the same program with the cursor on the same
`glTranslatef(-3.6, 1, 0)`; only the mode differs.

- **World** *(top)* - the strict reverse-order reading, drawn in world axes
  starting from the point the commands *after* the cursor have already placed
  (accumulation stops at the first draw call - `glBegin`, a `glutSolid*`
  shape, a tess polygon). Here nothing follows the cursor but the cube, so the
  arrow leaves the world origin and ends in empty space. When post-cursor
  transforms *do* carry that point off the origin, rotate sweeps the point
  itself instead of drawing a dial, and scale draws a single before → after
  arrow instead of the gizmo.
- **Frame** *(default, bottom)* - anchored where the modelview *before* the
  cursor has carried the origin: the first cube. The same vector now runs from
  the first cube to the second, lining up with the geometry as rendered. Only
  the anchor comes from the pre-cursor matrix - the guide still draws with
  world-axis orientation.

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

### Attribute scope

`glPushAttrib` / `glPopAttrib` protect a group of GL state
([tutorial](TUTORIAL.md#scoping-state-with-glpushattrib)), and two editor
affordances make that scope visible instead of imagined.

Park the cursor on a `glPushAttrib` line and each mask token lights up in its
own colour, while every earlier line whose value the push *saves* gets a
matching gutter marker - "what's about to be protected". Park it on the
`glPopAttrib` and the lines whose changes the pop *reverts* light up instead -
"what snaps back here". `GL_ALL_ATTRIB_BITS` has no per-bit colour of its own,
but the lines it covers still get their markers.

A push or pop with no partner gets the same red gutter warning as an unmatched
`glPushMatrix`.

### Auto-normals

Unlike everything above, **Auto-normals** does not draw anything - it writes
`glNormal3f` lines into your program. It is an experimental helper with one
narrow job: you have pasted in a slab of static geometry, polygon soup with no
normals, and want lighting to do something reasonable without hand-writing a
normal per face. It derives them in **Face** (each primitive gets its own
normal, hard edges) or **Smooth** (area-weighted average across the faces
meeting at a vertex).

Read the limitation before reaching for it: **the pass only touches a
`glBegin` block whose vertex coordinates are all literal numbers.** If any
vertex in the block is expression-driven, the whole block is skipped. That
rules out most real scenes - geometry inside a `for` loop uses the loop
iterator, and anything animated uses `t` or a variable - so on the scenes you
would most like it to help with, auto-normals will quietly do nothing. Write
those normals yourself, or compute them alongside the vertices.

Within that constraint: generated lines are tagged `auto` in the code panel and
drawn dimmer, and normals you typed are never tagged, dimmed, or overwritten.
Switching to **Off** removes the generated lines and puts the removal on the
undo stack; that is the only way back out, since while the mode is on the pass
re-derives its lines every frame. Editing a generated line makes it yours, and
Off will then leave it alone. Exported `.c` files mark generated normals with a
trailing `/* @auto */` comment so a reloaded scene keeps updating them.

---

## Diagnostic Views

Each of these re-renders the whole scene to answer one question - which way is
this face pointing, what does the depth buffer hold, what did my stencil pass
write. They are independent of the cursor, and they stack with the overlays
above.

### Winding & face diagnosis

**Winding** (Ctrl+Shift+W) re-renders the scene with front-facing polygons
in green and back-facing ones in red (as decided by the active
`glFrontFace`), so flipped or inside-out faces stand out immediately:

![Winding view: the left triangle is wound counter-clockwise (front, green), the right clockwise (back, red)](images/winding-view.png)

Both triangles here list three vertices; only the order differs. When
culling or lighting misbehaves, this view usually names the culprit in one
glance.

### Wireframe & hidden-line

**Ctrl+W** cycles Off / Wireframe / Hidden-line.

![Wireframe (left) and hidden-line (right), on a torus (top) and a Sierpinski sponge (bottom)](images/wireframe-hidden-line.png)

Wireframe draws polygon edges over the scene. Hidden-line goes further: it
draws all edges first in a muted color, seeds the depth buffer with filled
polygons, then redraws visible edges bright - so the silhouette reads
clearly while occluded structure stays faint - handy for visualizing dense,
recursive meshes like the sponge above, where a plain wireframe collapses
into an unreadable tangle but hidden-line still lets internal structure show
through. Tip: vertex outlines/points are on by default and draw over the
wires; turn them off for a clean wireframe look.

### Depth view

**Ctrl+Shift+D** cycles the Depth view: **Off / Linear / Scene / Split** - a
grayscale rendering of the depth buffer, for seeing what depth testing
actually sees. Near surfaces are bright, far ones dark, and empty
background is black:

![Depth view: Scene mode (left) normalizes the grayscale to the geometry's own depth range; Split (right) overlays depth on the right half of the normal render](images/depth-view.png)

- **Linear** spreads the gray ramp across the whole near/far range of the
  projection. Faithful, but since most scenes occupy a small slice of that
  range, everything tends to read bright and flat - use it when you care
  about absolute depth (e.g. how close geometry sits to the near plane).
- **Scene** stretches the ramp over *your geometry's* actual depth extent
  instead, so the nearest surface is white and the farthest keeps a dim
  floor above the background - full contrast for spotting depth ordering
  and z-fighting candidates. The range follows the scene smoothly as it
  animates.
- **Split** keeps the normal render and overlays the scene-normalized
  depth image on the right half, pixel-aligned - the same column of pixels
  in both halves is the same fragment, so you can read color and depth
  side by side.

The depth snapshot is taken right after your geometry draws, before the
grid, backdrop, and axes render - so the helpers never appear in the depth
image (notice the grid on Split's normal half only). It works during
replay, pairs with wireframe/hidden-line (they seed real depths), and the
depth image is taken from the final accumulation pass, so AA and motion
blur keep working in Split's normal half. On the web build the row is
inert - WebGL cannot read the depth buffer back.

### Stencil view

**Stencil view** (Ctrl+Shift+S) cycles **Off / Palette / Ramp / Split** - a
false-color overlay of the stencil buffer, with a legend in the scene's top-left
corner. It is the companion to [stencil masks](TUTORIAL.md#stencil-masks): a mask changes
what draws without ever showing itself, so this is the only way to see what
your mask pass actually wrote.

Zero is fully transparent, which makes the overlay sparse - the ordinary
render shows through everywhere the buffer is 0, and only stenciled pixels
take a color. **Palette** gives each value a fixed swatch, **Ramp** normalizes
the non-zero values to a smoothed min/max, and **Split** restricts the overlay
to the right half. Vertex outlines, points and guides draw *over* the overlay,
so you can read the viz and the geometry together.

Four things are worth knowing before you trust what you see:

- It shows **zero versus non-zero, not write history**. An untouched pixel, a
  clear to zero, and an explicit `GL_ZERO` write are all byte 0, and all
  invisible. That is a property of the buffer, not of the viewer.
- The palette **repeats every 16 values** (1 and 17 share a swatch), which is
  why every legend row prints its numeric value. The legend lists the biggest
  rows by pixel count - up to eight, plus a `+N more` row when there are more
  - and always keeps the background and total rows.
- Nothing clears the scene rect for you, so a program that omits
  `GL_STENCIL_BUFFER_BIT` from its `glClear` accumulates stencil across
  frames. With the view on, the status line points that out once per program
  change: *Stencil view: program never clears GL_STENCIL_BUFFER_BIT*.
- **Replay fades** re-execute old state commands before the snapshot is taken,
  so legend rows can appear during a fade that match no visible geometry.

With Depth view on at the same time, depth wins wherever it paints: it
replaces the whole rect after the stencil overlay composites, so the stencil
overlay survives only in the live half of depth's Split. That is a consequence
of the compositing order, and it is left that way deliberately: a menu row that
silently switched another one off would be the more surprising outcome.

The overlay needs a stencil readback, which the web build cannot do; there the
row refuses with *Stencil view unavailable: WebGL context can't read the
stencil buffer (works in the native build)*, and a native context that granted
no stencil planes refuses the same way. The commands themselves work
everywhere - only the visualization is native/OSMesa-only. A scene whose
`@cfg` header carries `stencil_view` loads its stored value regardless, so
files round-trip unchanged between machines.

---

## The Config Menu

Nearly every toggle in this guide has a home here, whether or not it also has
a key. Open the **Config** dropdown (or press **Ctrl+Shift+K**). Items are
grouped into sections - hovering one opens a flyout of its items, and the
trailing **All** row shows the entire table at once (the mouse wheel scrolls flyouts
taller than the window):

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
- **GEOMETRY** - Wireframe, Winding, Depth view, Stencil view, Auto-normals
- **OVERLAYS** - Overlay scope, Vertex labels, Vertex points, Vertex outlines,
  Vertex outline style, Normal vectors, Polygon highlight, Transform guides
- **INTERFACE** - Variable panel, Compute profile, Memory profile, Code panel,
  Wrap at commas, Syntax highlight, Paren match, Paren scope

**Left-click** a flyout item to cycle it forward, **right-click** to cycle
backward. Multi-state items show their current state name. Items this guide
names without a shortcut - Point attenuation, Post FX Effect,
Auto-normals, Vertex label placement - are menu-only; they have no key.

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

Twelve directly-selectable grid themes (**F2**): Off, Classic, Tron, Ember,
Ocean, XZ Ruler *(default)*, Adaptive Planes, Radar, Tilled Field, Sketchbook,
Neon Graph, Graph Planes. Some backdrops enable hidden companion grids; see
[Advanced Usage](ADVANCED_USAGE.md#cfg-backdropgrid-pairing).
**Grid major** (Ctrl+Shift+G) cycles the major-tick spacing (1/2/5/10),
**Grid extent** (F3) the grid's reach (Close / Mid / Far), and **Grid
brightness** (F4) the line weight (Dim / Normal / Bright / Bold). Theme
changes cross-fade, so a newly chosen grid takes a few seconds to fully appear.

![Grid brightness against a bright cube - Dim, Normal (top), Bright, Bold (bottom)](images/grid-brightness.png)

Grid brightness is at its most useful where the grid crosses **bright
geometry**, as above: a near-white cube sunk below the grid plane, so the
whole graticule draws over its faces. Dim all but drops the lines there,
while Bright and Bold keep them readable - those two levels give every line a
dark contrast casing, because past a point raising the line's opacity alone
stops helping: a blended line converges toward its own color, and a pale gray
line on a pale surface has nowhere left to go.

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
See [Scope & Current Limitations](#scope--current-limitations).

**Light indicators** (Ctrl+L) draw a marker at each light's position
(labelled `L0..L3`, with *off* noted for disabled lights), so you can see
where the rig sits. Park the cursor on a light's own
`glEnable`/`glDisable(GL_LIGHTn)` line and that light's indicator is wrapped
in a soft halo of its own color - on or off, so it also finds the light you
are about to switch on. It tracks the line you are typing, not just committed
ones.

**Reading the numbers.** Turn code focus off (**Ctrl+Shift+F**) to show the
generated C around your program. Each light's diffuse, ambient, and specular
colors appear in `init()`. Positions are re-issued in `display()` every frame:
eye-space positions before the camera transform, and world-space positions
after it.

![The generated light-position block in display(), and the per-light color block in init()](images/light-theme-inspect.png)

Switching themes updates both blocks. The exported program carries the same
lines, where they can be inspected or edited as ordinary C.

### Rendering quality

- **MSAA** (Ctrl+U) - hardware multisampling on/off.
- **Line smooth** (Ctrl+Shift+L) - GL line antialiasing.
- **Accum effect** (Ctrl+Shift+U) + **Accum passes** (Ctrl+= / Ctrl+-) - the
  accumulation buffer drives antialiasing and motion blur:
  - **AA** *(default, 1 pass)* - jitters the camera frustum per pass.
  - **Blur** - re-renders the scene per pass across the frame's
    animation-time window, so spinning geometry smears realistically -
    even while the camera moves (the camera itself stays crisp). Scenes
    that don't use `t` fall back to AA jitter.
  - **Blur Cam** - blurs camera motion only; falls back to AA when still.
  - Passes: 1/2/4/8/12/16. The status bar shows the active mode
    (`AA 1x`, `Blur 16x`). Blur is expensive - every pass is a full scene
    render. `--no-accum` disables the accumulation buffer entirely;
    `--accum` forces it on where it would otherwise auto-disable (Mesa
    emulates the accumulation buffer on the CPU).

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
value at that point, which is what lets you step a loop and read its numbers
off directly.
Two related config items: **Replay mode** (Polygon steps a primitive at a
time, Vertex steps a vertex at a time) and **Replay expand**. Its middle
**Expanded** mode annotates every line in place - assignments, loop and
function headers, and any call with an evaluated form (`glVertex`/`gluVertex`,
`glColor`/`gluColor`, `glNormal`/`gluNormal`, the `glutSolid*` shapes, the
transforms) keeps its evaluated call appended as a `//` comment, so one source
line stays one line. **Verbose** is the only mode that splits a source row,
adding substituted and evaluated rows beneath it. Evaluated
`glColor`/`gluColor` comments use the RGB color they set (kept opaque so
zero-alpha values remain readable).

---

## Scenes & Workspaces

gl-repl keeps up to 8 scenes in memory, shown as tabs below the menu bar.

- **Examples first** - a fresh launch opens on the default example. A user
  scene is created when you choose File -> New Scene, load a scene file or
  workspace, or edit an example.
- **Auto-promotion** - editing a built-in example, or saving it into a managed
  workspace, forks it into a fresh scene slot named after the example.
  Subsequent edits accumulate there.
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
| File → Save Scene (Ctrl+S) | Save the active scene; a visible example is promoted into the bound workspace first |
| File → Save Scene as .glr | Write the active scene in the built-in-example authoring format |
| File → Load Scene | Load a `.c` file into a new scene slot |
| File → New Workspace… | Create and open a named managed workspace; scenes from a session with no workspace bound come along into it, while a workspace created from an already-bound one starts empty |
| File → Save Workspace | Save every open tab at once, the visible example included |
| File → Save Workspace As… | Save the whole set of tabs into a new named workspace |
| File → Open Workspace → *name* | Switch to a managed workspace |
| File → Open Workspace → Other folder… | Open a managed workspace outside the normal workspace root |
| File → Reveal Workspace Folder | Reveal the bound managed workspace; disabled while no workspace is loaded |
| File → Delete Workspace Scene | Confirm and remove the active managed scene from its workspace; disabled for examples, tutorials, and unbound scenes |

Which workspace those rows act on is named in three always-available places:
the chip leading the scene tab strip, which reads as a breadcrumb
(`Demo > [scene][scene]`) and opens the workspace list when clicked; the
**WORKSPACE:** header inside the File menu, directly above the workspace rows;
and the window title, which reads `gl-repl - Workspace: <workspace> | <scene>` (or
`gl-repl - <scene>` when no workspace is bound). A scene collection that is
not bound to any workspace says so in the chip (`no workspace`) and menu header
(`(none)`), while dropping the workspace field from the window title - so
"nothing is bound yet" is visible without cluttering the title bar.

A workspace is just a directory holding your scene files plus a
`.glr-workspace` manifest that lists them in tab order. That manifest is what
makes the directory a workspace: gl-repl opens exactly the files it names, in
that order, and leaves any other `.c` file in the folder alone - it will
neither load it nor delete it. Point the app at a directory without a manifest
and it declines rather than guessing; reach for **Load Scene** when you just
want to open a loose file.

Opening a workspace is all-or-nothing. Every scene is read and checked before
anything replaces what you have, so a file that has gone missing or will not
parse leaves you exactly where you were, with the previous workspace and your
current document untouched. Switching to a different workspace saves the one
you are leaving first - and if the tabs you are leaving were never bound to a
workspace at all, they are copied somewhere safe rather than dropped.

Where a save lands depends on how you launched. Run gl-repl from a directory
you can write to and it keeps using that directory, so plain `output.c` shows
up next to the binary as it always has. The packaged macOS app has nowhere
like that to write, so it saves into your gl-repl data folder instead, and it
will ask you to name a scene the first time - an example you have been playing
with does not have a filename of its own yet.

---

## Exporting & Importing

### Standalone C export

**Ctrl+S** (File → Save Scene) writes the active scene as a complete,
compilable GLUT/OpenGL C program. Where the file lands depends on whether you
have a workspace open: with one bound it becomes a scene file inside that
workspace, and without one it is the standalone `output.c` next to the binary.
Either way, if what you are looking at is still a built-in example, saving
forks it into a scene of your own first - the built-ins are never written to.

Header comments carry the REPL state (variables, config, camera), your
functions become C functions, and your commands become the `display()` body.
The generated file is **C89-compliant**, so it builds anywhere a GL/GLUT
toolchain exists, old machines included:

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

#### What the export is free of

The exported program is not a screen recording of the REPL - it is your scene
with the interpreter removed. No code panel, no grid, no menu bar, and (the
part that matters most) **no flat-command budget**: your `for` loops stay
loops instead of being unrolled into the 8192-command flat program.

The *Swaying grass field* example is the clearest case, because in the REPL it
is already pressed right up against that ceiling. Each blade costs 60 flat
commands, so its 135 blades flatten to **8113 of 8192** - set `bladeCount` to
137 and the program exceeds the budget and stops rendering entirely. Exported,
`bladeCount` is nothing but a loop bound:

![The exported grass program at 9600 blades, with its generated @tune HUD in the corner](images/export-c-grass.png)

That is the same scene, exported and compiled with nothing changed but two
numbers, both raised past anything the REPL can flatten - 9600 blades (71×
the example's count, some 576k flat commands' worth) over a wider field.
Those extra
9465 blades cost about 2.4 ms of draw time per frame, well inside a 60 fps
budget, because a compiled loop has no per-frame flatten to pay for. See
[Performance & Scope](#performance--scope) for the workflow that implies.

### Editing exported code & reimporting

The exported file is meant to be worked on. You can extend it by hand in C
and load it back - on reload, the commands between the `// Snippet start` /
`// Snippet end` markers are imported, so edits inside that span come home to
the REPL. Two rules keep the round trip clean:

- **Stay inside the REPL language** for the snippet section - the supported
  commands and syntax in [The REPL Language](#the-repl-language). Lines the
  importer doesn't recognize are skipped with a warning. (Anything goes
  *outside* the snippet markers, but those edits live only in the C file.)
- **Stay within the command budget** - the source document holds 1024
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

Extending the interpreter itself is contributor work rather than scene
authoring. If you need a GL call the REPL does not yet support, see
[*Adding A New Command*](ARCHITECTURE.md#adding-a-new-command) in
`ARCHITECTURE.md`.

### Scene export (`.glr`)

**File → Save Scene as .glr** writes the same file shape the built-in
examples ship in - no C scaffold at all:

```
// @cfg grid = GRID_THEME_OFF          <- only the settings that differ
// @cfg projection = PROJ_ORTHO           from the presentation defaults
static float a, b;                     <- declarations first,
func0(r) { ... }                       <- then function definitions,
glTranslatef(0.0000f, 0.0000f, -6.5000f);   // @camera dist
glRotatef(35.2500f, 1.0f, 0.0f, 0.0f);      // @camera rx
glRotatef(45.0000f, 0.0f, 1.0f, 0.0f);      // @camera ry
glTranslatef(0.0000f, 0.0000f, 0.0000f);    // @camera pan
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
...your commands, verbatim...
```

That order - declarations, then function definitions, then camera and body -
is the exported C's own order, and it is the one shape the loader accepts:
a file that puts its helpers at the bottom is rejected with a message naming
both the offending line and the line that established the phase. Comments and
blank lines carry no phase and are legal anywhere.

`--export-glr <path>` writes the same file from the command line, without
opening a window - `./gl-repl --example torus --export-glr torus.glr`. It
pairs with `--export-c`, which writes the C form of the same session.

It goes to the same directory as Save Scene, but uses the distinct
`<scene-slug>.glr` filename and format. Reach for it when you are **authoring
or modifying an example** rather than exporting a program. To turn one into a
loadable example catalog, see [Authoring an example
catalog](ADVANCED_USAGE.md#authoring-an-example-catalog).

Two things the format deliberately drops, because the example loader ignores
them anyway: `@cfg` rows outside the per-scene presentation subset (`msaa`,
`accum_passes`, replay/panel settings - those stay session state), and the
`@scene-name` / `@workspace-dir` bookkeeping that `catalog.ini` supplies
instead. For a file that carries *everything* back, use Save Scene.

### Mesh export (PLY)

**File → Export .ply** captures the current scene as an ASCII PLY mesh
named after the active scene - `glVertex` polygons, GLU-tessellated shapes,
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

A knob needs a variable-panel row to live in, so the tag requires a
program-wide declaration. Inside a function body, write `static float amp = 1;
// @tune` - a [function-scoped local](#function-scoped-locals) has no panel row
and rejects the tag.

Tagged variables are still normal REPL variables while you are authoring. In
the variable panel, tagged rows get an accent mark so you can see which
values will export as knobs:

![Tagged rows get an accent mark: amp and freq carry it, t and spread do not](images/variable-panel.png)

### Exported controls

When you save/export C, each tagged variable becomes a keyboard knob in the
standalone program. The generated program also draws a small HUD listing each
knob, its current value, and its keys.

![The exported grass program's knob HUD: bladeCount as exported, and after holding q](images/export-c-knobs.png)

Both halves are the same corner of the same export - the *Swaying grass
field* scene, whose two `// @tune` rows become the HUD's two lines. Left is
the value the scene was exported with; right is 177 `q` presses later, which
the step ladder below puts at exactly 1200. The knob writes the same global
the `for` loop reads, so the field thickens as the key repeats - no edit, no
recompile.

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

The pattern continues by decade - values in the thousands step by `50`,
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
while anything is animating it re-flattens the whole program - loops
unrolled, functions inlined, every argument expression parsed and evaluated
again. That is what makes the live experience possible (edit any line, drag
any slider, scrub time backwards), but it costs real CPU. Scenes built on
the [stateless patterns](#working-without-state) - especially the
replay-from-the-start kind - add their own recompute cost on top, since
each frame redoes all the work `t` implies.

The exported C program has none of that machinery - it is the same scene as
plain compiled GL calls, roughly **100× lighter on the CPU**. So the
workflow for pushing limits is:

1. Sketch and tune the scene in the REPL until it looks right.
2. Tag the parameters you still want to play with as `// @tune`.
3. Export, compile, and push the numbers (particle counts, tessellation,
   iteration depth) in the standalone program - via the exported tune knobs
   or by editing the C directly.

A scene that drops frames in the REPL at 500 particles will typically run
thousands in the export without effort - the *Swaying grass field* example
goes from a flatten-capped 135 blades to 9600, shown under
[What the export is free of](#what-the-export-is-free-of).

That division of labor is by design: the REPL is a **launchpad, debugging
aid, and educational environment** - a place to see immediate-mode GL
respond line by line - not a platform to build a complete application on.
The export is the product; the REPL is where it is born.

---

## Fidelity to OpenGL

Two things gl-repl shows you are answers *about* OpenGL that it works out
itself, without asking the driver: the
[state inspector](#inspecting-opengl-state) re-implements the parts of the GL
state machine your program can reach - matrix composition, the lighting
equation, `glPushAttrib` group semantics - and
[attribute scope](#attribute-scope) decides which state each mask bit covers.
Both are claims about the spec, so both are tested as claims rather than
against fixtures. `make gl-tests` runs them as **differential oracles against
a live driver**:

- One `GLCmd` program is driven through *both* the real executor (against a
  real context) and the pure state model; then every row the inspector reports
  is read back with `glGet*` and compared.
- For each `glPushAttrib` bit, both directions are asserted: state the table
  says the bit covers is restored by `glPopAttrib`, and state it says the bit
  does *not* cover is not - so the mapping can be neither too narrow nor too
  broad.

Running those checks on more than one driver can also expose differences
between driver implementations. The differences found in this comparison all
affect `GL_CURRENT_RASTER_COLOR`: the color
[`label()`](#bitmap-text---label) bitmap text is drawn with, latched at the
`glRasterPos` and unchangeable by a later `glColor3f`. Three tested driver
configurations produced these results:

| Deviation | Apple M2 (2.1 Metal) | Mesa 25.2.8 (Intel) | NVIDIA 595.84 |
|---|---|---|---|
| `GL_COLOR_MATERIAL` enabled at the `glRasterPos` call | tracked components light as **zero** | correct | correct |
| Raster position lit from its **object-space** position - wrong light vector under the tested transformed modelview | correct | **wrong** | correct |
| `GL_NORMALIZE` honoured on that path | correct | **ignored** | correct |
| Unlit latch clamped to [0, 1] (GL 2.1 §2.14.6) | clamps | stores **raw** | clamps |

Across these four cases, the observed deviations did not overlap between the
three tested configurations. On each row gl-repl follows the behavior described
by the specification. The driver-specific behavior was characterized by
matching the observed values to six decimals, and those values are recorded in
the test beside the corresponding skip gate. A skip prints in the test output,
so it remains visible when a driver-specific case is not compared. That is what
the inspector's note about a driver computing this cell differently is pointing
at.

These are visible in ordinary scenes, not just in the inspector - lit bitmap
text is drawn through exactly this path. The sharpest case: on Apple's GL a
scene that enables `GL_COLOR_MATERIAL` before its `glRasterPos` gets **black
label text**; see the note under [`label()`](#bitmap-text---label) for the
one-line workaround.

When a bug turns out to be the driver's, it gets reduced to a standalone
program that depends on nothing here and written up with its spec citation in
[`third_party/bugs/`](../third_party/bugs/). The four deviations above are three
reports there (Mesa's two raster-lighting symptoms share one). The two lighting
reproducers check the driver against *itself*: they compare the raster colour
with the colour that same driver gives a vertex under identical state, which
the specification defines as the same computation. The unclamped-colour probe
instead checks the specified [0, 1] range and includes the lit path as a
control. A fourth report was found from a scene rendering differently on Mesa:
retargeting `glColorMaterial` to another face discards the colour the outgoing
face was tracking, turning the *glr-logo* example's exterior black on the
tested Mesa configurations.

The same standard applies to export. The C that `Ctrl+S` writes is compiled
and *run* in the test suite, and its GL call stream is compared against the
REPL executor's - call for call and **argument for argument**, over several
values of `t` run as successive frames. A frozen vertex or a drifted matrix
cell fails the build; what you see in the REPL is what the standalone program
draws.

---

## Scope & Current Limitations

gl-repl currently focuses on fixed-function, immediate-mode geometry. The
following features are intentionally outside that scope so the editor,
visualizations, and exported C all describe the same small language.

- **Geometry is edited through code.** There is no click-to-select,
  drag-a-vertex, or in-scene transform gizmo; viewport input moves only the
  camera. Keeping edits in the code panel preserves one source of truth for
  the scene and its export. Cursor guides, transform guides, and assignment
  plots help connect those lines to their visible results.
- **Textures are not supported.** There is no `glTexCoord`, texture binding,
  or image loading. Texture coordinates, sampling state, image assets, and
  their interactions would substantially broaden the language and its
  visualizations beyond the current focus on geometry, color, lighting, and
  fixed-function state.
- **Shaders and buffered drawing are not supported.** There are no shaders,
  vertex buffers, or draw-array APIs. Immediate-mode `glBegin`/`glEnd` calls
  keep individual source lines directly visible while editing and replaying.
- **Individual lights use presets.** A light theme supplies the four slots'
  positions and colors; scene code enables or disables the slots it needs.
  Per-light editing would require a larger set of position, color, attenuation,
  and spotlight controls, so it remains outside the current REPL language. The
  generated and exported C still exposes the underlying `glLightfv` calls; see
  [Lighting](#lighting).
- **The REPL is not an application framework.** It is intended for authoring,
  inspecting, and learning from scenes. Exported C is the path for scaling a
  scene further or integrating it into a larger program; see [Performance &
  Scope](#performance--scope).

These points describe the current release scope, not commitments about future
versions.

---

## Profiling & Diagnostics

When a scene starts feeling heavy, the built-in profilers show where the
frame goes before you reach for the export.

### The compute profile (Ctrl+G)

**Ctrl+G** cycles the compute profile through Off / FPS / Sections /
Histogram. *FPS* shows only the frame-rate graph. *Sections* adds the full
collapsible per-section timing tree. *Histogram* adds the section-distribution
graph as the final, more expensive diagnostic surface.

![Histogram mode: the section listing (CPU/GPU/Max), the log-log section histograms, and the FPS plot](images/profile-panels.png)

Three floating panels work together:

- **The section listing** (right) breaks the frame into named sections -
  *Render 3D*, *Code Panel*, *Flatten*, and so on, down to the three summary
  rows under the rule. *Frame Time* is the whole frame, start to finish, and it
  splits into the two rows below it: *Frame Work* is everything the frame
  spends producing the image, and *Present* is the rest - the buffer swap and,
  with vsync on, the wait for the next refresh. **Frame Work is the number to
  watch when something feels slow**; Present is the frame's leftover headroom
  rather than a cost, so it is colored the other way round - a long present is
  green, a vanishing one red, because that is the frame running out of slack.
  The **CPU** column is a running average of wall-clock time per frame; the
  **GPU** column comes from asynchronous GL timer queries (no stalls - the
  numbers arrive a few frames late), and reads `--` where the driver lacks
  timer queries or a section emits no GL. **Max** shows the worse of the
  two averages - the number to watch when deciding what to trim. Sections
  that did nothing this frame dim out.
- **The section histograms** (center) overlay every top-level section's
  timing distribution on one graph. Both axes are logarithmic: microsecond
  sections and a 100 ms stall fit on the same plot, so a rare spike shows
  up as a small bump far to the right instead of vanishing into an average.
  Each section keeps its listing color; the legend below maps them. Click a
  legend entry to drop that series from the plot (and from both axes, so
  hiding the fast sections zooms in on what is left); click it again to bring
  it back. **Right-click a legend entry** to plot *only* that series - the
  one-press way to isolate one distribution out of a dozen overlaid ones;
  right-click the soloed entry again to bring the rest back.
  **Rest the pointer on a legend entry** and that series' numbers pop
  up over the plot - sample count, fastest, mean and slowest sample, standard
  deviation, and the total time spent in the section. Those are exact,
  measured values rather than anything read back off the plot, so they stay
  meaningful for a section too fast to separate from the axis' left edge.
- **The FPS plot** (bottom-right) graphs frame rate over the last 10
  seconds, minute, and 10 minutes as three overlaid series, with the
  current rate in the corner.

Histogram mode is the tool for *hitches*: a scene that averages 60 fps but
stutters once a second shows a clean main hump plus a second bump at the
stall duration - and the bump's color names the section responsible. Hovering
that section's legend entry puts a number on the stall: the max is how bad the
worst one got, and the gap between mean and max is how much of an outlier it
is. The histograms and their statistics accumulate from load together (both
reset when you switch examples, or on the panel's `[reset]` control), so leave
the panel up while you reproduce the hiccup.

GPU timing needs timer-query support (GL 3.3 / `ARB_timer_query`, or the
`EXT_timer_query` fallback); `GLR_NO_GPU_PROF=1` disables it explicitly, and
the GPU column then reads `--`.

### Memory, messages, and startup

- **Memory profile** (Ctrl+Shift+B) - a floating panel with the process RSS
  history, a session baseline, and the delta since baseline. Useful for
  confirming a long editing session isn't growing without bound.
- **Message history** - click the button at the right end of the bottom
  message line to review recent status messages (parse errors you dismissed,
  save confirmations, budget warnings).
- **Ctrl+Shift+N** - dump debug state to stdout.
- Startup prints an init trace (`[init +N.NNNs] <phase>`) to stderr - useful
  for locating slow startup phases; `--detailed-prof` adds finer phases.

---

## Music

gl-repl plays background `.mp3`s found at startup, in filename order, from
three places combined:

1. **`./assets`** next to where you run it - override with `--assets <dir>`
   or `GLR_ASSETS_DIR`.
2. **Bundled with the app** - the macOS `gl-repl.app` ships a sample track.
3. **Your music folder** - `~/Library/Application Support/gl-repl/Music` on
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
./gl-repl [file.c | workspace-dir | -]   load a file, directory, or stdin

--example <name|idx>   start on a built-in example (case-insensitive name or 1-based index)
--list-examples        print the built-in examples and exit
--time <secs>          initial animation time t (also GLR_TIME; --time wins)
--window <WxH>         initial window size (default 1200x800)
--export-c <path>      write the session out as standalone C, then exit (no window)
--export-glr <path>    write it in the .glr authoring format instead (no window)
--export-ply <path>    capture frame 1 geometry to PLY, then exit
--export-ply-srgb      decode vertex colors sRGB -> linear during PLY export
--assets <dir>         scan this dir for *.mp3 (also GLR_ASSETS_DIR)
--no-audio             skip audio init entirely
--no-accum             disable the accumulation buffer (AA + motion blur)
--accum                force it on (default: off on software-accum renderers)
--dump-code            print the loaded buffer to stdout
--flat-histogram       print per-function / per-line flat-command costs
                       (where the 8192 budget goes; works with --example)
--detailed-prof        verbose startup timing trace (also GLR_DETAILED_PROF=1)
-h, --help             usage
```

`GLR_TIME` and `GLR_ASSETS_DIR` set the same values as `--time` and
`--assets`; `GLR_NO_GPU_PROF=1` turns off GPU timer-query profiling.

This is the day-to-day set. A second family of `GLR_*` variables exists to pose
the app for scripted screenshots and recordings - parking the cursor, feeding
keystrokes, opening the color picker, pinning the simulation to one tick per
frame. Those, the full flag reference, and headless (no-window) rendering are
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
| Right-click GL command | Show a short description of that command |
| Right-click `var = expr;` | Toggle the [assignment value plot](#plotting-an-assignments-values) for that row |
| Shift+right-click `var = expr;` | Add (or remove) that row as an extra series on the open plot |
| Right-click empty line | Toggle the [OpenGL state inspector](#inspecting-opengl-state) at that point |
| Shift+right-click empty line | Pin (or unpin) that row as the inspector's [comparison basis](#comparing-two-probe-points) |
| Esc | Clear input / close overlay |

### Scene & rendering

| Key | Action |
|---|---|
| Ctrl+T | Play/pause time |
| Ctrl+Shift+T | Reset time to 0 |
| Ctrl+Shift+W | Winding |
| Ctrl+R | Start/stop replay (Ctrl+K jump to cursor) |
| Ctrl+W | Wireframe |
| Ctrl+Shift+D | Depth view (Off / Linear / Scene / Split) |
| Ctrl+Shift+S | Stencil view (Off / Palette / Ramp / Split) |
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
