/*
 * test_ui_tabbed_overlay.c - pure geometry/hit-test for the modal
 * tabbed overlay, plus the help-overlay over-scroll clamp regression.
 *
 * Over-scroll bug (fixed): the help session scroll offset was only
 * clamped for *display* inside the renderer, never written back. So
 * scrolling past the end accumulated an unbounded offset that had to
 * be "unwound" before an upward scroll moved anything visible. The
 * controller now clamps the session via glr_ctrl_help_scroll_by().
 *
 * Core test (runs under `make test`).
 */
#include "app/glr_ctrl.h"
#include "app/glr_state.h"
#include "editor/help_session.h"
#include "ui/core/metrics.h"
#include "ui/app/state.h"
#include "ui/core/tabbed_overlay.h"
#include "support/test_harness.h"

#include <stdio.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond)   TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT_EQ(label, g, e) TEST_ASSERT_INT(&g_harness, label, g, e)

/* --- Synthetic content so geometry math is deterministic ---------- */

static const char *k_lines_100[101];
static const char *k_lines_5[6];

static void init_synthetic_lines(void) {
    for (int i = 0; i < 100; i++)
        k_lines_100[i] = "row";
    k_lines_100[100] = 0;
    for (int i = 0; i < 5; i++)
        k_lines_5[i] = "row";
    k_lines_5[5] = 0;
}

static UiOverlayContent make_content(const UiOverlayTab *tabs, int n) {
    UiOverlayContent c;
    c.title = "TEST";
    c.tabs = tabs;
    c.tab_count = n;
    return c;
}

/* overlay_compute_geom() math, mirrored here so the test pins the
 * exact bounds the renderer derives (LINE_H from ui/metrics.h). */
static int expected_max_scroll(int win_w, int win_h, int n_lines) {
    (void)win_w;
    int hh = win_h * 5 / 6;
    int pad_top = (LINE_H + 4) + (LINE_H + 2) + 6;
    int content_h = hh - pad_top - 20;
    int visible = content_h / LINE_H;
    if (visible < 1) visible = 1;
    int ms = n_lines - visible;
    return ms < 0 ? 0 : ms;
}

static void test_max_scroll_bounds(void) {
    UiOverlayTab tabs[1] = { { "Big", k_lines_100 } };
    UiOverlayContent content = make_content(tabs, 1);
    UiOverlayState st = {
        .visible = 1, .tab_idx = 0, .scroll = 0,
        .viewport_w = 1000, .viewport_h = 600, .content = &content,
    };
    ASSERT_INT_EQ("max_scroll 100 lines @1000x600",
                  ui_tabbed_overlay_max_scroll(&st),
                  expected_max_scroll(1000, 600, 100));

    /* Content that fits entirely => no scroll room. */
    UiOverlayTab small[1] = { { "Small", k_lines_5 } };
    UiOverlayContent sc = make_content(small, 1);
    st.content = &sc;
    ASSERT_INT_EQ("max_scroll clamps to 0 when content fits",
                  ui_tabbed_overlay_max_scroll(&st), 0);

    /* NULL / empty content must be safe and yield 0. */
    UiOverlayState empty = { 1, 0, 0, 800, 600, 0 };
    ASSERT_INT_EQ("max_scroll 0 for NULL content",
                  ui_tabbed_overlay_max_scroll(&empty), 0);
}

static void test_hit_test_regions(void) {
    UiOverlayTab tabs[3] = {
        { "A", k_lines_100 }, { "B", k_lines_100 }, { "C", k_lines_100 },
    };
    UiOverlayContent content = make_content(tabs, 3);
    UiOverlayState st = {
        .visible = 1, .tab_idx = 0, .scroll = 0,
        .viewport_w = 1000, .viewport_h = 600, .content = &content,
    };

    /* Panel rect: hx=166 hw=666 hy=50 hh=500 (GL y-up). Tab band:
     * tab_y = 50+500-22-20 = 508, height 20 -> gy in [508,528].
     * tab_w = 666/3 = 222. GLUT my = 600 - gy. */
    UiOverlayHit out = ui_tabbed_overlay_hit_test(&st, 5, 5);
    ASSERT_INT_EQ("far corner is OUTSIDE", out.kind, UI_OVERLAY_HIT_OUTSIDE);

    UiOverlayHit tab1 = ui_tabbed_overlay_hit_test(&st, 166 + 222 + 10,
                                                   600 - 515);
    ASSERT_INT_EQ("tab band -> TAB", tab1.kind, UI_OVERLAY_HIT_TAB);
    ASSERT_INT_EQ("tab band -> middle tab index", tab1.tab, 1);

    UiOverlayHit body = ui_tabbed_overlay_hit_test(&st, 400, 600 - 300);
    ASSERT_INT_EQ("panel interior -> BODY", body.kind, UI_OVERLAY_HIT_BODY);

    /* Not visible => always OUTSIDE even over the panel. */
    st.visible = 0;
    UiOverlayHit hidden = ui_tabbed_overlay_hit_test(&st, 400, 300);
    ASSERT_INT_EQ("hidden overlay -> OUTSIDE",
                  hidden.kind, UI_OVERLAY_HIT_OUTSIDE);
}

/* The regression: over-scrolling must NOT accumulate past the end,
 * so a single upward scroll is immediately visible (no unwind). */
static void test_help_scroll_clamp_regression(void) {
    glr_ctrl_reset_all();
    /* Small viewport guarantees the help text overflows -> max>0. */
    ui_state_viewport_set_size(420, 320);
    ui_state_help_mut()->visible = 1;
    editor_help_session_set_tab(0);
    editor_help_session_set_scroll(0);

    /* Recreate the controller's overlay-state to learn the bound. */
    UiViewportState vp = ui_state_viewport();
    EditorHelpSession s = editor_help_session_view();
    UiOverlayState st = {
        .visible = 1, .tab_idx = s.tab_idx, .scroll = s.scroll,
        .viewport_w = vp.window_w, .viewport_h = vp.window_h,
        .content = glr_ctrl_help_overlay_content(),
    };
    int max_scroll = ui_tabbed_overlay_max_scroll(&st);
    ASSERT_TRUE("help content overflows at small viewport",
                max_scroll > 0);

    /* Hammer the scroll way past the end many times. */
    for (int i = 0; i < 50; i++)
        glr_ctrl_help_scroll_by(100);
    ASSERT_INT_EQ("over-scroll pins at max (no runaway offset)",
                  editor_help_session_scroll(), max_scroll);

    /* One step up must move immediately - the bug made this a no-op
     * until the accumulated overshoot had been unwound. */
    glr_ctrl_help_scroll_by(-1);
    ASSERT_INT_EQ("single scroll-up responds immediately",
                  editor_help_session_scroll(), max_scroll - 1);

    /* Symmetric lower bound: cannot go negative. */
    for (int i = 0; i < 50; i++)
        glr_ctrl_help_scroll_by(-100);
    ASSERT_INT_EQ("scroll clamps at 0",
                  editor_help_session_scroll(), 0);
}

#ifdef GL_STUBS
static void test_short_lines_render_no_overflow(void) {
    const char *short_lines[] = {
        "",
        " ",
        "  ",
        "   ",
        "    ",
        "\t",
        "left\tright",
        0
    };
    UiOverlayTab tab[1] = { { "Short", short_lines } };
    UiOverlayContent content = make_content(tab, 1);
    UiOverlayState st = {
        .visible = 1, .tab_idx = 0, .scroll = 0,
        .viewport_w = 800, .viewport_h = 600, .content = &content,
    };

    /* Call render - under ASan/UBSan this will crash if we read past the NUL terminator. */
    ui_tabbed_overlay_render(&st);
    ASSERT_TRUE("rendered short lines without crash", 1);
}
#endif

int main(void) {
    printf("--- ui_tabbed_overlay tests ---\n");
    init_synthetic_lines();
    test_max_scroll_bounds();
    test_hit_test_regions();
    test_help_scroll_clamp_regression();
#ifdef GL_STUBS
    test_short_lines_render_no_overflow();
#endif
    return test_harness_report(&g_harness, "ui_tabbed_overlay");
}
