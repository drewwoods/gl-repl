# REPL Architecture

> For the quick module map, see [`MODULES.md`](MODULES.md). For the staged
> controller extraction plan, see
> [`feature/push-architecture-refinement.md`](feature/push-architecture-refinement.md).

## Direction

This document follows the controller-first direction from
`feature/push-architecture-refinement.md`.

The older "generic scene callback plus `ReplGeometryRenderPlan`" direction is
superseded. This is a one-frontend REPL sample, so the useful boundary is
between the REPL model/controller and the rendering views. The goal is not to
turn `scene_*` into a plugin host.

Current code already routes frame wiring through `imrepl_ctrl.c`. `repl_core.c`
now keeps the REPL model/pipeline wrappers, while `scene_render.c` consumes
explicit per-frame config and only keeps the remaining transitional replay-HUD
reads and fade-batch helpers.

## Ownership Model

```text
repl_*        = language, source model, flat program, replay model, input/model controllers
imrepl_ctrl   = app-frame controller between REPL state and scene/UI rendering
scene_*       = 3D stage: camera, projection, frame setup, decorators, 3D overlays
ui_*          = 2D editor chrome: code panel, menus, overlays, popups, HUDs
sample.c/h    = current GLUT app shell and legacy shared header
imrepl.c/h    = future app shell/shared header name, replacing sample.c/h
```

The main design rule:

```text
The REPL owns the user program.
The scene owns the 3D stage.
The UI owns the 2D editor/view.
The controller translates REPL state into per-frame view inputs.
```

Under Option B, scene modules may consume `FlatProgramView`, `CmdType`, and
other command-domain data when that data is already present in the
`SceneRenderConfig` or a derived frame snapshot. They should not fetch REPL
globals or call `repl_state_*` APIs directly during rendering.

## Core Tenets

1. **The REPL owns the user program.** It parses source, stores source commands,
   flattens loops/functions/conditionals, owns predefined variables, and owns
   replay policy.
2. **The executor is the narrow live-GL gate for user geometry.**
   `repl_executor.c` turns a flat program into OpenGL calls. General `repl_*`
   modules should not casually call OpenGL.
3. **The scene owns the stage, not the editor.** It sets viewport, clear,
   projection, camera, accumulation, baseline lighting, grid, axes, backdrop,
   light indicators, orbit target, and 3D overlay passes from config.
4. **The UI owns screen-space presentation.** UI renderers draw code rows,
   menus, popups, color picker, help, status, and profile views from snapshots
   and route mutations through REPL-owned actions or stores.
5. **The controller is the mixed layer.** The frame controller builds scene and
   UI inputs from REPL state, calls the scene renderer, then calls UI renderers.
   After the refinement lands, this role belongs in `imrepl_ctrl.c`.
6. **Replay is REPL policy.** Replay state machine, PC, mode, baseline values,
   and fade/highlight decisions belong in `repl_replay.c` or follow-up replay
   planning code. Any scene use of replay data should be via snapshots or
   documented transitional helpers.

## Target Frame Pipeline

Top-level frame orchestration belongs in the controller:

```text
sample.c GLUT display callback (future: imrepl.c)
  -> repl_display_func wrapper
     -> imrepl_ctrl_display_frame
        -> tick profiling
        -> rebuild autonormals if dirty
        -> rebuild flat program if dirty
        -> save live predefined variable values
        -> prepare replay frame if replay is active
        -> update export/camera strings
        -> build SceneRenderConfig from REPL state
        -> scene_render_3d_scene(&scene_cfg)
        -> render UI panels and overlays
        -> restore flat count and predefined variable values
```

The scene frame consumes the explicit config:

```text
scene_render_3d_scene(&scene_cfg)
  -> set viewport
  -> resolve and apply clear color from scene_cfg.flat_program
  -> for each accumulation sample:
       -> prepare FrameRenderContext from scene_cfg
       -> apply projection using scene-local jitter
       -> apply camera and quality flags
       -> set up baseline lighting/material state
       -> execute user geometry through the narrow execution boundary
       -> render replay fades/highlights while they remain in scene_render.c
       -> render backdrop, grid, axes, orbit target
       -> render REPL-aware 3D overlays from frame snapshots
       -> render light indicators and other scene foreground helpers
       -> accumulate sample if accumulation AA is active
```

The exact ordering may preserve current visuals. The ownership rule still
holds: the controller prepares the data, the scene decides where stage and
overlay passes occur, and the REPL owns the command/replay semantics behind the
data.

## Two-Level Command Model

The REPL keeps source commands and flattened commands separate.

```text
source commands
  one visible/editor line per command

flattened commands
  loops expanded
  functions inlined
  conditionals resolved
  provenance retained
```

Source commands are the editing model.

Flattened commands are the execution, replay, export, and 3D annotation model.

Code outside the command pipeline should use `FlatProgramView` or a snapshot
derived from it instead of poking raw global arrays.

## Command Lifecycle

A user line follows this path:

```text
input text
  -> commit handler
  -> parser
  -> source command store
  -> flatten
  -> scene config / overlay snapshots
  -> executor boundary
```

Owned stages:

| Stage | Owner |
|-------|-------|
| Input buffer and routing | `repl_editor.c` |
| Structured commits | `repl_commit.c` |
| Parsing | `repl_parser.c` |
| Source command mutation | `repl_command_store.c` |
| Source scope/depth | `repl_source_scope.c` |
| Flattening | `repl_flatten.c` |
| User geometry execution | `repl_executor.c` |
| Export/import | `repl_export.c` |

Outside code that needs to inject commands should use the public command/input
paths instead of directly mutating command arrays.

## Controller Layer

The controller layer is the home for app-frame wiring that used to live in
`repl_core.c`.

Responsibilities:

* rebuild flat program and autonormals when dirty
* prepare replay frame clamps and restore state after rendering
* build `SceneRenderConfig` and any guide/focus snapshots from REPL state
* call `scene_render_3d_scene(&config)`
* call UI renderers in the correct order
* keep profiling section boundaries around scene and UI rendering
* provide thin wrappers for the legacy `repl_display_func()` /
  `repl_reshape_func()` surface while `sample.c` still calls those names

`imrepl_ctrl.c` may include both REPL headers and scene/UI headers. Ordinary REPL
model modules should not.

`sample.c` and `sample.h` still carry the app entry point and shared legacy
types/constants. Renaming them to `imrepl.c` and `imrepl.h` is intentionally a
separate mechanical cleanup after controller extraction, because `sample.h` is
included broadly.

## Scene Render Config

`SceneRenderConfig` is the scene's explicit per-frame input. In Option B it is
allowed to carry REPL-aware data because this sample has one frontend and no
plugin host requirement.

The controller builds the config once per frame, and `scene_render_3d_scene()`
consumes it directly without calling back into REPL globals or rebuilding the
frame inputs itself. The config currently carries the execute callback,
`FlatProgramView`, viewport, camera, animation, quality flags, lighting,
backdrop, overlay toggles, replay/HUD layout, grid tables, cursor-block
metadata, and the `SceneFocusVertex` / `SceneGuideSnapshot` snapshots needed by
3D overlays.

Scene-local accumulation jitter no longer lives in the config. Derived
per-pass data belongs in `FrameRenderContext`, for example camera world height,
focus vertex, and other values that helper renderers should share.

## Scene Layer

Scene modules own 3D rendering and 3D helper visuals.

Responsibilities:

* viewport and projection setup
* camera transform
* accumulation-buffer sampling with scene-local jitter
* baseline scene lighting and material state
* grid, axes, backdrop, light indicators, orbit target
* REPL-aware 3D overlays while they remain under `scene_*`
* replay fade rendering until the replay simplification follow-up moves it

Neutral scene modules such as `scene_grid.c`, `scene_axes.c`,
`scene_backdrop.c`, and `scene_lights.c` should remain free of REPL state
access. Transitional REPL-aware scene files must consume snapshots rather than
pulling globals directly.

## UI Layer

The UI layer owns 2D editor rendering.

Responsibilities:

* code panel
* menus and dropdowns
* search slot
* autocomplete popup
* variable panel
* color picker
* help overlay
* profile HUD
* status banners and other screen-space overlays

UI renderers draw from model/config snapshots. Mutations route through
`repl_actions`, `repl_command_store`, `repl_var_drag`, or another REPL-owned
mutation path.

## Replay Architecture

Replay is REPL-owned. The scene may render the current visual effect, but it
should not own replay policy.

Current transitional responsibilities in `scene_render.c`:

* fade-batch rendering
* replay tessellation preview
* 2D replay HUD

Target follow-ups:

* simplify fade batches into a smaller replay highlight model
* move 2D replay HUD to `ui_replay_hud.c`
* pass remaining replay draw inputs through `SceneRenderConfig`
* trim the direct replay/presentation state reads from scene rendering where
   possible

## Boundary Rules

### Live OpenGL / GLU calls

Allowed:

```text
scene_*.c
ui_*.c
repl_executor.c
sample.c        GLUT/window lifecycle and buffer swap; future `imrepl.c`
```

Avoid live GL calls in all other `repl_*` files. Text emission of GL command
names in parser/export/example/spec code is not a live GL call.

### GLUT calls

Allowed:

```text
sample.c
imrepl.c        after the sample.c rename lands
repl_editor.c   input helpers only
repl_executor.c tessellator callback setup only
```

### Controller-only scene wiring

After controller extraction, ordinary `repl_*` model files should not include
`scene_*.h`. `imrepl_ctrl.c` is the scene/UI frame-rendering exception.

Existing input/layout exceptions such as `repl_editor.c`, `repl_actions.c`,
and `repl_export.c` still include selected `ui_*` headers. They are not part of
the Phase 1 scene-controller extraction and should be addressed only by a
separate input/UI-boundary cleanup.

### Scene state access

Target rule: `scene_*` files consume `SceneRenderConfig`, `FrameRenderContext`,
or explicit snapshot structs. They should not call `repl_state_*` directly.

Known transitional exceptions live in `scene_render.c` for replay HUD/fade
handling and a few direct replay/presentation reads. Focus/guide snapshot
assembly and accumulation jitter now come from the controller or local pass
state.

### UI / scene independence

`ui_*` and `scene_*` are sibling view layers. They should not include each
other's headers. Shared render-neutral helpers belong in local shared headers
or project-wide `include/` only when broadly reusable.

## Where To Put New Code

* New REPL syntax: `repl_parser.c`, `repl_command_spec.c`, `repl_commit.c`,
  `repl_flatten.c`, and `repl_executor.c` as needed.
* New user-geometry execution behavior: `repl_executor.c`.
* New 3D world decorator: `scene_*`.
* New 3D REPL-aware overlay: current home is still `scene_*`, consuming
  `FlatProgramView` or a snapshot from `SceneRenderConfig`.
* New 2D UI: `ui_*` renderer plus `repl_*` model/action code if mutation is
  required.
* New per-frame scene/UI wiring: `imrepl_ctrl.c`.
* New app lifecycle/window wiring: `sample.c` for now, `imrepl.c` after the
  deferred rename.
* New command mutation: `repl_command_store_*`.

## Open Refactor Edges

Completed in code: controller extraction, explicit `SceneRenderConfig` handoff,
focus/guide snapshot construction, and scene-local accumulation jitter.

Remaining follow-ups:

1. Simplify replay fade rendering and move surviving replay restore policy out
   of scene rendering where practical.
2. Move the 2D replay HUD out of `scene_render.c`.
3. Slim `SceneRenderConfig` after the controller and replay follow-ups land.
4. Rename `sample.c` / `sample.h` to `imrepl.c` / `imrepl.h` as a dedicated
   mechanical cleanup, updating includes and build rules separately from the
   controller extraction.

## Header Documentation Standard

Each public API header should document:

1. Module responsibility and ownership boundary.
2. Lifecycle: initialization, per-frame calls, mutation rules.
3. Public types and what layer owns them.
4. Public functions, parameters, return values, and preconditions.
5. Important cross-module invariants, especially GL/state ownership and render
   ordering.
