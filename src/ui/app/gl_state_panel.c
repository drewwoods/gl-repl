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
#define GLSP_MATRIX_ROWS     4

static const char GLSP_TITLE[]      = "OpenGL state at this line";
static const char GLSP_HDR_NAME[]   = "state";
static const char GLSP_HDR_CUR[]    = "current";
static const char GLSP_HDR_DEF[]    = "[-] default (GL 2.1)";
static const char GLSP_HDR_SOURCE[] = "source";
static const char GLSP_HDR_DETAILS_CLOSED[] = "[+] default/source";
static const char GLSP_SETUP_CHIP_FMT_CLOSED[] = "[+] %d from setup";
static const char GLSP_SETUP_CHIP_FMT_OPEN[]   = "[-] %d from setup";
#define GLSP_SETUP_CHIP_MAX 32
static const char GLSP_MODELVIEW_NAME[] = "GL_MODELVIEW_MATRIX";
static const char GLSP_EMPTY_MSG[]  =
    "No init() or display() OpenGL state has been touched.";
static const char GLSP_NO_USER_MSG[] =
    "This program has not set any OpenGL state yet.";

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
    int setup;                /* generated-setup rows folded in */
    int setup_count;          /* generated rows behind the fold */
    int row_count;            /* report rows this fold actually shows */
    int stu_x0, stu_x1;       /* setup fold chip cell, on the title row */
    int stu_y0, stu_y1;       /* (zero-width when there is nothing to fold) */
    int visible_rows;         /* semantic report rows drawn */
    int viewport_lines;       /* visual value lines reserved in the frame */
    int total_lines;          /* visual value lines across the report */
    int max_scroll;
    int scroll;               /* view->scroll_rows clamped to [0, max] */
} GlspLayout;

/* `gutter_label` is the code panel's left-margin number for the row's source
 * line, or -1 when the controller could not resolve one. Quote that rather
 * than source_line_idx + 1: with code focus off the panel's margin counts the
 * derived-C chrome rows too, and a popup citing the document index sends the
 * reader to the wrong line. The fallback keeps the old numbering for the
 * cases with no code-panel row to name (headless tests, hand-built views). */
static void glsp_source_text(const ReplGlStateReportRow *row,
                             int gutter_label,
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
                     gutter_label >= 0 ? gutter_label
                                       : row->source.source_line_idx + 1);
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

/* The report keeps matrices as one semantic state row so touched/default
 * comparison and provenance remain attached to a single state variable. The
 * popup expands that row into four visual lines without changing the report
 * format used by non-UI consumers. */
static int glsp_row_visual_lines(const ReplGlStateReportRow *row) {
    return row && strcmp(row->name, GLSP_MODELVIEW_NAME) == 0
               ? GLSP_MATRIX_ROWS : 1;
}

static int glsp_gutter_label(const UiGlStatePanelView *view, int row_idx) {
    return view && view->source_gutter_labels
               ? view->source_gutter_labels[row_idx] : -1;
}

/* Copy one visual value line. Matrix report strings use "; " between their
 * four rows; continuation rows gain one leading cell so their values align
 * with the first row's opening '['. */
static void glsp_value_line(const ReplGlStateReportRow *row,
                            const char *value, int line,
                            char *buf, int buf_size) {
    const char *start, *end;
    size_t len, used = 0;
    int i;

    if (!buf || buf_size <= 0)
        return;
    buf[0] = '\0';
    if (!value)
        return;
    if (glsp_row_visual_lines(row) == 1) {
        snprintf(buf, (size_t)buf_size, "%s", value);
        return;
    }
    if (line < 0 || line >= GLSP_MATRIX_ROWS)
        return;

    start = value;
    for (i = 0; i < line; i++) {
        end = strstr(start, "; ");
        if (!end)
            return;
        start = end + 2;
    }
    end = strstr(start, "; ");
    len = end ? (size_t)(end - start) : strlen(start);
    if (line > 0 && used + 1 < (size_t)buf_size)
        buf[used++] = ' ';
    if (len > (size_t)buf_size - used - 1)
        len = (size_t)buf_size - used - 1;
    memcpy(buf + used, start, len);
    buf[used + len] = '\0';
}

static int glsp_value_max_chars(const ReplGlStateReportRow *row,
                                const char *value) {
    char line[REPL_GL_STATE_VALUE_MAX + 2];
    int count = glsp_row_visual_lines(row);
    int max_chars = 0;
    int i;
    for (i = 0; i < count; i++) {
        int chars;
        glsp_value_line(row, value, i, line, (int)sizeof(line));
        chars = (int)strlen(line);
        if (chars > max_chars)
            max_chars = chars;
    }
    return max_chars;
}

static int glsp_report_visual_lines(const ReplGlStateReport *report,
                                    int row_count) {
    int lines = 0;
    int i;
    for (i = 0; report && i < row_count; i++)
        lines += glsp_row_visual_lines(&report->rows[i]);
    return lines;
}

/* Chip label for the authorship fold, e.g. "[+] 34 from setup". The count is
 * the generated group's size either way, so the label does not jump around as
 * the fold opens and closes. */
static void glsp_setup_chip_text(const GlspLayout *lo, char *buf, int buf_size) {
    snprintf(buf, (size_t)buf_size,
             lo->setup ? GLSP_SETUP_CHIP_FMT_OPEN : GLSP_SETUP_CHIP_FMT_CLOSED,
             lo->setup_count);
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
    /* Authorship fold: the report is partitioned user-rows-first, so the
     * shown set is always the prefix rows[0, row_count) — no filtering, which
     * keeps scroll indices and hit-testing identical to the unfolded case. */
    out->setup = view->setup_expanded ? 1 : 0;
    out->setup_count = report->count - report->user_row_count;
    if (out->setup_count < 0)
        out->setup_count = 0;
    out->row_count = out->setup ? report->count : report->user_row_count;
    if (out->row_count > report->count) out->row_count = report->count;
    if (out->row_count < 0)             out->row_count = 0;

    out->details = view->details_expanded ? 1 : 0;
    cur_floor = (int)strlen(GLSP_HDR_CUR);
    if (!out->details)
        cur_floor += GLSP_COL_GAP_CHARS +
                     (int)strlen(GLSP_HDR_DETAILS_CLOSED);
    out->name_chars = (int)strlen(GLSP_HDR_NAME);
    out->cur_chars  = (int)strlen(GLSP_HDR_CUR);
    out->def_chars  = out->details ? (int)strlen(GLSP_HDR_DEF) : 0;
    out->source_chars = out->details ? (int)strlen(GLSP_HDR_SOURCE) : 0;
    for (i = 0; i < out->row_count; i++) {
        const ReplGlStateReportRow *row = &report->rows[i];
        if ((int)strlen(row->name) > out->name_chars)
            out->name_chars = (int)strlen(row->name);
        int value_chars = glsp_value_max_chars(row, row->current);
        if (value_chars > out->cur_chars)
            out->cur_chars = value_chars;
        if (!out->details)
            continue;
        value_chars = glsp_value_max_chars(row, row->default_value);
        if (value_chars > out->def_chars)
            out->def_chars = value_chars;
        glsp_source_text(row, glsp_gutter_label(view, i), source_text,
                         (int)sizeof(source_text));
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
    if (out->row_count == 0) {
        const char *msg = report->count == 0 ? GLSP_EMPTY_MSG
                                             : GLSP_NO_USER_MSG;
        if ((int)strlen(msg) > table_chars)
            table_chars = (int)strlen(msg);
    }
    {
        /* The title row carries the setup chip on its right, so it floors the
         * popup width at title + gap + chip rather than the title alone. */
        char chip[GLSP_SETUP_CHIP_MAX];
        int title_chars = (int)strlen(GLSP_TITLE);
        if (out->setup_count > 0) {
            glsp_setup_chip_text(out, chip, (int)sizeof(chip));
            title_chars += GLSP_COL_GAP_CHARS + (int)strlen(chip);
        }
        if (title_chars > table_chars)
            table_chars = title_chars;
    }
    out->popup_w = table_chars * FONT_W + 2 * GLSP_PAD_X;
    if (out->popup_w > view->window_w - 2 * GLSP_EDGE_MARGIN)
        out->popup_w = view->window_w - 2 * GLSP_EDGE_MARGIN;

    /* Title + column-header rows (the empty message reuses the header
     * slot), then as many visual value lines as fit between the menu-bar
     * band and the bottom edge. Scrolling remains indexed by semantic report
     * rows, so the four matrix lines always move and render together. */
    chrome_rows = 2;
    row_capacity = (top_limit - GLSP_EDGE_MARGIN - 2 * GLSP_PAD_Y - 2)
                   / LINE_H - chrome_rows;
    if (row_capacity < 1)
        row_capacity = 1;
    for (i = 0; i < out->row_count; i++) {
        int row_lines = glsp_row_visual_lines(&report->rows[i]);
        if (row_lines > row_capacity)
            row_capacity = row_lines;
    }
    out->total_lines = glsp_report_visual_lines(report, out->row_count);
    out->viewport_lines = out->total_lines < row_capacity
                              ? out->total_lines : row_capacity;

    /* The final scroll position is the earliest semantic row whose suffix
     * fits in the viewport. A single over-tall row is still kept whole. */
    out->max_scroll = 0;
    if (out->total_lines > row_capacity) {
        int lines = 0;
        for (i = out->row_count - 1; i >= 0; i--) {
            int row_lines = glsp_row_visual_lines(&report->rows[i]);
            if (lines > 0 && lines + row_lines > row_capacity) {
                out->max_scroll = i + 1;
                break;
            }
            lines += row_lines;
            if (lines >= row_capacity) {
                out->max_scroll = i;
                break;
            }
        }
    }
    out->scroll = view->scroll_rows;
    if (out->scroll > out->max_scroll) out->scroll = out->max_scroll;
    if (out->scroll < 0) out->scroll = 0;

    out->visible_rows = 0;
    if (out->row_count > 0) {
        int lines = 0;
        for (i = out->scroll; i < out->row_count; i++) {
            int row_lines = glsp_row_visual_lines(&report->rows[i]);
            if (out->visible_rows > 0 &&
                lines + row_lines > row_capacity)
                break;
            lines += row_lines;
            out->visible_rows++;
            if (lines >= row_capacity)
                break;
        }
    }

    out->popup_h = 2 * GLSP_PAD_Y +
                   (chrome_rows + (out->row_count == 0 ? 0
                                                       : out->viewport_lines)) *
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
    if (out->row_count > 0) {
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

    /* Setup fold chip: right-aligned on the title row, one level above the
     * details chip both visually and semantically — it picks which rows the
     * table holds, where the details chip picks which columns. It survives
     * row_count == 0, since that is exactly when the user needs it to get the
     * folded rows back. */
    out->stu_x0 = out->stu_x1 = 0;
    out->stu_y0 = out->stu_y1 = 0;
    if (out->setup_count > 0) {
        char chip[GLSP_SETUP_CHIP_MAX];
        glsp_setup_chip_text(out, chip, (int)sizeof(chip));
        out->stu_x1 = out->px + out->popup_w - GLSP_PAD_X;
        out->stu_x0 = out->stu_x1 - (int)strlen(chip) * FONT_W;
        out->stu_y1 = out->py - GLSP_PAD_Y;
        out->stu_y0 = out->stu_y1 - LINE_H;
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

int ui_gl_state_panel_hit_test_setup_toggle(const UiGlStatePanelView *view,
                                            int mx, int my) {
    GlspLayout lo;
    int y_up;
    if (!glsp_solve(view, &lo))
        return 0;
    if (lo.stu_x1 <= lo.stu_x0)
        return 0;
    y_up = view->window_h - my;
    return mx >= lo.stu_x0 && mx <= lo.stu_x1 &&
           y_up >= lo.stu_y0 && y_up <= lo.stu_y1;
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
    char value_line[REPL_GL_STATE_VALUE_MAX + 2];
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
    if (lo.setup_count > 0) {
        char chip[GLSP_SETUP_CHIP_MAX];
        glsp_setup_chip_text(&lo, chip, (int)sizeof(chip));
        ui_clr(lo.setup ? UI_TOK_TEXT_SECTION : UI_TOK_TEXT_MUTED);
        gl2d_draw_string((float)lo.stu_x0, (float)ty, chip, FONT_MONO);
    }
    ty -= LINE_H;

    if (lo.row_count == 0) {
        ui_clr(UI_TOK_TEXT_MUTED);
        gl2d_draw_string((float)lo.col0_x, (float)ty,
                         report->count == 0 ? GLSP_EMPTY_MSG
                                            : GLSP_NO_USER_MSG,
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
        int line_count = glsp_row_visual_lines(row);
        int line;

        for (line = 0; line < line_count; line++) {
            if (line == 0) {
                ui_clr(row->source.source_line_idx >= 0 ? UI_TOK_TEXT_PRIMARY
                                                        : UI_TOK_TEXT_MUTED);
                glsp_clip(clipped, (int)sizeof(clipped), row->name,
                          lo.name_chars);
                gl2d_draw_string((float)lo.col0_x, (float)ty, clipped,
                                 FONT_MONO);
            }

            /* Two tiers, because "differs from the GL 2.1 default" is a
             * near-constant for a generated-setup row — it is in the report
             * precisely because the harness wrote it, so highlighting the
             * whole group says nothing and drowns out the rows that matter.
             * The saturated accents are reserved for state this program
             * wrote; setup rows keep the touched/default distinction in the
             * muted palette. */
            if (row->source.source_line_idx >= 0)
                ui_clr(row->differs_from_default ? UI_TOK_STATUS_WARN
                                                 : UI_TOK_STATUS_OK);
            else
                ui_clr(row->differs_from_default ? UI_TOK_TEXT_MUTED
                                                 : UI_TOK_TEXT_SECTION);
            glsp_value_line(row, row->current, line, value_line,
                            (int)sizeof(value_line));
            glsp_clip(clipped, (int)sizeof(clipped), value_line,
                      lo.cur_chars);
            gl2d_draw_string((float)lo.col1_x, (float)ty, clipped, FONT_MONO);

            if (lo.details) {
                ui_clr(UI_TOK_TEXT_MUTED);
                glsp_value_line(row, row->default_value, line, value_line,
                                (int)sizeof(value_line));
                glsp_clip(clipped, (int)sizeof(clipped), value_line,
                          lo.def_chars);
                gl2d_draw_string((float)lo.col2_x, (float)ty, clipped,
                                 FONT_MONO);

                if (line == 0) {
                    glsp_source_text(row, glsp_gutter_label(view, i),
                                     source_text, (int)sizeof(source_text));
                    glsp_clip(clipped, (int)sizeof(clipped), source_text,
                              lo.source_chars);
                    gl2d_draw_string((float)lo.col3_x, (float)ty, clipped,
                                     FONT_MONO);
                }
            }

            ty -= LINE_H;
        }
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
        float thumb_h   = track_h * (float)lo.viewport_lines /
                          (float)lo.total_lines;
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
