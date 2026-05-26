# `src/app` — the application shell + controller

> Part of the OpenGL Immediate-Mode REPL. The whole-tree ownership map is
> in [`../../MODULES.md`](../../MODULES.md); the per-frame pipeline narrative
> is in [`../../ARCHITECTURE.md`](../../ARCHITECTURE.md). This README is the
> module-local view: what an application shell *is* and what it does inside
> this app.

## What this is, in general

Every app needs a layer that **wires the parts together**: it receives raw
events from the window system, decides which subsystem should handle each
one, drives the frame, and hosts the app-level services that don't belong to
any one feature. In MVC terms this is the **controller / coordinator**; in
larger systems it is the **composition root** (the one place that knows about
all the concrete modules and assembles them) and a **mediator** (subsystems
talk to it, not to each other).

`src/app` is that layer. Its hub, `glr_ctrl.c`, is deliberately *thin*: it
**routes** input and **coordinates** frames, but it does **not** implement
editor behavior, parse the language, or draw widgets. The subsystems it wires
together (`src/repl`, `src/editor`, `src/ui`, `src/scene`, `src/subsystems`) do
not depend on it — the dependency arrows run one way, from `glr_ctrl` to the
subsystems, never back.
Alongside the router live the app-level services that are genuinely
app-specific: the camera, the audio playlist, the config/menu tables, and the
completion provider.

(The literal `main()` and the GLUT callback registration live in the
root-level `gl_repl.c`, which forwards directly to `glr_ctrl_*` — there is no
shim in between.)

## How it is exercised — the inverse of the demos

This module has no demo of its own, and that is the point. The three
standalone demos are each defined by **excluding this layer**:

- `repl_demo` drives `src/repl` *without* `glr_ctrl` or its router family.
- `editor_demo` drives `src/editor`'s model with its *own* input dispatcher
  and File menu instead of this app's.
- `scene_demo` drives `src/scene` with its *own* camera/HUD shell.

Each demo re-implements just enough of the controller's job to stand its
subsystem up alone. So `src/app` is, by construction, the **app-specific
glue** — the code that is *not* reusable because it encodes how *this*
particular program is composed. If you were to lift a subsystem into your own
engine (a stated design goal of this project), `src/app` is the part you
would replace.

## In the REPL app

Inside the full app this is **layer 0** of the ownership map. Per frame,
`glr_ctrl_display_frame()`:

1. rebuilds autonormals / the flat program if dirty, and prepares replay /
   export / camera strings;
2. builds a `SceneRenderConfig` from `ReplState` + view state and calls
   `glr_camera_load_modelview()` then `scene_render_3d_scene()` (with the
   owned `SceneRendererState`, once per accumulation-jitter sample);
3. builds a `UiRenderSnapshot` and fans it out to the `ui_*_render`
   functions.

On input, `glr_ctrl_keyboard` / `_mouse` normalize the event, ask UI to
hit-test, and route the result: non-editor concerns (replay, audio, config,
save, camera, variable panel, scene press, scroll-wheel zoom) go through the
`glr_ctrl_router_*` helpers *before* the editor dispatch entry points run.
The controller is the only input-event mutation gate, and it relays
diagnostics from the REPL to the editor/status line. A guard
(`check-glr-ctrl-not-editor-mirror`) keeps it from accreting one wrapper per
editor operation.

## File map

| File | Responsibility |
|---|---|
| `glr_ctrl.c` / `.h` | App-frame controller + input router: display/reshape/init-GL, snapshot builders, `glr_ctrl_router_*` non-editor routing |
| `glr_camera.c` / `.h` | Orbit/pan/zoom camera state, drag transactions, momentum, eased targets |
| `glr_camera_export.c` | Camera state ↔ exported `glTranslatef`/`glRotatef` text |
| `glr_audio.c` / `.h` | App-level playlist engine and persisted audio config (`glr_audio_*`) |
| `glr_actions.c` / `.h` | Config descriptor table (`g_cfg_items[]`), config shortcuts, menu actions |
| `glr_config.c` / `.h` | `ReplConfigKey` / `ReplConfigItem` descriptor API for keyed config access |
| `glr_completion.c` / `.h` | REPL-side completion provider; registers with `editor_completion` |
| `glr_state.c` / `.h` | App-level presentation/runtime toggles not owned by repl/editor/ui |
| `glr_source_document.c` | Binds the `source_document_*` contract to the live `EditorState` buffer |
| `glr_debug.c` / `.h` | Diagnostic dumps for CLI flags and tests |
| `glr_defaults.h` | Controller-side scene/presentation defaults (`CFG_DEFAULT_*`) |

**Boundary:** `glr_ctrl` routes raw input to the owning subsystem and builds
frame snapshots. It does **not** implement editor behavior or duplicate the
editor's API surface; new editor behavior belongs behind `editor_*`. App
services (camera, audio, config, completion) live here because they are
app-specific, not because the controller owns their bytes.
