#ifndef REPL_CLIPBOARD_H
#define REPL_CLIPBOARD_H

#include "sample.h"

void repl_clipboard_clear_selection(void);
int  repl_clipboard_sel_active(void);
int  repl_clipboard_sel_lo(void);
int  repl_clipboard_sel_hi(void);
void repl_selection_start(int line_idx);
int  repl_selection_end(void);
void repl_selection_set_end(int line_idx);

int  repl_selection_normalize_cmd_range(int start, int count,
                                        int *out_start, int *out_count);
int  repl_selection_cmds_contain_var_decl(const GLCmd *cmds, int count);
int  repl_selection_cmd_range_contains_var_decl(int start, int count);
void repl_selection_set_var_decl_action_status(const char *action);

void repl_clipboard_copy_current(void);
void repl_clipboard_cut_current(void);
void repl_clipboard_paste_current(void);

#endif /* REPL_CLIPBOARD_H */
