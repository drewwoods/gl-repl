/*
 * ui_assign_plot.c - The assignment-value plot panel.
 *
 * Layout, top to bottom:
 *
 *   header   target label, right-aligned [x]
 *   controls [once|1 Hz|frame] rate chip, right-aligned [reset]
 *   plot     linear Y over a gutter of value labels, X = execs or captures
 *   x-axis   which of the two X axes this is, and how many executions
 *   stats    min / max on one row, mean / sd on the next
 *
 * Every control is mouse-only and lives in this panel: the capture rate is a
 * property of one plot, not a global setting, so it deliberately has no
 * keymap slot and no config key (and therefore never reaches an exported
 * @cfg header).
 */
#include "ui/support/assign_plot.h"

#include "ui/core/gl_2d.h"
#include "ui/core/theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define AP_HEADER_H      20
#define AP_CTRL_H        16
#define AP_PLOT_H        90
#define AP_PLOT_GUTTER   44   /* y labels are signed and may carry an exponent */
#define AP_XAXIS_H       13
#define AP_STATS_ROW_H   13
#define AP_STATS_ROWS     2
#define AP_BOTTOM_PAD     8
#define AP_PAD            8   /* left/right text inset */
/* Gap between a stat cell's right-aligned value and the next cell's key, so
 * "-0.8" and "max" do not read as one token. */
#define AP_STAT_GAP      14

#define AP_CLOSE_LABEL  "[x]"
#define AP_RESET_LABEL  "[reset]"

/* Target gridline count. The nice-step search picks the coarsest 1/2/5 step
 * that keeps the axis at or under this many intervals. */
#define AP_Y_TARGET_DIVS 4

/* Plot line color: fixed data-viz identity like the FPS series, not a theme
 * token — the trace has to stay legible and stay the same hue in every
 * scheme. */
static const float k_ap_trace[3] = { 0.45f, 0.85f, 1.00f };  /* cyan */

static const char *k_ap_rate_labels[ASSIGN_PLOT_RATE_COUNT] = {
    "once", "1 Hz", "frame"
};

const char *ui_assign_plot_rate_label(int rate) {
    if (rate < 0 || rate >= ASSIGN_PLOT_RATE_COUNT) return "?";
    return k_ap_rate_labels[rate];
}

int ui_assign_plot_panel_width(void) {
    return ASSIGN_PLOT_PANEL_W;
}

int ui_assign_plot_panel_height(void) {
    return AP_HEADER_H + AP_CTRL_H + AP_PLOT_H + AP_XAXIS_H
         + AP_STATS_ROWS * AP_STATS_ROW_H + AP_BOTTOM_PAD;
}

/* Pixel width of a fixed-width label plus the 2px the other panels leave
 * between a right-aligned control and the panel edge. */
static int ap_control_w(const char *label) {
    return (int)strlen(label) * FONT_SMALL_W + 2;
}

static void ap_rate_chip_text(char *buf, size_t buf_sz, int rate) {
    snprintf(buf, buf_sz, "[%s]", ui_assign_plot_rate_label(rate));
}

/* Ellipsize `src` into `buf` if it does not fit `max_px`. FONT_SMALL is
 * fixed-width, so the pixel budget is a character count. */
static const char *ap_fit_label(const char *src, int max_px,
                                char *buf, size_t buf_sz) {
    int max_chars = max_px / FONT_SMALL_W;
    size_t len;

    if (!src) return "";
    len = strlen(src);
    if (max_chars <= 0) return "";
    if (len <= (size_t)max_chars) return src;
    if ((size_t)max_chars >= buf_sz) max_chars = (int)buf_sz - 1;
    if (max_chars <= 3) {
        memcpy(buf, src, (size_t)max_chars);
        buf[max_chars] = '\0';
        return buf;
    }
    memcpy(buf, src, (size_t)(max_chars - 3));
    memcpy(buf + max_chars - 3, "...", 4);
    return buf;
}

/* Assignment values are unitless, so there is no us/ms vocabulary to fall back
 * on: %g picks fixed or exponential notation per magnitude, and 4 significant
 * digits is about what fits beside a label in a 250px panel. */
static void ap_fmt_value(char *buf, size_t buf_sz, double v) {
    snprintf(buf, buf_sz, "%.4g", v);
}

/* Shorter form for the axis gutter, which has ~5 characters of room. */
static void ap_fmt_tick(char *buf, size_t buf_sz, double v) {
    /* Print a clean "0" rather than %g's "-0" for a negative zero tick. */
    if (v == 0.0) v = 0.0;
    snprintf(buf, buf_sz, "%.3g", v);
}

/* Coarsest 1/2/5 x 10^k step that divides `span` into at most
 * AP_Y_TARGET_DIVS intervals. Returns 0 for a non-positive span. */
static double ap_nice_step(double span) {
    double raw, mag, norm;

    if (!(span > 0.0)) return 0.0;
    raw  = span / (double)AP_Y_TARGET_DIVS;
    mag  = pow(10.0, floor(log10(raw)));
    norm = raw / mag;
    if (norm <= 1.0) return 1.0 * mag;
    if (norm <= 2.0) return 2.0 * mag;
    if (norm <= 5.0) return 5.0 * mag;
    return 10.0 * mag;
}

/* Value range of the plotted columns, widened to a nice step on both ends.
 * Returns 0 when there is nothing to scale.
 *
 * A perfectly flat trace has zero span, which no step can divide; it gets an
 * artificial band around the value so the line lands mid-plot instead of on an
 * edge. The band is relative to the value's own magnitude (so a constant 1e6
 * does not get a +/-1 window that rounds to nothing) with an absolute floor
 * for a constant zero. */
static int ap_y_range(const AssignPlotColumn *cols, int count,
                      double *out_lo, double *out_hi, double *out_step) {
    double lo, hi, step, pad;

    if (!cols || count <= 0) return 0;

    lo = cols[0].lo;
    hi = cols[0].hi;
    for (int i = 1; i < count; i++) {
        if (cols[i].lo < lo) lo = cols[i].lo;
        if (cols[i].hi > hi) hi = cols[i].hi;
    }

    if (hi - lo <= 0.0) {
        double mag = fabs(hi);
        pad = (mag > 0.0) ? mag * 0.5 : 1.0;
        lo -= pad;
        hi += pad;
    }

    step = ap_nice_step(hi - lo);
    if (!(step > 0.0)) return 0;

    /* Snap outward to whole steps so every gridline lands on a round number.
     * Deliberately NOT snapped to zero: a variable living in [100, 101] must
     * keep its shape rather than collapse onto the top of a 0..101 axis. */
    *out_lo   = floor(lo / step) * step;
    *out_hi   = ceil(hi / step) * step;
    if (*out_hi <= *out_lo) *out_hi = *out_lo + step;
    *out_step = step;
    return 1;
}

/* X center of a column, in pixels. A single column sits mid-plot rather than
 * on the left edge. */
static float ap_col_x(int idx, int count, int plot_x, int plot_w) {
    if (count <= 1)
        return (float)plot_x + (float)plot_w * 0.5f;
    return (float)plot_x
         + (float)plot_w * ((float)idx / (float)(count - 1));
}

static float ap_value_y(double v, double lo, double hi,
                        int plot_y, int plot_h) {
    double frac = (hi > lo) ? (v - lo) / (hi - lo) : 0.5;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    return (float)plot_y + (float)(frac * (double)plot_h);
}

/* Envelope band: one quad per column spanning lo..hi. Columns whose lo == hi
 * (every column of a non-decimated trace) contribute nothing visible, which is
 * exactly right — the line strip below carries them. */
static void ap_draw_envelope(const AssignPlotColumn *cols, int count,
                             int plot_x, int plot_y, int plot_w, int plot_h,
                             double lo, double hi) {
    float half = (count > 1)
               ? (float)plot_w / (float)(count - 1) * 0.5f
               : (float)plot_w * 0.5f;
    if (half < 0.5f) half = 0.5f;

    glColor4f(k_ap_trace[0], k_ap_trace[1], k_ap_trace[2], 0.30f);
    glBegin(GL_QUADS);
    for (int i = 0; i < count; i++) {
        float cx, y0, y1;
        if (cols[i].hi <= cols[i].lo) continue;
        cx = ap_col_x(i, count, plot_x, plot_w);
        y0 = ap_value_y(cols[i].lo, lo, hi, plot_y, plot_h);
        y1 = ap_value_y(cols[i].hi, lo, hi, plot_y, plot_h);
        glVertex2f(cx - half, y0);
        glVertex2f(cx + half, y0);
        glVertex2f(cx + half, y1);
        glVertex2f(cx - half, y1);
    }
    glEnd();
}

static void ap_draw_trace(const AssignPlotColumn *cols, int count,
                          int plot_x, int plot_y, int plot_w, int plot_h,
                          double lo, double hi) {
    glColor4f(k_ap_trace[0], k_ap_trace[1], k_ap_trace[2], 0.95f);
    if (count == 1) {
        /* A lone sample has no strip to draw; mark it so the plot is not
         * mysteriously blank on the first capture of a once-per-frame row. */
        float cx = ap_col_x(0, 1, plot_x, plot_w);
        float cy = ap_value_y((cols[0].lo + cols[0].hi) * 0.5,
                              lo, hi, plot_y, plot_h);
        glBegin(GL_LINES);
        glVertex2f(cx - 2.0f, cy);
        glVertex2f(cx + 3.0f, cy);
        glVertex2f(cx, cy - 2.0f);
        glVertex2f(cx, cy + 3.0f);
        glEnd();
        return;
    }
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i < count; i++) {
        double mid = ((double)cols[i].lo + (double)cols[i].hi) * 0.5;
        glVertex2f(ap_col_x(i, count, plot_x, plot_w),
                   ap_value_y(mid, lo, hi, plot_y, plot_h));
    }
    glEnd();
}

/* One "key   value" pair inside a half-width stats cell. */
static void ap_draw_stat(int x, int y, int cell_w,
                         const char *key, const char *value) {
    int vw = (int)strlen(value) * FONT_SMALL_W;
    ui_clr(UI_TOK_TEXT_MUTED);
    gl2d_draw_string((float)x, (float)y, key, FONT_SMALL);
    ui_clr(UI_TOK_TEXT_PRIMARY);
    gl2d_draw_string((float)(x + cell_w - vw), (float)y, value, FONT_SMALL);
}

void ui_assign_plot_panel_render(const UiAssignPlotPanelView *view) {
    int panel_h, panel_x, panel_y, tx, ty;
    int plot_x, plot_y, plot_w, plot_h;
    int close_w, reset_w;
    int have_data;
    double lo = 0.0, hi = 0.0, step = 0.0;
    char fit_buf[UI_ASSIGN_PLOT_TITLE_MAX + 4];
    char chip[16];

    if (!view || !view->visible) return;
    if (view->window_w <= 0 || view->window_h <= 0) return;

    panel_h = ui_assign_plot_panel_height();
    panel_x = view->panel_x;
    panel_y = view->panel_y;

    gl2d_begin(view->window_w, view->window_h);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)panel_x, (float)panel_y,
                     (float)ASSIGN_PLOT_PANEL_W, (float)panel_h,
                     UI_TOK_SUNKEN, 0.91f, UI_TOK_BORDER, 0.85f);
    glDisable(GL_BLEND);

    tx = panel_x + AP_PAD;
    ty = panel_y + panel_h - AP_HEADER_H + 2;

    /* Header: target label, then the close control on the right. */
    close_w = ap_control_w(AP_CLOSE_LABEL);
    ui_clr(UI_TOK_TEXT_PRIMARY);
    gl2d_draw_string((float)tx, (float)ty,
                     ap_fit_label(view->title,
                                  ASSIGN_PLOT_PANEL_W - 2 * AP_PAD - close_w - 4,
                                  fit_buf, sizeof(fit_buf)),
                     FONT_SMALL);
    ui_clr(UI_TOK_TEXT_MUTED);
    gl2d_draw_string((float)(panel_x + ASSIGN_PLOT_PANEL_W - close_w),
                     (float)ty, AP_CLOSE_LABEL, FONT_SMALL);
    ty -= AP_CTRL_H;

    /* Control row: rate chip left, reset right. */
    ap_rate_chip_text(chip, sizeof(chip), view->plot.rate);
    reset_w = ap_control_w(AP_RESET_LABEL);
    ui_clr(UI_TOK_TEXT_SECTION);
    gl2d_draw_string((float)tx, (float)ty, chip, FONT_SMALL);
    ui_clr(UI_TOK_TEXT_MUTED);
    gl2d_draw_string((float)(panel_x + ASSIGN_PLOT_PANEL_W - reset_w),
                     (float)ty, AP_RESET_LABEL, FONT_SMALL);

    /* Plot rect: gutter on the left for value labels, x-axis + stats below. */
    plot_x = panel_x + AP_PLOT_GUTTER + 4;
    plot_w = ASSIGN_PLOT_PANEL_W - AP_PLOT_GUTTER - 12;
    plot_y = panel_y + AP_BOTTOM_PAD + AP_STATS_ROWS * AP_STATS_ROW_H
           + AP_XAXIS_H;
    plot_h = ty - plot_y - 4;
    if (plot_h < 20) plot_h = 20;

    have_data = ap_y_range(view->plot.cols, view->plot.col_count,
                           &lo, &hi, &step);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)plot_x, (float)plot_y,
                     (float)plot_w, (float)plot_h,
                     UI_TOK_SUNKEN, 0.4f, UI_TOK_BORDER, 0.6f);

    if (have_data) {
        /* Gridlines on whole steps; the zero line, when the axis crosses it,
         * gets a brighter rule because sign changes are usually the thing
         * being looked for. */
        for (double v = lo; v <= hi + step * 0.5; v += step) {
            float gy = ap_value_y(v, lo, hi, plot_y, plot_h);
            int is_zero = (fabs(v) < step * 1e-6);
            ui_clr_a(UI_TOK_DIVIDER, is_zero ? 0.65f : 0.30f);
            glBegin(GL_LINES);
            glVertex2f((float)plot_x,            gy);
            glVertex2f((float)(plot_x + plot_w), gy);
            glEnd();
        }

        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        ap_draw_envelope(view->plot.cols, view->plot.col_count,
                         plot_x, plot_y, plot_w, plot_h, lo, hi);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        ap_draw_trace(view->plot.cols, view->plot.col_count,
                      plot_x, plot_y, plot_w, plot_h, lo, hi);
    }
    glDisable(GL_BLEND);

    if (!have_data) {
        const char *empty = view->plot.captured && view->plot.exec_count == 0
                          ? "(not executed)"
                          : "(collecting)";
        int ew = (int)strlen(empty) * FONT_SMALL_W;
        ui_clr(UI_TOK_TEXT_PLACEHOLDER);
        gl2d_draw_string((float)plot_x + ((float)plot_w - (float)ew) * 0.5f,
                         (float)plot_y + (float)plot_h * 0.5f,
                         empty, FONT_SMALL);
    } else {
        /* Value labels right-aligned in the gutter, vertically centered on
         * their gridline — but clamped into the plot's own band. Without the
         * clamp the bottom label drops into the x-axis caption below and the
         * top one is clipped by the control row above; both extremes are
         * always drawn, since they are the two numbers worth reading. */
        ui_clr(UI_TOK_TEXT_MUTED);
        for (double v = lo; v <= hi + step * 0.5; v += step) {
            char lab[16];
            int lw, gy, label_y;
            ap_fmt_tick(lab, sizeof(lab), v);
            lw = (int)strlen(lab) * FONT_SMALL_W;
            gy = (int)ap_value_y(v, lo, hi, plot_y, plot_h);
            label_y = gy - 4;
            if (label_y < plot_y) label_y = plot_y;
            if (label_y > plot_y + plot_h - FONT_SMALL_H)
                label_y = plot_y + plot_h - FONT_SMALL_H;
            gl2d_draw_string((float)(panel_x + AP_PLOT_GUTTER + 2 - lw),
                             (float)label_y, lab, FONT_SMALL);
        }
    }

    /* X-axis caption on the left, sample count on the right, sharing one row.
     * The count is placed first and the caption is fitted into what is left:
     * both strings grow with the data (a six-figure n, a five-figure exec
     * count), and only the caption can afford to be ellipsized. */
    {
        char axis[64];
        char count_buf[24];
        char axis_fit[64];
        int  count_w, budget;
        int  row_y = plot_y - AP_XAXIS_H + 2;

        snprintf(count_buf, sizeof(count_buf), "n=%llu",
                 (unsigned long long)view->plot.stats.count);
        count_w = (int)strlen(count_buf) * FONT_SMALL_W;

        if (view->plot.x_mode == ASSIGN_PLOT_X_FRAME)
            snprintf(axis, sizeof(axis), "x: captures (1/frame)");
        else
            snprintf(axis, sizeof(axis), "x: exec # (%d/frame)",
                     view->plot.exec_count);

        budget = ASSIGN_PLOT_PANEL_W - 2 * AP_PAD - count_w - FONT_SMALL_W;
        ui_clr(UI_TOK_TEXT_PLACEHOLDER);
        gl2d_draw_string((float)tx, (float)row_y,
                         ap_fit_label(axis, budget, axis_fit, sizeof(axis_fit)),
                         FONT_SMALL);
        gl2d_draw_string((float)(panel_x + ASSIGN_PLOT_PANEL_W - AP_PAD - count_w),
                         (float)row_y, count_buf, FONT_SMALL);
    }

    /* Stats: min/max, then mean/sd. Fed from every captured value, not from
     * the columns, so decimation never moves these numbers. */
    {
        int cell_w = (ASSIGN_PLOT_PANEL_W - 2 * AP_PAD) / 2;
        int row_y  = panel_y + AP_BOTTOM_PAD + AP_STATS_ROW_H;
        char buf[24];

        if (view->plot.stats.count > 0) {
            ap_fmt_value(buf, sizeof(buf), view->plot.stats.min);
            ap_draw_stat(tx, row_y, cell_w - AP_STAT_GAP, "min", buf);
            ap_fmt_value(buf, sizeof(buf), view->plot.stats.max);
            ap_draw_stat(tx + cell_w, row_y, cell_w - AP_STAT_GAP, "max", buf);

            row_y -= AP_STATS_ROW_H;
            ap_fmt_value(buf, sizeof(buf), view->plot.stats.mean);
            ap_draw_stat(tx, row_y, cell_w - AP_STAT_GAP, "mean", buf);
            ap_fmt_value(buf, sizeof(buf), view->plot.stats.stddev);
            ap_draw_stat(tx + cell_w, row_y, cell_w - AP_STAT_GAP, "sd", buf);
        } else {
            ui_clr(UI_TOK_TEXT_PLACEHOLDER);
            gl2d_draw_string((float)tx, (float)row_y,
                             "no samples yet", FONT_SMALL);
        }

    }

    gl2d_end();
}

int ui_assign_plot_panel_hit_test(const UiAssignPlotPanelView *view,
                                  int mx, int my) {
    int panel_h, gl_y, close_w, reset_w, chip_w, ctrl_y;
    char chip[16];

    if (!view || !view->visible ||
        view->window_w <= 0 || view->window_h <= 0)
        return UI_ASSIGN_PLOT_HIT_NONE;

    panel_h = ui_assign_plot_panel_height();
    gl_y = view->window_h - my;
    if (mx < view->panel_x || mx >= view->panel_x + ASSIGN_PLOT_PANEL_W ||
        gl_y < view->panel_y || gl_y >= view->panel_y + panel_h)
        return UI_ASSIGN_PLOT_HIT_NONE;

    /* Header band: the close control owns its right end. */
    close_w = ap_control_w(AP_CLOSE_LABEL);
    if (gl_y >= view->panel_y + panel_h - AP_HEADER_H) {
        if (mx >= view->panel_x + ASSIGN_PLOT_PANEL_W - close_w)
            return UI_ASSIGN_PLOT_HIT_CLOSE;
        return UI_ASSIGN_PLOT_HIT_NONE;
    }

    /* Control band, one row below. Mirrors the render's baseline walk:
     * the header consumes AP_HEADER_H, the control row AP_CTRL_H. */
    ctrl_y = view->panel_y + panel_h - AP_HEADER_H - AP_CTRL_H;
    if (gl_y >= ctrl_y) {
        ap_rate_chip_text(chip, sizeof(chip), view->plot.rate);
        chip_w  = (int)strlen(chip) * FONT_SMALL_W;
        reset_w = ap_control_w(AP_RESET_LABEL);
        if (mx >= view->panel_x + AP_PAD &&
            mx < view->panel_x + AP_PAD + chip_w)
            return UI_ASSIGN_PLOT_HIT_RATE;
        if (mx >= view->panel_x + ASSIGN_PLOT_PANEL_W - reset_w)
            return UI_ASSIGN_PLOT_HIT_RESET;
    }
    return UI_ASSIGN_PLOT_HIT_NONE;
}
