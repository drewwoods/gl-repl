/*
 * test_scene_render.c - Unit tests for scene_* modules (grid, axes, backdrop, lights, overlays).
 * Tests operate independently of repl_state with a minimal GL context.
 */
#include "scene_grid.h"
#include "scene_axes.h"
#include "scene_backdrop.h"
#include "scene_lights.h"
#include "scene_overlays.h"
#include "scene_render_types.h"
#include "repl_flatten.h"

#include <stdio.h>
#include <string.h>
#include <GL/gl.h>
#include <GLUT/glut.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s] (line %d)\n", label, __LINE__); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    g_run++; \
    if ((got) == (exp)) g_pass++; \
    else printf("FAIL [%s] got %d, expected %d (line %d)\n", \
                label, (int)(got), (int)(exp), __LINE__); \
} while (0)

#define ASSERT_FLOAT(label, got, exp) do { \
    g_run++; \
    float delta = (got) - (exp); \
    if (delta < 0) delta = -delta; \
    if (delta < 1e-5) g_pass++; \
    else printf("FAIL [%s] got %.6f, expected %.6f (line %d)\n", \
                label, (float)(got), (float)(exp), __LINE__); \
} while (0)

/* Minimal execute callback for testing. */
static void test_execute_noop(float alpha_scale, int skip_geom_before_pc,
                              int flat_cmd_count, FlatProgramView program,
                              void *user_data) {
    (void)alpha_scale;
    (void)skip_geom_before_pc;
    (void)flat_cmd_count;
    (void)program;
    (void)user_data;
}

static void test_execute_reset_noop(void *user_data) {
    (void)user_data;
}

/* Build a test SceneRenderConfig with sensible defaults. */
static SceneRenderConfig make_test_config(void) {
    SceneRenderConfig cfg = {0};
    cfg.execute_fn = test_execute_noop;
    cfg.execute_reset_fn = test_execute_reset_noop;
    cfg.execute_user_data = NULL;

    cfg.anim_time = 0.0f;

    cfg.use_accum = 1;
    cfg.accum_aa_enabled = 1;
    cfg.accum_samples = 2;

    cfg.viewport_w = 800;
    cfg.viewport_h = 600;

    cfg.user_lighting_enabled = 0;
    cfg.show_light_indicators = 0;
    cfg.backdrop_mode = 0;
    cfg.show_vertex_outlines = 0;

    /* Grid defaults. */
    for (int i = 0; i < GRID_MAJOR_COUNT; i++)
        cfg.grid_major_steps[i] = 1.0f;
    for (int i = 0; i < GRID_EXTENT_COUNT; i++)
        cfg.grid_extents[i] = 10.0f;

    cfg.cursor_block_begin_idx = -1;
    cfg.cursor_block_end_idx = -1;
    cfg.cursor_block_source_line = -1;
    cfg.edit_line_idx = -1;
    cfg.cursor_func_scope_mask = 0;
    cfg.cursor_call_src_cmd_idx = -1;

    /* Legacy fields. */
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
    cfg.multisample_enabled = 0;
    cfg.line_smooth_enabled = 0;
    cfg.wireframe = 0;
    cfg.grid_theme = 0;
    cfg.grid_extent_idx = 0;
    cfg.grid_major_idx = 0;
    cfg.axes_theme = 0;
    cfg.show_guides = 1;
    cfg.show_vpoints = 0;
    cfg.show_vnums = 0;
    cfg.show_normals = 0;
    cfg.replaying = 0;
    cfg.replay_mode = 0;
    cfg.replay_tess_preview = 0;
    cfg.replay_vertex_points = 0;
    cfg.replay_has_fades = 0;
    cfg.replay_base_limit = 0;
    cfg.show_current_poly = 0;
    cfg.alpha_scale = 1.0f;

    return cfg;
}

/* Build a test FrameRenderContext. */
static FrameRenderContext make_test_frame_ctx(void) {
    FrameRenderContext ctx = {0};
    ctx.config = make_test_config();
    ctx.focus.valid = 0;
    return ctx;
}

/* --- Tests for SceneRenderConfig structure ----------------------- */

static void test_config_defaults(void) {
    printf("--- SceneRenderConfig defaults ---\n");

    SceneRenderConfig cfg = make_test_config();

    ASSERT_INT("execute_fn set", cfg.execute_fn != NULL, 1);
    ASSERT_INT("execute_reset_fn set", cfg.execute_reset_fn != NULL, 1);
    ASSERT_FLOAT("viewport_w matches", cfg.viewport_w, 800);
    ASSERT_FLOAT("viewport_h matches", cfg.viewport_h, 600);
    ASSERT_INT("use_accum default", cfg.use_accum, 1);
    ASSERT_INT("accum aa default", cfg.accum_aa_enabled, 1);
    ASSERT_INT("accum samples default", cfg.accum_samples, 2);
    ASSERT_INT("user_lighting_enabled default", cfg.user_lighting_enabled, 0);
    ASSERT_FLOAT("anim_time default", cfg.anim_time, 0.0f);
    ASSERT_INT("cursor block invalid by default", cfg.cursor_block_begin_idx, -1);
}

/* --- Tests for FrameRenderContext ---------------------------------- */

static void test_frame_ctx_defaults(void) {
    printf("--- FrameRenderContext defaults ---\n");

    FrameRenderContext ctx = make_test_frame_ctx();

    ASSERT_INT("config has execute_fn", ctx.config.execute_fn != NULL, 1);
    ASSERT_INT("focus not valid by default", ctx.focus.valid, 0);
}

/* --- Tests for scene_grid_render (minimal) ----------------------- */

static void test_scene_grid_render(void) {
    printf("--- scene_grid_render ---\n");

    FrameRenderContext ctx = make_test_frame_ctx();

    /* Just ensure it doesn't crash with null/empty config. */
    scene_grid_render(&ctx);
    ASSERT_TRUE("scene_grid_render did not crash", 1);

    /* With viewport set. */
    ctx.config.viewport_w = 1024;
    ctx.config.viewport_h = 768;
    scene_grid_render(&ctx);
    ASSERT_TRUE("scene_grid_render with explicit viewport did not crash", 1);
}

/* --- Tests for scene_axes_render (minimal) ----------------------- */

static void test_scene_axes_render(void) {
    printf("--- scene_axes_render ---\n");

    FrameRenderContext ctx = make_test_frame_ctx();

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

    FrameRenderContext ctx = make_test_frame_ctx();

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

    FrameRenderContext ctx = make_test_frame_ctx();

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
    printf("--- scene_overlays (vertex_numbers, normals, outlines) ---\n");

    FrameRenderContext ctx = make_test_frame_ctx();

    /* Vertex numbers. */
    scene_overlays_render_vertex_numbers(&ctx);
    ASSERT_TRUE("scene_overlays_render_vertex_numbers did not crash", 1);

    /* Normal vectors. */
    scene_overlays_render_normal_vectors(&ctx);
    ASSERT_TRUE("scene_overlays_render_normal_vectors did not crash", 1);

    /* Polygon outlines. */
    scene_overlays_render_outlines(&ctx, 0, 0);
    ASSERT_TRUE("scene_overlays_render_outlines did not crash", 1);

    /* With config flags set. */
    ctx.config.show_vnums = 1;
    ctx.config.show_normals = 1;
    ctx.config.show_vertex_outlines = 1;
    scene_overlays_render_vertex_numbers(&ctx);
    scene_overlays_render_normal_vectors(&ctx);
    scene_overlays_render_outlines(&ctx, 1, 0);
    ASSERT_TRUE("scene_overlays with flags set did not crash", 1);
}

/* --- Tests for animation time propagation ----------------------- */

static void test_anim_time_propagation(void) {
    printf("--- anim_time propagation ---\n");

    FrameRenderContext ctx = make_test_frame_ctx();

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

    FrameRenderContext ctx = make_test_frame_ctx();

    /* With valid focus vertex. */
    ctx.focus.valid = 1;
    ctx.focus.pos[0] = 1.0f;
    ctx.focus.pos[1] = 2.0f;
    ctx.focus.pos[2] = 3.0f;

    scene_grid_render(&ctx);
    ASSERT_TRUE("grid with valid focus vertex did not crash", 1);
    ASSERT_INT("focus remains valid", ctx.focus.valid, 1);
}

/* --- Tests for lighting array access ----------------------------- */

static void test_lighting_array_access(void) {
    printf("--- lighting array in config ---\n");

    FrameRenderContext ctx = make_test_frame_ctx();

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

    FrameRenderContext ctx = make_test_frame_ctx();

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

    FrameRenderContext ctx = make_test_frame_ctx();

    /* Small viewport. */
    ctx.config.viewport_w = 100;
    ctx.config.viewport_h = 100;
    scene_grid_render(&ctx);
    ASSERT_TRUE("small viewport did not crash", 1);

    /* Large viewport. */
    ctx.config.viewport_w = 4096;
    ctx.config.viewport_h = 2160;
    scene_grid_render(&ctx);
    ASSERT_TRUE("large viewport did not crash", 1);

    /* Extreme aspect ratio. */
    ctx.config.viewport_w = 3840;
    ctx.config.viewport_h = 480;
    scene_grid_render(&ctx);
    ASSERT_TRUE("extreme aspect ratio did not crash", 1);
}

/* --- Tests for render modes/toggles ------------------------------ */

static void test_render_mode_toggles(void) {
    printf("--- render mode toggles ---\n");

    FrameRenderContext ctx = make_test_frame_ctx();

    /* Wireframe. */
    ctx.config.wireframe = 1;
    scene_grid_render(&ctx);
    ASSERT_INT("wireframe set", ctx.config.wireframe, 1);
    ctx.config.wireframe = 0;

    /* Replaying. */
    ctx.config.replaying = 1;
    scene_grid_render(&ctx);
    ASSERT_INT("replaying set", ctx.config.replaying, 1);
    ctx.config.replaying = 0;

    /* Show guides. */
    ctx.config.show_guides = 0;
    scene_grid_render(&ctx);
    ASSERT_INT("show_guides disabled", ctx.config.show_guides, 0);
    ctx.config.show_guides = 1;

    ASSERT_TRUE("all toggles tested", 1);
}

int main(int argc, char **argv) {
    /* Initialize GLUT and create a minimal window for GL context */
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA | GLUT_DEPTH);
    glutInitWindowSize(800, 600);
    glutCreateWindow("test_scene_render");

    printf("==> test_scene_render\n");
    printf("Scene render modules with minimal GL context\n\n");

    test_config_defaults();
    test_frame_ctx_defaults();
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

    printf("\ntest_scene_render: %d/%d passed\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
