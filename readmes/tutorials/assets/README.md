# Tutorial GIF assets

Drop the recorded GIFs here, named to match the `<img src="assets/…">` tags in
the tutorial pages. Each tutorial has an HTML comment above its image with a
**Capture:** note describing exactly what to record.

## Shot list

| File | Tutorial | What to capture |
|---|---|---|
| `00-overview.gif` | index | Type the teapot snippet, `Ctrl+T`, it spins (the hook) |
| `01-first-triangle.gif` | 1 | Type the 5 triangle lines, one `;` at a time |
| `01-edit-vertex.gif` | 1 | Re-edit a `glVertex2f` coordinate; corner moves |
| `02-gradient-triangle.gif` | 2 | Per-vertex colors → RGB gradient fill |
| `02-transformed-cube.gif` | 2 | Translate + rotate + `glutSolidCube` |
| `02-transform-guide.gif` | 2 | Cursor on a `glRotatef` line → arc guide |
| `03-spinning-teapot.gif` | 3 | Teapot snippet → `Ctrl+T`; edit the rate live |
| `03-bobbing-torus.gif` | 3 | Torus bobbing on `sin(t)` while spinning |
| `04-cube-ring.gif` | 4 | `for` loop → ring of 8 cubes |
| `04-flower-function.gif` | 4 | `func0` flower; edit body → all petals update |
| `05-tessellation.gif` | 5 | `float n;` drives sphere facets; raise n → smoother |
| `05-slider-drag.gif` | 5 | Drag the `wobble` slider; live pulse grows |
| `06-lighting-on.gif` | 6 | Sphere gains shading as LIGHTING + LIGHT0 enable |
| `06-shininess.gif` | 6 | Raise `GL_SHININESS`; highlight tightens |
| `06-normals.gif` | 6 | `Ctrl+Shift+N` → per-vertex normal arrows |
| `07-replay.gif` | 7 | `Ctrl+R` replay over the sphere ring + HUD |
| `07-save-reload.gif` | 7 | `Ctrl+S` → reload `./sample output.c` *(optional)* |
| `07-ply-export.gif` | 7 | `F11` → confirmation; optional cut to a mesh viewer *(optional)* |

## Recording tips

- Keep clips **5–10s** and loop cleanly (end where you began) so the GIF reads
  as continuous.
- Crop tight to the relevant panel — full-window captures get blurry once
  scaled to ~80% column width.
- For `Ctrl+T` animations, let it run at least one full cycle before looping.
- A consistent window size across clips keeps the page tidy.

> Suggested tools: macOS — [Gifski](https://gif.ski/) or
> `ffmpeg` from a screen recording. Aim for a small palette and ≤ a few MB each.
