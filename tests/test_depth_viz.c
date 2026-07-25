/*
 * test_depth_viz.c - synthetic depth-map tests for the pure conversion
 * core of the depth-buffer visualization (render3d_depth_viz_map).
 *
 * Drives the mapping directly with hand-built float buffers and a fixed
 * Render3dProjectionDesc — no GL context. Pins the linearization math,
 * the near=bright / background=black conventions, the SCENE
 * normalization (EMA smoothing, snap-on-jump, overshoot clamping, the
 * zero-span mid-gray fallback, the empty-scene LINEAR fallback), and
 * the ortho pass-through.
 */
#include "render3d/depth_viz.h"
#include "support/test_harness.h"

#include <string.h>

static TestHarness g_h = TEST_HARNESS_INIT;
#define AT(label, cond) TEST_ASSERT_TRUE(&g_h, label, cond)
#define AI(label, got, expected) TEST_ASSERT_INT(&g_h, label, got, expected)
/* Byte comparisons tolerate +/-1 for float rounding except the exact
 * sentinels 0 (background) and 255 (nearest pixel). */
#define AB(label, got, expected) do { int _d = (int)(got) - (int)(expected); \
    TEST_ASSERT_TRUE(&g_h, label, _d >= -1 && _d <= 1); } while (0)

#define NEAR_Z 0.1f
#define FAR_Z  200.0f

static Render3dProjectionDesc make_desc(Render3dProjectionMode mode) {
    Render3dProjectionDesc d;
    memset(&d, 0, sizeof d);
    d.projection = mode;
    d.fovy_deg   = 45.0;
    d.near_z     = NEAR_Z;
    d.far_z      = FAR_Z;
    d.ortho_near = -FAR_Z;
    d.ortho_far  = FAR_Z;
    return d;
}

/* Window z for a perspective eye distance L — the forward companion of
 * the map's inverse: z = f*(L - n) / (L*(f - n)). */
static float persp_z(float eye_dist) {
    return (FAR_Z * (eye_dist - NEAR_Z)) / (eye_dist * (FAR_Z - NEAR_Z));
}

static void test_linear_perspective(void) {
    Render3dProjectionDesc proj = make_desc(PROJ_PERSPECTIVE);
    float depth[4];
    unsigned char lum[4] = { 7, 7, 7, 7 };

    depth[0] = 0.0f;           /* at the near plane: brightest */
    depth[1] = persp_z(50.0f); /* quarter of the way through near..far */
    depth[2] = persp_z(150.0f);/* three quarters */
    depth[3] = 1.0f;           /* clear depth: background */
    render3d_depth_viz_map(depth, 4, RENDER3D_DEPTH_VIZ_LINEAR, &proj,
                           NULL, lum);

    AI("linear: near plane is full bright", lum[0], 255);
    /* d01 = (50 - 0.1)/(200 - 0.1) = 0.24956 -> 1 - d01 = 0.75044 */
    AB("linear: L=50 maps to ~0.75 gray", lum[1], 191);
    /* d01 = (150 - 0.1)/(200 - 0.1) = 0.74981 -> 1 - d01 = 0.25019 */
    AB("linear: L=150 maps to ~0.25 gray", lum[2], 64);
    AT("linear: monotonic near>far", lum[0] > lum[1] && lum[1] > lum[2]);
    AI("linear: background is black", lum[3], 0);
}

static void test_scene_two_depths(void) {
    Render3dProjectionDesc proj = make_desc(PROJ_PERSPECTIVE);
    Render3dDepthVizRange range = { 0.0f, 0.0f, 0 };
    float depth[3];
    unsigned char lum[3] = { 7, 7, 7 };

    depth[0] = persp_z(2.0f);  /* nearest scene pixel */
    depth[1] = persp_z(5.0f);  /* farthest scene pixel */
    depth[2] = 1.0f;           /* background */
    render3d_depth_viz_map(depth, 3, RENDER3D_DEPTH_VIZ_SCENE, &proj,
                           &range, lum);

    AT("scene: first capture snaps the range", range.valid);
    TEST_ASSERT_FLOAT(&g_h, "scene: range lo = nearest L", range.lo, 2.0f, 1e-3f);
    TEST_ASSERT_FLOAT(&g_h, "scene: range hi = farthest L", range.hi, 5.0f, 1e-3f);
    AI("scene: nearest pixel is full bright", lum[0], 255);
    /* Farthest scene pixel keeps the 0.1 luminance floor: 0.1*255 = 26. */
    AB("scene: farthest pixel keeps the floor", lum[1], 26);
    AI("scene: background is black", lum[2], 0);
}

static void test_scene_zero_span(void) {
    Render3dProjectionDesc proj = make_desc(PROJ_PERSPECTIVE);
    Render3dDepthVizRange range = { 0.0f, 0.0f, 0 };
    float depth[4];
    unsigned char lum[4] = { 7, 7, 7, 7 };
    float z = persp_z(3.0f);

    depth[0] = z;
    depth[1] = z;
    depth[2] = z;
    depth[3] = 1.0f;
    render3d_depth_viz_map(depth, 4, RENDER3D_DEPTH_VIZ_SCENE, &proj,
                           &range, lum);

    /* Constant-depth scene: span == 0 must not divide — every in-range
     * pixel maps to mid-gray (d01 = 0.5 -> 1 - 0.9*0.5 = 0.55). */
    AB("zero-span: constant depth maps to mid-gray", lum[0], 140);
    AI("zero-span: all in-range pixels agree", lum[1], lum[0]);
    AI("zero-span: all in-range pixels agree (2)", lum[2], lum[0]);
    AI("zero-span: background stays black", lum[3], 0);
}

static void test_scene_ema_overshoot_clamps(void) {
    Render3dProjectionDesc proj = make_desc(PROJ_PERSPECTIVE);
    /* Pre-seeded smoothed range [4, 8]. The raw capture below spans
     * [2, 9] (ratio < 2x either way, so EMA smoothing — no snap), which
     * leaves this frame's extremes OUTSIDE the lagging smoothed range. */
    Render3dDepthVizRange range = { 4.0f, 8.0f, 1 };
    float depth[2];
    unsigned char lum[2] = { 7, 7 };

    depth[0] = persp_z(2.0f);
    depth[1] = persp_z(9.0f);
    render3d_depth_viz_map(depth, 2, RENDER3D_DEPTH_VIZ_SCENE, &proj,
                           &range, lum);

    /* EMA step: lo = 4 + 0.25*(2-4) = 3.5, hi = 8 + 0.25*(9-8) = 8.25. */
    TEST_ASSERT_FLOAT(&g_h, "overshoot: lo eased toward raw", range.lo, 3.5f, 1e-3f);
    TEST_ASSERT_FLOAT(&g_h, "overshoot: hi eased toward raw", range.hi, 8.25f, 1e-3f);
    /* Both samples fall outside [3.5, 8.25]; d01 must clamp to [0,1]
     * (no wraparound past the byte edges). */
    AI("overshoot: below-range clamps to full bright", lum[0], 255);
    AB("overshoot: above-range clamps to the far floor", lum[1], 26);
}

static void test_scene_snap_on_jump(void) {
    Render3dProjectionDesc proj = make_desc(PROJ_PERSPECTIVE);
    /* Smoothed span 1 vs raw span 30: past the 2x ratio, must snap
     * (example switches shouldn't lag through the EMA). */
    Render3dDepthVizRange range = { 2.0f, 3.0f, 1 };
    float depth[2];
    unsigned char lum[2] = { 7, 7 };

    depth[0] = persp_z(10.0f);
    depth[1] = persp_z(40.0f);
    render3d_depth_viz_map(depth, 2, RENDER3D_DEPTH_VIZ_SCENE, &proj,
                           &range, lum);

    TEST_ASSERT_FLOAT(&g_h, "snap: lo jumps to raw", range.lo, 10.0f, 1e-2f);
    TEST_ASSERT_FLOAT(&g_h, "snap: hi jumps to raw", range.hi, 40.0f, 1e-2f);
    AI("snap: nearest maps full bright", lum[0], 255);
    AB("snap: farthest maps to the floor", lum[1], 26);
}

static void test_scene_empty_falls_back_to_linear(void) {
    Render3dProjectionDesc proj = make_desc(PROJ_PERSPECTIVE);
    Render3dDepthVizRange range = { 0.0f, 0.0f, 0 };
    float depth[3] = { 1.0f, 1.0f, 1.0f };
    unsigned char lum[3] = { 7, 7, 7 };

    render3d_depth_viz_map(depth, 3, RENDER3D_DEPTH_VIZ_SCENE, &proj,
                           &range, lum);

    AI("empty: all background maps to black", lum[0], 0);
    AI("empty: all background maps to black (2)", lum[2], 0);
    AT("empty: EMA range stays unseeded", !range.valid);
}

static void test_split_maps_like_scene(void) {
    Render3dProjectionDesc proj = make_desc(PROJ_PERSPECTIVE);
    Render3dDepthVizRange range_scene = { 0.0f, 0.0f, 0 };
    Render3dDepthVizRange range_split = { 0.0f, 0.0f, 0 };
    float depth[2];
    unsigned char lum_scene[2] = { 7, 7 };
    unsigned char lum_split[2] = { 7, 7 };

    depth[0] = persp_z(2.0f);
    depth[1] = persp_z(5.0f);
    render3d_depth_viz_map(depth, 2, RENDER3D_DEPTH_VIZ_SCENE, &proj,
                           &range_scene, lum_scene);
    render3d_depth_viz_map(depth, 2, RENDER3D_DEPTH_VIZ_SPLIT, &proj,
                           &range_split, lum_split);

    AI("split: byte 0 identical to scene mode", lum_split[0], lum_scene[0]);
    AI("split: byte 1 identical to scene mode", lum_split[1], lum_scene[1]);
}

static void test_ortho_passthrough(void) {
    Render3dProjectionDesc proj = make_desc(PROJ_ORTHO);
    Render3dDepthVizRange range = { 0.0f, 0.0f, 0 };
    float depth[3] = { 0.25f, 0.75f, 1.0f };
    unsigned char lum[3] = { 7, 7, 7 };

    /* LINEAR ortho: window z is already linear -> d01 = z directly. */
    render3d_depth_viz_map(depth, 3, RENDER3D_DEPTH_VIZ_LINEAR, &proj,
                           NULL, lum);
    AB("ortho linear: z=0.25 -> 0.75 gray", lum[0], 191);
    AB("ortho linear: z=0.75 -> 0.25 gray", lum[1], 64);
    AI("ortho linear: background black", lum[2], 0);

    /* SCENE ortho: normalized over the raw z extent. */
    render3d_depth_viz_map(depth, 3, RENDER3D_DEPTH_VIZ_SCENE, &proj,
                           &range, lum);
    TEST_ASSERT_FLOAT(&g_h, "ortho scene: range lo = min z", range.lo, 0.25f, 1e-4f);
    TEST_ASSERT_FLOAT(&g_h, "ortho scene: range hi = max z", range.hi, 0.75f, 1e-4f);
    AI("ortho scene: nearest full bright", lum[0], 255);
    AB("ortho scene: farthest at the floor", lum[1], 26);
}

static void test_degenerate_inputs(void) {
    Render3dProjectionDesc proj = make_desc(PROJ_PERSPECTIVE);
    float depth[1] = { 0.5f };
    unsigned char lum[1] = { 7 };

    /* Null/empty calls must be safe no-ops. */
    render3d_depth_viz_map(NULL, 1, RENDER3D_DEPTH_VIZ_LINEAR, &proj, NULL, lum);
    render3d_depth_viz_map(depth, 0, RENDER3D_DEPTH_VIZ_LINEAR, &proj, NULL, lum);
    render3d_depth_viz_map(depth, 1, RENDER3D_DEPTH_VIZ_LINEAR, NULL, NULL, lum);
    render3d_depth_viz_map(depth, 1, RENDER3D_DEPTH_VIZ_LINEAR, &proj, NULL, NULL);
    AI("degenerate: output untouched by no-op calls", lum[0], 7);

    /* SCENE with no range state supplied falls back to LINEAR. */
    render3d_depth_viz_map(depth, 1, RENDER3D_DEPTH_VIZ_SCENE, &proj,
                           NULL, lum);
    AT("degenerate: scene w/o range still writes", lum[0] != 7);
}

/* Mode-range validation belongs to this module, not to the renderer:
 * render3d fires a neutral buffer hook and never sees a viz mode, so an
 * out-of-range value has to bounce off render3d_depth_viz_render itself.
 * (It used to be rejected by validate_render_config, back when the mode
 * travelled as a Render3dRenderConfig field.)
 *
 * Every rejection path below returns before the first GL call, so this
 * runs with no GL context in either build: an out-of-range mode is
 * refused up front, and a valid mode with no capture from this frame
 * refuses on the capture check. */
static void test_render_rejects_bad_modes(void) {
    Render3dProjectionDesc proj = make_desc(PROJ_PERSPECTIVE);

    render3d_depth_viz_reset();
    render3d_depth_viz_render((Render3dDepthVizMode)-1, &proj, 0, 0, 4, 4);
    render3d_depth_viz_render(RENDER3D_DEPTH_VIZ_COUNT, &proj, 0, 0, 4, 4);
    render3d_depth_viz_render((Render3dDepthVizMode)(RENDER3D_DEPTH_VIZ_COUNT + 7),
                              &proj, 0, 0, 4, 4);
    /* OFF is in range but draws nothing. */
    render3d_depth_viz_render(RENDER3D_DEPTH_VIZ_OFF, &proj, 0, 0, 4, 4);
    /* In-range mode, but no capture this frame / degenerate rect. */
    render3d_depth_viz_render(RENDER3D_DEPTH_VIZ_LINEAR, &proj, 0, 0, 4, 4);
    render3d_depth_viz_render(RENDER3D_DEPTH_VIZ_LINEAR, NULL, 0, 0, 4, 4);
    render3d_depth_viz_render(RENDER3D_DEPTH_VIZ_LINEAR, &proj, 0, 0, 0, 0);
    AT("render: out-of-range and captureless modes are no-ops", 1);

    /* A degenerate rect never produces a capture to consume, so the
     * render above stays refused rather than reading a stale buffer. */
    render3d_depth_viz_capture(0, 0, 0, 0);
    render3d_depth_viz_render(RENDER3D_DEPTH_VIZ_LINEAR, &proj, 0, 0, 4, 4);
    AT("render: empty capture leaves nothing to draw", 1);
}

int main(void) {
    test_linear_perspective();
    test_scene_two_depths();
    test_scene_zero_span();
    test_scene_ema_overshoot_clamps();
    test_scene_snap_on_jump();
    test_scene_empty_falls_back_to_linear();
    test_split_maps_like_scene();
    test_ortho_passthrough();
    test_degenerate_inputs();
    test_render_rejects_bad_modes();
    return test_harness_report(&g_h, "depth_viz");
}
