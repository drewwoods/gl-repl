/*
 * scene_render.c - 3D scene rendering (frame prep, edit guides)
 *
 * Extracted from sample.c for maintainability.
 */
#include "axes.h"
#include "backdrop.h"
#include "grid.h"
#include "lights.h"
#include "overlays.h"
#include "render_types.h"
#include "render.h"
#include "prof.h"

#include <errno.h>

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

static void scene_apply_projection(const SceneRenderConfig *config,
                                   float accum_jitter_x,
                                   float accum_jitter_y) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    /* Build a jitter-aware perspective frustum.  With zero jitter this is
     * identical to gluPerspective(45, aspect, 0.1, 100). */
    double near_z = 0.1, far_z = 200.0;
    double aspect  = (double)config->scene_w / (double)config->scene_h;
    double top_v   = near_z * tan(45.0 * M_PI / 360.0);
    double right_v = top_v * aspect;
    double dx = (double)accum_jitter_x * 2.0 * right_v /
                (double)config->scene_w;
    double dy = (double)accum_jitter_y * 2.0 * top_v /
                (double)config->scene_h;

    glFrustum(-right_v + dx, right_v + dx,
              -top_v   + dy, top_v   + dy,
              near_z, far_z);
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

static void scene_prepare_frame_context(FrameRenderContext *ctx,
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
static void draw_orbit_target(const FrameRenderContext *frame_ctx) {
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
    glColor4f(1.00f, 0.70f, 0.25f, 0.18f * glow);
    glBegin(GL_LINES);
    glVertex3f(tx - r, ty, tz); glVertex3f(tx + r, ty, tz);
    glVertex3f(tx, ty, tz - r); glVertex3f(tx, ty, tz + r);
    glEnd();

    /* Core pass: thin bright crosshair */
    glLineWidth(1.5f);
    glColor4f(1.00f, 0.90f, 0.55f, 0.90f * glow);
    glBegin(GL_LINES);
    glVertex3f(tx - r, ty, tz); glVertex3f(tx + r, ty, tz);
    glVertex3f(tx, ty, tz - r); glVertex3f(tx, ty, tz + r);
    glEnd();
    glLineWidth(1.0f);

    /* Center dot */
    glPointSize(5.0f);
    glColor4f(1.00f, 0.95f, 0.75f, 0.95f * glow);
    glBegin(GL_POINTS);
    glVertex3f(tx, ty, tz);
    glEnd();
    glPointSize(1.0f);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    scene_render_pop_state();
}

static void scene_apply_clear_color(FlatProgramView flat_program) {
    float cr = 0.10f, cg = 0.10f, cb = 0.13f, ca = 1.0f;
    for (int ci = 0; ci < flat_program.cmd_count; ci++) {
        if (flat_program.cmds[ci].valid &&
            flat_program.cmds[ci].type == CMD_CLEAR_COLOR) {
            cr = flat_program.cmds[ci].args[0];
            cg = flat_program.cmds[ci].args[1];
            cb = flat_program.cmds[ci].args[2];
            ca = flat_program.cmds[ci].args[3];
        }
    }
    glClearColor(cr, cg, cb, ca);
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
    FrameRenderContext frame_ctx;
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

    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    GLfloat mspec[] = { 0.4f, 0.4f, 0.4f, 1.0f };
    GLfloat mshin[] = { 30.0f };
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mspec);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, mshin);

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

    /* Polygon outline overlay (optional) + current-block highlight.
     * Each overlay pass is wrapped in push/pop so transforms don't bleed
     * between passes.  CMD_TRANSLATE3F is replayed within each pass so
     * outlines are positioned correctly even when transforms separate blocks. */
    prof_begin(PROF_SCENE_3D_OUTLINES);
    scene_overlays_render_outlines(&frame_ctx, config->show_current_poly,
                                   config->replay_tess_preview);
    prof_accum_end(PROF_SCENE_3D_OUTLINES);

    /* Vertex dots and edit guides - replay transforms so positions match geometry */
    prof_begin(PROF_SCENE_3D_OVERLAYS);
    scene_overlays_render_vertex_points(&frame_ctx);
    prof_accum_end(PROF_SCENE_3D_OVERLAYS);

    prof_begin(PROF_SCENE_3D_HUD);

    scene_lights_render(&frame_ctx);

    if (config->show_vnums)   scene_overlays_render_vertex_numbers(&frame_ctx);
    if (config->show_normals) scene_overlays_render_normal_vectors(&frame_ctx);
    glPopAttrib();
    prof_accum_end(PROF_SCENE_3D_HUD);
}

int scene_render_3d_scene(const SceneRenderConfig *config) {
    if (validate_render_config(config) < 0)
        return -1;

    int accum_samples = config->accum_samples;
    glViewport(config->scene_x, config->scene_y,
               config->scene_w, config->scene_h);
    scene_apply_clear_color(config->flat_program);

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
    return 0;
}
