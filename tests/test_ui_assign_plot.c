/*
 * tests/test_ui_assign_plot.c -- the assignment-plot panel renderer.
 *
 * Views are built by hand here rather than through the capture subsystem:
 * the renderer treats AssignPlotView as data and calls nothing back, and
 * keeping it out of the link is what proves that. Draw assertions count stub
 * GL calls; the hit-test assertions pin the controls to the same geometry the
 * render walks.
 */
#ifdef GL_STUBS
#include "ui/support/assign_plot.h"
#include "ui/core/gl_2d.h"        /* gl2d_chip_state_w - the chips' own width */
#include "support/test_harness.h"
#include <GL/gl_stub_counts.h>
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef GL_STUBS
static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond)    TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, exp) TEST_ASSERT_INT(&g_harness, label, got, exp)
#define ASSERT_STR(label, got, exp) TEST_ASSERT_STR(&g_harness, label, got, exp)
#define ASSERT_FLOAT(label, got, exp) \
    TEST_ASSERT_FLOAT(&g_harness, label, got, exp, 1e-3f)

#define TRACE_PATH "build/test_ui_assign_plot_trace.txt"

enum { WIN_W = 1200, WIN_H = 800 };

static AssignPlotColumn g_cols[MAX_ASSIGN_PLOT_SERIES][ASSIGN_PLOT_COLS];

/* Fill series `s` with `count` columns carrying a ramp from lo to hi. */
static void set_series(UiAssignPlotPanelView *v, int s, const char *name,
                       int count, float lo, float hi) {
    for (int i = 0; i < count && i < ASSIGN_PLOT_COLS; i++) {
        float f = (count > 1) ? (float)i / (float)(count - 1) : 0.0f;
        g_cols[s][i].lo = lo + (hi - lo) * f;
        g_cols[s][i].hi = g_cols[s][i].lo;
        g_cols[s][i].valid = 1;
    }
    v->titles[s] = name;
    v->plot.series[s].exec_count = count;
    v->plot.series[s].cols       = g_cols[s];
    v->plot.series[s].col_count  = count;
    v->plot.series[s].stats.count  = (unsigned long long)count;
    v->plot.series[s].stats.min    = lo;
    v->plot.series[s].stats.max    = hi;
    v->plot.series[s].stats.mean   = (lo + hi) * 0.5;
    v->plot.series[s].stats.stddev = 1.0;
    if (s + 1 > v->plot.series_count) v->plot.series_count = s + 1;
}

/* A single-series view with `count` columns carrying a ramp from lo to hi. */
static UiAssignPlotPanelView make_view(int count, float lo, float hi) {
    UiAssignPlotPanelView v;
    memset(&v, 0, sizeof(v));
    v.window_w = WIN_W;
    v.window_h = WIN_H;
    v.visible  = 1;
    v.panel_x  = 900;
    v.panel_y  = 200;
    /* Off-panel: hover has to be opted into, so the stats block defaults to
     * the primary series in every test that does not care. */
    v.pointer_x = -1;
    v.pointer_y = -1;

    v.plot.open       = 1;
    v.plot.rate       = ASSIGN_PLOT_RATE_1HZ;
    v.plot.x_mode     = ASSIGN_PLOT_X_EXEC;
    v.plot.captured   = count > 0;
    v.plot.series_count = 1;
    set_series(&v, 0, "angle", count, lo, hi);
    return v;
}

/* Render one series and recover the Y coordinates of its line strip from the
 * GL-stub trace. Matching the series color and trace alpha excludes grid and
 * envelope vertices, so these values directly constrain the axis transform. */
static int render_series_trace_y(const UiAssignPlotPanelView *v, int series,
                                 float *out_y, int cap) {
    float rgb[3];
    FILE *trace;
    char line[128];
    int active = 0;
    int count = 0;

    ui_assign_plot_series_color(series, rgb);
    gl_stub_trace_open(TRACE_PATH);
    ui_assign_plot_panel_render(v);
    gl_stub_trace_close();

    trace = fopen(TRACE_PATH, "r");
    if (!trace) return 0;
    while (fgets(line, sizeof(line), trace)) {
        float r, g, b, a, x, y;
        if (sscanf(line, "glColor4f %f %f %f %f", &r, &g, &b, &a) == 4) {
            active = fabsf(r - rgb[0]) < 1e-4f
                  && fabsf(g - rgb[1]) < 1e-4f
                  && fabsf(b - rgb[2]) < 1e-4f
                  && fabsf(a - 0.95f) < 1e-4f;
        } else if (active
                   && sscanf(line, "glVertex2f %f %f", &x, &y) == 2) {
            if (count < cap) out_y[count] = y;
            count++;
        }
    }
    fclose(trace);
    return count;
}

/* Chip wells recovered from the trace. A state chip's well is the only rect
 * in the panel exactly GL2D_CHIP_H tall: the panel and plot frames are far
 * larger, the legend swatches are square and smaller. Counting the shape
 * rather than the call means an action chip that grew a background, or a
 * state chip that lost one, moves the number. */
typedef struct {
    int wells;
} ApChipShapes;

static ApChipShapes count_chip_shapes(const UiAssignPlotPanelView *v) {
    ApChipShapes out;
    FILE *trace;
    char line[128];

    out.wells = 0;
    gl_stub_trace_open(TRACE_PATH);
    ui_assign_plot_panel_render(v);
    gl_stub_trace_close();

    trace = fopen(TRACE_PATH, "r");
    if (!trace) return out;
    while (fgets(line, sizeof(line), trace)) {
        float x0, y0, x1, y1;
        if (sscanf(line, "glRectf %f %f %f %f", &x0, &y0, &x1, &y1) != 4)
            continue;
        if (fabsf((y1 - y0) - (float)GL2D_CHIP_H) < 0.5f)
            out.wells++;
    }
    fclose(trace);
    return out;
}

/* The panel's own size for the view's zoom state - every geometry assertion
 * below goes through these rather than the bare constant, so the expanded
 * cases exercise the same helpers as the collapsed ones. */
static int view_w(const UiAssignPlotPanelView *v) {
    int w;
    ui_assign_plot_panel_size(v->plot.expanded, v->plot.series_count, &w, NULL);
    return w;
}

static int view_h(const UiAssignPlotPanelView *v) {
    int h;
    ui_assign_plot_panel_size(v->plot.expanded, v->plot.series_count, NULL, &h);
    return h;
}

/* GLUT window coords (y down) for a point inside the panel, given an offset
 * measured from the panel's bottom-left in GL coords (y up). */
static void panel_point(const UiAssignPlotPanelView *v, int dx, int dy_from_bottom,
                        int *mx, int *my) {
    *mx = v->panel_x + dx;
    *my = v->window_h - (v->panel_y + dy_from_bottom);
}

static void test_metrics(void) {
    int w = 0, h = 0, xw = 0, xh = 0;

    ui_assign_plot_panel_size(0, 1, &w, &h);
    ASSERT_TRUE("width is positive", w > 0);
    ASSERT_TRUE("height is positive", h > 0);
    ASSERT_INT("width is the shared overlay-column width",
               w, ASSIGN_PLOT_PANEL_W);
    /* Header + controls + plot + axis + two stat rows has to leave the plot
     * well room to exist. */
    ASSERT_TRUE("height leaves room for a plot", h > 100);

    /* Expanded is strictly bigger on both axes - the overlay solver reserves
     * from this query, so a zoom that did not move it would overlap its
     * neighbours. */
    ui_assign_plot_panel_size(1, 1, &xw, &xh);
    ASSERT_INT("expanded doubles the width", xw, w * 2);
    ASSERT_TRUE("expanded grows the height", xh > h);
    /* Only the plot well scales: the fixed text rows must not be scaled with
     * it, or the expanded panel would be mostly padding. */
    ASSERT_TRUE("expanded height grows by less than double",
                xh < h * 2);

    /* Either out-param alone is legal. */
    w = -1;
    ui_assign_plot_panel_size(0, 1, &w, NULL);
    ASSERT_TRUE("width-only query works", w > 0);
    h = -1;
    ui_assign_plot_panel_size(0, 1, NULL, &h);
    ASSERT_TRUE("height-only query works", h > 0);
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
    v.plot.series[0].exec_count = 0;
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
        g_cols[0][i].hi = g_cols[0][i].lo + 5.0f;
    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("envelope columns add geometry",
                gl_stub_counts[GL_STUB_glVertex2f] > line_only);
}

/* Long titles must not overrun the close control. */
static void test_render_long_title(void) {
    UiAssignPlotPanelView v = make_view(10, 0.0f, 1.0f);
    v.titles[0] = "an_extremely_long_variable_name_that_cannot_possibly_fit";

    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("long title still renders", gl_stub_counts[GL_STUB_glRectf] > 0);

    v.titles[0] = NULL;
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

    panel_point(&v, view_w(&v) + 5, 10, &mx, &my);
    ASSERT_INT("right of the panel misses",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_NONE);

    panel_point(&v, 10, view_h(&v) + 5, &mx, &my);
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
    int panel_h = view_h(&v);
    int mx, my;

    /* Header row, right edge. */
    panel_point(&v, view_w(&v) - 6, panel_h - 8, &mx, &my);
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
    int panel_h = view_h(&v);
    /* One row below the header: the control band. */
    int ctrl_dy = panel_h - 28;
    int mx, my;

    panel_point(&v, 12, ctrl_dy, &mx, &my);
    ASSERT_INT("left of the control row cycles the rate",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_RATE);

    panel_point(&v, view_w(&v) - 6, ctrl_dy, &mx, &my);
    ASSERT_INT("right of the control row resets",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_RESET);

    /* Between the last left-aligned chip and the right-pinned [reset]:
     * nothing. Measured back from the right edge because that is the only
     * part of the row whose emptiness does not depend on how wide the chips
     * to its left happen to be. */
    panel_point(&v, view_w(&v) - 70, ctrl_dy, &mx, &my);
    ASSERT_INT("the gap before [reset] is inert",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_NONE);
}

/* The rate chip is padded to its widest label ("frame") so cycling the rate
 * doesn't shove the chips to its right back and forth. The hit test has to use
 * that same constant box: sweeping the control row must give byte-identical
 * results for every rate, and the chip has to still be hittable. */
static void test_hit_rate_chip_is_constant_width(void) {
    UiAssignPlotPanelView v = make_view(10, 0.0f, 1.0f);
    int panel_h = view_h(&v);
    int ctrl_dy = panel_h - 28;
    int mx, my;
    int drift = 0;

    for (int dx = 0; dx < ASSIGN_PLOT_PANEL_W; dx++) {
        panel_point(&v, dx, ctrl_dy, &mx, &my);
        v.plot.rate = ASSIGN_PLOT_RATE_FRAME;
        int wide = ui_assign_plot_panel_hit_test(&v, mx, my);
        for (int rate = 0; rate < ASSIGN_PLOT_RATE_COUNT; rate++) {
            v.plot.rate = rate;
            if (ui_assign_plot_panel_hit_test(&v, mx, my) != wide)
                drift++;
        }
    }
    ASSERT_INT("control row hit-tests the same at every rate", drift, 0);

    /* Anchor the sweep: a point inside the chip really is a rate hit, so the
     * agreement above can't be every column agreeing on "nothing here". */
    v.plot.rate = ASSIGN_PLOT_RATE_ONCE;
    panel_point(&v, 8 + 42, ctrl_dy, &mx, &my);
    ASSERT_INT("padded chip covers its last column",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_RATE);
}

/* The chip grammar (ui/core/gl_2d.h): a setting shows its value in a sunken
 * well, an action is bare bracketed text. Pinned here because the whole point
 * is that the two are told apart *visually*, so a future chip that reverts to
 * bare text - or wraps a verb in a well - has to fail something. */
static void test_chip_grammar(void) {
    UiAssignPlotPanelView v = make_view(10, 1.0f, 100.0f);
    ApChipShapes shapes;

    /* Every stateful chip labels itself with a value, never a verb. */
    ASSERT_TRUE("the rate chip shows a value",
                strchr(ui_assign_plot_rate_label(ASSIGN_PLOT_RATE_1HZ), '[')
                == NULL);

    shapes = count_chip_shapes(&v);
    /* Three settings (rate, Y scale, zoom) each get a well; the two actions
     * ([reset], [x]) get none. A well is a filled rect plus a border loop. */
    ASSERT_INT("one well per state chip", shapes.wells, 3);

    /* Toggling a setting keeps its well - the well marks the category, not
     * one particular value. */
    v.plot.y_log = 1;
    v.plot.expanded = 1;
    v.plot.rate = ASSIGN_PLOT_RATE_FRAME;
    shapes = count_chip_shapes(&v);
    ASSERT_INT("still one well per state chip", shapes.wells, 3);
}

/* The Y-scale and zoom chips sit between the rate chip and [reset]. Their
 * exact x is layout-derived, so the test walks the row and asserts it meets
 * each control once, in order, rather than hard-coding pixel columns. */
static void test_hit_yscale_and_expand(void) {
    UiAssignPlotPanelView v = make_view(10, 1.0f, 100.0f);
    int panel_h = view_h(&v);
    int ctrl_dy = panel_h - 28;
    int seen_rate = 0, seen_yscale = 0, seen_expand = 0, seen_reset = 0;
    int mx, my;

    for (int dx = 0; dx < view_w(&v); dx++) {
        panel_point(&v, dx, ctrl_dy, &mx, &my);
        switch (ui_assign_plot_panel_hit_test(&v, mx, my)) {
            case UI_ASSIGN_PLOT_HIT_RATE:   seen_rate++;   break;
            case UI_ASSIGN_PLOT_HIT_YSCALE: seen_yscale++; break;
            case UI_ASSIGN_PLOT_HIT_EXPAND: seen_expand++; break;
            case UI_ASSIGN_PLOT_HIT_RESET:  seen_reset++;  break;
            default: break;
        }
    }
    ASSERT_TRUE("the rate chip is reachable",    seen_rate > 0);
    ASSERT_TRUE("the Y-scale chip is reachable", seen_yscale > 0);
    ASSERT_TRUE("the zoom chip is reachable",    seen_expand > 0);
    ASSERT_TRUE("the reset control is reachable", seen_reset > 0);
    /* Each chip is exactly its label plus the state well's padding - a chip
     * that swallowed the whole row would still satisfy the assertions above.
     * Going through the shared width helper is the point: the hit region has
     * to be the well the renderer drew, not the text inside it. */
    ASSERT_INT("Y-scale chip is a \"lin\" well wide",
               seen_yscale, gl2d_chip_state_w("lin"));
    ASSERT_INT("zoom chip is a \"1x\" well wide",
               seen_expand, gl2d_chip_state_w("1x"));
}

/* Expanding moves every control, because the panel it is measured against
 * changed size. The [reset] control is the one pinned to the right edge, so
 * it is the one that must move. */
static void test_hit_tracks_expanded_geometry(void) {
    UiAssignPlotPanelView v = make_view(10, 1.0f, 100.0f);
    int mx, my;

    /* The collapsed panel's right edge is interior to the expanded one, so
     * this point is [reset] in one state and plot body in the other. */
    v.plot.expanded = 0;
    panel_point(&v, ASSIGN_PLOT_PANEL_W - 6, view_h(&v) - 28, &mx, &my);
    ASSERT_INT("collapsed: right edge resets",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_RESET);

    v.plot.expanded = 1;
    ASSERT_INT("expanded: the same point is no longer the right edge",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_NONE);

    panel_point(&v, view_w(&v) - 6, view_h(&v) - 28, &mx, &my);
    ASSERT_INT("expanded: the new right edge resets",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_RESET);

    /* And the expanded panel is clickable out to its wider bound. */
    panel_point(&v, ASSIGN_PLOT_PANEL_W + 20, view_h(&v) - 8, &mx, &my);
    ASSERT_INT("expanded: header extends past the collapsed width",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_NONE);
    panel_point(&v, view_w(&v) - 6, view_h(&v) - 8, &mx, &my);
    ASSERT_INT("expanded: close is at the new right edge",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_CLOSE);
}

/* Log Y is honored for anything with a magnitude - signed data lands on the
 * symmetric axis rather than being refused. The predicate is public because
 * the controller reports it; these are its edges. */
static void test_y_log_availability(void) {
    UiAssignPlotPanelView v = make_view(10, 1.0f, 1000.0f);
    ASSERT_INT("all-positive data can go log",
               ui_assign_plot_y_log_available(&v), 1);

    v = make_view(10, -5.0f, 5.0f);
    ASSERT_INT("a sign change goes symmetric rather than being refused",
               ui_assign_plot_y_log_available(&v), 1);

    v = make_view(10, 0.0f, 5.0f);
    ASSERT_INT("touching zero is fine too",
               ui_assign_plot_y_log_available(&v), 1);

    v = make_view(10, -5.0f, -1.0f);
    ASSERT_INT("all-negative data has magnitude",
               ui_assign_plot_y_log_available(&v), 1);

    /* The one real refusal: no magnitude anywhere to put on any decade. */
    v = make_view(10, 0.0f, 0.0f);
    ASSERT_INT("a trace pinned at zero cannot",
               ui_assign_plot_y_log_available(&v), 0);

    v = make_view(0, 0.0f, 0.0f);
    ASSERT_INT("no data cannot",
               ui_assign_plot_y_log_available(&v), 0);

    ASSERT_INT("NULL view cannot",
               ui_assign_plot_y_log_available(NULL), 0);
}

/* The case this exists for: a sinusoid, and two sinusoids of different
 * amplitude on one plot. Neither can use a positive-only log axis. */
static void test_render_symlog_axis(void) {
    UiAssignPlotPanelView v = make_view(3, -1.0f, 1.0f);
    float y[ASSIGN_PLOT_COLS];
    int n;

    /* -1, 0, 1 must retain sign and symmetry. A transform that collapses
     * everything to zero still emits the same number of GL calls, so assert
     * the actual trace coordinates. */
    v.plot.y_log = 1;
    n = render_series_trace_y(&v, 0, y, ASSIGN_PLOT_COLS);
    ASSERT_INT("signed trace has one vertex per sample", n, 3);
    ASSERT_TRUE("negative, zero and positive stay ordered",
                y[0] < y[1] && y[1] < y[2]);
    ASSERT_FLOAT("zero stays at the symmetric center",
                 y[1], (y[0] + y[2]) * 0.5f);

    /* Two amplitudes an order of magnitude apart: the small one must not
     * flatten onto the baseline, which is the whole reason for the mode. */
    {
        UiAssignPlotPanelView two = make_view(40, -100.0f, 100.0f);
        float large_y[ASSIGN_PLOT_COLS], small_y[ASSIGN_PLOT_COLS];
        int large_n, small_n;
        set_series(&two, 1, "small", 40, -1.0f, 1.0f);
        two.plot.y_log = 1;
        large_n = render_series_trace_y(&two, 0, large_y, ASSIGN_PLOT_COLS);
        small_n = render_series_trace_y(&two, 1, small_y, ASSIGN_PLOT_COLS);
        ASSERT_INT("large series trace is complete", large_n, 40);
        ASSERT_INT("small series trace is complete", small_n, 40);
        ASSERT_TRUE("small magnitude keeps a visible signed span",
                    small_y[0] < small_y[39]);
        ASSERT_TRUE("small trace nests inside the larger decades",
                    large_y[0] < small_y[0] && small_y[39] < large_y[39]);
    }

    /* Values near zero collapse into the floor band rather than diving for
     * however many decades the arithmetic happens to leave behind. */
    {
        UiAssignPlotPanelView tiny = make_view(4, -1.0f, 1.0f);
        float values[] = {-1.0f, 0.0f, 1.0e-11f, 1.0f};
        for (int i = 0; i < 4; i++)
            g_cols[0][i].lo = g_cols[0][i].hi = values[i];
        tiny.plot.y_log = 1;
        n = render_series_trace_y(&tiny, 0, y, ASSIGN_PLOT_COLS);
        ASSERT_INT("floor-band trace is complete", n, 4);
        ASSERT_FLOAT("a near-zero sample maps to zero", y[2], y[1]);
    }

    /* Small-magnitude data must not be swallowed by a fixed floor: the same
     * trace scaled down by 1e-6 has to plot the same way. */
    {
        UiAssignPlotPanelView unit, small;
        float unit_y[5], small_y[5];
        int unit_n, small_n;

        unit = make_view(5, -1.0f, 1.0f);
        unit.plot.y_log = 1;
        unit_n = render_series_trace_y(&unit, 0, unit_y, 5);

        small = make_view(5, -1.0e-6f, 1.0e-6f);
        small.plot.y_log = 1;
        small_n = render_series_trace_y(&small, 0, small_y, 5);
        ASSERT_INT("unit-scale trace is complete", unit_n, 5);
        ASSERT_INT("small-scale trace is complete", small_n, 5);
        for (int i = 0; i < 5; i++)
            ASSERT_FLOAT("relative floor is scale invariant",
                         small_y[i], unit_y[i]);
    }

    /* Entirely negative ranges use the symmetric axis too. Consecutive
     * negative decades must therefore be evenly spaced, unlike linear Y. */
    {
        UiAssignPlotPanelView negative = make_view(3, -100.0f, -1.0f);
        float values[] = {-100.0f, -10.0f, -1.0f};
        for (int i = 0; i < 3; i++)
            g_cols[0][i].lo = g_cols[0][i].hi = values[i];
        negative.plot.y_log = 1;
        n = render_series_trace_y(&negative, 0, y, ASSIGN_PLOT_COLS);
        ASSERT_INT("negative-only trace is complete", n, 3);
        ASSERT_TRUE("negative decades stay ordered",
                    y[0] < y[1] && y[1] < y[2]);
        ASSERT_FLOAT("negative decades are evenly spaced",
                     y[1] - y[0], y[2] - y[1]);
    }

    /* All-zero data has no decade in either direction: linear fallback. */
    {
        UiAssignPlotPanelView zero = make_view(40, 0.0f, 0.0f);
        zero.plot.y_log = 1;
        gl_stub_counts_reset();
        ui_assign_plot_panel_render(&zero);
        ASSERT_TRUE("an all-zero trace still frames itself",
                    gl_stub_counts[GL_STUB_glRectf] > 0);
    }
}

/* Log Y draws, and a log request over data that cannot support it silently
 * falls back to linear rather than emitting nothing (or NaN vertices). */
static void test_render_log_axis(void) {
    UiAssignPlotPanelView v = make_view(30, 0.01f, 1000.0f);
    int log_vertices, linear_vertices;

    v.plot.y_log = 0;
    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    linear_vertices = gl_stub_counts[GL_STUB_glVertex2f];

    v.plot.y_log = 1;
    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    log_vertices = gl_stub_counts[GL_STUB_glVertex2f];
    ASSERT_TRUE("log axis draws a trace", log_vertices > 0);
    /* Five decades: the log axis has its own gridline count, so the two are
     * not merely the same drawing relabeled. */
    ASSERT_TRUE("log axis is a different drawing",
                log_vertices != linear_vertices);

    /* Sign-crossing data uses symmetric log and still draws. */
    v = make_view(30, -10.0f, 10.0f);
    v.plot.y_log = 1;
    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("sign-crossing log request still plots",
                gl_stub_counts[GL_STUB_glVertex2f] > 0);

    /* Same for an empty plot, where there is no extent at all. */
    v = make_view(0, 0.0f, 0.0f);
    v.plot.y_log = 1;
    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("log request on an empty plot still frames itself",
                gl_stub_counts[GL_STUB_glRectf] > 0);
}

/* The expanded panel renders through the same path, just larger. */
static void test_render_expanded(void) {
    UiAssignPlotPanelView v = make_view(40, 0.0f, 10.0f);

    v.plot.expanded = 1;
    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("expanded panel draws its frame",
                gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("expanded panel draws its trace",
                gl_stub_counts[GL_STUB_glVertex2f] > 0);
    ASSERT_TRUE("expanded panel draws its labels",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0);
}

/* --- several series --- */

/* A two-series view: `angle` ramping 0..10 and `speed` ramping 100..200. */
static UiAssignPlotPanelView make_multi_view(int series) {
    UiAssignPlotPanelView v = make_view(20, 0.0f, 10.0f);
    if (series > 1) set_series(&v, 1, "speed",  20, 100.0f, 200.0f);
    if (series > 2) set_series(&v, 2, "radius", 20, -5.0f,  5.0f);
    if (series > 3) set_series(&v, 3, "phase",  20, 0.5f,   1.5f);
    return v;
}

/* The legend row only exists with more than one series, so the single-series
 * panel is exactly the panel it always was. */
static void test_legend_row_costs_nothing_for_one_series(void) {
    int h1 = 0, h2 = 0, h4 = 0;

    ui_assign_plot_panel_size(0, 1, NULL, &h1);
    ui_assign_plot_panel_size(0, 2, NULL, &h2);
    ui_assign_plot_panel_size(0, 4, NULL, &h4);

    ASSERT_TRUE("two series add a legend row", h2 > h1);
    ASSERT_INT("four series add the same one row", h4, h2);
    /* Entries share one row rather than stacking, so the panel does not grow
     * with the series count past the first. */
    ASSERT_TRUE("the legend is a single row", h2 - h1 < 20);
}

static void test_series_colors_are_distinct(void) {
    float seen[MAX_ASSIGN_PLOT_SERIES][3];

    for (int i = 0; i < MAX_ASSIGN_PLOT_SERIES; i++)
        ui_assign_plot_series_color(i, seen[i]);

    for (int a = 0; a < MAX_ASSIGN_PLOT_SERIES; a++)
        for (int b = a + 1; b < MAX_ASSIGN_PLOT_SERIES; b++)
            ASSERT_TRUE("series colors differ",
                        seen[a][0] != seen[b][0] || seen[a][1] != seen[b][1]
                        || seen[a][2] != seen[b][2]);

    /* Out of range must clamp rather than read past the table. */
    {
        float lo[3], hi[3];
        ui_assign_plot_series_color(-1, lo);
        ui_assign_plot_series_color(MAX_ASSIGN_PLOT_SERIES + 5, hi);
        ASSERT_TRUE("negative clamps to the first", lo[0] == seen[0][0]);
        ASSERT_TRUE("past the end clamps to the last",
                    hi[0] == seen[MAX_ASSIGN_PLOT_SERIES - 1][0]);
    }
}

static void test_render_multi_series(void) {
    UiAssignPlotPanelView one = make_view(20, 0.0f, 10.0f);
    UiAssignPlotPanelView four = make_multi_view(4);
    int one_v, four_v;

    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&one);
    one_v = gl_stub_counts[GL_STUB_glVertex2f];

    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&four);
    four_v = gl_stub_counts[GL_STUB_glVertex2f];

    ASSERT_TRUE("four series draw more geometry than one", four_v > one_v);
    /* Legend swatches are rects, on top of the panel and plot frames. */
    ASSERT_TRUE("legend swatches drawn", gl_stub_counts[GL_STUB_glRectf] > 2);
}

/* The Y axis spans every series: a second series out of the first's range has
 * to move the axis, or it would be drawn clipped to the plot edge. */
static void test_y_axis_spans_all_series(void) {
    UiAssignPlotPanelView v = make_multi_view(2);

    /* Log is available across the union either way - a negative in one series
     * puts the whole plot on the symmetric axis rather than refusing it. */
    ASSERT_INT("a zero in the first series still allows log",
               ui_assign_plot_y_log_available(&v), 1);

    set_series(&v, 0, "angle", 20, 1.0f, 10.0f);
    ASSERT_INT("all-positive across both series allows log",
               ui_assign_plot_y_log_available(&v), 1);

    set_series(&v, 1, "speed", 20, -1.0f, 5.0f);
    ASSERT_INT("a negative in the second series still allows log",
               ui_assign_plot_y_log_available(&v), 1);

    /* Both series flat at zero is the one case with no magnitude at all. */
    set_series(&v, 0, "angle", 20, 0.0f, 0.0f);
    set_series(&v, 1, "speed", 20, 0.0f, 0.0f);
    ASSERT_INT("two all-zero series have no decade to sit on",
               ui_assign_plot_y_log_available(&v), 0);
}

/* Gap columns break the line rather than being drawn as zero. */
static void test_render_gap_columns(void) {
    UiAssignPlotPanelView solid = make_view(20, 5.0f, 15.0f);
    UiAssignPlotPanelView gapped = make_view(20, 5.0f, 15.0f);
    int solid_v, gapped_v;

    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&solid);
    solid_v = gl_stub_counts[GL_STUB_glVertex2f];

    /* Punch a hole in the middle: two strips instead of one. */
    for (int i = 8; i < 12; i++)
        g_cols[0][i].valid = 0;
    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&gapped);
    gapped_v = gl_stub_counts[GL_STUB_glVertex2f];

    ASSERT_TRUE("a gap drops the vertices it covers", gapped_v < solid_v);
    ASSERT_TRUE("but the rest still draws", gapped_v > 0);

    /* An all-gap series draws no trace at all and must not crash. */
    for (int i = 0; i < 20; i++)
        g_cols[0][i].valid = 0;
    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&gapped);
    ASSERT_TRUE("an empty series still frames the panel",
                gl_stub_counts[GL_STUB_glRectf] > 0);

    for (int i = 0; i < ASSIGN_PLOT_COLS; i++)
        g_cols[0][i].valid = 1;
}

/* Hovering a legend entry selects whose statistics the panel shows. */
static void test_legend_hover_selects_series(void) {
    UiAssignPlotPanelView v = make_multi_view(2);
    int band_dy = 8 + 2 * 13 + 4;   /* inside the legend row */
    int cell_w  = (view_w(&v) - 2 * 8) / 2;
    int mx, my;

    /* Over the first entry. */
    panel_point(&v, 8 + 4, band_dy, &mx, &my);
    ASSERT_INT("first legend entry hit-tests to series 0",
               ui_assign_plot_panel_hit_test(&v, mx, my), 0);

    /* Over the second. */
    panel_point(&v, 8 + cell_w + 4, band_dy, &mx, &my);
    ASSERT_INT("second legend entry hit-tests to series 1",
               ui_assign_plot_panel_hit_test(&v, mx, my), 1);

    /* A single-series panel has no legend band at all. */
    {
        UiAssignPlotPanelView solo = make_view(20, 0.0f, 10.0f);
        panel_point(&solo, 12, band_dy, &mx, &my);
        ASSERT_INT("one series has no legend to hit",
                   ui_assign_plot_panel_hit_test(&solo, mx, my),
                   UI_ASSIGN_PLOT_HIT_NONE);
    }

    /* Hovering renders the hovered series' numbers; both cases must draw. */
    panel_point(&v, 8 + cell_w + 4, band_dy, &mx, &my);
    v.pointer_x = mx;
    v.pointer_y = my;
    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("hovering a legend entry still renders",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0);
}

/* A near-zero mean (the mean of a symmetric sine is ~1e-11, not 0) formats to
 * an exponent that does not fit beside its key at four significant digits.
 * It must lose digits rather than collide with the label or be clipped. */
static void test_render_tiny_exponent_stats(void) {
    UiAssignPlotPanelView v = make_view(20, -1.0f, 1.0f);
    char buf[32];

    /* The reported case: mean of sin over a symmetric range. At 4 significant
     * digits "1.027e-11" is 9 characters, which does not fit beside "mean" in
     * the 250px panel's cell - it must shed digits until it does. */
    ui_assign_plot_format_stat(buf, sizeof(buf), 1.0273455e-11, 9);
    ASSERT_STR("a fitting width keeps full precision", buf, "1.027e-11");

    ui_assign_plot_format_stat(buf, sizeof(buf), 1.0273455e-11, 8);
    ASSERT_TRUE("a tighter cell sheds digits", (int)strlen(buf) <= 8);
    ui_assign_plot_format_stat(buf, sizeof(buf), 1.0273455e-11, 5);
    ASSERT_STR("and keeps shedding down to one", buf, "1e-11");

    /* Still the magnitude, which is the entire content of a near-zero mean. */
    ASSERT_TRUE("the exponent survives", strstr(buf, "e-11") != NULL);

    /* No limit means the default four significant digits. */
    ui_assign_plot_format_stat(buf, sizeof(buf), 1.0273455e-11, 0);
    ASSERT_STR("no limit is full precision", buf, "1.027e-11");

    /* An impossible budget leaves the number too wide rather than truncating
     * it into a different, wrong number. */
    ui_assign_plot_format_stat(buf, sizeof(buf), 1.0273455e-11, 2);
    ASSERT_TRUE("an impossible width is not silently truncated",
                (int)strlen(buf) > 2);

    /* Ordinary values are untouched by any of this. */
    ui_assign_plot_format_stat(buf, sizeof(buf), 7.0, 12);
    ASSERT_STR("plain values format as before", buf, "7");

    v.plot.series[0].stats.mean   = 1.0273455e-11;
    v.plot.series[0].stats.stddev = 7.0710678e-1;
    v.plot.series[0].stats.min    = -9.9999994e-1;
    v.plot.series[0].stats.max    = 1.2345678e+12;

    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("wide-exponent statistics still render",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0);

    /* Same numbers in the expanded panel, where the cells are capped rather
     * than doubled - the fit has to hold in both sizes. */
    v.plot.expanded = 1;
    gl_stub_counts_reset();
    ui_assign_plot_panel_render(&v);
    ASSERT_TRUE("and in the expanded panel",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0);
}

/* The plot body itself is not a control; it must fall through so the caller
 * can consume it as inert overlay chrome. */
static void test_hit_plot_body_is_inert(void) {
    UiAssignPlotPanelView v = make_view(10, 0.0f, 1.0f);
    int mx, my;

    panel_point(&v, view_w(&v) / 2, view_h(&v) / 2, &mx, &my);
    ASSERT_INT("plot body is not a control",
               ui_assign_plot_panel_hit_test(&v, mx, my),
               UI_ASSIGN_PLOT_HIT_NONE);
}

/* ------------------------------------------------------------------ *
 * Replay program-counter markers.
 * ------------------------------------------------------------------ */

/* One marker recovered from the trace: its x, and the span it covers. */
typedef struct {
    float x, y0, y1;
} ApMarker;

/* Markers for `series`, identified by that series' color at the marker alpha -
 * which is what separates them from the trace (0.95) and the envelope (0.30)
 * drawn in the same hue. */
static int find_markers(const UiAssignPlotPanelView *v, int series,
                        ApMarker *out, int cap) {
    float rgb[3];
    FILE *trace;
    char line[128];
    int active = 0, count = 0, verts = 0;

    ui_assign_plot_series_color(series, rgb);
    gl_stub_trace_open(TRACE_PATH);
    ui_assign_plot_panel_render(v);
    gl_stub_trace_close();

    trace = fopen(TRACE_PATH, "r");
    if (!trace) return 0;
    while (fgets(line, sizeof(line), trace)) {
        float r, g, b, a, x, y;
        if (sscanf(line, "glColor4f %f %f %f %f", &r, &g, &b, &a) == 4) {
            active = fabsf(r - rgb[0]) < 1e-4f
                  && fabsf(g - rgb[1]) < 1e-4f
                  && fabsf(b - rgb[2]) < 1e-4f
                  && fabsf(a - 0.55f) < 1e-4f;
            verts = 0;
        } else if (active && sscanf(line, "glVertex2f %f %f", &x, &y) == 2) {
            if (count < cap) {
                if (verts == 0) {
                    out[count].x  = x;
                    out[count].y0 = y;
                    out[count].y1 = y;
                } else {
                    out[count].y1 = y;
                }
            }
            if (++verts == 2) { count++; verts = 0; }
        }
    }
    fclose(trace);
    return count;
}

static void test_replay_marker_is_placed_by_fraction(void) {
    UiAssignPlotPanelView v = make_view(16, 0.0f, 1.0f);
    ApMarker m[4];
    float left, right, height;

    v.replay_active = 1;

    /* The two ends give the plot well's span without the test having to know
     * the panel's internal geometry; the midpoint then has to land between
     * them, which is the only claim the marker actually makes. */
    v.replay_frac[0] = 0.0f;
    ASSERT_INT("one marker at the start", find_markers(&v, 0, m, 4), 1);
    left = m[0].x;
    height = m[0].y1 - m[0].y0;
    ASSERT_TRUE("the marker is a vertical rule", height > 10.0f);

    v.replay_frac[0] = 1.0f;
    ASSERT_INT("one marker at the end", find_markers(&v, 0, m, 4), 1);
    right = m[0].x;
    ASSERT_TRUE("the end is right of the start", right > left);

    v.replay_frac[0] = 0.5f;
    ASSERT_INT("one marker at the midpoint", find_markers(&v, 0, m, 4), 1);
    ASSERT_FLOAT("halfway through is halfway across",
                 m[0].x, (left + right) * 0.5f);
    /* The rule spans the plot well, so its height does not depend on where
     * along the axis it sits. */
    ASSERT_FLOAT("marker height is position-independent",
                 m[0].y1 - m[0].y0, height);
}

static void test_replay_marker_absent_without_replay(void) {
    UiAssignPlotPanelView v = make_view(16, 0.0f, 1.0f);
    ApMarker m[4];

    /* A fraction left over from an earlier frame must not draw on its own:
     * replay_active is the gate. */
    v.replay_frac[0] = 0.5f;
    ASSERT_INT("no marker when replay is off", find_markers(&v, 0, m, 4), 0);

    /* And a zero-initialized view - every hit-test-only caller - draws none. */
    memset(&v, 0, sizeof(v));
    v.window_w = WIN_W;
    v.window_h = WIN_H;
    v.visible = 1;
    ASSERT_INT("a zeroed view draws no marker", find_markers(&v, 0, m, 4), 0);
}

/* A negative fraction is "this series has no marker" - a series that has not
 * run yet must not be drawn as if the PC were at its first execution. */
static void test_replay_marker_skips_unrun_series(void) {
    UiAssignPlotPanelView v = make_view(16, 0.0f, 1.0f);
    ApMarker m[4];

    set_series(&v, 1, "beta", 16, 0.0f, 2.0f);
    v.replay_active = 1;
    v.replay_frac[0] = 0.25f;
    v.replay_frac[1] = -1.0f;

    ASSERT_INT("the running series is marked", find_markers(&v, 0, m, 4), 1);
    ASSERT_INT("the unrun series is not", find_markers(&v, 1, m, 4), 0);
}

/* Each series is spread across the full width as its own execution
 * percentage, so one PC is two different positions. */
static void test_replay_markers_are_per_series(void) {
    UiAssignPlotPanelView v = make_view(16, 0.0f, 1.0f);
    ApMarker a[4], b[4];

    set_series(&v, 1, "beta", 16, 0.0f, 2.0f);
    v.replay_active = 1;
    v.replay_frac[0] = 1.0f;
    v.replay_frac[1] = 0.25f;

    ASSERT_INT("first series has a marker", find_markers(&v, 0, a, 4), 1);
    ASSERT_INT("second series has its own", find_markers(&v, 1, b, 4), 1);
    ASSERT_TRUE("the two sit at different positions", a[0].x > b[0].x);
}

/* X_FRAME columns are whole captures: there is no within-frame position to
 * point at, so the renderer refuses even a fraction that was handed to it. */
static void test_replay_marker_absent_in_frame_mode(void) {
    UiAssignPlotPanelView v = make_view(16, 0.0f, 1.0f);
    ApMarker m[4];

    v.plot.x_mode = ASSIGN_PLOT_X_FRAME;
    v.replay_active = 1;
    v.replay_frac[0] = 0.5f;
    ASSERT_INT("no marker on the captures axis", find_markers(&v, 0, m, 4), 0);
}

/* An empty plot has no trace to annotate. */
static void test_replay_marker_absent_without_data(void) {
    UiAssignPlotPanelView v = make_view(0, 0.0f, 0.0f);
    ApMarker m[4];

    v.replay_active = 1;
    v.replay_frac[0] = 0.5f;
    ASSERT_INT("no marker over an empty plot", find_markers(&v, 0, m, 4), 0);
}

/* --- PC readouts: the dot on the trace and the number beside it --- */

/* Center y of the value dot for `series`: the first rect drawn in that
 * series' color at full alpha. Opaque is what separates it from the rule
 * (0.55), the trace (0.95) and the envelope (0.30); first, because the focused
 * legend swatch later in the render shares the color and the alpha. Returns 0
 * when the series has no dot. */
static int find_value_dot_y(const UiAssignPlotPanelView *v, int series,
                            float *out_y) {
    float rgb[3];
    FILE *trace;
    char line[128];
    int active = 0, found = 0;

    ui_assign_plot_series_color(series, rgb);
    gl_stub_trace_open(TRACE_PATH);
    ui_assign_plot_panel_render(v);
    gl_stub_trace_close();

    trace = fopen(TRACE_PATH, "r");
    if (!trace) return 0;
    while (fgets(line, sizeof(line), trace)) {
        float r, g, b, a, x0, y0, x1, y1;
        if (sscanf(line, "glColor4f %f %f %f %f", &r, &g, &b, &a) == 4) {
            active = fabsf(r - rgb[0]) < 1e-4f
                  && fabsf(g - rgb[1]) < 1e-4f
                  && fabsf(b - rgb[2]) < 1e-4f
                  && fabsf(a - 1.0f) < 1e-4f;
        } else if (active && !found
                   && sscanf(line, "glRectf %f %f %f %f",
                             &x0, &y0, &x1, &y1) == 4) {
            *out_y = (y0 + y1) * 0.5f;
            found = 1;
        }
    }
    fclose(trace);
    return found;
}

/* Every string the render printed, recovered from the trace: each
 * glRasterPos2f opens a new one and the glyph bytes that follow fill it. Both
 * text paths are read, since which one gl2d_draw_string takes is a build
 * flag. */
typedef struct {
    float x, y;
    char  text[32];
} ApText;

static int find_texts(const UiAssignPlotPanelView *v, ApText *out, int cap) {
    FILE *trace;
    char line[128];
    int count = 0, len = 0;

    gl_stub_trace_open(TRACE_PATH);
    ui_assign_plot_panel_render(v);
    gl_stub_trace_close();

    trace = fopen(TRACE_PATH, "r");
    if (!trace) return 0;
    while (fgets(line, sizeof(line), trace)) {
        float x, y;
        int ch;
        if (sscanf(line, "glRasterPos2f %f %f", &x, &y) == 2) {
            if (count < cap) {
                out[count].x = x;
                out[count].y = y;
                out[count].text[0] = '\0';
            }
            count++;
            len = 0;
        } else if (sscanf(line, "glutBitmapCharacter %d", &ch) == 1 ||
                   sscanf(line, "glutBitmapStringByte %d", &ch) == 1) {
            int slot = count - 1;
            if (slot >= 0 && slot < cap && len < 31) {
                out[slot].text[len++] = (char)ch;
                out[slot].text[len] = '\0';
            }
        }
    }
    fclose(trace);
    return count < cap ? count : cap;
}

static ApText g_texts[64];

/* The drawn string equal to `want`, or NULL. */
static const ApText *find_text(const UiAssignPlotPanelView *v,
                               const char *want) {
    int n = find_texts(v, g_texts, 64);
    for (int i = 0; i < n; i++)
        if (strcmp(g_texts[i].text, want) == 0) return &g_texts[i];
    return NULL;
}

static int text_is_drawn(const UiAssignPlotPanelView *v, const char *want) {
    return find_text(v, want) != NULL;
}

/* The number under the PC is the point of the marker: stepping a replay
 * through a loop is watching a value change, and a 90px axis cannot be read to
 * three digits. */
static void test_replay_readout_prints_the_value(void) {
    UiAssignPlotPanelView v = make_view(16, 0.0f, 1.0f);

    v.replay_active = 1;
    v.replay_frac[0] = 0.5f;
    v.replay_value[0] = 0.375f;
    ASSERT_TRUE("the value at the PC is printed",
                text_is_drawn(&v, "0.375"));

    /* It follows the value, not the position. */
    v.replay_value[0] = -12.5f;
    ASSERT_TRUE("a changed value reprints", text_is_drawn(&v, "-12.5"));
    ASSERT_TRUE("and the old one is gone", !text_is_drawn(&v, "0.375"));

    /* No replay, no readout - the same gate the rule is behind. */
    v.replay_active = 0;
    ASSERT_TRUE("nothing is printed outside replay",
                !text_is_drawn(&v, "-12.5"));
}

/* One readout per series, so an overlay of several rows says what each of
 * them is doing at this PC rather than only the primary. */
static void test_replay_readouts_are_per_series(void) {
    UiAssignPlotPanelView v = make_view(16, 0.0f, 1.0f);

    set_series(&v, 1, "beta", 16, 0.0f, 2.0f);
    v.replay_active = 1;
    v.replay_frac[0] = 0.25f;
    /* Values no gridline label could also produce - the gutter ticks are round
     * numbers on this axis, and the assertion is about the readouts. */
    v.replay_value[0] = 0.2917f;
    v.replay_frac[1] = 0.75f;
    v.replay_value[1] = 1.37f;

    ASSERT_TRUE("the first series' value is printed",
                text_is_drawn(&v, "0.2917"));
    ASSERT_TRUE("and so is the second's", text_is_drawn(&v, "1.37"));

    /* A series with no marker has no value to read off it. */
    v.replay_frac[1] = -1.0f;
    ASSERT_TRUE("an unrun series prints nothing", !text_is_drawn(&v, "1.37"));
    ASSERT_TRUE("without silencing the one that ran",
                text_is_drawn(&v, "0.2917"));
}

/* The dot marks the value the row computed, not the column the rule passes
 * through - a decimated column is a band whose midpoint is a number that never
 * existed. Pinned by putting the two in different places: the PC sits at the
 * left edge, where the ramp is at its minimum, while the value reported is the
 * maximum. */
static void test_replay_dot_sits_on_the_reported_value(void) {
    UiAssignPlotPanelView v = make_view(16, 0.0f, 1.0f);
    float trace_y[16];
    float dot_y;
    int n;

    n = render_series_trace_y(&v, 0, trace_y, 16);
    ASSERT_INT("the ramp drew a full strip", n, 16);

    v.replay_active = 1;
    v.replay_frac[0] = 0.0f;
    v.replay_value[0] = 1.0f;
    ASSERT_TRUE("the PC carries a dot", find_value_dot_y(&v, 0, &dot_y));
    ASSERT_FLOAT("the dot is at the value, not under the rule",
                 dot_y, trace_y[15]);

    v.replay_value[0] = 0.0f;
    ASSERT_TRUE("the dot survives a moved value",
                find_value_dot_y(&v, 0, &dot_y));
    ASSERT_FLOAT("and follows it down", dot_y, trace_y[0]);
}

/* A readout beside a marker at the right edge would run off the panel, which
 * is where a finished series parks - so it flips to the other side of its
 * rule. */
static void test_replay_readout_stays_inside_the_panel(void) {
    UiAssignPlotPanelView v = make_view(16, 0.0f, 1.0f);
    const ApText *label;
    int panel_w, right;

    ui_assign_plot_panel_size(v.plot.expanded, v.plot.series_count,
                              &panel_w, NULL);
    right = v.panel_x + panel_w;

    v.replay_active = 1;
    v.replay_frac[0] = 1.0f;
    v.replay_value[0] = 0.6183f;
    label = find_text(&v, "0.6183");
    ASSERT_TRUE("a finished series still prints its value", label != NULL);
    if (label)
        ASSERT_TRUE("and prints it inside the panel",
                    label->x + 6.0f * (float)FONT_SMALL_W < (float)right);

    /* The left edge is the mirror case: the label must not start outside. */
    v.replay_frac[0] = 0.0f;
    label = find_text(&v, "0.6183");
    ASSERT_TRUE("a just-started series prints too", label != NULL);
    if (label)
        ASSERT_TRUE("and not off the left edge",
                    label->x >= (float)v.panel_x);
}

/* Read the color the rate chip's label is drawn in. The chip is the first
 * thing in the control row, so its text color is the glColor* immediately
 * preceding the first glRasterPos2f at the control row's baseline - simpler to
 * pin by comparing whole renders, which is what the caller does. */
static int trace_render(const UiAssignPlotPanelView *v, char *buf, size_t cap) {
    FILE *trace;
    size_t n;

    gl_stub_trace_open(TRACE_PATH);
    ui_assign_plot_panel_render(v);
    gl_stub_trace_close();

    trace = fopen(TRACE_PATH, "r");
    if (!trace) return 0;
    n = fread(buf, 1, cap - 1, trace);
    buf[n] = '\0';
    fclose(trace);
    return (int)n;
}

static char g_trace_a[1 << 20];
static char g_trace_b[1 << 20];

/* The rate chip is drawn inert while replay overrides it - and is *not* at
 * ASSIGN_PLOT_RATE_ONCE, which replay leaves frozen and therefore in force. */
static void test_replay_grays_the_overridden_rate_chip(void) {
    UiAssignPlotPanelView v = make_view(16, 0.0f, 1.0f);

    /* No marker in either render, so the chip color is the only thing that can
     * differ between them. */
    v.replay_frac[0] = -1.0f;
    v.plot.rate = ASSIGN_PLOT_RATE_1HZ;
    v.replay_active = 0;
    ASSERT_TRUE("baseline render captured",
                trace_render(&v, g_trace_a, sizeof(g_trace_a)) > 0);
    v.replay_active = 1;
    trace_render(&v, g_trace_b, sizeof(g_trace_b));
    ASSERT_TRUE("an overridden rate chip renders differently",
                strcmp(g_trace_a, g_trace_b) != 0);

    /* ONCE is exempt from the override, so nothing about the panel changes.
     * The capture side produces no fraction for a frozen one-shot, so this is
     * what the controller hands the renderer in that case. */
    v.plot.rate = ASSIGN_PLOT_RATE_ONCE;
    v.replay_frac[0] = -1.0f;
    v.replay_active = 0;
    trace_render(&v, g_trace_a, sizeof(g_trace_a));
    v.replay_active = 1;
    trace_render(&v, g_trace_b, sizeof(g_trace_b));
    ASSERT_INT("a frozen one-shot's chip stays in force",
               strcmp(g_trace_a, g_trace_b), 0);
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
    test_render_log_axis();
    test_render_expanded();
    test_y_log_availability();
    test_render_symlog_axis();
    test_hit_outside_and_hidden();
    test_hit_close();
    test_hit_rate_and_reset();
    test_hit_rate_chip_is_constant_width();
    test_chip_grammar();
    test_hit_yscale_and_expand();
    test_hit_tracks_expanded_geometry();
    test_hit_plot_body_is_inert();
    test_legend_row_costs_nothing_for_one_series();
    test_series_colors_are_distinct();
    test_render_multi_series();
    test_y_axis_spans_all_series();
    test_render_gap_columns();
    test_legend_hover_selects_series();
    test_render_tiny_exponent_stats();
    test_replay_marker_is_placed_by_fraction();
    test_replay_marker_absent_without_replay();
    test_replay_marker_skips_unrun_series();
    test_replay_markers_are_per_series();
    test_replay_marker_absent_in_frame_mode();
    test_replay_marker_absent_without_data();
    test_replay_readout_prints_the_value();
    test_replay_readouts_are_per_series();
    test_replay_dot_sits_on_the_reported_value();
    test_replay_readout_stays_inside_the_panel();
    test_replay_grays_the_overridden_rate_chip();
    return test_harness_report(&g_harness, "test_ui_assign_plot");
}
#else
int main(void) {
    printf("test_ui_assign_plot: requires GL stubs (USE_GL_STUBS=1)\n");
    return 0;
}
#endif
