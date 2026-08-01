/*
 * ui_assign_plot.h - The assignment-value plot panel.
 *
 * A floating panel (its own overlay-layout slot) graphing the values one
 * assignment row produced, with min / max / mean / stddev underneath. Opened
 * by right-clicking a `var = expr;` row in the code panel; the values come
 * from src/subsystems/assign_plot/, which reads them straight out of the flat
 * program.
 *
 * Two X axes, chosen by the capture side and reported in the view: the
 * execution index within one frame for a row inside a loop, or successive
 * captures for a row that runs once per frame. The renderer only relabels the
 * axis — the shape of the drawing is identical either way, because a column is
 * a min/max envelope in both modes and simply collapses to a point whenever
 * there is one value per column.
 *
 * Y defaults to *linear* and does not force a zero baseline. Assignment values
 * are unitless and often signed, and a variable oscillating between 100 and 101
 * has to show its shape rather than a flat line pinned to the top of a
 * 0..101 axis. (This is the opposite call from the section histogram next
 * door, whose durations span decades and so want log spacing.)
 *
 * The lin/log chip requests log10 spacing for the runs where that is the
 * right question — values spread over orders of magnitude. Which log axis that
 * means is decided by the data, not by the user:
 *
 *   Strictly-positive data gets a plain log10 axis: a real bottom end, decade
 *   gridlines, nothing clipped.
 *
 *   Data that crosses or touches zero — every sinusoid does — gets a
 *   *symmetric* one: magnitudes below a derived floor read as zero, and above
 *   it the axis is log10(|v| / floor) carrying the value's sign. Two sinusoids
 *   an order of magnitude apart then sit a decade apart at their peaks instead
 *   of one flattening against the baseline, and both still cross a real center
 *   line. What it costs is resolution near zero, which is the trade being
 *   asked for.
 *
 * The floor scales with the largest magnitude on the plot rather than being a
 * fixed constant, so the same shape of plot comes out whether the values live
 * at 1e0 or 1e-6, with an absolute cap so unit-scale data does not open more
 * decades than are worth reading.
 *
 * The one thing no log axis can describe is a trace pinned at exactly zero:
 * there the plot stays linear, the chip draws in the placeholder color, and
 * the controller says so — an unexpectedly linear axis is visibly explained
 * rather than silently ignored.
 *
 * The zoom chip (1x / 2x) doubles the panel's width and its plot well. Everything
 * else about the drawing is scale-independent, so both sizes run the same
 * code — only ui_assign_plot_panel_size() and the plot rect change.
 *
 * Like its siblings in this directory the renderer is pure over a flat view
 * and links from {support, ui/support, ui/core} alone.
 */
#ifndef UI_ASSIGN_PLOT_H
#define UI_ASSIGN_PLOT_H

#include <stddef.h>   /* size_t */

#include "subsystems/assign_plot/assign_plot.h"

/* Panel width. Matches the FPS and variable panels so the overlay column has
 * one consistent edge. Expanded, it is twice this — wide enough that the
 * overlay solver will usually spill it into its own column, which is the
 * point. */
#define ASSIGN_PLOT_PANEL_W 250

/* Longest target label the panel will store. The controller writes the row's
 * left-hand side here ("angle", "A[3]"); anything longer is ellipsized at
 * draw time by the shared fit_label helper. */
enum { UI_ASSIGN_PLOT_TITLE_MAX = 48 };

/* Hit-test results. Controls are negative; a non-negative result is a legend
 * entry's series index. */
enum {
    UI_ASSIGN_PLOT_HIT_NONE   = -1,
    UI_ASSIGN_PLOT_HIT_CLOSE  = -2,
    UI_ASSIGN_PLOT_HIT_RATE   = -3,
    UI_ASSIGN_PLOT_HIT_RESET  = -4,
    UI_ASSIGN_PLOT_HIT_YSCALE = -5,
    UI_ASSIGN_PLOT_HIT_EXPAND = -6
};

/* Per-frame view. `titles[i]` names series i — caller-owned strings, rebuilt
 * by the controller from the live source rows each frame, so an edited row
 * retitles itself. `plot` is the capture side's own flat view.
 *
 * pointer_x/pointer_y are the last known pointer position in GLUT window
 * coords (y down), or negative for "no pointer". With more than one series the
 * stats block reports whichever legend entry the pointer is over, falling back
 * to the primary series — the same rectangle the legend draws, resolved by the
 * hit test, so what is highlighted is what is being described. A view left
 * zero-initialized reports a pointer at the window's top-left corner, which is
 * outside any panel the overlay layout places. */
typedef struct {
    int window_w, window_h;
    int visible;
    int panel_x, panel_y;   /* resolved position, controller-baked */
    const char *titles[MAX_ASSIGN_PLOT_SERIES];
    int pointer_x, pointer_y;
    AssignPlotView plot;
} UiAssignPlotPanelView;

void ui_assign_plot_panel_render(const UiAssignPlotPanelView *view);

/* Panel size for a given zoom state and series count. One query for both axes
 * (the variable panel's ui_variable_panel_size() model) because `expanded`
 * moves both, and the overlay solver needs them together anyway.
 *
 * `series_count` matters because the legend row only exists with more than one
 * series: a single-series plot is named by its header and is exactly the panel
 * it always was. Either out-param may be NULL. */
void ui_assign_plot_panel_size(int expanded, int series_count,
                               int *out_w, int *out_h);

/* Fixed plot color for series `idx`, written as RGB into `out_rgb`. Data-viz
 * identity, not theme tokens: a series has to keep its hue in every scheme and
 * against both panel backgrounds. Out-of-range indices are clamped, so this is
 * always safe to call. */
void ui_assign_plot_series_color(int idx, float out_rgb[3]);

/* Classify a click. `mx`/`my` are GLUT window coords (y down), as everywhere
 * else in the hit-test path. */
int  ui_assign_plot_panel_hit_test(const UiAssignPlotPanelView *view,
                                   int mx, int my);

/* Short label for a capture rate ("once", "1 Hz", "frame"). Public so tests
 * and the controller can name a rate without duplicating the table. */
const char *ui_assign_plot_rate_label(int rate);

/* Format a statistic into `buf`, dropping significant digits until it is at
 * most `max_chars` wide (<= 0 means "no limit"). Public so a test can pin the
 * fit: the numbers that need it — a near-zero mean printing as 1.027e-11 —
 * only show up as an overlap on screen otherwise. */
void ui_assign_plot_format_stat(char *buf, size_t buf_sz, double v,
                                int max_chars);

/* Whether a log-Y request can actually be honored for `view`'s data — which
 * now means only that some value has a non-zero magnitude, since signed data
 * goes on the symmetric axis. Public so the controller can say so in the
 * status line when the chip is clicked, rather than leaving a chip that
 * visibly does nothing. */
int ui_assign_plot_y_log_available(const UiAssignPlotPanelView *view);

#endif /* UI_ASSIGN_PLOT_H */
