/*
 * src/repl/executor.h - GL command execution (immediate-mode rendering).
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

#include "repl/flatten.h"
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

/* Apply a single state-mutating GL command (enable/disable/color/shade
 * model/blend func/etc.). Used inside the executor's own loop and by
 * the export fade pass to apply scene defaults without driving a full
 * program. `alpha_scale` multiplies any color alpha channel. */
int  repl_apply_state_cmd(const GLCmd *cmd, float alpha_scale);

/* Install a camera-distance source. The point-size fallback used when
 * the runtime GL context lacks glPointParameterfv needs the current
 * camera distance to scale `glPointSize` calls. Pipeline TUs cannot
 * include glr_camera.h (check-repl-state-no-glr-state would block
 * them), so the executor accepts a controller-installed callback
 * instead. The controller installs a function that returns
 * `glr_camera().dist`. The demo (and any caller without
 * point-attenuation) leaves the source unset; the fallback then emits
 * `glPointSize(sz)` unchanged.
 *
 * Pass NULL to clear. Always available; only consumed when point
 * parameters are unsupported (see below) so callers install
 * unconditionally. */
typedef float (*ReplExecutorCameraDistanceFn)(void);
void repl_executor_install_camera_distance_source(ReplExecutorCameraDistanceFn fn);

/* Runtime point-parameter capability (replaces the old compile-time
 * NO_POINT_PARAMETER macro). The controller detects support after the
 * GL context is current (glr_ctrl_init_gl) and sets this; everything
 * else defaults to supported. When unsupported, CMD_POINT_PARAMETER_FV
 * is a no-op in the executor and point sizes fall back to the
 * camera-distance approximation above; the scene controller mirrors
 * this into SceneRenderConfig so the star backdrop's direct call can
 * be gated too. */
void repl_executor_set_point_parameter_supported(int supported);
int  repl_executor_point_parameter_supported(void);

#endif /* REPL_EXECUTOR_H */
