/*
 * tests/test_ui_assign_plot.c -- the assignment-plot panel renderer.
 *
 * Views are built by hand here rather than through the capture subsystem:
 * the renderer treats AssignPlotView as data and calls nothing back, and
 * keeping it out of the link is what proves that. Draw assertions count stub
 * GL calls; the hit-test assertions pin the three controls to the same
 * geometry the render walks.
 */
#ifdef GL_STUBS
#include "ui/support/assign_plot.h"
#include "support/test_harness.h"
#include <GL/gl_stub_counts.h>
#endif

#include <stdio.h>
#include <string.h>

#ifdef GL_STUBS
static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond)    TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, exp) TEST_ASSERT_INT(&g_harness, label, got, exp)
#define ASSERT_STR(label, got, exp) TEST_ASSERT_STR(&g_harness, label, got, exp)

enum { WIN_W = 1200, WIN_H = 800 };

static AssignPlotColumn g_cols[ASSIGN_PLOT_COLS];

/* A view with `count` columns carrying a ramp from lo to hi. */
static UiAssignPlotPanelView make_view(int count, float lo, float hi) {
    UiAssignPlotPanelView v;
    memset(&v, 0, sizeof(v));
    v.window_w = WIN_W;
    v.window_h = WIN_H;
    v.visible  = 1;
    v.panel_x  = 900;
    v.panel_y  = 200;
    v.title    = "angle";

    for (int i = 0; i < count && i < ASSIGN_PLOT_COLS; i++) {
        float f = (count > 1) ? (float)i / (float)(count - 1) : 0.0f;
        g_cols[i].lo = lo + (hi - lo) * f;
        g_cols[i].hi = g_cols[i].lo;
    }
    v.plot.open       = 1;
    v.plot.rate       = ASSIGN_PLOT_RATE_1HZ;
    v.plot.x_mode     = ASSIGN_PLOT_X_EXEC;
    v.plot.captured   = count > 0;
    v.plot.exec_count = count;
    v.plot.cols       = g_cols;
    v.plot.col_count  = count;
    v.plot.stats.count  = (unsigned long long)count;
    v.plot.stats.min    = lo;
    v.plot.stats.max    = hi;
    v.plot.stats.mean   = (lo + hi) * 0.5;
    v.plot.stats.stddev = 1.0;
    return v;
}

/* GLUT window coords (y down) for a point inside the panel, given an offset
 * measured from the panel's bottom-left in GL coords (y up). */
static void panel_point(const UiAssignPlotPanelView *v, int dx, int dy_from_bottom,
                        int *mx, int *my) {
    *mx = v->panel_x + dx;
    *my = v->window_h - (v->panel_y + dy_from_bottom);
}

static void test_metrics(void) {
    ASSERT_TRUE("width is positive", ui_assign_plot_panel_width() > 0);
    ASSERT_TRUE("height is positive", ui_assign_plot_panel_height() > 0);
    ASSERT_INT("width is the shared overlay-column width",
               ui_assign_plot_panel_width(), ASSIGN_PLOT_PANEL_W);
    /* Header + controls + plot + axis + two stat rows has to leave the plot
     * well room to exist. */
    ASSERT_TRUE("height leaves room for a plot",
                ui_assign_plot_panel_height() > 100);
}

static void test_rate_labels(void) {
    ASSERT_STR("once", ui_assign_plot_rate_label(ASSIGN_PLOT_RATE_ONCE), "once");
    ASSERT_STR("1 Hz", ui_assign_plot_rate_label(ASSIGN_PLOT_RATE_1HZ), "1 Hz");
    ASSERT_STR("frame", ui_assign_plot_rate_label(ASSIGN_PLOT_RATE_FRAME), "frame");
    ASSERT_STR("out of range is not a crash",
               ui_assign_plot_rate_label(99), "?");
    ASSERT_STR("negative is not a crash",
               ui_assign_plot_rate_label(-1), "?");
}

static void test_render_draws(void) {
    UiAssignPlotPanelView v = make_view(40, 0.0f, 10.0f);

    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("panel background drawn", gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("trace drawn", gl_stub_counts[GL_STUB_glVertex2f] > 0);
    ASSERT_TRUE("labels drawn", gl_stub_counts[GL_STUB_glRasterPos2f] > 0);
}

static void test_render_hidden_is_noop(void) {
    UiAssignPlotPanelView v = make_view(40, 0.0f, 10.0f);
    v.visible = 0;

    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_INT("invisible panel draws nothing",
               gl_stub_counts[GL_STUB_glRectf], 0);

    gl_stub_counts_reset();
    ui_assign_plot_panel_render(NULL);
    ASSERT_INT("NULL view draws nothing",
               gl_stub_counts[GL_STUB_glRectf], 0);
}

static void test_render_degenerate_window(void) {
    UiAssignPlotPanelView v = make_view(40, 0.0f, 10.0f);
    v.window_w = 0;
    v.window_h = 0;

    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_INT("degenerate window draws nothing",
               gl_stub_counts[GL_STUB_glRectf], 0);
}

/* The empty states still have to paint the frame, so the panel does not
 * vanish while it waits for its first capture. */
static void test_render_empty_states(void) {
    UiAssignPlotPanelView v = make_view(0, 0.0f, 0.0f);

    v.plot.captured = 0;
    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("pre-capture panel still frames itself",
                gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("and prints a placeholder",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0);

    v.plot.captured   = 1;
    v.plot.exec_count = 0;
    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("not-executed panel still frames itself",
                gl_stub_counts[GL_STUB_glRectf] > 0);
}

/* A flat trace has no range to divide; it must still draw rather than
 * dividing by zero or collapsing onto the plot floor. */
static void test_render_flat_trace(void) {
    UiAssignPlotPanelView v = make_view(20, 5.0f, 5.0f);

    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("constant value still plots",
                gl_stub_counts[GL_STUB_glVertex2f] > 0);

    /* Constant zero is the case with no magnitude to derive a band from. */
    v = make_view(20, 0.0f, 0.0f);
    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("constant zero still plots",
                gl_stub_counts[GL_STUB_glVertex2f] > 0);
}

/* One column has no line strip; the renderer marks it instead. */
static void test_render_single_sample(void) {
    UiAssignPlotPanelView v = make_view(1, 3.0f, 3.0f);

    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("a lone sample is still marked",
                gl_stub_counts[GL_STUB_glVertex2f] > 0);
}

static void test_render_decimated_envelope(void) {
    UiAssignPlotPanelView v = make_view(ASSIGN_PLOT_COLS, -50.0f, 50.0f);
    int line_only;

    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    line_only = gl_stub_counts[GL_STUB_glVertex2f];

    /* Give every column a real span: the envelope quads are extra geometry
     * on top of the same line strip. */
    for (int i = 0; i < ASSIGN_PLOT_COLS; i++)
        g_cols[i].hi = g_cols[i].lo + 5.0f;
    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("envelope columns add geometry",
                gl_stub_counts[GL_STUB_glVertex2f] > line_only);
}

/* Long titles must not overrun the close control. */
static void test_render_long_title(void) {
    UiAssignPlotPanelView v = make_view(10, 0.0f, 1.0f);
    v.title = "an_extremely_long_variable_name_that_cannot_possibly_fit";

    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("long title still renders", gl_stub_counts[GL_STUB_glRectf] > 0);

    v.title = NULL;
    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("NULL title still renders", gl_stub_counts[GL_STUB_glRectf] > 0);
}

/* --- hit test --- */

static void test_hit_outside_and_hidden(void) {
    UiAssignPlotPanelView v = make_view(10, 0.0f, 1.0f);
    int mx, my;

    panel_point(&v, -20, 10, &mx, &my);
    ASSERT_INT("left of the panel misses",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_NONE);

    panel_point(&v, ASSIGN_PLOT_PANEL_W + 5, 10, &mx, &my);
    ASSERT_INT("right of the panel misses",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_NONE);

    panel_point(&v, 10, ui_assign_plot_panel_height() + 5, &mx, &my);
    ASSERT_INT("above the panel misses",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_NONE);

    panel_point(&v, 10, -5, &mx, &my);
    ASSERT_INT("below the panel misses",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_NONE);

    v.visible = 0;
    panel_point(&v, 10, 10, &mx, &my);
    ASSERT_INT("hidden panel is not clickable",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_NONE);

    ASSERT_INT("NULL view is not clickable",
               ui_assign_plot_panel_hit_test(NULL, 0, 0),
               UI_ASSIGN_PLOT_HIT_NONE);
}

static void test_hit_close(void) {
    UiAssignPlotPanelView v = make_view(10, 0.0f, 1.0f);
    int panel_h = ui_assign_plot_panel_height();
    int mx, my;

    /* Header row, right edge. */
    panel_point(&v, ASSIGN_PLOT_PANEL_W - 6, panel_h - 8, &mx, &my);
    ASSERT_INT("header right edge closes",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_CLOSE);

    /* Header row, over the title: inert (dragging is not a thing here). */
    panel_point(&v, 20, panel_h - 8, &mx, &my);
    ASSERT_INT("header title is inert",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_NONE);
}

static void test_hit_rate_and_reset(void) {
    UiAssignPlotPanelView v = make_view(10, 0.0f, 1.0f);
    int panel_h = ui_assign_plot_panel_height();
    /* One row below the header: the control band. */
    int ctrl_dy = panel_h - 28;
    int mx, my;

    panel_point(&v, 12, ctrl_dy, &mx, &my);
    ASSERT_INT("left of the control row cycles the rate",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_RATE);

    panel_point(&v, ASSIGN_PLOT_PANEL_W - 6, ctrl_dy, &mx, &my);
    ASSERT_INT("right of the control row resets",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_RESET);

    /* Between the two controls: nothing. */
    panel_point(&v, ASSIGN_PLOT_PANEL_W / 2, ctrl_dy, &mx, &my);
    ASSERT_INT("the gap between controls is inert",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_NONE);
}

/* The rate chip is as wide as its label, which changes with the rate. The
 * hit test has to track that rather than assuming a fixed box. */
static void test_hit_rate_chip_tracks_label_width(void) {
    UiAssignPlotPanelView v = make_view(10, 0.0f, 1.0f);
    int panel_h = ui_assign_plot_panel_height();
    int ctrl_dy = panel_h - 28;
    int mx, my;

    /* "[frame]" is 7 chars; "[once]" is 6. Column 6 (0-based) is inside the
     * wider chip and outside the narrower one. */
    v.plot.rate = ASSIGN_PLOT_RATE_FRAME;
    panel_point(&v, 8 + 6 * 8 + 4, ctrl_dy, &mx, &my);
    ASSERT_INT("wide chip covers its seventh column",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_RATE);

    v.plot.rate = ASSIGN_PLOT_RATE_ONCE;
    ASSERT_INT("narrow chip does not",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_NONE);
}

/* The plot body itself is not a control; it must fall through so the caller
 * can consume it as inert overlay chrome. */
static void test_hit_plot_body_is_inert(void) {
    UiAssignPlotPanelView v = make_view(10, 0.0f, 1.0f);
    int mx, my;

    panel_point(&v, ASSIGN_PLOT_PANEL_W / 2,
                ui_assign_plot_panel_height() / 2, &mx, &my);
    ASSERT_INT("plot body is not a control",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_NONE);
}

int main(void) {
    printf("--- ui_assign_plot tests ---\n\n");
    test_metrics();
    test_rate_labels();
    test_render_draws();
    test_render_hidden_is_noop();
    test_render_degenerate_window();
    test_render_empty_states();
    test_render_flat_trace();
    test_render_single_sample();
    test_render_decimated_envelope();
    test_render_long_title();
    test_hit_outside_and_hidden();
    test_hit_close();
    test_hit_rate_and_reset();
    test_hit_rate_chip_tracks_label_width();
    test_hit_plot_body_is_inert();
    return test_harness_report(&g_harness, "test_ui_assign_plot");
}
#else
int main(void) {
    printf("test_ui_assign_plot: requires GL stubs (USE_GL_STUBS=1)\n");
    return 0;
}
#endif
