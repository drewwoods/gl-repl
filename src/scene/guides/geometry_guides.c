/*
 * scene_geometry_guides.c - vertex/normal edit-guide rendering.
 */
#include "geometry_guides.h"

static void geometry_guides_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void geometry_guides_pop_state(void) {
    glPopAttrib();
}

/* Draw a semi-transparent plane perpendicular to the X axis at x=v (red) */
static void draw_guide_yz_plane(float v, float sz, float as) {
    glColor4f(0.90f, 0.65f, 0.60f, fminf(0.40f * as, 1.0f));
    glBegin(GL_QUADS);
    glVertex3f(v, -sz, -sz); glVertex3f(v,  sz, -sz);
    glVertex3f(v,  sz,  sz); glVertex3f(v, -sz,  sz);
    glEnd();
    glColor4f(0.90f, 0.30f, 0.30f, fminf(0.25f * as, 1.0f));
    glBegin(GL_LINE_LOOP);
    glVertex3f(v, -sz, -sz); glVertex3f(v,  sz, -sz);
    glVertex3f(v,  sz,  sz); glVertex3f(v, -sz,  sz);
    glEnd();
}

/* Draw a semi-transparent plane perpendicular to the Y axis at y=v (green) */
static void draw_guide_xz_plane(float v, float sz, float as) {
    glColor4f(0.65f, 0.90f, 0.60f, fminf(0.40f * as, 1.0f));
    glBegin(GL_QUADS);
    glVertex3f(-sz, v, -sz); glVertex3f( sz, v, -sz);
    glVertex3f( sz, v,  sz); glVertex3f(-sz, v,  sz);
    glEnd();
    glColor4f(0.30f, 0.70f, 0.30f, fminf(0.25f * as, 1.0f));
    glBegin(GL_LINE_LOOP);
    glVertex3f(-sz, v, -sz); glVertex3f( sz, v, -sz);
    glVertex3f( sz, v,  sz); glVertex3f(-sz, v,  sz);
    glEnd();
}

/* Draw a semi-transparent plane perpendicular to the Z axis at z=v (blue) */
static void draw_guide_xy_plane(float v, float sz, float as) {
    glColor4f(0.60f, 0.65f, 0.90f, fminf(0.40f * as, 1.0f));
    glBegin(GL_QUADS);
    glVertex3f(-sz, -sz, v); glVertex3f( sz, -sz, v);
    glVertex3f( sz,  sz, v); glVertex3f(-sz,  sz, v);
    glEnd();
    glColor4f(0.30f, 0.30f, 0.80f, fminf(0.25f * as, 1.0f));
    glBegin(GL_LINE_LOOP);
    glVertex3f(-sz, -sz, v); glVertex3f( sz, -sz, v);
    glVertex3f( sz,  sz, v); glVertex3f(-sz,  sz, v);
    glEnd();
}

static void draw_vertex_guides(const SceneGuideSnapshot *snapshot) {
    if (!snapshot->show_guides)
        return;

    /* Whether the input is glVertex2f vs the 3-arg variants matters for the
     * "two slots filled = complete 2D vertex at z=0" branch below. The
     * argument values themselves arrive pre-parsed in snapshot->vertex_args
     * so the scene module no longer needs to evaluate REPL expressions. */
    int is_vertex2f = 0;
    int is_vertex_kind = 0;
    if (strncmp(snapshot->input, "glVertex3f(", 11) == 0 &&
        snapshot->input_len > 11) {
        is_vertex_kind = 1;
    } else if (strncmp(snapshot->input, "glVertex2f(", 11) == 0 &&
               snapshot->input_len > 11) {
        is_vertex_kind = 1;
        is_vertex2f = 1;
    } else if (strncmp(snapshot->input, "gluVertex(", 10) == 0 &&
               snapshot->input_len > 10) {
        is_vertex_kind = 1;
    }
    if (!is_vertex_kind || snapshot->vertex_n_filled < 1)
        return;

    int n = snapshot->vertex_n_filled;
    float vals[3] = { snapshot->vertex_args[0], snapshot->vertex_args[1],
                      snapshot->vertex_args[2] };
    int   filled[3] = { snapshot->vertex_filled[0], snapshot->vertex_filled[1],
                        snapshot->vertex_filled[2] };

    float sz = 3.0f;
    float as = snapshot->alpha_scale;

    geometry_guides_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    /* use additive blending to make guides more visible over dark backgrounds, and
     * also to make overlapping guide elements more visible */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    if (n == 2 && is_vertex2f) {
        /* Both x,y filled for glVertex2f — treat as a complete vertex at z=0 */
        int depth = glIsEnabled(GL_DEPTH_TEST);
        if (depth) glDisable(GL_DEPTH_TEST);
        glColor4f(1.0f, 0.3f, 0.3f, 0.9f);
        glPointSize(8.0f);
        glBegin(GL_POINTS);
        glVertex3f(vals[0], vals[1], 0.0f);
        glEnd();
        glPointSize(1.0f);
        if (depth) glEnable(GL_DEPTH_TEST);
    } else if (n == 1) {
        if      (filled[0]) draw_guide_yz_plane(vals[0], sz, as);
        else if (filled[1]) draw_guide_xz_plane(vals[1], sz, as);
        else if (filled[2]) draw_guide_xy_plane(vals[2], sz, as);
    } else if (n == 2) {
        glLineWidth(2.0f);
        if (!filled[2]) {
            glColor4f(1.0f, 0.80f, 0.20f, 0.9f);
            glBegin(GL_LINES);
            glVertex3f(vals[0], vals[1], -sz);
            glVertex3f(vals[0], vals[1],  sz);
            glEnd();
        } else if (!filled[1]) {
            glColor4f(0.30f, 0.90f, 0.30f, 0.9f);
            glBegin(GL_LINES);
            glVertex3f(vals[0], -sz, vals[2]);
            glVertex3f(vals[0],  sz, vals[2]);
            glEnd();
        } else {
            glColor4f(1.0f, 0.35f, 0.35f, 0.9f);
            glBegin(GL_LINES);
            glVertex3f(-sz, vals[1], vals[2]);
            glVertex3f( sz, vals[1], vals[2]);
            glEnd();
        }
        glLineWidth(1.0f);
    } else {
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
    if (snapshot->user_lighting_enabled) glEnable(GL_LIGHTING);
    geometry_guides_pop_state();
}

static void draw_normal_guides(const SceneGuideSnapshot *snapshot) {
    if (!snapshot->show_guides)
        return;

    int paren_pos = 0;
    if (strncmp(snapshot->input, "glNormal3f(", 11) == 0 &&
        snapshot->input_len > 11) {
        paren_pos = 11;
    } else if (strncmp(snapshot->input, "gluNormal(", 10) == 0 &&
               snapshot->input_len > 10) {
        paren_pos = 10;
    } else {
        return;
    }

    /* Pre-parsed by the controller (see glr_ctrl_build_guide_snapshot). */
    if (snapshot->normal_n_filled < 3 || snapshot->cursor_pos < paren_pos)
        return;
    float vals[3] = { snapshot->normal_args[0], snapshot->normal_args[1],
                      snapshot->normal_args[2] };

    int close = snapshot->input_len;
    for (int ci = paren_pos; ci < snapshot->input_len; ci++) {
        if (snapshot->input[ci] == ')') {
            close = ci;
            break;
        }
    }

    int effective_cursor = (snapshot->cursor_pos > close)
                         ? close
                         : snapshot->cursor_pos;

    int component = 0;
    for (int ci = paren_pos; ci < effective_cursor; ci++) {
        if (snapshot->input[ci] == ',')
            component++;
    }
    if (component > 2)
        component = 2;

    int search_start = (snapshot->edit_line_idx < snapshot->source_cmd_count &&
                        !snapshot->inserting)
                     ? snapshot->edit_line_idx + 1
                     : snapshot->edit_line_idx;
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    int found = 0;
    for (int i = search_start; i < snapshot->source_cmd_count; i++) {
        const GLCmd *cmd = &snapshot->source_cmds[i];
        if (!cmd->valid) continue;
        if (cmd->type == CMD_VERTEX3F || cmd->type == CMD_TESS_VERTEX) {
            vx = cmd->args[0];
            vy = cmd->args[1];
            vz = cmd->args[2];
            found = 1;
            break;
        }
        if (cmd->type == CMD_END || cmd->type == CMD_BEGIN ||
            cmd->type == CMD_TESS_END || cmd->type == CMD_TESS_BEGIN_POLYGON) {
            break;
        }
    }
    if (!found)
        return;

    float doubled[3] = { vals[0], vals[1], vals[2] };
    float halved[3]  = { vals[0], vals[1], vals[2] };
    doubled[component] *= 2.0f;
    halved[component]  *= 0.5f;

    float dlen = sqrtf(doubled[0]*doubled[0] + doubled[1]*doubled[1] +
                       doubled[2]*doubled[2]);
    if (dlen > 1e-8f) {
        doubled[0] /= dlen;
        doubled[1] /= dlen;
        doubled[2] /= dlen;
    }
    float hlen = sqrtf(halved[0]*halved[0] + halved[1]*halved[1] +
                       halved[2]*halved[2]);
    if (hlen > 1e-8f) {
        halved[0] /= hlen;
        halved[1] /= hlen;
        halved[2] /= hlen;
    }

    float scale = 0.45f;

    geometry_guides_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float clen = sqrtf(vals[0]*vals[0] + vals[1]*vals[1] + vals[2]*vals[2]);
    float as = snapshot->alpha_scale;
    if (clen > 1e-8f) {
        float cn[3] = { vals[0]/clen, vals[1]/clen, vals[2]/clen };
        glColor4f(0.8f, 0.8f, 0.8f, fminf(0.4f * as, 1.0f));
        glLineWidth(3.0f);
        glBegin(GL_LINES);
        glVertex3f(vx, vy, vz);
        glVertex3f(vx + cn[0]*scale, vy + cn[1]*scale, vz + cn[2]*scale);
        glEnd();
    }

    glEnable(GL_LINE_STIPPLE);
    glLineStipple(1, 0xAAAA);
    glLineWidth(4.0f);

    glColor4f(0.2f, 0.95f, 0.2f, 0.75f);
    glBegin(GL_LINES);
    glVertex3f(vx, vy, vz);
    glVertex3f(vx + doubled[0]*scale, vy + doubled[1]*scale,
               vz + doubled[2]*scale);
    glEnd();

    glColor4f(0.95f, 0.2f, 0.2f, 0.75f);
    glBegin(GL_LINES);
    glVertex3f(vx, vy, vz);
    glVertex3f(vx + halved[0]*scale, vy + halved[1]*scale,
               vz + halved[2]*scale);
    glEnd();

    glDisable(GL_LINE_STIPPLE);
    glLineWidth(1.0f);

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
    if (snapshot->user_lighting_enabled) glEnable(GL_LIGHTING);
    geometry_guides_pop_state();
}

void geometry_guides_render_for_cursor(const SceneGuideSnapshot *snapshot) {
    if (!snapshot)
        return;
    draw_vertex_guides(snapshot);
    draw_normal_guides(snapshot);
}