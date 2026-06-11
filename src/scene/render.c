/*
 * render.c - 3D scene rendering (frame prep, edit guides)
 *
 * Extracted from gl_repl.c for maintainability.
 */
#include "axes.h"
#include "backdrop.h"
#include "grid.h"
#include "lights.h"
#include "overlays.h"   /* scene_draw_bitmap_text for the orbit-gizmo coord readout */
#include "postprocess_filter.h"
#include "render_types.h"
#include "palette.h"
#include "occluded_ghost.h"
#include "render.h"
#include "support/cpuprof.h"
#include "config.h"

#include <errno.h>
#include <math.h>      /* tan, M_PI (via gl_includes.h) */
#include <stdio.h>

#define SCENE_DEFAULT_FOVY_DEG 45.0
#define SCENE_DEFAULT_NEAR_Z 0.1
#define SCENE_DEFAULT_FAR_Z 200.0
#define SCENE_PROJECTION_DEPTH_EPSILON 1e-4

/* Half-extent of the wide-ortho box used by the GL_FEEDBACK depth
 * probe. Must be >= SCENE_DEFAULT_FAR_Z so the real frustum's visible
 * range fits without near/far clipping during the probe walk.
 * Aliased to the far-Z value today; if you change SCENE_DEFAULT_FAR_Z,
 * scene_probe_eye_dist's wide-ortho box grows in lockstep. */
#define SCENE_PROBE_BOX SCENE_DEFAULT_FAR_Z

/* Feedback-buffer length (in GLfloats) for scene_probe_eye_dist. The
 * fixed buffer caps how much geometry can be probed; very dense
 * scenes simply fall back to cam_dist, which is safe. */
#define SCENE_PROBE_FEEDBACK_FLOATS (96 * 1024)

/* Half-tangent of the default vertical FOV. Hoisted so the three
 * call sites in scene_apply_projection don't repeat the same
 * libm tan() per AA sample. */
#define SCENE_DEFAULT_HALF_FOVY_TAN \
    (tan(SCENE_DEFAULT_FOVY_DEG * M_PI / 360.0))

/* Sub-pixel jitter offsets (units: fraction of one pixel).
 * Table is ordered so the first N entries form a good N-sample set.
 * Supports 1, 2, 4, 8, or 16 samples. */
static const float g_jitter_table[MAX_ACCUM_SAMPLES][2] = {
    {  0.250f,  0.250f },
    { -0.250f, -0.250f },/* 2  */
    {  0.250f, -0.250f },
    { -0.250f,  0.250f },  /* 4  */
    { -0.125f,  0.375f },
    {  0.375f,  0.125f },
    { -0.375f, -0.125f },
    {  0.125f, -0.375f }, /* 8  */
    {  0.375f, -0.375f },
    { -0.375f,  0.375f },
    {  0.125f,  0.125f },
    { -0.125f, -0.125f },
    {  0.375f,  0.375f },
    { -0.375f, -0.375f },
    {  0.000f,  0.500f },
    {  0.500f,  0.000f },  /* 16 */
};

void scene_render_init_gl(void) {
    scene_lights_init_global_ambient();
    scene_postprocess_filter_reset();
}

void scene_renderer_state_init(SceneRendererState *state) {
    if (!state) return;
    state->ortho_ref_dist = 0.0;
    state->ortho_ref_cam_dist = 0.0;
    state->ortho_active = 0;
    /* Default mirrors the steady 3D frustum so a getter call before
     * the first frame is still sane. */
    state->active_projection = (SceneProjectionDesc){
        .ortho      = 0,
        .fovy_deg   = SCENE_DEFAULT_FOVY_DEG,
        .near_z     = SCENE_DEFAULT_NEAR_Z,
        .far_z      = SCENE_DEFAULT_FAR_Z,
        .ortho_top  = 0.0,
        .ortho_near = -SCENE_DEFAULT_FAR_Z,
        .ortho_far  = SCENE_DEFAULT_FAR_Z,
    };
}

/* Reject SceneRenderConfig values that would cause undefined behavior or
 * never-terminating loops downstream. Returns 0 on valid, sets errno and
 * returns -1 on failure. The grid renderer's `for (v = -extent; v <= extent;
 * v += step)` is the most consequential — step <= 0 hangs the GLUT main
 * loop, so a zeroed config (the natural memset default) is a hard error
 * the moment the grid is enabled. */
static int validate_render_config(const SceneRenderConfig *config) {
    int backdrop_mode;

    if (!config)                                       goto bad;
    if (config->scene_w <= 0 || config->scene_h <= 0)  goto bad;
    if (config->grid_theme < 0
        || config->grid_theme >= GRID_THEME_COUNT)     goto bad;
    if (config->axes_theme < 0
        || config->axes_theme >= AXES_THEME_COUNT)     goto bad;
    backdrop_mode = (int)config->backdrop_mode;
    if (backdrop_mode < 0
        || backdrop_mode >= SCENE_BACKDROP_COUNT)       goto bad;
    if (config->grid_theme != GRID_THEME_OFF) {
        if (config->grid_extent_idx < 0
            || config->grid_extent_idx >= GRID_EXTENT_COUNT)  goto bad;
        if (config->grid_major_idx < 0
            || config->grid_major_idx >= GRID_MAJOR_COUNT)    goto bad;
        if (!(config->grid_extents[config->grid_extent_idx] > 0.0f))
                                                              goto bad;
        if (!(config->grid_major_steps[config->grid_major_idx] > 0.0f))
                                                              goto bad;
    }
    /* accum_effect must be a known mode. accum_passes must come from the
     * supported {1,2,4,8,12,16} ladder, but only when accumulation is
     * actually active (effect != OFF on an accum-capable context) — a
     * memset(0) config from a non-accum caller (scene_demo) leaves
     * accum_passes == 0, which is fine while effect is OFF. Mirrors the
     * grid block above (validated only when grid_theme != OFF). */
    if (config->accum_effect < 0 ||
        config->accum_effect > SCENE_ACCUM_EFFECT_BLUR_CAMERA) goto bad;
    if (config->use_accum && config->accum_effect != SCENE_ACCUM_EFFECT_OFF) {
        int n = config->accum_passes;
        if (n != 1 && n != 2 && n != 4 && n != 8 && n != 12 && n != 16)
            goto bad;
    }
    return 0;

bad:
    errno = EINVAL;
    return -1;
}

/* ========================================================================= */
/* 3D scene helpers                                                           */
/* ========================================================================= */

/* Scene helpers intentionally push their own attribute state even though the
 * full frame has a top-level guard. This keeps blend/depth/line/point/fog
 * side effects local to the helper that introduced them. */
static void scene_render_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void scene_render_pop_state(void) {
    glPopAttrib();
}

void scene_get_active_projection(const SceneRendererState *state,
                                 SceneProjectionDesc *out) {
    if (!out) return;
    if (!state) {
        /* Defensive: if the caller hasn't initialized a state, hand
         * back the same default scene_renderer_state_init would have
         * written. */
        SceneRendererState tmp;
        scene_renderer_state_init(&tmp);
        *out = tmp.active_projection;
        return;
    }
    *out = state->active_projection;
}

/* Run the user geometry through GL_FEEDBACK with a wide ortho box so the
 * recorded window z maps linearly back to eye space, and return the
 * depth-center (midpoint of the min/max eye distance) of the geometry.
 *
 * An identity probe projection would clip to the NDC unit cube, so any
 * geometry past |z_eye| > 1 (essentially everything) is discarded and
 * feedback returns nothing. glOrtho(-SCENE_PROBE_BOX, ..., SCENE_PROBE_BOX)
 * keeps the whole frustum-visible scene; with the default glDepthRange(0,1)
 * a vertex's window z is (-z_eye/B + 1)/2, so z_eye = -B*(2*winz - 1) and
 * the camera distance is -z_eye.
 *
 * Returns 0.0 when there is nothing to measure or the feedback buffer
 * overflowed (glRenderMode < 0) — the caller treats 0 as "use cam_dist".
 * The fixed buffer caps how much geometry can be probed; very dense
 * scenes simply fall back, which is safe. */
static double scene_probe_eye_dist(const SceneRenderConfig *config) {
    static GLfloat fb[SCENE_PROBE_FEEDBACK_FLOATS]; /* ~384 KB; overflow -> cam_dist fallback */
    /* Probe-projection half-extent; >= far_z so nothing the real frustum
     * can show is clipped during the feedback pass. */
    GLint n;
    int i;
    double dmin = 0.0, dmax = 0.0;
    int have = 0;

    if (!config->execute_fn)
        return 0.0;

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(-SCENE_PROBE_BOX, SCENE_PROBE_BOX,
            -SCENE_PROBE_BOX, SCENE_PROBE_BOX,
            -SCENE_PROBE_BOX, SCENE_PROBE_BOX);
    glMatrixMode(GL_MODELVIEW); /* leave camera modelview untouched */

    glFeedbackBuffer((GLsizei)(sizeof fb / sizeof fb[0]), GL_3D, fb);
    glRenderMode(GL_FEEDBACK);

    glPushMatrix();
    {
        SceneExecuteContext ctx = { .purpose = SCENE_EXEC_DEPTH_PROBE };
        config->execute_fn(&ctx, config->execute_user_data);
    }
    glPopMatrix();

    n = glRenderMode(GL_RENDER);

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);

    if (n <= 0)
        return 0.0;

    /* Token walk. GL_3D records 3 floats per vertex (no color). */
    i = 0;
    while (i < n) {
        GLenum tok = (GLenum)fb[i++];
        int verts;

        switch (tok) {
        case GL_POINT_TOKEN:
        case GL_BITMAP_TOKEN:
        case GL_DRAW_PIXEL_TOKEN:
        case GL_COPY_PIXEL_TOKEN:
            verts = 1;
            break;
        case GL_LINE_TOKEN:
        case GL_LINE_RESET_TOKEN:
            verts = 2;
            break;
        case GL_POLYGON_TOKEN:
            if (i >= n) { verts = 0; break; }
            verts = (int)fb[i++];
            break;
        case GL_PASS_THROUGH_TOKEN:
            i++; /* one value, no vertex */
            continue;
        default:
            /* Unknown token: parser desynced, stop rather than guess. */
            n = i = 0;
            verts = 0;
            break;
        }

        while (verts-- > 0 && i + 3 <= n) {
            /* fb[i+2] is window z; invert the wide-ortho mapping above. */
            double dist = SCENE_PROBE_BOX * (2.0 * (double)fb[i + 2] - 1.0);
            if (dist > SCENE_PROJECTION_DEPTH_EPSILON
                && dist < SCENE_PROBE_BOX) {
                if (!have || dist < dmin) dmin = dist;
                if (!have || dist > dmax) dmax = dist;
                have = 1;
            }
            i += 3;
        }
    }

    /* Depth-center of the drawn geometry: the midpoint holds its size
     * across the switch, near (perspective-magnified) geometry shrinks
     * inward, far geometry grows off-screen. Midpoint, not mean, so a
     * dense near/far cluster can't drag the pivot off the visual center. */
    return have ? 0.5 * (dmin + dmax) : 0.0;
}

/* Refresh the ortho scale reference for this frame. mix == 1 is pure
 * perspective; anything below means ortho is contributing (a switch is
 * in progress or we are dwelling in 2D). Sampling strategy is chosen by
 * GLR_ORTHO_REF_MODE in render.h.
 *
 * FROZEN: capture once on the perspective->ortho edge, release on the
 * ortho->perspective edge. The controller sequences the 3D->2D switch as
 * "flatten camera, THEN blend projection", so by the time mix leaves 1.0
 * the camera is already top-down — exactly the camera ortho will use —
 * so the one sample is the scene's true on-screen size at the switch and
 * never breathes afterward.
 *
 * PERFRAME: re-probe whenever ortho is contributing; tracks animation
 * and camera live. Pure-perspective frames pay nothing either way. */
static void scene_update_ortho_ref(SceneRendererState *state,
                                   const SceneRenderConfig *config) {
    int ortho_now = (config->projection_mix < 0.999f);

#if GLR_ORTHO_REF_MODE == GLR_ORTHO_REF_PERFRAME
    (void)state->ortho_active;
    state->ortho_ref_dist = ortho_now ? scene_probe_eye_dist(config) : 0.0;
    /* The probe just ran at the live cam_dist, so the zoom delta is zero
     * this frame; record the baseline anyway to keep
     * scene_effective_ortho_ref's arithmetic correct. */
    state->ortho_ref_cam_dist = (double)config->cam_dist;
#else /* GLR_ORTHO_REF_FROZEN */
    {
        if (ortho_now && !state->ortho_active) {
            state->ortho_ref_dist = scene_probe_eye_dist(config);
            /* Anchor the zoom baseline at the switch: later cam_dist
             * changes (mouse-wheel zoom) rescale the frozen reference. */
            state->ortho_ref_cam_dist = (double)config->cam_dist;
        } else if (!ortho_now && state->ortho_active) {
            state->ortho_ref_dist = 0.0;
        }
        state->ortho_active = ortho_now;
    }
#endif
}

/* Effective 2D ortho scale reference for this frame: the sampled
 * depth-center plus the live zoom delta accrued since it was sampled,
 * so mouse-wheel zoom (which drives cam_dist) rescales the ortho view.
 * Moving the camera by d shifts every vertex's eye distance by d, so
 * adding (cam_dist - ortho_ref_cam_dist) reconstructs exactly what a
 * re-probe at the current distance would report — without the per-frame
 * re-probe (and its animation breathing) that FROZEN mode avoids.
 *
 * Falls back to raw cam_dist when there is no usable probe measurement
 * (empty scene / feedback overflow); that path already tracks zoom.
 * Clamped to a positive floor so a deep zoom-in can't collapse or
 * invert the ortho box. Shared by scene_compute_active_projection (the
 * cached desc) and scene_apply_projection (each AA sample) so the two
 * never diverge. */
static double scene_effective_ortho_ref(const SceneRendererState *state,
                                        const SceneRenderConfig *config) {
    double ref = 0.0;
    if (state->ortho_ref_dist > SCENE_PROJECTION_DEPTH_EPSILON)
        ref = state->ortho_ref_dist
            + ((double)config->cam_dist - state->ortho_ref_cam_dist);
    else
        ref = (double)config->cam_dist;
    if (ref < SCENE_PROJECTION_DEPTH_EPSILON)
        ref = SCENE_PROJECTION_DEPTH_EPSILON;
    return ref;
}

/* Resolve the canonical (zero-jitter) projection description for this
 * frame and write it into state->active_projection. Called once per
 * scene_render_3d_scene before the AA jitter loop so the cached desc
 * always reflects the discrete-mode projection a faithful reshape()
 * would emit — not a transient sample inside the loop. The continuous
 * mix is snapped to the dominant side because reshape() emits one
 * discrete mode, not an interpolation. */
static void scene_compute_active_projection(SceneRendererState *state,
                                            const SceneRenderConfig *config) {
    double near_z = SCENE_DEFAULT_NEAR_Z;
    double far_z = SCENE_DEFAULT_FAR_Z;
    double half_fovy_tan = SCENE_DEFAULT_HALF_FOVY_TAN;
    float mix = config->projection_mix;
    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;

    double ortho_ref = scene_effective_ortho_ref(state, config);
    state->active_projection = (SceneProjectionDesc){
        .ortho      = (mix < 0.5f) ? 1 : 0,
        .fovy_deg   = SCENE_DEFAULT_FOVY_DEG,
        .near_z     = near_z,
        .far_z      = far_z,
        .ortho_top  = ortho_ref * half_fovy_tan,
        .ortho_near = -far_z,
        .ortho_far  = far_z,
    };
}

/* Per-AA-sample projection apply. Reads state->ortho_ref_dist for the
 * ortho scale reference but does NOT write back — state is read-only
 * here, all mutations land in scene_compute_active_projection above. */
static void scene_apply_projection(const SceneRendererState *state,
                                   const SceneRenderConfig *config,
                                   float accum_jitter_x,
                                   float accum_jitter_y) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    /* Build a jitter-aware perspective frustum. With zero jitter this matches
     * gluPerspective(SCENE_DEFAULT_FOVY_DEG, aspect, SCENE_DEFAULT_NEAR_Z,
     * SCENE_DEFAULT_FAR_Z). */
    double near_z = SCENE_DEFAULT_NEAR_Z;
    double far_z = SCENE_DEFAULT_FAR_Z;
    double aspect  = (double)config->scene_w / (double)config->scene_h;
    double half_fovy_tan = SCENE_DEFAULT_HALF_FOVY_TAN;
    double persp_top   = near_z * half_fovy_tan;
    double persp_right = persp_top * aspect;
    double persp_dx = (double)accum_jitter_x * 2.0 * persp_right /
                      (double)config->scene_w;
    double persp_dy = (double)accum_jitter_y * 2.0 * persp_top /
                      (double)config->scene_h;
    float mix = config->projection_mix;

    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;

    if (mix >= 0.999f) {
        glFrustum(-persp_right + persp_dx, persp_right + persp_dx,
                  -persp_top   + persp_dy, persp_top   + persp_dy,
                  near_z, far_z);
    } else {
        double ortho_near = -far_z;
        double ortho_far = far_z;
        double ortho_ref = scene_effective_ortho_ref(state, config);
        double ortho_top = ortho_ref * half_fovy_tan;
        double ortho_right = ortho_top * aspect;
        double ortho_dx = (double)accum_jitter_x * 2.0 * ortho_right /
                          (double)config->scene_w;
        double ortho_dy = (double)accum_jitter_y * 2.0 * ortho_top /
                          (double)config->scene_h;

        if (mix <= 0.001f) {
            glOrtho(-ortho_right + ortho_dx, ortho_right + ortho_dx,
                    -ortho_top   + ortho_dy, ortho_top   + ortho_dy,
                    ortho_near, ortho_far);
        } else {
            /* GL can't blend two projection *modes*, so build the glOrtho
             * and glFrustum matrices by hand and lerp them element-wise by
             * `mix` (1 = pure perspective, 0 = pure ortho, between = the
             * 2D<->3D transition). The o-prefixed and p-prefixed locals are
             * the ortho / perspective frustum edges (left/right/bottom/top);
             * the indexed writes are the standard column-major glOrtho /
             * glFrustum terms.
             *
             *   o-prefix = ortho, p-prefix = perspective
             *   .l = left   .r = right (= or_; `or` is iso646 alt for ||)
             *   .b = bottom .t = top
             *
             * Abbreviated because the eight names appear in matrix
             * formulas where the longer ortho_right / persp_bottom etc.
             * would obscure the structure. */
            GLfloat ortho[16] = { 0.0f };
            GLfloat persp[16] = { 0.0f };
            GLfloat blended[16];
            double ol = -ortho_right + ortho_dx;
            double or_ = ortho_right + ortho_dx;
            double ob = -ortho_top + ortho_dy;
            double ot = ortho_top + ortho_dy;
            double pl = -persp_right + persp_dx;
            double pr = persp_right + persp_dx;
            double pb = -persp_top + persp_dy;
            double pt = persp_top + persp_dy;

            ortho[0]  = (GLfloat)(2.0 / (or_ - ol));
            ortho[5]  = (GLfloat)(2.0 / (ot - ob));
            ortho[10] = (GLfloat)(-2.0 / (ortho_far - ortho_near));
            ortho[12] = (GLfloat)(-(or_ + ol) / (or_ - ol));
            ortho[13] = (GLfloat)(-(ot + ob) / (ot - ob));
            ortho[14] = (GLfloat)(-(ortho_far + ortho_near) /
                                  (ortho_far - ortho_near));
            ortho[15] = 1.0f;

            persp[0]  = (GLfloat)((2.0 * near_z) / (pr - pl));
            persp[5]  = (GLfloat)((2.0 * near_z) / (pt - pb));
            persp[8]  = (GLfloat)((pr + pl) / (pr - pl));
            persp[9]  = (GLfloat)((pt + pb) / (pt - pb));
            persp[10] = (GLfloat)(-(far_z + near_z) / (far_z - near_z));
            persp[11] = -1.0f;
            persp[14] = (GLfloat)(-(2.0 * far_z * near_z) / (far_z - near_z));

            for (int i = 0; i < 16; i++)
                blended[i] = ortho[i] + (persp[i] - ortho[i]) * mix;
            glLoadMatrixf(blended);
        }
    }
}

/* scene_apply_camera moved to src/app/glr_camera.c as
 * glr_camera_load_modelview (audit #11). The scene module no longer
 * owns a camera apply helper; callers populate GL_MODELVIEW
 * themselves before invoking scene_render_3d_scene. */

static void scene_apply_quality_config(const SceneRenderConfig *config) {
    if (config->multisample_enabled) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);
    if (config->line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);
}

static void scene_apply_wireframe_config(const SceneRenderConfig *config) {
    if (config->wireframe)
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
}

static void scene_prepare_frame_context(SceneFrameRenderContext *ctx,
                                        const SceneRenderConfig *config) {
    ctx->config = *config;
}

/* ========================================================================= */
/* 3D scene render (viewport offset to the right of the code panel)           */
/* ========================================================================= */

/* Three orthogonal arms (X / Z ground plane + vertical Y) of the orbit
 * gizmo, emitted between an active glBegin(GL_LINES)/glEnd(). The Y arm
 * visualizes the axis that shift + right-drag pans the target along. */
static void orbit_gizmo_axes(float tx, float ty, float tz, float r) {
    glVertex3f(tx - r, ty, tz); glVertex3f(tx + r, ty, tz);
    glVertex3f(tx, ty, tz - r); glVertex3f(tx, ty, tz + r);
    glVertex3f(tx, ty - r, tz); glVertex3f(tx, ty + r, tz);
}

/* Crosshair gizmo at the orbit target. Visible only while the camera is
 * moving (during drag or while momentum carries it); fades out. REPL-only -
 * never exported. Styled to match the other scene helpers: soft halo line
 * under a bright core, alpha driven by g_cam_motion_glow. Drawn in two
 * passes so it is never fully hidden inside user geometry — pass 0 is a
 * depth-disabled stippled ghost at reduced alpha, pass 1 is the solid
 * depth-tested gizmo on top where it isn't occluded. */
static void draw_orbit_target(const SceneFrameRenderContext *frame_ctx) {
    const SceneRenderConfig *config = &frame_ctx->config;
    float glow = config->cam_motion_glow;
    if (glow <= 0.0f) return;
    if (glow > 1.0f) glow = 1.0f;

    scene_render_push_state();
    glDisable(GL_LIGHTING);
    glDepthMask(GL_FALSE);
    if (config->line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float r = 0.08f * config->cam_dist;
    float tx = config->cam_tx, ty = config->cam_ty, tz = config->cam_tz;

    /* scene_render_push/pop_state brackets GL_ALL_ATTRIB_BITS, so the
     * depth-test / stipple toggles below are restored by the outer pop. */
    for (int pass = 0; pass < 2; pass++) {
        float occ;
        if (pass == 0) {
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_LINE_STIPPLE);
            glLineStipple(1, SCENE_OCCLUDED_GHOST_STIPPLE);
            occ = SCENE_OCCLUDED_GHOST_ALPHA;
        } else {
            glEnable(GL_DEPTH_TEST);
            glDisable(GL_LINE_STIPPLE);
            occ = 1.0f;
        }

        /* Halo pass: wide, translucent warm amber under the crosshair */
        glLineWidth(6.0f);
        scene_clr_a(SCENE_CLR_ORBIT_GLOW_OUTER, 0.18f * glow * occ);
        glBegin(GL_LINES);
        orbit_gizmo_axes(tx, ty, tz, r);
        glEnd();

        /* Core pass: thin bright crosshair */
        glLineWidth(1.5f);
        scene_clr_a(SCENE_CLR_ORBIT_GLOW_MID, 0.90f * glow * occ);
        glBegin(GL_LINES);
        orbit_gizmo_axes(tx, ty, tz, r);
        glEnd();

        /* Center dot (stipple doesn't apply to GL_POINTS; the depth-off
         * pass just leaves a faint always-visible marker). */
        glPointSize(5.0f);
        scene_clr_a(SCENE_CLR_ORBIT_GLOW_INNER, 0.95f * glow * occ);
        glBegin(GL_POINTS);
        glVertex3f(tx, ty, tz);
        glEnd();
    }
    glLineWidth(1.0f);
    glPointSize(1.0f);

    /* World-position readout floating just above the Y arm. Depth-test off
     * so the coordinates stay legible even when the gizmo is inside
     * geometry; alpha follows the same glow fade as the crosshair. */
    char coord[48];
    snprintf(coord, sizeof coord, "(%.2f, %.2f, %.2f)", tx, ty, tz);
    glDisable(GL_DEPTH_TEST);
    scene_clr_a(SCENE_CLR_ORBIT_GLOW_INNER, 0.95f * glow);
    scene_draw_bitmap_text(FONT_SMALL,
                           tx + r * 0.15f, ty + r * 1.15f, tz,
                           coord);

    /* scene_render_pop_state restores depth mask and blend state via
     * GL_ALL_ATTRIB_BITS; no manual teardown needed. */
    scene_render_pop_state();
}

static void scene_apply_clear_color(const float clear_color[4]) {
    glClearColor(clear_color[0], clear_color[1],
                 clear_color[2], clear_color[3]);
}

/* --- render_3d_scene_pass phases ---
 *
 * The accumulation-AA loop calls render_3d_scene_pass once per
 * jitter sample, and each call does five distinct things in order:
 * projection/lighting setup, user fill, scene helpers (backdrop /
 * grid / axes / orbit target), and post-fill / post-overlays
 * caller hooks. The orchestrator below is now ~25 lines that just
 * brackets the outer glPushAttrib pair and calls into each phase. */

static void scene_pass_setup(const SceneRendererState *state,
                             const SceneFrameRenderContext *frame_ctx,
                             float accum_jitter_x, float accum_jitter_y) {
    const SceneRenderConfig *config = &frame_ctx->config;
    prof_begin(PROF_SCENE_3D_SETUP);
    glPushAttrib(GL_ALL_ATTRIB_BITS);

    scene_apply_projection(state, config, accum_jitter_x, accum_jitter_y);
    /* scene_apply_projection leaves matrix mode set to GL_PROJECTION;
     * switch back so the user's glTranslatef / glRotatef inside
     * execute_fn modifies modelview, not projection. The caller is
     * expected to have populated GL_MODELVIEW with the camera before
     * invoking us (see render.h's contract); we just need the mode
     * set so subsequent calls land on the right stack. */
    glMatrixMode(GL_MODELVIEW);

    scene_lights_setup(frame_ctx);
    scene_backdrop_setup_lights(frame_ctx);
    glDisable(GL_LIGHTING); /* baseline: disabled; execute_commands() enables if user typed it */

    /* glColorMaterial mode and default material specular/shininess are
     * set once at startup via repl_apply_init_bootstrap. The outer
     * glPushAttrib(GL_ALL_ATTRIB_BITS) preserves them across user
     * commands so this pass doesn't need to re-assert them per frame. */

    scene_apply_quality_config(config);
    scene_apply_wireframe_config(config);
    prof_accum_end(PROF_SCENE_3D_SETUP);
}

static void scene_pass_fill(const SceneRenderConfig *config) {
    prof_begin(PROF_SCENE_3D_FILL);
    glPushMatrix();
    if (config->execute_fn) {
        SceneExecuteContext ctx = { .purpose = SCENE_EXEC_MAIN_FILL };
        config->execute_fn(&ctx, config->execute_user_data);
    }
    glPopMatrix();
    prof_accum_end(PROF_SCENE_3D_FILL);

    if (config->post_fill_fn)
        config->post_fill_fn(config->post_fill_user_data);

    if (config->wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

/* Draw translucent scene helpers after the main geometry so antialiased
 * edges blend against the final background color rather than the clear
 * color from earlier in the frame. */
static void scene_pass_helpers(const SceneFrameRenderContext *frame_ctx) {
    prof_begin(PROF_SCENE_3D_HELPERS);
    prof_begin(PROF_SCENE_3D_BACKDROP);
    scene_backdrop_render(frame_ctx);
    prof_accum_end(PROF_SCENE_3D_BACKDROP);
    prof_begin(PROF_SCENE_3D_GRID);
    scene_grid_render(frame_ctx);
    prof_accum_end(PROF_SCENE_3D_GRID);
    prof_begin(PROF_SCENE_3D_AXES);
    scene_axes_render(frame_ctx);
    prof_accum_end(PROF_SCENE_3D_AXES);
    prof_begin(PROF_SCENE_3D_ORBIT_TARGET);
    draw_orbit_target(frame_ctx);
    prof_accum_end(PROF_SCENE_3D_ORBIT_TARGET);
    prof_accum_end(PROF_SCENE_3D_HELPERS);
}

/* Polygon outline overlay, vertex-point overlay, vertex-number /
 * normal-vector labels, and the cursor-edit guide stack all render
 * here through post_overlays_fn — none of them are scene-internal
 * any more (see src/app/glr_ctrl.c for the bodies). post_overlays_fn
 * fires after lights_render so its output sits on top of the
 * scene's helpers. */
static void scene_pass_overlays(const SceneFrameRenderContext *frame_ctx) {
    prof_begin(PROF_SCENE_3D_OVERLAYS);
    scene_lights_render(frame_ctx);
    if (frame_ctx->config.post_overlays_fn)
        frame_ctx->config.post_overlays_fn(
            frame_ctx->config.post_overlays_user_data);
    glPopAttrib();
    prof_accum_end(PROF_SCENE_3D_OVERLAYS);
}

static void render_3d_scene_pass(const SceneRendererState *state,
                                 const SceneRenderConfig *config,
                                 float accum_jitter_x,
                                 float accum_jitter_y) {
    SceneFrameRenderContext frame_ctx;
    scene_prepare_frame_context(&frame_ctx, config);

    scene_pass_setup(state, &frame_ctx, accum_jitter_x, accum_jitter_y);
    scene_pass_fill(config);
    scene_pass_helpers(&frame_ctx);
    scene_pass_overlays(&frame_ctx);
}

int scene_render_3d_scene(SceneRendererState *state,
                          const SceneRenderConfig *config) {
    if (validate_render_config(config) < 0)
        return -1;
    if (!state) { errno = EINVAL; return -1; }

    int accum_passes = config->accum_passes;
    glViewport(config->scene_x, config->scene_y,
               config->scene_w, config->scene_h);
    scene_apply_clear_color(config->clear_color);

    /* Refresh the ortho scale reference. Done here — modelview still
     * holds the caller's camera, nothing has touched it yet — so the
     * feedback probe sees the right matrix and one update serves every
     * jitter sample below. */
    scene_update_ortho_ref(state, config);

    /* Resolve the canonical active projection ONCE, before the AA
     * jitter loop. Per-sample apply reads it but no longer writes —
     * the cached desc reflects the discrete-mode projection a faithful
     * reshape() would emit, not a transient sample. */
    scene_compute_active_projection(state, config);

    int do_accum = config->use_accum &&
                   config->accum_effect != SCENE_ACCUM_EFFECT_OFF &&
                   accum_passes > 1;
    if (do_accum) {
        int blur = (SCENE_ACCUM_EFFECT_IS_BLUR(config->accum_effect) &&
                    config->setup_subframe_fn != NULL);
        /* Full-window clear once — this is the frame's only clear, so the
         * chrome regions outside the scene rect (menu bar, status bar,
         * code-panel backdrop) depend on it before the 2D overlays paint. */
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_ACCUM_BUFFER_BIT);
        /* Optionally confine the repeated per-pass clears and the glAccum
         * read/return to the scene viewport: glClear/glAccum are bounded by
         * the scissor box (not the viewport), so this lets the up-to-16x
         * accumulation work skip the dead region under the code panel
         * instead of scanning the whole window. In theory a smaller scissor
         * region is a strict reduction in pixel-copy work; in practice on
         * macOS it measured *slower*, so it's off by default — see
         * CFG_DEFAULT_USE_ACCUM_AA_SCISSORS in glr_defaults.h. */
        int use_scissor = config->use_accum_aa_scissors;
        if (use_scissor) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(config->scene_x, config->scene_y,
                      config->scene_w, config->scene_h);
        }
        float weight = 1.0f / (float)accum_passes;
        for (int pass_idx = 0; pass_idx < accum_passes; pass_idx++) {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (blur) {
                /* Per-pass mutable copy: the callback may rewrite cam_* so
                 * grid/axes/orbit-target/lights and the ortho projection
                 * blur with the camera. Recompute the active projection from
                 * the per-pass config (the once-before-loop computation above
                 * reflects the base camera; the AA path reuses it). */
                SceneRenderConfig pass_cfg = *config;
                config->setup_subframe_fn(config->setup_subframe_user_data,
                                          pass_idx, accum_passes, &pass_cfg);
                scene_compute_active_projection(state, &pass_cfg);
                render_3d_scene_pass(state, &pass_cfg, 0.0f, 0.0f);
            } else {
                render_3d_scene_pass(state, config,
                                     g_jitter_table[pass_idx % MAX_ACCUM_SAMPLES][0],
                                     g_jitter_table[pass_idx % MAX_ACCUM_SAMPLES][1]);
            }
            glAccum(GL_ACCUM, weight);
        }
        glAccum(GL_RETURN, 1.0f);
        if (use_scissor)
            glDisable(GL_SCISSOR_TEST);
    } else {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        render_3d_scene_pass(state, config, 0.0f, 0.0f);
    }

    /* Once per frame, on the fully resolved scene image (covers both
     * the accum and non-accum branches), before any 2D overlay. */
    if (config->post_filter_mode > SCENE_POST_FILTER_OFF) {
        prof_begin(PROF_SCENE_3D_POST_PROCESS);
        scene_postprocess_filter_render(config->post_filter_mode,
                                        config->scene_x, config->scene_y,
                                        config->scene_w, config->scene_h);
        prof_accum_end(PROF_SCENE_3D_POST_PROCESS);
    }
    return 0;
}
