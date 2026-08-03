/*
 * test_render3d_palette.c - Palette integrity for render3d/palette.h.
 *
 * Header-only target: links no project objects (mirrors
 * test_ui_theme / test_repl_code_panel_layout). The STATIC_ASSERT in
 * palette.h already guards the token count at compile time; this
 * exercises the data and the render3d_rgba accessor. render3d_clr /
 * render3d_clr_a are not invoked (they call glColor4f and need a live GL
 * context, exactly as test_ui_theme leaves ui_clr uncalled).
 */
#include "render3d/palette.h"
#include "support/test_harness.h"

#include <stdio.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond)  TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_F(label, got, exp) TEST_ASSERT_FLOAT_DEFAULT(&g_harness, label, got, exp)

int main(void) {
    /* 1. Every token is initialized. A designated-init gap leaves the
     * slot zero-filled; every real token carries alpha 1.0, so
     * alpha == 0 reliably flags a missing entry. This also confirms
     * RENDER3D_CLR_OUTLINE_EDGE (pure black RGB) is a real entry, not a
     * gap, because its alpha is still 1.0. */
    for (int t = 0; t < RENDER3D_CLR_COUNT; t++)
        ASSERT_TRUE("no zeroed/uninitialized token slot",
                    g_scene_palette[t].a > 0.0f);

    /* 2. render3d_rgba() returns the table row by value. */
    for (int t = 0; t < RENDER3D_CLR_COUNT; t++) {
        Render3dRgba c = render3d_rgba((Render3dColorToken)t);
        ASSERT_TRUE("render3d_rgba matches table",
                    c.r == g_scene_palette[t].r &&
                    c.g == g_scene_palette[t].g &&
                    c.b == g_scene_palette[t].b &&
                    c.a == g_scene_palette[t].a);
    }

    /* 3. Visual-no-op anchors: a few tokens transcribed verbatim from
     * the literals they replaced. A drift here means the sweep changed
     * an on-screen color. */
    {
        Render3dRgba a = render3d_rgba(RENDER3D_CLR_OUTLINE_ACTIVE);
        ASSERT_F("outline-active R", a.r, 0.00f);
        ASSERT_F("outline-active G", a.g, 0.90f);
        ASSERT_F("outline-active B", a.b, 0.90f);

        Render3dRgba lbl = render3d_rgba(RENDER3D_CLR_VERTEX_LABEL);
        ASSERT_F("vertex-label R", lbl.r, 1.00f);
        ASSERT_F("vertex-label G", lbl.g, 1.00f);
        ASSERT_F("vertex-label B", lbl.b, 0.30f);

        Render3dRgba ref = render3d_rgba(RENDER3D_CLR_GUIDE_REF);
        ASSERT_F("guide-ref gray R", ref.r, 0.55f);
        ASSERT_F("guide-ref gray G", ref.g, 0.55f);
        ASSERT_F("guide-ref gray B", ref.b, 0.55f);

        Render3dRgba edge = render3d_rgba(RENDER3D_CLR_OUTLINE_EDGE);
        ASSERT_F("outline-edge is black RGB", edge.r + edge.g + edge.b, 0.0f);
        ASSERT_F("outline-edge alpha is 1.0 (real entry)", edge.a, 1.0f);

        Render3dRgba wire_vis = render3d_rgba(RENDER3D_CLR_WIREFRAME_VISIBLE);
        ASSERT_F("wireframe-visible R", wire_vis.r, 0.96f);
        ASSERT_F("wireframe-visible G", wire_vis.g, 0.98f);
        ASSERT_F("wireframe-visible B", wire_vis.b, 1.00f);

        Render3dRgba wire_hid = render3d_rgba(RENDER3D_CLR_WIREFRAME_HIDDEN);
        ASSERT_F("wireframe-hidden R", wire_hid.r, 0.30f);
        ASSERT_F("wireframe-hidden G", wire_hid.g, 0.38f);
        ASSERT_F("wireframe-hidden B", wire_hid.b, 0.52f);
    }

    return test_harness_report(&g_harness, "test_scene_palette");
}
