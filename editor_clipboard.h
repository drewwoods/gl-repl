/*
 * repl_clipboard.h - Selection and clipboard operations.
 *
 * Manages line-range selection for copy/cut/paste operations. Selection is
 * anchored at a start line and extended/contracted to an end line; multiple
 * selections are not supported. Selection state persists across operations
 * until cleared.
 *
 * Clipboard buffer stores a contiguous range of commands (up to MAX_COMMANDS).
 * Copy/cut operations snapshot the selected commands into the buffer; paste
 * inserts them at the current edit line. Multiple pastes use the same buffer
 * without re-copying.
 *
 * Variable declaration guards: selecting and deleting a range that contains
 * float declarations (CMD_VAR_DECLARE) is normally rejected with a status
 * message because removing a declaration would orphan variable references.
 * The selection helpers query whether a range contains declarations and can
 * format appropriate status messages ("remove 2 declarations?").
 *
 * Selection is visual: the code panel highlights selected lines while editing.
 * The selection persists until the user starts a new operation or explicitly
 * calls repl_clipboard_clear_selection().
 */

#ifndef EDITOR_CLIPBOARD_H
#define EDITOR_CLIPBOARD_H

#include "sample.h"

/* --- Selection state --------------------------------------------------- */

void repl_clipboard_clear_selection(void);
int  repl_clipboard_sel_active(void);      /* 1 if anchor and end both set */
int  repl_clipboard_sel_lo(void);          /* min(anchor, end) */
int  repl_clipboard_sel_hi(void);          /* max(anchor, end) */

void repl_selection_start(int line_idx);   /* set anchor, clear end */
int  repl_selection_end(void);             /* current end index, -1 if not set */
void repl_selection_set_end(int line_idx); /* extend/contract selection */

/* --- Range validation -------------------------------------------------- */

/* Normalize a (start, count) range into valid bounds and return adjusted
 * (out_start, out_count). Returns 1 if non-empty, 0 if empty after clamping. */
int  repl_selection_normalize_cmd_range(int start, int count,
                                        int *out_start, int *out_count);

/* Query whether a command range contains any variable declarations. Used to
 * guard delete operations (removing a declaration orphans its references). */
int  repl_selection_cmds_contain_var_decl(const GLCmd *cmds, int count);
int  repl_selection_cmd_range_contains_var_decl(int start, int count);

/* Format a user-facing status message for a guarded operation (e.g.,
 * "remove 3 variable declarations?"). */
void repl_selection_set_var_decl_action_status(const char *action);

/* --- Copy / cut / paste ------------------------------------------------ */

void repl_clipboard_copy_current(void);    /* copy selection to buffer, or current line */
void repl_clipboard_cut_current(void);     /* cut to buffer and delete, guarded */
void repl_clipboard_paste_current(void);   /* paste buffer at edit line */

#endif /* EDITOR_CLIPBOARD_H */
