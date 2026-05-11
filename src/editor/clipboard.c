/*
 * editor_clipboard.c -- Line selection and command clipboard operations.
 *
 * Editor input routing decides when copy/cut/paste happens; this file decides
 * which command range that means, preserves var-declaration guards, and
 * performs the actual clipboard mutation. Phase 2 keeps selection and
 * clipboard storage in repl_state.c while this module owns the behavior.
 */

#include "state.h"
#include "clipboard.h"
#include "input.h"
#include "undo.h"

#include "repl_command_store.h"
#include "repl_core.h"
#include "repl_core_internal.h"
#include "repl_source_scope.h"
#include "repl_state_owners.h"

void editor_clipboard_clear_selection(void) {
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
    repl_set_status(msg);
}

static void clipboard_copy_range(int start, int count) {
    ReplClipboardState *clipboard = editor_state_clipboard_mut();
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

    if (repl_state_edit_line() >= repl_state_document_count())
        return 0;

    start = repl_state_edit_line();
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

    if (repl_state_edit_line() >= repl_state_document_count())
        return 0;

    start = repl_state_edit_line();
    if (repl_source_scope_block_extent(start, &block_start, &block_count)) {
        start = block_start;
        count = block_count;
    } else {
        count = 1;
    }

    return editor_selection_normalize_cmd_range(start, count, out_start, out_count);
}

void editor_clipboard_copy_current(void) {
    if (editor_insert_mode()) {
        editor_clipboard_clear_selection();
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
    } else if (repl_state_edit_line() < repl_state_document_count()) {
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

    editor_clipboard_clear_selection();
}

void editor_clipboard_cut_current(void) {
    int start;
    int count;

    if (editor_insert_mode()) {
        editor_clipboard_clear_selection();
        return;
    }

    if (!current_cut_range(&start, &count)) {
        editor_state_clipboard_count_set(0);
        editor_clipboard_clear_selection();
        return;
    }

    if (repl_range_contains_var_decl(start, count)) {
        editor_selection_set_var_decl_action_status("remove");
        return;
    }

    clipboard_copy_range(start, count);
    delete_cmd_range(start, count, "Cut");
}

void editor_clipboard_paste_current(void) {
    int count = editor_state_clipboard_count();
    if (count <= 0) {
        repl_set_status("Clipboard empty");
        return;
    }

    {
        ReplCommandStore store = repl_command_store_live();
        if (!repl_command_store_can_insert(&store, count)) {
            repl_set_status("Command buffer full!");
            return;
        }
    }

    /* Single undo for the whole paste; feed_line() runs the commit
     * chain per line but does not push undo itself, so structured
     * blocks (for / func / if / `}`) re-parse correctly as the
     * partial document grows. */
    editor_undo_push_snapshot();

    int n = repl_state_document_count();
    int edit = repl_state_edit_line();
    int pos = editor_insert_mode() ? edit : (edit < n ? edit : n);
    int saved_insert = editor_insert_mode();

    repl_state_edit_line_set(pos);
    editor_insert_mode_set(1);

    /* Snapshot the clipboard text before feeding — defensive against
     * any path that could mutate the clipboard mid-loop. */
    char buf[MAX_COMMANDS][MAX_LINE_LEN];
    {
        const ReplClipboardState *cb = editor_state_clipboard_mut();
        for (int i = 0; i < count; i++)
            repl_copy_string_fits(buf[i], MAX_LINE_LEN, cb->lines[i]);
    }

    for (int i = 0; i < count; i++)
        feed_line(buf[i]);

    editor_insert_mode_set(saved_insert);
    load_line_to_input(repl_state_edit_line());
    repl_mark_normals_dirty();

    {
        char msg[64];
        snprintf(msg, sizeof(msg), "Pasted %d line%s",
                 count, count > 1 ? "s" : "");
        repl_set_status(msg);
    }
}
