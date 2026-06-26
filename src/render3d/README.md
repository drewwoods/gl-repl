# `src/render3d` — the 3D renderer

> Part of the OpenGL Immediate-Mode REPL. The whole-tree ownership map is
> in [`../../docs/MODULES.md`](../../docs/MODULES.md); the per-frame pipeline narrative
> is in [`../../docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md). This README is the
> module-local view: what a 3D renderer *is*, how the standalone demo
> exercises it, and what it does inside this app.

## What this is, in general

A **3D renderer** is the part of a graphics program that owns the
*frame*: it sets the viewport, clears the buffers, builds the projection,
positions the camera, configures lighting, draws the world, and adds the
"studio" decorations (a reference grid, axes, a backdrop). It does **not**
decide *what* geometry exists — that is handed in.

`src/render3d` is a renderer for **fixed-function OpenGL** — the classic GL 1.x
style of `glBegin`/`glEnd`, the matrix stack, and `glLight*`/`glMaterial*`
lighting (no shaders). Its central abstraction is a **geometry callback**:
the caller fills a [`Render3dRenderConfig`](render_types.h#L131) (camera pose, lighting, grid/axes
themes, AA settings, clear color) and supplies an `execute_fn` that draws
the actual geometry. [`render3d_draw_scene()`](src/render3d/render.h#L135) does everything around that
callback:

```c
glr_camera_load_modelview(&pose);                /* caller owns the modelview camera transform */
render3d_draw_scene(&renderer_state, &cfg);    /* viewport → projection → cfg.execute_fn() → grid/axes/backdrop/overlays */
```

The render3d module owns no camera type — the caller picks one. The REPL
controller uses [`GlrCameraPose`](src/app/glr_camera.h#L120) + `glr_camera_load_modelview` from
[`src/app/glr_camera.h`](src/app/glr_camera.h); the standalone `render3d_demo` inlines the six
matrix calls directly (`glLoadIdentity`, `glTranslatef`, two
`glRotatef`s, `glTranslatef`) so it has no hard dep on [`glr_camera.h`](src/app/glr_camera.h).

This is the standard way to keep a renderer reusable: it depends on a
*contract* (config in, callback for geometry) rather than on the specific
application that produced the geometry. Perspective↔orthographic blending,
accumulation-buffer antialiasing, fog, grid/axes themes, and a procedural
backdrop are all renderer concerns; the geometry is not.

## The demo: `render3d_demo`

[`tools/render3d_demo/render3d_demo.c`](../../tools/render3d_demo/render3d_demo.c) drives
this module with a **non-REPL** geometry callback — a single
`glutSolidTeapot` — wrapped in a small interactive shell.

```bash
make render3d_demo     # opens a window: "render3d teapot demo"
./render3d_demo        # drag = orbit/pan, wheel = zoom, ? = on-screen controls
```

Controls cover the renderer's whole config surface: `V` blends
perspective↔orthographic, `L`/`1`–`4` toggle global lighting and individual
lights, `W` wireframe, `G`/`X` cycle grid/axes themes, `B` cycles the
backdrop, `I` toggles light indicators. A bitmap HUD prints the live config.

The demo is the **layer-independence proof** for `src/render3d`: it builds with
a deliberately slim object list and a geometry callback that knows nothing
about the REPL. If render3d code ever grew a hard dependency on the editor,
controller, or UI, this binary would stop linking. It also documents the
contract by example — `build_config()` shows exactly which [`Render3dRenderConfig`](render_types.h#L131)
fields must be set (e.g. the grid step tables, or the renderer's grid loop
never terminates).

## In the REPL app

Inside the full app this is **layer 4** of the ownership map. The controller
([`src/app/glr_ctrl.c`](src/app/glr_ctrl.c)) builds a [`Render3dRenderConfig`](render_types.h#L131) from REPL runtime state + view
state each frame, then calls [`glr_camera_load_modelview()`](src/app/glr_camera.h#L126) and
[`render3d_draw_scene()`](src/render3d/render.h#L135) once per accumulation-jitter sample (with its own
[`Render3dState`](src/render3d/render.h#L95)). The geometry callback is the REPL executor
(`repl_execute_program`), so the user's typed program becomes the rendered
geometry.

Render3d renderers **consume snapshots/configs and never read REPL runtime state,
[`EditorState`](src/editor/state.h#L175), or [`UiState`](src/ui/app/state.h#L20) directly.** The two REPL-aware overlay passes
under `guides/` (vertex/normal guides at the cursor, transform guides during
replay) still obey this: the `edit_overlays` peer subsystem
`src/subsystems/edit_overlays/` (driven by the controller each frame)
resolves their data into a [`Render3dGuideSnapshot`](src/render3d/guides/guides_shared.h#L16) and passes it in. The camera transform is the
controller's job — [`render.c`](src/render3d/render.c) only brackets sub-renderer push/pop and
applies a render3d-local frustum shift for jitter.

## File map

| File | Responsibility |
|---|---|
| [`render.c`](src/render3d/render.c) / `.h` | Frame orchestration: viewport, clear, projection, accumulation loop, geometry-callback hook, overlay/HUD passes |
| [`render_types.h`](src/render3d/render_types.h) | [`Render3dRgba`](src/render3d/render_types.h#L59), [`Render3dRenderConfig`](render_types.h#L131), frame-context types — the renderer contract |
| [`grid.c`](src/render3d/grid.c) / `.h` | Reference-grid rendering and grid themes (incl. ocean/ruler passes) |
| [`axes.c`](src/render3d/axes.c) / `.h` | Axis rendering and axis themes |
| [`render3d_transition.c`](src/render3d/render3d_transition.c) / `.h` | Pure grid/axes show↔hide fade state machine (no GL) |
| [`backdrop.c`](src/render3d/backdrop.c) / `.h` | Backdrop/environment dispatch + procedural cityscape |
| [`lights.c`](src/render3d/lights.c) / `.h` | Baseline lighting setup and light-indicator gizmos |
| [`overlays.c`](src/render3d/overlays.c) / `.h` | Tiny per-vertex primitives (vertex-number labels, normal arrows) |
| [`postprocess_filter.c`](src/render3d/postprocess_filter.c) / `.h` | Optional full-frame post-process pass |
| [`guides/geometry_guides.c`](src/render3d/guides/geometry_guides.c) | Vertex/primitive guides at the cursor (from [`Render3dGuideSnapshot`](src/render3d/guides/guides_shared.h#L16)) |
| [`guides/transform_guides.c`](src/render3d/guides/transform_guides.c) | Transform guides (pending matrix ops during replay) |
| [`guides/guides_shared.h`](src/render3d/guides/guides_shared.h) | Shared guide snapshot/planning types |
| [`palette.h`](src/render3d/palette.h), [`themes.h`](src/render3d/themes.h), [`occluded_ghost.h`](src/render3d/occluded_ghost.h) | Shared color/theme/style constants |

**Boundary:** render3d code renders. It does **not** parse, edit, save, or
dispatch UI actions, and `render3d_*` / `ui_*` do not include each other's
headers — shared types live in [`render_types.h`](src/render3d/render_types.h) / [`src/ui/app/snapshot.h`](src/ui/app/snapshot.h).
