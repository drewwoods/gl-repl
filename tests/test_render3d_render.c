/*
 * test_scene_render.c - Unit tests for scene_* modules (grid, axes, backdrop, lights, overlays).
 * Tests operate independently of repl_state. Stub builds exercise the GL call
 * paths through no-op counters; real-GL builds avoid opening a GUI window.
 */
#include "render3d/grid.h"
#include "render3d/axes.h"
#include "render3d/backdrop.h"
#include "render3d/lights.h"
#include "render3d/overlays.h"
#include "render3d/guides/geometry_guides.h"
#include "render3d/render.h"
#include "render3d/render_types.h"
#include "render3d/postprocess_filter.h"
#include "render3d/postprocess_surface.h"


#include "support/test_harness.h"
#include "support/cpuprof.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
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

#define ASSERT_FLOAT_TOL(label, got, exp, tol) do { \
    TEST_ASSERT_FLOAT(&g_harness, label, got, exp, tol); \
} while (0)

#ifdef GL_STUBS
#define TRACE_PATH "/tmp/test_render3d_render_trace.txt"

typedef struct TraceLog {
    char lines[2048][128];
    int n;
} TraceLog;

static void trace_begin(void) {
    gl_stub_counts_reset();
    gl_stub_trace_open(TRACE_PATH);
}

static void trace_end(TraceLog *log) {
    FILE *f;
    char buf[256];

    gl_stub_trace_close();
    if (!log)
        return;
    log->n = 0;
    f = fopen(TRACE_PATH, "r");
    if (!f)
        return;
    while (log->n < (int)(sizeof(log->lines) / sizeof(log->lines[0])) &&
           fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        if (len && buf[len - 1] == '\n')
            buf[--len] = '\0';
        snprintf(log->lines[log->n], sizeof(log->lines[0]), "%s", buf);
        log->n++;
    }
    fclose(f);
}

static int trace_count_line(const TraceLog *log, const char *exact) {
    int count = 0;
    if (!log || !exact)
        return 0;
    for (int i = 0; i < log->n; i++)
        if (strcmp(log->lines[i], exact) == 0)
            count++;
    return count;
}

static int trace_count_prefix(const TraceLog *log, const char *prefix) {
    int count = 0;
    size_t n;
    if (!log || !prefix)
        return 0;
    n = strlen(prefix);
    for (int i = 0; i < log->n; i++)
        if (strncmp(log->lines[i], prefix, n) == 0)
            count++;
    return count;
}

static int trace_find_after(const TraceLog *log, int start, const char *exact) {
    if (!log || !exact)
        return -1;
    if (start < 0)
        start = 0;
    for (int i = start; i < log->n; i++)
        if (strcmp(log->lines[i], exact) == 0)
            return i;
    return -1;
}
#endif

/* Minimal execute callback for testing. */
static void test_execute_noop(const Render3dExecuteContext *ctx, void *user_data) {
    (void)ctx;
    (void)user_data;
}

typedef struct PurposeCountCtx {
    int counts[8];
} PurposeCountCtx;

static void test_execute_count_purpose(const Render3dExecuteContext *ctx,
                                       void *user_data) {
    PurposeCountCtx *count_ctx = (PurposeCountCtx *)user_data;
    int purpose = ctx ? (int)ctx->purpose : (int)RENDER3D_EXEC_MAIN_FILL;
    if (count_ctx && purpose >= 0 &&
        purpose < (int)(sizeof(count_ctx->counts) / sizeof(count_ctx->counts[0])))
        count_ctx->counts[purpose]++;

    glBegin(GL_TRIANGLES);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glVertex3f(1.0f, 0.0f, 0.0f);
    glVertex3f(0.0f, 1.0f, 0.0f);
    glEnd();
}

typedef struct SubframeCountCtx {
    int calls;
} SubframeCountCtx;

static void test_setup_subframe_count(void *user_data, int pass_idx,
                                      int pass_count,
                                      Render3dRenderConfig *pass_config) {
    SubframeCountCtx *count_ctx = (SubframeCountCtx *)user_data;
    (void)pass_idx;
    (void)pass_count;
    (void)pass_config;
    if (count_ctx)
        count_ctx->calls++;
}

/* Build a test Render3dRenderConfig with sensible defaults. */
static Render3dRenderConfig make_test_config(void) {
    Render3dRenderConfig cfg = {0};
    cfg.execute_fn = test_execute_noop;
    cfg.execute_user_data = NULL;

    cfg.anim_time = 0.0f;

    cfg.use_accum = 1;
    cfg.accum_effect = RENDER3D_ACCUM_EFFECT_AA;
    cfg.accum_passes = 2;

    cfg.show_light_indicators = 0;
    cfg.backdrop_mode = 0;

    /* Grid defaults. */
    for (int i = 0; i < GRID_MAJOR_COUNT; i++)
        cfg.grid_major_steps[i] = 1.0f;
    for (int i = 0; i < GRID_EXTENT_COUNT; i++)
        cfg.grid_extents[i] = 10.0f;

    cfg.render3d_x = 0;
    cfg.render3d_y = 0;
    cfg.render3d_w = 800;
    cfg.render3d_h = 600;
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

/* Render3dState is the controller-owned per-renderer state that
 * replaced the file-static g_ortho_ref_dist / g_ortho_active /
 * g_active_projection trio. These tests pin the new contracts: init
 * gives a fresh state defaults; two states are independent; and the
 * active_projection cache reflects the canonical zero-jitter math no
 * matter how many AA samples the render walked. */
static void test_scene_renderer_state_init_defaults(void) {
    printf("--- Render3dState init defaults ---\n");

    Render3dState state;
    /* Fill with garbage first to make sure init actually writes. */
    memset(&state, 0xCC, sizeof state);
    render3d_state_init(&state);

    Render3dProjectionDesc p;
    render3d_get_active_projection(&state, &p);
    ASSERT_INT("init: ortho off", p.projection, PROJ_PERSPECTIVE);
    ASSERT_FLOAT("init: fovy_deg = 45", (float)p.fovy_deg, 45.0f);
    ASSERT_TRUE("init: near_z > 0", p.near_z > 0.0);
    ASSERT_TRUE("init: far_z > near_z", p.far_z > p.near_z);

    /* Defensive NULL state still hands back the same defaults. */
    Render3dProjectionDesc q;
    render3d_get_active_projection(NULL, &q);
    ASSERT_INT("NULL state defaults: ortho", q.projection, p.projection);
    ASSERT_FLOAT("NULL state defaults: fovy", (float)q.fovy_deg, (float)p.fovy_deg);
}

static void test_scene_renderer_state_independence(void) {
    printf("--- Render3dState independence ---\n");

#ifdef GL_STUBS
    /* Two states, two different mix values. Render through each and
     * verify the cached active_projection reflects each renderer's
     * input - proves there's no shared static under the API. */
    Render3dRenderConfig cfg = make_test_config();
    cfg.use_accum = 0;
    cfg.accum_effect = RENDER3D_ACCUM_EFFECT_OFF;
    cfg.accum_passes = 1;

    Render3dState state_a, state_b;
    render3d_state_init(&state_a);
    render3d_state_init(&state_b);

    cfg.projection_mix = 1.0f;  /* fully perspective */
    ASSERT_INT("render A (perspective)",
               render3d_draw_scene(&state_a, &cfg), 0);

    cfg.projection_mix = 0.0f;  /* fully ortho */
    ASSERT_INT("render B (ortho)",
               render3d_draw_scene(&state_b, &cfg), 0);

    Render3dProjectionDesc pa, pb;
    render3d_get_active_projection(&state_a, &pa);
    render3d_get_active_projection(&state_b, &pb);

    ASSERT_INT("state A keeps perspective", pa.projection, PROJ_PERSPECTIVE);
    ASSERT_INT("state B keeps ortho",       pb.projection, PROJ_ORTHO);
#else
    ASSERT_TRUE("state independence requires GL stubs", 1);
#endif
}

static void test_scene_renderer_state_aa_invariant(void) {
    printf("--- active_projection is jitter-invariant ---\n");

#ifdef GL_STUBS
    /* Cache resolves once before the AA jitter loop; per-sample apply
     * is read-only on the state. So accum_passes=1 and =16 should
     * produce identical active_projection contents. */
    Render3dRenderConfig cfg = make_test_config();
    cfg.use_accum = 1;
    cfg.accum_effect = RENDER3D_ACCUM_EFFECT_AA;
    cfg.projection_mix = 1.0f;

    Render3dState s1, s16;
    render3d_state_init(&s1);
    render3d_state_init(&s16);

    cfg.accum_passes = 1;
    ASSERT_INT("render @ 1 sample", render3d_draw_scene(&s1, &cfg), 0);

    cfg.accum_passes = 16;
    ASSERT_INT("render @ 16 samples", render3d_draw_scene(&s16, &cfg), 0);

    Render3dProjectionDesc p1, p16;
    render3d_get_active_projection(&s1, &p1);
    render3d_get_active_projection(&s16, &p16);

    ASSERT_INT("ortho field identical", p1.projection, p16.projection);
    ASSERT_FLOAT("near_z identical", (float)p1.near_z, (float)p16.near_z);
    ASSERT_FLOAT("far_z identical",  (float)p1.far_z,  (float)p16.far_z);
    ASSERT_FLOAT("ortho_top identical",
                 (float)p1.ortho_top, (float)p16.ortho_top);
#else
    ASSERT_TRUE("AA invariant requires GL stubs", 1);
#endif
}

static void test_scene_accum_effect_profile_section(void) {
    printf("--- accumulation effect profile section ---\n");

#ifdef GL_STUBS
    Render3dRenderConfig cfg = make_test_config();
    Render3dState state;
    render3d_state_init(&state);

    cfg.use_accum = 1;
    cfg.accum_effect = RENDER3D_ACCUM_EFFECT_AA;
    cfg.accum_passes = 4;

    TraceLog trace;
    int first_accum, second_accum, return_accum;
    prof_accum_reset(PROF_RENDER3D_ACCUM_EFFECT);
    trace_begin();
    ASSERT_INT("AA render ok", render3d_draw_scene(&state, &cfg), 0);
    trace_end(&trace);
    prof_accum_commit(PROF_RENDER3D_ACCUM_EFFECT);

    ASSERT_TRUE("AA effect section sampled",
                !prof_section_is_stale(PROF_RENDER3D_ACCUM_EFFECT));
    ASSERT_INT("AA path accumulates each sample plus return",
               (int)gl_stub_counts[GL_STUB_glAccum], 5);
    first_accum = trace_find_after(&trace, 0, "glAccum 257 0.25");
    second_accum = trace_find_after(&trace, first_accum + 1,
                                    "glAccum 256 0.25");
    return_accum = trace_find_after(&trace, second_accum + 1,
                                    "glAccum 258 1");
    ASSERT_TRUE("first sample loads instead of blending stale accum contents",
                first_accum >= 0);
    ASSERT_TRUE("later samples add after the first load", second_accum > first_accum);
    ASSERT_TRUE("return follows the accumulated samples", return_accum > second_accum);
    ASSERT_INT("GL_LOAD removes the standalone accum-buffer clear",
               trace_count_line(&trace, "glClear 512"), 0);

    SubframeCountCtx subframes;
    memset(&subframes, 0, sizeof(subframes));
    cfg.accum_effect = RENDER3D_ACCUM_EFFECT_BLUR;
    cfg.setup_subframe_fn = test_setup_subframe_count;
    cfg.setup_subframe_user_data = &subframes;

    prof_accum_reset(PROF_RENDER3D_ACCUM_EFFECT);
    gl_stub_counts_reset();
    ASSERT_INT("blur render ok", render3d_draw_scene(&state, &cfg), 0);
    prof_accum_commit(PROF_RENDER3D_ACCUM_EFFECT);

    ASSERT_INT("blur profile wraps every subframe setup",
               subframes.calls, cfg.accum_passes);
    ASSERT_TRUE("blur effect section sampled",
                !prof_section_is_stale(PROF_RENDER3D_ACCUM_EFFECT));
#else
    ASSERT_TRUE("accum effect profile section requires GL stubs", 1);
#endif
}

static void test_scene_projection_modes(void) {
    printf("--- scene projection modes ---\n");

#ifdef GL_STUBS
    Render3dRenderConfig cfg = make_test_config();
    cfg.use_accum = 0;
    cfg.accum_effect = RENDER3D_ACCUM_EFFECT_OFF;
    cfg.accum_passes = 1;

    Render3dState state;
    render3d_state_init(&state);

    gl_stub_counts_reset();
    cfg.projection_mix = 1.0f;
    ASSERT_INT("perspective render ok",
               render3d_draw_scene(&state, &cfg), 0);
    ASSERT_TRUE("perspective uses glFrustum",
                gl_stub_counts[GL_STUB_glFrustum] > 0);

    gl_stub_counts_reset();
    cfg.projection_mix = 0.0f;
    ASSERT_INT("ortho render ok",
               render3d_draw_scene(&state, &cfg), 0);
    ASSERT_TRUE("ortho uses glOrtho",
                gl_stub_counts[GL_STUB_glOrtho] > 0);

    gl_stub_counts_reset();
    cfg.projection_mix = 0.5f;
    ASSERT_INT("mixed projection render ok",
               render3d_draw_scene(&state, &cfg), 0);
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
 * pre-arm is clobbered to the cam_dist fallback, which tracks zoom too -
 * so the directional contract holds either way. */
static void test_scene_ortho_zoom_rescales(void) {
    printf("--- 2D ortho scale tracks cam_dist (zoom) ---\n");

#ifdef GL_STUBS
    Render3dRenderConfig cfg = make_test_config();
    cfg.use_accum = 0;
    cfg.accum_effect = RENDER3D_ACCUM_EFFECT_OFF;
    cfg.accum_passes = 1;
    cfg.projection_mix = 0.0f; /* ortho */

    Render3dState state;
    render3d_state_init(&state);
    /* Pretend we already froze a depth-center of 8.0 at cam_dist 30. */
    state.ortho_ref_dist = 8.0;
    state.ortho_ref_cam_dist = 30.0;
    state.ortho_active = 1;

    Render3dProjectionDesc p_base, p_in, p_out;

    cfg.cam_dist = 30.0f; /* no zoom: at the freeze distance */
    ASSERT_INT("ortho render @ freeze dist",
               render3d_draw_scene(&state, &cfg), 0);
    render3d_get_active_projection(&state, &p_base);

    cfg.cam_dist = 27.0f; /* zoom in: closer camera */
    ASSERT_INT("ortho render @ zoom-in dist",
               render3d_draw_scene(&state, &cfg), 0);
    render3d_get_active_projection(&state, &p_in);

    cfg.cam_dist = 33.0f; /* zoom out: farther camera */
    ASSERT_INT("ortho render @ zoom-out dist",
               render3d_draw_scene(&state, &cfg), 0);
    render3d_get_active_projection(&state, &p_out);

    ASSERT_TRUE("zoom in shrinks ortho_top",  p_in.ortho_top  < p_base.ortho_top);
    ASSERT_TRUE("zoom out grows ortho_top",   p_out.ortho_top > p_base.ortho_top);

    /* Exact ratio is htan-independent and pins the arithmetic. */
#if RENDER3D_ORTHO_REF_MODE == RENDER3D_ORTHO_REF_FROZEN
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

/* Build a test Render3dFrameRenderContext. */
static Render3dFrameRenderContext make_test_frame_ctx(void) {
    Render3dRenderConfig cfg = make_test_config();
    cfg.focus.valid = 0;
    Render3dFrameRenderContext ctx;
    render3d_prepare_frame_context(&ctx, &cfg);
    return ctx;
}

/* --- Tests for Render3dRenderConfig structure ----------------------- */

static void test_config_defaults(void) {
    printf("--- Render3dRenderConfig defaults ---\n");

    Render3dRenderConfig cfg = make_test_config();

    ASSERT_INT("execute_fn set", cfg.execute_fn != NULL, 1);
    ASSERT_INT("post_fill_fn unset by default", cfg.post_fill_fn == NULL, 1);
    ASSERT_FLOAT("render3d_w default", cfg.render3d_w, 800);
    ASSERT_FLOAT("render3d_h default", cfg.render3d_h, 600);
    ASSERT_INT("use_accum default", cfg.use_accum, 1);
    ASSERT_INT("accum effect default", cfg.accum_effect, RENDER3D_ACCUM_EFFECT_AA);
    ASSERT_INT("accum passes default", cfg.accum_passes, 2);
    ASSERT_FLOAT("anim_time default", cfg.anim_time, 0.0f);
}

/* --- Tests for Render3dFrameRenderContext ----------------------------- */

static void test_frame_ctx_defaults(void) {
    printf("--- Render3dFrameRenderContext defaults ---\n");

    Render3dFrameRenderContext ctx = make_test_frame_ctx();

    ASSERT_INT("config has execute_fn", ctx.config.execute_fn != NULL, 1);
    ASSERT_INT("focus not valid by default", ctx.config.focus.valid, 0);

    /* Verify derived camera pose from default make_test_config():
     * cam_dist=30, rx=0 deg, ry=0 deg, tx=ty=tz=0. */
    ASSERT_FLOAT_TOL("camera_world_pos X default", ctx.camera_world_pos[0], 0.0f, 0.001f);
    ASSERT_FLOAT_TOL("camera_world_pos Y default", ctx.camera_world_pos[1], 0.0f, 0.001f);
    ASSERT_FLOAT_TOL("camera_world_pos Z default", ctx.camera_world_pos[2], 30.0f, 0.001f);

    /* Verify derived camera pose with angled camera:
     * cam_dist=5, rx=20 deg, ry=30 deg, tx=ty=tz=0. */
    Render3dRenderConfig angled_cfg = make_test_config();
    angled_cfg.cam_dist = 5.0f;
    angled_cfg.cam_rx = 20.0f;
    angled_cfg.cam_ry = 30.0f;
    Render3dFrameRenderContext angled_ctx;
    render3d_prepare_frame_context(&angled_ctx, &angled_cfg);

    ASSERT_FLOAT_TOL("angled camera_world_pos X", angled_ctx.camera_world_pos[0], -2.34923f, 0.001f);
    ASSERT_FLOAT_TOL("angled camera_world_pos Y", angled_ctx.camera_world_pos[1], 1.71010f, 0.001f);
    ASSERT_FLOAT_TOL("angled camera_world_pos Z", angled_ctx.camera_world_pos[2], 4.06900f, 0.001f);

    /* Basis vectors are orthonormal: basis[0] . basis[1] == 0, length == 1. */
    float dot01 = angled_ctx.camera_basis[0][0]*angled_ctx.camera_basis[1][0] +
                  angled_ctx.camera_basis[0][1]*angled_ctx.camera_basis[1][1] +
                  angled_ctx.camera_basis[0][2]*angled_ctx.camera_basis[1][2];
    ASSERT_FLOAT_TOL("basis orthogonal", dot01, 0.0f, 0.0001f);

    float len0 = sqrtf(angled_ctx.camera_basis[0][0]*angled_ctx.camera_basis[0][0] +
                       angled_ctx.camera_basis[0][1]*angled_ctx.camera_basis[0][1] +
                       angled_ctx.camera_basis[0][2]*angled_ctx.camera_basis[0][2]);
    ASSERT_FLOAT_TOL("basis len 0 is 1", len0, 1.0f, 0.0001f);
}

/* --- Tests for render3d_grid_render (minimal) ----------------------- */

static void test_scene_grid_render(void) {
    printf("--- render3d_grid_render ---\n");

    Render3dFrameRenderContext ctx = make_test_frame_ctx();

    /* Just ensure it doesn't crash with null/empty config. */
    render3d_grid_render(&ctx);
    ASSERT_TRUE("render3d_grid_render did not crash", 1);

    /* With scene viewport sized. */
    ctx.config.render3d_w = 1024;
    ctx.config.render3d_h = 768;
    render3d_grid_render(&ctx);
    ASSERT_TRUE("render3d_grid_render with explicit scene rect did not crash", 1);
}

/* --- Tests for render3d_axes_render (minimal) ----------------------- */

static void test_scene_axes_render(void) {
    printf("--- render3d_axes_render ---\n");

    Render3dFrameRenderContext ctx = make_test_frame_ctx();

    render3d_axes_render(&ctx);
    ASSERT_TRUE("render3d_axes_render did not crash", 1);

    /* With different theme. */
    ctx.config.axes_theme = 1;
    render3d_axes_render(&ctx);
    ASSERT_TRUE("render3d_axes_render with different theme did not crash", 1);
}

/* --- Tests for render3d_backdrop_render (minimal) ------------------- */

static void test_scene_backdrop_render(void) {
    printf("--- render3d_backdrop_render ---\n");

    Render3dFrameRenderContext ctx = make_test_frame_ctx();

    /* Test all backdrop modes for setup_lights and rendering to cover backdrop.c */
    for (int mode = 0; mode < RENDER3D_BACKDROP_COUNT; mode++) {
        ctx.config.backdrop_mode = mode;

        /* Set parameters so drawing functions are fully exercised */
        ctx.config.anim_time = 1.5f;
        ctx.config.point_parameter_supported = 1;
        ctx.config.point_parameter_proc = (void (APIENTRY *)(GLenum, const GLfloat *))glPointParameterfv;
        ctx.config.nv_fog_distance_supported = 1;

        render3d_backdrop_setup_lights(&ctx);
        render3d_backdrop_render(&ctx);
    }

    ASSERT_TRUE("render3d_backdrop_render and setup_lights did not crash for all modes", 1);
}

/* --- Drones backdrop: the geometry-bounds hook contract --------------
 *
 * The drone rig is the only backdrop that reads
 * Render3dRenderConfig.geometry_bounds_fn, and the hook is optional in both
 * directions. What is pinned here is that the backdrop PULLS - it asks, and
 * copes with either answer - rather than requiring a caller to have
 * measured anything, because render3d_demo installs no hook at all.
 */
typedef struct DroneBoundsProbe {
    int   calls;
    int   answer;        /* what the hook returns */
    float min[3], max[3];
} DroneBoundsProbe;

static int drone_bounds_probe(void *user_data,
                              float out_min[3], float out_max[3]) {
    DroneBoundsProbe *p = (DroneBoundsProbe *)user_data;
    p->calls++;
    if (!p->answer)
        return 0;
    for (int k = 0; k < 3; k++) {
        out_min[k] = p->min[k];
        out_max[k] = p->max[k];
    }
    return 1;
}

/* Distance of the GL_LIGHT4 position from `center`, read back out of the
 * stub trace. The stub logs glLightfv as "glLightfv <light> <pname>" with
 * the values on the same line. */
static int drone_light4_distance(const TraceLog *log, const float center[3],
                                 float *out_dist) {
    char prefix[64];
    snprintf(prefix, sizeof prefix, "glLightfv %u %u ",
             (unsigned)GL_LIGHT4, (unsigned)GL_POSITION);
    for (int i = 0; i < log->n; i++) {
        if (strncmp(log->lines[i], prefix, strlen(prefix)) == 0) {
            float x, y, z, w;
            if (sscanf(log->lines[i] + strlen(prefix), "%f %f %f %f",
                       &x, &y, &z, &w) == 4) {
                float dx = x - center[0], dy = y - center[1], dz = z - center[2];
                *out_dist = sqrtf(dx * dx + dy * dy + dz * dz);
                return 1;
            }
        }
    }
    return 0;
}

static void test_scene_backdrop_drones(void) {
    printf("--- drones backdrop bounds hook ---\n");

    Render3dFrameRenderContext ctx = make_test_frame_ctx();
    DroneBoundsProbe probe;
    TraceLog log;
    char prefix[64];
    float small_d = 0.0f, large_d = 0.0f;
    float origin[3] = { 0.0f, 0.0f, 0.0f };

    ctx.config.backdrop_mode = RENDER3D_BACKDROP_DRONES;
    ctx.config.anim_time = 3.0f;

    /* No hook at all - render3d_demo's case. The rig must still place its
     * lights, from its own fallback scale. */
    ctx.config.geometry_bounds_fn = NULL;
    ctx.config.geometry_bounds_user_data = NULL;
    trace_begin();
    render3d_backdrop_setup_lights(&ctx);
    render3d_backdrop_render(&ctx);
    trace_end(&log);
    snprintf(prefix, sizeof prefix, "glLightfv %u %u ",
             (unsigned)GL_LIGHT4, (unsigned)GL_POSITION);
    ASSERT_TRUE("drones: no bounds hook still enables GL_LIGHT4",
                trace_count_prefix(&log, prefix) > 0);

    /* A hook that reports nothing measurable is the same case. */
    memset(&probe, 0, sizeof probe);
    probe.answer = 0;
    ctx.config.geometry_bounds_fn = drone_bounds_probe;
    ctx.config.geometry_bounds_user_data = &probe;
    trace_begin();
    render3d_backdrop_setup_lights(&ctx);
    render3d_backdrop_render(&ctx);
    trace_end(&log);
    ASSERT_TRUE("drones: the backdrop asks for bounds", probe.calls > 0);
    ASSERT_TRUE("drones: an unmeasurable scene still enables GL_LIGHT4",
                trace_count_prefix(&log, prefix) > 0);

    /* All four slots above the user's own GL_LIGHT0..3 get spot state, and
     * none of the user's slots are touched. */
    memset(&probe, 0, sizeof probe);
    probe.answer = 1;
    for (int k = 0; k < 3; k++) { probe.min[k] = -1.0f; probe.max[k] = 1.0f; }
    trace_begin();
    render3d_backdrop_setup_lights(&ctx);
    trace_end(&log);
    for (int i = 0; i < 4; i++) {
        char label[64], prefix[64];
        snprintf(prefix, sizeof prefix, "glLightf %u %u ",
                 (unsigned)(GL_LIGHT4 + i), (unsigned)GL_SPOT_CUTOFF);
        snprintf(label, sizeof label, "drones: GL_LIGHT%d gets a spot cutoff", 4 + i);
        ASSERT_TRUE(label, trace_count_prefix(&log, prefix) == 1);
    }
    for (int i = 0; i < 4; i++) {
        char label[64], prefix[64];
        snprintf(prefix, sizeof prefix, "glLightfv %u ", (unsigned)(GL_LIGHT0 + i));
        snprintf(label, sizeof label, "drones: user slot GL_LIGHT%d untouched", i);
        ASSERT_TRUE(label, trace_count_prefix(&log, prefix) == 0);
    }
    ASSERT_TRUE("drones: bounds measured once per setup pass", probe.calls == 1);

    /* The rig scales with the scene: a box ten times larger puts the lights
     * ten times further out. This is the whole reason the hook exists - a
     * fixed rig would sit inside the large scene and miss the small one. */
    if (drone_light4_distance(&log, origin, &small_d)) {
        memset(&probe, 0, sizeof probe);
        probe.answer = 1;
        for (int k = 0; k < 3; k++) { probe.min[k] = -10.0f; probe.max[k] = 10.0f; }
        trace_begin();
        render3d_backdrop_setup_lights(&ctx);
        trace_end(&log);
        ASSERT_TRUE("drones: light distance tracks scene size",
                    drone_light4_distance(&log, origin, &large_d) &&
                    large_d > small_d * 9.0f && large_d < small_d * 11.0f);
    } else {
        ASSERT_TRUE("drones: GL_LIGHT4 position found in trace", 0);
    }
}

/* --- Fairies backdrop: a positional-attenuation rig ------------------
 *
 * The fairies exist to demonstrate point-light attenuation, so the two
 * things worth pinning are exactly the two that make it a demonstration:
 * no slot is ever a spotlight, and the falloff coefficients scale with the
 * measured scene so the same curve applies at any size.
 */
static int trace_first_float3(const TraceLog *log, const char *prefix,
                              float *out) {
    size_t n = strlen(prefix);
    for (int i = 0; i < log->n; i++) {
        if (strncmp(log->lines[i], prefix, n) == 0) {
            if (sscanf(log->lines[i] + n, "%f", out) == 1)
                return 1;
        }
    }
    return 0;
}

static void fairy_run_setup(Render3dFrameRenderContext *ctx,
                            DroneBoundsProbe *probe, float half,
                            TraceLog *log) {
    memset(probe, 0, sizeof *probe);
    probe->answer = 1;
    for (int k = 0; k < 3; k++) {
        probe->min[k] = -half;
        probe->max[k] = half;
    }
    ctx->config.geometry_bounds_fn = drone_bounds_probe;
    ctx->config.geometry_bounds_user_data = probe;
    trace_begin();
    render3d_backdrop_setup_lights(ctx);
    trace_end(log);
}

static void test_scene_backdrop_fairies(void) {
    printf("--- fairies backdrop attenuation rig ---\n");

    Render3dFrameRenderContext ctx = make_test_frame_ctx();
    DroneBoundsProbe probe;
    TraceLog log;
    char prefix[64];
    float small_q = 0.0f, large_q = 0.0f;

    ctx.config.backdrop_mode = RENDER3D_BACKDROP_FAIRIES;
    /* Mid-visit: the resident is up, so its slot is fully configured. */
    ctx.config.anim_time = 5.0f;

    fairy_run_setup(&ctx, &probe, 1.0f, &log);

    /* The design statement. A spotlight would make the falloff ambiguous -
     * you could not tell the cone from the distance term - so no slot gets
     * any GL_SPOT_* state at all. */
    for (int i = 0; i < 8; i++) {
        char label[80];
        snprintf(prefix, sizeof prefix, "glLightf %u %u ",
                 (unsigned)(GL_LIGHT0 + i), (unsigned)GL_SPOT_CUTOFF);
        snprintf(label, sizeof label, "fairies: GL_LIGHT%d gets no spot cutoff", i);
        ASSERT_TRUE(label, trace_count_prefix(&log, prefix) == 0);
        snprintf(prefix, sizeof prefix, "glLightfv %u %u ",
                 (unsigned)(GL_LIGHT0 + i), (unsigned)GL_SPOT_DIRECTION);
        snprintf(label, sizeof label, "fairies: GL_LIGHT%d gets no spot direction", i);
        ASSERT_TRUE(label, trace_count_prefix(&log, prefix) == 0);
    }

    /* The resident is positional - a directional light (w = 0) has no
     * distance to attenuate over, which would make the whole rig inert. */
    snprintf(prefix, sizeof prefix, "glLightfv %u %u ",
             (unsigned)GL_LIGHT4, (unsigned)GL_POSITION);
    ASSERT_TRUE("fairies: the resident has a position",
                trace_count_prefix(&log, prefix) == 1);
    {
        float x, y, z, w;
        int found = 0;
        size_t n = strlen(prefix);
        for (int i = 0; i < log.n && !found; i++)
            if (strncmp(log.lines[i], prefix, n) == 0 &&
                sscanf(log.lines[i] + n, "%f %f %f %f", &x, &y, &z, &w) == 4)
                found = 1;
        ASSERT_TRUE("fairies: the resident light is positional (w == 1)",
                    found && w == 1.0f);
    }

    /* Falloff scales with the scene: ten times the radius is a hundredth
     * of the quadratic coefficient, which is what keeps the visible curve
     * identical on a 1-unit sketch and a 40-unit one. */
    snprintf(prefix, sizeof prefix, "glLightf %u %u ",
             (unsigned)GL_LIGHT4, (unsigned)GL_QUADRATIC_ATTENUATION);
    ASSERT_TRUE("fairies: the resident attenuates quadratically",
                trace_first_float3(&log, prefix, &small_q) && small_q > 0.0f);
    fairy_run_setup(&ctx, &probe, 10.0f, &log);
    ASSERT_TRUE("fairies: quadratic falloff scales with the scene",
                trace_first_float3(&log, prefix, &large_q) &&
                large_q > small_q / 110.0f && large_q < small_q / 90.0f);

    /* And it still works with no hook at all - render3d_demo's case. */
    ctx.config.geometry_bounds_fn = NULL;
    ctx.config.geometry_bounds_user_data = NULL;
    trace_begin();
    render3d_backdrop_setup_lights(&ctx);
    render3d_backdrop_render(&ctx);
    trace_end(&log);
    ASSERT_TRUE("fairies: no bounds hook still lights the resident",
                trace_count_prefix(&log, prefix) == 1);
}

/* --- Fairies: nothing ever flies inside the geometry ------------------
 *
 * The standing invariant for a rig that arranges itself around the user's
 * scene. It is checked here rather than reasoned about because the way it
 * broke was not visible in a still frame: hop-to-hop motion interpolated
 * linearly between two stations on the inspect sphere, and a straight line
 * between two points on a sphere is a chord - a time sweep found the
 * resident inside a unit sphere for a good fraction of every visit while
 * individual frames looked fine.
 *
 * Every light position the backdrop publishes is measured against the
 * bounding sphere of the reported box, which is the tightest bound that
 * holds for any shape the box could contain.
 */
/* Shared by both bounds-driven backdrops: sweep time, read every light
 * position the backdrop publishes out of the stub trace, and return the
 * closest any of them came to the centre of a box of half-extent 1. */
static float backdrop_worst_light_distance(Render3dFrameRenderContext *ctx,
                                           float step_secs, int steps,
                                           int *out_samples) {
    TraceLog log;
    float worst = 1e9f;
    int samples = 0;

    for (int step = 0; step < steps; step++) {
        char prefix[64];
        ctx->config.anim_time = (float)step * step_secs;

        trace_begin();
        render3d_backdrop_setup_lights(ctx);
        trace_end(&log);

        for (int slot = 4; slot < 8; slot++) {
            size_t n;
            snprintf(prefix, sizeof prefix, "glLightfv %u %u ",
                     (unsigned)(GL_LIGHT0 + slot), (unsigned)GL_POSITION);
            n = strlen(prefix);
            for (int i = 0; i < log.n; i++) {
                float x, y, z, w, d;
                if (strncmp(log.lines[i], prefix, n) != 0)
                    continue;
                if (sscanf(log.lines[i] + n, "%f %f %f %f", &x, &y, &z, &w) != 4)
                    continue;
                d = sqrtf(x * x + y * y + z * z);
                samples++;
                if (d < worst)
                    worst = d;
            }
        }
    }
    if (out_samples)
        *out_samples = samples;
    return worst;
}

/* The drones' twin of the fairy clearance sweep. It has its own bug to
 * remember: the phase cross-fade lerped between the outgoing and incoming
 * poses, which are routinely on opposite sides of the scene, so the blend
 * cut a chord straight through - 0.10 of the scene radius at the worst
 * sample, while both endpoint poses were comfortably outside. The strafe
 * formation's inner lane was independently too close. Neither was visible
 * in a still frame. */
static void test_scene_backdrop_drones_clearance(void) {
    printf("--- drones keep clear of the geometry ---\n");

    Render3dFrameRenderContext ctx = make_test_frame_ctx();
    DroneBoundsProbe probe;
    const float corner = 1.7320508f;
    float worst;
    int samples = 0;

    ctx.config.backdrop_mode = RENDER3D_BACKDROP_DRONES;
    memset(&probe, 0, sizeof probe);
    probe.answer = 1;
    for (int k = 0; k < 3; k++) { probe.min[k] = -1.0f; probe.max[k] = 1.0f; }
    ctx.config.geometry_bounds_fn = drone_bounds_probe;
    ctx.config.geometry_bounds_user_data = &probe;

    /* 60 s covers two full three-phase patrol cycles, so every phase and
     * every seam between them is swept. */
    worst = backdrop_worst_light_distance(&ctx, 1.0f / 30.0f, 1800, &samples);

    ASSERT_TRUE("drones: the sweep actually saw lights", samples > 500);
    if (worst < corner)
        printf("    closest approach %.4f (%.3f x corner)\n",
               (double)worst, (double)(worst / corner));
    ASSERT_TRUE("drones: no light ever enters the scene's bounding sphere",
                worst >= corner);
}

static void test_scene_backdrop_fairies_clearance(void) {
    printf("--- fairies keep clear of the geometry ---\n");

    Render3dFrameRenderContext ctx = make_test_frame_ctx();
    DroneBoundsProbe probe;
    /* A box of half-extent 1 on every axis: its bounding sphere - the corner
     * distance, and the radius the rig scales off - is sqrt(3). */
    const float corner = 1.7320508f;
    float worst;
    int samples = 0;

    ctx.config.backdrop_mode = RENDER3D_BACKDROP_FAIRIES;
    memset(&probe, 0, sizeof probe);
    probe.answer = 1;
    for (int k = 0; k < 3; k++) { probe.min[k] = -1.0f; probe.max[k] = 1.0f; }
    ctx.config.geometry_bounds_fn = drone_bounds_probe;
    ctx.config.geometry_bounds_user_data = &probe;

    /* 40 s at frame cadence: three full resident visits, and several runs of
     * each passer (their periods are 7.3 / 11.7 / 17.1 s). */
    worst = backdrop_worst_light_distance(&ctx, 1.0f / 30.0f, 1200, &samples);

    ASSERT_TRUE("fairies: the sweep actually saw lights", samples > 500);
    if (worst < corner)
        printf("    closest approach %.4f (%.3f x corner)\n",
               (double)worst, (double)(worst / corner));
    /* Deliberately the bare invariant rather than the tuned margin: the
     * constants are free to move, flying inside the scene is not. */
    ASSERT_TRUE("fairies: no light ever enters the scene's bounding sphere",
                worst >= corner);
}

static void test_render3d_winding_and_gizmo(void) {
    printf("--- render3d winding view and camera glow ---\n");

#ifdef GL_STUBS
    Render3dRenderConfig cfg = make_test_config();
    cfg.winding_view = 1;
    cfg.cam_motion_glow = 1.0f;
    cfg.cam_dist = 5.0f;
    cfg.cam_tx = 1.0f;
    cfg.cam_ty = 2.0f;
    cfg.cam_tz = 3.0f;

    Render3dState state;
    render3d_state_init(&state);

    gl_stub_counts_reset();
    int res = render3d_draw_scene(&state, &cfg);
    ASSERT_INT("winding view draw ok", res, 0);
    ASSERT_TRUE("winding view and glow calls glBegin/glEnd", gl_stub_counts[GL_STUB_glBegin] > 0);
#endif
}

/* --- Buffer-inspection hook contract ------------------------------- *
 *
 * render3d fires three neutral hooks and knows nothing about what
 * subscribes to them, so the ONLY thing keeping a subscriber correct is
 * where and how often they fire. Nothing else in the tree pins that, and
 * the depth/stencil visualizations depend on all of it: a per-frame
 * reader takes the pass flagged final, a per-pass compositor takes every
 * pass, and the full-rect composite must land after the resolve
 * overlays and before the post filter. */

#ifdef GL_STUBS

#define BUFFER_HOOK_TRACE_MAX 128

typedef enum {
    BUFFER_HOOK_READ = 1,
    BUFFER_HOOK_PASS_OVERLAY,
    BUFFER_HOOK_RESOLVE_OVERLAY,
    BUFFER_HOOK_POST_RESOLVE_OVERLAYS   /* the neighbouring existing hook */
} BufferHookKind;

typedef struct {
    int kind[BUFFER_HOOK_TRACE_MAX];
    int is_final[BUFFER_HOOK_TRACE_MAX];
    int count;
    int rect_ok;      /* every call saw the config's scene rect */
    int proj_ok;      /* the resolve hook received a non-NULL projection */
} BufferHookTrace;

static void buffer_hook_record(BufferHookTrace *tr, int kind, int is_final) {
    if (!tr || tr->count >= BUFFER_HOOK_TRACE_MAX)
        return;
    tr->kind[tr->count] = kind;
    tr->is_final[tr->count] = is_final;
    tr->count++;
}

static void buffer_hook_check_rect(BufferHookTrace *tr,
                                   int sx, int sy, int sw, int sh) {
    if (sx != 0 || sy != 0 || sw != 800 || sh != 600)
        tr->rect_ok = 0;
}

static void buffer_hook_read(void *ud, int is_final_pass,
                             int sx, int sy, int sw, int sh) {
    BufferHookTrace *tr = (BufferHookTrace *)ud;
    buffer_hook_record(tr, BUFFER_HOOK_READ, is_final_pass);
    buffer_hook_check_rect(tr, sx, sy, sw, sh);
}

static void buffer_hook_pass_overlay(void *ud, int is_final_pass,
                                     int sx, int sy, int sw, int sh) {
    BufferHookTrace *tr = (BufferHookTrace *)ud;
    buffer_hook_record(tr, BUFFER_HOOK_PASS_OVERLAY, is_final_pass);
    buffer_hook_check_rect(tr, sx, sy, sw, sh);
}

static void buffer_hook_resolve_overlay(void *ud,
                                        const Render3dProjectionDesc *proj,
                                        int sx, int sy, int sw, int sh) {
    BufferHookTrace *tr = (BufferHookTrace *)ud;
    buffer_hook_record(tr, BUFFER_HOOK_RESOLVE_OVERLAY, 1);
    buffer_hook_check_rect(tr, sx, sy, sw, sh);
    if (!proj)
        tr->proj_ok = 0;
}

static void buffer_hook_post_resolve_overlays(void *ud) {
    buffer_hook_record((BufferHookTrace *)ud,
                       BUFFER_HOOK_POST_RESOLVE_OVERLAYS, 1);
}

static void buffer_hook_install(Render3dRenderConfig *cfg,
                                BufferHookTrace *tr) {
    tr->count = 0;
    tr->rect_ok = 1;
    tr->proj_ok = 1;
    cfg->buffer_read_fn                    = buffer_hook_read;
    cfg->buffer_read_user_data             = tr;
    cfg->buffer_pass_overlay_fn            = buffer_hook_pass_overlay;
    cfg->buffer_pass_overlay_user_data     = tr;
    cfg->buffer_resolve_overlay_fn         = buffer_hook_resolve_overlay;
    cfg->buffer_resolve_overlay_user_data  = tr;
    cfg->post_resolve_overlays_fn          = buffer_hook_post_resolve_overlays;
    cfg->post_resolve_overlays_user_data   = tr;
}

/* Count occurrences of one hook kind, and (for the per-pass hooks) how
 * many of those carried is_final_pass. */
static int buffer_hook_count(const BufferHookTrace *tr, int kind,
                             int *out_final) {
    int n = 0, finals = 0;
    for (int i = 0; i < tr->count; i++) {
        if (tr->kind[i] != kind) continue;
        n++;
        if (tr->is_final[i]) finals++;
    }
    if (out_final) *out_final = finals;
    return n;
}

/* Index of the first call of `kind`, or -1. */
static int buffer_hook_first(const BufferHookTrace *tr, int kind) {
    for (int i = 0; i < tr->count; i++)
        if (tr->kind[i] == kind) return i;
    return -1;
}

/* read -> pass_overlay is a per-pass alternation; assert it holds for
 * every pass rather than only comparing first occurrences. */
static int buffer_hook_read_precedes_each_pass_overlay(
        const BufferHookTrace *tr) {
    int pending_read = 0;
    for (int i = 0; i < tr->count; i++) {
        if (tr->kind[i] == BUFFER_HOOK_READ) {
            if (pending_read) return 0;   /* two reads, no overlay between */
            pending_read = 1;
        } else if (tr->kind[i] == BUFFER_HOOK_PASS_OVERLAY) {
            if (!pending_read) return 0;  /* overlay with no read before it */
            pending_read = 0;
        }
    }
    return pending_read == 0;
}

static void buffer_hooks_check_one(const char *label,
                                   Render3dState *state,
                                   Render3dRenderConfig *cfg,
                                   int expect_passes) {
    BufferHookTrace tr;
    int read_finals = 0, overlay_finals = 0;
    int reads, overlays, resolves;
    char msg[128];

    buffer_hook_install(cfg, &tr);
    snprintf(msg, sizeof msg, "%s: draw ok", label);
    ASSERT_INT(msg, render3d_draw_scene(state, cfg), 0);

    reads    = buffer_hook_count(&tr, BUFFER_HOOK_READ, &read_finals);
    overlays = buffer_hook_count(&tr, BUFFER_HOOK_PASS_OVERLAY,
                                 &overlay_finals);
    resolves = buffer_hook_count(&tr, BUFFER_HOOK_RESOLVE_OVERLAY, NULL);

    snprintf(msg, sizeof msg, "%s: read fires once per pass", label);
    ASSERT_INT(msg, reads, expect_passes);
    snprintf(msg, sizeof msg, "%s: pass overlay fires once per pass", label);
    ASSERT_INT(msg, overlays, expect_passes);
    snprintf(msg, sizeof msg, "%s: exactly one final read", label);
    ASSERT_INT(msg, read_finals, 1);
    snprintf(msg, sizeof msg, "%s: exactly one final pass overlay", label);
    ASSERT_INT(msg, overlay_finals, 1);
    snprintf(msg, sizeof msg, "%s: resolve overlay fires once per frame",
             label);
    ASSERT_INT(msg, resolves, 1);
    snprintf(msg, sizeof msg, "%s: read precedes each pass overlay", label);
    ASSERT_TRUE(msg, buffer_hook_read_precedes_each_pass_overlay(&tr));
    snprintf(msg, sizeof msg, "%s: resolve overlay is last", label);
    ASSERT_INT(msg, tr.kind[tr.count - 1], BUFFER_HOOK_RESOLVE_OVERLAY);
    snprintf(msg, sizeof msg, "%s: resolve follows post_resolve_overlays",
             label);
    ASSERT_TRUE(msg,
                buffer_hook_first(&tr, BUFFER_HOOK_POST_RESOLVE_OVERLAYS) <
                buffer_hook_first(&tr, BUFFER_HOOK_RESOLVE_OVERLAY));
    snprintf(msg, sizeof msg, "%s: every hook saw the scene rect", label);
    ASSERT_TRUE(msg, tr.rect_ok);
    snprintf(msg, sizeof msg, "%s: resolve got a projection", label);
    ASSERT_TRUE(msg, tr.proj_ok);
}
#endif

/* Host chrome renders with GL_STENCIL_TEST suspended. A program that
 * enables the stencil test leaves it enabled for everything drawn after
 * its own geometry, and the grid / axes / backdrop / light gizmos are not
 * the user's geometry - masking them is never what a glStencilFunc meant.
 * The suspension is scoped so the geometry-reporting overlays that follow
 * still see (and reproduce) the program's real mask, which is the half of
 * the contract that would break silently if someone widened the bracket. */
static void test_helpers_suspend_stencil_test(void) {
    printf("--- helper passes suspend the stencil test ---\n");

#ifdef GL_STUBS
    Render3dRenderConfig cfg = make_test_config();
    Render3dState state;
    TraceLog trace;
    int helpers_disable, overlays_pop;
    /* GL_STENCIL_TEST = 0x0B90, GL_ENABLE_BIT = 0x2000. */
    const char *stencil_disable = "glDisable 2960";
    const char *enable_push = "glPushAttrib 8192";

    render3d_state_init(&state);
    cfg.accum_effect = RENDER3D_ACCUM_EFFECT_OFF;
    cfg.grid_theme = 1;             /* give the helper pass something to draw */
    cfg.post_overlays_fn = NULL;

    trace_begin();
    ASSERT_INT("stencil-suspend draw ok", render3d_draw_scene(&state, &cfg), 0);
    trace_end(&trace);

    ASSERT_TRUE("helpers are bracketed by a GL_ENABLE_BIT push",
                trace_count_line(&trace, enable_push) >= 1);
    helpers_disable = trace_find_after(&trace, 0, stencil_disable);
    ASSERT_TRUE("helpers run with the stencil test disabled",
                helpers_disable >= 0);
    /* The disable must land after the push that scopes it, so the pop puts
     * the program's enable state back rather than leaving it off. */
    ASSERT_TRUE("the disable is inside the scoped push",
                trace_find_after(&trace, 0, enable_push) < helpers_disable);
    /* And the bracket must close before the overlay pass: the scene pass's
     * own outermost glPopAttrib is the last one, so a pop has to precede
     * it. GL_ALL_ATTRIB_BITS push = 4294967295. */
    overlays_pop = trace_find_after(&trace, helpers_disable, "glPopAttrib");
    ASSERT_TRUE("the helper bracket closes before the overlays",
                overlays_pop > helpers_disable);
#endif
}

static void test_buffer_hooks(void) {
    printf("--- buffer-inspection hooks ---\n");

#ifdef GL_STUBS
    Render3dRenderConfig cfg = make_test_config();
    Render3dState state;
    render3d_state_init(&state);

    cfg.accum_effect = RENDER3D_ACCUM_EFFECT_OFF;
    buffer_hooks_check_one("single pass", &state, &cfg, 1);

    cfg.accum_effect = RENDER3D_ACCUM_EFFECT_AA;
    cfg.accum_passes = 16;
    buffer_hooks_check_one("16-pass accum", &state, &cfg, 16);

    /* Blur without a setup_subframe_fn degrades to the AA jitter path;
     * with one it takes the per-pass config-copy branch, which forwards
     * is_final_pass separately. Cover both. */
    cfg.accum_effect = RENDER3D_ACCUM_EFFECT_BLUR;
    cfg.accum_passes = 4;
    buffer_hooks_check_one("blur (jitter fallback)", &state, &cfg, 4);
    cfg.setup_subframe_fn = test_setup_subframe_count;
    cfg.setup_subframe_user_data = NULL;
    buffer_hooks_check_one("blur (subframe hook)", &state, &cfg, 4);
    cfg.setup_subframe_fn = NULL;

    /* Unsubscribed hooks are simply not called - a config that leaves
     * them NULL (render3d_demo, every non-REPL caller) still renders. */
    cfg.accum_effect = RENDER3D_ACCUM_EFFECT_OFF;
    cfg.buffer_read_fn = NULL;
    cfg.buffer_pass_overlay_fn = NULL;
    cfg.buffer_resolve_overlay_fn = NULL;
    cfg.post_resolve_overlays_fn = NULL;
    ASSERT_INT("no subscribers still renders",
               render3d_draw_scene(&state, &cfg), 0);
#endif
}

/* --- Tests for render3d_lights_setup/render (minimal) --------------- */

static void test_scene_lights(void) {
    printf("--- render3d_lights_setup and render3d_lights_render ---\n");

    Render3dFrameRenderContext ctx = make_test_frame_ctx();

    /* Setup. */
    render3d_lights_setup(&ctx);
    ASSERT_TRUE("render3d_lights_setup did not crash", 1);

    /* Render. */
    render3d_lights_render(&ctx);
    ASSERT_TRUE("render3d_lights_render did not crash", 1);
}

/* --- Tests for scene_overlay functions (minimal) ------------------ */

static void test_scene_overlays(void) {
    printf("--- scene_overlays primitives + outlines ---\n");

    Render3dFrameRenderContext ctx = make_test_frame_ctx();

    /* All overlay rendering moved out of render3d; the render3d module now
     * exposes only per-vertex / per-cmd primitives. Outlines + vertex
     * points became controller polygon-mode passes (see imrepl_ctrl.c)
     * and aren't tested here. */
    (void)ctx;
    render3d_draw_vertex_label_text(0.0f, 0.0f, 0.0f, " v0", NULL);
    ASSERT_TRUE("render3d_draw_vertex_label_text did not crash", 1);
    render3d_draw_normal_vector_arrow(0.0f, 0.0f, 0.0f,
                                   0.0f, 1.0f, 0.0f, 0.5f);
    ASSERT_TRUE("render3d_draw_normal_vector_arrow did not crash", 1);
}

/* --- Light theme presets (pure data; runs in both builds) ---------- *
 * Every theme must produce a distinct, non-degenerate rig; the names
 * array must be fully populated; the .enabled=0 invariant must hold
 * (the program's glEnable decides which slots light up); STUDIO must
 * keep the tools/render3d_demo key-light color it was lifted from; and the
 * eye-space carve-out stays HEADLIGHT-slot-0 only. */
static void test_light_theme_presets(void) {
    printf("--- light theme presets ---\n");

    Render3dLight themes[LIGHT_THEME_COUNT][MAX_LIGHTS];
    for (int t = 0; t < LIGHT_THEME_COUNT; t++) {
        render3d_lights_apply_theme(themes[t], t);
        ASSERT_TRUE("theme name present + non-empty",
                    render3d_light_theme_names[t] != NULL &&
                    render3d_light_theme_names[t][0] != '\0');
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

    /* STUDIO key light is the render3d_demo warm-white (1.00, 0.95, 0.85). */
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

    Render3dFrameRenderContext ctx = make_test_frame_ctx();

    /* Set animation time and ensure it's preserved through renderers. */
    ctx.config.anim_time = 2.5f;

    render3d_grid_render(&ctx);
    ASSERT_FLOAT("grid sees anim_time", ctx.config.anim_time, 2.5f);

    render3d_backdrop_render(&ctx);
    ASSERT_FLOAT("backdrop sees anim_time", ctx.config.anim_time, 2.5f);

    ASSERT_TRUE("anim_time propagation complete", 1);
}

/* --- Tests for focus vertex in grid context -------------------- */

static void test_focus_vertex_context(void) {
    printf("--- focus vertex in grid context ---\n");

    Render3dFrameRenderContext ctx = make_test_frame_ctx();

    /* With valid focus vertex. */
    ctx.config.focus.valid = 1;
    ctx.config.focus.pos[0] = 1.0f;
    ctx.config.focus.pos[1] = 2.0f;
    ctx.config.focus.pos[2] = 3.0f;

    render3d_grid_render(&ctx);
    ASSERT_TRUE("grid with valid focus vertex did not crash", 1);
    ASSERT_INT("focus remains valid", ctx.config.focus.valid, 1);
}

/* --- Tests for lighting array access ----------------------------- */

static void test_lighting_array_access(void) {
    printf("--- lighting array in config ---\n");

    Render3dFrameRenderContext ctx = make_test_frame_ctx();

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

    render3d_lights_setup(&ctx);
    ASSERT_TRUE("lights_setup with populated light did not crash", 1);
}

/* --- Tests for grid table arrays --------------------------------- */

static void test_grid_table_arrays(void) {
    printf("--- grid table arrays in config ---\n");

    Render3dFrameRenderContext ctx = make_test_frame_ctx();

    /* Populate grid tables. */
    for (int i = 0; i < GRID_MAJOR_COUNT; i++)
        ctx.config.grid_major_steps[i] = (float)(i + 1);
    for (int i = 0; i < GRID_EXTENT_COUNT; i++)
        ctx.config.grid_extents[i] = (float)(i + 10);

    ASSERT_FLOAT("grid_major_steps[0]", ctx.config.grid_major_steps[0], 1.0f);
    ASSERT_FLOAT("grid_major_steps[1]", ctx.config.grid_major_steps[1], 2.0f);
    ASSERT_FLOAT("grid_extents[0]", ctx.config.grid_extents[0], 10.0f);

    render3d_grid_render(&ctx);
    ASSERT_TRUE("grid_render with populated tables did not crash", 1);
}

/* --- Tests for viewport dimension changes ----------------------- */

static void test_viewport_dimensions(void) {
    printf("--- viewport dimension handling ---\n");

    Render3dFrameRenderContext ctx = make_test_frame_ctx();

    /* Small scene viewport. */
    ctx.config.render3d_w = 100;
    ctx.config.render3d_h = 100;
    render3d_grid_render(&ctx);
    ASSERT_TRUE("small scene viewport did not crash", 1);

    /* Large scene viewport. */
    ctx.config.render3d_w = 4096;
    ctx.config.render3d_h = 2160;
    render3d_grid_render(&ctx);
    ASSERT_TRUE("large scene viewport did not crash", 1);

    /* Extreme aspect ratio. */
    ctx.config.render3d_w = 3840;
    ctx.config.render3d_h = 480;
    render3d_grid_render(&ctx);
    ASSERT_TRUE("extreme aspect ratio did not crash", 1);
}

/* --- Tests for render modes/toggles ------------------------------ */

static void test_render_mode_toggles(void) {
    printf("--- render mode toggles ---\n");

    Render3dFrameRenderContext ctx = make_test_frame_ctx();

    /* Wireframe. */
    ctx.config.wireframe = 1;
    render3d_grid_render(&ctx);
    ASSERT_INT("wireframe set", ctx.config.wireframe, 1);
    ctx.config.wireframe = 0;

    ASSERT_TRUE("all toggles tested", 1);
}

static void test_wireframe_hidden_line_passes(void) {
    printf("--- wireframe hidden-line render passes ---\n");

#ifdef GL_STUBS
    Render3dRenderConfig cfg = make_test_config();
    PurposeCountCtx count_ctx;
    memset(&count_ctx, 0, sizeof(count_ctx));
    cfg.execute_fn = test_execute_count_purpose;
    cfg.execute_user_data = &count_ctx;
    cfg.use_accum = 0;
    cfg.accum_effect = RENDER3D_ACCUM_EFFECT_OFF;
    cfg.accum_passes = 1;
    cfg.wireframe = WIREFRAME_HIDDEN;

    Render3dState state;
    render3d_state_init(&state);

    gl_stub_counts_reset();
    ASSERT_INT("wireframe render ok",
               render3d_draw_scene(&state, &cfg), 0);

    ASSERT_INT("wireframe skips main fill purpose",
               count_ctx.counts[RENDER3D_EXEC_MAIN_FILL], 0);
    ASSERT_INT("wireframe hidden-line pass executes once",
               count_ctx.counts[RENDER3D_EXEC_WIREFRAME_HIDDEN_LINES], 1);
    ASSERT_INT("wireframe depth-fill pass executes once",
               count_ctx.counts[RENDER3D_EXEC_WIREFRAME_DEPTH_FILL], 1);
    ASSERT_INT("wireframe visible-line pass executes once",
               count_ctx.counts[RENDER3D_EXEC_WIREFRAME_VISIBLE_LINES], 1);

    ASSERT_TRUE("wireframe flips polygon modes for line/fill/line",
                gl_stub_counts[GL_STUB_glPolygonMode] >= 3);
    ASSERT_TRUE("wireframe owns color masks for hidden/depth/visible",
                gl_stub_counts[GL_STUB_glColorMask] >= 3);
    ASSERT_TRUE("wireframe owns depth func",
                gl_stub_counts[GL_STUB_glDepthFunc] >= 3);
    ASSERT_TRUE("wireframe owns depth mask",
                gl_stub_counts[GL_STUB_glDepthMask] >= 3);
    ASSERT_TRUE("wireframe applies fixed hidden/visible colors",
                gl_stub_counts[GL_STUB_glColor4f] >= 2);

    /* Test with line smoothing enabled. Counts alone cannot prove which
     * capability glEnable received, so use the stub trace for arguments and
     * the depth-fill/hidden/visible pass ordering. */
    cfg.line_smooth_enabled = 1;
    TraceLog trace;
    char blend_enable[64];
    char blend_func[96];
    char color_mask_depth[64];
    char depth_lequal[64];
    char depth_greater[64];
    char polygon_fill[64];
    char polygon_line[64];
    snprintf(blend_enable, sizeof(blend_enable), "glEnable %u",
             (unsigned)GL_BLEND);
    snprintf(blend_func, sizeof(blend_func), "glBlendFunc %u %u",
             (unsigned)GL_SRC_ALPHA, (unsigned)GL_ONE_MINUS_SRC_ALPHA);
    snprintf(color_mask_depth, sizeof(color_mask_depth),
             "glColorMask %u %u %u %u",
             (unsigned)GL_FALSE, (unsigned)GL_FALSE,
             (unsigned)GL_FALSE, (unsigned)GL_FALSE);
    snprintf(depth_lequal, sizeof(depth_lequal), "glDepthFunc %u",
             (unsigned)GL_LEQUAL);
    snprintf(depth_greater, sizeof(depth_greater), "glDepthFunc %u",
             (unsigned)GL_GREATER);
    snprintf(polygon_fill, sizeof(polygon_fill), "glPolygonMode %u %u",
             (unsigned)GL_FRONT_AND_BACK, (unsigned)GL_FILL);
    snprintf(polygon_line, sizeof(polygon_line), "glPolygonMode %u %u",
             (unsigned)GL_FRONT_AND_BACK, (unsigned)GL_LINE);

    trace_begin();
    ASSERT_INT("wireframe render with line smooth ok",
               render3d_draw_scene(&state, &cfg), 0);
    trace_end(&trace);
    ASSERT_TRUE("wireframe with line smooth enables GL_BLEND",
                trace_count_line(&trace, blend_enable) >= 2);
    ASSERT_TRUE("wireframe with line smooth sets blend func",
                trace_count_line(&trace, blend_func) >= 2);

    int depth_mask_idx = trace_find_after(&trace, 0, color_mask_depth);
    int fill_mode_idx = trace_find_after(&trace, depth_mask_idx + 1,
                                         polygon_fill);
    int hidden_depth_idx = trace_find_after(&trace, fill_mode_idx + 1,
                                            depth_greater);
    int hidden_line_idx = trace_find_after(&trace, hidden_depth_idx + 1,
                                           polygon_line);
    int visible_depth_idx = trace_find_after(&trace, hidden_line_idx + 1,
                                             depth_lequal);
    int visible_line_idx = trace_find_after(&trace, visible_depth_idx + 1,
                                            polygon_line);
    ASSERT_TRUE("wireframe fills depth before drawing hidden lines",
                depth_mask_idx >= 0 && fill_mode_idx > depth_mask_idx &&
                hidden_depth_idx > fill_mode_idx &&
                hidden_line_idx > hidden_depth_idx);
    ASSERT_TRUE("wireframe draws visible lines after hidden lines",
                visible_depth_idx > hidden_line_idx &&
                visible_line_idx > visible_depth_idx);
#else
    ASSERT_TRUE("wireframe hidden-line passes require GL stubs", 1);
#endif
}

static void test_plain_wireframe_uses_main_fill(void) {
    printf("--- plain wireframe render pass ---\n");

#ifdef GL_STUBS
    Render3dRenderConfig cfg = make_test_config();
    PurposeCountCtx count_ctx;
    memset(&count_ctx, 0, sizeof(count_ctx));
    cfg.execute_fn = test_execute_count_purpose;
    cfg.execute_user_data = &count_ctx;
    cfg.use_accum = 0;
    cfg.accum_effect = RENDER3D_ACCUM_EFFECT_OFF;
    cfg.accum_passes = 1;
    cfg.wireframe = WIREFRAME_PLAIN;

    Render3dState state;
    render3d_state_init(&state);

    gl_stub_counts_reset();
    ASSERT_INT("plain wireframe render ok",
               render3d_draw_scene(&state, &cfg), 0);

    ASSERT_INT("plain wireframe executes main fill once",
               count_ctx.counts[RENDER3D_EXEC_MAIN_FILL], 1);
    ASSERT_INT("plain wireframe does not run hidden pass",
               count_ctx.counts[RENDER3D_EXEC_WIREFRAME_HIDDEN_LINES], 0);
    ASSERT_INT("plain wireframe does not run depth-fill pass",
               count_ctx.counts[RENDER3D_EXEC_WIREFRAME_DEPTH_FILL], 0);
    ASSERT_INT("plain wireframe does not run visible-line pass",
               count_ctx.counts[RENDER3D_EXEC_WIREFRAME_VISIBLE_LINES], 0);
    ASSERT_TRUE("plain wireframe sets line then fill polygon mode",
                gl_stub_counts[GL_STUB_glPolygonMode] >= 2);
#else
    ASSERT_TRUE("plain wireframe pass requires GL stubs", 1);
#endif
}

/* --- Regression tests for glVertex2f parity with glVertex3f ----------- */

static void test_vertex2f_overlay_parity(void) {
    printf("--- vertex2f overlay parity (vertex numbers + outlines) ---\n");

#ifdef GL_STUBS
    /* The vertex-number overlay moved to the controller. The scene-side
     * primitive render3d_draw_vertex_label_text calls glRasterPos3f
     * directly - that's enough to pin the parity check here.
     * Outline pass moved to controller (polygon-mode trick); tested via
     * the integration suite, not as a scene-module unit test. */
    gl_stub_counts_reset();
    render3d_draw_vertex_label_text(1.0f, 2.0f, 0.0f, " v0", NULL);
    ASSERT_TRUE("vertex2f: vertex number label calls glRasterPos3f",
                gl_stub_counts[GL_STUB_glRasterPos3f] > 0);
#else
    ASSERT_TRUE("vertex2f overlay parity (GL stubs only)", 1);
#endif
}

static void test_vertex2f_guide_cursor_dot(void) {
    printf("--- vertex2f guide: red cursor dot when both args filled ---\n");

#ifdef GL_STUBS
    Render3dGuideSnapshot snap = {0};
    snap.show_guides = 1;
    snap.alpha_scale = 1.0f;

    /* The caller now pre-parses cursor args; the render3d module reads
     * snapshot->vertex_args / vertex_filled / vertex_n_filled. Each case
     * below populates those alongside the input string. */
    const char *input3f = "glVertex3f(1, 2, 3)";
    snap.input          = input3f;
    snap.input_len      = (int)strlen(input3f);
    snap.vertex_args[0] = 1; snap.vertex_args[1] = 2; snap.vertex_args[2] = 3;
    snap.vertex_filled[0] = snap.vertex_filled[1] = snap.vertex_filled[2] = 1;
    snap.vertex_n_filled = 3;
    gl_stub_counts_reset();
    render3d_geometry_guides_render_for_cursor(&snap);
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
    render3d_geometry_guides_render_for_cursor(&snap);
    ASSERT_TRUE("vertex2f(1,2): guide draws GL_POINTS (same as vertex3f)",
                gl_stub_counts[GL_STUB_glBegin] > 0);

    /* Partial entry: only one 2D arg. z is implicit, so only y remains free:
     * this is a line guide, not the 3D one-arg plane guide. */
    const char *input2f_partial = "glVertex2f(1,";
    snap.input          = input2f_partial;
    snap.input_len      = (int)strlen(input2f_partial);
    snap.vertex_args[0] = 1; snap.vertex_args[1] = 0; snap.vertex_args[2] = 0;
    snap.vertex_filled[0] = 1;
    snap.vertex_filled[1] = snap.vertex_filled[2] = 0;
    snap.vertex_n_filled = 1;
    TraceLog trace;
    char begin_lines[64];
    char begin_fan[64];
    /* The 1-DOF locus renders as an end-faded GL_LINE_STRIP; the 2-DOF
     * sheet's fill is a GL_TRIANGLE_FAN. Presence of the strip and
     * absence of the fan distinguishes line-guide from plane-guide. */
    snprintf(begin_lines, sizeof(begin_lines), "glBegin %u",
             (unsigned)GL_LINE_STRIP);
    snprintf(begin_fan, sizeof(begin_fan), "glBegin %u",
             (unsigned)GL_TRIANGLE_FAN);
    trace_begin();
    render3d_geometry_guides_render_for_cursor(&snap);
    trace_end(&trace);
    ASSERT_TRUE("vertex2f(1,): partial entry draws a line guide",
                trace_count_line(&trace, begin_lines) > 0);
    ASSERT_INT("vertex2f(1,): partial entry does not draw a plane",
               trace_count_line(&trace, begin_fan), 0);
#else
    ASSERT_TRUE("vertex2f guide cursor dot (GL stubs only)", 1);
#endif
}

/* --- Grid fog-owner predicate + the invariant it encodes ---------- */

/* Pure: render3d_grid_theme_uses_fog must be true exactly for the themes
 * whose own atmosphere collides with the synthesized clear-color recede.
 * Today that is just OCEAN's above-water EXP2 fog. FROZEN's mist is
 * under-ice-only, so it stays out. FAR is deliberately not a factor.
 * No GL - runs in both builds. */
static void test_grid_theme_uses_fog_predicate(void) {
    printf("--- render3d_grid_theme_uses_fog predicate ---\n");
    for (int th = 0; th < GRID_THEME_COUNT; th++) {
        int expect = (th == GRID_THEME_OCEAN);
        char lbl[64];
        snprintf(lbl, sizeof lbl, "uses_fog theme=%d", th);
        ASSERT_INT(lbl, render3d_grid_theme_uses_fog(th) ? 1 : 0, expect);
    }
}

/* Regression: at a non-FAR extent and *steady* opacity, a grid emits
 * GL fog calls iff render3d_grid_theme_uses_fog() says so. At opacity 1
 * there is no transition fog in either style (the synth recede
 * early-returns under FOG, compiled out under FADE), and at a non-FAR
 * extent there is no distance fog, so any glFog* must come from a
 * theme the predicate accounts for. Catches a future theme that starts
 * touching fog without updating the predicate (which would silently
 * break the FOG-style FADE-fallback decision). FAR is excluded because
 * its LINEAR distance fog fires for every theme by design and is
 * independent of the predicate. Default camera => OCEAN takes its
 * above-water (fog) branch and FROZEN its fog-less above-ice one. */
static void test_scene_grid_fog_matches_predicate(void) {
    printf("--- grid fog calls match uses_fog predicate (steady) ---\n");
#ifdef GL_STUBS
    for (int e = 0; e < GRID_EXTENT_COUNT; e++) {
        int is_far = (e == GRID_EXTENT_FAR);
        for (int th = GRID_THEME_OFF + 1; th < GRID_THEME_COUNT; th++) {
            Render3dFrameRenderContext ctx = make_test_frame_ctx();
            ctx.config.grid_theme = th;
            ctx.config.grid_extent_idx = e;
            ctx.config.grid_opacity = 1.0f;     /* steady: no transition fog */

            gl_stub_counts_reset();
            render3d_grid_render(&ctx);
            unsigned long long fog =
                gl_stub_counts[GL_STUB_glFogi] +
                gl_stub_counts[GL_STUB_glFogf] +
                gl_stub_counts[GL_STUB_glFogfv];

            char lbl[80];
            if (render3d_grid_theme_uses_edge_fade(th)) {
                /* Edge-fade line themes dissolve to the backdrop via
                 * per-vertex alpha, so they emit no fog at ANY extent
                 * (FAR included) or transition phase. */
                snprintf(lbl, sizeof lbl,
                         "theme=%d edge-fade: never fogs", th);
                ASSERT_INT(lbl, (fog > 0) ? 1 : 0, 0);
            } else if (is_far) {
                /* FAR is deliberately NOT in the uses_fog predicate: its
                 * LINEAR distance fog fires for every non-edge-fade
                 * theme, independent of render3d_grid_theme_uses_fog. */
                snprintf(lbl, sizeof lbl,
                         "theme=%d FAR extent always fogs", th);
                ASSERT_INT(lbl, (fog > 0) ? 1 : 0, 1);
            } else {
                snprintf(lbl, sizeof lbl,
                         "theme=%d extent=%d fog<->predicate", th, e);
                ASSERT_INT(lbl, (fog > 0) ? 1 : 0,
                           render3d_grid_theme_uses_fog(th) ? 1 : 0);
            }
        }
    }
#else
    ASSERT_TRUE("grid fog/predicate regression (GL stubs only)", 1);
#endif
}

/* GL_NV_fog_distance radial fog is *scoped*. With the capability flag set,
 * exactly the opted-in passes - the city backdrop and the Ocean/Radar grid
 * themes - emit one extra glFogi (the GL_EYE_RADIAL_NV distance-mode
 * switch) over the flag-off baseline; an eye-plane-tuned theme (Classic)
 * emits none. This locks the scoping so a global
 * glFogi(GL_FOG_DISTANCE_MODE_NV, ...) - which fogged out every tuned
 * theme - can't quietly come back. The stub glFogi only counts calls, so
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
        Render3dFrameRenderContext ctx = make_test_frame_ctx();
        ctx.config.grid_theme = themes[i];
        ctx.config.grid_extent_idx = GRID_EXTENT_MID;   /* non-FAR */
        ctx.config.grid_opacity = 1.0f;                 /* steady */

        ctx.config.nv_fog_distance_supported = 0;
        gl_stub_counts_reset();
        render3d_grid_render(&ctx);
        unsigned long long off = gl_stub_counts[GL_STUB_glFogi];

        ctx.config.nv_fog_distance_supported = 1;
        gl_stub_counts_reset();
        render3d_grid_render(&ctx);
        unsigned long long on = gl_stub_counts[GL_STUB_glFogi];

        char lbl[80];
        snprintf(lbl, sizeof lbl, "%s radial glFogi delta", names[i]);
        ASSERT_INT(lbl, (int)(on - off), delta[i]);
    }

    {
        Render3dFrameRenderContext ctx = make_test_frame_ctx();
        /* City+Stars is the only backdrop that draws the city now. Its
         * starfield pass touches fog with glDisable, never glFogi, so the
         * set-and-restore pair below still counts the city's alone. */
        ctx.config.backdrop_mode = RENDER3D_BACKDROP_CITY_AND_STARS;

        ctx.config.nv_fog_distance_supported = 0;
        gl_stub_counts_reset();
        render3d_backdrop_render(&ctx);
        unsigned long long off = gl_stub_counts[GL_STUB_glFogi];

        ctx.config.nv_fog_distance_supported = 1;
        gl_stub_counts_reset();
        render3d_backdrop_render(&ctx);
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
    render3d_draw_vertex_label_text(1.0f, 2.0f, 3.0f, NULL, NULL);
    ASSERT_INT("NULL primary emits no glRasterPos3f",
               (int)gl_stub_counts[GL_STUB_glRasterPos3f], 0);
    ASSERT_INT("NULL primary emits no glutBitmapCharacter",
               (int)gl_stub_counts[GL_STUB_glutBitmapCharacter], 0);

    gl_stub_counts_reset();
    render3d_draw_vertex_label_text(1.0f, 2.0f, 3.0f, " v0", NULL);
    ASSERT_INT("primary text calls glRasterPos3f",
               (int)gl_stub_counts[GL_STUB_glRasterPos3f], 1);
    int idx_chars = (int)gl_stub_counts[GL_STUB_glutBitmapCharacter];
    ASSERT_TRUE("primary text draws ' v0' (3 chars)", idx_chars == 3);

    gl_stub_counts_reset();
    render3d_draw_vertex_label_text(1.0f, 2.0f, 3.0f, " v5",
                                 " (1.00, 2.00, 3.00)");
    ASSERT_INT("primary+detail calls glRasterPos3f once",
               (int)gl_stub_counts[GL_STUB_glRasterPos3f], 1);
    ASSERT_TRUE("primary+detail draws more chars than primary",
                (int)gl_stub_counts[GL_STUB_glutBitmapCharacter] > idx_chars);
#else
    ASSERT_TRUE("vertex label text (GL stubs only)", 1);
#endif
}

#ifdef GL_STUBS
static void test_scene_axes_themes_and_opacity(void) {
    printf("--- axes themes, opacities, and transition fog ---\n");
    Render3dFrameRenderContext ctx = make_test_frame_ctx();

    /* Test all theme permutations */
    const int themes[] = {
        AXES_THEME_CLASSIC,
        AXES_THEME_PULSE,
        AXES_THEME_NEON,
        AXES_THEME_COMPASS,
        AXES_THEME_GIZMO,
        AXES_THEME_RULER
    };
    for (size_t i = 0; i < sizeof(themes)/sizeof(themes[0]); i++) {
        ctx.config.axes_theme = themes[i];

        /* Render fully opaque */
        ctx.config.axes_opacity = 1.0f;
        gl_stub_counts_reset();
        render3d_axes_render(&ctx);
        ASSERT_TRUE("axes render for theme did not crash", 1);
        ASSERT_TRUE("axes render calls glColor", gl_stub_counts[GL_STUB_glColor4f] > 0);

        /* Render translucent (exercises transition alpha and fog paths) */
        ctx.config.axes_opacity = 0.5f;
        gl_stub_counts_reset();
        render3d_axes_render(&ctx);
        ASSERT_TRUE("translucent axes render did not crash", 1);
    }
}

static void test_scene_lights_indicators(void) {
    printf("--- lights indicators (on/off/directional/positional/eye-space) ---\n");
    Render3dFrameRenderContext ctx = make_test_frame_ctx();
    ctx.config.show_light_indicators = 1;

    /* 1. Light 0 positional, enabled */
    ctx.config.lights[0].enabled = 1;
    ctx.config.lights[0].pos_is_eye_space = 0;
    ctx.config.lights[0].pos[3] = 1.0f; /* positional */
    ctx.config.lights[0].diffuse[0] = 1.0f;
    ctx.config.lights[0].diffuse[1] = 0.0f;
    ctx.config.lights[0].diffuse[2] = 0.0f;

    /* 2. Light 1 directional, enabled */
    ctx.config.lights[1].enabled = 1;
    ctx.config.lights[1].pos_is_eye_space = 0;
    ctx.config.lights[1].pos[3] = 0.0f; /* directional */
    ctx.config.lights[1].pos[0] = 0.0f;
    ctx.config.lights[1].pos[1] = 1.0f;
    ctx.config.lights[1].pos[2] = 0.0f;

    /* 3. Light 2 eye-space, enabled */
    ctx.config.lights[2].enabled = 1;
    ctx.config.lights[2].pos_is_eye_space = 1;

    /* 4. Light 3 disabled (off-state indicators) */
    ctx.config.lights[3].enabled = 0;

    gl_stub_counts_reset();
    render3d_lights_render(&ctx);
    ASSERT_TRUE("lights indicators render did not crash", 1);
    ASSERT_TRUE("renders indicator points", gl_stub_counts[GL_STUB_glBegin] > 0);
}

/* The config carries the background twice because the two uses are different
 * questions: baseline_clear_color is the GL state a program clear starts from,
 * presentation_rgba is what the scene is understood to sit on. A caller that
 * observes its program's real clears feeds the observation into the second
 * only - so the two legitimately differ, and each reader must take its own.
 * Feeding presentation history into the baseline would let a previous frame
 * decide the pixels this frame's own clear writes. */
static void test_background_colors_are_independent(void) {
    printf("--- baseline vs presentation background ---\n");

    Render3dState state;
    int theme;
    int fog_color_calls = 0;
    int clear_color_calls = 0;

    /* An edge-fade theme dissolves via per-vertex alpha and never sets a fog
     * color, so ask the predicate which themes actually exercise the fog path
     * rather than naming one that may join the edge-fade set later. FAR is the
     * extent whose distance fog fires for every non-edge-fade theme. */
    for (theme = GRID_THEME_OFF + 1; theme < GRID_THEME_COUNT; theme++) {
        Render3dRenderConfig cfg = make_test_config();
        TraceLog log;

        if (render3d_grid_theme_uses_edge_fade(theme))
            continue;

        memset(&state, 0, sizeof(state));
        cfg.use_accum = 0;
        cfg.accum_effect = RENDER3D_ACCUM_EFFECT_OFF;
        cfg.accum_passes = 1;
        cfg.grid_theme = theme;
        cfg.grid_extent_idx = GRID_EXTENT_FAR;
        cfg.grid_opacity = 1.0f;
        cfg.axes_theme = 0;
        cfg.baseline_clear_color[0] = 0.25f; cfg.baseline_clear_color[1] = 0.0f;
        cfg.baseline_clear_color[2] = 0.0f;  cfg.baseline_clear_color[3] = 1.0f;
        cfg.presentation_rgba[0] = 0.0f; cfg.presentation_rgba[1] = 0.0f;
        cfg.presentation_rgba[2] = 0.75f; cfg.presentation_rgba[3] = 1.0f;

        trace_begin();
        ASSERT_INT("draw_scene accepts the split config",
                   render3d_draw_scene(&state, &cfg), 0);
        trace_end(&log);

        clear_color_calls += trace_count_line(&log, "glClearColor 0.25 0 0 1");
        fog_color_calls   += trace_count_line(&log, "glFogfv 2918 0 0 0.75 1");

        {
            char lbl[80];
            snprintf(lbl, sizeof lbl,
                     "theme=%d: presentation is never the clear color", theme);
            ASSERT_INT(lbl, trace_count_line(&log, "glClearColor 0 0 0.75 1"), 0);
            snprintf(lbl, sizeof lbl,
                     "theme=%d: baseline is never the fog color", theme);
            ASSERT_INT(lbl, trace_count_line(&log, "glFogfv 2918 0.25 0 0 1"), 0);
        }
    }

    ASSERT_INT("baseline establishes the GL clear color",
               clear_color_calls > 0, 1);
    ASSERT_INT("presentation drives the recede fog color",
               fog_color_calls > 0, 1);
}

/* execute_fn == NULL stays supported: no program walk means no program-owned
 * clear, and render3d must not invent one for the scene rect - it draws its
 * helpers against the supplied presentation color over whatever the buffer
 * already holds. */
static void test_null_execute_fn_draws_helpers_without_clearing(void) {
    printf("--- execute_fn == NULL: helpers, no scene clear ---\n");

    Render3dState state;
    Render3dRenderConfig cfg = make_test_config();

    memset(&state, 0, sizeof(state));
    cfg.execute_fn = NULL;
    cfg.use_accum = 0;
    cfg.accum_effect = RENDER3D_ACCUM_EFFECT_OFF;
    cfg.accum_passes = 1;
    cfg.grid_theme = GRID_THEME_CLASSIC;
    cfg.grid_extent_idx = GRID_EXTENT_FAR;
    cfg.grid_opacity = 1.0f;

    gl_stub_counts_reset();
    ASSERT_INT("draw_scene accepts a NULL execute_fn",
               render3d_draw_scene(&state, &cfg), 0);
    ASSERT_INT("no glClear is issued on the program's behalf",
               (int)gl_stub_counts[GL_STUB_glClear], 0);
    ASSERT_TRUE("the scene helpers still draw",
                gl_stub_counts[GL_STUB_glBegin] > 0);
}

static void test_postprocess_filters(void) {
    printf("--- postprocess filters (vignette & chromatic aberration & scanlines) ---\n");

    /* RENDER3D_POST_FILTER_OFF does nothing */
    render3d_postprocess_filter_render(RENDER3D_POST_FILTER_OFF, 0, 0, 800, 600);

    /* RENDER3D_POST_FILTER_CHROMATIC_ABERRATION */
    gl_stub_counts_reset();
    render3d_postprocess_filter_render(RENDER3D_POST_FILTER_CHROMATIC_ABERRATION, 0, 0, 800, 600);
    ASSERT_TRUE("chromatic aberration calls glCopyTexSubImage2D",
                gl_stub_counts[GL_STUB_glCopyTexSubImage2D] > 0);
    ASSERT_TRUE("chromatic aberration calls glColorMask",
                gl_stub_counts[GL_STUB_glColorMask] > 0);

    /* RENDER3D_POST_FILTER_VIGNETTE */
    gl_stub_counts_reset();
    render3d_postprocess_filter_render(RENDER3D_POST_FILTER_VIGNETTE, 0, 0, 800, 600);
    ASSERT_TRUE("vignette draws triangle strips",
                gl_stub_counts[GL_STUB_glBegin] > 0);

    /* RENDER3D_POST_FILTER_SCANLINES: now captures the scene and redraws
     * it on the tessellated barrel surface, then bends the scanlines onto
     * it. So it must capture (glCopyTexSubImage2D) and multiplicatively
     * blend, and draw many tessellated primitives (grid strips + one
     * GL_LINE_STRIP per scanline row). */
    gl_stub_counts_reset();
    render3d_postprocess_filter_render(RENDER3D_POST_FILTER_SCANLINES, 0, 0, 800, 600);
    ASSERT_TRUE("scanlines captures the scene (barrel surface)",
                gl_stub_counts[GL_STUB_glCopyTexSubImage2D] > 0);
    ASSERT_TRUE("scanlines draws tessellated primitives",
                gl_stub_counts[GL_STUB_glBegin] > 1);
    ASSERT_TRUE("scanlines calls glBlendFunc",
                gl_stub_counts[GL_STUB_glBlendFunc] > 0);
    render3d_postprocess_filter_reset();

    /* Test reset function */
    /* RENDER3D_POST_FILTER_FILM_GRAIN */
    /* 1. First render generates the texture */
    gl_stub_counts_reset();
    render3d_postprocess_filter_render(RENDER3D_POST_FILTER_FILM_GRAIN, 0, 0, 800, 600);
    ASSERT_TRUE("film grain generates textures on first render",
                gl_stub_counts[GL_STUB_glGenTextures] > 0);
    ASSERT_TRUE("film grain uploads noise texture on first render",
                gl_stub_counts[GL_STUB_glTexImage2D] > 0);
    ASSERT_TRUE("film grain binds texture",
                gl_stub_counts[GL_STUB_glBindTexture] > 0);
    ASSERT_TRUE("film grain sets blend function",
                gl_stub_counts[GL_STUB_glBlendFunc] > 0);
    ASSERT_TRUE("film grain sets modulate mode",
                gl_stub_counts[GL_STUB_glTexEnvi] > 0);
    ASSERT_TRUE("film grain draws quads",
                gl_stub_counts[GL_STUB_glBegin] > 0);

    /* 2. Second render reuses the texture */
    gl_stub_counts_reset();
    render3d_postprocess_filter_render(RENDER3D_POST_FILTER_FILM_GRAIN, 0, 0, 800, 600);
    ASSERT_INT("film grain does not regenerate texture on subsequent render",
               (int)gl_stub_counts[GL_STUB_glGenTextures], 0);
    ASSERT_INT("film grain does not upload texture again",
               (int)gl_stub_counts[GL_STUB_glTexImage2D], 0);
    ASSERT_TRUE("film grain binds texture on subsequent render",
                gl_stub_counts[GL_STUB_glBindTexture] > 0);

    /* Test reset function (which should clean up texture) */
    gl_stub_counts_reset();
    render3d_postprocess_filter_reset();
    ASSERT_TRUE("reset deletes texture",
                gl_stub_counts[GL_STUB_glDeleteTextures] > 0);

    /* 3. Render after reset should regenerate the texture */
    gl_stub_counts_reset();
    render3d_postprocess_filter_render(RENDER3D_POST_FILTER_FILM_GRAIN, 0, 0, 800, 600);
    ASSERT_TRUE("film grain regenerates texture after reset",
                gl_stub_counts[GL_STUB_glGenTextures] > 0);
    ASSERT_TRUE("film grain uploads noise texture after reset",
                gl_stub_counts[GL_STUB_glTexImage2D] > 0);

    /* Clean up again */
    render3d_postprocess_filter_reset();

    /* Mode name strings */
    ASSERT_TRUE("chromatic name non-NULL",
                render3d_postprocess_filter_mode_name(RENDER3D_POST_FILTER_CHROMATIC_ABERRATION) != NULL);
    ASSERT_TRUE("vignette name non-NULL",
                render3d_postprocess_filter_mode_name(RENDER3D_POST_FILTER_VIGNETTE) != NULL);
    ASSERT_TRUE("scanlines name non-NULL",
                render3d_postprocess_filter_mode_name(RENDER3D_POST_FILTER_SCANLINES) != NULL);
    ASSERT_TRUE("film grain name matches",
                strcmp(render3d_postprocess_filter_mode_name(RENDER3D_POST_FILTER_FILM_GRAIN), "Film grain") == 0);
}

#endif

/* Pure geometry check of the warp-surface types - no GL calls, so it
 * runs in both the stub and real-GL builds. */
static void test_postprocess_surface_math(void) {
    printf("--- postprocess warp-surface types (pure) ---\n");
    const int W = 800, H = 600;
    float x, y;

    /* FLAT: an identity map onto the rect - the flat<->warp morph's zero
     * point. */
    Render3dPostSurface flat = render3d_post_surface_flat(W, H);
    render3d_post_surface_point(&flat, 0.5f, 0.5f, &x, &y);
    ASSERT_TRUE("flat centre maps to rect centre",
                fabsf(x - W * 0.5f) < 0.01f && fabsf(y - H * 0.5f) < 0.01f);
    render3d_post_surface_point(&flat, 0.0f, 0.0f, &x, &y);
    ASSERT_TRUE("flat (0,0) maps to rect origin",
                fabsf(x) < 0.01f && fabsf(y) < 0.01f);
    render3d_post_surface_point(&flat, 1.0f, 1.0f, &x, &y);
    ASSERT_TRUE("flat (1,1) maps to rect far corner",
                fabsf(x - W) < 0.01f && fabsf(y - H) < 0.01f);

    /* BARREL (convex CRT bulge): the centre stays put, the edges bow
     * outward, and the corners pull inward - so the rectangular corners
     * fall outside the image (the black vignette the scanlines filter
     * fills behind the mesh). */
    Render3dPostSurface barrel = render3d_post_surface_barrel(W, H, 0.10f);
    render3d_post_surface_point(&barrel, 0.5f, 0.5f, &x, &y);
    ASSERT_TRUE("barrel centre is fixed",
                fabsf(x - W * 0.5f) < 0.01f && fabsf(y - H * 0.5f) < 0.01f);
    /* Far corner (u=1,v=1) pulled inward: inside the rect, still in the
     * far quadrant. */
    render3d_post_surface_point(&barrel, 1.0f, 1.0f, &x, &y);
    ASSERT_TRUE("barrel pulls the far corner inward",
                x < (float)W && y < (float)H && x > W * 0.5f && y > H * 0.5f);
    /* Near corner (u=0,v=0) pulled inward likewise. */
    render3d_post_surface_point(&barrel, 0.0f, 0.0f, &x, &y);
    ASSERT_TRUE("barrel pulls the near corner inward",
                x > 0.0f && y > 0.0f && x < W * 0.5f && y < H * 0.5f);
    /* Convexity: the top-edge midpoint bows further out (higher y) than
     * the top corner - the tell-tale barrel (vs. concave pincushion). */
    float mid_x, mid_y, cor_x, cor_y;
    render3d_post_surface_point(&barrel, 0.5f, 1.0f, &mid_x, &mid_y);
    render3d_post_surface_point(&barrel, 1.0f, 1.0f, &cor_x, &cor_y);
    ASSERT_TRUE("barrel top edge bows outward (convex, not concave)",
                mid_y > cor_y);

    /* RIPPLE (reserved for a future underwater filter): an animated
     * horizontal offset - the same grid point moves in x as t advances,
     * and only in x (the wobble is horizontal). */
    Render3dPostSurface ra = render3d_post_surface_ripple(W, H, 0.0f, 4.0f);
    Render3dPostSurface rb = render3d_post_surface_ripple(W, H, 0.4f, 4.0f);
    float xa, ya, xb, yb;
    render3d_post_surface_point(&ra, 0.25f, 0.30f, &xa, &ya);
    render3d_post_surface_point(&rb, 0.25f, 0.30f, &xb, &yb);
    ASSERT_TRUE("ripple animates x with t", fabsf(xa - xb) > 1e-4f);
    ASSERT_TRUE("ripple leaves y unchanged", fabsf(ya - yb) < 1e-4f);
}

int main(int argc, char **argv) {
#ifdef GL_STUBS
    /* In stub mode these are no-ops, but keeping the calls preserves coverage
     * for GLUT symbol declarations. */
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA | GLUT_DEPTH | GLUT_STENCIL);
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
    test_scene_accum_effect_profile_section();
    test_scene_projection_modes();
    test_scene_ortho_zoom_rescales();
    test_frame_ctx_defaults();
    test_grid_theme_uses_fog_predicate();   /* pure; runs in both builds */
    test_light_theme_presets();             /* pure; runs in both builds */
    test_postprocess_surface_math();        /* pure; runs in both builds */

#ifndef GL_STUBS
    printf("--- GL-emitting scene smoke checks skipped in real-GL test build ---\n");
    printf("Run `make test-render3d-render` for no-op GL path coverage.\n");

    printf("\ntest_scene_render: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
#endif

    test_scene_grid_render();
    test_scene_axes_render();
    test_scene_backdrop_render();
    test_scene_backdrop_drones();
    test_scene_backdrop_fairies();
    test_scene_backdrop_fairies_clearance();
    test_scene_backdrop_drones_clearance();
    test_render3d_winding_and_gizmo();
    test_buffer_hooks();
    test_helpers_suspend_stencil_test();
    test_scene_lights();
    test_scene_overlays();
    test_anim_time_propagation();
    test_focus_vertex_context();
    test_lighting_array_access();
    test_grid_table_arrays();
    test_viewport_dimensions();
    test_render_mode_toggles();
    test_plain_wireframe_uses_main_fill();
    test_wireframe_hidden_line_passes();
    test_vertex2f_overlay_parity();
    test_vertex2f_guide_cursor_dot();
    test_vertex_label_text();
    test_background_colors_are_independent();
    test_null_execute_fn_draws_helpers_without_clearing();
    test_scene_grid_fog_matches_predicate();
    test_nv_fog_distance_radial_optin();
#ifdef GL_STUBS
    test_scene_axes_themes_and_opacity();
    test_scene_lights_indicators();
    test_postprocess_filters();
#endif

    printf("\ntest_scene_render: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
