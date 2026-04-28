/*
 * scene_render.c - 3D scene rendering (frame prep, edit guides)
 *
 * Extracted from sample.c for maintainability.
 */
#include "sample.h"
#include "scene_axes.h"
#include "scene_backdrop.h"
#include "scene_geometry_guides.h"
#include "scene_grid.h"
#include "scene_guides_shared.h"
#include "scene_lights.h"
#include "scene_overlays.h"
#include "scene_render_types.h"
#include "scene_render.h"
#include "scene_transform_guides.h"
#include "scene_transform_utils.h"
#include "prof.h"

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
    double near_z = 0.1, far_z = 100.0;
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

static void scene_apply_camera_view(const SceneRenderConfig *config) {
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -config->cam_dist);
    glRotatef(config->cam_rx, 1, 0, 0);
    glRotatef(config->cam_ry, 0, 1, 0);
    glTranslatef(-config->cam_tx, -config->cam_ty, -config->cam_tz);
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

    /* Modelview setup is T(-dist) * Rx * Ry * T(-target). Solving that for
     * the eye position gives target.y + sin(rx) * dist; ry does not affect Y. */
    float camera_rx_rad = config->cam_rx * (float)M_PI / 180.0f;
    ctx->camera_world_y = config->cam_ty +
                          sinf(camera_rx_rad) * config->cam_dist;
    ctx->camera_below_water_surface = (ctx->camera_world_y < 0.0f);
}

/* ========================================================================= */
/* 3D scene render (viewport offset to the right of the code panel)           */
/* ========================================================================= */

static void draw_replay_tess_preview(const SceneRenderConfig *config) {
    if (!config->replay_tess_preview)
        return;

    scene_render_push_state();
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.30f, 0.95f, 0.75f, 0.80f);
    glLineWidth(2.0f);

    glPushMatrix();
    {
        const GLCmd *flat_cmds = config->flat_program.cmds;
        int flat_cmd_count = config->flat_program.cmd_count;
        int in_contour = 0;
        int matrix_depth = 0;
        for (int i = 0; i < flat_cmd_count; i++) {
            if (!flat_cmds[i].valid) continue;

            if (is_transform_cmd(flat_cmds[i].type)) {
                if (!in_contour)
                    scene_apply_tracked_transform(&flat_cmds[i], &matrix_depth);
                continue;
            }

            switch (flat_cmds[i].type) {
            case CMD_TESS_BEGIN_CONTOUR:
                if (in_contour)
                    glEnd();
                glBegin(GL_LINE_STRIP);
                in_contour = 1;
                break;
            case CMD_TESS_VERTEX:
                if (in_contour)
                    glVertex3f(flat_cmds[i].args[0], flat_cmds[i].args[1],
                               flat_cmds[i].args[2]);
                break;
            case CMD_TESS_END:
                if (in_contour) {
                    glEnd();
                    in_contour = 0;
                }
                break;
            default:
                break;
            }
        }
        if (in_contour)
            glEnd();
        scene_unwind_transform_stack(&matrix_depth);
    }
    glPopMatrix();

    glLineWidth(1.0f);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    if (config->user_lighting_enabled) glEnable(GL_LIGHTING);
    scene_render_pop_state();
}

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

static void scene_restore_predef_values(const float *src, int max_vals) {
    int count = max_vals;

    if (count > g_num_predef_vars)
        count = g_num_predef_vars;
    for (int i = 0; i < count; i++)
        g_predef_vars[i].value = src[i];
}

static void render_3d_scene_pass(const SceneRenderConfig *config,
                                 float accum_jitter_x,
                                 float accum_jitter_y) {
    FrameRenderContext frame_ctx;
    scene_prepare_frame_context(&frame_ctx, config);

    prof_begin(PROF_SCENE_3D_SETUP);
    glPushAttrib(GL_ALL_ATTRIB_BITS);

    scene_apply_projection(config, accum_jitter_x, accum_jitter_y);
    scene_apply_camera_view(config);

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
        int flat_cmd_count = config->flat_program.cmd_count;
        if (config->replay_has_fades)
            flat_cmd_count = config->replay_base_limit;

        prof_begin(PROF_SCENE_3D_FILL);
        glPushMatrix();
        if (config->execute_fn)
            config->execute_fn(1.0f, 0, flat_cmd_count, config->flat_program,
                            config->execute_user_data);
        glPopMatrix();
        prof_accum_end(PROF_SCENE_3D_FILL);

        if (config->replay_has_fades && config->execute_fn) {
            prof_begin(PROF_SCENE_3D_FADE);
            {
                const ReplayFadePlan *fade_plan = &config->replay_fade_plan;
                int batch_count;

                prof_begin(PROF_SCENE_3D_FADE_PROLOGUE);
                batch_count = fade_plan->batch_count;
                prof_accum_end(PROF_SCENE_3D_FADE_PROLOGUE);

                if (batch_count > 0) {
                    glPushAttrib(GL_ALL_ATTRIB_BITS);
                    scene_lights_setup(&frame_ctx);
                    glDisable(GL_LIGHTING);
                    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
                    GLfloat mspec[] = { 0.4f, 0.4f, 0.4f, 1.0f };
                    GLfloat mshin[] = { 30.0f };
                    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mspec);
                    glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, mshin);
                    scene_apply_quality_config(config);
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    glColor4f(0.70f, 0.70f, 0.80f, 1.0f);
                    if (config->wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
                    else glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

                    for (int batch_idx = 0; batch_idx < batch_count; batch_idx++) {
                        const ReplayFadeBatch *batch = &fade_plan->batches[batch_idx];
                        float alpha = fade_plan->batch_alpha[batch_idx];

                        if (alpha <= 0.0f)
                            continue;

                        prof_begin(PROF_SCENE_3D_FADE_BATCH_PREP);
                        scene_restore_predef_values(fade_plan->baseline_predef_vals,
                                                    MAX_PREDEF_VARS);
                        glColor4f(0.70f, 0.70f, 0.80f, alpha);
                        glPushMatrix();
                        prof_accum_end(PROF_SCENE_3D_FADE_BATCH_PREP);

                        prof_begin(PROF_SCENE_3D_FADE_BATCH_EXEC);
                        config->execute_fn(alpha, fade_plan->skip_limits[batch_idx], batch->new_pc,
                                        config->flat_program, config->execute_user_data);
                        prof_accum_end(PROF_SCENE_3D_FADE_BATCH_EXEC);

                        glPopMatrix();
                    }

                    prof_begin(PROF_SCENE_3D_FADE_BATCH_POST);
                    if (config->execute_reset_fn)
                        config->execute_reset_fn(config->execute_user_data);
                    glPopAttrib();
                    prof_accum_end(PROF_SCENE_3D_FADE_BATCH_POST);
                }
            }
            prof_accum_end(PROF_SCENE_3D_FADE);
        }
    }

    if (config->wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* Draw translucent scene helpers after the main geometry so antialiased
     * edges blend against the final background color rather than the clear
     * color from earlier in the frame. */
    prof_begin(PROF_SCENE_3D_HELPERS);
    scene_backdrop_render(&frame_ctx);
    scene_grid_render(&frame_ctx);
    scene_axes_render(&frame_ctx);
    draw_orbit_target(&frame_ctx);
    prof_accum_end(PROF_SCENE_3D_HELPERS);

    /* Polygon outline overlay (optional) + current-block highlight.
     * Each overlay pass is wrapped in push/pop so transforms don't bleed
     * between passes.  CMD_TRANSLATE3F is replayed within each pass so
     * outlines are positioned correctly even when transforms separate blocks. */
    prof_begin(PROF_SCENE_3D_OUTLINES);
    scene_overlays_render_outlines(&frame_ctx, config->show_current_poly,
                                   config->replay_tess_preview);
    prof_accum_end(PROF_SCENE_3D_OUTLINES);

    /* Vertex dots - replay transforms so dots match the filled geometry */
    prof_begin(PROF_SCENE_3D_OVERLAYS);
    glPushMatrix();
    /* Snapshot the pure camera-view matrix (before any user transforms are
     * applied below) so the transform guide can render in world axes at an
     * anchor point, independent of pre-cursor rotations. */
    float tg_cam_view[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, tg_cam_view);
    const SceneGuideSnapshot *guide_snapshot = &config->guide_snapshot;
    SceneTransformGuidePlan xform_guide_plan;
    scene_transform_guides_prepare(guide_snapshot, &xform_guide_plan);
    {
        const GLCmd *flat_cmds = config->flat_program.cmds;
        int flat_cmd_count = config->flat_program.cmd_count;
        int matrix_depth = 0;
        glPointSize(8.0f);
        if (config->replay_vertex_points)
            glColor3f(1.0f, 0.88f, 0.20f);
        else
            glColor3f(0.0f, 0.0f, 0.0f);

        for (int i = 0; i < flat_cmd_count; i++) {
            if (!flat_cmds[i].valid) continue;
            int is_cursor = (flat_cmds[i].src_cmd_idx == guide_snapshot->edit_line_idx);

            // Draw vertex guides and normals if this vertex is at the cursor
            // position. Do this inline so that the current model matrix is applied
            // to the guides, ensuring they are positioned correctly even when
            // transforms separate blocks.
            if (is_cursor && !config->replaying) {
                scene_geometry_guides_render_for_cursor(guide_snapshot);
            }

            scene_transform_guides_render_if_due(guide_snapshot,
                                                 &xform_guide_plan,
                                                 i, tg_cam_view);

            if (is_transform_cmd(flat_cmds[i].type)) {
                scene_apply_tracked_transform(&flat_cmds[i], &matrix_depth);
            } else if ((config->show_vpoints || config->replay_vertex_points) &&
                       (flat_cmds[i].type == CMD_VERTEX3F ||
                        flat_cmds[i].type == CMD_TESS_VERTEX)) {
                glBegin(GL_POINTS);
                glVertex3f(flat_cmds[i].args[0], flat_cmds[i].args[1],
                           flat_cmds[i].args[2]);
                glEnd();
            }
        }
        glPointSize(1.0f);
        scene_unwind_transform_stack(&matrix_depth);
    }
    glPopMatrix();
    glDisable(GL_BLEND);
    if (config->user_lighting_enabled) glEnable(GL_LIGHTING);
    prof_accum_end(PROF_SCENE_3D_OVERLAYS);

    prof_begin(PROF_SCENE_3D_HUD);
    if (config->replay_tess_preview)
        draw_replay_tess_preview(config);

    scene_lights_render(&frame_ctx);

    if (config->show_vnums)   scene_overlays_render_vertex_numbers(&frame_ctx);
    if (config->show_normals) scene_overlays_render_normal_vectors(&frame_ctx);
    glPopAttrib();
    prof_accum_end(PROF_SCENE_3D_HUD);
}

void scene_render_3d_scene(const SceneRenderConfig *config) {
    int accum_samples = config->accum_samples;
    glViewport(config->scene_x, config->scene_y,
               config->scene_w, config->scene_h);
    scene_apply_clear_color(config->flat_program);

    if (config->use_accum && config->accum_aa_enabled && accum_samples > 1) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_ACCUM_BUFFER_BIT);
        float weight = 1.0f / (float)accum_samples;
        for (int sample_idx = 0; sample_idx < accum_samples; sample_idx++) {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (config->replaying)
                scene_restore_predef_values(config->replay_fade_plan.baseline_predef_vals,
                                            MAX_PREDEF_VARS);
            render_3d_scene_pass(config,
                                 g_jitter_table[sample_idx % MAX_ACCUM_SAMPLES][0],
                                 g_jitter_table[sample_idx % MAX_ACCUM_SAMPLES][1]);
            glAccum(GL_ACCUM, weight);
        }
        glAccum(GL_RETURN, 1.0f);
    } else {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (config->replaying)
            scene_restore_predef_values(config->replay_fade_plan.baseline_predef_vals,
                                        MAX_PREDEF_VARS);
        render_3d_scene_pass(config, 0.0f, 0.0f);
    }
}
