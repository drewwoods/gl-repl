/*
 * test_splash.c - unit tests for startup splash banner.
 */
#include "app/boot/splash.h"
#include "tests/gl-stubs/include/GL/gl_stub_counts.h"
#include "support/test_harness.h"

#include <stdio.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, g, e)  TEST_ASSERT_INT(&g_harness, label, g, e)

static void test_splash_lifecycle(void) {
    /* Initially active */
    ASSERT_INT("splash initially active", splash_active(), 1);

    gl_stub_counts_reset();

    /* Render first frame */
    splash_render(1200, 800);
    ASSERT_TRUE("rendering first frame generates GL calls",
                gl_stub_counts[GL_STUB_glRectf] > 0 || gl_stub_counts[GL_STUB_glBegin] > 0);

    /* Run up to SPLASH_HOLD_FRAMES (150) */
    for (int i = 1; i < 150; i++) {
        splash_render(1200, 800);
    }
    ASSERT_INT("splash active during hold period", splash_active(), 1);

    /* Run through SPLASH_FADE_FRAMES (36) */
    for (int i = 0; i < 36; i++) {
        splash_render(1200, 800);
    }
    /* Total frames run: 150 + 36 = 186. Active check should still be true on frame 185
     * but expires when frame 186 is rendered. */
    ASSERT_INT("splash active after 185 frames", splash_active(), 1);

    /* Frame 186 rendering causes expiration */
    splash_render(1200, 800);
    ASSERT_INT("splash inactive after 186 frames", splash_active(), 0);

    /* Further renders should do nothing */
    gl_stub_counts_reset();
    splash_render(1200, 800);
    ASSERT_INT("inactive splash render makes no GL calls",
               (int)gl_stub_counts[GL_STUB_glBegin], 0);

    /* splash_skip can still be called safely */
    splash_skip();
    ASSERT_INT("splash remains inactive after skip", splash_active(), 0);
}

int main(void) {
    test_splash_lifecycle();
    return test_harness_report(&g_harness, "splash");
}
