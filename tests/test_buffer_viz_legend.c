/*
 * test_buffer_viz_legend.c - the legend panel's row-selection policies and
 * its solved geometry.
 *
 * Two halves, matching the two places the legend can go wrong, and two
 * producers sharing the second:
 *
 *   - glr_ctrl_buffer_viz_legend_select_rows() - the controller decides
 *     which of up to 255 non-zero stencil values earn a row. A capture can
 *     hold them all and the panel has no scrolling, so the top-N cap, the
 *     "+N more" remainder, and the always-retained zero/total rows are
 *     load-bearing, not cosmetic.
 *   - glr_ctrl_call_depth_legend_select_rows() - the same panel, keyed by
 *     funcN call depth. Its rule is ASCENDING depth rather than top-N by
 *     count, because the colours it keys are an ordered ramp.
 *   - ui_buffer_viz_legend_size() - the renderer's pure solve. Drives it
 *     with no GL context; the panel must stay inside the scene rect and
 *     refuse to draw when it cannot.
 */
#include "app/glr_ctrl.h"
#include "app/glr_state.h"
#include "subsystems/buffer_viz/stencil_viz.h"
#include "ui/subsystems/buffer_viz_legend.h"
#include "support/test_harness.h"

#include <string.h>

static TestHarness g_h = TEST_HARNESS_INIT;
#define AT(label, cond) TEST_ASSERT_TRUE(&g_h, label, cond)
#define AI(label, got, expected) TEST_ASSERT_INT(&g_h, label, got, expected)

/* Build a histogram the way buffer_viz_stencil_scan would, from a list of
 * (value, count) pairs, so the tests state their input as the legend
 * reads it. */
typedef struct { int value; unsigned int count; } Bin;

static BufferVizStencilHistogram make_hist(unsigned int zero_px,
                                           const Bin *bins, int bin_count) {
    BufferVizStencilHistogram hist;
    int i;

    memset(&hist, 0, sizeof hist);
    hist.counts[0] = zero_px;
    hist.total_px = (int)zero_px;
    for (i = 0; i < bin_count; i++) {
        hist.counts[bins[i].value] = bins[i].count;
        hist.distinct++;
        hist.nonzero_px += (int)bins[i].count;
        hist.total_px += (int)bins[i].count;
    }
    hist.valid = 1;
    return hist;
}

/* --- row selection -------------------------------------------------- */

static void test_select_rows_reports_counts(void) {
    const Bin bins[] = { { 1, 300 }, { 2, 100 }, { 5, 50 } };
    BufferVizStencilHistogram hist = make_hist(1000, bins, 3);
    UiBufferVizLegendView view;

    memset(&view, 0, sizeof view);
    glr_ctrl_buffer_viz_legend_select_rows(&hist, BUFFER_VIZ_STENCIL_PALETTE,
                                           &view);

    AI("select: one row per non-zero value", view.row_count, 3);
    AI("select: nothing hidden", view.hidden_rows, 0);
    AI("select: no hidden pixels", (int)view.hidden_px, 0);
    AI("select: zero row keeps the background count", (int)view.zero_px, 1000);
    AI("select: total is every scanned pixel", (int)view.total_px, 1450);
    AI("select: busiest value first", view.rows[0].value, 1);
    AI("select: its count", (int)view.rows[0].count, 300);
    AI("select: then value 2", view.rows[1].value, 2);
    AI("select: then value 5", view.rows[2].value, 5);
}

/* Descending by count, and a tie resolves to the lower value - otherwise
 * two equally-covered values would swap rows frame to frame. */
static void test_select_rows_orders_by_count_then_value(void) {
    const Bin bins[] = { { 9, 40 }, { 4, 40 }, { 7, 90 }, { 200, 40 } };
    BufferVizStencilHistogram hist = make_hist(0, bins, 4);
    UiBufferVizLegendView view;

    memset(&view, 0, sizeof view);
    glr_ctrl_buffer_viz_legend_select_rows(&hist, BUFFER_VIZ_STENCIL_PALETTE,
                                           &view);

    AI("order: rows listed", view.row_count, 4);
    AI("order: highest count leads", view.rows[0].value, 7);
    AI("order: tie breaks to the lowest value", view.rows[1].value, 4);
    AI("order: then the next value up", view.rows[2].value, 9);
    AI("order: then the highest value", view.rows[3].value, 200);
}

/* A valid capture can carry all 255 non-zero values. The panel cannot
 * grow to fit them, so the tail collapses into one "+N more" row that
 * still accounts for its pixels. */
static void test_select_rows_caps_and_summarizes_the_tail(void) {
    Bin bins[20];
    BufferVizStencilHistogram hist;
    UiBufferVizLegendView view;
    unsigned int listed = 0;
    int i;

    for (i = 0; i < 20; i++) {
        bins[i].value = i + 1;
        bins[i].count = (unsigned int)(100 - i);  /* strictly descending */
    }
    hist = make_hist(7, bins, 20);
    memset(&view, 0, sizeof view);
    glr_ctrl_buffer_viz_legend_select_rows(&hist, BUFFER_VIZ_STENCIL_PALETTE,
                                           &view);

    AI("cap: rows stop at the panel's limit", view.row_count,
       UI_BUFFER_VIZ_LEGEND_MAX_ROWS);
    AI("cap: the rest are counted as hidden",
       view.hidden_rows, 20 - UI_BUFFER_VIZ_LEGEND_MAX_ROWS);
    for (i = 0; i < view.row_count; i++)
        listed += view.rows[i].count;
    AI("cap: listed + hidden accounts for every non-zero pixel",
       (int)(listed + view.hidden_px), hist.nonzero_px);
    AI("cap: listed rows are the busiest ones", view.rows[0].value, 1);
    AI("cap: last listed row", view.rows[UI_BUFFER_VIZ_LEGEND_MAX_ROWS - 1].value,
       UI_BUFFER_VIZ_LEGEND_MAX_ROWS);
    AI("cap: zero row survives the cap", (int)view.zero_px, 7);
}

/* An empty mask is a diagnosis, not an absence of data: the panel still
 * reports "everything is background". */
static void test_select_rows_all_background(void) {
    BufferVizStencilHistogram hist = make_hist(512, NULL, 0);
    UiBufferVizLegendView view;

    memset(&view, 0, sizeof view);
    glr_ctrl_buffer_viz_legend_select_rows(&hist, BUFFER_VIZ_STENCIL_PALETTE,
                                           &view);
    AI("empty: no value rows", view.row_count, 0);
    AI("empty: nothing hidden", view.hidden_rows, 0);
    AI("empty: zero row carries the frame", (int)view.zero_px, 512);
    AI("empty: total agrees", (int)view.total_px, 512);
}

static void test_select_rows_rejects_unscanned_histogram(void) {
    BufferVizStencilHistogram hist;
    UiBufferVizLegendView view;

    memset(&hist, 0, sizeof hist);   /* valid == 0: nothing scanned yet */
    hist.counts[3] = 99;
    memset(&view, 0, sizeof view);
    view.row_count = 5;              /* must be cleared, not left stale */
    glr_ctrl_buffer_viz_legend_select_rows(&hist, BUFFER_VIZ_STENCIL_PALETTE,
                                           &view);
    AI("invalid: rows cleared", view.row_count, 0);
    AI("invalid: totals cleared", (int)view.total_px, 0);

    memset(&view, 0, sizeof view);
    view.row_count = 5;
    glr_ctrl_buffer_viz_legend_select_rows(NULL, BUFFER_VIZ_STENCIL_PALETTE,
                                           &view);
    AI("null histogram: rows cleared", view.row_count, 0);
    glr_ctrl_buffer_viz_legend_select_rows(&hist, BUFFER_VIZ_STENCIL_PALETTE,
                                           NULL);  /* must not crash */
}

/* The swatch is a sample of the overlay, not a second opinion about it. */
static void test_select_rows_swatches_match_the_overlay(void) {
    const Bin bins[] = { { 3, 10 }, { 19, 5 } };
    BufferVizStencilHistogram hist = make_hist(0, bins, 2);
    UiBufferVizLegendView view;
    unsigned char expect[3];
    int i;

    memset(&view, 0, sizeof view);
    glr_ctrl_buffer_viz_legend_select_rows(&hist, BUFFER_VIZ_STENCIL_PALETTE,
                                           &view);
    AI("swatch: both values listed", view.row_count, 2);
    for (i = 0; i < view.row_count; i++) {
        buffer_viz_stencil_palette_rgb(view.rows[i].value, expect);
        AT("swatch: palette colour for the row's value",
           memcmp(view.rows[i].rgb, expect, 3) == 0);
    }
}

/* --- panel geometry -------------------------------------------------- */

static UiBufferVizLegendView drawable_view(int rows) {
    UiBufferVizLegendView view;
    int i;

    memset(&view, 0, sizeof view);
    view.visible = 1;
    view.window_w = 1200;
    view.window_h = 800;
    view.scene_x = 400;
    view.scene_y = 0;
    view.scene_w = 800;
    view.scene_h = 800;
    view.title = "Stencil: Palette";
    view.row_count = rows;
    for (i = 0; i < rows && i < UI_BUFFER_VIZ_LEGEND_MAX_ROWS; i++) {
        view.rows[i].value = i + 1;
        view.rows[i].count = 100u - (unsigned int)i;
        view.total_px += view.rows[i].count;
    }
    view.zero_px = 1000;
    view.total_px += view.zero_px;
    return view;
}

static void test_size_zero_when_not_drawn(void) {
    UiBufferVizLegendView view = drawable_view(2);
    int w = -1, h = -1;

    view.visible = 0;
    ui_buffer_viz_legend_size(&view, &w, &h);
    AI("size: invisible view has no width", w, 0);
    AI("size: invisible view has no height", h, 0);

    ui_buffer_viz_legend_size(NULL, &w, &h);
    AI("size: null view has no width", w, 0);
    ui_buffer_viz_legend_size(&view, NULL, NULL);  /* must not crash */
}

static void test_size_grows_with_rows_and_stays_bounded(void) {
    UiBufferVizLegendView few = drawable_view(1);
    UiBufferVizLegendView many = drawable_view(UI_BUFFER_VIZ_LEGEND_MAX_ROWS);
    int few_w = 0, few_h = 0, many_w = 0, many_h = 0;

    ui_buffer_viz_legend_size(&few, &few_w, &few_h);
    ui_buffer_viz_legend_size(&many, &many_w, &many_h);

    AT("size: a drawable panel has extent", few_w > 0 && few_h > 0);
    AT("size: more rows means more height", many_h > few_h);
    AT("size: the full panel still fits the scene rect",
       many_h <= many.scene_h && many_w <= many.scene_w);

    /* The "+N more" row is a row like any other. */
    many.hidden_rows = 40;
    many.hidden_px = 4000;
    {
        int more_w = 0, more_h = 0;
        ui_buffer_viz_legend_size(&many, &more_w, &more_h);
        AT("size: the truncation row adds a line", more_h > many_h);
    }
}

/* A wide count column must widen the panel, not overflow it. */
static void test_size_widens_for_large_counts(void) {
    UiBufferVizLegendView small = drawable_view(2);
    UiBufferVizLegendView big = drawable_view(2);
    int small_w = 0, big_w = 0;

    /* A short title, so the count column - not the header - is what the
     * solved width has to accommodate. */
    small.title = "Stencil";
    big.title = "Stencil";
    big.rows[0].count = 12345678u;
    big.total_px = 12345999u;
    ui_buffer_viz_legend_size(&small, &small_w, NULL);
    ui_buffer_viz_legend_size(&big, &big_w, NULL);
    AT("size: an eight-digit count widens the panel", big_w > small_w);
}

/* The "+N more" row is built into a fixed left-column buffer, and that buffer
 * used to be too small to hold the string for a large remainder - so the count
 * was silently clipped and the solved width described the clipped text rather
 * than what the panel draws. Two remainders that differ only past the old
 * 12-byte cap have to produce two different widths. */
static void test_size_tracks_a_large_truncation_count(void) {
    UiBufferVizLegendView narrow = drawable_view(2);
    UiBufferVizLegendView wide = drawable_view(2);
    int narrow_w = 0, wide_w = 0;

    /* Short title so the left column, not the header, drives the width. */
    narrow.title = "S";
    wide.title = "S";
    narrow.hidden_rows = 2000000;    /* "+2000000 more"  - 13 chars */
    wide.hidden_rows = 20000000;     /* "+20000000 more" - 14 chars */
    narrow.hidden_px = 4000u;
    wide.hidden_px = 4000u;

    ui_buffer_viz_legend_size(&narrow, &narrow_w, NULL);
    ui_buffer_viz_legend_size(&wide, &wide_w, NULL);
    AT("size: a longer \"+N more\" widens the panel", wide_w > narrow_w);
}

/* Refuse rather than spill: a scene rect too small for the solved panel
 * draws nothing. */
static void test_size_refuses_a_scene_too_small(void) {
    UiBufferVizLegendView view = drawable_view(UI_BUFFER_VIZ_LEGEND_MAX_ROWS);
    int w = -1, h = -1;

    view.scene_w = 40;
    view.scene_h = 40;
    ui_buffer_viz_legend_size(&view, &w, &h);
    AI("size: cramped scene suppresses the panel width", w, 0);
    AI("size: cramped scene suppresses the panel height", h, 0);
}

/* --- controller view ------------------------------------------------- */

/* Stencil view defaults to Off, so the panel is absent until the user
 * turns the viz on - the legend never draws next to an absent overlay. */
static void test_view_absent_while_viz_is_off(void) {
    UiBufferVizLegendView view;

    glr_state_presentation_mut()->stencil_viz = BUFFER_VIZ_STENCIL_OFF;
    view = glr_ctrl_build_buffer_viz_legend_view();
    AI("view: invisible with the viz off", view.visible, 0);

    /* Mode on, but no capture has been scanned yet (no GL context here):
     * still nothing to legend. */
    glr_state_presentation_mut()->stencil_viz = BUFFER_VIZ_STENCIL_PALETTE;
    buffer_viz_stencil_reset();
    view = glr_ctrl_build_buffer_viz_legend_view();
    AI("view: invisible until a capture is scanned", view.visible, 0);
    glr_state_presentation_mut()->stencil_viz = BUFFER_VIZ_STENCIL_OFF;
}

/* --- call-depth rows -------------------------------------------------
 *
 * The panel's second producer. Its selection rule is deliberately NOT the
 * stencil one: depths come out in ascending order, because the colours
 * they key are a ramp and a legend that reordered an ordered scale by
 * population would be unreadable.
 */

static CallDepthVizStats make_depth_stats(const unsigned int *counts,
                                          int count_len) {
    CallDepthVizStats s;
    int d;

    memset(&s, 0, sizeof s);
    for (d = 0; d < count_len && d < CALL_DEPTH_VIZ_SLOTS; d++) {
        if (!counts[d])
            continue;
        s.counts[d] = counts[d];
        s.distinct++;
        s.total += counts[d];
        s.max_depth = d;
    }
    s.valid = (s.total > 0);
    return s;
}

static void test_depth_rows_are_ascending_and_counted(void) {
    /* Deliberately not monotonic in count: depth 1 is the most populous,
     * and a top-N-by-count rule would list it first. */
    const unsigned int counts[] = { 40, 900, 0, 120 };
    CallDepthVizStats stats = make_depth_stats(counts, 4);
    UiBufferVizLegendView view;

    memset(&view, 0, sizeof view);
    glr_ctrl_call_depth_legend_select_rows(&stats, &view);

    AI("depth rows: one row per occupied depth", view.row_count, 3);
    AI("depth rows: first row is the shallowest depth", view.rows[0].value, 0);
    AI("depth rows: rows ascend by depth, not by count",
       view.rows[1].value, 1);
    AI("depth rows: an empty depth is skipped, not blank-rowed",
       view.rows[2].value, 3);
    AI("depth rows: counts come straight from the scan",
       (int)view.rows[1].count, 900);
    AI("depth rows: total is every command scanned",
       (int)view.total_px, 1060);
    AI("depth rows: nothing hidden when every depth fits",
       view.hidden_rows, 0);
}

static void test_depth_rows_swatch_matches_the_ramp(void) {
    const unsigned int counts[] = { 1, 1, 1, 1, 1 };
    CallDepthVizStats stats = make_depth_stats(counts, 5);
    UiBufferVizLegendView view;
    float want[3];
    int c, ok = 1;

    memset(&view, 0, sizeof view);
    glr_ctrl_call_depth_legend_select_rows(&stats, &view);
    AI("depth swatch: every depth listed", view.row_count, 5);

    /* The swatch must be the colour the executor tinted with - same ramp,
     * same max depth - or the panel describes a frame that never drew. */
    call_depth_viz_ramp_rgb(3, stats.max_depth, want);
    for (c = 0; c < 3; c++) {
        int expected = (int)(want[c] * 255.0f + 0.5f);
        if ((int)view.rows[3].rgb[c] != expected)
            ok = 0;
    }
    AT("depth swatch: row colour is the ramp colour for its depth", ok);
    AT("depth swatch: the shallow row is cooler than the deep one",
       view.rows[0].rgb[2] > view.rows[4].rgb[2]);
}

static void test_depth_rows_cap_and_summarize_the_tail(void) {
    unsigned int counts[UI_BUFFER_VIZ_LEGEND_MAX_ROWS + 3];
    CallDepthVizStats stats;
    UiBufferVizLegendView view;
    int overflow = 3;
    int d;

    for (d = 0; d < UI_BUFFER_VIZ_LEGEND_MAX_ROWS + overflow; d++)
        counts[d] = 10;
    stats = make_depth_stats(counts, UI_BUFFER_VIZ_LEGEND_MAX_ROWS + overflow);

    memset(&view, 0, sizeof view);
    glr_ctrl_call_depth_legend_select_rows(&stats, &view);

    AI("depth cap: rows stop at the panel's budget",
       view.row_count, UI_BUFFER_VIZ_LEGEND_MAX_ROWS);
    /* Truncation drops the DEEP tail, keeping the shallow levels a frame
     * is mostly made of - which is also what keeps the listing ascending. */
    AI("depth cap: the shallowest depth survives", view.rows[0].value, 0);
    AI("depth cap: the deep tail is the part elided", view.hidden_rows,
       overflow);
    AI("depth cap: the elided commands are still accounted for",
       (int)view.hidden_px, overflow * 10);
}

static void test_depth_rows_reject_unscanned_stats(void) {
    CallDepthVizStats stats;
    UiBufferVizLegendView view;

    memset(&stats, 0, sizeof stats);
    memset(&view, 0, sizeof view);
    view.row_count = 5;   /* stale rows must be cleared, not kept */
    glr_ctrl_call_depth_legend_select_rows(&stats, &view);
    AI("depth rows: an invalid scan lists nothing", view.row_count, 0);
    AI("depth rows: an invalid scan reports no total", (int)view.total_px, 0);

    glr_ctrl_call_depth_legend_select_rows(NULL, &view);
    AI("depth rows: a NULL scan lists nothing", view.row_count, 0);
    glr_ctrl_call_depth_legend_select_rows(&stats, NULL);  /* must not crash */
    AT("depth rows: a NULL view is survivable", 1);
}

int main(void) {
    test_select_rows_reports_counts();
    test_select_rows_orders_by_count_then_value();
    test_select_rows_caps_and_summarizes_the_tail();
    test_select_rows_all_background();
    test_select_rows_rejects_unscanned_histogram();
    test_select_rows_swatches_match_the_overlay();
    test_size_zero_when_not_drawn();
    test_size_grows_with_rows_and_stays_bounded();
    test_size_widens_for_large_counts();
    test_size_tracks_a_large_truncation_count();
    test_size_refuses_a_scene_too_small();
    test_view_absent_while_viz_is_off();
    test_depth_rows_are_ascending_and_counted();
    test_depth_rows_swatch_matches_the_ramp();
    test_depth_rows_cap_and_summarize_the_tail();
    test_depth_rows_reject_unscanned_stats();
    return test_harness_report(&g_h, "buffer_viz_legend");
}
