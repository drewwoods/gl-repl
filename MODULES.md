# REPL Module Guide

This document is the quick map for the immediate-mode REPL source tree. For the deeper ownership reference, see [`ARCHITECTURE.md`](ARCHITECTURE.md). For the staged cleanup plan, see [`feature/push-architecture-refinement.md`](feature/push-architecture-refinement.md).

The important split is not just by filename prefix. The important split is by ownership:

```text
repl_*   = REPL language, source model, flat geometry model, replay model, controller
scene_*  = independent 3D view: camera, frame setup, world decorators, callback hooks
ui_*     = independent 2D view: code panel, menus, overlays, popups, HUDs
```

`scene_*` and `ui_*` are views. They render from per-frame config snapshots. They should not own REPL semantics.

`repl_*` is mostly model/controller code. It parses source, owns the flat program, owns replay state, and builds the configs/plans consumed by the scene and UI views.

## Core Tenets

1. **The REPL owns the user program.** The REPL parses the C-like language, stores source commands, flattens loops/functions/conditionals, owns replay state, and describes the user geometry and REPL-aware geometry annotations.
2. **The executor is the narrow live-GL escape hatch for user geometry.** General `repl_*` modules should not casually call OpenGL. The executor turns a `FlatProgramView` into live GL.
3. **The scene owns the 3D stage, not the actor.** The scene establishes viewport, clear color, projection, camera, accumulation-buffer samples, lights, grid, axes, backdrop, orbit target, and other neutral 3D decorators.
4. **The scene calls generic user callbacks.** `scene_render_3d_scene()` should not know whether the user geometry came from the REPL, an exporter, a test, or a future alternate geometry source.
5. **The UI owns 2D presentation only.** UI renderers draw from per-frame config/model snapshots and route mutations through `repl_*` actions or command-store APIs.
6. **Replay is REPL policy.** Replay PC, replay mode, fade/highlight state, and replay variable snapshots belong behind the REPL geometry callback, not in generic scene config.

## Intended Frame Shape

At the top level, the REPL builds the models/configs and wires the scene callback:

```text
repl display/frame entry
  -> rebuild flat program if dirty
  -> build ReplGeometryRenderPlan
  -> build SceneRender3DConfig
  -> resolve REPL clear color into scene config
  -> install generic user draw callback backed by the REPL plan
  -> scene_render_3d_scene(&scene_cfg)
  -> build UiRenderConfig
  -> ui_render(&ui_cfg)
```

Inside the scene:

```text
scene_render_3d_scene
  -> set viewport / clear / projection / camera / quality state
  -> draw scene-owned decorators
  -> call generic user geometry callback
  -> call generic user annotation callback, if configured
  -> draw scene-owned foreground decorators
  -> finish accumulation-buffer sample/frame
```

The scene decides where the generic hooks are. The REPL decides what those hooks draw.

## Responsibility Layers

### 1. REPL command pipeline: source -> flatten -> execute

The central data flow. Edits mutate the source command array. Execution, replay, export, and geometry annotations consume the flattened program or plans derived from it.

| Module | Role |
|--------|------|
| `repl_core` | Top-level REPL orchestration, display/frame entry point, normalization wrapper |
| `repl_command_spec` | Declarative descriptors for fixed-arity GL-like commands |
| `repl_parser` | Source-line parser and canonical `GLCmd.source[]` generation |
| `repl_source_scope` | Source prefix-depth cache, indent helpers, block lookup |
| `repl_command_store` | Insert/delete/replace/load API over the source command array |
| `repl_commit` | Float declarations, variable assignments, structured block commits |
| `repl_flatten` | Explicit source-to-flat program builder for loops, functions, and `if` blocks |
| `repl_executor` | Narrow live-GL dispatch boundary for flat user geometry |
| `repl_eval` | Expression evaluator |
| `cmd_format` | Pure indentation/depth computation |

### 2. Editor and input controller

These modules route input and mutate REPL-owned state through focused ownership APIs.

| Module | Role |
|--------|------|
| `repl_editor` | Keyboard/mouse dispatcher, commit orchestration, `feed_line` |
| `repl_actions` | Config/menu side effects and action dispatch |
| `repl_keys` | Keybinding constants |
| `repl_camera_controls` | Scene camera drag + momentum state |
| `repl_clipboard` | Line selection and copy/cut/paste |
| `repl_undo` | Snapshot rings and restore paths |
| `repl_search` | Search state and navigation |
| `repl_var_drag` | Variable slider drag transaction + writeback |
| `repl_inline_rename` | Scene rename input buffer |

Input/controller modules may mutate models. Render modules should not.

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

`repl_replay` owns replay semantics. The scene should not know what a REPL replay is.

### 4. 3D scene rendering

`scene_*` owns the 3D frame and neutral world decorators. It may call generic user callbacks, but should not inspect REPL source, flat-command provenance, replay state, or editor state.

| Module | Role |
|--------|------|
| `scene_render` | 3D frame setup, viewport, clear, projection, camera, accumulation loop, generic callback hook order |
| `scene_render_types` | Scene config/context types and generic user callback types |
| `scene_grid` | Grid theme rendering |
| `scene_axes` | Axes theme rendering |
| `scene_backdrop` | Backdrop/environment rendering |
| `scene_lights` | Scene lighting baseline and light indicators |
| `scene_transform_utils` | Small GL matrix helpers used by renderers |
| `scene_geometry_guides` | Current transitional module: REPL-aware 3D guide rendering |
| `scene_transform_guides` | Current transitional module: REPL-aware transform guide rendering |
| `scene_overlays` | Current transitional module: REPL-aware outlines/labels/normals |

The last three modules draw inside the scene, but their inputs are REPL-specific. They should either move toward `repl_geometry_*` ownership or be fed by a generic enough plan that the `scene_` prefix remains honest.

### 5. 2D UI rendering

`ui_*` owns screen-space drawing from per-frame UI config/model state.

| Module | Role |
|--------|------|
| `ui_panels` | Code-panel rows, overlay viewport bracket, scene status banner |
| `ui_menu_bar` | Menus, dropdowns, pinned buttons, search slot |
| `ui_color_picker` | Floating HSV/alpha picker and literal color swatches |
| `ui_help_overlay` | Modal F1 help |
| `ui_variable_panel` | Floating variable slider panel |
| `ui_autocomplete_panel` | Completion popup |
| `ui_profile_panel` | CPU timing HUD |
| `repl_code_panel_layout` | Pure text wrapping model, no GL |
| `repl_code_panel_document` | Code-panel row/document model, no GL |

UI renderers read models and draw. Mutations go through `repl_actions`, `repl_command_store`, `repl_var_drag`, or another REPL-owned mutation API.

### 6. Persistence, audio, instrumentation, lifecycle

| Module | Role |
|--------|------|
| `repl_export` | Save/load, typed export scaffold, workspace headers, code-panel dumps |
| `repl_audio` | Playlist engine + persisted audio config |
| `prof` | Project-wide CPU timing instrumentation |
| `sample` | `main()`, GLUT callback wiring, GLUT buffer swap |
| `gl_stub_counts` | `USE_GL_STUBS` symbol tracking |

## Scene Render Config Direction

The scene config should describe the 3D frame and generic draw hooks. It should not be the dumping ground for flat-program, replay, and editor state.

Preferred shape:

```c
typedef enum SceneUserDrawPass {
    SCENE_USER_DRAW_MAIN,
    SCENE_USER_DRAW_ANNOTATIONS_3D,
} SceneUserDrawPass;

typedef struct SceneUserDrawRequest {
    SceneUserDrawPass pass;
    const struct SceneFrameContext *scene;
    int accum_sample_idx;
    int accum_sample_count;
    float alpha_scale;
} SceneUserDrawRequest;

typedef void (*SceneUserDrawFn)(const SceneUserDrawRequest *request,
                                void *user_data);

typedef struct SceneUserDrawCallbacks {
    SceneUserDrawFn draw;
    void (*reset)(void *user_data);
    void *user_data;
} SceneUserDrawCallbacks;

typedef struct SceneRender3DConfig {
    int scene_x, scene_y, scene_w, scene_h;
    int viewport_w, viewport_h;

    float clear_rgba[4];

    float cam_dist;
    float cam_rx, cam_ry;
    float cam_tx, cam_ty, cam_tz;
    float cam_motion_glow;

    int multisample_enabled;
    int line_smooth_enabled;
    int wireframe;

    int use_accum;
    int accum_aa_enabled;
    int accum_samples;

    int grid_theme;
    int grid_extent_idx;
    int grid_major_idx;
    float grid_major_steps[GRID_MAJOR_COUNT];
    float grid_extents[GRID_EXTENT_COUNT];

    int axes_theme;
    int backdrop_mode;

    int user_lighting_enabled;
    SceneLight lights[MAX_LIGHTS];
    int show_light_indicators;

    SceneUserDrawCallbacks user_draw;
} SceneRender3DConfig;
```

REPL-specific data belongs in the callback `user_data`, for example a `ReplGeometryRenderPlan`.

## How `repl_*` Calls the Scene

The REPL frame path should build separate snapshots:

```text
SceneRender3DConfig       scene_cfg
ReplGeometryRenderPlan    geometry_plan
UiRenderConfig            ui_cfg
```

Then wire the REPL plan into the generic scene callback:

```c
void repl_display_frame(void)
{
    SceneRender3DConfig scene_cfg;
    ReplGeometryRenderPlan geom_plan;
    UiRenderConfig ui_cfg;

    repl_geometry_render_plan_build(&geom_plan);
    scene_render_3d_config_build(&scene_cfg);

    repl_geometry_resolve_clear_color(&geom_plan, scene_cfg.clear_rgba);

    scene_cfg.user_draw.draw = repl_geometry_scene_draw_callback;
    scene_cfg.user_draw.reset = repl_geometry_scene_reset_callback;
    scene_cfg.user_draw.user_data = &geom_plan;

    scene_render_3d_scene(&scene_cfg);

    ui_render_config_build(&ui_cfg);
    ui_render(&ui_cfg);
}
```

`scene_render_3d_scene()` does not know that `user_data` points at a REPL geometry plan.

## Replay Handling

Replay should be handled inside the REPL geometry callback, not by adding a scene-level replay callback.

```text
scene_render_3d_scene
  -> generic user draw callback
       -> repl_geometry_render_main
            -> normal draw
            -> replay-limited draw
            -> replay fade/highlight draw
```

If full fade batches become too costly or invasive, simplify the replay visual model:

```text
fade only the most recently completed vertex or polygon
```

Candidate model:

```c
typedef enum ReplReplayHighlightKind {
    REPL_REPLAY_HIGHLIGHT_NONE,
    REPL_REPLAY_HIGHLIGHT_VERTEX,
    REPL_REPLAY_HIGHLIGHT_POLYGON,
} ReplReplayHighlightKind;

typedef struct ReplReplayHighlight {
    ReplReplayHighlightKind kind;
    int flat_begin_idx;
    int flat_end_idx;
    float alpha;
} ReplReplayHighlight;
```

That keeps replay policy in `repl_*` and keeps the scene generic.

## Boundary Rules

### Live OpenGL / GLU calls

Allowed:

```text
scene_*.c
ui_*.c
repl_executor.c
sample.c        only for GLUT/window lifecycle and buffer swap
```

Avoid live GL calls in general `repl_*` files. Text emission of GL command names in parser/export/example code is not a live GL call.

### GLUT calls

Allowed:

```text
sample.c
repl_editor.c   only through local editor helpers where possible
```

### Scene purity

Pure scene modules should consume only scene config/context and neutral helpers. They should not read:

```text
repl_state_* APIs
g_cmds / g_flat_cmds
g_predef_vars
repl_replay_* state
source command indexes
edit-line state
FlatProgramView
```

The REPL frame compositor may build scene config from REPL state. The scene renderer should consume the resulting config.

### UI / scene independence

`ui_*` and `scene_*` are sibling view layers. They should not include each other's headers. Shared render-neutral helpers belong under `include/`.

## Where to Put New Code

- New REPL syntax: `repl_parser.c`, `repl_command_spec.c`, `repl_commit.c`, `repl_flatten.c`, and `repl_executor.c` as needed.
- New user-geometry execution behavior: prefer `repl_executor.c` or a deliberately named executor-adjacent file.
- New geometry model/plan data: `repl_geometry_*`.
- New replay policy: `repl_replay.c` / `repl_replay_plan.c`.
- New 3D world decorator: `scene_*`.
- New 3D REPL annotation: REPL-owned plan plus executor/callback rendering.
- New 2D UI: `ui_*` renderer plus `repl_*` model/action code if mutation is required.
- New command mutation: use `repl_command_store_*`.

## Open Refactor Edges

- `SceneRenderConfig` currently carries too much REPL execution/replay data. Split it into a scene-only config plus REPL-owned geometry/overlay plans.
- `scene_render_3d_scene()` should accept an explicit config argument rather than rebuilding all state internally.
- `scene_overlays.c`, `scene_geometry_guides.c`, and `scene_transform_guides.c` currently sound scene-owned but contain REPL-aware concepts. Either migrate them toward REPL geometry annotation modules or make their input generic enough that scene ownership is honest.
- Replay fade rendering should move out of `scene_render.c`. The scene should call a generic callback; the REPL callback should decide how replay is drawn.

## Header Documentation Standard

Each public API header should document:

1. Module responsibility and ownership boundary.
2. Lifecycle: initialization, per-frame calls, mutation rules.
3. Public types and what layer owns them.
4. Public functions, parameters, return values, and preconditions.
5. Important cross-module invariants, especially GL/state ownership and callback ordering.
