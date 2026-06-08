/*
 * test_ui_memprof.c
 */
#ifdef GL_STUBS
#include "ui/support/memprof.h"
#include "support/memprof.h"
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

static void test_memprof_metrics(void) {
    ASSERT_TRUE("width is positive", ui_memory_panel_width() > 0);
    ASSERT_TRUE("height is positive", ui_memory_panel_height() > 0);
}

static void test_memprof_render_off(void) {
    UiMemoryPanelView view = {
        .window_w = 800,
        .window_h = 600,
        .mode = MEMORY_PANEL_OFF,
        .panel_x = 10,
        .panel_y = 10
    };
    gl_stub_counts_reset();
    ui_memory_panel_render(&view);
    ASSERT_INT_EQ("off mode draws nothing",
                  (int)gl_stub_counts[GL_STUB_glRectf], 0);
}

static int mock_reader_zeros(MemSample *out) {
    out->rss_bytes = 0;
    return 1;
}

static void test_memprof_render_on_empty(void) {
    UiMemoryPanelView view = {
        .window_w = 800,
        .window_h = 600,
        .mode = MEMORY_PANEL_ON,
        .panel_x = 10,
        .panel_y = 10
    };

    memprof_reset();
    memprof_set_reader(mock_reader_zeros);
    memprof_init_at(0.0);

    gl_stub_counts_reset();
    ui_memory_panel_render(&view);
    ASSERT_TRUE("on mode empty draws frame",
                gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("on mode empty draws placeholder text",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0 ||
                gl_stub_counts[GL_STUB_glutBitmapCharacter] > 0);
}

static int mock_reader_data(MemSample *out) {
    out->rss_bytes = 100 * 1024 * 1024;
    return 1;
}

static void test_memprof_render_on_with_data(void) {
    UiMemoryPanelView view = {
        .window_w = 800,
        .window_h = 600,
        .mode = MEMORY_PANEL_ON,
        .panel_x = 10,
        .panel_y = 10
    };

    memprof_reset();
    memprof_set_reader(mock_reader_data);
    memprof_init_at(0.0);
    memprof_frame_tick_at(10.0); // push one sample at 10s
    memprof_frame_tick_at(20.0); // push one sample at 20s

    gl_stub_counts_reset();
    ui_memory_panel_render(&view);
    ASSERT_TRUE("on mode draws frame",
                gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("on mode draws text",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0 ||
                gl_stub_counts[GL_STUB_glutBitmapCharacter] > 0);
    ASSERT_TRUE("on mode draws lines",
                gl_stub_counts[GL_STUB_glBegin] > 0);
}

int main(void) {
    printf("--- ui_memprof tests ---\n");
    test_memprof_metrics();
    test_memprof_render_off();
    test_memprof_render_on_empty();
    test_memprof_render_on_with_data();
    printf("\n");
    return test_harness_report(&g_harness, "test_ui_memprof");
}
#else
int main(void) {
    printf("This test requires GL stubs (USE_GL_STUBS=1)\n");
    return 0;
}
#endif
