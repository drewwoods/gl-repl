/*
 * test_ui_cpuprof.c
 */
#ifdef GL_STUBS
#include "ui/support/cpuprof.h"
#include "support/cpuprof.h"
#include "app/glr_prof.h"
#include "support/test_harness.h"
#include <GL/gl_stub_counts.h>
#endif

#include <stdlib.h>   /* abs */
#include <stdio.h>
#include <string.h>

#ifdef GL_STUBS
static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT_EQ(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

/* The bins are log-spaced, so tests ask the profiler where a duration lands
 * rather than reimplementing the mapping. prof_histogram_bin_for_us is tested
 * directly in test_cpuprof_log_bin_layout. */
static int hist_bin_for_us(double us) {
    return prof_histogram_bin_for_us(us);
}

static int hist_sum(const ProfHistogramBin *bins) {
    int total = 0;
    for (int i = 0; i < PROF_HISTOGRAM_BIN_COUNT; i++)
        total += (int)bins[i];
    return total;
}

static void test_cpuprof_metrics(void) {
    ASSERT_TRUE("width is positive", ui_profile_panel_width() > 0);
    ASSERT_INT_EQ("FPS mode has no section-list height",
                  ui_profile_panel_height(PROFILE_PANEL_FPS), 0);
    ASSERT_TRUE("sections height is positive",
                ui_profile_panel_height(PROFILE_PANEL_SECTIONS) > 0);
    ASSERT_TRUE("histogram listing height is positive",
                ui_profile_panel_height(PROFILE_PANEL_HISTOGRAM) > 0);
}

static void test_cpuprof_section_collapse(void) {
    UiProfileCollapseMask collapsed = 0;
    int expanded_h = ui_profile_panel_height_collapsed(PROFILE_PANEL_SECTIONS,
                                                       collapsed);

    collapsed = ui_profile_panel_toggle_mask(collapsed, PROF_CODE_PANEL);
    ASSERT_INT_EQ("collapsing Code Panel hides its sixteen descendants",
                  expanded_h - ui_profile_panel_height_collapsed(
                      PROFILE_PANEL_SECTIONS, collapsed),
                  16 * 16);
    ASSERT_TRUE("collapsed branch bit is retained",
                collapsed != 0);

    collapsed = ui_profile_panel_toggle_mask(collapsed, PROF_CODE_PANEL);
    ASSERT_INT_EQ("toggling a branch again restores full height",
                  ui_profile_panel_height_collapsed(PROFILE_PANEL_SECTIONS,
                                                    collapsed),
                  expanded_h);
    ASSERT_TRUE("second branch toggle clears its bit", collapsed == 0);

    collapsed = ui_profile_panel_toggle_mask(collapsed,
                                              UI_PROFILE_PANEL_TOGGLE_ALL);
    ASSERT_TRUE("collapse all reduces section-list height",
                ui_profile_panel_height_collapsed(PROFILE_PANEL_SECTIONS,
                                                  collapsed) < expanded_h);
    collapsed = ui_profile_panel_toggle_mask(collapsed,
                                              UI_PROFILE_PANEL_TOGGLE_ALL);
    ASSERT_TRUE("expand all clears branch mask", collapsed == 0);

    /* Both section-bearing modes render the same collapsible listing. */
    collapsed = ui_profile_panel_toggle_mask(0, PROF_RENDER3D);
    ASSERT_TRUE("sections height honors collapsed branches",
                ui_profile_panel_height_collapsed(PROFILE_PANEL_SECTIONS,
                                                  collapsed) < expanded_h);
    ASSERT_INT_EQ("histogram mode shares the collapsed listing height",
                  ui_profile_panel_height_collapsed(PROFILE_PANEL_HISTOGRAM,
                                                    collapsed),
                  ui_profile_panel_height_collapsed(PROFILE_PANEL_SECTIONS,
                                                    collapsed));
}

static void test_cpuprof_section_hit_test(void) {
    UiProfilePanelView view = {
        .window_w = 800,
        .window_h = 600,
        .mode = PROFILE_PANEL_SECTIONS,
        .collapsed_sections = 0,
        .panel_x = 10,
        .panel_y = 10
    };
    int panel_h = ui_profile_panel_height_collapsed(
        view.mode, view.collapsed_sections);
    int first_row_y = view.panel_y + panel_h - 54;

    ASSERT_INT_EQ("first section branch row hits Render 3D",
                  ui_profile_panel_hit_test(
                      &view, view.panel_x + 12,
                      view.window_h - (first_row_y + 4)),
                  PROF_RENDER3D);
    ASSERT_INT_EQ("section title control hits collapse all",
                  ui_profile_panel_hit_test(
                      &view, view.panel_x + PROFILE_PANEL_W - 4,
                      view.window_h - (view.panel_y + panel_h - 10)),
                  UI_PROFILE_PANEL_TOGGLE_ALL);

    view.mode = PROFILE_PANEL_HISTOGRAM;
    ASSERT_INT_EQ("histogram mode keeps section-tree hit targets",
                  ui_profile_panel_hit_test(
                      &view, view.panel_x + 12,
                      view.window_h - (first_row_y + 4)),
                  PROF_RENDER3D);

    view.mode = PROFILE_PANEL_FPS;
    ASSERT_INT_EQ("FPS mode has no section-tree hit targets",
                  ui_profile_panel_hit_test(
                      &view, view.panel_x + 12,
                      view.window_h - (first_row_y + 4)),
                  UI_PROFILE_PANEL_HIT_NONE);
}

static void test_gpu_section_policy(void) {
    /* GL-emitting sections are GPU-bracketed... */
    ASSERT_INT_EQ("scene 3d is gpu", glr_prof_section_is_gpu(PROF_RENDER3D), 1);
    ASSERT_INT_EQ("accum effect is gpu",
                  glr_prof_section_is_gpu(PROF_RENDER3D_ACCUM_EFFECT), 1);
    ASSERT_INT_EQ("code panel is gpu", glr_prof_section_is_gpu(PROF_CODE_PANEL), 1);
    ASSERT_INT_EQ("frame total is gpu", glr_prof_section_is_gpu(PROF_FRAME_TOTAL), 1);
    /* ...pure-CPU sections and the per-fade-batch budget exclusions are not. */
    ASSERT_INT_EQ("flatten is cpu-only", glr_prof_section_is_gpu(PROF_FLATTEN), 0);
    ASSERT_INT_EQ("rebake eval is cpu-only",
                  glr_prof_section_is_gpu(PROF_REBAKE_EVAL), 0);
    ASSERT_INT_EQ("snapshot is cpu-only", glr_prof_section_is_gpu(PROF_SNAPSHOT), 0);
    ASSERT_INT_EQ("frame restore is cpu-only",
                  glr_prof_section_is_gpu(PROF_FRAME_RESTORE), 0);
    ASSERT_INT_EQ("fade batch exec excluded",
                  glr_prof_section_is_gpu(PROF_RENDER3D_FADE_BATCH_EXEC), 0);
    ASSERT_INT_EQ("code panel build rows excluded",
                  glr_prof_section_is_gpu(PROF_CODE_PANEL_ROWS), 0);
    ASSERT_INT_EQ("code panel text layout excluded",
                  glr_prof_section_is_gpu(PROF_CODE_PANEL_TEXT_LAYOUT), 0);
    ASSERT_INT_EQ("profile fps child excluded",
                  glr_prof_section_is_gpu(PROF_PROFILE_PANEL_FPS), 0);
    ASSERT_INT_EQ("profile section-list child excluded",
                  glr_prof_section_is_gpu(PROF_PROFILE_PANEL_SECTIONS), 0);
    ASSERT_INT_EQ("profile histogram child excluded",
                  glr_prof_section_is_gpu(PROF_PROFILE_PANEL_HISTOGRAM), 0);
    ASSERT_INT_EQ("out-of-range section rejected",
                  glr_prof_section_is_gpu((ProfSection)-1), 0);
}

static void test_cpuprof_section_histogram_direct(void) {
    ProfHistogramBin bins[PROF_HISTOGRAM_BIN_COUNT];

    prof_test_reset();
    prof_test_set_now_us(1000.0);
    prof_begin(PROF_FLATTEN);
    prof_test_set_now_us(51000.0);
    prof_end(PROF_FLATTEN);

    ASSERT_INT_EQ("section histogram copies full bin count",
                  prof_section_histogram(PROF_FLATTEN, bins,
                                         PROF_HISTOGRAM_BIN_COUNT),
                  PROF_HISTOGRAM_BIN_COUNT);
    ASSERT_INT_EQ("50ms section sample lands in its log bin",
                  bins[hist_bin_for_us(50000.0)], 1);
    ASSERT_INT_EQ("direct section histogram has one sample",
                  hist_sum(bins), 1);
    ASSERT_INT_EQ("invalid section histogram rejected",
                  prof_section_histogram((ProfSection)-1, bins,
                                         PROF_HISTOGRAM_BIN_COUNT),
                  0);
}

static void test_cpuprof_section_histogram_accum_commit(void) {
    ProfHistogramBin bins[PROF_HISTOGRAM_BIN_COUNT];

    prof_test_reset();
    prof_accum_reset(PROF_RENDER3D_SETUP);

    prof_test_set_now_us(1000.0);
    prof_begin(PROF_RENDER3D_SETUP);
    prof_test_set_now_us(11000.0);
    prof_accum_end(PROF_RENDER3D_SETUP);

    prof_test_set_now_us(20000.0);
    prof_begin(PROF_RENDER3D_SETUP);
    prof_test_set_now_us(50000.0);
    prof_accum_end(PROF_RENDER3D_SETUP);

    prof_accum_commit(PROF_RENDER3D_SETUP);

    prof_section_histogram(PROF_RENDER3D_SETUP, bins,
                           PROF_HISTOGRAM_BIN_COUNT);
    ASSERT_INT_EQ("accum commit records summed 40ms sample",
                  bins[hist_bin_for_us(40000.0)], 1);
    ASSERT_INT_EQ("accum section histogram records one committed sample",
                  hist_sum(bins), 1);

    prof_accum_commit(PROF_RENDER3D_SETUP);
    prof_section_histogram(PROF_RENDER3D_SETUP, bins,
                           PROF_HISTOGRAM_BIN_COUNT);
    ASSERT_INT_EQ("duplicate accum commit does not duplicate histogram sample",
                  hist_sum(bins), 1);

    prof_accum_reset(PROF_RENDER3D_SETUP);
    prof_accum_commit(PROF_RENDER3D_SETUP);
    prof_section_histogram(PROF_RENDER3D_SETUP, bins,
                           PROF_HISTOGRAM_BIN_COUNT);
    ASSERT_INT_EQ("empty accum commit does not add zero histogram sample",
                  hist_sum(bins), 1);
}

static void test_cpuprof_frame_time_histogram(void) {
    ProfHistogramBin bins[PROF_HISTOGRAM_BIN_COUNT];
    ProfHistogramBin small[4];

    prof_test_reset();
    prof_test_set_now_us(1000.0);
    prof_frame_tick();
    prof_test_set_now_us(18000.0);
    prof_frame_tick();

    ASSERT_INT_EQ("frame histogram copies full bin count",
                  prof_frame_time_histogram(bins,
                                            PROF_HISTOGRAM_BIN_COUNT),
                  PROF_HISTOGRAM_BIN_COUNT);
    ASSERT_INT_EQ("17ms frame sample lands in its log bin",
                  bins[hist_bin_for_us(17000.0)], 1);
    ASSERT_INT_EQ("frame histogram has one sample",
                  hist_sum(bins), 1);
    ASSERT_INT_EQ("frame histogram supports partial copies",
                  prof_frame_time_histogram(small, 4), 4);
}

static void test_cpuprof_current_fps_averages_intervals(void) {
    double now = 1000.0;

    prof_test_reset();
    prof_test_set_now_us(now);
    prof_frame_tick();

    /* These alternating intervals cover 16.667 ms per pair: 120 callbacks/s
     * despite their uneven spacing. EMA(1 / dt) converges near 164 FPS;
     * 1 / EMA(dt) remains centered on the actual callback throughput. */
    for (int i = 0; i < 400; i++) {
        now += (i & 1) ? 12666.666666 : 4000.0;
        prof_test_set_now_us(now);
        prof_frame_tick();
    }

    ASSERT_TRUE("current FPS is not inflated by uneven frame spacing",
                prof_fps_current() > 115.0 && prof_fps_current() < 125.0);
}

static void test_cpuprof_histogram_saturates_16_bit_bins(void) {
    ProfHistogramBin bins[PROF_HISTOGRAM_BIN_COUNT];

    prof_test_reset();
    prof_test_set_now_us(0.0);
    for (int i = 0; i < 65536; i++) {
        prof_begin(PROF_REFORMAT);
        prof_end(PROF_REFORMAT);
    }

    prof_section_histogram(PROF_REFORMAT, bins,
                           PROF_HISTOGRAM_BIN_COUNT);
    ASSERT_INT_EQ("histogram bin saturates at 16-bit max",
                  bins[0], 65535);
}

static void test_cpuprof_histogram_reset(void) {
    ProfHistogramBin bins[PROF_HISTOGRAM_BIN_COUNT];

    prof_test_reset();
    prof_test_set_now_us(1000.0);
    prof_begin(PROF_FLATTEN);
    prof_test_set_now_us(51000.0);
    prof_end(PROF_FLATTEN);
    prof_frame_tick();
    prof_test_set_now_us(68000.0);
    prof_frame_tick();

    prof_section_histogram(PROF_FLATTEN, bins, PROF_HISTOGRAM_BIN_COUNT);
    ASSERT_INT_EQ("section histogram has a sample before reset",
                  hist_sum(bins), 1);
    prof_frame_time_histogram(bins, PROF_HISTOGRAM_BIN_COUNT);
    ASSERT_INT_EQ("frame histogram has a sample before reset",
                  hist_sum(bins), 1);

    double avg_before = prof_section_avg_us(PROF_FLATTEN);
    prof_histogram_reset();

    prof_section_histogram(PROF_FLATTEN, bins, PROF_HISTOGRAM_BIN_COUNT);
    ASSERT_INT_EQ("reset clears the section histogram", hist_sum(bins), 0);
    prof_frame_time_histogram(bins, PROF_HISTOGRAM_BIN_COUNT);
    ASSERT_INT_EQ("reset clears the frame-time histogram", hist_sum(bins), 0);

    /* Only the cumulative data is dropped: the EMAs and staleness re-converge
     * on their own and must survive, or every example switch would blank the
     * listing panel's columns. */
    ASSERT_TRUE("reset leaves the section EMA intact",
                prof_section_avg_us(PROF_FLATTEN) == avg_before);
    ASSERT_INT_EQ("reset leaves section staleness intact",
                  prof_section_is_stale(PROF_FLATTEN), 0);

    /* Post-reset samples land in a fresh distribution. */
    prof_test_set_now_us(100000.0);
    prof_begin(PROF_FLATTEN);
    prof_test_set_now_us(100000.0 + 20000.0);
    prof_end(PROF_FLATTEN);
    prof_section_histogram(PROF_FLATTEN, bins, PROF_HISTOGRAM_BIN_COUNT);
    ASSERT_INT_EQ("post-reset sample lands in its own bin",
                  bins[hist_bin_for_us(20000.0)], 1);
    ASSERT_INT_EQ("post-reset histogram holds only the new sample",
                  hist_sum(bins), 1);
    prof_test_reset();
}

/* The log bin layout: constant *relative* width, underflow/overflow clamps,
 * and edges that agree with the mapping. This is what lets a 3 us section and
 * a 30 ms one both be measured precisely. */
static void test_cpuprof_log_bin_layout(void) {
    ASSERT_INT_EQ("zero lands in the underflow bin",
                  prof_histogram_bin_for_us(0.0), 0);
    ASSERT_INT_EQ("a negative duration lands in the underflow bin",
                  prof_histogram_bin_for_us(-5.0), 0);
    ASSERT_INT_EQ("sub-minimum durations land in the underflow bin",
                  prof_histogram_bin_for_us(PROF_HISTOGRAM_MIN_US * 0.25), 0);
    ASSERT_INT_EQ("the minimum itself is the underflow bin's lower edge",
                  prof_histogram_bin_for_us(PROF_HISTOGRAM_MIN_US), 0);
    ASSERT_INT_EQ("the maximum lands in the overflow bin",
                  prof_histogram_bin_for_us(PROF_HISTOGRAM_MAX_US),
                  PROF_HISTOGRAM_BIN_COUNT - 1);
    ASSERT_INT_EQ("beyond the maximum stays in the overflow bin",
                  prof_histogram_bin_for_us(PROF_HISTOGRAM_MAX_US * 1000.0),
                  PROF_HISTOGRAM_BIN_COUNT - 1);

    /* Monotonic, and each decade is the same number of bins apart. */
    int b_1us   = prof_histogram_bin_for_us(1.0);
    int b_10us  = prof_histogram_bin_for_us(10.0);
    int b_100us = prof_histogram_bin_for_us(100.0);
    int b_1ms   = prof_histogram_bin_for_us(1000.0);
    ASSERT_TRUE("bins increase with duration",
                b_1us < b_10us && b_10us < b_100us && b_100us < b_1ms);
    /* A decade is 1024/6 = 170.67 bins, so consecutive decades differ by one
     * bin depending on where the floor lands. Even to within that. */
    ASSERT_TRUE("decades are evenly spaced (1us->10us vs 10us->100us)",
                abs((b_10us - b_1us) - (b_100us - b_10us)) <= 1);
    ASSERT_TRUE("decades are evenly spaced (100us->1ms)",
                abs((b_1ms - b_100us) - (b_100us - b_10us)) <= 1);

    /* The sub-100us sections a linear layout could not tell apart. */
    ASSERT_TRUE("3us and 30us occupy different bins",
                prof_histogram_bin_for_us(3.0) != prof_histogram_bin_for_us(30.0));
    ASSERT_TRUE("6us and 128us occupy different bins",
                prof_histogram_bin_for_us(6.0) != prof_histogram_bin_for_us(128.0));

    /* Edges are consistent with the mapping. */
    for (int b = 1; b < PROF_HISTOGRAM_BIN_COUNT - 1; b += 97) {
        double lo = prof_histogram_bin_lo_us(b);
        double hi = prof_histogram_bin_hi_us(b);
        ASSERT_TRUE("bin edges ascend", hi > lo);
        ASSERT_INT_EQ("a bin's lower edge maps back to that bin",
                      prof_histogram_bin_for_us(lo), b);
        ASSERT_INT_EQ("a bin's upper edge maps to the next bin",
                      prof_histogram_bin_for_us(hi), b + 1);
    }
}

/* The axis spans the occupied bins. An outlier extends it, but only by the
 * bounded number of bins that separates it on a log scale — the whole reason
 * the panel needs no percentile trim. */
static void test_histogram_axis_range(void) {
    int lo = -1, hi = -1;

    prof_test_reset();
    ASSERT_INT_EQ("no samples means no axis",
                  ui_histogram_axis_range(0ULL, &lo, &hi), 0);

    /* One section around 2 ms. */
    prof_test_reset();
    for (int i = 0; i < 50; i++) {
        prof_test_set_now_us((double)i * 100000.0);
        prof_begin(PROF_FRAME_TOTAL);
        prof_test_set_now_us((double)i * 100000.0 + 2000.0 + (double)(i % 10));
        prof_end(PROF_FRAME_TOTAL);
    }
    ASSERT_INT_EQ("a populated series yields an axis",
                  ui_histogram_axis_range(0ULL, &lo, &hi), 1);
    ASSERT_TRUE("axis spans at least the minimum width", hi - lo + 1 >= 64);
    ASSERT_TRUE("axis stays inside the bin range",
                lo >= 0 && hi <= PROF_HISTOGRAM_BIN_COUNT - 1);
    ASSERT_TRUE("axis contains the 2ms samples",
                prof_histogram_bin_for_us(2000.0) >= lo &&
                prof_histogram_bin_for_us(2000.0) <= hi);
    int hi_before = hi;

    /* Add one 200 ms stall: the axis must grow to include it, and by far less
     * than the 100x ratio in time — that is the log scale doing its job. */
    prof_test_set_now_us(9000000.0);
    prof_begin(PROF_FRAME_TOTAL);
    prof_test_set_now_us(9000000.0 + 200000.0);
    prof_end(PROF_FRAME_TOTAL);

    int lo_before = lo;
    ASSERT_INT_EQ("axis still resolves",
                  ui_histogram_axis_range(0ULL, &lo, &hi), 1);
    ASSERT_TRUE("the outlier is on-axis, not trimmed",
                hi >= prof_histogram_bin_for_us(200000.0));

    /* The whole point of log bins: a 100x-slower outlier pushes the axis out
     * by ~two decades' worth of bins, not by 100x its width. On the old linear
     * bins the same sample multiplied the axis span by ~100 and squashed the
     * 2 ms bulk into the leftmost pixel. */
    int decade_bins = prof_histogram_bin_for_us(100.0)
                    - prof_histogram_bin_for_us(10.0);
    int span_before = hi_before - lo_before + 1;
    int span_after  = hi - lo + 1;
    ASSERT_TRUE("the outlier widens the axis", span_after > span_before);
    ASSERT_TRUE("a 100x outlier costs about two decades of bins, not 100x",
                span_after <= span_before + 2 * decade_bins + 8);
    prof_test_reset();
}

/* A cheap section must not stop the axis from reaching a slow one, and the
 * cheap section must still land in its own bin rather than a shared floor. */
static void test_histogram_axis_covers_cheap_and_slow(void) {
    int lo = -1, hi = -1;

    prof_test_reset();
    for (int i = 0; i < 100; i++) {
        double base = (double)i * 100000.0;
        prof_test_set_now_us(base);
        prof_begin(PROF_FLATTEN);              /* ~6 us */
        prof_test_set_now_us(base + 6.0);
        prof_end(PROF_FLATTEN);

        prof_begin(PROF_FRAME_TOTAL);          /* ~2 ms */
        prof_test_set_now_us(base + 2000.0);
        prof_end(PROF_FRAME_TOTAL);
    }

    ASSERT_INT_EQ("axis resolves", ui_histogram_axis_range(0ULL, &lo, &hi), 1);
    ASSERT_TRUE("axis reaches the 6us section",
                prof_histogram_bin_for_us(6.0) >= lo);
    ASSERT_TRUE("axis reaches the 2ms section",
                prof_histogram_bin_for_us(2000.0) <= hi);
    ASSERT_TRUE("the two sections are distinguishable on the axis",
                prof_histogram_bin_for_us(2000.0)
                    - prof_histogram_bin_for_us(6.0) > 8);
    prof_test_reset();
}

/* The axis can carry ~1000 bins across ~338 plot pixels for a dozen series. A
 * silhouette vertex per column per series would be thousands of vertices, most
 * of them tracing the empty baseline between humps; equal-height runs collapse
 * instead. Guards against a rewrite quietly reinstating the per-column strip. */
static void test_histogram_silhouette_collapses_flat_runs(void) {
    UiHistogramPanelView view = {
        .window_w = 800, .window_h = 600,
        .visible = 1, .panel_x = 10, .panel_y = 10
    };
    /* Sections spread across four decades — the sparse, humped shape the
     * collapse exists for. */
    static const double dur_us[] = { 0.4, 6.0, 128.0, 560.0, 2090.0 };
    static const ProfSection secs[] = {
        PROF_FRAME_RESTORE, PROF_FLATTEN, PROF_SNAPSHOT,
        PROF_CODE_PANEL, PROF_FRAME_TOTAL
    };
    const int series = (int)(sizeof(secs) / sizeof(secs[0]));

    prof_test_reset();
    double now = 0.0;
    for (int frame = 0; frame < 200; frame++) {
        for (int k = 0; k < series; k++) {
            prof_test_set_now_us(now);
            prof_begin(secs[k]);
            now += dur_us[k] * (1.0 + 0.05 * (double)((frame % 7) - 3));
            prof_test_set_now_us(now);
            prof_end(secs[k]);
            now += 10.0;
        }
    }

    int lo = 0, hi = 0;
    ASSERT_INT_EQ("axis resolves", ui_histogram_axis_range(0ULL, &lo, &hi), 1);
    ASSERT_TRUE("axis is wide enough for the collapse to matter",
                hi - lo + 1 > 200);

    gl_stub_counts_reset();
    ui_histogram_panel_render(&view);

    /* A per-column strip alone would emit 2 verts x plot_w for EVERY plotted
     * series (all of the catalog's depth-0 rows, not just the 5 fed above), so
     * even one series' worth of naive vertices is a generous ceiling here. */
    unsigned long long naive_one_series =
        (unsigned long long)(2 * ui_histogram_panel_width());
    ASSERT_TRUE("silhouettes collapse flat runs",
                gl_stub_counts[GL_STUB_glVertex2f] < naive_one_series);
    ASSERT_TRUE("the panel still draws its series",
                gl_stub_counts[GL_STUB_glVertex2f] > 50);
    prof_test_reset();
}

static void test_histogram_panel_metrics(void) {
    ASSERT_TRUE("histogram width is positive", ui_histogram_panel_width() > 0);
    ASSERT_TRUE("histogram height is positive", ui_histogram_panel_height() > 0);

    /* Height is a function of the catalog, not of which sections happen to
     * have samples — otherwise the panel would resize under the overlay
     * stack as sections start and stop running. */
    int before = ui_histogram_panel_height();
    prof_test_reset();
    prof_test_set_now_us(0.0);
    prof_begin(PROF_FRAME_TOTAL);
    prof_test_set_now_us(5000.0);
    prof_end(PROF_FRAME_TOTAL);
    ASSERT_INT_EQ("histogram height is sample-independent",
                  ui_histogram_panel_height(), before);
}

static void test_histogram_reset_hit_test(void) {
    UiHistogramPanelView view = {
        .window_w = 800, .window_h = 600,
        .visible = 1, .panel_x = 10, .panel_y = 10
    };
    int panel_w = ui_histogram_panel_width();
    int panel_h = ui_histogram_panel_height();
    /* Title-row right edge — where the "[reset]" control renders. */
    int control_mx = view.panel_x + panel_w - 4;
    int header_my  = view.window_h - (view.panel_y + panel_h - 6);

    ASSERT_INT_EQ("header-right control hits reset",
                  ui_histogram_panel_hit_test(&view, control_mx, header_my),
                  UI_HISTOGRAM_PANEL_HIT_RESET);
    ASSERT_INT_EQ("header title side misses reset",
                  ui_histogram_panel_hit_test(&view, view.panel_x + 12,
                                              header_my),
                  UI_HISTOGRAM_PANEL_HIT_NONE);
    ASSERT_INT_EQ("plot body below the header misses reset",
                  ui_histogram_panel_hit_test(
                      &view, control_mx,
                      view.window_h - (view.panel_y + panel_h / 2)),
                  UI_HISTOGRAM_PANEL_HIT_NONE);
    ASSERT_INT_EQ("right of the panel misses reset",
                  ui_histogram_panel_hit_test(&view,
                                              view.panel_x + panel_w + 4,
                                              header_my),
                  UI_HISTOGRAM_PANEL_HIT_NONE);

    view.visible = 0;
    ASSERT_INT_EQ("hidden panel has no reset target",
                  ui_histogram_panel_hit_test(&view, control_mx, header_my),
                  UI_HISTOGRAM_PANEL_HIT_NONE);
}

/* A click on a legend swatch returns that series' ProfSection index, and the
 * pure toggle transition flips exactly that bit. The legend lives at the panel
 * bottom; its layout is hidden-independent, so the hit maps to the same series
 * whether or not it is currently plotted. */
static void test_histogram_legend_toggle(void) {
    UiHistogramPanelView view = {
        .window_w = 800, .window_h = 600,
        .visible = 1, .panel_x = 10, .panel_y = 10
    };
    int legend_rows;
    int tx = view.panel_x + 8;
    int series_before = 0;
    int hit_section;
    unsigned long long mask;

    /* Count plotted (depth-0) series to size the legend, matching the panel. */
    for (int s = 0; s < PROF_SECTION_COUNT; s++)
        if (prof_section_info((ProfSection)s).depth == 0 &&
            prof_section_info((ProfSection)s).label &&
            prof_section_info((ProfSection)s).label[0])
            series_before++;
    legend_rows = (series_before + 2 - 1) / 2;
    ASSERT_TRUE("catalog has legend series to toggle", series_before > 0);

    /* The first legend cell (ordinal 0 -> row 0, col 0) sits at the bottom
     * row's top. Aim at the swatch center. */
    {
        int ly = view.panel_y + 8 /* HIST_BOTTOM_PAD */
               + (legend_rows - 1) * 12 /* HIST_LEGEND_ROW_H */;
        int mx = tx + 3;                    /* over the swatch */
        int my = view.window_h - (ly + 6);  /* GLUT y-down */
        hit_section = ui_histogram_panel_hit_test(&view, mx, my);
    }
    ASSERT_TRUE("legend click returns a plotted series index",
                hit_section >= 0 && hit_section < PROF_SECTION_COUNT);
    ASSERT_INT_EQ("legend series is depth-0",
                  prof_section_info((ProfSection)hit_section).depth, 0);

    /* Pure transition: flip on, flip off, and reject junk indices. */
    mask = ui_histogram_panel_toggle_series(0ULL, hit_section);
    ASSERT_TRUE("toggle sets the series bit", mask != 0ULL);
    ASSERT_INT_EQ("double toggle clears the bit",
                  (int)ui_histogram_panel_toggle_series(mask, hit_section), 0);
    ASSERT_INT_EQ("out-of-range index leaves the mask untouched",
                  (int)ui_histogram_panel_toggle_series(0ULL, -1), 0);
    ASSERT_INT_EQ("non-plotted section leaves the mask untouched",
                  (int)ui_histogram_panel_toggle_series(0ULL,
                       PROF_SECTION_COUNT + 5), 0);
}

/* Hiding a populated series must remove its bars from the plot: with one of two
 * fed series masked off, the plot passes emit fewer vertices than with both
 * shown. */
static void test_histogram_hidden_series_drops_bars(void) {
    UiHistogramPanelView view = {
        .window_w = 800, .window_h = 600,
        .visible = 1, .panel_x = 10, .panel_y = 10
    };
    prof_test_reset();
    for (int i = 0; i < 30; i++) {
        prof_test_set_now_us((double)i * 100000.0);
        prof_begin(PROF_FRAME_TOTAL);
        prof_test_set_now_us((double)i * 100000.0 + 16000.0);
        prof_end(PROF_FRAME_TOTAL);

        prof_begin(PROF_FLATTEN);
        prof_test_set_now_us((double)i * 100000.0 + 20000.0);
        prof_end(PROF_FLATTEN);
    }

    view.hidden_series = 0;
    gl_stub_counts_reset();
    ui_histogram_panel_render(&view);
    unsigned long long shown_verts = gl_stub_counts[GL_STUB_glVertex2f];

    view.hidden_series =
        ui_histogram_panel_toggle_series(0ULL, (int)PROF_FRAME_TOTAL);
    gl_stub_counts_reset();
    ui_histogram_panel_render(&view);
    unsigned long long hidden_verts = gl_stub_counts[GL_STUB_glVertex2f];

    ASSERT_TRUE("hiding a series drops its plotted vertices",
                hidden_verts < shown_verts);
    ASSERT_TRUE("panel still draws the remaining series",
                hidden_verts > 0);
    prof_test_reset();
}

/* Every plotted (depth-0) series toggled off. */
static unsigned long long hist_all_series_hidden(void) {
    unsigned long long mask = 0ULL;
    for (int s = 0; s < PROF_SECTION_COUNT; s++)
        mask = ui_histogram_panel_toggle_series(mask, s);
    return mask;
}

/* The axis spans the plotted series only. A sub-microsecond section pins the
 * lower bound at bin 0 (1 us) for everyone; hiding it must let the axis climb
 * to the surviving distribution instead of leaving it crushed against the
 * right edge. */
static void test_histogram_axis_follows_hidden_series(void) {
    int lo = -1, hi = -1;
    int lo_all, hi_all;
    unsigned long long hide_fast;

    prof_test_reset();
    for (int i = 0; i < 40; i++) {
        double base = (double)i * 100000.0;
        prof_test_set_now_us(base);
        prof_begin(PROF_FRAME_RESTORE);        /* ~0.4 us -> bin 0 */
        prof_test_set_now_us(base + 0.4);
        prof_end(PROF_FRAME_RESTORE);

        prof_begin(PROF_FRAME_TOTAL);          /* ~2 ms */
        prof_test_set_now_us(base + 2000.0);
        prof_end(PROF_FRAME_TOTAL);
    }

    ASSERT_INT_EQ("axis resolves with both series shown",
                  ui_histogram_axis_range(0ULL, &lo, &hi), 1);
    lo_all = lo;
    hi_all = hi;
    ASSERT_INT_EQ("the sub-us series pins the lower bound at bin 0", lo_all, 0);

    hide_fast = ui_histogram_panel_toggle_series(0ULL, (int)PROF_FRAME_RESTORE);
    ASSERT_INT_EQ("axis still resolves with the fast series hidden",
                  ui_histogram_axis_range(hide_fast, &lo, &hi), 1);
    ASSERT_TRUE("hiding the fast series lifts the lower bound", lo > lo_all);
    ASSERT_TRUE("the hidden series' bin is off-axis",
                lo > prof_histogram_bin_for_us(0.4));
    ASSERT_TRUE("the 2ms samples are still on-axis",
                prof_histogram_bin_for_us(2000.0) >= lo &&
                prof_histogram_bin_for_us(2000.0) <= hi);
    ASSERT_TRUE("the surviving distribution gets a narrower axis",
                hi - lo < hi_all - lo_all);

    /* Nothing plotted is the same "no axis" answer as nothing sampled. */
    ASSERT_INT_EQ("hiding every series leaves no axis",
                  ui_histogram_axis_range(hist_all_series_hidden(), &lo, &hi), 0);
    prof_test_reset();
}

/* Toggling every series off must not take the legend down with the plot: the
 * legend is the only control that can put a series back. */
static void test_histogram_all_hidden_keeps_legend(void) {
    UiHistogramPanelView view = {
        .window_w = 800, .window_h = 600,
        .visible = 1, .panel_x = 10, .panel_y = 10
    };
    int hit_section;
    int legend_rows = 0;
    int series = 0;

    prof_test_reset();
    for (int i = 0; i < 30; i++) {
        prof_test_set_now_us((double)i * 100000.0);
        prof_begin(PROF_FRAME_TOTAL);
        prof_test_set_now_us((double)i * 100000.0 + 16000.0);
        prof_end(PROF_FRAME_TOTAL);
    }

    view.hidden_series = hist_all_series_hidden();
    ASSERT_TRUE("every plotted series is hidden", view.hidden_series != 0ULL);

    gl_stub_counts_reset();
    ui_histogram_panel_render(&view);
    /* Hidden swatches are hollow (GL_LINE_LOOP), so the legend shows up as
     * vertices and label text even with no bars to draw. */
    ASSERT_TRUE("all-hidden panel still draws the legend swatches",
                gl_stub_counts[GL_STUB_glVertex2f] > 0);
    ASSERT_TRUE("all-hidden panel still draws the legend labels",
                gl_stub_counts[GL_STUB_glutBitmapString] > 1);

    /* And the legend cell is still clickable, so a series can be restored. */
    for (int s = 0; s < PROF_SECTION_COUNT; s++)
        if (prof_section_info((ProfSection)s).depth == 0 &&
            prof_section_info((ProfSection)s).label &&
            prof_section_info((ProfSection)s).label[0])
            series++;
    legend_rows = (series + 2 - 1) / 2;
    {
        int ly = view.panel_y + 8 + (legend_rows - 1) * 12;
        hit_section = ui_histogram_panel_hit_test(&view, view.panel_x + 11,
                                                  view.window_h - (ly + 6));
    }
    ASSERT_TRUE("a legend cell still hit-tests while all series are hidden",
                hit_section >= 0);
    ASSERT_TRUE("toggling it clears that series' bit",
                ui_histogram_panel_toggle_series(view.hidden_series,
                                                 hit_section)
                    < view.hidden_series);
    prof_test_reset();
}

static void test_histogram_render_hidden(void) {
    UiHistogramPanelView view = {
        .window_w = 800, .window_h = 600,
        .visible = 0, .panel_x = 10, .panel_y = 10
    };
    gl_stub_counts_reset();
    ui_histogram_panel_render(&view);
    ASSERT_INT_EQ("hidden histogram panel draws nothing",
                  (int)gl_stub_counts[GL_STUB_glRectf], 0);
}

static void test_histogram_render_empty(void) {
    UiHistogramPanelView view = {
        .window_w = 800, .window_h = 600,
        .visible = 1, .panel_x = 10, .panel_y = 10
    };
    prof_test_reset();
    gl_stub_counts_reset();
    ui_histogram_panel_render(&view);
    ASSERT_TRUE("empty histogram panel still draws its frame",
                gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("empty histogram panel draws the placeholder text",
                gl_stub_counts[GL_STUB_glutBitmapString] > 0);
}

static void test_histogram_render_with_samples(void) {
    UiHistogramPanelView view = {
        .window_w = 800, .window_h = 600,
        .visible = 1, .panel_x = 10, .panel_y = 10
    };
    prof_test_reset();

    /* Baseline: the chrome (two panel frames) an empty panel already draws,
     * so the assertions below measure the series passes rather than the
     * frame borders those passes are layered on. */
    gl_stub_counts_reset();
    ui_histogram_panel_render(&view);
    unsigned long long empty_verts  = gl_stub_counts[GL_STUB_glVertex2f];
    unsigned long long empty_blends = gl_stub_counts[GL_STUB_glBlendFunc];

    /* Two top-level sections with distinct distributions — the case the
     * overlaid plot exists for. */
    for (int i = 0; i < 30; i++) {
        prof_test_set_now_us((double)i * 100000.0);
        prof_begin(PROF_FRAME_TOTAL);
        prof_test_set_now_us((double)i * 100000.0 + 16000.0);
        prof_end(PROF_FRAME_TOTAL);

        prof_begin(PROF_FLATTEN);
        prof_test_set_now_us((double)i * 100000.0 + 20000.0);
        prof_end(PROF_FLATTEN);
    }

    gl_stub_counts_reset();
    ui_histogram_panel_render(&view);
    ASSERT_TRUE("populated histogram panel draws bars and silhouettes",
                gl_stub_counts[GL_STUB_glVertex2f] > empty_verts);
    /* Fills are additively blended; the silhouette pass restores straight
     * alpha. Both modes get selected on top of the chrome's blend setup. */
    ASSERT_TRUE("populated histogram panel switches blend modes for its passes",
                gl_stub_counts[GL_STUB_glBlendFunc] > empty_blends);
    prof_test_reset();
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

    view.mode = PROFILE_PANEL_FPS;
    gl_stub_counts_reset();
    ui_profile_panel_render(&view);
    ASSERT_INT_EQ("FPS mode draws no section listing",
                  (int)gl_stub_counts[GL_STUB_glRectf], 0);
}

static void test_cpuprof_render_sections(void) {
    UiProfilePanelView view = {
        .window_w = 800,
        .window_h = 600,
        .mode = PROFILE_PANEL_SECTIONS,
        .panel_x = 10,
        .panel_y = 10
    };

    prof_frame_tick();

    gl_stub_counts_reset();
    ui_profile_panel_render(&view);
    ASSERT_TRUE("sections mode draws frame",
                gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("sections mode draws text",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0 ||
                gl_stub_counts[GL_STUB_glutBitmapCharacter] > 0);
    ASSERT_TRUE("sections mode draws filled disclosure bitmaps",
                gl_stub_counts[GL_STUB_glBitmap] > 0);
}

static void test_cpuprof_render_histogram_listing(void) {
    UiProfilePanelView view = {
        .window_w = 800,
        .window_h = 600,
        .mode = PROFILE_PANEL_HISTOGRAM,
        .panel_x = 10,
        .panel_y = 10
    };

    prof_frame_tick();

    gl_stub_counts_reset();
    ui_profile_panel_render(&view);
    ASSERT_TRUE("histogram mode draws section-list frame",
                gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("histogram mode draws section-list text",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0 ||
                gl_stub_counts[GL_STUB_glutBitmapCharacter] > 0);
    ASSERT_TRUE("histogram mode draws filled disclosure bitmaps",
                gl_stub_counts[GL_STUB_glBitmap] > 0);
}

int main(void) {
    printf("--- ui_cpuprof tests ---\n");
    test_cpuprof_metrics();
    test_cpuprof_section_collapse();
    test_cpuprof_section_hit_test();
    test_gpu_section_policy();
    test_cpuprof_section_histogram_direct();
    test_cpuprof_section_histogram_accum_commit();
    test_cpuprof_frame_time_histogram();
    test_cpuprof_current_fps_averages_intervals();
    test_cpuprof_histogram_saturates_16_bit_bins();
    test_cpuprof_histogram_reset();
    test_cpuprof_log_bin_layout();
    test_histogram_axis_range();
    test_histogram_axis_covers_cheap_and_slow();
    test_histogram_silhouette_collapses_flat_runs();
    test_histogram_panel_metrics();
    test_histogram_reset_hit_test();
    test_histogram_legend_toggle();
    test_histogram_hidden_series_drops_bars();
    test_histogram_axis_follows_hidden_series();
    test_histogram_all_hidden_keeps_legend();
    test_histogram_render_hidden();
    test_histogram_render_empty();
    test_histogram_render_with_samples();
    prof_test_reset();
    test_cpuprof_render_off();
    test_cpuprof_render_sections();
    test_cpuprof_render_histogram_listing();
    printf("\n");
    return test_harness_report(&g_harness, "test_ui_cpuprof");
}
#else
int main(void) {
    printf("This test requires GL stubs (USE_GL_STUBS=1)\n");
    return 0;
}
#endif
