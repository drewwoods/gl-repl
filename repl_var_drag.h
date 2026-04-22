/*
 * repl_var_drag.h -- Variable slider drag state and value writeback.
 *
 * Owns the drag transaction: which variable row is being dragged, log
 * vs. linear drag mode, the starting value/x, the motion-to-value
 * mapping, and the writeback into g_predef_vars[] plus any matching
 * CMD_VAR_ASSIGN command source.
 *
 * The editor's mouse handler calls begin()/motion()/reset(); the
 * variable panel renderer calls active_var() and log_mode() to decide
 * how to colour the active row.
 */
#ifndef REPL_VAR_DRAG_H
#define REPL_VAR_DRAG_H

int  repl_var_drag_active(void);        /* 1 while a drag is in progress */
int  repl_var_drag_active_var(void);    /* current drag row, or -1 */
int  repl_var_drag_log_mode(void);      /* 1 = log, 0 = linear */

void repl_var_drag_begin(int row, int log_mode, int x); /* uses current g_predef_vars[row].value as start */
void repl_var_drag_motion(int x);       /* writes g_predef_vars + syncs cmd source */
void repl_var_drag_reset(void);

#endif /* REPL_VAR_DRAG_H */
