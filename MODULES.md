# REPL Module Guide

This is the quick map for the immediate-mode REPL source tree. For the deeper
ownership reference, see [`ARCHITECTURE.md`](ARCHITECTURE.md). For the staged
cleanup plan, see
[`feature/push-architecture-refinement.md`](feature/push-architecture-refinement.md).

The current target is the Option B controller architecture from the refinement
plan. The older `ReplGeometryRenderPlan` plus generic scene-callback direction
is superseded.

## Ownership Split

```text
repl_*        = REPL language, source model, flat program, replay model, input/model controllers
imrepl_ctrl   = app-frame controller between REPL state and scene/UI rendering
scene_*       = 3D stage: camera, frame setup, decorators, 3D overlays
ui_*          = 2D editor chrome: panels, menus, overlays, popups, HUDs
sample.c/h    = current GLUT app shell and legacy shared header
imrepl.c/h    = future app shell/shared header name, replacing sample.c/h
```

Treat prefixes as ownership boundaries, not as a catch-all naming scheme.
`repl_*` is for REPL language/editor/source/replay model behavior. App-shell
and app-service code belongs under `imrepl_*` once the deferred rename lands;
generic instrumentation keeps neutral names such as `prof`. `repl_audio` is a
legacy-named app service and should be revisited in the namespace audit rather
than copied as a pattern.

`scene_*` and `ui_*` are views. They render from per-frame config or model
snapshots. They should not own REPL mutation paths.

`repl_*` owns the user program, editor/controller behavior, replay policy, and
the data snapshots passed to the views. `imrepl_ctrl.c` owns frame-level
scene/UI render wiring. It is deliberately outside the `repl_*` namespace.

`sample.c` and `sample.h` keep their current names during controller extraction
to avoid burying architecture changes under include churn. Rename them to
`imrepl.c` / `imrepl.h` in a later mechanical pass.

## Core Tenets

1. **The REPL owns the user program.** It parses source, stores source commands,
   flattens loops/functions/conditionals, owns variables, and owns replay
   policy.
2. **The executor is the narrow live-GL gate.** `repl_executor.c` turns a
   `FlatProgramView` into user geometry GL calls.
3. **The scene owns the 3D stage.** Scene sets viewport, projection, camera,
   accumulation, lighting baseline, grid, axes, backdrop, orbit target, and 3D
   overlays from config.
4. **The UI owns 2D editor chrome.** UI renderers draw from snapshots and route
   mutations through REPL-owned action/store APIs.
5. **The controller wires the frame.** `imrepl_ctrl.c` builds
   `SceneRenderConfig`, calls `scene_render_3d_scene(&cfg)`, then drives UI
   rendering. `repl_core.c` keeps model/pipeline work.
6. **Replay stays REPL-owned.** Scene may render transitional replay visuals,
   but replay PC/mode/baseline/fade policy belongs in `repl_replay.c` or a
   future replay-plan module.

## Intended Frame Shape

At the top level, `sample.c` registers the GLUT display callback and forwards
directly to the controller (no shim layer):

```text
sample.c glutDisplayFunc -> imrepl_ctrl_display_frame
  -> rebuild autonormals / flat program if dirty
  -> prepare replay frame clamp if needed
  -> build SceneRenderConfig from REPL state
  -> scene_render_3d_scene(&scene_cfg)
  -> render UI panels / popups / HUDs
  -> restore temporary replay/predef state
```

Inside the scene:

```text
scene_render_3d_scene(&scene_cfg)
  -> set viewport / clear / projection / camera / quality state
  -> execute user geometry through the narrow execution boundary
  -> render replay fade/highlight visuals while still transitional
  -> render scene-owned decorators
  -> render REPL-aware 3D overlays from supplied snapshots
  -> finish accumulation-buffer sample/frame
```

The scene decides ordering of 3D passes. The REPL/controller decides the data
and semantics behind those passes.

## Responsibility Layers

### 1. REPL command pipeline: source -> flatten -> execute

Edits mutate the source command array. Execution, replay, export, and geometry
annotations consume the flattened program or snapshots derived from it.

| Module | Role |
|--------|------|
| `repl_core` | Core REPL model/pipeline, normalization wrapper, and legacy lifecycle wrappers that forward to the controller |
| `imrepl_ctrl` | App-frame controller: display/reshape, scene config build, scene/UI render ordering |
| `repl_command_spec` | Declarative descriptors for fixed-arity GL-like commands |
| `repl_parser` | Source-line parser and canonical `GLCmd.source[]` generation |
| `repl_source_scope` | Source prefix-depth cache, indent helpers, block lookup |
| `repl_pipeline` | Public pipeline and lifecycle surface for frame orchestration (`flatten`, autonormal, replay/bootstrap snapshots) |
| `repl_command_store` | Insert/delete/replace/load API over the source command array |
| `repl_commit` | Float declarations, variable assignments, structured block commits |
| `repl_flatten` | Source-to-flat program builder for loops, functions, and `if` blocks |
| `repl_executor` | Narrow live-GL dispatch boundary for flat user geometry |
| `repl_eval` | Expression evaluator |
| `cmd_format` | Pure indentation/depth computation |

### 2. Editor and input controllers

These modules route input and mutate REPL-owned state through focused ownership
APIs.

| Module | Role |
|--------|------|
| `repl_editor` | Keyboard/mouse dispatcher, commit orchestration, `feed_line` |
| `repl_actions` | Config/menu side effects and action dispatch |
| `repl_keys` | Keybinding constants |
| `repl_camera_controls` | Scene camera drag and momentum state |
| `repl_clipboard` | Line selection and copy/cut/paste |
| `repl_undo` | Snapshot rings and restore paths |
| `repl_search` | Search state and navigation |
| `repl_search.h` | Search query helpers and input routing API |
| `repl_var_drag` | Variable slider drag transaction and writeback |
| `repl_inline_rename` | Scene rename input buffer |

Input/controller modules may mutate models. Render modules should not. Some
input modules still include `repl_layout.h` for hit-testing; that is a known
boundary exception outside the Phase 1 scene-controller extraction.

### 3. REPL domain models

These modules own REPL state that is not itself a renderer.

| Module | Role |
|--------|------|
| `repl_state` | Typed runtime-state facade over historical globals |
| `repl_scenes` | User-scene slots, workspace directory, LRU eviction |
| `repl_example_loader` | Built-in example loading and active tracking |
| `repl_examples` | Built-in example data |
| `repl_autocomplete` | Completion model: matches, selection, ghost text, hints |
| `repl_autonormal` | Auto-generated `glNormal3f` maintenance |
| `repl_replay` | Replay state machine, replay PC/mode, fade/highlight inputs |
| `repl_replay_annotations` | Source-line replay text expansion for the code panel |

`repl_replay` owns replay semantics. Scene rendering should consume replay
snapshots or documented transitional helpers, not own replay state.

### 4. 3D scene rendering

`scene_*` owns the 3D frame and world/decorator passes. Under Option B, scene
code may consume `FlatProgramView` and command provenance when those values are
on the per-frame config or guide snapshots.

| Module | Role |
|--------|------|
| `scene_render` | 3D frame setup from explicit config, viewport, clear, projection, camera, accumulation loop, user-geometry execution point, transitional replay/HUD code |
| `scene_render_types` | Scene config/context types, including focus/guide snapshots and the narrow execution hook |
| `scene_grid` | Grid theme rendering |
| `scene_axes` | Axes theme rendering |
| `scene_backdrop` | Backdrop/environment rendering |
| `scene_lights` | Scene lighting baseline and light indicators |
| `scene_transform_utils` | Small GL matrix helpers used by renderers |
| `scene_guides_shared` | Snapshot/planning types for REPL-aware 3D guides |
| `scene_geometry_guides` | REPL-aware vertex/primitive guide rendering from snapshots |
| `scene_transform_guides` | REPL-aware transform guide rendering from snapshots |
| `scene_overlays` | REPL-aware outlines, labels, normals |
| `ui_replay_hud` | 2D replay HUD rendered from `SceneRenderConfig` in the UI layer |

Neutral scene files should stay free of REPL state access. `scene_render.c`
still has transitional direct REPL reads for fade batches and accumulation AA.
R1 in `feature/push-architecture-refinement.md` removes all of these: the
controller builds a `ReplayFadePlan` snapshot; the replay HUD now lives in
`ui_replay_hud.c`; and accumulation-AA settings move to `SceneRenderConfig`.

### 5. 2D UI rendering

`ui_*` owns screen-space drawing from per-frame UI/config/model state.

| Module | Role |
|--------|------|
| `ui_panels` | Code-panel rows, overlay viewport bracket, scene status banner |
| `repl_layout` | Pure scene/code-panel rectangle geometry, no GL |
| `ui_menu_bar` | Menus, dropdowns, pinned buttons, search slot |
| `ui_color_picker` | Floating HSV/alpha picker and literal color swatches |
| `ui_help_overlay` | Modal F1 help |
| `ui_variable_panel` | Floating variable slider panel |
| `ui_autocomplete_panel` | Completion popup |
| `ui_profile_panel` | CPU timing HUD |
| `repl_code_panel_layout` | Pure text wrapping model, no GL |
| `repl_code_panel_document` | Code-panel row/document model, no GL |

UI renderers read models and draw. Mutations go through `repl_actions`,
`repl_command_store`, `repl_var_drag`, or another REPL-owned mutation API.

### 6. Persistence, audio, instrumentation, lifecycle

| Module | Role |
|--------|------|
| `repl_export` | Save/load, typed export scaffold, workspace headers, code-panel dumps |
| `repl_audio` | Legacy-named app-level playlist engine and persisted audio config; namespace audit candidate |
| `prof` | Project-wide CPU timing instrumentation |
| `sample` | Current `main()`, GLUT callback wiring, buffer swap, and legacy shared header; future rename target is `imrepl` |
| `gl_stub_counts` | `USE_GL_STUBS` symbol tracking |

## Scene Render Config Direction

`SceneRenderConfig` is the scene's explicit per-frame input. In this codebase it
is allowed to carry REPL-aware data because there is one frontend and no plugin
host requirement.

The controller builds it once per frame. It should include:

* scene rectangle and window dimensions
* camera pose and camera-motion glow
* quality flags and accumulation settings
* grid/axes/backdrop/light settings
* flat program view for execution and overlays
* guide/focus/replay snapshots needed by 3D overlay passes
* the existing narrow execution hook, until direct executor call cleanup is
  worth doing

Scene-local jitter no longer belongs in the config. `FrameRenderContext` holds
the per-pass state the scene helpers share.

It should not require `scene_render.c` to call `scene_render_config_build()` or
pull from `repl_state_*` during the frame.

## Boundary Rules

### Live OpenGL / GLU calls

Allowed:

```text
scene_*.c
ui_*.c
repl_executor.c
sample.c        future `imrepl.c`
```

Parser/spec/export/example modules may contain GL command names as text. That
is not a live GL call.

### Scene/UI includes from `repl_*`

Current rule:

```text
imrepl_ctrl.c may include scene/UI render headers.
repl_core.c should not include scene/UI headers after the controller move.
```

Existing exceptions: `repl_editor.c`, `repl_actions.c`, and `repl_export.c`
include selected `ui_*` headers. The `repl_editor.c` exception is eliminated
in Phase 2 by moving cross-layer input routing to `imrepl_ctrl.c`. Do not
silently expand these exceptions.

### Typed-state facade boundary (Phase 2 target)

`repl_state.h` will split into two headers (R6 in the refinement plan):

```text
repl_state_views.h   — read-only accessors; safe to include from scene_* and ui_*
repl_state_owners.h  — mutating accessors; owner modules and controller only
```

The rule is enforced by Makefile checks such as `check-scene-no-repl-state-mut`,
`check-state-boundaries`, and `check-views-no-owners`. `scene_*.c` and
`ui_*.c` include `repl_state_views.h` only; any accidental include of
`repl_state_owners.h` in those files is now caught automatically.

### Layout geometry (Phase 2 target)

`repl_layout.c` / `repl_layout.h` own `repl_layout_scene_rect` and
`repl_layout_code_panel_rect` (R3). Non-UI callers
(`imrepl_ctrl.c`, `repl_editor.c`, `repl_export.c`, tests) include
`repl_layout.h`; `ui_panels.h` stays on the UI/render side.

### UI / scene independence

`ui_*` and `scene_*` should not include each other's headers. Shared
render-neutral helpers belong in explicit shared headers.

## Where To Put New Code

* New REPL syntax: `repl_parser.c`, `repl_command_spec.c`,
  `repl_commit.c`, `repl_flatten.c`, and `repl_executor.c` as needed.
* New user-geometry execution behavior: `repl_executor.c`.
* New 3D world decorator: `scene_*`.
* New 3D REPL-aware overlay: current home is still `scene_*`, consuming
  `FlatProgramView` or guide snapshots from `SceneRenderConfig`.
* New 2D UI: `ui_*` renderer plus REPL-owned model/action code when mutation
  is needed.
* New per-frame scene/UI wiring: `imrepl_ctrl.c`.
* New app lifecycle/window wiring: `sample.c` for now, `imrepl.c` after the
  deferred rename.
* New command mutation: `repl_command_store_*`.

## Open Refactor Edges

Completed: controller extraction, explicit `SceneRenderConfig` handoff,
focus/guide snapshot construction, scene-local accumulation jitter, and
app-shell shim removal.

Phase 2 follow-ups are specified in `feature/push-architecture-refinement.md`.
Suggested order:

```
R10-phase1  delete stale GLUT decls from repl_core.h (zero risk, do first)
R1          replay/HUD migration — highest leverage
R2          UI → REPL mutation holes (parallel with R1)
R3          extract repl_layout.c
R5          slim SceneRenderConfig (requires R1)
R6          split repl_state facade (requires R1 + R2)
R7          add view-side grep guards
R11         harden file-level grep guards; shrink allowlists as Phase 2 lands
R4          controller off repl_core_internal.h
R10-ph2-5   dissolve repl_core.c into natural owners
R12         consolidate public REPL APIs into one concise repl.h
R8          sample → imrepl rename (last, mechanical)
```
