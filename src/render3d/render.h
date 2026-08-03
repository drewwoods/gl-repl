/*
 * render.h - 3D scene rendering surface.
 *
 * Exposes the render3d module's top-level render entry points. Callers build a
 * Render3dRenderConfig snapshot, populate GL_MODELVIEW with their own camera
 * transform, then call render3d_draw_scene() for one pass. The render3d
 * module owns projection setup, clear, helper renderers (backdrop, grid,
 * axes, lights), and the optional callback hooks that bracket the main
 * geometry fill - but does NOT own a camera type or apply helper.
 *
 * The public surface is application-independent: user geometry, replay work,
 * and edit overlays enter through callbacks and snapshots carried on
 * Render3dRenderConfig rather than direct reads from app, editor, REPL, or UI
 * state.
 */
#ifndef RENDER3D_RENDER_H
#define RENDER3D_RENDER_H

#include "render_types.h"
#include "projection_mode.h"

/* Accumulation-buffer AA supports any sample count in [1, MAX_ACCUM_SAMPLES].
 * The render3d renderer keeps a 16-entry jitter table whose first N offsets form
 * a good N-sample set; which counts an application offers is caller policy. */
#define MAX_ACCUM_SAMPLES 16

/* One-time GL initialization called once at startup. Currently:
 * - render3d_lights_init_global_ambient() (baseline ambient term).
 * - render3d_postprocess_filter_reset() (clears the post-process module's
 *   per-renderer caches).
 * No display lists, shaders, or tessellator allocation happen here -
 * those live elsewhere (the tessellator is owned by the executor). */
void render3d_init_gl(void);

/* Callers must populate GL_MODELVIEW with the camera transform before
 * invoking render3d_draw_scene. render3d_draw_scene then sets
 * GL_PROJECTION via render3d_apply_projection and switches back to
 * GL_MODELVIEW for user geometry, but it never overwrites the
 * modelview the caller set. The render3d module does not own a camera
 * type, an apply helper, or any camera state - those belong to the caller.
 * A caller may use its own camera helper or inline the matrix calls directly;
 * neither choice creates a renderer dependency. */

/* Canonical description of the projection the scene last applied this
 * frame. Computed once per render3d_draw_scene call (before the AA
 * jitter loop) so the value reflects the canonical zero-jitter math,
 * not a transient sample. Export/debug paths read it through
 * render3d_get_active_projection() so they emit a faithful reshape()
 * without re-deriving from scene internals. The blend mid-transition
 * is snapped to whichever side the mix is closest to, because
 * downstream callers want a discrete mode rather than an interpolated
 * matrix. ortho_top is the aspect-independent half-height (callers
 * multiply by their own aspect). */
typedef struct Render3dProjectionDesc {
    Render3dProjectionMode projection; /* perspective / orthographic */
    double fovy_deg;     /* perspective vertical field of view */
    double near_z;       /* perspective near plane (> 0) */
    double far_z;        /* perspective far plane */
    double ortho_top;    /* ortho half-height, aspect-independent */
    double ortho_near;   /* ortho near clip (signed eye Z) */
    double ortho_far;    /* ortho far clip */
} Render3dProjectionDesc;

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

/* Per-renderer state the render3d module needs to persist across frames.
 * Each embedding caller owns one instance, so multiple render3d viewports use
 * separate state objects rather than shared renderer globals.
 *
 * Callers MUST call render3d_state_init() before passing the
 * struct to render3d_draw_scene(), and own its lifetime across
 * frames (the renderer never frees it). */
typedef struct Render3dState {
    /* 2D ortho scale reference (depth-center of the drawn geometry).
     * How it's sampled - once at the switch vs. every frame - is
     * selected at compile time by GLR_ORTHO_REF_MODE above.
     * 0 means "no usable measurement" and the projection math falls
     * back to config->cam_dist (the camera's distance to the orbit
     * target plane). */
    double ortho_ref_dist;
    /* cam_dist at the instant ortho_ref_dist was sampled. The live 2D
     * scale reference is ortho_ref_dist + (cam_dist - ortho_ref_cam_dist),
     * so mouse-wheel zoom (which drives cam_dist) rescales the ortho view
     * without re-probing - the same eye-distance shift a re-probe would
     * measure, but without the per-frame breathing under animation that
     * FROZEN mode exists to avoid. Unused while ortho_ref_dist is 0 (the
     * fallback path reads cam_dist directly). */
    double ortho_ref_cam_dist;
    /* Edge tracker for the FROZEN sampling mode - true while ortho is
     * contributing. PERFRAME mode doesn't read this. */
    int    ortho_active;
    /* Canonical (zero-jitter) projection resolved by the most recent
     * render3d_draw_scene call. Read by export/debug paths. */
    Render3dProjectionDesc active_projection;
} Render3dState;

/* Initialize a Render3dState to its default values. Required
 * before the first render3d_draw_scene call so a getter invoked
 * before any frame returns sane defaults (the steady 3D frustum). */
void render3d_state_init(Render3dState *state);

/* Render the full 3D scene for one frame using an explicit config snapshot.
 * Orchestrates projection setup, user geometry execution, grid/axes, and
 * callback-provided overlays and guides.
 * Called once per frame (or multiple times per frame for accumulation-buffer
 * AA). The caller builds the config once and passes it in along with its
 * owned renderer state.
 *
 * Returns 0 on success, -1 with errno = EINVAL if the config is rejected
 * by validate_render_config: NULL config, non-positive render3d_w / render3d_h,
 * out-of-range grid_theme / axes_theme, grid index out of range or grid
 * extent/step <= 0 when grid is enabled, unknown accum_effect, or
 * accum_passes outside [1, MAX_ACCUM_SAMPLES] when accumulation is active. */
int render3d_draw_scene(Render3dState *state,
                          const Render3dRenderConfig *config);

/* Read the most-recently-resolved active projection from `state`. Safe
 * to call between render3d_draw_scene invocations; a fresh state
 * returns the default 3D frustum. */
void render3d_get_active_projection(const Render3dState *state,
                                 Render3dProjectionDesc *out);

/* Replay-fade overlays are injected through
 * Render3dRenderConfig.post_fill_fn, keeping replay batching details outside
 * the render3d renderer. */

#endif /* RENDER3D_RENDER_H */
