# `src/render3d` - the 3D renderer

> Part of the OpenGL Immediate-Mode REPL. The whole-tree ownership map is
> in [`../../docs/MODULES.md`](../../docs/MODULES.md); the per-frame pipeline narrative
> is in [`../../docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md). This README is the
> module-local view: what a 3D renderer *is*, how the standalone demo
> exercises it, and what it does inside this app.

## What this is, in general

A **3D renderer** is the part of a graphics program that owns the
*frame*: it sets the viewport, builds the projection, configures lighting,
draws the world, and adds the "studio" decorations (a reference grid, axes,
a backdrop), and manages internal accumulation loops. It does **not**
clear color/depth buffers, own a camera type, or decide *what* geometry exists -
those are handed in.

`src/render3d` is a renderer for **fixed-function OpenGL** - the classic GL 1.x
style of `glBegin`/`glEnd`, the matrix stack, and `glLight*`/`glMaterial*`
lighting (no shaders). Its central abstraction is a **geometry callback**:
the caller fills a [`Render3dRenderConfig`](render_types.h#L139) (camera pose, lighting, grid/axes
themes, AA settings, background colors) and supplies an `execute_fn` that draws
the actual geometry. [`render3d_draw_scene()`](render.h#L139) does everything around that
callback:

```c
glr_camera_load_modelview(&pose);                /* caller owns the modelview camera transform */
render3d_draw_scene(&renderer_state, &cfg);    /* viewport -> projection -> cfg.execute_fn() -> grid/axes/backdrop/overlays */
```

### Two background colors, not one

The config carries the background twice, because the two uses are not the same
question:

- `baseline_clear_color` - the GL clear-color state established before
  `execute_fn` runs, i.e. what a `glClear` inside the callback uses when the
  callback set no clear color of its own. It is a fixed configuration value.
- `presentation_rgba` - the background the scene is understood to sit *on*: the
  color the grid / axes recede fog fades toward, and the color the caller
  derived `alpha_scale` from. The derivation stays caller policy; render3d
  consumes the scale, never recomputes it.

A caller whose geometry callback clears with a color it computes at run time -
the REPL controller, whose user program owns its own `glClearColor`/`glClear` -
feeds the *observed* result into `presentation_rgba` while leaving
`baseline_clear_color` at its configuration default. Feeding an observation into
the baseline would let a previous frame's background decide the pixels this
frame's own clear writes. A caller with no such distinction (the demo) writes
the same value to both.

The render3d module owns no camera type - the caller picks one. The REPL
controller uses [`GlrCameraPose`](../app/glr_camera.h#L150) + `glr_camera_load_modelview` from
[`src/app/glr_camera.h`](../app/glr_camera.h); the standalone `render3d_demo` inlines the six
matrix calls directly (`glLoadIdentity`, `glTranslatef`, two
`glRotatef`s, `glTranslatef`) so it has no hard dep on [`glr_camera.h`](../app/glr_camera.h).

This is the standard way to keep a renderer reusable: it depends on a
*contract* (config in, callback for geometry) rather than on the specific
application that produced the geometry. Perspective <--> orthographic blending,
accumulation-buffer antialiasing, fog, grid/axes themes, and a procedural
backdrop are all renderer concerns; the geometry is not.

## The demo: `render3d_demo`

[`tools/render3d_demo/render3d_demo.c`](../../tools/render3d_demo/render3d_demo.c) drives
this module with a **non-REPL** geometry callback - a single
`glutSolidTeapot` - wrapped in a small interactive shell.

```bash
make render3d-demo     # opens a window: "render3d teapot demo"
./render3d_demo        # drag = orbit/pan, wheel = zoom, ? = on-screen controls
```

Controls cover the renderer's whole config surface: `V` blends
perspective <--> orthographic, `L`/`1`-`4` toggle global lighting and individual
lights, `W` wireframe, `G`/`X` cycle grid/axes themes, `B` cycles the
backdrop, `I` toggles light indicators. A bitmap HUD prints the live config.

The demo is the **layer-independence proof** for `src/render3d`: it builds with
a deliberately slim object list and a geometry callback that knows nothing
about the REPL. If render3d code ever grew a hard dependency on the editor,
controller, or UI, this binary would stop linking. It also documents the
contract by example - `build_config()` shows exactly which [`Render3dRenderConfig`](render_types.h#L139)
fields must be set (e.g. the grid step tables, or the renderer's grid loop
never terminates).

### Hot reload: `make render3d-hot`

```bash
make render3d-hot      # builds render3d_hot_demo + the reloadable library
./render3d_hot_demo    # then edit any src/render3d/*.c and save - it reloads live
```

Same teapot harness, but the `src/render3d` subtree is compiled into a shared
library the host `dlopen()`s instead of static-linking. The running host polls
the source tree; on a save it rebuilds just that library (`make
render3d-hot-lib`) and re-`dlopen()`s a fresh copy, so `grid.c` / `backdrop.c`
/ [`lights.c`](lights.c) / [`render.c`](render.c) can be tweaked and seen **without relaunching**. The
HUD shows a `Hot  reloads=N  last build=ok/FAILED` line; a build error keeps the
last good module loaded.

**State survives the reload** because every piece of demo state - camera pose,
2D/3D + projection blend, grid/axes themes, lighting, backdrop - lives in the
host TU ([`render3d_demo.c`](../../tools/render3d_demo/render3d_demo.c)), which is never reloaded; only the render3d `.c`
bodies are. [`Render3dState`](render.h#L101) crosses the boundary but is host-owned, and its
layout is fixed by [`render.h`](render.h) at host-compile time - so editing `.c` bodies is
free, while changing that struct's **layout** (a header edit) is the one case
that needs a relaunch.

Implementation: the reloadable library carries no freeglut/GL of its own and
resolves `glut*`/`gl*`/`glu*` from the host at load (macOS `-undefined
dynamic_lookup` + the host `-force_load`ing freeglut so every symbol is present
and exported; Linux binds against the already-loaded shared `libglut`/`libGL`).
A second freeglut copy in the library would have an uninitialised `fgState` and
`glutSolidSphere()` would abort "called before glutInit". Each reload
`dlopen()`s a uniquely-named on-disk copy of the rebuilt library - macOS dyld
caches images by path and keeps them mapped past `dlclose`, so reusing the
canonical path would hand back stale code. The plain static `render3d_demo`
target is untouched (it stays the link-proof `make test-full` and
`USE_GL_STUBS` builds rely on).

## In the REPL app

Inside the full app this is **layer 4** of the ownership map. The controller
([`src/app/glr_ctrl.c`](../app/glr_ctrl.c)) builds a [`Render3dRenderConfig`](render_types.h#L139) from REPL runtime state + view
state each frame, then calls [`glr_camera_load_modelview()`](../app/glr_camera.h#L165) and
[`render3d_draw_scene()`](render.h#L139) once per frame (the accumulation-AA jitter
loop is managed internally by `render3d_draw_scene()` with its own
[`Render3dState`](render.h#L101)). The geometry callback is the REPL executor
(`repl_execute_program`), so the user's typed program becomes the rendered
geometry.

Render3d renderers **consume snapshots/configs and never read REPL runtime state,
[`EditorState`](../editor/state.h#L200), or [`UiState`](../ui/app/state.h#L20) directly.** The two REPL-aware overlay passes
under `guides/` (vertex/normal guides at the cursor, transform guides during
replay) still obey this: the `edit_overlays` peer subsystem
`src/subsystems/edit_overlays/` (driven by the controller each frame)
resolves their data into a [`Render3dGuideSnapshot`](guides/guides_shared.h#L53) and passes it in. The camera transform is the
controller's job - [`render.c`](render.c) only brackets sub-renderer push/pop and
applies a render3d-local frustum shift for jitter.

## File map

| File | Responsibility |
|---|---|
| [`render.c`](render.c) / `.h` | Frame orchestration: viewport, baseline clear-color state, projection, accumulation loop, geometry-callback hook, overlay/HUD passes |
| [`render_types.h`](render_types.h) | [`Render3dRgba`](render_types.h#L63), [`Render3dRenderConfig`](render_types.h#L139), frame-context types - the renderer contract |
| [`grid.c`](grid.c) / `.h` | Reference-grid rendering and grid themes (incl. ocean/ruler passes) |
| [`axes.c`](axes.c) / `.h` | Axis rendering and axis themes |
| [`render3d_transition.c`](render3d_transition.c) / `.h` | Pure grid/axes show <--> hide fade state machine (no GL) |
| [`backdrop.c`](backdrop.c) / `.h` | Backdrop/environment dispatch + procedural cityscape |
| [`lights.c`](lights.c) / `.h` | Baseline lighting setup and light-indicator gizmos |
| [`overlays.c`](overlays.c) / `.h` | Tiny per-vertex primitives (vertex-number labels, normal arrows) |
| [`postprocess_filter.c`](postprocess_filter.c) / `.h` | Optional full-frame post-process pass |
| [`guides/geometry_guides.c`](guides/geometry_guides.c) | Vertex/primitive guides at the cursor (from [`Render3dGuideSnapshot`](guides/guides_shared.h#L53)) |
| [`guides/transform_guides.c`](guides/transform_guides.c) | Transform guides (pending matrix ops during replay) |
| [`guides/guides_shared.h`](guides/guides_shared.h) | Shared guide snapshots and per-frame guide-plan types |
| [`palette.h`](palette.h), [`themes.h`](themes.h), [`occluded_ghost.h`](occluded_ghost.h) | Shared color/theme/style constants |

**Boundary:** render3d code renders. It does **not** parse, edit, save, or
dispatch UI actions, and `render3d_*` / `ui_*` do not include each other's
headers - shared types live in [`render_types.h`](render_types.h) / [`src/ui/app/snapshot.h`](../ui/app/snapshot.h).
