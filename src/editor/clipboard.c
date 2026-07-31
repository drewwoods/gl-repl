/*
 * editor_clipboard.c -- Line selection and command clipboard operations.
 *
 * Editor input routing decides when copy/cut/paste happens; this file decides
 * which command range that means, preserves var-declaration guards, and
 * performs the actual clipboard mutation. Selection and clipboard storage
 * live in EditorState (src/editor/state.c); this module owns the behavior.
 */

#include <stdlib.h>
#include <string.h>

#include "state.h"
#include "clipboard.h"
#include "completion.h"
#include "edit_ops.h"
#include "input.h"
#include "undo.h"

#include "repl/command_store.h"
#include <stdio.h>
#include "repl/host_effects.h"
#include "repl/state_notify.h"
#include "repl/load.h"
#include "repl/source_scope.h"
#include "repl/state_views.h"
#include "repl/util.h"           /* repl_format_fits / repl_copy_string_fits */
#include "subsystems/tutorial/tutorial.h"

static EditorUndoSnapshot g_clipboard_paste_rollback_snapshot;

/* Installed by the app on the native builds; NULL everywhere else. See the
 * EditorClipboardHostBridge contract in clipboard.h. */
static const EditorClipboardHostBridge *g_host_bridge = NULL;

void editor_clipboard_install_host_bridge(const EditorClipboardHostBridge *bridge) {
    g_host_bridge = bridge;
}

/* Serializes the internal clipboard to plain text and hands it to the host.
 * Called only where a copy/cut actually produced a payload: a blocked one
 * (var-decl guard, tutorial guard, nothing selected) must leave whatever the
 * user has in the OS clipboard alone. */
static void clipboard_publish_to_host(void) {
    const EditorClipboardState *cb;

    if (!g_host_bridge || !g_host_bridge->publish)
        return;

    cb = editor_state_clipboard();

    if (cb->kind == EDITOR_CLIPBOARD_INPUT_TEXT) {
        char text[MAX_INPUT_LEN];
        int len = cb->input_text_len;
        if (len < 0 || len >= (int)sizeof(text))
            len = (int)sizeof(text) - 1;
        memcpy(text, cb->input_text, (size_t)len);
        text[len] = '\0';
        g_host_bridge->publish(text);
        return;
    }

    if (cb->kind == EDITOR_CLIPBOARD_LINES && cb->line_count > 0) {
        /* Newline-joined, no trailing newline — sized from the payload
         * rather than the MAX_EDITOR_COMMANDS x MAX_LINE_LEN worst case,
         * same as the paste path below. */
        size_t total = 1;
        char *text;
        char *p;

        for (int i = 0; i < cb->line_count; i++)
            total += strlen(cb->lines[i]) + 1;

        text = malloc(total);
        if (!text)
            return;

        p = text;
        for (int i = 0; i < cb->line_count; i++) {
            size_t len = strlen(cb->lines[i]);
            memcpy(p, cb->lines[i], len);
            p += len;
            if (i + 1 < cb->line_count)
                *p++ = '\n';
        }
        *p = '\0';

        g_host_bridge->publish(text);
        free(text);
    }
}

/* Splits external text on '\n' (tolerating a '\r' before it) into the
 * line clipboard, dropping empty segments — in particular the trailing one
 * a terminal newline produces, which would otherwise fail the whole paste
 * as an empty command. Clamped to the clipboard's capacity. */
static void clipboard_stage_external_lines(const char *text) {
    EditorClipboardState *cb = editor_state_clipboard_mut();
    const char *p = text;
    int n = 0;

    while (*p && n < MAX_EDITOR_COMMANDS) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);

        if (len > 0 && p[len - 1] == '\r')
            len--;
        if (len > 0) {
            if (len >= MAX_LINE_LEN)
                len = MAX_LINE_LEN - 1;
            memcpy(cb->lines[n], p, len);
            cb->lines[n][len] = '\0';
            n++;
        }
        if (!eol)
            break;
        p = eol + 1;
    }

    editor_state_clipboard_count_set(n);
}

/* Adopts the OS clipboard when something outside the app has taken it over
 * since our last copy. Plain text carries no payload kind, so it is
 * classified the same way the web bridge classifies a foreign paste:
 * anything multi-line is source lines, a single line lands in the input
 * buffer when the cursor is somewhere it can (an active input selection or
 * insert mode), and otherwise it is a source line too. */
static void clipboard_adopt_external_if_changed(void) {
    const char *text;

    if (!g_host_bridge || !g_host_bridge->poll_external)
        return;

    text = g_host_bridge->poll_external();
    if (!text || !text[0])
        return;

    if (strchr(text, '\n'))
        clipboard_stage_external_lines(text);
    else if (editor_input_selection_active() || editor_insert_mode())
        editor_clipboard_set_input_text(text, (int)strlen(text));
    else
        clipboard_stage_external_lines(text);
}

static int tutorial_guard_clipboard_change_or_status(int pos,
                                                     int delete_count,
                                                     int insert_count) {
    if (tutorial_guard_source_change(pos, delete_count, insert_count))
        return 1;
    repl_set_status_error("Tutorial line is read-only");
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
    for (int i = start; i < start + count && n < MAX_EDITOR_COMMANDS; i++) {
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

static void clipboard_restore_input_state(const char *input,
                                          int cursor_pos,
                                          int anchor_pos,
                                          int insert_mode,
                                          int edit_line) {
    editor_state_edit_line_set(edit_line);
    editor_insert_mode_set(insert_mode);
    editor_input_set_text(input ? input : "");
    editor_cursor_pos_set(cursor_pos);
    editor_input_anchor_set(anchor_pos);
    editor_completion_update();
}

static void clipboard_set_paste_failed_status(int line_no, int count,
                                              const char *err) {
    char msg[REPL_STATUS_TEXT_MAX];
    if (err && err[0]) {
        snprintf(msg, sizeof(msg), "Paste failed at line %d of %d: %.96s",
                 line_no, count, err);
    } else {
        snprintf(msg, sizeof(msg), "Paste failed at line %d of %d",
                 line_no, count);
    }
    repl_set_status_error(msg);
}

int editor_clipboard_copy_current_with_result(void) {
    int result = 0;

    /* Input-buffer selection wins over line-range / current line.
     * Works in insert mode too — the cut/copy of a partial input
     * substring is a pure input-buffer mutation. */
    if (editor_clipboard_copy_input_selection())
        return 1;

    if (editor_insert_mode()) {
        editor_selection_clear_line_range();
        return 0;
    }

    if (editor_clipboard_sel_active()) {
        int start;
        int count;

        if (!selected_cmd_range(&start, &count))
            return 0;
        if (repl_range_contains_var_decl(start, count)) {
            editor_selection_set_var_decl_action_status("copy");
            return 0;
        }

        clipboard_copy_range(start, count);
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "Copied %d line%s",
                     editor_state_clipboard_count(),
                     editor_state_clipboard_count() > 1 ? "s" : "");
            repl_set_status(msg);
        }
        result = 1;
    } else if (editor_state_edit_line() < repl_state_document_count()) {
        int start;
        int count;
        int copying_block = 0;

        if (!current_copy_range(&start, &count, &copying_block))
            return 0;
        if (repl_range_contains_var_decl(start, count)) {
            editor_selection_set_var_decl_action_status("copy");
            return 0;
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
        result = 1;
    } else {
        editor_state_clipboard_clear();
    }

    editor_selection_clear_line_range();
    return result;
}

/* The host bridge is driven from these two wrappers rather than from the
 * _with_result twins they call: the web build calls the twins and exports the
 * payload to JavaScript itself (glr_web_io.c), and has no native bridge
 * installed to publish through anyway. */
void editor_clipboard_copy_current(void) {
    if (editor_clipboard_copy_current_with_result())
        clipboard_publish_to_host();
}

int editor_clipboard_cut_current_with_result(void) {
    int start;
    int count;

    /* Same priority as copy: input-buffer selection wins. This path
     * deliberately runs *before* the insert-mode guard so partial-line
     * cut works in insert mode, where line-range cut is intentionally
     * disabled. */
    if (editor_clipboard_cut_input_selection())
        return 1;

    if (editor_insert_mode()) {
        editor_selection_clear_line_range();
        return 0;
    }

    if (!current_cut_range(&start, &count)) {
        editor_state_clipboard_clear();
        editor_selection_clear_line_range();
        return 0;
    }
    if (!tutorial_guard_clipboard_change_or_status(start, count, 0))
        return 0;

    if (repl_range_contains_var_decl(start, count)) {
        editor_selection_set_var_decl_action_status("remove");
        return 0;
    }

    clipboard_copy_range(start, count);
    editor_delete_cmd_range(start, count, "Cut");
    return 1;
}

void editor_clipboard_cut_current(void) {
    if (editor_clipboard_cut_current_with_result())
        clipboard_publish_to_host();
}

void editor_clipboard_paste_current(void) {
    clipboard_adopt_external_if_changed();

    switch (editor_state_clipboard_kind()) {
    case EDITOR_CLIPBOARD_EMPTY:
        repl_set_status("Clipboard empty");
        return;
    case EDITOR_CLIPBOARD_INPUT_TEXT:
        editor_clipboard_paste_input_text();
        return;
    case EDITOR_CLIPBOARD_LINES:
        break;
    }

    int count = editor_state_clipboard_count();

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
     * MAX_EDITOR_COMMANDS × MAX_LINE_LEN worst case on the stack. Allocated
     * before the undo push so a malloc failure leaves no side effects. */
    char (*buf)[MAX_LINE_LEN] = malloc((size_t)count * MAX_LINE_LEN);
    if (!buf) {
        repl_set_status_error("Paste failed: out of memory");
        return;
    }
    {
        const EditorClipboardState *cb = editor_state_clipboard();
        for (int i = 0; i < count; i++)
            repl_copy_string_fits(buf[i], MAX_LINE_LEN, cb->lines[i]);
    }

    EditorInputView input_before = editor_state_input();
    char saved_input[MAX_INPUT_LEN];
    int saved_cursor = input_before.cursor_pos;
    int saved_anchor = input_before.anchor_pos;
    int saved_insert = input_before.insert_mode;
    int saved_edit = editor_state_edit_line();
    EditorUndoRingState undo_before;
    int edit_after = pos;
    int failed_line = 0;
    char err[REPL_STATUS_TEXT_MAX];

    repl_copy_string_fits(saved_input, sizeof(saved_input), input_before.input);
    err[0] = '\0';
    editor_undo_ring_state_capture(&undo_before);
    editor_undo_snapshot_save(&g_clipboard_paste_rollback_snapshot);

    /* Single undo for the whole paste. Clipboard lines are source text, so
     * apply them through the line loader rather than the interactive input
     * dispatcher: loader failures are real failures, not "handled" editor
     * errors, and copied block delimiters paste as the exact copied rows. */
    editor_undo_push_snapshot();
    editor_insert_mode_set(0);
    editor_input_clear();

    for (int i = 0; i < count; i++) {
        err[0] = '\0';
        if (!repl_load_apply_line(buf[i], err, sizeof(err), &edit_after)) {
            failed_line = i + 1;
            break;
        }
    }
    free(buf);

    if (failed_line) {
        editor_undo_snapshot_restore(&g_clipboard_paste_rollback_snapshot);
        editor_undo_ring_state_restore(&undo_before);
        clipboard_restore_input_state(saved_input, saved_cursor, saved_anchor,
                                      saved_insert, saved_edit);
        clipboard_set_paste_failed_status(failed_line, count, err);
        return;
    }

    /* Turn off insert mode before the load_line call, even if we are going to
     * restore the entry insert mode: editor_load_line_to_input mirrors a
     * *committed* line into the input buffer for re-editing, which is
     * contradictory in insert mode — the panel renders an input row
     * *before* document_cmds[edit_line], so a pre-loaded following line
     * shows as a phantom duplicate and materializes a real duplicate on
     * the next commit. (This is the Enter-virtual-blank paste bug: an
     * unmodified-line Enter leaves insert_mode set with no doc mutation,
     * so saved_insert was 1 and the following line got staged.) */
    editor_state_edit_line_set(edit_after);
    editor_insert_mode_set(0);
    editor_load_line_to_input(editor_state_edit_line());
    repl_mark_source_dirty();

    {
        char msg[64];
        snprintf(msg, sizeof(msg), "Pasted %d line%s",
                 count, count > 1 ? "s" : "");
        repl_set_status(msg);
    }
}
