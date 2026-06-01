<div align="center">

<img src="../assets/hero-r11c.svg" alt="gl-repl feature showcase" width="100%">

<br>

# Feature Showcase

<sub>The REPL isn't just a renderer — it <i>explains</i> the scene as you build it.<br>On-screen guides, live previews, and interactive panels, each shown below.</sub>

</div>

---

> [!NOTE]
> **Capturing these:** every GIF is an interaction, not a still. The capture
> note under each one says exactly what to do on screen. Filenames match the
> `<img>` tags; full shot list in [`assets/README.md`](assets/README.md).

---

## ✦ Transform guides

Park your cursor on a committed `glTranslatef`, `glRotatef`, or `glScalef`
line and the REPL draws **what that line does**, in 3D, over the scene. The
guide only appears when the line parsed cleanly and your current input matches
the committed source — so it tracks the line you're actually reasoning about.

All three share one **"axes pulse"** visual language: a dim base shape with a
bright dot traveling along it and a short fading trail, so the *direction* of
the motion reads at a glance. The color is derived from the command's own
vector.

<!-- TODO: GIF — cursor on a glRotatef(t*30, 0, 1, 0) line; the green rotation arc with the pulse dot sweeping along it. Orbit the camera slightly so the arc is clearly 3D. -->
<div align="center">
<img src="assets/guide-transform-overview.gif" alt="Transform guide arc drawn over the scene" width="78%">
<br><sub><i>Capture: cursor on a <code>glRotatef</code> line → the arc with its sweeping pulse dot.</i></sub>
</div>

| Guide | Reads as | Color encodes |
|---|---|---|
| **Translate** | a shaft + 4-fin pyramid arrowhead to the destination | `(\|tx\|,\|ty\|,\|tz\|)` → a pure-axis move is a pure axis color |
| **Rotate** | an arc (sampled by Rodrigues) with an axis stub through the pivot | `(\|ax\|,\|ay\|,\|az\|)` → Y-rotation reads green, X reads red |
| **Scale** | an arrow from the "before" point to the scaled result | `(\|sx-1\|,\|sy-1\|,\|sz-1\|)` → highlights which axes deviate from 1 |

<table>
<tr>
<td width="33%" align="center">

<!-- TODO: GIF — cursor on glTranslatef(2,0,0); red shaft+arrowhead along world X. -->
<img src="assets/guide-translate.gif" alt="Translate guide" width="100%">

**Translate**
<br><sub>axis-colored shaft + arrowhead</sub>

</td>
<td width="33%" align="center">

<!-- TODO: GIF — cursor on a glRotatef where the pivot is off-axis so the arc sweeps visibly. -->
<img src="assets/guide-rotate.gif" alt="Rotate guide arc" width="100%">

**Rotate**
<br><sub>swept arc + axis stub</sub>

</td>
<td width="33%" align="center">

<!-- TODO: GIF — cursor on glScalef at origin → the 3-axis gizmo with per-axis pulsing arrows. -->
<img src="assets/guide-scale.gif" alt="Scale guide gizmo" width="100%">

**Scale**
<br><sub>arrow, or a 3-axis gizmo at origin</sub>

</td>
</tr>
</table>

### World vs. Frame anchoring

OpenGL applies transforms in **reverse** source order, so a line operates on
the point the *later* lines already placed. The guide starts at that "before"
point — and the **Xform guide mode** toggle controls where it's anchored:

- **World** *(default)* — drawn at the world origin; the strict reverse-order
  reading, independent of surrounding transforms.
- **Frame** — anchored where the pre-cursor modelview has carried the origin,
  so the guide lines up visually with geometry drawn by earlier blocks.

<!-- TODO: GIF — same glTranslatef line, toggle Xform guide mode World <-> Frame; the arrow's anchor jumps to match the rendered geometry. -->
<div align="center">
<img src="assets/guide-world-vs-frame.gif" alt="Switching transform guide between World and Frame anchoring" width="78%">
<br><sub><i>Capture: a scene with a pre-cursor translate; flip <b>Xform guide mode</b> and watch the anchor move.</i></sub>
</div>

<sub>Toggle guides entirely with <b>F8</b> (the <b>Vertex guides</b> config item).</sub>

---

## ✦ Geometry guides — see the vertex as you type

Before you even commit, the cursor line previews **where its geometry lands**.
Type a `glVertex3f(...)` and a marker shows the point in the scene; the guide
re-evaluates every frame, so an animated coordinate (anything in `t`) moves
the marker live. It resolves loop- and function-local values from the flattened
program, so a vertex inside a `funcN` body lands in the right place — not at the
local origin.

<!-- TODO: GIF — typing/editing a glVertex3f line; a marker tracks the point in 3D as the numbers change. Show an animated coord (uses t) so the marker moves on its own. -->
<div align="center">
<img src="assets/guide-vertex-preview.gif" alt="Vertex preview marker tracking the cursor line" width="78%">
<br><sub><i>Capture: edit a <code>glVertex3f</code>'s coordinates; the marker follows. Then add <code>t</code> and watch it animate.</i></sub>
</div>

For a `glNormal3f` line the guide draws the **normal arrow** anchored at the
live vertex it applies to — handy for spotting a flipped or missing normal that
would light wrong.

<!-- TODO: GIF — cursor on a glNormal3f line; the normal arrow drawn at the anchor vertex, flipping direction as you negate a component. -->
<div align="center">
<img src="assets/guide-normal.gif" alt="Normal-vector guide at the anchor vertex" width="78%">
<br><sub><i>Capture: cursor on a <code>glNormal3f</code> line; negate a component, the arrow flips.</i></sub>
</div>

---

## ✦ Inline color, picked visually

`glColor3f` / `glColor4f` lines carry an **inline swatch** in the code panel.
Click it to open a floating **color picker**; drag the sliders and the value is
written straight back into the source line.

<!-- TODO: GIF — click the swatch beside a glColor3f line, the picker opens, drag a slider, the source numbers and the rendered color update together. -->
<div align="center">
<img src="assets/feature-color-picker.gif" alt="Inline color swatch opening a floating color picker" width="78%">
<br><sub><i>Capture: click a <code>glColor3f</code> swatch → drag a slider → source + scene update live.</i></sub>
</div>

---

## ✦ Autocomplete that knows the API

Press **Tab** and the editor completes command names with **ghost text**, shows
a **parameter hint** below the cursor, and — inside an enum argument — offers
the **valid GL constants for that slot** (e.g. the right tokens after
`glEnable(`).

<!-- TODO: GIF — typing `glEna` -> Tab -> ghost completes to glEnable(, the enum-slot list shows GL_DEPTH_TEST/GL_LIGHTING/... ; also show a func param hint. -->
<div align="center">
<img src="assets/feature-autocomplete.gif" alt="Autocomplete ghost text, parameter hint, and enum-slot constants" width="78%">
<br><sub><i>Capture: type <code>glEna</code> → Tab → ghost + the slot's GL-constant list; then a func's param hint.</i></sub>
</div>

---

## ✦ Replay — watch the scene draw itself

**`Ctrl+R`** steps through the flattened command stream one call at a time.
Geometry up to the current step renders; older geometry fades on a trail as new
calls appear, and a HUD shows the program counter. The fastest way to find the
exact call where a scene goes wrong.

<!-- TODO: GIF — Ctrl+R on a multi-part scene (e.g. the cube ring); calls paint in one at a time with the fade trail and HUD visible. -->
<div align="center">
<img src="assets/feature-replay.gif" alt="Replay stepping through draw calls with a fade trail and HUD" width="78%">
<br><sub><i>Capture: <code>Ctrl+R</code> on a looped scene; draws appear one at a time with the trail + HUD.</i></sub>
</div>

---

## ✦ Live variable sliders

Declared `float` variables show up in a **variable panel** as sliders. Drag one
and the scene responds immediately — no re-typing, no commit. The slider edits
the same variable your source refers to.

<!-- TODO: GIF — drag a slider for a declared float that feeds an animation; the effect scales live while Ctrl+T runs. -->
<div align="center">
<img src="assets/feature-sliders.gif" alt="Dragging a variable-panel slider to tune the scene live" width="78%">
<br><sub><i>Capture: run an animated scene with a <code>float</code> param → drag its slider; the effect grows.</i></sub>
</div>

---

## ✦ Scene overlays — toggle what you need to see

A row of toggles turns the diagnostic overlays on and off, so you can debug
geometry as data without leaving the scene.

<table>
<tr>
<td width="25%" align="center">

<!-- TODO: GIF — F5 vertex labels toggling on; numbered vertices appear. -->
<img src="assets/overlay-vertex-labels.gif" alt="Vertex index labels" width="100%">

**Vertex labels** · `F5`
<br><sub>number every vertex</sub>

</td>
<td width="25%" align="center">

<!-- TODO: GIF — Ctrl+Shift+N normal vectors toggling on. -->
<img src="assets/overlay-normals.gif" alt="Per-vertex normal arrows" width="100%">

**Normals** · `Ctrl+Shift+N`
<br><sub>arrow per vertex</sub>

</td>
<td width="25%" align="center">

<!-- TODO: GIF — Ctrl+Shift+E vertex outlines / vertex points. -->
<img src="assets/overlay-outlines.gif" alt="Vertex outlines and points" width="100%">

**Outlines / points** · `Ctrl+Shift+E`
<br><sub>edges + vertex dots</sub>

</td>
<td width="25%" align="center">

<!-- TODO: GIF — Ctrl+G wireframe toggle on a solid. -->
<img src="assets/overlay-wireframe.gif" alt="Wireframe mode" width="100%">

**Wireframe** · `Ctrl+G`
<br><sub>see the topology</sub>

</td>
</tr>
<tr>
<td align="center">

<!-- TODO: GIF — Ctrl+Shift+L light indicators; markers appear where lights are. -->
<img src="assets/overlay-lights.gif" alt="Light position indicators" width="100%">

**Light indicators** · `Ctrl+Shift+L`
<br><sub>where each light sits</sub>

</td>
<td align="center">

<!-- TODO: GIF — F3/F4 cycling grid + axes themes. -->
<img src="assets/overlay-grid-axes.gif" alt="Grid and axes themes" width="100%">

**Grid / axes** · `F3` / `F4`
<br><sub>reference frame themes</sub>

</td>
<td align="center">

<!-- TODO: GIF — F6 backdrop cycling (e.g. cityscape). -->
<img src="assets/overlay-backdrop.gif" alt="Backdrop modes" width="100%">

**Backdrop** · `F6`
<br><sub>scene backdrops</sub>

</td>
<td align="center">

<!-- TODO: GIF — Ctrl+Shift+V toggling 2D ortho <-> 3D perspective. -->
<img src="assets/overlay-viewmode.gif" alt="2D / 3D view-mode toggle" width="100%">

**2D / 3D** · `Ctrl+Shift+V`
<br><sub>ortho ↔ perspective</sub>

</td>
</tr>
</table>

---

## ✦ Camera & focus

Orbit, pan, and zoom with the mouse; the camera has momentum. **`Ctrl+Shift+O`**
eases the orbit target to the origin, **`Ctrl+Shift+C`** resets the camera, and
**`Ctrl+Shift+R`** toggles auto-rotate.

<!-- TODO: GIF — orbit/zoom with the mouse, then Ctrl+Shift+C eases the camera back to default. -->
<div align="center">
<img src="assets/feature-camera.gif" alt="Orbit, zoom, and eased camera reset" width="78%">
<br><sub><i>Capture: orbit + zoom around a scene, then <code>Ctrl+Shift+C</code> for the eased reset.</i></sub>
</div>

---

## ✦ Take it with you

<table>
<tr>
<td width="33%" align="center">

<!-- TODO: GIF — F11 export; status confirms; optional cut to the .ply in a viewer. -->
<img src="assets/feature-ply.gif" alt="PLY mesh export" width="100%">

**`F11` → `.ply`**
<br><sub>one feedback pass captures everything on screen</sub>

</td>
<td width="33%" align="center">

<!-- TODO: GIF — Ctrl+S, then ./sample output.c reloading the same scene. -->
<img src="assets/feature-export-c.gif" alt="Standalone C export and reload" width="100%">

**`Ctrl+S` → `output.c`**
<br><sub>standalone, compilable C that round-trips</sub>

</td>
<td width="33%" align="center">

<!-- TODO: GIF — F12 cycling scene tabs / examples; the tab strip updating. -->
<img src="assets/feature-scenes.gif" alt="Scene tabs and workspaces" width="100%">

**Scenes & workspaces**
<br><sub>multiple scenes, saved as a folder of `*.c`</sub>

</td>
</tr>
</table>

---

<div align="center">

<sub>· · ·</sub>

<sub>more: <a href="../showcase/README.md">example gallery</a> · <a href="../tutorials/README.md">tutorials</a></sub>

<br>

<sub>READMEs: <a href="../README-blueprint.md">blueprint</a> · <a href="../README-minimal.md">minimal</a> · <a href="../README-manpage.md">man page</a> · <a href="../README-cookbook.md">cookbook</a> · <a href="../README-field-guide.md">field guide</a></sub>

<br>

<sub>see also <a href="../../README.md">README</a> · <a href="../../ARCHITECTURE.md">ARCHITECTURE.md</a></sub>

</div>
