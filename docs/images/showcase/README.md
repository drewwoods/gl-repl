# Showcase shot list

Assets referenced by [`SHOWCASE.md`](../../SHOWCASE.md). Each entry below
is a placeholder until captured — the `<!-- PLACEHOLDER -->` comments in
SHOWCASE.md carry the intent and a starting command. Camera angle, fps,
resolution, and duration are all refinable; filenames are the contract.

Headless capture pipeline:

```bash
make gl-repl FREEGLUT_OSMESA=1
scripts/record-gif.sh --example "<name>" --duration 6 --out <slug>   # GIFs
# stills: SIGUSR1 capture or scripts/docs-assets.sh-style staging
```

| File | Example | Notes |
|---|---|---|
| `torus-knot.gif` | Torus knot (animated) | hue cycling along the curve |
| `snowfall.gif` | Snowfall demo (550 particles) | density + motion |
| `parametric-torus.gif` | Parametric torus (nested for) | slow orbit, or a still |
| `recursive-tree.gif` | Recursive triangle tree (func + recursion) | sway via `t` |
| `spirograph.gif` | Animated spirograph curve | |
| `ripple-ring.gif` | Traveling ripple ring | |
| `bezier.png` | Bezier curve with guides | cursor on a control-point line (`GLR_EDIT_LINE`) |
| `de-casteljau.png` | Scratch arrays (de Casteljau curve) | still |
| `orbit-plot.png` | Annotated orbit plot (labels) | the `label()` text is the point |
| `wave-surface.gif` | Animated wave surface (analytic normals) | lighting rolling across the wave |
| `terrain.gif` | Procedural terrain (rand grid + sin ripple) | |
| `lit-cube.png` | Lit cube | still; the default example |
| `grass.gif` | Swaying grass field (rand + t) | |
| `jellyfish.gif` | Jellyfish (glDepthMask translucency) | translucent bell |
| `function-demo.png` | Function demo (named func) | still |
| `function-polygons.png` | Function polygons (args + for) | still |
| `conditional-colors.gif` | Conditional colors (if + t) | |
| `whale.gif` | Whale (particle system + lit model) | flagship scene |
| `stress-test.gif` | Stress test (all features) | |
| `feature-time.gif` | any animated example | Ctrl+T moment: still → moving |
| `feature-sliders.gif` | any with a `float` | slider drag, scene follows (interactive) |
| `feature-ply.png` | Parametric torus | exported `.ply` open in MeshLab/Blender |
| `feature-export-c.png` | any | `output.c` in an editor beside the running standalone binary |

Already covered by existing `docs/images/` assets (no capture needed):
`animated-ring.gif`, `glow-sprites.png`, `labels-orrery.png`, `glu-tess.png`,
`transform-stress.png`, `replay.gif`, `xform-guide.gif`, `variable-panel.png`.
