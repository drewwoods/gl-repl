# `src/app` - the application shell + controller (Draft)

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

`src/app` is that layer. Its hub, [`glr_ctrl.c`](glr_ctrl.c), is meant
to be a router and frame/snapshot coordinator: it should route input,
coordinate frames, and keep the app-specific wiring in one place without owning
feature behavior. The current file is broader than that target. It still carries
too much mixed policy for routing, frame order, snapshot assembly, timers, and
transitional glue. That bloat is a known design pressure, not a license to add
new feature behavior there by default.

The boundary still matters: `glr_ctrl` should not implement editor behavior,
parse the language, draw widgets, or own 3D rendering policy. The subsystems it
wires together (`src/repl`, `src/editor`, `src/ui`, `src/render3d`,
`src/subsystems`) do not depend on it - the dependency arrows run one way, from
`glr_ctrl` to the subsystems, never back.
Alongside the router live the app-level services that are genuinely
app-specific: the camera, the audio playlist, the config/menu tables, and the
completion provider.

(The literal `main()` and the GLUT callback registration live in the
root-level [`gl_repl.c`](../../gl_repl.c), which forwards directly to `glr_ctrl_*` - there is no
shim in between.)

## How it is exercised - the inverse of the demos

This module has no demo of its own, and that is the point. The standalone
demos are each defined by **excluding this layer**:

- `repl_demo` drives `src/repl` *without* `glr_ctrl` or its router family.
- `editor_demo` drives `src/editor`'s model with its *own* input dispatcher
  and File menu instead of this app's.
- `render3d_demo` drives `src/render3d` with its *own* camera/HUD shell.

Each demo re-implements just enough of the controller's job to stand its
subsystem up alone. So `src/app` is, by construction, the **app-specific
glue** - the code that is *not* reusable because it encodes how *this*
particular program is composed. If you were to lift a subsystem into your own
engine (a stated design goal of this project), `src/app` is the part you
would replace.

That independence is intentional documentation, not just a build
optimization. A demo should stay `src/app`-free unless a new exception is
explicitly justified in its object-list comment and local docs. Pulling in
`glr_ctrl`, `glr_actions`, `glr_config`, or [`UiRenderSnapshot`](../ui/app/snapshot.h#L85) to make a demo
easier usually means the owner module is missing a smaller contract or view
type.

## In the REPL app

Inside the full app this is **layer 0** of the ownership map. Per frame,
[`glr_ctrl_display_frame()`](glr_ctrl.h#L263):

1. rebuilds autonormals / the flat program if dirty, and prepares replay /
   export / camera strings;
2. builds a [`Render3dRenderConfig`](../render3d/render_types.h#L139) from REPL runtime state + view state and calls
   [`glr_camera_load_modelview()`](glr_camera.h#L165) then [`render3d_draw_scene()`](../render3d/render.h#L129) (with the
   owned [`Render3dState`](../render3d/render.h#L88), once per accumulation-jitter sample);
3. builds a [`UiRenderSnapshot`](../ui/app/snapshot.h#L85) and fans it out to the `ui_*_render`
   functions.

On input, the physical `glr_ctrl_keyboard` / `_mouse` family first arbitrates
controlled-tour transport, cancellation, and pointer ownership, then
normalizes the event, asks UI to hit-test, and routes the result. Synthetic
tour/capture input enters through `glr_ctrl_scripted_*`, bypassing only that
physical-input arbitration and joining the same dispatch beneath it.
Non-editor concerns (replay, audio, config, save, camera, variable panel,
scene press, scroll-wheel zoom) go through the `glr_ctrl_router_*` helpers
*before* the editor dispatch entry points run. The controller is the only
input-event mutation gate, and it relays diagnostics from the REPL to the
editor/status line. A guard
(`check-glr-ctrl-not-editor-mirror`) keeps it from accreting one wrapper per
editor operation.

## Two bands: boot lifecycle vs frame-time controller

`src/app` holds two bands, and the split is worth naming because they run in
different worlds:

- **Boot / lifecycle band - [`boot/`](boot/).** Process startup: the code that
  runs *before or without* a live GL context and never sits on the frame loop.
  CLI parse, the startup init trace, the headless capture-env `GLR_*` hooks,
  the frame pacer (a pure timer-delay calculator), the splash banner, and the
  `--dump-*` dump-and-exit path. These modules are reached **only from the host
  [`gl_repl.c`](../../gl_repl.c)** (plus their tests and sibling boot modules) -
  never from the controller.
- **Frame-time controller band - the files directly in `src/app/`.** The
  controller ([`glr_ctrl`](glr_ctrl.c)), its router / view-transition / snapshot
  satellites, and the app-level services (camera, audio, config, actions,
  completion, tours, pointer-script). These assume a **live GL context and a
  running frame loop**.

**Membership rule.** A module is boot if it runs during process startup and is
called only from [`gl_repl.c`](../../gl_repl.c) (or other boot modules) - put it in `boot/`.
Everything that participates in a frame stays directly in `src/app/`.

**Direction rule.** [`gl_repl.c`](../../gl_repl.c) → `boot/` → controller → subsystems. Boot
modules may call **down** into the controller band - that is how the
dump-and-exit path ([`boot/glr_boot_dumps.c`](boot/glr_boot_dumps.c)) reuses the
diagnostic formatters in [`glr_debug.c`](glr_debug.c). The controller band must
**never** reach back **up** into `boot/`: a frame-time file that includes
`"app/boot/..."` has taken a dependency on the startup lifecycle it is supposed
to run underneath. The guard `check-app-boot-band`
([`scripts/check/check-app-boot-band.sh`](../../scripts/check/check-app-boot-band.sh),
in the `check-state-ownership` suite) enforces the one-way edge.

`boot/glr_boot_dumps` is the shape this rule forces: the `--dump-*` dispatch
consumes [`GlrCliOptions`](boot/glr_cli.h#L32) (a boot type), so it lives in `boot/`, while the dump
*formatters* it drives stay in the controller-band `glr_debug` because the
router calls them at runtime on a debug keystroke. Splitting the two keeps the
formatters free of any boot dependency.

## File map

### Frame-time controller band (`src/app/`)

| File | Responsibility |
|---|---|
| [`glr_ctrl.c`](glr_ctrl.c) / `.h` | App-frame controller + input router: display/reshape/init-GL, snapshot builders, `glr_ctrl_router_*` non-editor routing |
| [`glr_camera.c`](glr_camera.c) / `.h` | Orbit/pan/zoom camera state, drag transactions, momentum, eased targets |
| [`glr_camera_export.c`](glr_camera_export.c) | Camera state ↔ exported `glTranslatef`/`glRotatef` text |
| [`glr_audio.c`](glr_audio.c) / `.h` | App-level playlist engine and persisted audio config (`glr_audio_*`) |
| [`glr_actions.c`](glr_actions.c) / `.h` | Config descriptor table (`g_cfg_items[]`), config shortcuts, menu actions |
| [`glr_config.c`](glr_config.c) / `.h` | `ReplConfigKey` / [`ReplConfigItem`](../repl/cfg_baseline.h#L29) descriptor API for keyed config access |
| [`glr_completion.c`](glr_completion.c) / `.h` | REPL-side completion provider; registers with `editor_completion` |
| [`glr_state.c`](glr_state.c) / `.h` | App-level presentation/runtime toggles not owned by repl/editor/ui |
| [`glr_source_document.c`](glr_source_document.c) | Binds the `source_document_*` contract to the live [`EditorState`](../editor/state.h#L199) buffer |
| [`glr_debug.c`](glr_debug.c) / `.h` | Diagnostic dump *formatters* for debug keystrokes and tests (the `--dump-*` dispatch lives in [`boot/glr_boot_dumps`](boot/glr_boot_dumps.c)) |
| [`glr_defaults.h`](glr_defaults.h) | Controller-side 3D presentation defaults (`CFG_DEFAULT_*`) |

(Router / view-transition / compositor / tours / pointer-script satellites live
here too - see [`../../docs/MODULES.md`](../../docs/MODULES.md) for the full roster.)

### Boot / lifecycle band ([`boot/`](boot/))

| File | Responsibility |
|---|---|
| [`boot/glr_cli.c`](boot/glr_cli.c) / `.h` | argv → [`GlrCliOptions`](boot/glr_cli.h#L32) bag; `print_usage`, `--list-*`/`-h` exit paths, `--example`/`--tour` name→index resolve |
| [`boot/glr_boot_dumps.c`](boot/glr_boot_dumps.c) / `.h` | `--dump-*` / `--flat-histogram` GL-free bootstrap-dump-and-exit path (drives `glr_debug` formatters) |
| [`boot/glr_init_trace.c`](boot/glr_init_trace.c) / `.h` | Startup stall diagnostic (`[init +N.NNNs] <phase>`); baseline + `--detailed-prof` phases |
| [`boot/glr_capture_env.c`](boot/glr_capture_env.c) / `.h` | Headless-capture `GLR_*` env hooks: `_apply` (bootstrap) + `_frame_hook` (per-frame overlays) |
| [`boot/glr_frame_pacer.c`](boot/glr_frame_pacer.c) / `.h` | Pure absolute-deadline 60 Hz timer-delay calculator used by the GLUT host |
| [`boot/splash.c`](boot/splash.c) / `.h` | Startup splash banner (host-drawn during the first frames; any keypress dismisses) |

**Boundary:** `glr_ctrl` routes raw input to the owning subsystem and builds
frame snapshots. It does **not** implement editor behavior or duplicate the
editor's API surface; new editor behavior belongs behind `editor_*`. App
services (camera, audio, config, completion) live here because they are
app-specific, not because the controller owns their bytes.
