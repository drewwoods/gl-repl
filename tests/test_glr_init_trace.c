/*
 * test_glr_init_trace.c - startup init-trace diagnostic.
 *
 * Covers the two decisions the module actually owns: the elapsed-seconds
 * clock (lazily anchored on first use, monotonic non-decreasing) and the
 * detailed-phase gate (glr_init_trace always emits; glr_init_trace_detail
 * emits only while the detailed flag is set). Emission is observed by
 * capturing stderr to a temp file, since the trace format is the contract
 * the startup-stall diagnostics are read with.
 *
 * Standalone: links only src/app/glr_init_trace.o - no GL, no controller.
 */
#include "app/boot/glr_init_trace.h"

#include "support/test_harness.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, g, e)  TEST_ASSERT_INT(&g_harness, label, g, e)

#define CAPTURE_PATH "test_glr_init_trace_stderr.tmp"

static int g_saved_stderr = -1;

/* Redirect stderr to CAPTURE_PATH. fileno(stderr) is reused by freopen, so
 * the saved dup restores the original stream in capture_end. */
static void capture_begin(void) {
    fflush(stderr);
    g_saved_stderr = dup(fileno(stderr));
    if (!freopen(CAPTURE_PATH, "w", stderr)) {
        /* Leave stderr alone; the content assertions will report the miss. */
        if (g_saved_stderr >= 0) { close(g_saved_stderr); g_saved_stderr = -1; }
    }
}

static void capture_end(char *buf, size_t buf_sz) {
    fflush(stderr);
    if (g_saved_stderr >= 0) {
        dup2(g_saved_stderr, fileno(stderr));
        close(g_saved_stderr);
        g_saved_stderr = -1;
    }
    clearerr(stderr);

    buf[0] = '\0';
    FILE *f = fopen(CAPTURE_PATH, "r");
    if (f) {
        size_t n = fread(buf, 1, buf_sz - 1, f);
        buf[n] = '\0';
        fclose(f);
    }
    remove(CAPTURE_PATH);
}

static void test_detailed_flag(void) {
    /* Default is off so a normal start emits only baseline phases. */
    ASSERT_INT("detailed defaults off", glr_init_trace_detailed(), 0);

    glr_init_trace_set_detailed(1);
    ASSERT_INT("detailed set on", glr_init_trace_detailed(), 1);

    glr_init_trace_set_detailed(0);
    ASSERT_INT("detailed set off", glr_init_trace_detailed(), 0);

    /* Any non-zero normalizes to 1 (callers pass raw env/flag truthiness). */
    glr_init_trace_set_detailed(42);
    ASSERT_INT("detailed normalizes truthy to 1", glr_init_trace_detailed(), 1);

    glr_init_trace_set_detailed(0);
}

static void test_elapsed_clock(void) {
    double a = glr_init_trace_elapsed_seconds();
    double b = glr_init_trace_elapsed_seconds();

    ASSERT_TRUE("elapsed is non-negative", a >= 0.0);
    ASSERT_TRUE("elapsed is monotonic non-decreasing", b >= a);
    /* Anchored on first use, not on some epoch: a freshly started test
     * process must not report a large elapsed time. */
    ASSERT_TRUE("elapsed anchors on first use", a < 60.0);
}

static void test_baseline_phase_always_emits(void) {
    char buf[4096];

    glr_init_trace_set_detailed(0);
    capture_begin();
    glr_init_trace("PHASE_ALPHA");
    glr_init_trace_detail("PHASE_BETA");
    capture_end(buf, sizeof(buf));

    ASSERT_TRUE("baseline phase emitted", strstr(buf, "PHASE_ALPHA") != NULL);
    ASSERT_TRUE("trace carries [init + prefix", strstr(buf, "[init +") != NULL);
    ASSERT_TRUE("detail phase suppressed when flag off",
                strstr(buf, "PHASE_BETA") == NULL);
}

static void test_detail_phase_gated_on_flag(void) {
    char buf[4096];

    glr_init_trace_set_detailed(1);
    capture_begin();
    glr_init_trace_detail("PHASE_GAMMA");
    capture_end(buf, sizeof(buf));

    ASSERT_TRUE("detail phase emitted when flag on",
                strstr(buf, "PHASE_GAMMA") != NULL);

    glr_init_trace_set_detailed(0);
}

int main(void) {
    test_detailed_flag();
    test_elapsed_clock();
    test_baseline_phase_always_emits();
    test_detail_phase_gated_on_flag();

    return test_harness_report(&g_harness, "glr_init_trace");
}
