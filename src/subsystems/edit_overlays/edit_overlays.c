#include "subsystems/edit_overlays/edit_overlays.h"
#include "gl_includes.h"
#include "repl/flatten.h"
#include "subsystems/replay/replay.h"
#include "repl/executor.h"
#include "render3d/palette.h"
#include "render3d/overlays.h"
#include "render3d/guides/geometry_guides.h"
#include "render3d/guides/transform_guides.h"
#include "repl/transform_utils.h"  /* apply_tracked_transform / unwind_transform_stack */
#include "support/cpuprof.h"
#include "config.h"
#include <stdio.h>
#include <string.h>

/* Transform a point by a column-major (OpenGL-layout) 4x4 matrix, dividing by
 * the resulting w (1 for the affine modelview transforms we deal with, but the
 * divide keeps it correct if a projection ever slips in). */
static void mat4_transform_point(const float m[16], float x, float y, float z,
                                 float out[3]) {
    float ox = m[0] * x + m[4] * y + m[8]  * z + m[12];
    float oy = m[1] * x + m[5] * y + m[9]  * z + m[13];
    float oz = m[2] * x + m[6] * y + m[10] * z + m[14];
    float ow = m[3] * x + m[7] * y + m[11] * z + m[15];
    if (ow != 0.0f && ow != 1.0f) {
        ox /= ow; oy /= ow; oz /= ow;
    }
    out[0] = ox; out[1] = oy; out[2] = oz;
}

/* Invert a column-major (OpenGL-layout) 4x4 matrix via cofactor expansion
 * (the classic MESA gluInvertMatrix). Returns 1 on success, 0 if singular
 * (out is left untouched then). Used once per label pass to undo the camera
 * view so vertex labels can report world-space (post-model-transform) coords. */
static int mat4_invert(const float m[16], float out[16]) {
    float inv[16], det;

    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
             + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
             - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
             + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
             - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
             - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
             + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
             - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
             + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15]
             + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15]
             - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15]
             + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14]
             - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11]
             - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11]
             + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11]
             - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10]
             + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

    det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det == 0.0f)
        return 0;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++)
        out[i] = inv[i] * det;
    return 1;
}

static int outline_begin_mode_has_overlay(GLenum mode) {
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

static int outline_cmd_matches_cursor(int flat_idx,
                                      const OverlayWalkCtx *ctx) {
    if (flat_idx < 0 || flat_idx >= ctx->program.cmd_count) return 0;
    const GLCmd *cmd = &ctx->program.cmds[flat_idx];
    if (!cmd->valid) return 0;
    if (ctx->cursor.edit_line_idx < 0) return 0;

    if (cmd->call_src_cmd_idx == ctx->cursor.edit_line_idx ||
        cmd->root_call_src_cmd_idx == ctx->cursor.edit_line_idx)
        return 1;
    if (ctx->cursor.cursor_func_scope_mask != 0 &&
        (cmd->func_scope_mask & ctx->cursor.cursor_func_scope_mask) != 0)
        return 1;
    if (ctx->cursor.cursor_block_begin >= 0 &&
        flat_idx >= ctx->cursor.cursor_block_begin &&
        flat_idx <= ctx->cursor.cursor_block_end)
        return 1;
    return cmd->src_cmd_idx == ctx->cursor.edit_line_idx;
}

static int outline_block_matches_cursor(int begin_idx, int is_tess,
                                        const OverlayWalkCtx *ctx) {
    const GLCmd *cmds = ctx->program.cmds;
    int cmd_count = ctx->program.cmd_count;
    int depth = is_tess ? 1 : 0;

    for (int i = begin_idx; i < cmd_count; i++) {
        if (!cmds[i].valid) continue;
        if (outline_cmd_matches_cursor(i, ctx)) return 1;
        if (!is_tess && i > begin_idx && cmds[i].type == CMD_END) break;
        if (is_tess && i > begin_idx) {
            if (cmds[i].type == CMD_TESS_BEGIN_POLYGON) depth++;
            else if (cmds[i].type == CMD_TESS_END) {
                depth--;
                if (depth == 0) break;
            }
        }
    }
    return 0;
}

static void render_outlines_glbegin_pass(const OverlayWalkCtx *ctx) {
    const GLCmd *cmds = ctx->program.cmds;
    int cmd_count = ctx->program.cmd_count;
    int in_begin = 0;
    int matrix_depth = 0;
    int block_is_current = 0;

    glPushMatrix();
    for (int i = 0; i < cmd_count; i++) {
        if (!cmds[i].valid) continue;

        if (repl_cmd_is_transform(cmds[i].type)) {
            if (!in_begin)
                apply_tracked_transform(&cmds[i], &matrix_depth);
            continue;
        }

        switch (cmds[i].type) {
        case CMD_BEGIN: {
            int draw_outline = outline_begin_mode_has_overlay((GLenum)cmds[i].args[0]);
            if (in_begin) glEnd();
            block_is_current = ctx->highlight_current_poly &&
                               outline_block_matches_cursor(i, 0, ctx);
            if (block_is_current) {
                glLineWidth(3.0f);
                render3d_clr(RENDER3D_CLR_OUTLINE_ACTIVE);
            } else if (ctx->show_vertex_outlines && draw_outline) {
                glLineWidth(1.2f);
                render3d_clr(RENDER3D_CLR_OUTLINE_EDGE);
            } else {
                in_begin = 0;
                break;
            }
            glBegin((GLenum)cmds[i].args[0]);
            in_begin = 1;
            break;
        }
        case CMD_END:
            if (in_begin) {
                glEnd();
                in_begin = 0;
                glLineWidth(1.0f);
                render3d_clr(RENDER3D_CLR_OUTLINE_EDGE);
            }
            block_is_current = 0;
            break;
        case CMD_VERTEX3F:
            if (in_begin && (block_is_current || ctx->show_vertex_outlines))
                glVertex3f(cmds[i].args[0], cmds[i].args[1], cmds[i].args[2]);
            break;
        case CMD_VERTEX2F:
            if (in_begin && (block_is_current || ctx->show_vertex_outlines))
                glVertex2f(cmds[i].args[0], cmds[i].args[1]);
            break;
        default:
            break;
        }
    }

    if (in_begin) {
        glEnd();
        glLineWidth(1.0f);
    }
    unwind_transform_stack(&matrix_depth);
    glPopMatrix();
}

static void render_outlines_tess_pass(const OverlayWalkCtx *ctx) {
    const GLCmd *cmds = ctx->program.cmds;
    int cmd_count = ctx->program.cmd_count;
    int matrix_depth = 0;
    int tess_in_contour = 0;
    int tess_poly_is_current = 0;

    glPushMatrix();
    for (int i = 0; i < cmd_count; i++) {
        if (!cmds[i].valid) continue;

        if (repl_cmd_is_transform(cmds[i].type)) {
            if (!tess_in_contour)
                apply_tracked_transform(&cmds[i], &matrix_depth);
            continue;
        }

        switch (cmds[i].type) {
        case CMD_TESS_BEGIN_POLYGON:
            tess_poly_is_current = ctx->highlight_current_poly &&
                                   outline_block_matches_cursor(i, 1, ctx);
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            if (ctx->replay_tess_preview) break;
            if (tess_in_contour) {
                glEnd();
                glLineWidth(1.0f);
            }
            if (ctx->show_vertex_outlines || tess_poly_is_current) {
                glLineWidth(1.5f);
                if (tess_poly_is_current)
                    render3d_clr(RENDER3D_CLR_OUTLINE_ACTIVE);
                else
                    render3d_clr(RENDER3D_CLR_OUTLINE);
                glBegin(GL_LINE_LOOP);
                tess_in_contour = 1;
            }
            break;
        case CMD_TESS_VERTEX:
            if (ctx->replay_tess_preview) break;
            if (tess_in_contour)
                glVertex3f(cmds[i].args[0], cmds[i].args[1], cmds[i].args[2]);
            break;
        case CMD_TESS_END:
            if (ctx->replay_tess_preview) {
                tess_poly_is_current = 0;
                break;
            }
            if (tess_in_contour) {
                glEnd();
                glLineWidth(1.0f);
                tess_in_contour = 0;
            }
            if (!tess_in_contour) tess_poly_is_current = 0;
            break;
        default:
            break;
        }
    }

    if (tess_in_contour) {
        glEnd();
        glLineWidth(1.0f);
    }
    unwind_transform_stack(&matrix_depth);
    glPopMatrix();
}

/* glutSolid* shapes emit no REPL-tracked vertices, so the glBegin/tess
 * passes above can't trace their edges. Instead, re-invoke each shape
 * under the already-active glPolygonMode(GL_LINE) + polygon offset so
 * the GL pipeline rasterizes the wireframe itself. Modelview transforms
 * are tracked exactly like the other passes so each shape lands at its
 * own matrix. */
static void render_outlines_glut_pass(const OverlayWalkCtx *ctx) {
    const GLCmd *cmds = ctx->program.cmds;
    int cmd_count = ctx->program.cmd_count;
    int matrix_depth = 0;

    glPushMatrix();
    for (int i = 0; i < cmd_count; i++) {
        if (!cmds[i].valid) continue;

        if (repl_cmd_is_transform(cmds[i].type)) {
            apply_tracked_transform(&cmds[i], &matrix_depth);
            continue;
        }
        if (!repl_cmd_is_glut_solid(cmds[i].type)) continue;

        int is_current = ctx->highlight_current_poly &&
                         outline_cmd_matches_cursor(i, ctx);
        if (is_current) {
            glLineWidth(3.0f);
            render3d_clr(RENDER3D_CLR_OUTLINE_ACTIVE);
        } else if (ctx->show_vertex_outlines) {
            glLineWidth(1.2f);
            render3d_clr(RENDER3D_CLR_OUTLINE_EDGE);
        } else {
            continue;
        }
        repl_executor_draw_glut_solid(&cmds[i]);
        glLineWidth(1.0f);
    }
    unwind_transform_stack(&matrix_depth);
    glPopMatrix();
}

void edit_overlays_render_outlines(const OverlayWalkCtx *ctx,
                                   int multisample_enabled,
                                   int line_smooth_enabled) {
    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    if (multisample_enabled) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);
    if (line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POLYGON_OFFSET_LINE);
    glPolygonOffset(REPL_OUTLINE_POLYGON_OFFSET_FACTOR,
                    REPL_OUTLINE_POLYGON_OFFSET_UNITS);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    if (ctx->show_vertex_outlines || ctx->highlight_current_poly) {
        render_outlines_glbegin_pass(ctx);
        render_outlines_tess_pass(ctx);
        render_outlines_glut_pass(ctx);
    }

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_POLYGON_OFFSET_LINE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

void edit_overlays_render_vertex_points(const OverlayWalkCtx *ctx) {
    if (!ctx->show_vertex_points && !ctx->replay_vertex_points) return;

    /* When replay is providing the dots and the user hasn't explicitly turned
     * on vertex points, only draw the single most-recent replay vertex
     * (the anchor).  When the user toggle is on, draw all vertices. */
    int replay_only = ctx->replay_vertex_points && !ctx->show_vertex_points;
    int anchor_idx  = ctx->replay_anchor_flat_idx;

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POINT_SMOOTH);
    glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);

    if (ctx->replay_vertex_points)
        render3d_clr_a(RENDER3D_CLR_VERTEX_POINT_REPLAY, 0.75f);
    else
        render3d_clr_a(RENDER3D_CLR_VERTEX_POINT, 0.80f);

    glPushMatrix();
    {
        const GLCmd *flat_cmds = ctx->program.cmds;
        int flat_cmd_count = ctx->program.cmd_count;
        int matrix_depth = 0;
        GLenum primitive_mode = 0;

        for (int i = 0; i < flat_cmd_count; i++) {
            if (!flat_cmds[i].valid) continue;
            if (repl_cmd_is_transform(flat_cmds[i].type)) {
                apply_tracked_transform(&flat_cmds[i], &matrix_depth);
            } else if (flat_cmds[i].type == CMD_BEGIN) {
                primitive_mode = (GLenum)flat_cmds[i].args[0];
            } else if (flat_cmds[i].type == CMD_END) {
                primitive_mode = 0;
            } else if (repl_cmd_emits_vertex(flat_cmds[i].type)) {
                if (replay_only && i != anchor_idx)
                    continue;
                int is_line = (primitive_mode == GL_LINES ||
                               primitive_mode == GL_LINE_STRIP ||
                               primitive_mode == GL_LINE_LOOP);
                glPointSize(is_line ? 2.0f : 4.0f);
                glBegin(GL_POINTS);
                glVertex3f(flat_cmds[i].args[0], flat_cmds[i].args[1],
                           flat_cmds[i].args[2]);
                glEnd();
            }
        }
        glPointSize(1.0f);
        unwind_transform_stack(&matrix_depth);
    }
    glPopMatrix();

    glPopAttrib();
}

/* ---- Vertex-number label declutter -------------------------------------
 *
 * Labels used to draw straight at each vertex's projected point. Two vertices
 * that project near the same pixel then stamped their bitmap strings on top of
 * each other into an unreadable jumble (classic when a primitive is viewed
 * edge-on). Instead we now project every label to screen space during the
 * walk, collect them, then lay them out in a 2D pass in collection (walk)
 * order — a camera-independent priority, so co-visible labels never swap as the
 * camera moves — nudging any label whose box overlaps an already-placed one
 * vertically (with a thin leader line back to the vertex) until it clears.
 *
 * The nudge search is BOUNDED: a label that can't find a free slot within a
 * few rows of its anchor is dropped rather than stacked arbitrarily far. That
 * keeps a dense block (e.g. a 50-vertex parametric-torus quad strip) from
 * piling every label into a tall column of long leader streaks — the
 * earlier-in-walk labels are the ones that survive. */

/* Collection ceiling. The cursor's "block" can be a whole looped surface (the
 * parametric torus selects ~1200 vertices), and we want to consider all of
 * them so the surviving labels spread evenly over the geometry instead of
 * truncating to whatever came first in program order (one spatial arc). Beyond
 * this many the labelling is meaningless anyway. */
#define VERTEX_LABEL_MAX          2048
#define VERTEX_LABEL_LINE_H       15.0f  /* collision-box height, px          */
#define VERTEX_LABEL_GAP           2.0f  /* vertical gap between stacked rows  */
#define VERTEX_LABEL_NUDGE_STEPS     4   /* max stack offsets per direction;   \
                                          * beyond this a crowded label drops  */
#define VERTEX_LABEL_LEADER_MIN     3.0f /* draw a leader past this Y nudge    */

typedef struct {
    float anchor_x, anchor_y;  /* projected vertex point, viewport-local px   */
    float width;               /* measured glyph width, px (filled at layout) */
    float draw_y;              /* baseline Y after declutter                  */
    int   drawn;               /* 0 if dropped as un-placeable (too crowded)  */
    char  idx[16];             /* "v<n>" run, FONT_MONO                       */
    char  detail[48];          /* " (x, y, z)" run, FONT_TINY (may be empty)  */
} VertexLabel;

typedef struct {
    OverlayVertexLabelMode mode;
    int   is_ortho;
    /* For OVERLAY_VERTEX_LABEL_INDEX_WORLD: inverse of the camera/view matrix
     * snapshotted at the start of the walk, so a per-vertex world position is
     * view_inv * modelview * vertex (modelview = view * accumulated model
     * transforms). view_inv_ok is 0 if the view matrix was singular. */
    float view_inv[16];
    int   view_inv_ok;
    /* Projection + viewport snapshotted before the walk (the walker only
     * touches the modelview), used to project each vertex to screen space. */
    float proj[16];
    int   vw, vh;
    VertexLabel labels[VERTEX_LABEL_MAX];
    int   count;
    /* Scope: 0 labels only the first unrolled copy of the cursor's looped
     * primitive (the parametric torus shows v0..vN once, not per ring);
     * 1 labels every unrolled vertex with a globally-unique number;
     * 2 labels all instances at their vertex positions (without decluttering). */
    int   label_options;
    int   block_instances;   /* selected blocks (loop iterations) seen so far  */
    int   labelable_seen;    /* running index over all visited labelable verts */
} VertexLabelCtx;

static float sanitize_zero(float val) {
    if (val > -0.005f && val < 0.005f) {
        return 0.0f;
    }
    return val;
}

/* Multiply column-major (OpenGL-layout) 4x4 `m` by the homogeneous vector
 * (x, y, z, w), writing the 4-component result (no perspective divide). */
static void mat4_mul_vec4(const float m[16], float x, float y, float z, float w,
                          float out[4]) {
    out[0] = m[0] * x + m[4] * y + m[8]  * z + m[12] * w;
    out[1] = m[1] * x + m[5] * y + m[9]  * z + m[13] * w;
    out[2] = m[2] * x + m[6] * y + m[10] * z + m[14] * w;
    out[3] = m[3] * x + m[7] * y + m[11] * z + m[15] * w;
}

/* Project object-space (x, y, z) through modelview `mv` then projection `proj`
 * into viewport-local window pixels (origin = scene viewport's lower-left).
 * Returns 0 when the point is at/behind the eye (clip w <= 0). */
static int project_to_screen(const float mv[16], const float proj[16],
                             int vw, int vh, float x, float y, float z,
                             float *sx, float *sy) {
    float eye[4], clip[4], inv;
    mat4_mul_vec4(mv, x, y, z, 1.0f, eye);
    mat4_mul_vec4(proj, eye[0], eye[1], eye[2], eye[3], clip);
    if (clip[3] <= 1e-5f)
        return 0;
    inv    = 1.0f / clip[3];
    *sx    = (clip[0] * inv * 0.5f + 0.5f) * (float)vw;
    *sy    = (clip[1] * inv * 0.5f + 0.5f) * (float)vh;
    return 1;
}

static float bitmap_text_width(void *font, const char *s) {
    float w = 0.0f;
    for (; s && *s; s++)
        w += (float)glutBitmapWidth(font, (unsigned char)*s);
    return w;
}

/* Axis-aligned box overlap; both boxes share the fixed height `h`. */
static int label_box_overlap(float ax, float ay, float aw,
                             float bx, float by, float bw, float h) {
    return ax < bx + bw && bx < ax + aw &&
           ay < by + h  && by < ay + h;
}

static void on_vertex_number_label(const ReplayVertexWalkState *state,
                                   float vx, float vy, float vz,
                                   void *user) {
    VertexLabelCtx *ctx = (VertexLabelCtx *)user;
    float mv[16], sx, sy;
    VertexLabel *lbl;
    int label_num, global_num;

    if (!ctx ||
        (ctx->mode != OVERLAY_VERTEX_LABEL_INDEX &&
         ctx->mode != OVERLAY_VERTEX_LABEL_INDEX_POS &&
         ctx->mode != OVERLAY_VERTEX_LABEL_INDEX_WORLD))
        return;

    /* A flat program unrolls loops, so a looped primitive becomes many blocks
     * that all match the cursor's source line. vertex_idx_in_block resets to 0
     * at each block, so it marks block boundaries; count them. The global index
     * advances for every labelable vertex (independent of the off-screen cull
     * below), so a given vertex keeps the same number as the camera moves. */
    if (state->vertex_idx_in_block == 0)
        ctx->block_instances++;
    global_num = ctx->labelable_seen++;

    /* One-instance mode: only the first unrolled copy (else the torus repeats
     * v0..vN once per ring). All-instances and At-vertex modes keep all copies;
     * declutter pass still drops whatever doesn't fit for All-instances. */
    if (ctx->label_options == 0 && ctx->block_instances != 1)
        return;
    if (ctx->count >= VERTEX_LABEL_MAX)
        return;

    /* GL_MODELVIEW here is view * model (the walker has applied the model
     * transforms up to this vertex), so this is exactly where a direct
     * glRasterPos3f(vx, vy, vz) would have landed the text. */
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    if (!project_to_screen(mv, ctx->proj, ctx->vw, ctx->vh, vx, vy, vz,
                           &sx, &sy))
        return;  /* behind the camera */
    if (sx < 0.0f || sx >= (float)ctx->vw || sy < 0.0f || sy >= (float)ctx->vh)
        return;  /* projects off-screen: a direct glRasterPos3f would have
                    produced an invalid raster position and drawn nothing here */

    label_num = (ctx->label_options == 1) ? global_num : state->vertex_idx_in_block;
    lbl = &ctx->labels[ctx->count];
    snprintf(lbl->idx, sizeof(lbl->idx), " v%d", label_num);
    lbl->detail[0] = '\0';
    if (ctx->mode == OVERLAY_VERTEX_LABEL_INDEX_POS) {
        if (ctx->is_ortho)
            snprintf(lbl->detail, sizeof(lbl->detail), " (%.2f, %.2f)",
                     sanitize_zero(vx), sanitize_zero(vy));
        else
            snprintf(lbl->detail, sizeof(lbl->detail), " (%.2f, %.2f, %.2f)",
                     sanitize_zero(vx), sanitize_zero(vy), sanitize_zero(vz));
    } else if (ctx->mode == OVERLAY_VERTEX_LABEL_INDEX_WORLD && ctx->view_inv_ok) {
        /* Map the vertex into eye space, then undo the camera to land in world
         * space. Cheap: labels only fire for the cursor's selected block. */
        float eye[3], world[3];
        mat4_transform_point(mv, vx, vy, vz, eye);
        mat4_transform_point(ctx->view_inv, eye[0], eye[1], eye[2], world);
        if (ctx->is_ortho)
            snprintf(lbl->detail, sizeof(lbl->detail), " (%.2f, %.2f)",
                     sanitize_zero(world[0]), sanitize_zero(world[1]));
        else
            snprintf(lbl->detail, sizeof(lbl->detail), " (%.2f, %.2f, %.2f)",
                     sanitize_zero(world[0]), sanitize_zero(world[1]), sanitize_zero(world[2]));
    }
    lbl->anchor_x = sx;
    lbl->anchor_y = sy;
    lbl->draw_y   = sy;
    lbl->width    = 0.0f;
    ctx->count++;
}

/* Lay the collected labels out in a 2D pass over the scene viewport: walk them
 * in collection order, push each overlapping label off its neighbours along Y,
 * then draw leaders + glyphs. Assumes the caller has set the label color and a
 * lighting/depth-disabled state (we only swap the matrices). */
static void vertex_labels_layout_and_draw(VertexLabelCtx *ctx) {
    float placed_x[VERTEX_LABEL_MAX], placed_y[VERTEX_LABEL_MAX],
          placed_w[VERTEX_LABEL_MAX];
    int   placed_n = 0;
    const float dy = VERTEX_LABEL_LINE_H + VERTEX_LABEL_GAP;
    int i, oi, step, s, p;

    if (ctx->count <= 0)
        return;

    for (i = 0; i < ctx->count; i++) {
        VertexLabel *l = &ctx->labels[i];
        l->width = bitmap_text_width(FONT_MONO, l->idx) +
                   bitmap_text_width(FONT_TINY, l->detail);
        if (l->width < 1.0f)
            l->width = 1.0f;
    }

    if (ctx->label_options == 2) {
        for (i = 0; i < ctx->count; i++) {
            VertexLabel *l = &ctx->labels[i];
            l->drawn = 1;
            l->draw_y = l->anchor_y;
        }
    } else {
        /* Place in collection (walk) order — a camera-independent priority — so two
         * labels competing for the same screen region never swap which one claims
         * the anchor vs. gets nudged/dropped as the camera moves. Sorting by depth
         * instead made near-equal depths flip every frame (a z-fighting-like
         * shimmer). The trade is that the kept subset is the earlier-in-walk one
         * rather than strictly the closest. */
        for (oi = 0; oi < ctx->count; oi++) {
            VertexLabel *l = &ctx->labels[oi];
            float best_y = 0.0f;
            int   found = 0;
            for (step = 0; step <= VERTEX_LABEL_NUDGE_STEPS && !found; step++) {
                for (s = (step == 0 ? 0 : -1); s <= 1; s += 2) {
                    float cand = l->anchor_y + (float)s * (float)step * dy;
                    int clash = 0;
                    for (p = 0; p < placed_n; p++) {
                        if (label_box_overlap(l->anchor_x, cand, l->width,
                                              placed_x[p], placed_y[p],
                                              placed_w[p], VERTEX_LABEL_LINE_H)) {
                            clash = 1;
                            break;
                        }
                    }
                    if (!clash) {
                        best_y = cand;
                        found = 1;
                        break;
                    }
                }
            }
            /* Crowded out within the bounded search: drop the label rather than
             * stack it far from its vertex (which would streak a long leader). */
            l->drawn = found;
            if (!found)
                continue;
            l->draw_y          = best_y;
            placed_x[placed_n] = l->anchor_x;
            placed_y[placed_n] = best_y;
            placed_w[placed_n] = l->width;
            placed_n++;
        }
    }

    /* 2D overlay pass in viewport-local pixels. */
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluOrtho2D(0.0, (double)ctx->vw, 0.0, (double)ctx->vh);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    /* Leaders first so glyphs paint over them. */
    glLineWidth(1.0f);
    glBegin(GL_LINES);
    for (i = 0; i < ctx->count; i++) {
        VertexLabel *l = &ctx->labels[i];
        float d;
        if (!l->drawn) continue;
        d = l->draw_y - l->anchor_y;
        if (d < 0.0f) d = -d;
        if (d > VERTEX_LABEL_LEADER_MIN) {
            glVertex2f(l->anchor_x, l->anchor_y);
            glVertex2f(l->anchor_x, l->draw_y);
        }
    }
    glEnd();

    for (i = 0; i < ctx->count; i++) {
        VertexLabel *l = &ctx->labels[i];
        const char *c;
        float y;
        if (!l->drawn) continue;
        y = l->draw_y;
        /* The anchor is on-screen (off-screen vertices were culled at collect),
         * but a declutter nudge can push draw_y past the top/bottom edge, where
         * glRasterPos would invalidate and drop the whole string — keep Y in. */
        if (y < 0.0f)                  y = 0.0f;
        if (y > (float)ctx->vh - 1.0f) y = (float)ctx->vh - 1.0f;
        glRasterPos2f(l->anchor_x, y);
        for (c = l->idx;    *c; c++) glutBitmapCharacter(FONT_MONO, (unsigned char)*c);
        for (c = l->detail; *c; c++) glutBitmapCharacter(FONT_TINY, (unsigned char)*c);
    }

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

static void on_normal_vector_arrow(const ReplayVertexWalkState *state,
                                   float vx, float vy, float vz,
                                   void *user) {
    float scale = *(const float *)user;
    render3d_draw_normal_vector_arrow(vx, vy, vz,
                                   state->normal[0],
                                   state->normal[1],
                                   state->normal[2],
                                   scale);
}

static ReplayVertexWalkContext edit_overlays_build_vertex_walk_context(
    const OverlayWalkCtx *ctx,
    int selected_block_only) {
    ReplayVertexWalkContext walk;

    memset(&walk, 0, sizeof(walk));
    if (!ctx)
        return walk;

    walk.program = ctx->program;
    walk.cursor = ctx->cursor;
    walk.selected_block_only = selected_block_only;
    return walk;
}

void edit_overlays_render_vertex_numbers(const OverlayWalkCtx *walk_ctx,
                                         OverlayVertexLabelMode mode,
                                         int is_ortho,
                                         int label_options) {
    ReplayVertexWalkContext ctx;
    /* Big label store (the all-instances walk can collect a whole looped
     * surface); keep it off the render stack. Render is single-threaded and
     * this is reset per call. */
    static VertexLabelCtx label_ctx;
    GLint vp[4];

    if (mode == OVERLAY_VERTEX_LABEL_OFF)
        return;

    ctx = edit_overlays_build_vertex_walk_context(walk_ctx, 1);
    if (!ctx.program.cmds || ctx.program.cmd_count <= 0)
        return;

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    render3d_clr(RENDER3D_CLR_VERTEX_LABEL);

    label_ctx.mode            = mode;
    label_ctx.is_ortho        = is_ortho;
    label_ctx.view_inv_ok     = 0;
    label_ctx.count           = 0;
    label_ctx.label_options   = label_options;
    label_ctx.block_instances = 0;
    label_ctx.labelable_seen  = 0;

    /* The scene's projection + viewport are live here and the walker leaves
     * them intact (it only pushes/pops the modelview), so snapshot them once
     * for the per-vertex projection in the callback. */
    glGetFloatv(GL_PROJECTION_MATRIX, label_ctx.proj);
    glGetIntegerv(GL_VIEWPORT, vp);
    label_ctx.vw = vp[2];
    label_ctx.vh = vp[3];

    if (mode == OVERLAY_VERTEX_LABEL_INDEX_WORLD) {
        /* The modelview here is the camera/view matrix — the walker has not yet
         * applied any model transform (it pushes/translates per cmd below). Cache
         * its inverse once so each label can strip the camera back out. */
        float cam_view[16];
        glGetFloatv(GL_MODELVIEW_MATRIX, cam_view);
        label_ctx.view_inv_ok = mat4_invert(cam_view, label_ctx.view_inv);
    }

    static const ReplayVertexWalkCallbacks cb = {
        .on_vertex = on_vertex_number_label,
    };
    replay_walk_user_vertices(&ctx, &cb, &label_ctx);

    if (label_ctx.vw > 0 && label_ctx.vh > 0)
        vertex_labels_layout_and_draw(&label_ctx);

    glPopAttrib();
}

#define GLR_NORMAL_ARROW_SCALE 0.35f

void edit_overlays_render_normal_vectors(const OverlayWalkCtx *walk_ctx) {
    ReplayVertexWalkContext ctx;

    ctx = edit_overlays_build_vertex_walk_context(walk_ctx, 0);
    if (!ctx.program.cmds || ctx.program.cmd_count <= 0)
        return;

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    render3d_clr(RENDER3D_CLR_NORMAL_LABEL);

    static const ReplayVertexWalkCallbacks cb = {
        .on_vertex = on_normal_vector_arrow,
    };
    float scale = GLR_NORMAL_ARROW_SCALE;
    replay_walk_user_vertices(&ctx, &cb, &scale);

    glPopAttrib();
}

typedef struct {
    const Render3dGuideSnapshot *snapshot;
    Render3dTransformGuidePlan   xform_plan;
    float                     cam_view[16];
    int                       have_xform;
    int                       geometry_guide_done;
    int                       early_stop;
} CursorGuideRenderCtx;

static int find_next_vertex_args_in_flat(const FlatProgramView *flat,
                                         int start_idx, float out[3]) {
    if (!flat || !out) return 0;
    for (int i = start_idx + 1; i < flat->cmd_count; i++) {
        const GLCmd *c = &flat->cmds[i];
        if (!c->valid) continue;
        if (repl_cmd_emits_vertex(c->type)) {
            out[0] = c->args[0];
            out[1] = c->args[1];
            out[2] = (c->type == CMD_VERTEX2F) ? 0.0f : c->args[2];
            return 1;
        }
        if (c->type == CMD_END || c->type == CMD_BEGIN ||
            c->type == CMD_TESS_END || c->type == CMD_TESS_BEGIN_POLYGON)
            return 0;
    }
    return 0;
}

Render3dGuideSnapshot cursor_guide_snapshot_with_flat_args(const Render3dGuideSnapshot *snapshot,
                                                        const GLCmd *flat,
                                                        int flat_idx) {
    Render3dGuideSnapshot snap = *snapshot;
    if (!flat) return snap;
    if (repl_cmd_emits_vertex(flat->type)) {
        snap.vertex_args[0] = flat->args[0];
        snap.vertex_args[1] = flat->args[1];
        snap.vertex_args[2] = (flat->type == CMD_VERTEX2F) ? 0.0f
                                                           : flat->args[2];
    } else if (flat->type == CMD_NORMAL3F || flat->type == CMD_TESS_NORMAL) {
        snap.normal_args[0] = flat->args[0];
        snap.normal_args[1] = flat->args[1];
        snap.normal_args[2] = flat->args[2];
        if (find_next_vertex_args_in_flat(&snap.flat_program, flat_idx,
                                          snap.normal_base_pos))
            snap.normal_base_pos_valid = 1;
    }
    return snap;
}

static void on_cmd_render_cursor_guides(const ReplayVertexWalkState *state,
                                        void *user) {
    CursorGuideRenderCtx *ctx = (CursorGuideRenderCtx *)user;
    int is_cursor = (state->src_cmd_idx == ctx->snapshot->edit_line_idx);

    if (is_cursor && !ctx->geometry_guide_done && !ctx->snapshot->replaying) {
        const GLCmd *flat =
            (state->flat_cmd_idx >= 0 &&
             state->flat_cmd_idx < ctx->snapshot->flat_program.cmd_count)
              ? &ctx->snapshot->flat_program.cmds[state->flat_cmd_idx]
              : NULL;
        Render3dGuideSnapshot snap =
            cursor_guide_snapshot_with_flat_args(ctx->snapshot, flat,
                                                 state->flat_cmd_idx);
        render3d_geometry_guides_render_for_cursor(&snap);
        ctx->geometry_guide_done = 1;
    }

    if (ctx->have_xform && !ctx->xform_plan.consumed) {
        render3d_transform_guides_render_if_due(ctx->snapshot, &ctx->xform_plan,
                                             state->flat_cmd_idx, ctx->cam_view);
    }

    int xform_done = (!ctx->have_xform || ctx->xform_plan.consumed);
    int geometry_done = (ctx->geometry_guide_done || ctx->snapshot->replaying);
    if (geometry_done && xform_done)
        ctx->early_stop = 1;
}

static void on_end_render_cursor_guides(const ReplayVertexWalkState *state,
                                        void *user) {
    CursorGuideRenderCtx *ctx = (CursorGuideRenderCtx *)user;

    if (!ctx->geometry_guide_done && !ctx->snapshot->replaying &&
        state->in_block) {
        render3d_geometry_guides_render_for_cursor(ctx->snapshot);
        ctx->geometry_guide_done = 1;
    }
    /* A tail-anchored transform plan (brand-new line) is flushed by the
     * post-walk step in edit_overlays_render_cursor_guides, not here — that
     * one path also covers the empty-program case where the walk never runs. */
}

void edit_overlays_render_cursor_guides(const Render3dGuideSnapshot *snapshot,
                                        const OverlayWalkCtx *walk_ctx) {
    ReplayVertexWalkContext walk;

    if (!snapshot || !snapshot->show_guides) return;

    CursorGuideRenderCtx ctx;
    ctx.snapshot = snapshot;
    ctx.geometry_guide_done = 0;
    ctx.early_stop = 0;
    ctx.have_xform = render3d_transform_guides_prepare(snapshot, &ctx.xform_plan);

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glPushMatrix();
    glGetFloatv(GL_MODELVIEW_MATRIX, ctx.cam_view);

    /* The flat-program walk drives the geometry (vertex/normal) guides, which
     * render in the walk's accumulated modelview, and consumes the transform
     * plan for the committed / replay / mid-document cases. It's skipped for an
     * empty program (nothing to walk) — but the transform guide must still
     * render below, so the early-out only bypasses the walk, not the flush.
     *
     * During replay, render3d_transform_guides_prepare() builds a plan anchored
     * on the replay-focused vertex (req 6), so the same per-op guide renderer
     * draws the live-style transform guide there. No separate axes pass. */
    walk = edit_overlays_build_vertex_walk_context(walk_ctx, 0);
    if (walk.program.cmds && walk.program.cmd_count > 0) {
        static const ReplayVertexWalkCallbacks cb = {
            .on_each_cmd = on_cmd_render_cursor_guides,
            .on_end = on_end_render_cursor_guides,
        };
        walk.stop_flag = &ctx.early_stop;
        replay_walk_user_vertices(&walk, &cb, &ctx);
    }

    /* Transform guides are position-independent: render3d_transform_guides_render_if_due()
     * recomputes its own anchor frame, so a plan the walk never reached — an
     * empty flat program, or a brand-new line anchored at the flat tail —
     * flushes here. This is the transform analog of the vertex guide's
     * on_end / in_block append-row path (it's why a first-time transform
     * draws live, before commit, just like a first-time glVertex). */
    if (ctx.have_xform && !ctx.xform_plan.consumed) {
        render3d_transform_guides_render_if_due(snapshot, &ctx.xform_plan,
                                             ctx.xform_plan.cursor_flat_idx,
                                             ctx.cam_view);
    }

    glPopMatrix();
    glPopAttrib();
}

void edit_overlays_post_overlays(void *user_data) {
    const OverlaySnapshotPack *pack = (const OverlaySnapshotPack *)user_data;
    if (!pack) return;

    prof_begin(PROF_RENDER3D_OVERLAY_OUTLINES);
    edit_overlays_render_outlines(&pack->walk, pack->multisample_enabled, pack->line_smooth_enabled);
    edit_overlays_render_vertex_points(&pack->walk);
    prof_accum_end(PROF_RENDER3D_OVERLAY_OUTLINES);

    prof_begin(PROF_RENDER3D_OVERLAY_TRANSFORM_GUIDES);
    edit_overlays_render_cursor_guides(&pack->snapshot, &pack->walk);
    prof_accum_end(PROF_RENDER3D_OVERLAY_TRANSFORM_GUIDES);

    if (pack->vertex_label_mode != OVERLAY_VERTEX_LABEL_OFF) {
        prof_begin(PROF_RENDER3D_OVERLAY_VERTEX_NUMBERS);
        edit_overlays_render_vertex_numbers(&pack->walk,
                                            pack->vertex_label_mode,
                                            pack->ortho_mode,
                                            pack->vertex_label_scope);
        prof_accum_end(PROF_RENDER3D_OVERLAY_VERTEX_NUMBERS);
    }
    if (pack->show_normal_vectors) {
        prof_begin(PROF_RENDER3D_OVERLAY_NORMALS);
        edit_overlays_render_normal_vectors(&pack->walk);
        prof_accum_end(PROF_RENDER3D_OVERLAY_NORMALS);
    }
}
