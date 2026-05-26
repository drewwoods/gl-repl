/*
 * test_scene_underwater_fill_gl.c - real-GL contract test for the
 * OCEAN-theme underwater color fill.
 *
 * Drives scene_grid_render with grid_theme = GRID_THEME_OCEAN and a
 * camera under the water plane (cam_world_y < 0), then samples five
 * pixels of the resulting framebuffer (four corners + centre) and
 * asserts every one is teal-tinted. The contract is the obvious one:
 * the underwater fill must cover the entire scene viewport.
 *
 * Why a real-GL test
 * ------------------
 * The stub harness (tests/gl-stubs/) does not rasterize, so it cannot
 * distinguish "the rect filled the viewport" from "the rect drew at the
 * wrong scale"; both call glRectf once. A glReadPixels check needs a
 * real GL context.
 *
 * Scope and history
 * -----------------
 * Commit 905cb0d (May 25) reworked the underwater-fill block of
 * scene_grid_render_ocean_theme:
 *
 *   - corrected the depth-test push/pop ordering (depth_test save now
 *     happens INSIDE glPushAttrib), and
 *   - migrated the rect coordinates from viewport_w/h (window-pixel
 *     fields, removed in the next commit) to scene_w/h (scene-rect
 *     dimensions).
 *
 * Field-observed regression after 905cb0d: the `gluOrtho2D(0, scene_w,
 * 0, scene_h)` + `glRectf(0, 0, scene_w, scene_h)` form on the dev
 * machine's freeglut + Apple OpenGL stack rendered the rect at a tiny
 * fraction of the viewport (only the lower-left corner). The math is
 * invariant in the rect/ortho range, so this looks like a driver-level
 * interaction with state set up by the rest of the 3D pipeline (the
 * outer scene_pass_setup attrib push, the perspective projection at the
 * bottom of the projection stack, etc.). The fix the code uses now
 * sidesteps the ortho scaling entirely: render in NDC with identity
 * matrices and `glRectf(-1, -1, 1, 1)`.
 *
 * In its current isolated form the test does NOT reproduce the
 * driver-specific corner-only failure — that bug needed live-pipeline
 * state we have not managed to bottle. What the test DOES catch is any
 * future change that breaks the contract more straightforwardly: a
 * scissor leak, a depth-test that excludes the fill, blend disabled,
 * matrix-stack imbalance that lands the rect off-viewport, etc. That
 * still keeps "the rect must cover the viewport" pinned in CI rather
 * than relying on a manual eyeball test.
 *
 * Like test_ui_gl_state, this is in GL_TEST_BINS, not TEST_BINS — it
 * needs a display and runs via `make gl-tests`, not `make test`.
 */
#include "gl_includes.h"
#include "scene/grid.h"
#include "scene/render_types.h"
#include "scene/themes.h"
#include "support/test_harness.h"

#include <stdio.h>
#include <stdlib.h>

static TestHarness g_harness = TEST_HARNESS_INIT;
#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)

/* Stand-in for the scene-executor callback. The grid pass never invokes
 * execute_fn (it draws its own grid/water geometry), but the config
 * validator in scene_grid_render expects a populated wrapper, and a
 * defensive NULL check would mask future breakage of that contract. */
static void test_execute_noop(const SceneExecuteContext *ctx, void *ud) {
    (void)ctx;
    (void)ud;
}

/* Build a scene config that places the camera under the water plane
 * (cam_world_y < 0) so scene_grid_render_ocean_theme takes the underwater
 * branch. */
static SceneFrameRenderContext make_underwater_ctx(int scene_w, int scene_h) {
    SceneFrameRenderContext ctx = {0};
    ctx.config.execute_fn = test_execute_noop;
    ctx.config.scene_x = 0;
    ctx.config.scene_y = 0;
    ctx.config.scene_w = scene_w;
    ctx.config.scene_h = scene_h;
    ctx.config.grid_theme = GRID_THEME_OCEAN;
    ctx.config.grid_opacity = 1.0f;
    ctx.config.alpha_scale = 1.0f;
    ctx.config.grid_extent_idx = 0;
    ctx.config.grid_major_idx = 0;
    for (int i = 0; i < GRID_MAJOR_COUNT; i++) ctx.config.grid_major_steps[i] = 1.0f;
    for (int i = 0; i < GRID_EXTENT_COUNT; i++) ctx.config.grid_extents[i] = 10.0f;
    /* cam_ty + sin(cam_rx) * cam_dist must be < 0. With cam_rx = 0 the
     * camera is at world Y = cam_ty, so a negative target Y suffices. */
    ctx.config.cam_dist = 5.0f;
    ctx.config.cam_rx = 0.0f;
    ctx.config.cam_ry = 0.0f;
    ctx.config.cam_ty = -5.0f;
    ctx.config.projection_mix = 1.0f;
    return ctx;
}

/* Sample the framebuffer at (px, py) in viewport coords (origin at bottom-
 * left, matching glReadPixels). Returns the 8-bit RGB triple in `out`. */
static void sample_px(int px, int py, int vp_w, int vp_h,
                      const unsigned char *buf, unsigned char out[3]) {
    if (px < 0) px = 0; if (px >= vp_w) px = vp_w - 1;
    if (py < 0) py = 0; if (py >= vp_h) py = vp_h - 1;
    size_t off = ((size_t)py * (size_t)vp_w + (size_t)px) * 3u;
    out[0] = buf[off];
    out[1] = buf[off + 1];
    out[2] = buf[off + 2];
}

/* The underwater fill is `glColor4f(0.05, 0.25, 0.35, 0.75)` blended over
 * whatever was drawn beneath it (which, in the test, is the clear color).
 * With clear=(0,0,0) and src_alpha=0.75, the post-blend RGB is roughly
 * (0.05*0.75, 0.25*0.75, 0.35*0.75) ≈ (10, 48, 67) in 8-bit. The
 * green channel is the strongest visual signature. We assert the sample's
 * green > 30 and blue > green (teal characteristic) instead of pinning a
 * tight numeric value, so subtle changes to the underwater RGBA literal
 * don't break the test for cosmetic reasons. */
static int looks_teal(const unsigned char rgb[3]) {
    return rgb[1] > 30 && rgb[2] > rgb[1] && rgb[0] < rgb[1];
}

static void test_underwater_fill_covers_viewport(void) {
    /* Match the scene-rect dimensions seen in the field bug report
     * (~1200 x 440). Smaller dimensions still pass the original code
     * — the regression scales with the ortho range. */
    const int W = 1200;
    const int H = 440;
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Mirror scene_pass_setup's state at the point it would hand off to
     * scene_grid_render in the live frame:
     *   PROJECTION = perspective matched to the viewport aspect
     *   MODELVIEW  = camera transform
     *   GL_ALL_ATTRIB_BITS pushed (the outer pass save).
     * Pre-905cb0d the underwater path's ortho2D worked from this state;
     * the regression test below pins that. */
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    GLdouble aspect = (GLdouble)W / (GLdouble)H;
    GLdouble fovy_rad = 45.0 * (3.14159265358979 / 180.0);
    GLdouble top = 0.1 * tan(fovy_rad * 0.5);
    GLdouble right = top * aspect;
    glFrustum(-right, right, -top, top, 0.1, 1000.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    /* Camera at world Y = -5 (under the water plane), looking forward. */
    glTranslatef(0.0f, 5.0f, -10.0f);

    SceneFrameRenderContext ctx = make_underwater_ctx(W, H);
    scene_grid_render(&ctx);
    glFinish();

    unsigned char *buf = (unsigned char *)malloc((size_t)W * (size_t)H * 3u);
    if (!buf) { ASSERT_TRUE("alloc readback buffer", 0); return; }
    glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, buf);

    /* Five samples that the post-905cb0d ortho2D(scene_w, scene_h) +
     * glRectf(0, 0, scene_w, scene_h) regression failed to fill: all four
     * corners (inset by 8 px to dodge edge AA) plus the geometric centre. */
    unsigned char c_bl[3], c_br[3], c_tl[3], c_tr[3], c_mid[3];
    sample_px(8,       8,       W, H, buf, c_bl);
    sample_px(W - 9,   8,       W, H, buf, c_br);
    sample_px(8,       H - 9,   W, H, buf, c_tl);
    sample_px(W - 9,   H - 9,   W, H, buf, c_tr);
    sample_px(W / 2,   H / 2,   W, H, buf, c_mid);

    ASSERT_TRUE("underwater fill reaches bottom-left",  looks_teal(c_bl));
    ASSERT_TRUE("underwater fill reaches bottom-right", looks_teal(c_br));
    ASSERT_TRUE("underwater fill reaches top-left",     looks_teal(c_tl));
    ASSERT_TRUE("underwater fill reaches top-right",    looks_teal(c_tr));
    ASSERT_TRUE("underwater fill reaches centre",       looks_teal(c_mid));

    /* If any corner failed, print the samples so the failure mode (the
     * pre-fix bug rendered ONLY the bottom-left as teal) is obvious in
     * the log without having to dump the framebuffer. */
    if (!(looks_teal(c_bl) && looks_teal(c_br) &&
          looks_teal(c_tl) && looks_teal(c_tr) && looks_teal(c_mid))) {
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
    }

    free(buf);
    glPopAttrib();
}

int main(int argc, char **argv) {
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE | GLUT_DEPTH);
    /* Match the live app's window>viewport configuration: window is wider
     * than the scene rect (the REPL code panel takes the rest of the
     * window). The bug specifically scales with the ortho range vs the
     * actual window/framebuffer geometry. */
    glutInitWindowSize(1600, 440);
    if (glutCreateWindow("underwater-fill-test") <= 0) {
        fprintf(stderr, "test_scene_underwater_fill_gl: no GL context "
                        "(need a display); skipping\n");
        return 0;
    }

    printf("--- scene underwater-fill tests (real GL context) ---\n");
    test_underwater_fill_covers_viewport();
    return test_harness_report(&g_harness, "scene_underwater_fill_gl");
}
