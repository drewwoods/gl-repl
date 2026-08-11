# Third-party bug reports

Reduced reproducers for bugs traced to something *outside* this tree — a GL
driver, a system library, a compiler. Each bug gets a `<slug>.md` write-up and,
where possible, a self-contained `<slug>.c` that depends on nothing from this
project.

Kept in-repo because the reductions are expensive to rediscover and because a
scene here may have to carry a workaround until the upstream fix reaches the
distros we build on. When a report is filed upstream, add the issue link to the
top of its `.md`.

Nothing here is compiled by the build. These are standalone; each file's header
comment carries its own build line.

| Report | Component | Affects |
|---|---|---|
| [`mesa-colormaterial-face-switch`](mesa-colormaterial-face-switch.md) | Mesa core (`iris` + `llvmpipe`) | `glColorMaterial` face switch drops the tracked color; renders the `glr-logo` example's exterior faces black |
| [`apple-colormaterial-rasterpos`](apple-colormaterial-rasterpos.md) | Apple OpenGL (`2.1 Metal`) | Components tracked by `GL_COLOR_MATERIAL` are lit as zero at `glRasterPos`; renders lit `label()` text black |
| [`mesa-rasterpos-lighting-untransformed`](mesa-rasterpos-lighting-untransformed.md) | Mesa core (`iris`) | `glRasterPos` lit from the object-space position, and `GL_NORMALIZE` ignored; wrong shade on lit bitmap text under a transformed modelview |
| [`mesa-rasterpos-color-unclamped`](mesa-rasterpos-color-unclamped.md) | Mesa core (`iris`) | `GL_CURRENT_RASTER_COLOR` latched unclamped with lighting off; visible only to code that reads the cell back |

The last three all land on `GL_CURRENT_RASTER_COLOR` and were found by the
differential oracles in `make gl-tests`, which diff the pure
`gl_state_inspector` model against a live driver. Each reproducer here is
standalone (GLUT, no project headers) and checks the driver against **itself**
rather than against a hardcoded expectation - comparing the raster colour with
the colour the same driver gives a vertex under identical state, which the spec
defines to be the same computation. Each exits 0 on the drivers that are
conformant, so they double as cross-driver probes.

