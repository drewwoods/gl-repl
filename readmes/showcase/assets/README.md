# Showcase GIF assets

Drop recorded GIFs here, named to match the `<img src="assets/…">` tags in
[`../README.md`](../README.md).

Every scene is built in. Recording one is two steps:

```bash
./sample --example "<name>"      # load it (Ctrl+T if animated)
# record ~6s, loop cleanly
```

Run `./sample --list-examples` for the canonical names/indices — the table
below uses the names so it survives reordering.

## Shot list

| File | `--example` name | Animated? |
|---|---|---|
| `torus-knot.gif` | Torus knot (animated) | yes — `Ctrl+T` |
| `snowfall.gif` / `snowfall-thumb.gif` | Snowfall demo (550 particles) | yes |
| `parametric-torus.gif` | Parametric torus (nested for) | static (orbit the camera) |
| `recursive-tree.gif` | Recursive triangle tree (func + recursion) | yes |
| `spirograph.gif` | Animated spirograph curve | yes |
| `ripple-ring.gif` | Traveling ripple ring | yes |
| `animated-ring.gif` | Animated ring (for + t) | yes |
| `bezier.gif` | Bezier curve with guides | static |
| `de-casteljau.gif` | Scratch arrays (de Casteljau curve) | static |
| `orbit-plot.gif` | Annotated orbit plot (labels) | static |
| `wave-surface.gif` | Animated wave surface (analytic normals) | yes |
| `terrain.gif` | Procedural terrain (rand grid + sin ripple) | yes |
| `lit-cube.gif` | Lit cube | static |
| `glow-sprites.gif` | Glow sprites (blend + point attenuation) | yes |
| `function-demo.gif` | Function demo (named func) | static |
| `function-polygons.gif` | Function polygons (args + for) | static |
| `conditional-colors.gif` | Conditional colors (if + t) | yes |
| `tessellator.gif` | GLU tessellator (concave arrow) | static |
| `transform-stress.gif` | Transform stress (translate/rotate/scale guides) | static |
| `stress-test.gif` | Stress test (all features) | yes |

### Interaction GIFs (not a single `--example`)

| File | What to capture |
|---|---|
| `feature-time.gif` | `Ctrl+T` toggling time on any animated example |
| `feature-replay.gif` | `Ctrl+R` replay stepping through draws (fade trail + HUD) |
| `feature-sliders.gif` | Dragging a variable-panel slider; scene responds live |
| `feature-guides.gif` | Cursor on a `glRotatef`/`glTranslatef` line → on-screen guide |
| `feature-ply.gif` | `F11` export, optional cut to the `.ply` in MeshLab/Blender |
| `feature-export-c.gif` | `Ctrl+S`, then `./sample output.c` reloading the scene |

## Recording tips

- Keep clips **~6s** and loop cleanly; for `t`-driven scenes let one full cycle
  pass before the loop point.
- Crop tight to the 3D viewport for gallery thumbnails (they render at 70–100%
  column width).
- Keep a consistent window size across clips so the gallery rows line up.
- Small palette, ≤ a few MB each. Suggested: [Gifski](https://gif.ski/) or
  `ffmpeg` from a screen recording.
