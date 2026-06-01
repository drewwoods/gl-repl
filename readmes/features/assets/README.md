# Feature-showcase GIF assets

Drop recorded GIFs here, named to match the `<img src="assets/…">` tags in
[`../README.md`](../README.md). These are **interactions** — record the action,
not a still frame.

## Shot list

### Guides (the headline)

| File | Capture |
|---|---|
| `guide-transform-overview.gif` | Cursor on a `glRotatef` line → the arc with its sweeping pulse dot (orbit so it's clearly 3D) |
| `guide-translate.gif` | Cursor on `glTranslatef(2,0,0)` → red shaft + 4-fin arrowhead along world X |
| `guide-rotate.gif` | Cursor on a `glRotatef` with the pivot off-axis so the arc sweeps visibly |
| `guide-scale.gif` | Cursor on a `glScalef` at the origin → the 3-axis gizmo with per-axis pulsing arrows |
| `guide-world-vs-frame.gif` | Same translate line; flip **Xform guide mode** World ↔ Frame; the anchor jumps |
| `guide-vertex-preview.gif` | Edit a `glVertex3f`'s coords → marker follows; add `t` → it animates |
| `guide-normal.gif` | Cursor on a `glNormal3f` line → arrow at the anchor vertex; negate a component, it flips |

### Editing

| File | Capture |
|---|---|
| `feature-color-picker.gif` | Click a `glColor3f` swatch → picker opens → drag a slider → source + scene update |
| `feature-autocomplete.gif` | Type `glEna` → Tab → ghost completes; show the enum-slot constant list + a param hint |

### Seeing the scene

| File | Capture |
|---|---|
| `feature-replay.gif` | `Ctrl+R` on a looped scene; draws appear one at a time with the fade trail + HUD |
| `feature-sliders.gif` | Drag a `float` slider on a running animation; the effect scales live |
| `overlay-vertex-labels.gif` | `F5` → numbered vertices appear |
| `overlay-normals.gif` | `Ctrl+Shift+N` → per-vertex normal arrows |
| `overlay-outlines.gif` | `Ctrl+Shift+E` → vertex outlines / points |
| `overlay-wireframe.gif` | `Ctrl+G` → wireframe on a solid |
| `overlay-lights.gif` | `Ctrl+Shift+L` → light position markers |
| `overlay-grid-axes.gif` | `F3` / `F4` → cycle grid + axes themes |
| `overlay-backdrop.gif` | `F6` → cycle backdrops (e.g. cityscape) |
| `overlay-viewmode.gif` | `Ctrl+Shift+V` → 2D ortho ↔ 3D perspective |

### Camera & export

| File | Capture |
|---|---|
| `feature-camera.gif` | Orbit + zoom, then `Ctrl+Shift+C` for the eased reset |
| `feature-ply.gif` | `F11` export → status confirms; optional cut to the `.ply` in MeshLab/Blender |
| `feature-export-c.gif` | `Ctrl+S`, then `./sample output.c` reloading the same scene |
| `feature-scenes.gif` | `F12` cycling scenes; the scene-tab strip updating |

## Tips

- A good guide GIF needs a **clearly 3D** camera angle so the arc/arrow reads —
  orbit a little before recording.
- For "as you type" previews, record the **typing/editing** so the marker is
  visibly tracking your edits.
- Keep clips ~6s, loop cleanly, crop tight to the viewport, consistent window
  size. Small palette, ≤ a few MB. [Gifski](https://gif.ski/) or `ffmpeg`.

> Any built-in example is a quick stage for these — e.g. load
> `./sample --example "Transform stress (translate/rotate/scale guides)"` to
> show the transform guides on a scene built for them.
