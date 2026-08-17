/*
 * call_depth_viz.c - see call_depth_viz.h.
 *
 * Pure: no GL, no globals, no state. Both entry points are functions of
 * their arguments alone, which is what lets the tests drive them on
 * synthetic command arrays with no context at all.
 */
#include "subsystems/call_depth_viz/call_depth_viz.h"

#include <string.h>

/* Ramp stops, cool -> warm. Four rather than two: a straight azure-to-coral
 * lerp passes through a muddy mauve at the midpoint, so the middle depths of
 * any scene deeper than two would be the least legible ones - exactly
 * backwards. Routing through cyan and gold keeps every interval a real hue
 * step, and the sweep stays monotonically warmer so the ordering survives a
 * greyscale screenshot as well as it can. "Monotonically warmer" is a
 * property of these numbers, not a wish: red never falls and blue never
 * rises from one stop to the next, and test_call_depth_viz asserts it - so
 * a retune that put a bluer colour deeper would fail rather than quietly
 * break the ordering the whole view exists to show.
 *
 * Kept here rather than in ui_theme for the same reason the stencil palette
 * is: this is data the mapping core indexes, not chrome a theme restyles. */
#define CDV_STOPS 4
static const float k_ramp[CDV_STOPS][3] = {
    { 0.16f, 0.52f, 0.96f },   /* azure  - depth 0 */
    { 0.22f, 0.80f, 0.76f },   /* teal              */
    { 0.98f, 0.80f, 0.32f },   /* gold              */
    { 1.00f, 0.44f, 0.30f },   /* coral  - deepest  */
};

void call_depth_viz_scan(const GLCmd *cmds, int count,
                         CallDepthVizStats *out) {
    int i;

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->max_depth = 0;
    if (!cmds || count <= 0)
        return;

    for (i = 0; i < count; i++) {
        int d = cmds[i].call_depth;
        if (!cmds[i].valid)
            continue;
        /* Clamp rather than trust: flatten cannot produce a depth outside
         * this range, but the array arrives from a caller and an
         * out-of-range read here would be the bug, not the diagnosis. */
        if (d < 0)
            d = 0;
        if (d >= CALL_DEPTH_VIZ_SLOTS)
            d = CALL_DEPTH_VIZ_SLOTS - 1;
        if (out->counts[d] == 0)
            out->distinct++;
        out->counts[d]++;
        out->total++;
        if (d > out->max_depth)
            out->max_depth = d;
    }
    out->valid = (out->total > 0);
    if (!out->valid)
        out->max_depth = 0;
}

void call_depth_viz_ramp_rgb(int depth, int max_depth, float rgb_out[3]) {
    float pos, frac;
    int seg, i;

    if (!rgb_out)
        return;
    if (depth < 0)
        depth = 0;
    if (max_depth < 0)
        max_depth = 0;
    if (depth > max_depth)
        depth = max_depth;

    if (max_depth == 0) {
        /* One depth in the whole program: the ramp has nothing to express,
         * so everything sits at the cool end. A uniformly azure scene is
         * the honest picture of "no funcN calls here". */
        rgb_out[0] = k_ramp[0][0];
        rgb_out[1] = k_ramp[0][1];
        rgb_out[2] = k_ramp[0][2];
        return;
    }

    pos = ((float)depth / (float)max_depth) * (float)(CDV_STOPS - 1);
    seg = (int)pos;
    if (seg >= CDV_STOPS - 1) {
        seg = CDV_STOPS - 2;
        frac = 1.0f;
    } else {
        frac = pos - (float)seg;
    }
    for (i = 0; i < 3; i++)
        rgb_out[i] = k_ramp[seg][i] +
                     (k_ramp[seg + 1][i] - k_ramp[seg][i]) * frac;
}

int call_depth_viz_build_table(int max_depth, float (*table)[3], int cap) {
    int rows, d;

    if (!table || cap <= 0)
        return 0;
    if (max_depth < 0)
        max_depth = 0;
    rows = max_depth + 1;
    if (rows > cap)
        rows = cap;
    for (d = 0; d < rows; d++)
        call_depth_viz_ramp_rgb(d, max_depth, table[d]);
    return rows;
}
