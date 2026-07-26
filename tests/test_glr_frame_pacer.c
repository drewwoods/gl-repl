/*
 * test_glr_frame_pacer.c - unit tests for absolute-deadline 60 Hz frame pacer.
 */
#include "app/boot/glr_frame_pacer.h"
#include "support/test_harness.h"

#include <math.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, g, e)  TEST_ASSERT_INT(&g_harness, label, g, e)

static void test_null_pacer_safety(void) {
    int delay = glr_frame_pacer_next_delay_ms(NULL, 1000.0);
    ASSERT_INT("NULL pacer returns 1", delay, 1);
}

static void test_first_frame_delay(void) {
    GlrFramePacer pacer = GLR_FRAME_PACER_INIT;
    int delay = glr_frame_pacer_next_delay_ms(&pacer, 1000.0);
    /* 1000.0 + 16.6667 = 1016.6667, delay = lround(16.6667) = 17 */
    ASSERT_INT("first frame delay is 17ms", delay, 17);
    ASSERT_TRUE("deadline initialized", pacer.next_deadline_ms > 1016.0);
}

static void test_subsequent_frame_delay(void) {
    GlrFramePacer pacer = GLR_FRAME_PACER_INIT;
    /* First frame at t=1000.0 -> deadline becomes 1016.6667 */
    glr_frame_pacer_next_delay_ms(&pacer, 1000.0);

    /* Second frame called early at t=1005.0.
     * Deadline becomes 1016.6667 + 16.6667 = 1033.3333.
     * Expected delay = 1033.3333 - 1005.0 = 28.3333 -> lround(28.3333) = 28. */
    int delay = glr_frame_pacer_next_delay_ms(&pacer, 1005.0);
    ASSERT_INT("early frame delay is 28ms", delay, 28);
}

static void test_missed_deadline_resync(void) {
    GlrFramePacer pacer = GLR_FRAME_PACER_INIT;
    /* First frame at t=1000.0 -> deadline becomes 1016.6667 */
    glr_frame_pacer_next_delay_ms(&pacer, 1000.0);

    /* Next frame called extremely late at t=1200.0.
     * Deadline advances to 1016.6667 + 16.6667 = 1033.3333.
     * Since 1033.3333 < 1200.0, the deadline resynchronizes to 1200.0.
     * Delay becomes lround(1200.0 - 1200.0) = 0, which clamps to 1. */
    int delay = glr_frame_pacer_next_delay_ms(&pacer, 1200.0);
    ASSERT_INT("missed deadline clamps to 1ms delay", delay, 1);
    ASSERT_TRUE("deadline resynchronized to now", pacer.next_deadline_ms == 1200.0);
}

int main(void) {
    test_null_pacer_safety();
    test_first_frame_delay();
    test_subsequent_frame_delay();
    test_missed_deadline_resync();

    return test_harness_report(&g_harness, "glr_frame_pacer");
}
