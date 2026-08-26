/*
 * transform_utils.h - Shared GL matrix transform helpers.
 *
 * Inline helpers for applying and unwinding tracked transform commands while
 * walking source or flat command arrays. The replay walkers, edit-overlay
 * orchestration, and transform-guide renderers use these so they can mirror
 * executor-style matrix tracking without depending on (or linking)
 * src/repl/executor.h.
 *
 * The header depends only on repl/command.h and gl_includes.h, which keeps it
 * usable from repl-, subsystem-, or scene-adjacent helpers without adding a
 * link dependency (load-bearing for render3d_demo's no-REPL-objects build).
 */
#ifndef REPL_TRANSFORM_UTILS_H
#define REPL_TRANSFORM_UTILS_H

#include "repl/command.h"
#include "gl_includes.h"

#include <math.h>

/* Apply a single transform command to the GL matrix stack.
 * Increments *depth on glPushMatrix, decrements on glPopMatrix. */
static inline void apply_tracked_transform(const GLCmd *cmd, int *depth) {
    if (!cmd)
        return;

    switch (cmd->type) {
    case CMD_PUSH_MATRIX:
        glPushMatrix();
        if (depth)
            (*depth)++;
        break;
    case CMD_POP_MATRIX:
        if (!depth || *depth > 0) {
            glPopMatrix();
            if (depth)
                (*depth)--;
        }
        break;
    case CMD_LOAD_IDENTITY:
        glLoadIdentity();
        break;
    case CMD_TRANSLATE3F:
        glTranslatef(cmd->args[0], cmd->args[1], cmd->args[2]);
        break;
    case CMD_SCALEF:
        glScalef(cmd->args[0], cmd->args[1], cmd->args[2]);
        break;
    case CMD_ROTATEF:
        glRotatef(cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
        break;
    case CMD_MULT_MATRIXF:
        /* Values ride on the command (flatten snapshots the scratch array
         * into payload.matrix), so this stays a pure GLCmd read - no
         * scratch-table lookup, which is what keeps this header linkable
         * from render3d. */
        glMultMatrixf(cmd->payload.matrix.m);
        break;
    default:
        break;
    }
}

/* Pop the GL matrix stack until *depth reaches zero. */
static inline void unwind_transform_stack(int *depth) {
    if (!depth)
        return;

    while (*depth > 0) {
        glPopMatrix();
        (*depth)--;
    }
}

/* Backward in-scope transform scan over a flat command array, honoring
 * glPushMatrix/glPopMatrix/glLoadIdentity accounting: a pop crossed
 * walking back opens a skipped scope, the matching push closes it, and
 * a load-identity in the live scope terminates the scan. Shared by the
 * autonormal flat affecting-transform resolver and the transform-guide
 * replay focus walk (the consumers differ only in what they collect
 * per hit). Init with the index of the anchor command; each _next call
 * returns the index of the closest remaining in-scope glTranslatef /
 * glScalef / glRotatef (deliberately narrower than
 * repl_cmd_is_transform - the stack ops are the scan's bookkeeping,
 * not results), newest first, or -1 when the scan is exhausted. */
typedef struct TransformScopeScan {
    const GLCmd *cmds;
    int i;            /* next index to inspect (walks toward 0) */
    int popped_depth; /* scopes closed by pops crossed so far */
} TransformScopeScan;

static inline void transform_scope_scan_init(TransformScopeScan *scan,
                                             const GLCmd *cmds,
                                             int anchor_idx) {
    scan->cmds = cmds;
    scan->i = anchor_idx - 1;
    scan->popped_depth = 0;
}

static inline int transform_scope_scan_next(TransformScopeScan *scan) {
    while (scan->i >= 0) {
        int idx = scan->i--;
        CmdType t;
        if (!scan->cmds[idx].valid)
            continue;
        t = scan->cmds[idx].type;
        if (t == CMD_POP_MATRIX) {
            scan->popped_depth++;
        } else if (t == CMD_PUSH_MATRIX) {
            if (scan->popped_depth > 0)
                scan->popped_depth--;
        } else if (t == CMD_LOAD_IDENTITY) {
            if (scan->popped_depth == 0)
                break;
        } else if (t == CMD_TRANSLATE3F || t == CMD_SCALEF ||
                   t == CMD_ROTATEF) {
            if (scan->popped_depth == 0)
                return idx;
        }
    }
    scan->i = -1;
    return -1;
}

/* out = a * b for column-major 4x4 matrices (glGetFloatv layout). */
static inline void mat4_mul_col_major(const float a[16], const float b[16],
                                      float out[16]) {
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

/* Transform a point through a column-major 4x4 (implicit w = 1). */
static inline void mat4_point_col_major(const float m[16], const float p[3],
                                        float out[3]) {
    out[0] = m[0] * p[0] + m[4] * p[1] + m[8]  * p[2] + m[12];
    out[1] = m[1] * p[0] + m[5] * p[1] + m[9]  * p[2] + m[13];
    out[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
}

/* --- Software (GL-free) modelview tracking -----------------------------
 *
 * apply_tracked_transform above drives the *real* GL matrix stack, which is
 * what a walker that also draws wants. Consumers that need the modelview's
 * numeric value partway through a program cannot use it: reading the stack
 * back means glGetFloatv, a synchronous pipeline drain per query. These
 * mirror the same command set onto a caller-owned stack instead, in the
 * same column-major layout GL uses, so the two stay comparable cell for
 * cell. Consumers: the edit-overlay normal-frame solve and the scene-bounds
 * walk (src/repl/program_bounds.c).
 *
 * Kept inline in this header for the same reason the rest of the file is:
 * render3d links none of src/repl's objects. */

/* Degenerate-axis threshold for mat4_apply_rotate. glRotatef with a
 * zero-length axis is undefined; treating it as a no-op matches what the
 * drivers in practice do and keeps the walk total. */
#define MAT4_ROTATE_AXIS_EPS 1e-8f

/* Modelview stack depth. GL only guarantees 32 levels, but a flat program
 * can nest deeper than the driver would accept without the walk being
 * wrong about the commands it did see, so this is deliberately generous
 * and overflow latches rather than truncating silently. */
#define MAT4_STACK_MAX 128

static inline void mat4_identity(float m[16]) {
    for (int i = 0; i < 16; i++)
        m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static inline void mat4_copy(float dst[16], const float src[16]) {
    for (int i = 0; i < 16; i++)
        dst[i] = src[i];
}

/* m = m * rhs - the post-multiply every glTranslatef/glScalef/glRotatef
 * performs on the active matrix. */
static inline void mat4_post_mul(float m[16], const float rhs[16]) {
    float tmp[16];
    mat4_mul_col_major(m, rhs, tmp);
    mat4_copy(m, tmp);
}

static inline void mat4_apply_translate(float m[16], float x, float y, float z) {
    float t[16];
    mat4_identity(t);
    t[12] = x;
    t[13] = y;
    t[14] = z;
    mat4_post_mul(m, t);
}

static inline void mat4_apply_scale(float m[16], float x, float y, float z) {
    float s[16];
    mat4_identity(s);
    s[0]  = x;
    s[5]  = y;
    s[10] = z;
    mat4_post_mul(m, s);
}

static inline void mat4_apply_rotate(float m[16], float angle_deg,
                                     float x, float y, float z) {
    float axis_len = (float)sqrt((double)(x * x + y * y + z * z));
    float r[16];
    float angle, c, s, omc;

    if (axis_len <= MAT4_ROTATE_AXIS_EPS)
        return;

    x /= axis_len;
    y /= axis_len;
    z /= axis_len;

    angle = angle_deg * 0.01745329251994329577f;
    c = (float)cos((double)angle);
    s = (float)sin((double)angle);
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

/* Software twin of the GL matrix stack. `top` is the active matrix;
 * `saved` holds the pushed copies. `overflow` latches when a push ran past
 * MAT4_STACK_MAX - the walk continues (so the caller still sees the rest of
 * the program) but the caller can no longer trust the result. */
typedef struct Mat4Stack {
    float top[16];
    float saved[MAT4_STACK_MAX][16];
    int   depth;
    int   overflow;
} Mat4Stack;

static inline void mat4_stack_init(Mat4Stack *st) {
    mat4_identity(st->top);
    st->depth = 0;
    st->overflow = 0;
}

/* Apply one flat command to `st`, mirroring apply_tracked_transform's
 * command set exactly - glMultMatrixf included, which reads the 4x4 that
 * flatten baked onto the command (see GLCmd.payload.matrix). Non-transform
 * commands are ignored, so this is safe to call for every command in a
 * walk. */
static inline void mat4_stack_apply_cmd(Mat4Stack *st, const GLCmd *cmd) {
    if (!st || !cmd)
        return;

    switch (cmd->type) {
    case CMD_PUSH_MATRIX:
        if (st->depth >= MAT4_STACK_MAX) {
            st->overflow = 1;
            break;
        }
        mat4_copy(st->saved[st->depth], st->top);
        st->depth++;
        break;
    case CMD_POP_MATRIX:
        if (st->depth > 0) {
            st->depth--;
            mat4_copy(st->top, st->saved[st->depth]);
        }
        break;
    case CMD_LOAD_IDENTITY:
        mat4_identity(st->top);
        break;
    case CMD_TRANSLATE3F:
        mat4_apply_translate(st->top, cmd->args[0], cmd->args[1], cmd->args[2]);
        break;
    case CMD_SCALEF:
        mat4_apply_scale(st->top, cmd->args[0], cmd->args[1], cmd->args[2]);
        break;
    case CMD_ROTATEF:
        mat4_apply_rotate(st->top, cmd->args[0], cmd->args[1],
                          cmd->args[2], cmd->args[3]);
        break;
    case CMD_MULT_MATRIXF:
        mat4_post_mul(st->top, cmd->payload.matrix.m);
        break;
    default:
        break;
    }
}

#endif /* REPL_TRANSFORM_UTILS_H */
