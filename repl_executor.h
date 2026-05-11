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
#include "source_document.h"  /* SourceTextView (Phase 1 of feature/source-document-port.md) */

#define TESS_VERT_BUF_SIZE 256

typedef struct TessVertex {
    GLdouble pos[3];
    GLdouble normal[3];
    GLdouble color[4];
} TessVertex;

/* Input: a flat program view, the number of commands to execute
 * (typically the full count, or the replay PC if replay is active),
 * and a source-text view used to resolve display text for status
 * messages (goto-label resolution etc.). The view is non-owning and
 * stays valid for the duration of the execute call. */
typedef struct {
    int             flat_cmd_count;
    FlatProgramView program;
    SourceTextView  text;
} ReplExecutionOptions;

/* Get a view over the live flat program (g_flat_cmds, g_flat_local_vars).
 * The pointers are valid until the next call to repl_flatten_program()
 * on the live buffers. */
FlatProgramView repl_flat_program_view_live(void);

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

/* One-time init: create the shared GLU quadric and tessellator used by quadric
 * primitives. */
void repl_executor_init_resources(void);

/* One-time cleanup: destroy the shared quadric and tessellator. */
void repl_executor_destroy_resources(void);

/* Update the executor's fade overlay context before a frame render. */
void repl_execute_set_fade_context(float alpha_scale, int skip_geom_before_pc);

/* Execute the live flat program against the current editor buffer view. */
void repl_execute_commands(void);

/* Execute a flat program: walk cmds[0..flat_cmd_count), emit GL calls,
 * re-evaluate expressions with current predefined variable values. Called
 * once per frame from scene_render.c. */
void repl_execute_program(const ReplExecutionOptions *options);

/* Install a camera-distance source. The legacy point-size fallback
 * compiled in via `NO_POINT_PARAMETER=1` (for platforms missing
 * glPointParameterfv) needs the current camera distance to scale
 * `glPointSize` calls. Pipeline TUs cannot include glr_camera.h
 * (check-repl-state-no-glr-state would block them), so the executor
 * accepts a controller-installed callback instead. The controller
 * installs a function that returns `glr_camera().dist`. The demo
 * (and any caller without point-attenuation) leaves the source
 * unset; the fallback then emits `glPointSize(sz)` unchanged.
 *
 * Pass NULL to clear. Always available even when NO_POINT_PARAMETER
 * is not set (the executor stores the callback regardless; only the
 * compile-time fallback path consumes it) so callers can install
 * unconditionally. */
typedef float (*ReplExecutorCameraDistanceFn)(void);
void repl_executor_install_camera_distance_source(ReplExecutorCameraDistanceFn fn);
ReplExecutorCameraDistanceFn repl_executor_camera_distance_source(void);

#endif /* REPL_EXECUTOR_H */
