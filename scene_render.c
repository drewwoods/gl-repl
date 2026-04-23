/*
 * scene_render.c — 3D scene rendering (frame prep, edit guides, replay HUD)
 *
 * Extracted from sample.c for maintainability.
 */
#include "sample.h"
#include "repl_core.h"
#include "scene_axes.h"
#include "scene_backdrop.h"
#include "scene_grid.h"
#include "scene_lights.h"
#include "scene_overlays.h"
#include "scene_render_types.h"
#include "scene_render.h"
#include "ui_panels.h"
#include "ui_profile_panel.h"

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
    g_focus_vtx[0] = x;
    g_focus_vtx[1] = y;
    g_focus_vtx[2] = z;
    g_focus_vtx_valid = 1;
}

static SceneFocusVertex scene_prepare_focus_vertex(void) {
    SceneFocusVertex focus = {
        .pos = { g_focus_vtx[0], g_focus_vtx[1], g_focus_vtx[2] },
        .valid = g_focus_vtx_valid,
    };

    if (g_edit_line >= 0 && g_edit_line < g_num_cmds &&
        cmd_is_focus_vertex(&g_cmds[g_edit_line])) {
        scene_focus_store(g_cmds[g_edit_line].args[0],
                          g_cmds[g_edit_line].args[1],
                          g_cmds[g_edit_line].args[2]);
    } else if (!g_focus_vtx_valid) {
        for (int i = g_edit_line - 1; i >= 0; i--) {
            if (cmd_is_focus_vertex(&g_cmds[i])) {
                scene_focus_store(g_cmds[i].args[0],
                                  g_cmds[i].args[1],
                                  g_cmds[i].args[2]);
                break;
            }
        }
    }

    focus.pos[0] = g_focus_vtx[0];
    focus.pos[1] = g_focus_vtx[1];
    focus.pos[2] = g_focus_vtx[2];
    focus.valid = g_focus_vtx_valid;
    return focus;
}

static void scene_render_config_init(SceneRenderConfig *config) {
    scene_rect(&config->scene_x, &config->scene_y,
               &config->scene_w, &config->scene_h);
    if (config->scene_w < 1) config->scene_w = 1;
    if (config->scene_h < 1) config->scene_h = 1;

    config->cam_dist = g_cam_dist;
    config->cam_rx = g_cam_rx;
    config->cam_ry = g_cam_ry;
    config->cam_tx = g_cam_tx;
    config->cam_ty = g_cam_ty;
    config->cam_tz = g_cam_tz;
    config->cam_motion_glow = g_cam_motion_glow;
    config->accum_jitter_x = g_accum_jitter_x;
    config->accum_jitter_y = g_accum_jitter_y;
    config->multisample_enabled = g_multisample_enabled;
    config->line_smooth_enabled = g_line_smooth_enabled;
    config->wireframe = g_wireframe;
    config->grid_theme = g_grid_theme;
    config->grid_extent_idx = g_grid_extent_idx;
    config->grid_major_idx = g_grid_major_idx;
    config->axes_theme = g_axes_theme;
    config->show_guides = g_show_guides;
    config->show_vpoints = g_show_vpoints;
    config->show_vnums = g_show_vnums;
    config->show_normals = g_show_normals;
    config->replaying = g_replay_active;
    config->replay_mode = g_replay_mode;
    config->replay_tess_preview = config->replaying &&
                                  config->replay_mode == REPLAY_MODE_VERTEX;
    config->replay_vertex_points = config->replay_tess_preview;
    config->replay_has_fades = replay_has_active_fades();
    config->replay_fill_base_limit = config->replay_has_fades
                                   ? replay_fill_base_limit()
                                   : 0;
    config->show_current_poly = g_highlight_current_poly && !config->replaying;
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
    ctx->config = *config;
    ctx->focus.pos[0] = g_focus_vtx[0];
    ctx->focus.pos[1] = g_focus_vtx[1];
    ctx->focus.pos[2] = g_focus_vtx[2];
    ctx->focus.valid = g_focus_vtx_valid;

    /* Modelview setup is T(-dist) * Rx * Ry * T(-target). Solving that for
     * the eye position gives target.y + sin(rx) * dist; ry does not affect Y. */
    float camera_rx_rad = config->cam_rx * (float)M_PI / 180.0f;
    ctx->camera_world_y = config->cam_ty +
                          sinf(camera_rx_rad) * config->cam_dist;
    ctx->camera_below_water_surface = (ctx->camera_world_y < 0.0f);

    if (config->grid_theme == GRID_THEME_FOCUS)
        ctx->focus = scene_prepare_focus_vertex();
}

/* ========================================================================= */
/* Vertex input guides — plane / line / point for partial glVertex3f args     */
/* ========================================================================= */

/* Draw a semi-transparent plane perpendicular to the X axis at x=v (red) */
static void draw_guide_yz_plane(float v, float sz) {
    glColor4f(0.90f, 0.65f, 0.60f, 0.72f);
    glBegin(GL_QUADS);
    glVertex3f(v, -sz, -sz); glVertex3f(v,  sz, -sz);
    glVertex3f(v,  sz,  sz); glVertex3f(v, -sz,  sz);
    glEnd();
    glColor4f(0.90f, 0.30f, 0.30f, 0.45f);
    glBegin(GL_LINE_LOOP);
    glVertex3f(v, -sz, -sz); glVertex3f(v,  sz, -sz);
    glVertex3f(v,  sz,  sz); glVertex3f(v, -sz,  sz);
    glEnd();
}

/* Draw a semi-transparent plane perpendicular to the Y axis at y=v (green) */
static void draw_guide_xz_plane(float v, float sz) {
    glColor4f(0.65f, 0.90f, 0.60f, 0.72f);
    glBegin(GL_QUADS);
    glVertex3f(-sz, v, -sz); glVertex3f( sz, v, -sz);
    glVertex3f( sz, v,  sz); glVertex3f(-sz, v,  sz);
    glEnd();
    glColor4f(0.30f, 0.70f, 0.30f, 0.45f);
    glBegin(GL_LINE_LOOP);
    glVertex3f(-sz, v, -sz); glVertex3f( sz, v, -sz);
    glVertex3f( sz, v,  sz); glVertex3f(-sz, v,  sz);
    glEnd();
}

/* Draw a semi-transparent plane perpendicular to the Z axis at z=v (blue) */
static void draw_guide_xy_plane(float v, float sz) {
    glColor4f(0.60f, 0.65f, 0.90f, 0.72f);
    glBegin(GL_QUADS);
    glVertex3f(-sz, -sz, v); glVertex3f( sz, -sz, v);
    glVertex3f( sz,  sz, v); glVertex3f(-sz,  sz, v);
    glEnd();
    glColor4f(0.30f, 0.30f, 0.80f, 0.45f);
    glBegin(GL_LINE_LOOP);
    glVertex3f(-sz, -sz, v); glVertex3f( sz, -sz, v);
    glVertex3f( sz,  sz, v); glVertex3f(-sz,  sz, v);
    glEnd();
}

/* Parse up to 3 comma-separated glVertex argument slots, recording which
 * positions are actually present.  parse_exprs() skips leading commas so
 * ",1," and "1," look identical to it; this version tracks slot indices.
 * Empty slots (e.g. x in ",1,") leave filled[i]=0, out[i] unchanged. */
static int parse_vertex_slots(const char *s, float out[3], int filled[3]) {
    filled[0] = filled[1] = filled[2] = 0;
    int n_filled = 0;
    for (int slot = 0; slot < 3; slot++) {
        while (*s == ' ' || *s == '\t') s++;
        const char *start = s;
        while (*s && *s != ',' && *s != ')') s++;
        const char *end = s;
        /* check for non-whitespace content */
        const char *c = start;
        while (c < end && (*c == ' ' || *c == '\t')) c++;
        if (c < end) {
            ExprCtx ctx = { start, g_predef_vars, g_num_predef_vars };
            float val = eval_expr(&ctx);
            if (ctx.p > start) {
                out[slot] = val;
                filled[slot] = 1;
                n_filled++;
            }
        }
        if (*s == ',') s++;
        else break;
    }
    return n_filled;
}

static void draw_vertex_guides(const SceneRenderConfig *config) {
    if (!config->show_guides) return;

    /* Check current input for a partial glVertex3f( or gluVertex( */
    const char *args_str = NULL;
    if (strncmp(g_input, "glVertex3f(", 11) == 0 && g_input_len > 11)
        args_str = g_input + 11;
    else if (strncmp(g_input, "gluVertex(", 10) == 0 && g_input_len > 10)
        args_str = g_input + 10;
    if (!args_str) return;

    float vals[3] = {0};
    int filled[3];
    int n = parse_vertex_slots(args_str, vals, filled);
    if (n < 1) return;

    float sz = 3.0f;  /* half-size of guide geometry */

    scene_render_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (n == 1) {
        /* One axis known — draw the plane perpendicular to that axis */
        if      (filled[0]) draw_guide_yz_plane(vals[0], sz);  /* x → YZ plane */
        else if (filled[1]) draw_guide_xz_plane(vals[1], sz);  /* y → XZ plane */
        else if (filled[2]) draw_guide_xy_plane(vals[2], sz);  /* z → XY plane */
    } else if (n == 2) {
        /* Two axes known — draw the line parallel to the missing axis */
        glLineWidth(2.0f);
        if (!filled[2]) {
            /* x,y known → line || Z  (yellow) */
            glColor4f(1.0f, 0.80f, 0.20f, 0.9f);
            glBegin(GL_LINES);
            glVertex3f(vals[0], vals[1], -sz);
            glVertex3f(vals[0], vals[1],  sz);
            glEnd();
        } else if (!filled[1]) {
            /* x,z known → line || Y  (green) */
            glColor4f(0.30f, 0.90f, 0.30f, 0.9f);
            glBegin(GL_LINES);
            glVertex3f(vals[0], -sz, vals[2]);
            glVertex3f(vals[0],  sz, vals[2]);
            glEnd();
        } else {
            /* y,z known → line || X  (red) */
            glColor4f(1.0f, 0.35f, 0.35f, 0.9f);
            glBegin(GL_LINES);
            glVertex3f(-sz, vals[1], vals[2]);
            glVertex3f( sz, vals[1], vals[2]);
            glEnd();
        }
        glLineWidth(1.0f);
    } else {
        /* All three values known — draw a point */
        int depth = glIsEnabled(GL_DEPTH_TEST);
        if (depth) glDisable(GL_DEPTH_TEST);
        glColor4f(1.0f, 0.3f, 0.3f, 0.9f);
        glPointSize(8.0f);
        glBegin(GL_POINTS);
        glVertex3f(vals[0], vals[1], vals[2]);
        glEnd();
        glPointSize(1.0f);
        if (depth) glEnable(GL_DEPTH_TEST);
    }

    glDisable(GL_BLEND);
    if (g_user_lighting_enabled) glEnable(GL_LIGHTING);
    scene_render_pop_state();
}

/* ========================================================================= */
/* Normal edit guides — show doubled/halved component impact                  */
/* ========================================================================= */

static void draw_normal_guides(const SceneRenderConfig *config) {
    if (!config->show_guides) return;

    /* Check current input for glNormal3f( or gluNormal( */
    const char *args_str = NULL;
    int paren_pos = 0;
    if (strncmp(g_input, "glNormal3f(", 11) == 0 && g_input_len > 11)
        { args_str = g_input + 11; paren_pos = 11; }
    else if (strncmp(g_input, "gluNormal(", 10) == 0 && g_input_len > 10)
        { args_str = g_input + 10; paren_pos = 10; }
    if (!args_str) return;

    float vals[3];
    int n = parse_exprs(args_str, vals, 3, NULL, 0);
    if (n < 3) return;
    if (g_cursor_pos < paren_pos) return;
    int close = g_input_len;
    for (int ci = paren_pos; ci < g_input_len; ci++)
        if (g_input[ci] == ')') { close = ci; break; }
    /* Allow cursor past ')' — navigating to an existing normal line places the
     * cursor at end-of-string; still show the guide for the full expression. */
    int effective_cursor = (g_cursor_pos > close) ? close : g_cursor_pos;

    int component = 0;
    for (int ci = paren_pos; ci < effective_cursor; ci++)
        if (g_input[ci] == ',') component++;
    if (component > 2) component = 2;

    /* Find the associated vertex — next CMD_VERTEX3F or CMD_TESS_VERTEX */
    int search_start = (g_edit_line < g_num_cmds && !g_inserting)
                      ? g_edit_line + 1 : g_edit_line;
    float vx = 0, vy = 0, vz = 0;
    int found = 0;
    for (int i = search_start; i < g_num_cmds; i++) {
        if (!g_cmds[i].valid) continue;
        if (g_cmds[i].type == CMD_VERTEX3F ||
            g_cmds[i].type == CMD_TESS_VERTEX) {
            vx = g_cmds[i].args[0];
            vy = g_cmds[i].args[1];
            vz = g_cmds[i].args[2];
            found = 1;
            break;
        }
        if (g_cmds[i].type == CMD_END   || g_cmds[i].type == CMD_BEGIN ||
            g_cmds[i].type == CMD_TESS_END ||
            g_cmds[i].type == CMD_TESS_BEGIN_POLYGON) break;
    }
    if (!found) return;

    /* Compute doubled and halved normals (re-normalized) */
    float doubled[3] = { vals[0], vals[1], vals[2] };
    float halved[3]  = { vals[0], vals[1], vals[2] };
    doubled[component] *= 2.0f;
    halved[component]  *= 0.5f;

    float dlen = sqrtf(doubled[0]*doubled[0] + doubled[1]*doubled[1]
                      + doubled[2]*doubled[2]);
    if (dlen > 1e-8f) { doubled[0]/=dlen; doubled[1]/=dlen; doubled[2]/=dlen; }
    float hlen = sqrtf(halved[0]*halved[0] + halved[1]*halved[1]
                      + halved[2]*halved[2]);
    if (hlen > 1e-8f) { halved[0]/=hlen; halved[1]/=hlen; halved[2]/=hlen; }

    float scale = 0.45f;

    scene_render_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Current normal — thin white reference line */
    float clen = sqrtf(vals[0]*vals[0] + vals[1]*vals[1] + vals[2]*vals[2]);
    if (clen > 1e-8f) {
        float cn[3] = { vals[0]/clen, vals[1]/clen, vals[2]/clen };
        glColor4f(0.8f, 0.8f, 0.8f, 0.4f);
        glLineWidth(3.0f);
        glBegin(GL_LINES);
        glVertex3f(vx, vy, vz);
        glVertex3f(vx + cn[0]*scale, vy + cn[1]*scale, vz + cn[2]*scale);
        glEnd();
    }

    /* Doubled component — green stippled arrow */
    glEnable(GL_LINE_STIPPLE);
    glLineStipple(1, 0xAAAA);
    glLineWidth(4.0f);

    glColor4f(0.2f, 0.95f, 0.2f, 0.75f);
    glBegin(GL_LINES);
    glVertex3f(vx, vy, vz);
    glVertex3f(vx + doubled[0]*scale, vy + doubled[1]*scale,
               vz + doubled[2]*scale);
    glEnd();

    /* Halved component — red stippled arrow */
    glColor4f(0.95f, 0.2f, 0.2f, 0.75f);
    glBegin(GL_LINES);
    glVertex3f(vx, vy, vz);
    glVertex3f(vx + halved[0]*scale, vy + halved[1]*scale,
               vz + halved[2]*scale);
    glEnd();

    glDisable(GL_LINE_STIPPLE);
    glLineWidth(1.0f);

    /* Dots at arrow tips */
    glPointSize(8.0f);
    glBegin(GL_POINTS);
    glColor4f(0.2f, 0.95f, 0.2f, 0.85f);
    glVertex3f(vx + doubled[0]*scale, vy + doubled[1]*scale,
               vz + doubled[2]*scale);
    glColor4f(0.95f, 0.2f, 0.2f, 0.85f);
    glVertex3f(vx + halved[0]*scale, vy + halved[1]*scale,
               vz + halved[2]*scale);
    glEnd();
    glPointSize(1.0f);

    glDisable(GL_BLEND);
    if (g_user_lighting_enabled) glEnable(GL_LIGHTING);
    scene_render_pop_state();
}

/* ========================================================================= */
/* Transform edit guides — arrow for glTranslatef, arc for glRotatef          */
/* ========================================================================= */

/* Cmds that emit geometry — hitting one of these means "something just got
 * drawn with the current modelview", so further transforms shouldn't factor
 * into the cursor-line's guide. */
static int is_geometry_emit_cmd(CmdType t) {
    return (t == CMD_BEGIN ||
            t == CMD_GLU_SPHERE || t == CMD_GLU_CYLINDER ||
            t == CMD_GLU_DISK   || t == CMD_GLU_PARTIAL_DISK ||
            t == CMD_GLUT_TORUS ||
            t == CMD_TESS_BEGIN_POLYGON);
}

static void mat4_mul_col_major(const float a[16], const float b[16], float out[16]) {
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            out[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
}

/* Walk flat cmds strictly before cursor_flat_idx, applying only transform
 * cmds to a fresh identity matrix. Returns the full scene-world frame at the
 * cursor line, so Frame guide mode can account for prior rotations as well as
 * translations. */
static void compute_before_cursor_matrix(int cursor_flat_idx, float out[16]) {
    glPushMatrix();
    glLoadIdentity();
    int depth = 0;
    for (int i = 0; i < cursor_flat_idx; i++) {
        if (!g_flat_cmds[i].valid) continue;
        /* Apply the full pre-cursor modelview — translations, rotations,
         * and scales — so the anchor lines up with where earlier blocks
         * (func0(), glBegin...) actually rendered. A "translations only"
         * anchor looks right when the scene is translation-only, but once
         * a pre-cursor rotation is in play it diverges from where geometry
         * actually drew, which is the opposite of what Frame mode is for. */
        if (is_transform_cmd(g_flat_cmds[i].type))
            apply_tracked_transform_cmd(&g_flat_cmds[i], &depth);
    }
    glGetFloatv(GL_MODELVIEW_MATRIX, out);
    unwind_tracked_transform_stack(&depth);
    glPopMatrix();
}

/* Walk flat cmds forward from first_after_idx, applying only transform cmds
 * to a fresh identity matrix (keeps its own push/pop depth so nested blocks
 * don't leak). Stops at the first rendering action so transforms that come
 * after an intervening draw don't bleed into the guide. Returns the origin
 * transformed by the accumulated matrix. */
static void compute_after_cursor_origin(int first_after_idx, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    glPushMatrix();
    glLoadIdentity();
    int depth = 0;
    for (int i = first_after_idx; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        if (is_geometry_emit_cmd(g_flat_cmds[i].type)) break;
        if (is_transform_cmd(g_flat_cmds[i].type))
            apply_tracked_transform_cmd(&g_flat_cmds[i], &depth);
    }
    float m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    out[0] = m[12];
    out[1] = m[13];
    out[2] = m[14];
    unwind_tracked_transform_stack(&depth);
    glPopMatrix();
}

/* Color from the normalized absolute component values of a vector. Maps
 * (|x|,|y|,|z|) / max to RGB so an axis-aligned translation reads as a pure
 * axis color (X=red, Y=green, Z=blue) and diagonals blend. Falls back to a
 * neutral gray for zero vectors. */
static void xform_axis_color(float x, float y, float z, float out[3]) {
    float ax = fabsf(x), ay = fabsf(y), az = fabsf(z);
    float m = ax; if (ay > m) m = ay; if (az > m) m = az;
    if (m < 1e-6f) { out[0] = out[1] = out[2] = 0.85f; return; }
    out[0] = ax / m;
    out[1] = ay / m;
    out[2] = az / m;
}

/* Pulse shader for a straight segment in the axes-pulse style: a dim solid
 * base line, a bright dot traveling a→b, and a short trail behind the dot. */
static void draw_pulse_segment(const float a[3], const float b[3],
                               const float rgb[3]) {
    /* Dim solid base */
    glLineWidth(2.0f);
    glBegin(GL_LINES);
    glColor4f(rgb[0], rgb[1], rgb[2], 0.30f);
    glVertex3f(a[0], a[1], a[2]);
    glVertex3f(b[0], b[1], b[2]);
    glEnd();

    float ph = fmodf(g_anim_time * 0.6f, 1.0f);
    float glow = sinf(ph * (float)M_PI) * 0.8f + 0.2f;
    float pos[3] = {
        a[0] + (b[0]-a[0])*ph, a[1] + (b[1]-a[1])*ph, a[2] + (b[2]-a[2])*ph
    };
    float tp = ph - 0.25f; if (tp < 0) tp = 0;
    float trail[3] = {
        a[0] + (b[0]-a[0])*tp, a[1] + (b[1]-a[1])*tp, a[2] + (b[2]-a[2])*tp
    };

    glLineWidth(3.5f);
    glBegin(GL_LINES);
    glColor4f(rgb[0], rgb[1], rgb[2], 0.05f);
    glVertex3f(trail[0], trail[1], trail[2]);
    glColor4f(rgb[0], rgb[1], rgb[2], glow * 0.75f);
    glVertex3f(pos[0], pos[1], pos[2]);
    glEnd();
    glLineWidth(1.0f);

    glPointSize(8.0f);
    glBegin(GL_POINTS);
    glColor4f(rgb[0], rgb[1], rgb[2], glow);
    glVertex3f(pos[0], pos[1], pos[2]);
    glEnd();
    glPointSize(1.0f);
}

/* Arrow starting at p_after in the current local frame and extending by the
 * translate command's vector. Shaft is an axes-pulse-style traveling dot
 * over a dim base line; the solid 4-fin arrowhead at the tip keeps the
 * direction unambiguous. Shaft color is (|tx|,|ty|,|tz|)/max mapped to RGB. */
static void draw_translate_guide(const GLCmd *cmd, const float p_after[3]) {
    float tx = cmd->args[0], ty = cmd->args[1], tz = cmd->args[2];
    float p0[3] = { p_after[0], p_after[1], p_after[2] };
    float p1[3] = { p0[0] + tx, p0[1] + ty, p0[2] + tz };

    float dlen = sqrtf(tx*tx + ty*ty + tz*tz);
    if (dlen < 1e-6f) return;
    float dx = tx/dlen, dy = ty/dlen, dz = tz/dlen;

    /* Orthonormal basis perpendicular to the arrow direction, used to splay
     * the four arrowhead fins. */
    float ux, uy, uz;
    if (fabsf(dy) < 0.9f) { ux = 0.0f; uy = 1.0f; uz = 0.0f; }
    else                  { ux = 1.0f; uy = 0.0f; uz = 0.0f; }
    float rx = dy*uz - dz*uy;
    float ry = dz*ux - dx*uz;
    float rz = dx*uy - dy*ux;
    float rlen = sqrtf(rx*rx + ry*ry + rz*rz);
    if (rlen > 1e-8f) { rx/=rlen; ry/=rlen; rz/=rlen; }
    float bx = ry*dz - rz*dy;
    float by = rz*dx - rx*dz;
    float bz = rx*dy - ry*dx;

    float head_len = dlen * 0.22f;
    if (head_len > 0.25f) head_len = 0.25f;
    if (head_len < 0.06f) head_len = (dlen < 0.06f ? dlen * 0.5f : 0.06f);
    float fin = head_len * 0.45f;

    float base[3] = { p1[0] - dx*head_len, p1[1] - dy*head_len, p1[2] - dz*head_len };

    float rgb[3]; xform_axis_color(tx, ty, tz, rgb);
    float head_rgb[3] = {
        rgb[0]*0.6f + 0.4f, rgb[1]*0.6f + 0.4f, rgb[2]*0.6f + 0.4f
    };

    scene_render_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    draw_pulse_segment(p0, base, rgb);

    /* Arrowhead — four fins from the tip back to the base ring. */
    glLineWidth(3.0f);
    glColor4f(head_rgb[0], head_rgb[1], head_rgb[2], 0.95f);
    glBegin(GL_LINES);
    for (int i = 0; i < 4; i++) {
        float sx = (i == 0 ?  rx : i == 1 ? -rx : i == 2 ?  bx : -bx);
        float sy = (i == 0 ?  ry : i == 1 ? -ry : i == 2 ?  by : -by);
        float sz = (i == 0 ?  rz : i == 1 ? -rz : i == 2 ?  bz : -bz);
        glVertex3f(p1[0], p1[1], p1[2]);
        glVertex3f(base[0] + sx*fin, base[1] + sy*fin, base[2] + sz*fin);
    }
    /* Connect the fins around the base so the head reads as a pyramid. */
    glVertex3f(base[0] + rx*fin, base[1] + ry*fin, base[2] + rz*fin);
    glVertex3f(base[0] + bx*fin, base[1] + by*fin, base[2] + bz*fin);
    glVertex3f(base[0] + bx*fin, base[1] + by*fin, base[2] + bz*fin);
    glVertex3f(base[0] - rx*fin, base[1] - ry*fin, base[2] - rz*fin);
    glVertex3f(base[0] - rx*fin, base[1] - ry*fin, base[2] - rz*fin);
    glVertex3f(base[0] - bx*fin, base[1] - by*fin, base[2] - bz*fin);
    glVertex3f(base[0] - bx*fin, base[1] - by*fin, base[2] - bz*fin);
    glVertex3f(base[0] + rx*fin, base[1] + ry*fin, base[2] + rz*fin);
    glEnd();
    glLineWidth(1.0f);

    /* Tail dot and tip dot. */
    glPointSize(6.0f);
    glBegin(GL_POINTS);
    glColor4f(rgb[0], rgb[1], rgb[2], 0.7f);
    glVertex3f(p0[0], p0[1], p0[2]);
    glEnd();
    glPointSize(9.0f);
    glBegin(GL_POINTS);
    glColor4f(head_rgb[0], head_rgb[1], head_rgb[2], 1.0f);
    glVertex3f(p1[0], p1[1], p1[2]);
    glEnd();
    glPointSize(1.0f);

    glDisable(GL_BLEND);
    if (g_user_lighting_enabled) glEnable(GL_LIGHTING);
    scene_render_pop_state();
}

/* Arc from p_start swept by glRotatef(angle, ax,ay,az) about local origin. */
/* Helper: small 4-fin arrowhead from `tip` pointing along `dir` (unit). */
static void draw_arrow_head(const float tip[3], const float dir[3], float head_len) {
    float ux, uy, uz;
    if (fabsf(dir[1]) < 0.9f) { ux = 0.0f; uy = 1.0f; uz = 0.0f; }
    else                      { ux = 1.0f; uy = 0.0f; uz = 0.0f; }
    float rx = dir[1]*uz - dir[2]*uy;
    float ry = dir[2]*ux - dir[0]*uz;
    float rz = dir[0]*uy - dir[1]*ux;
    float rlen = sqrtf(rx*rx + ry*ry + rz*rz);
    if (rlen > 1e-8f) { rx/=rlen; ry/=rlen; rz/=rlen; }
    float bx = ry*dir[2] - rz*dir[1];
    float by = rz*dir[0] - rx*dir[2];
    float bz = rx*dir[1] - ry*dir[0];
    float base[3] = { tip[0] - dir[0]*head_len, tip[1] - dir[1]*head_len, tip[2] - dir[2]*head_len };
    float fin = head_len * 0.45f;
    glBegin(GL_LINES);
    for (int k = 0; k < 4; k++) {
        float sx = (k == 0 ?  rx : k == 1 ? -rx : k == 2 ?  bx : -bx);
        float sy = (k == 0 ?  ry : k == 1 ? -ry : k == 2 ?  by : -by);
        float sz = (k == 0 ?  rz : k == 1 ? -rz : k == 2 ?  bz : -bz);
        glVertex3f(tip[0], tip[1], tip[2]);
        glVertex3f(base[0] + sx*fin, base[1] + sy*fin, base[2] + sz*fin);
    }
    glEnd();
}

/* Scale guide. Draws the identity reference (origin → axis·1, gray) with a
 * bright tick at the 1.0 position on each axis, then a pulse arrow showing
 * only the distortion — from the 1.0 tick to the scaled tip. Scale of 1 on
 * an axis draws no pulse (identity); negative factors produce an arrow that
 * passes through origin. The non-origin branch is the World-mode variant
 * where p_start is the actual anchor point and the 1.0 reference is p_start
 * itself; at exact scale (1,1,1) only the marker is drawn. */
static void draw_scale_guide(const GLCmd *cmd, const float p_start[3]) {
    float sx = cmd->args[0], sy = cmd->args[1], sz = cmd->args[2];
    float p0[3] = { p_start[0], p_start[1], p_start[2] };
    float plen = sqrtf(p0[0]*p0[0] + p0[1]*p0[1] + p0[2]*p0[2]);

    scene_render_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (plen > 1e-4f) {
        /* Non-origin anchor: 1.0 position is p_start itself. Draw a
         * 3-axis cross marker there, then a pulse arrow for the distortion
         * p_start → p_start·(sx,sy,sz). */
        float p1[3] = { p0[0]*sx, p0[1]*sy, p0[2]*sz };
        float delta[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
        float dlen = sqrtf(delta[0]*delta[0] + delta[1]*delta[1] + delta[2]*delta[2]);

        float tick = 0.08f;
        glLineWidth(2.0f);
        glColor4f(0.9f, 0.9f, 0.9f, 0.9f);
        glBegin(GL_LINES);
        glVertex3f(p0[0]-tick, p0[1], p0[2]); glVertex3f(p0[0]+tick, p0[1], p0[2]);
        glVertex3f(p0[0], p0[1]-tick, p0[2]); glVertex3f(p0[0], p0[1]+tick, p0[2]);
        glVertex3f(p0[0], p0[1], p0[2]-tick); glVertex3f(p0[0], p0[1], p0[2]+tick);
        glEnd();
        glPointSize(6.0f);
        glBegin(GL_POINTS);
        glColor4f(0.95f, 0.95f, 0.95f, 1.0f);
        glVertex3f(p0[0], p0[1], p0[2]);
        glEnd();
        glPointSize(1.0f);

        if (dlen > 1e-4f) {
            float rgb[3]; xform_axis_color(sx - 1.0f, sy - 1.0f, sz - 1.0f, rgb);
            float head_rgb[3] = {
                rgb[0]*0.6f + 0.4f, rgb[1]*0.6f + 0.4f, rgb[2]*0.6f + 0.4f
            };
            float dir[3] = { delta[0]/dlen, delta[1]/dlen, delta[2]/dlen };
            float head_len = dlen * 0.22f;
            if (head_len > 0.25f) head_len = 0.25f;
            if (head_len < 0.06f) head_len = (dlen < 0.06f ? dlen * 0.5f : 0.06f);
            float base[3] = { p1[0] - dir[0]*head_len, p1[1] - dir[1]*head_len, p1[2] - dir[2]*head_len };

            draw_pulse_segment(p0, base, rgb);

            glLineWidth(3.0f);
            glColor4f(head_rgb[0], head_rgb[1], head_rgb[2], 0.95f);
            draw_arrow_head(p1, dir, head_len);
            glLineWidth(1.0f);

            glPointSize(9.0f);
            glBegin(GL_POINTS);
            glColor4f(head_rgb[0], head_rgb[1], head_rgb[2], 1.0f);
            glVertex3f(p1[0], p1[1], p1[2]);
            glEnd();
            glPointSize(1.0f);
        }
    } else {
        /* Origin anchor — per-axis view. For each axis: gray identity segment
         * (origin → axis·1), a perpendicular cross tick at axis·1 marking
         * the 1.0 position, then (only when factor != 1) a pulse arrow from
         * the 1.0 tick to axis·f showing just the distortion delta. */
        const float axes[3][3] = { {1,0,0}, {0,1,0}, {0,0,1} };
        /* Two axes perpendicular to axis a, used to draw the 1.0 tick cross. */
        const int perp_a[3] = { 1, 0, 0 };
        const int perp_b[3] = { 2, 2, 1 };
        const float axis_rgb[3][3] = {
            {1.0f, 0.3f, 0.3f}, {0.3f, 1.0f, 0.3f}, {0.3f, 0.3f, 1.0f}
        };
        const float factors[3] = { sx, sy, sz };
        const float tick = 0.06f;
        for (int a = 0; a < 3; a++) {
            float f = factors[a];
            const float *ax = axes[a];
            const float *pa = axes[perp_a[a]];
            const float *pb = axes[perp_b[a]];

            /* Identity reference — thin gray segment from origin to unit tip. */
            glLineWidth(1.5f);
            glColor4f(0.55f, 0.55f, 0.55f, 0.45f);
            glBegin(GL_LINES);
            glVertex3f(0.0f, 0.0f, 0.0f);
            glVertex3f(ax[0], ax[1], ax[2]);
            glEnd();

            /* 1.0 tick — bright cross perpendicular to the axis. */
            glLineWidth(2.0f);
            glColor4f(0.9f, 0.9f, 0.9f, 0.9f);
            glBegin(GL_LINES);
            glVertex3f(ax[0] - pa[0]*tick, ax[1] - pa[1]*tick, ax[2] - pa[2]*tick);
            glVertex3f(ax[0] + pa[0]*tick, ax[1] + pa[1]*tick, ax[2] + pa[2]*tick);
            glVertex3f(ax[0] - pb[0]*tick, ax[1] - pb[1]*tick, ax[2] - pb[2]*tick);
            glVertex3f(ax[0] + pb[0]*tick, ax[1] + pb[1]*tick, ax[2] + pb[2]*tick);
            glEnd();
            glPointSize(5.0f);
            glBegin(GL_POINTS);
            glColor4f(0.95f, 0.95f, 0.95f, 1.0f);
            glVertex3f(ax[0], ax[1], ax[2]);
            glEnd();
            glPointSize(1.0f);

            /* Identity on this axis — no distortion to draw. */
            if (fabsf(f - 1.0f) < 1e-4f) continue;

            /* Pulse shows only the distortion: 1.0 tick → axis·f. Negative
             * f produces an arrow that passes through the origin; length
             * scales with |f - 1|. */
            float tip[3] = { ax[0]*f, ax[1]*f, ax[2]*f };
            float from[3] = { ax[0], ax[1], ax[2] };
            float delta[3] = { tip[0]-from[0], tip[1]-from[1], tip[2]-from[2] };
            float dlen = sqrtf(delta[0]*delta[0] + delta[1]*delta[1] + delta[2]*delta[2]);
            float dir[3] = { delta[0]/dlen, delta[1]/dlen, delta[2]/dlen };

            float head_len = dlen * 0.22f;
            if (head_len > 0.2f) head_len = 0.2f;
            if (head_len < 0.05f) head_len = (dlen < 0.05f ? dlen * 0.5f : 0.05f);
            float base[3] = { tip[0] - dir[0]*head_len, tip[1] - dir[1]*head_len, tip[2] - dir[2]*head_len };

            draw_pulse_segment(from, base, axis_rgb[a]);

            glLineWidth(2.5f);
            glColor4f(axis_rgb[a][0]*0.6f + 0.4f,
                      axis_rgb[a][1]*0.6f + 0.4f,
                      axis_rgb[a][2]*0.6f + 0.4f, 0.95f);
            draw_arrow_head(tip, dir, head_len);
            glLineWidth(1.0f);

            glPointSize(7.0f);
            glBegin(GL_POINTS);
            glColor4f(axis_rgb[a][0]*0.6f + 0.4f,
                      axis_rgb[a][1]*0.6f + 0.4f,
                      axis_rgb[a][2]*0.6f + 0.4f, 1.0f);
            glVertex3f(tip[0], tip[1], tip[2]);
            glEnd();
            glPointSize(1.0f);
        }
    }

    glDisable(GL_BLEND);
    if (g_user_lighting_enabled) glEnable(GL_LIGHTING);
    scene_render_pop_state();
}

static void draw_rotate_guide(const GLCmd *cmd, const float p_start[3]) {
#if 0 // Keep angle [-360,360]
    float angle_deg = fmodf(cmd->args[0], 360.0f); // Keep angle [-360,360]
#else // Keep angle [-720,720], unlike the above, |angle| > 360 gets a full rotation + the offset
    float angle_deg = cmd->args[0];
    while (angle_deg > 720.0f) angle_deg -= 360.0f; // Handle large angles by wrapping to a single rotation
    while (angle_deg < -720.0f) angle_deg += 360.0f;
#endif
    float ax = cmd->args[1], ay = cmd->args[2], az = cmd->args[3];
    float alen = sqrtf(ax*ax + ay*ay + az*az);
    if (alen < 1e-6f) return;
    if (fabsf(angle_deg) < 1e-4f) return;

    /* Color from the rotation axis: (|ax|,|ay|,|az|)/max → RGB. */
    float rgb[3]; xform_axis_color(ax, ay, az, rgb);
    float bright[3] = {
        rgb[0]*0.6f + 0.4f, rgb[1]*0.6f + 0.4f, rgb[2]*0.6f + 0.4f
    };

    ax /= alen; ay /= alen; az /= alen;

    float plen = sqrtf(p_start[0]*p_start[0] + p_start[1]*p_start[1]
                      + p_start[2]*p_start[2]);

    scene_render_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Axis stub through local origin — length scales with arc radius so it's
     * visible even when p_start is near origin. */
    float axis_len = (plen > 0.5f ? plen : 0.5f) * 1.1f;
    glLineWidth(2.0f);
    glColor4f(rgb[0], rgb[1], rgb[2], 0.55f);
    glBegin(GL_LINES);
    glVertex3f(-ax*axis_len, -ay*axis_len, -az*axis_len);
    glVertex3f( ax*axis_len,  ay*axis_len,  az*axis_len);
    glEnd();

    /* Arc — Rodrigues rotation of p_start about axis, sampled in segments.
     * When p_start sits on (or near) the pivot the arc collapses to a point,
     * so fall back to a helix that winds around the axis itself. */
    const int segs = 48;
    float angle_rad = angle_deg * (float)(3.14159265358979323846 / 180.0);
    int use_helix = (plen < 0.05f);

    float arc[49][3];
    if (!use_helix) {
        for (int i = 0; i <= segs; i++) {
            float t = (float)i / (float)segs;
            float th = angle_rad * t;
            float c = cosf(th), s = sinf(th), k = 1.0f - c;
            float px = p_start[0], py = p_start[1], pz = p_start[2];
            arc[i][0] = (c + ax*ax*k)    * px + (ax*ay*k - az*s) * py + (ax*az*k + ay*s) * pz;
            arc[i][1] = (ay*ax*k + az*s) * px + (c + ay*ay*k)    * py + (ay*az*k - ax*s) * pz;
            arc[i][2] = (az*ax*k - ay*s) * px + (az*ay*k + ax*s) * py + (c + az*az*k)    * pz;
        }
    } else {
        /* Orthonormal basis (u, v) perpendicular to the axis. */
        float helper[3] = {1.0f, 0.0f, 0.0f};
        if (fabsf(ax) > 0.9f) { helper[0] = 0.0f; helper[1] = 1.0f; }
        float u[3] = {
            helper[1]*az - helper[2]*ay,
            helper[2]*ax - helper[0]*az,
            helper[0]*ay - helper[1]*ax,
        };
        float ul = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
        if (ul < 1e-6f) ul = 1.0f;
        u[0] /= ul; u[1] /= ul; u[2] /= ul;
        float v[3] = {
            ay*u[2] - az*u[1],
            az*u[0] - ax*u[2],
            ax*u[1] - ay*u[0],
        };

        float radius = axis_len * 0.28f;
        float pitch  = axis_len * 0.50f; /* axial distance per 2π of sweep */
        const float TAU = 6.28318530717958647692f;
        float axial_span = pitch * (fabsf(angle_rad) / TAU);
        if (axial_span > axis_len * 1.4f) axial_span = axis_len * 1.4f;
        float axial_start = -axial_span * 0.5f;

        for (int i = 0; i <= segs; i++) {
            float t = (float)i / (float)segs;
            float th = angle_rad * t;
            float c = cosf(th), s = sinf(th);
            float a = axial_start + axial_span * t;
            arc[i][0] = ax*a + radius * (c*u[0] + s*v[0]);
            arc[i][1] = ay*a + radius * (c*u[1] + s*v[1]);
            arc[i][2] = az*a + radius * (c*u[2] + s*v[2]);
        }
    }

    /* Dim solid base arc. */
    glLineWidth(2.0f);
    glColor4f(rgb[0], rgb[1], rgb[2], 0.30f);
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i <= segs; i++) glVertex3fv(arc[i]);
    glEnd();

    /* Pulse — bright dot sweeps along the arc with a short fading trail. */
    float ph = fmodf(g_anim_time * 0.6f, 1.0f);
    float glow = sinf(ph * (float)M_PI) * 0.8f + 0.2f;
    float fpos = ph * (float)segs;
    int i_pos = (int)fpos; if (i_pos >= segs) i_pos = segs - 1;
    float fr = fpos - (float)i_pos;
    float pos[3] = {
        arc[i_pos][0] + (arc[i_pos+1][0] - arc[i_pos][0]) * fr,
        arc[i_pos][1] + (arc[i_pos+1][1] - arc[i_pos][1]) * fr,
        arc[i_pos][2] + (arc[i_pos+1][2] - arc[i_pos][2]) * fr,
    };
    float tp = ph - 0.25f; if (tp < 0) tp = 0;
    float ftp = tp * (float)segs;
    int i_tp = (int)ftp; if (i_tp >= segs) i_tp = segs - 1;
    float ftr = ftp - (float)i_tp;
    float trail[3] = {
        arc[i_tp][0] + (arc[i_tp+1][0] - arc[i_tp][0]) * ftr,
        arc[i_tp][1] + (arc[i_tp+1][1] - arc[i_tp][1]) * ftr,
        arc[i_tp][2] + (arc[i_tp+1][2] - arc[i_tp][2]) * ftr,
    };

    /* Trail as a short bright strip between trail→pos, following arc samples
     * in between so curvature is preserved. */
    glLineWidth(3.5f);
    glBegin(GL_LINE_STRIP);
    glColor4f(rgb[0], rgb[1], rgb[2], 0.05f);
    glVertex3f(trail[0], trail[1], trail[2]);
    for (int i = i_tp + 1; i <= i_pos; i++) {
        float u = (float)(i - i_tp) / (float)(i_pos - i_tp + 1);
        glColor4f(rgb[0], rgb[1], rgb[2], 0.05f + (glow * 0.7f) * u);
        glVertex3fv(arc[i]);
    }
    glColor4f(rgb[0], rgb[1], rgb[2], glow * 0.75f);
    glVertex3f(pos[0], pos[1], pos[2]);
    glEnd();
    glLineWidth(1.0f);

    glPointSize(8.0f);
    glBegin(GL_POINTS);
    glColor4f(bright[0], bright[1], bright[2], glow);
    glVertex3f(pos[0], pos[1], pos[2]);
    glEnd();
    glPointSize(1.0f);

    /* Start/end endpoint dots. */
    glPointSize(6.0f);
    glBegin(GL_POINTS);
    glColor4f(rgb[0], rgb[1], rgb[2], 0.7f);
    glVertex3f(p_start[0], p_start[1], p_start[2]);
    glEnd();
    glPointSize(10.0f);
    glBegin(GL_POINTS);
    glColor4f(bright[0], bright[1], bright[2], 0.95f);
    glVertex3fv(arc[segs]);
    glEnd();
    glPointSize(1.0f);

    glDisable(GL_BLEND);
    if (g_user_lighting_enabled) glEnable(GL_LIGHTING);
    scene_render_pop_state();
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
        for (int i = 0; i < g_num_flat_cmds; i++) {
            if (!g_flat_cmds[i].valid) continue;

            if (is_transform_cmd(g_flat_cmds[i].type)) {
                if (!in_contour)
                    apply_tracked_transform_cmd(&g_flat_cmds[i], &matrix_depth);
                continue;
            }

            switch (g_flat_cmds[i].type) {
            case CMD_TESS_BEGIN_CONTOUR:
                if (in_contour)
                    glEnd();
                glBegin(GL_LINE_STRIP);
                in_contour = 1;
                break;
            case CMD_TESS_VERTEX:
                if (in_contour)
                    glVertex3f(g_flat_cmds[i].args[0], g_flat_cmds[i].args[1],
                               g_flat_cmds[i].args[2]);
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
        unwind_tracked_transform_stack(&matrix_depth);
    }
    glPopMatrix();

    glLineWidth(1.0f);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    if (g_user_lighting_enabled) glEnable(GL_LIGHTING);
    scene_render_pop_state();
}

static void draw_replay_hud(const SceneRenderConfig *config) {
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
        hud_y = g_code_panel_layout == CODE_PANEL_LAYOUT_TOP
              ? scene_y + scene_h - REPLAY_HUD_HEIGHT - 4
              : min_y;
    }
    if (g_replay_total_flat > 0)
        progress = (float)g_replay_pc / (float)g_replay_total_flat;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    scene_render_push_state();
    glViewport(0, 0, g_win_w, g_win_h);
    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Panel bg matches the menubar palette: #1d1d1d with subtle green tint
     * on the border so the HUD reads as paired with the green Replay button. */
    glColor4f(0.114f, 0.118f, 0.114f, 0.94f); /* #1d1e1d */
    draw_quad((float)hud_x, (float)hud_y, (float)hud_w, (float)REPLAY_HUD_HEIGHT);
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
    if (g_replay_state == REPLAY_PLAYING) {
        float bw = 3.0f, gap = 3.0f;
        float by0 = (float)icon_cy - (float)icon_sz * 0.5f;
        draw_quad((float)icon_cx - bw - gap * 0.5f, by0, bw, (float)icon_sz);
        draw_quad((float)icon_cx + gap * 0.5f,      by0, bw, (float)icon_sz);
    } else if (g_replay_state == REPLAY_DONE) {
        /* Square — run complete */
        float sx = (float)icon_cx - (float)icon_sz * 0.5f;
        float sy = (float)icon_cy - (float)icon_sz * 0.5f;
        draw_quad(sx, sy, (float)icon_sz, (float)icon_sz);
    } else {
        /* Play triangle — paused / stopped, click to (re)start */
        float x0 = (float)icon_cx - (float)icon_sz * 0.5f;
        float cy = (float)icon_cy;
        glBegin(GL_TRIANGLES);
        glVertex2f(x0,              cy - (float)icon_sz * 0.5f);
        glVertex2f(x0,              cy + (float)icon_sz * 0.5f);
        glVertex2f(x0 + icon_sz,    cy);
        glEnd();
    }

    /* Line 1 — "Replay  4.0 cmd/s | Polygon" in green; command count
     * is right-aligned so 4-digit totals don't push other fields around. */
    snprintf(progress_txt, sizeof(progress_txt),
             "Replay  %11.1f cmd/s  | %7s  | %s",
             g_replay_speed,
             config->replay_mode == REPLAY_MODE_VERTEX ? "Vertex" : "Polygon",
             g_replay_expand_args ? "Code Expanded" : ""
             );
    glColor3f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B);
    draw_string((float)text_col_x,
                (float)(hud_y + REPLAY_HUD_TEXT_LINE1_Y),
                progress_txt, FONT_SMALL);

    char count_txt[32];
    snprintf(count_txt, sizeof(count_txt), "%d / %d",
             g_replay_pc, g_replay_total_flat);
    int count_w = (int)strlen(count_txt) * FONT_SMALL_W;
    draw_string((float)(hud_x + hud_w - REPLAY_HUD_TEXT_PAD_X - count_w),
                (float)(hud_y + REPLAY_HUD_TEXT_LINE1_Y),
                count_txt, FONT_SMALL);

    /* Progress groove + green fill */
    int groove_x = hud_x + REPLAY_HUD_TEXT_PAD_X;
    int groove_w = hud_w - 2 * REPLAY_HUD_TEXT_PAD_X;
    int groove_y = hud_y + REPLAY_HUD_PROGRESS_Y;
    glColor4f(0.094f, 0.118f, 0.102f, 1.0f);  /* #181e1a */
    draw_quad((float)groove_x, (float)groove_y,
              (float)groove_w, (float)REPLAY_HUD_PROGRESS_H);
    glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, 1.0f);
    draw_quad((float)groove_x, (float)groove_y,
              (float)groove_w * progress, (float)REPLAY_HUD_PROGRESS_H);
    glColor4f(0.227f, 0.298f, 0.243f, 1.0f);  /* #3a4c3e border */
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)groove_x + 0.5f,                     (float)groove_y + 0.5f);
    glVertex2f((float)(groove_x + groove_w) - 0.5f,        (float)groove_y + 0.5f);
    glVertex2f((float)(groove_x + groove_w) - 0.5f,        (float)(groove_y + REPLAY_HUD_PROGRESS_H) - 0.5f);
    glVertex2f((float)groove_x + 0.5f,                     (float)(groove_y + REPLAY_HUD_PROGRESS_H) - 0.5f);
    glEnd();

    /* Line 2 — compact kbd hints along the bottom in muted gray */
    snprintf(kbd_txt, sizeof(kbd_txt),
             "Space pause  |  +/- speed  |  m mode  |  e expand |  %c %c step |  Esc stop", 0xAB, 0xBB);
    glColor3f(0.533f, 0.533f, 0.533f);  /* #888 */
    draw_string((float)text_col_x,
                (float)(hud_y + REPLAY_HUD_TEXT_LINE2_Y),
                kbd_txt, FONT_SMALL);

    glDisable(GL_BLEND);
    end_2d();
    scene_render_pop_state();
}

/* Ground-plane crosshair gizmo at the orbit target. Visible only while the
 * camera is moving (during drag or while momentum carries it); fades out.
 * REPL-only — never exported. Styled to match the other scene helpers:
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

void render_3d_scene(void) {
    SceneRenderConfig config;
    FrameRenderContext frame_ctx;
    scene_render_config_init(&config);
    scene_prepare_frame_context(&frame_ctx, &config);

    prof_begin(PROF_SCENE_3D_SETUP);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glViewport(config.scene_x, config.scene_y, config.scene_w, config.scene_h);

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

    {
        FlatProgramView flat_program = repl_flat_program_view_live();
        ReplExecutionOptions exec_options = {
            .flat_cmd_count = flat_program.cmd_count,
            .program = flat_program
        };

        if (config.replay_has_fades)
            exec_options.flat_cmd_count = config.replay_fill_base_limit;

        prof_begin(PROF_SCENE_3D_FILL);
        glPushMatrix();
        repl_execute_program(&exec_options);
        glPopMatrix();
        prof_accum_end(PROF_SCENE_3D_FILL);

        if (config.replay_has_fades) {
            prof_begin(PROF_SCENE_3D_FADE);
            scene_lights_setup();
            glDisable(GL_LIGHTING);
            glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
            glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mspec);
            glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, mshin);
            scene_apply_quality_config(&config);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(0.70f, 0.70f, 0.80f, 1.0f);
            if (config.wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            else glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

            glPushMatrix();
            execute_replay_fade_batches();
            glPopMatrix();
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

    /* Vertex dots — replay transforms so dots match the filled geometry */
    prof_begin(PROF_SCENE_3D_OVERLAYS);
    glPushMatrix();
    /* Snapshot the pure camera-view matrix (before any user transforms are
     * applied below) so the transform guide can render in world axes at an
     * anchor point, independent of pre-cursor rotations. */
    float tg_cam_view[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, tg_cam_view);
    {
    int matrix_depth = 0;
    glPointSize(8.0f);
    if (config.replay_vertex_points)
        glColor3f(1.0f, 0.88f, 0.20f);
    else
        glColor3f(0.0f, 0.0f, 0.0f);

    /* Transform-guide setup: when the cursor is on a committed glTranslatef
     * or glRotatef source line, find the first flat cmd matching that source
     * line and the next flat cmd in execution order whose source index
     * differs (the starting point for the post-cursor modelview walk).
     *
     * Using flat execution order — rather than src_cmd_idx > g_edit_line —
     * is important because flat cmds expanded from a function call carry
     * the function body's src_cmd_idx, not the call site's; a numeric
     * comparison against g_edit_line would skip right past them. */
    int tg_want = 0;
    int tg_first_cursor_flat = -1;
    int tg_first_after_flat  = -1;
    if (!config.replaying && config.show_guides &&
        g_edit_line >= 0 && g_edit_line < g_num_cmds) {
        const GLCmd *sc = &g_cmds[g_edit_line];

        /* "Only when complete and correct" — require the committed parse to
         * be valid AND the live input buffer to match the normalized source,
         * so we don't keep rendering a stale guide while the user is editing
         * the line into an invalid/partial state. Mirrors the `unmodified`
         * check in repl_editor.c. */
        int unmodified = 0;
        {
            const char *s = sc->source;
            while (*s && isspace((unsigned char)*s)) s++;
            int slen = (int)strlen(s);
            while (slen > 0 &&
                   (s[slen - 1] == ';' || isspace((unsigned char)s[slen - 1])))
                slen--;
            if ((slen == g_input_len && strncmp(g_input, s, (size_t)slen) == 0) ||
                g_input_len == 0)
                unmodified = 1;
        }

        if (sc->valid && unmodified &&
            (sc->type == CMD_TRANSLATE3F || sc->type == CMD_ROTATEF ||
             sc->type == CMD_SCALEF)) {
            for (int j = 0; j < g_num_flat_cmds; j++) {
                if (!g_flat_cmds[j].valid) continue;
                if (g_flat_cmds[j].src_cmd_idx == g_edit_line) {
                    if (tg_first_cursor_flat < 0) tg_first_cursor_flat = j;
                }
            }
            if (tg_first_cursor_flat >= 0) {
                for (int j = tg_first_cursor_flat + 1; j < g_num_flat_cmds; j++) {
                    if (!g_flat_cmds[j].valid) continue;
                    if (g_flat_cmds[j].src_cmd_idx == g_edit_line) continue;
                    tg_first_after_flat = j;
                    break;
                }
                if (tg_first_after_flat < 0) tg_first_after_flat = g_num_flat_cmds;
                tg_want = 1;
            }
        }
    }

    for (int i = 0; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        int is_cursor = (g_flat_cmds[i].src_cmd_idx == g_edit_line);

        // Draw vertex guides and normals if this vertex is at the cursor
        // position. Do this inline so that the current model matrix is applied
        // to the guides, ensuring they are positioned correctly even when
        // transforms separate blocks.
        if (is_cursor && !config.replaying) {
            draw_vertex_guides(&config);
            draw_normal_guides(&config);
        }

        /* Transform guide: two modes, selectable via the config menu
         * g_xform_guide_mode ("Xform guide mode"):
         *
         *   World (0): render in world axes at world origin. Matches strict
         *     OpenGL reverse-order semantics — for cursor command C_k,
         *     p_after = M_after·origin is the point C_k acts on, and
         *     pre-cursor transforms (which wrap this sub-expression) don't
         *     move the guide.
         *
         *   Frame (1): render at the live frame. Loads view · M_before as the
         *     modelview so pre-cursor rotations orient translate arrows, scale
         *     axes, and rotate arcs with the frame being edited. The cursor
         *     command's "acts-on" point is the local origin. */
        if (tg_want && i == tg_first_cursor_flat) {
            /* Read args from the flat cmd, not the source cmd — when the
             * command uses variables (e.g. glRotatef(3*t, 0,1,0)), flatten
             * re-evaluates into g_flat_cmds[].args[] each frame, while
             * g_cmds[].args[] keeps its initial-parse value. */
            const GLCmd *live_cmd = &g_flat_cmds[i];
            float p_guide[3];
            glPushMatrix();
            if (g_xform_guide_mode == 1) {
                /* Frame mode: load view · M_before so the guide renders in the
                 * cursor's local frame — pre-cursor rotations re-orient the
                 * translate arrow, scale axes, and rotate arc together. */
                float frame[16];
                float guide_mv[16];
                compute_before_cursor_matrix(tg_first_cursor_flat, frame);
                mat4_mul_col_major(tg_cam_view, frame, guide_mv);
                glLoadMatrixf(guide_mv);
                /* In local frame the "after" point the cursor command acts on
                 * is the local origin. draw_scale_guide's origin fallback then
                 * draws three per-axis arrows, which is what scale-of-origin
                 * naturally collapses to. */
                p_guide[0] = p_guide[1] = p_guide[2] = 0.0f;
            } else {
                /* World mode: render in world axes at the world-space point
                 * the cursor command acts on. */
                glLoadMatrixf(tg_cam_view);
                compute_after_cursor_origin(tg_first_after_flat, p_guide);
            }
            if (live_cmd->type == CMD_TRANSLATE3F)
                draw_translate_guide(live_cmd, p_guide);
            else if (live_cmd->type == CMD_SCALEF)
                draw_scale_guide(live_cmd, p_guide);
            else
                draw_rotate_guide(live_cmd, p_guide);
            glPopMatrix();
            tg_want = 0;
        }

        if (is_transform_cmd(g_flat_cmds[i].type)) {
            apply_tracked_transform_cmd(&g_flat_cmds[i], &matrix_depth);
        } else if ((config.show_vpoints || config.replay_vertex_points) &&
                   (g_flat_cmds[i].type == CMD_VERTEX3F ||
                    g_flat_cmds[i].type == CMD_TESS_VERTEX)) {
            glBegin(GL_POINTS);
            glVertex3f(g_flat_cmds[i].args[0], g_flat_cmds[i].args[1],
                       g_flat_cmds[i].args[2]);
            glEnd();
        }
    }
    glPointSize(1.0f);
    unwind_tracked_transform_stack(&matrix_depth);
    }
    glPopMatrix();
    glDisable(GL_BLEND);
    if (g_user_lighting_enabled) glEnable(GL_LIGHTING);
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
