# REPL Architecture

> For the quick module map, see [`MODULES.md`](MODULES.md). This document is the ownership reference for the immediate-mode REPL.

## Overview

The immediate-mode REPL is organized around three major components:

```text
repl_*   = language, source model, flattened geometry model, replay model, controller
scene_*  = independent 3D view/decorator layer
ui_*     = independent 2D view layer
````

The main design rule is simple:

```text
The REPL owns the user program.
The scene owns the 3D stage.
The UI owns the 2D editor/view.
```

The scene may call a generic callback to draw user geometry inside the 3D frame, but the scene should not know that the callback is backed by a REPL flat program. That keeps scene rendering reusable and prevents REPL semantics from leaking into camera/grid/axis/backdrop/light code.

## Three Core Components

### 1. REPL model/controller

The REPL owns the C-like input language and the user-authored geometry program.

Responsibilities:

* parse source lines into source commands
* maintain the source command array
* flatten loops/functions/conditionals into an executable flat program
* own predefined variables and expression evaluation
* own replay state and replay policy
* describe geometry, outlines, and annotations as REPL-owned plans
* provide the callback that draws user geometry inside a scene frame

The REPL should be able to describe geometry in a non-rendering way, much like `repl_export.c` describes emitted C. Live GL execution should remain isolated to the narrow executor boundary.

### 2. Scene view

The scene layer owns the 3D frame and neutral 3D decorators.

Responsibilities:

* viewport rectangle
* clear color
* projection
* camera
* accumulation-buffer and jitter loop
* quality flags such as multisample, line smoothing, and wireframe
* baseline scene lighting
* grid
* axes
* backdrop
* light indicators
* orbit target / camera manipulation visualization
* generic callback hook points for user geometry and 3D annotations

The scene should be able to render an empty environment without a REPL program. If no user callback is installed, the scene should still be capable of drawing the camera/world decorators.

### 3. UI view

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

The UI renders from per-frame config/model snapshots. It should route mutations through `repl_*` actions, command-store APIs, or other REPL-owned mutation paths.

## Target Frame Pipeline

The top-level frame path should build all view inputs before rendering:

```text
repl display/frame entry
  -> rebuild flat program if dirty
  -> build ReplGeometryRenderPlan
  -> build SceneRender3DConfig
  -> resolve REPL-owned clear color into scene config
  -> install generic user-draw callbacks into scene config
  -> scene_render_3d_scene(&scene_cfg)
  -> build UiRenderConfig
  -> ui_render(&ui_cfg)
```

The scene frame should then run independently:

```text
scene_render_3d_scene
  -> for each accumulation sample:
       -> set viewport
       -> clear color/depth buffers
       -> apply projection + jitter
       -> apply camera
       -> apply quality flags
       -> setup baseline scene lighting/material state
       -> draw scene decorators
       -> call user draw callback: SCENE_USER_DRAW_MAIN
       -> call user draw callback: SCENE_USER_DRAW_ANNOTATIONS_3D
       -> draw scene foreground decorators
       -> accumulate sample if accumulation AA is active
  -> return final accumulated frame
```

The exact order can preserve current visuals. For example, if a translucent grid blends better after user geometry, keep that order. The ownership rule still holds: scene decides where generic hooks happen, while REPL decides what its callback draws.

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

Flattened commands are the execution/replay model.

Everything that draws user geometry should consume a `FlatProgramView` or a REPL-owned plan derived from one. Code outside the REPL should not poke raw command arrays.

## Command Lifecycle

A user line follows this path:

```text
input text
  -> commit handler
  -> parser
  -> source command store
  -> flatten
  -> geometry render plan
  -> executor callback
```

Owned stages:

| Stage                    | Owner                  |
| ------------------------ | ---------------------- |
| Input buffer and routing | `repl_editor.c`        |
| Structured commits       | `repl_commit.c`        |
| Parsing                  | `repl_parser.c`        |
| Source command mutation  | `repl_command_store.c` |
| Source scope/depth       | `repl_source_scope.c`  |
| Flattening               | `repl_flatten.c`       |
| Execution                | `repl_executor.c`      |
| Export/import            | `repl_export.c`        |

This is a load-bearing boundary. Outside code that needs to inject commands should use the existing public command/input paths instead of directly mutating command arrays.

## REPL Geometry Rendering

REPL geometry rendering has two conceptual parts:

1. a mostly pure render plan
2. a narrow live-GL executor/callback adapter

### Geometry render plan

The plan is REPL-owned. It may contain REPL-specific concepts because the scene never inspects it.

Candidate shape:

```c
typedef struct ReplGeometryRenderPlan {
    FlatProgramView flat_program;

    int wireframe;
    int user_lighting_enabled;

    int replay_active;
    int replay_mode;
    int replay_base_limit;

    int show_current_poly;
    int show_vertex_points;
    int show_vertex_labels;
    int show_normal_vectors;
    int show_vertex_guides;
    int show_transform_guides;

    int edit_line_idx;
    int cursor_block_begin_idx;
    int cursor_block_end_idx;
    int cursor_block_source_line;

    ReplReplayHighlight replay_highlight;
} ReplGeometryRenderPlan;
```

This structure should not be part of the pure scene config. It belongs in the callback `user_data` passed from the REPL frame compositor to the scene.

### Live executor callback

The callback is the seam where REPL geometry enters the 3D frame.

```c
void repl_geometry_scene_draw_callback(const SceneUserDrawRequest *request,
                                       void *user_data)
{
    ReplGeometryRenderPlan *plan = user_data;

    switch (request->pass) {
    case SCENE_USER_DRAW_MAIN:
        repl_geometry_render_main(plan, request);
        break;

    case SCENE_USER_DRAW_ANNOTATIONS_3D:
        repl_geometry_render_annotations(plan, request);
        break;
    }
}
```

The adapter can delegate to `repl_executor.c` for actual flat-program execution. The important rule is that the scene does not inspect `ReplGeometryRenderPlan`.

## Scene Render Config

The scene render config should be a complete per-frame snapshot of the 3D environment and generic draw hooks.

It should not include REPL source/flat/replay internals.

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
    int scene_x;
    int scene_y;
    int scene_w;
    int scene_h;

    int viewport_w;
    int viewport_h;

    float clear_rgba[4];

    float cam_dist;
    float cam_rx;
    float cam_ry;
    float cam_tx;
    float cam_ty;
    float cam_tz;
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

Derived per-frame data belongs in a context:

```c
typedef struct SceneFrameContext {
    SceneRender3DConfig config;

    float camera_world_y;
    int camera_below_water_surface;

    int focus_point_valid;
    float focus_point[3];
} SceneFrameContext;
```

If focus-point data depends on the REPL edit line, the REPL should compute it and pass it as generic optional scene focus data. The scene should not inspect the document to find it.

## `scene_render_3d_scene()` Main Loop

Target signature:

```c
void scene_render_3d_scene(const SceneRender3DConfig *config);
```

Target flow:

```c
void scene_render_3d_scene(const SceneRender3DConfig *config)
{
    int sample_count = scene_resolve_sample_count(config);

    glViewport(config->scene_x, config->scene_y,
               config->scene_w, config->scene_h);

    glClearColor(config->clear_rgba[0],
                 config->clear_rgba[1],
                 config->clear_rgba[2],
                 config->clear_rgba[3]);

    if (scene_using_accumulation(config))
        glClear(GL_ACCUM_BUFFER_BIT);

    for (int sample_idx = 0; sample_idx < sample_count; sample_idx++) {
        SceneFrameContext frame_ctx;

        scene_frame_context_prepare(&frame_ctx, config,
                                    sample_idx, sample_count);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        scene_apply_projection(&frame_ctx);
        scene_apply_camera_view(&frame_ctx);
        scene_apply_quality_config(&frame_ctx);
        scene_lights_setup(&frame_ctx);

        scene_backdrop_render(&frame_ctx);
        scene_grid_render(&frame_ctx);
        scene_axes_render(&frame_ctx);

        scene_call_user_draw(&frame_ctx,
                             SCENE_USER_DRAW_MAIN,
                             sample_idx,
                             sample_count);

        scene_call_user_draw(&frame_ctx,
                             SCENE_USER_DRAW_ANNOTATIONS_3D,
                             sample_idx,
                             sample_count);

        scene_lights_render_indicators(&frame_ctx);
        scene_orbit_target_render(&frame_ctx);

        if (scene_using_accumulation(config))
            glAccum(GL_ACCUM, 1.0f / (float)sample_count);
    }

    if (scene_using_accumulation(config))
        glAccum(GL_RETURN, 1.0f);
}
```

The callback helper should be tiny and generic:

```c
static void scene_call_user_draw(const SceneFrameContext *frame_ctx,
                                 SceneUserDrawPass pass,
                                 int sample_idx,
                                 int sample_count)
{
    const SceneUserDrawCallbacks *cb = &frame_ctx->config.user_draw;

    if (!cb->draw)
        return;

    SceneUserDrawRequest request = {
        .pass = pass,
        .scene = frame_ctx,
        .accum_sample_idx = sample_idx,
        .accum_sample_count = sample_count,
        .alpha_scale = 1.0f,
    };

    cb->draw(&request, cb->user_data);
}
```

No REPL header is required for this helper.

## How REPL Calls the Scene

The REPL display path should prepare both the scene config and the REPL geometry plan.

```c
void repl_core_display_frame(void)
{
    SceneRender3DConfig scene_cfg;
    ReplGeometryRenderPlan geom_plan;
    UiRenderConfig ui_cfg;

    repl_flatten_if_dirty();

    repl_geometry_render_plan_build(&geom_plan);
    scene_render_3d_config_build_from_repl_state(&scene_cfg);

    /*
     * glClearColor is part of the user command stream, but clearing happens
     * before the scene calls the user geometry callback. Resolve it up front.
     */
    repl_geometry_resolve_clear_color(&geom_plan, scene_cfg.clear_rgba);

    scene_cfg.user_draw.draw = repl_geometry_scene_draw_callback;
    scene_cfg.user_draw.reset = repl_geometry_scene_reset_callback;
    scene_cfg.user_draw.user_data = &geom_plan;

    scene_render_3d_scene(&scene_cfg);

    ui_render_config_build(&ui_cfg);
    ui_render(&ui_cfg);
}
```

The dependency direction is the point:

```text
repl builds scene config
repl installs generic callbacks
scene invokes callbacks
scene does not inspect repl data
```

## Replay Architecture

Replay is REPL-owned.

The scene layer should not know about replay PC, replay mode, fade batches, variable snapshots, source annotations, or command expansion.

Recommended design:

```text
repl_replay
  owns replay state machine

repl_replay_plan
  converts replay state into draw limits/highlight/fade data

repl_geometry_render_plan
  embeds the replay draw plan

repl_geometry_scene_draw_callback
  draws replay through repl_executor
```

Do not add a scene-level replay callback.

Avoid this:

```c
scene_cfg.draw_replay = repl_replay_draw_callback;
```

Prefer this:

```c
scene_cfg.user_draw.draw = repl_geometry_scene_draw_callback;
```

Inside that callback:

```c
if (plan->replay_active) {
    repl_geometry_render_replay(plan, request);
} else {
    repl_geometry_render_normal(plan, request);
}
```

### Fade batch simplification

If the current fade-batch logic becomes too complex, simplify replay visuals instead of letting the scene absorb replay policy.

Minimum acceptable replay visual:

```text
draw the program up to the current replay limit
fade/highlight only the most recently drawn vertex or polygon
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

Then replay rendering becomes:

```text
draw flat commands up to replay_base_limit
draw latest vertex/polygon highlight with alpha
draw optional annotations
```

## 3D Annotations and Outlines

There are two kinds of 3D visuals.

### Scene-owned decorators

These are independent of the REPL program:

* grid
* axes
* backdrop
* scene light indicators
* orbit target
* camera/world guides

These belong in `scene_*`.

### REPL-owned geometry annotations

These depend on the REPL program:

* current polygon outline
* current block outline
* vertex labels
* vertex points
* normal vectors derived from user geometry
* pending vertex guides
* transform guides tied to source cursor
* replay tessellation preview
* replay latest vertex/polygon highlight

These should be driven by REPL-owned plans.

They may still draw inside the 3D scene, but they are not scene-owned decorators.

Naming options:

```text
repl_geometry_overlay_plan.c     pure plan/model
repl_geometry_guides_plan.c      pure guide plan
repl_replay_plan.c               pure replay draw plan
repl_executor.c                  live GL execution
```

If live GL helper code is needed for annotations, either keep it inside `repl_executor.c` or introduce an explicitly allowed executor-adjacent file and update the boundary checks.

## UI Architecture

The UI layer should follow the same push-model idea as scene rendering.

```text
repl builds UiRenderConfig
ui_render(&config) draws from config
ui renderers do not mutate command state
```

UI modules may own transient view-local state such as hover state or open menu state, but command/source mutations should route through `repl_actions`, `repl_command_store`, `repl_var_drag`, or other REPL-owned mutation APIs.

## Boundary Rules

### Live OpenGL calls

Allowed:

```text
scene_*.c
ui_*.c
repl_executor.c
sample.c        for GLUT/window lifecycle and buffer swap
```

Avoid live GL in all other `repl_*` files. If a new exception is needed, document it and update the grep guard. Do not let exceptions accumulate accidentally.

### GLUT calls

Allowed:

```text
sample.c
repl_editor.c
```

Inside `repl_editor.c`, funnel GLUT usage through local helpers where possible.

### Scene purity

Pure scene modules should not include REPL runtime headers or inspect REPL state.

Allowed scene inputs:

```text
SceneRender3DConfig
SceneFrameContext
generic callback request/context
neutral math/render helper headers
```

Disallowed in pure scene modules:

```text
repl_state_* reads
g_cmds / g_flat_cmds
g_predef_vars
repl_replay_* state queries
source command indexes
edit-line state
FlatProgramView
```

The top-level REPL compositor may build scene config from REPL state. That is expected. The scene renderer itself should consume the resulting config.

### UI/scene independence

`ui_*` and `scene_*` should not include each other's headers.

Shared render-neutral helpers belong under `include/`.

## Refactoring Ownership Map

| Concern                        | Owner                               |
| ------------------------------ | ----------------------------------- |
| Source command array mechanics | `repl_command_store`                |
| Parsing and normalized source  | `repl_parser` / `repl_command_spec` |
| Structured user intent         | `repl_commit`                       |
| Source-to-flat expansion       | `repl_flatten`                      |
| User geometry GL execution     | `repl_executor`                     |
| Replay state machine           | `repl_replay`                       |
| Replay draw plan               | `repl_replay_plan` or equivalent    |
| Geometry render plan           | `repl_geometry_*`                   |
| 3D frame setup/decorators      | `scene_*`                           |
| 2D UI rendering                | `ui_*`                              |
| Code-panel text layout model   | `repl_code_panel_layout`            |
| Code-panel document model      | `repl_code_panel_document`          |
| Import/export                  | `repl_export`                       |
| Input routing                  | `repl_editor`                       |
| Config/menu actions            | `repl_actions`                      |

## Current Refactor Gaps

The current push-model work moved in the right direction by making scene helpers consume per-frame config/context snapshots. The remaining issue is that the scene config and scene renderer still carry too much REPL-specific execution and replay knowledge.

Primary gaps:

1. `scene_render_3d_scene()` should take an explicit config argument.
2. `SceneRenderConfig` should become a scene-only config.
3. `FlatProgramView` should move out of scene config and into REPL geometry render plans.
4. Replay PC/mode/fade fields should move out of scene config.
5. REPL-aware overlays/guides should be renamed or split so ownership is clear.
6. `scene_render.c` should call generic callbacks, not REPL-specific execution paths.
7. Clear color should be resolved before scene clear by a REPL-owned helper.
8. Boundary checks should distinguish pure scene decorators from REPL geometry annotation/executor code.

## Acceptance Criteria

The architecture is clean when:

1. `scene_render_3d_scene()` can render a 3D environment with no REPL program.
2. The scene layer receives a generic callback, not a REPL executor-specific API.
3. Scene config contains scene/environment fields, not REPL source/replay fields.
4. REPL builds geometry/replay/annotation plans from the flat program.
5. Replay rendering is owned by REPL geometry code, not scene code.
6. REPL-aware outlines/guides/labels are modeled as REPL geometry annotations.
7. UI rendering is driven by a UI config/model snapshot.
8. Live GL calls remain mechanically auditable.
9. Tests and manual visual behavior remain stable across the split.

```
