/*
 * test_memprof.c - Tests for src/support/memprof.c
 *
 * Pure-support module: no GL/UI deps, so this links against memprof.o only.
 * Each test case begins with memprof_reset() to avoid state leaking across
 * cases when run in the same process.
 */
#include "support/memprof.h"
#include "support/test_harness.h"

#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_INT(label, got, expected) \
    TEST_ASSERT_INT(&g_harness, label, got, expected)
#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_STR(label, got, expected) \
    TEST_ASSERT_STR(&g_harness, label, got, expected)

/* Fake-reader machinery for deterministic cadence/ring assertions. */
static unsigned long long g_test_rss = 100;
static int g_test_reader_calls = 0;

static int fake_reader(MemSample *out) {
    g_test_reader_calls++;
    out->rss_bytes = g_test_rss;
    return 1;
}

/* ---- Test cases -------------------------------------------------------- */

static void test_baseline_nonzero_on_host(void) {
    printf("test_baseline_nonzero_on_host\n");
    memprof_reset();
    memprof_init();
    MemSample base = memprof_baseline();
#if defined(__APPLE__) || defined(__linux__)
    ASSERT_TRUE("baseline RSS > 0 on host", base.rss_bytes > 0);
#else
    /* _WIN32 stub returns zeros in v1; no assertion. */
    (void)base;
    ASSERT_TRUE("baseline call returned on unsupported platform", 1);
#endif
}

static void test_cadence_honored(void) {
    printf("test_cadence_honored\n");
    memprof_reset();
    memprof_set_reader(fake_reader);
    g_test_rss = 100;
    memprof_init_at(0.0);

    /* Init should not push anything. */
    ASSERT_INT("count 0 after init", memprof_history_count(), 0);

    /* Subinterval ticks should not push. */
    for (double t = 0.5; t < MEMPROF_PUSH_INTERVAL_S; t += 0.5) {
        memprof_frame_tick_at(t);
    }
    ASSERT_INT("count still 0 before first interval", memprof_history_count(), 0);

    memprof_frame_tick_at(MEMPROF_PUSH_INTERVAL_S);
    ASSERT_INT("count 1 at first interval", memprof_history_count(), 1);
    MemSample s0;
    memprof_history_get(0, &s0, NULL);
    ASSERT_TRUE("sample 0 rss == 100", s0.rss_bytes == 100);

    memprof_frame_tick_at(2.0 * MEMPROF_PUSH_INTERVAL_S);
    ASSERT_INT("count 2 at second interval", memprof_history_count(), 2);
}

static void test_ring_wraps(void) {
    printf("test_ring_wraps\n");
    memprof_reset();
    memprof_set_reader(fake_reader);
    g_test_rss = 0;
    memprof_init_at(0.0);

    const int cap = memprof_history_capacity();
    const int N   = cap + 5;
    for (int i = 1; i <= N; i++) {
        g_test_rss = (unsigned long long)i;
        memprof_frame_tick_at((double)i * MEMPROF_PUSH_INTERVAL_S);
    }

    ASSERT_INT("count at capacity", memprof_history_count(), cap);

    MemSample oldest, newest;
    memprof_history_get(0, &oldest, NULL);
    memprof_history_get(cap - 1, &newest, NULL);
    /* Oldest surviving push is the (N - cap + 1)th = 6th. */
    ASSERT_INT("oldest survivor rss == 6", (int)oldest.rss_bytes, 6);
    ASSERT_INT("newest rss == N",          (int)newest.rss_bytes, N);
}

static void test_history_latest_t_right_anchored(void) {
    printf("test_history_latest_t_right_anchored\n");
    memprof_reset();
    memprof_set_reader(fake_reader);
    memprof_init_at(0.0);

    /* No samples yet: latest_t should be 0. */
    ASSERT_TRUE("latest_t == 0 with empty history",
                memprof_history_latest_t() == 0.0);

    double t1 = MEMPROF_PUSH_INTERVAL_S;
    double t2 = 2.0 * MEMPROF_PUSH_INTERVAL_S;
    double t3 = 3.0 * MEMPROF_PUSH_INTERVAL_S;
    memprof_frame_tick_at(t1);
    memprof_frame_tick_at(t2);
    memprof_frame_tick_at(t3);

    double latest = memprof_history_latest_t();
    /* Within an epsilon of the last pushed t_rel. */
    ASSERT_TRUE("latest_t ~ third interval after 3 pushes",
                latest > t3 - 0.01 && latest < t3 + 0.01);
}

static void test_format_bytes_ranges(void) {
    printf("test_format_bytes_ranges\n");
    char buf[24];

    memprof_format_bytes(buf, sizeof(buf), 0ULL);
    ASSERT_STR("0 -> --", buf, "--");

    memprof_format_bytes(buf, sizeof(buf), 1500ULL);
    ASSERT_STR("1500 -> 1 KB", buf, "1 KB");

    /* MB / GB output is right-aligned to MEMPROF_FMT_WIDTH chars so the
     * panel's RSS / init / delta unit column stays in line. KB output
     * stays unpadded (KB-range values are atypical for a long-running
     * process and skipping the pad keeps tiny values readable). */
    memprof_format_bytes(buf, sizeof(buf), 1500000ULL);
    ASSERT_STR("1.5M -> '   1.4 MB' (width-padded)", buf, "   1.4 MB");

    memprof_format_bytes(buf, sizeof(buf), 2500000000ULL);
    ASSERT_STR("2.5G -> '  2.33 GB' (width-padded)", buf, "  2.33 GB");
}

static void test_set_reader_null_restores_platform(void) {
    printf("test_set_reader_null_restores_platform\n");
    memprof_reset();

    /* Install a fake, init via the fake, then swap back to platform reader. */
    memprof_set_reader(fake_reader);
    g_test_rss = 1234;
    memprof_init_at(0.0);

    MemSample cur_fake = memprof_current();
    ASSERT_TRUE("current uses fake reader", cur_fake.rss_bytes == 1234);

    memprof_set_reader(NULL);
    memprof_frame_tick_at(0.1);
    MemSample cur_platform = memprof_current();
#if defined(__APPLE__) || defined(__linux__)
    ASSERT_TRUE("current non-zero after platform reader restored",
                cur_platform.rss_bytes > 0 && cur_platform.rss_bytes != 1234);
#else
    /* _WIN32 stub returns zeros; just confirm the call did not crash. */
    (void)cur_platform;
    ASSERT_TRUE("platform reader call returned on unsupported platform", 1);
#endif
}

int main(void) {
    printf("=== memprof tests ===\n\n");

    test_baseline_nonzero_on_host();
    test_cadence_honored();
    test_ring_wraps();
    test_history_latest_t_right_anchored();
    test_format_bytes_ranges();
    test_set_reader_null_restores_platform();

    printf("\n=== Results: ");
    return test_harness_report(&g_harness, "memprof");
}
