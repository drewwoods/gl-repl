# Showcase shot list

Assets referenced by [`SHOWCASE.md`](../../SHOWCASE.md). These are generated
by [`scripts/docs-assets.sh`](../../../scripts/docs-assets.sh) from the
**native** build (`make gl-repl`) - the real GPU driver, so true colors and
MSAA. (The old OSMesa software-rasterizer path mis-rendered the grid, so it
was retired.) The filename stem is the script's asset name, prefixed `sc-`.
Camera angle, fps, resolution, and duration are all refinable in that script;
filenames are the contract.

Capture pipeline (needs a display - each capture opens a window briefly):

```bash
make gl-repl
scripts/docs-assets.sh sc-torus-knot     # one asset
scripts/docs-assets.sh                    # everything (core + showcase)
scripts/docs-assets.sh --list             # asset names
```

| File | Asset | Example |
|---|---|---|
| `torus-knot.gif` | `sc-torus-knot` | Torus knot (animated) |
| `snowfall.gif` | `sc-snowfall` | Snowfall demo (550 particles) |
| `parametric-torus.png` | `sc-parametric-torus` | Parametric torus (nested for) - still (static geometry) |
| `recursive-tree.gif` | `sc-recursive-tree` | Recursive triangle tree (func + recursion) |
| `spirograph.gif` | `sc-spirograph` | Animated spirograph curve |
| `ripple-ring.gif` | `sc-ripple-ring` | Traveling ripple ring |
| `bezier.png` | `sc-bezier` | Bezier curve with guides (draws its own control points) |
| `bubble-sort.gif` | `sc-bubble-sort` | Bubble sort (scratch arrays) |
| `orbit-plot.png` | `sc-orbit-plot` | Annotated orbit plot (labels) |
| `wave-surface.gif` | `sc-wave-surface` | Animated wave surface (analytic normals) |
| `ringed-planet.gif` | `sc-ringed-planet` | Ringed planet (nebula skies) - replaces the retired "Procedural terrain" |
| `gl-repl-logo.png` | `sc-gl-repl-logo` | gl-repl Logo |
| `grass.gif` | `sc-grass` | Swaying grass field (rand + t) |
| `jellyfish.gif` | `sc-jellyfish` | Jellyfish (glDepthMask translucency) |
| `function-demo.png` | `sc-function-demo` | Function demo (named func) |
| `function-polygons.png` | `sc-function-polygons` | Function polygons (args + for) |
| `conditional-colors.gif` | `sc-conditional-colors` | Conditional colors (if + t) |
| `whale.gif` | `sc-whale` | Whale (particle system + lit model) |
| `stress-test.gif` | `sc-stress-test` | Dusk lighthouse atoll (stress test) |
| `planar-shadows.gif` | `sc-planar-shadows` | Planar shadows (glMultMatrixf) |
| `feature-time.gif` | `sc-feature-time` | Conditional colors (if + t) - a t-driven clip |

Stand-ins (the ideal shot needs an external tool we can't drive headlessly):

| File | Asset | Stand-in / ideal shot |
|---|---|---|
| `feature-ply.png` | `sc-feature-ply` | Reuses the parametric-torus scene. Ideal: its exported `.ply` in MeshLab/Blender. |
| `feature-export-c.png` | `sc-feature-export-c` | Reuses a full-UI scene ("it's all code in the panel"). Ideal: `output.c` in an editor beside the running standalone binary. |
| `feature-sliders.gif` | _(none)_ | SHOWCASE points at `images/variable-panel.png`. Ideal: a variable-panel slider drag with the scene responding live. |

Already covered by existing `docs/images/` assets (no showcase capture needed):
`animated-ring.gif`, `glow-sprites.png`, `labels-orrery.png`, `glu-tess.png`,
`transform-stress.png`, `replay.gif`, `xform-guide.gif`, `variable-panel.png`.
