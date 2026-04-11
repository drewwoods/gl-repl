#ifndef REPL_CORE_INTERNAL_H
#define REPL_CORE_INTERNAL_H

#include "repl_core.h"

/*
 * Test-visible internals for the normalization/commit pipeline.
 * These stay outside the public REPL API, but are shared with regression tests
 * so they can exercise the same source-text rules as the runtime.
 */
int  repl_parse_and_normalize(const char *line, int pos,
                              ExprVar *vars, int num_vars,
                              int preserve_expr, GLCmd *out_cmd);
void update_render_state_strings(void);
void ensure_init_bootstrap_ready(void);
void apply_init_bootstrap(void);
void save_output(const char *filename);
int  load_from_file(const char *filename);
void trim_in_place(char *s);
int  extract_for_args_text(const char *src,
                           char *var, int var_sz,
                           char *args, int args_sz);
int  parse_expr_list_exact(const char *src, float *out_vals, int max_vals,
                           ExprVar *vars, int num_vars, int *out_count);
int  parse_repl_func_signature(const char *src, int *fn,
                               char param_names[][16], int max_params,
                               int *param_count);
int  extract_func_call_args_text(const char *src, int *fn,
                                 char *args, int args_sz);
void format_func_header(char *out, int out_sz, const char *indent,
                        int fn, char param_names[][16], int param_count);
int  input_has_expr_vars(const char *s, ExprVar *vars, int num_vars);
int  input_has_any_visible_vars(const char *s, ExprVar *vars, int num_vars);
void repl_normalize_from_parsed(const char *parsed_source,
                                const char *raw_expr,
                                int ensure_semicolon,
                                char *out, int out_sz);
int  repl_extract_paren_payload(const char *src, char *out, int out_sz);
int  repl_extract_label_name(const char *src, char *name, int name_sz);
int  repl_extract_goto_label(const char *src, char *name, int name_sz);
int  repl_extract_assignment_parts(const char *src,
                                   char *name, int name_sz,
                                   char *rhs, int rhs_sz);
void repl_dump_code_panel_text(FILE *out);
void repl_dump_code_panel_visual_text(FILE *out);
void depth_cache_invalidate(void);
int  apply_state_cmd(const GLCmd *cmd, float alpha_scale);
void load_line_to_input(int idx);
void update_selected_autocomplete_preview(void);
void update_autocomplete(void);
void accept_autocomplete(void);
int  find_block_end(int begin_idx);
int  block_depth_at(int pos);
CmdType nearest_open_block_at(int pos);
int  collect_visible_vars(int pos, ExprVar *vars, int max_vars);
void replay_tick_fade_batches(float dt);
void replay_seek(int new_pc);
void replay_step_back(void);
void replay_restart_from_beginning(void);
int  feed_line(const char *line);
void search_clear_all(void);
int  handle_search_key(unsigned char key);
int  handle_search_special(int key);

#endif /* REPL_CORE_INTERNAL_H */
