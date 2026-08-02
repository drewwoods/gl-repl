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

#endif /* REPL_TRANSFORM_UTILS_H */
