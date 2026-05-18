/*
 * scene_render.h - 3D scene rendering surface.
 *
 * Exposes the scene module's top-level render entry points. Callers build a
 * SceneRenderConfig snapshot, apply the camera modelview with
 * scene_apply_camera(), then call scene_render_3d_scene() for one pass. The
 * scene module owns projection setup, clear, helper renderers (backdrop, grid,
 * axes, lights), and the optional callback hooks that bracket the main geometry
 * fill.
 *
 * The public surface stays REPL-independent: user geometry and replay-specific
 * overlay work enter through callbacks carried on SceneRenderConfig rather than
 * direct reads from REPL globals.
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
 * the scene module does not own camera state. The transform matches the app's
 * orbit camera convention: translate by distance, apply pitch/yaw, then
 * translate by the target offset. */
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
 * frame. scene_apply_projection() caches it so export/debug paths can emit a
 * faithful reshape() description without re-deriving the math from scene
 * internals. The blend mid-transition is snapped to whichever side the mix is
 * closest to, because downstream callers want a discrete mode rather than an
 * interpolated matrix. ortho_top is the aspect-independent half-height
 * (callers multiply by their own aspect). */
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

/* Replay-fade overlay work no longer has a public scene entry point. Callers
 * that need it inject the pass through SceneRenderConfig.post_fill_fn, which
 * keeps src/scene/ unaware of replay batching details. */

#endif /* SCENE_RENDER_H */
