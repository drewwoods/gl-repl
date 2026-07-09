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

#include <stdio.h>
#include <string.h>

#ifdef GL_STUBS
static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT_EQ(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

static int hist_bin_for_us(double us) {
    if (!(us > 0.0)) return 0;
    if (us >= PROF_HISTOGRAM_MAX_US)
        return PROF_HISTOGRAM_BIN_COUNT - 1;
    return (int)(us * (double)PROF_HISTOGRAM_BIN_COUNT
                 / PROF_HISTOGRAM_MAX_US);
}

static int hist_sum(const ProfHistogramBin *bins) {
    int total = 0;
    for (int i = 0; i < PROF_HISTOGRAM_BIN_COUNT; i++)
        total += (int)bins[i];
    return total;
}

static void test_cpuprof_metrics(void) {
    ASSERT_TRUE("width is positive", ui_profile_panel_width() > 0);
    ASSERT_TRUE("height is positive on", ui_profile_panel_height(PROFILE_PANEL_SECTIONS) > 0);
    ASSERT_TRUE("height is positive details", ui_profile_panel_height(PROFILE_PANEL_DETAILS) > 0);
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
    ASSERT_INT_EQ("snapshot is cpu-only", glr_prof_section_is_gpu(PROF_SNAPSHOT), 0);
    ASSERT_INT_EQ("frame restore is cpu-only",
                  glr_prof_section_is_gpu(PROF_FRAME_RESTORE), 0);
    ASSERT_INT_EQ("fade batch exec excluded",
                  glr_prof_section_is_gpu(PROF_RENDER3D_FADE_BATCH_EXEC), 0);
    ASSERT_INT_EQ("code panel build rows excluded",
                  glr_prof_section_is_gpu(PROF_CODE_PANEL_ROWS), 0);
    ASSERT_INT_EQ("code panel text layout excluded",
                  glr_prof_section_is_gpu(PROF_CODE_PANEL_TEXT_LAYOUT), 0);
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
    ASSERT_INT_EQ("50ms section sample lands in fixed bin",
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
    ASSERT_INT_EQ("17ms frame sample lands in fixed bin",
                  bins[hist_bin_for_us(17000.0)], 1);
    ASSERT_INT_EQ("frame histogram has one sample",
                  hist_sum(bins), 1);
    ASSERT_INT_EQ("frame histogram supports partial copies",
                  prof_frame_time_histogram(small, 4), 4);
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

/* The x-axis policy in isolation. */
static void test_histogram_series_axis_bin(void) {
    static ProfHistogramBin bins[PROF_HISTOGRAM_BIN_COUNT];

    memset(bins, 0, sizeof(bins));
    ASSERT_INT_EQ("an empty series has no axis",
                  ui_histogram_series_axis_bin(bins), -1);

    /* The shape a real Frame Total series has after a short capture: a tight
     * bulk plus a handful of startup transients trailing out to the overflow
     * bin. The axis must follow the bulk, not the transients. */
    memset(bins, 0, sizeof(bins));
    for (int i = 26; i <= 35; i++) bins[i] = 14;   /* 140 typical frames */
    bins[44] = 1; bins[62] = 1;
    bins[PROF_HISTOGRAM_BIN_COUNT - 1] = 1;        /* a >100 ms stall */
    ASSERT_TRUE("startup transients do not pin the axis at the overflow bin",
                ui_histogram_series_axis_bin(bins) <= 35);
    ASSERT_TRUE("the axis still covers the bulk of the distribution",
                ui_histogram_series_axis_bin(bins) >= 33);

    /* A genuinely bimodal series is not an outlier: 10% of samples at 100 ms
     * is real behavior the axis has to show. */
    memset(bins, 0, sizeof(bins));
    bins[10] = 90;
    bins[PROF_HISTOGRAM_BIN_COUNT - 1] = 10;
    ASSERT_INT_EQ("a fat tail stays on-axis",
                  ui_histogram_series_axis_bin(bins),
                  PROF_HISTOGRAM_BIN_COUNT - 1);

    /* A series too small for a percentile to mean anything keeps its sample:
     * ceil() of the keep target never rounds the only sample away. */
    memset(bins, 0, sizeof(bins));
    bins[500] = 1;
    ASSERT_INT_EQ("a lone sample defines its own axis",
                  ui_histogram_series_axis_bin(bins), 500);

    /* Every sub-100us section looks like this: all mass in bin 0. */
    memset(bins, 0, sizeof(bins));
    bins[0] = 500;
    ASSERT_INT_EQ("an all-in-bin-0 series asks for the narrowest axis",
                  ui_histogram_series_axis_bin(bins), 0);
}

/* The panel's axis is the max across series, so the many sub-100us sections
 * (all mass in bin 0) must not be able to clip the slowest section's spread —
 * the flaw a pooled percentile would have. Observable through the rendered
 * bar count: a wide axis emits more silhouette vertices than a 4-bin one. */
static void test_histogram_axis_follows_slowest_series(void) {
    UiHistogramPanelView view = {
        .window_w = 800, .window_h = 600,
        .visible = 1, .panel_x = 10, .panel_y = 10
    };

    /* Only cheap sections: every sample under one bin's width. */
    prof_test_reset();
    for (int i = 0; i < 200; i++) {
        prof_test_set_now_us((double)i * 1000.0);
        prof_begin(PROF_FLATTEN);
        prof_end(PROF_FLATTEN);
    }
    gl_stub_counts_reset();
    ui_histogram_panel_render(&view);
    unsigned long long narrow_verts = gl_stub_counts[GL_STUB_glVertex2f];

    /* Same cheap section, plus one slow section spread around 20 ms. Its
     * spread must widen the axis rather than be buried by the bin-0 mass. */
    prof_test_reset();
    for (int i = 0; i < 200; i++) {
        prof_test_set_now_us((double)i * 100000.0);
        prof_begin(PROF_FLATTEN);
        prof_end(PROF_FLATTEN);

        prof_begin(PROF_FRAME_TOTAL);
        prof_test_set_now_us((double)i * 100000.0 + 20000.0 + (double)(i % 8) * 200.0);
        prof_end(PROF_FRAME_TOTAL);
    }
    gl_stub_counts_reset();
    ui_histogram_panel_render(&view);
    ASSERT_TRUE("a slow series widens the axis past the bin-0 crowd",
                gl_stub_counts[GL_STUB_glVertex2f] > narrow_verts);
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
                gl_stub_counts[GL_STUB_glutBitmapCharacter] > 0);
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
}

static void test_cpuprof_render_on(void) {
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
    test_gpu_section_policy();
    test_cpuprof_section_histogram_direct();
    test_cpuprof_section_histogram_accum_commit();
    test_cpuprof_frame_time_histogram();
    test_cpuprof_histogram_saturates_16_bit_bins();
    test_histogram_series_axis_bin();
    test_histogram_axis_follows_slowest_series();
    test_histogram_panel_metrics();
    test_histogram_render_hidden();
    test_histogram_render_empty();
    test_histogram_render_with_samples();
    prof_test_reset();
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
