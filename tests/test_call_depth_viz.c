/*
 * test_call_depth_viz.c - the pure core behind the colour-by-call-depth
 * view (call_depth_viz_scan / _ramp_rgb / _build_table).
 *
 * Drives the mapping directly with hand-built GLCmd arrays - no flatten,
 * no GL context, no frame. Pins the three properties the view's whole
 * value rests on:
 *
 *   - the scan bins by call_depth and ignores commands that emit nothing;
 *   - the ramp is normalized over the OBSERVED max depth, so a shallow
 *     scene gets the full colour range rather than four adjacent samples
 *     from the low end of a 64-stop ramp;
 *   - the ramp is monotonically warmer with depth, which is the ordering
 *     a categorical palette would have destroyed.
 */
#include "subsystems/call_depth_viz/call_depth_viz.h"
#include "support/test_harness.h"

#include <string.h>

static TestHarness g_h = TEST_HARNESS_INIT;
#define AT(label, cond) TEST_ASSERT_TRUE(&g_h, label, cond)
#define AI(label, got, expected) TEST_ASSERT_INT(&g_h, label, got, expected)

/* One valid command at `depth`. Only `valid` and `call_depth` matter to
 * this module; everything else stays zeroed so a future field can't
 * quietly start influencing the result. */
static GLCmd cmd_at(int depth, int valid) {
    GLCmd c;
    memset(&c, 0, sizeof c);
    c.type = CMD_VERTEX3F;
    c.valid = valid;
    c.call_depth = depth;
    return c;
}

static void test_scan_bins_by_depth(void) {
    GLCmd cmds[6];
    CallDepthVizStats s;

    cmds[0] = cmd_at(0, 1);
    cmds[1] = cmd_at(0, 1);
    cmds[2] = cmd_at(1, 1);
    cmds[3] = cmd_at(3, 1);
    cmds[4] = cmd_at(3, 1);
    cmds[5] = cmd_at(3, 1);
    call_depth_viz_scan(cmds, 6, &s);

    AT("scan: a non-empty program is valid", s.valid);
    AI("scan: depth 0 count", (int)s.counts[0], 2);
    AI("scan: depth 1 count", (int)s.counts[1], 1);
    AI("scan: unoccupied depth 2 counts zero", (int)s.counts[2], 0);
    AI("scan: depth 3 count", (int)s.counts[3], 3);
    AI("scan: max depth is the deepest occupied one", s.max_depth, 3);
    AI("scan: distinct counts occupied depths, not the range", s.distinct, 3);
    AI("scan: total is every command binned", (int)s.total, 6);
}

static void test_scan_skips_invalid(void) {
    GLCmd cmds[3];
    CallDepthVizStats s;

    cmds[0] = cmd_at(0, 1);
    cmds[1] = cmd_at(5, 0);   /* invalid: emits nothing, so it is nothing */
    cmds[2] = cmd_at(1, 1);
    call_depth_viz_scan(cmds, 3, &s);

    AI("scan: invalid command is not counted", (int)s.total, 2);
    AI("scan: invalid command does not deepen the range", s.max_depth, 1);
    AI("scan: invalid command's depth stays empty", (int)s.counts[5], 0);
}

static void test_scan_degenerate_inputs(void) {
    CallDepthVizStats s;
    GLCmd one = cmd_at(0, 0);

    call_depth_viz_scan(NULL, 4, &s);
    AI("scan: NULL program is invalid", s.valid, 0);
    AI("scan: NULL program has no depth", s.max_depth, 0);

    call_depth_viz_scan(&one, 0, &s);
    AI("scan: zero count is invalid", s.valid, 0);

    /* A program of nothing but invalid commands scanned nothing, so it
     * must not claim a range - the legend would show a row for a depth no
     * command occupies. */
    call_depth_viz_scan(&one, 1, &s);
    AI("scan: all-invalid program is invalid", s.valid, 0);
    AI("scan: all-invalid program has no depth", s.max_depth, 0);

    call_depth_viz_scan(NULL, 0, NULL);   /* must not crash */
    AT("scan: NULL output is survivable", 1);
}

static void test_scan_clamps_out_of_range_depth(void) {
    GLCmd cmds[2];
    CallDepthVizStats s;

    cmds[0] = cmd_at(-3, 1);
    cmds[1] = cmd_at(CALL_DEPTH_VIZ_SLOTS + 100, 1);
    call_depth_viz_scan(cmds, 2, &s);

    AI("scan: negative depth clamps to 0", (int)s.counts[0], 1);
    AI("scan: over-deep clamps into the last slot",
       (int)s.counts[CALL_DEPTH_VIZ_SLOTS - 1], 1);
    AI("scan: clamped max stays inside the table",
       s.max_depth, CALL_DEPTH_VIZ_SLOTS - 1);
}

static void test_ramp_spans_the_observed_range(void) {
    float shallow_lo[3], shallow_hi[3];
    float deep_lo[3], deep_hi[3];
    int i;

    /* This is the whole argument for normalizing over the observed max: a
     * scene nesting 3 deep must get the SAME endpoints as one nesting 40
     * deep, or its four levels would be four near-identical blues. */
    call_depth_viz_ramp_rgb(0, 3, shallow_lo);
    call_depth_viz_ramp_rgb(3, 3, shallow_hi);
    call_depth_viz_ramp_rgb(0, 40, deep_lo);
    call_depth_viz_ramp_rgb(40, 40, deep_hi);

    for (i = 0; i < 3; i++) {
        AT("ramp: depth 0 is the same colour at any max depth",
           shallow_lo[i] == deep_lo[i]);
        AT("ramp: the deepest depth is the same colour at any max depth",
           shallow_hi[i] == deep_hi[i]);
    }
    AT("ramp: the cool end is blue-dominant",
       shallow_lo[2] > shallow_lo[0]);
    AT("ramp: the warm end is red-dominant",
       shallow_hi[0] > shallow_hi[2]);
}

static void test_ramp_is_monotonically_warmer(void) {
    int max_depth = 12;
    float prev[3];
    int d, ok_red = 1, ok_blue = 1, ok_distinct = 1;

    call_depth_viz_ramp_rgb(0, max_depth, prev);
    for (d = 1; d <= max_depth; d++) {
        float cur[3];
        call_depth_viz_ramp_rgb(d, max_depth, cur);
        /* Red never falls and blue never rises: "deeper" reads as warmer
         * at every step, which is what makes the ramp an ordering rather
         * than a set of labels. */
        if (cur[0] < prev[0] - 1e-6f) ok_red = 0;
        if (cur[2] > prev[2] + 1e-6f) ok_blue = 0;
        if (cur[0] == prev[0] && cur[1] == prev[1] && cur[2] == prev[2])
            ok_distinct = 0;
        memcpy(prev, cur, sizeof prev);
    }
    AT("ramp: red never decreases with depth", ok_red);
    AT("ramp: blue never increases with depth", ok_blue);
    AT("ramp: adjacent depths are never the same colour", ok_distinct);
}

static void test_ramp_degenerate_inputs(void) {
    float flat[3], cool[3], over[3], under[3];

    /* A program with no funcN calls: one depth, nothing to express. It
     * must land on the cool end rather than dividing by zero. */
    call_depth_viz_ramp_rgb(0, 0, flat);
    call_depth_viz_ramp_rgb(0, 5, cool);
    AT("ramp: max depth 0 is the cool end",
       flat[0] == cool[0] && flat[1] == cool[1] && flat[2] == cool[2]);

    call_depth_viz_ramp_rgb(9, 5, over);
    call_depth_viz_ramp_rgb(5, 5, flat);
    AT("ramp: depth past the max clamps to the warm end",
       over[0] == flat[0] && over[1] == flat[1] && over[2] == flat[2]);

    call_depth_viz_ramp_rgb(-2, 5, under);
    AT("ramp: negative depth clamps to the cool end",
       under[0] == cool[0] && under[1] == cool[1] && under[2] == cool[2]);

    call_depth_viz_ramp_rgb(0, 0, NULL);  /* must not crash */
    AT("ramp: NULL output is survivable", 1);
}

static void test_build_table_matches_the_ramp(void) {
    float table[CALL_DEPTH_VIZ_SLOTS][3];
    float want[3];
    int rows, d, ok = 1;

    rows = call_depth_viz_build_table(4, table, CALL_DEPTH_VIZ_SLOTS);
    AI("table: one row per depth including 0", rows, 5);
    for (d = 0; d < rows; d++) {
        call_depth_viz_ramp_rgb(d, 4, want);
        if (table[d][0] != want[0] || table[d][1] != want[1] ||
            table[d][2] != want[2])
            ok = 0;
    }
    /* The executor tints from the table and the legend swatches call the
     * ramp directly; if these two ever disagree the panel is describing a
     * frame that does not exist. */
    AT("table: every row is the ramp colour for its depth", ok);

    rows = call_depth_viz_build_table(40, table, 3);
    AI("table: a short table truncates to its capacity", rows, 3);

    rows = call_depth_viz_build_table(-1, table, CALL_DEPTH_VIZ_SLOTS);
    AI("table: a negative max still yields the depth-0 row", rows, 1);

    AI("table: NULL storage yields no rows",
       call_depth_viz_build_table(4, NULL, 8), 0);
    AI("table: zero capacity yields no rows",
       call_depth_viz_build_table(4, table, 0), 0);
}

int main(void) {
    test_scan_bins_by_depth();
    test_scan_skips_invalid();
    test_scan_degenerate_inputs();
    test_scan_clamps_out_of_range_depth();
    test_ramp_spans_the_observed_range();
    test_ramp_is_monotonically_warmer();
    test_ramp_degenerate_inputs();
    test_build_table_matches_the_ramp();
    return test_harness_report(&g_h, "call_depth_viz");
}
