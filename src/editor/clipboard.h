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

/* --- OS clipboard bridge ----------------------------------------------- */

/*
 * Optional bridge to the window system's clipboard, installed by the app
 * (src/app/glr_clipboard.c) so this module keeps making no platform calls
 * of its own. With no bridge installed the internal buffer is the whole
 * story - that is what the tests, the editor demo, and the web build (whose
 * OS clipboard arrives through the DOM instead, see glr_web_io.c) run with.
 *
 * The internal clipboard stays the thing that is pasted: the bridge only
 * mirrors it outward on copy/cut, and replaces it on paste when something
 * else has since taken ownership outside. That ordering is what preserves
 * the payload kind and the block-aware line ranges across a copy/paste that
 * never left the app, which a plain-text round trip through the OS would
 * flatten.
 */
typedef struct {
    /* Mirror freshly copied/cut clipboard text out to the OS clipboard. */
    void (*publish)(const char *text);

    /* OS clipboard text if it differs from what we last published or
     * consumed, else NULL. NULL means "nothing new outside". */
    const char *(*poll_external)(void);
} EditorClipboardHostBridge;

/* NULL uninstalls. The bridge is stored by pointer, not copied, so it must
 * outlive the editor (a file-scope static in the installing module). */
void editor_clipboard_install_host_bridge(const EditorClipboardHostBridge *bridge);

#endif /* EDITOR_CLIPBOARD_H */
