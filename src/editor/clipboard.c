/*
 * editor_clipboard.c -- Line selection and command clipboard operations.
 *
 * Editor input routing decides when copy/cut/paste happens; this file decides
 * which command range that means, preserves var-declaration guards, and
 * performs the actual clipboard mutation. Selection and clipboard storage
 * live in EditorState (src/editor/state.c); this module owns the behavior.
 */

#include <stdlib.h>

#include "state.h"
#include "clipboard.h"
#include "completion.h"
#include "edit_ops.h"
#include "input.h"
#include "undo.h"

#include "repl/command_store.h"
#include "repl/core.h"
#include "repl/core_internal.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"
#include "subsystems/tutorial/tutorial.h"

static int tutorial_guard_clipboard_change_or_status(int pos,
                                                     int delete_count,
                                                     int insert_count) {
    if (tutorial_guard_source_change(pos, delete_count, insert_count))
        return 1;
    repl_set_status_error("Tutorial comment is read-only");
    return 0;
}

void editor_selection_clear_line_range(void) {
    editor_state_selection_clear();
}

int editor_clipboard_sel_active(void) {
    return editor_state_selection_anchor() >= 0 && editor_state_selection_end_idx() >= 0;
}

int editor_clipboard_sel_lo(void) {
    int a = editor_state_selection_anchor(), e = editor_state_selection_end_idx();
    return a < e ? a : e;
}

int editor_clipboard_sel_hi(void) {
    int a = editor_state_selection_anchor(), e = editor_state_selection_end_idx();
    return a > e ? a : e;
}

void editor_selection_start(int line_idx) {
    editor_state_selection_set(line_idx, line_idx);
}

int editor_selection_end(void) {
    return editor_state_selection_end_idx();
}

void editor_selection_set_end(int line_idx) {
    editor_state_selection_set(editor_state_selection_anchor(), line_idx);
}

int editor_selection_normalize_cmd_range(int start, int count,
                                       int *out_start, int *out_count) {
    ReplCommandStore store = repl_command_store_live();
    return repl_command_store_normalize_range(&store, start, count,
                                             out_start, out_count);
}

void editor_selection_set_var_decl_action_status(const char *action) {
    char msg[96];
    snprintf(msg, sizeof(msg), "Cannot %s float declarations", action);
    repl_set_status_error(msg);
}

static void clipboard_copy_range(int start, int count) {
    EditorClipboardState *clipboard = editor_state_clipboard_mut();
    EditorBufferView text = editor_buffer_view();
    int n = 0;
    for (int i = start; i < start + count && n < MAX_COMMANDS; i++) {
        repl_copy_string_fits(clipboard->lines[n], MAX_LINE_LEN,
                              editor_buffer_view_line(text, i));
        n++;
    }
    editor_state_clipboard_count_set(n);
}

static int selected_cmd_range(int *out_start, int *out_count) {
    int start = editor_clipboard_sel_lo();
    int hi = editor_clipboard_sel_hi();
    int n = repl_state_document_count();

    if (hi >= n)
        hi = n - 1;

    return editor_selection_normalize_cmd_range(start, hi - start + 1,
                                             out_start, out_count);
}

static int current_copy_range(int *out_start, int *out_count,
                              int *out_is_block) {
    int start;
    int count = 1;
    int is_block = 0;
    int block_start;
    int block_count;

    if (editor_state_edit_line() >= repl_state_document_count())
        return 0;

    start = editor_state_edit_line();
    if (repl_source_scope_block_extent(start, &block_start, &block_count)) {
        start = block_start;
        count = block_count;
        is_block = 1;
    }

    if (!editor_selection_normalize_cmd_range(start, count, out_start, out_count))
        return 0;
    if (out_is_block)
        *out_is_block = is_block;
    return 1;
}

static int current_cut_range(int *out_start, int *out_count) {
    int start;
    int count;
    int block_start;
    int block_count;

    if (editor_clipboard_sel_active())
        return selected_cmd_range(out_start, out_count);

    if (editor_state_edit_line() >= repl_state_document_count())
        return 0;

    start = editor_state_edit_line();
    if (repl_source_scope_block_extent(start, &block_start, &block_count)) {
        start = block_start;
        count = block_count;
    } else {
        count = 1;
    }

    return editor_selection_normalize_cmd_range(start, count, out_start, out_count);
}

/* Copy [lo, hi) from the active input buffer into the input-text
 * clipboard. Preserves the visual selection — copy is non-destructive
 * and shouldn't make the user re-select if they want to copy again. */
static int editor_clipboard_copy_input_selection(void) {
    if (!editor_input_selection_active())
        return 0;
    int lo = editor_input_selection_lo();
    int hi = editor_input_selection_hi();
    int len = hi - lo;
    if (len <= 0)
        return 0;
    editor_clipboard_set_input_text(editor_input_text() + lo, len);
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "Copied %d char%s",
                 len, len == 1 ? "" : "s");
        repl_set_status(msg);
    }
    return 1;
}

/* Cut: copy step then delete the range via the shared
 * edit_op_consume_input_selection helper. Works in any mode because the
 * mutation lives on the active input buffer, not on source commands —
 * the line-range insert-mode guard doesn't apply.
 *
 * Intentionally does NOT push an undo snapshot. EditorUndoSnapshot does
 * not capture the input buffer (restore reloads input via
 * editor_load_line_to_input from the committed source line), so a push here
 * cannot rewind the cut — it would only have the side effect of
 * tripping the example auto-promotion hook in
 * editor_undo_push_snapshot. This matches typed-char and backspace,
 * which never push undo either; the undo history is reserved for
 * source-command mutations. */
static int editor_clipboard_cut_input_selection(void) {
    if (!editor_input_selection_active())
        return 0;
    int lo = editor_input_selection_lo();
    int hi = editor_input_selection_hi();
    int len = hi - lo;
    if (len <= 0)
        return 0;
    editor_clipboard_set_input_text(editor_input_text() + lo, len);
    (void)edit_op_consume_input_selection();
    editor_completion_update();
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "Cut %d char%s",
                 len, len == 1 ? "" : "s");
        repl_set_status(msg);
    }
    return 1;
}

/* Paste of an INPUT_TEXT clipboard. If a destination input selection
 * is active, consume it first (same selection-consume primitive the
 * cut path uses) so paste replaces the selected range instead of
 * inserting beside it.
 *
 * No undo push here — same reasoning as cut above: the undo snapshot
 * doesn't capture input bytes, so it cannot rewind the paste, and
 * pushing would falsely auto-promote a loaded example before any
 * source command is touched. */
static int editor_clipboard_paste_input_text(void) {
    if (!editor_clipboard_has_input_text())
        return 0;
    (void)edit_op_consume_input_selection();

    int cur = editor_cursor_pos();
    int paste_len = editor_clipboard_input_text_len();
    const char *paste = editor_clipboard_input_text();
    EditorInputState *inp = editor_state_input_mut();
    int existing = inp->input_len;
    int room = (MAX_INPUT_LEN - 1) - existing;
    if (room < 0)
        room = 0;
    if (paste_len > room)
        paste_len = room;
    if (paste_len > 0) {
        /* Shift the trailing bytes (including the NUL) right by
         * paste_len, then copy the substring in. */
        memmove(&inp->input[cur + paste_len], &inp->input[cur],
                (size_t)(existing - cur + 1));
        memcpy(&inp->input[cur], paste, (size_t)paste_len);
        inp->input_len = existing + paste_len;
        editor_cursor_pos_set(cur + paste_len);
    }
    editor_completion_update();
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "Pasted %d char%s",
                 paste_len, paste_len == 1 ? "" : "s");
        repl_set_status(msg);
    }
    return 1;
}

void editor_clipboard_copy_current(void) {
    /* Input-buffer selection wins over line-range / current line.
     * Works in insert mode too — the cut/copy of a partial input
     * substring is a pure input-buffer mutation. */
    if (editor_clipboard_copy_input_selection())
        return;

    if (editor_insert_mode()) {
        editor_selection_clear_line_range();
        return;
    }

    if (editor_clipboard_sel_active()) {
        int start;
        int count;

        if (!selected_cmd_range(&start, &count))
            return;
        if (repl_range_contains_var_decl(start, count)) {
            editor_selection_set_var_decl_action_status("copy");
            return;
        }

        clipboard_copy_range(start, count);
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "Copied %d line%s",
                     editor_state_clipboard_count(),
                     editor_state_clipboard_count() > 1 ? "s" : "");
            repl_set_status(msg);
        }
    } else if (editor_state_edit_line() < repl_state_document_count()) {
        int start;
        int count;
        int copying_block = 0;

        if (!current_copy_range(&start, &count, &copying_block))
            return;
        if (repl_range_contains_var_decl(start, count)) {
            editor_selection_set_var_decl_action_status("copy");
            return;
        }

        clipboard_copy_range(start, count);
        if (copying_block) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Copied block (%d lines)",
                     editor_state_clipboard_count());
            repl_set_status(msg);
        } else {
            repl_set_status("Copied line");
        }
    } else {
        editor_state_clipboard_count_set(0);
    }

    editor_selection_clear_line_range();
}

void editor_clipboard_cut_current(void) {
    int start;
    int count;

    /* Same priority as copy: input-buffer selection wins. This path
     * deliberately runs *before* the insert-mode guard so partial-line
     * cut works in insert mode, where line-range cut is intentionally
     * disabled. */
    if (editor_clipboard_cut_input_selection())
        return;

    if (editor_insert_mode()) {
        editor_selection_clear_line_range();
        return;
    }

    if (!current_cut_range(&start, &count)) {
        editor_state_clipboard_count_set(0);
        editor_selection_clear_line_range();
        return;
    }
    if (!tutorial_guard_clipboard_change_or_status(start, count, 0))
        return;

    if (repl_range_contains_var_decl(start, count)) {
        editor_selection_set_var_decl_action_status("remove");
        return;
    }

    clipboard_copy_range(start, count);
    editor_delete_cmd_range(start, count, "Cut");
}

void editor_clipboard_paste_current(void) {
    /* INPUT_TEXT paste targets the active input buffer (replacing any
     * destination selection); LINES paste runs the existing
     * editor_feed_line() chain; EMPTY emits the same "Clipboard empty"
     * status the count-based check below has always produced. */
    if (editor_clipboard_paste_input_text())
        return;

    int count = editor_state_clipboard_count();
    if (count <= 0) {
        repl_set_status("Clipboard empty");
        return;
    }

    {
        ReplCommandStore store = repl_command_store_live();
        if (!repl_command_store_can_insert(&store, count)) {
            repl_set_status_error("Command buffer full!");
            return;
        }
    }

    int n = repl_state_document_count();
    int edit = editor_state_edit_line();
    int pos = editor_insert_mode() ? edit : (edit < n ? edit : n);

    /* The tutorial read-only guard MUST run before the undo push: a
     * rejected paste must not push a phantom undo entry, clear the redo
     * ring, or auto-promote a loaded example to a user scene — all are
     * side effects of editor_undo_push_snapshot(). This matches the
     * guard-then-push ordering of every other mutation path (the cut
     * path above, and editor_delete_cmd_range / editor_clear_all_cmds /
     * the commit routes in input.c). */
    if (!tutorial_guard_clipboard_change_or_status(pos, 0, count))
        return;

    /* Snapshot the clipboard text before any state mutation — defensive
     * against any path that could mutate the clipboard mid-loop, and
     * sized by count so the snapshot doesn't allocate the 1 MB
     * MAX_COMMANDS × MAX_LINE_LEN worst case on the stack. Allocated
     * before the undo push so a malloc failure leaves no side effects. */
    char (*buf)[MAX_LINE_LEN] = malloc((size_t)count * MAX_LINE_LEN);
    if (!buf) {
        repl_set_status_error("Paste failed: out of memory");
        return;
    }
    {
        const EditorClipboardState *cb = editor_state_clipboard_mut();
        for (int i = 0; i < count; i++)
            repl_copy_string_fits(buf[i], MAX_LINE_LEN, cb->lines[i]);
    }

    /* Single undo for the whole paste; editor_feed_line() runs the commit
     * chain per line but does not push undo itself, so structured
     * blocks (for / func / if / `}`) re-parse correctly as the
     * partial document grows. */
    editor_undo_push_snapshot();

    editor_state_edit_line_set(pos);
    editor_insert_mode_set(1);

    for (int i = 0; i < count; i++)
        editor_feed_line(buf[i]);
    free(buf);

    /* Turn off insert mode before the load_line call, even if we are going to
     * restore the entry insert mode: editor_load_line_to_input mirrors a
     * *committed* line into the input buffer for re-editing, which is
     * contradictory in insert mode — the panel renders an input row
     * *before* document_cmds[edit_line], so a pre-loaded following line
     * shows as a phantom duplicate and materializes a real duplicate on
     * the next commit. (This is the Enter-virtual-blank paste bug: an
     * unmodified-line Enter leaves insert_mode set with no doc mutation,
     * so saved_insert was 1 and the following line got staged.) */
    editor_insert_mode_set(0);
    editor_load_line_to_input(editor_state_edit_line());
    repl_mark_normals_dirty();

    {
        char msg[64];
        snprintf(msg, sizeof(msg), "Pasted %d line%s",
                 count, count > 1 ? "s" : "");
        repl_set_status(msg);
    }
}
