/*
 * transform_utils.h - Shared GL matrix transform helpers.
 *
 * Inline helpers for applying and unwinding tracked transform commands while
 * walking source or flat command arrays. The controller and transform-guide
 * renderers use these so they can mirror executor-style matrix tracking without
 * depending on src/repl/executor.h.
 *
 * The header depends only on repl/command.h and gl_includes.h, which keeps it
 * usable from either app- or scene-adjacent helpers.
 */
#ifndef TRANSFORM_UTILS_H
#define TRANSFORM_UTILS_H

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

#endif /* TRANSFORM_UTILS_H */
