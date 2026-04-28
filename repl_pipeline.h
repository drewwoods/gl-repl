/*
 * repl_pipeline.h - REPL pipeline and lifecycle operations for the controller.
 *
 * These entry points are driven from imrepl_ctrl.c and other frame-level
 * orchestration code. They are not test internals.
 */
#ifndef REPL_PIPELINE_H
#define REPL_PIPELINE_H

void flatten_commands(void);
void recompute_autonormals(void);
void update_cam_lines(void);
void update_render_state_strings(void);
void ensure_init_bootstrap_ready(void);
void apply_init_bootstrap(void);
void repl_copy_predef_values(float *dst, int max_vals);
void repl_restore_predef_values(const float *src, int max_vals);
void repl_execute_set_fade_context(float alpha_scale, int skip_geom_before_pc);

#endif /* REPL_PIPELINE_H */
