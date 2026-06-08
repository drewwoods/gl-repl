/*
 * test_ui_cpuprof.c
 */
#ifdef GL_STUBS
#include "ui/support/cpuprof.h"
#include "support/cpuprof.h"
#include "support/test_harness.h"
#include <GL/gl_stub_counts.h>
#endif

#include <stdio.h>
#include <string.h>

#ifdef GL_STUBS
static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT_EQ(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

static void test_cpuprof_metrics(void) {
    ASSERT_TRUE("width is positive", ui_profile_panel_width() > 0);
    ASSERT_TRUE("height is positive on", ui_profile_panel_height(PROFILE_PANEL_ON) > 0);
    ASSERT_TRUE("height is positive details", ui_profile_panel_height(PROFILE_PANEL_DETAILS) > 0);
}

static void test_cpuprof_render_off(void) {
    UiProfilePanelView view = {
        .window_w = 800,
        .window_h = 600,
        .mode = PROFILE_PANEL_OFF,
        .panel_x = 10,
        .panel_y = 10
    };
    gl_stub_counts_reset();
    ui_profile_panel_render(&view);
    ASSERT_INT_EQ("off mode draws nothing",
                  (int)gl_stub_counts[GL_STUB_glRectf], 0);
}

static void test_cpuprof_render_on(void) {
    UiProfilePanelView view = {
        .window_w = 800,
        .window_h = 600,
        .mode = PROFILE_PANEL_ON,
        .panel_x = 10,
        .panel_y = 10
    };

    prof_frame_tick();

    gl_stub_counts_reset();
    ui_profile_panel_render(&view);
    ASSERT_TRUE("on mode draws frame",
                gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("on mode draws text",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0 ||
                gl_stub_counts[GL_STUB_glutBitmapCharacter] > 0);
}

static void test_cpuprof_render_details(void) {
    UiProfilePanelView view = {
        .window_w = 800,
        .window_h = 600,
        .mode = PROFILE_PANEL_DETAILS,
        .panel_x = 10,
        .panel_y = 10
    };

    prof_frame_tick();

    gl_stub_counts_reset();
    ui_profile_panel_render(&view);
    ASSERT_TRUE("details mode draws frame",
                gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("details mode draws text",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0 ||
                gl_stub_counts[GL_STUB_glutBitmapCharacter] > 0);
}

int main(void) {
    printf("--- ui_cpuprof tests ---\n");
    test_cpuprof_metrics();
    test_cpuprof_render_off();
    test_cpuprof_render_on();
    test_cpuprof_render_details();
    printf("\n");
    return test_harness_report(&g_harness, "test_ui_cpuprof");
}
#else
int main(void) {
    printf("This test requires GL stubs (USE_GL_STUBS=1)\n");
    return 0;
}
#endif
