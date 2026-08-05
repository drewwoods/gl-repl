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
```
