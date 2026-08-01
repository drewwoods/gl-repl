/*
 * src/ui/subsystems/buffer_viz_legend.c -- Stencil-buffer legend panel.
 *
 * Pure renderer over UiBufferVizLegendView (see the header for the
 * subsystem -> controller -> UI pull). Solves one small table — swatch,
 * value, pixel count — and parks it in the scene rect's top-left corner,
 * the one corner no other scene chrome claims (the status bar and replay
 * HUD sit along the bottom, the floating panel stack down the right
 * edge). Everything is fixed-width FONT_SMALL text, so column widths are
 * character counts rather than measured strings.
 */
#include "ui/subsystems/buffer_viz_legend.h"

#include <stdio.h>
#include <string.h>

#include "ui/core/gl_2d.h"
#include "ui/core/theme.h"

#define BVL_MARGIN      10   /* inset from the scene-rect corner */
#define BVL_PAD_X        9
#define BVL_PAD_Y        6
#define BVL_ROW_H       14
#define BVL_SWATCH_W     9
#define BVL_SWATCH_H     9
#define BVL_SWATCH_GAP   7
#define BVL_COL_GAP      8
#define BVL_RULE_H       5   /* vertical room a divider rule occupies */

/* Wide enough for the longest left-column string, "+<int> more" (16 chars at
 * INT_MAX), so the count can't be silently clipped — which is also what
 * -Wformat-truncation was pointing at. */
#define BVL_LEFT_MAX    20
#define BVL_RIGHT_MAX   16
/* Listed values + "+N more" + zero + total. */
#define BVL_LINE_MAX    (UI_BUFFER_VIZ_LEGEND_MAX_ROWS + 3)

typedef enum {
    BVL_LINE_VALUE = 0,   /* filled swatch in the overlay's colour */
    BVL_LINE_MORE,        /* "+N more", no swatch */
    BVL_LINE_ZERO,        /* background: hollow swatch, transparent in the viz */
    BVL_LINE_TOTAL        /* footer, under a rule */
} BvlLineKind;

typedef struct {
    BvlLineKind kind;
    unsigned char rgb[3];
    char left[BVL_LEFT_MAX];
    char right[BVL_RIGHT_MAX];
} BvlLine;

typedef struct {
    int px, py;             /* panel top-left, y-up window coords */
    int panel_w, panel_h;
    int left_col_px;        /* text column widths in pixels */
    int right_col_px;
    int line_count;
    BvlLine lines[BVL_LINE_MAX];
} BvlLayout;

static void bvl_push_line(BvlLayout *out, BvlLineKind kind,
                          const unsigned char *rgb,
                          const char *left, unsigned int count) {
    BvlLine *line;
    if (out->line_count >= BVL_LINE_MAX)
        return;
    line = &out->lines[out->line_count++];
    line->kind = kind;
    line->rgb[0] = rgb ? rgb[0] : 0;
    line->rgb[1] = rgb ? rgb[1] : 0;
    line->rgb[2] = rgb ? rgb[2] : 0;
    snprintf(line->left, sizeof line->left, "%s", left);
    snprintf(line->right, sizeof line->right, "%u", count);
}

/* Solve the table and its placement. Returns 0 when the panel would not
 * draw (invisible view, degenerate window/scene rect, or a scene too
 * small to hold the solved panel). Pure — no GL, no state reads. */
static int bvl_solve(const UiBufferVizLegendView *view, BvlLayout *out) {
    char label[BVL_LEFT_MAX];
    int left_chars = 0, right_chars = 0, title_chars;
    int content_w, i;

    memset(out, 0, sizeof(*out));
    if (!view || !view->visible)
        return 0;
    if (view->window_w <= 0 || view->window_h <= 0)
        return 0;
    if (view->scene_w <= 0 || view->scene_h <= 0)
        return 0;

    for (i = 0; i < view->row_count && i < UI_BUFFER_VIZ_LEGEND_MAX_ROWS; i++) {
        snprintf(label, sizeof label, "%d", view->rows[i].value);
        bvl_push_line(out, BVL_LINE_VALUE, view->rows[i].rgb, label,
                      view->rows[i].count);
    }
    if (view->hidden_rows > 0) {
        snprintf(label, sizeof label, "+%d more", view->hidden_rows);
        bvl_push_line(out, BVL_LINE_MORE, NULL, label, view->hidden_px);
    }
    /* The zero row is never dropped: "how much of the frame is still
     * background" is the question this panel most often answers, and an
     * empty mask shows up as nothing but zeros. */
    bvl_push_line(out, BVL_LINE_ZERO, NULL, "0", view->zero_px);
    bvl_push_line(out, BVL_LINE_TOTAL, NULL, "total", view->total_px);

    for (i = 0; i < out->line_count; i++) {
        int l = (int)strlen(out->lines[i].left);
        int r = (int)strlen(out->lines[i].right);
        if (l > left_chars) left_chars = l;
        if (r > right_chars) right_chars = r;
    }
    out->left_col_px = left_chars * FONT_SMALL_W;
    out->right_col_px = right_chars * FONT_SMALL_W;

    content_w = BVL_SWATCH_W + BVL_SWATCH_GAP + out->left_col_px +
                BVL_COL_GAP + out->right_col_px;
    title_chars = view->title ? (int)strlen(view->title) : 0;
    if (title_chars * FONT_SMALL_W > content_w)
        content_w = title_chars * FONT_SMALL_W;

    out->panel_w = content_w + 2 * BVL_PAD_X;
    /* Title row + one rule, the body lines, and a second rule above the
     * total (already counted as a line). */
    out->panel_h = 2 * BVL_PAD_Y + (out->line_count + 1) * BVL_ROW_H +
                   2 * BVL_RULE_H;

    if (out->panel_w > view->scene_w || out->panel_h > view->scene_h)
        return 0;

    out->px = view->scene_x + BVL_MARGIN;
    out->py = view->scene_y + view->scene_h - BVL_MARGIN;
    if (out->px + out->panel_w > view->scene_x + view->scene_w)
        out->px = view->scene_x + view->scene_w - out->panel_w;
    if (out->px < view->scene_x)
        out->px = view->scene_x;
    if (out->py - out->panel_h < view->scene_y)
        out->py = view->scene_y + out->panel_h;
    return 1;
}

void ui_buffer_viz_legend_size(const UiBufferVizLegendView *view,
                               int *w, int *h) {
    BvlLayout lo;
    int ok = bvl_solve(view, &lo);
    if (w) *w = ok ? lo.panel_w : 0;
    if (h) *h = ok ? lo.panel_h : 0;
}

static void bvl_draw_rule(const BvlLayout *lo, int y) {
    ui_clr(UI_TOK_DIVIDER);
    glRectf((float)(lo->px + 1), (float)y,
            (float)(lo->px + lo->panel_w - 1), (float)(y + 1));
}

static void bvl_draw_swatch(const BvlLine *line, int x, int y) {
    if (line->kind == BVL_LINE_VALUE) {
        glColor3f((float)line->rgb[0] / 255.0f,
                  (float)line->rgb[1] / 255.0f,
                  (float)line->rgb[2] / 255.0f);
        glRectf((float)x, (float)y,
                (float)(x + BVL_SWATCH_W), (float)(y + BVL_SWATCH_H));
        return;
    }
    if (line->kind != BVL_LINE_ZERO)
        return;
    /* Zero is transparent in every viz mode, so it gets the outline of a
     * swatch rather than a fill — the panel says "background", not "a
     * colour you should be looking for on screen". */
    ui_clr(UI_TOK_TEXT_MUTED);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)x + 0.5f,                     (float)y + 0.5f);
    glVertex2f((float)(x + BVL_SWATCH_W) - 0.5f,    (float)y + 0.5f);
    glVertex2f((float)(x + BVL_SWATCH_W) - 0.5f,
               (float)(y + BVL_SWATCH_H) - 0.5f);
    glVertex2f((float)x + 0.5f, (float)(y + BVL_SWATCH_H) - 0.5f);
    glEnd();
}

void ui_buffer_viz_legend_render(const UiBufferVizLegendView *view) {
    BvlLayout lo;
    int text_x, right_x, row_top, i;

    if (!bvl_solve(view, &lo))
        return;

    gl2d_begin(view->window_w, view->window_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)lo.px, (float)(lo.py - lo.panel_h),
                     (float)lo.panel_w, (float)lo.panel_h,
                     UI_TOK_RAISED, 0.92f, UI_TOK_BORDER, 0.88f);

    text_x = lo.px + BVL_PAD_X + BVL_SWATCH_W + BVL_SWATCH_GAP;
    right_x = lo.px + lo.panel_w - BVL_PAD_X;

    /* Rows are laid out top-down by their band's top edge; the baseline
     * sits BVL_ROW_H - 4 below it, which is also where a swatch's bottom
     * edge goes. */
    row_top = lo.py - BVL_PAD_Y;
    ui_clr(UI_TOK_TEXT_SECTION);
    gl2d_draw_string((float)(lo.px + BVL_PAD_X),
                     (float)(row_top - BVL_ROW_H + 4),
                     view->title ? view->title : "Stencil", FONT_SMALL);
    row_top -= BVL_ROW_H;
    bvl_draw_rule(&lo, row_top - 2);
    row_top -= BVL_RULE_H;

    for (i = 0; i < lo.line_count; i++) {
        const BvlLine *line = &lo.lines[i];
        int baseline, right_w;

        if (line->kind == BVL_LINE_TOTAL) {
            bvl_draw_rule(&lo, row_top - 2);
            row_top -= BVL_RULE_H;
        }
        baseline = row_top - BVL_ROW_H + 4;
        bvl_draw_swatch(line, lo.px + BVL_PAD_X, baseline);

        ui_clr(line->kind == BVL_LINE_VALUE ? UI_TOK_TEXT_PRIMARY
                                            : UI_TOK_TEXT_MUTED);
        gl2d_draw_string((float)text_x, (float)baseline, line->left,
                         FONT_SMALL);
        /* Counts are right-aligned against the panel's right pad so the
         * column reads as magnitudes at a glance. */
        right_w = (int)strlen(line->right) * FONT_SMALL_W;
        gl2d_draw_string((float)(right_x - right_w), (float)baseline,
                         line->right, FONT_SMALL);
        row_top -= BVL_ROW_H;
    }

    glDisable(GL_BLEND);
    gl2d_end();
}
