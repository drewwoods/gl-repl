/*
 * glr_color_picker_bridge.c - REPL/editor-backed ColorPickerHostBridge.
 *
 * The color-picker peer hands this host a cmd index + RGBA; the host owns
 * the document read, the glColor3f/glColor4f/gluColor/glClearColor source
 * synthesis, the parse re-check, and the editor commit pipeline (the body
 * that used to live in color_picker_state.c::color_picker_write_cmd). It
 * also answers the picker's viewport + code-panel geometry queries.
 */
#include "app/glr_color_picker_bridge.h"

#include "subsystems/color_picker/color_picker_state.h"  /* ColorPickerHostBridge */
#include "config.h"               /* MAX_LINE_LEN, REPL_STATUS_TEXT_MAX */
#include "editor/commit.h"        /* editor_commit_apply_external_change */
#include "editor/input.h"         /* editor_load_line_to_input */
#include "editor/state.h"         /* editor_state_edit_line */
#include "repl/color_limits.h"    /* REPL_CLEAR_COLOR_MAX_V */
#include "repl/command.h"         /* GLCmd, CMD_* */
#include "repl/compile.h"         /* ReplCompiledChange */
#include "repl/core.h"            /* repl_set_status */
#include "repl/parser.h"          /* repl_parser_parse_command_ctx */
#include "repl/state_views.h"     /* repl_state_document_count / _cmd_at */
#include "ui/app/layout.h"        /* ui_layout_code_panel_rect */
#include "ui/app/state.h"         /* ui_state_viewport */

#include <stdio.h>
#include <string.h>

static const GLCmd *cp_cmd_at(int cmd_idx) {
    if (cmd_idx < 0 || cmd_idx >= repl_state_document_count())
        return NULL;
    return repl_state_document_cmd_at(cmd_idx);
}

static int glr_cp_read_color(int cmd_idx, float *r, float *g, float *b, float *a,
                             int *has_alpha, float *value_max) {
    const GLCmd *cmd = cp_cmd_at(cmd_idx);
    if (!cmd || !cmd->valid || cmd->has_vars)
        return 0;
    if (cmd->type != CMD_COLOR3F && cmd->type != CMD_COLOR4F &&
        cmd->type != CMD_TESS_COLOR && cmd->type != CMD_CLEAR_COLOR)
        return 0;
    int ha = (cmd->type == CMD_COLOR4F ||
              cmd->type == CMD_TESS_COLOR ||
              cmd->type == CMD_CLEAR_COLOR);
    if (r) *r = cmd->args[0];
    if (g) *g = cmd->args[1];
    if (b) *b = cmd->args[2];
    if (a) *a = ha ? cmd->args[3] : 1.0f;
    if (has_alpha) *has_alpha = ha;
    if (value_max)
        *value_max = (cmd->type == CMD_CLEAR_COLOR)
                   ? REPL_CLEAR_COLOR_MAX_V : 1.0f;
    return 1;
}

static int glr_cp_write_color(int cmd_idx, float r, float g, float b, float a,
                              int capture_undo) {
    const GLCmd *cmd = cp_cmd_at(cmd_idx);
    if (!cmd)
        return 0;

    char new_line[MAX_LINE_LEN];
    int written;
    if (cmd->type == CMD_CLEAR_COLOR) {
        /* V is already clamped at the picker's drag sites. */
        written = snprintf(new_line, sizeof(new_line),
                           "glClearColor(%g, %g, %g, %g);", r, g, b, a);
    } else if (cmd->type == CMD_COLOR3F) {
        written = snprintf(new_line, sizeof(new_line),
                           "glColor3f(%g, %g, %g);", r, g, b);
    } else if (cmd->type == CMD_COLOR4F) {
        written = snprintf(new_line, sizeof(new_line),
                           "glColor4f(%g, %g, %g, %g);", r, g, b, a);
    } else if (cmd->type == CMD_TESS_COLOR) {
        written = snprintf(new_line, sizeof(new_line),
                           "gluColor(%g, %g, %g, %g);", r, g, b, a);
    } else {
        return 0;
    }

    if (written < 0 || written >= (int)sizeof(new_line)) {
        repl_set_status("Command too long");
        return 0;
    }

    /* Surface parser errors so a malformed rewrite (shouldn't happen for
     * synthesized color commands but defends the path) shows in the status
     * bar instead of failing silently. */
    char picker_parse_err[REPL_STATUS_TEXT_MAX];
    picker_parse_err[0] = '\0';
    ReplParseContext parse_ctx = {
        .source_line_idx = cmd_idx,
        .err_buf = picker_parse_err,
        .err_sz  = (int)sizeof(picker_parse_err),
    };
    ReplParsedLine pl;
    if (!repl_parser_parse_command_ctx(new_line, &pl, &parse_ctx)) {
        if (picker_parse_err[0])
            repl_set_status(picker_parse_err);
        return 0;
    }

    /* Route the cmd-store + editor-buffer writes through the editor commit
     * pipeline so the picker stays a pure value-emitter. The session's first
     * writeback captures undo; subsequent drags pass capture_undo=0 so each
     * session lands as one undo entry. */
    ReplCompiledChange change;
    repl_compiled_change_init(&change);
    change.kind  = REPL_COMPILED_REPLACE_ONE;
    change.pos   = cmd_idx;
    change.count = 1;
    change.cmds[0] = pl.cmd;
    int text_len = (int)strlen(pl.text);
    if (text_len >= MAX_LINE_LEN) text_len = MAX_LINE_LEN - 1;
    memcpy(change.text[0], pl.text, (size_t)text_len);
    change.text[0][text_len] = '\0';

    int applied = editor_commit_apply_external_change(&change, capture_undo, 0);

    /* The editor input buffer is a live shadow of the active edit line.
     * The REPLACE_ONE above updated the command store + editor buffer
     * text, but not the input buffer — so when the picker edits the line
     * the cursor is parked on, the code panel keeps showing the stale
     * pre-edit text, and a later navigation re-commits that stale input
     * over the picker's change (commit_before_navigation), silently
     * reverting it. Reload the shadow whenever we just wrote the active
     * edit line so the displayed text tracks the picker and navigation
     * has nothing stale to re-commit. */
    if (applied && cmd_idx == editor_state_edit_line())
        editor_load_line_to_input(cmd_idx);

    return applied;
}

static void glr_cp_viewport(int *w, int *h) {
    UiViewportState vp = ui_state_viewport();
    if (w) *w = vp.window_w;
    if (h) *h = vp.window_h;
}

static void glr_cp_code_panel_rect(int *x, int *w) {
    ui_layout_code_panel_rect(x, NULL, w, NULL);
}

static const ColorPickerHostBridge g_glr_cp_host = {
    glr_cp_read_color,
    glr_cp_write_color,
    glr_cp_viewport,
    glr_cp_code_panel_rect,
};

void glr_color_picker_install_host(void) {
    color_picker_install_host(&g_glr_cp_host);
}
