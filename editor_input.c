/*
 * editor_input.c — Editor input dispatch.
 *
 * Phase J1 commit 44 migrated the keyboard dispatch body and its
 * editor-owned helpers out of repl_editor.c. Special / mouse /
 * motion / mousewheel still delegate to the legacy repl_*_func
 * bodies pending commits 45 and 46.
 *
 * Editor-owned concerns hosted here:
 *  - Effect accumulation (g_pending_input_effects + reset/take helpers)
 *  - Modifier provider test seam
 *  - Audio-gesture one-shot (relocated to imrepl_ctrl in commit 48a)
 *  - Cmd-range deletion + repl_clear_all_cmds
 *  - load_line_to_input / save_newline_buf / line navigation
 *  - Commit-attempt orchestration (try_commit_*, navigation commit,
 *    parse_for_overwrite_enter, rewrite_source_text_with_indent)
 *  - Code-panel-hidden helpers used by the keyboard dispatch
 *  - keyboard_func body and its 19 handle_*_key_route helpers
 *  - feed_line() programmatic commit entry
 *
 * Non-editor concerns reachable through these handlers (config menu
 * open, replay forwarding, cfg shortcuts, accum samples, audio
 * prev/next, F1, F12, Ctrl+G, Ctrl+S, Ctrl+P, Ctrl+Q) are still
 * inline-called for behaviour preservation; commits 47a / 47b lift
 * them up into imrepl_ctrl as router pre-dispatch.
 */
#include "editor_input.h"
#include "sample.h"
#include "repl_state.h"
#include "repl_parser.h"
#include "repl_actions.h"
#include "repl_core_internal.h"
#include "editor_commit.h"
#include "repl_debug.h"
#include "repl_command_store.h"
#include "repl_source_scope.h"
#include "repl_camera_controls.h"
#include "editor_clipboard.h"
#include "editor_undo.h"
#include "replay.h"
#include "replay_state.h"
#include "editor_help_session.h"
#include "editor_completion.h"
#include "repl_keys.h"
#include "ui_panels.h"
#include "ui_layout.h"
#include "ui_menu_bar.h"
#include "ui_state.h"
#include "ui_variable_panel.h"
#include "variable_panel_drag.h"
#include "variable_panel.h"
#include "editor_inline_rename.h"
#include "repl_audio.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations. */
static void keyboard_func(unsigned char key, int x, int y);
static void special_func(int key, int x, int y);

typedef enum {
    COMMIT_UNCHANGED,
    COMMIT_OK,
    COMMIT_REJECTED
} CommitResult;

/* Browser autoplay policy: the Web Audio context stays suspended until
 * a user gesture. We call repl_audio_on_user_gesture() the first time
 * a key or mouse event arrives; native builds make this a no-op. */
static int g_audio_gesture_sent = 0;
static ReplModifierProvider g_modifier_provider_for_test = NULL;
static ReplInputDispatchEffects g_pending_input_effects = {0};

void editor_input_set_modifier_provider_for_test(ReplModifierProvider provider) {
    g_modifier_provider_for_test = provider;
}

void editor_reset_input_effects(void) {
    g_pending_input_effects = (ReplInputDispatchEffects){0};
}

ReplInputDispatchEffects editor_take_input_effects(void) {
    ReplInputDispatchEffects out = g_pending_input_effects;
    editor_reset_input_effects();
    return out;
}

int editor_get_modifiers(void) {
    if (g_modifier_provider_for_test)
        return g_modifier_provider_for_test();
    return glutGetModifiers();
}

void editor_request_redraw(void) {
    g_pending_input_effects.request_redraw = 1;
}

void editor_set_cursor(int cursor) {
    g_pending_input_effects.set_cursor = 1;
    g_pending_input_effects.cursor = cursor;
}

void editor_schedule_timer(unsigned int millis, int value) {
    g_pending_input_effects.schedule_timer = 1;
    g_pending_input_effects.timer_millis = millis;
    g_pending_input_effects.timer_value = value;
}

int editor_input_active_modifiers(void) {
    return editor_get_modifiers();
}

void editor_input_notify_audio_gesture_once(void) {
    if (g_audio_gesture_sent) return;
    g_audio_gesture_sent = 1;
    repl_audio_on_user_gesture();
}

static const int g_accum_steps[] = { 1, 2, 4, 8, 16 };

static int delete_cmd_range_allowed(int start, int count) {
    if (repl_selection_cmd_range_contains_var_decl(start, count)) {
        repl_selection_set_var_decl_action_status("remove");
        return 0;
    }

    return 1;
}

static void remove_cmd_range_unchecked(int start, int count, const char *what) {
    char msg[128];
    int end = start + count;

    /* Snapshot names to undeclare after the memmove */
    char removed_names[MAX_PREDEF_VARS][16] = {{0}};
    int n_removed = 0;
    for (int i = start; i < end; i++) {
        if (repl_state_document_cmds_mut()[i].type != CMD_VAR_DECLARE) continue;
        for (int n = 0; n < repl_state_document_cmds_mut()[i].var_decl_count && n_removed < MAX_PREDEF_VARS; n++) {
            repl_copy_string_fits(removed_names[n_removed],
                                  sizeof(removed_names[n_removed]),
                                  repl_state_document_cmds_mut()[i].var_names[n]);
            n_removed++;
        }
    }

    repl_undo_push_snapshot();
    ReplCommandStore store = repl_command_store_live();
    repl_command_store_delete_range(&store, start, count);
    editor_buffer_delete_range(start, count);

    /* Compact g_predef_vars and shift CMD_VAR_ASSIGN indices */
    for (int r = 0; r < n_removed; r++) {
        int slot = repl_eval_find_predef_var_idx(removed_names[r]);
        if (slot < 0) continue;
        repl_eval_undeclare_predef_var(removed_names[r]);
        for (int j = 0; j < repl_state_document_count(); j++) {
            if (repl_state_document_cmds_mut()[j].type == CMD_VAR_ASSIGN && repl_state_document_cmds_mut()[j].num_args > slot)
                repl_state_document_cmds_mut()[j].num_args--;
        }
    }

    repl_state_edit_line_set(start);
    if (repl_state_edit_line() > repl_state_document_count())
        repl_state_edit_line_set(repl_state_document_count());
    load_line_to_input(repl_state_edit_line());
    mark_normals_dirty();
    repl_clipboard_clear_selection();
    snprintf(msg, sizeof(msg), "%s %d line%s",
             what, count, count > 1 ? "s" : "");
    set_status(msg);
}

void delete_cmd_range(int start, int count, const char *what) {
    if (!repl_selection_normalize_cmd_range(start, count, &start, &count))
        return;
    if (!delete_cmd_range_allowed(start, count))
        return;
    remove_cmd_range_unchecked(start, count, what);
}

void repl_clear_all_cmds(void) {
    repl_undo_push_snapshot();
    ReplCommandStore store = repl_command_store_live();
    repl_command_store_clear(&store);
    repl_state_edit_line_set(0);
    editor_insert_mode_set(0);
    {
        ReplEditorInputState *inp = editor_state_input_mut();
        inp->input[0] = '\0';
        inp->input_len = 0;
    }
    editor_cursor_pos_set(0);
    {
        ReplEditorInputState *inp = editor_state_input_mut();
        inp->pending_newline[0] = '\0';
        inp->pending_newline_len = 0;
    }
    repl_eval_init_predef_vars();
    mark_normals_dirty();
    set_status("All commands cleared");
}

static const char *editor_committed_line_text(int idx) {
    const char *text = editor_buffer_line(idx);
    return (text && text[0]) ? text : "";
}

void load_line_to_input(int idx) {
    ReplEditorInputState *inp = editor_state_input_mut();
    if (idx >= 0 && idx < repl_state_document_count()) {
        const char *s = editor_committed_line_text(idx);
        while (*s && isspace((unsigned char)*s))
            s++;

        if (repl_state_document_cmds_mut()[idx].type == CMD_LABEL) {
            if (*s == ':') {
                int len = (int)strlen(s);
                while (len > 0 && isspace((unsigned char)s[len - 1]))
                    len--;
                if (len >= MAX_INPUT_LEN)
                    len = MAX_INPUT_LEN - 1;
                memcpy(inp->input, s, (size_t)len);
                inp->input[len] = '\0';
                inp->input_len = len;
                editor_cursor_pos_set(inp->input_len);
                return;
            }

            int len = (int)strlen(s);
            while (len > 0 &&
                   (s[len - 1] == ':' || isspace((unsigned char)s[len - 1])))
                len--;
            if (len > MAX_INPUT_LEN - 2)
                len = MAX_INPUT_LEN - 2;
            inp->input[0] = ':';
            memcpy(inp->input + 1, s, (size_t)len);
            inp->input[len + 1] = '\0';
            inp->input_len = len + 1;
            editor_cursor_pos_set(inp->input_len);
            return;
        }

        int len = (int)strlen(s);
        while (len > 0 &&
               (s[len - 1] == ';' || isspace((unsigned char)s[len - 1])))
            len--;
        if (len >= MAX_INPUT_LEN)
            len = MAX_INPUT_LEN - 1;
        memcpy(inp->input, s, (size_t)len);
        inp->input[len] = '\0';
        inp->input_len = len;
        editor_cursor_pos_set(len);
    } else {
        memcpy(inp->input, inp->pending_newline, (size_t)inp->pending_newline_len + 1);
        inp->input_len = inp->pending_newline_len;
        editor_cursor_pos_set(inp->pending_newline_len);
    }
}

static void save_newline_buf(void) {
    ReplEditorInputState *inp = editor_state_input_mut();
    memcpy(inp->pending_newline, inp->input, (size_t)inp->input_len + 1);
    inp->pending_newline_len = inp->input_len;
}

void repl_editor_reset_transients(void) {
    editor_commit_reset_transients();
    repl_camera_controls_reset();
    ui_panels_close_menus();
    ui_panels_handle_code_panel_release();
}

static int normalize_navigation_target(int target) {
    target = editor_commit_resolve_insert_exit_target(target);
    if (target < 0)
        target = 0;
    if (target > repl_state_document_count())
        target = repl_state_document_count();
    return target;
}

static void navigate_to_line_raw_resolved(int target) {
    if (target == repl_state_edit_line() && !editor_insert_mode())
        return;

    if (repl_state_edit_line() == repl_state_document_count() && !editor_insert_mode())
        save_newline_buf();

    repl_state_edit_line_set(target);
    editor_insert_mode_set(0);
    load_line_to_input(target);
    clear_autocomplete_state();
}

/* Rewrite the canonical source text for g_input with proper indentation.
 * Strips leading whitespace and trailing `;`/whitespace from g_input,
 * prefixes indent (2 outside a glBegin block, 4 inside), then appends `;`.
 * With include_block_depth, adds 2 spaces per open for/func/if scope at pos.
 * Writes the result into text_out[text_sz]. */
static void rewrite_source_text_with_indent(char *text_out, int text_sz,
                                            int pos, int include_block_depth) {
    char stripped[MAX_LINE_LEN];
    const char *sp = editor_state_input().input;
    while (*sp && isspace((unsigned char)*sp)) sp++;
    strncpy(stripped, sp, MAX_LINE_LEN - 1);
    stripped[MAX_LINE_LEN - 1] = '\0';
    int slen = (int)strlen(stripped);
    while (slen > 0 &&
           (stripped[slen - 1] == ';' ||
            isspace((unsigned char)stripped[slen - 1])))
        stripped[--slen] = '\0';
    int indent_len = repl_source_scope_in_begin_block_at(pos) ? 4 : 2;
    if (include_block_depth)
        indent_len += repl_source_scope_block_depth_at(pos) * 2;
    char indent[32];
    if (indent_len > (int)sizeof(indent) - 1)
        indent_len = (int)sizeof(indent) - 1;
    memset(indent, ' ', (size_t)indent_len);
    indent[indent_len] = '\0';
    snprintf(text_out, (size_t)text_sz, "%s%s;", indent, stripped);
}

/* Parse g_input into `cmd` as if it were being committed at source-line
 * `insert_idx`. Handles the three-way fan-out used by the overwrite-Enter
 * and append-at-end Enter paths:
 *   - loop/function locals visible at that line → parse_with_vars +
 *     reindent
 *   - else, predef vars referenced → plain parse, mark has_vars, reindent
 *     without vars
 *   - else, plain parse only
 * Returns 1 if parsing succeeded. */
static int parse_for_overwrite_enter(GLCmd *cmd, char *text_out, int text_sz,
                                     int insert_idx) {
    ExprVar vis_vars[MAX_EXPR_VARS];
    int num_vis_vars = collect_visible_vars(insert_idx, vis_vars, MAX_EXPR_VARS);
    memset(cmd, 0, sizeof(*cmd));
    if (text_out && text_sz > 0)
        text_out[0] = '\0';
    int parsed;
    char editor_parse_err[REPL_STATUS_TEXT_MAX];
    editor_parse_err[0] = '\0';
    if (num_vis_vars > 0) {
        ReplParseContext parse_ctx = {
            .source_line_idx = insert_idx,
            .vars = vis_vars, .num_vars = num_vis_vars,
            .err_buf = editor_parse_err,
            .err_sz  = (int)sizeof(editor_parse_err),
        };
        ReplParsedLine pl;
        parsed = repl_parser_parse_command_ctx(editor_state_input().input, &pl, &parse_ctx);
        if (parsed) {
            *cmd = pl.cmd;
            rewrite_source_text_with_indent(text_out, text_sz, insert_idx, 1);
        }
    } else {
        ReplParseContext parse_ctx = {
            .source_line_idx = insert_idx,
            .err_buf = editor_parse_err,
            .err_sz  = (int)sizeof(editor_parse_err),
        };
        ReplParsedLine pl;
        parsed = repl_parser_parse_command_ctx(editor_state_input().input, &pl, &parse_ctx);
        if (parsed) {
            *cmd = pl.cmd;
            if (repl_eval_input_has_predef_vars(editor_state_input().input)) {
                cmd->has_vars = 1;
                rewrite_source_text_with_indent(text_out, text_sz, insert_idx, 0);
            } else {
                /* No local vars: use the parsed canonical text directly. */
                if (text_out && text_sz > 0)
                    repl_copy_string_fits(text_out, text_sz, pl.text);
            }
        }
    }
    if (!parsed && editor_parse_err[0])
        set_status(editor_parse_err);
    return parsed;
}

typedef struct {
    ReplUndoSnapshot undo;
    char input[MAX_INPUT_LEN];
    int input_len;
    int cursor_pos;
    int inserting;
    char newline_buf[MAX_INPUT_LEN];
    int newline_len;
} CommitAttemptState;

static CommitAttemptState g_commit_attempt_before;
static CommitAttemptState g_navigation_commit_before;

static void capture_commit_attempt_state(CommitAttemptState *s) {
    ReplEditorInputView inp = editor_state_input();
    repl_undo_snapshot_save(&s->undo);
    memcpy(s->input, inp.input, sizeof(s->input));
    s->input_len = inp.input_len;
    s->cursor_pos = editor_cursor_pos();
    s->inserting = editor_insert_mode();
    memcpy(s->newline_buf, inp.pending_newline, sizeof(s->newline_buf));
    s->newline_len = inp.pending_newline_len;
}

/* Navigation rejection reverts commands/predefs and the saved append-line
 * buffer.  The transient typed input stays discarded by the undo snapshot
 * restore; captured input fields are used only to detect commit progress. */
static void restore_commit_attempt_committed_state(const CommitAttemptState *s) {
    ReplEditorInputState *inp = editor_state_input_mut();
    memcpy(inp->pending_newline, s->newline_buf, sizeof(s->newline_buf));
    inp->pending_newline_len = s->newline_len;
    repl_undo_snapshot_restore(&s->undo);
}

static int input_matches_committed_line(int line) {
    if (line < 0 || line >= repl_state_document_count())
        return 0;

    const char *s = editor_committed_line_text(line);
    while (*s && isspace((unsigned char)*s))
        s++;

    int slen = (int)strlen(s);
    while (slen > 0 &&
           (s[slen - 1] == ';' || isspace((unsigned char)s[slen - 1])))
        slen--;

    {
        ReplEditorInputView inp = editor_state_input();
        return slen == inp.input_len && strncmp(inp.input, s, (size_t)slen) == 0;
    }
}

static int commit_progressed_since(const CommitAttemptState *s) {
    ReplEditorInputView inp = editor_state_input();
    if (repl_state_document_count() != s->undo.num_cmds ||
        repl_state_edit_line() != s->undo.edit_line ||
        editor_insert_mode() != s->inserting ||
        inp.input_len != s->input_len ||
        editor_cursor_pos() != s->cursor_pos)
        return 1;

    if (memcmp(inp.input, s->input, (size_t)inp.input_len + 1) != 0)
        return 1;

    if (repl_state_document_count() > 0 &&
        memcmp(repl_state_document_cmds_mut(), s->undo.cmds,
               (size_t)repl_state_document_count() * sizeof(GLCmd)) != 0)
        return 1;

    return 0;
}

static int current_input_needs_navigation_commit(void) {
    if (editor_state_input().input_len <= 0)
        return 0;
    if (!editor_insert_mode() && repl_state_edit_line() < repl_state_document_count() &&
        input_matches_committed_line(repl_state_edit_line()))
        return 0;
    return 1;
}

/* Shared line-commit path for Enter and navigation.  Enter keeps its
 * line-advance/insert-mode behavior for unchanged lines; navigation treats
 * unchanged input as a no-op and only uses this helper for modified text. */
static CommitResult commit_current_input(int enter_mode) {
    if (!enter_mode && !current_input_needs_navigation_commit())
        return COMMIT_UNCHANGED;

    if (!editor_insert_mode() && repl_state_edit_line() < repl_state_document_count()) {
        int unmodified = (editor_state_input().input_len == 0 ||
                          input_matches_committed_line(repl_state_edit_line()));
        if (unmodified) {
            if (!enter_mode)
                return COMMIT_UNCHANGED;
            if (editor_cursor_pos() > 0)
                repl_state_edit_line_set(repl_state_edit_line() + 1);
            editor_insert_mode_set(1);
            {
                ReplEditorInputState *inp = editor_state_input_mut();
                inp->input[0] = '\0';
                inp->input_len = 0;
            }
            editor_cursor_pos_set(0);
            clear_autocomplete_state();
            set_status("Insert mode");
            mark_normals_dirty();
            return COMMIT_OK;
        }
    }

    if (editor_state_input().input_len > 0)
        repl_undo_push_snapshot();

    CommitAttemptState *before = &g_commit_attempt_before;
    capture_commit_attempt_state(before);

    if ((editor_insert_mode() || repl_state_edit_line() >= repl_state_document_count()) &&
        editor_state_input().input_len > 0 && try_commit_block_structs()) {
        return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;
    }

    if (editor_insert_mode()) {
        if (editor_state_input().input_len > 0) {
            GLCmd cmd;
            int parsed;
            int insert_idx = repl_state_edit_line();
            ExprVar vis_vars[MAX_EXPR_VARS];
            int num_vis_vars = collect_visible_vars(insert_idx, vis_vars, MAX_EXPR_VARS);

            char cmd_text[MAX_LINE_LEN] = "";
            char enter_parse_err[REPL_STATUS_TEXT_MAX];
            enter_parse_err[0] = '\0';
            memset(&cmd, 0, sizeof(cmd));
            if (num_vis_vars > 0) {
                ReplParseContext parse_ctx = {
                    .source_line_idx = insert_idx,
                    .vars = vis_vars, .num_vars = num_vis_vars,
                    .err_buf = enter_parse_err,
                    .err_sz  = (int)sizeof(enter_parse_err),
                };
                if (try_commit_var_statements())
                    return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;
                ReplParsedLine pl;
                parsed = repl_parser_parse_command_ctx(editor_state_input().input, &pl, &parse_ctx);
                if (parsed) {
                    cmd = pl.cmd;
                    rewrite_source_text_with_indent(cmd_text, sizeof(cmd_text),
                                                    insert_idx, 1);
                }
            } else {
                ReplParseContext parse_ctx = {
                    .source_line_idx = insert_idx,
                    .err_buf = enter_parse_err,
                    .err_sz  = (int)sizeof(enter_parse_err),
                };
                ReplParsedLine pl;
                parsed = repl_parser_parse_command_ctx(editor_state_input().input, &pl, &parse_ctx);
                if (parsed) {
                    cmd = pl.cmd;
                    repl_copy_string_fits(cmd_text, sizeof(cmd_text), pl.text);
                }
            }
            if (!parsed && enter_parse_err[0])
                set_status(enter_parse_err);

            if (parsed) {
                ReplCommandStore store = repl_command_store_live();
                int insert_pos = repl_state_edit_line();
                if (!repl_command_store_insert_one(&store, insert_pos,
                                                   &cmd, 0)) {
                    set_status("Command buffer full!");
                    return COMMIT_REJECTED;
                }
                editor_buffer_insert_line(insert_pos, cmd_text);
                repl_state_edit_line_set(repl_state_edit_line() + 1);
                {
                    ReplEditorInputState *inp = editor_state_input_mut();
                    inp->input[0] = '\0';
                    inp->input_len = 0;
                }
                editor_cursor_pos_set(0);
                set_status("Inserted");
                return COMMIT_OK;
            }
            return COMMIT_REJECTED;
        }

        if (enter_mode) {
            editor_insert_mode_set(0);
            if (repl_state_edit_line() <= repl_state_document_count())
                load_line_to_input(repl_state_edit_line());
            return COMMIT_OK;
        }
        return COMMIT_UNCHANGED;
    }

    if (repl_state_edit_line() < repl_state_document_count()) {
        int can_advance = 1;

        if (editor_state_input().input_len > 0) {
            if (repl_state_document_cmds_mut()[repl_state_edit_line()].type == CMD_FOR_BEGIN) {
                if (try_commit_for_loop())
                    return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;
                can_advance = 0;
            }
            if (repl_state_document_cmds_mut()[repl_state_edit_line()].type == CMD_FUNC_DEF) {
                if (try_commit_func_def())
                    return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;
                can_advance = 0;
            }
            if (repl_state_document_cmds_mut()[repl_state_edit_line()].type == CMD_IF_BEGIN) {
                if (try_commit_if_block())
                    return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;
                can_advance = 0;
            }
            if (try_commit_var_statements_then_insert())
                return commit_progressed_since(before) ? COMMIT_OK : COMMIT_REJECTED;

            GLCmd cmd;
            char cmd_text[MAX_LINE_LEN] = "";
            int parsed = parse_for_overwrite_enter(&cmd, cmd_text, sizeof(cmd_text),
                                                   repl_state_edit_line());
            if (parsed) {
                ReplCommandStore store = repl_command_store_live();
                int replace_idx = repl_state_edit_line();
                if (repl_command_store_replace_one(&store, replace_idx, &cmd))
                    editor_buffer_replace_line(replace_idx, cmd_text);
            } else {
                can_advance = 0;
            }
        }

        if (can_advance) {
            repl_state_edit_line_set(repl_state_edit_line() + 1);
            editor_insert_mode_set(1);
            {
                ReplEditorInputState *inp = editor_state_input_mut();
                inp->input[0] = '\0';
                inp->input_len = 0;
            }
            editor_cursor_pos_set(0);
            set_status("Insert mode");
            return COMMIT_OK;
        }
        return COMMIT_REJECTED;
    }

    if (editor_state_input().input_len > 0) {
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        int parsed = parse_for_overwrite_enter(&cmd, cmd_text, sizeof(cmd_text),
                                               repl_state_document_count());

        if (parsed) {
            ReplCommandStore store = repl_command_store_live();
            int insert_pos = repl_state_document_count();
            if (!repl_command_store_insert_one(&store, insert_pos,
                                               &cmd, 0)) {
                set_status("Command buffer full!");
                return COMMIT_REJECTED;
            }
            editor_buffer_insert_line(insert_pos, cmd_text);
            repl_state_edit_line_set(repl_state_document_count());
            {
                ReplEditorInputState *inp = editor_state_input_mut();
                inp->input[0] = '\0';
                inp->input_len = 0;
            }
            editor_cursor_pos_set(0);
            {
                ReplEditorInputState *inp = editor_state_input_mut();
                inp->pending_newline[0] = '\0';
                inp->pending_newline_len = 0;
            }
            set_status("OK");
            return COMMIT_OK;
        }
        return COMMIT_REJECTED;
    }

    return COMMIT_UNCHANGED;
}

static CommitResult commit_before_navigation(void) {
    CommitAttemptState *before = &g_navigation_commit_before;
    ReplUndoRingState undo_before;
    char rejected_status[REPL_STATUS_TEXT_MAX];
    int rejected_ttl;

    if (!current_input_needs_navigation_commit())
        return COMMIT_UNCHANGED;

    capture_commit_attempt_state(before);
    repl_undo_ring_state_capture(&undo_before);
    CommitResult result = commit_current_input(0);
    if (result != COMMIT_REJECTED)
        return result;

    {
        ReplStatusState *status = ui_state_status_mut();
        memcpy(rejected_status, status->text, sizeof(rejected_status));
        rejected_ttl = status->ttl;
        restore_commit_attempt_committed_state(before);
        repl_undo_ring_state_restore(&undo_before);
        memcpy(status->text, rejected_status, sizeof(rejected_status));
        status->text[REPL_STATUS_TEXT_MAX - 1] = '\0';
        status->ttl = rejected_ttl;
    }
    clear_autocomplete_state();
    return COMMIT_REJECTED;
}

void navigate_to_line(int target) {
    target = normalize_navigation_target(target);
    if (target == repl_state_edit_line() && !editor_insert_mode())
        return;

    if (target != repl_state_edit_line())
        (void)commit_before_navigation();

    if (target > repl_state_document_count())
        target = repl_state_document_count();
    navigate_to_line_raw_resolved(target);
}

/* Code-panel-hidden helpers used by both keyboard and special dispatch.
 * Until the special dispatch migrates (commit 45) these are exposed via
 * editor_input_code_panel_* declarations consumed by repl_editor.c. */
int editor_input_code_panel_layout(void) {
    if (repl_state_presentation().code_panel_layout < 0 || repl_state_presentation().code_panel_layout >= CODE_PANEL_LAYOUT_COUNT)
        return CODE_PANEL_LAYOUT_LEFT;
    return repl_state_presentation().code_panel_layout;
}

int editor_input_code_panel_hidden(void) {
    return editor_input_code_panel_layout() == CODE_PANEL_LAYOUT_HIDDEN;
}

int editor_input_restore_hidden_code_panel(void) {
    if (!editor_input_code_panel_hidden())
        return 0;
    repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    ui_panels_close_menus();
    return 1;
}

static int editor_key_restores_hidden_code_panel(unsigned char key, int mods) {
    if (mods & (GLUT_ACTIVE_CTRL | GLUT_ACTIVE_ALT))
        return 0;
    return key == KEY_BACKSPACE ||
           key == KEY_DELETE ||
           key == '\t' ||
           key == '\r' ||
           key == '\n' ||
           (key >= 32 && key < 127);
}

static void keyboard_begin_key(unsigned char key) {
    ReplCodePanelRuntimeState *code_panel_state = ui_state_code_panel_mut();
    code_panel_state->cursor_visible = 1;
    code_panel_state->blink_tick = 0;

    /* Cut / copy / backspace / delete preserve any active line-range
     * selection; everything else clears it before processing the key. */
    if (key != KEY_CTRL_C && key != KEY_CTRL_D && key != KEY_BACKSPACE &&
        key != KEY_CTRL_X && key != KEY_DELETE)
        repl_clipboard_clear_selection();

    editor_scroll_follow_cursor_set(1);
}

static int handle_rename_key_route(unsigned char key) {
    /* Rename overlay captures every keystroke while active, ahead of
     * the backtick/config, replay, and search branches - otherwise
     * typing `, or keys bound to replay would leak out of the rename
     * buffer and trigger unrelated UI. */
    return repl_inline_rename_handle_key(key);
}

static int handle_config_menu_key_route(unsigned char key) {
    if (!editor_state_search().active && key == '`') {
        if (replay_active())
            repl_replay_stop();
        editor_input_restore_hidden_code_panel();
        ui_panels_open_config();
        return 1;
    }
    return 0;
}

static int handle_active_replay_key_route(unsigned char key) {
    return replay_active() && replay_handle_key(key);
}

static void restore_hidden_code_panel_for_key(unsigned char key) {
    if (editor_input_code_panel_hidden()) {
        int key_mods = editor_get_modifiers();
        if (editor_key_restores_hidden_code_panel(key, key_mods))
            editor_input_restore_hidden_code_panel();
    }
}

static int handle_search_key_route(unsigned char key) {
    return handle_search_key(key);
}

static int handle_escape_key_route(unsigned char key) {
    if (key == KEY_ESC) {
        if (ui_panels_handle_escape()) {
            editor_request_redraw();
            return 1;
        }
        if (ui_state_help().visible) {
            ui_state_help_mut()->visible = 0;
            editor_help_session_set_tab(0);
            editor_help_session_set_scroll(0);
        } else if (editor_state_autocomplete().match_count > 0) {
            clear_autocomplete_state();
        } else if (editor_insert_mode()) {
            editor_insert_mode_set(0);
            if (repl_state_edit_line() <= repl_state_document_count())
                load_line_to_input(repl_state_edit_line());
            set_status("Insert mode exited");
        } else {
            {
                ReplEditorInputState *inp = editor_state_input_mut();
                inp->input[0] = '\0';
                inp->input_len = 0;
            }
            editor_cursor_pos_set(0);
            set_status("Input cleared");
        }
        return 1;
    }
    return 0;
}

static int handle_cfg_shortcut_key_route(unsigned char key) {
    return repl_cfg_handle_ascii_shortcut(key);
}

static int handle_cursor_endpoint_key_route(unsigned char key) {
    if (key == KEY_CTRL_A) {
        editor_cursor_pos_set(0);
        update_autocomplete();
        return 1;
    }
    if (key == KEY_CTRL_E) {
        editor_cursor_pos_set(editor_state_input().input_len);
        update_autocomplete();
        return 1;
    }
    return 0;
}

static int handle_undo_redo_key_route(unsigned char key) {
    if (key == KEY_CTRL_Z) {
        if (editor_get_modifiers() & GLUT_ACTIVE_SHIFT)
            repl_undo_do_redo();
        else
            repl_undo_pop_snapshot();
        return 1;
    }

    if (key == KEY_CTRL_Y) {
        repl_undo_do_redo();
        return 1;
    }
    return 0;
}

static int handle_replay_key_route(unsigned char key) {
    return replay_handle_key(key);
}

static int handle_line_delete_key_route(unsigned char key) {
    if (key == KEY_CTRL_D) {
        if (editor_insert_mode()) {
            editor_insert_mode_set(0);
            if (repl_state_edit_line() <= repl_state_document_count())
                load_line_to_input(repl_state_edit_line());
            set_status("Insert mode exited");
        } else if (repl_clipboard_sel_active()) {
            int start = repl_clipboard_sel_lo();
            int hi = repl_clipboard_sel_hi();
            if (hi >= repl_state_document_count())
                hi = repl_state_document_count() - 1;
            delete_cmd_range(start, hi - start + 1, "Deleted");
        } else if (repl_state_edit_line() < repl_state_document_count()) {
            delete_cmd_range(repl_state_edit_line(), 1, "Deleted");
        }
        return 1;
    }
    return 0;
}

/* Editor halves of the buffer-command route. Ctrl+S and Ctrl+P live
 * router-side (see repl_editor.c handle_buffer_command_router_key);
 * commit 47a folds them into imrepl_ctrl. */
static int handle_buffer_command_key_route(unsigned char key) {
    if (key == KEY_CTRL_L) {
        repl_clear_all_cmds();
        return 1;
    }

    if (key == KEY_CTRL_BACKSLASH) {
        if (repl_state_document_count() > 0) {
            repl_undo_push_snapshot();
            repl_reformat_commands();
            set_status("Reformatted command buffer");
        } else {
            set_status("Nothing to reformat");
        }
        return 1;
    }
    return 0;
}

/* Router stubs reachable from repl_editor.c during the keyboard
 * migration. Commit 47a moves Ctrl+S, Ctrl+P, Ctrl+Q out of
 * editor_input.c into imrepl_ctrl router pre-dispatch. */
int editor_input_router_handle_save_key(unsigned char key) {
    if (key == KEY_CTRL_S) {
        repl_save_default_output();
        return 1;
    }
    return 0;
}

int editor_input_router_handle_debug_dump_key(unsigned char key) {
    if (key == KEY_CTRL_P) {
        repl_debug_dump_editor(stdout, editor_buffer_view());
        repl_debug_dump_flat_commands(stdout, editor_buffer_view());
        set_status("Dumped editor + flat commands to stdout");
        return 1;
    }
    return 0;
}

int editor_input_router_handle_quit_key(unsigned char key) {
    if (key == KEY_CTRL_Q) {
        repl_export_save_output("/tmp/temp-output.c", editor_buffer_view());
        printf("Saved to %s\n", "/tmp/temp-output.c");
        exit(0);
    }
    return 0;
}

static int handle_copy_key_route(unsigned char key) {
    if (key == KEY_CTRL_C) {
        repl_clipboard_copy_current();
        return 1;
    }
    return 0;
}

static int handle_cut_key_route(unsigned char key) {
    if (key == KEY_CTRL_X) {
        repl_clipboard_cut_current();
        return 1;
    }
    return 0;
}

static int handle_paste_key_route(unsigned char key) {
    if (key == KEY_CTRL_V) {
        repl_clipboard_paste_current();
        return 1;
    }
    return 0;
}

static int handle_comment_toggle_key_route(unsigned char key) {
    if (key == '/' && (editor_get_modifiers() & GLUT_ACTIVE_CTRL)) {
        if (repl_state_edit_line() < repl_state_document_count() && !editor_insert_mode()) {
            repl_undo_push_snapshot();
            {
                GLCmd *cur = &repl_state_document_cmds_mut()[repl_state_edit_line()];
                if (cur->type == CMD_COMMENT) {
                    const char *s = editor_buffer_line(repl_state_edit_line());
                    if (!s) s = "";
                    while (*s && isspace((unsigned char)*s))
                        s++;
                    if (s[0] == '/' && s[1] == '/') {
                        s += 2;
                        if (*s == ' ')
                            s++;
                    }
                    {
                        GLCmd new_cmd;
                        char new_cmd_text[MAX_LINE_LEN] = "";
                        /* Uncomment-line path: the parser's diagnostic is
                         * deliberately dropped — the friendly "Cannot
                         * uncomment" fallback below is more actionable for
                         * this key. The err_buf catches the message away
                         * from the status bar. */
                        char uncomment_parse_err[REPL_STATUS_TEXT_MAX]; /* deliberately unread */
                        uncomment_parse_err[0] = '\0';
                        ReplParseContext parse_ctx = {
                            .source_line_idx = repl_state_edit_line(),
                            .err_buf = uncomment_parse_err,
                            .err_sz  = (int)sizeof(uncomment_parse_err),
                        };
                        memset(&new_cmd, 0, sizeof(new_cmd));
                        int built = 0;
                        int fallback_set_status = 0;

                        /* Clear any prior status (e.g. "Commented out" from the
                         * previous key press) so we can tell whether the
                         * fallback produced an actionable error of its own. */
                        ui_state_status_mut()->text[0] = '\0';

                        {
                            ReplParsedLine pl;
                            if (repl_parser_parse_command_ctx(s, &pl, &parse_ctx)) {
                                new_cmd = pl.cmd;
                                repl_copy_string_fits(new_cmd_text, sizeof(new_cmd_text),
                                                      pl.text);
                                built = 1;
                            }
                        }
                        if (!built) {
                            /* Parser may have written into uncomment_parse_err -
                             * drop it; the friendly "Cannot uncomment" message
                             * below is clearer for this key path. */

                            /* Fallback: variable assignments (`x = expr;`) live
                             * in the commit chain, not the GL-command parser.
                             * Build a CMD_VAR_ASSIGN in place so uncommenting
                             * an assignment line works. */
                            char name[16];
                            char rhs[MAX_LINE_LEN];
                            if (repl_extract_assignment_parts(s, name, sizeof(name),
                                                              rhs, sizeof(rhs))) {
                                int var_idx = repl_eval_find_predef_var_idx(name);
                                ExprVar vis[MAX_EXPR_VARS];
                                int vis_n = collect_visible_vars(repl_state_edit_line(),
                                                                 vis, MAX_EXPR_VARS);
                                char verr[128];
                                if (var_idx < 0) {
                                    char buf[128];
                                    snprintf(buf, sizeof(buf),
                                             "undeclared variable '%s' - use 'float %s;' first",
                                             name, name);
                                    set_status(buf);
                                    fallback_set_status = 1;
                                } else if (!repl_eval_validate_expression_idents(
                                        rhs, vis_n > 0 ? vis : NULL, vis_n,
                                        verr, sizeof(verr))) {
                                    set_status(verr);
                                    fallback_set_status = 1;
                                } else {
                                    ExprCtx ectx = { rhs, g_predef_vars, g_num_predef_vars };
                                    float val = repl_eval_expr(&ectx);
                                    int indent_len = (repl_source_scope_in_begin_block_at(repl_state_edit_line()) ? 4 : 2)
                                                     + repl_source_scope_block_depth_at(repl_state_edit_line()) * 2;
                                    char indent[32];
                                    if (indent_len > (int)sizeof(indent) - 1)
                                        indent_len = (int)sizeof(indent) - 1;
                                    memset(indent, ' ', (size_t)indent_len);
                                    indent[indent_len] = '\0';
                                    memset(&new_cmd, 0, sizeof(new_cmd));
                                    new_cmd.type     = CMD_VAR_ASSIGN;
                                    new_cmd.valid    = 1;
                                    new_cmd.args[0]  = val;
                                    new_cmd.num_args = var_idx;
                                    new_cmd.has_vars = repl_eval_input_has_predef_vars(rhs);
                                    new_cmd_text[0] = '\0';
                                    if (repl_format_fits(new_cmd_text,
                                                         sizeof(new_cmd_text),
                                                         "%s%s = %s;",
                                                         indent, name, rhs)) {
                                        g_predef_vars[var_idx].value = val;
                                        built = 1;
                                    } else {
                                        set_status("Command too long");
                                        fallback_set_status = 1;
                                    }
                                }
                            }
                        }
                        if (built) {
                            ReplCommandStore store = repl_command_store_live();
                            int replace_idx = repl_state_edit_line();
                            if (repl_command_store_replace_one(&store, replace_idx,
                                                               &new_cmd))
                                editor_buffer_replace_line(replace_idx,
                                                           new_cmd_text);
                            load_line_to_input(repl_state_edit_line());
                            mark_normals_dirty();
                            set_status("Uncommented");
                        } else if (!fallback_set_status) {
                            set_status("Cannot uncomment: not a valid command");
                        }
                    }
                } else if (cur->type != CMD_FOR_BEGIN &&
                           cur->type != CMD_FOR_END) {
                    char new_src[MAX_LINE_LEN];
                    GLCmd commented = *cur;
                    const char *s = editor_buffer_line(repl_state_edit_line());
                    if (!s) s = "";
                    int leading_ws_len = 0;
                    while (s[leading_ws_len] && isspace((unsigned char)s[leading_ws_len]))
                        leading_ws_len++;
                    snprintf(new_src, sizeof(new_src), "%.*s// %s", leading_ws_len, s, s + leading_ws_len);
                    commented.type = CMD_COMMENT;
                    commented.valid = 1;
                    {
                        ReplCommandStore store = repl_command_store_live();
                        int replace_idx = repl_state_edit_line();
                        if (repl_command_store_replace_one(&store, replace_idx,
                                                           &commented))
                            editor_buffer_replace_line(replace_idx, new_src);
                    }
                    load_line_to_input(repl_state_edit_line());
                    mark_normals_dirty();
                    set_status("Commented out");
                }
            }
        }
        return 1;
    }
    return 0;
}

static int handle_accum_samples_key_route(unsigned char key) {
    ReplRenderState *rs = repl_state_render_mut();
    if (key == '=' || key == '+') {
        int mods = editor_get_modifiers();
        if (!(mods & GLUT_ACTIVE_CTRL))
            return 0;
        if (rs->use_accum) {
            for (int i = 0; i < ACCUM_STEP_COUNT - 1; i++) {
                if (rs->accum_samples <= g_accum_steps[i]) {
                    rs->accum_samples = g_accum_steps[i + 1];
                    break;
                }
            }
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "Accum samples: %d", rs->accum_samples);
                set_status(msg);
            }
        }
        return 1;
    }

    if (key == KEY_CTRL_DASH ||
        (key == '-' && (editor_get_modifiers() & GLUT_ACTIVE_CTRL))) {
        if (rs->use_accum) {
            for (int i = ACCUM_STEP_COUNT - 1; i > 0; i--) {
                if (rs->accum_samples >= g_accum_steps[i]) {
                    rs->accum_samples = g_accum_steps[i - 1];
                    break;
                }
            }
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "Accum samples: %d", rs->accum_samples);
                set_status(msg);
            }
        }
        return 1;
    }
    return 0;
}

static int handle_text_delete_key_route(unsigned char key) {
    if (key == KEY_BACKSPACE || key == KEY_DELETE) {
        if (repl_clipboard_sel_active() && !editor_insert_mode()) {
            int start = repl_clipboard_sel_lo();
            int hi = repl_clipboard_sel_hi();
            if (hi >= repl_state_document_count())
                hi = repl_state_document_count() - 1;
            delete_cmd_range(start, hi - start + 1, "Deleted");
            return 1;
        }
        {
            int cur = editor_cursor_pos();
            ReplEditorInputState *inp = editor_state_input_mut();
            if (cur > 0 && inp->input_len > 0) {
                memmove(&inp->input[cur - 1], &inp->input[cur],
                        (size_t)(inp->input_len - cur + 1));
                inp->input_len--;
                editor_cursor_pos_set(cur - 1);
                update_autocomplete();
            }
        }
        return 1;
    }
    return 0;
}

static int handle_tab_key_route(unsigned char key) {
    if (key == '\t') {
        if (editor_state_autocomplete().match_count > 0) {
            accept_autocomplete();
            update_autocomplete();
        }
        return 1;
    }
    return 0;
}

static int handle_enter_key_route(unsigned char key) {
    if (key == '\r' || key == '\n') {
        if (editor_state_autocomplete().match_count > 0) {
            accept_autocomplete();
            update_autocomplete();
            return 1;
        }

        (void)commit_current_input(1);
        clear_autocomplete_state();
        mark_normals_dirty();
        return 1;
    }
    return 0;
}

static int handle_semicolon_commit_key_route(unsigned char key) {
    if (key == ';') {
        if (editor_state_input().input_len > 0) {
            repl_undo_push_snapshot();
            if (try_commit_any()) {
                clear_autocomplete_state();
                return 1;
            }
            {
                GLCmd cmd;
                char cmd_text[MAX_LINE_LEN] = "";
                int insert_idx = editor_insert_mode() ? repl_state_edit_line() :
                           (repl_state_edit_line() < repl_state_document_count() ? repl_state_edit_line() : repl_state_document_count());
                int parsed;
                ExprVar vis_vars[MAX_EXPR_VARS];
                int num_vis_vars = collect_visible_vars(insert_idx, vis_vars, MAX_EXPR_VARS);

                memset(&cmd, 0, sizeof(cmd));
                if (num_vis_vars > 0)
                    parsed = repl_parse_and_normalize_strict(editor_state_input().input, insert_idx, vis_vars, num_vis_vars,
                                                             input_has_any_visible_vars(editor_state_input().input, vis_vars, num_vis_vars),
                                                             &cmd, cmd_text, sizeof(cmd_text));
                else
                    parsed = repl_parse_and_normalize_strict(editor_state_input().input, insert_idx, NULL, 0,
                                                             repl_eval_input_has_predef_vars(editor_state_input().input),
                                                             &cmd, cmd_text, sizeof(cmd_text));

                if (parsed) {
                    ReplCommandStore store = repl_command_store_live();
                    if (editor_insert_mode()) {
                        int insert_pos = repl_state_edit_line();
                        if (repl_command_store_insert_one(&store, insert_pos,
                                                          &cmd, 0)) {
                            editor_buffer_insert_line(insert_pos, cmd_text);
                            repl_state_edit_line_set(repl_state_edit_line() + 1);
                            {
                                ReplEditorInputState *inp = editor_state_input_mut();
                                inp->input[0] = '\0';
                                inp->input_len = 0;
                            }
                            editor_cursor_pos_set(0);
                            set_status("Inserted");
                        } else {
                            set_status("Command buffer full!");
                        }
                    } else if (repl_state_edit_line() < repl_state_document_count()) {
                        int replace_idx = repl_state_edit_line();
                        if (repl_command_store_replace_one(&store, replace_idx, &cmd))
                            editor_buffer_replace_line(replace_idx, cmd_text);
                        set_status("Line updated");
                        repl_state_edit_line_set(repl_state_edit_line() + 1);
                        load_line_to_input(repl_state_edit_line());
                    } else {
                        int insert_pos = repl_state_document_count();
                        if (repl_command_store_insert_one(&store, insert_pos,
                                                          &cmd, 0)) {
                            editor_buffer_insert_line(insert_pos, cmd_text);
                            repl_state_edit_line_set(repl_state_document_count());
                            set_status("OK");
                            {
                                ReplEditorInputState *inp = editor_state_input_mut();
                                inp->input[0] = '\0';
                                inp->input_len = 0;
                            }
                            editor_cursor_pos_set(0);
                            {
                                ReplEditorInputState *inp = editor_state_input_mut();
                                inp->pending_newline[0] = '\0';
                                inp->pending_newline_len = 0;
                            }
                        } else {
                            set_status("Command buffer full!");
                        }
                    }
                }
            }
        }
        clear_autocomplete_state();
        mark_normals_dirty();
        return 1;
    }
    return 0;
}

static int handle_printable_input_key_route(unsigned char key) {
    int cur = editor_cursor_pos();
    ReplEditorInputState *inp = editor_state_input_mut();
    if (key >= 32 && key < 127 && inp->input_len < MAX_INPUT_LEN - 2) {
        memmove(&inp->input[cur + 1], &inp->input[cur],
                (size_t)(inp->input_len - cur + 1));
        inp->input[cur] = (char)key;
        inp->input_len++;
        editor_cursor_pos_set(cur + 1);
        update_autocomplete();
        return 1;
    }
    return 0;
}

static void keyboard_func(unsigned char key, int x, int y) {
    (void)x;
    (void)y;

    keyboard_begin_key(key);

    if (handle_rename_key_route(key))       return;
    if (handle_config_menu_key_route(key))  return;
    if (handle_active_replay_key_route(key)) return;

    restore_hidden_code_panel_for_key(key);

    if (handle_search_key_route(key))       return;
    if (handle_escape_key_route(key))       return;
    if (handle_cfg_shortcut_key_route(key)) return;
    if (handle_cursor_endpoint_key_route(key)) return;
    if (handle_undo_redo_key_route(key))    return;
    if (handle_replay_key_route(key))       return;
    if (handle_line_delete_key_route(key))  return;
    if (handle_buffer_command_key_route(key)) return;
    if (editor_input_router_handle_save_key(key)) return;
    if (editor_input_router_handle_debug_dump_key(key)) return;
    if (handle_copy_key_route(key))         return;
    if (handle_cut_key_route(key))          return;
    if (handle_paste_key_route(key))        return;
    if (handle_comment_toggle_key_route(key)) return;
    if (handle_accum_samples_key_route(key)) return;
    if (handle_text_delete_key_route(key))  return;
    if (handle_tab_key_route(key))          return;
    if (handle_enter_key_route(key))        return;
    if (handle_semicolon_commit_key_route(key)) return;
    if (editor_input_router_handle_quit_key(key)) return;
    (void)handle_printable_input_key_route(key);
}

int feed_line(const char *line) {
    {
        ReplEditorInputState *inp = editor_state_input_mut();
        strncpy(inp->input, line, MAX_INPUT_LEN - 1);
        inp->input[MAX_INPUT_LEN - 1] = '\0';
        inp->input_len = (int)strlen(inp->input);
        editor_cursor_pos_set(inp->input_len);
    }

    if (try_commit_any())
        return 1;

    {
        int handled = 0;
        GLCmd cmd;
        int insert_idx = editor_insert_mode() ? repl_state_edit_line() :
                   (repl_state_edit_line() < repl_state_document_count() ? repl_state_edit_line() : repl_state_document_count());
        int parsed;
        ExprVar vis_vars[MAX_EXPR_VARS];
        int num_vis_vars = collect_visible_vars(insert_idx, vis_vars, MAX_EXPR_VARS);

        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        if (num_vis_vars > 0)
            parsed = repl_parse_and_normalize_strict(editor_state_input().input, insert_idx, vis_vars, num_vis_vars,
                                                     input_has_any_visible_vars(editor_state_input().input, vis_vars, num_vis_vars),
                                                     &cmd, cmd_text, sizeof(cmd_text));
        else
            parsed = repl_parse_and_normalize_strict(editor_state_input().input, insert_idx, NULL, 0,
                                                     repl_eval_input_has_predef_vars(editor_state_input().input),
                                                     &cmd, cmd_text, sizeof(cmd_text));

        if (parsed) {
            ReplCommandStore store = repl_command_store_live();
            if (editor_insert_mode()) {
                int insert_pos = repl_state_edit_line();
                if (!repl_command_store_insert_one(&store, insert_pos,
                                                   &cmd, 0))
                    goto feed_line_done;
                editor_buffer_insert_line(insert_pos, cmd_text);
                repl_state_edit_line_set(repl_state_edit_line() + 1);
            } else if (repl_state_edit_line() < repl_state_document_count()) {
                int replace_idx = repl_state_edit_line();
                if (repl_command_store_replace_one(&store, replace_idx, &cmd))
                    editor_buffer_replace_line(replace_idx, cmd_text);
                repl_state_edit_line_set(repl_state_edit_line() + 1);
            } else {
                int insert_pos = repl_state_document_count();
                if (!repl_command_store_insert_one(&store, insert_pos,
                                                   &cmd, 0))
                    goto feed_line_done;
                editor_buffer_insert_line(insert_pos, cmd_text);
                repl_state_edit_line_set(repl_state_document_count());
            }
            handled = 1;
        }
feed_line_done:
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            inp->input[0] = '\0';
            inp->input_len = 0;
        }
        editor_cursor_pos_set(0);
        return handled;
    }
}

void repl_navigate_to_line(int target) {
    navigate_to_line(target);
}

void repl_feed_line_public(const char *line) {
    feed_line(line);
}

/* ===========================================================================
 * Special-key dispatch (F-keys, arrows, Page Up/Down, Home/End).
 * Phase J1 commit 45 migrated the body and its 9 handle_*_special_route
 * helpers from repl_editor.c. Pre-split: handle_horizontal_special_key_route
 * keeps cursor-move halves (bare Left/Right + Home/End) inline; Ctrl+Left/Right
 * (audio prev/next) and help-tab toggle land router-side via
 * editor_input_router_handle_horizontal_audio_special and
 * editor_input_router_handle_help_tab_special so commit 47b can lift them
 * into imrepl_ctrl pre-dispatch.
 * ===========================================================================
 */

static void special_begin_key(int key) {
    (void)key;
    ReplCodePanelRuntimeState *code_panel_state = ui_state_code_panel_mut();
    code_panel_state->cursor_visible = 1;
    code_panel_state->blink_tick = 0;
    editor_scroll_follow_cursor_set(1);
}

static int handle_rename_special_route(int key) {
    /* Rename captures arrows and F-keys ahead of replay/search/navigation so
     * modal text entry cannot leak actions into the editor. */
    return repl_inline_rename_handle_special(key);
}

int editor_input_router_handle_replay_special(int key) {
    return replay_handle_special(key);
}

static int editor_special_restores_hidden_code_panel(int key, int mods) {
    if (mods & (GLUT_ACTIVE_CTRL | GLUT_ACTIVE_ALT))
        return 0;
    return key == GLUT_KEY_LEFT ||
           key == GLUT_KEY_RIGHT ||
           key == GLUT_KEY_UP ||
           key == GLUT_KEY_DOWN ||
           key == GLUT_KEY_HOME ||
           key == GLUT_KEY_END ||
           key == GLUT_KEY_PAGE_UP ||
           key == GLUT_KEY_PAGE_DOWN;
}

static void restore_hidden_code_panel_for_special(int key) {
    if (editor_input_code_panel_hidden()) {
        int key_mods = editor_get_modifiers();
        if (editor_special_restores_hidden_code_panel(key, key_mods))
            editor_input_restore_hidden_code_panel();
    }
}

static int handle_search_special_route(int key) {
    return handle_search_special(key);
}

int editor_input_router_handle_cfg_special_shortcut(int key) {
    return repl_cfg_handle_special_shortcut(key);
}

int editor_input_router_handle_horizontal_audio_special(int key) {
    if (key != GLUT_KEY_LEFT && key != GLUT_KEY_RIGHT)
        return 0;
    if (!(editor_get_modifiers() & GLUT_ACTIVE_CTRL))
        return 0;
    if (key == GLUT_KEY_LEFT)
        repl_audio_prev_track();
    else
        repl_audio_next_track();
    return 1;
}

int editor_input_router_handle_help_tab_special(int key) {
    if (!ui_state_help().visible)
        return 0;
    if (key == GLUT_KEY_LEFT) {
        repl_action_help_tab_prev();
        return 1;
    }
    if (key == GLUT_KEY_RIGHT) {
        repl_action_help_tab_next();
        return 1;
    }
    return 0;
}

/* Editor-side cursor moves: bare Left/Right + Home/End. Ctrl+Left/Right
 * (audio) and help-tab toggling are handled before this by the router
 * stubs above. */
static int handle_horizontal_special_key_route(int key) {
    switch (key) {
    case GLUT_KEY_LEFT:
        if (editor_cursor_pos() > 0)
            editor_cursor_pos_set(editor_cursor_pos() - 1);
        update_autocomplete();
        return 1;
    case GLUT_KEY_RIGHT:
        if (editor_cursor_pos() < editor_state_input().input_len)
            editor_cursor_pos_set(editor_cursor_pos() + 1);
        update_autocomplete();
        return 1;
    case GLUT_KEY_HOME:
        editor_cursor_pos_set(0);
        update_autocomplete();
        return 1;
    case GLUT_KEY_END:
        editor_cursor_pos_set(editor_state_input().input_len);
        update_autocomplete();
        return 1;
    default:
        return 0;
    }
}

static int handle_vertical_special_key_route(int key) {
    ReplAutocompleteState *ac = editor_state_autocomplete_mut();
    switch (key) {
    case GLUT_KEY_UP:
        if (ui_state_help().visible) {
            editor_help_session_scroll_by(-1);
            return 1;
        }
        if (ac->match_count > 1) {
            ac->selected_idx = (ac->selected_idx - 1 + ac->match_count) % ac->match_count;
            update_selected_autocomplete_preview();
        } else if (editor_get_modifiers() & GLUT_ACTIVE_SHIFT) {
            if (!repl_clipboard_sel_active()) {
                repl_selection_start(repl_state_edit_line());
            }
            int selection_end = repl_selection_end();
            if (selection_end > 0)
                selection_end--;
            repl_selection_set_end(selection_end);
            navigate_to_line(selection_end);
        } else {
            repl_clipboard_clear_selection();
            navigate_to_line(repl_state_edit_line() - 1);
        }
        return 1;
    case GLUT_KEY_DOWN:
        if (ui_state_help().visible) {
            editor_help_session_scroll_by(1);
            return 1;
        }
        if (ac->match_count > 1) {
            ac->selected_idx = (ac->selected_idx + 1) % ac->match_count;
            update_selected_autocomplete_preview();
        } else if (editor_get_modifiers() & GLUT_ACTIVE_SHIFT) {
            if (!repl_clipboard_sel_active()) {
                repl_selection_start(repl_state_edit_line());
            }
            int selection_end = repl_selection_end();
            if (selection_end < repl_state_document_count())
                selection_end++;
            repl_selection_set_end(selection_end);
            navigate_to_line(selection_end);
        } else {
            repl_clipboard_clear_selection();
            navigate_to_line(repl_state_edit_line() + 1);
        }
        return 1;
    default:
        return 0;
    }
}

int editor_input_router_handle_help_toggle_special(int key) {
    if (key == GLUT_KEY_F1) {
        ReplHelpState *help = ui_state_help_mut();
        help->visible = !help->visible;
        editor_help_session_set_tab(0);
        editor_help_session_set_scroll(0);
        return 1;
    }
    return 0;
}

static void cycle_example_or_user_scene(void) {
    /* F12 cycles: examples[0..N-1] -> user scenes (in slot order) -> back.
     * Active example moves to the next example, then first user scene.
     * Active user scene moves to the next occupied user slot, then example 0. */
    int count = repl_example_count();
    int active_scene = repl_active_user_scene();

    if (active_scene >= 0) {
        for (int scene_idx = active_scene + 1; scene_idx < MAX_USER_SCENES; scene_idx++) {
            if (repl_user_scene_slot_used(scene_idx)) {
                repl_load_user_scene_idx(scene_idx);
                return;
            }
        }
        if (count > 0)
            repl_load_example(0);
        return;
    }

    if (count > 0) {
        int next = repl_state_scenes().active_example_idx + 1;
        if (next < count) {
            repl_load_example(next);
            return;
        }
    }

    for (int scene_idx = 0; scene_idx < MAX_USER_SCENES; scene_idx++) {
        if (repl_user_scene_slot_used(scene_idx)) {
            repl_load_user_scene_idx(scene_idx);
            return;
        }
    }
    if (count > 0)
        repl_load_example(0);
}

int editor_input_router_handle_scene_cycle_special(int key) {
    if (key == GLUT_KEY_F12) {
        cycle_example_or_user_scene();
        return 1;
    }
    return 0;
}

static int handle_page_scroll_special_key_route(int key) {
    switch (key) {
    case GLUT_KEY_PAGE_UP:
        if (ui_state_help().visible)
            editor_help_session_scroll_by(-5);
        else
            editor_scroll_set(editor_scroll() - 5);
        editor_scroll_follow_cursor_set(0);
        return 1;
    case GLUT_KEY_PAGE_DOWN:
        if (ui_state_help().visible)
            editor_help_session_scroll_by(5);
        else
            editor_scroll_set(editor_scroll() + 5);
        editor_scroll_follow_cursor_set(0);
        return 1;
    default:
        return 0;
    }
}

static void special_func(int key, int x, int y) {
    (void)x;
    (void)y;

    special_begin_key(key);

    if (handle_rename_special_route(key))   return;
    if (editor_input_router_handle_replay_special(key)) return;

    restore_hidden_code_panel_for_special(key);

    if (handle_search_special_route(key))   return;
    if (editor_input_router_handle_cfg_special_shortcut(key)) return;
    if (editor_input_router_handle_horizontal_audio_special(key)) return;
    if (editor_input_router_handle_help_tab_special(key)) return;
    if (handle_horizontal_special_key_route(key)) return;
    if (handle_vertical_special_key_route(key)) return;
    if (editor_input_router_handle_help_toggle_special(key)) return;
    if (editor_input_router_handle_scene_cycle_special(key)) return;
    if (handle_page_scroll_special_key_route(key)) return;
}

ReplInputDispatchEffects editor_handle_key(unsigned char key, int x, int y) {
    editor_reset_input_effects();
    editor_input_notify_audio_gesture_once();
    keyboard_func(key, x, y);
    return editor_take_input_effects();
}

ReplInputDispatchEffects editor_handle_special(int key, int x, int y) {
    editor_reset_input_effects();
    editor_input_notify_audio_gesture_once();
    special_func(key, x, y);
    return editor_take_input_effects();
}

ReplInputDispatchEffects editor_handle_mouse(int button, int state, int x, int y) {
    return repl_mouse_func(button, state, x, y);
}

ReplInputDispatchEffects editor_handle_motion(int x, int y) {
    return repl_motion_func(x, y);
}

ReplInputDispatchEffects editor_handle_passive_motion(int x, int y) {
    return repl_passive_motion_func(x, y);
}
