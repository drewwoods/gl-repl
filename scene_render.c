/*
 * scene_render.c - 3D scene rendering (frame prep, edit guides, replay HUD)
 *
 * Extracted from sample.c for maintainability.
 */
#include "sample.h"
#include "repl_core_internal.h"
#include "repl_executor.h"
#include "repl_replay.h"
#include "repl_state.h"
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
#include "ui_panels.h"
#include "prof.h"
#include "./include/gl_2d.h"

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

static int cmd_is_focus_vertex(const GLCmd *cmd) {
    return cmd->valid &&
           (cmd->type == CMD_VERTEX3F || cmd->type == CMD_TESS_VERTEX);
}

static void scene_focus_store(float x, float y, float z) {
    ReplRenderDerivedState *derived = repl_state_render_derived_mut();
    derived->focus_vertex[0] = x;
    derived->focus_vertex[1] = y;
    derived->focus_vertex[2] = z;
    *derived->focus_vertex_valid = 1;
}

static SceneFocusVertex scene_prepare_focus_vertex(void) {
    const ReplRenderDerivedState *derived = repl_state_render_derived();
    SceneFocusVertex focus = {
        .pos = {
            derived->focus_vertex[0],
            derived->focus_vertex[1],
            derived->focus_vertex[2],
        },
        .valid = *derived->focus_vertex_valid,
    };

    if (repl_state_edit_line() >= 0 && repl_state_edit_line() < repl_state_document_count() &&
        cmd_is_focus_vertex(&repl_state_document_cmds_mut()[repl_state_edit_line()])) {
        scene_focus_store(repl_state_document_cmds_mut()[repl_state_edit_line()].args[0],
                          repl_state_document_cmds_mut()[repl_state_edit_line()].args[1],
                          repl_state_document_cmds_mut()[repl_state_edit_line()].args[2]);
    } else if (!*derived->focus_vertex_valid) {
        for (int i = repl_state_edit_line() - 1; i >= 0; i--) {
            if (cmd_is_focus_vertex(&repl_state_document_cmds_mut()[i])) {
                scene_focus_store(repl_state_document_cmds_mut()[i].args[0],
                                  repl_state_document_cmds_mut()[i].args[1],
                                  repl_state_document_cmds_mut()[i].args[2]);
                break;
            }
        }
    }

    focus.pos[0] = derived->focus_vertex[0];
    focus.pos[1] = derived->focus_vertex[1];
    focus.pos[2] = derived->focus_vertex[2];
    focus.valid = *derived->focus_vertex_valid;
    return focus;
}

static void scene_render_config_init(SceneRenderConfig *config) {
    const ReplRenderState *render = repl_state_render();
    const ReplReplayRuntimeState *replay = repl_state_replay();
    scene_rect(&config->scene_x, &config->scene_y,
               &config->scene_w, &config->scene_h);
    if (config->scene_w < 1) config->scene_w = 1;
    if (config->scene_h < 1) config->scene_h = 1;

    {
        const ReplCameraState *cam = repl_state_camera();
        config->cam_dist = *cam->dist;
        config->cam_rx = *cam->rx;
        config->cam_ry = *cam->ry;
        config->cam_tx = *cam->tx;
        config->cam_ty = *cam->ty;
        config->cam_tz = *cam->tz;
        config->cam_motion_glow = *cam->motion_glow;
    }
    config->accum_jitter_x = *render->accum_jitter_x;
    config->accum_jitter_y = *render->accum_jitter_y;
    config->multisample_enabled = *render->multisample_enabled;
    config->line_smooth_enabled = *render->line_smooth_enabled;
    config->wireframe = *repl_state_presentation()->wireframe;
    config->grid_theme = *repl_state_presentation()->grid_theme;
    config->grid_extent_idx = *repl_state_presentation()->grid_extent_idx;
    config->grid_major_idx = *repl_state_presentation()->grid_major_idx;
    config->axes_theme = *repl_state_presentation()->axes_theme;
    config->show_guides = *repl_state_presentation()->show_vertex_guides;
    config->show_vpoints = *repl_state_presentation()->show_vertex_points;
    config->show_vnums = *repl_state_presentation()->show_vertex_labels;
    config->show_normals = *repl_state_presentation()->show_normal_vectors;
    config->replaying = *replay->active;
    config->replay_mode = *replay->mode;
    config->replay_tess_preview = config->replaying &&
                                  config->replay_mode == REPLAY_MODE_VERTEX;
    config->replay_vertex_points = config->replay_tess_preview;
    config->replay_has_fades = repl_replay_has_active_fades();
    config->replay_base_limit = config->replay_has_fades
                                   ? repl_replay_fill_base_limit()
                                   : 0;
    config->show_current_poly = *repl_state_presentation()->highlight_current_poly && !config->replaying;

    /* Boost translucent overlay alphas when the bg is darker than the
     * design-point luminance (~0.10).  K=0.02 softens the curve near
     * zero; result is clamped to [1, 3] so colours never blow out. */
    float bg_lum = 0.2126f * render->clear_color[0]
                 + 0.7152f * render->clear_color[1]
                 + 0.0722f * render->clear_color[2];
    float as_val = (0.10f + 0.02f) / fmaxf(bg_lum + 0.02f, 1e-4f);
    config->alpha_scale = as_val < 1.0f ? 1.0f : (as_val > 3.0f ? 3.0f : as_val);
}

static void scene_apply_projection(const SceneRenderConfig *config) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    /* Build a jitter-aware perspective frustum.  With zero jitter this is
     * identical to gluPerspective(45, aspect, 0.1, 100). */
    double near_z = 0.1, far_z = 100.0;
    double aspect  = (double)config->scene_w / (double)config->scene_h;
    double top_v   = near_z * tan(45.0 * M_PI / 360.0);
    double right_v = top_v * aspect;
    double dx = (double)config->accum_jitter_x * 2.0 * right_v /
                (double)config->scene_w;
    double dy = (double)config->accum_jitter_y * 2.0 * top_v /
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
    const ReplRenderDerivedState *derived = repl_state_render_derived();
    ctx->config = *config;
    ctx->focus.pos[0] = derived->focus_vertex[0];
    ctx->focus.pos[1] = derived->focus_vertex[1];
    ctx->focus.pos[2] = derived->focus_vertex[2];
    ctx->focus.valid = *derived->focus_vertex_valid;

    /* Modelview setup is T(-dist) * Rx * Ry * T(-target). Solving that for
     * the eye position gives target.y + sin(rx) * dist; ry does not affect Y. */
    float camera_rx_rad = config->cam_rx * (float)M_PI / 180.0f;
    ctx->camera_world_y = config->cam_ty +
                          sinf(camera_rx_rad) * config->cam_dist;
    ctx->camera_below_water_surface = (ctx->camera_world_y < 0.0f);

    if (config->grid_theme == GRID_THEME_FOCUS)
        ctx->focus = scene_prepare_focus_vertex();
}

static SceneGuideSnapshot scene_build_guide_snapshot(const SceneRenderConfig *config,
                                                     FlatProgramView flat_program) {
    const ReplFlatProgramState *flat_state = repl_state_flat_program();
    SceneGuideSnapshot snapshot = {
        .show_guides = config->show_guides,
        .replaying = config->replaying,
        .xform_guide_mode = *repl_state_presentation()->xform_guide_mode,
        .user_lighting_enabled = *flat_state->user_lighting_enabled,
        .anim_time = (*repl_state_variables()->anim_time),
        .input = repl_state_editor_input()->input,
        .input_len = *repl_state_editor_input()->input_len,
        .cursor_pos = *repl_state_editor_input()->cursor_pos,
        .edit_line_idx = repl_state_edit_line(),
        .inserting = repl_state_insert_mode(),
        .source_cmds = repl_state_document_cmds_mut(),
        .source_cmd_count = repl_state_document_count(),
        .flat_program = flat_program,
        .predef_vars = g_predef_vars,
        .predef_var_count = g_num_predef_vars,
        .alpha_scale = config->alpha_scale,
    };
    return snapshot;
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
        int in_contour = 0;
        int matrix_depth = 0;
        for (int i = 0; i < repl_state_flat_program_count(); i++) {
            if (!repl_state_flat_program_cmds_mut()[i].valid) continue;

            if (is_transform_cmd(repl_state_flat_program_cmds_mut()[i].type)) {
                if (!in_contour)
                    repl_executor_apply_tracked_transform_cmd(&repl_state_flat_program_cmds_mut()[i], &matrix_depth);
                continue;
            }

            switch (repl_state_flat_program_cmds_mut()[i].type) {
            case CMD_TESS_BEGIN_CONTOUR:
                if (in_contour)
                    glEnd();
                glBegin(GL_LINE_STRIP);
                in_contour = 1;
                break;
            case CMD_TESS_VERTEX:
                if (in_contour)
                    glVertex3f(repl_state_flat_program_cmds_mut()[i].args[0], repl_state_flat_program_cmds_mut()[i].args[1],
                               repl_state_flat_program_cmds_mut()[i].args[2]);
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
        repl_executor_unwind_tracked_transform_stack(&matrix_depth);
    }
    glPopMatrix();

    glLineWidth(1.0f);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    if (repl_state_flat_program_user_lighting_enabled()) glEnable(GL_LIGHTING);
    scene_render_pop_state();
}

static void draw_replay_hud(const SceneRenderConfig *config) {
    const ReplReplayRuntimeState *replay = repl_state_replay();
    char progress_txt[64];
    char kbd_txt[128];
    float progress = 0.0f;
    int scene_x = config->scene_x;
    int scene_y = config->scene_y;
    int scene_w = config->scene_w;
    int scene_h = config->scene_h;
    int hud_x = scene_x + REPLAY_HUD_MARGIN_X;
    /* Lifted by STATUSBAR_H so the HUD clears the amber status strip along
     * the bottom of the scene. */
    int hud_y = scene_y + REPLAY_HUD_MARGIN_Y + STATUSBAR_H;
    int hud_w = scene_w - 2 * REPLAY_HUD_MARGIN_X;
    int min_y = scene_y + STATUSBAR_H + 4;
    int max_y = scene_y + scene_h - REPLAY_HUD_HEIGHT - 4;

    if (!config->replaying)
        return;

    if (hud_w < REPLAY_HUD_MIN_WIDTH)
        hud_w = REPLAY_HUD_MIN_WIDTH;
    if (max_y >= min_y) {
        if (hud_y < min_y) hud_y = min_y;
        if (hud_y > max_y) hud_y = max_y;
    } else {
        hud_y = *repl_state_presentation()->code_panel_layout == CODE_PANEL_LAYOUT_TOP
              ? scene_y + scene_h - REPLAY_HUD_HEIGHT - 4
              : min_y;
    }
    if (*replay->total_flat_cmds > 0)
        progress = (float)*replay->pc / (float)*replay->total_flat_cmds;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    scene_render_push_state();
    glViewport(0, 0, *repl_state_viewport()->window_w, *repl_state_viewport()->window_h);
    gl2d_begin(*repl_state_viewport()->window_w, *repl_state_viewport()->window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Panel bg matches the menubar palette: #1d1d1d with subtle green tint
     * on the border so the HUD reads as paired with the green Replay button. */
    glColor4f(0.114f, 0.118f, 0.114f, 0.94f); /* #1d1e1d */
    glRectf((float)(hud_x), (float)(hud_y), (float)(hud_x)+(float)(hud_w), (float)(hud_y)+(float)(REPLAY_HUD_HEIGHT));
    glColor4f(0.188f, 0.298f, 0.220f, 0.95f); /* #304c38 */
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)hud_x + 0.5f,                      (float)hud_y + 0.5f);
    glVertex2f((float)(hud_x + hud_w) - 0.5f,            (float)hud_y + 0.5f);
    glVertex2f((float)(hud_x + hud_w) - 0.5f,            (float)(hud_y + REPLAY_HUD_HEIGHT) - 0.5f);
    glVertex2f((float)hud_x + 0.5f,                      (float)(hud_y + REPLAY_HUD_HEIGHT) - 0.5f);
    glEnd();

    /* Column layout: icon in a fixed gutter, both text rows share one
     * left edge so line1 (green status) and line2 (kbd hints) align. */
    int text_col_x = hud_x + REPLAY_HUD_TEXT_PAD_X + 18;   /* after 18px icon gutter */
    int icon_cx    = hud_x + REPLAY_HUD_TEXT_PAD_X + 7;    /* centered in gutter */
    int icon_cy    = hud_y + REPLAY_HUD_TEXT_LINE1_Y + FONT_SMALL_H / 2;
    int icon_sz    = 10;

    glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, 1.0f);
    if (*replay->state == REPLAY_PLAYING) {
        float bw = 3.0f, gap = 3.0f;
        float by0 = (float)icon_cy - (float)icon_sz * 0.5f;
        glRectf((float)(icon_cx - bw - gap * 0.5f), (float)(by0), (float)(icon_cx - bw - gap * 0.5f) + (float)(bw), (float)(by0) + (float)(icon_sz));
        glRectf((float)(icon_cx + gap * 0.5f),      (float)(by0), (float)(icon_cx + gap * 0.5f) + (float)(bw), (float)(by0) + (float)(icon_sz));
    } else if (*replay->state == REPLAY_DONE) {
        /* Square - run complete */
        float sx = (float)icon_cx - (float)icon_sz * 0.5f;
        float sy = (float)icon_cy - (float)icon_sz * 0.5f;
        glRectf((float)(sx), (float)(sy), (float)(sx) + (float)(icon_sz), (float)(sy) + (float)(icon_sz));
    } else {
        /* Play triangle - paused / stopped, click to (re)start */
        float x0 = (float)icon_cx - (float)icon_sz * 0.5f;
        float cy = (float)icon_cy;
        glBegin(GL_TRIANGLES);
        glVertex2f(x0,              cy - (float)icon_sz * 0.5f);
        glVertex2f(x0,              cy + (float)icon_sz * 0.5f);
        glVertex2f(x0 + icon_sz,    cy);
        glEnd();
    }

    /* Line 1 - "Replay  4.0 cmd/s | Polygon" in green; command count
     * is right-aligned so 4-digit totals don't push other fields around. */
    snprintf(progress_txt, sizeof(progress_txt),
             "Replay  %11.1f cmd/s  | %7s  | %s",
             *replay->speed,
             config->replay_mode == REPLAY_MODE_VERTEX ? "Vertex" : "Polygon",
             *replay->expand_args ? "Code Expanded" : ""
             );
    glColor3f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B);
    gl2d_draw_string((float)text_col_x,
                (float)(hud_y + REPLAY_HUD_TEXT_LINE1_Y),
                progress_txt, FONT_SMALL);

    char count_txt[32];
    snprintf(count_txt, sizeof(count_txt), "%d / %d",
             *replay->pc, *replay->total_flat_cmds);
    int count_w = (int)strlen(count_txt) * FONT_SMALL_W;
    gl2d_draw_string((float)(hud_x + hud_w - REPLAY_HUD_TEXT_PAD_X - count_w),
                (float)(hud_y + REPLAY_HUD_TEXT_LINE1_Y),
                count_txt, FONT_SMALL);

    /* Progress groove + green fill */
    int groove_x = hud_x + REPLAY_HUD_TEXT_PAD_X;
    int groove_w = hud_w - 2 * REPLAY_HUD_TEXT_PAD_X;
    int groove_y = hud_y + REPLAY_HUD_PROGRESS_Y;
    glColor4f(0.094f, 0.118f, 0.102f, 1.0f);  /* #181e1a */
    glRectf((float)(groove_x), (float)(groove_y),
              (float)(groove_x) + (float)(groove_w), (float)(groove_y) + (float)(REPLAY_HUD_PROGRESS_H));
    glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, 1.0f);
    glRectf((float)(groove_x), (float)(groove_y),
              (float)(groove_x) + (float)(groove_w * progress), (float)(groove_y) + (float)(REPLAY_HUD_PROGRESS_H));
    glColor4f(0.227f, 0.298f, 0.243f, 1.0f);  /* #3a4c3e border */
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)groove_x + 0.5f,                     (float)groove_y + 0.5f);
    glVertex2f((float)(groove_x + groove_w) - 0.5f,        (float)groove_y + 0.5f);
    glVertex2f((float)(groove_x + groove_w) - 0.5f,        (float)(groove_y + REPLAY_HUD_PROGRESS_H) - 0.5f);
    glVertex2f((float)groove_x + 0.5f,                     (float)(groove_y + REPLAY_HUD_PROGRESS_H) - 0.5f);
    glEnd();

    /* Line 2 - compact kbd hints along the bottom in muted gray */
    snprintf(kbd_txt, sizeof(kbd_txt),
             "Space pause  |  +/- speed  |  m mode  |  e expand |  %c %c step |  Esc stop", 0xAB, 0xBB);
    glColor3f(0.533f, 0.533f, 0.533f);  /* #888 */
    gl2d_draw_string((float)text_col_x,
                (float)(hud_y + REPLAY_HUD_TEXT_LINE2_Y),
                kbd_txt, FONT_SMALL);

    glDisable(GL_BLEND);
    gl2d_end();
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

void scene_render_replay_fade_pass(void) {
    SceneRenderConfig config;
    FlatProgramView flat_program;
    ReplayFadeBatchView fade_batches;
    int skip_limits[REPLAY_FADE_BATCH_MAX];
    int batch_count;

    scene_render_config_init(&config);
    if (!config.replay_has_fades)
        return;

    flat_program = repl_state_flat_program_view();
    fade_batches = repl_replay_fade_batches_view();

    prof_begin(PROF_SCENE_3D_FADE_PROLOGUE);
    batch_count = repl_replay_compute_fade_skip_limits(skip_limits, REPLAY_FADE_BATCH_MAX);
    prof_accum_end(PROF_SCENE_3D_FADE_PROLOGUE);
    if (batch_count <= 0)
        return;

    glPushAttrib(GL_ALL_ATTRIB_BITS);

    scene_lights_setup();
    glDisable(GL_LIGHTING);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    GLfloat mspec[] = { 0.4f, 0.4f, 0.4f, 1.0f };
    GLfloat mshin[] = { 30.0f };
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mspec);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, mshin);
    scene_apply_quality_config(&config);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.70f, 0.70f, 0.80f, 1.0f);
    if (config.wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    else glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    for (int batch_idx = 0; batch_idx < batch_count; batch_idx++) {
        const ReplayFadeBatch *batch = &fade_batches.batches[batch_idx];
        float alpha = repl_replay_batch_alpha(batch);

        if (alpha <= 0.0f)
            continue;

        prof_begin(PROF_SCENE_3D_FADE_BATCH_PREP);
        ReplExecutionOptions exec_options = {
            .flat_cmd_count = batch->new_pc,
            .program = flat_program
        };
        repl_replay_restore_baseline_predef_values();
        repl_execute_set_fade_context(alpha, skip_limits[batch_idx]);
        glColor4f(0.70f, 0.70f, 0.80f, alpha);
        glPushMatrix();
        prof_accum_end(PROF_SCENE_3D_FADE_BATCH_PREP);

        prof_begin(PROF_SCENE_3D_FADE_BATCH_EXEC);
        repl_execute_program(&exec_options);
        prof_accum_end(PROF_SCENE_3D_FADE_BATCH_EXEC);

        glPopMatrix();
    }

    prof_begin(PROF_SCENE_3D_FADE_BATCH_POST);
    repl_execute_set_fade_context(1.0f, 0);
    glPopAttrib();
    prof_accum_end(PROF_SCENE_3D_FADE_BATCH_POST);
}

static void render_3d_scene_pass(void) {
    SceneRenderConfig config;
    FrameRenderContext frame_ctx;
    scene_render_config_init(&config);
    scene_prepare_frame_context(&frame_ctx, &config);

    prof_begin(PROF_SCENE_3D_SETUP);
    glPushAttrib(GL_ALL_ATTRIB_BITS);

    scene_apply_projection(&config);
    scene_apply_camera_view(&config);

    scene_lights_setup();
    glDisable(GL_LIGHTING); /* baseline: disabled; execute_commands() enables if user typed it */

    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    GLfloat mspec[] = { 0.4f, 0.4f, 0.4f, 1.0f };
    GLfloat mshin[] = { 30.0f };
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mspec);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, mshin);

    scene_apply_quality_config(&config);
    scene_apply_wireframe_config(&config);
    prof_accum_end(PROF_SCENE_3D_SETUP);

    FlatProgramView flat_program = repl_state_flat_program_view();
    {
        ReplExecutionOptions exec_options = {
            .flat_cmd_count = flat_program.cmd_count,
            .program = flat_program
        };

        if (config.replay_has_fades)
            exec_options.flat_cmd_count = config.replay_base_limit;

        prof_begin(PROF_SCENE_3D_FILL);
        glPushMatrix();
        repl_execute_program(&exec_options);
        glPopMatrix();
        prof_accum_end(PROF_SCENE_3D_FILL);

        if (config.replay_has_fades) {
            prof_begin(PROF_SCENE_3D_FADE);
            scene_render_replay_fade_pass();
            prof_accum_end(PROF_SCENE_3D_FADE);
        }
    }

    if (config.wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* Draw translucent scene helpers after the main geometry so antialiased
     * edges blend against the final background color rather than the clear
     * color from earlier in the frame. */
    prof_begin(PROF_SCENE_3D_HELPERS);
    scene_backdrop_render();
    scene_grid_render(&frame_ctx);
    scene_axes_render(&frame_ctx);
    draw_orbit_target(&frame_ctx);
    prof_accum_end(PROF_SCENE_3D_HELPERS);

    /* Polygon outline overlay (optional) + current-block highlight.
     * Each overlay pass is wrapped in push/pop so transforms don't bleed
     * between passes.  CMD_TRANSLATE3F is replayed within each pass so
     * outlines are positioned correctly even when transforms separate blocks. */
    prof_begin(PROF_SCENE_3D_OUTLINES);
    scene_overlays_render_outlines(config.show_current_poly,
                                   config.replay_tess_preview);
    prof_accum_end(PROF_SCENE_3D_OUTLINES);

    /* Vertex dots - replay transforms so dots match the filled geometry */
    prof_begin(PROF_SCENE_3D_OVERLAYS);
    glPushMatrix();
    /* Snapshot the pure camera-view matrix (before any user transforms are
     * applied below) so the transform guide can render in world axes at an
     * anchor point, independent of pre-cursor rotations. */
    float tg_cam_view[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, tg_cam_view);
    SceneGuideSnapshot guide_snapshot =
        scene_build_guide_snapshot(&config, flat_program);
    SceneTransformGuidePlan xform_guide_plan;
    scene_transform_guides_prepare(&guide_snapshot, &xform_guide_plan);
    {
    const GLCmd *flat_cmds = flat_program.cmds;
    int flat_cmd_count = flat_program.cmd_count;
    int matrix_depth = 0;
    glPointSize(8.0f);
    if (config.replay_vertex_points)
        glColor3f(1.0f, 0.88f, 0.20f);
    else
        glColor3f(0.0f, 0.0f, 0.0f);

    for (int i = 0; i < flat_cmd_count; i++) {
        if (!flat_cmds[i].valid) continue;
        int is_cursor = (flat_cmds[i].src_cmd_idx == guide_snapshot.edit_line_idx);

        // Draw vertex guides and normals if this vertex is at the cursor
        // position. Do this inline so that the current model matrix is applied
        // to the guides, ensuring they are positioned correctly even when
        // transforms separate blocks.
        if (is_cursor && !config.replaying) {
            scene_geometry_guides_render_for_cursor(&guide_snapshot);
        }

        scene_transform_guides_render_if_due(&guide_snapshot,
                                             &xform_guide_plan,
                                             i, tg_cam_view);

        if (is_transform_cmd(flat_cmds[i].type)) {
            repl_executor_apply_tracked_transform_cmd(&flat_cmds[i], &matrix_depth);
        } else if ((config.show_vpoints || config.replay_vertex_points) &&
                   (flat_cmds[i].type == CMD_VERTEX3F ||
                    flat_cmds[i].type == CMD_TESS_VERTEX)) {
            glBegin(GL_POINTS);
            glVertex3f(flat_cmds[i].args[0], flat_cmds[i].args[1],
                       flat_cmds[i].args[2]);
            glEnd();
        }
    }
    glPointSize(1.0f);
    repl_executor_unwind_tracked_transform_stack(&matrix_depth);
    }
    glPopMatrix();
    glDisable(GL_BLEND);
    if (repl_state_flat_program_user_lighting_enabled()) glEnable(GL_LIGHTING);
    prof_accum_end(PROF_SCENE_3D_OVERLAYS);

    prof_begin(PROF_SCENE_3D_HUD);
    if (config.replay_tess_preview)
        draw_replay_tess_preview(&config);

    scene_lights_render();

    if (config.show_vnums)   scene_overlays_render_vertex_numbers();
    if (config.show_normals) scene_overlays_render_normal_vectors();
    if (config.replaying)
        draw_replay_hud(&config);
    glPopAttrib();
    prof_accum_end(PROF_SCENE_3D_HUD);
}

void scene_render_3d_scene(void) {
    SceneRenderConfig viewport_config;
    const ReplRenderState *render = repl_state_render();
    ReplRenderState *render_mut = repl_state_render_mut();
    const ReplReplayRuntimeState *replay = repl_state_replay();
    FlatProgramView flat_program = repl_state_flat_program_view();

    scene_render_config_init(&viewport_config);
    glViewport(viewport_config.scene_x, viewport_config.scene_y,
               viewport_config.scene_w, viewport_config.scene_h);
    scene_apply_clear_color(flat_program);

    if (*render->use_accum && *render->accum_aa_enabled && *render->accum_samples > 1) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_ACCUM_BUFFER_BIT);
        float weight = 1.0f / (float)*render->accum_samples;
        for (int sample_idx = 0; sample_idx < *render->accum_samples; sample_idx++) {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (*replay->active)
                repl_replay_restore_baseline_predef_values();
            *render_mut->accum_jitter_x = g_jitter_table[sample_idx % MAX_ACCUM_SAMPLES][0];
            *render_mut->accum_jitter_y = g_jitter_table[sample_idx % MAX_ACCUM_SAMPLES][1];
            render_3d_scene_pass();
            glAccum(GL_ACCUM, weight);
        }
        *render_mut->accum_jitter_x = 0.0f;
        *render_mut->accum_jitter_y = 0.0f;
        glAccum(GL_RETURN, 1.0f);
    } else {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (*replay->active)
            repl_replay_restore_baseline_predef_values();
        render_3d_scene_pass();
    }
}
