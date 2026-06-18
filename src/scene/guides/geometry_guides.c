/*
 * geometry_guides.c - vertex/normal edit-guide rendering.
 */
#include "geometry_guides.h"
#include "scene/palette.h"
#include "scene/occluded_ghost.h"  /* SCENE_OCCLUDED_GHOST_STIPPLE */

#include <math.h>   /* sqrtf, fminf, fmodf, cosf, sinf */
#include <string.h> /* strncmp */

#define GEOMETRY_GUIDE_VERTEX_MARK_POINT_SIZE 8.0f
#define GEOMETRY_GUIDE_NORMAL_POINT_SIZE 8.0f

static void geometry_guides_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void geometry_guides_pop_state(void) {
    glPopAttrib();
}

/* Per-axis fill and edge color tokens, indexed by the constrained axis
 * (0=X → YZ plane / red, 1=Y → XZ plane / green, 2=Z → XY plane /
 * blue). Used by draw_guide_axis_plane below. */
static const SceneColorToken k_guide_plane_fill[3] = {
    SCENE_CLR_GUIDE_PLANE_X_FILL,
    SCENE_CLR_GUIDE_PLANE_Y_FILL,
    SCENE_CLR_GUIDE_PLANE_Z_FILL,
};
static const SceneColorToken k_guide_plane_edge[3] = {
    SCENE_CLR_GUIDE_PLANE_X_EDGE,
    SCENE_CLR_GUIDE_PLANE_Y_EDGE,
    SCENE_CLR_GUIDE_PLANE_Z_EDGE,
};

/* Draw a semi-transparent plane perpendicular to `free_axis` at
 * coordinate v. `free_axis` ∈ {0=X, 1=Y, 2=Z}; the other two axes span
 * the [-sz, +sz] face. `as` is the snapshot's alpha-scale boost. The
 * three call sites used to be near-identical 13-line yz/xz/xy_plane
 * helpers — this one indexes the per-axis tokens and computes the four
 * corner vertices from a single basis. */
static void draw_guide_axis_plane(int free_axis, float v, float sz, float as) {
    /* Corner offsets in the basis (a, b) of the free face. */
    static const float corners[4][2] = {
        { -1.0f, -1.0f },
        { +1.0f, -1.0f },
        { +1.0f, +1.0f },
        { -1.0f, +1.0f },
    };
    /* The free axis is held at v; the other two axes get the corner
     * offsets * sz. This indexing table picks (a-axis, b-axis) for
     * each free axis. */
    int a_axis, b_axis;
    if (free_axis == 0)      { a_axis = 1; b_axis = 2; } /* YZ plane */
    else if (free_axis == 1) { a_axis = 0; b_axis = 2; } /* XZ plane */
    else                     { a_axis = 0; b_axis = 1; } /* XY plane */

    scene_clr_a(k_guide_plane_fill[free_axis], fminf(0.40f * as, 1.0f));
    glBegin(GL_QUADS);
    for (int i = 0; i < 4; i++) {
        float p[3];
        p[free_axis] = v;
        p[a_axis] = corners[i][0] * sz;
        p[b_axis] = corners[i][1] * sz;
        glVertex3f(p[0], p[1], p[2]);
    }
    glEnd();

    scene_clr_a(k_guide_plane_edge[free_axis], fminf(0.25f * as, 1.0f));
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < 4; i++) {
        float p[3];
        p[free_axis] = v;
        p[a_axis] = corners[i][0] * sz;
        p[b_axis] = corners[i][1] * sz;
        glVertex3f(p[0], p[1], p[2]);
    }
    glEnd();
}

/* Per-free-axis color token for the n==2 line guide, indexed by the
 * single still-unconstrained axis (0=X, 1=Y, 2=Z). */
static const SceneColorToken k_guide_line_clr[3] = {
    SCENE_CLR_GUIDE_LINE_X,
    SCENE_CLR_GUIDE_LINE_Y,
    SCENE_CLR_GUIDE_LINE_Z,
};

/* Recognize whether the partial input line is a vertex command. Returns
 * 1 for glVertex3f / glVertex2f / gluVertex (with at least one char past
 * the open paren), 0 otherwise. Sets *is_vertex2f for the glVertex2f
 * form, whose two args already pin a complete 2D vertex at z=0. */
static int input_is_vertex_kind(const SceneGuideSnapshot *snapshot,
                                int *is_vertex2f) {
    *is_vertex2f = 0;
    if (strncmp(snapshot->input, "glVertex3f(", 11) == 0 &&
        snapshot->input_len > 11)
        return 1;
    if (strncmp(snapshot->input, "glVertex2f(", 11) == 0 &&
        snapshot->input_len > 11) {
        *is_vertex2f = 1;
        return 1;
    }
    if (strncmp(snapshot->input, "gluVertex(", 10) == 0 &&
        snapshot->input_len > 10)
        return 1;
    return 0;
}

/* Solid point marker at (x,y,z) for a fully-determined vertex (0 DOF).
 * Depth test is temporarily disabled so the mark stays visible through
 * scene geometry. Caller sets up blend state. */
static void draw_vertex_point_marker(float x, float y, float z) {
    int depth = glIsEnabled(GL_DEPTH_TEST);
    if (depth) glDisable(GL_DEPTH_TEST);
    scene_clr_a(SCENE_CLR_GUIDE_VERTEX_MARK, 0.9f);
    glPointSize(GEOMETRY_GUIDE_VERTEX_MARK_POINT_SIZE);
    glBegin(GL_POINTS);
    glVertex3f(x, y, z);
    glEnd();
    glPointSize(1.0f);
    if (depth) glEnable(GL_DEPTH_TEST);
}

/* Line locus of a vertex with one coordinate still untyped (1 DOF): the
 * two filled axes are pinned to vals[], and `free_axis` sweeps the
 * [-sz, +sz] range. */
static void draw_vertex_line_guide(int free_axis, const float vals[3], float sz) {
    float a[3] = { vals[0], vals[1], vals[2] };
    float b[3] = { vals[0], vals[1], vals[2] };
    a[free_axis] = -sz;
    b[free_axis] =  sz;

    scene_clr_a(k_guide_line_clr[free_axis], 0.9f);
    glLineWidth(2.0f);
    glBegin(GL_LINES);
    glVertex3f(a[0], a[1], a[2]);
    glVertex3f(b[0], b[1], b[2]);
    glEnd();
    glLineWidth(1.0f);
}

/*
 * Edit guide for the vertex command under the cursor, visualizing the
 * degrees of freedom (DOF) still open as the user types its coordinates.
 *
 * snapshot->vertex_n_filled (see SceneGuideSnapshot) is how many of the
 * comma-separated coordinate slots have a value so far; vertex_filled[]
 * flags which specific axes those are (both pre-evaluated by the
 * controller, so this module never touches repl_eval). The guide shows
 * where the vertex *could* land given what's typed:
 *
 *   1 coord typed  -> 2 DOF -> a plane (perpendicular to the typed axis)
 *   2 coords typed -> 1 DOF -> a line  (sweeping the one untyped axis)
 *   all coords     -> 0 DOF -> a point (the exact vertex)
 *
 * glVertex2f is special-cased: two typed slots already pin a complete 2D
 * vertex at z=0, so it draws a point rather than a line.
 */
static void draw_vertex_guides(const SceneGuideSnapshot *snapshot) {
    if (!snapshot->show_guides)
        return;

    int is_vertex2f = 0;
    if (!input_is_vertex_kind(snapshot, &is_vertex2f) ||
        snapshot->vertex_n_filled < 1)
        return;

    int n = snapshot->vertex_n_filled;
    float vals[3] = { snapshot->vertex_args[0], snapshot->vertex_args[1],
                      snapshot->vertex_args[2] };
    const int *filled = snapshot->vertex_filled;
    float sz = 3.0f;
    float as = snapshot->alpha_scale;

    geometry_guides_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    /* Additive blend so guides stay visible over dark backgrounds and
     * overlapping guide elements reinforce rather than occlude. */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    if (n == 2 && is_vertex2f) {
        /* 0 DOF: x,y typed for a 2D vertex => complete vertex at z=0. */
        draw_vertex_point_marker(vals[0], vals[1], 0.0f);
    } else if (n == 1) {
        /* 2 DOF: one axis typed => plane perpendicular to it. */
        if      (filled[0]) draw_guide_axis_plane(0, vals[0], sz, as);
        else if (filled[1]) draw_guide_axis_plane(1, vals[1], sz, as);
        else if (filled[2]) draw_guide_axis_plane(2, vals[2], sz, as);
    } else if (n == 2) {
        /* 1 DOF: two axes typed => line along the remaining free axis. */
        int free_axis = !filled[2] ? 2 : (!filled[1] ? 1 : 0);
        draw_vertex_line_guide(free_axis, vals, sz);
    } else {
        /* 0 DOF: all coords typed => exact vertex point. */
        draw_vertex_point_marker(vals[0], vals[1], vals[2]);
    }

    glDisable(GL_BLEND);
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

    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    int found = 0;

    /* Prefer the live (flat-program) anchor position when the
     * controller has supplied one — it's re-evaluated every frame, so
     * it tracks dynamic vars (e.g. waves' `x = -b/2 + b*j/n` inside a
     * loop). The source-cmd fallback below is parse-time-frozen and
     * lands the arrow at the literal source coords. */
    if (snapshot->normal_base_pos_valid) {
        vx = snapshot->normal_base_pos[0];
        vy = snapshot->normal_base_pos[1];
        vz = snapshot->normal_base_pos[2];
        found = 1;
    } else {
        int search_start =
            (snapshot->edit_line_idx < snapshot->source_cmd_count &&
             !snapshot->inserting)
                ? snapshot->edit_line_idx + 1
                : snapshot->edit_line_idx;
        for (int i = search_start; i < snapshot->source_cmd_count; i++) {
            const GLCmd *cmd = &snapshot->source_cmds[i];
            if (!cmd->valid) continue;
            if (repl_cmd_emits_vertex(cmd->type)) {
                /* For glVertex2f, cmd->args[2] is zero (parser leaves
                 * it default-initialised) — the right z-value for a
                 * 2D vertex used as a guide reference. */
                vx = cmd->args[0];
                vy = cmd->args[1];
                vz = cmd->args[2];
                found = 1;
                break;
            }
            if (cmd->type == CMD_END || cmd->type == CMD_BEGIN ||
                cmd->type == CMD_TESS_END ||
                cmd->type == CMD_TESS_BEGIN_POLYGON) {
                break;
            }
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
        scene_clr_a(SCENE_CLR_GUIDE_NORMAL_BASE, fminf(0.4f * as, 1.0f));
        glLineWidth(3.0f);
        glBegin(GL_LINES);
        glVertex3f(vx, vy, vz);
        glVertex3f(vx + cn[0]*scale, vy + cn[1]*scale, vz + cn[2]*scale);
        glEnd();
    }

    glEnable(GL_LINE_STIPPLE);
    glLineStipple(1, SCENE_OCCLUDED_GHOST_STIPPLE);
    glLineWidth(4.0f);

    scene_clr_a(SCENE_CLR_GUIDE_NORMAL_DOUBLE, 0.75f);
    glBegin(GL_LINES);
    glVertex3f(vx, vy, vz);
    glVertex3f(vx + doubled[0]*scale, vy + doubled[1]*scale,
               vz + doubled[2]*scale);
    glEnd();

    scene_clr_a(SCENE_CLR_GUIDE_NORMAL_HALF, 0.75f);
    glBegin(GL_LINES);
    glVertex3f(vx, vy, vz);
    glVertex3f(vx + halved[0]*scale, vy + halved[1]*scale,
               vz + halved[2]*scale);
    glEnd();

    glDisable(GL_LINE_STIPPLE);
    glLineWidth(1.0f);

    glPointSize(GEOMETRY_GUIDE_NORMAL_POINT_SIZE);
    glBegin(GL_POINTS);
    scene_clr_a(SCENE_CLR_GUIDE_NORMAL_DOUBLE, 0.85f);
    glVertex3f(vx + doubled[0]*scale, vy + doubled[1]*scale,
               vz + doubled[2]*scale);
    scene_clr_a(SCENE_CLR_GUIDE_NORMAL_HALF, 0.85f);
    glVertex3f(vx + halved[0]*scale, vy + halved[1]*scale,
               vz + halved[2]*scale);
    glEnd();
    glPointSize(1.0f);

    glDisable(GL_BLEND);
    geometry_guides_pop_state();
}

void scene_geometry_guides_render_for_cursor(const SceneGuideSnapshot *snapshot) {
    if (!snapshot)
        return;
    draw_vertex_guides(snapshot);
    draw_normal_guides(snapshot);
}
