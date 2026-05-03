/*
 * ui_code_panel_layout.c -- Pure code-panel text wrapping helpers.
 *
 * The code panel has several consumers that must agree on visual rows:
 * renderer, hit-testing, search/cursor positioning, tests, and visual text
 * dumps. Keep wrapping math here so those paths do not grow separate copies.
 */
#include "ui_code_panel_layout.h"

#include <ctype.h>
#include <string.h>

CodePanelTextLayout repl_code_panel_layout_make(int panel_w, int first_x,
                                                int char_w, int wrap_at_comma) {
    CodePanelTextLayout layout;

    layout.panel_w = panel_w;
    layout.first_x = first_x;
    layout.char_w = char_w > 0 ? char_w : 1;
    layout.wrap_at_comma = wrap_at_comma;
    layout.right_pad_px = CODE_PANEL_LAYOUT_DEFAULT_RIGHT_PAD_PX;
    layout.max_hang_indent_chars =
        CODE_PANEL_LAYOUT_DEFAULT_MAX_HANG_INDENT_CHARS;
    return layout;
}

int repl_code_panel_available_chars(const CodePanelTextLayout *layout, int x) {
    int char_w = layout && layout->char_w > 0 ? layout->char_w : 1;
    int right_pad = layout ? layout->right_pad_px
                           : CODE_PANEL_LAYOUT_DEFAULT_RIGHT_PAD_PX;
    int panel_w = layout ? layout->panel_w : 0;
    int avail_px = panel_w - x - right_pad;

    if (avail_px < char_w)
        return 0;
    return avail_px / char_w;
}

int repl_code_panel_cont_indent_chars(const char *text,
                                      int max_hang_indent_chars) {
    const char *src = text ? text : "";
    int leading = 0;

    if (max_hang_indent_chars < 0)
        max_hang_indent_chars = 0;

    while (src[leading] && isspace((unsigned char)src[leading]))
        leading++;

    const char *paren = strchr(src, '(');
    if (paren && paren[1] != '\0') {
        int align = (int)(paren - src) + 1;
        int max_align = leading + max_hang_indent_chars;
        if (align > max_align)
            align = max_align;
        return align;
    }

    return leading + 4;
}

static int is_secondary_break(char c) {
    return c == ')' || c == ' ' || c == '+' ||
           c == '*' || c == '-' || c == '/';
}

int repl_code_panel_find_wrap_break(const char *text, int start,
                                    int max_chars, int len) {
    if (!text || start < 0 || max_chars < 1 || len <= 0 || start >= len)
        return -1;

    int end = start + max_chars - 1;
    int search_start = start;
    if (end >= len)
        end = len - 1;

    while (search_start < len &&
           isspace((unsigned char)text[search_start]))
        search_start++;

    /* Commas are preferred so argument lists wrap at stable boundaries. */
    for (int i = end; i > search_start; i--) {
        if (text[i] == ',')
            return i;
    }

    for (int i = end; i > search_start; i--) {
        if (is_secondary_break(text[i]))
            return i;
    }

    /* If the line has no good in-window break, extend to the next one rather
     * than hard-splitting a token. This matches the historical code panel. */
    for (int i = end + 1; i < len; i++) {
        if (text[i] == ',' ||
            (i > search_start && is_secondary_break(text[i])))
            return i;
    }

    return -1;
}

void repl_code_panel_wrap_iter_init(CodePanelWrapIter *it, const char *text,
                                    const CodePanelTextLayout *layout) {
    if (!it)
        return;

    it->text = text ? text : "";
    it->len = (int)strlen(it->text);
    it->layout = layout ? *layout
                        : repl_code_panel_layout_make(0, 0, 1, 0);
    if (it->layout.char_w <= 0)
        it->layout.char_w = 1;
    it->pos = 0;
    it->x = it->layout.first_x;
    it->cont_x = it->layout.first_x +
                 repl_code_panel_cont_indent_chars(
                     it->text, it->layout.max_hang_indent_chars) *
                 it->layout.char_w;
    it->done = 0;
}

int repl_code_panel_wrap_iter_next(CodePanelWrapIter *it, int *out_start,
                                   int *out_len, int *out_x) {
    if (!it || it->done)
        return 0;

    if (out_start) *out_start = it->pos;
    if (out_x) *out_x = it->x;

    if (it->len == 0) {
        if (out_len) *out_len = 0;
        it->done = 1;
        return 1;
    }

    int width_chars = repl_code_panel_available_chars(&it->layout, it->x);
    int remaining = it->len - it->pos;

    if (!it->layout.wrap_at_comma ||
        width_chars < 1 ||
        remaining <= width_chars) {
        if (out_len) *out_len = remaining;
        it->done = 1;
        return 1;
    }

    int break_idx = repl_code_panel_find_wrap_break(it->text, it->pos,
                                                    width_chars, it->len);
    if (break_idx < 0) {
        if (out_len) *out_len = remaining;
        it->done = 1;
        return 1;
    }

    if (out_len) *out_len = break_idx - it->pos + 1;
    it->pos = break_idx + 1;
    it->x = it->cont_x;
    return 1;
}

int repl_code_panel_row_count_for_text(const char *text,
                                       const CodePanelTextLayout *layout) {
    CodePanelWrapIter it;
    int rows = 0;
    int start, len, x;

    repl_code_panel_wrap_iter_init(&it, text, layout);
    while (repl_code_panel_wrap_iter_next(&it, &start, &len, &x))
        rows++;

    return rows > 0 ? rows : 1;
}

int repl_code_panel_segment_for_row(const char *text,
                                    const CodePanelTextLayout *layout,
                                    int want_row, int *out_start,
                                    int *out_len, int *out_x) {
    CodePanelWrapIter it;
    int row = 0;
    int start, len, x;

    if (want_row < 0)
        return 0;

    repl_code_panel_wrap_iter_init(&it, text, layout);
    while (repl_code_panel_wrap_iter_next(&it, &start, &len, &x)) {
        if (row == want_row) {
            if (out_start) *out_start = start;
            if (out_len) *out_len = len;
            if (out_x) *out_x = x;
            return 1;
        }
        row++;
    }

    return 0;
}

int repl_code_panel_cursor_row_for_text(const char *text,
                                        const CodePanelTextLayout *layout,
                                        int cursor_pos,
                                        int *out_seg_start,
                                        int *out_seg_len,
                                        int *out_seg_x) {
    CodePanelWrapIter it;
    int row = 0;
    int start, len, x;
    int text_len = (int)strlen(text ? text : "");

    if (cursor_pos < 0)
        cursor_pos = 0;
    if (cursor_pos > text_len)
        cursor_pos = text_len;

    repl_code_panel_wrap_iter_init(&it, text, layout);
    while (repl_code_panel_wrap_iter_next(&it, &start, &len, &x)) {
        int next_start = start + len;
        if (next_start >= text_len || cursor_pos < next_start) {
            if (out_seg_start) *out_seg_start = start;
            if (out_seg_len) *out_seg_len = len;
            if (out_seg_x) *out_seg_x = x;
            return row;
        }
        row++;
    }

    if (out_seg_start) *out_seg_start = text_len;
    if (out_seg_len) *out_seg_len = 0;
    if (out_seg_x) *out_seg_x = layout ? layout->first_x : 0;
    return 0;
}
