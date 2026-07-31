/*
 * ui_assign_plot.c - The assignment-value plot panel.
 *
 * Layout, top to bottom:
 *
 *   header   target label, right-aligned [x]
 *   controls [once|1 Hz|frame] rate, [lin|log], [expand|shrink], then [reset]
 *   plot     Y over a gutter of value labels, X = execs or captures
 *   x-axis   which of the two X axes this is, and how many executions
 *   legend   one swatch + name per series (only when there is more than one)
 *   stats    min / max on one row, mean / sd on the next
 *
 * With several series the stats block describes *one* of them: the legend
 * entry under the pointer, or the primary series when the pointer is
 * elsewhere. Four sets of four numbers would not fit the collapsed panel, and
 * stacking them would make the common single-series case pay for the rare one.
 * The described series' legend entry is drawn lit so the block is never
 * ambiguous about whose numbers those are.
 *
 * Every control is mouse-only and lives in this panel: the capture rate, the Y
 * scale and the zoom are properties of one plot, not global settings, so they
 * deliberately have no keymap slot and no config key (and therefore never
 * reach an exported @cfg header).
 *
 * The control row is laid out once, by ap_ctrl_layout(), which both the
 * renderer and the hit-test consume — four chips whose widths change with
 * their own state (`[1 Hz]` vs `[frame]`, `[expand]` vs `[shrink]`) is more
 * mirrored arithmetic than two code paths can be trusted to keep in step.
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
/* Expanded zoom. Applied to the plot well and the panel width only: the
 * header, control, axis and stats rows are text at a fixed font size and gain
 * nothing from being spaced further apart. */
#define AP_EXPAND_SCALE   2
#define AP_PLOT_GUTTER   44   /* y labels are signed and may carry an exponent */
#define AP_XAXIS_H       13
#define AP_LEGEND_H      13
#define AP_LEGEND_SWATCH  7   /* square, vertically centered in the row */
#define AP_STATS_ROW_H   13
#define AP_STATS_ROWS     2
#define AP_BOTTOM_PAD     8
#define AP_PAD            8   /* left/right text inset */
/* Gap between a stat cell's right-aligned value and the next cell's key, so
 * "-0.8" and "max" do not read as one token. */
#define AP_STAT_GAP      14
/* Minimum gap between a stat's own key and its value. Below this the value is
 * re-formatted with fewer significant digits rather than run into the key. */
#define AP_STAT_MIN_GAP   8

#define AP_CLOSE_LABEL  "[x]"
#define AP_RESET_LABEL  "[reset]"
#define AP_LIN_LABEL    "[lin]"
#define AP_LOG_LABEL    "[log]"
#define AP_EXPAND_LABEL "[expand]"
#define AP_SHRINK_LABEL "[shrink]"

/* Gap between adjacent left-aligned chips in the control row. */
#define AP_CHIP_GAP      6

/* Target gridline count. The nice-step search picks the coarsest 1/2/5 step
 * that keeps the axis at or under this many intervals. */
#define AP_Y_TARGET_DIVS 4

/* Plot line colors: fixed data-viz identity like the FPS series, not theme
 * tokens — a trace has to stay legible and stay the same hue in every scheme.
 * Series 0 keeps the cyan the single-series plot has always used; the rest are
 * spaced around it and chosen to stay distinguishable in the common forms of
 * color blindness (no red/green pair carries meaning on its own — the legend
 * names every series in text as well). */
static const float k_ap_series[MAX_ASSIGN_PLOT_SERIES][3] = {
    { 0.45f, 0.85f, 1.00f },  /* cyan   */
    { 1.00f, 0.72f, 0.30f },  /* amber  */
    { 0.72f, 0.62f, 1.00f },  /* violet */
    { 0.45f, 0.90f, 0.62f },  /* mint   */
};

void ui_assign_plot_series_color(int idx, float out_rgb[3]) {
    if (idx < 0) idx = 0;
    if (idx >= MAX_ASSIGN_PLOT_SERIES) idx = MAX_ASSIGN_PLOT_SERIES - 1;
    out_rgb[0] = k_ap_series[idx][0];
    out_rgb[1] = k_ap_series[idx][1];
    out_rgb[2] = k_ap_series[idx][2];
}

static const char *k_ap_rate_labels[ASSIGN_PLOT_RATE_COUNT] = {
    "once", "1 Hz", "frame"
};

const char *ui_assign_plot_rate_label(int rate) {
    if (rate < 0 || rate >= ASSIGN_PLOT_RATE_COUNT) return "?";
    return k_ap_rate_labels[rate];
}

/* Plot-well height for a zoom state; the panel height is this plus the fixed
 * text rows. Kept separate because the renderer sizes its plot rect from the
 * panel height it was given, and the two must agree. */
static int ap_plot_well_h(int expanded) {
    return expanded ? AP_PLOT_H * AP_EXPAND_SCALE : AP_PLOT_H;
}

/* The legend exists only to tell series apart, so one series does not get one
 * — its name is already the panel header. */
static int ap_legend_h(int series_count) {
    return series_count > 1 ? AP_LEGEND_H : 0;
}

/* --- legend geometry ---
 *
 * The legend band sits directly above the stats rows. Both the renderer and
 * the hit-test derive it from here, for the same reason the control row does:
 * the region that names a series must be the region that describes it. */
static int ap_legend_band_y(int panel_y) {
    return panel_y + AP_BOTTOM_PAD + AP_STATS_ROWS * AP_STATS_ROW_H;
}

/* Panel-relative x and width of legend entry `idx`. Entries split the row
 * evenly rather than sizing to their names: equal cells keep the swatches on a
 * predictable grid, and a name too long for its cell is ellipsized. */
static void ap_legend_cell(int panel_w, int series_count, int idx,
                           int *out_x, int *out_w) {
    int usable = panel_w - 2 * AP_PAD;
    int cell = (series_count > 0) ? usable / series_count : usable;
    *out_x = AP_PAD + idx * cell;
    *out_w = cell;
}

void ui_assign_plot_panel_size(int expanded, int series_count,
                               int *out_w, int *out_h) {
    if (out_w)
        *out_w = expanded ? ASSIGN_PLOT_PANEL_W * AP_EXPAND_SCALE
                          : ASSIGN_PLOT_PANEL_W;
    if (out_h)
        *out_h = AP_HEADER_H + AP_CTRL_H + ap_plot_well_h(expanded)
               + AP_XAXIS_H + ap_legend_h(series_count)
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

/* Resolved control-row geometry: each chip's label plus its left edge and
 * width, in panel-relative pixels. Built once per render and once per
 * hit-test from the same inputs, so the two can never disagree about where a
 * chip is. */
typedef struct {
    const char *label;
    int x;      /* panel-relative left edge */
    int w;
} ApChip;

typedef struct {
    char    rate_buf[16];
    ApChip  rate, yscale, expand, reset;
} ApCtrlRow;

static void ap_ctrl_layout(const UiAssignPlotPanelView *view, int panel_w,
                           ApCtrlRow *out) {
    int x = AP_PAD;

    ap_rate_chip_text(out->rate_buf, sizeof(out->rate_buf), view->plot.rate);
    out->rate.label = out->rate_buf;

    /* The Y-scale chip names the mode it is *in*, not the one it switches to
     * — same reading as the rate chip beside it. */
    out->yscale.label = view->plot.y_log ? AP_LOG_LABEL : AP_LIN_LABEL;
    /* The zoom chip names the action instead: there is no "you are small"
     * state worth reporting, only the move available from here. */
    out->expand.label = view->plot.expanded ? AP_SHRINK_LABEL
                                            : AP_EXPAND_LABEL;
    out->reset.label  = AP_RESET_LABEL;

    out->rate.x = x;
    out->rate.w = (int)strlen(out->rate.label) * FONT_SMALL_W;
    x += out->rate.w + AP_CHIP_GAP;

    out->yscale.x = x;
    out->yscale.w = (int)strlen(out->yscale.label) * FONT_SMALL_W;
    x += out->yscale.w + AP_CHIP_GAP;

    out->expand.x = x;
    out->expand.w = (int)strlen(out->expand.label) * FONT_SMALL_W;

    /* Reset stays pinned to the right edge in both sizes. */
    out->reset.w = ap_control_w(out->reset.label);
    out->reset.x = panel_w - out->reset.w;
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
 * digits is about what fits beside a label in a 250px panel.
 *
 * `max_chars` is what the cell actually has left after its key, and precision
 * is dropped until the number fits. An exponent eats characters a plain
 * decimal does not — the mean of a symmetric sine lands on something like
 * 1.027e-11 — and at 4 digits that runs straight into the key beside it.
 * Significant digits are the right thing to spend: 1e-11 still says "this is
 * zero, to rounding", which is the whole content of that number.
 *
 * If even one digit does not fit, the value is left too wide rather than
 * truncated: a clipped number reads as a different, wrong number. */
void ui_assign_plot_format_stat(char *buf, size_t buf_sz, double v,
                                int max_chars) {
    static const char *const k_precisions[] = { "%.4g", "%.3g", "%.2g", "%.1g" };

    if (!buf || buf_sz == 0) return;
    for (size_t i = 0; i < sizeof(k_precisions) / sizeof(k_precisions[0]); i++) {
        snprintf(buf, buf_sz, k_precisions[i], v);
        if (max_chars <= 0 || (int)strlen(buf) <= max_chars)
            return;
    }
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

/* The Y axis actually drawn. `lo`/`hi`/`step` live in *axis space*: value
 * units when linear, log10(value) when log — every consumer goes through
 * ap_value_y() / ap_tick_value(), so nothing else has to know which. */
typedef struct {
    int    log;        /* log space (a request that could be honored) */
    double lo, hi, step;
} ApYAxis;

/* Min/max across every series' plotted columns — the Y axis is shared, so the
 * extent has to be too. Columns a series has no value for are skipped rather
 * than read as zero. Returns 0 when there is nothing anywhere. */
static int ap_col_extent(const AssignPlotView *plot,
                         double *out_lo, double *out_hi) {
    double lo = 0.0, hi = 0.0;
    int found = 0;

    if (!plot) return 0;
    for (int s = 0; s < plot->series_count; s++) {
        const AssignPlotColumn *cols = plot->series[s].cols;
        int count = plot->series[s].col_count;
        if (!cols) continue;
        for (int i = 0; i < count; i++) {
            if (!cols[i].valid) continue;
            if (!found || cols[i].lo < lo) lo = cols[i].lo;
            if (!found || cols[i].hi > hi) hi = cols[i].hi;
            found = 1;
        }
    }
    if (!found) return 0;
    *out_lo = lo;
    *out_hi = hi;
    return 1;
}

int ui_assign_plot_y_log_available(const UiAssignPlotPanelView *view) {
    double lo, hi;
    if (!view) return 0;
    if (!ap_col_extent(&view->plot, &lo, &hi))
        return 0;
    /* The *minimum* is the test: one zero or negative sample anywhere in the
     * trace has no position on a log axis, and dropping it would silently
     * redraw a different dataset than the one captured. */
    return lo > 0.0;
}

/* Value range of the plotted columns, widened to a nice step on both ends.
 * Returns 0 when there is nothing to scale.
 *
 * A perfectly flat trace has zero span, which no step can divide; it gets an
 * artificial band around the value so the line lands mid-plot instead of on an
 * edge. The band is relative to the value's own magnitude (so a constant 1e6
 * does not get a +/-1 window that rounds to nothing) with an absolute floor
 * for a constant zero. */
static int ap_y_axis_linear(double lo, double hi, ApYAxis *out) {
    double step, pad;

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
    out->log  = 0;
    out->lo   = floor(lo / step) * step;
    out->hi   = ceil(hi / step) * step;
    if (out->hi <= out->lo) out->hi = out->lo + step;
    out->step = step;
    return 1;
}

/* Log axis over a strictly-positive range: whole decades, so every gridline
 * label is a power of ten. A range inside one decade still gets that decade's
 * full span rather than a fractional window — the point of asking for log is
 * to see magnitude, and a 2..3 trace stretched over its own tiny log slice
 * would read exactly like the linear plot it replaced. */
static int ap_y_axis_log(double lo, double hi, ApYAxis *out) {
    double lo_e = floor(log10(lo));
    double hi_e = ceil(log10(hi));
    double decades;

    if (hi_e <= lo_e) hi_e = lo_e + 1.0;
    decades = hi_e - lo_e;

    out->log  = 1;
    out->lo   = lo_e;
    out->hi   = hi_e;
    /* One gridline per decade until there are more decades than the target
     * division count, then every other / every fifth, so the gutter never
     * fills with labels. */
    out->step = ceil(decades / (double)AP_Y_TARGET_DIVS);
    if (!(out->step > 0.0)) out->step = 1.0;
    return 1;
}

/* Resolve the axis for this frame's columns. `want_log` is the chip; it is
 * honored only when every value is positive (see the header). */
static int ap_y_axis(const AssignPlotView *plot, int want_log, ApYAxis *out) {
    double lo, hi;
    if (!ap_col_extent(plot, &lo, &hi)) return 0;
    if (want_log && lo > 0.0) return ap_y_axis_log(lo, hi, out);
    return ap_y_axis_linear(lo, hi, out);
}

/* Axis-space position of a value: identity when linear, log10 when log.
 * Callers hand in raw values throughout. */
static double ap_axis_pos(const ApYAxis *ax, double v) {
    if (!ax->log) return v;
    /* Guarded for completeness: a log axis only exists over positive data, so
     * this floor is unreachable through ap_y_axis(). */
    return (v > 0.0) ? log10(v) : ax->lo;
}

/* Value a gridline at axis-space `p` stands for — what the gutter labels. */
static double ap_tick_value(const ApYAxis *ax, double p) {
    return ax->log ? pow(10.0, p) : p;
}

/* X center of a column, in pixels. A single column sits mid-plot rather than
 * on the left edge. */
static float ap_col_x(int idx, int count, int plot_x, int plot_w) {
    if (count <= 1)
        return (float)plot_x + (float)plot_w * 0.5f;
    return (float)plot_x
         + (float)plot_w * ((float)idx / (float)(count - 1));
}

static float ap_value_y(const ApYAxis *ax, double v, int plot_y, int plot_h) {
    double p = ap_axis_pos(ax, v);
    double frac = (ax->hi > ax->lo) ? (p - ax->lo) / (ax->hi - ax->lo) : 0.5;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    return (float)plot_y + (float)(frac * (double)plot_h);
}

/* Envelope band: one quad per column spanning lo..hi. Columns whose lo == hi
 * (every column of a non-decimated trace) contribute nothing visible, which is
 * exactly right — the line strip below carries them. */
static void ap_draw_envelope(const AssignPlotColumn *cols, int count,
                             int plot_x, int plot_y, int plot_w, int plot_h,
                             const ApYAxis *ax, const float rgb[3]) {
    float half = (count > 1)
               ? (float)plot_w / (float)(count - 1) * 0.5f
               : (float)plot_w * 0.5f;
    if (half < 0.5f) half = 0.5f;

    glColor4f(rgb[0], rgb[1], rgb[2], 0.30f);
    glBegin(GL_QUADS);
    for (int i = 0; i < count; i++) {
        float cx, y0, y1;
        if (!cols[i].valid) continue;
        if (cols[i].hi <= cols[i].lo) continue;
        cx = ap_col_x(i, count, plot_x, plot_w);
        y0 = ap_value_y(ax, cols[i].lo, plot_y, plot_h);
        y1 = ap_value_y(ax, cols[i].hi, plot_y, plot_h);
        glVertex2f(cx - half, y0);
        glVertex2f(cx + half, y0);
        glVertex2f(cx + half, y1);
        glVertex2f(cx - half, y1);
    }
    glEnd();
}

/* Vertical center of a column's envelope, in pixels. Averaged after the axis
 * mapping rather than before it, so on a log axis the trace sits in the
 * visual middle of its band instead of being dragged toward the top by an
 * arithmetic mean. */
static float ap_col_mid_y(const ApYAxis *ax, const AssignPlotColumn *col,
                          int plot_y, int plot_h) {
    return (ap_value_y(ax, col->lo, plot_y, plot_h)
          + ap_value_y(ax, col->hi, plot_y, plot_h)) * 0.5f;
}

/* A lone sample has no strip to draw; mark it so the plot is not mysteriously
 * blank on the first capture of a once-per-frame row. Also used for an
 * isolated value between two gaps, which has no neighbour to connect to. */
static void ap_draw_point_marker(float cx, float cy) {
    glBegin(GL_LINES);
    glVertex2f(cx - 2.0f, cy);
    glVertex2f(cx + 3.0f, cy);
    glVertex2f(cx, cy - 2.0f);
    glVertex2f(cx, cy + 3.0f);
    glEnd();
}

/* One series' line. Runs of valid columns are drawn as separate strips, so a
 * capture the series had no value for leaves a visible gap instead of a line
 * interpolated across data that does not exist. */
static void ap_draw_trace(const AssignPlotColumn *cols, int count,
                          int plot_x, int plot_y, int plot_w, int plot_h,
                          const ApYAxis *ax, const float rgb[3]) {
    int run_start = -1;

    glColor4f(rgb[0], rgb[1], rgb[2], 0.95f);

    for (int i = 0; i <= count; i++) {
        int valid = (i < count) && cols[i].valid;
        if (valid) {
            if (run_start < 0) run_start = i;
            continue;
        }
        if (run_start < 0) continue;

        if (i - run_start == 1) {
            ap_draw_point_marker(
                ap_col_x(run_start, count, plot_x, plot_w),
                ap_col_mid_y(ax, &cols[run_start], plot_y, plot_h));
        } else {
            glBegin(GL_LINE_STRIP);
            for (int j = run_start; j < i; j++)
                glVertex2f(ap_col_x(j, count, plot_x, plot_w),
                           ap_col_mid_y(ax, &cols[j], plot_y, plot_h));
            glEnd();
        }
        run_start = -1;
    }
}

/* One "key   value" pair inside a half-width stats cell: key left, value right,
 * formatted to whatever room the key leaves it. */
static void ap_draw_stat(int x, int y, int cell_w,
                         const char *key, double value) {
    char buf[32];
    int key_w = (int)strlen(key) * FONT_SMALL_W;
    int vw;

    ui_assign_plot_format_stat(buf, sizeof(buf), value,
                               (cell_w - key_w - AP_STAT_MIN_GAP) / FONT_SMALL_W);
    vw = (int)strlen(buf) * FONT_SMALL_W;

    ui_clr(UI_TOK_TEXT_MUTED);
    gl2d_draw_string((float)x, (float)y, key, FONT_SMALL);
    ui_clr(UI_TOK_TEXT_PRIMARY);
    gl2d_draw_string((float)(x + cell_w - vw), (float)y, buf, FONT_SMALL);
}

void ui_assign_plot_panel_render(const UiAssignPlotPanelView *view) {
    int panel_w, panel_h, panel_x, panel_y, tx, ty;
    int plot_x, plot_y, plot_w, plot_h;
    int close_w;
    int have_data, log_ok, series_count, focus;
    ApYAxis ax;
    ApCtrlRow ctrl;
    char fit_buf[UI_ASSIGN_PLOT_TITLE_MAX + 4];

    if (!view || !view->visible) return;
    if (view->window_w <= 0 || view->window_h <= 0) return;

    memset(&ax, 0, sizeof(ax));
    series_count = view->plot.series_count;
    if (series_count > MAX_ASSIGN_PLOT_SERIES)
        series_count = MAX_ASSIGN_PLOT_SERIES;
    /* Which series the stats block describes: the hovered legend entry, else
     * the primary. Resolved through the hit test so the described entry and
     * the lit one are the same rectangle by construction. */
    focus = ui_assign_plot_panel_hit_test(view, view->pointer_x,
                                          view->pointer_y);
    if (focus < 0 || focus >= series_count) focus = 0;
    ui_assign_plot_panel_size(view->plot.expanded, series_count,
                              &panel_w, &panel_h);
    panel_x = view->panel_x;
    panel_y = view->panel_y;

    gl2d_begin(view->window_w, view->window_h);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)panel_x, (float)panel_y,
                     (float)panel_w, (float)panel_h,
                     UI_TOK_SUNKEN, 0.91f, UI_TOK_BORDER, 0.85f);
    glDisable(GL_BLEND);

    tx = panel_x + AP_PAD;
    ty = panel_y + panel_h - AP_HEADER_H + 2;

    /* Header: the primary series' label — with more than one series the
     * legend below names them all, so the header says how many rather than
     * pretending the first one is the whole plot. */
    close_w = ap_control_w(AP_CLOSE_LABEL);
    ui_clr(UI_TOK_TEXT_PRIMARY);
    {
        char header[UI_ASSIGN_PLOT_TITLE_MAX + 16];
        const char *primary = (series_count > 0 && view->titles[0])
                            ? view->titles[0] : "";
        if (series_count > 1)
            snprintf(header, sizeof(header), "%s +%d", primary,
                     series_count - 1);
        else
            snprintf(header, sizeof(header), "%s", primary);
        gl2d_draw_string((float)tx, (float)ty,
                         ap_fit_label(header,
                                      panel_w - 2 * AP_PAD - close_w - 4,
                                      fit_buf, sizeof(fit_buf)),
                         FONT_SMALL);
    }
    ui_clr(UI_TOK_TEXT_MUTED);
    gl2d_draw_string((float)(panel_x + panel_w - close_w),
                     (float)ty, AP_CLOSE_LABEL, FONT_SMALL);
    ty -= AP_CTRL_H;

    /* Control row: rate / Y-scale / zoom chips from the left, reset right. */
    log_ok = ui_assign_plot_y_log_available(view);
    ap_ctrl_layout(view, panel_w, &ctrl);
    ui_clr(UI_TOK_TEXT_SECTION);
    gl2d_draw_string((float)(panel_x + ctrl.rate.x), (float)ty,
                     ctrl.rate.label, FONT_SMALL);
    /* A log request the data cannot support draws inert, so the axis being
     * linear anyway is visibly accounted for rather than looking like a dead
     * chip. */
    ui_clr(view->plot.y_log && !log_ok ? UI_TOK_TEXT_PLACEHOLDER
                                       : UI_TOK_TEXT_SECTION);
    gl2d_draw_string((float)(panel_x + ctrl.yscale.x), (float)ty,
                     ctrl.yscale.label, FONT_SMALL);
    ui_clr(UI_TOK_TEXT_MUTED);
    gl2d_draw_string((float)(panel_x + ctrl.expand.x), (float)ty,
                     ctrl.expand.label, FONT_SMALL);
    gl2d_draw_string((float)(panel_x + ctrl.reset.x), (float)ty,
                     ctrl.reset.label, FONT_SMALL);

    /* Plot rect: gutter on the left for value labels, x-axis + stats below. */
    plot_x = panel_x + AP_PLOT_GUTTER + 4;
    plot_w = panel_w - AP_PLOT_GUTTER - 12;
    plot_y = ap_legend_band_y(panel_y) + ap_legend_h(series_count)
           + AP_XAXIS_H;
    plot_h = ty - plot_y - 4;
    if (plot_h < 20) plot_h = 20;

    have_data = ap_y_axis(&view->plot, view->plot.y_log, &ax);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl2d_panel_frame((float)plot_x, (float)plot_y,
                     (float)plot_w, (float)plot_h,
                     UI_TOK_SUNKEN, 0.4f, UI_TOK_BORDER, 0.6f);

    if (have_data) {
        /* Gridlines on whole steps; the zero line, when the axis crosses it,
         * gets a brighter rule because sign changes are usually the thing
         * being looked for. A log axis is strictly positive and so never has
         * one — the test simply never fires there. */
        for (double p = ax.lo; p <= ax.hi + ax.step * 0.5; p += ax.step) {
            double v = ap_tick_value(&ax, p);
            float gy = ap_value_y(&ax, v, plot_y, plot_h);
            int is_zero = !ax.log && (fabs(p) < ax.step * 1e-6);
            ui_clr_a(UI_TOK_DIVIDER, is_zero ? 0.65f : 0.30f);
            glBegin(GL_LINES);
            glVertex2f((float)plot_x,            gy);
            glVertex2f((float)(plot_x + plot_w), gy);
            glEnd();
        }

        /* Envelopes for every series first, then every line: additive bands
         * under all the traces rather than each series burying the one drawn
         * before it. */
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        for (int s = 0; s < series_count; s++) {
            float rgb[3];
            ui_assign_plot_series_color(s, rgb);
            ap_draw_envelope(view->plot.series[s].cols,
                             view->plot.series[s].col_count,
                             plot_x, plot_y, plot_w, plot_h, &ax, rgb);
        }
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (int s = 0; s < series_count; s++) {
            float rgb[3];
            ui_assign_plot_series_color(s, rgb);
            ap_draw_trace(view->plot.series[s].cols,
                          view->plot.series[s].col_count,
                          plot_x, plot_y, plot_w, plot_h, &ax, rgb);
        }
    }
    glDisable(GL_BLEND);

    if (!have_data) {
        /* "not executed" only when nothing ran anywhere: with several series
         * one silent row is not an empty plot. */
        int any_execs = 0;
        const char *empty;
        for (int s = 0; s < series_count; s++)
            if (view->plot.series[s].exec_count > 0) any_execs = 1;
        empty = (view->plot.captured && !any_execs) ? "(not executed)"
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
        for (double p = ax.lo; p <= ax.hi + ax.step * 0.5; p += ax.step) {
            double v = ap_tick_value(&ax, p);
            char lab[16];
            int lw, gy, label_y;
            ap_fmt_tick(lab, sizeof(lab), v);
            lw = (int)strlen(lab) * FONT_SMALL_W;
            gy = (int)ap_value_y(&ax, v, plot_y, plot_h);
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

        /* Both numbers belong to the focused series, matching the stats block
         * below — n and the execution count are per-row facts, and a sum
         * across series would describe nothing that exists. */
        snprintf(count_buf, sizeof(count_buf), "n=%llu",
                 (unsigned long long)view->plot.series[focus].stats.count);
        count_w = (int)strlen(count_buf) * FONT_SMALL_W;

        if (view->plot.x_mode == ASSIGN_PLOT_X_FRAME)
            snprintf(axis, sizeof(axis), "x: captures (1/frame)");
        else if (series_count > 1)
            /* Series may run different numbers of times, so X is each
             * series' own progress rather than a shared index. */
            snprintf(axis, sizeof(axis), "x: exec %% (%d/frame)",
                     view->plot.series[focus].exec_count);
        else
            snprintf(axis, sizeof(axis), "x: exec # (%d/frame)",
                     view->plot.series[focus].exec_count);

        budget = panel_w - 2 * AP_PAD - count_w - FONT_SMALL_W;
        ui_clr(UI_TOK_TEXT_PLACEHOLDER);
        gl2d_draw_string((float)tx, (float)row_y,
                         ap_fit_label(axis, budget, axis_fit, sizeof(axis_fit)),
                         FONT_SMALL);
        gl2d_draw_string((float)(panel_x + panel_w - AP_PAD - count_w),
                         (float)row_y, count_buf, FONT_SMALL);
    }

    /* Legend: one swatch + name per series, only when there is more than one.
     * The focused entry — the one the stats below describe — draws its name in
     * primary text; the others are muted. */
    if (series_count > 1) {
        int row_y = ap_legend_band_y(panel_y) + 2;
        for (int s = 0; s < series_count; s++) {
            float rgb[3];
            int cell_x, cell_w, text_x;
            char name_buf[UI_ASSIGN_PLOT_TITLE_MAX + 4];

            ap_legend_cell(panel_w, series_count, s, &cell_x, &cell_w);
            ui_assign_plot_series_color(s, rgb);

            glColor4f(rgb[0], rgb[1], rgb[2], s == focus ? 1.0f : 0.55f);
            glRectf((float)(panel_x + cell_x),
                    (float)(row_y + 1),
                    (float)(panel_x + cell_x + AP_LEGEND_SWATCH),
                    (float)(row_y + 1 + AP_LEGEND_SWATCH));

            text_x = cell_x + AP_LEGEND_SWATCH + 3;
            ui_clr(s == focus ? UI_TOK_TEXT_PRIMARY : UI_TOK_TEXT_MUTED);
            gl2d_draw_string(
                (float)(panel_x + text_x), (float)row_y,
                ap_fit_label(view->titles[s] ? view->titles[s] : "",
                             cell_w - (AP_LEGEND_SWATCH + 3) - 4,
                             name_buf, sizeof(name_buf)),
                FONT_SMALL);
        }
    }

    /* Stats: min/max, then mean/sd, for the focused series. Fed from every
     * captured value, not from the columns, so decimation never moves these
     * numbers. */
    {
        const RunStatsSummary *st = &view->plot.series[focus].stats;
        /* Half the panel, but never wider than the collapsed panel's cell:
         * these are four short numbers, and letting them spread across an
         * expanded panel puts the key and its value at opposite ends of a
         * 250px gap. */
        int cell_w = (panel_w - 2 * AP_PAD) / 2;
        int cell_cap = (ASSIGN_PLOT_PANEL_W - 2 * AP_PAD) / 2;
        if (cell_w > cell_cap) cell_w = cell_cap;
        int row_y  = panel_y + AP_BOTTOM_PAD + AP_STATS_ROW_H;

        if (st->count > 0) {
            ap_draw_stat(tx, row_y, cell_w - AP_STAT_GAP, "min", st->min);
            ap_draw_stat(tx + cell_w, row_y, cell_w - AP_STAT_GAP,
                         "max", st->max);

            row_y -= AP_STATS_ROW_H;
            ap_draw_stat(tx, row_y, cell_w - AP_STAT_GAP, "mean", st->mean);
            ap_draw_stat(tx + cell_w, row_y, cell_w - AP_STAT_GAP,
                         "sd", st->stddev);
        } else {
            ui_clr(UI_TOK_TEXT_PLACEHOLDER);
            gl2d_draw_string((float)tx, (float)row_y,
                             "no samples yet", FONT_SMALL);
        }

    }

    gl2d_end();
}

/* Is `local_x` (panel-relative) inside this chip? */
static int ap_chip_hit(const ApChip *chip, int local_x) {
    return local_x >= chip->x && local_x < chip->x + chip->w;
}

int ui_assign_plot_panel_hit_test(const UiAssignPlotPanelView *view,
                                  int mx, int my) {
    int panel_w, panel_h, gl_y, close_w, ctrl_y, local_x, series_count;
    ApCtrlRow ctrl;

    if (!view || !view->visible ||
        view->window_w <= 0 || view->window_h <= 0)
        return UI_ASSIGN_PLOT_HIT_NONE;

    series_count = view->plot.series_count;
    if (series_count > MAX_ASSIGN_PLOT_SERIES)
        series_count = MAX_ASSIGN_PLOT_SERIES;
    ui_assign_plot_panel_size(view->plot.expanded, series_count,
                              &panel_w, &panel_h);
    gl_y = view->window_h - my;
    if (mx < view->panel_x || mx >= view->panel_x + panel_w ||
        gl_y < view->panel_y || gl_y >= view->panel_y + panel_h)
        return UI_ASSIGN_PLOT_HIT_NONE;

    local_x = mx - view->panel_x;

    /* Header band: the close control owns its right end. */
    close_w = ap_control_w(AP_CLOSE_LABEL);
    if (gl_y >= view->panel_y + panel_h - AP_HEADER_H) {
        if (local_x >= panel_w - close_w)
            return UI_ASSIGN_PLOT_HIT_CLOSE;
        return UI_ASSIGN_PLOT_HIT_NONE;
    }

    /* Control band, one row below. The row's vertical extent still mirrors
     * the render's baseline walk (header AP_HEADER_H, controls AP_CTRL_H);
     * the horizontal extents come from the shared layout, so a chip that
     * changes width changes it in both passes at once. */
    ctrl_y = view->panel_y + panel_h - AP_HEADER_H - AP_CTRL_H;
    if (gl_y >= ctrl_y) {
        ap_ctrl_layout(view, panel_w, &ctrl);
        if (ap_chip_hit(&ctrl.rate, local_x))   return UI_ASSIGN_PLOT_HIT_RATE;
        if (ap_chip_hit(&ctrl.yscale, local_x)) return UI_ASSIGN_PLOT_HIT_YSCALE;
        if (ap_chip_hit(&ctrl.expand, local_x)) return UI_ASSIGN_PLOT_HIT_EXPAND;
        if (local_x >= ctrl.reset.x)            return UI_ASSIGN_PLOT_HIT_RESET;
        return UI_ASSIGN_PLOT_HIT_NONE;
    }

    /* Legend band: a non-negative result is the series index. Read-only —
     * the caller consumes it as inert chrome, and the renderer uses it to
     * decide whose statistics to show. */
    if (series_count > 1) {
        int band_y = ap_legend_band_y(view->panel_y);
        if (gl_y >= band_y && gl_y < band_y + AP_LEGEND_H) {
            for (int s = 0; s < series_count; s++) {
                int cell_x, cell_w;
                ap_legend_cell(panel_w, series_count, s, &cell_x, &cell_w);
                if (local_x >= cell_x && local_x < cell_x + cell_w)
                    return s;
            }
        }
    }
    return UI_ASSIGN_PLOT_HIT_NONE;
}
