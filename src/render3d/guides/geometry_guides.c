/*
 * geometry_guides.c - vertex/normal edit-guide rendering.
 */
#include "geometry_guides.h"
#include "render3d/palette.h"
#include "render3d/occluded_ghost.h"  /* RENDER3D_OCCLUDED_GHOST_STIPPLE */
#include "render3d/overlays.h"

#include <math.h>   /* sqrtf, fminf, fabsf */
#include <stdio.h>  /* snprintf */
#include <string.h> /* strncmp */

#define GEOMETRY_GUIDE_VERTEX_MARK_POINT_SIZE 8.0f
/* Breathing pulse for the full-vertex marker, in rad/s on the
 * free-running anim clock (~2s per breath) so the highlight stays
 * alive while t is paused. */
#define GEOMETRY_GUIDE_VERTEX_MARK_BREATH_RATE 3.0f

static void geometry_guides_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void geometry_guides_pop_state(void) {
    glPopAttrib();
}

/* Per-axis fill and edge color tokens, indexed by the constrained axis
 * (0=X -> YZ plane / red, 1=Y -> XZ plane / green, 2=Z -> XY plane /
 * blue). Used by draw_guide_axis_plane below. */
static const Render3dColorToken k_guide_plane_fill[3] = {
    RENDER3D_CLR_GUIDE_PLANE_X_FILL,
    RENDER3D_CLR_GUIDE_PLANE_Y_FILL,
    RENDER3D_CLR_GUIDE_PLANE_Z_FILL,
};
static const Render3dColorToken k_guide_plane_edge[3] = {
    RENDER3D_CLR_GUIDE_PLANE_X_EDGE,
    RENDER3D_CLR_GUIDE_PLANE_Y_EDGE,
    RENDER3D_CLR_GUIDE_PLANE_Z_EDGE,
};

/* Axis letters for guide labels, indexed like the color tables. */
static const char k_guide_axis_letter[3] = { 'x', 'y', 'z' };

/* Draw a semi-transparent "coordinate paper" sheet perpendicular to
 * `free_axis` at coordinate v. `free_axis` in {0=X, 1=Y, 2=Z}; the
 * other two axes span the [-sz, +sz] face. `as` is the snapshot's
 * alpha-scale boost. Same soft-glass language as the clip-plane
 * guide, keeping the per-axis color identity and the square shape
 * (axis-aligned sheets are square; arbitrary clip planes are round):
 *
 *   - a radially-fading fill (fan from the sheet center, so the
 *     wash stays localized instead of flooding the whole scene)
 *   - integer grid chords with mid-peak alpha - the sheet reads as
 *     graph paper, so the two open coordinates can be eyeballed;
 *     the two zero lines (the in-plane axes) render brighter
 *   - a solid rim under depth test plus a stippled ghost rim with
 *     depth off (shared occluded-ghost dash language)
 *   - an "x=1.2"-style readout naming the typed coordinate
 */
static void draw_guide_axis_plane(const Render3dGuideSnapshot *snapshot,
                                  int free_axis, float v, float sz, float as) {
    int a_axis, b_axis;
    if (free_axis == 0)      { a_axis = 1; b_axis = 2; } /* YZ plane */
    else if (free_axis == 1) { a_axis = 0; b_axis = 2; } /* XZ plane */
    else                     { a_axis = 0; b_axis = 1; } /* XY plane */

    /* Square perimeter, corners + edge midpoints, wound as one loop.
     * Offsets in the (a, b) basis of the free face. */
    static const float perim[8][2] = {
        { -1.0f, -1.0f }, {  0.0f, -1.0f }, { +1.0f, -1.0f },
        { +1.0f,  0.0f }, { +1.0f, +1.0f }, {  0.0f, +1.0f },
        { -1.0f, +1.0f }, { -1.0f,  0.0f },
    };

#define GUIDE_PLANE_POINT(pa_, pb_) do {              \
        float p_[3];                                   \
        p_[free_axis] = v;                             \
        p_[a_axis] = (pa_);                            \
        p_[b_axis] = (pb_);                            \
        glVertex3f(p_[0], p_[1], p_[2]);               \
    } while (0)

    glShadeModel(GL_SMOOTH);

    /* Radial-fade fill: bright at the sheet center, near-clear rim. */
    glBegin(GL_TRIANGLE_FAN);
    render3d_clr_a(k_guide_plane_fill[free_axis], fminf(0.30f * as, 1.0f));
    GUIDE_PLANE_POINT(0.0f, 0.0f);
    render3d_clr_a(k_guide_plane_fill[free_axis], 0.03f * as);
    for (int i = 0; i <= 8; i++)
        GUIDE_PLANE_POINT(perim[i % 8][0] * sz, perim[i % 8][1] * sz);
    glEnd();

    /* Integer grid chords, mid-peak alpha fading to 0 at the rim.
     * The o == 0 pair are the in-plane axes - drawn brighter. */
    glLineWidth(1.0f);
    for (int axis = 0; axis < 2; axis++) {
        for (int gi = (int)-sz + 1; gi <= (int)sz - 1; gi++) {
            float o = (float)gi;
            float mid_alpha = fminf((gi == 0 ? 0.45f : 0.22f) * as, 1.0f);
            glBegin(GL_LINE_STRIP);
            render3d_clr_a(k_guide_plane_edge[free_axis], 0.0f);
            if (axis) GUIDE_PLANE_POINT(o, -sz); else GUIDE_PLANE_POINT(-sz, o);
            render3d_clr_a(k_guide_plane_edge[free_axis], mid_alpha);
            if (axis) GUIDE_PLANE_POINT(o, 0.0f); else GUIDE_PLANE_POINT(0.0f, o);
            render3d_clr_a(k_guide_plane_edge[free_axis], 0.0f);
            if (axis) GUIDE_PLANE_POINT(o, sz); else GUIDE_PLANE_POINT(sz, o);
            glEnd();
        }
    }

    /* Rim, two passes: solid under depth test, stippled ghost over. */
    for (int ghost = 0; ghost < 2; ghost++) {
        int depth_was = glIsEnabled(GL_DEPTH_TEST);
        if (ghost) {
            if (depth_was) glDisable(GL_DEPTH_TEST);
            glEnable(GL_LINE_STIPPLE);
            glLineStipple(1, RENDER3D_OCCLUDED_GHOST_STIPPLE);
            render3d_clr_a(k_guide_plane_edge[free_axis],
                           fminf(0.20f * as, 1.0f));
        } else {
            render3d_clr_a(k_guide_plane_edge[free_axis],
                           fminf(0.45f * as, 1.0f));
        }
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < 8; i++)
            GUIDE_PLANE_POINT(perim[i][0] * sz, perim[i][1] * sz);
        glEnd();
        if (ghost) {
            glDisable(GL_LINE_STIPPLE);
            if (depth_was) glEnable(GL_DEPTH_TEST);
        }
    }

    /* Typed-coordinate readout at the sheet's top edge (depth off so
     * it stays legible; the outer push/pop attrib restores). "Top"
     * prefers the in-plane world-up axis when the sheet has one, so
     * the label floats above the scene instead of projecting into the
     * lower screen corners. */
    {
        char label[32];
        float p[3];
        int up_axis = (a_axis == 1) ? a_axis : b_axis;
        int side_axis = (up_axis == a_axis) ? b_axis : a_axis;
        snprintf(label, sizeof(label), "%c = %g",
                 k_guide_axis_letter[free_axis], (double)v);
        p[free_axis] = v;
        /* Mid-height keeps the readout inside the default camera's
         * view (the sheet's top edge projects off-screen); the small
         * sideways nudge lifts it off the center gridline. */
        p[up_axis] = sz * 0.55f;
        p[side_axis] = sz * 0.08f;
        glDisable(GL_DEPTH_TEST);
        render3d_clr_a(k_guide_plane_edge[free_axis], fminf(0.9f * as, 1.0f));
        render3d_guide_record_label(snapshot, p, FONT_SMALL, label, NULL, NULL);
        render3d_draw_bitmap_text(FONT_SMALL, p[0], p[1], p[2], label);
    }

#undef GUIDE_PLANE_POINT
}

/* Per-free-axis color token for the n==2 line guide, indexed by the
 * single still-unconstrained axis (0=X, 1=Y, 2=Z). */
static const Render3dColorToken k_guide_line_clr[3] = {
    RENDER3D_CLR_GUIDE_LINE_X,
    RENDER3D_CLR_GUIDE_LINE_Y,
    RENDER3D_CLR_GUIDE_LINE_Z,
};

/* Recognize whether the partial input line is a vertex command. Returns
 * 1 for glVertex3f / glVertex2f / gluVertex (with at least one char past
 * the open paren), 0 otherwise. Sets *is_vertex2f for the glVertex2f
 * form, whose two args already pin a complete 2D vertex at z=0. */
static int input_is_vertex_kind(const Render3dGuideSnapshot *snapshot,
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

/* Solid marker at (x,y,z) for a fully-determined vertex (0 DOF), breathing
 * on the free-running anim clock so it reads as "this is the live vertex"
 * at a glance: a soft round halo swells and fades around a gently pulsing
 * core. Depth test is temporarily disabled so the mark stays visible through
 * scene geometry. Caller sets up blend state (additive, so the halo glows
 * over the scene). */
static void draw_vertex_point_marker(const Render3dGuideSnapshot *snapshot,
                                     float x, float y, float z) {
    float as = snapshot->alpha_scale;
    float breath = 0.5f + 0.5f *
        sinf(snapshot->anim_time * GEOMETRY_GUIDE_VERTEX_MARK_BREATH_RATE);
    int depth = glIsEnabled(GL_DEPTH_TEST);
    if (depth) glDisable(GL_DEPTH_TEST);
    /* Desktop GL rasterizes points as circles under multisample rasterization,
     * so the native build barely needs this - gl4es has no such implicit path
     * and honours GL_POINT_SMOOTH only when it is explicitly enabled (its
     * fixed-pipeline shader derives coverage from gl_PointCoord; see
     * packaging/web/patches/gl4es-point-smooth.patch). */
    glEnable(GL_POINT_SMOOTH);
    glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);

    /* Halo: large, faint, swelling with the breath. */
    render3d_clr_a(RENDER3D_CLR_GUIDE_VERTEX_MARK,
                   fminf((0.15f + 0.25f * breath) * as, 1.0f));
    glPointSize(GEOMETRY_GUIDE_VERTEX_MARK_POINT_SIZE * (1.7f + 1.1f * breath));
    glBegin(GL_POINTS);
    glVertex3f(x, y, z);
    glEnd();

    /* Core: bright, with a subtler swell so it never shrinks away. */
    render3d_clr_a(RENDER3D_CLR_GUIDE_VERTEX_MARK, fminf(0.9f * as, 1.0f));
    glPointSize(GEOMETRY_GUIDE_VERTEX_MARK_POINT_SIZE * (1.0f + 0.35f * breath));
    glBegin(GL_POINTS);
    glVertex3f(x, y, z);
    glEnd();

    /* No glPointSize(1) reset: both callers bracket this in
     * geometry_guides_push_state()'s glPushAttrib(GL_ALL_ATTRIB_BITS), which
     * restores the size. Resetting it here would overwrite the marker size. */
    glDisable(GL_POINT_SMOOTH);
    if (depth) glEnable(GL_DEPTH_TEST);
}

/* Line locus of a vertex with one coordinate still untyped (1 DOF): the
 * two filled axes are pinned to vals[], and `free_axis` sweeps the
 * [-sz, +sz] range. Polished to the same soft language as the sheet:
 * the line's alpha peaks where the free coordinate crosses 0 and fades
 * to nothing at the ends (no more hard-cut bar), integer tick dots
 * make the open coordinate readable, a stippled depth-off ghost keeps
 * the locus visible through geometry, and the still-free axis is named
 * at the line's positive end. */
static void draw_vertex_line_guide(const Render3dGuideSnapshot *snapshot,
                                   int free_axis, const float vals[3],
                                   float sz, float as) {
    float p[3] = { vals[0], vals[1], vals[2] };

#define GUIDE_LINE_POINT(s_) do {                     \
        float q_[3] = { p[0], p[1], p[2] };            \
        q_[free_axis] = (s_);                          \
        glVertex3f(q_[0], q_[1], q_[2]);               \
    } while (0)

    glShadeModel(GL_SMOOTH);

    /* Two passes: solid under depth test, stippled ghost with depth
     * off. Both fade end -> mid -> end. */
    for (int ghost = 0; ghost < 2; ghost++) {
        int depth_was = glIsEnabled(GL_DEPTH_TEST);
        float mid_alpha = fminf((ghost ? 0.40f : 0.95f) * as, 1.0f);
        if (ghost) {
            if (depth_was) glDisable(GL_DEPTH_TEST);
            glEnable(GL_LINE_STIPPLE);
            glLineStipple(1, RENDER3D_OCCLUDED_GHOST_STIPPLE);
        }
        glLineWidth(2.0f);
        glBegin(GL_LINE_STRIP);
        render3d_clr_a(k_guide_line_clr[free_axis], 0.0f);
        GUIDE_LINE_POINT(-sz);
        render3d_clr_a(k_guide_line_clr[free_axis], mid_alpha);
        GUIDE_LINE_POINT(0.0f);
        render3d_clr_a(k_guide_line_clr[free_axis], 0.0f);
        GUIDE_LINE_POINT(sz);
        glEnd();
        if (ghost) {
            glDisable(GL_LINE_STIPPLE);
            if (depth_was) glEnable(GL_DEPTH_TEST);
        }
    }
    glLineWidth(1.0f);

    /* Integer tick dots along the free axis so the open coordinate can
     * be eyeballed; the 0 tick is larger and brighter. Depth off so the
     * scale stays readable through geometry. */
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_POINT_SMOOTH);
    glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
    for (int gi = (int)-sz + 1; gi <= (int)sz - 1; gi++) {
        float fade = 1.0f - (float)(gi < 0 ? -gi : gi) / sz;
        glPointSize(gi == 0 ? 6.0f : 4.0f);
        render3d_clr_a(k_guide_line_clr[free_axis],
                       fminf((gi == 0 ? 0.95f : 0.55f * fade + 0.15f) * as,
                             1.0f));
        glBegin(GL_POINTS);
        GUIDE_LINE_POINT((float)gi);
        glEnd();
    }
    glPointSize(1.0f);
    glDisable(GL_POINT_SMOOTH);

    /* Name the still-free axis at the line's positive end. */
    {
        char label[2] = { k_guide_axis_letter[free_axis], '\0' };
        float q[3] = { p[0], p[1], p[2] };
        q[free_axis] = sz * 1.05f;
        render3d_clr_a(k_guide_line_clr[free_axis], fminf(0.9f * as, 1.0f));
        render3d_guide_record_label(snapshot, q, FONT_SMALL, label, NULL, NULL);
        render3d_draw_bitmap_text(FONT_SMALL, q[0], q[1], q[2], label);
    }

#undef GUIDE_LINE_POINT
}

static float guide_sanitize_zero(float val) {
    if (val > -0.005f && val < 0.005f)
        return 0.0f;
    return val;
}

static int normal_dirs_differ(const float a[3], const float b[3]) {
    return fabsf(a[0] - b[0]) > 0.005f ||
           fabsf(a[1] - b[1]) > 0.005f ||
           fabsf(a[2] - b[2]) > 0.005f;
}

static void draw_normal_component_handle(float vx, float vy, float vz,
                                         const float unit_n[3],
                                         int component,
                                         float scale,
                                         float alpha_scale) {
    float axis[3] = { 0.0f, 0.0f, 0.0f };
    float tangent[3];
    float tlen;
    float tip[3];
    float handle_len = scale * 0.20f;
    float alpha;

    if (component < 0 || component > 2)
        return;

    alpha = fminf(0.95f * alpha_scale, 1.0f);
    if (alpha <= 0.0f)
        return;

    axis[component] = 1.0f;
    tangent[0] = axis[0] - unit_n[component] * unit_n[0];
    tangent[1] = axis[1] - unit_n[component] * unit_n[1];
    tangent[2] = axis[2] - unit_n[component] * unit_n[2];
    tlen = sqrtf(tangent[0] * tangent[0] +
                 tangent[1] * tangent[1] +
                 tangent[2] * tangent[2]);

    tip[0] = vx + unit_n[0] * scale;
    tip[1] = vy + unit_n[1] * scale;
    tip[2] = vz + unit_n[2] * scale;

    render3d_clr_a(k_guide_line_clr[component], alpha);
    if (tlen <= 1e-5f) {
        glPointSize(10.0f);
        glBegin(GL_POINTS);
        glVertex3f(tip[0], tip[1], tip[2]);
        glEnd();
        glPointSize(1.0f);
        return;
    }

    tangent[0] /= tlen;
    tangent[1] /= tlen;
    tangent[2] /= tlen;

    glLineWidth(4.0f);
    glBegin(GL_LINES);
    glVertex3f(tip[0], tip[1], tip[2]);
    glVertex3f(tip[0] + tangent[0] * handle_len,
               tip[1] + tangent[1] * handle_len,
               tip[2] + tangent[2] * handle_len);
    glEnd();

    render3d_clr_a(k_guide_line_clr[component], fminf(0.35f * alpha_scale, 1.0f));
    glEnable(GL_LINE_STIPPLE);
    glLineStipple(1, RENDER3D_OCCLUDED_GHOST_STIPPLE);
    glBegin(GL_LINES);
    glVertex3f(tip[0], tip[1], tip[2]);
    glVertex3f(tip[0] - tangent[0] * handle_len,
               tip[1] - tangent[1] * handle_len,
               tip[2] - tangent[2] * handle_len);
    glEnd();
    glDisable(GL_LINE_STIPPLE);
    glLineWidth(1.0f);
}

/*
 * Edit guide for the vertex command under the cursor, visualizing the
 * degrees of freedom (DOF) still open as the user types its coordinates.
 *
 * snapshot->vertex_n_filled (see Render3dGuideSnapshot) is how many of the
 * comma-separated coordinate slots have a value so far; vertex_filled[]
 * flags which specific axes those are (both pre-evaluated by the
 * controller, so this module never touches repl_eval). The guide shows
 * where the vertex *could* land given what's typed:
 *
 *   1 coord fixed  -> 2 DOF -> a plane (perpendicular to the typed axis)
 *   2 coords fixed -> 1 DOF -> a line  (sweeping the one untyped axis)
 *   all coords     -> 0 DOF -> a point (the exact vertex)
 *
 * glVertex2f is special-cased: z is implicitly fixed at 0, so one typed slot
 * leaves only one DOF (line), and two typed slots pin a complete 2D vertex
 * (point).
 */
static void draw_vertex_guides(const Render3dGuideSnapshot *snapshot) {
    if (!snapshot->show_guides)
        return;

    int is_vertex2f = 0;
    if (!input_is_vertex_kind(snapshot, &is_vertex2f) ||
        snapshot->vertex_n_filled < 1)
        return;

    int n = snapshot->vertex_n_filled;
    float vals[3] = { snapshot->vertex_args[0], snapshot->vertex_args[1],
                      snapshot->vertex_args[2] };
    int filled[3] = { snapshot->vertex_filled[0], snapshot->vertex_filled[1],
                      snapshot->vertex_filled[2] };
    float sz = 3.0f;
    float as = snapshot->alpha_scale;

    if (is_vertex2f && !filled[2]) {
        vals[2] = 0.0f;
        filled[2] = 1;
        n++;
    }

    geometry_guides_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    /* Additive blend so guides stay visible over dark backgrounds and
     * overlapping guide elements reinforce rather than occlude. */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    if (n == 1) {
        /* 2 DOF: one axis typed => plane perpendicular to it. */
        if      (filled[0]) draw_guide_axis_plane(snapshot, 0, vals[0], sz, as);
        else if (filled[1]) draw_guide_axis_plane(snapshot, 1, vals[1], sz, as);
        else if (filled[2]) draw_guide_axis_plane(snapshot, 2, vals[2], sz, as);
    } else if (n == 2) {
        /* 1 DOF: two axes typed => line along the remaining free axis. */
        int free_axis = !filled[2] ? 2 : (!filled[1] ? 1 : 0);
        draw_vertex_line_guide(snapshot, free_axis, vals, sz, as);
    } else {
        /* 0 DOF: all coords typed => exact vertex point. */
        draw_vertex_point_marker(snapshot, vals[0], vals[1], vals[2]);
    }

    glDisable(GL_BLEND);
    geometry_guides_pop_state();
}

static void draw_raster_pos_guide(const Render3dGuideSnapshot *snapshot) {
    if (!snapshot->show_guides)
        return;
    if (strncmp(snapshot->input, "glRasterPos3f(", 14) != 0 ||
        snapshot->input_len <= 14 ||
        snapshot->raster_pos_n_filled < 3)
        return;

    geometry_guides_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    draw_vertex_point_marker(snapshot,
                             snapshot->raster_pos_args[0],
                             snapshot->raster_pos_args[1],
                             snapshot->raster_pos_args[2]);
    glDisable(GL_BLEND);
    geometry_guides_pop_state();
}

static void draw_normal_guides(const Render3dGuideSnapshot *snapshot) {
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
     * controller has supplied one - it's re-evaluated every frame, so
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
                 * it default-initialised) - the right z-value for a
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

    float scale = 0.45f;
    float clen = sqrtf(vals[0]*vals[0] + vals[1]*vals[1] + vals[2]*vals[2]);
    float cn[3] = { 0.0f, 0.0f, 1.0f };
    char detail[128];
    if (clen > 1e-8f) {
        cn[0] = vals[0] / clen;
        cn[1] = vals[1] / clen;
        cn[2] = vals[2] / clen;
    }
    if (snapshot->xform_guide_mode == RENDER3D_XFORM_GUIDE_FRAME &&
        snapshot->normal_frame_args_valid &&
        normal_dirs_differ(cn, snapshot->normal_frame_args)) {
        snprintf(detail, sizeof(detail),
                 "=(%.2f, %.2f, %.2f) -> frame=(%.2f, %.2f, %.2f)",
                 guide_sanitize_zero(vals[0]),
                 guide_sanitize_zero(vals[1]),
                 guide_sanitize_zero(vals[2]),
                 guide_sanitize_zero(snapshot->normal_frame_args[0]),
                 guide_sanitize_zero(snapshot->normal_frame_args[1]),
                 guide_sanitize_zero(snapshot->normal_frame_args[2]));
    } else {
        snprintf(detail, sizeof(detail), "=(%.2f, %.2f, %.2f)",
                 guide_sanitize_zero(vals[0]),
                 guide_sanitize_zero(vals[1]),
                 guide_sanitize_zero(vals[2]));
    }

    geometry_guides_push_state();
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float as = snapshot->alpha_scale;
    if (clen > 1e-8f) {
        float label_pos[3] = {
            vx + cn[0] * scale,
            vy + cn[1] * scale,
            vz + cn[2] * scale
        };
        render3d_guide_record_label(snapshot, label_pos,
                                     FONT_MONO, " n", FONT_TINY, detail);
    }
    render3d_draw_focused_normal_glyph(vx, vy, vz,
                                    vals[0], vals[1], vals[2],
                                    scale, as, " n", detail);
    if (clen > 1e-8f)
        draw_normal_component_handle(vx, vy, vz, cn, component, scale, as);

    glDisable(GL_BLEND);
    geometry_guides_pop_state();
}

/* Segments for the clip-plane guide disc rim / fan. */
#define GEOMETRY_GUIDE_CLIP_DISC_SEGS 48
/* Disc radius, matching the vertex guide's axis-plane half-size. */
#define GEOMETRY_GUIDE_CLIP_DISC_RADIUS 3.0f

/*
 * Edit guide for the glClipPlane command under the cursor: a soft
 * "glass sheet" lying in the plane a*x + b*y + c*z + d = 0, in the
 * modelview frame where the call executes (GL transforms the equation
 * by the modelview at call time, so the guide is drawn in that same
 * frame by the cursor-guide walk).
 *
 *   - a radially-fading translucent disc (additive, so it glows over
 *     the scene rather than dimming it)
 *   - faint in-plane grid chords whose alpha peaks mid-chord and fades
 *     to nothing at the rim - the "gridded glass" read
 *   - a solid rim under depth test plus a stippled ghost rim over it
 *     (the shared occluded-ghost dash language: dashes = behind things)
 *   - the focused-normal arrow glyph pointing into the KEPT half-space
 *     (a*x+b*y+c*z+d >= 0), labeled " Pn" with the equation readout
 *
 * The whole guide dims when the program never enables the plane's
 * GL_CLIP_PLANEn cap, and the readout appends "(off)".
 */
static void draw_clip_plane_guide(const Render3dGuideSnapshot *snapshot) {
    if (!snapshot->show_guides)
        return;
    if (strncmp(snapshot->input, "glClipPlane(", 12) != 0 ||
        snapshot->input_len <= 12)
        return;
    if (snapshot->clip_plane_idx < 0 || snapshot->clip_plane_n_filled < 3)
        return;

    float a = snapshot->clip_plane_args[0];
    float b = snapshot->clip_plane_args[1];
    float c = snapshot->clip_plane_args[2];
    float d = (snapshot->clip_plane_n_filled >= 4)
                  ? snapshot->clip_plane_args[3] : 0.0f;
    float len = sqrtf(a * a + b * b + c * c);
    if (len < 1e-6f)
        return;

    float n[3] = { a / len, b / len, c / len };
    /* Point on the plane closest to the origin: -d/|n| along n. */
    float p0[3] = { -d / len * n[0], -d / len * n[1], -d / len * n[2] };

    /* In-plane tangent basis (u, v). */
    float ref[3] = { 0.0f, 1.0f, 0.0f };
    if (fabsf(n[1]) > 0.9f) { ref[0] = 1.0f; ref[1] = 0.0f; }
    float u[3] = { n[1] * ref[2] - n[2] * ref[1],
                   n[2] * ref[0] - n[0] * ref[2],
                   n[0] * ref[1] - n[1] * ref[0] };
    float ulen = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (ulen < 1e-6f)
        return;
    u[0] /= ulen; u[1] /= ulen; u[2] /= ulen;
    float v[3] = { n[1] * u[2] - n[2] * u[1],
                   n[2] * u[0] - n[0] * u[2],
                   n[0] * u[1] - n[1] * u[0] };

    float sz = GEOMETRY_GUIDE_CLIP_DISC_RADIUS;
    float as = snapshot->alpha_scale;
    /* Dim the whole guide when the plane's cap is never enabled. */
    float on = snapshot->clip_plane_cap_enabled ? 1.0f : 0.45f;

    geometry_guides_push_state();
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    /* Soft radial sheet: brighter center fading to a transparent rim. */
    glShadeModel(GL_SMOOTH);
    glBegin(GL_TRIANGLE_FAN);
    render3d_clr_a(RENDER3D_CLR_GUIDE_CLIP_FILL, fminf(0.30f * as * on, 1.0f));
    glVertex3f(p0[0], p0[1], p0[2]);
    render3d_clr_a(RENDER3D_CLR_GUIDE_CLIP_FILL, 0.02f * as * on);
    for (int k = 0; k <= GEOMETRY_GUIDE_CLIP_DISC_SEGS; k++) {
        float ang = (float)(2.0 * M_PI) * (float)k /
                    (float)GEOMETRY_GUIDE_CLIP_DISC_SEGS;
        float ca = cosf(ang), sa = sinf(ang);
        glVertex3f(p0[0] + (ca * u[0] + sa * v[0]) * sz,
                   p0[1] + (ca * u[1] + sa * v[1]) * sz,
                   p0[2] + (ca * u[2] + sa * v[2]) * sz);
    }
    glEnd();

    /* In-plane grid chords: alpha peaks mid-chord, fades to 0 at the
     * rim (two smooth-shaded segments per chord). */
    glLineWidth(1.0f);
    for (int axis = 0; axis < 2; axis++) {
        const float *ga = axis ? v : u; /* chord offset direction */
        const float *gb = axis ? u : v; /* chord line direction */
        for (int gi = -2; gi <= 2; gi++) {
            float o = (float)gi * (sz / 3.0f);
            float h = sqrtf(sz * sz * 0.98f - o * o);
            float mid[3] = { p0[0] + o * ga[0], p0[1] + o * ga[1],
                             p0[2] + o * ga[2] };
            float mid_alpha = fminf(0.28f * as * on, 1.0f);
            glBegin(GL_LINE_STRIP);
            render3d_clr_a(RENDER3D_CLR_GUIDE_CLIP_EDGE, 0.0f);
            glVertex3f(mid[0] - h * gb[0], mid[1] - h * gb[1],
                       mid[2] - h * gb[2]);
            render3d_clr_a(RENDER3D_CLR_GUIDE_CLIP_EDGE, mid_alpha);
            glVertex3f(mid[0], mid[1], mid[2]);
            render3d_clr_a(RENDER3D_CLR_GUIDE_CLIP_EDGE, 0.0f);
            glVertex3f(mid[0] + h * gb[0], mid[1] + h * gb[1],
                       mid[2] + h * gb[2]);
            glEnd();
        }
    }

    /* Rim, two passes: solid under depth test, then the stippled ghost
     * with depth off so the outline reads through occluding geometry. */
    for (int ghost = 0; ghost < 2; ghost++) {
        if (ghost) {
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_LINE_STIPPLE);
            glLineStipple(1, RENDER3D_OCCLUDED_GHOST_STIPPLE);
            render3d_clr_a(RENDER3D_CLR_GUIDE_CLIP_EDGE,
                           fminf(0.30f * as * on, 1.0f));
        } else {
            render3d_clr_a(RENDER3D_CLR_GUIDE_CLIP_EDGE,
                           fminf(0.55f * as * on, 1.0f));
        }
        glBegin(GL_LINE_LOOP);
        for (int k = 0; k < GEOMETRY_GUIDE_CLIP_DISC_SEGS; k++) {
            float ang = (float)(2.0 * M_PI) * (float)k /
                        (float)GEOMETRY_GUIDE_CLIP_DISC_SEGS;
            float ca = cosf(ang), sa = sinf(ang);
            glVertex3f(p0[0] + (ca * u[0] + sa * v[0]) * sz,
                       p0[1] + (ca * u[1] + sa * v[1]) * sz,
                       p0[2] + (ca * u[2] + sa * v[2]) * sz);
        }
        glEnd();
    }
    glDisable(GL_LINE_STIPPLE);

    /* Kept-half-space arrow + equation readout (depth already off). */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    char primary[16];
    char detail[128];
    snprintf(primary, sizeof(primary), " P%d", snapshot->clip_plane_idx);
    snprintf(detail, sizeof(detail), "=(%.2f, %.2f, %.2f, %.2f)%s",
             guide_sanitize_zero(a), guide_sanitize_zero(b),
             guide_sanitize_zero(c), guide_sanitize_zero(d),
             snapshot->clip_plane_cap_enabled ? "" : " (off)");
    {
        float label_pos[3] = {
            p0[0] + n[0] * 0.9f,
            p0[1] + n[1] * 0.9f,
            p0[2] + n[2] * 0.9f
        };
        render3d_guide_record_label(snapshot, label_pos,
                                     FONT_MONO, primary, FONT_TINY, detail);
    }
    render3d_draw_focused_normal_glyph(p0[0], p0[1], p0[2],
                                    n[0], n[1], n[2],
                                    0.9f, as * on, primary, detail);

    glDisable(GL_BLEND);
    geometry_guides_pop_state();
}

void render3d_geometry_guides_render_for_cursor(const Render3dGuideSnapshot *snapshot) {
    if (!snapshot)
        return;
    draw_vertex_guides(snapshot);
    draw_raster_pos_guide(snapshot);
    draw_normal_guides(snapshot);
    draw_clip_plane_guide(snapshot);
}
