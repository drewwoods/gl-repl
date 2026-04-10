#ifndef REPL_CORE_H
#define REPL_CORE_H

#include "sample.h"

/* Public core API */
int  repl_parse_command(const char *line, GLCmd *cmd);
int  repl_parse_command_with_vars(const char *line, GLCmd *cmd,
                                  ExprVar *vars, int num_vars);
void repl_save_default_output(void);
int  repl_load_from_file(const char *filename);
void repl_save_output(const char *filename);
void repl_flatten_commands(void);
void repl_recompute_autonormals(void);
int  repl_example_count(void);
const char *repl_example_name(int idx);
void repl_load_example(int idx);
void replay_start(void);
void replay_stop(void);
void repl_navigate_to_line(int target);
void repl_load_initial_commands(const char *import_file);
void repl_reformat_commands(void);
void repl_debug_dump_editor(FILE *out);
int  repl_flat_cmd_matches_cursor(int flat_idx);
int  repl_find_feeding_normal_cmd(int line_idx);
int  repl_find_feeding_color_cmd(int line_idx);

/* Runtime entry points used by sample.c callback wrappers */
void repl_display_func(void);
void repl_reshape_func(int w, int h);
void repl_keyboard_func(unsigned char key, int x, int y);
void repl_special_func(int key, int x, int y);
void repl_mouse_func(int button, int state, int x, int y);
void repl_motion_func(int x, int y);
void repl_passive_motion_func(int x, int y);
#ifndef USE_GLUT
void repl_mousewheel_func(int wheel, int direction, int x, int y);
#endif
void repl_timer_func(int value);
void repl_init_gl(void);

/* Test helpers */
void repl_reset_state(void);
void repl_feed_line_public(const char *line);

#endif /* REPL_CORE_H */
