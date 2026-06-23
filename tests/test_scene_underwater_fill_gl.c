/*
 * test_scene_underwater_fill_gl.c - real-GL regression for the OCEAN
 * underwater color fill.
 *
 * Bug: after fb976f0 (the scoped GL_NV_fog_distance opt-in for the
 * city / ocean / radar passes), the OCEAN underwater fill was rendering
 * as a tiny teal patch in the lower-left of the scene viewport instead
 * of filling it. Cause: render3d_grid_render_ocean_theme draws the fill as
 *
 *     gluOrtho2D(0, render3d_w, 0, render3d_h);
 *     // ...glLoadIdentity modelview, glPushAttrib(DEPTH|LIGHTING)...
 *     glRectf(0, 0, render3d_w, render3d_h);
 *
 * which puts the rect's eye-space vertices at coordinates up to
 * (render3d_w, render3d_h, 0) — radial distances 0 .. sqrt(W^2+H^2). The
 * outer grid pass leaves GL_FOG enabled and, under fb976f0's NV opt-in,
 * has just switched the OCEAN case to GL_EYE_RADIAL_NV. So with linear
 * fog whose end is keyed to a normal world-space grid extent (single
 * digits), every rect pixel except the few near the (0,0) corner is
 * fully past fog-end and ends up at fog colour. The underwater
 * glPushAttrib only saves GL_DEPTH_BUFFER_BIT | GL_LIGHTING_BIT — fog
 * state isn't bracketed, and there's no glDisable(GL_FOG) before the
 * rect.
 *
 * Why a real-GL test
 * ------------------
 * The stub harness counts calls but does not rasterize, so it can't
 * tell "the rect filled the viewport" from "the rect was fogged to a
 * lower-left sliver"; both make the same glRectf. This test creates a
 * real GLUT context, drives render3d_grid_render(GRID_THEME_OCEAN) with
 * cam_world_y < 0 and nv_fog_distance_supported = 1, then glReadPixels
 * the framebuffer and asserts the four corners + centre all look
 * teal-tinted. The radial-fog interaction reproduces here because the
 * test sets the config flag itself — no live-pipeline state needed.
 *
 * Tokens: GL_FOG_DISTANCE_MODE_NV / GL_EYE_RADIAL_NV are defined as
 * registry-value fallbacks in scene/render_types.h when glext isn't
 * pulled in (e.g. the Apple-GLUT framework path), so the test compiles
 * even on machines without the extension. If the runtime driver
 * doesn't support GL_NV_fog_distance, glFogi(0x855A, ...) silently
 * generates GL_INVALID_ENUM and the radial mode never takes effect —
 * in that case the test passes (because the bug can't fire on that
 * machine either) but does NOT prove anything: see SKIP message.
 *
 * Like test_ui_gl_state this is in GL_TEST_BINS / `make gl-tests`,
 * NOT in `make test`. Needs a display.
 */
#include "gl_includes.h"
#include "render3d/grid.h"
#include "render3d/render_types.h"
#include "render3d/themes.h"
#include "support/test_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;
#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)

static void test_execute_noop(const Render3dExecuteContext *ctx, void *ud) {
    (void)ctx;
    (void)ud;
}

/* Build a config that lands in render3d_grid_render_ocean_theme's underwater
 * branch (cam_world_y < 0) with the GL_NV_fog_distance opt-in active. */
static Render3dFrameRenderContext make_underwater_ctx(int render3d_w, int render3d_h) {
    Render3dFrameRenderContext ctx = {0};
    ctx.config.execute_fn = test_execute_noop;
    ctx.config.render3d_x = 0;
    ctx.config.render3d_y = 0;
    ctx.config.render3d_w = render3d_w;
    ctx.config.render3d_h = render3d_h;
    ctx.config.grid_theme = GRID_THEME_OCEAN;
    ctx.config.grid_opacity = 1.0f;
    ctx.config.alpha_scale = 1.0f;
    ctx.config.grid_extent_idx = GRID_EXTENT_MID;
    ctx.config.grid_major_idx = GRID_MAJOR_1;
    for (int i = 0; i < GRID_MAJOR_COUNT; i++) ctx.config.grid_major_steps[i] = 1.0f;
    /* MID extent is what the live whale.c uses (@cfg grid_extent = 2); the
     * fog end is keyed to extent, so this reproduces the live numerics. */
    ctx.config.grid_extents[GRID_EXTENT_CLOSE] = 5.0f;
    ctx.config.grid_extents[GRID_EXTENT_MID]   = 25.0f;
    ctx.config.grid_extents[GRID_EXTENT_FAR]   = 100.0f;
    ctx.config.cam_dist = 5.0f;
    ctx.config.cam_rx = 0.0f;
    ctx.config.cam_ty = -5.0f;   /* under the water plane */
    ctx.config.projection_mix = 1.0f;
    /* The flag that arms the bug. The dispatcher in render3d_grid_render
     * calls glFogi(GL_FOG_DISTANCE_MODE_NV, GL_EYE_RADIAL_NV) when this
     * is set, then enters render3d_grid_render_ocean_theme. */
    ctx.config.nv_fog_distance_supported = 1;
    return ctx;
}

static void sample_px(int px, int py, int vp_w, int vp_h,
                      const unsigned char *buf, unsigned char out[3]) {
    if (px < 0) px = 0; if (px >= vp_w) px = vp_w - 1;
    if (py < 0) py = 0; if (py >= vp_h) py = vp_h - 1;
    size_t off = ((size_t)py * (size_t)vp_w + (size_t)px) * 3u;
    out[0] = buf[off];
    out[1] = buf[off + 1];
    out[2] = buf[off + 2];
}

/* The fill is glColor4f(0.05, 0.25, 0.35, 0.75) src-over against the
 * black clear. Post-blend that's roughly (10, 48, 67). Threshold on the
 * green channel — it's the strongest signal and survives small tweaks
 * to the underwater RGBA literal. */
static int looks_teal(const unsigned char rgb[3]) {
    return rgb[1] > 30 && rgb[2] > rgb[1] && rgb[0] < rgb[1];
}

static void test_underwater_fill_covers_viewport(void) {
    const int W = 1200;
    const int H = 440;
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Detection match: render3d_grid_render uses
     * Render3dRenderConfig.nv_fog_distance_supported (set in our config
     * helper). But the radial-fog *effect* only happens if the runtime
     * driver actually honours GL_FOG_DISTANCE_MODE_NV. Probe explicitly
     * and report skip if not — otherwise a green test on a hardware
     * that can't trigger the bug would be a silent false positive. */
    int radial_supported = glutExtensionSupported("GL_NV_fog_distance") ? 1 : 0;
    if (!radial_supported) {
        fprintf(stderr,
                "test_scene_underwater_fill_gl: GL_NV_fog_distance not "
                "advertised; the radial-fog interaction can't fire on "
                "this driver. Test passes vacuously.\n");
    }

    /* Pre-rect outer fog state: simulate the grid_apply_far_fog tail
     * that the live frame would leave enabled before grid_dispatch_theme
     * runs. Linear fog with end keyed to the grid extent (MID = 25.0f
     * here), so anything past ~25 eye-radial units fades to fog colour.
     * (Even without this, the FAR-extent branch of the live grid pass
     * enables fog the same way; the rect inherits it.) */
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 25.0f * 0.7f);
    glFogf(GL_FOG_END,   25.0f);
    float fog_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    glFogfv(GL_FOG_COLOR, fog_color);

    Render3dFrameRenderContext ctx = make_underwater_ctx(W, H);
    render3d_grid_render(&ctx);
    glFinish();

    unsigned char *buf = (unsigned char *)malloc((size_t)W * (size_t)H * 3u);
    if (!buf) { ASSERT_TRUE("alloc readback buffer", 0); return; }
    glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, buf);

    unsigned char c_bl[3], c_br[3], c_tl[3], c_tr[3], c_mid[3];
    sample_px(8,       8,       W, H, buf, c_bl);
    sample_px(W - 9,   8,       W, H, buf, c_br);
    sample_px(8,       H - 9,   W, H, buf, c_tl);
    sample_px(W - 9,   H - 9,   W, H, buf, c_tr);
    sample_px(W / 2,   H / 2,   W, H, buf, c_mid);

    /* Always log the samples — the failure mode (only c_bl teal, the
     * others fog colour) is the live bug and worth seeing in CI logs
     * even when it passes. */
    fprintf(stderr,
            "underwater_fill samples (RGB):\n"
            "  bottom-left  = (%u, %u, %u)\n"
            "  bottom-right = (%u, %u, %u)\n"
            "  top-left     = (%u, %u, %u)\n"
            "  top-right    = (%u, %u, %u)\n"
            "  centre       = (%u, %u, %u)\n",
            c_bl[0],  c_bl[1],  c_bl[2],
            c_br[0],  c_br[1],  c_br[2],
            c_tl[0],  c_tl[1],  c_tl[2],
            c_tr[0],  c_tr[1],  c_tr[2],
            c_mid[0], c_mid[1], c_mid[2]);

    ASSERT_TRUE("underwater fill reaches bottom-left",  looks_teal(c_bl));
    ASSERT_TRUE("underwater fill reaches bottom-right", looks_teal(c_br));
    ASSERT_TRUE("underwater fill reaches top-left",     looks_teal(c_tl));
    ASSERT_TRUE("underwater fill reaches top-right",    looks_teal(c_tr));
    ASSERT_TRUE("underwater fill reaches centre",       looks_teal(c_mid));

    free(buf);
}

int main(int argc, char **argv) {
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE | GLUT_DEPTH);
    glutInitWindowSize(1200, 440);
    if (glutCreateWindow("underwater-fill-test") <= 0) {
        fprintf(stderr, "test_scene_underwater_fill_gl: no GL context "
                        "(need a display); skipping\n");
        return 0;
    }

    printf("--- scene underwater-fill tests (real GL context) ---\n");
    test_underwater_fill_covers_viewport();
    return test_harness_report(&g_harness, "render3d_underwater_fill_gl");
}
