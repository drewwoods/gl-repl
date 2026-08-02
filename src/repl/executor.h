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
 * The executor evaluates no expression text: every command renders from the
 * args[] baked by the most recent flatten. Time-dependent expressions like
 * sin(t*speed) animate because a playing 't' marks the flat program dirty
 * and the per-frame re-flatten re-bakes has_vars args before execution
 * (see ARCHITECTURE.md section 13.4).
 *
 * Matrix stack tracking (repl_executor_apply_tracked_transform_cmd,
 * repl_executor_unwind_tracked_transform_stack) maintains a depth counter
 * that render3d_render.c uses for polygon outline and normal-vector overlays:
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
#include "repl/attrib_bits.h"  /* REPL_ATTRIB_STACK_CAP */
#include "source_document.h"  /* SourceTextView (Phase 1 of feature/source-document-port.md) */

#ifndef APIENTRY
#define APIENTRY
#endif

#define TESS_VERT_BUF_SIZE 256

#include "repl/state_views.h"  /* ReplRenderState (attrib bookkeeping snapshot) */

/* One saved attribute-stack frame: the push mask plus the REPL render-state
 * bookkeeping mirror (light-enable mask + clear color) captured at push time.
 * glPopAttrib restores GL's own state; this mirror is restored alongside so
 * the light-indicator overlay and next-frame clear color track user pops. */
typedef struct {
    unsigned        mask;
    ReplRenderState render;
} ReplAttribSave;

/* Reference distance (world units) at which a point renders at its literal
 * glPointSize: point size scales as REF_DIST/d, a constant on-screen
 * footprint on the model. The single source of truth for both point-size
 * paths, so they can't drift:
 *   - The software fallback (no glPointParameterfv) scales glPointSize by
 *     REF_DIST/cam_dist directly - see repl_exec_point_size in executor.c.
 *   - The hardware default seeds GL_POINT_DISTANCE_ATTENUATION with a
 *     pure-quadratic coefficient 1/REF_DIST^2, which makes GL's
 *     size/sqrt(c*d^2) reduce to the same size*REF_DIST/d - see the init
 *     bootstrap in src/repl/export.c. */
#define REPL_POINT_SIZE_REF_DIST 4.0f

typedef struct TessVertex {
    GLdouble pos[3];
    GLdouble normal[3];
    GLdouble color[4];
} TessVertex;

/* Input: a flat program view, the number of commands to execute
 * (typically the full count, or the replay PC if replay is active),
 * and a source-text view used to resolve display text for status
 * messages (goto-label resolution etc.). The view is non-owning and
 * stays valid for the duration of the execute call.
 *
 * `suppress_tess_finalize` skips the trailing gluTessEndContour /
 * gluTessEndPolygon cleanup at the end of repl_execute_program.
 * Default 0 = finalize, which is correct for the live frame path and
 * for replay's POLYGON mode. Replay's VERTEX-mode fade batches set
 * it to 1 because each batch is a partial slice of the original
 * tess sequence; finalizing each slice would emit incomplete
 * geometry.
 *
 * `status_out` / `status_out_sz` give the executor a place to record
 * a non-fatal diagnostic (currently only the goto-loop-limit case).
 * If `status_out` is NULL, the executor drops the message; otherwise
 * it snprintfs up to `status_out_sz - 1` chars + NUL. Callers decide
 * whether to forward the captured text to the status bar
 * (controller live-frame: yes; replay/test/demo: no). */
typedef struct {
    int             flat_cmd_count;
    FlatProgramView program;
    SourceTextView  text;
    float           fade_alpha_scale;
    int             skip_geom_before_pc;
    int             has_fade_context;
    int             suppress_tess_finalize;
    /* When set (the .ply export pass), the executor: (1) mirrors each vertex's
     * current normal - transformed to world space - into the texture
     * coordinate channel, bracketing user glBegin/glEnd primitives with
     * glPassThrough(MESH_PLY_PASS_NORMALS / _NO_NORMALS) so the feedback parser
     * knows which texcoords are authored normals (vs solids / tess, which fall
     * back to synthesized normals); and (2) suppresses the program's
     * glEnable(GL_LIGHTING / GL_CULL_FACE) so feedback returns the raw glColor
     * material color (not per-vertex lit shading) and all faces (not just
     * front). No effect on the live frame. */
    int             encode_feedback_normals;
    /* Optional filter over the program's GL state/color-emitting commands.
     * Returns nonzero to emit the command's GL normally, zero to suppress
     * the GL emission (the REPL bookkeeping a state command carries - the
     * GL_LIGHTn enable mask and clear color - still runs, so render state
     * stays coherent). NULL emits everything (the default live-frame path).
     *
     * This lets a single render pass own the material/lighting/cull state
     * and stop the user program from clobbering it, without forking the
     * whole execution walk the way the hidden-line wireframe pass does. The
     * winding-visualization view installs one to suppress user materials,
     * glColorMaterial, and glEnable/Disable of lighting / cull-face / lights
     * so its two-sided-lighting setup survives the program. (The export
     * pass's encode_feedback_normals lighting/cull carve-out is the older,
     * special-cased ancestor of this hook.) */
    int           (*state_filter)(CmdType type, const GLCmd *cmd, void *ud);
    void           *state_filter_ud;
    /* When set, CMD_PUSH_ATTRIB / CMD_POP_ATTRIB still scope the REPL
     * bookkeeping mirror (light-enable mask + clear color) and maintain the
     * virtual/real attrib depth, but issue NO glPushAttrib/glPopAttrib. The
     * hidden-line wireframe pass owns its own depth/color/polygon GL state and
     * must not have the user program save/restore it out from under the pass;
     * it sets this so push/pop bookkeeping stays coherent without touching the
     * pass's live GL state. Default 0 = normal GL semantics. */
    int             suppress_attrib_gl;
    char           *status_out;
    int             status_out_sz;
} ReplExecutionOptions;

/* Stack-owned execution cursor over a flat REPL program. This is the same
 * execution machinery used by repl_execute_program(), exposed one command at a
 * time so specialized render passes can drive normal REPL semantics while
 * skipping pass-local commands explicitly. */
typedef struct ReplExecCursor {
    ReplExecutionOptions options;
    FlatProgramView      program;
    SourceTextView       text;
    int                  flat_cmd_count;
    int                  pc;
    int                  in_begin;
    int                  encode_normals;
    float                cur_normal[3];
    float                begin_mv[16];
    int                  tess_depth;
    int                  matrix_depth;
    /* glPushAttrib/glPopAttrib scoping. attrib_depth is the virtual (user-
     * source) push depth, unbounded; the real GL stack is only pushed while
     * attrib_depth <= REPL_ATTRIB_STACK_CAP, so attrib_save[] holds the frames
     * within the cap (LIFO). */
    int                  attrib_depth;
    ReplAttribSave       attrib_save[REPL_ATTRIB_STACK_CAP];
    GLdouble             tess_current_normal[3];
    GLdouble             tess_current_color[4];
    int                  goto_count;
    float                alpha_scale;
    int                  skip_geom_before_pc;
} ReplExecCursor;

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

/* Execute the live flat program against the current editor buffer view. */
void repl_execute_commands(void);

/* Execute a flat program: walk cmds[0..flat_cmd_count), emit GL calls,
 * re-evaluate expressions with current predefined variable values. Called
 * once per frame from render3d_render.c. */
void repl_execute_program(const ReplExecutionOptions *options);

/* Cursor API used by repl_execute_program() and specialized render passes.
 * begin() snapshots the non-owning program/text views from `options` (or live
 * REPL state when omitted), step() executes the current flat command and
 * advances the cursor, and end() performs the old whole-program cleanup:
 * closing an open glBegin, finalizing tessellation unless suppressed, and
 * unwinding tracked matrix pushes. advance() intentionally skips the current
 * command without executing it; callers that skip structural commands own the
 * matching state consequences. */
ReplExecCursor repl_exec_cursor_begin(const ReplExecutionOptions *options);
int repl_exec_cursor_step(ReplExecCursor *cursor);
void repl_exec_cursor_end(ReplExecCursor *cursor);
int repl_exec_cursor_done(const ReplExecCursor *cursor);
const GLCmd *repl_exec_cursor_peek(const ReplExecCursor *cursor);
void repl_exec_cursor_advance(ReplExecCursor *cursor);
int repl_exec_cursor_in_begin(const ReplExecCursor *cursor);

/* Draw a single glutSolid* command (CMD_GLUT_TORUS/CUBE/SPHERE/TEAPOT/
 * CONE) at the current modelview by dispatching the matching freeglut
 * shape call with the command's args. A no-op for any other type.
 * Shared by the executor's live render loop and the outline overlay's
 * GL_LINE wireframe redraw pass (edit_overlays.c), which can't trace
 * these shapes from REPL-tracked vertices because they emit none. */
void repl_executor_draw_glut_solid(const GLCmd *cmd);

/* Apply a single state-mutating GL command (enable/disable/color/shade
 * model/blend func/etc.). Used inside the executor's own loop and by
 * the export fade pass to apply scene defaults without driving a full
 * program. `alpha_scale` multiplies any color alpha channel. Folds in
 * repl_apply_state_bookkeeping() before emitting GL. */
int  repl_apply_state_cmd(const GLCmd *cmd, float alpha_scale);

/* Apply only the REPL render-state bookkeeping a state command carries -
 * the GL_LIGHTn enable mask (for the light-indicator overlay) and the
 * clear color (for export / next-frame clear) - without emitting any GL.
 * This is the single source of truth for "which commands carry REPL
 * render bookkeeping": repl_apply_state_cmd() calls it alongside the GL
 * emission, and specialized passes that own GL state themselves and skip
 * the user's state commands (the hidden-line wireframe pass) call it
 * directly so they stay in sync. A no-op for commands with no bookkeeping. */
void repl_apply_state_bookkeeping(const GLCmd *cmd);

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

/* Install the runtime-loaded glPointParameterfv entry point. On older
 * Linux libGL stacks the context can advertise point-parameter support
 * while the unsuffixed C symbol is not safely callable, so the caller
 * resolves a proc after the GL context is current and installs it here.
 * Pass NULL to clear. */
typedef void (APIENTRY *ReplExecutorPointParameterProc)(GLenum pname,
                                                        const GLfloat *params);
void repl_executor_install_point_parameter_proc(ReplExecutorPointParameterProc fn);
ReplExecutorPointParameterProc repl_executor_point_parameter_proc(void);

/* Runtime point-parameter capability (replaces the old compile-time
 * NO_POINT_PARAMETER macro). The controller detects support after the
 * GL context is current (glr_ctrl_init_gl), resolves a callable
 * glPointParameterfv/glPointParameterfvARB/glPointParameterfvEXT proc,
 * and sets both this flag and the proc above. When unsupported,
 * CMD_POINT_PARAMETER_FV is a no-op in the executor and point sizes
 * fall back to the camera-distance approximation above. export.c consults
 * repl_executor_point_parameter_supported() to decide whether to
 * apply/emit the point-attenuation init bootstrap entry, and the scene
 * controller mirrors both into Render3dRenderConfig so the star backdrop's
 * point-attenuation reset can call through the same loaded proc. */
void repl_executor_set_point_parameter_supported(int supported);
int  repl_executor_point_parameter_supported(void);

#endif /* REPL_EXECUTOR_H */
