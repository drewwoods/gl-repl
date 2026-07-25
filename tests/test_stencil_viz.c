/*
 * test_stencil_viz.c - synthetic-buffer tests for the pure conversion
 * core of the stencil-buffer visualization (buffer_viz_stencil_scan and
 * buffer_viz_stencil_map).
 *
 * Drives both directly with hand-built byte buffers — no GL context.
 * Pins the properties the visualization's usefulness rests on: zero is
 * transparent (the whole compositing model), the palette is
 * view-independent and deterministic, the histogram counts what a legend
 * would print, and RAMP normalizes against NON-ZERO values only.
 */
#include "subsystems/buffer_viz/stencil_viz.h"
#include "support/test_harness.h"

#include <string.h>

static TestHarness g_h = TEST_HARNESS_INIT;
#define AT(label, cond) TEST_ASSERT_TRUE(&g_h, label, cond)
#define AI(label, got, expected) TEST_ASSERT_INT(&g_h, label, got, expected)

/* --- scan ---------------------------------------------------------- */

static void test_scan_counts_and_extent(void) {
    const unsigned char buf[8] = { 0, 3, 3, 0, 7, 1, 0, 3 };
    BufferVizStencilHistogram hist;
    float lo = -1.0f, hi = -1.0f;
    int found;

    memset(&hist, 0, sizeof hist);
    found = buffer_viz_stencil_scan(buf, 8, &hist, &lo, &hi);

    AT("scan: reports non-zero values present", found == 1);
    AT("scan: histogram valid", hist.valid == 1);
    AI("scan: counts zeros too", (int)hist.counts[0], 3);
    AI("scan: counts value 3", (int)hist.counts[3], 3);
    AI("scan: counts value 1", (int)hist.counts[1], 1);
    AI("scan: counts value 7", (int)hist.counts[7], 1);
    AI("scan: total pixels", hist.total_px, 8);
    AI("scan: non-zero pixels", hist.nonzero_px, 5);
    /* distinct counts VALUES, not pixels, and excludes zero. */
    AI("scan: distinct non-zero values", hist.distinct, 3);
    TEST_ASSERT_FLOAT(&g_h, "scan: lo is the smallest non-zero", lo, 1.0f, 1e-6f);
    TEST_ASSERT_FLOAT(&g_h, "scan: hi is the largest", hi, 7.0f, 1e-6f);
}

/* Zero must not enter the extent — it is the clear value and dominates
 * every real scene, so including it would pin lo at 0 and flatten RAMP
 * into a fixed 0..max mapping. */
static void test_scan_extent_excludes_zero(void) {
    unsigned char buf[64];
    float lo = -1.0f, hi = -1.0f;

    memset(buf, 0, sizeof buf);
    buf[10] = 4;
    buf[40] = 6;
    AT("scan: mostly-zero buffer still finds values",
       buffer_viz_stencil_scan(buf, (int)sizeof buf, NULL, &lo, &hi) == 1);
    TEST_ASSERT_FLOAT(&g_h, "scan: lo skips the zero majority", lo, 4.0f, 1e-6f);
    TEST_ASSERT_FLOAT(&g_h, "scan: hi is 6", hi, 6.0f, 1e-6f);
}

static void test_scan_all_zero(void) {
    const unsigned char buf[4] = { 0, 0, 0, 0 };
    BufferVizStencilHistogram hist;
    float lo = -1.0f, hi = -1.0f;

    memset(&hist, 0, sizeof hist);
    AT("scan: all-zero reports nothing found",
       buffer_viz_stencil_scan(buf, 4, &hist, &lo, &hi) == 0);
    AT("scan: all-zero leaves lo/hi untouched", lo == -1.0f && hi == -1.0f);
    AI("scan: all-zero histogram is still valid", hist.valid, 1);
    AI("scan: all-zero distinct is 0", hist.distinct, 0);
    AI("scan: all-zero non-zero pixels", hist.nonzero_px, 0);
    AI("scan: all-zero counts the zeros", (int)hist.counts[0], 4);
}

static void test_scan_degenerate_inputs(void) {
    BufferVizStencilHistogram hist;
    const unsigned char buf[1] = { 5 };

    memset(&hist, 0xAB, sizeof hist);
    AT("scan: NULL buffer finds nothing",
       buffer_viz_stencil_scan(NULL, 4, &hist, NULL, NULL) == 0);
    AI("scan: NULL buffer reports zero pixels", hist.total_px, 0);
    AT("scan: zero count finds nothing",
       buffer_viz_stencil_scan(buf, 0, NULL, NULL, NULL) == 0);
    /* Every output pointer optional. */
    AT("scan: NULL outputs are safe",
       buffer_viz_stencil_scan(buf, 1, NULL, NULL, NULL) == 1);
}

/* --- map: the zero-is-transparent contract ------------------------- */

static void test_zero_is_transparent_in_every_mode(void) {
    const unsigned char buf[3] = { 0, 1, 0 };
    BufferVizRange range = { 1.0f, 4.0f, 1 };
    static const BufferVizStencilMode modes[3] = {
        BUFFER_VIZ_STENCIL_PALETTE,
        BUFFER_VIZ_STENCIL_RAMP,
        BUFFER_VIZ_STENCIL_SPLIT
    };

    for (int m = 0; m < 3; m++) {
        unsigned char rgba[12];
        memset(rgba, 0x7F, sizeof rgba);
        buffer_viz_stencil_map(buf, 3, modes[m], &range, rgba);
        AI("map: zero is fully transparent", rgba[3], 0);
        AI("map: zero is fully transparent (2)", rgba[11], 0);
        AT("map: non-zero is opaque enough to see", rgba[7] > 0);
        AI("map: non-zero uses the overlay alpha", rgba[7],
           BUFFER_VIZ_STENCIL_ALPHA);
    }
}

/* --- map: palette determinism -------------------------------------- */

static void test_palette_is_view_independent(void) {
    const unsigned char lonely[1] = { 2 };
    const unsigned char crowded[4] = { 2, 9, 200, 41 };
    unsigned char a[4], b[16];

    buffer_viz_stencil_map(lonely, 1, BUFFER_VIZ_STENCIL_PALETTE, NULL, a);
    buffer_viz_stencil_map(crowded, 4, BUFFER_VIZ_STENCIL_PALETTE, NULL, b);

    /* The whole point of PALETTE: a value's colour does not depend on what
     * else happens to be in the buffer this frame. */
    AI("palette: value 2 red is scene-independent", b[0], a[0]);
    AI("palette: value 2 green is scene-independent", b[1], a[1]);
    AI("palette: value 2 blue is scene-independent", b[2], a[2]);
}

static void test_palette_distinguishes_low_values(void) {
    const unsigned char buf[4] = { 1, 2, 3, 4 };
    unsigned char rgba[16];
    int i, j;

    buffer_viz_stencil_map(buf, 4, BUFFER_VIZ_STENCIL_PALETTE, NULL, rgba);
    for (i = 0; i < 4; i++) {
        for (j = i + 1; j < 4; j++) {
            int same = rgba[i * 4 + 0] == rgba[j * 4 + 0] &&
                       rgba[i * 4 + 1] == rgba[j * 4 + 1] &&
                       rgba[i * 4 + 2] == rgba[j * 4 + 2];
            AT("palette: adjacent low values differ", !same);
        }
    }
}

/* The documented aliasing: `value & 15` gives values 16 apart the same
 * swatch. Pinned so it stays a deliberate, documented property rather
 * than something a future palette change silently breaks (the legend
 * prints the numeric value, which is what disambiguates it). */
static void test_palette_aliases_every_sixteen(void) {
    unsigned char one[3], seventeen[3], thirty_three[3];

    buffer_viz_stencil_palette_rgb(1, one);
    buffer_viz_stencil_palette_rgb(17, seventeen);
    buffer_viz_stencil_palette_rgb(33, thirty_three);
    AT("palette: 1 and 17 alias", memcmp(one, seventeen, 3) == 0);
    AT("palette: 1 and 33 alias", memcmp(one, thirty_three, 3) == 0);
}

/* The accessor a legend uses must agree with what the overlay drew, or
 * the key lies about the picture. */
static void test_palette_accessor_matches_map(void) {
    const unsigned char buf[1] = { 6 };
    unsigned char rgba[4];
    unsigned char swatch[3];

    buffer_viz_stencil_map(buf, 1, BUFFER_VIZ_STENCIL_PALETTE, NULL, rgba);
    buffer_viz_stencil_palette_rgb(6, swatch);
    AT("palette: legend swatch matches the overlay",
       memcmp(rgba, swatch, 3) == 0);
}

static void test_split_maps_like_palette(void) {
    const unsigned char buf[3] = { 0, 5, 12 };
    unsigned char pal[12], split[12];

    buffer_viz_stencil_map(buf, 3, BUFFER_VIZ_STENCIL_PALETTE, NULL, pal);
    buffer_viz_stencil_map(buf, 3, BUFFER_VIZ_STENCIL_SPLIT, NULL, split);
    /* SPLIT differs in WHERE it is composited, not in what it contains. */
    AT("split: identical bytes to palette", memcmp(pal, split, 12) == 0);
}

/* --- map: ramp ----------------------------------------------------- */

static void test_ramp_spans_the_range(void) {
    const unsigned char buf[3] = { 1, 5, 9 };
    BufferVizRange range = { 1.0f, 9.0f, 1 };
    unsigned char rgba[12];

    buffer_viz_stencil_map(buf, 3, BUFFER_VIZ_STENCIL_RAMP, &range, rgba);
    /* Cool/dim at the low value, warm/bright at the high one: red rises
     * and blue falls monotonically across the range. */
    AT("ramp: red rises with value",
       rgba[0] < rgba[4] && rgba[4] < rgba[8]);
    AT("ramp: blue falls with value",
       rgba[2] > rgba[6] && rgba[6] > rgba[10]);
}

static void test_ramp_clamps_outside_the_smoothed_range(void) {
    /* EMA lag legitimately puts this frame's values outside the smoothed
     * range; the mapping must clamp rather than wrap past the byte edges. */
    const unsigned char buf[2] = { 1, 40 };
    BufferVizRange range = { 10.0f, 20.0f, 1 };
    unsigned char rgba[8];
    unsigned char ends[8];
    const unsigned char at_ends[2] = { 10, 20 };

    buffer_viz_stencil_map(buf, 2, BUFFER_VIZ_STENCIL_RAMP, &range, rgba);
    buffer_viz_stencil_map(at_ends, 2, BUFFER_VIZ_STENCIL_RAMP, &range, ends);
    AT("ramp: below-range clamps to the low end",
       memcmp(rgba, ends, 4) == 0);
    AT("ramp: above-range clamps to the high end",
       memcmp(rgba + 4, ends + 4, 4) == 0);
}

static void test_ramp_degenerate_and_unseeded_ranges(void) {
    const unsigned char buf[2] = { 3, 3 };
    BufferVizRange same = { 3.0f, 3.0f, 1 };
    BufferVizRange unseeded = { 0.0f, 0.0f, 0 };
    unsigned char a[8], b[8];

    /* Single-value buffer: must not divide by ~0. */
    buffer_viz_stencil_map(buf, 2, BUFFER_VIZ_STENCIL_RAMP, &same, a);
    AI("ramp: zero span still writes alpha", a[3], BUFFER_VIZ_STENCIL_ALPHA);
    AT("ramp: zero span maps both pixels alike", memcmp(a, a + 4, 4) == 0);

    /* No range seeded yet (first frame): same mid-ramp fallback, no NaN. */
    buffer_viz_stencil_map(buf, 2, BUFFER_VIZ_STENCIL_RAMP, &unseeded, b);
    AT("ramp: unseeded range falls back to the zero-span colour",
       memcmp(a, b, 8) == 0);
    /* And a NULL range is the same case, not a crash. */
    buffer_viz_stencil_map(buf, 2, BUFFER_VIZ_STENCIL_RAMP, NULL, b);
    AT("ramp: NULL range behaves like unseeded", memcmp(a, b, 8) == 0);
}

/* --- map: degenerate inputs ---------------------------------------- */

static void test_map_degenerate_inputs(void) {
    const unsigned char buf[1] = { 1 };
    unsigned char rgba[4] = { 7, 7, 7, 7 };

    buffer_viz_stencil_map(NULL, 1, BUFFER_VIZ_STENCIL_PALETTE, NULL, rgba);
    buffer_viz_stencil_map(buf, 0, BUFFER_VIZ_STENCIL_PALETTE, NULL, rgba);
    buffer_viz_stencil_map(buf, 1, BUFFER_VIZ_STENCIL_PALETTE, NULL, NULL);
    AI("map: no-op calls leave the output untouched", rgba[0], 7);

    /* Mode-range validation is this module's job: render3d fires a
     * neutral hook and never sees a viz mode. */
    buffer_viz_stencil_map(buf, 1, BUFFER_VIZ_STENCIL_OFF, NULL, rgba);
    AI("map: OFF writes nothing", rgba[0], 7);
    buffer_viz_stencil_map(buf, 1, (BufferVizStencilMode)-1, NULL, rgba);
    buffer_viz_stencil_map(buf, 1, BUFFER_VIZ_STENCIL_COUNT, NULL, rgba);
    buffer_viz_stencil_map(buf, 1,
                           (BufferVizStencilMode)(BUFFER_VIZ_STENCIL_COUNT + 9),
                           NULL, rgba);
    AI("map: out-of-range modes write nothing", rgba[0], 7);
}

/* The GL shell's rejection paths all return before the first GL call, so
 * they run with no context in either build. */
static void test_render_rejects_bad_modes(void) {
    buffer_viz_stencil_reset();
    buffer_viz_stencil_render(BUFFER_VIZ_STENCIL_OFF, 1, 0, 0, 4, 4);
    buffer_viz_stencil_render((BufferVizStencilMode)-1, 1, 0, 0, 4, 4);
    buffer_viz_stencil_render(BUFFER_VIZ_STENCIL_COUNT, 1, 0, 0, 4, 4);
    /* In-range mode, but no capture this pass / degenerate rect. */
    buffer_viz_stencil_render(BUFFER_VIZ_STENCIL_PALETTE, 1, 0, 0, 4, 4);
    buffer_viz_stencil_render(BUFFER_VIZ_STENCIL_PALETTE, 0, 0, 0, 0, 0);
    buffer_viz_stencil_capture(0, 0, 0, 0);
    buffer_viz_stencil_render(BUFFER_VIZ_STENCIL_PALETTE, 1, 0, 0, 4, 4);
    AT("render: out-of-range and captureless passes are no-ops", 1);

    /* A mode of Off must retract the published histogram, so a legend
     * built from it disappears together with the overlay. */
    AT("render: OFF leaves no histogram for a legend",
       buffer_viz_stencil_histogram()->valid == 0);
}

static void test_histogram_starts_empty(void) {
    const BufferVizStencilHistogram *hist;

    buffer_viz_stencil_reset();
    hist = buffer_viz_stencil_histogram();
    AT("histogram: accessor is never NULL", hist != NULL);
    AI("histogram: invalid until a scan completes", hist->valid, 0);
    AI("histogram: reset clears the counts", (int)hist->counts[1], 0);
}

int main(void) {
    test_scan_counts_and_extent();
    test_scan_extent_excludes_zero();
    test_scan_all_zero();
    test_scan_degenerate_inputs();
    test_zero_is_transparent_in_every_mode();
    test_palette_is_view_independent();
    test_palette_distinguishes_low_values();
    test_palette_aliases_every_sixteen();
    test_palette_accessor_matches_map();
    test_split_maps_like_palette();
    test_ramp_spans_the_range();
    test_ramp_clamps_outside_the_smoothed_range();
    test_ramp_degenerate_and_unseeded_ranges();
    test_map_degenerate_inputs();
    test_render_rejects_bad_modes();
    test_histogram_starts_empty();
    return test_harness_report(&g_h, "stencil_viz");
}
