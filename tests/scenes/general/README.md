# GLR General Test Scene Corpus

This directory contains general `.glr` REPL example scene files designed for runtime loading, testing, and validation in `gl-repl`.

Like [`tests/scenes/stress/`](../stress/README.md), this corpus can be loaded dynamically at runtime via `--examples-dir tests/scenes/general`.

## Scene Catalog

| File | Primary Features | Description |
|---|---|---|
| `orbiting-cubes-solar-system.glr` | 3D, Hierarchical, Lighting | Multi-body solar system with orbiting cubes, spheres, and moons using push/pop matrix stacks and materials. |
| `procedural-terrain-grid.glr` | 3D, Curves & surfaces, Math | Dynamic wave heightfield mesh built with triangle strips, math expressions (`sin`, `cos`), and height color gradients. |
| `particle-fireworks-fountain.glr` | 3D, Particles, Blending | Particle fountain system using point sizing, dynamic velocity trajectories, alpha fading, and additive blending (`glBlendFunc`). |
| `geometric-polyhedra-showcase.glr` | 3D, Polygons, Materials | Ring of GLUT solids (teapot, cube, sphere, torus, cone) showcasing lighting and material shininess. |
| `rainbow-spiral-tunnel.glr` | 3D, Lines, Parametric curves | Animated 3D rainbow ribbon spiral tunnel generated parametrically via quad strips and color math. |
| `interactive-color-matrix.glr` | 3D, Polygons, Math | Grid matrix of animated bar columns with dynamic height wave motion and color thresholding. |
| `lissajous-3d-knot-ribbon.glr` | 3D, Lines, Parametric curves | 3D Lissajous curve knot ribbon generated with multi-frequency trig math and color gradients. |
| `audio-spectrum-equalizer-3d.glr` | 3D, Polygons, Animation | Dynamic 3D equalizer bar columns with height-based materials and animated frequency simulation. |
| `metaball-lava-lamp-simulation.glr` | 3D, Particles, Lighting | Floating lava lamp metaball particle cloud with dynamic movement trajectories and material colors. |
| `nested-fractal-gasket-tree.glr` | 3D, Lines, Recursion | Swaying 3D recursive fractal tree using named functions, arguments, line width scaling, and push/pop matrices. |
| `cyberpunk-grid-horizon.glr` | 3D, Lines, Showcase | Synthwave perspective grid highway horizon with scrolling transverse lines and retro sun disk. |
| `clip-plane-hollow-sphere.glr` | 3D, Clipping, Materials | Concentric outer/inner spheres with dynamic clipping plane (`glClipPlane`) revealing internal geometry. |
| `dynamic-dna-helix.glr` | 3D, Curves & surfaces, Animation | Rotating double helix of colored spheres and rungs using sine/cosine functions and light/material properties. |
| `torus-knot-slinky.glr` | 3D, Hierarchical, Lighting | Complex torus knot curve rendered as an animated, color-shifting beaded slinky of spheres. |
| `stardust-spiral-galaxy.glr` | 3D, Particles, Blending | Dual-arm spiral galaxy of glowing, blending dust particles whose speed and radius vary over time. |
| `rubiks-cube-rotation.glr` | 3D, Hierarchical, Lighting | A true 3x3x3 Rubik's cube: one face turns at a time on the lattice, cubies carry their stickers through the permutation, and the 8-move sequence winds up and unwinds back to solved. |
| `pendulum-wave-machine.glr` | 3D, Particles, Animation | A row of 14 pendulums with slightly different lengths and swing frequencies, creating the classic pendulum wave illusion. |
| `moebius-strip-twist.glr` | 3D, Curves & surfaces, Parametric curves | Parametrically generated Möbius strip with animated rainbow color shifting and wireframe edges. |
| `interlocking-gear-system.glr` | 3D, Hierarchical, Lighting | Three interlocking, counter-rotating metallic gears (copper, steel-blue, gold) with gear teeth and axel hubs. |
| `crystalline-cave-formation.glr` | 3D, Polygons, Materials | Dynamic cluster of translucent crystal spires radiating from a central geode structure with high specular highlights. |
| `wave-interference-pattern.glr` | 3D, Particles, Math | Circular wave superposition rendering constructive and destructive interference fringes on a 2D point grid. |
| `ferris-wheel-ride.glr` | 3D, Hierarchical, Lighting | Rotating Ferris wheel with 8 uniquely-colored gondola cubes that counter-rotate to stay upright. |
| `spiral-staircase-tower.glr` | 3D, Hierarchical, Lighting | Helical stone staircase tower with 30 steps, central pillar, and connecting handrail. |
| `aurora-borealis-curtain.glr` | 3D, Curves & surfaces, Blending | Animated layered ribbons of waving auroras with green/cyan/magenta gradients and stars. |
| `atom-electron-orbits.glr` | 3D, Hierarchical, Lighting | Bohr model atom featuring a glowing nucleus and five electrons orbiting along tilted orbital rings. |
| `kaleidoscope-mandala.glr` | 3D, Polygons, Animation | An 8-fold symmetric rotating mandala with pulsing triangle fans, outer petals, and vibrant color cycles. |
| `basic-shapes.glr` | General, Basics, 3D | Rotating neon teal cube and amber sphere. |
| `helix-spiral.glr` | General, Math, Lines | 3D helix spiral via `for` loop with a teal-to-magenta color sweep. |
| `translucent-blend.glr` | General, Blending, 3D | Two overlapping translucent quads using `GL_BLEND`. |
| `line-stipple-patterns.glr` | General, Stipple, Lines | Four stipple masks with different factors via `glLineStipple`. |
| `fog-depth-range.glr` | General, Fog, 3D | Linear fog (`GL_LINEAR`) with a retreating row of spheres. |
| `material-properties.glr` | General, Lighting, 3D | Specular/shininess on a teapot under `GL_LIGHT0`. |
| `polygon-modes.glr` | General, PolygonMode, 3D | Three solids side-by-side in `GL_FILL`, `GL_LINE`, and `GL_POINT`. |
| `depth-mask-layers.glr` | General, DepthMask, Blending | Opaque cube + translucent sphere shell using `glDepthMask`. |
| `recursive-sierpinski-2d.glr` | General, Recursion, 2D | Recursive Sierpinski triangle (2D) using a function body. |
| `parametric-wave-grid.glr` | General, Math, Loops, 3D | Animated wave surface with per-quad color from nested `for` loops. |
| `clip-plane-slice.glr` | General, ClipPlane, 3D | `glClipPlane` bisects a torus, with a disc drawn at the cut face. |
| `shade-model-flat-smooth.glr` | General, Shading, 3D | Side-by-side comparison of `GL_FLAT` vs `GL_SMOOTH` shading. |
| `color-mask-channels.glr` | General, ColorMask, State | Channel-selective rendering (`glColorMask`) for Red, Green, and Blue. |
| `nested-transform-solar.glr` | General, Transforms, 3D | Hierarchical matrix transform propagation (Sun -> Planet -> Moon). |
| `polygon-offset-overlay.glr` | General, PolygonOffset, 3D | Wireframe overlay on filled solid without z-fighting via `glPolygonOffset`. |
| `multi-light-rig.glr` | General, Lighting, 3D | Multiple positional lights (`GL_LIGHT0`, `GL_LIGHT1`) with distinct colors. |
| `stencil-mask-window.glr` | General, Stencil, State | Passing rendering exclusively inside a `glStencilFunc`-masked quad. |
| `multiple-planar-shadow-projections.glr` | General, Stencil, Shadows, Lighting | Moving virtual-light planar projections from a cube onto the floor, back wall, and left wall, combined through stencil. |
| `vertex-normals-lighting.glr` | General, Normals, Lighting | Explicit per-vertex `glNormal3f` assignments on custom geometry. |
| `fog-exponential.glr` | General, Fog, 3D | Exponential squared fog (`GL_EXP2`) against a deep perspective grid. |
| `stencil-shadow-volume.glr` | General, Stencil, Shadows, Lighting | True stencil shadow volume: the cube's silhouette from a moving point light is extruded into a closed hull, and z-pass INCR/DECR stencil counting shadows the floor and both walls in one pass. |
| `stencil-shadow-volume-zfail.glr` | General, Stencil, Shadows, Lighting | The z-fail (Carmack's reverse) twin of the above: identical hull, counting moved to the depth-fail slot, so the light can orbit fully and the shadow stays correct even when the camera is inside the volume. |
| `stencil-shadow-volumes-multi.glr` | General, Stencil, Shadows, Lighting | Three differently-sized, independently-spinning cubes casting into one stencil tally, showing that z-fail counting generalises to N occluders and that overlapping shadows stay a single shade. |
| `stencil-shadow-volumes-infinite.glr` | General, Stencil, Shadows, glVertex4f | The multi-occluder scene with the extrusion done in homogeneous coordinates: `glVertex4f(p - light, 0)` sends each silhouette vertex to infinity, deleting the extrusion-length knob and every local in the two hull helpers. |


## How to Run & Validate

### 1. Load as an Example Catalog
```bash
./gl-repl --examples-dir tests/scenes/general
./gl-repl --examples-dir tests/scenes/general --list-examples
```

### 2. Load Individual Scene Files
```bash
./gl-repl tests/scenes/general/orbiting-cubes-solar-system.glr
./gl-repl tests/scenes/general/procedural-terrain-grid.glr
./gl-repl tests/scenes/general/particle-fireworks-fountain.glr
./gl-repl tests/scenes/general/geometric-polyhedra-showcase.glr
./gl-repl tests/scenes/general/rainbow-spiral-tunnel.glr
./gl-repl tests/scenes/general/interactive-color-matrix.glr
./gl-repl tests/scenes/general/lissajous-3d-knot-ribbon.glr
./gl-repl tests/scenes/general/audio-spectrum-equalizer-3d.glr
./gl-repl tests/scenes/general/metaball-lava-lamp-simulation.glr
./gl-repl tests/scenes/general/nested-fractal-gasket-tree.glr
./gl-repl tests/scenes/general/cyberpunk-grid-horizon.glr
./gl-repl tests/scenes/general/clip-plane-hollow-sphere.glr
./gl-repl tests/scenes/general/dynamic-dna-helix.glr
./gl-repl tests/scenes/general/torus-knot-slinky.glr
./gl-repl tests/scenes/general/stardust-spiral-galaxy.glr
./gl-repl tests/scenes/general/rubiks-cube-rotation.glr
./gl-repl tests/scenes/general/pendulum-wave-machine.glr
./gl-repl tests/scenes/general/moebius-strip-twist.glr
./gl-repl tests/scenes/general/interlocking-gear-system.glr
./gl-repl tests/scenes/general/crystalline-cave-formation.glr
./gl-repl tests/scenes/general/wave-interference-pattern.glr
./gl-repl tests/scenes/general/ferris-wheel-ride.glr
./gl-repl tests/scenes/general/spiral-staircase-tower.glr
./gl-repl tests/scenes/general/aurora-borealis-curtain.glr
./gl-repl tests/scenes/general/atom-electron-orbits.glr
./gl-repl tests/scenes/general/kaleidoscope-mandala.glr
./gl-repl tests/scenes/general/basic-shapes.glr
./gl-repl tests/scenes/general/helix-spiral.glr
./gl-repl tests/scenes/general/translucent-blend.glr
./gl-repl tests/scenes/general/line-stipple-patterns.glr
./gl-repl tests/scenes/general/fog-depth-range.glr
./gl-repl tests/scenes/general/material-properties.glr
./gl-repl tests/scenes/general/polygon-modes.glr
./gl-repl tests/scenes/general/depth-mask-layers.glr
./gl-repl tests/scenes/general/recursive-sierpinski-2d.glr
./gl-repl tests/scenes/general/parametric-wave-grid.glr
./gl-repl tests/scenes/general/clip-plane-slice.glr
./gl-repl tests/scenes/general/shade-model-flat-smooth.glr
./gl-repl tests/scenes/general/color-mask-channels.glr
./gl-repl tests/scenes/general/nested-transform-solar.glr
./gl-repl tests/scenes/general/polygon-offset-overlay.glr
./gl-repl tests/scenes/general/multi-light-rig.glr
./gl-repl tests/scenes/general/stencil-mask-window.glr
./gl-repl tests/scenes/general/multiple-planar-shadow-projections.glr
./gl-repl tests/scenes/general/vertex-normals-lighting.glr
./gl-repl tests/scenes/general/fog-exponential.glr
./gl-repl tests/scenes/general/stencil-shadow-volume.glr
./gl-repl tests/scenes/general/stencil-shadow-volume-zfail.glr
./gl-repl tests/scenes/general/stencil-shadow-volumes-multi.glr
./gl-repl tests/scenes/general/stencil-shadow-volumes-infinite.glr

```
