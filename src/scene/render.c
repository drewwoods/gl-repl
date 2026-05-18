/*
 * scene_render.c - 3D scene rendering (frame prep, edit guides)
 *
 * Extracted from sample.c for maintainability.
 */
#include "axes.h"
#include "backdrop.h"
#include "grid.h"
#include "lights.h"
#include "postprocess_filter.h"
#include "render_types.h"
#include "palette.h"
#include "render.h"
#include "prof.h"
#include "config.h"

#include <errno.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

/* Reject SceneRenderConfig values that would cause undefined behavior or
 * never-terminating loops downstream. Returns 0 on valid, sets errno and
 * returns -1 on failure. The grid renderer's `for (v = -extent; v <= extent;
 * v += step)` is the most consequential — step <= 0 hangs the GLUT main
 * loop, so a zeroed config (the natural memset default) is a hard error
 * the moment the grid is enabled. */
static int validate_render_config(const SceneRenderConfig *config) {
    if (!config)                                       goto bad;
    if (config->scene_w <= 0 || config->scene_h <= 0)  goto bad;
    if (config->grid_theme < 0
        || config->grid_theme >= GRID_THEME_COUNT)     goto bad;
    if (config->axes_theme < 0
        || config->axes_theme >= AXES_THEME_COUNT)     goto bad;
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
    if (config->use_accum && config->accum_aa_enabled) {
        if (config->accum_samples < 1
            || config->accum_samples > MAX_ACCUM_SAMPLES)     goto bad;
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

/* 2D ortho scale reference (depth-center of the drawn geometry). How it
 * is sampled — once at the switch vs. every frame — is selected at
 * compile time by GLR_ORTHO_REF_MODE in config.h; see
 * scene_update_ortho_ref. 0 means "no usable measurement" and
 * scene_apply_projection falls back to config->cam_dist (the old
 * orbit-target-plane behavior). */
static double g_ortho_ref_dist = 0.0;

/* Last projection scene_apply_projection() resolved this frame. Cached
 * for scene_get_active_projection() so the exporter/code panel can emit
 * a faithful reshape() without re-deriving the math (Tenet 3). Default
 * mirrors the steady 3D frustum so a getter call before the first frame
 * is still sane. */
static SceneProjectionDesc g_active_projection = {
    0, 45.0, 0.1, 200.0, 0.0, -200.0, 200.0
};

void scene_get_active_projection(SceneProjectionDesc *out) {
    if (out)
        *out = g_active_projection;
}

/* Run the user geometry through GL_FEEDBACK with a wide ortho box so the
 * recorded window z maps linearly back to eye space, and return the
 * depth-center (midpoint of the min/max eye distance) of the geometry.
 *
 * An identity probe projection would clip to the NDC unit cube, so any
 * geometry past |z_eye| > 1 (essentially everything) is discarded and
 * feedback returns nothing. glOrtho(-B,B,-B,B,-B,B) keeps the whole
 * frustum-visible scene; with the default glDepthRange(0,1) a vertex's
 * window z is (-z_eye/B + 1)/2, so z_eye = -B*(2*winz - 1) and the
 * camera distance is -z_eye.
 *
 * Returns 0.0 when there is nothing to measure or the feedback buffer
 * overflowed (glRenderMode < 0) — the caller treats 0 as "use cam_dist".
 * The fixed buffer caps how much geometry can be probed; very dense
 * scenes simply fall back, which is safe. */
static double scene_probe_eye_dist(const SceneRenderConfig *config) {
    static GLfloat fb[96 * 1024]; /* ~384 KB; overflow -> cam_dist fallback */
    /* Probe-projection half-extent; >= far_z so nothing the real frustum
     * can show is clipped during the feedback pass. */
    const double PROBE_BOX = 200.0;
    GLint n;
    int i;
    double dmin = 0.0, dmax = 0.0;
    int have = 0;

    if (!config->execute_fn)
        return 0.0;

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(-PROBE_BOX, PROBE_BOX, -PROBE_BOX, PROBE_BOX,
            -PROBE_BOX, PROBE_BOX);
    glMatrixMode(GL_MODELVIEW); /* leave camera modelview untouched */

    glFeedbackBuffer((GLsizei)(sizeof fb / sizeof fb[0]), GL_3D, fb);
    glRenderMode(GL_FEEDBACK);

    glPushMatrix();
    {
        SceneExecuteContext ctx = { 0 };
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
            double dist = PROBE_BOX * (2.0 * (double)fb[i + 2] - 1.0);
            if (dist > 1e-4 && dist < PROBE_BOX) {
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
 * in progress or we are dwelling in 2D). Sampling strategy is chosen at
 * compile time by GLR_ORTHO_REF_MODE.
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
static void scene_update_ortho_ref(const SceneRenderConfig *config) {
    int ortho_now = (config->projection_mix < 0.999f);

#if GLR_ORTHO_REF_MODE == GLR_ORTHO_REF_PERFRAME
    g_ortho_ref_dist = ortho_now ? scene_probe_eye_dist(config) : 0.0;
#else /* GLR_ORTHO_REF_FROZEN */
    {
        static int s_ortho_active = 0;
        if (ortho_now && !s_ortho_active)
            g_ortho_ref_dist = scene_probe_eye_dist(config);
        else if (!ortho_now && s_ortho_active)
            g_ortho_ref_dist = 0.0;
        s_ortho_active = ortho_now;
    }
#endif
}

static void scene_apply_projection(const SceneRenderConfig *config,
                                   float accum_jitter_x,
                                   float accum_jitter_y) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    /* Build a jitter-aware perspective frustum.  With zero jitter this is
     * identical to gluPerspective(45, aspect, 0.1, 200). */
    double near_z = 0.1, far_z = 200.0;
    double aspect  = (double)config->scene_w / (double)config->scene_h;
    double persp_top   = near_z * tan(45.0 * M_PI / 360.0);
    double persp_right = persp_top * aspect;
    double persp_dx = (double)accum_jitter_x * 2.0 * persp_right /
                      (double)config->scene_w;
    double persp_dy = (double)accum_jitter_y * 2.0 * persp_top /
                      (double)config->scene_h;
    float mix = config->projection_mix;

    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;

    /* Cache the canonical (jitter-free) projection for the exporter /
     * code panel. The continuous blend is snapped to the dominant side
     * because reshape() emits one discrete mode, not an interpolation. */
    {
        double ortho_ref = (g_ortho_ref_dist > 1e-4)
                               ? g_ortho_ref_dist
                               : (double)config->cam_dist;
        g_active_projection.ortho      = (mix < 0.5f) ? 1 : 0;
        g_active_projection.fovy_deg   = 45.0;
        g_active_projection.near_z     = near_z;
        g_active_projection.far_z      = far_z;
        g_active_projection.ortho_top  = ortho_ref * tan(45.0 * M_PI / 360.0);
        g_active_projection.ortho_near = -far_z;
        g_active_projection.ortho_far  = far_z;
    }

    if (mix >= 0.999f) {
        glFrustum(-persp_right + persp_dx, persp_right + persp_dx,
                  -persp_top   + persp_dy, persp_top   + persp_dy,
                  near_z, far_z);
    } else {
        double ortho_near = -far_z;
        double ortho_far = far_z;
        double ortho_ref = (g_ortho_ref_dist > 1e-4)
                               ? g_ortho_ref_dist
                               : (double)config->cam_dist;
        double ortho_top = ortho_ref * tan(45.0 * M_PI / 360.0);
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

void scene_apply_camera(float rx, float ry, float dist,
                        float tx, float ty, float tz) {
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -dist);
    glRotatef(rx, 1, 0, 0);
    glRotatef(ry, 0, 1, 0);
    glTranslatef(-tx, -ty, -tz);
}

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
    ctx->focus = config->focus;
}

/* ========================================================================= */
/* 3D scene render (viewport offset to the right of the code panel)           */
/* ========================================================================= */

/* The replay-mode tess-preview wireframe overlay used to live here, but it
 * iterated the user's flat program and applied REPL-side transforms —
 * both REPL concerns. The walk now lives behind a callback API in the
 * REPL replay module; the visual rendering lives in the controller's
 * post_fill_fn body, which installs glBegin / glVertex / glEnd
 * callbacks. */

/* Ground-plane crosshair gizmo at the orbit target. Visible only while the
 * camera is moving (during drag or while momentum carries it); fades out.
 * REPL-only - never exported. Styled to match the other scene helpers:
 * soft halo line under a bright core, alpha driven by g_cam_motion_glow. */
static void draw_orbit_target(const SceneFrameRenderContext *frame_ctx) {
    const SceneRenderConfig *config = &frame_ctx->config;
    float glow = config->cam_motion_glow;
    if (glow <= 0.0f) return;
    if (glow > 1.0f) glow = 1.0f;

    scene_render_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    if (config->line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float r = 0.08f * config->cam_dist;
    float tx = config->cam_tx, ty = config->cam_ty, tz = config->cam_tz;

    /* Halo pass: wide, translucent warm amber under the crosshair */
    glLineWidth(6.0f);
    scene_clr_a(SCENE_CLR_ORBIT_GLOW_OUTER, 0.18f * glow);
    glBegin(GL_LINES);
    glVertex3f(tx - r, ty, tz); glVertex3f(tx + r, ty, tz);
    glVertex3f(tx, ty, tz - r); glVertex3f(tx, ty, tz + r);
    glEnd();

    /* Core pass: thin bright crosshair */
    glLineWidth(1.5f);
    scene_clr_a(SCENE_CLR_ORBIT_GLOW_MID, 0.90f * glow);
    glBegin(GL_LINES);
    glVertex3f(tx - r, ty, tz); glVertex3f(tx + r, ty, tz);
    glVertex3f(tx, ty, tz - r); glVertex3f(tx, ty, tz + r);
    glEnd();
    glLineWidth(1.0f);

    /* Center dot */
    glPointSize(5.0f);
    scene_clr_a(SCENE_CLR_ORBIT_GLOW_INNER, 0.95f * glow);
    glBegin(GL_POINTS);
    glVertex3f(tx, ty, tz);
    glEnd();
    glPointSize(1.0f);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    scene_render_pop_state();
}

static void scene_apply_clear_color(const float clear_color[4]) {
    glClearColor(clear_color[0], clear_color[1],
                 clear_color[2], clear_color[3]);
}

/* The replay-fade overlay pass used to live here; it has been moved out
 * to the REPL controller (imrepl_ctrl.c) and is invoked through the
 * generic post_fill_fn hook on SceneRenderConfig. The scene module no
 * longer knows what's being drawn between the main fill and the
 * grid/axes/backdrop helpers — only that some caller-supplied function
 * may want a turn at GL state when post_fill_fn != NULL. */

static void render_3d_scene_pass(const SceneRenderConfig *config,
                                 float accum_jitter_x,
                                 float accum_jitter_y) {
    SceneFrameRenderContext frame_ctx;
    scene_prepare_frame_context(&frame_ctx, config);

    prof_begin(PROF_SCENE_3D_SETUP);
    glPushAttrib(GL_ALL_ATTRIB_BITS);

    scene_apply_projection(config, accum_jitter_x, accum_jitter_y);
    /* scene_apply_projection leaves matrix mode set to GL_PROJECTION; the
     * old scene_apply_camera_view() helper used to switch back to
     * GL_MODELVIEW as a side effect. With camera apply now done by the
     * caller before scene_render_3d_scene, the switch must happen here
     * — otherwise the user's glTranslatef / glRotatef inside execute_fn
     * would modify the projection matrix and objects would render in
     * unrelated locations. The caller's prior scene_apply_camera() left
     * the modelview correctly populated; we just need the mode set. */
    glMatrixMode(GL_MODELVIEW);

    scene_lights_setup(&frame_ctx);
    glDisable(GL_LIGHTING); /* baseline: disabled; execute_commands() enables if user typed it */

    /* glColorMaterial mode and default material specular/shininess are
     * set once at startup via repl_apply_init_bootstrap. The outer
     * glPushAttrib(GL_ALL_ATTRIB_BITS) preserves them across user
     * commands so this pass doesn't need to re-assert them per frame. */

    scene_apply_quality_config(config);
    scene_apply_wireframe_config(config);
    prof_accum_end(PROF_SCENE_3D_SETUP);

    {
        prof_begin(PROF_SCENE_3D_FILL);
        glPushMatrix();
        if (config->execute_fn) {
            SceneExecuteContext ctx = { 0 };
            config->execute_fn(&ctx, config->execute_user_data);
        }
        glPopMatrix();
        prof_accum_end(PROF_SCENE_3D_FILL);

        if (config->post_fill_fn)
            config->post_fill_fn(config->post_fill_user_data);
    }

    if (config->wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* Draw translucent scene helpers after the main geometry so antialiased
     * edges blend against the final background color rather than the clear
     * color from earlier in the frame. */
    prof_begin(PROF_SCENE_3D_HELPERS);
    prof_begin(PROF_SCENE_3D_BACKDROP);
    scene_backdrop_render(&frame_ctx);
    prof_accum_end(PROF_SCENE_3D_BACKDROP);
    prof_begin(PROF_SCENE_3D_GRID);
    scene_grid_render(&frame_ctx);
    prof_accum_end(PROF_SCENE_3D_GRID);
    prof_begin(PROF_SCENE_3D_AXES);
    scene_axes_render(&frame_ctx);
    prof_accum_end(PROF_SCENE_3D_AXES);
    prof_begin(PROF_SCENE_3D_ORBIT_TARGET);
    draw_orbit_target(&frame_ctx);
    prof_accum_end(PROF_SCENE_3D_ORBIT_TARGET);
    prof_accum_end(PROF_SCENE_3D_HELPERS);

    /* Polygon outline overlay, vertex-point overlay, vertex-number /
     * normal-vector labels, and the cursor-edit guide stack all render
     * here through post_overlays_fn — none of them are scene-internal
     * any more (see imrepl_ctrl.c for the bodies). post_overlays_fn
     * fires after lights_render so its output sits on top of the
     * scene's helpers. */
    prof_begin(PROF_SCENE_3D_OVERLAYS);

    scene_lights_render(&frame_ctx);

    if (config->post_overlays_fn)
        config->post_overlays_fn(config->post_overlays_user_data);

    glPopAttrib();
    prof_accum_end(PROF_SCENE_3D_OVERLAYS);
}

int scene_render_3d_scene(const SceneRenderConfig *config) {
    if (validate_render_config(config) < 0)
        return -1;

    int accum_samples = config->accum_samples;
    glViewport(config->scene_x, config->scene_y,
               config->scene_w, config->scene_h);
    scene_apply_clear_color(config->clear_color);

    /* Refresh the ortho scale reference. Done here — modelview still
     * holds the caller's camera, nothing has touched it yet — so the
     * feedback probe sees the right matrix and one update serves every
     * jitter sample below. */
    scene_update_ortho_ref(config);

    if (config->use_accum && config->accum_aa_enabled && accum_samples > 1) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_ACCUM_BUFFER_BIT);
        float weight = 1.0f / (float)accum_samples;
        for (int sample_idx = 0; sample_idx < accum_samples; sample_idx++) {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            render_3d_scene_pass(config,
                                 g_jitter_table[sample_idx % MAX_ACCUM_SAMPLES][0],
                                 g_jitter_table[sample_idx % MAX_ACCUM_SAMPLES][1]);
            glAccum(GL_ACCUM, weight);
        }
        glAccum(GL_RETURN, 1.0f);
    } else {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        render_3d_scene_pass(config, 0.0f, 0.0f);
    }

    /* Once per frame, on the fully resolved scene image (covers both
     * the accum and non-accum branches), before any 2D overlay. */
    if (config->post_filter_mode > SCENE_POST_FILTER_OFF)
        scene_postprocess_filter_render(config->post_filter_mode,
                                        config->scene_x, config->scene_y,
                                        config->scene_w, config->scene_h);
    return 0;
}
