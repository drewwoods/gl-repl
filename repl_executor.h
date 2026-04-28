/*
 * repl_executor.h - GL command execution (immediate-mode rendering).
 *
 * Walks the flat command array emitting OpenGL calls. Handles two distinct
 * workflows:
 *
 *   1. Live frame rendering: repl_execute_program() called once per display
 *      callback, using the live g_flat_cmds[] with a clamp to the replay PC
 *      (if replay is active) or the full command count.
 *
 *   2. On-demand execution: Tests and tools can pass a FlatProgramView over
 *      temporary buffers for isolated execution without side effects.
 *
 * Each frame, expressions with the has_vars flag are re-evaluated with the
 * current predefined variable values (crucial for time-dependent expressions
 * like sin(t*speed) where 't' increments each frame). Expressions without
 * variable references use cached values.
 *
 * Matrix stack tracking (repl_executor_apply_tracked_transform_cmd,
 * repl_executor_unwind_tracked_transform_stack) maintains a depth counter
 * that scene_render.c uses for polygon outline and normal-vector overlays:
 * geometry drawn under different transform stacks is highlighted differently.
 *
 * Transform matrices are applied immediately (glTranslatef, glRotatef, etc.)
 * and are cumulative: the matrix stack is the canonical source of truth at
 * execution time (not stored in GLCmd). Color and other state is similarly
 * stateful: each glColor3f call sets the current color for subsequent vertices.
 */

#ifndef REPL_EXECUTOR_H
#define REPL_EXECUTOR_H

#include "repl_flatten.h"

/* Input: a flat program view and the number of commands to execute (typically
 * the full count, or the replay PC if replay is active). */
typedef struct {
    int             flat_cmd_count;
    FlatProgramView program;
} ReplExecutionOptions;

/* Apply a transform command (glTranslatef, glRotatef, glScalef, glPushMatrix,
 * glPopMatrix). Used independently by step-back code when re-executing from
 * a new PC. */
void repl_executor_apply_transform_cmd(const GLCmd *cmd);

/* Apply a transform command while tracking matrix stack depth. Used during
 * normal execution and replay to maintain an accurate depth counter for
 * overlay rendering (polygon outlines, normal vectors drawn at different
 * transform depths are colored distinctly). matrix_depth is incremented on
 * glPushMatrix and decremented on glPopMatrix. */
void repl_executor_apply_tracked_transform_cmd(const GLCmd *cmd, int *matrix_depth);

/* Pop the tracked transform stack back to depth 0 (called when execution
 * ends to clean up any unmatched glPushMatrix calls). */
void repl_executor_unwind_tracked_transform_stack(int *matrix_depth);

/* --- Lifecycle --------------------------------------------------------- */

/* One-time init: create display lists, compile shaders, allocate tessellator. */
void repl_executor_init_resources(void);

/* One-time cleanup: destroy tessellator, release display lists. */
void repl_executor_destroy_resources(void);

/* Update the executor's fade overlay context before a frame render. */
void repl_execute_set_fade_context(float alpha_scale, int skip_geom_before_pc);

/* Execute a flat program: walk cmds[0..flat_cmd_count), emit GL calls,
 * re-evaluate expressions with current predefined variable values. Called
 * once per frame from scene_render.c. */
void repl_execute_program(const ReplExecutionOptions *options);

#endif /* REPL_EXECUTOR_H */
