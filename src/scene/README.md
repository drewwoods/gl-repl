# `src/scene` — the 3D scene renderer

> Part of the OpenGL Immediate-Mode REPL. The whole-tree ownership map is
> in [`../../MODULES.md`](../../MODULES.md); the per-frame pipeline narrative
> is in [`../../ARCHITECTURE.md`](../../ARCHITECTURE.md). This README is the
> module-local view: what a scene renderer *is*, how the standalone demo
> exercises it, and what it does inside this app.

## What this is, in general

A **scene renderer** is the part of a graphics program that owns the
*frame*: it sets the viewport, clears the buffers, builds the projection,
positions the camera, configures lighting, draws the world, and adds the
"studio" decorations (a reference grid, axes, a backdrop). It does **not**
decide *what* geometry exists — that is handed in.

`src/scene` is a renderer for **fixed-function OpenGL** — the classic GL 1.x
style of `glBegin`/`glEnd`, the matrix stack, and `glLight*`/`glMaterial*`
lighting (no shaders). Its central abstraction is a **geometry callback**:
the caller fills a `SceneRenderConfig` (camera pose, lighting, grid/axes
themes, AA settings, clear color) and supplies an `execute_fn` that draws
the actual geometry. `scene_render_3d_scene()` does everything around that
callback:

```c
glr_camera_load_modelview(&pose);                /* caller owns the modelview camera transform */
scene_render_3d_scene(&renderer_state, &cfg);    /* viewport → projection → cfg.execute_fn() → grid/axes/backdrop/overlays */
```

The scene module owns no camera type — the caller picks one. The REPL
controller uses `GlrCameraPose` + `glr_camera_load_modelview` from
`src/app/glr_camera.h`; the standalone `scene_demo` inlines the six
matrix calls directly (`glLoadIdentity`, `glTranslatef`, two
`glRotatef`s, `glTranslatef`) so it has no hard dep on `glr_camera.h`.

This is the standard way to keep a renderer reusable: it depends on a
*contract* (config in, callback for geometry) rather than on the specific
application that produced the geometry. Perspective↔orthographic blending,
accumulation-buffer antialiasing, fog, grid/axes themes, and a procedural
backdrop are all renderer concerns; the geometry is not.

## The demo: `scene_demo`

[`tools/scene_demo/scene_demo.c`](../../tools/scene_demo/scene_demo.c) drives
this module with a **non-REPL** geometry callback — a single
`glutSolidTeapot` — wrapped in a small interactive shell.

```bash
make scene_demo     # opens a window: "scene-module teapot demo"
./scene_demo        # drag = orbit/pan, wheel = zoom, ? = on-screen controls
```

Controls cover the renderer's whole config surface: `V` blends
perspective↔orthographic, `L`/`1`–`4` toggle global lighting and individual
lights, `W` wireframe, `G`/`X` cycle grid/axes themes, `B` cycles the
backdrop, `I` toggles light indicators. A bitmap HUD prints the live config.

The demo is the **layer-independence proof** for `src/scene`: it builds with
a deliberately slim object list and a geometry callback that knows nothing
about the REPL. If scene code ever grew a hard dependency on the editor,
controller, or UI, this binary would stop linking. It also documents the
contract by example — `build_config()` shows exactly which `SceneRenderConfig`
fields must be set (e.g. the grid step tables, or the renderer's grid loop
never terminates).

## In the REPL app

Inside the full app this is **layer 4** of the ownership map. The controller
(`src/app/glr_ctrl.c`) builds a `SceneRenderConfig` from `ReplState` + view
state each frame, then calls `glr_camera_load_modelview()` and
`scene_render_3d_scene()` once per accumulation-jitter sample (with its own
`SceneRendererState`). The geometry callback is the REPL executor
(`repl_execute_program`), so the user's typed program becomes the scene's
geometry.

Scene renderers **consume snapshots/configs and never read `ReplState`,
`EditorState`, or `UiState` directly.** The two REPL-aware overlay passes
under `guides/` (vertex/normal guides at the cursor, transform guides during
replay) still obey this: the `edit_overlays` peer subsystem
(`src/subsystems/edit_overlays/`, driven by the controller each frame)
resolves their data into a `SceneGuideSnapshot` and passes it in. The camera transform is the
controller's job — `render.c` only brackets sub-renderer push/pop and
applies a scene-local frustum shift for jitter.

## File map

| File | Responsibility |
|---|---|
| `render.c` / `.h` | Frame orchestration: viewport, clear, projection, accumulation loop, geometry-callback hook, overlay/HUD passes |
| `render_types.h` | `SceneRgba`, `SceneRenderConfig`, frame-context types — the renderer contract |
| `grid.c` / `.h` | Reference-grid rendering and grid themes (incl. ocean/ruler passes) |
| `axes.c` / `.h` | Axis rendering and axis themes |
| `scene_transition.c` / `.h` | Pure grid/axes show↔hide fade state machine (no GL) |
| `backdrop.c` / `.h` | Backdrop/environment dispatch + procedural cityscape |
| `lights.c` / `.h` | Baseline lighting setup and light-indicator gizmos |
| `overlays.c` / `.h` | Tiny per-vertex primitives (vertex-number labels, normal arrows) |
| `postprocess_filter.c` / `.h` | Optional full-frame post-process pass |
| `guides/geometry_guides.c` | Vertex/primitive guides at the cursor (from `SceneGuideSnapshot`) |
| `guides/transform_guides.c` | Transform guides (pending matrix ops during replay) |
| `guides/guides_shared.h` | Shared guide snapshot/planning types |
| `palette.h`, `themes.h`, `occluded_ghost.h` | Shared color/theme/style constants |

**Boundary:** scene code renders. It does **not** parse, edit, save, or
dispatch UI actions, and `scene_*` / `ui_*` do not include each other's
headers — shared types live in `render_types.h` / `src/ui/app/snapshot.h`.
