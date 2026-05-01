/*
 * repl_clipboard.c -- Line selection and command clipboard operations.
 *
 * Editor input routing decides when copy/cut/paste happens; this file decides
 * which command range that means, preserves var-declaration guards, and
 * performs the actual clipboard mutation. Phase 2 keeps selection and
 * clipboard storage in repl_state.c while this module owns the behavior.
 */
#include "repl_clipboard.h"
#include "repl_command_store.h"
#include "repl_core_internal.h"
#include "repl_source_scope.h"
#include "repl_undo.h"
#include "repl_state.h"

void repl_clipboard_clear_selection(void) {
    repl_state_selection_clear();
}

int repl_clipboard_sel_active(void) {
    return repl_state_selection_anchor() >= 0 && repl_state_selection_end_idx() >= 0;
}

int repl_clipboard_sel_lo(void) {
    int a = repl_state_selection_anchor(), e = repl_state_selection_end_idx();
    return a < e ? a : e;
}

int repl_clipboard_sel_hi(void) {
    int a = repl_state_selection_anchor(), e = repl_state_selection_end_idx();
    return a > e ? a : e;
}

void repl_selection_start(int line_idx) {
    repl_state_selection_set(line_idx, line_idx);
}

int repl_selection_end(void) {
    return repl_state_selection_end_idx();
}

void repl_selection_set_end(int line_idx) {
    repl_state_selection_set(repl_state_selection_anchor(), line_idx);
}

int repl_selection_normalize_cmd_range(int start, int count,
                                       int *out_start, int *out_count) {
    ReplCommandStore store = repl_command_store_live();
    return repl_command_store_normalize_range(&store, start, count,
                                             out_start, out_count);
}

int repl_selection_cmds_contain_var_decl(const GLCmd *cmds, int count) {
    for (int idx = 0; idx < count; idx++) {
        if (cmds[idx].type == CMD_VAR_DECLARE)
            return 1;
    }
    return 0;
}

int repl_selection_cmd_range_contains_var_decl(int start, int count) {
    int n = repl_state_document_count();
    if (start < 0 || count <= 0 || start >= n)
        return 0;
    if (start + count > n)
        count = n - start;
    return repl_selection_cmds_contain_var_decl(repl_state_document_cmds() + start, count);
}

void repl_selection_set_var_decl_action_status(const char *action) {
    char msg[96];
    snprintf(msg, sizeof(msg), "Cannot %s float declarations", action);
    set_status(msg);
}

static void clipboard_copy_range(int start, int count) {
    ReplClipboardState *clipboard = repl_state_clipboard_mut();
    GLCmd *cb = repl_state_clipboard_cmds_mut();
    const GLCmd *cmds = repl_state_document_cmds();
    int n = 0;
    for (int i = start; i < start + count && n < MAX_COMMANDS; i++) {
        cb[n++] = cmds[i];
        repl_copy_string_fits(clipboard->lines[n - 1], MAX_LINE_LEN,
                              repl_state_editor_buffer_line(i));
    }
    repl_state_clipboard_count_set(n);
}

static int selected_cmd_range(int *out_start, int *out_count) {
    int start = repl_clipboard_sel_lo();
    int hi = repl_clipboard_sel_hi();
    int n = repl_state_document_count();

    if (hi >= n)
        hi = n - 1;

    return repl_selection_normalize_cmd_range(start, hi - start + 1,
                                             out_start, out_count);
}

static int current_copy_range(int *out_start, int *out_count,
                              int *out_is_for_block) {
    int start;
    int count = 1;
    int copying_for;
    int n = repl_state_document_count();

    if (repl_state_edit_line() >= n)
        return 0;

    start = repl_state_edit_line();
    copying_for = (repl_state_document_cmds()[start].type == CMD_FOR_BEGIN);
    if (copying_for) {
        int block_end_idx = repl_source_scope_find_block_end(start);
        int end_idx = (block_end_idx < n) ? block_end_idx + 1 : n;
        count = end_idx - start;
    }

    if (!repl_selection_normalize_cmd_range(start, count, out_start, out_count))
        return 0;
    if (out_is_for_block)
        *out_is_for_block = copying_for;
    return 1;
}

static int current_cut_range(int *out_start, int *out_count) {
    int start;
    int count;
    int n = repl_state_document_count();

    if (repl_clipboard_sel_active())
        return selected_cmd_range(out_start, out_count);

    if (repl_state_edit_line() >= n)
        return 0;

    start = repl_state_edit_line();
    if (repl_state_document_cmds()[start].type == CMD_FOR_BEGIN) {
        int block_end_idx = repl_source_scope_find_block_end(start);
        count = ((block_end_idx < n) ? block_end_idx + 1 : n) - start;
    } else {
        count = 1;
    }

    return repl_selection_normalize_cmd_range(start, count, out_start, out_count);
}

void repl_clipboard_copy_current(void) {
    if (repl_state_insert_mode()) {
        repl_clipboard_clear_selection();
        return;
    }

    if (repl_clipboard_sel_active()) {
        int start;
        int count;

        if (!selected_cmd_range(&start, &count))
            return;
        if (repl_selection_cmd_range_contains_var_decl(start, count)) {
            repl_selection_set_var_decl_action_status("copy");
            return;
        }

        clipboard_copy_range(start, count);
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "Copied %d line%s",
                     repl_state_clipboard_count(),
                     repl_state_clipboard_count() > 1 ? "s" : "");
            set_status(msg);
        }
    } else if (repl_state_edit_line() < repl_state_document_count()) {
        int start;
        int count;
        int copying_for = 0;

        if (!current_copy_range(&start, &count, &copying_for))
            return;
        if (repl_selection_cmd_range_contains_var_decl(start, count)) {
            repl_selection_set_var_decl_action_status("copy");
            return;
        }

        clipboard_copy_range(start, count);
        if (copying_for) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Copied for-loop (%d lines)",
                     repl_state_clipboard_count());
            set_status(msg);
        } else {
            set_status("Copied line");
        }
    } else {
        repl_state_clipboard_count_set(0);
    }

    repl_clipboard_clear_selection();
}

void repl_clipboard_cut_current(void) {
    int start;
    int count;

    if (repl_state_insert_mode()) {
        repl_clipboard_clear_selection();
        return;
    }

    if (!current_cut_range(&start, &count)) {
        repl_state_clipboard_count_set(0);
        repl_clipboard_clear_selection();
        return;
    }

    if (repl_selection_cmd_range_contains_var_decl(start, count)) {
        repl_selection_set_var_decl_action_status("remove");
        return;
    }

    clipboard_copy_range(start, count);
    delete_cmd_range(start, count, "Cut");
}

void repl_clipboard_paste_current(void) {
    if (repl_state_clipboard_count() > 0) {
        if (repl_selection_cmds_contain_var_decl(repl_state_clipboard_cmds_mut(),
                                                 repl_state_clipboard_count())) {
            repl_selection_set_var_decl_action_status("paste");
            return;
        }

        {
            ReplCommandStore store = repl_command_store_live();
            if (!repl_command_store_can_insert(&store, repl_state_clipboard_count())) {
                set_status("Command buffer full!");
                return;
            }
        }

        repl_undo_push_snapshot();
        {
            ReplCommandStore store = repl_command_store_live();
            int edit = repl_state_edit_line();
            int n = repl_state_document_count();
            int pos = repl_state_insert_mode() ? edit :
                      (edit < n ? edit : n);
            const char *lines[MAX_COMMANDS];

            for (int i = 0; i < repl_state_clipboard_count(); i++)
                lines[i] = repl_state_clipboard_mut()->lines[i];
            if (!repl_command_store_insert_many(&store, pos, repl_state_clipboard_cmds_mut(),
                                                repl_state_clipboard_count(), 0,
                                                lines)) {
                set_status("Command buffer full!");
                return;
            }
            repl_state_edit_line_set(pos + repl_state_clipboard_count());
            repl_state_insert_mode_set(0);
            load_line_to_input(repl_state_edit_line());
            mark_normals_dirty();
        }

        {
            char msg[64];
            snprintf(msg, sizeof(msg), "Pasted %d line%s",
                     repl_state_clipboard_count(),
                     repl_state_clipboard_count() > 1 ? "s" : "");
            set_status(msg);
        }
    } else {
        set_status("Clipboard empty");
    }
}
