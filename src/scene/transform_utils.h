/*
 * scene_transform_utils.h - GL matrix transform helpers for scene modules.
 *
 * Inline helpers for applying and unwinding GL matrix transforms.
 * Mirrors repl_executor.c functionality without requiring repl_executor.h.
 * Depends only on repl_command.h (GLCmd, CmdType).
 */
#ifndef SCENE_TRANSFORM_UTILS_H
#define SCENE_TRANSFORM_UTILS_H

#include "repl_command.h"
#include <gl_includes.h>

/* Apply a single transform command to the GL matrix stack.
 * Increments *depth on glPushMatrix, decrements on glPopMatrix. */
static inline void scene_apply_tracked_transform(const GLCmd *cmd, int *depth) {
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
static inline void scene_unwind_transform_stack(int *depth) {
    if (!depth)
        return;

    while (*depth > 0) {
        glPopMatrix();
        (*depth)--;
    }
}

#endif /* SCENE_TRANSFORM_UTILS_H */
