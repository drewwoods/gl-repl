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
#include "repl/attrib_bits.h"      /* repl_attrib_bits_for_type (restore gating) */
#include "support/cpuprof.h"
#include "config.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NORMAL_FRAME_STACK_MAX 128
#define NORMAL_FRAME_EPS 1e-8f

static int vertex_outline_style_bold(const OverlayWalkCtx *ctx) {
    int style = ctx ? ctx->vertex_outline_style : VERTEX_OUTLINE_STYLE_DEFAULT;
    return style == VERTEX_OUTLINE_STYLE_BOLD ||
           style == VERTEX_OUTLINE_STYLE_BOLD_INVERTED;
}

static int vertex_outline_style_inverted(const OverlayWalkCtx *ctx) {
    int style = ctx ? ctx->vertex_outline_style : VERTEX_OUTLINE_STYLE_DEFAULT;
    return style == VERTEX_OUTLINE_STYLE_INVERTED ||
           style == VERTEX_OUTLINE_STYLE_BOLD_INVERTED;
}

static float vertex_outline_width(const OverlayWalkCtx *ctx, float base_width) {
    return vertex_outline_style_bold(ctx) ? base_width * VERTEX_OUTLINE_BOLD_SCALE : base_width;
}

static void vertex_outline_color(const OverlayWalkCtx *ctx, Render3dColorToken token) {
    if (vertex_outline_style_inverted(ctx)) {
        Render3dRgba c = render3d_rgba(token);
        glColor4f(1.0f - c.r, 1.0f - c.g, 1.0f - c.b, c.a);
    } else {
        render3d_clr(token);
    }
}

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

static void mat4_identity(float m[16]) {
    for (int i = 0; i < 16; i++)
        m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_copy(float dst[16], const float src[16]) {
    memcpy(dst, src, 16 * sizeof(float));
}

static void mat4_post_mul(float m[16], const float rhs[16]) {
    float tmp[16];
    mat4_mul_col_major(m, rhs, tmp);
    mat4_copy(m, tmp);
}

static void mat4_apply_translate(float m[16], float x, float y, float z) {
    float t[16];
    mat4_identity(t);
    t[12] = x;
    t[13] = y;
    t[14] = z;
    mat4_post_mul(m, t);
}

static void mat4_apply_scale(float m[16], float x, float y, float z) {
    float s[16];
    mat4_identity(s);
    s[0] = x;
    s[5] = y;
    s[10] = z;
    mat4_post_mul(m, s);
}

static void mat4_apply_rotate(float m[16], float angle_deg,
                              float x, float y, float z) {
    float axis_len = sqrtf(x * x + y * y + z * z);
    float r[16];
    float c, s, omc;

    if (axis_len <= NORMAL_FRAME_EPS)
        return;

    x /= axis_len;
    y /= axis_len;
    z /= axis_len;

    float angle = angle_deg * 0.01745329251994329577f;
    c = cosf(angle);
    s = sinf(angle);
    omc = 1.0f - c;

    mat4_identity(r);
    r[0]  = x * x * omc + c;
    r[1]  = y * x * omc + z * s;
    r[2]  = x * z * omc - y * s;
    r[4]  = x * y * omc - z * s;
    r[5]  = y * y * omc + c;
    r[6]  = y * z * omc + x * s;
    r[8]  = x * z * omc + y * s;
    r[9]  = y * z * omc - x * s;
    r[10] = z * z * omc + c;
    mat4_post_mul(m, r);
}

static int mat4_transform_normal_frame(const float model[16],
                                       const float normal[3],
                                       float out[3]) {
    float inv[16];
    float len;

    if (!mat4_invert(model, inv))
        return 0;

    out[0] = inv[0] * normal[0] + inv[1] * normal[1] + inv[2] * normal[2];
    out[1] = inv[4] * normal[0] + inv[5] * normal[1] + inv[6] * normal[2];
    out[2] = inv[8] * normal[0] + inv[9] * normal[1] + inv[10] * normal[2];

    len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (len <= NORMAL_FRAME_EPS)
        return 0;

    out[0] /= len;
    out[1] /= len;
    out[2] /= len;
    return 1;
}

static int compute_normal_frame_args(const FlatProgramView *flat,
                                     int flat_idx,
                                     const float normal[3],
                                     float out[3]) {
    float model[16];
    float stack[NORMAL_FRAME_STACK_MAX][16];
    int depth = 0;

    if (!flat || !flat->cmds || flat_idx < 0 || flat_idx > flat->cmd_count)
        return 0;

    mat4_identity(model);
    for (int i = 0; i < flat_idx; i++) {
        const GLCmd *cmd = &flat->cmds[i];
        if (!cmd->valid)
            continue;

        switch (cmd->type) {
        case CMD_PUSH_MATRIX:
            if (depth >= NORMAL_FRAME_STACK_MAX)
                return 0;
            mat4_copy(stack[depth], model);
            depth++;
            break;
        case CMD_POP_MATRIX:
            if (depth > 0) {
                depth--;
                mat4_copy(model, stack[depth]);
            }
            break;
        case CMD_LOAD_IDENTITY:
            mat4_identity(model);
            break;
        case CMD_TRANSLATE3F:
            mat4_apply_translate(model, cmd->args[0], cmd->args[1], cmd->args[2]);
            break;
        case CMD_SCALEF:
            mat4_apply_scale(model, cmd->args[0], cmd->args[1], cmd->args[2]);
            break;
        case CMD_ROTATEF:
            mat4_apply_rotate(model, cmd->args[0], cmd->args[1],
                              cmd->args[2], cmd->args[3]);
            break;
        default:
            break;
        }
    }

    return mat4_transform_normal_frame(model, normal, out);
}

/* --- overlays that honor the program's clipping, culling and stencil ---
 *
 * The user program runs inside a glPushAttrib(GL_ALL_ATTRIB_BITS) bracket, so
 * by the time these passes retrace its geometry the clip planes, the cull
 * state and the stencil test it set are gone. An overlay would then trace the
 * whole uncut, unculled, unmasked silhouette - outlining a sphere over what
 * the frame actually shows as a dome, or drawing every far-side edge of a
 * culled solid. So every pass re-issues the program's own clip / cull /
 * stencil-test commands as it walks:
 *
 *   - glClipPlane bakes its equation into eye space using the modelview
 *     current at the call, and these passes track the same modelview the
 *     executor had, so issuing it at the same point in the walk reproduces the
 *     executor's planes exactly - an equation riding an animated variable
 *     included.
 *   - Culling needs no such care: GL_CULL_FACE / glCullFace / glFrontFace are
 *     plain state, and GL culls polygons in glPolygonMode(GL_LINE) and
 *     (GL_POINT) exactly as it does when filling, so the outline and
 *     glut-solid point passes get the program's own facing rule for free.
 *     GL_POINTS primitives are never culled, so the authored vertex-point walk
 *     tracks the state but keeps drawing (a vertex shared by a front and a
 *     back face still sits on a face you can see).
 *   - The stencil TEST replays (GL_STENCIL_TEST + glStencilFunc): an outline
 *     around geometry a mask threw away is a lie about what is on screen.
 *     The stencil WRITE state does not - see overlay_gl_walk_begin. This is
 *     the one place where "reproduce the program's state" splits in two, and
 *     it splits along test-vs-write, not along command boundaries.
 *
 * Host chrome takes the opposite policy and is handled elsewhere: render.c
 * brackets render3d_pass_helpers (backdrop / grid / axes / orbit target /
 * light gizmos) with the stencil test suspended, because masking chrome the
 * user never authored is never what was meant. That split works because the
 * two render3d passes align exactly with the two policies - helpers are all
 * chrome, overlays are all geometry reporting.
 *
 * Vertex points and plain outlines honor all of it unconditionally: they exist
 * to report the geometry you can see. The cursor highlight is the one opt-out -
 * under POLY_HIGHLIGHT_ON it draws the shape as authored, via
 * overlay_gl_suspend/_resume around that single draw, so you can still see the
 * whole solid a plane cuts into, the far side of a culled shell, or the part a
 * mask removed. POLY_HIGHLIGHT_CLIPPED_CULLED (ctx->highlight_clipped_culled)
 * drops the opt-out.
 *
 * overlay_gl_reset() drops what the pass turned on, so the label / guide passes
 * that run afterward are neither clipped, culled, nor masked.
 */
#define OUTLINE_CLIP_PLANE_COUNT 6

/* What a pass has mirrored into live GL so far, so it can suspend and undo
 * exactly its own edits rather than guessing at GL's state. color_writes is
 * the one non-GL member: the walkers *gate* on the program's glColorMask
 * instead of replaying it (see the color-mask comment below), but it scopes
 * under GL_COLOR_BUFFER_BIT like the rest, so it lives in the same mirror. */
typedef struct OverlayGlState {
    unsigned clip_enabled_mask;  /* bit i = we enabled GL_CLIP_PLANEi */
    int      cull_enabled;       /* we enabled GL_CULL_FACE */
    GLenum   front_face;         /* live glFrontFace mode, GL's own default */
    int      color_writes;       /* program's glColorMask writes RGB */
    int      stencil_enabled;    /* we enabled GL_STENCIL_TEST */
    GLenum   stencil_func;       /* live glStencilFunc comparison */
    GLint    stencil_ref;        /* live glStencilFunc reference value */
    GLuint   stencil_value_mask; /* live glStencilFunc compare mask */
} OverlayGlState;

/* One saved attribute frame: the push mask, the mirror snapshot, and the live
 * eye-space clip-plane equations at push time (captured from GL, since the
 * mirror never knows them - glClipPlane bakes the walk's modelview in). */
typedef struct OverlayAttribFrame {
    unsigned       mask;
    OverlayGlState snap;
    GLdouble       clip_eq[OUTLINE_CLIP_PLANE_COUNT][4];
} OverlayAttribFrame;

/* The mirror plus its attribute-stack scoping - one per walker, stepped by
 * overlay_gl_track_cmd so every walk interprets glPushAttrib/glPopAttrib the
 * same way. */
typedef struct OverlayGlTracker {
    OverlayGlState     st;
    OverlayAttribFrame frames[REPL_ATTRIB_STACK_CAP];
    int                depth;    /* virtual (unbounded) push depth */
} OverlayGlTracker;

static void overlay_gl_tracker_init(OverlayGlTracker *trk) {
    memset(trk, 0, sizeof(*trk));
    trk->st.front_face = GL_CCW;   /* GL's own default */
    trk->st.color_writes = 1;
    trk->st.stencil_func = GL_ALWAYS;          /* GL's own defaults */
    trk->st.stencil_ref = 0;
    trk->st.stencil_value_mask = 0xFFu;
}

/* Start a walk: reset the mirror AND put live GL into the baseline the
 * mirror claims, rather than assuming the frame left one.
 *
 * The stencil write state is set once here and never touched again for
 * the rest of the walk - see overlay_gl_apply_cmd, which deliberately
 * refuses to replay glStencilOp / glStencilMask. An overlay pass redraws
 * the user's geometry, so with the program's own op/mask live it would
 * *write* to the stencil buffer as a side effect: corrupting the mask a
 * later pass depends on, and the buffer the stencil visualization is
 * about to read. Reporting on a buffer must not modify it.
 *
 * Restoring all of this is the frame's job: every overlay walk runs
 * inside the scene pass's glPushAttrib(GL_ALL_ATTRIB_BITS), which covers
 * GL_STENCIL_BUFFER_BIT. */
static void overlay_gl_walk_begin(OverlayGlTracker *trk) {
    overlay_gl_tracker_init(trk);
    glDisable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 0, 0xFFu);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilMask(0);
}

static int outline_clip_cap_index(GLenum cap) {
    int idx = (int)cap - (int)GL_CLIP_PLANE0;
    return (idx >= 0 && idx < OUTLINE_CLIP_PLANE_COUNT) ? idx : -1;
}

/* --- color-mask-aware overlays ---
 *
 * glColorMask(GL_FALSE, ...) marks geometry the frame never shows - the
 * classic depth-only seed pass. Outlining or dotting it would advertise a
 * shape that isn't on screen, so the plain outline and vertex-point passes
 * track the program's mask as they walk and skip whatever is drawn while no
 * color channel is writable. Alpha is deliberately not part of the test: an
 * RGB-masked draw writes nothing you can see whether or not alpha lands.
 *
 * Unlike the clip planes this is a gate, not a replay. Mirroring the mask into
 * GL would mask the *overlay's own* pixels, so a partial mask (say red-only)
 * would tint the outline instead of reporting the geometry, and an all-off
 * mask would still pay for a draw that writes nothing.
 *
 * The cursor highlight ignores the mask entirely: finding the line you are
 * editing matters even when its geometry is a depth seed you cannot see.
 */
static int color_mask_writes_color(const GLCmd *cmd) {
    return cmd->args[0] != 0.0f || cmd->args[1] != 0.0f || cmd->args[2] != 0.0f;
}

/* Mirror one clip/cull/color-mask command into the walk state (and live GL
 * where it replays). Returns 1 if the command was one of ours (consumed). */
static int overlay_gl_apply_cmd(const GLCmd *cmd, OverlayGlState *st) {
    switch (cmd->type) {
    case CMD_CLIP_PLANE: {
        GLdouble eq[4] = {
            (GLdouble)cmd->args[1], (GLdouble)cmd->args[2],
            (GLdouble)cmd->args[3], (GLdouble)cmd->args[4]
        };
        if (outline_clip_cap_index((GLenum)cmd->args[0]) >= 0)
            glClipPlane((GLenum)cmd->args[0], eq);
        return 1;
    }
    case CMD_CULL_FACE:
        glCullFace((GLenum)cmd->args[0]);
        return 1;
    case CMD_FRONT_FACE:
        st->front_face = (GLenum)cmd->args[0];
        glFrontFace(st->front_face);
        return 1;
    case CMD_COLOR_MASK:
        st->color_writes = color_mask_writes_color(cmd);
        return 1;
    case CMD_STENCIL_FUNC:
        /* Replayed, because it is a *test*: an outline drawn around
         * geometry the program's mask threw away would be a lie about
         * what is on screen. Same principle as the clip planes above. */
        st->stencil_func = (GLenum)cmd->args[0];
        st->stencil_ref = (GLint)cmd->args[1];
        st->stencil_value_mask = (GLuint)cmd->args[2];
        glStencilFunc(st->stencil_func, st->stencil_ref, st->stencil_value_mask);
        return 1;
    case CMD_STENCIL_OP:
    case CMD_STENCIL_MASK:
        /* Consumed but deliberately NOT replayed: these are write state,
         * and an overlay pass must never write to the stencil buffer (see
         * overlay_gl_walk_begin). Returning 1 is what keeps them out of
         * the caller's draw path; leaving them unmirrored is what keeps
         * the buffer intact. */
        return 1;
    case CMD_ENABLE:
    case CMD_DISABLE: {
        GLenum cap = (GLenum)cmd->args[0];
        int on = (cmd->type == CMD_ENABLE);
        int idx = outline_clip_cap_index(cap);
        if (idx >= 0) {
            if (on) { glEnable(cap);  st->clip_enabled_mask |= 1u << idx; }
            else    { glDisable(cap); st->clip_enabled_mask &= ~(1u << idx); }
            return 1;
        }
        if (cap == GL_CULL_FACE) {
            if (on) glEnable(cap); else glDisable(cap);
            st->cull_enabled = on;
            return 1;
        }
        if (cap == GL_STENCIL_TEST) {
            if (on) glEnable(cap); else glDisable(cap);
            st->stencil_enabled = on;
            return 1;
        }
        return 0;  /* some other cap: not ours to mirror */
    }
    default:
        return 0;
    }
}

/* Restore the mirrored state a matching glPushAttrib saved, re-applying to
 * live GL, but only the components the push's `mask` actually covers (so a
 * GL_POLYGON_BIT pop reverts cull/front-face without touching clip enables a
 * GL_TRANSFORM_BIT scope owns). Each component's bit membership is routed
 * through attrib_bits (keyed by the command that sets it), so the overlay walk
 * cannot drift from the analyzer / executor / inspector. */
static void overlay_gl_restore_frame(const OverlayAttribFrame *fr,
                                     OverlayGlState *st) {
    const OverlayGlState *target = &fr->snap;
    unsigned mask = fr->mask;
    if (mask & repl_attrib_bits_for_type(CMD_ENABLE, GL_CLIP_PLANE0)) {
        for (int i = 0; i < OUTLINE_CLIP_PLANE_COUNT; i++) {
            unsigned bit = 1u << i;
            int want = (target->clip_enabled_mask & bit) != 0;
            int have = (st->clip_enabled_mask & bit) != 0;
            if (want != have) {
                if (want) glEnable((GLenum)(GL_CLIP_PLANE0 + i));
                else      glDisable((GLenum)(GL_CLIP_PLANE0 + i));
            }
        }
        st->clip_enabled_mask = target->clip_enabled_mask;
    }
    if (mask & repl_attrib_bits_for_type(CMD_CLIP_PLANE, 0)) {
        /* Put back the eye-space equations captured at push time. glClipPlane
         * transforms by the current modelview, so restore under identity -
         * exactly the frame glGetClipPlane reported them in. */
        glPushMatrix();
        glLoadIdentity();
        for (int i = 0; i < OUTLINE_CLIP_PLANE_COUNT; i++)
            glClipPlane((GLenum)(GL_CLIP_PLANE0 + i), fr->clip_eq[i]);
        glPopMatrix();
    }
    if (mask & repl_attrib_bits_for_type(CMD_ENABLE, GL_CULL_FACE)) {
        if (target->cull_enabled != st->cull_enabled) {
            if (target->cull_enabled) glEnable(GL_CULL_FACE);
            else                      glDisable(GL_CULL_FACE);
        }
        st->cull_enabled = target->cull_enabled;
    }
    if (mask & repl_attrib_bits_for_type(CMD_FRONT_FACE, 0)) {
        if (target->front_face != st->front_face)
            glFrontFace(target->front_face);
        st->front_face = target->front_face;
    }
    if (mask & repl_attrib_bits_for_type(CMD_COLOR_MASK, 0))
        st->color_writes = target->color_writes;
    if (mask & repl_attrib_bits_for_type(CMD_ENABLE, GL_STENCIL_TEST)) {
        if (target->stencil_enabled != st->stencil_enabled) {
            if (target->stencil_enabled) glEnable(GL_STENCIL_TEST);
            else                         glDisable(GL_STENCIL_TEST);
        }
        st->stencil_enabled = target->stencil_enabled;
    }
    /* glStencilFunc belongs to GL_STENCIL_BUFFER_BIT, which the REPL does
     * not accept in glPushAttrib yet - repl_attrib_bits_for_type returns 0
     * for CMD_STENCIL_FUNC, so this branch is unreachable today and turns
     * itself on with the rest of the stencil attrib group. Routing it
     * through attrib_bits rather than hardcoding a bit is what makes that
     * automatic (and is why the whole restore reads that way). */
    if (mask & repl_attrib_bits_for_type(CMD_STENCIL_FUNC, 0)) {
        if (target->stencil_func != st->stencil_func ||
            target->stencil_ref != st->stencil_ref ||
            target->stencil_value_mask != st->stencil_value_mask) {
            glStencilFunc(target->stencil_func, target->stencil_ref,
                          target->stencil_value_mask);
        }
        st->stencil_func = target->stencil_func;
        st->stencil_ref = target->stencil_ref;
        st->stencil_value_mask = target->stencil_value_mask;
    }
}

/* Step one flat command through the tracker: glPushAttrib/glPopAttrib scope
 * the mirrored clip/cull/front-face/color-mask state (so a scoped
 * glDisable(GL_CULL_FACE) reverts at the pop, like the real scene render);
 * everything else defers to overlay_gl_apply_cmd. Returns 1 when the command
 * was consumed (state bookkeeping - nothing for the caller to draw). Every
 * walker steps this so no pass can miss the push/pop scoping. */
static int overlay_gl_track_cmd(const GLCmd *cmd, OverlayGlTracker *trk) {
    switch (cmd->type) {
    case CMD_PUSH_ATTRIB:
        if (trk->depth < REPL_ATTRIB_STACK_CAP) {
            OverlayAttribFrame *fr = &trk->frames[trk->depth];
            fr->mask = (unsigned)cmd->args[0];
            fr->snap = trk->st;
            if (fr->mask & repl_attrib_bits_for_type(CMD_CLIP_PLANE, 0)) {
                for (int i = 0; i < OUTLINE_CLIP_PLANE_COUNT; i++)
                    glGetClipPlane((GLenum)(GL_CLIP_PLANE0 + i),
                                   fr->clip_eq[i]);
            }
        }
        trk->depth++;
        return 1;
    case CMD_POP_ATTRIB:
        if (trk->depth > 0) {
            trk->depth--;
            if (trk->depth < REPL_ATTRIB_STACK_CAP)
                overlay_gl_restore_frame(&trk->frames[trk->depth], &trk->st);
        }
        return 1;
    default:
        return overlay_gl_apply_cmd(cmd, &trk->st);
    }
}

static void overlay_gl_reset(OverlayGlState *st) {
    for (int i = 0; i < OUTLINE_CLIP_PLANE_COUNT; i++) {
        if (st->clip_enabled_mask & (1u << i))
            glDisable((GLenum)(GL_CLIP_PLANE0 + i));
    }
    if (st->cull_enabled)
        glDisable(GL_CULL_FACE);
    /* glCullFace / glFrontFace modes are left as the program set them: with
     * GL_CULL_FACE off they select nothing, and no later overlay pass culls.
     * The stencil test is dropped for the same reason the clip planes are:
     * the label and guide passes that run afterward report positions, not
     * geometry, and must not be masked away. glStencilFunc is left as the
     * walk set it - with the test off it selects nothing. */
    if (st->stencil_enabled)
        glDisable(GL_STENCIL_TEST);
    st->clip_enabled_mask = 0;
    st->cull_enabled = 0;
    st->stencil_enabled = 0;
}

/* Lift the program's clipping and culling for one highlight draw
 * (POLY_HIGHLIGHT_ON) and put them back afterward, so the plain outlines
 * around it stay cut. Both are no-ops in CLIPPED_CULLED mode, and cost nothing
 * when the program clips and culls nothing. Suspend/resume must bracket the
 * same draw: they restore from the caller's state, not from GL. */
static void overlay_gl_suspend(const OverlayWalkCtx *ctx, const OverlayGlState *st) {
    if (ctx->highlight_clipped_culled)
        return;
    for (int i = 0; i < OUTLINE_CLIP_PLANE_COUNT; i++) {
        if (st->clip_enabled_mask & (1u << i))
            glDisable((GLenum)(GL_CLIP_PLANE0 + i));
    }
    if (st->cull_enabled)
        glDisable(GL_CULL_FACE);
    /* The stencil test joins the opt-out for the same reason: finding the
     * line you are editing matters even when its geometry is the part a
     * mask threw away. */
    if (st->stencil_enabled)
        glDisable(GL_STENCIL_TEST);
}

static void overlay_gl_resume(const OverlayWalkCtx *ctx, const OverlayGlState *st) {
    if (ctx->highlight_clipped_culled)
        return;
    for (int i = 0; i < OUTLINE_CLIP_PLANE_COUNT; i++) {
        if (st->clip_enabled_mask & (1u << i))
            glEnable((GLenum)(GL_CLIP_PLANE0 + i));
    }
    if (st->cull_enabled)
        glEnable(GL_CULL_FACE);
    if (st->stencil_enabled)
        glEnable(GL_STENCIL_TEST);
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

/* Does the scope highlight exactly one unrolled copy of the cursor's block?
 * SINGLE_POLYGON narrows LAST_INSTANCE, so it shares the gate (tess and glut
 * shapes, which the per-primitive filter deliberately excludes, degrade to
 * plain last-instance behavior). */
static int cursor_scope_selects_one_instance(const OverlayWalkCtx *ctx) {
    return ctx->cursor_overlay_scope == OVERLAY_SCOPE_LAST_INSTANCE ||
           ctx->cursor_overlay_scope == OVERLAY_SCOPE_SINGLE_POLYGON;
}

/* Index of the LAST block head (CMD_BEGIN, or CMD_TESS_BEGIN_POLYGON when
 * is_tess) whose block matches the cursor line, or -1 if none does. The
 * render passes walk forward, so the copy to highlight has to be resolved
 * before they start; the predicate is the same one they use, and it costs a
 * second GL-free walk of the flat program. */
static int last_matching_block_head(const OverlayWalkCtx *ctx,
                                    CmdType head_type, int is_tess) {
    const GLCmd *cmds = ctx->program.cmds;
    int found = -1;

    for (int i = 0; i < ctx->program.cmd_count; i++) {
        if (!cmds[i].valid || cmds[i].type != head_type) continue;
        if (outline_block_matches_cursor(i, is_tess, ctx)) found = i;
    }
    return found;
}

/* Same, gated on the highlight actually being on - the outline passes have
 * nothing to resolve when it is off. The normal-vector pass wants the
 * ungated form above: it honors the scope whether or not the highlight does. */
static int last_selected_block_head(const OverlayWalkCtx *ctx,
                                    CmdType head_type, int is_tess) {
    if (!ctx->highlight_current_poly) return -1;
    return last_matching_block_head(ctx, head_type, is_tess);
}

/* Same, for the glutSolid* pass - those match per command, not per block. */
static int last_selected_glut_shape(const OverlayWalkCtx *ctx) {
    const GLCmd *cmds = ctx->program.cmds;
    int found = -1;

    if (!ctx->highlight_current_poly) return -1;
    for (int i = 0; i < ctx->program.cmd_count; i++) {
        if (!cmds[i].valid || !repl_cmd_is_glut_solid(cmds[i].type)) continue;
        if (outline_cmd_matches_cursor(i, ctx)) found = i;
    }
    return found;
}

/* SINGLE_POLYGON scope with a resolved primitive range: does block-local
 * vertex ordinal `ord` belong to the cursor's primitive? */
static int cursor_poly_contains_ordinal(const OverlayWalkCtx *ctx, int ord) {
    return (ord >= ctx->cursor_poly_first && ord <= ctx->cursor_poly_last) ||
           (ctx->cursor_poly_fan_anchor && ord == 0);
}

/* Emit only the cursor's primitive from the glBegin block headed at
 * begin_idx, in the active-highlight style. Re-issuing the block's own
 * mode with just the selected ordinal run renders exactly one primitive
 * for every grouping the resolver produces: a quad/triangle/line/point
 * group, a strip sub-window, or a fan triangle (center vertex 0 plus
 * the [first, last] pair). Transforms cannot occur inside a begin
 * block, so re-walking the range under the caller's modelview is safe.
 *
 * Unlike the whole-block active-highlight path (which skips the plain
 * outline pass entirely via block_is_current), this fires while
 * block_is_current is forced back to 0, so the caller's plain-outline
 * branch still redraws the WHOLE block afterward at the edge width/color
 * - including the primitive just highlighted here. In the default outline
 * style that redraw is thinner (EDGE_WIDTH) than this highlight
 * (ACTIVE_WIDTH) and stays visible around it, but Bold scales EDGE_WIDTH
 * by the same factor ACTIVE_WIDTH would need to stay ahead of it
 * (1.2 * 2.5 = 3.0 = ACTIVE_WIDTH), so an unscaled highlight is an exact
 * width tie with the later edge redraw and gets fully overwritten. Scale
 * this width the same way so it stays strictly thicker in Bold too.
 *
 * One wrinkle once culling is live (CLIPPED_CULLED): GL implicitly reverses
 * the winding of every odd-numbered GL_TRIANGLE_STRIP triangle, and the
 * resolver hands us `first` == that triangle's index. Re-issued on its own the
 * run becomes strip triangle 0, losing the flip - so an odd triangle would be
 * culled exactly when the real face is visible. Swapping glFrontFace for the
 * draw restores the parity. Quad strips (first is always even, and a 4-vertex
 * run rebuilds the same quad) and fans (center + pair, in order) need none. */
static GLenum overlay_front_face_opposite(GLenum front_face) {
    return front_face == GL_CW ? GL_CCW : GL_CW;
}

static void render_single_polygon_highlight(const OverlayWalkCtx *ctx,
                                            int begin_idx,
                                            const OverlayGlState *st) {
    const GLCmd *cmds = ctx->program.cmds;
    int cmd_count = ctx->program.cmd_count;
    GLenum mode = (GLenum)cmds[begin_idx].args[0];
    int ord = 0;
    int flip_winding = ctx->highlight_clipped_culled && st->cull_enabled &&
                       mode == GL_TRIANGLE_STRIP && (ctx->cursor_poly_first & 1);

    overlay_gl_suspend(ctx, st);
    if (flip_winding)
        glFrontFace(overlay_front_face_opposite(st->front_face));
    glLineWidth(vertex_outline_width(ctx, VERTEX_OUTLINE_ACTIVE_WIDTH));
    render3d_clr(RENDER3D_CLR_OUTLINE_ACTIVE);
    glBegin(mode);
    for (int i = begin_idx + 1; i < cmd_count; i++) {
        if (!cmds[i].valid) continue;
        if (cmds[i].type == CMD_END) break;
        if (cmds[i].type != CMD_VERTEX3F && cmds[i].type != CMD_VERTEX2F)
            continue;
        if (cursor_poly_contains_ordinal(ctx, ord)) {
            if (cmds[i].type == CMD_VERTEX3F)
                glVertex3f(cmds[i].args[0], cmds[i].args[1], cmds[i].args[2]);
            else
                glVertex2f(cmds[i].args[0], cmds[i].args[1]);
        }
        ord++;
    }
    glEnd();
    glLineWidth(1.0f);
    render3d_clr(RENDER3D_CLR_OUTLINE_EDGE);
    if (flip_winding)
        glFrontFace(st->front_face);
    overlay_gl_resume(ctx, st);
}

static void render_outlines_glbegin_pass(const OverlayWalkCtx *ctx) {
    const GLCmd *cmds = ctx->program.cmds;
    int cmd_count = ctx->program.cmd_count;
    int in_begin = 0;
    int matrix_depth = 0;
    int block_is_current = 0;
    int one_instance = cursor_scope_selects_one_instance(ctx);
    int selected_head = one_instance
                        ? last_selected_block_head(ctx, CMD_BEGIN, 0) : -1;
    OverlayGlTracker trk;
    int block_clip_suspended = 0;

    overlay_gl_walk_begin(&trk);
    glPushMatrix();
    for (int i = 0; i < cmd_count; i++) {
        if (!cmds[i].valid) continue;

        if (repl_cmd_is_transform(cmds[i].type)) {
            if (!in_begin)
                apply_tracked_transform(&cmds[i], &matrix_depth);
            continue;
        }
        /* State (clip/cull/color-mask) and push/pop scoping land between
         * blocks; inside one they would be an illegal glBegin/glEnd
         * interleave. */
        if (!in_begin && overlay_gl_track_cmd(&cmds[i], &trk))
            continue;

        switch (cmds[i].type) {
        case CMD_BEGIN: {
            int draw_outline = outline_begin_mode_has_overlay((GLenum)cmds[i].args[0]);
            int block_matches_current = ctx->highlight_current_poly &&
                                        outline_block_matches_cursor(i, 0, ctx);
            if (in_begin) glEnd();
            /* An unterminated highlight block (no CMD_END) still has to put
             * the planes back before the next block outlines. */
            if (block_clip_suspended) {
                overlay_gl_resume(ctx, &trk.st);
                block_clip_suspended = 0;
            }
            block_is_current = block_matches_current &&
                               (!one_instance || i == selected_head);
            if (block_is_current &&
                ctx->cursor_overlay_scope == OVERLAY_SCOPE_SINGLE_POLYGON &&
                ctx->cursor_poly_valid) {
                /* Highlight only the cursor's primitive; the rest of the
                 * block falls through to the plain outline (or nothing). */
                render_single_polygon_highlight(ctx, i, &trk.st);
                block_is_current = 0;
            }
            if (block_is_current) {
                overlay_gl_suspend(ctx, &trk.st);
                block_clip_suspended = 1;
                glLineWidth(VERTEX_OUTLINE_ACTIVE_WIDTH);
                render3d_clr(RENDER3D_CLR_OUTLINE_ACTIVE);
            } else if (ctx->show_vertex_outlines && draw_outline && trk.st.color_writes) {
                glLineWidth(vertex_outline_width(ctx, VERTEX_OUTLINE_EDGE_WIDTH));
                vertex_outline_color(ctx, RENDER3D_CLR_OUTLINE_EDGE);
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
            if (block_clip_suspended) {
                overlay_gl_resume(ctx, &trk.st);
                block_clip_suspended = 0;
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
    if (block_clip_suspended)
        overlay_gl_resume(ctx, &trk.st);
    overlay_gl_reset(&trk.st);
    unwind_transform_stack(&matrix_depth);
    glPopMatrix();
}

static void render_outlines_tess_pass(const OverlayWalkCtx *ctx) {
    const GLCmd *cmds = ctx->program.cmds;
    int cmd_count = ctx->program.cmd_count;
    int matrix_depth = 0;
    int tess_in_contour = 0;
    int tess_poly_is_current = 0;
    int one_instance = cursor_scope_selects_one_instance(ctx);
    int selected_head = one_instance
                        ? last_selected_block_head(ctx, CMD_TESS_BEGIN_POLYGON, 1)
                        : -1;
    OverlayGlTracker trk;
    int contour_clip_suspended = 0;

    overlay_gl_walk_begin(&trk);
    glPushMatrix();
    for (int i = 0; i < cmd_count; i++) {
        if (!cmds[i].valid) continue;

        if (repl_cmd_is_transform(cmds[i].type)) {
            if (!tess_in_contour)
                apply_tracked_transform(&cmds[i], &matrix_depth);
            continue;
        }
        if (!tess_in_contour && overlay_gl_track_cmd(&cmds[i], &trk))
            continue;

        switch (cmds[i].type) {
        case CMD_TESS_BEGIN_POLYGON:
            tess_poly_is_current = ctx->highlight_current_poly &&
                                   outline_block_matches_cursor(i, 1, ctx) &&
                                   (!one_instance || i == selected_head);
            break;
        case CMD_TESS_BEGIN_CONTOUR:
            if (ctx->replay_tess_preview) break;
            if (tess_in_contour) {
                glEnd();
                glLineWidth(1.0f);
            }
            if (contour_clip_suspended) {
                overlay_gl_resume(ctx, &trk.st);
                contour_clip_suspended = 0;
            }
            if ((ctx->show_vertex_outlines && trk.st.color_writes) || tess_poly_is_current) {
                if (tess_poly_is_current) {
                    overlay_gl_suspend(ctx, &trk.st);
                    contour_clip_suspended = 1;
                    glLineWidth(VERTEX_OUTLINE_TESS_WIDTH);
                    render3d_clr(RENDER3D_CLR_OUTLINE_ACTIVE);
                } else {
                    glLineWidth(vertex_outline_width(ctx, VERTEX_OUTLINE_TESS_WIDTH));
                    vertex_outline_color(ctx, RENDER3D_CLR_OUTLINE);
                }
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
            if (contour_clip_suspended) {
                overlay_gl_resume(ctx, &trk.st);
                contour_clip_suspended = 0;
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
    if (contour_clip_suspended)
        overlay_gl_resume(ctx, &trk.st);
    overlay_gl_reset(&trk.st);
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
    int one_instance = cursor_scope_selects_one_instance(ctx);
    int selected_shape = one_instance ? last_selected_glut_shape(ctx) : -1;
    OverlayGlTracker trk;

    overlay_gl_walk_begin(&trk);
    glPushMatrix();
    for (int i = 0; i < cmd_count; i++) {
        if (!cmds[i].valid) continue;

        if (repl_cmd_is_transform(cmds[i].type)) {
            apply_tracked_transform(&cmds[i], &matrix_depth);
            continue;
        }
        if (overlay_gl_track_cmd(&cmds[i], &trk))
            continue;
        if (!repl_cmd_is_glut_solid(cmds[i].type)) continue;

        int is_current = ctx->highlight_current_poly &&
                         outline_cmd_matches_cursor(i, ctx) &&
                         (!one_instance || i == selected_shape);
        if (is_current) {
            overlay_gl_suspend(ctx, &trk.st);
            glLineWidth(VERTEX_OUTLINE_ACTIVE_WIDTH);
            render3d_clr(RENDER3D_CLR_OUTLINE_ACTIVE);
        } else if (ctx->show_vertex_outlines && trk.st.color_writes) {
            glLineWidth(vertex_outline_width(ctx, VERTEX_OUTLINE_EDGE_WIDTH));
            vertex_outline_color(ctx, RENDER3D_CLR_OUTLINE_EDGE);
        } else {
            continue;
        }
        repl_executor_draw_glut_solid(&cmds[i]);
        glLineWidth(1.0f);
        if (is_current)
            overlay_gl_resume(ctx, &trk.st);
    }
    overlay_gl_reset(&trk.st);
    unwind_transform_stack(&matrix_depth);
    glPopMatrix();
}

/* The manual vertex-point walk below only sees REPL-authored glVertex*
 * commands. glutSolid* meshes are generated inside GLUT, so redraw those
 * shapes with polygon rasterization switched to points. Keep this tied to
 * show_vertex_points, not replay-only: replay-only is a single authored draw
 * anchor, while GL_POINT mode exposes every generated mesh vertex. */
static void render_vertex_points_glut_pass(const OverlayWalkCtx *ctx) {
    const GLCmd *cmds = ctx->program.cmds;
    int cmd_count = ctx->program.cmd_count;
    int matrix_depth = 0;
    int has_glut_solid = 0;
    OverlayGlTracker trk;

    if (!ctx->show_vertex_points)
        return;
    for (int i = 0; i < cmd_count; i++) {
        if (cmds[i].valid && repl_cmd_is_glut_solid(cmds[i].type)) {
            has_glut_solid = 1;
            break;
        }
    }
    if (!has_glut_solid)
        return;

    overlay_gl_walk_begin(&trk);
    glPushMatrix();
    glPointSize(EDIT_OVERLAY_VERTEX_POINT_SIZE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
    for (int i = 0; i < cmd_count; i++) {
        if (!cmds[i].valid) continue;

        if (repl_cmd_is_transform(cmds[i].type)) {
            apply_tracked_transform(&cmds[i], &matrix_depth);
            continue;
        }
        if (overlay_gl_track_cmd(&cmds[i], &trk))
            continue;
        if (!repl_cmd_is_glut_solid(cmds[i].type)) continue;
        if (!trk.st.color_writes) continue;

        repl_executor_draw_glut_solid(&cmds[i]);
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glPointSize(1.0f);
    overlay_gl_reset(&trk.st);
    unwind_transform_stack(&matrix_depth);
    glPopMatrix();
}

/* Draw one authored vertex-point overlay. WebGL's gl4es compatibility path
 * cannot reliably render the point-smooth GL_POINTS form at these sizes, so
 * Emscripten uses a direct eight-triangle octahedron instead. Its perspective
 * projection supplies the same distance behavior as native point attenuation,
 * without making a GLUT call for every authored vertex. */
static void draw_vertex_point_overlay(float x, float y, float z, int is_line) {
#if defined(__EMSCRIPTEN__)
    float radius = is_line ? EDIT_OVERLAY_VERTEX_LINE_POINT_OCTAHEDRON_RADIUS
                           : EDIT_OVERLAY_VERTEX_POINT_OCTAHEDRON_RADIUS;

    glDisable(GL_CULL_FACE);  /* vertex points are never cullable */
    glPushMatrix();
    glTranslatef(x, y, z);
    glScalef(radius, radius, radius);
    glBegin(GL_TRIANGLES);
    /* +Y cap, around the XZ ring (+X, -Z, -X, +Z). */
    glVertex3f(0.0f,  1.0f,  0.0f); glVertex3f( 1.0f, 0.0f,  0.0f); glVertex3f( 0.0f, 0.0f, -1.0f);
    glVertex3f(0.0f,  1.0f,  0.0f); glVertex3f( 0.0f, 0.0f, -1.0f); glVertex3f(-1.0f, 0.0f,  0.0f);
    glVertex3f(0.0f,  1.0f,  0.0f); glVertex3f(-1.0f, 0.0f,  0.0f); glVertex3f( 0.0f, 0.0f,  1.0f);
    glVertex3f(0.0f,  1.0f,  0.0f); glVertex3f( 0.0f, 0.0f,  1.0f); glVertex3f( 1.0f, 0.0f,  0.0f);
    /* -Y cap, with the opposite winding. */
    glVertex3f(0.0f, -1.0f,  0.0f); glVertex3f( 0.0f, 0.0f, -1.0f); glVertex3f( 1.0f, 0.0f,  0.0f);
    glVertex3f(0.0f, -1.0f,  0.0f); glVertex3f(-1.0f, 0.0f,  0.0f); glVertex3f( 0.0f, 0.0f, -1.0f);
    glVertex3f(0.0f, -1.0f,  0.0f); glVertex3f( 0.0f, 0.0f,  1.0f); glVertex3f(-1.0f, 0.0f,  0.0f);
    glVertex3f(0.0f, -1.0f,  0.0f); glVertex3f( 1.0f, 0.0f,  0.0f); glVertex3f( 0.0f, 0.0f,  1.0f);
    glEnd();
    glPopMatrix();
#else
    glPointSize(is_line ? EDIT_OVERLAY_VERTEX_LINE_POINT_SIZE
                        : EDIT_OVERLAY_VERTEX_POINT_SIZE);
    glBegin(GL_POINTS);
    glVertex3f(x, y, z);
    glEnd();
#endif
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
    /* Unconditional, not part of the #if below: desktop GL rasterizes points as
     * circles under multisample rasterization, so the native build barely needs
     * this - gl4es has no such implicit path and honours GL_POINT_SMOOTH only
     * when it is explicitly enabled. Keeping it outside the branch is what lets
     * the GL_POINTS form ever be restored on the web target. */
    glEnable(GL_POINT_SMOOTH);
    glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
#if defined(__EMSCRIPTEN__)
    /* The octahedron markers respect scene depth testing but must not add
     * their own translucent surface to it. GL_ALL_ATTRIB_BITS restores this. */
    glDepthMask(GL_FALSE);
#endif

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
        OverlayGlTracker trk;

        overlay_gl_walk_begin(&trk);
        for (int i = 0; i < flat_cmd_count; i++) {
            if (!flat_cmds[i].valid) continue;
            if (repl_cmd_is_transform(flat_cmds[i].type)) {
                apply_tracked_transform(&flat_cmds[i], &matrix_depth);
            } else if (overlay_gl_track_cmd(&flat_cmds[i], &trk)) {
                /* keep the program's planes: a point on a clipped-away face
                 * is not on the shape you can see */
            } else if (flat_cmds[i].type == CMD_BEGIN) {
                primitive_mode = (GLenum)flat_cmds[i].args[0];
            } else if (flat_cmds[i].type == CMD_END) {
                primitive_mode = 0;
            } else if (repl_cmd_emits_vertex(flat_cmds[i].type)) {
                if (replay_only && i != anchor_idx)
                    continue;
                if (!trk.st.color_writes)
                    continue;
                int is_line = (primitive_mode == GL_LINES ||
                               primitive_mode == GL_LINE_STRIP ||
                               primitive_mode == GL_LINE_LOOP);
                draw_vertex_point_overlay(flat_cmds[i].args[0],
                                          flat_cmds[i].args[1],
                                          flat_cmds[i].args[2], is_line);
            }
        }
#if !defined(__EMSCRIPTEN__)
        glPointSize(1.0f);
#endif
        /* Drop them before the glut pass re-applies the program's clip state
         * from scratch: a cap left on here is not in that walk's mask, so it
         * would clip shapes drawn before the program ever enabled it. */
        overlay_gl_reset(&trk.st);
        unwind_transform_stack(&matrix_depth);
    }
    glPopMatrix();

    render_vertex_points_glut_pass(ctx);

    glPopAttrib();
}

/* ---- Vertex-number label declutter -------------------------------------
 *
 * Labels used to draw straight at each vertex's projected point. Two vertices
 * that project near the same pixel then stamped their bitmap strings on top of
 * each other into an unreadable jumble (classic when a primitive is viewed
 * edge-on). Instead we now project every label to screen space during the
 * walk, collect them, then lay them out in a 2D pass in collection (walk)
 * order - a camera-independent priority, so co-visible labels never swap as the
 * camera moves - nudging any label whose box overlaps an already-placed one
 * vertically (with a thin leader line back to the vertex) until it clears.
 *
 * The nudge search is BOUNDED: a label that can't find a free slot within a
 * few rows of its anchor is dropped rather than stacked arbitrarily far. That
 * keeps a dense block (e.g. a 50-vertex parametric-torus quad strip) from
 * piling every label into a tall column of long leader streaks - the
 * earlier-in-walk labels are the ones that survive.
 *
 * The solve is also TEMPORALLY STICKY. A memoryless per-frame solve made
 * continuous anchor motion (camera orbit, animated geometry) flip overlap
 * tests discretely: labels popped whole rows, flapped between the -dy and
 * +dy candidates, and strobed in/out at the crowding boundary. Each label's
 * placement is remembered across frames in g_label_sticky[], keyed by its
 * stable number (the walk-order numbering is camera-independent), and per
 * frame:
 *   - an incumbent first homes to a row nearer its anchor, but only with
 *     comfortable headroom (VERTEX_LABEL_ENTER_PAD) so home-and-back can't
 *     flap; failing that it keeps its held row, judged leniently
 *     (VERTEX_LABEL_KEEP_SHRINK) so a boundary graze doesn't evict it;
 *   - a label that wasn't visible last frame enters only with the same
 *     headroom margin, so the crowding boundary doesn't strobe;
 *   - the drawn offset eases toward the solved row (VERTEX_LABEL_EASE per
 *     frame) so a forced row change glides instead of popping. Collision
 *     boxes always use the quantized target row, so placement decisions
 *     stay stable while a label is mid-glide. */

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
/* Visible-only scope: window-depth slack when testing a vertex against the
 * scene depth buffer. A vertex sits on the rendered surface, so its depth
 * ~equals the buffer there; the bias absorbs projection/raster precision so
 * front vertices aren't culled, while anything clearly behind is dropped. */
#define VERTEX_LABEL_OCCLUDE_BIAS  0.002f
/* Temporal-coherence knobs (see the TEMPORALLY STICKY note above). */
#define VERTEX_LABEL_ENTER_PAD     2.0f  /* headroom (px) to appear or home    */
#define VERTEX_LABEL_KEEP_SHRINK   4.0f  /* leniency (px) before eviction      */
#define VERTEX_LABEL_EASE          0.20f /* per-frame glide toward the row     */
#define GUIDE_LABEL_OBSTACLE_MAX      32  /* fixed edit-guide label rectangles  */

typedef struct {
    float anchor_x, anchor_y;  /* projected vertex point, viewport-local px   */
    float width;               /* measured glyph width, px (filled at layout) */
    float draw_y;              /* baseline Y after declutter                  */
    int   drawn;               /* 0 if dropped as un-placeable (too crowded)  */
    int   num;                 /* stable label number (sticky-table key)      */
    char  idx[16];             /* "v<n>" run, FONT_MONO                       */
    char  detail[48];          /* " (x, y, z)" run, FONT_TINY (may be empty)  */
} VertexLabel;

/* Sticky per-label placement memory. Direct-indexed by the label's stable
 * number, which is bounded by MAX_FLAT_COMMANDS (every labelable vertex is
 * one flat command). last_drawn is a g_label_sticky_frame stamp; an entry is an
 * incumbent when it drew on the immediately preceding layout pass. */
typedef struct {
    unsigned last_drawn;  /* frame stamp of the last actual draw            */
    int      row;         /* signed nudge row held on that frame            */
    float    eased_dy;    /* eased pixel offset the label actually drew at  */
} VertexLabelSticky;

static VertexLabelSticky g_label_sticky[MAX_FLAT_COMMANDS];
static unsigned g_label_sticky_frame = 1; /* starts past 0 so zeroed entries
                                           * never read as incumbents */
static int g_label_sticky_scope = -1;
static int g_label_sticky_mode  = -1;

/* Edit-guide labels draw in the in-pass overlay stage, before vertex labels.
 * Record their object-space raster anchor, current modelview, and measured
 * width on the final accumulation subpass; the post-resolve vertex-label pass
 * reprojects them with the canonical jitter-free projection. */
typedef struct {
    float pos[3];
    float modelview[16];
    float width;
} GuideLabelObstacle;

typedef struct {
    GuideLabelObstacle items[GUIDE_LABEL_OBSTACLE_MAX];
    int count;
} GuideLabelObstacleStore;

static GuideLabelObstacleStore g_guide_label_obstacles;

typedef struct {
    OverlayVertexLabelMode mode;
    int   is_ortho;
    /* For OVERLAY_VERTEX_LABEL_INDEX_WORLD and INDEX_WORLD_FINE: inverse of the
     * camera/view matrix snapshotted at the start of the walk, so a per-vertex
     * world position is view_inv * modelview * vertex (modelview = view *
     * accumulated model transforms). view_inv_ok is 0 if the view matrix was singular. */
    float view_inv[16];
    int   view_inv_ok;
    /* Projection + viewport snapshotted before the walk (the walker only
     * touches the modelview), used to project each vertex to screen space. */
    float proj[16];
    int   vw, vh;
    VertexLabel labels[VERTEX_LABEL_MAX];
    int   count;
    /* Which vertices get a label (OverlayScope) and where each one sits
     * (OverlayLabelPlacement). Orthogonal: scope narrows the walk, placement
     * only decides whether the 2D layout pass runs. */
    int   scope;
    int   placement;
    int   block_instances;   /* selected blocks (loop iterations) seen so far  */
    int   labelable_seen;    /* running index over all visited labelable verts */
    /* SINGLE_POLYGON scope: the cursor primitive's block-local ordinal range
     * (copied from OverlayWalkCtx). poly_valid == 0 -> whole-block labels. */
    int   poly_valid;
    int   poly_first;
    int   poly_last;
    int   poly_fan_anchor;
    /* Scene depth buffer (vw*vh, viewport-local, bottom-up) read once before
     * the walk, for the occlusion cull; NULL only if the readback failed. */
    const float *depthbuf;
} VertexLabelCtx;

static float sanitize_zero(float val) {
    if (val > -0.005f && val < 0.005f) {
        return 0.0f;
    }
    return val;
}

static int normalize_vec3(const float in[3], float out[3]) {
    float len = sqrtf(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
    if (len <= NORMAL_FRAME_EPS)
        return 0;
    out[0] = in[0] / len;
    out[1] = in[1] / len;
    out[2] = in[2] / len;
    return 1;
}

static int vec3_dirs_differ(const float a[3], const float b[3]) {
    return fabsf(a[0] - b[0]) > 0.005f ||
           fabsf(a[1] - b[1]) > 0.005f ||
           fabsf(a[2] - b[2]) > 0.005f;
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
                             float *sx, float *sy, float *depth) {
    float eye[4], clip[4], inv;
    mat4_mul_vec4(mv, x, y, z, 1.0f, eye);
    mat4_mul_vec4(proj, eye[0], eye[1], eye[2], eye[3], clip);
    if (clip[3] <= 1e-5f)
        return 0;
    inv    = 1.0f / clip[3];
    *sx    = (clip[0] * inv * 0.5f + 0.5f) * (float)vw;
    *sy    = (clip[1] * inv * 0.5f + 0.5f) * (float)vh;
    *depth = (clip[2] * inv * 0.5f + 0.5f);  /* window depth [0,1] */
    return 1;
}

static float bitmap_text_width(void *font, const char *s) {
    float w = 0.0f;
    for (; s && *s; s++)
        w += (float)glutBitmapWidth(font, (unsigned char)*s);
    return w;
}

static void guide_label_obstacles_reset(void) {
    g_guide_label_obstacles.count = 0;
}

static void guide_label_obstacles_record(
    void *user_data,
    const Render3dGuideLabelSpec *label) {
    GuideLabelObstacle *obstacle;
    float width = 0.0f;
    int run_count;
    int i;

    (void)user_data;
    if (!label || g_guide_label_obstacles.count >= GUIDE_LABEL_OBSTACLE_MAX)
        return;

    run_count = label->run_count;
    if (run_count < 0)
        run_count = 0;
    if (run_count > RENDER3D_GUIDE_LABEL_MAX_RUNS)
        run_count = RENDER3D_GUIDE_LABEL_MAX_RUNS;
    for (i = 0; i < run_count; i++) {
        if (label->runs[i].font && label->runs[i].text)
            width += bitmap_text_width(label->runs[i].font,
                                       label->runs[i].text);
    }
    if (width < 1.0f)
        return;

    obstacle = &g_guide_label_obstacles.items[g_guide_label_obstacles.count++];
    obstacle->pos[0] = label->pos[0];
    obstacle->pos[1] = label->pos[1];
    obstacle->pos[2] = label->pos[2];
    obstacle->width = width;
    glGetFloatv(GL_MODELVIEW_MATRIX, obstacle->modelview);
}

static int project_guide_label_obstacles(
    const VertexLabelCtx *ctx,
    float *out_x, float *out_y, float *out_w, int out_cap) {
    int count = 0;
    int i;

    if (!ctx || !out_x || !out_y || !out_w || out_cap <= 0)
        return 0;

    for (i = 0; i < g_guide_label_obstacles.count && count < out_cap; i++) {
        const GuideLabelObstacle *obstacle = &g_guide_label_obstacles.items[i];
        float sx, sy, depth;
        if (!project_to_screen(obstacle->modelview, ctx->proj, ctx->vw, ctx->vh,
                               obstacle->pos[0], obstacle->pos[1],
                               obstacle->pos[2], &sx, &sy, &depth))
            continue;
        if (sx < 0.0f || sx >= (float)ctx->vw ||
            sy < 0.0f || sy >= (float)ctx->vh)
            continue;
        out_x[count] = sx;
        out_y[count] = sy;
        out_w[count] = obstacle->width;
        count++;
    }
    return count;
}

/* Does a candidate label box - inflated by `margin` px on every side -
 * overlap any already-placed box? All boxes share VERTEX_LABEL_LINE_H.
 * margin > 0 demands headroom (a label entering or homing); margin < 0 is
 * lenient (an incumbent keeping its held row). */
static int label_cand_clashes(const float *px, const float *py,
                              const float *pw, int placed_n,
                              float x, float y, float w, float margin) {
    const float h = VERTEX_LABEL_LINE_H;
    int p;
    for (p = 0; p < placed_n; p++) {
        if (x - margin < px[p] + pw[p] && px[p] < x + w + margin &&
            y - margin < py[p] + h    && py[p] < y + h + margin)
            return 1;
    }
    return 0;
}

/* Guide labels are fixed-priority obstacles. Positive entry/homing margin
 * applies to them just like placed vertex labels, but incumbent keep-shrink
 * never relaxes them below strict non-overlap. */
static int vertex_label_cand_clashes(
    const float *placed_x, const float *placed_y, const float *placed_w,
    int placed_n,
    const float *obstacle_x, const float *obstacle_y, const float *obstacle_w,
    int obstacle_n,
    float x, float y, float w, float margin) {
    float obstacle_margin = margin > 0.0f ? margin : 0.0f;

    if (label_cand_clashes(placed_x, placed_y, placed_w, placed_n,
                           x, y, w, margin))
        return 1;
    return label_cand_clashes(obstacle_x, obstacle_y, obstacle_w, obstacle_n,
                              x, y, w, obstacle_margin);
}

static void on_vertex_number_label(const ReplayVertexWalkState *state,
                                   float vx, float vy, float vz,
                                   void *user) {
    VertexLabelCtx *ctx = (VertexLabelCtx *)user;
    float mv[16], sx, sy, depth;
    VertexLabel *lbl;
    int label_num, global_num;

    if (!ctx ||
        (ctx->mode != OVERLAY_VERTEX_LABEL_INDEX &&
         ctx->mode != OVERLAY_VERTEX_LABEL_INDEX_POS &&
         ctx->mode != OVERLAY_VERTEX_LABEL_INDEX_WORLD &&
         ctx->mode != OVERLAY_VERTEX_LABEL_INDEX_WORLD_FINE))
        return;

    /* A flat program unrolls loops, so a looped primitive becomes many blocks
     * that all match the cursor's source line. vertex_idx_in_block resets to 0
     * at each block, so it marks block boundaries; count them. The global index
     * advances for every labelable vertex (independent of the off-screen cull
     * below), so a given vertex keeps the same number as the camera moves. */
    /* Last-instance mode: only the last unrolled copy (else the torus repeats
     * v0..vN once per ring). The walk is forward-only and can't know which copy
     * is last, so every new copy simply discards what the previous one
     * collected. All-instances, Whole-scene and At-vertex modes keep all
     * copies; declutter pass still drops whatever doesn't fit. Single-polygon
     * narrows last-instance further to the primitive under the cursor - but
     * only for glBegin blocks (primitive_mode != 0); tess (GLU) polygons keep
     * whole-block labels by design. */
    if (state->vertex_idx_in_block == 0) {
        ctx->block_instances++;
        if ((ctx->scope == OVERLAY_SCOPE_LAST_INSTANCE ||
             ctx->scope == OVERLAY_SCOPE_SINGLE_POLYGON) &&
            ctx->block_instances > 1)
            ctx->count = 0;
    }
    global_num = ctx->labelable_seen++;

    if (ctx->scope == OVERLAY_SCOPE_SINGLE_POLYGON &&
        ctx->poly_valid && state->primitive_mode != 0) {
        int ord = state->vertex_idx_in_block;
        if (!((ord >= ctx->poly_first && ord <= ctx->poly_last) ||
              (ctx->poly_fan_anchor && ord == 0)))
            return;
    }
    if (ctx->count >= VERTEX_LABEL_MAX)
        return;

    /* GL_MODELVIEW here is view * model (the walker has applied the model
     * transforms up to this vertex), so this is exactly where a direct
     * glRasterPos3f(vx, vy, vz) would have landed the text. */
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    if (!project_to_screen(mv, ctx->proj, ctx->vw, ctx->vh, vx, vy, vz,
                           &sx, &sy, &depth))
        return;  /* behind the camera */
    if (sx < 0.0f || sx >= (float)ctx->vw || sy < 0.0f || sy >= (float)ctx->vh)
        return;  /* projects off-screen: a direct glRasterPos3f would have
                    produced an invalid raster position and drawn nothing here */

    /* Drop the vertex if the scene rendered nearer geometry at this pixel
     * (i.e. the vertex is occluded) - labels never show through solid
     * geometry, in any scope. depthbuf is NULL only when the readback failed;
     * the off-screen guard above keeps the index in range. */
    if (ctx->depthbuf) {
        float scene_d = ctx->depthbuf[(int)sy * ctx->vw + (int)sx];
        if (depth > scene_d + VERTEX_LABEL_OCCLUDE_BIAS)
            return;
    }

    /* Numbering exists to disambiguate, so it keys on whether ambiguity is
     * possible - which is a question about both scope and placement.
     *
     * A decluttered label floats away from its vertex on a leader line, so
     * once a scope keeps more than one unrolled copy a repeated v0..vN per
     * copy is genuinely unreadable: number those globally. An at-vertex label
     * sits ON the vertex it names, so there is nothing to disambiguate and
     * the in-block ordinal is the more useful reading (and stays small -
     * global numbering over a torus puts four-digit numbers on every vertex).
     * Single-copy scopes are unambiguous either way. */
    label_num = (ctx->placement != OVERLAY_LABEL_PLACEMENT_AT_VERTEX &&
                 (ctx->scope == OVERLAY_SCOPE_ALL_INSTANCES ||
                  ctx->scope == OVERLAY_SCOPE_WHOLE_SCENE))
                ? global_num : state->vertex_idx_in_block;
    lbl = &ctx->labels[ctx->count];
    lbl->num = label_num;
    snprintf(lbl->idx, sizeof(lbl->idx), " v%d", label_num);
    lbl->detail[0] = '\0';
    if (ctx->mode == OVERLAY_VERTEX_LABEL_INDEX_POS) {
        if (ctx->is_ortho)
            snprintf(lbl->detail, sizeof(lbl->detail), " (%.2f, %.2f)",
                     sanitize_zero(vx), sanitize_zero(vy));
        else
            snprintf(lbl->detail, sizeof(lbl->detail), " (%.2f, %.2f, %.2f)",
                     sanitize_zero(vx), sanitize_zero(vy), sanitize_zero(vz));
    } else if ((ctx->mode == OVERLAY_VERTEX_LABEL_INDEX_WORLD ||
                ctx->mode == OVERLAY_VERTEX_LABEL_INDEX_WORLD_FINE) &&
               ctx->view_inv_ok) {
        /* Map the vertex into eye space, then undo the camera to land in world
         * space. Cheap: labels only fire for the cursor's selected block. */
        float eye[3], world[3];
        const char *fmt = (ctx->mode == OVERLAY_VERTEX_LABEL_INDEX_WORLD_FINE) ? " (%.6g, %.6g, %.6g)" : " (%.2f, %.2f, %.2f)";
        const char *fmt_ortho = (ctx->mode == OVERLAY_VERTEX_LABEL_INDEX_WORLD_FINE) ? " (%.6g, %.6g)" : " (%.2f, %.2f)";
        mat4_transform_point(mv, vx, vy, vz, eye);
        mat4_transform_point(ctx->view_inv, eye[0], eye[1], eye[2], world);
        if (ctx->is_ortho)
            snprintf(lbl->detail, sizeof(lbl->detail), fmt_ortho,
                     sanitize_zero(world[0]), sanitize_zero(world[1]));
        else
            snprintf(lbl->detail, sizeof(lbl->detail), fmt,
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
    float obstacle_x[GUIDE_LABEL_OBSTACLE_MAX];
    float obstacle_y[GUIDE_LABEL_OBSTACLE_MAX];
    float obstacle_w[GUIDE_LABEL_OBSTACLE_MAX];
    int   placed_n = 0;
    int   obstacle_n = 0;
    const float dy = VERTEX_LABEL_LINE_H + VERTEX_LABEL_GAP;
    int i, oi, step, s;

    if (ctx->count <= 0)
        return;

    for (i = 0; i < ctx->count; i++) {
        VertexLabel *l = &ctx->labels[i];
        l->width = bitmap_text_width(FONT_MONO, l->idx) +
                   bitmap_text_width(FONT_TINY, l->detail);
        if (l->width < 1.0f)
            l->width = 1.0f;
    }

    if (ctx->placement == OVERLAY_LABEL_PLACEMENT_AT_VERTEX) {
        for (i = 0; i < ctx->count; i++) {
            VertexLabel *l = &ctx->labels[i];
            l->drawn = 1;
            l->draw_y = l->anchor_y;
        }
    } else {
        obstacle_n = project_guide_label_obstacles(
            ctx, obstacle_x, obstacle_y, obstacle_w,
            GUIDE_LABEL_OBSTACLE_MAX);
        /* Place in collection (walk) order - a camera-independent priority - so two
         * labels competing for the same screen region never swap which one claims
         * the anchor vs. gets nudged/dropped as the camera moves. Sorting by depth
         * instead made near-equal depths flip every frame (a z-fighting-like
         * shimmer). The trade is that the kept subset is the earlier-in-walk one
         * rather than strictly the closest.
         *
         * On top of that ordering, each label's placement is temporally
         * sticky across frames (see the TEMPORALLY STICKY note above). */
        g_label_sticky_frame++;
        if ((int)ctx->mode != g_label_sticky_mode ||
            ctx->scope != g_label_sticky_scope) {
            memset(g_label_sticky, 0, sizeof(g_label_sticky));
            g_label_sticky_mode  = (int)ctx->mode;
            g_label_sticky_scope = ctx->scope;
        }
        for (oi = 0; oi < ctx->count; oi++) {
            VertexLabel *l = &ctx->labels[oi];
            VertexLabelSticky *st =
                (l->num >= 0 && l->num < MAX_FLAT_COMMANDS)
                    ? &g_label_sticky[l->num]
                    : NULL;
            int   was_drawn = st && st->last_drawn + 1u == g_label_sticky_frame;
            int   prev_row  = was_drawn ? st->row : 0;
            int   prev_dist = prev_row < 0 ? -prev_row : prev_row;
            int   best_row  = 0;
            int   found     = 0;
            float target_dy, draw_dy;

            /* Phase A: incumbent homing. A row nearer the anchor than the
             * held one, taken only with comfortable headroom so
             * home-and-back can't flap. */
            if (was_drawn && prev_row != 0) {
                for (step = 0; step < prev_dist && !found; step++) {
                    for (s = (step == 0 ? 0 : -1); s <= 1 && !found; s += 2) {
                        int row = s * step;
                        if (!vertex_label_cand_clashes(
                                placed_x, placed_y, placed_w, placed_n,
                                obstacle_x, obstacle_y, obstacle_w, obstacle_n,
                                l->anchor_x,
                                l->anchor_y + (float)row * dy,
                                l->width, VERTEX_LABEL_ENTER_PAD)) {
                            best_row = row;
                            found = 1;
                        }
                    }
                }
            }
            /* Phase B: keep the held row, judged leniently - evicting a
             * visible label needs a real collision, not a boundary graze. */
            if (was_drawn && !found &&
                !vertex_label_cand_clashes(
                    placed_x, placed_y, placed_w, placed_n,
                    obstacle_x, obstacle_y, obstacle_w, obstacle_n,
                    l->anchor_x,
                    l->anchor_y + (float)prev_row * dy,
                    l->width, -VERTEX_LABEL_KEEP_SHRINK)) {
                best_row = prev_row;
                found = 1;
            }
            /* Phase C: plain near-to-far scan. A label that wasn't visible
             * last frame enters only with headroom, so the crowding
             * boundary doesn't strobe. */
            if (!found) {
                float margin = was_drawn ? 0.0f : VERTEX_LABEL_ENTER_PAD;
                for (step = 0; step <= VERTEX_LABEL_NUDGE_STEPS && !found;
                     step++) {
                    for (s = (step == 0 ? 0 : -1); s <= 1 && !found; s += 2) {
                        int row = s * step;
                        if (was_drawn && row == prev_row)
                            continue; /* already tested leniently */
                        if (!vertex_label_cand_clashes(
                                placed_x, placed_y, placed_w, placed_n,
                                obstacle_x, obstacle_y, obstacle_w, obstacle_n,
                                l->anchor_x,
                                l->anchor_y + (float)row * dy,
                                l->width, margin)) {
                            best_row = row;
                            found = 1;
                        }
                    }
                }
            }
            /* Crowded out within the bounded search: drop the label rather than
             * stack it far from its vertex (which would streak a long leader). */
            l->drawn = found;
            if (!found)
                continue;
            target_dy = (float)best_row * dy;
            draw_dy   = target_dy;
            if (st) {
                if (was_drawn) {
                    /* Glide from last frame's drawn offset toward the solved
                     * row; snap when close so it settles exactly. */
                    float d = target_dy - st->eased_dy;
                    if (d > 0.5f || d < -0.5f)
                        draw_dy = st->eased_dy + d * VERTEX_LABEL_EASE;
                }
                st->eased_dy   = draw_dy;
                st->row        = best_row;
                st->last_drawn = g_label_sticky_frame;
            }
            l->draw_y          = l->anchor_y + draw_dy;
            /* Collision record uses the quantized slot, not the eased draw
             * position, so decisions stay stable while a label glides. */
            placed_x[placed_n] = l->anchor_x;
            placed_y[placed_n] = l->anchor_y + target_dy;
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
         * glRasterPos would invalidate and drop the whole string - keep Y in. */
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

typedef struct NormalVectorRenderCtx {
    float scale;
    int show_all;
    int replay_display;
    int anchor_idx;
    int xform_guide_mode;
    const FlatProgramView *program;
    int *stop_flag;
    Render3dGuideLabelSink label_sink;
    /* Overlay scope, applied to the show_all arrows only. The walk itself
     * stays unnarrowed (selected_block_only == 0) so the replay anchor below
     * is still reachable from any block; the scope is enforced per vertex
     * instead. `target_head` is the pre-resolved last matching block head for
     * the single-copy scopes, or -1 for "any matching block". cur_* is walk
     * state, maintained by on_normal_vector_cmd. */
    const OverlayWalkCtx *walk;
    int scope;
    int target_head;
    int cur_head;
    int cur_head_matches;
} NormalVectorRenderCtx;

/* Track which block the walk is inside, and whether that block belongs to the
 * cursor - the walk only computes block selection when it is narrowing itself,
 * and this pass deliberately is not. */
static void on_normal_vector_cmd(const ReplayVertexWalkState *state,
                                 void *user) {
    NormalVectorRenderCtx *ctx = (NormalVectorRenderCtx *)user;
    const GLCmd *cmd;

    if (!ctx || !ctx->walk || !ctx->program || !ctx->program->cmds)
        return;
    if (state->flat_cmd_idx < 0 || state->flat_cmd_idx >= ctx->program->cmd_count)
        return;
    cmd = &ctx->program->cmds[state->flat_cmd_idx];
    if (cmd->type == CMD_END || cmd->type == CMD_TESS_END) {
        /* Clear on the way out, or a stray vertex between blocks would
         * inherit the previous block's verdict. */
        ctx->cur_head = -1;
        ctx->cur_head_matches = 0;
        return;
    }
    if (cmd->type != CMD_BEGIN && cmd->type != CMD_TESS_BEGIN_POLYGON)
        return;

    ctx->cur_head = state->flat_cmd_idx;
    ctx->cur_head_matches = outline_block_matches_cursor(
        state->flat_cmd_idx, cmd->type == CMD_TESS_BEGIN_POLYGON, ctx->walk);
}

/* Does this vertex fall inside the overlay scope? Whole-scene takes every
 * vertex; the rest take the cursor's block, narrowed to one unrolled copy for
 * the single-copy scopes and to one primitive for single-polygon. */
static int normal_vector_in_scope(const NormalVectorRenderCtx *ctx,
                                  const ReplayVertexWalkState *state) {
    if (ctx->scope == OVERLAY_SCOPE_WHOLE_SCENE)
        return 1;
    if (!ctx->cur_head_matches)
        return 0;
    if (ctx->target_head >= 0 && ctx->cur_head != ctx->target_head)
        return 0;
    if (ctx->scope == OVERLAY_SCOPE_SINGLE_POLYGON && ctx->walk &&
        ctx->walk->cursor_poly_valid && state->primitive_mode != 0 &&
        !cursor_poly_contains_ordinal(ctx->walk, state->vertex_idx_in_block))
        return 0;
    return 1;
}

static void on_normal_vector_arrow(const ReplayVertexWalkState *state,
                                   float vx, float vy, float vz,
                                   void *user) {
    const NormalVectorRenderCtx *ctx = (const NormalVectorRenderCtx *)user;
    int is_anchor;
    int draw_focused;
    int show_this;
    if (!ctx)
        return;

    is_anchor = ctx->anchor_idx >= 0 && state->flat_cmd_idx == ctx->anchor_idx;
    show_this = ctx->show_all && normal_vector_in_scope(ctx, state);
    if (!ctx->show_all) {
        if (!is_anchor)
            return;
        if (ctx->stop_flag)
            *ctx->stop_flag = 1;
        if (!state->lighting_enabled)
            return;
    }

    draw_focused = ctx->replay_display != REPLAY_NORMAL_DISPLAY_OFF &&
                   is_anchor &&
                   state->lighting_enabled;

    if (show_this) {
        render3d_clr(RENDER3D_CLR_NORMAL_LABEL);
        render3d_draw_normal_vector_arrow(vx, vy, vz,
                                       state->normal[0],
                                       state->normal[1],
                                       state->normal[2],
                                       ctx->scale);
    }

    if (draw_focused) {
        char detail[128];
        const char *primary = NULL;
        const char *detail_text = NULL;
        if (ctx->replay_display == REPLAY_NORMAL_DISPLAY_DIRECTION) {
            float authored[3] = { state->normal[0],
                                  state->normal[1],
                                  state->normal[2] };
            float authored_unit[3];
            float frame_normal[3];
            if (ctx->xform_guide_mode == RENDER3D_XFORM_GUIDE_FRAME &&
                ctx->program &&
                normalize_vec3(authored, authored_unit) &&
                compute_normal_frame_args(ctx->program, state->flat_cmd_idx,
                                          authored, frame_normal) &&
                vec3_dirs_differ(authored_unit, frame_normal)) {
                snprintf(detail, sizeof(detail),
                         "=(%.2f, %.2f, %.2f) -> frame=(%.2f, %.2f, %.2f)",
                         sanitize_zero(state->normal[0]),
                         sanitize_zero(state->normal[1]),
                         sanitize_zero(state->normal[2]),
                         sanitize_zero(frame_normal[0]),
                         sanitize_zero(frame_normal[1]),
                         sanitize_zero(frame_normal[2]));
            } else {
                snprintf(detail, sizeof(detail), "=(%.2f, %.2f, %.2f)",
                         sanitize_zero(state->normal[0]),
                         sanitize_zero(state->normal[1]),
                         sanitize_zero(state->normal[2]));
            }
            primary = " n";
            detail_text = detail;
        }
        if (primary && ctx->label_sink.record) {
            float authored[3] = { state->normal[0], state->normal[1],
                                  state->normal[2] };
            float unit[3];
            if (normalize_vec3(authored, unit)) {
                Render3dGuideLabelSpec label;
                memset(&label, 0, sizeof(label));
                label.pos[0] = vx + unit[0] * ctx->scale;
                label.pos[1] = vy + unit[1] * ctx->scale;
                label.pos[2] = vz + unit[2] * ctx->scale;
                label.runs[0].font = FONT_MONO;
                label.runs[0].text = primary;
                label.runs[1].font = FONT_TINY;
                label.runs[1].text = detail_text;
                label.run_count = 2;
                ctx->label_sink.record(ctx->label_sink.user_data, &label);
            }
        }
        render3d_draw_focused_normal_glyph(vx, vy, vz,
                                        state->normal[0],
                                        state->normal[1],
                                        state->normal[2],
                                        ctx->scale,
                                        1.0f,
                                        primary,
                                        detail_text);
    }
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
                                         int scope,
                                         int placement,
                                         int depth_readback_supported) {
    ReplayVertexWalkContext ctx;
    /* Big label store (the all-instances walk can collect a whole looped
     * surface); keep it off the render stack. Render is single-threaded and
     * this is reset per call. */
    static VertexLabelCtx label_ctx;
    GLint vp[4];
    float *depthbuf = NULL;   /* scene depth snapshot: occlusion cull */

    if (mode == OVERLAY_VERTEX_LABEL_OFF)
        return;

    /* Whole-scene is the one scope that is not anchored to the cursor's
     * block, so it is the one that walks the program unfiltered. */
    ctx = edit_overlays_build_vertex_walk_context(
        walk_ctx, scope != OVERLAY_SCOPE_WHOLE_SCENE);
    if (!ctx.program.cmds || ctx.program.cmd_count <= 0)
        return;

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    if (placement == OVERLAY_LABEL_PLACEMENT_AT_VERTEX) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        render3d_clr_a(RENDER3D_CLR_VERTEX_LABEL, 0.5f);
    } else {
        render3d_clr(RENDER3D_CLR_VERTEX_LABEL);
    }

    label_ctx.mode            = mode;
    label_ctx.is_ortho        = is_ortho;
    label_ctx.view_inv_ok     = 0;
    label_ctx.count           = 0;
    label_ctx.scope           = scope;
    label_ctx.placement       = placement;
    label_ctx.block_instances = 0;
    label_ctx.labelable_seen  = 0;
    label_ctx.depthbuf        = NULL;
    label_ctx.poly_valid      = walk_ctx ? walk_ctx->cursor_poly_valid : 0;
    label_ctx.poly_first      = walk_ctx ? walk_ctx->cursor_poly_first : 0;
    label_ctx.poly_last       = walk_ctx ? walk_ctx->cursor_poly_last : 0;
    label_ctx.poly_fan_anchor = walk_ctx ? walk_ctx->cursor_poly_fan_anchor : 0;

    /* The scene's projection + viewport are live here and the walker leaves
     * them intact (it only pushes/pops the modelview), so snapshot them once
     * for the per-vertex projection in the callback. */
    glGetFloatv(GL_PROJECTION_MATRIX, label_ctx.proj);
    glGetIntegerv(GL_VIEWPORT, vp);
    label_ctx.vw = vp[2];
    label_ctx.vh = vp[3];

    /* Snapshot the scene depth buffer once (one readback, not a per-vertex GL
     * round-trip) so the callback can cull occluded vertices. The scene
     * geometry has already drawn its depths by the time overlays run. Indexed
     * viewport-local (bottom-up), matching project_to_screen. Occlusion
     * applies in every scope when the context supports this readback. WebGL
     * forbids reading default-framebuffer depth and gl4es may silently no-op,
     * so the controller's capability probe must gate both the allocation and
     * read; without a valid snapshot, labels deliberately remain visible. */
    if (depth_readback_supported && label_ctx.vw > 0 && label_ctx.vh > 0) {
        size_t n = (size_t)label_ctx.vw * (size_t)label_ctx.vh;
        depthbuf = malloc(n * sizeof(float));
        if (depthbuf) {
            glReadPixels(vp[0], vp[1], label_ctx.vw, label_ctx.vh,
                         GL_DEPTH_COMPONENT, GL_FLOAT, depthbuf);
            label_ctx.depthbuf = depthbuf;
        }
    }

    if (mode == OVERLAY_VERTEX_LABEL_INDEX_WORLD ||
        mode == OVERLAY_VERTEX_LABEL_INDEX_WORLD_FINE) {
        /* The modelview here is the camera/view matrix - the walker has not yet
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

    free(depthbuf);
    glPopAttrib();
}

typedef struct ReplayVertexLabelCtx {
    int anchor_idx;
    int next_vertex_idx;
    int *stop_flag;
} ReplayVertexLabelCtx;

static void on_replay_vertex_label(const ReplayVertexWalkState *state,
                                   float vx, float vy, float vz,
                                   void *user) {
    ReplayVertexLabelCtx *ctx = (ReplayVertexLabelCtx *)user;
    int label_idx;
    char text[24];

    if (!ctx)
        return;

    label_idx = ctx->next_vertex_idx++;
    if (state->flat_cmd_idx != ctx->anchor_idx)
        return;

    snprintf(text, sizeof(text), " v%d", label_idx);
    render3d_draw_vertex_label_text(vx, vy, vz, text, "");
    if (ctx->stop_flag)
        *ctx->stop_flag = 1;
}

static void edit_overlays_render_replay_vertex_label(const OverlayWalkCtx *walk_ctx) {
    ReplayVertexWalkContext ctx;
    ReplayVertexLabelCtx label_ctx;
    int stop = 0;
    int anchor_idx;

    if (!walk_ctx || !walk_ctx->replay_vertex_label)
        return;

    ctx = edit_overlays_build_vertex_walk_context(walk_ctx, 0);
    if (!ctx.program.cmds || ctx.program.cmd_count <= 0)
        return;

    anchor_idx = walk_ctx->replay_anchor_flat_idx;
    if (anchor_idx < 0 || anchor_idx >= ctx.program.cmd_count)
        return;
    if (!ctx.program.cmds[anchor_idx].valid ||
        !repl_cmd_emits_vertex(ctx.program.cmds[anchor_idx].type))
        return;

    ctx.stop_flag = &stop;
    label_ctx.anchor_idx = anchor_idx;
    label_ctx.next_vertex_idx = 0;
    label_ctx.stop_flag = &stop;

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    render3d_clr(RENDER3D_CLR_VERTEX_LABEL);

    static const ReplayVertexWalkCallbacks cb = {
        .on_vertex = on_replay_vertex_label,
    };
    replay_walk_user_vertices(&ctx, &cb, &label_ctx);

    glPopAttrib();
}

#define GLR_NORMAL_ARROW_SCALE 0.35f

static void edit_overlays_render_normal_vectors_with_sink(
    const OverlayWalkCtx *walk_ctx,
    const Render3dGuideLabelSink *label_sink) {
    ReplayVertexWalkContext ctx;

    ctx = edit_overlays_build_vertex_walk_context(walk_ctx, 0);
    if (!ctx.program.cmds || ctx.program.cmd_count <= 0)
        return;

    int replay_display = walk_ctx
                         ? walk_ctx->replay_normal_display
                         : REPLAY_NORMAL_DISPLAY_OFF;
    int replay_requested;
    int show_all;
    int stop = 0;
    int anchor_idx = walk_ctx ? walk_ctx->replay_anchor_flat_idx : -1;
    if (replay_display < REPLAY_NORMAL_DISPLAY_OFF ||
        replay_display >= REPLAY_NORMAL_DISPLAY_COUNT) {
        replay_display = REPLAY_NORMAL_DISPLAY_OFF;
    }
    replay_requested = replay_display != REPLAY_NORMAL_DISPLAY_OFF;
    show_all = !walk_ctx || walk_ctx->show_normal_vectors || !replay_requested;
    if (replay_requested) {
        int anchor_valid = anchor_idx >= 0 && anchor_idx < ctx.program.cmd_count;
        if (anchor_valid) {
            anchor_valid = ctx.program.cmds[anchor_idx].valid &&
                           repl_cmd_emits_vertex(ctx.program.cmds[anchor_idx].type);
        }
        if (!anchor_valid)
            replay_display = REPLAY_NORMAL_DISPLAY_OFF;
        if (replay_display == REPLAY_NORMAL_DISPLAY_OFF && !show_all)
            return;
        if (!show_all)
            ctx.stop_flag = &stop;
    }

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    render3d_clr(RENDER3D_CLR_NORMAL_LABEL);

    static const ReplayVertexWalkCallbacks cb = {
        .on_each_cmd = on_normal_vector_cmd,
        .on_vertex = on_normal_vector_arrow,
    };
    int scope = walk_ctx ? (int)walk_ctx->cursor_overlay_scope
                         : OVERLAY_SCOPE_WHOLE_SCENE;
    NormalVectorRenderCtx render_ctx = {
        .scale = GLR_NORMAL_ARROW_SCALE,
        .show_all = show_all,
        .replay_display = replay_display,
        .anchor_idx = anchor_idx,
        .xform_guide_mode = walk_ctx ? walk_ctx->xform_guide_mode
                                     : RENDER3D_XFORM_GUIDE_OFF,
        .program = &ctx.program,
        .stop_flag = show_all ? NULL : &stop,
        .label_sink = label_sink ? *label_sink
                                 : (Render3dGuideLabelSink){0},
        .walk = walk_ctx,
        .scope = scope,
        /* Resolved before the walk: it runs forward and cannot know which
         * matching copy is the last one until it has passed them all, and an
         * arrow already drawn cannot be taken back the way a collected label
         * can. Costs one GL-free pass over the flat program. */
        .target_head = -1,
        .cur_head = -1,
        .cur_head_matches = 0,
    };
    if (walk_ctx && cursor_scope_selects_one_instance(walk_ctx)) {
        int head = last_matching_block_head(walk_ctx, CMD_BEGIN, 0);
        if (head < 0)
            head = last_matching_block_head(walk_ctx, CMD_TESS_BEGIN_POLYGON, 1);
        render_ctx.target_head = head;
    }
    replay_walk_user_vertices(&ctx, &cb, &render_ctx);

    glPopAttrib();
}

void edit_overlays_render_normal_vectors(const OverlayWalkCtx *walk_ctx) {
    edit_overlays_render_normal_vectors_with_sink(walk_ctx, NULL);
}

typedef struct {
    const Render3dGuideSnapshot *snapshot;
    Render3dTransformGuidePlan   xform_plan;
    float                     cam_view[16];
    int                       have_xform;
    int                       geometry_guide_done;
    /* Flat index the geometry (vertex/normal) guide must wait for, or -1 to
     * take the first command from the cursor row. Set for the last-instance
     * scopes so the guide reads the same unrolled copy the highlight uses. */
    int                       geometry_target_flat_idx;
    int                       early_stop;
} CursorGuideRenderCtx;

/* Last valid flat command originating from `src_line`, or -1. */
static int last_flat_idx_for_source_line(const FlatProgramView *flat,
                                         int src_line) {
    int found = -1;

    if (!flat || !flat->cmds) return -1;
    for (int i = 0; i < flat->cmd_count; i++) {
        if (flat->cmds[i].valid && flat->cmds[i].src_cmd_idx == src_line)
            found = i;
    }
    return found;
}

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
    } else if (flat->type == CMD_RASTER_POS3F) {
        snap.raster_pos_args[0] = flat->args[0];
        snap.raster_pos_args[1] = flat->args[1];
        snap.raster_pos_args[2] = flat->args[2];
        snap.raster_pos_n_filled = 3;
    } else if (flat->type == CMD_CLIP_PLANE) {
        /* args[0] is the plane enum; the equation follows. Flatten
         * re-bakes these each frame for has_vars lines, so an animated
         * plane (d = t, say) sweeps live. */
        snap.clip_plane_args[0] = flat->args[1];
        snap.clip_plane_args[1] = flat->args[2];
        snap.clip_plane_args[2] = flat->args[3];
        snap.clip_plane_args[3] = flat->args[4];
        snap.clip_plane_n_filled = 4;
    } else if (flat->type == CMD_NORMAL3F || flat->type == CMD_TESS_NORMAL) {
        snap.normal_args[0] = flat->args[0];
        snap.normal_args[1] = flat->args[1];
        snap.normal_args[2] = flat->args[2];
        snap.normal_frame_args_valid = 0;
        if (snap.xform_guide_mode == RENDER3D_XFORM_GUIDE_FRAME &&
            compute_normal_frame_args(&snap.flat_program, flat_idx,
                                      snap.normal_args,
                                      snap.normal_frame_args)) {
            snap.normal_frame_args_valid = 1;
        }
        if (find_next_vertex_args_in_flat(&snap.flat_program, flat_idx,
                                          snap.normal_base_pos))
            snap.normal_base_pos_valid = 1;
    }
    return snap;
}

static void on_cmd_render_cursor_guides(const ReplayVertexWalkState *state,
                                        void *user) {
    CursorGuideRenderCtx *ctx = (CursorGuideRenderCtx *)user;
    int is_cursor = (ctx->geometry_target_flat_idx >= 0)
                    ? (state->flat_cmd_idx == ctx->geometry_target_flat_idx)
                    : (state->src_cmd_idx == ctx->snapshot->edit_line_idx);

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
     * post-walk step in edit_overlays_render_cursor_guides, not here - that
     * one path also covers the empty-program case where the walk never runs. */
}

void edit_overlays_render_cursor_guides(const Render3dGuideSnapshot *snapshot,
                                        const OverlayWalkCtx *walk_ctx) {
    ReplayVertexWalkContext walk;

    if (!snapshot || !snapshot->show_guides) return;

    CursorGuideRenderCtx ctx;
    ctx.snapshot = snapshot;
    ctx.geometry_guide_done = 0;
    ctx.geometry_target_flat_idx =
        (snapshot->prefer_last_instance && !snapshot->replaying)
        ? last_flat_idx_for_source_line(&snapshot->flat_program,
                                        snapshot->edit_line_idx)
        : -1;
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
     * empty program (nothing to walk) - but the transform guide must still
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
     * recomputes its own anchor frame, so a plan the walk never reached - an
     * empty flat program, or a brand-new line anchored at the flat tail -
     * flushes here. This is the transform analog of the vertex guide's
     * on_end / in_block append-row path (it's why a first-time transform
     * draws live, before commit, just like a first-time glVertex). */
    if (ctx.have_xform && !ctx.xform_plan.consumed) {
        render3d_transform_guides_render_if_due(snapshot, &ctx.xform_plan,
                                             ctx.xform_plan.cursor_flat_idx,
                                             ctx.cam_view);
    }

    /* A clip-plane line the walk never anchored (brand-new / uncommitted
     * row, or an empty program) still previews live while typing -
     * rendered in the world frame, since there's no flat instance whose
     * transform context could be replayed yet. Committed lines take the
     * walk path above and render in their true modelview. */
    if (!ctx.geometry_guide_done && !snapshot->replaying &&
        strncmp(snapshot->input, "glClipPlane(", 12) == 0)
        render3d_geometry_guides_render_for_cursor(snapshot);

    glPopMatrix();
    glPopAttrib();
}

void edit_overlays_post_overlays(void *user_data) {
    const OverlaySnapshotPack *pack = (const OverlaySnapshotPack *)user_data;
    Render3dGuideSnapshot snapshot;
    const Render3dGuideLabelSink *normal_label_sink = NULL;
    if (!pack) return;

    /* One call per scene subpass. Clearing here means the final accumulation
     * pass replaces earlier jittered/blurred records; the saved modelviews are
     * reprojected later with the canonical post-resolve projection. */
    guide_label_obstacles_reset();
    snapshot = pack->snapshot;
    snapshot.label_sink = (Render3dGuideLabelSink){0};
    /* At-vertex labels claim no layout row, so they are not obstacles the
     * guide text has to route around. */
    if (pack->vertex_label_mode != OVERLAY_VERTEX_LABEL_OFF &&
        pack->vertex_label_placement != OVERLAY_LABEL_PLACEMENT_AT_VERTEX) {
        snapshot.label_sink.record = guide_label_obstacles_record;
        snapshot.label_sink.user_data = NULL;
        normal_label_sink = &snapshot.label_sink;
    }

    prof_begin(PROF_RENDER3D_OVERLAY_OUTLINES);
    edit_overlays_render_outlines(&pack->walk, pack->multisample_enabled, pack->line_smooth_enabled);
    edit_overlays_render_vertex_points(&pack->walk);
    prof_accum_end(PROF_RENDER3D_OVERLAY_OUTLINES);

    prof_begin(PROF_RENDER3D_OVERLAY_TRANSFORM_GUIDES);
    edit_overlays_render_cursor_guides(&snapshot, &pack->walk);
    prof_accum_end(PROF_RENDER3D_OVERLAY_TRANSFORM_GUIDES);

    if (pack->show_normal_vectors ||
        pack->walk.show_normal_vectors ||
        pack->walk.replay_normal_display != REPLAY_NORMAL_DISPLAY_OFF) {
        prof_begin(PROF_RENDER3D_OVERLAY_NORMALS);
        edit_overlays_render_normal_vectors_with_sink(&pack->walk,
                                                      normal_label_sink);
        prof_accum_end(PROF_RENDER3D_OVERLAY_NORMALS);
    }
}

/* Bitmap-text label passes. Runs once per frame via the scene's
 * post_resolve_overlays_fn - after the accumulation resolve, not inside
 * the per-pass loop - so AA jitter / blur sub-passes can't stamp the
 * pixel-snapped glyphs at N slightly different positions and ghost the
 * text (the geometry overlays above stay in-pass and accumulate). */
void edit_overlays_post_resolve_overlays(void *user_data) {
    const OverlaySnapshotPack *pack = (const OverlaySnapshotPack *)user_data;
    if (!pack) return;

    /* Screen-anchored annotation, not geometry: a label says where a
     * vertex is, so a stencil mask meant for the program's polygons must
     * not delete it. Unlike the in-pass overlays this runs after the
     * scene pass popped its own GL_ALL_ATTRIB_BITS frame, so it brackets
     * its own suspension. */
    glPushAttrib(GL_ENABLE_BIT);
    glDisable(GL_STENCIL_TEST);

    if (pack->vertex_label_mode != OVERLAY_VERTEX_LABEL_OFF) {
        prof_begin(PROF_RENDER3D_OVERLAY_VERTEX_NUMBERS);
        edit_overlays_render_vertex_numbers(&pack->walk,
                                            pack->vertex_label_mode,
                                            pack->ortho_mode,
                                            pack->overlay_scope,
                                            pack->vertex_label_placement,
                                            pack->depth_readback_supported);
        prof_accum_end(PROF_RENDER3D_OVERLAY_VERTEX_NUMBERS);
    }
    if (pack->walk.replay_vertex_label) {
        prof_begin(PROF_RENDER3D_OVERLAY_VERTEX_NUMBERS);
        edit_overlays_render_replay_vertex_label(&pack->walk);
        prof_accum_end(PROF_RENDER3D_OVERLAY_VERTEX_NUMBERS);
    }

    glPopAttrib();
}
