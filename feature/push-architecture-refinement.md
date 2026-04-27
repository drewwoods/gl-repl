# Plan: Push-Architecture Refinement

## Context

The current push-model work moved scene helpers toward per-frame config/context snapshots. That is good, but the boundary is still muddy.

The desired architecture has three components:

```text
repl_*   = language/controller/model, including geometry/replay/annotation plans
scene_*  = independent 3D frame/decorator view
ui_*     = independent 2D UI view
```

The scene should be an independent module that can decorate a 3D world while calling a generic user callback to draw user geometry.

The REPL should provide that callback.

The scene should not know that the callback is backed by a REPL flat program.

## Goal

Make `scene_*` own the 3D frame and 3D decorators, while `repl_*` owns the user geometry/replay/annotation model.

The target relationship is:

```text
repl builds SceneRender3DConfig
repl builds ReplGeometryRenderPlan
repl installs generic callbacks into SceneRender3DConfig
scene_render_3d_scene(&scene_cfg) renders the 3D frame
scene calls the generic callbacks at defined hook points
REPL callback draws geometry/replay/annotations through the executor boundary
```

## Non-Goals

- Do not change the REPL language.
- Do not change exported file compatibility.
- Do not change visual behavior unless a simplification is explicitly chosen.
- Do not make user geometry a 2D overlay.
- Do not let generic `repl_*` files freely call OpenGL.
- Do not make scene modules depend on REPL source/replay state.

## Current Problem

The current `SceneRenderConfig` is doing too much.

It contains scene data, but it also carries execution callback data, flat program data, replay fields, source/edit-line fields, and overlay fields.

That makes scene rendering look independent locally, while still encoding REPL semantics in the scene config.

The deeper issue:

```text
scene_render.c still decides too much about REPL geometry and replay.
```

The scene should decide frame order and callback hook points. The REPL should decide what its callback draws.

## Proposed Shape

### Top-level frame path

```text
repl_core_display_frame
  -> rebuild flat program if dirty
  -> build ReplGeometryRenderPlan
  -> build SceneRender3DConfig
  -> resolve clear color from REPL geometry plan into scene config
  -> attach generic user draw callbacks
  -> scene_render_3d_scene(&scene_cfg)
  -> build UiRenderConfig
  -> ui_render(&ui_cfg)
```

### Scene frame path

```text
scene_render_3d_scene
  -> set viewport
  -> clear buffers
  -> apply projection/camera/jitter
  -> apply quality and baseline lighting
  -> render scene decorators
  -> callback: user main geometry
  -> callback: user 3D annotations
  -> render scene foreground decorators
  -> finish accumulation sample/frame
```

The callback names are generic. They do not mention REPL.

## Proposed Config Types

### Generic scene callback API

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
```

This is the only thing the scene needs in order to draw user geometry.

### Scene render config

```c
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

This config is scene-owned.

It should not contain:

- `FlatProgramView`
- replay PC
- replay mode
- replay fade batches
- edit-line index
- source command indexes
- predef variables
- current REPL input text
- parser state

### Scene frame context

```c
typedef struct SceneFrameContext {
    SceneRender3DConfig config;

    float camera_world_y;
    int camera_below_water_surface;

    int focus_point_valid;
    float focus_point[3];
} SceneFrameContext;
```

Derived scene data belongs here.

If focus-point data depends on the REPL edit line, the REPL should compute it and pass it as generic focus-point config. The scene should not inspect the document to find it.

### REPL geometry render plan

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

This plan is REPL-owned.

It can contain REPL-specific concepts because it is never inspected by the scene.

## Callback Wiring

The REPL installs the generic callback:

```c
void repl_core_display_frame(void)
{
    SceneRender3DConfig scene_cfg;
    ReplGeometryRenderPlan geom_plan;
    UiRenderConfig ui_cfg;

    repl_flatten_if_dirty();

    repl_geometry_render_plan_build(&geom_plan);
    scene_render_3d_config_build_from_repl_state(&scene_cfg);

    repl_geometry_resolve_clear_color(&geom_plan, scene_cfg.clear_rgba);

    scene_cfg.user_draw.draw = repl_geometry_scene_draw_callback;
    scene_cfg.user_draw.reset = repl_geometry_scene_reset_callback;
    scene_cfg.user_draw.user_data = &geom_plan;

    scene_render_3d_scene(&scene_cfg);

    ui_render_config_build(&ui_cfg);
    ui_render(&ui_cfg);
}
```

The scene calls it like this:

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

## Replay Decision

Do **not** add a replay-specific scene callback.

Replay should not look like a scene concern.

Bad direction:

```c
scene_cfg.draw_replay = repl_replay_draw;
```

Better direction:

```c
scene_cfg.user_draw.draw = repl_geometry_scene_draw_callback;
```

Then inside the REPL callback:

```c
void repl_geometry_scene_draw_callback(const SceneUserDrawRequest *request,
                                       void *user_data)
{
    ReplGeometryRenderPlan *plan = user_data;

    switch (request->pass) {
    case SCENE_USER_DRAW_MAIN:
        if (plan->replay_active)
            repl_geometry_render_replay(plan, request);
        else
            repl_geometry_render_normal(plan, request);
        break;

    case SCENE_USER_DRAW_ANNOTATIONS_3D:
        repl_geometry_render_annotations(plan, request);
        break;
    }
}
```

This keeps replay inside REPL geometry ownership.

## Replay Fade Simplification

The existing fade-batch model is visually nice but architecturally expensive. It mixes:

- replay PC state
- variable baseline restore
- executor limits
- blend/material state
- skip ranges
- old/new command ranges

If preserving it makes the split difficult, simplify it.

Minimum acceptable replay visual:

```text
draw the program up to the current replay limit
fade/highlight only the most recently drawn vertex or polygon
```

Candidate type:

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

The replay plan builder decides which range is the latest vertex/polygon.

The executor/callback draws it.

The scene still knows nothing about replay.

## Clear Color Handling

`glClearColor(...)` is part of the REPL command stream, but the frame must clear before the user callback executes.

Therefore clear color needs a REPL-owned pre-pass:

```c
void repl_geometry_resolve_clear_color(const ReplGeometryRenderPlan *plan,
                                       float out_rgba[4]);
```

Frame flow:

```c
repl_geometry_render_plan_build(&geom_plan);
scene_render_3d_config_build_from_repl_state(&scene_cfg);
repl_geometry_resolve_clear_color(&geom_plan, scene_cfg.clear_rgba);
scene_render_3d_scene(&scene_cfg);
```

This does not make scene rendering dependent on the REPL. The REPL contributes a resolved clear color before the scene starts.

## Ownership of 3D Overlays

Current names such as `scene_overlays.c`, `scene_geometry_guides.c`, and `scene_transform_guides.c` are ambiguous because some of their work is REPL-aware.

Classify each pass.

### Keep in `scene_*`

Scene/world decorators:

- grid
- axes
- backdrop
- scene light indicators
- orbit target
- generic camera/world focus point

### Move or reframe as REPL geometry annotation

REPL-aware 3D annotations:

- current polygon outline
- current block outline
- vertex points tied to flat commands
- vertex numbers
- normal vectors derived from user geometry
- vertex/normal edit guides tied to current input
- transform guides tied to current source cursor
- replay tessellation preview
- replay latest-primitive highlight

These can still draw inside the scene frame, but their ownership is REPL geometry/annotation, not scene decoration.

## Staged Refactor

Each step should preserve behavior unless a simplification is explicitly chosen.

| Step | Change | Risk | Rationale |
|------|--------|------|-----------|
| 1 | Introduce `SceneUserDrawRequest`, `SceneUserDrawCallbacks`, and explicit `scene_render_3d_scene(const SceneRender3DConfig *)`. | Medium | Establishes the generic callback boundary. |
| 2 | Keep old `scene_render_3d_scene(void)` as a compatibility wrapper temporarily. | Low | Allows migration in small slices. |
| 3 | Split current `SceneRenderConfig` into scene-only `SceneRender3DConfig` and REPL-owned `ReplGeometryRenderPlan`. | Medium | Removes REPL concepts from scene config. |
| 4 | Move clear-color scan into `repl_geometry_resolve_clear_color()`. | Low-Medium | Preserves `glClearColor` behavior without scene knowing flat commands. |
| 5 | Make `repl_core` or a small frame-compositor helper build both configs and wire callbacks. | Medium | Establishes the intended dependency direction. |
| 6 | Move normal user geometry execution behind `repl_geometry_scene_draw_callback`. | Medium | Scene calls generic callback; REPL owns execution. |
| 7 | Move replay rendering out of `scene_render.c` and into REPL geometry/replay code. | High | Most delicate part; simplify to latest vertex/polygon fade if needed. |
| 8 | Reclassify REPL-aware overlays/guides as REPL geometry annotations. | Medium-High | Clarifies ownership; may require rename or split. |
| 9 | Tighten boundary checks so pure scene modules cannot read REPL state. | Medium | Prevents regression after the split. |
| 10 | Update docs and header comments to match the new ownership model. | Low | Locks the vocabulary in place. |

## Mechanical Boundary Checks

Add or update grep guards.

### Live GL boundary

Allowed live GL files:

```text
scene_*.c
ui_*.c
repl_executor.c
sample.c
```

If a new executor-adjacent file is introduced, add it explicitly. Do not allow generic `repl_*` GL drift.

### Pure scene boundary

Pure scene modules should not read REPL runtime state:

```makefile
check-scene-purity:
	@echo "Checking pure scene modules..."
	@! grep -nE '\b(repl_state_|repl_replay_|repl_execute_|g_cmds|g_flat_cmds|g_predef_vars|FlatProgramView)' \
		scene_grid.c scene_axes.c scene_backdrop.c scene_lights.c \
		|| (echo "ERROR: pure scene modules must not depend on REPL runtime state" && exit 1)
	@echo "Scene purity OK"
```

Do not apply that rule to REPL-owned geometry annotation files. Those files are allowed to know about the REPL, but their naming should make that obvious.

### UI/scene coupling

Keep:

```text
ui_* must not include scene_*
scene_* must not include ui_*
```

Shared helpers go under `include/`.

## Acceptance Criteria

This refinement is done when:

1. `scene_render_3d_scene()` takes an explicit config.
2. `scene_render_3d_scene()` can render an empty 3D scene without a REPL program.
3. Scene config contains scene/environment state and generic callbacks only.
4. `FlatProgramView` is not part of pure scene config.
5. Replay fields are not part of pure scene config.
6. REPL builds a geometry/replay/annotation render plan.
7. The scene calls a generic callback for user geometry.
8. Replay rendering lives behind the REPL geometry callback.
9. If fade batches are too complex, replay falls back to latest vertex/polygon fade/highlight.
10. REPL-aware 3D overlays/guides are documented or renamed as REPL geometry annotations.
11. Existing tests pass.
12. Manual smoke test shows no obvious visual ordering regression.

## Bottom Line

The clean split is:

```text
scene_* creates and decorates the 3D stage;
repl_* describes the actor and its annotations;
scene_* calls a generic callback to let the actor draw;
ui_* draws the editor around it.
```

Replay should not become a scene concept.

Keep replay inside the REPL geometry callback. If full fade batches are too much work, fade only the most recently drawn vertex or polygon.
