# Plan: Push-Architecture Refinement

## Context

The current push-model work decouples render modules from live `repl_state` reads by
building per-frame snapshots. That is the right local direction, but the broader
architecture can be cleaned up further by separating three concepts that are currently
interleaved inside the 3D scene path:

1. **Scene frame / environment** — viewport, projection, camera, clear color, quality
   state, grid, axes, backdrop, lights, orbit target.
2. **REPL geometry** — the user-authored OpenGL program emitted by the REPL executor
   from `FlatProgramView`.
3. **REPL-aware geometry annotations** — current polygon outline, vertex labels,
   normal vectors, edit guides, transform guides, replay tess preview, replay fades,
   and replay HUD.

The important conceptual shift is this:

```text
The scene is the stage.
The REPL executor draws the actor.
The REPL overlays annotate the actor.
The UI draws the editor around it.
```

Today `scene_render.c` still acts as the master of the REPL geometry pass: it prepares
the scene, calls the executor callback, handles replay fade execution, and then draws
both decorative scene helpers and REPL-specific overlays. The callback abstraction is
useful, but ownership is only half-inverted: the executor is injected, while
`scene_render.c` still decides when and how user geometry is emitted.

This refinement proposes moving that ownership out of `scene_render.c`. The top-level
frame compositor (`repl_core.c`, or a future `frame_render.c`) should orchestrate scene,
REPL geometry, REPL overlays, and 2D UI as separate layers.

---

## Goal

Make `scene_*` able to render a complete camera/environment frame without knowing how
REPL geometry is executed.

The scene layer should not own the user program. It should own the 3D world the user
program is viewed inside.

---

## Non-Goals

- Do not change visual behavior in this phase.
- Do not remove the REPL executor callback immediately if it is still useful as a
  seam for tests or alternate geometry sources.
- Do not make user geometry a 2D overlay. It must still render inside the same 3D
  projection, camera, lighting, depth, and quality state.
- Do not move editor/input mutation paths as part of this refinement.

---

## Current Shape

The current design already has a useful seam:

```c
typedef void (*SceneExecuteProgramFn)(float alpha_scale,
                                      int skip_geom_before_pc,
                                      int flat_cmd_count,
                                      FlatProgramView program,
                                      void *user_data);
```

`SceneRenderConfig` carries `execute_fn`, `execute_reset_fn`, and `flat_program`, so
`scene_render.c` does not need to call `repl_execute_program()` directly. That is good.

However, the scene layer still owns the pass order:

```text
repl_core/display_func
  -> scene_render_3d_scene
       -> scene frame setup
       -> execute_fn(...) for user geometry
       -> replay fade batches via execute_fn(...)
       -> scene helpers
       -> REPL overlays/guides/HUD
  -> 2D UI render
```

That makes `scene_render.c` responsible for both the stage and the actor.

---

## Proposed Shape

Move pass ordering to a top-level frame compositor:

```text
repl_core/display_func or frame_render_render
  -> build SceneFrameConfig
  -> build ReplGeometryRenderConfig
  -> build ReplOverlayRenderConfig
  -> scene_frame_begin
  -> scene_render_environment passes
  -> repl_geometry_render
  -> repl_geometry_overlay passes
  -> scene_render_foreground indicators
  -> scene_frame_end
  -> 2D UI render
```

In code form:

```c
void repl_render_frame(void) {
    SceneFrameConfig scene_cfg;
    ReplGeometryRenderConfig geom_cfg;
    ReplOverlayRenderConfig overlay_cfg;
    UiRenderConfig ui_cfg;
    UiRenderOutputs ui_out = { 0 };

    scene_frame_config_build(&scene_cfg);
    repl_geometry_render_config_build(&geom_cfg);
    repl_overlay_render_config_build(&overlay_cfg);
    ui_render_config_build(&ui_cfg);

    scene_frame_begin(&scene_cfg);

    scene_backdrop_render(&scene_cfg);
    scene_grid_render(&scene_cfg);
    scene_axes_render(&scene_cfg);

    repl_geometry_render(&geom_cfg);
    repl_geometry_overlays_render(&overlay_cfg);

    scene_lights_render_indicators(&scene_cfg);
    scene_orbit_target_render(&scene_cfg);

    scene_frame_end(&scene_cfg);

    ui_render(&ui_cfg, &ui_out);
    ui_render_outputs_apply(&ui_out);
}
```

The exact function names can change, but the architectural boundary should be clear:
scene sets up and decorates the 3D frame; REPL geometry renders the user program;
REPL overlays render editor/program annotations.

---

## Ownership Split

### `scene_*`: camera/environment/decorative world

These modules remain scene-owned because they describe the world around the geometry:

| Module / concern | Ownership rationale |
|------------------|---------------------|
| `scene_render` frame begin/end | Owns viewport, projection, camera, clear, frame-level GL state discipline |
| `scene_grid` | Decorative/world reference plane |
| `scene_axes` | Decorative/world orientation aid |
| `scene_backdrop` | Decorative background/environment pass |
| `scene_lights` setup | Scene lighting baseline and light positions |
| light indicators | Scene/world visualization of lights |
| orbit target | Camera manipulation visualization |

These modules should be able to render without a REPL program. If no geometry pass is
provided, the user should still see grid, axes, backdrop, lights, and camera guides.

### `repl_geometry_*`: user program execution

The REPL geometry layer owns execution of the flattened user program:

| Module / concern | Ownership rationale |
|------------------|---------------------|
| user geometry fill pass | The flat command stream is the REPL program, not scene decoration |
| replay base-limit execution | Replay limits are derived from REPL playback state |
| replay fade batches | Fade logic depends on replay PCs, baseline variable snapshots, and executor semantics |
| executor callback | Callback remains useful, but belongs to the geometry layer rather than the scene layer |
| clear-color extraction | The user program can define frame clear color; extract it before scene clear |

Candidate modules:

```text
repl_geometry_render.c
repl_geometry_render.h
repl_replay_render.c        optional, if replay pass logic is large enough
```

### `repl_geometry_overlays_*`: REPL-aware 3D annotations

These are 3D overlays, but they are not neutral scene decoration. They know about
source lines, edit cursor state, flat-command provenance, replay mode, and command
semantics.

| Current concern | Proposed ownership |
|-----------------|-------------------|
| polygon/current-block outlines | `repl_geometry_overlays.c` |
| vertex labels / vertex numbers | `repl_geometry_overlays.c` |
| normal-vector overlays | `repl_geometry_overlays.c` |
| vertex/normal edit guides | `repl_geometry_guides.c` |
| transform guides | `repl_transform_guides.c` or `repl_geometry_guides.c` |
| replay tess preview | `repl_replay_render.c` or `repl_geometry_overlays.c` |
| replay HUD | either `repl_replay_render.c` or 2D UI, but not generic scene decoration |

These may still render in 3D and use the scene camera/projection, but they should be
fed by REPL overlay snapshots rather than by generic scene config.

---

## Proposed Config Types

### Scene frame config

The scene frame config should contain only the data needed to prepare and decorate the
3D world:

```c
typedef struct SceneFrameConfig {
    int scene_x, scene_y, scene_w, scene_h;
    int viewport_w, viewport_h;

    float cam_dist, cam_rx, cam_ry;
    float cam_tx, cam_ty, cam_tz;
    float cam_motion_glow;

    float clear_rgba[4];

    int multisample_enabled;
    int line_smooth_enabled;
    int wireframe;

    int grid_theme;
    int grid_extent_idx;
    int grid_major_idx;
    int axes_theme;
    int backdrop_mode;

    float grid_major_steps[GRID_MAJOR_COUNT];
    float grid_extents[GRID_EXTENT_COUNT];

    int user_lighting_enabled;
    SceneLight lights[MAX_LIGHTS];
    int show_light_indicators;

    float accum_jitter_x;
    float accum_jitter_y;
} SceneFrameConfig;
```

### Scene frame derived context

```c
typedef struct SceneFrameContext {
    SceneFrameConfig config;
    float camera_world_y;
    int camera_below_water_surface;
} SceneFrameContext;
```

This replaces the current `FrameRenderContext` for truly scene-owned modules.

### REPL geometry render config

```c
typedef struct ReplGeometryExecutor {
    void (*execute)(float alpha_scale,
                    int skip_geom_before_pc,
                    int flat_cmd_count,
                    FlatProgramView program,
                    void *user_data);
    void (*reset)(void *user_data);
    void *user_data;
} ReplGeometryExecutor;

typedef struct ReplGeometryRenderConfig {
    FlatProgramView flat_program;
    ReplGeometryExecutor executor;

    int wireframe;
    int user_lighting_enabled;
    int line_smooth_enabled;

    int replay_active;
    int replay_mode;
    int replay_has_fades;
    int replay_base_limit;
    int replay_expand_args;

    float alpha_scale;
} ReplGeometryRenderConfig;
```

The executor callback remains available, but it is no longer a field in the scene
frame config. Geometry execution is a REPL geometry concern.

### REPL overlay render config

```c
typedef struct ReplOverlayRenderConfig {
    FlatProgramView flat_program;

    int edit_line_idx;
    int insert_mode;
    const char *input;
    int input_len;
    int cursor_pos;

    const ExprVar *predef_vars;
    int predef_var_count;

    int show_current_poly;
    int show_vertex_outlines;
    int show_vertex_points;
    int show_vertex_labels;
    int show_normal_vectors;
    int show_vertex_guides;
    int xform_guide_mode;

    int replaying;
    int replay_mode;
    int replay_tess_preview;
    int replay_vertex_points;
    int replay_pc;
    int replay_total_cmds;
    int replay_state;
    float replay_speed;
    int replay_expand_args;

    int cursor_block_begin_idx;
    int cursor_block_end_idx;
    int cursor_block_source_line;

    float alpha_scale;
} ReplOverlayRenderConfig;
```

The overlay config can also carry the scene camera/view matrix if transform guides need
it. That dependency should be explicit: overlays render in the scene frame, but they
are not owned by the scene layer.

---

## Rendering Order

The exact order matters. The split should preserve current behavior while making the
ownership clear.

Recommended order:

```text
1. scene_frame_begin
   - viewport
   - clear color
   - projection
   - camera
   - quality flags
   - baseline lighting/material state

2. scene background/environment
   - backdrop
   - grid
   - axes

3. REPL geometry
   - normal fill pass
   - replay fade passes
   - executor reset

4. REPL geometry overlays/guides
   - current polygon/current block
   - vertex points
   - vertex labels
   - normal vectors
   - edit guides
   - transform guides
   - replay tess preview

5. scene foreground indicators
   - light indicators
   - orbit target

6. 2D overlays
   - replay HUD if kept as 2D overlay
   - code panel
   - menu/dropdowns/search
   - autocomplete
   - variable panel
   - help/profile
```

The order can be adjusted for specific visual effects, but the ownership should not
collapse back into `scene_render.c`.

---

## Clear Color Caution

`glClearColor(...)` is currently part of the user command stream. The scene frame must
clear before the REPL geometry executor runs, so clear color has to be resolved before
`scene_frame_begin()`.

That means the frame builder needs a small pre-pass:

```c
void repl_geometry_resolve_clear_color(FlatProgramView program,
                                       float out_rgba[4]);
```

Then:

```c
repl_geometry_resolve_clear_color(geom_cfg.flat_program, scene_cfg.clear_rgba);
scene_frame_begin(&scene_cfg);
```

This does not make scene rendering dependent on REPL execution. It only lets the REPL
program contribute frame clear state.

---

## Replay Caution

Replay fade execution currently mixes scene setup, baseline variable restore, fade
batch calculation, executor calls, and GL blend/material state. That logic should move
with the REPL geometry renderer, not remain in `scene_render.c`.

Reason: replay fade rendering is not environment decoration. It is a visualization of
how the REPL command stream executes over time.

Suggested split:

```text
repl_geometry_render.c
  - normal fill pass
  - executor callback adapter

repl_replay_render.c
  - replay base-limit pass
  - fade-batch loop
  - baseline variable restore
  - executor reset
  - replay tess preview / HUD, if kept with replay
```

If that feels too large for the first refinement, keep replay rendering in
`repl_geometry_render.c` initially and split it later.

---

## Staged Refactor

Each step should preserve behavior and pass `make test-stubs TEST_JOBS=4`.

| Step | Change | Risk | Rationale |
|------|--------|------|-----------|
| 1 | Introduce `SceneFrameConfig` / `SceneFrameContext` aliases or new types while keeping old wrappers. | Low | Establish vocabulary without behavior change. |
| 2 | Split `scene_frame_begin()` / `scene_frame_end()` out of `scene_render_3d_scene()`. | Medium | Makes scene setup reusable by an external compositor. |
| 3 | Move clear-color resolution into a small REPL geometry helper used before `scene_frame_begin()`. | Low-Medium | Keeps user `glClearColor` behavior while removing clear ownership confusion. |
| 4 | Add `repl_geometry_render.c` and move normal executor pass from `scene_render.c`. | Medium | Removes the main user-geometry pass from scene ownership. |
| 5 | Move replay fade execution into `repl_geometry_render.c` or `repl_replay_render.c`. | High | Most delicate GL-state and replay-state interaction. |
| 6 | Move REPL-aware overlays/guides from `scene_*` naming toward `repl_geometry_*` modules. | Medium-High | Clarifies ownership; may be mostly renames plus config reshaping. |
| 7 | Keep decorative passes in `scene_*` and make them consume only `SceneFrameContext`. | Medium | Completes scene independence from REPL program details. |
| 8 | Introduce `frame_render.c` if `repl_core.c` display orchestration becomes too large. | Low-Medium | Gives the compositor a clear home without bloating core. |
| 9 | Update `ARCHITECTURE.md`, `MODULES.md`, and Makefile guards for the new ownership boundary. | Low | Prevents future drift. |

---

## Mechanical Boundary Checks

Once the split is complete, enforce the boundary mechanically.

Potential guard rules:

```makefile
check-scene-frame-boundary:
	@echo "Checking scene frame independence..."
	@! grep -nE '\b(repl_state_|repl_execute_|repl_replay_|g_predef_vars|g_num_predef_vars)' \
		scene_grid.c scene_axes.c scene_backdrop.c scene_lights.c \
		|| (echo "ERROR: pure scene modules must not read REPL runtime state" && exit 1)
	@echo "Scene frame boundary OK"
```

A stricter future version should also ensure pure scene modules do not include
REPL-specific headers other than shared render-neutral types.

Allow REPL-specific state in:

```text
repl_geometry_render.c
repl_geometry_overlays.c
repl_geometry_guides.c
repl_replay_render.c
```

Do not allow REPL-specific state in decorative scene modules.

---

## Naming Guidance

Use names to make ownership obvious:

| Current / possible file | Preferred ownership name |
|-------------------------|--------------------------|
| `scene_render.c` | frame begin/end, viewport/camera/environment compositor helpers only |
| `scene_overlays.c` | `repl_geometry_overlays.c` if it depends on flat/source command provenance |
| `scene_geometry_guides.c` | `repl_geometry_guides.c` if it depends on edit cursor/input state |
| `scene_transform_guides.c` | `repl_transform_guides.c` or `repl_geometry_guides.c` |
| replay HUD/tess preview | `repl_replay_render.c` or 2D UI layer, not generic scene |

The prefix should describe ownership, not merely where pixels appear. A 3D overlay
can still be REPL-owned if it visualizes REPL/editor state.

---

## Acceptance Criteria

This refinement is successful when:

1. `scene_*` can render the environment frame without an executor callback.
2. User geometry execution is owned by `repl_geometry_render.c` or equivalent.
3. Replay fade execution is owned by REPL geometry/replay rendering, not by
   `scene_render.c`.
4. REPL-aware overlays/guides are named and documented as REPL geometry overlays,
   even if they render in the 3D scene projection.
5. `scene_grid`, `scene_axes`, `scene_backdrop`, and scene-owned light/orbit helpers
   consume only scene frame config/context.
6. The top-level frame compositor owns pass ordering.
7. `glClearColor` behavior from the user program is preserved by a pre-frame clear
   color resolver.
8. Existing tests pass, and a manual smoke test shows no visual ordering regression.

---

## Bottom Line

Yes: the scene render path can and should become independent of REPL geometry execution.

The clean split is not "scene draws first, then REPL draws a 2D overlay." The clean split
is:

```text
scene frame establishes the 3D world;
REPL geometry renders inside that world;
REPL overlays annotate that geometry;
the 2D UI renders around/above everything.
```

That gives the codebase a stronger architecture than making `scene_render.c` responsible
for both the decorative world and the user-authored program.
