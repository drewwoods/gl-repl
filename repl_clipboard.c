/*
 * repl_clipboard.c -- Line selection and command clipboard operations.
 *
 * This module owns the selection anchors and clipboard buffer.  Editor input
 * routing decides when copy/cut/paste happens; this file decides which command
 * range that means, preserves var-declaration guards, and performs the actual
 * clipboard mutation.
 */
#include "repl_clipboard.h"
#include "repl_command_store.h"
#include "repl_core_internal.h"

GLCmd g_clipboard[MAX_COMMANDS];
int   g_clipboard_count = 0;

int g_sel_anchor = -1;
int g_sel_end = -1;

void clear_selection(void) {
    g_sel_anchor = g_sel_end = -1;
}

int sel_active(void) {
    return g_sel_anchor >= 0 && g_sel_end >= 0;
}

int sel_lo(void) {
    return g_sel_anchor < g_sel_end ? g_sel_anchor : g_sel_end;
}

int sel_hi(void) {
    return g_sel_anchor > g_sel_end ? g_sel_anchor : g_sel_end;
}

void repl_selection_start(int line_idx) {
    g_sel_anchor = line_idx;
    g_sel_end = line_idx;
}

int repl_selection_end(void) {
    return g_sel_end;
}

void repl_selection_set_end(int line_idx) {
    g_sel_end = line_idx;
}

int repl_selection_normalize_cmd_range(int start, int count,
                                       int *out_start, int *out_count) {
    ReplCommandStore store = repl_command_store_live();
    return repl_command_store_normalize_range(&store, start, count,
                                             out_start, out_count);
}

int repl_selection_cmds_contain_var_decl(const GLCmd *cmds, int count) {
    for (int i = 0; i < count; i++) {
        if (cmds[i].type == CMD_VAR_DECLARE)
            return 1;
    }
    return 0;
}

int repl_selection_cmd_range_contains_var_decl(int start, int count) {
    if (start < 0 || count <= 0 || start >= g_num_cmds)
        return 0;
    if (start + count > g_num_cmds)
        count = g_num_cmds - start;
    return repl_selection_cmds_contain_var_decl(&g_cmds[start], count);
}

void repl_selection_set_var_decl_action_status(const char *action) {
    char msg[96];
    snprintf(msg, sizeof(msg), "Cannot %s float declarations", action);
    set_status(msg);
}

static void clipboard_clear(void) {
    g_clipboard_count = 0;
}

static void clipboard_copy_range(int start, int count) {
    clipboard_clear();
    for (int i = start; i < start + count && g_clipboard_count < MAX_COMMANDS; i++)
        g_clipboard[g_clipboard_count++] = g_cmds[i];
}

static int selected_cmd_range(int *out_start, int *out_count) {
    int start = sel_lo();
    int hi = sel_hi();

    if (hi >= g_num_cmds)
        hi = g_num_cmds - 1;

    return repl_selection_normalize_cmd_range(start, hi - start + 1,
                                             out_start, out_count);
}

static int current_copy_range(int *out_start, int *out_count,
                              int *out_is_for_block) {
    int start;
    int count = 1;
    int copying_for;

    if (g_edit_line >= g_num_cmds)
        return 0;

    start = g_edit_line;
    copying_for = (g_cmds[start].type == CMD_FOR_BEGIN);
    if (copying_for) {
        int block_end_idx = find_block_end(start);
        int end_idx = (block_end_idx < g_num_cmds) ? block_end_idx + 1 : g_num_cmds;
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

    if (sel_active())
        return selected_cmd_range(out_start, out_count);

    if (g_edit_line >= g_num_cmds)
        return 0;

    start = g_edit_line;
    if (g_cmds[start].type == CMD_FOR_BEGIN) {
        int block_end_idx = find_block_end(start);
        count = ((block_end_idx < g_num_cmds) ? block_end_idx + 1 : g_num_cmds) - start;
    } else {
        count = 1;
    }

    return repl_selection_normalize_cmd_range(start, count, out_start, out_count);
}

void repl_clipboard_copy_current(void) {
    if (g_inserting) {
        clear_selection();
        return;
    }

    if (sel_active()) {
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
                     g_clipboard_count, g_clipboard_count > 1 ? "s" : "");
            set_status(msg);
        }
    } else if (g_edit_line < g_num_cmds) {
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
                     g_clipboard_count);
            set_status(msg);
        } else {
            set_status("Copied line");
        }
    } else {
        clipboard_clear();
    }

    clear_selection();
}

void repl_clipboard_cut_current(void) {
    int start;
    int count;

    if (g_inserting) {
        clear_selection();
        return;
    }

    if (!current_cut_range(&start, &count)) {
        clipboard_clear();
        clear_selection();
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
    if (g_clipboard_count > 0) {
        if (repl_selection_cmds_contain_var_decl(g_clipboard, g_clipboard_count)) {
            repl_selection_set_var_decl_action_status("paste");
            return;
        }

        {
            ReplCommandStore store = repl_command_store_live();
            if (!repl_command_store_can_insert(&store, g_clipboard_count)) {
                set_status("Command buffer full!");
                return;
            }
        }

        push_undo_snapshot();
        {
            ReplCommandStore store = repl_command_store_live();
            int pos = g_inserting ? g_edit_line :
                      (g_edit_line < g_num_cmds ? g_edit_line : g_num_cmds);
            if (!repl_command_store_insert_many(&store, pos, g_clipboard,
                                                g_clipboard_count, 0)) {
                set_status("Command buffer full!");
                return;
            }
            g_edit_line = pos + g_clipboard_count;
            g_inserting = 0;
            load_line_to_input(g_edit_line);
            mark_normals_dirty();
        }

        {
            char msg[64];
            snprintf(msg, sizeof(msg), "Pasted %d line%s",
                     g_clipboard_count, g_clipboard_count > 1 ? "s" : "");
            set_status(msg);
        }
    } else {
        set_status("Clipboard empty");
    }
}
