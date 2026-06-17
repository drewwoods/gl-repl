/*
 * render.h - 3D scene rendering surface.
 *
 * Exposes the scene module's top-level render entry points. Callers build a
 * SceneRenderConfig snapshot, populate GL_MODELVIEW with their own camera
 * transform (e.g. via glr_camera_load_modelview from src/app/glr_camera.h,
 * or inline matrix calls in tools/scene_demo), then call
 * scene_render_3d_scene() for one pass. The scene module owns projection
 * setup, clear, helper renderers (backdrop, grid, axes, lights), and the
 * optional callback hooks that bracket the main geometry fill — but does
 * NOT own a camera type or apply helper.
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

/* One-time GL initialization called once at startup. Currently:
 * - scene_lights_init_global_ambient() (baseline ambient term).
 * - scene_postprocess_filter_reset() (clears the post-process module's
 *   per-renderer caches).
 * No display lists, shaders, or tessellator allocation happen here —
 * those live elsewhere (the tessellator is owned by the executor). */
void scene_render_init_gl(void);

/* Callers must populate GL_MODELVIEW with the camera transform before
 * invoking scene_render_3d_scene. scene_render_3d_scene then sets
 * GL_PROJECTION via scene_apply_projection and switches back to
 * GL_MODELVIEW for user geometry, but it never overwrites the
 * modelview the caller set. The scene module does not own a camera
 * type, an apply helper, or any camera state — that lives in
 * src/app/glr_camera.h (glr_camera_load_modelview / GlrCameraPose).
 *
 * For non-app callers (scene_demo, future scene viewports): inline
 * the six matrix calls (glLoadIdentity / glTranslatef / glRotatef /
 * glTranslatef) directly. The function isn't worth importing for one
 * helper. */

/* Canonical description of the projection the scene last applied this
 * frame. Computed once per scene_render_3d_scene call (before the AA
 * jitter loop) so the value reflects the canonical zero-jitter math,
 * not a transient sample. Export/debug paths read it through
 * scene_get_active_projection() so they emit a faithful reshape()
 * without re-deriving from scene internals. The blend mid-transition
 * is snapped to whichever side the mix is closest to, because
 * downstream callers want a discrete mode rather than an interpolated
 * matrix. ortho_top is the aspect-independent half-height (callers
 * multiply by their own aspect). */
typedef struct SceneProjectionDesc {
    int    ortho;        /* 0 = perspective, 1 = orthographic */
    double fovy_deg;     /* perspective vertical field of view */
    double near_z;       /* perspective near plane (> 0) */
    double far_z;        /* perspective far plane */
    double ortho_top;    /* ortho half-height, aspect-independent */
    double ortho_near;   /* ortho near clip (signed eye Z) */
    double ortho_far;    /* ortho far clip */
} SceneProjectionDesc;

/* 2D ortho scale reference mode.
 *
 * The orthographic projection picks one eye distance whose on-screen scale it
 * reproduces. Both modes probe drawn geometry and pivot on the midpoint of its
 * eye-distance span (the depth-center):
 *
 *   GLR_ORTHO_REF_FROZEN   sample once at the perspective->ortho edge and
 *                          hold that reference until perspective returns.
 *   GLR_ORTHO_REF_PERFRAME re-probe every frame while ortho contributes.
 *
 * A probe that finds nothing falls back to cam_dist. */
#define GLR_ORTHO_REF_FROZEN   0
#define GLR_ORTHO_REF_PERFRAME 1
#ifndef GLR_ORTHO_REF_MODE
#define GLR_ORTHO_REF_MODE GLR_ORTHO_REF_FROZEN
#endif

/* Per-renderer state the scene module needs to persist across frames.
 * Replaces the file-static g_ortho_ref_dist / g_ortho_active /
 * g_active_projection trio in render.c. Each caller (controller,
 * scene_demo, tests) owns one instance — the single-renderer
 * assumption is now an instance count, not a global. Multiple scene
 * viewports become possible later by holding multiple state objects.
 *
 * Callers MUST call scene_renderer_state_init() before passing the
 * struct to scene_render_3d_scene(), and own its lifetime across
 * frames (the renderer never frees it). */
typedef struct SceneRendererState {
    /* 2D ortho scale reference (depth-center of the drawn geometry).
     * How it's sampled — once at the switch vs. every frame — is
     * selected at compile time by GLR_ORTHO_REF_MODE above.
     * 0 means "no usable measurement" and the projection math falls
     * back to config->cam_dist (the old orbit-target-plane behavior). */
    double ortho_ref_dist;
    /* cam_dist at the instant ortho_ref_dist was sampled. The live 2D
     * scale reference is ortho_ref_dist + (cam_dist - ortho_ref_cam_dist),
     * so mouse-wheel zoom (which drives cam_dist) rescales the ortho view
     * without re-probing — the same eye-distance shift a re-probe would
     * measure, but without the per-frame breathing under animation that
     * FROZEN mode exists to avoid. Unused while ortho_ref_dist is 0 (the
     * fallback path reads cam_dist directly). */
    double ortho_ref_cam_dist;
    /* Edge tracker for the FROZEN sampling mode — true while ortho is
     * contributing. PERFRAME mode doesn't read this. */
    int    ortho_active;
    /* Canonical (zero-jitter) projection resolved by the most recent
     * scene_render_3d_scene call. Read by export/debug paths. */
    SceneProjectionDesc active_projection;
} SceneRendererState;

/* Initialize a SceneRendererState to its default values. Required
 * before the first scene_render_3d_scene call so a getter invoked
 * before any frame returns sane defaults (the steady 3D frustum). */
void scene_renderer_state_init(SceneRendererState *state);

/* Render the full 3D scene for one frame using an explicit config snapshot.
 * Orchestrates projection setup, camera transforms, user geometry execution,
 * grid/axes, overlays, and edit guides.
 * Called once per frame (or multiple times per frame for accumulation-buffer
 * AA). The controller builds the config once and passes it in along with
 * its owned renderer state.
 *
 * Returns 0 on success, -1 with errno = EINVAL if the config is rejected
 * by validate_render_config: NULL config, non-positive scene_w / scene_h,
 * out-of-range grid_theme / axes_theme, grid index out of range or grid
 * extent/step <= 0 when grid is enabled, unknown accum_effect, or
 * accum_passes off the supported ladder when accumulation is active. */
int scene_render_3d_scene(SceneRendererState *state,
                          const SceneRenderConfig *config);

/* Read the most-recently-resolved active projection from `state`. Safe
 * to call between scene_render_3d_scene invocations; a fresh state
 * returns the default 3D frustum. */
void scene_get_active_projection(const SceneRendererState *state,
                                 SceneProjectionDesc *out);

/* The scene's fixed perspective vertical field of view, in degrees. Exposed
 * so the controller's fit-frame math can size a camera distance against the
 * same FOV the scene renders with, without duplicating the constant. */
double scene_default_fovy_deg(void);

/* Replay-fade overlay work no longer has a public scene entry point. Callers
 * that need it inject the pass through SceneRenderConfig.post_fill_fn, which
 * keeps src/scene/ unaware of replay batching details. */

#endif /* SCENE_RENDER_H */
