/*
 * repl_pipeline.h - REPL pipeline and lifecycle operations for the controller.
 *
 * These entry points are driven from glr_ctrl.c and other frame-level
 * orchestration code. They are not test internals.
 */
#ifndef REPL_PIPELINE_H
#define REPL_PIPELINE_H

void repl_flatten_commands(void);
void repl_recompute_autonormals(int autonormal_enabled);
void repl_refresh_camera_lines(void);
void repl_refresh_render_state_strings(void);
void repl_ensure_init_bootstrap_ready(void);
void repl_apply_init_bootstrap(void);
void repl_copy_predef_values(float *dst, int max_vals);
void repl_restore_predef_values(const float *src, int max_vals);

#endif /* REPL_PIPELINE_H */
