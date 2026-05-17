/*
 * scene_render.h - 3D scene rendering orchestration.
 *
 * Top-level entry points for scene rendering. Orchestrates the full rendering
 * pipeline: projection setup, camera transforms, user geometry execution,
 * replay fade passes, grid/axes, and visual overlays (polygon outlines, vertex
 * numbers, normal vectors, orbit target).
 *
 * Pipeline flow (per frame from display_func in src/repl/core.c):
 *   1. scene_render_init_gl() — one-time setup of GL state, display lists,
 *      shaders, tessellator
 *   2. Per-accum-sample (if AA enabled):
 *      - Set up projection with optional sub-pixel jitter
 *      - Apply camera transforms
 *      - Execute user commands (src/repl/executor.c)
 *      - Render grid, axes, orbit target
 *      - Render overlays (polygon outline, vertex guides, normals, etc.)
 *      - Accumulate into accumulation buffer
 *   3. scene_render_replay_fade_pass() — if replay active, render fading
 *      geometry snapshots (old geometry fades out as new appears)
 *
 * Rendering modes: The scene can display multiple visual elements toggled via
 * config (F3/F4 for grid/axes themes, F1-F11 for overlays). Grid and axes are
 * themeable; overlays include polygon outlines, vertex numbers, normal vectors,
 * vertex guides (cross-hairs), light indicators, and orbit target.
 *
 * Edit guides: The scene also renders edit-guide visualization when editing
 * (geometry guides show vertex/primitive context, transform guides show pending
 * matrix operations during step-through). These are controlled by scene_*_guides
 * modules.
 */
#ifndef SCENE_RENDER_H
#define SCENE_RENDER_H

#include "render_types.h"

/* Accumulation-buffer AA supports 1, 2, 4, 8, and 16-sample passes. The
 * controller cycles across that fixed ladder, and the scene renderer keeps a
 * 16-entry jitter table whose first N offsets form a good N-sample set. */
#define MAX_ACCUM_SAMPLES 16
#define ACCUM_STEP_COUNT  5

/* One-time GL initialization: create display lists, compile shaders, allocate
 * tessellator, set up default light state. Called once on startup. */
void scene_render_init_gl(void);

/* Apply the caller's camera as the modelview transform. The caller is
 * responsible for invoking this once per frame before scene_render_3d_scene;
 * the scene module no longer manages camera state. Mirrors the legacy
 * orbit transform: glTranslatef(0,0,-dist); glRotatef(rx,1,0,0);
 * glRotatef(ry,0,1,0); glTranslatef(-tx,-ty,-tz). */
void scene_apply_camera(float rx, float ry, float dist,
                        float tx, float ty, float tz);

/* Render the full 3D scene for one frame using an explicit config snapshot.
 * Orchestrates projection setup, camera transforms, user geometry execution,
 * grid/axes, overlays, and edit guides.
 * Called once per frame (or multiple times per frame for accumulation-buffer
 * AA). The controller builds the config once and passes it in.
 *
 * Returns 0 on success, -1 with errno = EINVAL if the config is rejected
 * by validate_render_config: NULL config, non-positive scene_w / scene_h,
 * out-of-range grid_theme / axes_theme, grid index out of range or grid
 * extent/step <= 0 when grid is enabled, or accum_samples out of range
 * when accumulation AA is on. */
int scene_render_3d_scene(const SceneRenderConfig *config);

/* Canonical description of the projection the scene last applied this
 * frame. scene_apply_projection() caches it (Tenet 3: a render-time
 * discovery the controller actualizes back into GL-free state) so the
 * exporter / code panel can emit a faithful reshape() without re-deriving
 * the math or reaching into scene internals. The blend mid-transition is
 * snapped to whichever side the mix is closest to — exporters want a
 * discrete mode, not an interpolated matrix. ortho_top is the
 * aspect-independent half-height (callers multiply by their own aspect).
 * Read it through the controller-installed export projection bridge, not
 * directly from repl_*. */
typedef struct SceneProjectionDesc {
    int    ortho;        /* 0 = perspective, 1 = orthographic */
    double fovy_deg;     /* perspective vertical field of view */
    double near_z;       /* perspective near plane (> 0) */
    double far_z;        /* perspective far plane */
    double ortho_top;    /* ortho half-height, aspect-independent */
    double ortho_near;   /* ortho near clip (signed eye Z) */
    double ortho_far;    /* ortho far clip */
} SceneProjectionDesc;

void scene_get_active_projection(SceneProjectionDesc *out);

/* The replay-fade overlay pass used to live in src/scene/render.c and was
 * exposed here for benchmark use. It moved out to the REPL controller
 * (imrepl_ctrl.c) and now runs through SceneRenderConfig.post_fill_fn —
 * the scene module no longer has a notion of replay batches. */

#endif /* SCENE_RENDER_H */
