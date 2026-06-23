# `src/app` — the application shell + controller

> Part of the OpenGL Immediate-Mode REPL. The whole-tree ownership map is
> in [`../../docs/MODULES.md`](../../docs/MODULES.md); the per-frame pipeline narrative
> is in [`../../docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md). This README is the
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

`src/app` is that layer. Its hub, [`glr_ctrl.c`](src/app/glr_ctrl.c), is meant
to be a router and frame/snapshot coordinator: it should route input,
coordinate frames, and keep the app-specific wiring in one place without owning
feature behavior. The current file is broader than that target. It still carries
too much mixed policy for routing, frame order, snapshot assembly, timers, and
transitional glue. That bloat is a known design pressure, not a license to add
new feature behavior there by default.

The boundary still matters: `glr_ctrl` should not implement editor behavior,
parse the language, draw widgets, or own 3D rendering policy. The subsystems it
wires together (`src/repl`, `src/editor`, `src/ui`, `src/render3d`,
`src/subsystems`) do not depend on it — the dependency arrows run one way, from
`glr_ctrl` to the subsystems, never back.
Alongside the router live the app-level services that are genuinely
app-specific: the camera, the audio playlist, the config/menu tables, and the
completion provider.

(The literal `main()` and the GLUT callback registration live in the
root-level [`gl_repl.c`](gl_repl.c), which forwards directly to `glr_ctrl_*` — there is no
shim in between.)

## How it is exercised — the inverse of the demos

This module has no demo of its own, and that is the point. The standalone
demos are each defined by **excluding this layer**:

- `repl_demo` drives `src/repl` *without* `glr_ctrl` or its router family.
- `editor_demo` drives `src/editor`'s model with its *own* input dispatcher
  and File menu instead of this app's.
- `render3d_demo` drives `src/render3d` with its *own* camera/HUD shell.

Each demo re-implements just enough of the controller's job to stand its
subsystem up alone. So `src/app` is, by construction, the **app-specific
glue** — the code that is *not* reusable because it encodes how *this*
particular program is composed. If you were to lift a subsystem into your own
engine (a stated design goal of this project), `src/app` is the part you
would replace.

That independence is intentional documentation, not just a build
optimization. A demo should stay `src/app`-free unless a new exception is
explicitly justified in its object-list comment and local docs. Pulling in
`glr_ctrl`, `glr_actions`, `glr_config`, or [`UiRenderSnapshot`](src/ui/app/snapshot.h#L70) to make a demo
easier usually means the owner module is missing a smaller contract or view
type.

## In the REPL app

Inside the full app this is **layer 0** of the ownership map. Per frame,
[`glr_ctrl_display_frame()`](src/app/glr_ctrl.h#L113):

1. rebuilds autonormals / the flat program if dirty, and prepares replay /
   export / camera strings;
2. builds a [`Render3dRenderConfig`](src/render3d/render_types.h#L130) from REPL runtime state + view state and calls
   [`glr_camera_load_modelview()`](src/app/glr_camera.h#L135) then [`render3d_draw_scene()`](src/render3d/render.h#L135) (with the
   owned [`Render3dState`](src/render3d/render.h#L95), once per accumulation-jitter sample);
3. builds a [`UiRenderSnapshot`](src/ui/app/snapshot.h#L70) and fans it out to the `ui_*_render`
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
| [`glr_ctrl.c`](src/app/glr_ctrl.c) / `.h` | App-frame controller + input router: display/reshape/init-GL, snapshot builders, `glr_ctrl_router_*` non-editor routing |
| [`glr_camera.c`](src/app/glr_camera.c) / `.h` | Orbit/pan/zoom camera state, drag transactions, momentum, eased targets |
| [`glr_camera_export.c`](src/app/glr_camera_export.c) | Camera state ↔ exported `glTranslatef`/`glRotatef` text |
| [`glr_audio.c`](src/app/glr_audio.c) / `.h` | App-level playlist engine and persisted audio config (`glr_audio_*`) |
| [`glr_actions.c`](src/app/glr_actions.c) / `.h` | Config descriptor table (`g_cfg_items[]`), config shortcuts, menu actions |
| [`glr_config.c`](src/app/glr_config.c) / `.h` | `ReplConfigKey` / [`ReplConfigItem`](src/repl/cfg_baseline.h#L29) descriptor API for keyed config access |
| [`glr_completion.c`](src/app/glr_completion.c) / `.h` | REPL-side completion provider; registers with `editor_completion` |
| [`glr_state.c`](src/app/glr_state.c) / `.h` | App-level presentation/runtime toggles not owned by repl/editor/ui |
| [`glr_source_document.c`](src/app/glr_source_document.c) | Binds the `source_document_*` contract to the live [`EditorState`](src/editor/state.h#L175) buffer |
| [`glr_debug.c`](src/app/glr_debug.c) / `.h` | Diagnostic dumps for CLI flags and tests |
| [`glr_defaults.h`](src/app/glr_defaults.h) | Controller-side 3D presentation defaults (`CFG_DEFAULT_*`) |

**Boundary:** `glr_ctrl` routes raw input to the owning subsystem and builds
frame snapshots. It does **not** implement editor behavior or duplicate the
editor's API surface; new editor behavior belongs behind `editor_*`. App
services (camera, audio, config, completion) live here because they are
app-specific, not because the controller owns their bytes.
