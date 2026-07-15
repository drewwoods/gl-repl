/*
 * ui_gl_state_panel.c -- Floating OpenGL-state popup table.
 *
 * Pure renderer + hit-test: consumes the controller-built
 * UiGlStatePanelView and draws the right-click state report as a table
 * (state name, current value, and — behind the header's expand chip,
 * collapsed by default so the popup stays narrow — the OpenGL 2.1
 * default and latest change source columns) anchored near the click.
 * When the report is taller than the window the row window scrolls
 * (wheel, routed by the controller) with a flyout-style right-edge
 * scrollbar hint. Report construction, anchor validation, and
 * open/close/scroll/expand state all live outside (glr_ctrl / ui_state).
 */
#include <stdio.h>
#include <string.h>

#include "ui/app/gl_state_panel.h"
#include "ui/app/layout.h"
#include "ui/core/metrics.h"
#include "ui/core/theme.h"

#include "ui/core/gl_2d.h"

/* Popup paddings and column caps (in character cells; FONT_MONO). */
#define GLSP_PAD_X          10
#define GLSP_PAD_Y           6
#define GLSP_COL_GAP_CHARS   2
#define GLSP_NAME_CHARS_MAX 36
#define GLSP_VAL_CHARS_MAX  44
#define GLSP_SOURCE_CHARS_MAX 18
#define GLSP_EDGE_MARGIN     8

static const char GLSP_TITLE[]      = "OpenGL state at this line";
static const char GLSP_HDR_NAME[]   = "state";
static const char GLSP_HDR_CUR[]    = "current";
static const char GLSP_HDR_DEF[]    = "[-] default (GL 2.1)";
static const char GLSP_HDR_SOURCE[] = "source";
static const char GLSP_HDR_DETAILS_CLOSED[] = "[+] default/source";
static const char GLSP_EMPTY_MSG[]  =
    "No init() or display() OpenGL state has been touched.";

/* Solved popup geometry, shared by render and hit-test so the click-away
 * and wheel routing agree exactly with the drawn frame. */
typedef struct {
    int px, py;               /* top-left, y-up */
    int popup_w, popup_h;
    int details;              /* default/source columns expanded */
    int name_chars, cur_chars, def_chars, source_chars;
    int col0_x, col1_x, col2_x, col3_x;
    int tog_x0, tog_x1;       /* expand/collapse header chip cell (y-up; */
    int tog_y0, tog_y1;       /* zero-width when the report is empty) */
    int visible_rows;         /* report rows drawn (0 for the empty msg) */
    int max_scroll;
    int scroll;               /* view->scroll_rows clamped to [0, max] */
} GlspLayout;

static void glsp_source_text(const ReplGlStateReportRow *row,
                             char *buf, int buf_size) {
    if (!buf || buf_size <= 0)
        return;
    if (!row) {
        buf[0] = '\0';
        return;
    }
    switch (row->source.kind) {
    case REPL_GL_STATE_SOURCE_INIT:
        snprintf(buf, (size_t)buf_size, "init()");
        break;
    case REPL_GL_STATE_SOURCE_DISPLAY:
        if (row->source.source_line_idx >= 0)
            snprintf(buf, (size_t)buf_size, "display():%d",
                     row->source.source_line_idx + 1);
        else
            snprintf(buf, (size_t)buf_size, "display()");
        break;
    default:
        snprintf(buf, (size_t)buf_size, "--");
        break;
    }
}

/* Copy src into dst clipped to max_chars cells, marking truncation with
 * a trailing "..". Returns the resulting length. */
static int glsp_clip(char *dst, int dst_size, const char *src, int max_chars) {
    int len = (int)strlen(src);
    if (max_chars > dst_size - 1)
        max_chars = dst_size - 1;
    if (len <= max_chars) {
        memcpy(dst, src, (size_t)len + 1);
        return len;
    }
    memcpy(dst, src, (size_t)max_chars);
    dst[max_chars] = '\0';
    if (max_chars >= 2) {
        dst[max_chars - 1] = '.';
        dst[max_chars - 2] = '.';
    }
    return max_chars;
}

static int glsp_clamp_chars(int chars, int min_chars, int max_chars) {
    if (chars < min_chars) return min_chars;
    if (chars > max_chars) return max_chars;
    return chars;
}

/* Solve the popup's frame, columns, and scrolled row window. Returns 0
 * when the view is hidden / degenerate (nothing to draw or hit). */
static int glsp_solve(const UiGlStatePanelView *view, GlspLayout *out) {
    const ReplGlStateReport *report;
    int table_chars, max_table_chars, chrome_rows, row_capacity;
    int top_limit, menu_y = 0;
    int cur_floor;
    char source_text[32];
    int i;

    if (!view || !view->visible || !view->report)
        return 0;
    if (view->window_w <= 0 || view->window_h <= 0)
        return 0;
    report = view->report;

    /* The popup never covers the menu bar band: menu clicks must keep
     * routing to the menu, and the click-inside-popup swallow would eat
     * them otherwise (same reason the autocomplete popup reads live
     * layout geometry from its renderer). */
    ui_layout_menu_bar_rect(NULL, &menu_y, NULL, NULL);
    top_limit = (menu_y > 0 && menu_y < view->window_h)
                    ? menu_y - 2 : view->window_h - GLSP_EDGE_MARGIN;

    /* Column widths from content, floored at the header labels and
     * capped so one long value (a matrix row) can't blow up the popup.
     * Collapsed, the default/source columns drop out entirely; the
     * header's "[+]" chip rides inside the current column's header cell
     * (right-aligned), so it only floors that column's width instead of
     * adding a blank third column under itself. */
    out->details = view->details_expanded ? 1 : 0;
    cur_floor = (int)strlen(GLSP_HDR_CUR);
    if (!out->details)
        cur_floor += GLSP_COL_GAP_CHARS +
                     (int)strlen(GLSP_HDR_DETAILS_CLOSED);
    out->name_chars = (int)strlen(GLSP_HDR_NAME);
    out->cur_chars  = (int)strlen(GLSP_HDR_CUR);
    out->def_chars  = out->details ? (int)strlen(GLSP_HDR_DEF) : 0;
    out->source_chars = out->details ? (int)strlen(GLSP_HDR_SOURCE) : 0;
    for (i = 0; i < report->count; i++) {
        const ReplGlStateReportRow *row = &report->rows[i];
        if ((int)strlen(row->name) > out->name_chars)
            out->name_chars = (int)strlen(row->name);
        if ((int)strlen(row->current) > out->cur_chars)
            out->cur_chars = (int)strlen(row->current);
        if (!out->details)
            continue;
        if ((int)strlen(row->default_value) > out->def_chars)
            out->def_chars = (int)strlen(row->default_value);
        glsp_source_text(row, source_text, (int)sizeof(source_text));
        if ((int)strlen(source_text) > out->source_chars)
            out->source_chars = (int)strlen(source_text);
    }
    out->name_chars = glsp_clamp_chars(out->name_chars,
                                       (int)strlen(GLSP_HDR_NAME),
                                       GLSP_NAME_CHARS_MAX);
    out->cur_chars  = glsp_clamp_chars(out->cur_chars, cur_floor,
                                       GLSP_VAL_CHARS_MAX);
    if (out->details) {
        out->def_chars  = glsp_clamp_chars(out->def_chars,
                                           (int)strlen(GLSP_HDR_DEF),
                                           GLSP_VAL_CHARS_MAX);
        out->source_chars = glsp_clamp_chars(out->source_chars,
                                             (int)strlen(GLSP_HDR_SOURCE),
                                             GLSP_SOURCE_CHARS_MAX);
    }

    table_chars = out->name_chars + out->cur_chars + GLSP_COL_GAP_CHARS;
    if (out->details)
        table_chars += out->def_chars + out->source_chars +
                       2 * GLSP_COL_GAP_CHARS;
    max_table_chars = (view->window_w - 2 * GLSP_EDGE_MARGIN -
                       2 * GLSP_PAD_X) / FONT_W;
    /* Preserve all four headers on narrow windows and share remaining width
     * across the potentially long state/value columns. */
    while (table_chars > max_table_chars) {
        if (out->cur_chars > cur_floor &&
            out->cur_chars >= out->def_chars)
            out->cur_chars--;
        else if (out->def_chars > (int)strlen(GLSP_HDR_DEF))
            out->def_chars--;
        else if (out->name_chars > (int)strlen(GLSP_HDR_NAME))
            out->name_chars--;
        else if (out->source_chars > (int)strlen(GLSP_HDR_SOURCE))
            out->source_chars--;
        else
            break;
        table_chars--;
    }
    if (report->count == 0 && (int)strlen(GLSP_EMPTY_MSG) > table_chars)
        table_chars = (int)strlen(GLSP_EMPTY_MSG);
    if ((int)strlen(GLSP_TITLE) > table_chars)
        table_chars = (int)strlen(GLSP_TITLE);
    out->popup_w = table_chars * FONT_W + 2 * GLSP_PAD_X;
    if (out->popup_w > view->window_w - 2 * GLSP_EDGE_MARGIN)
        out->popup_w = view->window_w - 2 * GLSP_EDGE_MARGIN;

    /* Title + column-header rows (the empty message reuses the header
     * slot), then as many report rows as fit between the menu-bar band
     * and the bottom edge; the rest scroll. */
    chrome_rows = 2;
    row_capacity = (top_limit - GLSP_EDGE_MARGIN - 2 * GLSP_PAD_Y - 2)
                   / LINE_H - chrome_rows;
    if (row_capacity < 1)
        row_capacity = 1;
    out->visible_rows = report->count < row_capacity ? report->count
                                                     : row_capacity;
    out->max_scroll = report->count - out->visible_rows;
    out->scroll = view->scroll_rows;
    if (out->scroll > out->max_scroll) out->scroll = out->max_scroll;
    if (out->scroll < 0) out->scroll = 0;

    out->popup_h = 2 * GLSP_PAD_Y +
                   (chrome_rows + (report->count == 0 ? 0
                                                      : out->visible_rows)) *
                       LINE_H + 2;

    /* Anchor beside the click, clamped fully inside the window (y-up;
     * py is the popup's top edge). */
    out->px = view->anchor_px + 12;
    out->py = view->anchor_py;
    if (out->px + out->popup_w > view->window_w - GLSP_EDGE_MARGIN)
        out->px = view->window_w - GLSP_EDGE_MARGIN - out->popup_w;
    if (out->px < GLSP_EDGE_MARGIN)
        out->px = GLSP_EDGE_MARGIN;
    if (out->py - out->popup_h < GLSP_EDGE_MARGIN)
        out->py = GLSP_EDGE_MARGIN + out->popup_h;
    if (out->py > top_limit)
        out->py = top_limit;

    out->col0_x = out->px + GLSP_PAD_X;
    out->col1_x = out->col0_x +
                  (out->name_chars + GLSP_COL_GAP_CHARS) * FONT_W;
    out->col2_x = out->col1_x +
                  (out->cur_chars + GLSP_COL_GAP_CHARS) * FONT_W;
    out->col3_x = out->col2_x +
                  (out->def_chars + GLSP_COL_GAP_CHARS) * FONT_W;

    /* Expand/collapse chip cell: expanded it is the "[-]"-prefixed
     * default header at col2; collapsed the "[+]" chip right-aligns to
     * the popup edge inside the current column's header cell (the
     * cur_floor above reserves that room past the "current" label). The
     * empty-report popup has no header row and thus no chip. */
    out->tog_x0 = out->tog_x1 = 0;
    out->tog_y0 = out->tog_y1 = 0;
    if (report->count > 0) {
        if (out->details) {
            int chip_chars = (int)strlen(GLSP_HDR_DEF);
            if (chip_chars > out->def_chars)
                chip_chars = out->def_chars;
            out->tog_x0 = out->col2_x;
            out->tog_x1 = out->col2_x + chip_chars * FONT_W;
        } else {
            out->tog_x1 = out->px + out->popup_w - GLSP_PAD_X;
            out->tog_x0 = out->tog_x1 -
                          (int)strlen(GLSP_HDR_DETAILS_CLOSED) * FONT_W;
        }
        out->tog_y1 = out->py - GLSP_PAD_Y - LINE_H;
        out->tog_y0 = out->tog_y1 - LINE_H;
    }
    return 1;
}

int ui_gl_state_panel_hit_test(const UiGlStatePanelView *view,
                               int mx, int my) {
    GlspLayout lo;
    int y_up;
    if (!glsp_solve(view, &lo))
        return 0;
    y_up = view->window_h - my;
    return mx >= lo.px && mx <= lo.px + lo.popup_w &&
           y_up <= lo.py && y_up >= lo.py - lo.popup_h;
}

int ui_gl_state_panel_hit_test_details_toggle(const UiGlStatePanelView *view,
                                              int mx, int my) {
    GlspLayout lo;
    int y_up;
    if (!glsp_solve(view, &lo))
        return 0;
    if (lo.tog_x1 <= lo.tog_x0)
        return 0;
    y_up = view->window_h - my;
    return mx >= lo.tog_x0 && mx <= lo.tog_x1 &&
           y_up >= lo.tog_y0 && y_up <= lo.tog_y1;
}

int ui_gl_state_panel_max_scroll(const UiGlStatePanelView *view) {
    GlspLayout lo;
    if (!glsp_solve(view, &lo))
        return 0;
    return lo.max_scroll;
}

void ui_gl_state_panel_render(const UiGlStatePanelView *view) {
    const ReplGlStateReport *report;
    GlspLayout lo;
    char clipped[REPL_GL_STATE_VALUE_MAX];
    char source_text[32];
    int ty, i;

    if (!glsp_solve(view, &lo))
        return;
    report = view->report;

    gl2d_begin(view->window_w, view->window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    gl2d_panel_frame((float)lo.px, (float)(lo.py - lo.popup_h),
                     (float)lo.popup_w, (float)lo.popup_h,
                     UI_TOK_RAISED, 0.96f, UI_TOK_BORDER, 0.85f);

    ty = lo.py - GLSP_PAD_Y - LINE_H + 5;
    ui_clr(UI_TOK_TEXT_PRIMARY);
    gl2d_draw_string((float)lo.col0_x, (float)ty, GLSP_TITLE, FONT_MONO);
    ty -= LINE_H;

    if (report->count == 0) {
        ui_clr(UI_TOK_TEXT_MUTED);
        gl2d_draw_string((float)lo.col0_x, (float)ty, GLSP_EMPTY_MSG,
                         FONT_MONO);
        glDisable(GL_BLEND);
        gl2d_end();
        return;
    }

    ui_clr(UI_TOK_TEXT_SECTION);
    gl2d_draw_string((float)lo.col0_x, (float)ty, GLSP_HDR_NAME, FONT_MONO);
    gl2d_draw_string((float)lo.col1_x, (float)ty, GLSP_HDR_CUR, FONT_MONO);
    if (lo.details) {
        glsp_clip(clipped, (int)sizeof(clipped), GLSP_HDR_DEF, lo.def_chars);
        gl2d_draw_string((float)lo.col2_x, (float)ty, clipped, FONT_MONO);
        gl2d_draw_string((float)lo.col3_x, (float)ty, GLSP_HDR_SOURCE,
                         FONT_MONO);
    } else {
        gl2d_draw_string((float)lo.tog_x0, (float)ty,
                         GLSP_HDR_DETAILS_CLOSED, FONT_MONO);
    }

    ui_clr(UI_TOK_DIVIDER);
    glRectf((float)(lo.px + 1), (float)(ty - 4),
            (float)(lo.px + lo.popup_w - 1), (float)(ty - 3));
    ty -= LINE_H;

    for (i = lo.scroll; i < lo.scroll + lo.visible_rows; i++) {
        const ReplGlStateReportRow *row = &report->rows[i];

        ui_clr(UI_TOK_TEXT_PRIMARY);
        glsp_clip(clipped, (int)sizeof(clipped), row->name, lo.name_chars);
        gl2d_draw_string((float)lo.col0_x, (float)ty, clipped, FONT_MONO);

        ui_clr(row->differs_from_default ? UI_TOK_STATUS_WARN
                                         : UI_TOK_STATUS_OK);
        glsp_clip(clipped, (int)sizeof(clipped), row->current, lo.cur_chars);
        gl2d_draw_string((float)lo.col1_x, (float)ty, clipped, FONT_MONO);

        if (lo.details) {
            ui_clr(UI_TOK_TEXT_MUTED);
            glsp_clip(clipped, (int)sizeof(clipped), row->default_value,
                      lo.def_chars);
            gl2d_draw_string((float)lo.col2_x, (float)ty, clipped, FONT_MONO);

            glsp_source_text(row, source_text, (int)sizeof(source_text));
            glsp_clip(clipped, (int)sizeof(clipped), source_text,
                      lo.source_chars);
            gl2d_draw_string((float)lo.col3_x, (float)ty, clipped, FONT_MONO);
        }

        ty -= LINE_H;
    }

    /* Overflow scrollbar hint: thin track + proportional thumb on the
     * popup's right inner edge, shown only when rows are hidden. Purely
     * a visual cue — the wheel does the scrolling (same convention as
     * the menu flyouts), so there is no hit region. */
    if (lo.max_scroll > 0) {
        float bx1       = (float)(lo.px + lo.popup_w - 2);
        float bx0       = bx1 - 3.0f;
        float track_top = (float)(lo.py - GLSP_PAD_Y - 2 * LINE_H);
        float track_bot = (float)(lo.py - lo.popup_h + GLSP_PAD_Y);
        float track_h   = track_top - track_bot;
        float thumb_h   = track_h * (float)lo.visible_rows /
                          (float)report->count;
        float max_off, thumb_top;
        if (thumb_h < 10.0f)   thumb_h = 10.0f;
        if (thumb_h > track_h) thumb_h = track_h;
        max_off   = track_h - thumb_h;
        thumb_top = track_top -
                    max_off * (float)lo.scroll / (float)lo.max_scroll;
        ui_clr_a(UI_TOK_DIVIDER, 0.9f);
        glRectf(bx0, track_bot, bx1, track_top);
        ui_clr_a(UI_TOK_TEXT_MUTED, 0.9f);
        glRectf(bx0, thumb_top - thumb_h, bx1, thumb_top);
    }

    glDisable(GL_BLEND);
    gl2d_end();
}
