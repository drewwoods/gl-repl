/*
 * src/repl/pipeline.h - REPL pipeline and lifecycle operations for the controller.
 *
 * These entry points are driven from glr_ctrl.c and other frame-level
 * orchestration code. They are not test internals.
 */
#ifndef REPL_PIPELINE_H
#define REPL_PIPELINE_H

void repl_flatten_commands(int edit_line_idx);
void repl_recompute_autonormals(int autonormal_enabled,
                                int *edit_line_inout);
void repl_refresh_camera_lines(void);
void repl_refresh_render_state_strings(void);
void repl_ensure_init_bootstrap_ready(void);
void repl_apply_init_bootstrap(void);

/* repl_copy_predef_values / repl_restore_predef_values now live in
 * src/repl/eval.h, next to the rest of the predef-variable helpers. */

#endif /* REPL_PIPELINE_H */
