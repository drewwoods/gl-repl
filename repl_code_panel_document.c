/*
 * repl_code_panel_document.c -- Code-panel document row model.
 *
 * This module translates source/header/footer state into wrapped document rows.
 * Rendering and mouse hit-testing consume the same layout so scrolling,
 * selection, replay annotations, and visual dumps do not drift apart.
 */
#include "sample.h"
#include "repl_export.h"
#include "repl_code_panel_document.h"
#include "repl_replay_annotations.h"
#include "repl_state.h"

#define IMPORT_EXPORT_STATE (repl_state_import_export())
#define g_workspace_header_lines (IMPORT_EXPORT_STATE->workspace_header_lines)
#define g_workspace_header_line_count (*IMPORT_EXPORT_STATE->workspace_header_line_count)
#define g_render_state_lines (IMPORT_EXPORT_STATE->render_state_lines)
#define g_cam_lines (IMPORT_EXPORT_STATE->cam_lines)

CodePanelTextLayout repl_code_panel_document_text_layout(int panel_w,
                                                         int first_x) {
    return repl_code_panel_layout_make(panel_w, first_x, FONT_W,
                                       *repl_state_presentation()->wrap_at_comma);
}

void repl_code_panel_document_wrap_iter_init(CodePanelWrapIter *it,
                                             const char *text,
                                             int first_x, int panel_w) {
    CodePanelTextLayout layout =
        repl_code_panel_document_text_layout(panel_w, first_x);
    repl_code_panel_wrap_iter_init(it, text, &layout);
}

int repl_code_panel_document_wrap_iter_next(CodePanelWrapIter *it,
                                            int *out_start,
                                            int *out_len,
                                            int *out_x) {
    return repl_code_panel_wrap_iter_next(it, out_start, out_len, out_x);
}

int repl_code_panel_document_row_count_for_text(const char *text,
                                                int first_x, int panel_w) {
    CodePanelTextLayout layout =
        repl_code_panel_document_text_layout(panel_w, first_x);
    return repl_code_panel_row_count_for_text(text, &layout);
}

int repl_code_panel_document_segment_for_row(const char *text,
                                             int first_x, int panel_w,
                                             int want_row,
                                             int *out_start,
                                             int *out_len,
                                             int *out_x) {
    CodePanelTextLayout layout =
        repl_code_panel_document_text_layout(panel_w, first_x);
    return repl_code_panel_segment_for_row(text, &layout, want_row,
                                           out_start, out_len, out_x);
}

int repl_code_panel_document_cursor_row_for_text(const char *text,
                                                 int first_x, int panel_w,
                                                 int cursor_pos,
                                                 int *out_seg_start,
                                                 int *out_seg_len,
                                                 int *out_seg_x) {
    CodePanelTextLayout layout =
        repl_code_panel_document_text_layout(panel_w, first_x);
    return repl_code_panel_cursor_row_for_text(text, &layout, cursor_pos,
                                               out_seg_start, out_seg_len,
                                               out_seg_x);
}

static int code_panel_header_row_count(int panel_w, int text_x) {
    int rows = 0;

    for (int i = 0; i < g_workspace_header_line_count; i++)
        rows += repl_code_panel_document_row_count_for_text(
            g_workspace_header_lines[i], text_x, panel_w);
    for (int i = 0; g_header_pre[i]; i++)
        rows += repl_code_panel_document_row_count_for_text(
            g_header_pre[i], text_x, panel_w);
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++)
        rows += repl_code_panel_document_row_count_for_text(
            g_render_state_lines[i], text_x, panel_w);
    for (int i = 0; i < CAM_LINE_COUNT; i++)
        rows += repl_code_panel_document_row_count_for_text(
            g_cam_lines[i], text_x, panel_w);
    for (int i = 0; g_header_post[i]; i++)
        rows += repl_code_panel_document_row_count_for_text(
            g_header_post[i], text_x, panel_w);

    return rows;
}

static int code_panel_footer_row_count(int panel_w, int text_x) {
    int rows = 0;
    char line[MAX_LINE_LEN];

    for (int i = 0; g_footer_pre_init[i]; i++)
        rows += repl_code_panel_document_row_count_for_text(
            g_footer_pre_init[i], text_x, panel_w);
    for (int i = 0; i < init_section_line_count(); i++) {
        init_section_line(i, line, sizeof(line));
        rows += repl_code_panel_document_row_count_for_text(
            line, text_x, panel_w);
    }
    for (int i = 0; g_footer_post_init[i]; i++)
        rows += repl_code_panel_document_row_count_for_text(
            g_footer_post_init[i], text_x, panel_w);

    return rows;
}

static int code_panel_leading_ws_chars(const char *text) {
    int n = 0;

    while (text && text[n] && isspace((unsigned char)text[n]))
        n++;
    return n;
}

int repl_code_panel_document_active_indent_chars(void) {
    if (repl_state_insert_mode())
        return cmd_indent_chars(repl_state_edit_line());
    if (repl_state_edit_line() >= 0 && repl_state_edit_line() < repl_state_document_count())
        return code_panel_leading_ws_chars(repl_state_document_cmds_mut()[repl_state_edit_line()].source);
    return cmd_indent_chars(repl_state_document_count());
}

static int code_panel_command_main_rows(int cmd_idx, int panel_w, int text_x) {
    if (!repl_state_insert_mode() && cmd_idx == repl_state_edit_line()) {
        int indent_chars = repl_code_panel_document_active_indent_chars();
        return repl_code_panel_document_row_count_for_text(
            repl_state_editor_input()->input, text_x + indent_chars * FONT_W, panel_w);
    }

    {
        char display_text[MAX_INPUT_LEN];
        if (!code_panel_get_command_display_text(cmd_idx, display_text,
                                                 sizeof(display_text)))
            return 1;
        return repl_code_panel_document_row_count_for_text(display_text,
                                                           text_x, panel_w);
    }
}

static void code_panel_precompute_layout_rows(int panel_w, int text_x,
                                              int *main_rows,
                                              int *replay_extra_rows) {
    for (int i = 0; i < repl_state_document_count(); i++) {
        if (main_rows)
            main_rows[i] = code_panel_command_main_rows(i, panel_w, text_x);
        if (replay_extra_rows)
            replay_extra_rows[i] =
                repl_replay_annotation_extra_rows_for_line(i);
    }
}

static int code_panel_insert_rows(int panel_w, int text_x) {
    int indent_chars = repl_code_panel_document_active_indent_chars();
    return repl_code_panel_document_row_count_for_text(
        repl_state_editor_input()->input, text_x + indent_chars * FONT_W, panel_w);
}

static int code_panel_newline_rows(int panel_w, int text_x) {
    if (repl_state_edit_line() == repl_state_document_count()) {
        int indent_chars = repl_code_panel_document_active_indent_chars();
        return repl_code_panel_document_row_count_for_text(
            repl_state_editor_input()->input, text_x + indent_chars * FONT_W, panel_w);
    }
    return 1;
}

static int code_panel_cursor_doc_line_from_layout(
    int header_rows, const int *cmd_main_rows, const int *replay_extra_rows,
    int panel_w, int text_x) {
    int cursor_doc_line = header_rows;

    if (repl_state_insert_mode()) {
        for (int i = 0; i < repl_state_edit_line() && i < repl_state_document_count(); i++) {
            cursor_doc_line += cmd_main_rows[i];
            cursor_doc_line += replay_extra_rows[i];
        }
        cursor_doc_line += repl_code_panel_document_cursor_row_for_text(
            repl_state_editor_input()->input, text_x + repl_code_panel_document_active_indent_chars() * FONT_W,
            panel_w, repl_state_cursor_pos(), NULL, NULL, NULL);
    } else if (repl_state_edit_line() < repl_state_document_count()) {
        for (int i = 0; i < repl_state_edit_line(); i++) {
            cursor_doc_line += cmd_main_rows[i];
            cursor_doc_line += replay_extra_rows[i];
        }
        cursor_doc_line += repl_code_panel_document_cursor_row_for_text(
            repl_state_editor_input()->input, text_x + repl_code_panel_document_active_indent_chars() * FONT_W,
            panel_w, repl_state_cursor_pos(), NULL, NULL, NULL);
    } else {
        for (int i = 0; i < repl_state_document_count(); i++) {
            cursor_doc_line += cmd_main_rows[i];
            cursor_doc_line += replay_extra_rows[i];
        }
        cursor_doc_line += repl_code_panel_document_cursor_row_for_text(
            repl_state_editor_input()->input, text_x + repl_code_panel_document_active_indent_chars() * FONT_W,
            panel_w, repl_state_cursor_pos(), NULL, NULL, NULL);
    }

    return cursor_doc_line;
}

static int code_panel_follow_doc_line_from_layout(
    int cursor_doc_line, int header_rows, const int *cmd_main_rows,
    const int *replay_extra_rows) {
    const ReplReplayRuntimeState *replay = repl_state_replay();
    int follow_doc_line = cursor_doc_line;

    if (*replay->active &&
        *replay->src_line_idx >= 0 &&
        *replay->src_line_idx < repl_state_document_count()) {
        follow_doc_line = header_rows;
        for (int i = 0; i < *replay->src_line_idx; i++) {
            follow_doc_line += cmd_main_rows[i];
            follow_doc_line += replay_extra_rows[i];
        }
        if (replay_extra_rows[*replay->src_line_idx] > 0) {
            follow_doc_line += cmd_main_rows[*replay->src_line_idx];
            follow_doc_line += replay_extra_rows[*replay->src_line_idx] - 1;
        } else if (cmd_main_rows[*replay->src_line_idx] > 0) {
            follow_doc_line += cmd_main_rows[*replay->src_line_idx] - 1;
        }
    }

    return follow_doc_line;
}

int repl_code_panel_document_visible_lines_for_height(int cp_h) {
    int available = cp_h - CODE_MARGIN_Y - 2 * LINE_H - 3 - STATUSBAR_H;
    if (available < 0)
        return 1;
    return available / LINE_H + 1;
}

void repl_code_panel_document_build(CodePanelDocumentLayout *layout,
                                    int panel_w, int text_x, int cp_h) {
    int total_lines;

    if (!layout)
        return;
    memset(layout, 0, sizeof(*layout));
    layout->panel_w = panel_w;
    layout->text_x = text_x;
    layout->cp_h = cp_h;
    layout->visible_lines =
        repl_code_panel_document_visible_lines_for_height(cp_h);

    repl_replay_annotations_prepare();

    layout->header_rows = code_panel_header_row_count(panel_w, text_x);
    layout->footer_rows = code_panel_footer_row_count(panel_w, text_x);
    code_panel_precompute_layout_rows(panel_w, text_x,
                                      layout->cmd_main_rows,
                                      layout->replay_extra_rows);

    total_lines = layout->header_rows + layout->footer_rows
                + code_panel_newline_rows(panel_w, text_x);
    for (int i = 0; i < repl_state_document_count(); i++) {
        if (repl_state_insert_mode() && i == repl_state_edit_line())
            total_lines += code_panel_insert_rows(panel_w, text_x);
        total_lines += layout->cmd_main_rows[i];
        total_lines += layout->replay_extra_rows[i];
    }
    layout->total_lines = total_lines;

    layout->cursor_doc_line = code_panel_cursor_doc_line_from_layout(
        layout->header_rows, layout->cmd_main_rows, layout->replay_extra_rows,
        panel_w, text_x);
    layout->follow_doc_line = code_panel_follow_doc_line_from_layout(
        layout->cursor_doc_line, layout->header_rows, layout->cmd_main_rows,
        layout->replay_extra_rows);
}

void repl_code_panel_document_apply_follow_scroll(
    const CodePanelDocumentLayout *layout) {
    int max_scroll;

    if (!layout)
        return;

    max_scroll = layout->total_lines - layout->visible_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    if (g_scroll_follow_cursor) {
        if (layout->follow_doc_line < g_scroll)
            g_scroll = layout->follow_doc_line;
        if (layout->follow_doc_line >= g_scroll + layout->visible_lines)
            g_scroll = layout->follow_doc_line - layout->visible_lines + 1;
        if (g_scroll > max_scroll) g_scroll = max_scroll;
        if (g_scroll < 0) g_scroll = 0;
        g_scroll_follow_cursor = 0;
    }
}

int repl_code_panel_document_target_for_doc_line(
    int doc_line, const CodePanelDocumentLayout *layout,
    int *out_target, int *out_on_insert_line, int *out_row_offset) {
    int row;

    if (!layout)
        return 0;

    row = doc_line - layout->header_rows;
    if (row < 0)
        return 0;

    for (int i = 0; i <= repl_state_document_count(); i++) {
        if (repl_state_insert_mode() && i == repl_state_edit_line()) {
            int insert_rows = code_panel_insert_rows(layout->panel_w,
                                                     layout->text_x);
            if (row < insert_rows) {
                if (out_target) *out_target = -1;
                if (out_on_insert_line) *out_on_insert_line = 1;
                if (out_row_offset) *out_row_offset = row;
                return 1;
            }
            row -= insert_rows;
        }

        if (i < repl_state_document_count()) {
            int main_rows = layout->cmd_main_rows[i];
            if (row < main_rows) {
                if (out_target) *out_target = i;
                if (out_on_insert_line) *out_on_insert_line = 0;
                if (out_row_offset) *out_row_offset = row;
                return 1;
            }
            row -= main_rows;

            {
                int replay_rows = layout->replay_extra_rows[i];
                if (row < replay_rows) {
                    if (out_target) *out_target = i;
                    if (out_on_insert_line) *out_on_insert_line = 0;
                    if (out_row_offset) *out_row_offset = 0;
                    return 1;
                }
                row -= replay_rows;
            }
        } else {
            int newline_rows = code_panel_newline_rows(layout->panel_w,
                                                       layout->text_x);
            if (row < newline_rows) {
                if (out_target) *out_target = repl_state_document_count();
                if (out_on_insert_line) *out_on_insert_line = 0;
                if (out_row_offset) *out_row_offset = row;
                return 1;
            }
            return 0;
        }
    }

    return 0;
}
