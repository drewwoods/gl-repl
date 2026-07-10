/*
 * editor_clipboard.h - Selection and clipboard operations.
 *
 * Manages line-range selection for copy/cut/paste operations. Selection is
 * anchored at a start line and extended/contracted to an end line; multiple
 * selections are not supported. Selection state persists across operations
 * until cleared.
 *
 * Clipboard buffer stores a contiguous range of commands (up to MAX_EDITOR_COMMANDS).
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
 * calls editor_selection_clear_line_range().
 */

#ifndef EDITOR_CLIPBOARD_H
#define EDITOR_CLIPBOARD_H

/* --- Selection state --------------------------------------------------- */

void editor_selection_clear_line_range(void);
int  editor_clipboard_sel_active(void);      /* 1 if anchor and end both set */
int  editor_clipboard_sel_lo(void);          /* min(anchor, end) */
int  editor_clipboard_sel_hi(void);          /* max(anchor, end) */

void editor_selection_start(int line_idx);   /* set anchor, clear end */
int  editor_selection_end(void);             /* current end index, -1 if not set */
void editor_selection_set_end(int line_idx); /* extend/contract selection */

/* --- Range validation -------------------------------------------------- */

/* Normalize a (start, count) range into valid bounds and return adjusted
 * (out_start, out_count). Returns 1 if non-empty, 0 if empty after clamping. */
int  editor_selection_normalize_cmd_range(int start, int count,
                                        int *out_start, int *out_count);

/* Format a user-facing status message for a guarded operation (e.g.,
 * "remove 3 variable declarations?"). */
void editor_selection_set_var_decl_action_status(const char *action);

/* --- Copy / cut / paste ------------------------------------------------ */

void editor_clipboard_copy_current(void);    /* copy selection to buffer, or current line */
void editor_clipboard_cut_current(void);     /* cut to buffer and delete, guarded */
void editor_clipboard_paste_current(void);   /* paste buffer at edit line */

/* Same operations, reporting whether a fresh clipboard payload was actually
 * produced (1) vs. a no-op/blocked case (0: insert mode with no input
 * selection, nothing to copy/cut, var-decl guard, tutorial guard, ...). The
 * void editor_clipboard_copy_current()/cut_current() above are thin wrappers
 * around these for native callers that don't need the result. Bridges that
 * must not export a stale clipboard after a blocked operation (e.g. the web
 * build's OS-clipboard bridge) should call these instead. */
int editor_clipboard_copy_current_with_result(void);
int editor_clipboard_cut_current_with_result(void);

#endif /* EDITOR_CLIPBOARD_H */
