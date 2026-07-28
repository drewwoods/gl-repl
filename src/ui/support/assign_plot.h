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
 * Y is *linear* and does not force a zero baseline. Assignment values are
 * unitless and often signed, and a variable oscillating between 100 and 101
 * has to show its shape rather than a flat line pinned to the top of a
 * 0..101 axis. (This is the opposite call from the section histogram next
 * door, whose durations span decades and so want log spacing.)
 *
 * Like its siblings in this directory the renderer is pure over a flat view
 * and links from {support, ui/support, ui/core} alone.
 */
#ifndef UI_ASSIGN_PLOT_H
#define UI_ASSIGN_PLOT_H

#include "subsystems/assign_plot/assign_plot.h"

/* Panel width. Matches the FPS and variable panels so the overlay column has
 * one consistent edge. */
#define ASSIGN_PLOT_PANEL_W 250

/* Longest target label the panel will store. The controller writes the row's
 * left-hand side here ("angle", "A[3]"); anything longer is ellipsized at
 * draw time by the shared fit_label helper. */
enum { UI_ASSIGN_PLOT_TITLE_MAX = 48 };

enum {
    UI_ASSIGN_PLOT_HIT_NONE  = -1,
    UI_ASSIGN_PLOT_HIT_CLOSE = -2,
    UI_ASSIGN_PLOT_HIT_RATE  = -3,
    UI_ASSIGN_PLOT_HIT_RESET = -4
};

/* Per-frame view. `title` is a caller-owned string (the controller rebuilds it
 * from the live source row each frame, so an edited row retitles itself);
 * `plot` is the capture side's own flat view. */
typedef struct {
    int window_w, window_h;
    int visible;
    int panel_x, panel_y;   /* resolved position, controller-baked */
    const char *title;
    AssignPlotView plot;
} UiAssignPlotPanelView;

void ui_assign_plot_panel_render(const UiAssignPlotPanelView *view);
int  ui_assign_plot_panel_width(void);
int  ui_assign_plot_panel_height(void);

/* Classify a click. `mx`/`my` are GLUT window coords (y down), as everywhere
 * else in the hit-test path. */
int  ui_assign_plot_panel_hit_test(const UiAssignPlotPanelView *view,
                                   int mx, int my);

/* Short label for a capture rate ("once", "1 Hz", "frame"). Public so tests
 * and the controller can name a rate without duplicating the table. */
const char *ui_assign_plot_rate_label(int rate);

#endif /* UI_ASSIGN_PLOT_H */
