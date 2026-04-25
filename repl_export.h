#ifndef REPL_EXPORT_H
#define REPL_EXPORT_H

#include "sample.h"

extern const char  *g_header_pre[];
extern const char  *g_header_post[];
extern const char  *g_footer_pre_init[];
extern const char  *g_footer_post_init[];

void repl_save_output(const char *filename);
int  repl_load_from_file(const char *filename);

void repl_state_refresh_workspace_header_lines(void);
int  repl_state_parse_workspace_header_line(const char *line);
void repl_state_update_render_state_strings(void);
void repl_state_update_cam_lines(void);

#endif
