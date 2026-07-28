/*
 * test_gpuprof.c - unit tests for the GPU timer-query profiler.
 *
 * Include-as-unit: pulls in support/gpuprof.c with a tiny segment cap and
 * ring so the overflow and ring-exhaustion paths are reachable, and drives
 * it with scripted fake GL query functions — no GL anywhere. The fakes
 * model the contract gpuprof relies on: gen hands out ids, results are
 * keyed by id, availability is global (queries complete in order).
 */
#include <stdio.h>
#include <math.h>
#include "support/test_harness.h"

#include <string.h>

#define GPU_PROF_MAX_SEGMENTS 8
#define GPU_PROF_RING_SLOTS   2
#include "support/gpuprof.c"

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT_EQ(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

#define ASSERT_NEAR(label, got, exp) \
    TEST_ASSERT_TRUE(&g_harness, label, fabs((got) - (exp)) < 1e-9)

/* ========================================================================= */
/* Fake GL query functions                                                    */
/* ========================================================================= */

#define FAKE_MAX_QUERIES 64

static unsigned long long g_fake_result_ns[FAKE_MAX_QUERIES + 1]; /* by id */
static int      g_fake_available = 0;   /* global: all submitted results ready? */
static unsigned g_fake_next_id = 1;
static unsigned g_fake_active_id = 0;   /* nonzero while a query is running */
static int      g_fake_begin_calls = 0;
static int      g_fake_end_calls = 0;
static int      g_fake_unbalanced = 0;  /* begin while active / end while idle */
static unsigned g_fake_begin_order[FAKE_MAX_QUERIES];
static int      g_fake_begin_order_count = 0;

static void fake_gen_queries(int n, unsigned *ids) {
    for (int i = 0; i < n; i++)
        ids[i] = g_fake_next_id++;
}

static void fake_delete_queries(int n, const unsigned *ids) {
    (void)n; (void)ids;
}

static void fake_begin_query(unsigned target, unsigned id) {
    (void)target;
    if (g_fake_active_id != 0) g_fake_unbalanced = 1;
    g_fake_active_id = id;
    g_fake_begin_calls++;
    if (g_fake_begin_order_count < FAKE_MAX_QUERIES)
        g_fake_begin_order[g_fake_begin_order_count++] = id;
}

static void fake_end_query(unsigned target) {
    (void)target;
    if (g_fake_active_id == 0) g_fake_unbalanced = 1;
    g_fake_active_id = 0;
    g_fake_end_calls++;
}

static void fake_get_query_iv(unsigned id, unsigned pname, int *params) {
    (void)id; (void)pname;
    *params = g_fake_available;
}

static void fake_get_query_ui64v(unsigned id, unsigned pname, GpuProfU64 *params) {
    (void)pname;
    *params = (id <= FAKE_MAX_QUERIES) ? g_fake_result_ns[id] : 0;
}

/* Timestamp markers reuse the begin-order log (a marker is "issued" the
 * same way a bracket query is). */
static void fake_query_counter(unsigned id, unsigned target) {
    (void)target;
    g_fake_begin_calls++;
    if (g_fake_begin_order_count < FAKE_MAX_QUERIES)
        g_fake_begin_order[g_fake_begin_order_count++] = id;
}

static GpuProfGlFns fake_fns(void) {
    GpuProfGlFns fns;
    fns.gen_queries     = fake_gen_queries;
    fns.delete_queries  = fake_delete_queries;
    fns.begin_query     = fake_begin_query;
    fns.end_query       = fake_end_query;
    fns.get_query_iv    = fake_get_query_iv;
    fns.get_query_ui64v = fake_get_query_ui64v;
    fns.query_counter   = 0;   /* elapsed mode unless a test opts in */
    return fns;
}

static void reset_fake_state(void) {
    gpu_prof_shutdown();
    memset(g_fake_result_ns, 0, sizeof(g_fake_result_ns));
    g_fake_available = 0;
    g_fake_next_id = 1;
    g_fake_active_id = 0;
    g_fake_begin_calls = 0;
    g_fake_end_calls = 0;
    g_fake_unbalanced = 0;
    g_fake_begin_order_count = 0;
}

/* Fresh profiler + fresh fake state for every test. */
static void reset_and_init(void) {
    reset_fake_state();
    GpuProfGlFns fns = fake_fns();
    gpu_prof_init(&fns);
}

static void reset_and_init_timestamps(void) {
    reset_fake_state();
    GpuProfGlFns fns = fake_fns();
    fns.query_counter = fake_query_counter;
    gpu_prof_init(&fns);
}

/* The two sections used throughout; arbitrary distinct catalog ids. */
static const ProfSection SEC_A = (ProfSection)1;
static const ProfSection SEC_B = (ProfSection)2;
static const ProfSection SEC_HIGH = PROF_TEST_HIGH;

/* ========================================================================= */
/* Tests                                                                      */
/* ========================================================================= */

static void test_disabled_without_init(void) {
    gpu_prof_shutdown();
    ASSERT_INT_EQ("disabled by default", gpu_prof_enabled(), 0);

    GpuProfGlFns missing = fake_fns();
    missing.get_query_ui64v = NULL;
    ASSERT_INT_EQ("init rejects partial table", gpu_prof_init(&missing), 0);
    ASSERT_INT_EQ("init NULL rejected", gpu_prof_init(NULL), 0);

    /* Everything no-ops cleanly while disabled. */
    gpu_prof_frame_begin();
    gpu_prof_begin(SEC_A);
    gpu_prof_end(SEC_A);
    ASSERT_INT_EQ("no data while disabled", gpu_prof_section_has_data(SEC_A), 0);
    ASSERT_NEAR("avg 0 while disabled", gpu_prof_section_avg_us(SEC_A), 0.0);
}

static void test_nested_sections_credit_inclusive_time(void) {
    reset_and_init();
    ASSERT_INT_EQ("init enables", gpu_prof_enabled(), 1);

    gpu_prof_frame_begin();
    /* A wraps B: three segments — {A}, {A,B}, {A} — on slot 0's ids 1,2,3. */
    gpu_prof_begin(SEC_A);
    gpu_prof_begin(SEC_B);
    gpu_prof_end(SEC_B);
    gpu_prof_end(SEC_A);
    ASSERT_INT_EQ("three segments issued", g_fake_begin_calls, 3);
    ASSERT_INT_EQ("queries balanced", g_fake_unbalanced, 0);
    ASSERT_INT_EQ("no query left running", (int)g_fake_active_id, 0);

    g_fake_result_ns[g_fake_begin_order[0]] = 100;
    g_fake_result_ns[g_fake_begin_order[1]] = 200;
    g_fake_result_ns[g_fake_begin_order[2]] = 300;
    g_fake_available = 1;
    gpu_prof_frame_begin();   /* closes the frame and harvests it */

    ASSERT_INT_EQ("A has data", gpu_prof_section_has_data(SEC_A), 1);
    ASSERT_INT_EQ("B has data", gpu_prof_section_has_data(SEC_B), 1);
    ASSERT_NEAR("A = all three segments", gpu_prof_section_last_us(SEC_A), 0.6);
    ASSERT_NEAR("B = middle segment only", gpu_prof_section_last_us(SEC_B), 0.2);
    ASSERT_NEAR("avg seeded with first sample",
                gpu_prof_section_avg_us(SEC_A), 0.6);
    ASSERT_INT_EQ("untouched section has no data",
                  gpu_prof_section_has_data((ProfSection)3), 0);
}

static void test_nested_section_above_63(void) {
    reset_and_init();

    /* Same inclusive shape as A > B, but the child lives in word 1 of the
     * catalog-sized section set. */
    gpu_prof_frame_begin();
    gpu_prof_begin(SEC_A);
    gpu_prof_begin(SEC_HIGH);
    gpu_prof_end(SEC_HIGH);
    gpu_prof_end(SEC_A);

    g_fake_result_ns[g_fake_begin_order[0]] = 1000;
    g_fake_result_ns[g_fake_begin_order[1]] = 2000;
    g_fake_result_ns[g_fake_begin_order[2]] = 3000;
    g_fake_available = 1;
    gpu_prof_frame_begin();

    ASSERT_NEAR("low-word parent receives all nested intervals",
                gpu_prof_section_last_us(SEC_A), 6.0);
    ASSERT_NEAR("high-word child receives its interval",
                gpu_prof_section_last_us(SEC_HIGH), 2.0);
    ASSERT_INT_EQ("high-word child publishes data",
                  gpu_prof_section_has_data(SEC_HIGH), 1);
}

static void test_repeated_brackets_accumulate(void) {
    reset_and_init();

    gpu_prof_frame_begin();
    /* Two flat begin/end pairs for one section in a frame (the accum-loop
     * shape) sum into one sample. */
    gpu_prof_begin(SEC_A);
    gpu_prof_end(SEC_A);
    gpu_prof_begin(SEC_A);
    gpu_prof_end(SEC_A);
    g_fake_result_ns[g_fake_begin_order[0]] = 1000;
    g_fake_result_ns[g_fake_begin_order[1]] = 2500;
    g_fake_available = 1;
    gpu_prof_frame_begin();

    ASSERT_NEAR("passes sum", gpu_prof_section_last_us(SEC_A), 3.5);
}

static void test_delayed_results_and_ring_exhaustion(void) {
    reset_and_init();

    /* Frame 1 submits; results stay unavailable. */
    gpu_prof_frame_begin();
    gpu_prof_begin(SEC_A);
    gpu_prof_end(SEC_A);
    g_fake_result_ns[g_fake_begin_order[0]] = 5000;

    /* Frame 2: nothing harvestable yet, but the second ring slot opens. */
    gpu_prof_frame_begin();
    ASSERT_INT_EQ("no data before results land",
                  gpu_prof_section_has_data(SEC_A), 0);
    gpu_prof_begin(SEC_A);
    gpu_prof_end(SEC_A);
    g_fake_result_ns[g_fake_begin_order[1]] = 5000;
    int begin_calls_after_two_frames = g_fake_begin_calls;

    /* Frame 3: both (RING=2) slots in flight -> frame goes unsampled
     * instead of blocking; begins issue no queries. */
    gpu_prof_frame_begin();
    gpu_prof_begin(SEC_A);
    gpu_prof_end(SEC_A);
    ASSERT_INT_EQ("ring-full frame issues no queries",
                  g_fake_begin_calls, begin_calls_after_two_frames);

    /* Results arrive; the next frame harvests both back-to-back. */
    g_fake_available = 1;
    gpu_prof_frame_begin();
    ASSERT_INT_EQ("data after results land", gpu_prof_section_has_data(SEC_A), 1);
    ASSERT_NEAR("delayed sample harvested",
                gpu_prof_section_last_us(SEC_A), 5.0);

    /* And the ring has room again: sampling resumes. */
    gpu_prof_begin(SEC_A);
    gpu_prof_end(SEC_A);
    ASSERT_TRUE("sampling resumes after harvest",
                g_fake_begin_calls > begin_calls_after_two_frames);
}

static void test_segment_overflow_discards_frame(void) {
    reset_and_init();

    gpu_prof_frame_begin();
    /* MAX_SEGMENTS is 8 here: the 9th flat pair must overflow the slot. */
    for (int pair_idx = 0; pair_idx < GPU_PROF_MAX_SEGMENTS + 1; pair_idx++) {
        gpu_prof_begin(SEC_A);
        gpu_prof_end(SEC_A);
    }
    ASSERT_INT_EQ("issues stop at the cap",
                  g_fake_begin_calls, GPU_PROF_MAX_SEGMENTS);
    ASSERT_INT_EQ("queries stay balanced through overflow",
                  g_fake_unbalanced, 0);

    g_fake_available = 1;
    gpu_prof_frame_begin();
    ASSERT_INT_EQ("overflowed frame is discarded",
                  gpu_prof_section_has_data(SEC_A), 0);

    /* The slot is recycled clean: the next frame samples normally. */
    gpu_prof_begin(SEC_A);
    gpu_prof_end(SEC_A);
    g_fake_result_ns[g_fake_begin_order[g_fake_begin_order_count - 1]] = 7000;
    gpu_prof_frame_begin();
    ASSERT_INT_EQ("clean frame after overflow has data",
                  gpu_prof_section_has_data(SEC_A), 1);
    ASSERT_NEAR("clean frame value", gpu_prof_section_last_us(SEC_A), 7.0);
}

static void test_unbalanced_begin_discards_frame(void) {
    reset_and_init();

    gpu_prof_frame_begin();
    gpu_prof_begin(SEC_A);   /* never ended */
    g_fake_available = 1;
    gpu_prof_frame_begin();  /* must close the dangling query and poison the slot */
    ASSERT_INT_EQ("dangling query force-ended", (int)g_fake_active_id, 0);
    ASSERT_INT_EQ("queries balanced after recovery", g_fake_unbalanced, 0);
    gpu_prof_frame_begin();
    ASSERT_INT_EQ("unbalanced frame is discarded",
                  gpu_prof_section_has_data(SEC_A), 0);
}

static void test_timestamp_mode_nested(void) {
    reset_and_init_timestamps();
    ASSERT_INT_EQ("timestamp mode active", gpu_prof_uses_timestamps(), 1);

    gpu_prof_frame_begin();
    /* A wraps B: four markers bound intervals {A}, {A,B}, {A}. */
    gpu_prof_begin(SEC_A);
    gpu_prof_begin(SEC_B);
    gpu_prof_end(SEC_B);
    gpu_prof_end(SEC_A);
    ASSERT_INT_EQ("four markers issued", g_fake_begin_calls, 4);
    ASSERT_INT_EQ("no bracket queries used", g_fake_end_calls, 0);

    g_fake_result_ns[g_fake_begin_order[0]] = 1000;
    g_fake_result_ns[g_fake_begin_order[1]] = 1300;
    g_fake_result_ns[g_fake_begin_order[2]] = 1600;
    g_fake_result_ns[g_fake_begin_order[3]] = 2000;
    g_fake_available = 1;
    gpu_prof_frame_begin();

    /* Deltas tile the timeline: A is exactly last - first, additive. */
    ASSERT_NEAR("A = whole run window", gpu_prof_section_last_us(SEC_A), 1.0);
    ASSERT_NEAR("B = inner interval", gpu_prof_section_last_us(SEC_B), 0.3);
}

static void test_timestamp_mode_gap_uncredited(void) {
    reset_and_init_timestamps();

    gpu_prof_frame_begin();
    /* Two separate top-level runs; the gap between them belongs to
     * neither (its interval mask is 0). */
    gpu_prof_begin(SEC_A);
    gpu_prof_end(SEC_A);
    gpu_prof_begin(SEC_B);
    gpu_prof_end(SEC_B);
    ASSERT_INT_EQ("a marker per transition", g_fake_begin_calls, 4);

    g_fake_result_ns[g_fake_begin_order[0]] = 0;
    g_fake_result_ns[g_fake_begin_order[1]] = 100;
    g_fake_result_ns[g_fake_begin_order[2]] = 500;  /* 400ns idle gap */
    g_fake_result_ns[g_fake_begin_order[3]] = 800;
    g_fake_available = 1;
    gpu_prof_frame_begin();

    ASSERT_NEAR("A gets its run only", gpu_prof_section_last_us(SEC_A), 0.1);
    ASSERT_NEAR("B gets its run only", gpu_prof_section_last_us(SEC_B), 0.3);
}

static void test_elapsed_mode_reports_no_timestamps(void) {
    reset_and_init();
    ASSERT_INT_EQ("elapsed mode without query_counter",
                  gpu_prof_uses_timestamps(), 0);
    gpu_prof_shutdown();
    ASSERT_INT_EQ("flag cleared on shutdown", gpu_prof_uses_timestamps(), 0);
}

static void test_ema_smoothing(void) {
    reset_and_init();

    /* First sample seeds the EMA; the second nudges it by the alpha. */
    gpu_prof_frame_begin();
    gpu_prof_begin(SEC_A);
    gpu_prof_end(SEC_A);
    g_fake_result_ns[g_fake_begin_order[0]] = 1000;
    g_fake_available = 1;
    gpu_prof_frame_begin();
    ASSERT_NEAR("EMA seeded", gpu_prof_section_avg_us(SEC_A), 1.0);

    gpu_prof_begin(SEC_A);
    gpu_prof_end(SEC_A);
    g_fake_result_ns[g_fake_begin_order[g_fake_begin_order_count - 1]] = 2000;
    gpu_prof_frame_begin();
    ASSERT_NEAR("EMA blends second sample",
                gpu_prof_section_avg_us(SEC_A),
                GPU_PROF_EMA_ALPHA * 2.0 + (1.0 - GPU_PROF_EMA_ALPHA) * 1.0);
    ASSERT_NEAR("last is unsmoothed", gpu_prof_section_last_us(SEC_A), 2.0);
}

int main(void) {
    printf("--- gpuprof tests ---\n");
    test_disabled_without_init();
    test_nested_sections_credit_inclusive_time();
    test_nested_section_above_63();
    test_repeated_brackets_accumulate();
    test_delayed_results_and_ring_exhaustion();
    test_segment_overflow_discards_frame();
    test_unbalanced_begin_discards_frame();
    test_timestamp_mode_nested();
    test_timestamp_mode_gap_uncredited();
    test_elapsed_mode_reports_no_timestamps();
    test_ema_smoothing();
    printf("\n");
    return test_harness_report(&g_harness, "test_gpuprof");
}
