/*
 * text_panel.c -- Generic text panel renderer and hit-tester.
 *
 * This file has no dependency on repl, editor, or app headers.
 */
#include "config.h"
#include "ui/gl_2d.h"
#include "ui/text_layout.h"
#include "ui/text_search.h"
#include "ui/text_panel.h"
#include "ui/metrics.h"

#include <stdio.h>
#include <string.h>

static const GLfloat k_clr_input_text[3]     = { 0.95f, 0.95f, 0.90f };
static const GLfloat k_clr_active_row_bg[4]  = { 0.15f, 0.18f, 0.28f, 0.70f };
static const GLfloat k_clr_selection_band[4] = { 0.20f, 0.30f, 0.50f, 0.55f };
static const GLfloat k_clr_search_match[4]   = { 0.25f, 0.45f, 0.85f, 0.30f };
static const GLfloat k_clr_search_hit[4]     = { 0.95f, 0.65f, 0.18f, 0.55f };
static const GLfloat k_clr_ghost_text[4]     = { 0.50f, 0.55f, 0.65f, 0.55f };
static const GLfloat k_clr_hint_text[4]      = { 0.56f, 0.62f, 0.72f, 0.38f };
static const GLfloat k_clr_cursor_caret[4]   = { 0.90f, 0.80f, 0.25f, 0.85f };

static int text_panel_statusbar_h(const UiTextPanelSnapshot *snap) {
    return (snap && (snap->chrome_flags & UI_TEXT_PANEL_CHROME_STATUSBAR))
         ? STATUSBAR_H : 0;
}

static CodeLayout text_panel_row_layout(const UiTextPanelSnapshot *snap,
                                        const UiTextPanelRow *row) {
    int first_x = snap->text_x + row->indent_chars * FONT_W;
    return code_layout_make(snap->cp_w, first_x, FONT_W, snap->wrap_at_comma);
}

static int text_panel_color_uses_blend(const UiTextPanelColor *color) {
    return color && color->has_alpha && color->a < 1.0f;
}

static void text_panel_set_color(const UiTextPanelColor *color) {
    if (!color)
        return;
    if (color->has_alpha)
        glColor4f(color->r, color->g, color->b, color->a);
    else
        glColor3f(color->r, color->g, color->b);
}

static void text_panel_draw_segment(int x, int y, const char *text,
                                    int start, int len, void *font) {
    if (!text || len <= 0)
        return;

    glRasterPos2f((float)x, (float)y);
    for (int i = 0; i < len; i++)
        glutBitmapCharacter(font, (unsigned char)text[start + i]);
}

static int text_panel_row_uses_blend(const UiTextPanelRow *row) {
    int seg_count;

    if (!row)
        return 0;
    if (text_panel_color_uses_blend(&row->color))
        return 1;

    seg_count = row->color_segment_count;
    if (seg_count > UI_TEXT_PANEL_MAX_COLOR_SEGMENTS)
        seg_count = UI_TEXT_PANEL_MAX_COLOR_SEGMENTS;

    for (int i = 0; i < seg_count; i++) {
        if (text_panel_color_uses_blend(&row->color_segments[i].color))
            return 1;
    }

    return 0;
}

static void text_panel_draw_colored_span(const UiTextPanelSnapshot *snap,
                                         const char *text,
                                         int wrap_start,
                                         int wrap_x,
                                         int span_start,
                                         int span_end,
                                         int line_y,
                                         const UiTextPanelColor *color) {
    int span_len;

    if (!snap || !text || !color || span_end <= span_start)
        return;
    if (color->has_alpha && color->a <= 0.0f)
        return;

    span_len = span_end - span_start;
    text_panel_set_color(color);
    text_panel_draw_segment(snap->cp_x + wrap_x +
                            (span_start - wrap_start) * FONT_W,
                            line_y, text, span_start, span_len, FONT_MONO);
}

static void text_panel_draw_colored_text(const UiTextPanelSnapshot *snap,
                                         const UiTextPanelRow *row,
                                         const char *text,
                                         int wrap_start,
                                         int wrap_len,
                                         int wrap_x,
                                         int line_y) {
    int wrap_end;
    int seg_count;
    int cursor;
    int use_segments;

    if (!snap || !row || !text || wrap_len <= 0)
        return;

    wrap_end = wrap_start + wrap_len;
    seg_count = row->color_segment_count;
    if (seg_count > UI_TEXT_PANEL_MAX_COLOR_SEGMENTS)
        seg_count = UI_TEXT_PANEL_MAX_COLOR_SEGMENTS;
    use_segments = seg_count > 0;

    if (text_panel_row_uses_blend(row)) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    if (!use_segments) {
        text_panel_set_color(&row->color);
        text_panel_draw_segment(snap->cp_x + wrap_x, line_y, text,
                                wrap_start, wrap_len, FONT_MONO);
        if (text_panel_row_uses_blend(row))
            glDisable(GL_BLEND);
        return;
    }

    cursor = wrap_start;
    for (int i = 0; i < seg_count; i++) {
        const UiTextPanelColorSegment *segment = &row->color_segments[i];
        int seg_start = segment->char_start;
        int seg_end = segment->char_start + segment->char_count;

        if (seg_end <= wrap_start || seg_start >= wrap_end)
            continue;

        if (seg_start < wrap_start)
            seg_start = wrap_start;
        if (seg_end > wrap_end)
            seg_end = wrap_end;
        if (cursor < seg_start) {
            text_panel_draw_colored_span(snap, text, wrap_start, wrap_x,
                                         cursor, seg_start, line_y,
                                         &row->color);
        }
        text_panel_draw_colored_span(snap, text, wrap_start, wrap_x,
                                     seg_start, seg_end, line_y,
                                     &segment->color);
        if (seg_end > cursor)
            cursor = seg_end;
    }

    if (cursor < wrap_end) {
        text_panel_draw_colored_span(snap, text, wrap_start, wrap_x,
                                     cursor, wrap_end, line_y,
                                     &row->color);
    }

    if (text_panel_row_uses_blend(row))
        glDisable(GL_BLEND);
}

static void text_panel_draw_line_number(const UiTextPanelSnapshot *snap,
                                        int line_y, int file_line) {
    char ln[16];

    if (!(snap->chrome_flags & UI_TEXT_PANEL_CHROME_LINE_NUMS) ||
        file_line <= 0)
        return;

    snprintf(ln, sizeof(ln), "%3d", file_line);
    glColor3f(0.30f, 0.30f, 0.38f);
    gl2d_draw_string((float)(snap->cp_x + CODE_MARGIN_X),
                     (float)line_y, ln, FONT_MONO);
}

static void text_panel_draw_left_aux(const UiTextPanelSnapshot *snap,
                                     const UiTextPanelRow *row,
                                     int line_y) {
    int aux_x;

    if (!(snap->chrome_flags & UI_TEXT_PANEL_CHROME_AUX_COL) ||
        !row->left_aux_label[0])
        return;

    aux_x = snap->cp_x + snap->text_x - 6 * FONT_W;
    glColor3f(0.45f, 0.50f, 0.65f);
    gl2d_draw_string((float)aux_x, (float)line_y,
                     row->left_aux_label, FONT_MONO);
}

static void text_panel_draw_right_action(const UiTextPanelSnapshot *snap,
                                         const UiTextPanelRow *row,
                                         int line_y) {
    int sx;
    int sy;
    int sw;

    if (!row->right_action.active)
        return;

    sw = UI_TEXT_PANEL_RIGHT_ACTION_W;
    sx = snap->cp_x + snap->cp_w - CODE_MARGIN_X - sw - 2;
    sy = line_y + (LINE_H - sw) / 2 - 1;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    text_panel_set_color(&row->right_action.color);
    glRectf((float)sx, (float)sy,
            (float)(sx + sw), (float)(sy + sw));
    glColor4f(0.10f, 0.10f, 0.12f, 0.85f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)sx,        (float)sy);
    glVertex2f((float)(sx + sw), (float)sy);
    glVertex2f((float)(sx + sw), (float)(sy + sw));
    glVertex2f((float)sx,        (float)(sy + sw));
    glEnd();
    if (row->right_action.emphasized) {
        glColor4f(1.0f, 1.0f, 1.0f, 0.9f);
        glBegin(GL_LINE_LOOP);
        glVertex2f((float)(sx - 1),        (float)(sy - 1));
        glVertex2f((float)(sx + sw + 1),   (float)(sy - 1));
        glVertex2f((float)(sx + sw + 1),   (float)(sy + sw + 1));
        glVertex2f((float)(sx - 1),        (float)(sy + sw + 1));
        glEnd();
    }
    glDisable(GL_BLEND);
}

static void text_panel_draw_row_background(const UiTextPanelSnapshot *snap,
                                           const UiTextPanelRow *row,
                                           int line_y) {
    if (!snap || !row)
        return;

    if (row->background_active) {
        if (text_panel_color_uses_blend(&row->background_color)) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        text_panel_set_color(&row->background_color);
        glRectf((float)snap->cp_x, (float)(line_y - 3),
                (float)(snap->cp_x + snap->cp_w),
                (float)(line_y - 3 + LINE_H));
        if (text_panel_color_uses_blend(&row->background_color))
            glDisable(GL_BLEND);
    }

    if (row->left_marker_active) {
        if (text_panel_color_uses_blend(&row->left_marker_color)) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        text_panel_set_color(&row->left_marker_color);
        glRectf((float)(snap->cp_x + 1), (float)(line_y - 3),
                (float)(snap->cp_x + 4),
                (float)(line_y - 3 + LINE_H));
        if (text_panel_color_uses_blend(&row->left_marker_color))
            glDisable(GL_BLEND);
    }
}

static void text_panel_draw_search_highlights(const UiTextPanelSnapshot *snap,
                                              const UiTextPanelRow *row,
                                              const char *text,
                                              int seg_start, int seg_len,
                                              int seg_x, int line_y) {
    int abs_seg_x = snap->cp_x + seg_x;
    int drew = 0;

    if (!snap->search.active || snap->search.query_len <= 0 ||
        row->search_row_idx < 0 || !text || seg_len <= 0)
        return;

    for (int pos = ui_text_find_next_in_text(text, snap->search.query, 0);
         pos >= 0;
         pos = ui_text_find_next_in_text(text, snap->search.query, pos + 1)) {
        int match_end = pos + snap->search.query_len;
        int seg_end = seg_start + seg_len;
        int draw_start = pos > seg_start ? pos : seg_start;
        int draw_end = match_end < seg_end ? match_end : seg_end;

        if (draw_start >= draw_end)
            continue;

        if (!drew) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            drew = 1;
        }

        if (row->search_row_idx == snap->search.hit_row &&
            pos == snap->search.hit_char)
            glColor4fv(k_clr_search_hit);
        else
            glColor4fv(k_clr_search_match);

        glRectf((float)(abs_seg_x + (draw_start - seg_start) * FONT_W),
                (float)(line_y - 2),
            (float)(abs_seg_x + (draw_end - seg_start) * FONT_W),
                (float)(line_y - 2 + FONT_H + 4));
    }

    if (drew)
        glDisable(GL_BLEND);
}

static void text_panel_draw_indent(int x, int y, int indent_chars) {
    char spaces[32];
    int draw_indent = indent_chars;

    if (draw_indent <= 0)
        return;
    if (draw_indent > (int)sizeof(spaces) - 1)
        draw_indent = (int)sizeof(spaces) - 1;

    memset(spaces, ' ', (size_t)draw_indent);
    spaces[draw_indent] = '\0';
    glColor3f(0.30f, 0.30f, 0.38f);
    gl2d_draw_string((float)x, (float)y, spaces, FONT_MONO);
}

static int text_panel_draw_input_row(const UiTextPanelSnapshot *snap,
                                     const UiTextPanelRow *row,
                                     int visible_rows,
                                     int *io_cur,
                                     int *io_line_y,
                                     UiTextPanelOutput *out) {
    const char *input = snap->input.input ? snap->input.input : "";
    int input_len = snap->input.input_len >= 0
                  ? snap->input.input_len
                  : (int)strlen(input);
    int cursor_pos = snap->input.cursor;
    int anchor_pos = snap->input.anchor;
    int cursor_seg_start = 0;
    int cursor_seg_len = 0;
    int cursor_seg_x = snap->text_x + row->indent_chars * FONT_W;
    int wrap_row = 0;
    int wrap_start;
    int wrap_len;
    int wrap_x;
    CodeWrapIter wrap_it;
    CodeLayout layout = text_panel_row_layout(snap, row);
    int cursor_row;
    int cursor_col;

    if (cursor_pos < 0)
        cursor_pos = 0;
    if (cursor_pos > input_len)
        cursor_pos = input_len;
    if (anchor_pos < 0)
        anchor_pos = cursor_pos;

    cursor_row = code_layout_cursor_row_for_text(input, &layout, cursor_pos,
                                                 &cursor_seg_start,
                                                 &cursor_seg_len,
                                                 &cursor_seg_x);
    cursor_col = cursor_pos - cursor_seg_start;

    code_layout_wrap_iter_init(&wrap_it, input, &layout);
    while (code_layout_wrap_iter_next(&wrap_it, &wrap_start, &wrap_len, &wrap_x)) {
        if (*io_cur >= snap->scroll && *io_cur < snap->scroll + visible_rows) {
            text_panel_draw_row_background(snap, row, *io_line_y);
            if (wrap_row == 0) {
                text_panel_draw_line_number(snap, *io_line_y,
                                            row->left_gutter_label);
                text_panel_draw_left_aux(snap, row, *io_line_y);
                text_panel_draw_right_action(snap, row, *io_line_y);
            }

            glEnable(GL_BLEND);
            glColor4fv(k_clr_active_row_bg);
            glRectf((float)snap->cp_x, (float)(*io_line_y - 3),
                    (float)(snap->cp_x + snap->cp_w),
                    (float)(*io_line_y - 3 + LINE_H));
            glDisable(GL_BLEND);

            if (wrap_row == 0)
                text_panel_draw_indent(snap->cp_x + snap->text_x,
                                       *io_line_y, row->indent_chars);

            text_panel_draw_search_highlights(snap, row, input,
                                              wrap_start, wrap_len,
                                              wrap_x, *io_line_y);

            if (anchor_pos != cursor_pos) {
                int sel_lo = anchor_pos < cursor_pos ? anchor_pos : cursor_pos;
                int sel_hi = anchor_pos > cursor_pos ? anchor_pos : cursor_pos;
                int row_lo = sel_lo > wrap_start ? sel_lo : wrap_start;
                int row_hi = sel_hi < wrap_start + wrap_len
                           ? sel_hi : wrap_start + wrap_len;

                if (row_hi > row_lo) {
                    float bx = (float)(snap->cp_x + wrap_x +
                                       (row_lo - wrap_start) * FONT_W);
                    float bw = (float)((row_hi - row_lo) * FONT_W);
                    glEnable(GL_BLEND);
                    glColor4fv(k_clr_selection_band);
                    glRectf(bx, (float)(*io_line_y - 3),
                            bx + bw, (float)(*io_line_y - 3 + LINE_H));
                    glDisable(GL_BLEND);
                }
            }

            glColor3fv(k_clr_input_text);
            text_panel_draw_segment(snap->cp_x + wrap_x, *io_line_y, input,
                                    wrap_start, wrap_len, FONT_MONO);

            if (wrap_row == cursor_row) {
                int cursor_x = snap->cp_x + wrap_x + cursor_col * FONT_W;
                int hint_x = cursor_x;

                if (snap->input.ghost && snap->input.ghost[0] &&
                    cursor_pos == input_len) {
                    glEnable(GL_BLEND);
                    glColor4fv(k_clr_ghost_text);
                    gl2d_draw_string((float)cursor_x, (float)(*io_line_y),
                                     snap->input.ghost, FONT_MONO);
                    glDisable(GL_BLEND);
                    hint_x += (int)strlen(snap->input.ghost) * FONT_W;
                }

                if (snap->input.hint && snap->input.hint[0] &&
                    cursor_pos == input_len) {
                    glEnable(GL_BLEND);
                    glColor4fv(k_clr_hint_text);
                    gl2d_draw_string((float)hint_x, (float)(*io_line_y),
                                     snap->input.hint, FONT_MONO);
                    glDisable(GL_BLEND);
                }

                if (snap->input.cursor_visible && !snap->search.active) {
                    glEnable(GL_BLEND);
                    glColor4fv(k_clr_cursor_caret);
                    glRectf((float)cursor_x, (float)(*io_line_y - 2),
                            (float)(cursor_x + 2),
                            (float)(*io_line_y - 2 + FONT_H + 2));
                    glDisable(GL_BLEND);
                }

                if (out) {
                    out->cursor_px = cursor_x;
                    out->cursor_py = *io_line_y;
                    out->cursor_valid = 1;
                }
            }

            *io_line_y -= LINE_H;
        }

        (*io_cur)++;
        wrap_row++;
    }

    return wrap_row > 0 ? wrap_row : 1;
}

static int text_panel_draw_regular_row(const UiTextPanelSnapshot *snap,
                                       const UiTextPanelRow *row,
                                       int visible_rows,
                                       int *io_cur,
                                       int *io_line_y) {
    const char *text = row->text ? row->text : "";
    int wrap_row = 0;
    int wrap_start;
    int wrap_len;
    int wrap_x;
    CodeWrapIter wrap_it;
    CodeLayout layout = text_panel_row_layout(snap, row);

    if (row->kind == UI_TEXT_PANEL_ROW_PLACEHOLDER && text[0] == '\0') {
        if (*io_cur >= snap->scroll && *io_cur < snap->scroll + visible_rows) {
            text_panel_draw_row_background(snap, row, *io_line_y);
            text_panel_draw_line_number(snap, *io_line_y, row->left_gutter_label);
            text_panel_draw_left_aux(snap, row, *io_line_y);
            text_panel_draw_right_action(snap, row, *io_line_y);
            text_panel_draw_indent(snap->cp_x + snap->text_x,
                                   *io_line_y, row->indent_chars);
            *io_line_y -= LINE_H;
        }
        (*io_cur)++;
        return 1;
    }

    code_layout_wrap_iter_init(&wrap_it, text, &layout);
    while (code_layout_wrap_iter_next(&wrap_it, &wrap_start, &wrap_len, &wrap_x)) {
        if (*io_cur >= snap->scroll && *io_cur < snap->scroll + visible_rows) {
            text_panel_draw_row_background(snap, row, *io_line_y);
            if (wrap_row == 0) {
                text_panel_draw_line_number(snap, *io_line_y,
                                            row->left_gutter_label);
                text_panel_draw_left_aux(snap, row, *io_line_y);
                text_panel_draw_right_action(snap, row, *io_line_y);
            }

            text_panel_draw_search_highlights(snap, row, text,
                                              wrap_start, wrap_len,
                                              wrap_x, *io_line_y);
            text_panel_draw_colored_text(snap, row, text,
                                         wrap_start, wrap_len,
                                         wrap_x, *io_line_y);
            *io_line_y -= LINE_H;
        }
        (*io_cur)++;
        wrap_row++;
    }

    return wrap_row > 0 ? wrap_row : 1;
}

static void text_panel_draw_scrollbar(const UiTextPanelSnapshot *snap,
                                      int total_rows,
                                      int visible_rows) {
    int bar_h;
    float frac;
    float pos;
    int thumb_h;
    int thumb_y;
    int panel_top;

    if (!(snap->chrome_flags & UI_TEXT_PANEL_CHROME_SCROLLBAR) ||
        total_rows <= visible_rows)
        return;

    panel_top = snap->cp_y + snap->cp_h;
    bar_h = snap->cp_h - CODE_MARGIN_Y - LINE_H - text_panel_statusbar_h(snap);
    frac = (float)visible_rows / (float)total_rows;
    pos = (float)snap->scroll / (float)total_rows;
    thumb_h = (int)(bar_h * frac);
    if (thumb_h < 12)
        thumb_h = 12;
    thumb_y = panel_top - CODE_MARGIN_Y - LINE_H - (int)(bar_h * pos) - thumb_h;

    glEnable(GL_BLEND);
    glColor4f(0.50f, 0.50f, 0.65f, 0.35f);
    glRectf((float)(snap->cp_x + snap->cp_w - 6), (float)thumb_y,
            (float)(snap->cp_x + snap->cp_w - 1),
            (float)(thumb_y + thumb_h));
    glDisable(GL_BLEND);
}

static int text_panel_point_on_divider(const UiTextPanelSnapshot *snap,
                                       int mx, int gl_y) {
    if (!snap)
        return 0;

    if (snap->cp_w < snap->vp_w) {
        int div_x = snap->cp_x + snap->cp_w;
        return mx >= div_x - 3 && mx <= div_x + 3;
    }

    if (snap->cp_y <= 0) {
        int div_y = snap->cp_y + snap->cp_h;
        return gl_y >= div_y - 3 && gl_y <= div_y + 3;
    }

    return gl_y >= snap->cp_y - 3 && gl_y <= snap->cp_y + 3;
}

static void text_panel_draw_chrome(const UiTextPanelSnapshot *snap) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.06f, 0.06f, 0.10f, 0.92f);
    glRectf((float)snap->cp_x, (float)snap->cp_y,
            (float)(snap->cp_x + snap->cp_w),
            (float)(snap->cp_y + snap->cp_h));

    glColor4f(0.30f, 0.30f, 0.50f, 0.80f);
    glBegin(GL_LINES);
    if (snap->cp_w < snap->vp_w) {
        glVertex2f((float)(snap->cp_x + snap->cp_w), 0.0f);
        glVertex2f((float)(snap->cp_x + snap->cp_w), (float)snap->vp_h);
    } else if (snap->cp_y <= 0) {
        glVertex2f(0.0f, (float)(snap->cp_y + snap->cp_h));
        glVertex2f((float)snap->vp_w, (float)(snap->cp_y + snap->cp_h));
    } else {
        glVertex2f(0.0f, (float)snap->cp_y);
        glVertex2f((float)snap->vp_w, (float)snap->cp_y);
    }
    glEnd();
    glDisable(GL_BLEND);
}

static int text_panel_row_wrap_count(const UiTextPanelSnapshot *snap,
                                     const UiTextPanelRow *row) {
    if (row->kind == UI_TEXT_PANEL_ROW_INPUT) {
        CodeLayout layout = text_panel_row_layout(snap, row);
        return code_layout_row_count_for_text(snap->input.input ? snap->input.input : "",
                                              &layout);
    }
    if (row->kind == UI_TEXT_PANEL_ROW_PLACEHOLDER &&
        (!row->text || row->text[0] == '\0'))
        return 1;
    CodeLayout layout = text_panel_row_layout(snap, row);
    return code_layout_row_count_for_text(row->text ? row->text : "",
                                          &layout);
}

static int text_panel_char_for_click(const UiTextPanelSnapshot *snap,
                                     const UiTextPanelRow *row,
                                     int mx,
                                     int row_offset) {
    const char *text = row->kind == UI_TEXT_PANEL_ROW_INPUT
                     ? (snap->input.input ? snap->input.input : "")
                     : (row->text ? row->text : "");
    int text_len = row->kind == UI_TEXT_PANEL_ROW_INPUT
                 ? (snap->input.input_len >= 0
                    ? snap->input.input_len
                    : (int)strlen(text))
                 : (int)strlen(text);
    int seg_start = text_len;
    int seg_len = 0;
    int seg_x = snap->text_x + row->indent_chars * FONT_W;
    int col;
    int new_cursor;
    CodeLayout layout = text_panel_row_layout(snap, row);

    code_layout_segment_for_row(text, &layout, row_offset,
                                &seg_start, &seg_len, &seg_x);
    col = (mx - (snap->cp_x + seg_x) + FONT_W / 2) / FONT_W;
    if (col < 0)
        col = 0;
    if (col > seg_len)
        col = seg_len;
    new_cursor = seg_start + col;
    if (new_cursor > text_len)
        new_cursor = text_len;
    return new_cursor;
}

int ui_text_panel_visible_lines_for_height(int panel_h, int chrome_flags) {
    int statusbar_h = (chrome_flags & UI_TEXT_PANEL_CHROME_STATUSBAR)
                    ? STATUSBAR_H : 0;
    int available = panel_h - CODE_MARGIN_Y - 2 * LINE_H - 3 - statusbar_h;
    if (available < 0)
        return 1;
    return available / LINE_H + 1;
}

void ui_text_panel_render(const UiTextPanelSnapshot *snap,
                          UiTextPanelOutput         *out) {
    int panel_top;
    int line_y;
    int cur = 0;
    int visible_rows;
    int total_rows = 0;
    int statusbar_h;

    if (out)
        *out = (UiTextPanelOutput){0};
    if (!snap || !out)
        return;

    visible_rows = ui_text_panel_visible_lines_for_height(snap->cp_h,
                                                          snap->chrome_flags);
    statusbar_h = text_panel_statusbar_h(snap);

    out->visible_rows = visible_rows;
    out->statusbar_slot = (UiTextPanelRect){
        .x = snap->cp_x,
        .y = snap->cp_y,
        .w = snap->cp_w,
        .h = statusbar_h,
    };
    out->text_area = (UiTextPanelRect){
        .x = snap->cp_x + snap->text_x,
        .y = snap->cp_y + statusbar_h,
        .w = snap->cp_w - snap->text_x - CODE_MARGIN_X,
        .h = snap->cp_h - statusbar_h,
    };

    if (snap->vp_w <= 0 || snap->vp_h <= 0 || snap->cp_w <= 0 || snap->cp_h <= 0)
        return;

    gl2d_begin(snap->vp_w, snap->vp_h);
    text_panel_draw_chrome(snap);

    panel_top = snap->cp_y + snap->cp_h;
    line_y = panel_top - CODE_MARGIN_Y - 2 * LINE_H;

    for (int i = 0; i < snap->row_count; i++) {
        const UiTextPanelRow *row = &snap->rows[i];

        if (row->kind == UI_TEXT_PANEL_ROW_INPUT)
            total_rows += text_panel_draw_input_row(snap, row, visible_rows,
                                                    &cur, &line_y, out);
        else
            total_rows += text_panel_draw_regular_row(snap, row, visible_rows,
                                                      &cur, &line_y);
    }

    out->total_rows = total_rows;
    text_panel_draw_scrollbar(snap, total_rows, visible_rows);
    gl2d_end();
}

UiHit ui_text_panel_hit_test(const UiTextPanelSnapshot *snap,
                              int mx, int my) {
    UiHit h = ui_hit_none();
    int gl_y;
    int panel_top;
    int line_y_start;
    int row_from_top;
    int visible_rows;
    int target_visual_row;
    int cur = 0;

    if (!snap || snap->vp_w <= 0 || snap->vp_h <= 0)
        return h;

    gl_y = snap->vp_h - my;

    if (text_panel_point_on_divider(snap, mx, gl_y)) {
        h.kind = UI_HIT_PANEL_DIVIDER;
        h.local_x = (float)mx;
        h.local_y = (float)gl_y;
        return h;
    }

    if (mx < snap->cp_x || mx >= snap->cp_x + snap->cp_w ||
        gl_y < snap->cp_y || gl_y >= snap->cp_y + snap->cp_h)
        return h;

    panel_top = snap->cp_y + snap->cp_h;
    line_y_start = panel_top - CODE_MARGIN_Y - 2 * LINE_H;
    visible_rows = ui_text_panel_visible_lines_for_height(snap->cp_h,
                                                          snap->chrome_flags);
    row_from_top = (line_y_start + LINE_H - 3 - gl_y) / LINE_H;
    if (row_from_top < 0 || row_from_top >= visible_rows)
        return h;

    target_visual_row = snap->scroll + row_from_top;
    h.local_x = (float)(mx - snap->cp_x);
    h.local_y = (float)(gl_y - snap->cp_y);

    for (int i = 0; i < snap->row_count; i++) {
        const UiTextPanelRow *row = &snap->rows[i];
        int wrap_count = text_panel_row_wrap_count(snap, row);

        if (target_visual_row < cur + wrap_count) {
            int row_offset = target_visual_row - cur;
            int in_gutter = mx < snap->cp_x + snap->text_x;
            int resolved_line = row->source_line_idx >= 0
                              ? row->source_line_idx
                              : row->hit_target_line_idx;

            if (!row->hit_eligible)
                return h;

            if (in_gutter) {
                h.kind = UI_HIT_CODE_GUTTER;
                h.line_idx = resolved_line;
                h.visual_row = row_offset;
                h.cmd_idx = i;
                return h;
            }

            if (row->kind == UI_TEXT_PANEL_ROW_PLACEHOLDER ||
                (row->kind == UI_TEXT_PANEL_ROW_INPUT &&
                 row->source_line_idx < 0)) {
                h.kind = UI_HIT_CODE_INSERT_LINE;
                h.line_idx = resolved_line;
                h.visual_row = row_offset;
                h.char_idx = text_panel_char_for_click(snap, row, mx, row_offset);
                h.cmd_idx = i;
                return h;
            }

            h.kind = UI_HIT_CODE_TEXT;
            h.line_idx = resolved_line;
            h.visual_row = row_offset;
            h.char_idx = text_panel_char_for_click(snap, row, mx, row_offset);
            h.cmd_idx = i;
            return h;
        }

        cur += wrap_count;
    }

    return h;
}
