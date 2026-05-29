/*
 * test_scene_render.c - Unit tests for scene_* modules (grid, axes, backdrop, lights, overlays).
 * Tests operate independently of repl_state. Stub builds exercise the GL call
 * paths through no-op counters; real-GL builds avoid opening a GUI window.
 */
#include "scene/grid.h"
#include "scene/axes.h"
#include "scene/backdrop.h"
#include "scene/lights.h"
#include "scene/overlays.h"
#include "scene/guides/geometry_guides.h"
#include "scene/render.h"
#include "scene/render_types.h"

#include "support/test_harness.h"
#include <stdio.h>
#include <string.h>
#include "gl_includes.h"

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    TEST_ASSERT_INT(&g_harness, label, got, exp); \
} while (0)

#define ASSERT_FLOAT(label, got, exp) do { \
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, label, got, exp); \
} while (0)

/* Minimal execute callback for testing. */
static void test_execute_noop(const SceneExecuteContext *ctx, void *user_data) {
    (void)ctx;
    (void)user_data;
}

/* Build a test SceneRenderConfig with sensible defaults. */
static SceneRenderConfig make_test_config(void) {
    SceneRenderConfig cfg = {0};
    cfg.execute_fn = test_execute_noop;
    cfg.execute_user_data = NULL;

    cfg.anim_time = 0.0f;

    cfg.use_accum = 1;
    cfg.accum_aa_enabled = 1;
    cfg.accum_samples = 2;

    cfg.user_lighting_enabled = 0;
    cfg.show_light_indicators = 0;
    cfg.backdrop_mode = 0;

    /* Grid defaults. */
    for (int i = 0; i < GRID_MAJOR_COUNT; i++)
        cfg.grid_major_steps[i] = 1.0f;
    for (int i = 0; i < GRID_EXTENT_COUNT; i++)
        cfg.grid_extents[i] = 10.0f;

    cfg.scene_x = 0;
    cfg.scene_y = 0;
    cfg.scene_w = 800;
    cfg.scene_h = 600;
    cfg.cam_dist = 30.0f;
    cfg.cam_rx = 0.0f;
    cfg.cam_ry = 0.0f;
    cfg.cam_tx = 0.0f;
    cfg.cam_ty = 0.0f;
    cfg.cam_tz = 0.0f;
    cfg.cam_motion_glow = 0.0f;
    cfg.projection_mix = 1.0f;
    cfg.multisample_enabled = 0;
    cfg.line_smooth_enabled = 0;
    cfg.wireframe = 0;
    cfg.grid_theme = 0;
    cfg.grid_extent_idx = 0;
    cfg.grid_major_idx = 0;
    cfg.axes_theme = 0;
    cfg.alpha_scale = 1.0f;

    return cfg;
}

/* SceneRendererState is the controller-owned per-renderer state that
 * replaced the file-static g_ortho_ref_dist / g_ortho_active /
 * g_active_projection trio. These tests pin the new contracts: init
 * gives a fresh state defaults; two states are independent; and the
 * active_projection cache reflects the canonical zero-jitter math no
 * matter how many AA samples the render walked. */
static void test_scene_renderer_state_init_defaults(void) {
    printf("--- SceneRendererState init defaults ---\n");

    SceneRendererState state;
    /* Fill with garbage first to make sure init actually writes. */
    memset(&state, 0xCC, sizeof state);
    scene_renderer_state_init(&state);

    SceneProjectionDesc p;
    scene_get_active_projection(&state, &p);
    ASSERT_INT("init: ortho off", p.ortho, 0);
    ASSERT_FLOAT("init: fovy_deg = 45", (float)p.fovy_deg, 45.0f);
    ASSERT_TRUE("init: near_z > 0", p.near_z > 0.0);
    ASSERT_TRUE("init: far_z > near_z", p.far_z > p.near_z);

    /* Defensive NULL state still hands back the same defaults. */
    SceneProjectionDesc q;
    scene_get_active_projection(NULL, &q);
    ASSERT_INT("NULL state defaults: ortho", q.ortho, p.ortho);
    ASSERT_FLOAT("NULL state defaults: fovy", (float)q.fovy_deg, (float)p.fovy_deg);
}

static void test_scene_renderer_state_independence(void) {
    printf("--- SceneRendererState independence ---\n");

#ifdef GL_STUBS
    /* Two states, two different mix values. Render through each and
     * verify the cached active_projection reflects each renderer's
     * input — proves there's no shared static under the API. */
    SceneRenderConfig cfg = make_test_config();
    cfg.use_accum = 0;
    cfg.accum_aa_enabled = 0;
    cfg.accum_samples = 1;

    SceneRendererState state_a, state_b;
    scene_renderer_state_init(&state_a);
    scene_renderer_state_init(&state_b);

    cfg.projection_mix = 1.0f;  /* fully perspective */
    ASSERT_INT("render A (perspective)",
               scene_render_3d_scene(&state_a, &cfg), 0);

    cfg.projection_mix = 0.0f;  /* fully ortho */
    ASSERT_INT("render B (ortho)",
               scene_render_3d_scene(&state_b, &cfg), 0);

    SceneProjectionDesc pa, pb;
    scene_get_active_projection(&state_a, &pa);
    scene_get_active_projection(&state_b, &pb);

    ASSERT_INT("state A keeps perspective", pa.ortho, 0);
    ASSERT_INT("state B keeps ortho",       pb.ortho, 1);
#else
    ASSERT_TRUE("state independence requires GL stubs", 1);
#endif
}

static void test_scene_renderer_state_aa_invariant(void) {
    printf("--- active_projection is jitter-invariant ---\n");

#ifdef GL_STUBS
    /* Cache resolves once before the AA jitter loop; per-sample apply
     * is read-only on the state. So accum_samples=1 and =16 should
     * produce identical active_projection contents. */
    SceneRenderConfig cfg = make_test_config();
    cfg.use_accum = 1;
    cfg.accum_aa_enabled = 1;
    cfg.projection_mix = 1.0f;

    SceneRendererState s1, s16;
    scene_renderer_state_init(&s1);
    scene_renderer_state_init(&s16);

    cfg.accum_samples = 1;
    ASSERT_INT("render @ 1 sample", scene_render_3d_scene(&s1, &cfg), 0);

    cfg.accum_samples = 16;
    ASSERT_INT("render @ 16 samples", scene_render_3d_scene(&s16, &cfg), 0);

    SceneProjectionDesc p1, p16;
    scene_get_active_projection(&s1, &p1);
    scene_get_active_projection(&s16, &p16);

    ASSERT_INT("ortho field identical", p1.ortho, p16.ortho);
    ASSERT_FLOAT("near_z identical", (float)p1.near_z, (float)p16.near_z);
    ASSERT_FLOAT("far_z identical",  (float)p1.far_z,  (float)p16.far_z);
    ASSERT_FLOAT("ortho_top identical",
                 (float)p1.ortho_top, (float)p16.ortho_top);
#else
    ASSERT_TRUE("AA invariant requires GL stubs", 1);
#endif
}

static void test_scene_projection_modes(void) {
    printf("--- scene projection modes ---\n");

#ifdef GL_STUBS
    SceneRenderConfig cfg = make_test_config();
    cfg.use_accum = 0;
    cfg.accum_aa_enabled = 0;
    cfg.accum_samples = 1;

    SceneRendererState state;
    scene_renderer_state_init(&state);

    gl_stub_counts_reset();
    cfg.projection_mix = 1.0f;
    ASSERT_INT("perspective render ok",
               scene_render_3d_scene(&state, &cfg), 0);
    ASSERT_TRUE("perspective uses glFrustum",
                gl_stub_counts[GL_STUB_glFrustum] > 0);

    gl_stub_counts_reset();
    cfg.projection_mix = 0.0f;
    ASSERT_INT("ortho render ok",
               scene_render_3d_scene(&state, &cfg), 0);
    ASSERT_TRUE("ortho uses glOrtho",
                gl_stub_counts[GL_STUB_glOrtho] > 0);

    gl_stub_counts_reset();
    cfg.projection_mix = 0.5f;
    ASSERT_INT("mixed projection render ok",
               scene_render_3d_scene(&state, &cfg), 0);
    ASSERT_TRUE("mixed projection loads custom matrix",
                gl_stub_counts[GL_STUB_glLoadMatrixf] > 0);
#else
    ASSERT_TRUE("projection mode GL-call checks require GL stubs", 1);
#endif
}

/* Mouse-wheel zoom drives cam_dist; in 2D (ortho) the projection must
 * rescale with it. With a frozen depth-center reference, the live scale
 * is ref + (cam_dist - cam_dist_at_freeze), so zooming in (smaller
 * cam_dist) shrinks ortho_top and zooming out grows it. The probe finds
 * nothing under GL stubs, so we pre-arm a frozen reference and mark
 * ortho already active; a steady ortho frame in FROZEN mode then holds
 * it (no re-probe), exercising the zoom-delta path. Under PERFRAME the
 * pre-arm is clobbered to the cam_dist fallback, which tracks zoom too —
 * so the directional contract holds either way. */
static void test_scene_ortho_zoom_rescales(void) {
    printf("--- 2D ortho scale tracks cam_dist (zoom) ---\n");

#ifdef GL_STUBS
    SceneRenderConfig cfg = make_test_config();
    cfg.use_accum = 0;
    cfg.accum_aa_enabled = 0;
    cfg.accum_samples = 1;
    cfg.projection_mix = 0.0f; /* ortho */

    SceneRendererState state;
    scene_renderer_state_init(&state);
    /* Pretend we already froze a depth-center of 8.0 at cam_dist 30. */
    state.ortho_ref_dist = 8.0;
    state.ortho_ref_cam_dist = 30.0;
    state.ortho_active = 1;

    SceneProjectionDesc p_base, p_in, p_out;

    cfg.cam_dist = 30.0f; /* no zoom: at the freeze distance */
    ASSERT_INT("ortho render @ freeze dist",
               scene_render_3d_scene(&state, &cfg), 0);
    scene_get_active_projection(&state, &p_base);

    cfg.cam_dist = 27.0f; /* zoom in: closer camera */
    ASSERT_INT("ortho render @ zoom-in dist",
               scene_render_3d_scene(&state, &cfg), 0);
    scene_get_active_projection(&state, &p_in);

    cfg.cam_dist = 33.0f; /* zoom out: farther camera */
    ASSERT_INT("ortho render @ zoom-out dist",
               scene_render_3d_scene(&state, &cfg), 0);
    scene_get_active_projection(&state, &p_out);

    ASSERT_TRUE("zoom in shrinks ortho_top",  p_in.ortho_top  < p_base.ortho_top);
    ASSERT_TRUE("zoom out grows ortho_top",   p_out.ortho_top > p_base.ortho_top);

    /* Exact ratio is htan-independent and pins the arithmetic. */
#if GLR_ORTHO_REF_MODE == GLR_ORTHO_REF_FROZEN
    /* ref = 8 + (cam_dist - 30): 5, 8, 11 -> ratios 5/8 and 11/8. */
    ASSERT_FLOAT("frozen zoom-in ratio",
                 (float)(p_in.ortho_top / p_base.ortho_top), 5.0f / 8.0f);
    ASSERT_FLOAT("frozen zoom-out ratio",
                 (float)(p_out.ortho_top / p_base.ortho_top), 11.0f / 8.0f);
#else
    /* Probe returns nothing under stubs -> ref = cam_dist: 27, 30, 33. */
    ASSERT_FLOAT("perframe zoom-in ratio",
                 (float)(p_in.ortho_top / p_base.ortho_top), 27.0f / 30.0f);
    ASSERT_FLOAT("perframe zoom-out ratio",
                 (float)(p_out.ortho_top / p_base.ortho_top), 33.0f / 30.0f);
#endif
#else
    ASSERT_TRUE("ortho zoom rescale requires GL stubs", 1);
#endif
}

/* Build a test SceneFrameRenderContext. */
static SceneFrameRenderContext make_test_frame_ctx(void) {
    SceneFrameRenderContext ctx = {0};
    ctx.config = make_test_config();
    ctx.config.focus.valid = 0;
    return ctx;
}

/* --- Tests for SceneRenderConfig structure ----------------------- */

static void test_config_defaults(void) {
    printf("--- SceneRenderConfig defaults ---\n");

    SceneRenderConfig cfg = make_test_config();

    ASSERT_INT("execute_fn set", cfg.execute_fn != NULL, 1);
    ASSERT_INT("post_fill_fn unset by default", cfg.post_fill_fn == NULL, 1);
    ASSERT_FLOAT("scene_w default", cfg.scene_w, 800);
    ASSERT_FLOAT("scene_h default", cfg.scene_h, 600);
    ASSERT_INT("use_accum default", cfg.use_accum, 1);
    ASSERT_INT("accum aa default", cfg.accum_aa_enabled, 1);
    ASSERT_INT("accum samples default", cfg.accum_samples, 2);
    ASSERT_INT("user_lighting_enabled default", cfg.user_lighting_enabled, 0);
    ASSERT_FLOAT("anim_time default", cfg.anim_time, 0.0f);
}

/* --- Tests for SceneFrameRenderContext ----------------------------- */

static void test_frame_ctx_defaults(void) {
    printf("--- SceneFrameRenderContext defaults ---\n");

    SceneFrameRenderContext ctx = make_test_frame_ctx();

    ASSERT_INT("config has execute_fn", ctx.config.execute_fn != NULL, 1);
    ASSERT_INT("focus not valid by default", ctx.config.focus.valid, 0);
}

/* --- Tests for scene_grid_render (minimal) ----------------------- */

static void test_scene_grid_render(void) {
    printf("--- scene_grid_render ---\n");

    SceneFrameRenderContext ctx = make_test_frame_ctx();

    /* Just ensure it doesn't crash with null/empty config. */
    scene_grid_render(&ctx);
    ASSERT_TRUE("scene_grid_render did not crash", 1);

    /* With scene viewport sized. */
    ctx.config.scene_w = 1024;
    ctx.config.scene_h = 768;
    scene_grid_render(&ctx);
    ASSERT_TRUE("scene_grid_render with explicit scene rect did not crash", 1);
}

/* --- Tests for scene_axes_render (minimal) ----------------------- */

static void test_scene_axes_render(void) {
    printf("--- scene_axes_render ---\n");

    SceneFrameRenderContext ctx = make_test_frame_ctx();

    scene_axes_render(&ctx);
    ASSERT_TRUE("scene_axes_render did not crash", 1);

    /* With different theme. */
    ctx.config.axes_theme = 1;
    scene_axes_render(&ctx);
    ASSERT_TRUE("scene_axes_render with different theme did not crash", 1);
}

/* --- Tests for scene_backdrop_render (minimal) ------------------- */

static void test_scene_backdrop_render(void) {
    printf("--- scene_backdrop_render ---\n");

    SceneFrameRenderContext ctx = make_test_frame_ctx();

    scene_backdrop_render(&ctx);
    ASSERT_TRUE("scene_backdrop_render did not crash", 1);

    /* With different backdrop mode. */
    ctx.config.backdrop_mode = 1;
    scene_backdrop_render(&ctx);
    ASSERT_TRUE("scene_backdrop_render with different mode did not crash", 1);
}

/* --- Tests for scene_lights_setup/render (minimal) --------------- */

static void test_scene_lights(void) {
    printf("--- scene_lights_setup and scene_lights_render ---\n");

    SceneFrameRenderContext ctx = make_test_frame_ctx();

    /* Setup. */
    scene_lights_setup(&ctx);
    ASSERT_TRUE("scene_lights_setup did not crash", 1);

    /* Render. */
    scene_lights_render(&ctx);
    ASSERT_TRUE("scene_lights_render did not crash", 1);

    /* With lighting enabled. */
    ctx.config.user_lighting_enabled = 1;
    scene_lights_setup(&ctx);
    scene_lights_render(&ctx);
    ASSERT_TRUE("scene_lights with user_lighting_enabled did not crash", 1);
}

/* --- Tests for scene_overlay functions (minimal) ------------------ */

static void test_scene_overlays(void) {
    printf("--- scene_overlays primitives + outlines ---\n");

    SceneFrameRenderContext ctx = make_test_frame_ctx();

    /* All overlay rendering moved out of scene; the scene module now
     * exposes only per-vertex / per-cmd primitives. Outlines + vertex
     * points became controller polygon-mode passes (see imrepl_ctrl.c)
     * and aren't tested here. */
    (void)ctx;
    scene_draw_vertex_label_text(0.0f, 0.0f, 0.0f, " v0", NULL);
    ASSERT_TRUE("scene_draw_vertex_label_text did not crash", 1);
    scene_draw_normal_vector_arrow(0.0f, 0.0f, 0.0f,
                                   0.0f, 1.0f, 0.0f, 0.5f);
    ASSERT_TRUE("scene_draw_normal_vector_arrow did not crash", 1);
}

/* --- Light theme presets (pure data; runs in both builds) ---------- *
 * Every theme must produce a distinct, non-degenerate rig; the names
 * array must be fully populated; the .enabled=0 invariant must hold
 * (the program's glEnable decides which slots light up); STUDIO must
 * keep the tools/scene_demo key-light color it was lifted from; and the
 * eye-space carve-out stays HEADLIGHT-slot-0 only. */
static void test_light_theme_presets(void) {
    printf("--- light theme presets ---\n");

    SceneLight themes[LIGHT_THEME_COUNT][MAX_LIGHTS];
    for (int t = 0; t < LIGHT_THEME_COUNT; t++) {
        scene_lights_apply_theme(themes[t], t);
        ASSERT_TRUE("theme name present + non-empty",
                    scene_light_theme_names[t] != NULL &&
                    scene_light_theme_names[t][0] != '\0');
        const float *d = themes[t][0].diffuse;
        ASSERT_TRUE("theme light0 diffuse is non-zero",
                    d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f);
        for (int i = 0; i < MAX_LIGHTS; i++)
            ASSERT_INT("theme slot ships disabled (program enables)",
                       themes[t][i].enabled, 0);
    }

    /* No two themes share an identical rig. */
    for (int a = 0; a < LIGHT_THEME_COUNT; a++)
        for (int b = a + 1; b < LIGHT_THEME_COUNT; b++)
            ASSERT_TRUE("themes are pairwise distinct",
                        memcmp(themes[a], themes[b], sizeof(themes[a])) != 0);

    /* STUDIO key light is the scene_demo warm-white (1.00, 0.95, 0.85). */
    ASSERT_FLOAT("studio key diffuse r",
                 themes[LIGHT_THEME_STUDIO][0].diffuse[0], 1.00f);
    ASSERT_FLOAT("studio key diffuse g",
                 themes[LIGHT_THEME_STUDIO][0].diffuse[1], 0.95f);
    ASSERT_FLOAT("studio key diffuse b",
                 themes[LIGHT_THEME_STUDIO][0].diffuse[2], 0.85f);

    /* Eye-space carve-out is HEADLIGHT slot 0 only. */
    ASSERT_INT("headlight slot0 is eye-space",
               themes[LIGHT_THEME_HEADLIGHT][0].pos_is_eye_space, 1);
    ASSERT_INT("default slot0 is not eye-space",
               themes[LIGHT_THEME_DEFAULT][0].pos_is_eye_space, 0);
    ASSERT_INT("studio slot0 is not eye-space",
               themes[LIGHT_THEME_STUDIO][0].pos_is_eye_space, 0);
    ASSERT_INT("neon slot0 is not eye-space",
               themes[LIGHT_THEME_NEON][0].pos_is_eye_space, 0);
}

/* --- Tests for animation time propagation ----------------------- */

static void test_anim_time_propagation(void) {
    printf("--- anim_time propagation ---\n");

    SceneFrameRenderContext ctx = make_test_frame_ctx();

    /* Set animation time and ensure it's preserved through renderers. */
    ctx.config.anim_time = 2.5f;

    scene_grid_render(&ctx);
    ASSERT_FLOAT("grid sees anim_time", ctx.config.anim_time, 2.5f);

    scene_backdrop_render(&ctx);
    ASSERT_FLOAT("backdrop sees anim_time", ctx.config.anim_time, 2.5f);

    ASSERT_TRUE("anim_time propagation complete", 1);
}

/* --- Tests for focus vertex in grid context -------------------- */

static void test_focus_vertex_context(void) {
    printf("--- focus vertex in grid context ---\n");

    SceneFrameRenderContext ctx = make_test_frame_ctx();

    /* With valid focus vertex. */
    ctx.config.focus.valid = 1;
    ctx.config.focus.pos[0] = 1.0f;
    ctx.config.focus.pos[1] = 2.0f;
    ctx.config.focus.pos[2] = 3.0f;

    scene_grid_render(&ctx);
    ASSERT_TRUE("grid with valid focus vertex did not crash", 1);
    ASSERT_INT("focus remains valid", ctx.config.focus.valid, 1);
}

/* --- Tests for lighting array access ----------------------------- */

static void test_lighting_array_access(void) {
    printf("--- lighting array in config ---\n");

    SceneFrameRenderContext ctx = make_test_frame_ctx();

    /* Populate a light. */
    ctx.config.lights[0].pos[0] = 1.0f;
    ctx.config.lights[0].pos[1] = 2.0f;
    ctx.config.lights[0].pos[2] = 3.0f;
    ctx.config.lights[0].pos[3] = 1.0f;
    ctx.config.lights[0].ambient[0] = 0.2f;
    ctx.config.lights[0].ambient[1] = 0.2f;
    ctx.config.lights[0].ambient[2] = 0.2f;
    ctx.config.lights[0].ambient[3] = 1.0f;

    ASSERT_FLOAT("light pos[0]", ctx.config.lights[0].pos[0], 1.0f);
    ASSERT_FLOAT("light pos[1]", ctx.config.lights[0].pos[1], 2.0f);
    ASSERT_FLOAT("light ambient[0]", ctx.config.lights[0].ambient[0], 0.2f);

    scene_lights_setup(&ctx);
    ASSERT_TRUE("lights_setup with populated light did not crash", 1);
}

/* --- Tests for grid table arrays --------------------------------- */

static void test_grid_table_arrays(void) {
    printf("--- grid table arrays in config ---\n");

    SceneFrameRenderContext ctx = make_test_frame_ctx();

    /* Populate grid tables. */
    for (int i = 0; i < GRID_MAJOR_COUNT; i++)
        ctx.config.grid_major_steps[i] = (float)(i + 1);
    for (int i = 0; i < GRID_EXTENT_COUNT; i++)
        ctx.config.grid_extents[i] = (float)(i + 10);

    ASSERT_FLOAT("grid_major_steps[0]", ctx.config.grid_major_steps[0], 1.0f);
    ASSERT_FLOAT("grid_major_steps[1]", ctx.config.grid_major_steps[1], 2.0f);
    ASSERT_FLOAT("grid_extents[0]", ctx.config.grid_extents[0], 10.0f);

    scene_grid_render(&ctx);
    ASSERT_TRUE("grid_render with populated tables did not crash", 1);
}

/* --- Tests for viewport dimension changes ----------------------- */

static void test_viewport_dimensions(void) {
    printf("--- viewport dimension handling ---\n");

    SceneFrameRenderContext ctx = make_test_frame_ctx();

    /* Small scene viewport. */
    ctx.config.scene_w = 100;
    ctx.config.scene_h = 100;
    scene_grid_render(&ctx);
    ASSERT_TRUE("small scene viewport did not crash", 1);

    /* Large scene viewport. */
    ctx.config.scene_w = 4096;
    ctx.config.scene_h = 2160;
    scene_grid_render(&ctx);
    ASSERT_TRUE("large scene viewport did not crash", 1);

    /* Extreme aspect ratio. */
    ctx.config.scene_w = 3840;
    ctx.config.scene_h = 480;
    scene_grid_render(&ctx);
    ASSERT_TRUE("extreme aspect ratio did not crash", 1);
}

/* --- Tests for render modes/toggles ------------------------------ */

static void test_render_mode_toggles(void) {
    printf("--- render mode toggles ---\n");

    SceneFrameRenderContext ctx = make_test_frame_ctx();

    /* Wireframe. */
    ctx.config.wireframe = 1;
    scene_grid_render(&ctx);
    ASSERT_INT("wireframe set", ctx.config.wireframe, 1);
    ctx.config.wireframe = 0;

    ASSERT_TRUE("all toggles tested", 1);
}

/* --- Regression tests for glVertex2f parity with glVertex3f ----------- */

static void test_vertex2f_overlay_parity(void) {
    printf("--- vertex2f overlay parity (vertex numbers + outlines) ---\n");

#ifdef GL_STUBS
    /* The vertex-number overlay moved to the controller. The scene-side
     * primitive scene_draw_vertex_label_text calls glRasterPos3f
     * directly — that's enough to pin the parity check here.
     * Outline pass moved to controller (polygon-mode trick); tested via
     * the integration suite, not as a scene-module unit test. */
    gl_stub_counts_reset();
    scene_draw_vertex_label_text(1.0f, 2.0f, 0.0f, " v0", NULL);
    ASSERT_TRUE("vertex2f: vertex number label calls glRasterPos3f",
                gl_stub_counts[GL_STUB_glRasterPos3f] > 0);
#else
    ASSERT_TRUE("vertex2f overlay parity (GL stubs only)", 1);
#endif
}

static void test_vertex2f_guide_cursor_dot(void) {
    printf("--- vertex2f guide: red cursor dot when both args filled ---\n");

#ifdef GL_STUBS
    SceneGuideSnapshot snap = {0};
    snap.show_guides = 1;
    snap.alpha_scale = 1.0f;

    /* The controller now pre-parses cursor args; the scene module reads
     * snapshot->vertex_args / vertex_filled / vertex_n_filled. Each case
     * below populates those alongside the input string. */
    const char *input3f = "glVertex3f(1, 2, 3)";
    snap.input          = input3f;
    snap.input_len      = (int)strlen(input3f);
    snap.vertex_args[0] = 1; snap.vertex_args[1] = 2; snap.vertex_args[2] = 3;
    snap.vertex_filled[0] = snap.vertex_filled[1] = snap.vertex_filled[2] = 1;
    snap.vertex_n_filled = 3;
    gl_stub_counts_reset();
    scene_geometry_guides_render_for_cursor(&snap);
    unsigned long long points3f = gl_stub_counts[GL_STUB_glBegin];
    ASSERT_TRUE("vertex3f(1,2,3): guide draws GL_POINTS", points3f > 0);

    const char *input2f = "glVertex2f(1, 2)";
    snap.input          = input2f;
    snap.input_len      = (int)strlen(input2f);
    snap.vertex_args[0] = 1; snap.vertex_args[1] = 2; snap.vertex_args[2] = 0;
    snap.vertex_filled[0] = snap.vertex_filled[1] = 1;
    snap.vertex_filled[2] = 0;
    snap.vertex_n_filled = 2;
    gl_stub_counts_reset();
    scene_geometry_guides_render_for_cursor(&snap);
    ASSERT_TRUE("vertex2f(1,2): guide draws GL_POINTS (same as vertex3f)",
                gl_stub_counts[GL_STUB_glBegin] > 0);

    /* Partial entry: only one arg — should still produce a guide (plane), not a dot */
    const char *input2f_partial = "glVertex2f(1,";
    snap.input          = input2f_partial;
    snap.input_len      = (int)strlen(input2f_partial);
    snap.vertex_args[0] = 1; snap.vertex_args[1] = 0; snap.vertex_args[2] = 0;
    snap.vertex_filled[0] = 1;
    snap.vertex_filled[1] = snap.vertex_filled[2] = 0;
    snap.vertex_n_filled = 1;
    gl_stub_counts_reset();
    scene_geometry_guides_render_for_cursor(&snap);
    ASSERT_TRUE("vertex2f(1,): partial entry still draws a guide",
                gl_stub_counts[GL_STUB_glBegin] > 0);
#else
    ASSERT_TRUE("vertex2f guide cursor dot (GL stubs only)", 1);
#endif
}

/* --- Grid FOG-fallback predicate + the invariant it encodes ------- */

/* Pure: scene_grid_theme_uses_fog must be true exactly for the
 * EXP2-fog themes (FOG, OCEAN) — the ones that take the FADE fallback
 * under the FOG transition style. FAR is deliberately not a factor.
 * No GL — runs in both builds. */
static void test_grid_theme_uses_fog_predicate(void) {
    printf("--- scene_grid_theme_uses_fog predicate ---\n");
    for (int th = 0; th < GRID_THEME_COUNT; th++) {
        int expect = (th == GRID_THEME_FOG || th == GRID_THEME_OCEAN);
        char lbl[64];
        snprintf(lbl, sizeof lbl, "uses_fog theme=%d", th);
        ASSERT_INT(lbl, scene_grid_theme_uses_fog(th) ? 1 : 0, expect);
    }
}

/* Regression: at a non-FAR extent and *steady* opacity, a grid emits
 * GL fog calls iff scene_grid_theme_uses_fog() says so. At opacity 1
 * there is no transition fog in either style (the synth recede
 * early-returns under FOG, compiled out under FADE), and at a non-FAR
 * extent there is no distance fog, so any glFog* must come from a
 * theme the predicate accounts for. Catches a future theme that starts
 * touching fog without updating the predicate (which would silently
 * break the FOG-style FADE-fallback decision). FAR is excluded because
 * its LINEAR distance fog fires for every theme by design and is
 * independent of the predicate. Default camera => OCEAN takes its
 * above-water (fog) branch. */
static void test_scene_grid_fog_matches_predicate(void) {
    printf("--- grid fog calls match uses_fog predicate (steady) ---\n");
#ifdef GL_STUBS
    for (int e = 0; e < GRID_EXTENT_COUNT; e++) {
        int is_far = (e == GRID_EXTENT_FAR);
        for (int th = GRID_THEME_OFF + 1; th < GRID_THEME_COUNT; th++) {
            SceneFrameRenderContext ctx = make_test_frame_ctx();
            ctx.config.grid_theme = th;
            ctx.config.grid_extent_idx = e;
            ctx.config.grid_opacity = 1.0f;     /* steady: no transition fog */

            gl_stub_counts_reset();
            scene_grid_render(&ctx);
            unsigned long long fog =
                gl_stub_counts[GL_STUB_glFogi] +
                gl_stub_counts[GL_STUB_glFogf] +
                gl_stub_counts[GL_STUB_glFogfv];

            char lbl[80];
            if (is_far) {
                /* FAR is deliberately NOT in the predicate: its LINEAR
                 * distance fog fires for every theme, independent of
                 * scene_grid_theme_uses_fog. Lock that in. */
                snprintf(lbl, sizeof lbl,
                         "theme=%d FAR extent always fogs", th);
                ASSERT_INT(lbl, (fog > 0) ? 1 : 0, 1);
            } else {
                snprintf(lbl, sizeof lbl,
                         "theme=%d extent=%d fog<->predicate", th, e);
                ASSERT_INT(lbl, (fog > 0) ? 1 : 0,
                           scene_grid_theme_uses_fog(th) ? 1 : 0);
            }
        }
    }
#else
    ASSERT_TRUE("grid fog/predicate regression (GL stubs only)", 1);
#endif
}

/* GL_NV_fog_distance radial fog is *scoped*. With the capability flag set,
 * exactly the opted-in passes — the city backdrop and the Ocean/Radar grid
 * themes — emit one extra glFogi (the GL_EYE_RADIAL_NV distance-mode
 * switch) over the flag-off baseline; an eye-plane-tuned theme (Classic)
 * emits none. This locks the scoping so a global
 * glFogi(GL_FOG_DISTANCE_MODE_NV, ...) — which fogged out every tuned
 * theme — can't quietly come back. The stub glFogi only counts calls, so
 * the assertion is the +1 differential, not the enum argument. */
static void test_nv_fog_distance_radial_optin(void) {
    printf("--- GL_NV_fog_distance radial opt-in is scoped ---\n");
#ifdef GL_STUBS
    const int   themes[]  = { GRID_THEME_RADAR, GRID_THEME_CLASSIC };
    /* Each radial-opted theme calls glFogi(GL_FOG_DISTANCE_MODE_NV, ...)
     * twice now: once to set GL_EYE_RADIAL_NV, once to restore the
     * snapshotted prior value at the tail. Both calls only happen
     * when nv_fog_distance_supported is true. */
    const int   delta[]   = { 2, 0 };
    const char *names[]   = { "Radar", "Classic" };
    for (int i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        SceneFrameRenderContext ctx = make_test_frame_ctx();
        ctx.config.grid_theme = themes[i];
        ctx.config.grid_extent_idx = GRID_EXTENT_MID;   /* non-FAR */
        ctx.config.grid_opacity = 1.0f;                 /* steady */

        ctx.config.nv_fog_distance_supported = 0;
        gl_stub_counts_reset();
        scene_grid_render(&ctx);
        unsigned long long off = gl_stub_counts[GL_STUB_glFogi];

        ctx.config.nv_fog_distance_supported = 1;
        gl_stub_counts_reset();
        scene_grid_render(&ctx);
        unsigned long long on = gl_stub_counts[GL_STUB_glFogi];

        char lbl[80];
        snprintf(lbl, sizeof lbl, "%s radial glFogi delta", names[i]);
        ASSERT_INT(lbl, (int)(on - off), delta[i]);
    }

    {
        SceneFrameRenderContext ctx = make_test_frame_ctx();
        ctx.config.backdrop_mode = SCENE_BACKDROP_CITYSCAPE;

        ctx.config.nv_fog_distance_supported = 0;
        gl_stub_counts_reset();
        scene_backdrop_render(&ctx);
        unsigned long long off = gl_stub_counts[GL_STUB_glFogi];

        ctx.config.nv_fog_distance_supported = 1;
        gl_stub_counts_reset();
        scene_backdrop_render(&ctx);
        unsigned long long on = gl_stub_counts[GL_STUB_glFogi];

        /* Same set-and-restore pair as the grid OCEAN/RADAR themes. */
        ASSERT_INT("Cityscape radial glFogi delta", (int)(on - off), 2);
    }
#else
    ASSERT_TRUE("nv fog radial opt-in scoped (GL stubs only)", 1);
#endif
}

static void test_vertex_label_text(void) {
    printf("--- vertex label text rendering ---\n");

#ifdef GL_STUBS
    gl_stub_counts_reset();
    scene_draw_vertex_label_text(1.0f, 2.0f, 3.0f, NULL, NULL);
    ASSERT_INT("NULL primary emits no glRasterPos3f",
               (int)gl_stub_counts[GL_STUB_glRasterPos3f], 0);
    ASSERT_INT("NULL primary emits no glutBitmapCharacter",
               (int)gl_stub_counts[GL_STUB_glutBitmapCharacter], 0);

    gl_stub_counts_reset();
    scene_draw_vertex_label_text(1.0f, 2.0f, 3.0f, " v0", NULL);
    ASSERT_INT("primary text calls glRasterPos3f",
               (int)gl_stub_counts[GL_STUB_glRasterPos3f], 1);
    int idx_chars = (int)gl_stub_counts[GL_STUB_glutBitmapCharacter];
    ASSERT_TRUE("primary text draws ' v0' (3 chars)", idx_chars == 3);

    gl_stub_counts_reset();
    scene_draw_vertex_label_text(1.0f, 2.0f, 3.0f, " v5",
                                 " (1.00, 2.00, 3.00)");
    ASSERT_INT("primary+detail calls glRasterPos3f once",
               (int)gl_stub_counts[GL_STUB_glRasterPos3f], 1);
    ASSERT_TRUE("primary+detail draws more chars than primary",
                (int)gl_stub_counts[GL_STUB_glutBitmapCharacter] > idx_chars);
#else
    ASSERT_TRUE("vertex label text (GL stubs only)", 1);
#endif
}

int main(int argc, char **argv) {
#ifdef GL_STUBS
    /* In stub mode these are no-ops, but keeping the calls preserves coverage
     * for GLUT symbol declarations. */
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA | GLUT_DEPTH);
    glutInitWindowSize(800, 600);
    glutCreateWindow("test_scene_render");
#else
    (void)argc;
    (void)argv;
#endif

    printf("==> test_scene_render\n");
    printf("Scene render modules\n\n");

    test_config_defaults();
    test_scene_renderer_state_init_defaults();
    test_scene_renderer_state_independence();
    test_scene_renderer_state_aa_invariant();
    test_scene_projection_modes();
    test_scene_ortho_zoom_rescales();
    test_frame_ctx_defaults();
    test_grid_theme_uses_fog_predicate();   /* pure; runs in both builds */
    test_light_theme_presets();             /* pure; runs in both builds */

#ifndef GL_STUBS
    printf("--- GL-emitting scene smoke checks skipped in real-GL test build ---\n");
    printf("Run `make test_scene_render USE_GL_STUBS=1` for no-op GL path coverage.\n");

    printf("\ntest_scene_render: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
#endif

    test_scene_grid_render();
    test_scene_axes_render();
    test_scene_backdrop_render();
    test_scene_lights();
    test_scene_overlays();
    test_anim_time_propagation();
    test_focus_vertex_context();
    test_lighting_array_access();
    test_grid_table_arrays();
    test_viewport_dimensions();
    test_render_mode_toggles();
    test_vertex2f_overlay_parity();
    test_vertex2f_guide_cursor_dot();
    test_vertex_label_text();
    test_scene_grid_fog_matches_predicate();
    test_nv_fog_distance_radial_optin();

    printf("\ntest_scene_render: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
