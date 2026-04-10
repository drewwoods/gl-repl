/*
 * scene_render.c — 3D scene rendering (grid, axes, lights, vertex overlays)
 *
 * Extracted from sample.c for maintainability.
 */
#include "sample.h"
#include "repl_core.h"
#include "scene_render.h"
#include "ui_panels.h"

/* ========================================================================= */
/* 3D scene helpers                                                           */
/* ========================================================================= */

static void draw_grid(void) {
    if (g_grid_theme == 0) return;

    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    if (g_multisample_enabled) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);
    if (g_line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Nudge grid slightly below Y=0 to avoid z-fighting with axes */
    glPushMatrix();
    glTranslatef(0, -0.002f, 0);

    float breath = sinf(g_anim_time * 0.8f) * 0.5f + 0.5f; /* 0..1 */

    switch (g_grid_theme) {

    case 1: { /* Classic */
        float extent = 5.0f, step = 0.5f;
        glBegin(GL_LINES);
        for (float v = -extent; v <= extent + 0.01f; v += step) {
            float a = (fabsf(v) < 0.01f) ? 0.45f : 0.12f;
            glColor4f(0.50f, 0.50f, 0.60f, a);
            glVertex3f(v, 0, -extent); glVertex3f(v, 0, extent);
            glVertex3f(-extent, 0, v); glVertex3f(extent, 0, v);
        }
        glEnd();
        break;
    }

    case 2: { /* Fog — large grid, breathing fog density */
        float extent = 15.0f, step = 0.5f;
        float fog_density = 0.06f + breath * 0.04f;
        GLfloat fog_col[] = { 0.10f, 0.10f, 0.13f, 1.0f };
        glEnable(GL_FOG);
        glFogi(GL_FOG_MODE, GL_EXP2);
        glFogfv(GL_FOG_COLOR, fog_col);
        glFogf(GL_FOG_DENSITY, fog_density);

        glBegin(GL_LINES);
        for (float v = -extent; v <= extent + 0.01f; v += step) {
            int is_major = (fabsf(fmodf(fabsf(v) + 0.01f, 2.0f)) < 0.1f);
            int is_origin = (fabsf(v) < 0.01f);
            float a = is_origin ? 0.55f : (is_major ? 0.25f : 0.10f);
            glColor4f(0.45f, 0.50f, 0.65f, a);
            glVertex3f(v, 0, -extent); glVertex3f(v, 0, extent);
            glVertex3f(-extent, 0, v); glVertex3f(extent, 0, v);
        }
        glEnd();

        glDisable(GL_FOG);
        break;
    }

    case 3: { /* Tron — cyan/blue glow, distance fade */
        float extent = 12.0f, step = 0.5f;
        float glow = 0.7f + breath * 0.3f;

        glLineWidth(1.0f);
        glBegin(GL_LINES);
        for (float v = -extent; v <= extent + 0.01f; v += step) {
            float dist = fabsf(v) / extent;
            float fade = (1.0f - dist * dist);
            if (fade < 0.0f) fade = 0.0f;
            int is_origin = (fabsf(v) < 0.01f);
            int is_major = (fabsf(fmodf(fabsf(v) + 0.01f, 2.0f)) < 0.1f);
            float base = is_origin ? 0.8f : (is_major ? 0.35f : 0.12f);
            float a = base * fade * glow;
            float r = 0.05f, g = is_origin ? 0.9f : 0.55f, b = 0.95f;
            glColor4f(r, g, b, a);
            /* Draw both directions with distance fade per-endpoint */
            glVertex3f(v, 0, -extent); glVertex3f(v, 0, extent);
            glVertex3f(-extent, 0, v); glVertex3f(extent, 0, v);
        }
        glEnd();

        /* Subtle glow line on axes */
        glLineWidth(2.0f);
        float ga = 0.25f * glow;
        glBegin(GL_LINES);
        glColor4f(0.0f, 0.8f, 1.0f, ga);
        glVertex3f(0, 0, -extent); glVertex3f(0, 0, extent);
        glVertex3f(-extent, 0, 0); glVertex3f(extent, 0, 0);
        glEnd();
        glLineWidth(1.0f);
        break;
    }

    case 4: { /* Ember — warm orange/red, pulsing ripple */
        float extent = 12.0f, step = 0.5f;

        glBegin(GL_LINES);
        for (float v = -extent; v <= extent + 0.01f; v += step) {
            float dist = fabsf(v) / extent;
            /* Ripple: wave traveling outward from center */
            float ripple = sinf(dist * 12.0f - g_anim_time * 2.5f);
            ripple = ripple * 0.5f + 0.5f; /* 0..1 */
            float fade = 1.0f - dist;
            if (fade < 0.0f) fade = 0.0f;
            int is_origin = (fabsf(v) < 0.01f);
            int is_major = (fabsf(fmodf(fabsf(v) + 0.01f, 2.0f)) < 0.1f);
            float base = is_origin ? 0.7f : (is_major ? 0.30f : 0.10f);
            float a = base * fade * (0.6f + ripple * 0.4f);
            float r = 0.95f, g = 0.35f + ripple * 0.25f, b = 0.05f;
            glColor4f(r, g, b, a);
            glVertex3f(v, 0, -extent); glVertex3f(v, 0, extent);
            glVertex3f(-extent, 0, v); glVertex3f(extent, 0, v);
        }
        glEnd();
        break;
    }

    case 5: { /* Faint — very subtle 0.5 reference lines */
        float extent = 5.0f, step = 0.5f;
        glBegin(GL_LINES);
        for (float v = -extent; v <= extent + 0.01f; v += step) {
            int is_origin = (fabsf(v) < 0.01f);
            int is_major = (fabsf(fmodf(fabsf(v) + 0.01f, 1.0f)) < 0.1f);
            float a = is_origin ? 0.18f : (is_major ? 0.07f : 0.03f);
            glColor4f(0.50f, 0.50f, 0.60f, a);
            glVertex3f(v, 0, -extent); glVertex3f(v, 0, extent);
            glVertex3f(-extent, 0, v); glVertex3f(extent, 0, v);
        }
        glEnd();
        break;
    }

    case 6: { /* Focus — fades rapidly around selected vertex */
        /* Update focus vertex from current or nearest vertex line */
        if (g_edit_line < g_num_cmds &&
            g_cmds[g_edit_line].valid &&
            (g_cmds[g_edit_line].type == CMD_VERTEX3F ||
             g_cmds[g_edit_line].type == CMD_TESS_VERTEX)) {
            g_focus_vtx[0] = g_cmds[g_edit_line].args[0];
            g_focus_vtx[1] = g_cmds[g_edit_line].args[1];
            g_focus_vtx[2] = g_cmds[g_edit_line].args[2];
            g_focus_vtx_valid = 1;
        } else if (!g_focus_vtx_valid) {
            /* Scan backwards from edit line for nearest vertex */
            for (int i = g_edit_line - 1; i >= 0; i--) {
                if (g_cmds[i].valid &&
                    (g_cmds[i].type == CMD_VERTEX3F ||
                     g_cmds[i].type == CMD_TESS_VERTEX)) {
                    g_focus_vtx[0] = g_cmds[i].args[0];
                    g_focus_vtx[1] = g_cmds[i].args[1];
                    g_focus_vtx[2] = g_cmds[i].args[2];
                    g_focus_vtx_valid = 1;
                    break;
                }
            }
        }

        float cx = g_focus_vtx[0], cz = g_focus_vtx[2];
        float extent = 8.0f, step = 0.5f;
        float radius = 3.0f;  /* fade-out radius */

        glBegin(GL_LINES);
        for (float v = -extent; v <= extent + 0.01f; v += step) {
            int is_origin = (fabsf(v) < 0.01f);
            int is_major = (fabsf(fmodf(fabsf(v) + 0.01f, 1.0f)) < 0.1f);
            float base = is_origin ? 0.40f : (is_major ? 0.18f : 0.06f);

            /* Vertical line at x=v: fade based on distance from cx */
            float dx = v - cx;
            float fx = 1.0f - (dx * dx) / (radius * radius);
            if (fx < 0.0f) fx = 0.0f;
            fx = fx * fx;  /* sharper falloff */
            if (fx > 0.001f) {
                glColor4f(0.50f, 0.55f, 0.70f, base * fx);
                /* Clamp line Z extent around focus */
                float z0 = cz - radius, z1 = cz + radius;
                if (z0 < -extent) z0 = -extent;
                if (z1 > extent) z1 = extent;
                glVertex3f(v, 0, z0); glVertex3f(v, 0, z1);
            }

            /* Horizontal line at z=v: fade based on distance from cz */
            float dz = v - cz;
            float fz = 1.0f - (dz * dz) / (radius * radius);
            if (fz < 0.0f) fz = 0.0f;
            fz = fz * fz;
            if (fz > 0.001f) {
                glColor4f(0.50f, 0.55f, 0.70f, base * fz);
                float x0 = cx - radius, x1 = cx + radius;
                if (x0 < -extent) x0 = -extent;
                if (x1 > extent) x1 = extent;
                glVertex3f(x0, 0, v); glVertex3f(x1, 0, v);
            }
        }
        glEnd();

        /* Crosshair at focus point */
        if (g_focus_vtx_valid) {
            glLineWidth(1.5f);
            glBegin(GL_LINES);
            glColor4f(0.80f, 0.85f, 0.95f, 0.25f);
            glVertex3f(cx - 0.3f, 0, cz);
            glVertex3f(cx + 0.3f, 0, cz);
            glVertex3f(cx, 0, cz - 0.3f);
            glVertex3f(cx, 0, cz + 0.3f);
            glEnd();
            glLineWidth(1.0f);
        }
        break;
    }

    default: break;
    }

    glPopMatrix();
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    if (g_user_lighting_enabled) glEnable(GL_LIGHTING);
}

/* Helper: draw an axis label at a 3D position */
static void draw_axis_label(float x, float y, float z, char ch,
                            float r, float g, float b) {
    glColor3f(r, g, b);
    glRasterPos3f(x, y, z);
    glutBitmapCharacter(FONT_MONO, ch);
}

static void draw_axes(void) {
    if (g_axes_theme == 0) return;

    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    if (g_multisample_enabled) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);
    if (g_line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float breath = sinf(g_anim_time * 0.8f) * 0.5f + 0.5f; /* 0..1 */

    switch (g_axes_theme) {

    case 1: { /* Classic */
        float len = 2.0f;
        glLineWidth(2.0f);
        glBegin(GL_LINES);
        glColor3f(0.90f, 0.20f, 0.20f);
        glVertex3f(0, 0, 0); glVertex3f(len, 0, 0);
        glColor3f(0.20f, 0.90f, 0.20f);
        glVertex3f(0, 0, 0); glVertex3f(0, len, 0);
        glColor3f(0.20f, 0.20f, 0.90f);
        glVertex3f(0, 0, 0); glVertex3f(0, 0, len);
        glEnd();
        glLineWidth(1.0f);

        draw_axis_label(2.15f, 0, 0, 'X', 0.90f, 0.30f, 0.30f);
        draw_axis_label(0, 2.15f, 0, 'Y', 0.30f, 0.90f, 0.30f);
        draw_axis_label(0, 0, 2.15f, 'Z', 0.30f, 0.30f, 0.90f);
        break;
    }

    case 2: { /* Pulse — dot traveling outward along each axis */
        float len = 3.0f;
        /* Solid dim axes */
        glLineWidth(1.5f);
        glBegin(GL_LINES);
        glColor4f(0.90f, 0.20f, 0.20f, 0.30f);
        glVertex3f(0, 0, 0); glVertex3f(len, 0, 0);
        glColor4f(0.20f, 0.90f, 0.20f, 0.30f);
        glVertex3f(0, 0, 0); glVertex3f(0, len, 0);
        glColor4f(0.20f, 0.20f, 0.90f, 0.30f);
        glVertex3f(0, 0, 0); glVertex3f(0, 0, len);
        glEnd();
        glLineWidth(1.0f);

        /* Pulsing dot position (loops 0..1) */
        float t = fmodf(g_anim_time * 0.6f, 1.0f);
        float pos = t * len;
        float glow = sinf(t * (float)M_PI); /* bright in middle, dim at ends */
        glow = glow * 0.8f + 0.2f;

        glPointSize(8.0f);
        glBegin(GL_POINTS);
        glColor4f(1.0f, 0.3f, 0.3f, glow);
        glVertex3f(pos, 0, 0);
        glColor4f(0.3f, 1.0f, 0.3f, glow);
        glVertex3f(0, pos, 0);
        glColor4f(0.3f, 0.3f, 1.0f, glow);
        glVertex3f(0, 0, pos);
        glEnd();
        glPointSize(1.0f);

        /* Bright trail behind the dot */
        glLineWidth(3.0f);
        float trail = 0.6f;
        float t0 = pos - trail;
        if (t0 < 0) t0 = 0;
        glBegin(GL_LINES);
        glColor4f(1.0f, 0.3f, 0.3f, 0.05f);
        glVertex3f(t0, 0, 0);
        glColor4f(1.0f, 0.3f, 0.3f, glow * 0.7f);
        glVertex3f(pos, 0, 0);

        glColor4f(0.3f, 1.0f, 0.3f, 0.05f);
        glVertex3f(0, t0, 0);
        glColor4f(0.3f, 1.0f, 0.3f, glow * 0.7f);
        glVertex3f(0, pos, 0);

        glColor4f(0.3f, 0.3f, 1.0f, 0.05f);
        glVertex3f(0, 0, t0);
        glColor4f(0.3f, 0.3f, 1.0f, glow * 0.7f);
        glVertex3f(0, 0, pos);
        glEnd();
        glLineWidth(1.0f);

        draw_axis_label(len + 0.15f, 0, 0, 'X', 0.70f, 0.25f, 0.25f);
        draw_axis_label(0, len + 0.15f, 0, 'Y', 0.25f, 0.70f, 0.25f);
        draw_axis_label(0, 0, len + 0.15f, 'Z', 0.25f, 0.25f, 0.70f);
        break;
    }

    case 3: { /* Neon — bright glowing axes with breathing intensity */
        float len = 2.5f;
        float glow = 0.6f + breath * 0.4f;

        /* Outer glow (wide, dim) */
        glLineWidth(6.0f);
        glBegin(GL_LINES);
        glColor4f(1.0f, 0.1f, 0.1f, 0.12f * glow);
        glVertex3f(0, 0, 0); glVertex3f(len, 0, 0);
        glColor4f(0.1f, 1.0f, 0.1f, 0.12f * glow);
        glVertex3f(0, 0, 0); glVertex3f(0, len, 0);
        glColor4f(0.1f, 0.1f, 1.0f, 0.12f * glow);
        glVertex3f(0, 0, 0); glVertex3f(0, 0, len);
        glEnd();

        /* Core (narrow, bright) */
        glLineWidth(2.0f);
        glBegin(GL_LINES);
        glColor4f(1.0f, 0.4f, 0.4f, 1.0f * glow);
        glVertex3f(0, 0, 0); glVertex3f(len, 0, 0);
        glColor4f(0.4f, 1.0f, 0.4f, 1.0f * glow);
        glVertex3f(0, 0, 0); glVertex3f(0, len, 0);
        glColor4f(0.4f, 0.4f, 1.0f, 1.0f * glow);
        glVertex3f(0, 0, 0); glVertex3f(0, 0, len);
        glEnd();
        glLineWidth(1.0f);

        /* Bright tip dots */
        glPointSize(6.0f);
        glBegin(GL_POINTS);
        glColor4f(1.0f, 0.5f, 0.5f, glow);
        glVertex3f(len, 0, 0);
        glColor4f(0.5f, 1.0f, 0.5f, glow);
        glVertex3f(0, len, 0);
        glColor4f(0.5f, 0.5f, 1.0f, glow);
        glVertex3f(0, 0, len);
        glEnd();
        glPointSize(1.0f);

        float la = 0.5f + glow * 0.5f;
        draw_axis_label(len + 0.15f, 0, 0, 'X', 1.0f * la, 0.3f * la, 0.3f * la);
        draw_axis_label(0, len + 0.15f, 0, 'Y', 0.3f * la, 1.0f * la, 0.3f * la);
        draw_axis_label(0, 0, len + 0.15f, 'Z', 0.3f * la, 0.3f * la, 1.0f * la);
        break;
    }

    case 4: { /* Compass — positive and negative axes, dashed negative */
        float len = 2.5f;

        /* Positive axes (solid) */
        glLineWidth(2.0f);
        glBegin(GL_LINES);
        glColor4f(1.0f, 0.30f, 0.30f, 0.85f);
        glVertex3f(0, 0, 0); glVertex3f(len, 0, 0);
        glColor4f(0.30f, 1.0f, 0.30f, 0.85f);
        glVertex3f(0, 0, 0); glVertex3f(0, len, 0);
        glColor4f(0.30f, 0.30f, 1.0f, 0.85f);
        glVertex3f(0, 0, 0); glVertex3f(0, 0, len);
        glEnd();

        /* Negative axes (stippled) */
        glEnable(GL_LINE_STIPPLE);
        glLineStipple(2, 0xAAAA);
        glBegin(GL_LINES);
        glColor4f(1.0f, 0.30f, 0.30f, 0.35f);
        glVertex3f(0, 0, 0); glVertex3f(-len, 0, 0);
        glColor4f(0.30f, 1.0f, 0.30f, 0.35f);
        glVertex3f(0, 0, 0); glVertex3f(0, -len, 0);
        glColor4f(0.30f, 0.30f, 1.0f, 0.35f);
        glVertex3f(0, 0, 0); glVertex3f(0, 0, -len);
        glEnd();
        glDisable(GL_LINE_STIPPLE);
        glLineWidth(1.0f);

        /* Arrowheads at positive tips */
        glPointSize(7.0f);
        glBegin(GL_POINTS);
        glColor4f(1.0f, 0.4f, 0.4f, 0.9f);
        glVertex3f(len, 0, 0);
        glColor4f(0.4f, 1.0f, 0.4f, 0.9f);
        glVertex3f(0, len, 0);
        glColor4f(0.4f, 0.4f, 1.0f, 0.9f);
        glVertex3f(0, 0, len);
        glEnd();

        /* Small dots at negative tips */
        glPointSize(4.0f);
        glBegin(GL_POINTS);
        glColor4f(1.0f, 0.3f, 0.3f, 0.30f);
        glVertex3f(-len, 0, 0);
        glColor4f(0.3f, 1.0f, 0.3f, 0.30f);
        glVertex3f(0, -len, 0);
        glColor4f(0.3f, 0.3f, 1.0f, 0.30f);
        glVertex3f(0, 0, -len);
        glEnd();
        glPointSize(1.0f);

        /* Origin sphere-ish dot */
        glPointSize(5.0f);
        glBegin(GL_POINTS);
        glColor4f(0.9f, 0.9f, 0.9f, 0.6f);
        glVertex3f(0, 0, 0);
        glEnd();
        glPointSize(1.0f);

        draw_axis_label(len + 0.15f, 0, 0, 'X', 0.90f, 0.30f, 0.30f);
        draw_axis_label(0, len + 0.15f, 0, 'Y', 0.30f, 0.90f, 0.30f);
        draw_axis_label(0, 0, len + 0.15f, 'Z', 0.30f, 0.30f, 0.90f);
        draw_axis_label(-len - 0.15f, 0, 0, 'x', 0.55f, 0.25f, 0.25f);
        draw_axis_label(0, -len - 0.15f, 0, 'y', 0.25f, 0.55f, 0.25f);
        draw_axis_label(0, 0, -len - 0.15f, 'z', 0.25f, 0.25f, 0.55f);
        break;
    }

    default: break;
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    if (g_user_lighting_enabled) glEnable(GL_LIGHTING);
}

static int flat_block_matches_cursor(int begin_idx, int is_tess) {
    int depth = is_tess ? 1 : 0;

    for (int i = begin_idx; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        if (repl_flat_cmd_matches_cursor(i))
            return 1;
        if (!is_tess && i > begin_idx && g_flat_cmds[i].type == CMD_END)
            break;
        if (is_tess && i > begin_idx) {
            if (g_flat_cmds[i].type == CMD_TESS_BEGIN_POLYGON) depth++;
            else if (g_flat_cmds[i].type == CMD_TESS_END) {
                depth--;
                if (depth == 0) break;
            }
        }
    }

    return 0;
}

static void draw_vertex_numbers(void) {
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glColor3f(1.0f, 1.0f, 0.30f);

    /* Draw vertex labels only for the target block, replaying transforms */
    glPushMatrix();
    int in_block = 0;
    int block_selected = 0;
    int vn = 0;
    int tess_depth = 0;
    int matrix_depth = 0;
    for (int i = 0; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        if (!in_block && is_transform_cmd(g_flat_cmds[i].type)) {
            apply_tracked_transform_cmd(&g_flat_cmds[i], &matrix_depth);
        } else if (g_flat_cmds[i].type == CMD_BEGIN) {
            in_block = 1;
            block_selected = flat_block_matches_cursor(i, 0);
            vn = 0;
            tess_depth = 0;
        } else if (g_flat_cmds[i].type == CMD_END) {
            in_block = 0;
            block_selected = 0;
        } else if (g_flat_cmds[i].type == CMD_TESS_BEGIN_POLYGON) {
            in_block = 1;
            block_selected = flat_block_matches_cursor(i, 1);
            vn = 0;
            tess_depth = 1;
        } else if (g_flat_cmds[i].type == CMD_TESS_BEGIN_CONTOUR) {
            tess_depth = 2;
        } else if (g_flat_cmds[i].type == CMD_TESS_END && tess_depth > 0) {
            tess_depth--;
            if (tess_depth == 0) { in_block = 0; block_selected = 0; }
        } else if (g_flat_cmds[i].type == CMD_VERTEX3F ||
                   g_flat_cmds[i].type == CMD_TESS_VERTEX) {
            if (in_block && block_selected) {
                char label[16];
                snprintf(label, sizeof(label), " v%d", vn);
                glRasterPos3f(g_flat_cmds[i].args[0], g_flat_cmds[i].args[1],
                              g_flat_cmds[i].args[2]);
                for (const char *c = label; *c; c++)
                    glutBitmapCharacter(FONT_MONO, (unsigned char)*c);
            }
            vn++;
        }
    }
    unwind_tracked_transform_stack(&matrix_depth);
    glPopMatrix();

    glEnable(GL_DEPTH_TEST);
    if (g_user_lighting_enabled) glEnable(GL_LIGHTING);
}

static void draw_normal_vectors(void) {
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glColor3f(0.80f, 0.80f, 0.30f);
    float scale = 0.35f;
    float nx = 0, ny = 0, nz = 1;

    /* Single pass — replay CMD_TRANSLATE3F so normals align with geometry.
     * Use per-vertex glBegin/glEnd pairs so transforms can be applied
     * between blocks without being inside a begin/end. */
    glPushMatrix();
    int in_begin = 0;
    int matrix_depth = 0;
    for (int i = 0; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        if (!in_begin && is_transform_cmd(g_flat_cmds[i].type)) {
            apply_tracked_transform_cmd(&g_flat_cmds[i], &matrix_depth);
        } else if (g_flat_cmds[i].type == CMD_BEGIN ||
                   g_flat_cmds[i].type == CMD_TESS_BEGIN_POLYGON) {
            in_begin = 1;
            nx = 0; ny = 0; nz = 1; /* reset normal per block */
        } else if (g_flat_cmds[i].type == CMD_END) {
            in_begin = 0;
        } else if (g_flat_cmds[i].type == CMD_NORMAL3F ||
                   g_flat_cmds[i].type == CMD_TESS_NORMAL) {
            nx = g_flat_cmds[i].args[0]; ny = g_flat_cmds[i].args[1];
            nz = g_flat_cmds[i].args[2];
        } else if (g_flat_cmds[i].type == CMD_VERTEX3F ||
                   g_flat_cmds[i].type == CMD_TESS_VERTEX) {
            float vx = g_flat_cmds[i].args[0], vy = g_flat_cmds[i].args[1],
                  vz = g_flat_cmds[i].args[2];
            glBegin(GL_LINES);
            glVertex3f(vx, vy, vz);
            glVertex3f(vx + nx * scale, vy + ny * scale, vz + nz * scale);
            glEnd();
            glPointSize(4.0f);
            glBegin(GL_POINTS);
            glVertex3f(vx + nx * scale, vy + ny * scale, vz + nz * scale);
            glEnd();
            glPointSize(1.0f);
        }
    }
    unwind_tracked_transform_stack(&matrix_depth);
    glPopMatrix();

    glEnable(GL_DEPTH_TEST);
    if (g_user_lighting_enabled) glEnable(GL_LIGHTING);
}

/* ========================================================================= */
/* Vertex input guides — plane / line / point for partial glVertex3f args     */
/* ========================================================================= */

/* Draw a semi-transparent plane perpendicular to the X axis at x=v */
static void draw_guide_yz_plane(float v, float sz) {
    glColor4f(0.9f, 0.65f, 0.6f, 0.72f);
    glBegin(GL_QUADS);
    glVertex3f(v, -sz, -sz); glVertex3f(v,  sz, -sz);
    glVertex3f(v,  sz,  sz); glVertex3f(v, -sz,  sz);
    glEnd();
    glColor4f(0.9f, 0.3f, 0.3f, 0.45f);
    glBegin(GL_LINE_LOOP);
    glVertex3f(v, -sz, -sz); glVertex3f(v,  sz, -sz);
    glVertex3f(v,  sz,  sz); glVertex3f(v, -sz,  sz);
    glEnd();
}


static void draw_vertex_guides(void) {
    if (!g_show_guides) return;

    /* Check current input for a partial glVertex3f( or gluVertex( */
    const char *args_str = NULL;
    if (strncmp(g_input, "glVertex3f(", 11) == 0 && g_input_len > 11)
        args_str = g_input + 11;
    else if (strncmp(g_input, "gluVertex(", 10) == 0 && g_input_len > 10)
        args_str = g_input + 10;
    if (!args_str) return;

    float vals[3];
    int n = parse_exprs(args_str, vals, 3, NULL, 0);
    if (n < 1) return;

    float sz = 3.0f;  /* half-size of guide geometry */

    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (n == 1) {
        /* One value (x) known — draw YZ plane at x=vals[0] */
        draw_guide_yz_plane(vals[0], sz);
    } else if (n == 2) {
        /* Two values (x,y) known — draw line parallel to Z at (x,y) */
        glColor4f(1.0f, 0.8f, 0.2f, 0.9f);
        glLineWidth(2.0f);
        glBegin(GL_LINES);
        glVertex3f(vals[0], vals[1], -sz);
        glVertex3f(vals[0], vals[1],  sz);
        glEnd();
        glLineWidth(1.0f);
    } else {
        int depth = glIsEnabled(GL_DEPTH_TEST);
        if (depth) glDisable(GL_DEPTH_TEST);
        /* All three values known — draw a point */
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
}

/* ========================================================================= */
/* Normal edit guides — show doubled/halved component impact                  */
/* ========================================================================= */

static void draw_normal_guides(void) {
    if (!g_show_guides) return;

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
}

/* ========================================================================= */
/* Scene lights — setup and visualization                                     */
/* ========================================================================= */

/* Set light properties (position/colors) only — enable/disable is driven
   by the user's REPL commands via execute_commands(). */
static void setup_lights(void) {
    /* Reset all lights to disabled; execute_commands() will enable them */
    for (int i = 0; i < MAX_LIGHTS; i++) {
        glDisable(g_lights[i].id);
        g_lights[i].enabled = 0;
        glLightfv(g_lights[i].id, GL_POSITION, g_lights[i].pos);
        glLightfv(g_lights[i].id, GL_DIFFUSE,  g_lights[i].diffuse);
        glLightfv(g_lights[i].id, GL_AMBIENT,  g_lights[i].ambient);
        glLightfv(g_lights[i].id, GL_SPECULAR, g_lights[i].specular);
    }
}

static void draw_lights(void) {
    if (!g_show_lights) return;

    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float breath = sinf(g_anim_time * 1.2f) * 0.5f + 0.5f;

    for (int i = 0; i < MAX_LIGHTS; i++) {
        float *d = g_lights[i].diffuse;
        float *p = g_lights[i].pos;
        int is_dir = (p[3] == 0.0f);
        int on = g_lights[i].enabled;

        /* Compute display position */
        float lx, ly, lz;
        if (is_dir) {
            float len = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
            if (len < 1e-6f) continue;
            lx = p[0] / len * 3.5f;
            ly = p[1] / len * 3.5f;
            lz = p[2] / len * 3.5f;
        } else {
            lx = p[0]; ly = p[1]; lz = p[2];
        }

        if (on) {
            /* === ENABLED: bright, glowing, animated === */
            float glow = 0.6f + breath * 0.4f;

            /* Outer glow */
            glPointSize(18.0f);
            glBegin(GL_POINTS);
            glColor4f(d[0], d[1], d[2], 0.15f * glow);
            glVertex3f(lx, ly, lz);
            glEnd();

            /* Inner core */
            glPointSize(8.0f);
            glBegin(GL_POINTS);
            glColor4f(d[0], d[1], d[2], 0.7f * glow);
            glVertex3f(lx, ly, lz);
            glEnd();

            /* White hot center */
            glPointSize(3.0f);
            glBegin(GL_POINTS);
            glColor4f(1.0f, 1.0f, 1.0f, 0.9f * glow);
            glVertex3f(lx, ly, lz);
            glEnd();

            /* Directional: ray toward origin */
            if (is_dir) {
                glEnable(GL_LINE_STIPPLE);
                glLineStipple(2, 0xAAAA);
                glLineWidth(1.0f);
                glBegin(GL_LINES);
                glColor4f(d[0], d[1], d[2], 0.35f * glow);
                glVertex3f(lx, ly, lz);
                glColor4f(d[0], d[1], d[2], 0.05f);
                glVertex3f(0, 0, 0);
                glEnd();
                glDisable(GL_LINE_STIPPLE);
            } else {
                /* Positional: radiating lines */
                float rlen = 0.25f + breath * 0.1f;
                glLineWidth(1.0f);
                glBegin(GL_LINES);
                float dirs[][3] = {
                    {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
                };
                for (int r = 0; r < 6; r++) {
                    glColor4f(d[0], d[1], d[2], 0.4f * glow);
                    glVertex3f(lx, ly, lz);
                    glColor4f(d[0], d[1], d[2], 0.0f);
                    glVertex3f(lx + dirs[r][0] * rlen,
                               ly + dirs[r][1] * rlen,
                               lz + dirs[r][2] * rlen);
                }
                glEnd();
            }

            /* Label */
            char label[8];
            snprintf(label, sizeof(label), " L%d", i);
            glColor4f(d[0] * 0.7f + 0.3f, d[1] * 0.7f + 0.3f,
                      d[2] * 0.7f + 0.3f, 0.8f);
            glRasterPos3f(lx, ly, lz);
            for (const char *c = label; *c; c++)
                glutBitmapCharacter(FONT_SMALL, (unsigned char)*c);

        } else {
            /* === DISABLED: dim grey marker with X === */

            /* Small grey dot */
            glPointSize(6.0f);
            glBegin(GL_POINTS);
            glColor4f(0.4f, 0.4f, 0.4f, 0.3f);
            glVertex3f(lx, ly, lz);
            glEnd();

            /* X cross through the light position */
            float xsz = 0.12f;
            glEnable(GL_LINE_STIPPLE);
            glLineStipple(1, 0xAAAA);
            glLineWidth(1.0f);
            glBegin(GL_LINES);
            glColor4f(0.7f, 0.2f, 0.2f, 0.45f);
            glVertex3f(lx - xsz, ly - xsz, lz);
            glVertex3f(lx + xsz, ly + xsz, lz);
            glVertex3f(lx - xsz, ly + xsz, lz);
            glVertex3f(lx + xsz, ly - xsz, lz);
            glEnd();
            glDisable(GL_LINE_STIPPLE);

            /* Dimmed label */
            char label[16];
            snprintf(label, sizeof(label), " L%d off", i);
            glColor4f(0.5f, 0.3f, 0.3f, 0.45f);
            glRasterPos3f(lx, ly, lz);
            for (const char *c = label; *c; c++)
                glutBitmapCharacter(FONT_SMALL, (unsigned char)*c);
        }
    }

    glPointSize(1.0f);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    if (g_user_lighting_enabled) glEnable(GL_LIGHTING);
}

/* ========================================================================= */
/* 3D scene render (viewport offset to the right of the code panel)           */
/* ========================================================================= */

static const char *replay_state_name(void) {
    switch ((ReplayState)g_replay_state) {
    case REPLAY_PLAYING: return "PLAYING";
    case REPLAY_PAUSED:  return "PAUSED";
    case REPLAY_DONE:    return "DONE";
    case REPLAY_OFF:
    default:
        return "OFF";
    }
}

static int begin_mode_has_outline_overlay(GLenum mode) {
    switch (mode) {
    case GL_POINTS:
    case GL_LINES:
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
        return 0;
    default:
        return 1;
    }
}

static void draw_replay_tess_preview(void) {
    if (!g_replay_active || g_replay_state == REPLAY_OFF ||
        g_replay_mode != REPLAY_MODE_VERTEX)
        return;

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
}

static void draw_replay_hud(int scene_x, int scene_y, int scene_w, int scene_h) {
    (void)scene_h;
    char line1[128];
    char line2[192];
    float progress = 0.0f;
    int hud_x = scene_x + 18;
    int hud_y = scene_y + 18;
    int hud_w = scene_w - 36;

    if (!g_replay_active || g_replay_state == REPLAY_OFF)
        return;

    if (hud_w < 220)
        hud_w = 220;
    if (g_replay_total_flat > 0)
        progress = (float)g_replay_pc / (float)g_replay_total_flat;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    glViewport(0, 0, g_win_w, g_win_h);
    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glColor4f(0.04f, 0.07f, 0.08f, 0.86f);
    draw_quad((float)hud_x, (float)hud_y, (float)hud_w, 56.0f);
    glColor4f(0.22f, 0.55f, 0.48f, 0.90f);
    draw_quad((float)hud_x, (float)(hud_y + 36), (float)hud_w, 8.0f);
    glColor4f(0.35f, 0.95f, 0.70f, 0.95f);
    draw_quad((float)hud_x, (float)(hud_y + 36), (float)hud_w * progress, 8.0f);

    snprintf(line1, sizeof(line1), "REPLAY %s | %d/%d | %.1f step/s | %s",
             replay_state_name(), g_replay_pc, g_replay_total_flat,
             g_replay_speed,
             g_replay_mode == REPLAY_MODE_VERTEX ? "Vertex" : "Polygon");
    snprintf(line2, sizeof(line2),
             "[Space] play/pause  [+/-] speed  [m] mode  [Esc] stop  [Left/Right] step");

    glColor3f(0.88f, 0.96f, 0.92f);
    draw_string((float)(hud_x + 10), (float)(hud_y + 18), line1, FONT_SMALL);
    glColor3f(0.60f, 0.78f, 0.72f);
    draw_string((float)(hud_x + 10), (float)(hud_y + 4), line2, FONT_SMALL);

    glDisable(GL_BLEND);
    end_2d();
}

void render_3d_scene(void) {
    int sc_x, sc_y, sc_w, sc_h;
    scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
    int replaying = g_replay_active && g_replay_state != REPLAY_OFF;
    int show_current_poly = g_highlight_current_poly && !replaying;
    int replay_tess_preview = replaying && g_replay_mode == REPLAY_MODE_VERTEX;
    int replay_vertex_points = replaying && g_replay_mode == REPLAY_MODE_VERTEX;
    if (sc_w < 1) sc_w = 1;
    if (sc_h < 1) sc_h = 1;

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glViewport(sc_x, sc_y, sc_w, sc_h);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    {
        /* Build a jitter-aware perspective frustum.
         * When g_accum_jitter_x/y are both 0 this is identical to
         * gluPerspective(45, aspect, 0.1, 100). */
        double near_z = 0.1, far_z = 100.0;
        double aspect  = (double)sc_w / (double)sc_h;
        double top_v   = near_z * tan(45.0 * M_PI / 360.0);
        double right_v = top_v * aspect;
        /* Convert sub-pixel offset (fraction of 1 pixel) -> frustum units */
        double dx = (double)g_accum_jitter_x * 2.0 * right_v / (double)sc_w;
        double dy = (double)g_accum_jitter_y * 2.0 * top_v   / (double)sc_h;
        glFrustum(-right_v + dx, right_v + dx,
                  -top_v   + dy, top_v   + dy,
                  near_z, far_z);
    }
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glTranslatef(g_cam_px, g_cam_py, -g_cam_dist);
    glRotatef(g_cam_rx, 1, 0, 0);
    glRotatef(g_cam_ry, 0, 1, 0);

    setup_lights();
    glDisable(GL_LIGHTING); /* baseline: disabled; execute_commands() enables if user typed it */

    GLfloat mspec[] = { 0.4f, 0.4f, 0.4f, 1.0f };
    GLfloat mshin[] = { 30.0f };
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mspec);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, mshin);

    if (g_multisample_enabled) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);
    if (g_line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);

    if (g_wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    {
        int fill_limit = g_num_flat_cmds;

        if (replay_has_active_fades())
            g_num_flat_cmds = replay_fill_base_limit();

        glPushMatrix();
        execute_commands();
        glPopMatrix();

        g_num_flat_cmds = fill_limit;

        if (replay_has_active_fades()) {
            setup_lights();
            glDisable(GL_LIGHTING);
            glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mspec);
            glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, mshin);
            if (g_multisample_enabled) glEnable(GL_MULTISAMPLE);
            else glDisable(GL_MULTISAMPLE);
            if (g_line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
            else glDisable(GL_LINE_SMOOTH);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(0.70f, 0.70f, 0.80f, 1.0f);
            if (g_wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            else glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

            glPushMatrix();
            execute_replay_fade_batches();
            glPopMatrix();
        }
    }

    if (g_wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* Draw translucent scene helpers after the main geometry so antialiased
     * edges blend against the final background color rather than the clear
     * color from earlier in the frame. */
    draw_grid();
    draw_axes();

    /* Polygon outline overlay (optional) + current-block highlight.
     * Each overlay pass is wrapped in push/pop so transforms don't bleed
     * between passes.  CMD_TRANSLATE3F is replayed within each pass so
     * outlines are positioned correctly even when transforms separate blocks. */
    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    if (g_multisample_enabled) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);
    if (g_line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POLYGON_OFFSET_LINE);
    glPolygonOffset(-1.0f, -1.0f);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    if (g_show_outlines || show_current_poly) {
        glPushMatrix();
        int in_begin = 0;
        int matrix_depth = 0;
        int block_is_current = 0;
        int tess_in_contour = 0;  /* drawing a tess contour as GL_LINE_LOOP */
        int tess_poly_is_current = 0;
        for (int i = 0; i < g_num_flat_cmds; i++) {
            if (!g_flat_cmds[i].valid) continue;

            if (is_transform_cmd(g_flat_cmds[i].type)) {
                if (!in_begin && !tess_in_contour)
                    apply_tracked_transform_cmd(&g_flat_cmds[i], &matrix_depth);
                continue;
            }
            switch (g_flat_cmds[i].type) {
            case CMD_TESS_BEGIN_CONTOUR:
                if (replay_tess_preview)
                    break;
                if (tess_in_contour) { glEnd(); glLineWidth(1.0f); }
                if (g_show_outlines || tess_poly_is_current) {
                    glLineWidth(1.5f);
                    if (tess_poly_is_current)
                        glColor3f(0.0f, 0.9f, 0.9f);
                    else
                        glColor3f(0.55f, 0.20f, 0.70f); /* violet — matches syntax color */
                    glBegin(GL_LINE_LOOP);
                    tess_in_contour = 1;
                }
                break;
            case CMD_TESS_VERTEX:
                if (replay_tess_preview)
                    break;
                if (tess_in_contour)
                    glVertex3f(g_flat_cmds[i].args[0], g_flat_cmds[i].args[1],
                               g_flat_cmds[i].args[2]);
                break;
            case CMD_TESS_END:
                if (replay_tess_preview) {
                    tess_poly_is_current = 0;
                    break;
                }
                if (tess_in_contour) {
                    glEnd(); glLineWidth(1.0f);
                    tess_in_contour = 0;
                }
                if (!tess_in_contour)
                    tess_poly_is_current = 0;
                break;
            case CMD_BEGIN: {
                int draw_outline = begin_mode_has_outline_overlay(g_flat_cmds[i].mode);
                if (in_begin) glEnd();
                /* Choose outline color: bright cyan for highlighted block, black otherwise */
                block_is_current = show_current_poly &&
                                   flat_block_matches_cursor(i, 0);
                if (block_is_current) {
                    glLineWidth(3.0f);
                    glColor3f(0.0f, 0.9f, 0.9f); /* bright cyan */
                } else if (g_show_outlines && draw_outline) {
                    glLineWidth(1.2f); // thinker lines work better for line smoothing
                    glColor3f(0.0f, 0.0f, 0.0f);
                } else {
                    in_begin = 0;
                    break;
                }
                glBegin(g_flat_cmds[i].mode);
                in_begin = 1;
                break;
            }
            case CMD_END:
                if (in_begin) {
                    glEnd(); in_begin = 0;
                    glLineWidth(1.0f);
                    glColor3f(0.0f, 0.0f, 0.0f);
                }
                block_is_current = 0;
                break;
            case CMD_VERTEX3F:
                if (in_begin) {
                    if (block_is_current || g_show_outlines)
                        glVertex3f(g_flat_cmds[i].args[0], g_flat_cmds[i].args[1],
                                   g_flat_cmds[i].args[2]);
                }
                break;
            case CMD_TESS_BEGIN_POLYGON:
                tess_poly_is_current = show_current_poly &&
                                       flat_block_matches_cursor(i, 1);
                break;
            default: break;
            }
        }
        if (in_begin) { glEnd(); glLineWidth(1.0f); }
        if (tess_in_contour) { glEnd(); glLineWidth(1.0f); }
        unwind_tracked_transform_stack(&matrix_depth);
        glPopMatrix();
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_POLYGON_OFFSET_LINE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    /* Vertex dots — replay transforms so dots match the filled geometry */
    glPushMatrix();
    {
    int matrix_depth = 0;
    glPointSize(5.0f);
    if (replay_vertex_points)
        glColor3f(1.0f, 0.88f, 0.20f);
    else
        glColor3f(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < g_num_flat_cmds; i++) {
        if (!g_flat_cmds[i].valid) continue;
        int is_cursor = (g_flat_cmds[i].src_cmd_idx == g_edit_line);

        // Draw vertex guides and normals if this vertex is at the cursor
        // position. Do this inline so that the current model matrix is applied
        // to the guides, ensuring they are positioned correctly even when
        // transforms separate blocks.
        if (is_cursor && !replaying) {
            draw_vertex_guides();
            draw_normal_guides();
        }

        if (is_transform_cmd(g_flat_cmds[i].type)) {
            apply_tracked_transform_cmd(&g_flat_cmds[i], &matrix_depth);
        } else if (g_flat_cmds[i].type == CMD_VERTEX3F ||
                   g_flat_cmds[i].type == CMD_TESS_VERTEX) {
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

    if (replay_tess_preview)
        draw_replay_tess_preview();

    draw_lights();

    if (g_show_vnums)   draw_vertex_numbers();
    if (g_show_normals) draw_normal_vectors();
    if (replaying)
        draw_replay_hud(sc_x, sc_y, sc_w, sc_h);
    glPopAttrib();
}
