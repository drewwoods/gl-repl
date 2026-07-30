#define _DEFAULT_SOURCE  /* mkdtemp() */
#include "editor/state.h"
#include "app/glr_camera.h"
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "app/glr_defaults.h"
#include "app/glr_pointer_script.h"
#include "config.h"      /* QUIT_RECOVERY_FILE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "subsystems/assign_plot/assign_plot.h"
#include "editor/undo.h"
#include "repl/example_loader.h"
#include "repl/export.h"
#include "repl/flatten.h"
#include "repl/pipeline.h"
#include "repl/state_notify.h"
#include "editor/input.h"
#include "editor/search.h"
#include "keys.h"
#include "ui/app/layout.h"   /* CODE_PANEL_LAYOUT_* */
#include "ui/core/metrics.h"
#include "support/test_harness.h"
#ifdef GL_STUBS
#include <GL/gl_stub_counts.h>
#endif

void glr_color_picker_install_host(void);

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

#define ASSERT_FLOAT(label, got, exp) \
    TEST_ASSERT_FLOAT(&g_harness, label, got, exp, 1e-5f)

#define ASSERT_STR(label, got, exp) \
    TEST_ASSERT_STR(&g_harness, label, got, exp)

/* Rename the controller's downstream render delegates so this test can stub
 * them and inspect the per-frame config without a real GL context. */
#define render3d_draw_scene              test_scene_render_3d_scene
#define glr_camera_load_modelview          test_glr_camera_load_modelview
#define replay_ui_hud_render               test_replay_ui_hud_render
#define ui_panels_render_code_panel        test_ui_panels_render_code_panel
#define ui_autocomplete_panel_render       test_ui_autocomplete_panel_render
#define ui_menu_bar_render_example_dropdown test_ui_menu_bar_render_example_dropdown
#define ui_variable_panel_render           test_ui_variable_panel_render
#define ui_panels_render_scene_status      test_ui_panels_render_scene_status
#define ui_tabbed_overlay_render           test_ui_tabbed_overlay_render
#define ui_profile_panel_render            test_ui_profile_panel_render
#define ui_memory_panel_render             test_ui_memory_panel_render
#define glutSetCursor                      test_glutSetCursor
#define glr_export_mesh_ply                test_glr_export_mesh_ply

/* glr_camera.h was already pulled in at line 3 (before the #define),
 * so its `glr_camera_load_modelview` declaration is preserved as the
 * real name. Forward-declare the test stub explicitly so glr_ctrl.c's
 * macro-substituted call has a visible prototype. The other stubs
 * are reached only through glr_ctrl.c's includes, which the macros
 * cover. */
void test_glr_camera_load_modelview(const GlrCameraPose *pose);
/* ui/subsystems/variable_panel.h is pulled in before the #define (via
 * ui/app/snapshot.h), so its real-name prototype is preserved too;
 * forward-declare this stub the same way. */
void test_ui_variable_panel_render(const UiVariablePanelView *view);
void test_glutSetCursor(int cursor);
int test_glr_export_mesh_ply(const char *path, int srgb_decode);

#include "app/glr_ctrl.c"
/* The input router was carved out of glr_ctrl.c into glr_ctrl_router.c; this
 * white-box test drives router entry points and a few router statics
 * (glr_ctrl_save_quit_recovery, route_numeric_swatch_hit), so include that TU
 * too while the UI-render mocks are active. glr_ctrl_router.o is filtered out
 * of test_glr_ctrl_OBJS to avoid a double definition. The router calls no
 * mocked symbol, so compiling it in this mock context is identical to normal. */
#include "app/glr_ctrl_router.c"

#undef render3d_draw_scene
#undef glr_camera_load_modelview
#undef replay_ui_hud_render
#undef ui_panels_render_code_panel
#undef ui_autocomplete_panel_render
#undef ui_menu_bar_render_example_dropdown
#undef ui_variable_panel_render
#undef ui_panels_render_scene_status
#undef ui_tabbed_overlay_render
#undef ui_profile_panel_render
#undef ui_memory_panel_render
#undef glutSetCursor
#undef glr_export_mesh_ply

static Render3dRenderConfig g_last_scene_config;
/* Snapshot copy captured by the replay HUD stub; replaces the old
 * UiReplayHudState struct after replay_ui_hud_render was reshaped to
 * take the per-frame snapshot directly. */
static UiRenderSnapshot  g_last_replay_hud_snap;
static int g_scene_render_calls = 0;
static int g_replay_hud_calls = 0;
static int g_t_idx = -1;
static float g_predef_value_seen_in_scene = 0.0f;
static float g_predef_value_seen_in_hud = 0.0f;
static int g_flat_count_seen_in_hud = -1;
static float g_scratch_value_seen_in_scene = 0.0f;

static const float g_mutated_predef_value = 123.0f;
static const int g_mutated_flat_count = 99;
static const float g_mutated_scratch_value = 77.0f;

static int g_simulated_mods = 0;
static int simulated_mods_provider(void) {
    return g_simulated_mods;
}

static void set_cmd3(GLCmd *cmd, CmdType type, int src_idx,
                     const char *source, float a, float b, float c) {
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = type;
    cmd->valid = 1;
    cmd->src_cmd_idx = src_idx;
    cmd->call_src_cmd_idx = -1;
    cmd->root_call_src_cmd_idx = -1;
    cmd->num_args = 3;
    cmd->args[0] = a;
    cmd->args[1] = b;
    cmd->args[2] = c;
    /* source text is stored in the editor buffer, not in GLCmd */
    (void)source;
}

static int count_highlight_kind_on_line(UiHighlightKind kind, int line_idx) {
    const UiHighlightList *list = editor_state_highlights();
    int count = 0;

    if (!list)
        return 0;
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].kind == kind &&
            list->items[i].line_idx == line_idx)
            count++;
    }
    return count;
}

int test_scene_render_3d_scene(Render3dState *state,
                               const Render3dRenderConfig *config) {
    (void)state;
    g_scene_render_calls++;
    g_last_scene_config = *config;

    if (g_t_idx >= 0) {
        g_predef_value_seen_in_scene = g_predef_vars[g_t_idx].value;
        g_predef_vars_mut[g_t_idx].value = g_mutated_predef_value;
    }
    repl_eval_scratch_get(0, 0, &g_scratch_value_seen_in_scene);
    repl_eval_scratch_set(0, 0, g_mutated_scratch_value);

    repl_state_flat_program_set_count(g_mutated_flat_count);
    return 0;
}

void test_glr_camera_load_modelview(const GlrCameraPose *pose) {
    (void)pose;
}

void test_replay_ui_hud_render(const struct UiRenderSnapshot *snap) {
    g_replay_hud_calls++;
    g_last_replay_hud_snap = *snap;

    if (g_t_idx >= 0)
        g_predef_value_seen_in_hud = g_predef_vars[g_t_idx].value;
    g_flat_count_seen_in_hud = repl_state_flat_program_count();
}

void test_ui_panels_render_code_panel(const UiRenderSnapshot *snap,
                                      UiCodePanelOutput *out) {
    (void)snap; (void)out;
}
void test_ui_autocomplete_panel_render(const UiRenderSnapshot *snap,
                                       int cursor_px, int cursor_py) {
    (void)snap; (void)cursor_px; (void)cursor_py;
}
void test_ui_menu_bar_render_example_dropdown(const UiRenderSnapshot *snap) { (void)snap; }
void test_ui_variable_panel_render(const UiVariablePanelView *view) { (void)view; }
void test_ui_panels_render_scene_status(const UiRenderSnapshot *snap) { (void)snap; }
void test_ui_tabbed_overlay_render(const UiOverlayState *in) { (void)in; }
void test_ui_profile_panel_render(const UiProfilePanelView *view) { (void)view; }
void test_ui_memory_panel_render(const UiMemoryPanelView *view)  { (void)view; }
/* Last cursor the controller pushed to the host. -1 until something sets
 * one, so a test can tell "asked for inherit" from "never asked". */
static int g_last_set_cursor = -1;
void test_glutSetCursor(int cursor) { g_last_set_cursor = cursor; }
int test_glr_export_mesh_ply(const char *path, int srgb_decode) {
    (void)path; (void)srgb_decode;
    return 0;
}
static int g_test_point_parameter_loader_calls = 0;
static ReplExecutorPointParameterProc test_missing_point_parameter_loader(
    const char *proc_name) {
    (void)proc_name;
    g_test_point_parameter_loader_calls++;
    return NULL;
}

/* Build the variable-panel view from live app state (mirrors the
 * pre-narrowing NULL-snapshot path) so these tests can drive rect/hit by
 * count without assembling a full UiRenderSnapshot. */
static void vp_rect(int count, int *px, int *py, int *pw, int *ph) {
    UiVariablePanelView v = ui_app_variable_panel_view_live(count);
    ui_variable_panel_rect(&v, px, py, pw, ph);
}
static int vp_hit_row(int count, int gx, int gy, int *row) {
    UiVariablePanelView v = ui_app_variable_panel_view_live(count);
    return ui_variable_panel_hit_row(&v, gx, gy, row);
}

static int find_hit_point_in_code_panel(int desired_kind, UiHit *out_hit,
                                        int *out_x, int *out_y) {
    UiRenderSnapshot snap;
    int cp_x, cp_y, cp_w, cp_h;
    int win_h;

    glr_ctrl_build_ui_snapshot(&snap);
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    win_h = ui_state_viewport().window_h;
    if (cp_w <= 0 || cp_h <= 0 || win_h <= 0)
        return 0;

    for (int gl_y = cp_y + 1; gl_y < cp_y + cp_h - 1; gl_y++) {
        int my = win_h - gl_y;
        for (int mx = cp_x + 1; mx < cp_x + cp_w - 1; mx++) {
            UiHit hit = ui_panels_hit_test(&snap, mx, my,
                                           repl_eval_predef_view().count);
            if (hit.kind == desired_kind &&
                (desired_kind != UI_HIT_CODE_TEXT ||
                 (hit.line_idx >= 0 && hit.char_idx >= 0))) {
                if (out_hit) *out_hit = hit;
                if (out_x) *out_x = mx;
                if (out_y) *out_y = my;
                return 1;
            }
        }
    }
    return 0;
}

static int find_code_text_hit_for_line(int desired_line_idx, UiHit *out_hit,
                                       int *out_x, int *out_y) {
    UiRenderSnapshot snap;
    int cp_x, cp_y, cp_w, cp_h;
    int win_h;

    glr_ctrl_build_ui_snapshot(&snap);
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    win_h = ui_state_viewport().window_h;
    if (cp_w <= 0 || cp_h <= 0 || win_h <= 0)
        return 0;

    for (int gl_y = cp_y + 1; gl_y < cp_y + cp_h - 1; gl_y++) {
        int my = win_h - gl_y;
        for (int mx = cp_x + 1; mx < cp_x + cp_w - 1; mx++) {
            UiHit hit = ui_panels_hit_test(&snap, mx, my,
                                           repl_eval_predef_view().count);
            if (hit.kind == UI_HIT_CODE_TEXT &&
                hit.line_idx == desired_line_idx) {
                if (out_hit) *out_hit = hit;
                if (out_x) *out_x = mx;
                if (out_y) *out_y = my;
                return 1;
            }
        }
    }
    return 0;
}

static void prepare_display_fixture(void) {
    GLCmd *doc_cmds;
    GLCmd *flat_cmds;
    GlrPresentationState *presentation;

    memset(&g_last_scene_config, 0, sizeof(g_last_scene_config));
    memset(&g_last_replay_hud_snap, 0, sizeof(g_last_replay_hud_snap));
    g_scene_render_calls = 0;
    g_replay_hud_calls = 0;
    g_predef_value_seen_in_scene = 0.0f;
    g_predef_value_seen_in_hud = 0.0f;
    g_flat_count_seen_in_hud = -1;
    g_scratch_value_seen_in_scene = 0.0f;
    g_t_idx = -1;

    glr_ctrl_reset_all();
    repl_eval_init_predef_vars();

    ui_state_viewport_set_size(800, 600);
    glr_camera_set(11.0f, 22.0f, 7.5f, 0.5f, -0.25f, 1.75f, 0.9f);
    glr_state_render_mut()->use_accum = 0;
    glr_state_render_mut()->accum_effect = RENDER3D_ACCUM_EFFECT_OFF;
    glr_state_render_mut()->accum_passes = 1;
    glr_state_render_mut()->multisample_enabled = 0;
    glr_state_render_mut()->line_smooth_enabled = 1;
    repl_state_render_mut()->clear_color[0] = 0.0f;
    repl_state_render_mut()->clear_color[1] = 0.0f;
    repl_state_render_mut()->clear_color[2] = 0.0f;
    repl_state_render_mut()->clear_color[3] = 1.0f;

    repl_state_flat_program_set_user_lighting_enabled(1);

    presentation = glr_state_presentation_mut();
    presentation->show_vertex_points = 1;
    presentation->show_light_indicators = 1;
    presentation->highlight_current_poly = 1;
    presentation->xform_guide_mode = (Render3dXformGuideMode)9; /* out-of-range: exercises clamp path */
    presentation->code_panel_layout = CODE_PANEL_LAYOUT_BOTTOM;

    repl_state_variables_mut()->anim_time = 4.25f;
    editor_insert_mode_set(1);

    doc_cmds = repl_state_document_cmds_mut();
    flat_cmds = repl_state_flat_program_cmds_mut();
    set_cmd3(&doc_cmds[0], CMD_VERTEX3F, 0, "glVertex3f(1, 2, 3);",
             1.0f, 2.0f, 3.0f);
    set_cmd3(&doc_cmds[1], CMD_COLOR3F, 1, "glColor3f(0.2, 0.4, 0.6);",
             0.2f, 0.4f, 0.6f);
    /* Store source text in editor buffer (flat cmds resolve text via src_cmd_idx) */
    editor_buffer_set_line(0, "glVertex3f(1, 2, 3);");
    editor_buffer_set_line(1, "glColor3f(0.2, 0.4, 0.6);");
    set_cmd3(&flat_cmds[0], CMD_VERTEX3F, 0, "glVertex3f(1, 2, 3);",
             1.0f, 2.0f, 3.0f);
    set_cmd3(&flat_cmds[1], CMD_COLOR3F, 1, "glColor3f(0.2, 0.4, 0.6);",
             0.2f, 0.4f, 0.6f);
    repl_state_document_count_set(2);
    repl_state_flat_program_set_count(2);
    editor_state_edit_line_set(repl_state_document_count());
    editor_input_set_text("glColor3f(0.2, 0.4, 0.6);");
    editor_cursor_pos_set(7);

    g_t_idx = repl_eval_find_predef_var_idx("t");
    ASSERT_TRUE("t predef exists", g_t_idx >= 0);
    if (g_t_idx >= 0)
        g_predef_vars_mut[g_t_idx].value = 9.0f;
    repl_eval_scratch_set(0, 0, 4.0f);

    replay_start();
    repl_state_flat_program_set_user_lighting_enabled(1);

    replay_state_mut()->active = 1;
    replay_state_mut()->state = REPLAY_PLAYING;
    replay_state_mut()->pc = 1;
    replay_state_mut()->mode = REPLAY_MODE_VERTEX;
    replay_state_mut()->speed = 2.5f;
    replay_state_mut()->expand_args = 1;
    replay_state_mut()->normal_display = REPLAY_NORMAL_DISPLAY_DIRECTION;
    replay_state_mut()->vertex_label = 1;
    replay_state_mut()->total_flat_cmds = 777;

    repl_state_flat_program_clear_dirty();
    repl_state_normals_dirty_clear();
}

static void test_display_frame_builds_config_and_restores_live_state(void) {
    int saved_flat_count;
    float saved_t_value;

    printf("--- imrepl_ctrl display_frame ---\n");
    prepare_display_fixture();

    saved_flat_count = repl_state_flat_program_count();
    saved_t_value = g_predef_vars[g_t_idx].value;

    glr_ctrl_display_frame();

    ASSERT_INT("scene render called once", g_scene_render_calls, 1);
    ASSERT_INT("replay HUD called once", g_replay_hud_calls, 1);

    ASSERT_TRUE("scene execute fn wired", g_last_scene_config.execute_fn != NULL);
    ASSERT_TRUE("scene execute user data null", g_last_scene_config.execute_user_data == NULL);
    /* viewport_w/h removed from Render3dRenderConfig — scene helpers use
     * render3d_w/render3d_h (the active GL viewport) instead. The HUD asserts
     * below read the window viewport from the snapshot directly. */
    ASSERT_FLOAT("camera distance forwarded", g_last_scene_config.cam_dist, 7.5f);
    ASSERT_FLOAT("camera tx forwarded", g_last_scene_config.cam_tx, 0.5f);
    ASSERT_FLOAT("camera glow forwarded", g_last_scene_config.cam_motion_glow, 0.9f);
    ASSERT_INT("user lighting copied", g_last_scene_config.user_lighting_enabled, 1);
    ASSERT_INT("light indicators copied", g_last_scene_config.show_light_indicators, 1);
    /* The replay-fade plan moved out of Render3dRenderConfig and is now a
     * controller-private static (g_replay_fade_plan). Inspect it directly
     * since this TU includes imrepl_ctrl.c as a compilation unit. */
    ASSERT_INT("replay fade plan inactive without active fades",
               g_replay_fade_plan.active, 0);
    ASSERT_INT("replay fade base limit zero without fades",
               g_replay_fade_plan.base_limit, 0);
    ASSERT_INT("replay fade batches empty",
               g_replay_fade_plan.batch_count, 0);
    ASSERT_FLOAT("replay baseline scratch copied",
                 g_replay_fade_plan.baseline_scratch_arrays[0][0], 4.0f);
    /* The fixture has replay_mode == VERTEX, which gates the tess-preview
     * wireframe overlay — so post_fill_fn is wired even though no fade
     * batches are active. */
    ASSERT_TRUE("post_fill_fn wired when a replay overlay is active",
                g_last_scene_config.post_fill_fn != NULL);
    ASSERT_TRUE("post_overlays_fn wired",
                g_last_scene_config.post_overlays_fn != NULL);
    ASSERT_TRUE("post_resolve_overlays_fn wired (label passes)",
                g_last_scene_config.post_resolve_overlays_fn != NULL);
    ASSERT_INT("tess preview marked active for VERTEX replay mode",
               g_replay_fade_plan.tess_preview_active, 1);
    ASSERT_FLOAT("clear color default r", g_last_scene_config.clear_color[0],
                 CFG_DEFAULT_CLEAR_R);
    ASSERT_FLOAT("clear color default g", g_last_scene_config.clear_color[1],
                 CFG_DEFAULT_CLEAR_G);
    ASSERT_FLOAT("clear color default b", g_last_scene_config.clear_color[2],
                 CFG_DEFAULT_CLEAR_B);
    ASSERT_FLOAT("clear color default a", g_last_scene_config.clear_color[3],
                 CFG_DEFAULT_CLEAR_A);
    ASSERT_FLOAT("alpha scale uses default background",
                 g_last_scene_config.alpha_scale, 1.0f);
    ASSERT_INT("focus vertex valid", g_last_scene_config.focus.valid, 1);
    ASSERT_FLOAT("focus vertex x", g_last_scene_config.focus.pos[0], 1.0f);
    ASSERT_FLOAT("focus vertex y", g_last_scene_config.focus.pos[1], 2.0f);
    ASSERT_FLOAT("focus vertex z", g_last_scene_config.focus.pos[2], 3.0f);

    /* Scene/viewport rect: replay_ui_hud_render now derives the scene
     * rect via ui_layout_scene_rect() and reads the viewport from
     * snap->viewport — neither field travels on the snapshot we capture
     * in the stub. The rect+viewport contracts are exercised by the
     * scene_config asserts above; here we focus on what the snapshot
     * carries. */
    ASSERT_INT("HUD viewport w on snap", g_last_replay_hud_snap.viewport.window_w, 800);
    ASSERT_INT("HUD viewport h on snap", g_last_replay_hud_snap.viewport.window_h, 600);
    ASSERT_INT("HUD layout on snap", g_last_replay_hud_snap.code_panel.layout_mode, CODE_PANEL_LAYOUT_BOTTOM);
    ASSERT_INT("HUD replay mode on snap", g_last_replay_hud_snap.replay.mode, REPLAY_MODE_VERTEX);
    ASSERT_INT("HUD replay pc on snap", g_last_replay_hud_snap.replay.pc, 1);
    ASSERT_INT("HUD replay total cmds on snap", g_last_replay_hud_snap.replay.total_flat_cmds, 777);
    ASSERT_INT("HUD replay state on snap", g_last_replay_hud_snap.replay.state, REPLAY_PLAYING);
    ASSERT_FLOAT("HUD replay speed on snap", g_last_replay_hud_snap.replay.speed, 2.5f);
    ASSERT_INT("HUD replay expand args on snap", g_last_replay_hud_snap.replay.expand_args, 1);
    ASSERT_INT("HUD replay normal display on snap",
               g_last_replay_hud_snap.replay.normal_display,
               REPLAY_NORMAL_DISPLAY_DIRECTION);
    ASSERT_INT("HUD replay vertex label on snap",
               g_last_replay_hud_snap.replay.vertex_label, 1);
    ASSERT_INT("HUD replaying on snap", g_last_replay_hud_snap.replay.active, 1);

    ASSERT_FLOAT("scene saw live predef before mutation", g_predef_value_seen_in_scene, 9.0f);
    ASSERT_FLOAT("scene saw live scratch before mutation", g_scratch_value_seen_in_scene, 4.0f);
    ASSERT_FLOAT("HUD observed mutated predef during frame", g_predef_value_seen_in_hud, g_mutated_predef_value);
    ASSERT_INT("HUD observed restored flat count during frame", g_flat_count_seen_in_hud,
               saved_flat_count);

    ASSERT_FLOAT("predef restored after frame", g_predef_vars[g_t_idx].value, saved_t_value);
    {
        float scratch = 0.0f;
        ASSERT_TRUE("scratch restored after frame",
                    repl_eval_scratch_get(0, 0, &scratch) && fabsf(scratch - 4.0f) < 1e-6f);
    }
    ASSERT_INT("flat count restored after frame", repl_state_flat_program_count(), saved_flat_count);
}

static void test_reshape_clamps_height(void) {
    printf("--- imrepl_ctrl reshape ---\n");

    ui_state_viewport_set_size(320, 240);
    glr_ctrl_reshape(640, 0);

    ASSERT_INT("reshape forwards width", ui_state_viewport().window_w, 640);
    ASSERT_INT("reshape clamps height", ui_state_viewport().window_h, 1);
}

/* Regression test for the profile-coverage bug: a frame should land
 * profile samples in every major section, and the sum of major
 * sections should approximate PROF_FRAME_TOTAL (no section big
 * enough to matter goes unprofiled). The test drives
 * glr_ctrl_display_frame() and inspects the profile state.
 *
 * The "all major sections non-stale" half is deterministic. The
 * "sum approximately equals total" half uses a generous lower bound
 * (50% of PROF_FRAME_TOTAL) to avoid flake from OS scheduling noise
 * — any future regression that drops a major section entirely will
 * blow the lower bound. A tighter upper bound is not asserted
 * because per-section start/end overhead can stack to a real but
 * harmless gap.
 *
 * Even with the loose bound a single frame can be unlucky (a
 * scheduler preemption inside PROF_FRAME_TOTAL but outside every
 * subsection inflates the denominator alone), so the ratio halves
 * are best-of-PROFILE_COVERAGE_ATTEMPTS: retry only while a bound is
 * unmet, and assert against the best measurement. A real regression
 * fails every attempt; noise does not. Every attempt prints its
 * numbers pass or fail, so a run that only just cleared the bound
 * leaves the same evidence a failing one does. */
#define PROFILE_COVERAGE_ATTEMPTS      3
#define PROFILE_MAJOR_COVERAGE_MIN     0.5
#define PROFILE_SNAPSHOT_COVERAGE_MIN  0.7

static void test_display_frame_profile_coverage(void) {
    double best_major_coverage = 0.0;
    double best_snapshot_coverage = 0.0;
    double best_total_us = 0.0;
    double best_sum_us = 0.0;
    int snapshot_measured = 0;
    int attempts_run = 0;

    printf("--- imrepl_ctrl profile coverage ---\n");
    prepare_display_fixture();

    /* Drive one frame after marking both dirty so PROF_AUTONORMAL
     * and PROF_FLATTEN both run. The fixture also leaves replay
     * active so PROF_REPLAY_HUD lands. The frame_begin/_end pair is the
     * host's PROF_FRAME_TOTAL bracket — without it the total never opens. */
    repl_mark_source_dirty();
    repl_state_mark_flat_dirty();
    glr_ctrl_frame_begin();
    glr_ctrl_display_frame();
    glr_ctrl_frame_end();

    /* Sections that should have landed a sample this frame. */
    ProfSection major[] = {
        PROF_FRAME_TOTAL,
        PROF_AUTONORMAL,
        PROF_FLATTEN,
        PROF_SNAPSHOT,
        PROF_SNAPSHOT_TRANSFORMERS,
        PROF_SNAPSHOT_HIGHLIGHTS,
        PROF_SNAPSHOT_VIRTUAL_LINES,
        PROF_SNAPSHOT_PREP,
        PROF_SNAPSHOT_SCENE_CONFIG,
        PROF_SNAPSHOT_UI,
        PROF_RENDER3D,
        PROF_REPLAY_HUD,    /* fixture has replay active */
        PROF_CODE_PANEL,
        PROF_UI_PANELS,
        PROF_PROFILE_PANEL,
        PROF_PROFILE_PANEL_FPS,
        PROF_PROFILE_PANEL_SECTIONS,
        PROF_PROFILE_PANEL_HISTOGRAM,
        PROF_MEMORY_PANEL,
        PROF_COMPOSITOR,
        PROF_FRAME_RESTORE,
    };
    for (size_t i = 0; i < sizeof(major) / sizeof(major[0]); i++) {
        char label[64];
        snprintf(label, sizeof(label), "section %d not stale", (int)major[i]);
        ASSERT_TRUE(label, !prof_section_is_stale(major[i]));
    }

    /* Measure the two ratios, retrying while either is short of its
     * bound. The first pass reuses the frame already driven above. */
    for (int attempt = 1; attempt <= PROFILE_COVERAGE_ATTEMPTS; attempt++) {
        double total_us, sum_us, snapshot_us, snapshot_sub_us;
        double major_coverage, snapshot_coverage;

        if (attempt > 1) {
            repl_mark_source_dirty();
            repl_state_mark_flat_dirty();
            glr_ctrl_frame_begin();
            glr_ctrl_display_frame();
            glr_ctrl_frame_end();
        }
        attempts_run = attempt;

        /* Sum of disjoint top-level sections should be a substantial
         * fraction of PROF_FRAME_TOTAL. PROF_SNAPSHOT / PROF_RENDER3D
         * are themselves aggregates, so summing them with the leaves
         * outside (autonormal, flatten, replay_hud, code_panel,
         * ui_panels, profile_panel, memory_panel, compositor,
         * frame_restore) covers the
         * controller's whole frame body. */
        total_us = prof_section_last_us(PROF_FRAME_TOTAL);
        sum_us =
            prof_section_last_us(PROF_AUTONORMAL) +
            prof_section_last_us(PROF_FLATTEN) +
            prof_section_last_us(PROF_SNAPSHOT) +
            prof_section_last_us(PROF_RENDER3D) +
            prof_section_last_us(PROF_REPLAY_HUD) +
            prof_section_last_us(PROF_CODE_PANEL) +
            prof_section_last_us(PROF_UI_PANELS) +
            prof_section_last_us(PROF_PROFILE_PANEL) +
            prof_section_last_us(PROF_MEMORY_PANEL) +
            prof_section_last_us(PROF_COMPOSITOR) +
            prof_section_last_us(PROF_FRAME_RESTORE);

        /* PROF_SNAPSHOT subsections should sum to near the parent
         * (they are disjoint and exhaustive). */
        snapshot_us = prof_section_last_us(PROF_SNAPSHOT);
        snapshot_sub_us =
            prof_section_last_us(PROF_SNAPSHOT_TRANSFORMERS) +
            prof_section_last_us(PROF_SNAPSHOT_HIGHLIGHTS) +
            prof_section_last_us(PROF_SNAPSHOT_VIRTUAL_LINES) +
            prof_section_last_us(PROF_SNAPSHOT_PREP) +
            prof_section_last_us(PROF_SNAPSHOT_SCENE_CONFIG) +
            prof_section_last_us(PROF_SNAPSHOT_UI);

        major_coverage = total_us > 0.0 ? sum_us / total_us : 0.0;
        snapshot_coverage = snapshot_us > 0.0 ? snapshot_sub_us / snapshot_us : 0.0;

        printf("    attempt %d/%d: FRAME_TOTAL %.1fus, major sum %.1fus (%.1f%%); "
               "SNAPSHOT %.1fus, subs %.1fus (%.1f%%)\n",
               attempt, PROFILE_COVERAGE_ATTEMPTS,
               total_us, sum_us, major_coverage * 100.0,
               snapshot_us, snapshot_sub_us, snapshot_coverage * 100.0);

        if (total_us > best_total_us) best_total_us = total_us;
        if (sum_us > best_sum_us) best_sum_us = sum_us;
        if (major_coverage > best_major_coverage) best_major_coverage = major_coverage;
        if (snapshot_us > 0.0) {
            snapshot_measured = 1;
            if (snapshot_coverage > best_snapshot_coverage)
                best_snapshot_coverage = snapshot_coverage;
        }

        if (best_major_coverage >= PROFILE_MAJOR_COVERAGE_MIN
                && (!snapshot_measured
                    || best_snapshot_coverage >= PROFILE_SNAPSHOT_COVERAGE_MIN))
            break;
    }

    printf("    best of %d attempt(s): major %.1f%%, SNAPSHOT subs %.1f%%\n",
           attempts_run, best_major_coverage * 100.0,
           best_snapshot_coverage * 100.0);

    /* Both should be positive (frame did real work). */
    ASSERT_TRUE("frame total positive", best_total_us > 0.0);
    ASSERT_TRUE("major-section sum positive", best_sum_us > 0.0);

    /* Sum should cover at least half the frame; missing a major
     * section drops it well below this threshold on every attempt. */
    if (best_total_us > 0.0) {
        char label[96];
        snprintf(label, sizeof(label),
                 "major sections cover ≥50%% of FRAME_TOTAL (best %.1f%%)",
                 best_major_coverage * 100.0);
        ASSERT_TRUE(label, best_major_coverage >= PROFILE_MAJOR_COVERAGE_MIN);
    }

    if (snapshot_measured) {
        char label[96];
        snprintf(label, sizeof(label),
                 "SNAPSHOT subs cover ≥70%% of parent (best %.1f%%)",
                 best_snapshot_coverage * 100.0);
        ASSERT_TRUE(label, best_snapshot_coverage >= PROFILE_SNAPSHOT_COVERAGE_MIN);
    }
}

/* PROF_FRAME_TOTAL must span the *host callback*, not just the controller's
 * share of it. The stages gl_repl.c runs on either side of
 * glr_ctrl_display_frame() — scripted input, the post-composite splash / tour
 * overlays, the present — are real per-frame cost: a guided tour's caption
 * overlay alone measured ~10 ms/frame while Frame Total reported ~1.5 ms,
 * because the bracket lived inside the controller and the overlay draws after
 * it returns.
 *
 * Driven on the profiler's test clock so the arithmetic is exact and no GL is
 * needed: the host stages are stood in for by their own prof brackets, exactly
 * as display_func() places them. */
static void test_frame_total_spans_host_stages(void) {
    double total_us, overlay_us, tour_us, present_us;

    printf("--- imrepl_ctrl frame total spans host stages ---\n");

    prof_test_set_now_us(0.0);
    glr_ctrl_frame_begin();          /* host: first statement of the callback */

    /* ... scripted input (1 ms) ... */
    prof_begin(PROF_SCRIPTED_INPUT);
    prof_test_set_now_us(1000.0);
    prof_end(PROF_SCRIPTED_INPUT);

    /* ... glr_ctrl_display_frame() would run here (4 ms) ... */
    prof_test_set_now_us(5000.0);

    /* ... host overlays: the tour's caption/cursor overlay (10 ms) ... */
    prof_begin(PROF_HOST_OVERLAYS);
    prof_begin(PROF_TOUR_OVERLAY);
    prof_test_set_now_us(15000.0);
    prof_end(PROF_TOUR_OVERLAY);
    prof_end(PROF_HOST_OVERLAYS);

    /* ... glFinish + swap (2 ms) ... */
    prof_begin(PROF_PRESENT);
    prof_test_set_now_us(17000.0);
    prof_end(PROF_PRESENT);

    glr_ctrl_frame_end();            /* host: after the swap */

    total_us   = prof_section_last_us(PROF_FRAME_TOTAL);
    overlay_us = prof_section_last_us(PROF_HOST_OVERLAYS);
    tour_us    = prof_section_last_us(PROF_TOUR_OVERLAY);
    present_us = prof_section_last_us(PROF_PRESENT);

    ASSERT_TRUE("host overlays not stale", !prof_section_is_stale(PROF_HOST_OVERLAYS));
    ASSERT_TRUE("tour overlay not stale", !prof_section_is_stale(PROF_TOUR_OVERLAY));
    ASSERT_TRUE("present not stale", !prof_section_is_stale(PROF_PRESENT));
    ASSERT_TRUE("scripted input not stale", !prof_section_is_stale(PROF_SCRIPTED_INPUT));

    ASSERT_TRUE("tour overlay measured 10ms", tour_us == 10000.0);
    ASSERT_TRUE("present measured 2ms", present_us == 2000.0);
    /* The whole callback, not the controller's slice of it. */
    ASSERT_TRUE("frame total is end to end (17ms)", total_us == 17000.0);
    ASSERT_TRUE("frame total contains the host stages",
                total_us >= overlay_us + present_us);

    /* An unpaired end must not close a total that was never opened —
     * glr_ctrl_display_frame() is called bare by tests and tools. */
    prof_test_set_now_us(99000.0);
    glr_ctrl_frame_end();
    ASSERT_TRUE("unpaired frame_end leaves the total alone",
                prof_section_last_us(PROF_FRAME_TOTAL) == 17000.0);

    prof_test_clear_now_us();
}

static void test_variable_panel_motion_routes_through_compile_and_coalesces_undo(void) {
    int px, py, pw, ph;
    int click_x, click_y;
    int hit_row;
    int window_h;
    int var_idx;
    EditorUndoRingState undo_state;

    printf("--- imrepl_ctrl variable panel drag route ---\n");

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(1000, 1000);
    variable_panel_set_visible(1);
    editor_feed_line("float testvar = 1.0;");

    var_idx = repl_eval_find_predef_var_idx("testvar");
    ASSERT_TRUE("testvar declared", var_idx >= 0);
    ASSERT_FLOAT("testvar starts at 1", g_predef_vars[var_idx].value, 1.0f);

    vp_rect(g_num_predef_vars, &px, &py, &pw, &ph);
    window_h = ui_state_viewport().window_h;
    click_x = px + pw / 2;
    click_y = -1;
    for (int gl_y = py; gl_y < py + ph; gl_y++) {
        int candidate_y = window_h - gl_y;
        hit_row = -1;
        if (vp_hit_row(g_num_predef_vars, click_x, candidate_y, &hit_row)
                && hit_row == var_idx) {
            click_y = candidate_y;
            break;
        }
    }
    ASSERT_TRUE("found click target for testvar row", click_y >= 0);

    ASSERT_INT("drag begin handled",
               glr_ctrl_router_handle_variable_panel_drag_begin(
                   GLUT_LEFT_BUTTON, GLUT_DOWN, click_x, click_y),
               1);
    ASSERT_TRUE("drag active after begin", variable_panel_drag_active());

    for (int step = 1; step <= 10; step++) {
        ASSERT_INT("drag motion handled",
                   glr_ctrl_router_handle_variable_panel_motion(
                       click_x + step * 10, click_y),
                   1);
    }

    editor_undo_ring_state_capture(&undo_state);
    ASSERT_INT("drag motions coalesce to one undo snapshot",
               undo_state.undo_count, 1);
    /* Motion is live-only: the declaration keeps its drag-start text until
     * mouse-up, so a drag performs no source mutation per pointer event. */
    ASSERT_STR("drag leaves declaration source at its start text",
               editor_buffer_line(0), "  static float testvar = 1;");
    ASSERT_FLOAT("drag updates live predef value", g_predef_vars[var_idx].value, 11.0f);

    ASSERT_INT("drag release handled",
               glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP),
               1);
    ASSERT_STR("release rewrites declaration source through compiler",
               editor_buffer_line(0), "  static float testvar = 11;");
    ASSERT_FLOAT("release keeps live predef value",
                 g_predef_vars[var_idx].value, 11.0f);
    editor_undo_ring_state_capture(&undo_state);
    ASSERT_INT("release captures no second undo snapshot",
               undo_state.undo_count, 1);
    ASSERT_TRUE("drag inactive after release", !variable_panel_drag_active());
    ASSERT_INT("undo flag cleared after release",
               variable_panel_drag_undo_snapshot_pushed(), 0);

    editor_undo_pop_snapshot();
    ASSERT_STR("undo restores declaration source",
               editor_buffer_line(0), "  static float testvar = 1;");
    ASSERT_FLOAT("undo restores live predef value", g_predef_vars[var_idx].value, 1.0f);
}

static void test_variable_panel_shift_left_drag_uses_fine_scale(void) {
    int px, py, pw, ph;
    int click_x, click_y;
    int hit_row;
    int window_h;
    int var_idx;

    printf("--- imrepl_ctrl variable panel shift fine drag ---\n");

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(1000, 1000);
    variable_panel_set_visible(1);
    editor_feed_line("float testvar = 1.0;");

    var_idx = repl_eval_find_predef_var_idx("testvar");
    ASSERT_TRUE("fine testvar declared", var_idx >= 0);
    ASSERT_FLOAT("fine testvar starts at 1", g_predef_vars[var_idx].value, 1.0f);

    vp_rect(g_num_predef_vars, &px, &py, &pw, &ph);
    window_h = ui_state_viewport().window_h;
    click_x = px + pw / 2;
    click_y = -1;
    for (int gl_y = py; gl_y < py + ph; gl_y++) {
        int candidate_y = window_h - gl_y;
        hit_row = -1;
        if (vp_hit_row(g_num_predef_vars, click_x, candidate_y, &hit_row)
                && hit_row == var_idx) {
            click_y = candidate_y;
            break;
        }
    }
    ASSERT_TRUE("found click target for fine testvar row", click_y >= 0);

    editor_input_set_modifier_provider_for_test(simulated_mods_provider);
    g_simulated_mods = GLUT_ACTIVE_SHIFT;

    ASSERT_INT("fine drag begin handled",
               glr_ctrl_router_handle_variable_panel_drag_begin(
                   GLUT_LEFT_BUTTON, GLUT_DOWN, click_x, click_y),
               1);
    ASSERT_TRUE("fine drag active after begin", variable_panel_drag_active());

    ASSERT_INT("fine drag motion handled",
               glr_ctrl_router_handle_variable_panel_motion(click_x + 100, click_y),
               1);
    ASSERT_FLOAT("fine drag applies shared fine scale",
                 g_predef_vars[var_idx].value,
                 1.0f + 10.0f * GLR_ADJUST_FINE_SCALE);
    ASSERT_STR("fine drag motion leaves declaration source alone",
               editor_buffer_line(0), "  static float testvar = 1;");

    ASSERT_INT("fine drag release handled",
               glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP),
               1);
    ASSERT_STR("fine drag release rewrites declaration source",
               editor_buffer_line(0), "  static float testvar = 3;");
    ASSERT_TRUE("fine drag inactive after release", !variable_panel_drag_active());

    g_simulated_mods = 0;
    editor_input_set_modifier_provider_for_test(NULL);
}

static void test_variable_panel_motion_preserves_reset_assignment_without_declaration(void) {
    int px, py, pw, ph;
    int click_x, click_y;
    int hit_row;
    int window_h;
    int var_idx;
    EditorUndoRingState undo_state;

    printf("--- imrepl_ctrl variable panel preserves reset assignment ---\n");

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(1000, 1000);
    variable_panel_set_visible(1);
    editor_feed_line("t = 0;");

    var_idx = repl_eval_find_predef_var_idx("t");
    ASSERT_TRUE("t is predefined", var_idx >= 0);
    ASSERT_FLOAT("t starts at reset value", g_predef_vars[var_idx].value, 0.0f);

    vp_rect(g_num_predef_vars, &px, &py, &pw, &ph);
    window_h = ui_state_viewport().window_h;
    click_x = px + pw / 2;
    click_y = -1;
    for (int gl_y = py; gl_y < py + ph; gl_y++) {
        int candidate_y = window_h - gl_y;
        hit_row = -1;
        if (vp_hit_row(g_num_predef_vars, click_x, candidate_y, &hit_row)
                && hit_row == var_idx) {
            click_y = candidate_y;
            break;
        }
    }
    ASSERT_TRUE("found click target for t row", click_y >= 0);

    ASSERT_INT("reset drag begin handled",
               glr_ctrl_router_handle_variable_panel_drag_begin(
                   GLUT_LEFT_BUTTON, GLUT_DOWN, click_x, click_y),
               1);
    ASSERT_TRUE("reset drag active after begin", variable_panel_drag_active());

    ASSERT_INT("reset drag motion handled",
               glr_ctrl_router_handle_variable_panel_motion(click_x + 100, click_y),
               1);

    editor_undo_ring_state_capture(&undo_state);
    ASSERT_INT("reset drag captures one undo snapshot",
               undo_state.undo_count, 1);
    ASSERT_STR("reset assignment source preserved",
               editor_buffer_line(0), "  t = 0;");
    ASSERT_FLOAT("reset drag updates live t value", g_predef_vars[var_idx].value, 10.0f);

    ASSERT_INT("reset drag release handled",
               glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP),
               1);
    ASSERT_TRUE("reset drag inactive after release", !variable_panel_drag_active());

    editor_undo_pop_snapshot();
    ASSERT_STR("undo keeps reset assignment source",
               editor_buffer_line(0), "  t = 0;");
    ASSERT_FLOAT("undo restores live t value", g_predef_vars[var_idx].value, 0.0f);
}

static void test_variable_panel_motion_initializes_uninitialized_declaration(void) {
    int px, py, pw, ph;
    int click_x, click_y;
    int hit_row;
    int window_h;
    int var_idx;
    EditorUndoRingState undo_state;

    printf("--- imrepl_ctrl variable panel initializes decl ---\n");

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(1000, 1000);
    variable_panel_set_visible(1);
    editor_feed_line("float testvar;");

    var_idx = repl_eval_find_predef_var_idx("testvar");
    ASSERT_TRUE("uninitialized testvar declared", var_idx >= 0);
    ASSERT_FLOAT("uninitialized testvar starts at 0",
                 g_predef_vars[var_idx].value, 0.0f);

    vp_rect(g_num_predef_vars, &px, &py, &pw, &ph);
    window_h = ui_state_viewport().window_h;
    click_x = px + pw / 2;
    click_y = -1;
    for (int gl_y = py; gl_y < py + ph; gl_y++) {
        int candidate_y = window_h - gl_y;
        hit_row = -1;
        if (vp_hit_row(g_num_predef_vars, click_x, candidate_y, &hit_row)
                && hit_row == var_idx) {
            click_y = candidate_y;
            break;
        }
    }
    ASSERT_TRUE("found click target for uninitialized testvar row", click_y >= 0);

    ASSERT_INT("uninitialized drag begin handled",
               glr_ctrl_router_handle_variable_panel_drag_begin(
                   GLUT_LEFT_BUTTON, GLUT_DOWN, click_x, click_y),
               1);
    ASSERT_TRUE("uninitialized drag active after begin",
                variable_panel_drag_active());

    for (int step = 1; step <= 10; step++) {
        ASSERT_INT("uninitialized drag motion handled",
                   glr_ctrl_router_handle_variable_panel_motion(
                       click_x + step * 10, click_y),
                   1);
    }

    editor_undo_ring_state_capture(&undo_state);
    ASSERT_INT("uninitialized drag motions coalesce to one undo snapshot",
               undo_state.undo_count, 1);
    ASSERT_STR("uninitialized drag motion leaves the bare declaration",
               editor_buffer_line(0), "  static float testvar;");
    ASSERT_FLOAT("uninitialized drag updates live predef value",
                 g_predef_vars[var_idx].value, 10.0f);

    ASSERT_INT("uninitialized drag release handled",
               glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP),
               1);
    ASSERT_STR("uninitialized drag release adds explicit initializer",
               editor_buffer_line(0), "  static float testvar = 10;");
    ASSERT_TRUE("uninitialized drag inactive after release",
                !variable_panel_drag_active());
    ASSERT_INT("uninitialized undo flag cleared after release",
               variable_panel_drag_undo_snapshot_pushed(), 0);

    editor_undo_pop_snapshot();
    ASSERT_STR("undo restores bare declaration",
               editor_buffer_line(0), "  static float testvar;");
    ASSERT_FLOAT("undo restores live value to zero",
                 g_predef_vars[var_idx].value, 0.0f);
}

/* Find the window-space y that hits `var_idx`'s slider row. */
static int vp_click_y_for_row(int click_x, int var_idx) {
    int px, py, pw, ph;
    int window_h = ui_state_viewport().window_h;
    vp_rect(g_num_predef_vars, &px, &py, &pw, &ph);
    for (int gl_y = py; gl_y < py + ph; gl_y++) {
        int candidate_y = window_h - gl_y;
        int hit_row = -1;
        if (vp_hit_row(g_num_predef_vars, click_x, candidate_y, &hit_row) &&
            hit_row == var_idx)
            return candidate_y;
    }
    return -1;
}

/* The drag transaction split (docs/plans/in-review/
 * flatten-performance-without-vm.md, phase 1B): motion applies the live value
 * only. repl_state_mark_source_dirty() is the single seam every source-derived
 * cache invalidates from — autonormals, the flat program, the source-scope
 * depth cache — so a drag that never trips it during motion is a drag that
 * cannot rebuild those caches per pointer event. Mouse-up trips it exactly
 * once. */
static void test_variable_panel_drag_motion_never_marks_source_dirty(void) {
    int px, py, pw, ph;
    int click_x, click_y;
    int var_idx;

    printf("--- imrepl_ctrl variable panel drag defers source write ---\n");

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(1000, 1000);
    variable_panel_set_visible(1);
    editor_feed_line("float testvar = 1.0;");

    var_idx = repl_eval_find_predef_var_idx("testvar");
    ASSERT_TRUE("deferred-write testvar declared", var_idx >= 0);

    vp_rect(g_num_predef_vars, &px, &py, &pw, &ph);
    click_x = px + pw / 2;
    click_y = vp_click_y_for_row(click_x, var_idx);
    ASSERT_TRUE("found click target for deferred-write row", click_y >= 0);

    ASSERT_INT("deferred-write drag begin handled",
               glr_ctrl_router_handle_variable_panel_drag_begin(
                   GLUT_LEFT_BUTTON, GLUT_DOWN, click_x, click_y),
               1);

    /* 100 motion events, the plan's drag benchmark shape. */
    repl_state_normals_dirty_clear();
    for (int step = 1; step <= 100; step++)
        ASSERT_INT("deferred-write motion handled",
                   glr_ctrl_router_handle_variable_panel_motion(
                       click_x + step, click_y),
                   1);

    ASSERT_INT("100 motions mark the source dirty zero times",
               repl_state_normals_dirty(), 0);
    ASSERT_STR("100 motions leave the declaration text alone",
               editor_buffer_line(0), "  static float testvar = 1;");
    ASSERT_FLOAT("100 motions update the live value",
                 g_predef_vars[var_idx].value, 1.0f + 100.0f * 0.1f);

    ASSERT_INT("deferred-write release handled",
               glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP),
               1);
    ASSERT_INT("release marks the source dirty once",
               repl_state_normals_dirty(), 1);
    ASSERT_STR("release persists the settled value",
               editor_buffer_line(0), "  static float testvar = 11;");
    ASSERT_FLOAT("release keeps the live value",
                 g_predef_vars[var_idx].value, 11.0f);
}

/* Press + release with no motion in between: no compile, no source write, no
 * undo entry. */
static void test_variable_panel_drag_release_without_motion_is_a_noop(void) {
    int px, py, pw, ph;
    int click_x, click_y;
    int var_idx;
    EditorUndoRingState undo_state;

    printf("--- imrepl_ctrl variable panel no-motion release ---\n");

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(1000, 1000);
    variable_panel_set_visible(1);
    editor_feed_line("float testvar = 1.0;");

    var_idx = repl_eval_find_predef_var_idx("testvar");
    ASSERT_TRUE("no-motion testvar declared", var_idx >= 0);

    vp_rect(g_num_predef_vars, &px, &py, &pw, &ph);
    click_x = px + pw / 2;
    click_y = vp_click_y_for_row(click_x, var_idx);
    ASSERT_TRUE("found click target for no-motion row", click_y >= 0);

    ASSERT_INT("no-motion drag begin handled",
               glr_ctrl_router_handle_variable_panel_drag_begin(
                   GLUT_LEFT_BUTTON, GLUT_DOWN, click_x, click_y),
               1);
    repl_state_normals_dirty_clear();

    ASSERT_INT("no-motion release handled",
               glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP),
               1);
    ASSERT_INT("no-motion release does not mark the source dirty",
               repl_state_normals_dirty(), 0);
    ASSERT_STR("no-motion release leaves the declaration text alone",
               editor_buffer_line(0), "  static float testvar = 1;");
    ASSERT_FLOAT("no-motion release leaves the live value alone",
                 g_predef_vars[var_idx].value, 1.0f);
    editor_undo_ring_state_capture(&undo_state);
    ASSERT_INT("no-motion release creates no undo entry",
               undo_state.undo_count, 0);
    ASSERT_TRUE("no-motion drag inactive after release",
                !variable_panel_drag_active());
}

/* A dragged variable with no declaration row (only `t = 0;`) has nothing to
 * persist: release compiles a NO_CHANGE and touches neither source nor undo. */
static void test_variable_panel_drag_release_without_declaration_is_a_noop(void) {
    int px, py, pw, ph;
    int click_x, click_y;
    int var_idx;
    EditorUndoRingState undo_state;

    printf("--- imrepl_ctrl variable panel release without decl ---\n");

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(1000, 1000);
    variable_panel_set_visible(1);
    editor_feed_line("t = 0;");

    var_idx = repl_eval_find_predef_var_idx("t");
    ASSERT_TRUE("no-decl t is predefined", var_idx >= 0);

    vp_rect(g_num_predef_vars, &px, &py, &pw, &ph);
    click_x = px + pw / 2;
    click_y = vp_click_y_for_row(click_x, var_idx);
    ASSERT_TRUE("found click target for no-decl row", click_y >= 0);

    ASSERT_INT("no-decl drag begin handled",
               glr_ctrl_router_handle_variable_panel_drag_begin(
                   GLUT_LEFT_BUTTON, GLUT_DOWN, click_x, click_y),
               1);
    ASSERT_INT("no-decl motion handled",
               glr_ctrl_router_handle_variable_panel_motion(click_x + 100, click_y),
               1);
    repl_state_normals_dirty_clear();
    editor_undo_ring_state_capture(&undo_state);
    ASSERT_INT("no-decl motion captures one undo snapshot",
               undo_state.undo_count, 1);

    ASSERT_INT("no-decl release handled",
               glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP),
               1);
    ASSERT_INT("no-decl release does not mark the source dirty",
               repl_state_normals_dirty(), 0);
    ASSERT_STR("no-decl release preserves the assignment row",
               editor_buffer_line(0), "  t = 0;");
    ASSERT_FLOAT("no-decl release keeps the live value",
                 g_predef_vars[var_idx].value, 10.0f);
    editor_undo_ring_state_capture(&undo_state);
    ASSERT_INT("no-decl release captures no second undo snapshot",
               undo_state.undo_count, 1);
}

/* Audit #18 (Tier B, commit 783d7e3) regression: variable_drag must arrive
 * in UiRenderSnapshot from the controller's snapshot-build phase, not be
 * re-fetched from peer file-statics inside ui_variable_panel_render. Pin
 * that glr_ctrl_build_ui_snapshot copies both fields (active_var, coarse)
 * — a revert to live peer reads would silently still pass every existing
 * variable_drag test but would re-introduce the snapshot-purity violation. */
static void test_variable_drag_snapshot_wiring(void) {
    printf("--- imrepl_ctrl variable_drag snapshot wiring ---\n");

    prepare_display_fixture();
    ASSERT_INT("no drag active before begin",
               variable_panel_drag_active(), 0);

    variable_panel_handle_drag_begin(0, /*coarse=*/1, /*x=*/100);
    ASSERT_INT("drag active after begin",
               variable_panel_drag_active(), 1);
    ASSERT_INT("active var seeded", variable_panel_drag_active_var(), 0);
    ASSERT_INT("coarse mode seeded", variable_panel_drag_coarse(), 1);

    glr_ctrl_display_frame();

    ASSERT_INT("snap.variable_drag.active_var arrives in snapshot",
               g_last_replay_hud_snap.variable_drag.active_var, 0);
    ASSERT_INT("snap.variable_drag.coarse arrives in snapshot",
               g_last_replay_hud_snap.variable_drag.coarse, 1);

    /* Release: the next frame's snapshot must reflect the cleared state
     * (proves the snapshot path is re-evaluated, not stale-cached). */
    variable_panel_handle_drag_reset();
    glr_ctrl_display_frame();
    ASSERT_INT("snap.variable_drag.active_var clears after release",
               g_last_replay_hud_snap.variable_drag.active_var, -1);

    /* Reset for next test. */
    glr_ctrl_reset_all();
}

static int find_variable_panel_row(const UiRenderSnapshot *snap,
                                   const char *name) {
    for (int i = 0; i < snap->variable_panel_vars.count; i++) {
        if (strcmp(snap->variable_panel_vars.vars[i].name, name) == 0)
            return i;
    }
    return -1;
}

static void test_variable_panel_written_snapshot_wiring(void) {
    UiRenderSnapshot snap;
    int radius_row;
    int accum_row;
    int t_row;
    int segs_row;

    printf("--- imrepl_ctrl variable panel written snapshot wiring ---\n");

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(800, 600);
    editor_feed_line("float radius = 1;");
    editor_feed_line("float accum = 0;");
    editor_feed_line("float segs = 8; // @config");
    editor_feed_line("glVertex3f(radius, 0, 0);");
    editor_feed_line("accum = accum + radius;");
    editor_feed_line("segs = min(20, max(segs, 3));");

    glr_ctrl_build_ui_snapshot(&snap);
    radius_row = find_variable_panel_row(&snap, "radius");
    accum_row = find_variable_panel_row(&snap, "accum");
    t_row = find_variable_panel_row(&snap, "t");
    segs_row = find_variable_panel_row(&snap, "segs");

    ASSERT_TRUE("radius row present", radius_row >= 0);
    ASSERT_TRUE("accum row present", accum_row >= 0);
    ASSERT_TRUE("t row present", t_row >= 0);
    ASSERT_TRUE("segs row present", segs_row >= 0);
    if (radius_row >= 0 && accum_row >= 0 && t_row >= 0 && segs_row >= 0) {
        ASSERT_INT("read-only radius is not dimmed",
                   snap.variable_panel_vars.vars[radius_row].written, 0);
        ASSERT_INT("assigned accum is dimmed",
                   snap.variable_panel_vars.vars[accum_row].written, 1);
        ASSERT_INT("runtime time var is not source-written",
                   snap.variable_panel_vars.vars[t_row].written, 0);
        ASSERT_INT("assigned-but-@config segs stays bright",
                   snap.variable_panel_vars.vars[segs_row].written, 0);
    }
}

static void test_pointer_state_tracks_controller_mouse_routes(void) {
    printf("--- imrepl_ctrl pointer state routing ---\n");

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(1000, 1000);

    ASSERT_INT("initial pointer x", ui_state_pointer().mouse_x, 0);
    ASSERT_INT("initial pointer y", ui_state_pointer().mouse_y, 0);
    ASSERT_INT("initial pointer button", ui_state_pointer().mouse_button, -1);

    ASSERT_INT("passive pointer route handled",
               glr_ctrl_router_handle_camera_pointer_set(123, 234), 1);
    ASSERT_INT("passive pointer route x", ui_state_pointer().mouse_x, 123);
    ASSERT_INT("passive pointer route y", ui_state_pointer().mouse_y, 234);
    ASSERT_INT("passive pointer leaves button",
               ui_state_pointer().mouse_button, -1);

    ASSERT_INT("mouse down route handled",
               glr_ctrl_router_handle_camera_mouse(GLUT_LEFT_BUTTON,
                                                   GLUT_DOWN, 345, 456), 1);
    ASSERT_INT("mouse down route x", ui_state_pointer().mouse_x, 345);
    ASSERT_INT("mouse down route y", ui_state_pointer().mouse_y, 456);
    ASSERT_INT("mouse down route button", ui_state_pointer().mouse_button,
               GLUT_LEFT_BUTTON);

    ASSERT_INT("mouse up route handled",
               glr_ctrl_router_handle_camera_mouse(GLUT_LEFT_BUTTON,
                                                   GLUT_UP, 567, 678), 1);
    ASSERT_INT("mouse up route x", ui_state_pointer().mouse_x, 567);
    ASSERT_INT("mouse up route y", ui_state_pointer().mouse_y, 678);
    ASSERT_INT("mouse up route clears button", ui_state_pointer().mouse_button, -1);
}

static void prepare_code_panel_mouse_fixture(void) {
    glr_ctrl_reset_all();
    ui_state_viewport_set_size(800, 600);
    ui_state_code_panel_mut()->panel_frac = 0.45f;
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_menu_bar_close();
    ui_state_help_mut()->visible = 0;
    variable_panel_set_visible(0);

    editor_feed_line("glVertex3f(1, 2, 3);");
    editor_feed_line("glColor3f(0.2, 0.4, 0.6);");
    editor_navigate_to_line(0);
}

static void test_right_click_code_panel_does_not_start_camera_pan(void) {
    UiHit hit;
    int x = -1;
    int y = -1;
    GlrCameraState before;
    GlrCameraState after;

    printf("--- imrepl_ctrl right-click editor blocks camera pan ---\n");

    prepare_code_panel_mouse_fixture();
    ASSERT_TRUE("found code text hit for right-click test",
                find_hit_point_in_code_panel(UI_HIT_CODE_TEXT, &hit, &x, &y));

    glr_camera_set(20.0f, 30.0f, 5.0f, 1.0f, 2.0f, 3.0f, 0.0f);
    glr_camera_controls_reset();
    before = glr_camera();

    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    glr_ctrl_motion(x + 80, y + 40);
    after = glr_camera();

    ASSERT_FLOAT("right-click editor leaves camera rx", after.rx, before.rx);
    ASSERT_FLOAT("right-click editor leaves camera ry", after.ry, before.ry);
    ASSERT_FLOAT("right-click editor leaves camera tx", after.tx, before.tx);
    ASSERT_FLOAT("right-click editor leaves camera ty", after.ty, before.ty);
    ASSERT_FLOAT("right-click editor leaves camera tz", after.tz, before.tz);

    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x + 80, y + 40);
}

/* Right-clicking an assignment row opens its value plot. Before this branch
 * existed the row fell into route_right_code_panel_hit's inert `else` (there
 * is no authored description record for either assignment type), so the
 * gesture did nothing at all. */
static void test_right_click_assignment_opens_value_plot(void) {
    UiHit hit;
    int x = -1;
    int y = -1;

    printf("--- imrepl_ctrl right-click assignment value plot ---\n");

    glr_ctrl_reset_all();
    assign_plot_reset_all();
    ui_state_viewport_set_size(800, 600);
    ui_state_code_panel_mut()->panel_frac = 0.45f;
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_menu_bar_close();
    ui_state_help_mut()->visible = 0;
    variable_panel_set_visible(0);

    editor_feed_line("float angle;");
    editor_feed_line("angle = t * 30;");
    editor_feed_line("glVertex3f(1, 2, 3);");
    editor_navigate_to_line(0);

    ASSERT_TRUE("found the assignment row", find_code_text_hit_for_line(1, &hit, &x, &y));
    ASSERT_INT("plot starts closed", assign_plot_is_open(), 0);

    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    ASSERT_INT("right-click on an assignment opens the plot",
               assign_plot_is_open(), 1);
    ASSERT_INT("targeting the clicked row", assign_plot_source_line(), 1);
    ASSERT_INT("and it closes the description card",
               ui_state_command_description().visible, 0);
    ASSERT_INT("and the state inspector",
               ui_state_gl_state_inspector().visible, 0);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);

    /* The panel title is rebuilt from the live row each frame. */
    {
        UiRenderSnapshot snap;
        glr_ctrl_build_ui_snapshot(&snap);
        ASSERT_STR("title is the row's left-hand side",
                   snap.assign_plot_title, "angle");
        ASSERT_INT("snapshot carries the open flag", snap.assign_plot.open, 1);
    }

    /* Right-clicking the same row again closes it. */
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    ASSERT_INT("right-clicking the same row closes the plot",
               assign_plot_is_open(), 0);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);

    /* A GL command row opens its description card, not a plot. */
    ASSERT_TRUE("found the glVertex3f row",
                find_code_text_hit_for_line(2, &hit, &x, &y));
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    ASSERT_INT("a GL command row opens no plot", assign_plot_is_open(), 0);
    ASSERT_INT("it opens the description instead",
               ui_state_command_description().visible, 1);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
    ui_state_command_description_close();
    assign_plot_reset_all();
}

static void test_right_click_gl_command_description_popup(void) {
    UiCommandDescriptionPanelView view;
    UiHit hit;
    GLCmd probe;
    ReplCommandDescription description;
    int enable_line;
    int x = -1;
    int y = -1;

    printf("--- imrepl_ctrl right-click GL command description ---\n");

    prepare_code_panel_mouse_fixture();
    ASSERT_TRUE("found glVertex3f source row hit",
                find_code_text_hit_for_line(0, &hit, &x, &y));
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    ASSERT_INT("right-click command opens description",
               ui_state_command_description().visible, 1);
    ASSERT_INT("description anchors to clicked source line",
               ui_state_command_description().source_line_idx, 0);
    ASSERT_INT("command description closes state inspector",
               ui_state_gl_state_inspector().visible, 0);

    view = glr_ctrl_build_command_description_panel_view();
    ASSERT_INT("command description view is visible", view.visible, 1);
    ASSERT_STR("command description title carries parameters", view.title,
               "glVertex3f(x, y, z)");
    ASSERT_TRUE("command description body is command-specific",
                view.body && strstr(view.body, "3D vertex") != NULL);

    /* The release completing the opening gesture must not immediately close
     * the popup. A later key event closes it without swallowing the key. */
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
    ASSERT_INT("opening right-button release keeps description",
               ui_state_command_description().visible, 1);
    glr_ctrl_keyboard('x', 0, 0);
    ASSERT_INT("subsequent key dismisses description",
               ui_state_command_description().visible, 0);
    ASSERT_TRUE("dismiss key still reaches editor",
                strchr(editor_input_text(), 'x') != NULL);

    /* A new editor mouse press also dismisses while continuing to route to
     * the underlying row. */
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
    ASSERT_TRUE("found second command row for dismiss click",
                find_code_text_hit_for_line(1, &hit, &x, &y));
    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, x, y);
    ASSERT_INT("subsequent editor click dismisses description",
               ui_state_command_description().visible, 0);
    ASSERT_INT("dismiss click still navigates to clicked line",
               editor_state_edit_line(), 1);
    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_UP, x, y);

    /* glEnable/glDisable bypass the command record and resolve args[0] in
     * the capability catalog. */
    memset(&probe, 0, sizeof(probe));
    probe.type = CMD_ENABLE;
    probe.args[0] = (float)GL_BLEND;
    ASSERT_INT("GL_BLEND capability description resolves",
               repl_command_description_lookup(&probe, &description), 1);
    ASSERT_STR("GL_BLEND capability title", description.title, "GL_BLEND");
    ASSERT_TRUE("GL_BLEND capability body explains blending",
                description.body && strstr(description.body, "blending") != NULL);

    probe.type = CMD_DISABLE;
    probe.args[0] = (float)GL_DEPTH_TEST;
    ASSERT_INT("GL_DEPTH_TEST capability description resolves",
               repl_command_description_lookup(&probe, &description), 1);
    ASSERT_STR("GL_DEPTH_TEST capability title", description.title,
               "GL_DEPTH_TEST");
    ASSERT_TRUE("enable capabilities select different descriptions",
                strstr(description.body, "depth buffer") != NULL);

    probe.type = CMD_VAR_ASSIGN;
    ASSERT_INT("language command has no GL description",
               repl_command_description_lookup(&probe, &description), 0);

    prepare_code_panel_mouse_fixture();
    enable_line = repl_state_document_count();
    editor_state_edit_line_set(enable_line);
    editor_insert_mode_set(0);
    ASSERT_INT("append glEnable line",
               editor_feed_line("glEnable(GL_BLEND);"), 1);
    ASSERT_TRUE("found glEnable source row hit",
                find_code_text_hit_for_line(enable_line, &hit, &x, &y));
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    view = glr_ctrl_build_command_description_panel_view();
    ASSERT_INT("glEnable popup visible", view.visible, 1);
    ASSERT_STR("glEnable popup title includes capability", view.title,
               "glEnable(GL_BLEND)");
    ASSERT_TRUE("glEnable popup body comes from capability",
                view.body && strstr(view.body, "glBlendFunc") != NULL);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);

    glr_ctrl_special(GLUT_KEY_DOWN, 0, 0);
    ASSERT_INT("subsequent special key dismisses description",
               ui_state_command_description().visible, 0);

    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
    glr_ctrl_mouse(4, GLUT_DOWN, x, y);
    ASSERT_INT("subsequent editor wheel event dismisses description",
               ui_state_command_description().visible, 0);
}

static void test_right_click_empty_line_toggles_gl_state_report(void) {
    UiHit hit;
    UiGlStatePanelView view;
    int blank_line;
    int x = -1;
    int y = -1;
    int found_color = 0;
    int found_init_state = 0;
    int found_display_color = 0;
    int found_global_light_ambient = 0;
    int found_light_diffuse = 0;
    int found_light_position = 0;
    int found_world_light_position = 0;
    int found_attrib_stack = 0;
    int i;

    printf("--- imrepl_ctrl right-click blank OpenGL state report ---\n");

    prepare_code_panel_mouse_fixture();
    /* The report only carries a light's parameter rows while that light is
     * enabled, so switch one on before asserting the generated light values
     * reach the popup. */
    editor_state_edit_line_set(repl_state_document_count());
    ASSERT_INT("append light enable for the report fixture",
               editor_feed_line("glEnable(GL_LIGHT0);"), 1);
    blank_line = repl_state_document_count();
    editor_state_edit_line_set(blank_line);
    editor_insert_mode_set(0);
    ASSERT_INT("append committed blank line", editor_feed_line(""), 1);
    ASSERT_INT("blank command type",
               repl_state_document_cmd_at(blank_line)->type, CMD_EMPTY);
    ASSERT_TRUE("found empty source row hit",
                find_code_text_hit_for_line(blank_line, &hit, &x, &y));

    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    ASSERT_INT("right-click blank opens state report",
               ui_state_gl_state_inspector().visible, 1);
    ASSERT_INT("state report anchored to clicked line",
               ui_state_gl_state_inspector().source_line_idx, blank_line);
    ASSERT_INT("popup anchor keeps click x",
               ui_state_gl_state_inspector().anchor_px, x);
    ASSERT_INT("popup anchor keeps click y",
               ui_state_gl_state_inspector().anchor_py, y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);

#ifdef GL_STUBS
    glr_ctrl_display_frame();
#else
    if (repl_state_normals_dirty()) {
        int edit_line = editor_state_edit_line();
        repl_recompute_autonormals(glr_state_presentation().autonormal,
                                   &edit_line);
        editor_state_edit_line_set(edit_line);
        repl_state_normals_dirty_clear();
    }
    if (repl_state_flat_program_dirty()) {
        repl_flatten_commands(editor_state_edit_line());
        repl_state_flat_program_clear_dirty();
    }
#endif
    view = glr_ctrl_build_gl_state_panel_view(NULL);
    ASSERT_INT("popup view visible for valid anchor", view.visible, 1);
    ASSERT_TRUE("popup view carries a report", view.report != NULL);
    ASSERT_TRUE("popup report has touched state",
                view.report && view.report->count > 0);
    for (i = 0; view.report && i < view.report->count; i++) {
        const ReplGlStateReportRow *row = &view.report->rows[i];
        if (row->source.kind == REPL_GL_STATE_SOURCE_INIT)
            found_init_state = 1;
        if (strcmp(row->name, "GL_CURRENT_COLOR") == 0) {
            found_color = 1;
            if (row->source.kind == REPL_GL_STATE_SOURCE_DISPLAY)
                found_display_color = 1;
        }
        if (strcmp(row->name, "GL_LIGHT_MODEL_AMBIENT") == 0 &&
            row->source.kind == REPL_GL_STATE_SOURCE_INIT)
            found_global_light_ambient = 1;
        if (strcmp(row->name, "GL_LIGHT0_DIFFUSE") == 0 &&
            row->source.kind == REPL_GL_STATE_SOURCE_INIT)
            found_light_diffuse = 1;
        if (strcmp(row->name, "GL_LIGHT0_POSITION (eye)") == 0 &&
            row->source.kind == REPL_GL_STATE_SOURCE_DISPLAY)
            found_light_position = 1;
        if (strcmp(row->name, "GL_LIGHT0_POSITION (world)") == 0 &&
            row->source.kind == REPL_GL_STATE_SOURCE_DISPLAY)
            found_world_light_position = 1;
        if (strcmp(row->name, "GL_ATTRIB_STACK_DEPTH") == 0 &&
            row->source.kind == REPL_GL_STATE_SOURCE_DISPLAY)
            found_attrib_stack = 1;
    }
    ASSERT_TRUE("popup report includes init state", found_init_state);
    ASSERT_TRUE("popup report includes touched color state", found_color);
    ASSERT_TRUE("popup distinguishes display color source", found_display_color);
    ASSERT_TRUE("popup includes init global light ambient",
                found_global_light_ambient);
    ASSERT_TRUE("popup includes init light color", found_light_diffuse);
    ASSERT_TRUE("popup includes display light position",
                found_light_position);
    ASSERT_TRUE("popup includes display world light position",
                found_world_light_position);
    ASSERT_TRUE("popup includes display attribute stack",
                found_attrib_stack);

    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    ASSERT_INT("second right-click on anchor closes report",
               ui_state_gl_state_inspector().visible, 0);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
    view = glr_ctrl_build_gl_state_panel_view(NULL);
    ASSERT_INT("popup view hidden after close", view.visible, 0);

    /* Left-click routing: a click on the popup surface is swallowed (the
     * popup stays open); a click anywhere off it dismisses the popup. */
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
    ASSERT_INT("third right-click reopens report",
               ui_state_gl_state_inspector().visible, 1);
    view = glr_ctrl_build_gl_state_panel_view(NULL);
    {
        int in_x = -1, in_y = -1, out_x = -1, out_y = -1;
        int win_w = ui_state_viewport().window_w;
        int win_h = ui_state_viewport().window_h;
        int px, py;
        for (py = 4; py < win_h - 4 && in_x < 0; py += 8) {
            for (px = 4; px < win_w - 4 && in_x < 0; px += 8) {
                if (ui_gl_state_panel_hit_test(&view, px, py)) {
                    in_x = px;
                    in_y = py;
                }
            }
        }
        /* Off-popup point kept inside the code panel and below the
         * menu-bar / tab band so the fall-through click stays inert. */
        for (py = 80; py < win_h - 4 && out_x < 0; py += 8) {
            for (px = 4; px < win_w - 4 && out_x < 0; px += 8) {
                if (!ui_gl_state_panel_hit_test(&view, px, py) &&
                    editor_input_point_in_code_panel(px, py)) {
                    out_x = px;
                    out_y = py;
                }
            }
        }
        ASSERT_TRUE("found a point on the popup", in_x >= 0);
        ASSERT_TRUE("found a point off the popup", out_x >= 0);

        glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, in_x, in_y);
        ASSERT_INT("left-click on popup keeps it open",
                   ui_state_gl_state_inspector().visible, 1);
        glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_UP, in_x, in_y);

        glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, out_x, out_y);
        ASSERT_INT("left-click away dismisses popup",
                   ui_state_gl_state_inspector().visible, 0);
        glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_UP, out_x, out_y);
    }

    ASSERT_TRUE("found non-empty source row hit",
                find_code_text_hit_for_line(0, &hit, &x, &y));
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    ASSERT_INT("right-click non-empty line does not open report",
               ui_state_gl_state_inspector().visible, 0);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
}

/* The resize cursor must promise exactly what a press delivers. A panel
 * painted in front of the divider owns the pixel — the OpenGL-state popup is
 * classified ahead of the canonical hit-test in mouse_dispatch, so a press
 * there never reaches the divider — and the hover treatment, which the editor
 * derives from divider geometry alone, has to yield to it. */
static void test_divider_hover_yields_to_front_panel(void) {
    UiHit hit;
    UiGlStatePanelView view;
    int x = -1, y = -1;
    int cp_x, cp_y, cp_w, cp_h;
    int win_h, div_x, my;
    int covered_my = -1;
    int clear_my = -1;

    printf("--- imrepl_ctrl divider hover yields to a front panel ---\n");

    /* The report opens on a blank row only, so anchor it on the trailing
     * uncommitted one (same fixture as the live-row report test). */
    prepare_code_panel_mouse_fixture();
    editor_navigate_to_line(repl_state_document_count());
    ASSERT_TRUE("found a blank row to anchor the state popup",
                find_hit_point_in_code_panel(UI_HIT_CODE_INSERT_LINE,
                                             &hit, &x, &y));
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
    ASSERT_INT("state popup open over the panel",
               ui_state_gl_state_inspector().visible, 1);
#ifdef GL_STUBS
    glr_ctrl_display_frame();
#endif

    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    win_h = ui_state_viewport().window_h;
    div_x = cp_x + cp_w;   /* LEFT layout: the divider is this column */
    view = glr_ctrl_build_gl_state_panel_view(NULL);
    ASSERT_INT("popup view visible for the hover test", view.visible, 1);

    /* One divider pixel the popup covers and one it doesn't. The clear one
     * is confirmed against the canonical hit-test so the comparison is
     * "front panel or not", not "some other surface owns it anyway". */
    for (my = 0; my < win_h && (covered_my < 0 || clear_my < 0); my++) {
        if (ui_gl_state_panel_hit_test(&view, div_x, my)) {
            if (covered_my < 0)
                covered_my = my;
        } else if (clear_my < 0) {
            UiRenderSnapshot snap;
            glr_ctrl_build_ui_snapshot(&snap);
            if (ui_panels_hit_test(&snap, div_x, my,
                                   repl_eval_predef_view().count).kind
                    == UI_HIT_PANEL_DIVIDER)
                clear_my = my;
        }
    }
    ASSERT_TRUE("popup covers part of the divider column", covered_my >= 0);
    ASSERT_TRUE("divider column has an uncovered stretch", clear_my >= 0);

    g_last_set_cursor = -1;
    glr_ctrl_passive_motion(div_x, clear_my);
    ASSERT_INT("resize cursor on the open divider",
               g_last_set_cursor, GLUT_CURSOR_LEFT_RIGHT);

    g_last_set_cursor = -1;
    glr_ctrl_passive_motion(div_x, covered_my);
    ASSERT_INT("no resize cursor where a panel covers the divider",
               g_last_set_cursor, GLUT_CURSOR_INHERIT);

    /* ...and that is what the press does, too. */
    ui_state_code_panel_mut()->resizing_panel = 0;
    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, div_x, covered_my);
    ASSERT_INT("press on the covering panel starts no resize",
               ui_state_code_panel().resizing_panel, 0);
    ASSERT_INT("press on the covering panel keeps it open",
               ui_state_gl_state_inspector().visible, 1);
    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_UP, div_x, covered_my);

    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, div_x, clear_my);
    ASSERT_INT("press on the open divider starts the resize",
               ui_state_code_panel().resizing_panel, 1);
    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_UP, div_x, clear_my);
    ASSERT_INT("release ends the resize",
               ui_state_code_panel().resizing_panel, 0);
}

static void test_right_click_uncommitted_empty_line_opens_gl_state_report(void) {
    UiHit hit;
    UiGlStatePanelView view;
    int document_count;
    int x = -1;
    int y = -1;

    printf("--- imrepl_ctrl right-click uncommitted OpenGL state row ---\n");

    prepare_code_panel_mouse_fixture();
    document_count = repl_state_document_count();
    editor_navigate_to_line(document_count);
    ASSERT_STR("trailing editor row is blank but uncommitted",
               editor_input_text(), "");
    ASSERT_INT("blank live row does not change document count",
               repl_state_document_count(), document_count);
    ASSERT_TRUE("found uncommitted input-row hit",
                find_hit_point_in_code_panel(UI_HIT_CODE_INSERT_LINE,
                                             &hit, &x, &y));
    ASSERT_INT("uncommitted hit carries its state boundary",
               hit.line_idx, document_count);

    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    ASSERT_INT("right-click blank live row opens state report",
               ui_state_gl_state_inspector().visible, 1);
    ASSERT_INT("live-row report anchors before pending line",
               ui_state_gl_state_inspector().source_line_idx,
               document_count);
    ASSERT_INT("opening report does not commit live row",
               repl_state_document_count(), document_count);
    view = glr_ctrl_build_gl_state_panel_view(NULL);
    ASSERT_INT("live-row state report view remains visible", view.visible, 1);
    ASSERT_TRUE("live-row report is built", view.report != NULL);
    ASSERT_INT("live-row report uses pending-line boundary",
               view.report ? view.report->source_line_idx : -1,
               document_count);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);

    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    ASSERT_INT("second right-click toggles live-row report closed",
               ui_state_gl_state_inspector().visible, 0);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);

    editor_input_set_text("glVertex3f(9, 9, 9)");
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    ASSERT_INT("nonblank uncommitted row does not open state report",
               ui_state_gl_state_inspector().visible, 0);
    ASSERT_INT("right-click never commits pending input",
               repl_state_document_count(), document_count);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
}

/* Pure popup-geometry checks: a report taller than the window solves to a
 * scrollable row window, and the hit-test frame matches the solved rect. */
static void test_gl_state_popup_scroll_geometry(void) {
    static ReplGlStateReport report;
    UiGlStatePanelView view;
    int max_scroll, matrix_max_scroll;
    int i;

    printf("--- imrepl_ctrl OpenGL-state popup scroll geometry ---\n");

    prepare_code_panel_mouse_fixture();
    memset(&report, 0, sizeof(report));
    report.count = REPL_GL_STATE_REPORT_MAX_ROWS;
    report.user_row_count = report.count;
    for (i = 0; i < report.count; i++) {
        snprintf(report.rows[i].name, sizeof(report.rows[i].name),
                 "GL_ROW_%02d", i);
        snprintf(report.rows[i].current, sizeof(report.rows[i].current), "1");
        snprintf(report.rows[i].default_value,
                 sizeof(report.rows[i].default_value), "0");
        report.rows[i].differs_from_default = 1;
    }

    memset(&view, 0, sizeof(view));
    view.visible = 1;
    view.window_w = 800;
    view.window_h = 300;
    view.anchor_px = 100;
    view.anchor_py = 150;
    view.report = &report;

    max_scroll = ui_gl_state_panel_max_scroll(&view);
    ASSERT_TRUE("overflowing report is scrollable", max_scroll > 0);
    ASSERT_TRUE("some rows stay visible at full scroll",
                max_scroll < report.count);

    /* A matrix at the end consumes four visual lines, leaving room for three
     * fewer scalar report rows in the final semantic-row viewport. */
    snprintf(report.rows[report.count - 1].name,
             sizeof(report.rows[report.count - 1].name),
             "GL_MODELVIEW_MATRIX");
    snprintf(report.rows[report.count - 1].current,
             sizeof(report.rows[report.count - 1].current),
             "[1 0 0 0; 0 1 0 0; 0 0 1 0; 0 0 0 1]");
    snprintf(report.rows[report.count - 1].default_value,
             sizeof(report.rows[report.count - 1].default_value),
             "[1 0 0 0; 0 1 0 0; 0 0 1 0; 0 0 0 1]");
    matrix_max_scroll = ui_gl_state_panel_max_scroll(&view);
    ASSERT_INT("matrix visual lines reduce the final row viewport",
               matrix_max_scroll - max_scroll, 3);
    max_scroll = matrix_max_scroll;

    /* The popup frame answers hits; far-off points do not. Scrolling to
     * the end must not move the solved frame. */
    ASSERT_TRUE("point on popup frame hits",
                ui_gl_state_panel_hit_test(&view, 130, 150));
    ASSERT_TRUE("far point misses popup",
                !ui_gl_state_panel_hit_test(&view, 780, 150));
    view.scroll_rows = max_scroll;
    ASSERT_TRUE("scrolled popup keeps its frame",
                ui_gl_state_panel_hit_test(&view, 130, 150));

    view.visible = 0;
    ASSERT_TRUE("hidden view never hits",
                !ui_gl_state_panel_hit_test(&view, 130, 150));
    ASSERT_INT("hidden view has no scroll range",
               ui_gl_state_panel_max_scroll(&view), 0);
}

static int gl_state_popup_hit_height(const UiGlStatePanelView *view) {
    int mx = view->anchor_px + 20;
    int first = -1, last = -1;
    int my;
    for (my = 0; my < view->window_h; my++) {
        if (!ui_gl_state_panel_hit_test(view, mx, my))
            continue;
        if (first < 0)
            first = my;
        last = my;
    }
    return first < 0 ? 0 : last - first;
}

/* The modelview state remains one report row, but its current/default values
 * consume four visual lines. Popup geometry and rendering must both account
 * for the three additional lines. */
static void test_gl_state_popup_modelview_uses_four_lines(void) {
    static ReplGlStateReport report;
    UiGlStatePanelView view;
    int scalar_height, matrix_height;
#ifdef GL_STUBS
    int scalar_draws, matrix_draws;
#endif

    printf("--- imrepl_ctrl OpenGL-state matrix visual rows ---\n");

    memset(&report, 0, sizeof(report));
    report.count = 1;
    report.user_row_count = report.count;
    snprintf(report.rows[0].name, sizeof(report.rows[0].name),
             "GL_CURRENT_COLOR");
    snprintf(report.rows[0].current, sizeof(report.rows[0].current),
             "(0.25, 0.5, 0.75, 1)");
    snprintf(report.rows[0].default_value,
             sizeof(report.rows[0].default_value), "(1, 1, 1, 1)");
    report.rows[0].source.kind = REPL_GL_STATE_SOURCE_DISPLAY;

    memset(&view, 0, sizeof(view));
    view.visible = 1;
    view.window_w = 800;
    view.window_h = 600;
    view.anchor_px = 40;
    view.anchor_py = 500;
    view.report = &report;

    scalar_height = gl_state_popup_hit_height(&view);
#ifdef GL_STUBS
    gl_stub_counts_reset();
    ui_gl_state_panel_render(&view);
    scalar_draws = (int)gl_stub_counts[GL_STUB_glRasterPos2f];
#endif

    snprintf(report.rows[0].name, sizeof(report.rows[0].name),
             "GL_MODELVIEW_MATRIX");
    snprintf(report.rows[0].current, sizeof(report.rows[0].current),
             "[0.866025 -0.5 0 1.25; 0.5 0.866025 0 -2.5; "
             "0 0 1 3.75; 0 0 0 1]");
    snprintf(report.rows[0].default_value,
             sizeof(report.rows[0].default_value),
             "[1 0 0 0; 0 1 0 0; 0 0 1 0; 0 0 0 1]");

    matrix_height = gl_state_popup_hit_height(&view);
    ASSERT_INT("matrix adds three visual lines to popup geometry",
               matrix_height - scalar_height, 3 * LINE_H);
#ifdef GL_STUBS
    gl_stub_counts_reset();
    ui_gl_state_panel_render(&view);
    matrix_draws = (int)gl_stub_counts[GL_STUB_glRasterPos2f];
    ASSERT_INT("collapsed matrix draws four current-value lines",
               matrix_draws - scalar_draws, 3);

    /* Expanded details draw the four default rows beside the four current
     * rows while the state name/source remain single-line metadata. */
    view.details_expanded = 1;
    snprintf(report.rows[0].name, sizeof(report.rows[0].name),
             "GL_CURRENT_COLOR");
    gl_stub_counts_reset();
    ui_gl_state_panel_render(&view);
    scalar_draws = (int)gl_stub_counts[GL_STUB_glRasterPos2f];
    snprintf(report.rows[0].name, sizeof(report.rows[0].name),
             "GL_MODELVIEW_MATRIX");
    gl_stub_counts_reset();
    ui_gl_state_panel_render(&view);
    matrix_draws = (int)gl_stub_counts[GL_STUB_glRasterPos2f];
    ASSERT_INT("expanded matrix aligns four current and default lines",
               matrix_draws - scalar_draws, 6);
#endif
}

/* Rightmost x (GLUT coords) answering the popup's surface hit-test; -1
 * when nothing hits. Coarse scan is fine — we only compare widths. */
static int gl_state_popup_rightmost_hit_x(const UiGlStatePanelView *view) {
    int mx, my, right = -1;
    for (my = 0; my < view->window_h; my += 6) {
        for (mx = view->window_w - 1; mx > right; mx -= 2) {
            if (ui_gl_state_panel_hit_test(view, mx, my)) {
                right = mx;
                break;
            }
        }
    }
    return right;
}

static int gl_state_popup_find_details_toggle(const UiGlStatePanelView *view,
                                              int *out_x, int *out_y) {
    int mx, my;
    for (my = 0; my < view->window_h; my += 3) {
        for (mx = 0; mx < view->window_w; mx += 3) {
            if (ui_gl_state_panel_hit_test_details_toggle(view, mx, my)) {
                *out_x = mx;
                *out_y = my;
                return 1;
            }
        }
    }
    return 0;
}

/* Popup height, probed through the public hit-test so the measurement is the
 * frame the router actually routes against. */
static int gl_state_popup_solved_height(const UiGlStatePanelView *view) {
    int mx, my, top = -1, bottom = -1;
    for (my = 0; my < view->window_h; my++) {
        int hit = 0;
        for (mx = 0; mx < view->window_w && !hit; mx += 3)
            hit = ui_gl_state_panel_hit_test(view, mx, my);
        if (hit) {
            if (top < 0)
                top = my;
            bottom = my;
        }
    }
    return top < 0 ? 0 : bottom - top + 1;
}

static int gl_state_popup_find_setup_toggle(const UiGlStatePanelView *view,
                                            int *out_x, int *out_y) {
    int mx, my;
    for (my = 0; my < view->window_h; my += 3) {
        for (mx = 0; mx < view->window_w; mx += 3) {
            if (ui_gl_state_panel_hit_test_setup_toggle(view, mx, my)) {
                *out_x = mx;
                *out_y = my;
                return 1;
            }
        }
    }
    return 0;
}

/* The default/source detail columns are collapsed by default and toggle
 * from the header's "[+]"/"[-]" chip: collapsed the popup solves
 * narrower than expanded, and a left press on the chip flips the
 * inspector chrome while keeping the popup open. */
static void test_gl_state_popup_details_toggle(void) {
    static ReplGlStateReport report;
    UiGlStatePanelView view;
    UiHit hit;
    int blank_line;
    int x = -1, y = -1;
    int collapsed_right, expanded_right;
    int i;

    printf("--- imrepl_ctrl OpenGL-state popup detail-column toggle ---\n");

    /* Pure geometry: the same report solves narrower collapsed. */
    memset(&report, 0, sizeof(report));
    report.count = 4;
    report.user_row_count = report.count;
    for (i = 0; i < report.count; i++) {
        snprintf(report.rows[i].name, sizeof(report.rows[i].name),
                 "GL_ROW_%02d", i);
        snprintf(report.rows[i].current, sizeof(report.rows[i].current), "1");
        snprintf(report.rows[i].default_value,
                 sizeof(report.rows[i].default_value),
                 "(0.123, 0.456, 0.789, 1)");
        report.rows[i].source.kind = REPL_GL_STATE_SOURCE_DISPLAY;
        report.rows[i].source.source_line_idx = 10 + i;
    }
    memset(&view, 0, sizeof(view));
    view.visible = 1;
    view.window_w = 800;
    view.window_h = 600;
    view.anchor_px = 40;
    view.anchor_py = 400;
    view.report = &report;

    collapsed_right = gl_state_popup_rightmost_hit_x(&view);
    ASSERT_TRUE("collapsed popup answers hits", collapsed_right >= 0);
    ASSERT_TRUE("collapsed popup has a toggle chip",
                gl_state_popup_find_details_toggle(&view, &x, &y));
    view.details_expanded = 1;
    expanded_right = gl_state_popup_rightmost_hit_x(&view);
    ASSERT_TRUE("expanded popup is wider than collapsed",
                expanded_right > collapsed_right);
    ASSERT_TRUE("expanded popup keeps a toggle chip",
                gl_state_popup_find_details_toggle(&view, &x, &y));

    /* Routed: right-click a blank line, click the chip twice. */
    prepare_code_panel_mouse_fixture();
    blank_line = repl_state_document_count();
    editor_state_edit_line_set(blank_line);
    editor_insert_mode_set(0);
    ASSERT_INT("append committed blank line", editor_feed_line(""), 1);
    ASSERT_TRUE("found empty source row hit",
                find_code_text_hit_for_line(blank_line, &hit, &x, &y));
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
    ASSERT_INT("right-click opens the popup",
               ui_state_gl_state_inspector().visible, 1);
    ASSERT_INT("popup opens with details collapsed",
               ui_state_gl_state_inspector().details_expanded, 0);

#ifdef GL_STUBS
    glr_ctrl_display_frame();
#else
    if (repl_state_normals_dirty()) {
        int edit_line = editor_state_edit_line();
        repl_recompute_autonormals(glr_state_presentation().autonormal,
                                   &edit_line);
        editor_state_edit_line_set(edit_line);
        repl_state_normals_dirty_clear();
    }
    if (repl_state_flat_program_dirty()) {
        repl_flatten_commands(editor_state_edit_line());
        repl_state_flat_program_clear_dirty();
    }
#endif

    view = glr_ctrl_build_gl_state_panel_view(NULL);
    ASSERT_INT("popup view visible", view.visible, 1);
    ASSERT_TRUE("found the live popup's toggle chip",
                gl_state_popup_find_details_toggle(&view, &x, &y));
    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, x, y);
    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_UP, x, y);
    ASSERT_INT("chip click expands the detail columns",
               ui_state_gl_state_inspector().details_expanded, 1);
    ASSERT_INT("chip click keeps the popup open",
               ui_state_gl_state_inspector().visible, 1);

    view = glr_ctrl_build_gl_state_panel_view(NULL);
    ASSERT_TRUE("expanded live popup keeps a toggle chip",
                gl_state_popup_find_details_toggle(&view, &x, &y));
    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, x, y);
    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_UP, x, y);
    ASSERT_INT("second chip click collapses the detail columns",
               ui_state_gl_state_inspector().details_expanded, 0);
    ASSERT_INT("second chip click keeps the popup open",
               ui_state_gl_state_inspector().visible, 1);

    ui_state_gl_state_inspector_close();
}

static void test_editor_input_dismisses_gl_state_report(void) {
    UiHit hit;
    UiGlStatePanelView view;
    int blank_line;
    int old_cpu_profile;
    int anchor_x = -1;
    int anchor_y = -1;
    int off_x = -1;
    int off_y = -1;
    int px, py;

    printf("--- imrepl_ctrl editor input dismisses OpenGL state ---\n");

    prepare_code_panel_mouse_fixture();
    blank_line = repl_state_document_count();
    editor_state_edit_line_set(blank_line);
    editor_insert_mode_set(0);
    ASSERT_INT("append editor-dismiss blank line", editor_feed_line(""), 1);
    ASSERT_TRUE("find editor-dismiss blank line",
                find_code_text_hit_for_line(blank_line, &hit,
                                            &anchor_x, &anchor_y));

    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, anchor_x, anchor_y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, anchor_x, anchor_y);
    ASSERT_INT("state open before editor key",
               ui_state_gl_state_inspector().visible, 1);
    glr_ctrl_keyboard('x', 0, 0);
    ASSERT_INT("ordinary editor key dismisses state",
               ui_state_gl_state_inspector().visible, 0);

    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, anchor_x, anchor_y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, anchor_x, anchor_y);
    ASSERT_INT("state reopens before editor special key",
               ui_state_gl_state_inspector().visible, 1);
    glr_ctrl_special(GLUT_KEY_LEFT, 0, 0);
    ASSERT_INT("editor special key dismisses state",
               ui_state_gl_state_inspector().visible, 0);

    /* Cocoa FreeGLUT emits Ctrl itself as a special-key transition before
     * delivering Ctrl+W through the keyboard callback. The controller must
     * discard that transition: neither it nor the controller-owned shortcut
     * that follows is editor input, so both must leave the inspector open. */
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, anchor_x, anchor_y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, anchor_x, anchor_y);
    ASSERT_INT("state reopens before modifier-only special keys",
               ui_state_gl_state_inspector().visible, 1);
#ifdef GLUT_KEY_SHIFT_L
    {
        static const int modifier_keys[] = {
            GLUT_KEY_SHIFT_L, GLUT_KEY_SHIFT_R,
            GLUT_KEY_CTRL_L, GLUT_KEY_CTRL_R,
            GLUT_KEY_ALT_L, GLUT_KEY_ALT_R,
            GLUT_KEY_SUPER_L, GLUT_KEY_SUPER_R
        };
        int i;
        for (i = 0; i < (int)(sizeof(modifier_keys) /
                              sizeof(modifier_keys[0])); i++)
            glr_ctrl_special(modifier_keys[i], 0, 0);
    }
#endif
    ASSERT_INT("modifier-only special keys preserve state",
               ui_state_gl_state_inspector().visible, 1);

    editor_input_set_modifier_provider_for_test(simulated_mods_provider);
    g_simulated_mods = GLUT_ACTIVE_CTRL;
    old_cpu_profile = glr_config_get(GLR_CONFIG_CPU_PROFILE);
    glr_ctrl_keyboard(KEY_CTRL_W, 0, 0);
    ASSERT_TRUE("Ctrl+W remains a controller-owned profile shortcut",
                glr_config_get(GLR_CONFIG_CPU_PROFILE) != old_cpu_profile);
    ASSERT_INT("controller-owned Ctrl+W preserves state",
               ui_state_gl_state_inspector().visible, 1);
    g_simulated_mods = 0;
    editor_input_set_modifier_provider_for_test(NULL);

    glr_ctrl_special(GLUT_KEY_LEFT, 0, 0);
    ASSERT_INT("later editor special key still dismisses state",
               ui_state_gl_state_inspector().visible, 0);

    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, anchor_x, anchor_y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, anchor_x, anchor_y);
#ifdef GL_STUBS
    /* Real-GL test binaries have no GL context; rendering a frame with
     * the popup visible would call into libGL (ui_gl_state_panel_render)
     * and crash. The stub build exercises the full frame. */
    glr_ctrl_display_frame();
#else
    if (repl_state_normals_dirty()) {
        int edit_line = editor_state_edit_line();
        repl_recompute_autonormals(glr_state_presentation().autonormal,
                                   &edit_line);
        editor_state_edit_line_set(edit_line);
        repl_state_normals_dirty_clear();
    }
    if (repl_state_flat_program_dirty()) {
        repl_flatten_commands(editor_state_edit_line());
        repl_state_flat_program_clear_dirty();
    }
#endif
    view = glr_ctrl_build_gl_state_panel_view(NULL);
    for (py = 80; py < ui_state_viewport().window_h - 4 && off_x < 0;
         py += 4) {
        for (px = 4; px < ui_state_viewport().window_w - 4 && off_x < 0;
             px += 4) {
            if (editor_input_point_in_code_panel(px, py) &&
                !ui_gl_state_panel_hit_test(&view, px, py)) {
                off_x = px;
                off_y = py;
            }
        }
    }
    ASSERT_TRUE("find code-panel wheel point outside state", off_x >= 0);
    if (off_x >= 0) {
        route_wheel(off_x, off_y, 1);
        ASSERT_INT("editor code-panel wheel dismisses state",
                   ui_state_gl_state_inspector().visible, 0);
    }
}

/* The generated setup rows are folded away by default, so the popup opens on
 * what the program itself wrote. The title-row chip unfolds them: the report
 * partition means the shown set is always the rows[0, user_row_count) prefix,
 * so folding is a row-count change, never a filter. */
static void test_gl_state_popup_setup_fold(void) {
    static ReplGlStateReport report;
    UiGlStatePanelView view;
    UiHit hit;
    int blank_line;
    int x = -1, y = -1;
    int folded_h, unfolded_h;
    int i;

    printf("--- imrepl_ctrl OpenGL-state popup setup fold ---\n");

    /* Two authored rows in front of six generated ones. */
    memset(&report, 0, sizeof(report));
    report.count = 8;
    report.user_row_count = 2;
    for (i = 0; i < report.count; i++) {
        snprintf(report.rows[i].name, sizeof(report.rows[i].name),
                 "GL_ROW_%02d", i);
        snprintf(report.rows[i].current, sizeof(report.rows[i].current), "1");
        snprintf(report.rows[i].default_value,
                 sizeof(report.rows[i].default_value), "0");
        report.rows[i].differs_from_default = 1;
        report.rows[i].source.kind = i < report.user_row_count
            ? REPL_GL_STATE_SOURCE_DISPLAY : REPL_GL_STATE_SOURCE_INIT;
        report.rows[i].source.source_line_idx =
            i < report.user_row_count ? 10 + i : -1;
    }
    memset(&view, 0, sizeof(view));
    view.visible = 1;
    view.window_w = 800;
    view.window_h = 600;
    view.anchor_px = 40;
    view.anchor_py = 400;
    view.report = &report;

    folded_h = gl_state_popup_solved_height(&view);
    view.setup_expanded = 1;
    unfolded_h = gl_state_popup_solved_height(&view);
    ASSERT_TRUE("folded popup is shorter than unfolded",
                folded_h > 0 && unfolded_h > folded_h);
    view.setup_expanded = 0;

    ASSERT_TRUE("folded popup offers the setup chip",
                gl_state_popup_find_setup_toggle(&view, &x, &y));
    view.setup_expanded = 1;
    ASSERT_TRUE("unfolded popup keeps the setup chip",
                gl_state_popup_find_setup_toggle(&view, &x, &y));
    view.setup_expanded = 0;

    /* A report with nothing generated has nothing to fold, so no chip. */
    report.user_row_count = report.count;
    ASSERT_TRUE("all-authored report has no setup chip",
                !gl_state_popup_find_setup_toggle(&view, &x, &y));
    report.user_row_count = 2;

    /* Routed: right-click a blank line, then click the chip. */
    prepare_code_panel_mouse_fixture();
    blank_line = repl_state_document_count();
    editor_state_edit_line_set(blank_line);
    editor_insert_mode_set(0);
    ASSERT_INT("append committed blank line", editor_feed_line(""), 1);
    ASSERT_TRUE("found empty source row hit",
                find_code_text_hit_for_line(blank_line, &hit, &x, &y));
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
    ASSERT_INT("right-click opens the popup",
               ui_state_gl_state_inspector().visible, 1);
    ASSERT_INT("popup opens with setup folded",
               ui_state_gl_state_inspector().setup_expanded, 0);

    view = glr_ctrl_build_gl_state_panel_view(NULL);
    ASSERT_TRUE("live report has generated rows to fold",
                view.report &&
                view.report->count > view.report->user_row_count);
    if (gl_state_popup_find_setup_toggle(&view, &x, &y)) {
        glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, x, y);
        glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_UP, x, y);
        ASSERT_INT("chip click unfolds the setup rows",
                   ui_state_gl_state_inspector().setup_expanded, 1);
        ASSERT_INT("chip click keeps the popup open",
                   ui_state_gl_state_inspector().visible, 1);
        ASSERT_INT("chip click leaves the detail columns alone",
                   ui_state_gl_state_inspector().details_expanded, 0);

        view = glr_ctrl_build_gl_state_panel_view(NULL);
        if (gl_state_popup_find_setup_toggle(&view, &x, &y)) {
            glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, x, y);
            glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_UP, x, y);
            ASSERT_INT("a second chip click folds them back",
                       ui_state_gl_state_inspector().setup_expanded, 0);
        }
    } else {
        ASSERT_TRUE("live popup exposes a setup chip", 0);
    }
}

/* The popup's source column quotes a line number, and the only thing that
 * number is good for is finding the line in the code panel — so it has to be
 * the panel's own gutter label, not the document index. The two diverge the
 * moment code focus is off, because the gutter counts the derived-C chrome
 * rows the focus view hides. Before this was resolved through the panel's
 * built rows, a three-line scene reported display():1 for a row the gutter
 * numbered 101. */
static void test_gl_state_popup_source_line_tracks_gutter(void) {
    UiGlStatePanelView view;
    UiHit hit;
    int blank_line, enable_line;
    int x = -1, y = -1;
    int focused_label = -1, unfocused_label = -1;
    int focused_right, unfocused_right;
    int i;

    printf("--- imrepl_ctrl OpenGL-state popup source line vs gutter ---\n");

    prepare_code_panel_mouse_fixture();
    if (!glr_state_presentation().code_focus)
        glr_ctrl_toggle_code_focus();

    enable_line = repl_state_document_count();
    editor_state_edit_line_set(enable_line);
    ASSERT_INT("append a state write to cite",
               editor_feed_line("glDepthFunc(GL_LEQUAL);"), 1);
    blank_line = repl_state_document_count();
    editor_state_edit_line_set(blank_line);
    editor_insert_mode_set(0);
    ASSERT_INT("append committed blank line", editor_feed_line(""), 1);
    ASSERT_TRUE("found empty source row hit",
                find_code_text_hit_for_line(blank_line, &hit, &x, &y));
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
    ASSERT_INT("right-click opens the popup",
               ui_state_gl_state_inspector().visible, 1);
    /* The source column only exists with the detail columns expanded. */
    ui_state_gl_state_inspector_toggle_details();

    glr_ctrl_display_frame();
    view = glr_ctrl_build_gl_state_panel_view(NULL);
    ASSERT_TRUE("popup view carries gutter labels",
                view.source_gutter_labels != NULL);
    ASSERT_TRUE("popup view carries a report", view.report != NULL);
    if (!view.report || !view.source_gutter_labels)
        return;
    for (i = 0; i < view.report->count; i++) {
        if (view.report->rows[i].source.source_line_idx == enable_line) {
            focused_label = view.source_gutter_labels[i];
            break;
        }
    }
    ASSERT_TRUE("the cited row resolved a gutter label", focused_label > 0);
    /* Focus on: chrome hidden, so the gutter is the 1-based document line. */
    ASSERT_INT("focused gutter label is the document line",
               focused_label, enable_line + 1);
    focused_right = gl_state_popup_rightmost_hit_x(&view);

    /* Focus off: the derived-C boilerplate is emitted above the program, so
     * the same row's gutter label moves well past its document index. */
    glr_ctrl_toggle_code_focus();
    ASSERT_INT("code focus is off", glr_state_presentation().code_focus, 0);
    glr_ctrl_display_frame();
    view = glr_ctrl_build_gl_state_panel_view(NULL);
    if (!view.report || !view.source_gutter_labels)
        return;
    for (i = 0; i < view.report->count; i++) {
        if (view.report->rows[i].source.source_line_idx == enable_line) {
            unfocused_label = view.source_gutter_labels[i];
            break;
        }
    }
    ASSERT_TRUE("the cited row still resolves a gutter label",
                unfocused_label > 0);
    ASSERT_TRUE("unfocused gutter label clears the chrome rows",
                unfocused_label > focused_label);

    /* And it reaches the renderer, not just the view: a wider source string
     * widens the solved popup. */
    unfocused_right = gl_state_popup_rightmost_hit_x(&view);
    ASSERT_TRUE("the longer line number widens the solved popup",
                unfocused_right > focused_right);

    glr_ctrl_toggle_code_focus();
}

static void test_gl_state_popup_defers_to_front_overlay(void) {
    UiRenderSnapshot snap;
    UiVariablePanelView var_view;
    UiGlStatePanelView state_view;
    UiHit front_hit;
    int blank_line;
    int vx, vy, vw, vh;
    int overlap_x = -1;
    int overlap_y = -1;
    int max_scroll;
    int initial_scroll;
    int px, gl_y;

    printf("--- imrepl_ctrl OpenGL-state/front-overlay z routing ---\n");

    prepare_code_panel_mouse_fixture();
    /* This test needs a report tall enough to overflow the popup. Light
     * parameter rows are the bulk of a full report and are gated on the
     * light being enabled, so light the scene rather than relying on a
     * particular row count falling out of the fixture. */
    editor_state_edit_line_set(repl_state_document_count());
    ASSERT_INT("append light 0 for the overflow fixture",
               editor_feed_line("glEnable(GL_LIGHT0);"), 1);
    editor_state_edit_line_set(repl_state_document_count());
    ASSERT_INT("append light 1 for the overflow fixture",
               editor_feed_line("glEnable(GL_LIGHT1);"), 1);
    editor_state_edit_line_set(repl_state_document_count());
    ASSERT_INT("append light 2 for the overflow fixture",
               editor_feed_line("glEnable(GL_LIGHT2);"), 1);
    editor_state_edit_line_set(repl_state_document_count());
    ASSERT_INT("append light 3 for the overflow fixture",
               editor_feed_line("glEnable(GL_LIGHT3);"), 1);
    blank_line = repl_state_document_count();
    editor_state_edit_line_set(blank_line);
    editor_insert_mode_set(0);
    ASSERT_INT("append overlap-test blank line", editor_feed_line(""), 1);
    variable_panel_set_visible(1);

    glr_ctrl_build_ui_snapshot(&snap);
    var_view = ui_app_variable_panel_view(&snap);
    ui_variable_panel_rect(&var_view, &vx, &vy, &vw, &vh);

    /* Anchor directly at the later-rendered variable panel. The state table
     * is wide enough to clamp across that area; scan for a pixel that is both
     * state-popup surface and inert variable-panel chrome. */
    ui_state_gl_state_inspector_open(
        blank_line, vx + vw / 2,
        snap.viewport.window_h - (vy + vh / 2));
    /* This test is about z-order and wheel routing, so it needs a popup tall
     * enough to overflow: unfold the generated setup rows, which the popup
     * hides by default. */
    ui_state_gl_state_inspector_toggle_setup();
#ifdef GL_STUBS
    /* Real-GL test binaries have no GL context; rendering a frame with
     * the popup visible would call into libGL (ui_gl_state_panel_render)
     * and crash. The stub build exercises the full frame. */
    glr_ctrl_display_frame();
#else
    if (repl_state_normals_dirty()) {
        int edit_line = editor_state_edit_line();
        repl_recompute_autonormals(glr_state_presentation().autonormal,
                                   &edit_line);
        editor_state_edit_line_set(edit_line);
        repl_state_normals_dirty_clear();
    }
    if (repl_state_flat_program_dirty()) {
        repl_flatten_commands(editor_state_edit_line());
        repl_state_flat_program_clear_dirty();
    }
#endif
    state_view = glr_ctrl_build_gl_state_panel_view(NULL);
    glr_ctrl_build_ui_snapshot(&snap);
    for (gl_y = vy; gl_y < vy + vh && overlap_x < 0; gl_y++) {
        int my = snap.viewport.window_h - gl_y;
        for (px = vx; px < vx + vw && overlap_x < 0; px++) {
            front_hit = ui_panels_hit_test_above_gl_state(
                &snap, px, my, repl_eval_predef_view().count);
            if (front_hit.kind == UI_HIT_OVERLAY_CHROME &&
                ui_gl_state_panel_hit_test(&state_view, px, my)) {
                overlap_x = px;
                overlap_y = my;
            }
        }
    }
    ASSERT_TRUE("found variable chrome over state popup", overlap_x >= 0);
    if (overlap_x < 0)
        return;

    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, overlap_x, overlap_y);
    ASSERT_INT("front overlay left click keeps state popup open",
               ui_state_gl_state_inspector().visible, 1);
    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_UP, overlap_x, overlap_y);

    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, overlap_x, overlap_y);
    ASSERT_INT("front overlay right click keeps state popup open",
               ui_state_gl_state_inspector().visible, 1);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, overlap_x, overlap_y);

    state_view = glr_ctrl_build_gl_state_panel_view(NULL);
    max_scroll = ui_gl_state_panel_max_scroll(&state_view);
    ASSERT_TRUE("overlapped state report is scrollable", max_scroll > 0);
    if (max_scroll > 0) {
        initial_scroll = max_scroll > 1 ? max_scroll - 1 : 0;
        ui_state_gl_state_inspector_set_scroll(initial_scroll);
        route_wheel(overlap_x, overlap_y, 1);
        ASSERT_INT("front overlay wheel does not scroll state popup",
                   ui_state_gl_state_inspector().scroll_rows,
                   initial_scroll);
    }
}

static void test_left_click_code_panel_exits_search_and_places_cursor(void) {
    UiHit hit;
    int x = -1;
    int y = -1;

    printf("--- imrepl_ctrl search click refocuses editor ---\n");

    prepare_code_panel_mouse_fixture();
    editor_handle_key(KEY_CTRL_F, 0, 0);
    editor_handle_key('V', 0, 0);
    ASSERT_INT("search active before code click", editor_state_search()->active, 1);
    ASSERT_TRUE("search query populated before code click",
                editor_state_search()->query_len > 0);

    ASSERT_TRUE("found code text hit for search refocus test",
                find_hit_point_in_code_panel(UI_HIT_CODE_TEXT, &hit, &x, &y));
    ASSERT_TRUE("code text hit has line", hit.line_idx >= 0);
    ASSERT_TRUE("code text hit has char", hit.char_idx >= 0);

    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, x, y);

    ASSERT_INT("left-click code exits search", editor_state_search()->active, 0);
    ASSERT_INT("left-click code clears search query",
               editor_state_search()->query_len, 0);
    ASSERT_INT("left-click code moves edit line",
               editor_state_edit_line(), hit.line_idx);
    ASSERT_INT("left-click code places cursor",
               editor_state_input().cursor_pos, hit.char_idx);

    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_UP, x, y);
}

/* Grid/axes in-out transition wiring: the controller diffs the
 * presentation theme, ticks g_grid_xn/g_axes_xn, and writes the
 * effective {theme, opacity, phase} into Render3dRenderConfig. Drives the
 * machines via the real glr_ctrl_tick (the animation timer) and reads
 * back through the g_last_scene_config capture stub. */
static void test_overlay_transition_machine_wiring(void) {
    printf("--- imrepl_ctrl grid/axes transition wiring ---\n");
    prepare_display_fixture();
    /* Quiesce replay so repeated ticks don't churn unrelated state. */
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;

    /* Ticks to fully complete an OUT then IN at the *configured* grid
     * durations (glr_ctrl_tick advances a fixed 0.016s). Derived from
     * the constants, not hardcoded, so retuning GRID_FADE_*_SECS in
     * render3d_transition.h doesn't break this test. +6 ticks of margin. */
    const int settle = (int)((GRID_FADE_OUT_SECS + GRID_FADE_IN_SECS)
                             / 0.016f) + 6;

    /* 1. First frame is a SNAP — rule 8 seeding (in glr_ctrl_reset_all)
     *    means the non-off default grid does NOT animate in at startup. */
    glr_ctrl_display_frame();
    ASSERT_INT("snap grid theme = default",
               g_last_scene_config.grid_theme, CFG_DEFAULT_GRID_THEME);
    ASSERT_FLOAT("snap grid opacity 1", g_last_scene_config.grid_opacity, 1.0f);
    ASSERT_INT("snap grid phase STEADY",
               g_last_scene_config.grid_xn_phase, RENDER3D_XN_STEADY);
    ASSERT_FLOAT("snap axes opacity 1", g_last_scene_config.axes_opacity, 1.0f);
    ASSERT_INT("snap axes phase STEADY",
               g_last_scene_config.axes_xn_phase, RENDER3D_XN_STEADY);

    /* 2. A theme change drives FADE_OUT (old theme still drawn) then,
     *    past the opacity-0 crossing, FADE_IN of the adopted theme. */
    glr_state_presentation_mut()->grid_theme = GRID_THEME_CLASSIC;
    glr_ctrl_tick();
    glr_ctrl_display_frame();
    ASSERT_INT("OUT keeps old theme",
               g_last_scene_config.grid_theme, CFG_DEFAULT_GRID_THEME);
    ASSERT_INT("phase FADE_OUT",
               g_last_scene_config.grid_xn_phase, RENDER3D_XN_FADE_OUT);
    ASSERT_TRUE("opacity dropping during OUT",
                g_last_scene_config.grid_opacity < 1.0f &&
                g_last_scene_config.grid_opacity > 0.0f);

    for (int i = 0; i < settle; i++) glr_ctrl_tick();
    glr_ctrl_display_frame();
    ASSERT_INT("adopted new theme after crossing",
               g_last_scene_config.grid_theme, GRID_THEME_CLASSIC);
    ASSERT_FLOAT("faded fully back in",
                 g_last_scene_config.grid_opacity, 1.0f);
    ASSERT_INT("phase STEADY after IN",
               g_last_scene_config.grid_xn_phase, RENDER3D_XN_STEADY);

    /* 3. Rapid toggle: retarget mid-OUT; the latest selection is
     *    adopted at the crossing and the ephemeral one is skipped. */
    glr_state_presentation_mut()->grid_theme = GRID_THEME_TRON;
    glr_ctrl_tick();
    glr_state_presentation_mut()->grid_theme = GRID_THEME_EMBER;
    for (int i = 0; i < settle; i++) glr_ctrl_tick();
    glr_ctrl_display_frame();
    ASSERT_INT("adopts latest, skips ephemeral TRON",
               g_last_scene_config.grid_theme, GRID_THEME_EMBER);

    /* 4. Rule 6 off-source: from the off index the controller calls
     *    render3d_xn_show, so the new theme fades IN from 0 with NO
     *    preceding FADE_OUT (current jumps straight to the theme). */
    glr_state_presentation_mut()->grid_theme = GRID_THEME_OFF;
    for (int i = 0; i < settle; i++) glr_ctrl_tick();   /* settle to off */
    glr_state_presentation_mut()->grid_theme = GRID_THEME_CLASSIC;
    glr_ctrl_tick();
    glr_ctrl_display_frame();
    ASSERT_INT("show: current jumps straight to theme",
               g_last_scene_config.grid_theme, GRID_THEME_CLASSIC);
    ASSERT_INT("show: phase FADE_IN (no dead OUT)",
               g_last_scene_config.grid_xn_phase, RENDER3D_XN_FADE_IN);
    ASSERT_TRUE("show: fading up from 0",
                g_last_scene_config.grid_opacity > 0.0f &&
                g_last_scene_config.grid_opacity < 1.0f);

    /* 5. glr_ctrl_reset_all() re-seeds both machines to the post-reset
     *    presentation at full opacity, STEADY — no post-reset animation. */
    glr_ctrl_reset_all();
    glr_ctrl_display_frame();
    ASSERT_INT("reset snaps grid to default",
               g_last_scene_config.grid_theme, CFG_DEFAULT_GRID_THEME);
    ASSERT_FLOAT("reset grid opacity 1",
                 g_last_scene_config.grid_opacity, 1.0f);
    ASSERT_INT("reset grid STEADY",
               g_last_scene_config.grid_xn_phase, RENDER3D_XN_STEADY);
}

static void test_tick_per_frame_scheduling(void) {
    printf("--- imrepl_ctrl tick-per-frame scheduling ---\n");
    glr_ctrl_reset_all();
    repl_set_time(0.0f);

    /* Default mode: presentation does not own time; the paced timer does. */
    glr_ctrl_set_tick_per_frame(0);
    glr_ctrl_frame_presented();
    ASSERT_FLOAT("timer mode: presentation does not tick",
                 repl_state_variables().anim_time, 0.0f);
    glr_ctrl_on_frame_timer();
    ASSERT_FLOAT("timer mode: timer advances once",
                 repl_state_variables().anim_time, GLR_FRAME_DT_SECS);

    /* Capture mode: the timer only requests a redraw and every completed
     * frame advances the whole fixed-dt simulation exactly once. */
    repl_set_time(0.0f);
    glr_ctrl_set_tick_per_frame(1);
    glr_ctrl_on_frame_timer();
    ASSERT_FLOAT("frame mode: timer does not tick",
                 repl_state_variables().anim_time, 0.0f);
    glr_ctrl_frame_presented();
    ASSERT_FLOAT("frame mode: first frame advances once",
                 repl_state_variables().anim_time, GLR_FRAME_DT_SECS);
    glr_ctrl_frame_presented();
    ASSERT_FLOAT("frame mode: second frame advances once",
                 repl_state_variables().anim_time, 2.0f * GLR_FRAME_DT_SECS);

    /* This controller process serves the rest of the test suite too. */
    glr_ctrl_set_tick_per_frame(0);
}

static void test_view_mode_projection_transition_wiring(void) {
    GlrCameraState cam;
    int projection_settle_ticks =
        (int)(GLR_VIEW_PROJECTION_TRANSITION_SECS / 0.016f) + 4;

    printf("--- imrepl_ctrl view mode projection transition ---\n");
    prepare_display_fixture();
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;

    glr_ctrl_display_frame();
    ASSERT_FLOAT("view mode starts perspective",
                 g_last_scene_config.projection_mix, 1.0f);
    ASSERT_INT("camera controls start 3d",
               glr_camera_control_mode(), GLR_CAMERA_CONTROL_3D);

    glr_state_presentation_mut()->ortho_mode = RENDER3D_VIEW_2D;
    glr_ctrl_tick();
    glr_ctrl_display_frame();
    ASSERT_INT("camera controls switch to 2d",
               glr_camera_control_mode(), GLR_CAMERA_CONTROL_2D);
    ASSERT_FLOAT("projection waits for camera before ortho",
                 g_last_scene_config.projection_mix, 1.0f);
    ASSERT_INT("camera-to-2d target is active",
               glr_camera_target_active(), 1);
    ASSERT_TRUE("camera rx eases toward xy view",
                glr_camera().rx < 11.0f);
    ASSERT_TRUE("camera ry eases toward xy view",
                glr_camera().ry < 22.0f);

    for (int i = 0; i < 200 && glr_camera_target_active(); i++)
        glr_ctrl_tick();
    ASSERT_INT("camera-to-2d target completes",
               glr_camera_target_active(), 0);
    glr_ctrl_display_frame();
    ASSERT_FLOAT("projection still perspective when camera completes",
                 g_last_scene_config.projection_mix, 1.0f);
    cam = glr_camera();
    ASSERT_FLOAT("camera rx reaches xy view", cam.rx, 0.0f);
    ASSERT_FLOAT("camera ry reaches xy view", cam.ry, 0.0f);
    ASSERT_FLOAT("camera z pan reaches xy plane", cam.tz, 0.0f);

    glr_ctrl_tick();
    glr_ctrl_display_frame();
    ASSERT_TRUE("projection starts moving toward ortho after camera",
                g_last_scene_config.projection_mix < 1.0f &&
                g_last_scene_config.projection_mix > 0.0f);

    for (int i = 0; i < projection_settle_ticks; i++)
        glr_ctrl_tick();
    glr_ctrl_display_frame();
    ASSERT_FLOAT("projection settles on ortho",
                 g_last_scene_config.projection_mix, 0.0f);

    glr_state_presentation_mut()->ortho_mode = RENDER3D_VIEW_3D;
    glr_ctrl_tick();
    glr_ctrl_display_frame();
    ASSERT_INT("camera controls stay 2d while projection exits ortho",
               glr_camera_control_mode(), GLR_CAMERA_CONTROL_2D);
    ASSERT_TRUE("projection starts moving toward perspective",
                g_last_scene_config.projection_mix > 0.0f &&
                g_last_scene_config.projection_mix < 1.0f);
    cam = glr_camera();
    ASSERT_FLOAT("camera rx waits while projection exits ortho", cam.rx, 0.0f);
    ASSERT_FLOAT("camera ry waits while projection exits ortho", cam.ry, 0.0f);

    for (int i = 0; i < projection_settle_ticks; i++)
        glr_ctrl_tick();
    glr_ctrl_display_frame();
    ASSERT_FLOAT("projection settles on perspective before camera",
                 g_last_scene_config.projection_mix, 1.0f);
    ASSERT_INT("camera controls return to 3d after projection",
               glr_camera_control_mode(), GLR_CAMERA_CONTROL_3D);
    ASSERT_TRUE("camera rx returns toward saved 3d orbit",
                glr_camera().rx > 0.0f);
    ASSERT_TRUE("camera ry returns toward saved 3d orbit",
                glr_camera().ry > 0.0f);
}

/* glr_ctrl_start_camera_to_2d applies GLR_VIEW_CAMERA_TO_2D_DECAY to
 * the ease so the orbit flattening doesn't drag the subsequent
 * projection blend. Verify the first tick after the ortho toggle
 * covers strictly more rx/ry distance than the global default decay
 * would produce, AND that the 2D->3D leg keeps the global default
 * (no override leak through start_camera_to_3d). */
static void test_view_mode_3d_to_2d_uses_faster_decay(void) {
    float rx_default_step;
    float ry_default_step;
    GlrCameraState cam;

    printf("--- imrepl_ctrl 3d->2d faster decay ---\n");
    prepare_display_fixture();
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;
    glr_ctrl_display_frame();

    /* What the GLOBAL default decay would cover in a single tick from
     * the fixture's rx=11, ry=22 toward (rx=ry=0). The override must
     * beat both. */
    rx_default_step = 11.0f * (1.0f - GLR_CAMERA_TARGET_DECAY);
    ry_default_step = 22.0f * (1.0f - GLR_CAMERA_TARGET_DECAY);

    /* 3D -> 2D: first tick uses the faster decay. */
    glr_state_presentation_mut()->ortho_mode = RENDER3D_VIEW_2D;
    glr_ctrl_tick();

    cam = glr_camera();
    ASSERT_TRUE("3D->2D first tick covers more rx than global default",
                (11.0f - cam.rx) > rx_default_step + 0.1f);
    ASSERT_TRUE("3D->2D first tick covers more ry than global default",
                (22.0f - cam.ry) > ry_default_step + 0.1f);
    /* Sanity-check the override actually matches GLR_VIEW_CAMERA_TO_2D_DECAY
     * within float epsilon, not some other intermediate value. */
    {
        float expected_rx = 11.0f * GLR_VIEW_CAMERA_TO_2D_DECAY;
        float expected_ry = 22.0f * GLR_VIEW_CAMERA_TO_2D_DECAY;
        ASSERT_TRUE("rx matches GLR_VIEW_CAMERA_TO_2D_DECAY step",
                    fabsf(cam.rx - expected_rx) < 0.05f);
        ASSERT_TRUE("ry matches GLR_VIEW_CAMERA_TO_2D_DECAY step",
                    fabsf(cam.ry - expected_ry) < 0.05f);
    }

    /* Settle 3D->2D. */
    for (int i = 0; i < 400 && glr_camera_target_active(); i++)
        glr_ctrl_tick();
    /* Drain the projection-blend phase too so the FSM lands in IDLE
     * before we test the reverse direction. */
    for (int i = 0; i < 200; i++) {
        glr_ctrl_tick();
        glr_ctrl_display_frame();
        if (g_last_scene_config.projection_mix == 0.0f)
            break;
    }

    /* 2D -> 3D: projection moves first under the sequential FSM. The
     * tick where projection reaches 1.0 also calls start_camera_to_3d
     * AND runs glr_camera_tick once (the tick chain is view_transition
     * then camera_tick), so the camera is already 1 tick into its ease
     * the moment projection settles — that's the sample to measure
     * against the default decay. */
    glr_state_presentation_mut()->ortho_mode = RENDER3D_VIEW_3D;
    int converged_at = -1;
    for (int i = 0; i < 200; i++) {
        glr_ctrl_tick();
        glr_ctrl_display_frame();
        if (g_last_scene_config.projection_mix == 1.0f) {
            converged_at = i;
            break;
        }
    }
    ASSERT_TRUE("2D->3D projection reached 1.0 within drain budget",
                converged_at >= 0);
    cam = glr_camera();
    {
        /* Camera eased from (0, 0) toward saved (11, 22) for exactly
         * one tick with the global default decay (2D->3D does NOT apply
         * the faster override). Expected first-tick position:
         *   0 + (11 - 0) * (1 - 0.93) = 0.77 deg for rx.
         * A revert that propagated the faster decay (0.85) would push
         * cam.rx to ~1.65, well outside the 0.05 window. */
        float expected_rx_default = 11.0f * (1.0f - GLR_CAMERA_TARGET_DECAY);
        float expected_ry_default = 22.0f * (1.0f - GLR_CAMERA_TARGET_DECAY);
        ASSERT_TRUE("2D->3D first camera tick uses global default decay (rx)",
                    fabsf(cam.rx - expected_rx_default) < 0.05f);
        ASSERT_TRUE("2D->3D first camera tick uses global default decay (ry)",
                    fabsf(cam.ry - expected_ry_default) < 0.05f);
    }
}

/* The Projection config (GLR_CONFIG_PROJECTION) is a SECOND, independent
 * projection blend combined with the view-mode blend via min() (ortho wins).
 * Unlike View mode (RENDER3D_VIEW_2D), it must NOT flatten or lock the
 * camera: it swaps the projection matrix only, so the scene renders ortho
 * from the live orbit angle. This test pins the two load-bearing properties:
 * (1) toggling it to Ortho eases projection_mix to 0 while the camera stays
 * put at the fixture's (rx=11, ry=22) with control mode 3D and no ease in
 * flight, and (2) the combine is min() — 3D view + Ortho toggle still yields
 * projection_mix 0. */
static void test_projection_toggle_free_camera(void) {
    GlrCameraState cam;
    int projection_settle_ticks =
        (int)(GLR_VIEW_PROJECTION_TRANSITION_SECS / 0.016f) + 4;

    printf("--- imrepl_ctrl projection toggle (free-camera ortho) ---\n");
    prepare_display_fixture();
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;

    glr_ctrl_display_frame();
    ASSERT_FLOAT("projection toggle starts perspective",
                 g_last_scene_config.projection_mix, 1.0f);
    ASSERT_INT("view mode stays 3d perspective",
               glr_state_presentation().ortho_mode, RENDER3D_VIEW_3D);

    /* Toggle Projection -> Ortho via the config path (exercises the wiring). */
    glr_config_set(GLR_CONFIG_PROJECTION, PROJ_ORTHO);
    ASSERT_INT("projection config reads back ortho",
               glr_config_get(GLR_CONFIG_PROJECTION), PROJ_ORTHO);
    glr_ctrl_tick();
    glr_ctrl_display_frame();
    ASSERT_TRUE("projection starts easing toward ortho (free camera)",
                g_last_scene_config.projection_mix < 1.0f &&
                g_last_scene_config.projection_mix > 0.0f);
    /* The distinguishing property vs. View mode 2D: NO camera flatten, NO
     * control-mode switch, NO ease kicked off. */
    ASSERT_INT("camera stays in 3d control mode under projection toggle",
               glr_camera_control_mode(), GLR_CAMERA_CONTROL_3D);
    ASSERT_INT("projection toggle starts no camera ease",
               glr_camera_target_active(), 0);
    cam = glr_camera();
    ASSERT_FLOAT("camera rx unchanged by projection toggle", cam.rx, 11.0f);
    ASSERT_FLOAT("camera ry unchanged by projection toggle", cam.ry, 22.0f);

    for (int i = 0; i < projection_settle_ticks; i++)
        glr_ctrl_tick();
    glr_ctrl_display_frame();
    /* min(view_mix=1, toggle_mix=0) == 0 — the combine picks the ortho
     * contributor even though View mode is still 3D perspective. */
    ASSERT_FLOAT("projection settles on ortho (min picks the toggle)",
                 g_last_scene_config.projection_mix, 0.0f);
    ASSERT_INT("view mode still 3d after projection settles on ortho",
               glr_state_presentation().ortho_mode, RENDER3D_VIEW_3D);
    ASSERT_INT("camera still 3d control mode in free ortho",
               glr_camera_control_mode(), GLR_CAMERA_CONTROL_3D);
    cam = glr_camera();
    ASSERT_FLOAT("camera rx still at orbit angle in free ortho", cam.rx, 11.0f);
    ASSERT_FLOAT("camera ry still at orbit angle in free ortho", cam.ry, 22.0f);

    /* Toggle back to Perspective -> eases to 1, camera never moved. */
    glr_config_set(GLR_CONFIG_PROJECTION, PROJ_PERSPECTIVE);
    for (int i = 0; i < projection_settle_ticks; i++)
        glr_ctrl_tick();
    glr_ctrl_display_frame();
    ASSERT_FLOAT("projection returns to perspective",
                 g_last_scene_config.projection_mix, 1.0f);
    cam = glr_camera();
    ASSERT_FLOAT("camera rx never moved across the toggle", cam.rx, 11.0f);
    ASSERT_FLOAT("camera ry never moved across the toggle", cam.ry, 22.0f);

    /* reset_all clears the toggle blend back to perspective. */
    glr_config_set(GLR_CONFIG_PROJECTION, PROJ_ORTHO);
    for (int i = 0; i < projection_settle_ticks; i++)
        glr_ctrl_tick();
    glr_ctrl_reset_all();
    prepare_display_fixture();
    glr_ctrl_display_frame();
    ASSERT_INT("reset clears projection config to perspective",
               glr_state_presentation().projection_mode, PROJ_PERSPECTIVE);
    ASSERT_FLOAT("reset clears projection blend to perspective",
                 g_last_scene_config.projection_mix, 1.0f);
}

/* The controller's saved-3D snapshot drives the 2D->3D restoration.
 * Without an external refresh, switching examples while dwelling in 2D
 * leaves the snapshot pointing at the pose captured on 2D entry — so
 * pressing 3D restores the *previous* example's angle, not the one
 * currently loaded. The bridge call into
 * glr_ctrl_view_record_external_3d_pose lets the example loader keep
 * the snapshot aligned. */
static void tick_until_view_settled(int max_iters) {
    /* The view-mode transition runs projection and camera in sequence;
     * tick until both finish so this test stays oblivious to whichever
     * order the controller chains them in. */
    for (int i = 0; i < max_iters; i++) {
        glr_ctrl_tick();
        if (glr_camera_target_active())
            continue;
        glr_ctrl_display_frame();
        float mix = g_last_scene_config.projection_mix;
        if (mix == 0.0f || mix == 1.0f)
            return;
    }
}

static void test_view_record_external_3d_pose_tracks_in_ortho(void) {
    GlrCameraState cam;

    printf("--- imrepl_ctrl saved 3d pose refresh ---\n");
    prepare_display_fixture();
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;
    glr_ctrl_display_frame();

    /* 1) Enter 2D from a non-flat 3D pose; saved snapshot is the
     *    pre-ortho camera (rx=11, ry=22 from prepare_display_fixture). */
    glr_state_presentation_mut()->ortho_mode = RENDER3D_VIEW_2D;
    tick_until_view_settled(400);
    cam = glr_camera();
    ASSERT_FLOAT("camera flattened to rx=0", cam.rx, 0.0f);
    ASSERT_FLOAT("camera flattened to ry=0", cam.ry, 0.0f);

    /* 2) An example load (via the camera bridge) reports the new 3D
     *    target pose while we're still in 2D. */
    glr_ctrl_view_record_external_3d_pose(35.0f, 70.0f, 0.0f);

    /* 3) Returning to 3D should ease toward the reported pose, not the
     *    snapshot captured on 2D entry. */
    glr_state_presentation_mut()->ortho_mode = RENDER3D_VIEW_3D;
    tick_until_view_settled(400);
    cam = glr_camera();
    ASSERT_FLOAT("camera-to-3d uses refreshed rx", cam.rx, 35.0f);
    ASSERT_FLOAT("camera-to-3d uses refreshed ry", cam.ry, 70.0f);
}

/* Switching from a 3D example to a 2D example issues the new example's
 * `// camera` ease (e.g. dist=2.5) one frame before the view-mode
 * transition fires. The 3D->2D camera flatten must carry that ease
 * *destination* into 2D, not the still-live previous-example pose
 * (dist=7.5 here) — otherwise the ortho zoom locks onto the old camera
 * distance. Regression for the 3D-example -> 2D-example dist bug. */
static void test_view_mode_2d_honors_pending_camera_ease(void) {
    GlrCameraState cam;

    printf("--- imrepl_ctrl 3d->2d honors pending camera ease ---\n");
    prepare_display_fixture(); /* live camera dist = 7.5 (the "old" example) */
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;
    glr_ctrl_display_frame();

    /* Mimic the example-load order: the new 2D example's camera block
     * eases toward dist=2.5 (target active, live pose still at 7.5), then
     * its @cfg flips view_mode to 2D. The transition tick runs next. */
    glr_camera_ease_to(0.0f, 0.0f, 2.5f, 0.0f, 0.0f, 0.0f);
    glr_state_presentation_mut()->ortho_mode = RENDER3D_VIEW_2D;
    tick_until_view_settled(400);

    cam = glr_camera();
    ASSERT_TRUE("2D camera honors the example's eased dist, not the stale 7.5",
                fabsf(cam.dist - 2.5f) < 0.05f);
    ASSERT_FLOAT("2D camera flattened to rx=0", cam.rx, 0.0f);
    ASSERT_FLOAT("2D camera flattened to ry=0", cam.ry, 0.0f);
}

/* Drive the projection blend to a discrete endpoint (0 = ortho, 1 =
 * perspective). Unlike tick_until_view_settled, this keeps ticking until
 * the projection MIX reaches the target — tick_until_view_settled returns
 * the instant the camera flatten finishes, while the 2D projection blend
 * hasn't started, so it leaves mix == 1.0 (still perspective). */
static void tick_until_projection_mix(float want, int max_iters) {
    for (int i = 0; i < max_iters; i++) {
        glr_ctrl_tick();
        glr_ctrl_display_frame();
        if (g_last_scene_config.projection_mix == want)
            return;
    }
}

/* Once the projection has returned to 3D, its camera-restore leg may still be
 * easing toward the saved orbit. Resetting during that short window and then
 * entering 2D again must snapshot the reset destination, not retain the old
 * saved orbit for the next 2D->3D restore. */
static void test_view_mode_restore_honors_pending_camera_reset(void) {
    GlrCameraState cam;

    printf("--- imrepl_ctrl 3d reset survives 2d round trip ---\n");
    prepare_display_fixture();
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;
    glr_ctrl_display_frame();

    editor_input_set_modifier_provider_for_test(simulated_mods_provider);
    g_simulated_mods = GLUT_ACTIVE_SHIFT;

    /* First round trip reaches a perspective projection but deliberately
     * stops while the camera is still restoring the modified fixture pose. */
    glr_ctrl_keyboard(KEY_CTRL_V, 0, 0);
    tick_until_projection_mix(0.0f, 400);
    glr_ctrl_keyboard(KEY_CTRL_V, 0, 0);
    tick_until_projection_mix(1.0f, 400);
    ASSERT_INT("precondition: 3d camera restore is still easing",
               glr_camera_target_active(), 1);

    /* Exact user route in that window: Ctrl+Shift+C starts the camera reset,
     * followed immediately by Ctrl+Shift+V before the reset ease has settled. */
    glr_ctrl_keyboard(KEY_CTRL_C, 0, 0);
    ASSERT_INT("camera reset starts an ease", glr_camera_target_active(), 1);
    glr_ctrl_keyboard(KEY_CTRL_V, 0, 0);
    tick_until_projection_mix(0.0f, 400);

    /* Return to 3D through the same shortcut and settle both legs. */
    glr_ctrl_keyboard(KEY_CTRL_V, 0, 0);
    for (int i = 0; i < 600; i++) {
        glr_ctrl_tick();
        glr_ctrl_display_frame();
        if (g_last_scene_config.projection_mix == 1.0f &&
            !glr_camera_target_active())
            break;
    }

    cam = glr_camera();
    ASSERT_TRUE("reset rx restored after 2d round trip",
                fabsf(cam.rx - 20.0f) < 0.05f);
    ASSERT_TRUE("reset ry restored after 2d round trip",
                fabsf(cam.ry - 30.0f) < 0.05f);
    ASSERT_TRUE("reset distance restored after 2d round trip",
                fabsf(cam.dist - 5.0f) < 0.05f);
    ASSERT_FLOAT("reset tx restored after 2d round trip", cam.tx, 0.0f);
    ASSERT_FLOAT("reset ty restored after 2d round trip", cam.ty, 0.0f);
    ASSERT_FLOAT("reset tz restored after 2d round trip", cam.tz, 0.0f);

    g_simulated_mods = 0;
    editor_input_set_modifier_provider_for_test(NULL);
}

/* A file loaded from disk carries its authored `// camera` block just like a
 * built-in example does, so "Reset camera" (Ctrl+Shift+C) must return to that
 * pose — not to the global built-in defaults. Import streams the block through
 * the camera bridge line by line; the end-of-import hook is what turns the
 * result into the scene default. */
static void test_import_camera_block_becomes_scene_default(void) {
    static const char *const k_lines[] = {
        "// camera",
        "glTranslatef(0.0f, 0.0f, -15.0f);",
        "glRotatef(26.0f, 1.0f, 0.0f, 0.0f);",
        "glRotatef(-20.0f, 0.0f, 1.0f, 0.0f);",
        "glTranslatef(-1.0f, -2.0f, -3.0f);",
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glColor3f(1.0f, 0.0f, 0.0f);",
        NULL
    };
    GlrCameraState cam;

    printf("--- imrepl_ctrl imported camera block is the reset target ---\n");
    glr_ctrl_reset_all();

    ASSERT_INT("scene file imports",
               repl_export_load_from_lines(k_lines, "cam-default.glr", NULL), 1);

    /* Fly somewhere else, then reset. */
    glr_camera_set(45.0f, 90.0f, 12.0f, 7.0f, -2.0f, 3.0f, 0.0f);
    glr_camera_ease_to_default();
    for (int i = 0; i < 600 && glr_camera_target_active(); i++)
        glr_camera_tick();

    cam = glr_camera();
    ASSERT_TRUE("reset returns to the file's rx", fabsf(cam.rx - 26.0f) < 0.05f);
    ASSERT_TRUE("reset returns to the file's ry", fabsf(cam.ry + 20.0f) < 0.05f);
    ASSERT_TRUE("reset returns to the file's dist", fabsf(cam.dist - 15.0f) < 0.05f);
    ASSERT_TRUE("reset returns to the file's tx", fabsf(cam.tx - 1.0f) < 0.05f);
    ASSERT_TRUE("reset returns to the file's ty", fabsf(cam.ty - 2.0f) < 0.05f);
    ASSERT_TRUE("reset returns to the file's tz", fabsf(cam.tz - 3.0f) < 0.05f);
}

/* A file with no `// camera` header has no authored pose, so reset must keep
 * using the built-in defaults. */
static void test_import_without_camera_block_keeps_global_default(void) {
    static const char *const k_lines[] = {
        "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);",
        "glColor3f(0.0f, 1.0f, 0.0f);",
        NULL
    };
    GlrCameraState cam;

    printf("--- imrepl_ctrl header-less import keeps global camera default ---\n");
    glr_ctrl_reset_all();

    ASSERT_INT("header-less scene imports",
               repl_export_load_from_lines(k_lines, "no-cam.glr", NULL), 1);

    glr_camera_set(45.0f, 90.0f, 12.0f, 7.0f, -2.0f, 3.0f, 0.0f);
    glr_camera_ease_to_default();
    for (int i = 0; i < 600 && glr_camera_target_active(); i++)
        glr_camera_tick();

    cam = glr_camera();
    ASSERT_TRUE("reset falls back to default rx", fabsf(cam.rx - 20.0f) < 0.05f);
    ASSERT_TRUE("reset falls back to default ry", fabsf(cam.ry - 30.0f) < 0.05f);
    ASSERT_TRUE("reset falls back to default dist", fabsf(cam.dist - 5.0f) < 0.05f);
}

/* Cycling 2D-example -> 3D-example-A -> 3D-example-B fast: example A
 * starts a 2D->3D projection blend; example B loads mid-blend. The
 * saved-3D snapshot (consumed by start_camera_to_3d when the blend ends)
 * must refresh to B's pose, not stay stuck on A's — and the carried
 * pan/zoom must track B's full target, not the ~98%-eased live pose. */
static void test_view_mode_3d_restore_tracks_example_loaded_midblend(void) {
    GlrCameraState cam;

    printf("--- imrepl_ctrl 2d->3d restore tracks mid-blend example ---\n");
    prepare_display_fixture();
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;
    glr_ctrl_display_frame();

    /* Settle fully into 2D: drive the projection all the way to ortho. */
    glr_state_presentation_mut()->ortho_mode = RENDER3D_VIEW_2D;
    tick_until_projection_mix(0.0f, 400);
    ASSERT_FLOAT("precondition: settled into 2D ortho",
                 g_last_scene_config.projection_mix, 0.0f);

    /* Example A (3D) loads while still in 2D: records its 3D pose into the
     * saved snapshot, eases the camera toward it, then flips to 3D. */
    glr_camera_ease_to(30.0f, 60.0f, 4.0f, 0.0f, 0.0f, 0.0f);
    glr_ctrl_view_record_external_3d_pose(30.0f, 60.0f, 0.0f);
    glr_state_presentation_mut()->ortho_mode = RENDER3D_VIEW_3D;

    /* One tick starts the ~0.6s projection blend (PROJECTION_TO_3D): we are
     * now genuinely mid-transition and start_camera_to_3d has NOT fired. */
    glr_ctrl_tick();
    glr_ctrl_display_frame();
    ASSERT_TRUE("precondition: mid-blend (0 < mix < 1)",
                g_last_scene_config.projection_mix > 0.0f &&
                g_last_scene_config.projection_mix < 1.0f);

    /* Example B (3D) loads mid-blend: same bridge calls, new pose. */
    glr_camera_ease_to(45.0f, 90.0f, 2.0f, 0.0f, 0.0f, 0.0f);
    glr_ctrl_view_record_external_3d_pose(45.0f, 90.0f, 0.0f);

    /* Finish the blend (fires start_camera_to_3d), then let the camera
     * ease settle. */
    for (int i = 0; i < 400; i++) {
        glr_ctrl_tick();
        glr_ctrl_display_frame();
        if (g_last_scene_config.projection_mix == 1.0f &&
            !glr_camera_target_active())
            break;
    }

    cam = glr_camera();
    ASSERT_TRUE("3D restore uses example B's orbit rx, not stale example A",
                fabsf(cam.rx - 45.0f) < 0.5f);
    ASSERT_TRUE("3D restore uses example B's orbit ry, not stale example A",
                fabsf(cam.ry - 90.0f) < 0.5f);
    ASSERT_TRUE("3D restore lands on example B's full dist, not the midblend live pose",
                fabsf(cam.dist - 2.0f) < 0.05f);
}

/* In 3D mode the live camera is authoritative, so an external 3D-pose
 * report (a stale callback firing outside the ortho window) must not
 * smear the saved snapshot. */
static void test_view_record_external_3d_pose_noop_in_perspective(void) {
    GlrCameraState cam_before;
    GlrCameraState cam_after;

    printf("--- imrepl_ctrl saved 3d pose noop in 3d ---\n");
    prepare_display_fixture();
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;
    glr_ctrl_display_frame();

    /* Seed a saved snapshot by entering and leaving 2D. */
    glr_state_presentation_mut()->ortho_mode = RENDER3D_VIEW_2D;
    tick_until_view_settled(400);
    glr_state_presentation_mut()->ortho_mode = RENDER3D_VIEW_3D;
    tick_until_view_settled(400);
    cam_before = glr_camera();

    /* Fire the bridge while in 3D — the saved snapshot must be ignored. */
    glr_ctrl_view_record_external_3d_pose(99.0f, 99.0f, 99.0f);

    glr_state_presentation_mut()->ortho_mode = RENDER3D_VIEW_2D;
    tick_until_view_settled(400);
    glr_state_presentation_mut()->ortho_mode = RENDER3D_VIEW_3D;
    tick_until_view_settled(400);
    cam_after = glr_camera();

    ASSERT_FLOAT("noop in 3d: rx unchanged", cam_after.rx, cam_before.rx);
    ASSERT_FLOAT("noop in 3d: ry unchanged", cam_after.ry, cam_before.ry);
    ASSERT_FLOAT("noop in 3d: tz unchanged", cam_after.tz, cam_before.tz);
}

/* QUIT_RECOVERY_FILE is the filename Ctrl+Q / SIGINT writes a recovery
 * copy to. Verify the constant value and that the save helper actually
 * lands a file by that name in the current directory. */
static void test_quit_recovery_file(void) {
    char cwd[1024];
    char temp_dir[] = "/tmp/test_glr_ctrl_recovery.XXXXXX";
    char *made_dir;
    int have_cwd;

    printf("--- imrepl_ctrl quit recovery file ---\n");

    ASSERT_STR("QUIT_RECOVERY_FILE value", QUIT_RECOVERY_FILE,
               "recovery.c");

    glr_ctrl_reset_all();
    editor_feed_line("glVertex3f(1, 2, 3);");

    made_dir = mkdtemp(temp_dir);
    have_cwd = getcwd(cwd, sizeof(cwd)) != NULL;
    ASSERT_TRUE("mkdtemp recovery dir", made_dir != NULL);
    ASSERT_TRUE("getcwd before recovery save", have_cwd);
    if (!made_dir || !have_cwd)
        return;

    ASSERT_INT("chdir recovery dir", chdir(made_dir), 0);
    glr_ctrl_save_quit_recovery();
    ASSERT_INT("recovery save wrote QUIT_RECOVERY_FILE",
               access(QUIT_RECOVERY_FILE, F_OK), 0);
    unlink(QUIT_RECOVERY_FILE);
    ASSERT_INT("restore cwd after recovery save", chdir(cwd), 0);
    rmdir(made_dir);
}

/* Audit #39 prep: glr_ctrl_build_ui_snapshot is called twice per
 * display frame, defended as "the second build picks up post-
 * follow-scroll offsets." The fix proposes splitting snapshot into
 * stable + scroll-dependent halves. To make that safe, pin that
 * building the snapshot twice in a row (with no intervening state
 * change) produces equal observable fields.
 *
 * We don't memcmp() the whole struct because it contains pointers
 * (document_cmds, snapshots of subsystem state) that may legitimately
 * vary across builds in some refactors. Instead spot-check the fields
 * a renderer would actually consume — viewport, code-panel, replay,
 * camera-derived selection, autocomplete — so the test pins the
 * observable contract rather than internal layout. */
static void test_build_ui_snapshot_is_idempotent(void) {
    UiRenderSnapshot snap_a;
    UiRenderSnapshot snap_b;

    printf("--- imrepl_ctrl snapshot idempotence ---\n");
    prepare_display_fixture();
    repl_state_flat_program_mut()->overflow_cmd_count = MAX_FLAT_COMMANDS + 7;

    glr_ctrl_build_ui_snapshot(&snap_a);
    glr_ctrl_build_ui_snapshot(&snap_b);

    ASSERT_INT("snapshot viewport_w idempotent",
               snap_b.viewport.window_w, snap_a.viewport.window_w);
    ASSERT_INT("snapshot viewport_h idempotent",
               snap_b.viewport.window_h, snap_a.viewport.window_h);
    ASSERT_INT("snapshot code_panel layout idempotent",
               snap_b.code_panel.layout_mode, snap_a.code_panel.layout_mode);
    ASSERT_INT("snapshot edit_line idempotent",
               snap_b.edit_line, snap_a.edit_line);
    ASSERT_INT("snapshot document_count idempotent",
               snap_b.document_count, snap_a.document_count);
    ASSERT_INT("snapshot flat_program_count idempotent",
               snap_b.flat_program_count, snap_a.flat_program_count);
    ASSERT_INT("snapshot flat_program_overflow_count idempotent",
               snap_b.flat_program_overflow_count,
               snap_a.flat_program_overflow_count);
    ASSERT_INT("snapshot carries flat overflow count for toolbar",
               snap_a.flat_program_overflow_count,
               MAX_FLAT_COMMANDS + 7);
    ASSERT_INT("snapshot replay.active idempotent",
               snap_b.replay.active, snap_a.replay.active);
    ASSERT_INT("snapshot replay.state idempotent",
               snap_b.replay.state, snap_a.replay.state);
    ASSERT_INT("snapshot replay.pc idempotent",
               snap_b.replay.pc, snap_a.replay.pc);
    ASSERT_INT("snapshot variable_drag.active_var idempotent",
               snap_b.variable_drag.active_var,
               snap_a.variable_drag.active_var);
    ASSERT_INT("snapshot selection_active idempotent",
               snap_b.selection_active, snap_a.selection_active);
    ASSERT_INT("snapshot selection_lo idempotent",
               snap_b.selection_lo, snap_a.selection_lo);
    ASSERT_INT("snapshot autocomplete.match_count idempotent",
               snap_b.autocomplete.match_count, snap_a.autocomplete.match_count);
    ASSERT_FLOAT("snapshot anim_time idempotent",
                 snap_b.anim_time, snap_a.anim_time);
    ASSERT_STR("snapshot status.text idempotent",
               snap_b.status.text, snap_a.status.text);
    ASSERT_INT("snapshot active_indent_chars idempotent",
               snap_b.active_indent_chars, snap_a.active_indent_chars);
    ASSERT_INT("snapshot user_scene_active_idx idempotent",
               snap_b.user_scene_active_idx, snap_a.user_scene_active_idx);
}

/* Audit #14 prep: scene-config invariants over the per-frame display
 * path. The audit proposes extracting ~600 lines of overlay rendering
 * from glr_ctrl into src/scene/. The contract that survives the move
 * is what fields the controller must populate on Render3dRenderConfig so
 * the (post-refactor) scene module can drive overlay passes directly.
 *
 * Most fields are already pinned by
 * test_display_frame_builds_config_and_restores_live_state; this test
 * adds the invariants that travel ACROSS frames — repeated frames must
 * not introduce hysteresis, and the post-overlays hook's user_data
 * must point at the config that hosts the guides (so the future
 * scene-side overlay code can rely on that handle). */
static void test_display_frame_scene_config_is_stable_across_frames(void) {
    Render3dRenderConfig frame1;
    Render3dRenderConfig frame2;

    printf("--- imrepl_ctrl scene config purity ---\n");
    prepare_display_fixture();
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;

    glr_ctrl_display_frame();
    frame1 = g_last_scene_config;

    glr_ctrl_display_frame();
    frame2 = g_last_scene_config;

    /* Stable inputs across frames. Note: anim_time advances inside
     * render3d_render (via the replay tick), so don't pin that here —
     * the rest of the config is steady-state. */
    ASSERT_INT("render3d_w stable across frames",
               frame2.render3d_w, frame1.render3d_w);
    ASSERT_INT("render3d_h stable across frames",
               frame2.render3d_h, frame1.render3d_h);
    ASSERT_FLOAT("scene cam_dist stable across frames",
                 frame2.cam_dist, frame1.cam_dist);
    ASSERT_FLOAT("scene cam_rx stable across frames",
                 frame2.cam_rx, frame1.cam_rx);
    ASSERT_FLOAT("scene cam_ry stable across frames",
                 frame2.cam_ry, frame1.cam_ry);
    ASSERT_FLOAT("scene projection_mix stable across frames",
                 frame2.projection_mix, frame1.projection_mix);
    ASSERT_INT("scene wireframe stable across frames",
               frame2.wireframe, frame1.wireframe);
    ASSERT_INT("scene grid_theme stable across frames",
               frame2.grid_theme, frame1.grid_theme);
    ASSERT_INT("scene axes_theme stable across frames",
               frame2.axes_theme, frame1.axes_theme);
    ASSERT_INT("scene user_lighting_enabled stable across frames",
               frame2.user_lighting_enabled, frame1.user_lighting_enabled);
    ASSERT_INT("scene show_light_indicators stable across frames",
               frame2.show_light_indicators, frame1.show_light_indicators);
    ASSERT_FLOAT("scene alpha_scale stable across frames",
                 frame2.alpha_scale, frame1.alpha_scale);

    /* The post_overlays_fn hook is wired with config-as-user_data so
     * the hook reads guides off Render3dRenderConfig. Pin that pointer
     * identity — a refactor that switches user_data to NULL or to a
     * different pointer would break the guide overlay. */
    ASSERT_TRUE("post_overlays_fn wired in frame 1",
                frame1.post_overlays_fn != NULL);
    ASSERT_TRUE("post_overlays_fn wired in frame 2",
                frame2.post_overlays_fn != NULL);
    /* user_data IS the live config the controller passed into
     * render3d_draw_scene (not the cached frame1/frame2 copies).
     * Catch it by reading g_last_scene_config.post_overlays_user_data
     * after the second frame and asserting it equals &g_last_scene_config
     * is intentionally NOT done — the controller hands the scene module
     * its OWN local Render3dRenderConfig. We pin the looser invariant: the
     * hook always carries a non-NULL user_data alongside the fn. */
    ASSERT_TRUE("post_overlays_user_data non-NULL",
                frame2.post_overlays_user_data != NULL);
    /* Same contract for the once-per-frame label hook. */
    ASSERT_TRUE("post_resolve_overlays_fn wired in frame 2",
                frame2.post_resolve_overlays_fn != NULL);
    ASSERT_TRUE("post_resolve_overlays_user_data non-NULL",
                frame2.post_resolve_overlays_user_data != NULL);
}

/* Audit #14 prep: the replay-fade overlay is the largest of the
 * controller's overlay reaches. When replay is OFF, no fade plumbing
 * should be wired (no post_fill_fn for fades, base_limit at zero,
 * empty batch ring). Pin this so an extraction that pushes fade
 * machinery into src/scene/ keeps the inactive-replay path costless. */
static void test_display_frame_no_replay_means_no_fade_plumbing(void) {
    printf("--- imrepl_ctrl no-replay fade gating ---\n");
    prepare_display_fixture();
    /* Force a polygon-replay mode so the tess-preview overlay (which
     * also wires post_fill_fn) is OFF; we want a clean signal that no
     * fade overhead is present when replay itself is inactive. */
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;
    replay_state_mut()->mode  = REPLAY_MODE_POLYGON;

    glr_ctrl_display_frame();

    ASSERT_INT("replay inactive across the frame",
               replay_state_view().active, 0);
    ASSERT_INT("replay fade plan inactive",
               g_replay_fade_plan.active, 0);
    ASSERT_INT("replay fade base limit zero",
               g_replay_fade_plan.base_limit, 0);
    ASSERT_INT("replay fade batch ring empty",
               g_replay_fade_plan.batch_count, 0);
    ASSERT_INT("tess preview not active in polygon mode",
               g_replay_fade_plan.tess_preview_active, 0);
    /* post_fill_fn may still be NULL or set to a no-op overlay; the
     * meaningful contract is that nothing fade-related is queued.
     * post_overlays_fn is always wired (the guides hook). */
    ASSERT_TRUE("post_overlays_fn still wired (guides) even without replay",
                g_last_scene_config.post_overlays_fn != NULL);
}

static void test_display_frame_follows_replay_line_after_tick(void) {
    int follow_doc_line = -1;
    int visible_lines = -1;

    printf("--- imrepl_ctrl replay follow after tick ---\n");

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(800, 230);
    ui_state_code_panel_mut()->panel_frac = 0.5f;
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_state_presentation_mut()->show_vertex_indices = 0;
    glr_ctrl_sync_ui_chrome();

    for (int i = 0; i < 30; i++) {
        char line[64];
        snprintf(line, sizeof(line), "glVertex3f(%d, 0, 0);", i);
        editor_feed_line(line);
    }
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    replay_state_mut()->mode = REPLAY_MODE_VERTEX;
    replay_state_mut()->pc = 25;
    replay_state_mut()->src_line_idx = 24;
    replay_state_mut()->last_src_line = 24;
    replay_state_mut()->accum = 0.99f;
    replay_state_mut()->speed = 1.0f;

    editor_scroll_set(0);
    editor_scroll_follow_cursor_set(0);

    glr_ctrl_tick();
    ASSERT_INT("tick advances replay to the next source line",
               replay_src_line(), 25);
    ASSERT_INT("tick alone does not move scroll",
               editor_scroll(), 0);

    glr_ctrl_display_frame();

    ASSERT_TRUE("display publishes a valid UI snapshot",
                g_last_ui_snapshot_valid != 0);
    ASSERT_INT("snapshot carries the new replay source line",
               g_last_ui_snapshot.replay.src_line_idx, 25);
    ASSERT_TRUE("display keeps the new replay line visible",
                glr_ctrl_code_panel_apply_scroll_follow_for_test(
                    &g_last_ui_snapshot, &follow_doc_line, &visible_lines));
    ASSERT_TRUE("display scrolls the code panel down for replay",
                editor_scroll() > 0);
    ASSERT_TRUE("follow target remains inside the viewport",
                follow_doc_line >= editor_scroll() &&
                follow_doc_line < editor_scroll() + visible_lines);
}

/* Cursor on a glPushAttrib line colours exactly the parsed mask's GL_*_BIT
 * tokens, confined to the (...) argument range — a bit token mentioned in a
 * trailing comment is never coloured — and the saved-setter line and matching
 * pop bracket get their highlights. */
static void test_push_attrib_bit_token_highlights(void) {
    printf("--- imrepl_ctrl glPushAttrib bit-token highlights ---\n");

    glr_ctrl_reset_all();
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("glPushAttrib(GL_CURRENT_BIT); // GL_LIGHTING_BIT note");
    editor_feed_line("glColor3f(0, 1, 0);");
    editor_feed_line("glPopAttrib();");

    glr_ctrl_set_edit_line(1);
    editor_insert_mode_set(0);
    glr_ctrl_push_highlights();

    const UiHighlightList *list = editor_state_highlights();
    const char *text = editor_buffer_view_line(editor_buffer_view(), 1);
    const char *cmt = text ? strstr(text, "//") : NULL;
    int comment_start = cmt ? (int)(cmt - text) : -1;
    int token_count = 0;
    int tokens_in_comment = 0;
    for (int i = 0; list && i < list->count; i++) {
        const UiHighlight *h = &list->items[i];
        if (h->kind != HIGHLIGHT_ATTRIB_BIT_TOKEN)
            continue;
        token_count++;
        if (comment_start >= 0 && h->char_start >= comment_start)
            tokens_in_comment++;
    }
    ASSERT_TRUE("push line has canonical text", text != NULL);
    ASSERT_TRUE("trailing comment kept in canonical text", cmt != NULL);
    ASSERT_INT("exactly one bit token coloured (the parsed mask)",
               token_count, 1);
    ASSERT_INT("no token coloured inside the trailing comment",
               tokens_in_comment, 0);
    ASSERT_INT("saved setter line gets the attrib-state marker",
               count_highlight_kind_on_line(HIGHLIGHT_ATTRIB_STATE, 0), 1);
    ASSERT_INT("matching pop is bracket-highlighted",
               count_highlight_kind_on_line(HIGHLIGHT_MATCHING_PUSH_MATRIX, 3), 1);

    /* Cursor on the pop: scoped setter marked, push's tokens still confined
     * to the push line's argument range. */
    glr_ctrl_set_edit_line(3);
    editor_insert_mode_set(0);
    glr_ctrl_push_highlights();
    ASSERT_INT("pop cursor marks the scoped setter it reverts",
               count_highlight_kind_on_line(HIGHLIGHT_ATTRIB_STATE, 2), 1);
    ASSERT_INT("pop cursor bracket-highlights the push",
               count_highlight_kind_on_line(HIGHLIGHT_MATCHING_PUSH_MATRIX, 1), 1);

    /* The compact all-bits alias has no single per-bit token hue, but its
     * resolved union still drives every saved-setter marker. */
    glr_ctrl_reset_all();
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("glLineWidth(2);");
    editor_feed_line("glPushAttrib(GL_ALL_ATTRIB_BITS);");
    editor_feed_line("glPopAttrib();");
    glr_ctrl_set_edit_line(2);
    editor_insert_mode_set(0);
    glr_ctrl_push_highlights();

    list = editor_state_highlights();
    token_count = 0;
    for (int i = 0; list && i < list->count; i++) {
        if (list->items[i].kind == HIGHLIGHT_ATTRIB_BIT_TOKEN)
            token_count++;
    }
    ASSERT_INT("all alias has no ambiguous per-bit token colour",
               token_count, 0);
    ASSERT_INT("all alias marks CURRENT setter",
               count_highlight_kind_on_line(HIGHLIGHT_ATTRIB_STATE, 0), 1);
    ASSERT_INT("all alias marks LINE setter",
               count_highlight_kind_on_line(HIGHLIGHT_ATTRIB_STATE, 1), 1);
}

/* Cursor on a glBegin brackets its glEnd and vice versa, the same way the
 * matrix and attrib stacks pair. Two sequential primitive blocks, so a
 * mispaired matcher would light the wrong partner rather than nothing. */
static void test_begin_end_bracket_highlights(void) {
    printf("--- imrepl_ctrl glBegin/glEnd bracket highlights ---\n");

    glr_ctrl_reset_all();
    editor_feed_line("glBegin(GL_LINES);");
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("glEnd();");
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glColor3f(0, 0, 1);");
    editor_feed_line("glEnd();");

    glr_ctrl_set_edit_line(0);
    editor_insert_mode_set(0);
    glr_ctrl_push_highlights();
    ASSERT_INT("begin cursor brackets its own end",
               count_highlight_kind_on_line(HIGHLIGHT_MATCHING_PUSH_MATRIX, 2), 1);
    ASSERT_INT("begin cursor leaves the later block's end alone",
               count_highlight_kind_on_line(HIGHLIGHT_MATCHING_PUSH_MATRIX, 5), 0);

    glr_ctrl_set_edit_line(5);
    editor_insert_mode_set(0);
    glr_ctrl_push_highlights();
    ASSERT_INT("end cursor brackets its own begin",
               count_highlight_kind_on_line(HIGHLIGHT_MATCHING_PUSH_MATRIX, 3), 1);
    ASSERT_INT("end cursor leaves the earlier block's begin alone",
               count_highlight_kind_on_line(HIGHLIGHT_MATCHING_PUSH_MATRIX, 0), 0);
}

static void test_replay_call_site_highlights_are_pushed(void) {
    printf("--- imrepl_ctrl replay call-site highlights ---\n");

    glr_ctrl_reset_all();
    editor_feed_line("func1(a, b) {");
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(a, b, 0);");
    editor_feed_line("glEnd();");
    editor_feed_line("}");
    editor_feed_line("func0(scale) {");
    editor_feed_line("func1(scale, scale + 1);");
    editor_feed_line("}");
    editor_feed_line("func0(2);");
    editor_feed_line("func0(4);");
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    replay_state_mut()->state = REPLAY_PAUSED;
    replay_state_mut()->mode = REPLAY_MODE_VERTEX;

    replay_advance(repl_state_flat_program_view());
    glr_ctrl_push_highlights();

    ASSERT_INT("first nested replay PC highlights body line",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_PC, 2), 1);
    ASSERT_INT("first nested replay highlights immediate call site",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_SITE, 6), 1);
    ASSERT_INT("first nested replay highlights root call site",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_ROOT_CALL_SITE, 8), 1);
    ASSERT_INT("first nested replay has no root marker on second outer call",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_ROOT_CALL_SITE, 9), 0);

    replay_advance(repl_state_flat_program_view());
    glr_ctrl_push_highlights();

    ASSERT_INT("second nested replay still highlights immediate call site",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_SITE, 6), 1);
    ASSERT_INT("second nested replay moves root marker to second outer call",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_ROOT_CALL_SITE, 9), 1);
    ASSERT_INT("second nested replay clears prior root marker",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_ROOT_CALL_SITE, 8), 0);

    replay_stop();
    glr_ctrl_push_highlights();
    ASSERT_INT("inactive replay clears immediate call-site highlight",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_SITE, 6), 0);
}

/* Req 5: during replay the affecting-transform highlight tracks the
 * replay-focused vertex (via the req-4 exact-flat resolver), not the edit
 * cursor — and each expansion shows only its own in-scope transforms. */
static void test_replay_focus_vertex_affecting_transforms(void) {
    printf("--- imrepl_ctrl replay focus-vertex affecting transforms ---\n");

    glr_ctrl_reset_all();
    editor_feed_line("func0() {");          /* 0 */
    editor_feed_line("glBegin(GL_POINTS);"); /* 1 */
    editor_feed_line("glVertex3f(0, 0, 0);"); /* 2 */
    editor_feed_line("glEnd();");            /* 3 */
    editor_feed_line("}");                   /* 4 */
    editor_feed_line("glTranslatef(1, 0, 0);"); /* 5 */
    editor_feed_line("func0();");            /* 6 */
    editor_feed_line("glRotatef(45, 0, 0, 1);"); /* 7 */
    editor_feed_line("glTranslatef(2, 0, 0);"); /* 8 */
    editor_feed_line("func0();");            /* 9 */
    repl_flatten_commands(editor_state_edit_line());

    /* Park the edit cursor on the in-body vertex (line 2). Off replay, its
     * flat resolver would union BOTH expansions → {5, 7, 8}; this lets the
     * test prove replay precedence (the first-vertex focus shows only {5}). */
    editor_insert_mode_set(0);
    editor_state_edit_line_set(2);

    replay_start();
    replay_state_mut()->state = REPLAY_PAUSED;
    replay_state_mut()->mode = REPLAY_MODE_VERTEX;

    /* First vertex: modelview is just translate@5. */
    replay_advance(repl_state_flat_program_view());
    glr_ctrl_push_highlights();
    ASSERT_INT("first replay vertex highlights its call-site translate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 5), 1);
    ASSERT_INT("first replay vertex does NOT show second-call rotate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 7), 0);
    ASSERT_INT("first replay vertex does NOT show second-call translate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 8), 0);

    /* Second vertex: modelview accumulated translate@5, rotate@7, translate@8. */
    replay_advance(repl_state_flat_program_view());
    glr_ctrl_push_highlights();
    ASSERT_INT("second replay vertex shows first translate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 5), 1);
    ASSERT_INT("second replay vertex shows the rotate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 7), 1);
    ASSERT_INT("second replay vertex shows second translate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 8), 1);

    /* Off replay the edit-cursor set now follows overlay_scope. Last-instance
     * resolves the LAST flat expansion of the cursor line — here the second
     * func0() call — so it shows that expansion's transform set: all three. */
    replay_stop();
    glr_state_presentation_mut()->overlay_scope = OVERLAY_SCOPE_LAST_INSTANCE;
    glr_ctrl_push_highlights();
    ASSERT_INT("post-replay last-instance shows first translate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 5), 1);
    ASSERT_INT("post-replay last-instance shows second-call rotate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 7), 1);
    ASSERT_INT("post-replay last-instance shows second-call translate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 8), 1);

    /* All-instances restores the old source-line union behavior. */
    glr_state_presentation_mut()->overlay_scope = OVERLAY_SCOPE_ALL_INSTANCES;
    glr_ctrl_push_highlights();
    ASSERT_INT("post-replay all-instances shows first translate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 5), 1);
    ASSERT_INT("post-replay all-instances shows the rotate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 7), 1);
    ASSERT_INT("post-replay all-instances shows second translate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 8), 1);
}

/* Last-instance scope resolves the cursor line's LAST flat expansion — the
 * copy whose loop/call state the variable panel still holds. Scoping the
 * first call in a glPushMatrix/glPopMatrix pair makes the two expansions
 * disagree, so this pins which one the highlight set comes from. */
static void test_overlay_scope_last_instance_affecting_transforms(void) {
    printf("--- imrepl_ctrl last-instance affecting transforms ---\n");

    glr_ctrl_reset_all();
    editor_feed_line("func0() {");               /* 0 */
    editor_feed_line("glBegin(GL_POINTS);");     /* 1 */
    editor_feed_line("glVertex3f(0, 0, 0);");    /* 2 */
    editor_feed_line("glEnd();");                /* 3 */
    editor_feed_line("}");                       /* 4 */
    editor_feed_line("glPushMatrix();");         /* 5 */
    editor_feed_line("glRotatef(45, 0, 0, 1);"); /* 6 */
    editor_feed_line("func0();");                /* 7 */
    editor_feed_line("glPopMatrix();");          /* 8 */
    editor_feed_line("glTranslatef(2, 0, 0);");  /* 9 */
    editor_feed_line("func0();");                /* 10 */
    repl_flatten_commands(editor_state_edit_line());

    editor_insert_mode_set(0);
    editor_state_edit_line_set(2);

    glr_state_presentation_mut()->overlay_scope = OVERLAY_SCOPE_LAST_INSTANCE;
    glr_ctrl_push_highlights();
    ASSERT_INT("last-instance shows the second call's translate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 9), 1);
    ASSERT_INT("last-instance hides the popped rotate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 6), 0);

    /* All-instances unions both expansions, so the popped rotate returns. */
    glr_state_presentation_mut()->overlay_scope = OVERLAY_SCOPE_ALL_INSTANCES;
    glr_ctrl_push_highlights();
    ASSERT_INT("all-instances shows the popped rotate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 6), 1);
    ASSERT_INT("all-instances still shows the translate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 9), 1);
}

/* A glutSolid* is a replay draw anchor too (it emits no REPL vertex but vertex
 * stepping stops on it). The replay affecting-transform highlight must reach
 * the transforms shaping the focused glut solid, just like for a vertex. */
static void test_replay_focus_glut_solid_affecting_transforms(void) {
    printf("--- imrepl_ctrl replay focus glut-solid affecting transforms ---\n");

    glr_ctrl_reset_all();
    editor_feed_line("glTranslatef(1, 0, 0);");   /* 0 */
    editor_feed_line("glutSolidCube(1);");         /* 1 */
    editor_feed_line("glRotatef(45, 0, 0, 1);");   /* 2 */
    editor_feed_line("glutSolidSphere(1, 8, 8);"); /* 3 */
    repl_flatten_commands(editor_state_edit_line());
    /* flat: TRANSLATE(0) GLUT_CUBE(1) ROTATE(2) GLUT_SPHERE(3) */

    /* Park the edit cursor off the solids so the highlight is provably driven
     * by the replay anchor, not the cursor. */
    editor_insert_mode_set(0);
    editor_state_edit_line_set(0);

    replay_start();
    replay_state_mut()->state = REPLAY_PAUSED;
    replay_state_mut()->mode = REPLAY_MODE_VERTEX;

    /* First solid (cube): only translate@0 is in scope. */
    replay_advance(repl_state_flat_program_view());
    glr_ctrl_push_highlights();
    ASSERT_INT("cube anchor highlights the translate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 0), 1);
    ASSERT_INT("cube anchor does NOT show the later rotate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 2), 0);

    /* Second solid (sphere): translate@0 and rotate@2 are both in scope. */
    replay_advance(repl_state_flat_program_view());
    glr_ctrl_push_highlights();
    ASSERT_INT("sphere anchor shows the translate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 0), 1);
    ASSERT_INT("sphere anchor shows the rotate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 2), 1);

    replay_stop();
}

/* Audit #41 prep: route_numeric_swatch_hit open-codes 68 lines of
 * compile + parse + ReplCompiledChange construction + apply + reload
 * inside the controller's router. The audit proposes extracting that
 * into a numeric_swatch_apply_step helper (an editor service or a peer
 * subsystem). To make the move safe, pin the OBSERVABLE swatch contract:
 *
 *  - Stepping up rewrites the line with a larger value.
 *  - Stepping down rewrites the line with a smaller value.
 *  - The change is undoable (single Ctrl+Z restores the original line).
 *  - The editor input buffer reflects the new line text post-commit.
 *  - The swatch is a no-op outside a numeric arg or in insert mode.
 *
 * We drive the static router directly (this TU includes glr_ctrl.c) by
 * constructing a UiHit and calling route_numeric_swatch_hit. */
static void seed_swatch_fixture(const char *line) {
    glr_ctrl_reset_all();
    repl_eval_init_predef_vars();
    editor_insert_mode_set(0);
    editor_feed_line(line);
    /* feed_line lands cursor at end of document; place it back on the
     * row we just typed. */
    editor_state_edit_line_set(0);
    editor_load_line_to_input(0);
}

static void seed_swatch_assignment_fixture(void) {
    glr_ctrl_reset_all();
    repl_eval_init_predef_vars();
    ui_state_viewport_set_size(800, 600);
    editor_insert_mode_set(0);
    editor_feed_line("float x;");
    editor_feed_line("x = 5;");
    editor_state_edit_line_set(1);
    editor_load_line_to_input(1);
}

static void seed_swatch_loop_fixture(void) {
    glr_ctrl_reset_all();
    repl_eval_init_predef_vars();
    ui_state_viewport_set_size(800, 600);
    editor_insert_mode_set(0);
    editor_feed_line("for(i, 0, 10) {");
    editor_feed_line("glVertex3f(1, 2*i, 3);");
    editor_feed_line("}");
    editor_state_edit_line_set(1);
    editor_load_line_to_input(1);
}

static void test_numeric_swatch_step_commits_line_and_undoes(void) {
    UiHit hit_up   = ui_hit_none();
    UiHit hit_down = ui_hit_none();
    EditorBufferView buf;
    char before[MAX_LINE_LEN];

    printf("--- imrepl_ctrl numeric swatch commit ---\n");

    /* Seed with two-decimal value so the commit's canonicalization
     * doesn't strip it down to "1" and shift the arg span. */
    seed_swatch_fixture("glVertex3f(1.5, 0, 0);");

    /* Cursor inside the '1.5' arg. The commit canonicalizes the
     * source line (re-emits via the parser) so the input buffer's
     * arg span and the assertion offsets are best measured from
     * what the input actually is, not the seed string. */
    editor_cursor_pos_set(13);

    buf = editor_buffer_view();
    snprintf(before, sizeof(before), "%s", editor_buffer_view_line(buf, 0));

    hit_up.kind     = UI_HIT_NUMERIC_SWATCH;
    hit_up.item_idx = 1;
    int rc = route_numeric_swatch_hit(&hit_up, 1.0f);
    ASSERT_INT("swatch step-up returns consumed", rc, 1);

    buf = editor_buffer_view();
    const char *after_up = editor_buffer_view_line(buf, 0);
    ASSERT_TRUE("swatch step-up modified the source line",
                strcmp(after_up, before) != 0);
    /* The new arg is greater than 1.5 — pin the direction without
     * pinning the exact step size (which lives in repl_eval). The
     * arg span shifts after re-emission, so re-derive it from the
     * post-commit input buffer rather than guessing offsets. */
    {
        EditorInputView in_after = editor_state_input();
        ReplNumericArgAtCursor d_after =
            repl_eval_numeric_arg_at_cursor(in_after.input, in_after.cursor_pos);
        ASSERT_TRUE("swatch step-up found numeric arg after commit",
                    d_after.found);
        ASSERT_TRUE("swatch step-up value is greater than 1.5",
                    d_after.value > 1.5f);
    }

    /* Step DOWN three times: from value > 1.5, back below 1.5. */
    hit_down.kind     = UI_HIT_NUMERIC_SWATCH;
    hit_down.item_idx = -1;
    route_numeric_swatch_hit(&hit_down, 1.0f);
    route_numeric_swatch_hit(&hit_down, 1.0f);
    {
        EditorInputView in_down = editor_state_input();
        ReplNumericArgAtCursor d_down =
            repl_eval_numeric_arg_at_cursor(in_down.input, in_down.cursor_pos);
        ASSERT_TRUE("swatch two steps down found numeric arg",
                    d_down.found);
        ASSERT_TRUE("swatch two steps down value is less than 1.5",
                    d_down.value < 1.5f);
    }

    /* Each swatch step is a discrete undo unit — three steps total
     * (one up, two down) so three Ctrl+Z restores the original. */
    editor_undo_pop_snapshot();
    editor_undo_pop_snapshot();
    editor_undo_pop_snapshot();
    buf = editor_buffer_view();
    ASSERT_STR("undo×3 restores the original line",
               editor_buffer_view_line(buf, 0), before);
}

static void test_numeric_swatch_step_commits_bare_assignment(void) {
    EditorBufferView buf;
    EditorInputView in;
    UiRenderSnapshot snap;
    const char *five;
    int x_idx;

    printf("--- imrepl_ctrl numeric swatch bare assignment ---\n");

    seed_swatch_assignment_fixture();
    in = editor_state_input();
    five = strchr(in.input, '5');
    ASSERT_TRUE("bare assignment swatch seed has rhs", five != NULL);
    if (five)
        editor_cursor_pos_set((int)(five - in.input));

    memset(&snap, 0, sizeof snap);
    glr_ctrl_build_ui_snapshot(&snap);
    ASSERT_INT("bare assignment swatch visible", snap.numeric_swatch.visible, 1);
    ASSERT_FLOAT("bare assignment swatch value", snap.numeric_swatch.value, 5.0f);

    ASSERT_INT("bare assignment swatch apply",
               editor_commit_apply_swatch_change(1, 1, 1.0f), 1);

    buf = editor_buffer_view();
    ASSERT_STR("bare assignment swatch rewrites line",
               editor_buffer_view_line(buf, 1), "  x = 5.05;");

    x_idx = repl_eval_find_predef_var_idx("x");
    ASSERT_TRUE("bare assignment swatch x declared", x_idx >= 0);
    if (x_idx >= 0)
        ASSERT_FLOAT("bare assignment swatch updates x",
                     g_predef_vars[x_idx].value, 5.05f);
}

static void test_numeric_swatch_visible_inside_loop_expr(void) {
    UiRenderSnapshot snap;
    EditorInputView in;
    const char *one;
    const char *two;
    const char *three;

    printf("--- imrepl_ctrl numeric swatch loop expr visibility ---\n");

    seed_swatch_loop_fixture();
    in = editor_state_input();
    one = strchr(in.input, '1');
    ASSERT_TRUE("loop swatch seed has first arg", one != NULL);
    if (one) {
        editor_cursor_pos_set((int)(one - in.input));
        memset(&snap, 0, sizeof snap);
        glr_ctrl_build_ui_snapshot(&snap);
        ASSERT_INT("first arg swatch visible inside loop",
                   snap.numeric_swatch.visible, 1);
        ASSERT_FLOAT("first arg swatch value inside loop",
                     snap.numeric_swatch.value, 1.0f);
    }

    seed_swatch_loop_fixture();
    in = editor_state_input();
    two = strstr(in.input, "2*i");
    ASSERT_TRUE("loop swatch seed has literal multiplied by i", two != NULL);
    if (two) {
        editor_cursor_pos_set((int)(two - in.input));
        memset(&snap, 0, sizeof snap);
        glr_ctrl_build_ui_snapshot(&snap);
        ASSERT_INT("expr literal swatch visible inside loop",
                   snap.numeric_swatch.visible, 1);
        ASSERT_FLOAT("expr literal swatch value inside loop",
                     snap.numeric_swatch.value, 2.0f);
    }

    seed_swatch_loop_fixture();
    in = editor_state_input();
    three = strrchr(in.input, '3');
    ASSERT_TRUE("loop swatch seed has last arg", three != NULL);
    if (three) {
        editor_cursor_pos_set((int)(three - in.input));
        memset(&snap, 0, sizeof snap);
        glr_ctrl_build_ui_snapshot(&snap);
        ASSERT_INT("last arg swatch visible inside loop",
                   snap.numeric_swatch.visible, 1);
        ASSERT_FLOAT("last arg swatch value inside loop",
                     snap.numeric_swatch.value, 3.0f);
    }
}

static void test_numeric_swatch_step_commits_loop_expr_literal(void) {
    EditorBufferView buf;
    EditorInputView in;
    const char *two;
    const char *line;

    printf("--- imrepl_ctrl numeric swatch loop expr commit ---\n");

    seed_swatch_loop_fixture();
    in = editor_state_input();
    two = strstr(in.input, "2*i");
    ASSERT_TRUE("loop expr swatch seed has literal", two != NULL);
    if (two)
        editor_cursor_pos_set((int)(two - in.input));

    ASSERT_INT("loop expr swatch apply",
               editor_commit_apply_swatch_change(1, 1, 1.0f), 1);

    buf = editor_buffer_view();
    line = editor_buffer_view_line(buf, 1);
    ASSERT_STR("loop expr swatch preserves indentation",
               line, "    glVertex3f(1, 2.05*i, 3);");
}

static void test_numeric_swatch_no_op_outside_numeric_arg(void) {
    UiHit hit_up = ui_hit_none();
    EditorBufferView buf;
    char before[MAX_LINE_LEN];

    seed_swatch_fixture("// hello world");
    /* Cursor mid-comment — no numeric arg available. */
    editor_cursor_pos_set(6);
    buf = editor_buffer_view();
    snprintf(before, sizeof(before), "%s", editor_buffer_view_line(buf, 0));

    hit_up.kind     = UI_HIT_NUMERIC_SWATCH;
    hit_up.item_idx = 1;
    int rc = route_numeric_swatch_hit(&hit_up, 1.0f);
    /* Consumed (returns 1) — the swatch hit is the router's
     * responsibility — but no change to the line. */
    ASSERT_INT("swatch on comment still consumed", rc, 1);
    buf = editor_buffer_view();
    ASSERT_STR("swatch on comment leaves line unchanged",
               editor_buffer_view_line(buf, 0), before);
}

static void test_numeric_swatch_no_op_in_insert_mode(void) {
    UiHit hit_up = ui_hit_none();
    EditorBufferView buf;
    char before[MAX_LINE_LEN];

    seed_swatch_fixture("glVertex3f(1.0, 0, 0);");
    editor_cursor_pos_set(12);
    /* Flip to insert mode — swatch should refuse. */
    editor_insert_mode_set(1);

    buf = editor_buffer_view();
    snprintf(before, sizeof(before), "%s", editor_buffer_view_line(buf, 0));

    hit_up.kind     = UI_HIT_NUMERIC_SWATCH;
    hit_up.item_idx = 1;
    int rc = route_numeric_swatch_hit(&hit_up, 1.0f);
    ASSERT_INT("swatch in insert mode still consumed", rc, 1);
    buf = editor_buffer_view();
    ASSERT_STR("swatch in insert mode leaves line unchanged",
               editor_buffer_view_line(buf, 0), before);
}

/* The scale arg multiplies the value-derived base step. Pin that one
 * step at the shared coarse scale moves the value ten base-steps, and
 * the shared fine scale moves a fifth of one base-step. */
static float swatch_value_after_one_step(const char *line, int cursor,
                                         float scale) {
    UiHit hit_up = ui_hit_none();
    EditorInputView in;
    ReplNumericArgAtCursor d;

    seed_swatch_fixture(line);
    editor_cursor_pos_set(cursor);
    hit_up.kind     = UI_HIT_NUMERIC_SWATCH;
    hit_up.item_idx = 1;
    route_numeric_swatch_hit(&hit_up, scale);

    in = editor_state_input();
    d = repl_eval_numeric_arg_at_cursor(in.input, in.cursor_pos);
    ASSERT_TRUE("scaled swatch step found numeric arg", d.found);
    return d.value;
}

static void test_numeric_swatch_scale_coarse_and_fine(void) {
    float base, coarse, fine;

    printf("--- imrepl_ctrl numeric swatch coarse/fine scale ---\n");

    /* Base step for a |value| < 10 arg is 0.05 (repl_eval_swatch_step). */
    base   = swatch_value_after_one_step("glVertex3f(1.5, 0, 0);", 13, 1.0f);
    coarse = swatch_value_after_one_step(
        "glVertex3f(1.5, 0, 0);", 13, GLR_ADJUST_COARSE_SCALE);
    fine   = swatch_value_after_one_step(
        "glVertex3f(1.5, 0, 0);", 13, GLR_ADJUST_FINE_SCALE);

    ASSERT_FLOAT("base step is +0.05", base, 1.55f);
    ASSERT_FLOAT("coarse (x10) step is +0.5", coarse, 2.0f);
    ASSERT_FLOAT("fine (x1/5) step is +0.01", fine, 1.51f);
}

static int first_flat_vertex_x(float *out_x) {
    FlatProgramView flat = repl_state_flat_program_view();
    if (!out_x)
        return 0;
    for (int i = 0; i < flat.cmd_count; i++) {
        if (flat.cmds[i].valid && flat.cmds[i].type == CMD_VERTEX3F) {
            *out_x = flat.cmds[i].args[0];
            return 1;
        }
    }
    return 0;
}

static void test_variable_panel_t_change_reflattens_when_time_paused(void) {
    printf("--- variable panel t drag re-flattens while time paused ---\n");

    glr_ctrl_reset_all();
    g_t_idx = -1; /* keep the scene-render stub from mutating t in this test */

    int t_idx = repl_eval_find_predef_var_idx("t");
    ASSERT_TRUE("t predef exists", t_idx >= 0);
    ASSERT_TRUE("t is visible in variable panel rows", t_idx >= 0);
    repl_state_variables_mut()->time_playing = 0;
    g_predef_vars_mut[t_idx].value = 0.0f;

    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(t, 0, 0);");
    editor_feed_line("glEnd();");
    repl_flatten_commands(editor_state_edit_line());
    repl_state_flat_program_clear_dirty();

    float x = -1.0f;
    ASSERT_TRUE("seed flat vertex exists", first_flat_vertex_x(&x));
    ASSERT_FLOAT("seed flat vertex uses initial t", x, 0.0f);

    variable_panel_handle_drag_begin(t_idx, 0, 0);
    ASSERT_TRUE("variable-panel t motion consumed",
                glr_ctrl_router_handle_variable_panel_motion(50, 0));
    ASSERT_FLOAT("variable panel changed t", g_predef_vars[t_idx].value, 5.0f);
    /* glVertex3f(t, ...) is a value-only use of t, so the drag routes to
     * args_dirty_mask (rebake) rather than the full flat-dirty flag; the
     * frame gate below still rebuilds from it. */
    ASSERT_INT("panel t change leaves full flag clean",
               repl_state_flat_program_dirty(), 0);
    ASSERT_INT("panel t change sets t's args-dirty bit",
               (int)((repl_state_flat_program_args_dirty_mask()
                      >> t_idx) & 1u), 1);

    glr_ctrl_display_frame();

    ASSERT_TRUE("post-frame flat vertex exists", first_flat_vertex_x(&x));
    ASSERT_FLOAT("post-frame flat vertex uses panel t", x, 5.0f);
    ASSERT_FLOAT("time remains paused at panel value",
                 g_predef_vars[t_idx].value, 5.0f);
    ASSERT_INT("auto time remains off",
               repl_state_variables().time_playing, 0);

    repl_state_flat_program_clear_dirty();
    variable_panel_handle_drag_begin(t_idx, 0, 0);
    ASSERT_TRUE("same-value variable-panel t motion consumed",
                glr_ctrl_router_handle_variable_panel_motion(0, 0));
    ASSERT_FLOAT("same-value variable-panel leaves t unchanged",
                 g_predef_vars[t_idx].value, 5.0f);
    ASSERT_INT("same-value variable-panel leaves flat clean",
               repl_state_flat_program_dirty(), 0);
    ASSERT_INT("same-value variable-panel leaves args-dirty clean",
               (int)repl_state_flat_program_args_dirty_mask(), 0);
}

/* scene_execute_adapter is called by render.c on both the main fill
 * pass and the render3d_probe_eye_dist feedback pass. The probe pass
 * runs every frame in ortho/projection-transition mode; before the
 * Render3dExecutePurpose wiring its execute_fn invocation mutated REPL
 * state (predef vars, scratch arrays, light enables, clear_color)
 * the same as the main fill, so the user's `t = t + 1` style code
 * advanced twice per frame and the probe's glEnable(GL_LIGHT0) /
 * glClearColor() leaked across frames (the frame-end restore in
 * glr_ctrl_display_frame only snapshots predef + scratch, not the
 * persistent render state).
 *
 * This test exercises the adapter directly and pins the invariant:
 * DEPTH_PROBE plus the hidden/depth wireframe passes don't mutate
 * predef vars or scratch arrays; MAIN_FILL and the visible wireframe
 * pass still do (would-be-regression for accidentally suppressing the
 * real render path, including wireframe mode). Clear-color and
 * light-enable side effects are intentionally excluded here — the
 * parser clamps glClearColor channels to 0.15 max, which complicates
 * a clean test signal, but the snapshot/restore path covers them the
 * same way it covers the predef/scratch state. */
static void test_auxiliary_scene_pass_side_effects(void) {
    printf("--- auxiliary scene pass side effects (#2 P1 review) ---\n");

#ifndef GL_STUBS
    printf("Run `make test-glr-ctrl` for scene-pass adapter coverage.\n");
    return;
#endif

    glr_ctrl_reset_all();
    editor_feed_line("float probevar;");
    editor_feed_line("probevar = probevar + 1;");
    editor_feed_line("A[0] = A[0] + 1;");
    editor_feed_line("glVertex3f(0, 0, 0);");

    int probevar_idx = repl_eval_find_predef_var_idx("probevar");
    ASSERT_TRUE("probevar declared", probevar_idx >= 0);
    int scratch_a_idx = repl_eval_scratch_array_index("A");
    ASSERT_TRUE("A scratch array index", scratch_a_idx >= 0);

    /* Force a known starting state *before* re-flattening — the
     * executor applies precomputed `args[0]` directly (the comment in
     * executor.c on CMD_VAR_ASSIGN explains why: re-evaluating at exec
     * time would double-apply self-referential assigns). So the
     * post-execute value depends on what predef state was live when
     * flatten last ran. Forcing probevar=0 and A[0]=0 before the
     * re-flatten pins the per-execute advancement to exactly +1. */
    g_predef_vars_mut[probevar_idx].value = 0.0f;
    float scratch_zero[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN] = {0};
    repl_eval_restore_scratch_arrays(scratch_zero);
    repl_flatten_commands(0);

    /* Sanity check: exactly one probevar/A[0] assign in the flat
     * program so the post-execute advancement is unambiguous. */
    int probevar_assigns_per_execute = 0;
    int a0_assigns_per_execute       = 0;
    {
        FlatProgramView prog = repl_state_flat_program_view();
        int flat_count = repl_state_flat_program_count();
        for (int i = 0; i < flat_count; i++) {
            if (prog.cmds[i].type == CMD_VAR_ASSIGN &&
                prog.cmds[i].var_idx == probevar_idx)
                probevar_assigns_per_execute++;
            if (prog.cmds[i].type == CMD_SCRATCH_ASSIGN &&
                (int)prog.cmds[i].args[0] == scratch_a_idx &&
                (int)prog.cmds[i].args[1] == 0)
                a0_assigns_per_execute++;
        }
    }
    ASSERT_INT("one probevar assign per execute",
               probevar_assigns_per_execute, 1);
    ASSERT_INT("one A[0] assign per execute",
               a0_assigns_per_execute, 1);

    /* Flatten itself eagerly applied the assignments (probevar -> 1,
     * A[0] -> 1) when it computed the precomputed args[0]; reset back
     * to the baseline that flatten saw so the test's before/after
     * deltas are clean. The flat program's args[0] still encodes
     * "what probevar/A[0] would become after one execute" = 1. */
    g_predef_vars_mut[probevar_idx].value = 0.0f;
    repl_eval_restore_scratch_arrays(scratch_zero);

    float scratch_before;
    repl_eval_scratch_get(scratch_a_idx, 0, &scratch_before);
    float predef_before = g_predef_vars[probevar_idx].value;

    /* Probe call: state must not change. */
    Render3dExecuteContext probe_ctx = { .purpose = RENDER3D_EXEC_DEPTH_PROBE };
    scene_execute_adapter(&probe_ctx, NULL);

    float predef_after_probe = g_predef_vars[probevar_idx].value;
    float scratch_after_probe;
    repl_eval_scratch_get(scratch_a_idx, 0, &scratch_after_probe);

    ASSERT_FLOAT("probe: probevar unchanged", predef_after_probe, predef_before);
    ASSERT_FLOAT("probe: A[0] unchanged", scratch_after_probe, scratch_before);

    /* Main fill call: state SHOULD mutate. */
    Render3dExecuteContext fill_ctx = { .purpose = RENDER3D_EXEC_MAIN_FILL };
    scene_execute_adapter(&fill_ctx, NULL);

    float predef_after_fill = g_predef_vars[probevar_idx].value;
    float scratch_after_fill;
    repl_eval_scratch_get(scratch_a_idx, 0, &scratch_after_fill);

    ASSERT_FLOAT("fill: probevar advanced once",
                 predef_after_fill, predef_before + 1.0f);
    ASSERT_FLOAT("fill: A[0] advanced once",
                 scratch_after_fill, scratch_before + 1.0f);

    /* Wireframe hidden/depth passes are auxiliary; visible lines are
     * the one side-effecting execution that replaces MAIN_FILL when
     * wireframe mode is enabled. */
    g_predef_vars_mut[probevar_idx].value = 0.0f;
    repl_eval_restore_scratch_arrays(scratch_zero);
    predef_before = g_predef_vars[probevar_idx].value;
    repl_eval_scratch_get(scratch_a_idx, 0, &scratch_before);

    Render3dExecuteContext hidden_ctx = {
        .purpose = RENDER3D_EXEC_WIREFRAME_HIDDEN_LINES
    };
    scene_execute_adapter(&hidden_ctx, NULL);
    ASSERT_FLOAT("wire hidden: probevar unchanged",
                 g_predef_vars[probevar_idx].value, predef_before);
    repl_eval_scratch_get(scratch_a_idx, 0, &scratch_after_probe);
    ASSERT_FLOAT("wire hidden: A[0] unchanged",
                 scratch_after_probe, scratch_before);

    Render3dExecuteContext depth_ctx = {
        .purpose = RENDER3D_EXEC_WIREFRAME_DEPTH_FILL
    };
    scene_execute_adapter(&depth_ctx, NULL);
    ASSERT_FLOAT("wire depth: probevar unchanged",
                 g_predef_vars[probevar_idx].value, predef_before);
    repl_eval_scratch_get(scratch_a_idx, 0, &scratch_after_probe);
    ASSERT_FLOAT("wire depth: A[0] unchanged",
                 scratch_after_probe, scratch_before);

    Render3dExecuteContext visible_ctx = {
        .purpose = RENDER3D_EXEC_WIREFRAME_VISIBLE_LINES
    };
    scene_execute_adapter(&visible_ctx, NULL);
    ASSERT_FLOAT("wire visible: probevar advanced once",
                 g_predef_vars[probevar_idx].value, predef_before + 1.0f);
    repl_eval_scratch_get(scratch_a_idx, 0, &scratch_after_fill);
    ASSERT_FLOAT("wire visible: A[0] advanced once",
                 scratch_after_fill, scratch_before + 1.0f);
}

static void test_wireframe_renderer_ignores_user_draw_state(void) {
    printf("--- wireframe renderer ignores user draw state ---\n");

#ifndef GL_STUBS
    printf("Run `make test-glr-ctrl` for wireframe renderer coverage.\n");
    return;
#else
    glr_ctrl_reset_all();
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("glEnable(GL_LIGHTING);");
    editor_feed_line("glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);");
    editor_feed_line("glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);");
    editor_feed_line("glDepthFunc(GL_GREATER);");
    editor_feed_line("glDepthMask(GL_FALSE);");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glEdgeFlag(GL_FALSE);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glEnd();");
    repl_flatten_commands(0);

#ifdef GL_STUBS
    gl_stub_counts_reset();
    Render3dExecuteContext visible_ctx = {
        .purpose = RENDER3D_EXEC_WIREFRAME_VISIBLE_LINES
    };
    scene_execute_adapter(&visible_ctx, NULL);

    ASSERT_INT("wire renderer ignores user color",
               (int)gl_stub_counts[GL_STUB_glColor4f], 0);
    ASSERT_INT("wire renderer ignores user GL enable",
               (int)gl_stub_counts[GL_STUB_glEnable], 0);
    ASSERT_INT("wire renderer ignores user color material",
               (int)gl_stub_counts[GL_STUB_glColorMaterial], 0);
    ASSERT_INT("wire renderer ignores user color mask",
               (int)gl_stub_counts[GL_STUB_glColorMask], 0);
    ASSERT_INT("wire renderer ignores user depth func",
               (int)gl_stub_counts[GL_STUB_glDepthFunc], 0);
    ASSERT_INT("wire renderer ignores user depth mask",
               (int)gl_stub_counts[GL_STUB_glDepthMask], 0);
    ASSERT_INT("wire renderer honors edge flags",
               (int)gl_stub_counts[GL_STUB_glEdgeFlag], 1);
    ASSERT_INT("wire renderer emits one primitive",
               (int)gl_stub_counts[GL_STUB_glBegin], 1);
    ASSERT_INT("wire renderer emits vertices",
               (int)gl_stub_counts[GL_STUB_glVertex3f], 3);
#endif /* GL_STUBS */
#endif /* !GL_STUBS */
}

/* Regression: loading an example resets the light_theme *name* to the
 * default, and that reset must ALSO re-apply the theme to the app-state
 * lights[] (positions / colors / eye-space). Before the fix
 * glr_ctrl_reset_example_chrome reset the cfg field directly — bypassing
 * the render3d_lights_apply_theme hook in glr_config_set — so the lights[]
 * array (and the light indicators that read it) stayed on the *previous*
 * theme: the name said default but the geometry stayed e.g. SOLAR.
 *
 * The dimensional light table is app-owned (GlrRenderState.lights); the
 * REPL pipeline owns only the enable bitmask, so this checks app state. */
static void test_example_reset_reapplies_light_theme(void) {
    printf("--- glr_ctrl example reset re-applies light theme to lights[] ---\n");

    Render3dLight expected_default[MAX_LIGHTS];
    render3d_lights_apply_theme(expected_default, LIGHT_THEME_DEFAULT);

    /* Switch to a non-default theme through the real setter, which fires
     * the render3d_lights_apply_theme hook, so lights[] holds the SOLAR preset. */
    glr_config_set(GLR_CONFIG_LIGHT_THEME, LIGHT_THEME_SOLAR);
    ASSERT_INT("theme set to SOLAR",
               glr_config_get(GLR_CONFIG_LIGHT_THEME), LIGHT_THEME_SOLAR);
    ASSERT_TRUE("lights[] differ from default after switching to SOLAR",
                memcmp(glr_state_render_mut()->lights, expected_default,
                       sizeof(expected_default)) != 0);

    /* Example-load presentation reset, with no tag defaults (mask 0 applies
     * nothing), so light_theme returns to the built-in default. */
    glr_ctrl_reset_example_chrome(0u);

    ASSERT_INT("light_theme name reset to default",
               glr_config_get(GLR_CONFIG_LIGHT_THEME), LIGHT_THEME_DEFAULT);
    ASSERT_TRUE("lights[] re-applied to the default theme after example reset",
                memcmp(glr_state_render_mut()->lights, expected_default,
                       sizeof(expected_default)) == 0);
}

/* The light-split contract: Render3dRenderConfig.lights[] is assembled per frame
 * from two owners — the app-owned theme-seeded dimensional data
 * (GlrRenderState.lights: position / color / id / eye-space) merged with the
 * REPL-owned enable bitmask (ReplRenderState.light_enabled_mask). This drives
 * a frame (scene render is stubbed, so the executor never re-derives the mask)
 * and checks each slot's `.enabled` tracks the mask while the rest tracks the
 * theme. */
static void test_display_frame_merges_light_theme_and_enable_mask(void) {
    printf("--- imrepl_ctrl light merge (theme + enable mask) ---\n");
    prepare_display_fixture();

    Render3dLight theme[MAX_LIGHTS];
    render3d_lights_apply_theme(theme, LIGHT_THEME_SOLAR);

    /* App side: the theme seeds positions/colors. REPL side: only slots 0
     * and 2 are enabled by the program. */
    glr_config_set(GLR_CONFIG_LIGHT_THEME, LIGHT_THEME_SOLAR);
    repl_state_render_mut()->light_enabled_mask = (1u << 0) | (1u << 2);

    glr_ctrl_display_frame();

    ASSERT_INT("scene render called once", g_scene_render_calls, 1);
    /* `.enabled` comes from the REPL mask. */
    ASSERT_INT("merge: L0 enabled from mask", g_last_scene_config.lights[0].enabled, 1);
    ASSERT_INT("merge: L1 disabled (mask bit clear)", g_last_scene_config.lights[1].enabled, 0);
    ASSERT_INT("merge: L2 enabled from mask", g_last_scene_config.lights[2].enabled, 1);
    ASSERT_INT("merge: L3 disabled (mask bit clear)", g_last_scene_config.lights[3].enabled, 0);
    /* Dimensional data comes from the app-owned theme. */
    ASSERT_INT("merge: L0 id from theme",
               (int)g_last_scene_config.lights[0].id, (int)theme[0].id);
    ASSERT_TRUE("merge: L0 diffuse from theme",
                g_last_scene_config.lights[0].diffuse[0] == theme[0].diffuse[0] &&
                g_last_scene_config.lights[0].diffuse[1] == theme[0].diffuse[1] &&
                g_last_scene_config.lights[0].diffuse[2] == theme[0].diffuse[2]);
    ASSERT_TRUE("merge: L2 position from theme",
                g_last_scene_config.lights[2].pos[0] == theme[2].pos[0] &&
                g_last_scene_config.lights[2].pos[1] == theme[2].pos[1] &&
                g_last_scene_config.lights[2].pos[2] == theme[2].pos[2]);
}

/* The exporter is scene/app-free and reads the dimensional light data through
 * the controller-installed light bridge. Verify the installed bridge copies
 * the live app-owned light table verbatim, so exported glLightfv blocks match
 * the active theme. */
static void test_export_light_bridge_reads_app_state(void) {
    printf("--- imrepl_ctrl export light bridge reads app-owned lights ---\n");
    prepare_display_fixture();
    glr_config_set(GLR_CONFIG_LIGHT_THEME, LIGHT_THEME_STUDIO);

    const ReplExportLightBridge *b = repl_export_light_bridge();
    ASSERT_TRUE("light bridge installed", b != NULL && b->fill_slot != NULL);
    if (!b || !b->fill_slot) return;

    GlrRenderState render = glr_state_render();
    for (int slot = 0; slot < MAX_LIGHTS; slot++) {
        ReplExportLightInfo info;
        memset(&info, 0xAB, sizeof(info)); /* bridge must overwrite all fields */
        b->fill_slot(slot, &info);
        const Render3dLight *l = &render.lights[slot];
        ASSERT_TRUE("bridge diffuse matches app light",
                    info.diffuse[0] == l->diffuse[0] && info.diffuse[3] == l->diffuse[3]);
        ASSERT_TRUE("bridge ambient matches app light",
                    info.ambient[0] == l->ambient[0]);
        ASSERT_TRUE("bridge specular matches app light",
                    info.specular[0] == l->specular[0]);
        ASSERT_TRUE("bridge position matches app light",
                    info.pos[0] == l->pos[0] && info.pos[3] == l->pos[3]);
        ASSERT_INT("bridge eye-space matches app light",
                   info.pos_is_eye_space, l->pos_is_eye_space);
    }
}

static void test_mouse_routing_and_hit_testing(void) {
    printf("--- imrepl_ctrl mouse routing and hit testing ---\n");
    prepare_display_fixture();
    glr_color_picker_install_host();

    /* 1. Simulate mouse left click down on help overlay */
    ui_state_help_mut()->visible = 1;
    editor_help_session_set_tab(0);
    ui_state_viewport_set_size(800, 600);
    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, 10, 10);
    ASSERT_TRUE("help closed on click away", !ui_state_help().visible);

    /* 2. Test menu buttons, items, pin buttons, dividers, and swatch hit routing directly. */
    UiHit hit = ui_hit_none();

    // Test route_menu_button_hit
    hit.kind = UI_HIT_MENU_BUTTON;
    hit.cmd_idx = GLR_MENU_FILE;
    int rc = route_menu_button_hit(&hit);
    ASSERT_INT("menu button hit consumed", rc, 1);
    ASSERT_INT("menu opened", ui_menu_bar_open_menu_id(), GLR_MENU_FILE);

    // Close menu button
    rc = route_menu_button_hit(&hit);
    ASSERT_INT("menu button close consumed", rc, 1);
    ASSERT_INT("menu closed", ui_menu_bar_open_menu_id(), -1);

    // Resolve row index for GLR_CONFIG_GRID_THEME
    int grid_theme_row = -1;
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        if (item && item->key == GLR_CONFIG_GRID_THEME) {
            grid_theme_row = i;
            break;
        }
    }
    ASSERT_TRUE("found grid theme row", grid_theme_row >= 0);

    // Test route_submenu_item_hit for GLR_MENU_CONFIG
    hit.kind = UI_HIT_SUBMENU_ITEM;
    hit.cmd_idx = GLR_MENU_CONFIG;
    hit.item_idx = grid_theme_row;
    int old_grid = glr_config_get(GLR_CONFIG_GRID_THEME);
    rc = route_submenu_item_hit(&hit);
    ASSERT_INT("submenu config item consumed", rc, 1);
    ASSERT_TRUE("grid config changed", glr_config_get(GLR_CONFIG_GRID_THEME) != old_grid);

    // Test route_submenu_item_hit for tutorials
    hit.cmd_idx = GLR_MENU_TUTORIALS;
    hit.item_idx = 0;
    rc = route_submenu_item_hit(&hit);
    ASSERT_INT("submenu tutorial item consumed", rc, 1);

    // Test route_submenu_item_hit for Audio
    {
        GlrAudioTrackSpec tracks[] = {
            { "missing_audio_menu_track.mp3", "Assets", "Missing" },
        };
        setenv("GLR_AUDIO_NO_DEVICE", "1", 1);
        glr_audio_shutdown();
        ASSERT_INT("audio route init no-device", glr_audio_init(), 0);
        ASSERT_INT("audio route playlist",
                   glr_audio_set_playlist_specs(tracks, 1), 1);
        hit.cmd_idx = GLR_MENU_AUDIO;
        hit.item_idx = 0;
        rc = route_submenu_item_hit(&hit);
        ASSERT_INT("submenu audio item consumed", rc, 1);
        glr_audio_shutdown();
        unsetenv("GLR_AUDIO_NO_DEVICE");
    }

    // Test route_panel_divider_hit
    hit.kind = UI_HIT_PANEL_DIVIDER;
    ui_state_code_panel_mut()->resizing_panel = 0;
    rc = route_panel_divider_hit(&hit);
    ASSERT_INT("panel divider hit consumed", rc, 1);
    ASSERT_INT("panel resizing set", ui_state_code_panel().resizing_panel, 1);

    // Test route_pin_button_hit
    hit.kind = UI_HIT_PIN_BUTTON;
    hit.item_idx = UI_MENU_BAR_PIN_REPLAY;
    replay_state_mut()->state = REPLAY_PAUSED;
    rc = route_pin_button_hit(&hit);
    ASSERT_INT("pin button hit consumed", rc, 1);
    ASSERT_INT("replay state toggled to PLAYING", replay_state_view().state, REPLAY_PLAYING);

    /* Clicking the pinned Replay button after playback completes dismisses
     * the Done state instead of restarting the replay. */
    replay_state_mut()->active = 1;
    replay_state_mut()->state = REPLAY_DONE;
    rc = route_pin_button_hit(&hit);
    ASSERT_INT("Done pin button hit consumed", rc, 1);
    ASSERT_TRUE("Done pin button stops replay", !replay_state_view().active);
    ASSERT_INT("Done pin button enters OFF", replay_state_view().state, REPLAY_OFF);

    // Test route_pin_button_hit for view mode swatch toggle
    hit.kind = UI_HIT_PIN_BUTTON;
    hit.item_idx = UI_MENU_BAR_PIN_VIEW_MODE;
    glr_config_set(GLR_CONFIG_ORTHO_MODE, 0); /* 3D */
    rc = route_pin_button_hit(&hit);
    ASSERT_INT("view-mode pin hit consumed", rc, 1);
    ASSERT_INT("view mode toggled to 2D", glr_config_get(GLR_CONFIG_ORTHO_MODE), 1);

    rc = route_pin_button_hit(&hit);
    ASSERT_INT("view-mode pin hit consumed again", rc, 1);
    ASSERT_INT("view mode toggled back to 3D", glr_config_get(GLR_CONFIG_ORTHO_MODE), 0);

    // Test route_inline_color_swatch_hit
    prepare_display_fixture();
    glr_color_picker_install_host();

    hit.kind = UI_HIT_INLINE_COLOR_SWATCH;
    hit.line_idx = 1;
    color_picker_stop();
    rc = route_inline_color_swatch_hit(&hit, 300);
    ASSERT_INT("color swatch hit consumed", rc, 1);
    ASSERT_INT("color picker started on line 1", color_picker_active_line(), 1);
    rc = route_inline_color_swatch_hit(&hit, 300);
    ASSERT_INT("color swatch hit toggle off consumed", rc, 1);
    ASSERT_TRUE("color picker stopped", color_picker_active_line() < 0);

    /* 3. Simulate glr_ctrl_mousewheel, glr_ctrl_motion, glr_ctrl_passive_motion */
    glr_ctrl_mousewheel(0, 1, 100, 100);
    glr_ctrl_passive_motion(150, 150);

    // Let's test GLUT middle / scroll wheel emulation buttons
    ui_state_help_mut()->visible = 1;
    editor_help_session_set_scroll(10);
    glr_ctrl_mouse(3, GLUT_DOWN, 100, 100); // GLUT scroll wheel up (button 3)
    glr_ctrl_mouse(4, GLUT_DOWN, 100, 100); // GLUT scroll wheel down (button 4)

    // And test cleanup of dragging on UP
    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_UP, 100, 100);
}

/* glMaterialfv's RGBA value lives in a compound literal at args[2..5],
 * behind two baked enum tokens — so both the read (swatch color) and the
 * write (source rewrite) take their own path through the picker host. */
static void test_color_picker_materialfv(void) {
    printf("--- imrepl_ctrl color picker on glMaterialfv ---\n");
    prepare_display_fixture();
    glr_color_picker_install_host();
    glr_ctrl_reset_all();

    editor_feed_line("glMaterialfv(GL_FRONT, GL_DIFFUSE, (GLfloat[]){0.9, 0.3, 0.2, 1});");
    editor_feed_line("glMaterialfv(GL_FRONT, GL_SHININESS, (GLfloat[]){32});");
    ASSERT_INT("both material lines committed", repl_state_document_count(), 2);

    float r = 0, g = 0, b = 0, a = 0;
    int has_alpha = 0;
    ASSERT_INT("RGBA glMaterialfv is pickable",
               color_picker_read_cmd_color(0, &r, &g, &b, &a, &has_alpha), 1);
    ASSERT_TRUE("swatch reads the compound literal, not face/pname",
                r == 0.9f && g == 0.3f && b == 0.2f && a == 1.0f);
    ASSERT_INT("RGBA glMaterialfv carries alpha", has_alpha, 1);
    ASSERT_INT("GL_SHININESS glMaterialfv has no color to pick",
               color_picker_can_edit_cmd(1), 0);

    /* Press the SV square's bottom-left corner (S = 0, V = 0 => black): the
     * writeback must rewrite only the literal and keep face/pname. */
    color_picker_stop();
    color_picker_start(0, 300);
    ASSERT_INT("picker opened on the material line", color_picker_active_line(), 0);
    ColorPickerView v = color_picker_view();
    ColorPickerInputResult res =
        color_picker_handle_press(v.rects.sv_x,
                                  ui_state_viewport().window_h - v.rects.sv_y);
    ASSERT_INT("SV press consumed", res.consumed, 1);
    ASSERT_INT("SV press wrote back", res.changed, 1);
    color_picker_handle_release();

    const char *line = editor_buffer_view_line(editor_buffer_view(), 0);
    ASSERT_TRUE("rewrite keeps face and pname tokens",
                line && strstr(line, "glMaterialfv(GL_FRONT, GL_DIFFUSE, (GLfloat[]){") != NULL);
    ASSERT_INT("rewritten line still parses as a material",
               repl_state_document_cmd_at(0)->type, CMD_MATERIALFV);
    ASSERT_TRUE("SV corner drove the literal to black",
                repl_state_document_cmd_at(0)->args[2] == 0.0f &&
                repl_state_document_cmd_at(0)->args[3] == 0.0f &&
                repl_state_document_cmd_at(0)->args[4] == 0.0f);
    ASSERT_INT("face token survives as args[0]",
               (int)repl_state_document_cmd_at(0)->args[0], GL_FRONT);
    color_picker_stop();
}

static void test_special_key_shortcuts(void) {
    printf("--- imrepl_ctrl special key shortcuts ---\n");
    prepare_display_fixture();

    editor_input_set_modifier_provider_for_test(simulated_mods_provider);

    /* 1. Replay special shortcut */
    replay_state_mut()->active = 1;
    replay_state_mut()->state = REPLAY_PLAYING;
    glr_ctrl_special(GLUT_KEY_UP, 0, 0);

    // Stop replay so subsequent keystrokes aren't intercepted by replay_handle_special
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;

    /* 2. Config special shortcut */
    int old_grid = glr_config_get(GLR_CONFIG_GRID_THEME);
    glr_ctrl_special(GLUT_KEY_F2, 0, 0);
    ASSERT_TRUE("F2 toggled grid config", glr_config_get(GLR_CONFIG_GRID_THEME) != old_grid);

    /* 3. Audio special shortcuts (Ctrl+Left / Ctrl+Right) */
    g_simulated_mods = GLUT_ACTIVE_CTRL;
    int rc = glr_ctrl_router_handle_horizontal_audio_special(GLUT_KEY_LEFT);
    ASSERT_INT("audio prev handled", rc, 1);
    rc = glr_ctrl_router_handle_horizontal_audio_special(GLUT_KEY_RIGHT);
    ASSERT_INT("audio next handled", rc, 1);
    g_simulated_mods = 0;

    /* Ctrl+Shift+A toggles Audio play/pause through the controller-owned
     * audio route. Plain Ctrl+A is still editor line-start and must not
     * toggle audio here. */
    glr_config_set(GLR_CONFIG_AUDIO_MODE, 1);
    ASSERT_INT("plain Ctrl+A not claimed by audio route",
               glr_ctrl_router_handle_audio_key(KEY_CTRL_A), 0);
    ASSERT_INT("plain Ctrl+A leaves audio on",
               glr_config_get(GLR_CONFIG_AUDIO_MODE), 1);

    g_simulated_mods = GLUT_ACTIVE_SHIFT;
    /* Audio was never initialized here (the --no-audio case): the route
     * still consumes the key, but the toggle is a status-only no-op. */
    ASSERT_INT("Ctrl+Shift+A consumed while audio disabled",
               glr_ctrl_router_handle_audio_key(KEY_CTRL_A), 1);
    ASSERT_INT("Ctrl+Shift+A no-op while audio disabled",
               glr_config_get(GLR_CONFIG_AUDIO_MODE), 1);
    ASSERT_STR("Ctrl+Shift+A reports disabled audio",
               ui_state_status_mut()->text, "Audio: disabled");

    setenv("GLR_AUDIO_NO_DEVICE", "1", 1);
    ASSERT_INT("audio toggle init no-device", glr_audio_init(), 0);
    ASSERT_INT("Ctrl+Shift+A audio route handled",
               glr_ctrl_router_handle_audio_key(KEY_CTRL_A), 1);
    ASSERT_INT("Ctrl+Shift+A pauses audio",
               glr_config_get(GLR_CONFIG_AUDIO_MODE), 0);

    ASSERT_INT("Ctrl+Shift+A audio route handled again",
               glr_ctrl_router_handle_audio_key(KEY_CTRL_A), 1);
    ASSERT_INT("Ctrl+Shift+A resumes audio",
               glr_config_get(GLR_CONFIG_AUDIO_MODE), 1);
    glr_audio_shutdown();
    unsetenv("GLR_AUDIO_NO_DEVICE");
    g_simulated_mods = 0;

    /* 4. Help overlay actions */
    ui_state_help_mut()->visible = 1;
    editor_help_session_set_tab(1);
    glr_ctrl_special(GLUT_KEY_LEFT, 0, 0);
    ASSERT_INT("help tab prev", editor_help_session_tab_idx(), 0);
    glr_ctrl_special(GLUT_KEY_RIGHT, 0, 0);
    ASSERT_INT("help tab next", editor_help_session_tab_idx(), 1);

    editor_help_session_set_scroll(10);
    glr_ctrl_special(GLUT_KEY_UP, 0, 0);
    ASSERT_TRUE("help scrolled up", editor_help_session_scroll() < 10);
    glr_ctrl_special(GLUT_KEY_DOWN, 0, 0);
    ASSERT_TRUE("help scrolled down", editor_help_session_scroll() > 0);

    ui_state_help_mut()->visible = 1;
    glr_ctrl_special(GLUT_KEY_F1, 0, 0);
    ASSERT_TRUE("help toggled off via F1", !ui_state_help().visible);

    /* 5. Scene cycle (F12, Shift+F12) */
    int examples = repl_example_count();
    if (examples > 1) {
        repl_load_example(0);
        g_simulated_mods = 0;
        glr_ctrl_special(GLUT_KEY_F12, 0, 0); // next
        ASSERT_INT("cycled to example 1", repl_state_scenes().active_example_idx, 1);
        g_simulated_mods = GLUT_ACTIVE_SHIFT;
        glr_ctrl_special(GLUT_KEY_F12, 0, 0); // prev
        ASSERT_INT("cycled back to example 0", repl_state_scenes().active_example_idx, 0);
        g_simulated_mods = 0;
    }

    /* 6. Tutorial cycle special (F11) */
    int tutorials = repl_tutorial_count();
    if (tutorials > 1) {
        tutorial_start(0);
        g_simulated_mods = 0;
        rc = glr_ctrl_router_handle_tutorial_cycle_special(GLUT_KEY_F11);
        ASSERT_INT("tutorial cycle next handled", rc, 1);
        ASSERT_INT("cycled to tutorial 1", tutorial_state_view().tutorial_idx, 1);
        g_simulated_mods = GLUT_ACTIVE_SHIFT;
        rc = glr_ctrl_router_handle_tutorial_cycle_special(GLUT_KEY_F11);
        ASSERT_INT("tutorial cycle prev handled", rc, 1);
        ASSERT_INT("cycled back to tutorial 0", tutorial_state_view().tutorial_idx, 0);

        /* Completion leaves the tutorial inactive, but F11 must still start
         * at the lesson after the one just completed rather than lesson 0. */
        for (int step = 0; step < repl_tutorial_step_count(0); step++) {
            TutorialMatchResult match;
            const char *expected = tutorial_current_expected_text();
            ASSERT_TRUE("completed-cycle tutorial step has expected input",
                        expected != NULL);
            ASSERT_TRUE("completed-cycle tutorial step matches",
                        tutorial_handle_commit_attempt(expected, &match));
            tutorial_advance_after_successful_commit();
        }
        ASSERT_INT("tutorial inactive after completion", tutorial_active(), 0);
        ASSERT_INT("completed tutorial index retained", tutorial_state_view().tutorial_idx, 0);
        g_simulated_mods = 0;
        rc = glr_ctrl_router_handle_tutorial_cycle_special(GLUT_KEY_F11);
        ASSERT_INT("tutorial cycle after completion handled", rc, 1);
        ASSERT_INT("completion advances to tutorial 1",
                   tutorial_state_view().tutorial_idx, 1);
        g_simulated_mods = 0;
    }

    editor_input_set_modifier_provider_for_test(NULL);
}

/* Drive one untimed pointer-script line and fire its single event. The
 * scripted `chord` verb is the only production path that synthesizes a Shift
 * modifier, so these tests install NO modifier provider — the scripted override
 * pushed by glr_ctrl_*_with_modifiers is the sole modifier source, exactly as
 * in a real capture/tour run. */
static void run_pointer_script_line(const char *line) {
    const char *lines[1];
    lines[0] = line;
    ASSERT_INT("pointer script line accepted",
               glr_pointer_script_start_tour("Test", "test.pointer", lines, 1),
               1);
    /* Frame 1 captures the rewind baseline and enters Playing; frame 2 fires
     * the untimed event (a chord completes immediately, PS_WAIT_NONE). */
    glr_pointer_script_frame();
    glr_pointer_script_frame();
    glr_pointer_script_stop();
}

static void test_scripted_chord_reaches_shift_shortcuts(void) {
    printf("--- imrepl_ctrl scripted chord modifiers ---\n");
    prepare_display_fixture();
    editor_input_set_modifier_provider_for_test(NULL);

    /* Parse acceptance / rejection through the public load path. */
    const char *ok1[1]  = { "chord ctrl+shift c" };
    const char *ok2[1]  = { "chord shift f12" };
    const char *bad1[1] = { "chord shift a" };           /* shift-only printable */
    const char *bad2[1] = { "chord bogus c" };           /* unknown modifier     */
    const char *bad3[1] = { "chord ctrl++shift c" };     /* empty component      */
    const char *bad4[1] = { "chord ctrl+ctrl c" };       /* duplicate modifier   */
    const char *bad5[1] = { "chord ctrl+shift c junk" }; /* trailing garbage     */
    ASSERT_INT("chord ctrl+shift c parses",
               glr_pointer_script_start_tour("T", "t.pointer", ok1, 1), 1);
    glr_pointer_script_stop();
    ASSERT_INT("chord shift f12 parses",
               glr_pointer_script_start_tour("T", "t.pointer", ok2, 1), 1);
    glr_pointer_script_stop();
    ASSERT_INT("shift-only printable rejected",
               glr_pointer_script_start_tour("T", "t.pointer", bad1, 1), 0);
    ASSERT_INT("unknown modifier rejected",
               glr_pointer_script_start_tour("T", "t.pointer", bad2, 1), 0);
    ASSERT_INT("empty modifier component rejected",
               glr_pointer_script_start_tour("T", "t.pointer", bad3, 1), 0);
    ASSERT_INT("duplicate modifier rejected",
               glr_pointer_script_start_tour("T", "t.pointer", bad4, 1), 0);
    ASSERT_INT("trailing garbage rejected",
               glr_pointer_script_start_tour("T", "t.pointer", bad5, 1), 0);

    /* Dispatch: the fixture camera sits at a non-default pose, so Ctrl+Shift+C
     * (GLR_RESET_CAMERA) begins an eased return to default. */
    glr_camera_controls_reset();
    ASSERT_INT("no camera ease before chord", glr_camera_target_active(), 0);
    run_pointer_script_line("chord ctrl+shift c");
    ASSERT_TRUE("chord ctrl+shift c triggered camera reset ease",
                glr_camera_target_active() != 0);

    /* The override must not linger past the dispatch: with no provider and glut
     * reads disabled, modifiers read back as none once the chord returns. */
    ASSERT_INT("scripted modifier override popped after chord",
               editor_input_active_modifiers(), 0);

    /* Shift must not leak: plain `key \cC` (byte 3, no override) is Copy, not
     * Ctrl+Shift+C, so it must NOT start a camera reset. */
    glr_camera_controls_reset();
    ASSERT_INT("camera ease cleared before Copy", glr_camera_target_active(), 0);
    run_pointer_script_line("key \\cC");
    ASSERT_INT("plain Ctrl+C did not reset camera (Shift did not leak)",
               glr_camera_target_active(), 0);
}

static void test_app_lifecycle_bootstrap_shutdown(void) {
    printf("--- imrepl_ctrl app lifecycle ---\n");
    prepare_display_fixture();

    /* 1. Test bootstrap */
    glr_ctrl_bootstrap_repl(NULL);

    /* Predef vars initialized? */
    ReplPredefView predef = repl_eval_predef_view();
    ASSERT_TRUE("Predef variables initialized during bootstrap", predef.count > 0);
    ASSERT_TRUE("time_var_idx is valid", repl_state_variables().time_var_idx >= 0);

    /* Status banner set? */
    ASSERT_STR("Status banner set during bootstrap",
               ui_state_status_mut()->text,
               "Ready - type GL commands, press ; to execute. F1 for help. F12 for examples.");

    /* 2. Test shutdown */
    glr_shutdown();
    ASSERT_TRUE("Shutdown completed successfully", 1);
}

static void test_init_gl_requires_loaded_point_parameter_proc(void) {
#ifndef GL_STUBS
    printf("Run `make test-glr-ctrl` for init_gl point-parameter loader coverage.\n");
    return;
#endif
    g_test_point_parameter_loader_calls = 0;
    g_glr_ctrl_point_parameter_loader = test_missing_point_parameter_loader;
    glr_ctrl_init_gl();
    ASSERT_TRUE("init_gl attempted to load point-parameter proc",
                g_test_point_parameter_loader_calls > 0);
    ASSERT_INT("missing point-parameter proc disables support",
               repl_executor_point_parameter_supported(), 0);
    ASSERT_TRUE("missing point-parameter proc is cleared",
                repl_executor_point_parameter_proc() == NULL);

    g_glr_ctrl_point_parameter_loader = glr_ctrl_default_point_parameter_loader;
    glr_ctrl_init_gl();
    ASSERT_INT("default proc loader restores support",
               repl_executor_point_parameter_supported(), 1);
    ASSERT_TRUE("default proc loader installs a proc",
                repl_executor_point_parameter_proc() != NULL);
}

static void test_code_panel_scroll_clamping_and_follow(void) {
    printf("--- imrepl_ctrl scroll clamping and follow ---\n");
    int follow_doc_line = 0;
    int visible_lines = 0;

    prepare_display_fixture();

    /* Make a large document to allow scrolling */
    for (int i = 0; i < 100; i++) {
        editor_buffer_set_line(i, "glVertex3f(1, 2, 3);");
    }
    repl_state_document_count_set(100);
    editor_state_edit_line_set(50);
    editor_input_set_text("glVertex3f(1, 2, 3);");

    /* Build snapshot and layout */
    ui_state_viewport_set_size(800, 600);
    glr_ctrl_display_frame();

    /* 1. Clamping test: scroll to an out-of-bounds target */
    editor_scroll_set(5000);
    glr_ctrl_code_panel_apply_scroll_follow_for_test(
        &g_last_ui_snapshot, &follow_doc_line, &visible_lines);

    ASSERT_TRUE("scroll clamped down to max_scroll", editor_scroll() < 5000);
    ASSERT_TRUE("scroll clamped is non-negative", editor_scroll() >= 0);

    /* Clamping test: scroll to negative value */
    editor_scroll_set(-10);
    glr_ctrl_code_panel_apply_scroll_follow_for_test(
        &g_last_ui_snapshot, &follow_doc_line, &visible_lines);
    ASSERT_INT("scroll clamped up to 0", editor_scroll(), 0);

    /* 2. Cursor follow-scroll test */
    editor_scroll_set(0);
    editor_scroll_follow_cursor_set(0);
    glr_ctrl_set_edit_line(80);
    ASSERT_INT("programmatic cursor park requests scroll follow",
               editor_scroll_follow_cursor(), 1);

    glr_ctrl_display_frame();

    glr_ctrl_code_panel_apply_scroll_follow_for_test(
        &g_last_ui_snapshot, &follow_doc_line, &visible_lines);

    ASSERT_TRUE("cursor park scrolls the code panel down", editor_scroll() > 0);
    ASSERT_TRUE("parked cursor is inside visible range",
                follow_doc_line >= editor_scroll() &&
                follow_doc_line < editor_scroll() + visible_lines);
}

static void test_post_filter_key_cycling(void) {
    printf("--- imrepl_ctrl Post FX scope/effect key cycling ---\n");
    prepare_display_fixture();

    /* Make sure replay doesn't intercept keys (prepare_display_fixture sets active=1) */
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;

    GlrPresentationState *p = glr_state_presentation_mut();
    glr_config_set(GLR_CONFIG_POST_FX_EFFECT,
                   GLR_POST_FX_EFFECT_CHROMATIC_ABERRATION);
    glr_config_set(GLR_CONFIG_POST_FX_SCOPE, GLR_POST_FX_SCOPE_OFF);

    /* F10 cycles the scope row (Ctrl+N was removed); direction is
     * read live off the modifier state, so install the test seam up front. */
    editor_input_set_modifier_provider_for_test(simulated_mods_provider);
    g_simulated_mods = 0;

    glr_ctrl_special(GLUT_KEY_F10, 0, 0);
    ASSERT_INT("scope becomes 3D View",
               glr_config_get(GLR_CONFIG_POST_FX_SCOPE), GLR_POST_FX_SCOPE_VIEW_3D);
    ASSERT_INT("post_filter_mode becomes chromatic aberration", (int)p->post_filter_mode, (int)RENDER3D_POST_FILTER_CHROMATIC_ABERRATION);
    ASSERT_INT("compositor_filter_mode remains off", (int)p->compositor_filter_mode, (int)RENDER3D_POST_FILTER_OFF);

    glr_ctrl_special(GLUT_KEY_F10, 0, 0);
    ASSERT_INT("scope becomes Frame",
               glr_config_get(GLR_CONFIG_POST_FX_SCOPE), GLR_POST_FX_SCOPE_FRAME);
    ASSERT_INT("post_filter_mode becomes off", (int)p->post_filter_mode, (int)RENDER3D_POST_FILTER_OFF);
    ASSERT_INT("compositor_filter_mode becomes chromatic aberration", (int)p->compositor_filter_mode, (int)RENDER3D_POST_FILTER_CHROMATIC_ABERRATION);

    glr_ctrl_special(GLUT_KEY_F10, 0, 0);
    ASSERT_INT("scope cycles back to Off",
               glr_config_get(GLR_CONFIG_POST_FX_SCOPE), GLR_POST_FX_SCOPE_OFF);
    ASSERT_INT("effect remains chromatic aberration",
               glr_config_get(GLR_CONFIG_POST_FX_EFFECT), GLR_POST_FX_EFFECT_CHROMATIC_ABERRATION);
    ASSERT_INT("post_filter_mode cycles back to off", (int)p->post_filter_mode, (int)RENDER3D_POST_FILTER_OFF);
    ASSERT_INT("compositor_filter_mode cycles back to off", (int)p->compositor_filter_mode, (int)RENDER3D_POST_FILTER_OFF);

    glr_config_set(GLR_CONFIG_POST_FX_EFFECT, GLR_POST_FX_EFFECT_VIGNETTE);
    ASSERT_INT("effect row changes while scope is off",
               glr_config_get(GLR_CONFIG_POST_FX_EFFECT), GLR_POST_FX_EFFECT_VIGNETTE);
    ASSERT_INT("post_filter_mode remains off", (int)p->post_filter_mode, (int)RENDER3D_POST_FILTER_OFF);
    ASSERT_INT("compositor_filter_mode remains off", (int)p->compositor_filter_mode, (int)RENDER3D_POST_FILTER_OFF);

    glr_ctrl_special(GLUT_KEY_F10, 0, 0);
    ASSERT_INT("scope returns to 3D View",
               glr_config_get(GLR_CONFIG_POST_FX_SCOPE), GLR_POST_FX_SCOPE_VIEW_3D);
    ASSERT_INT("post_filter_mode becomes vignette", (int)p->post_filter_mode, (int)RENDER3D_POST_FILTER_VIGNETTE);
    ASSERT_INT("compositor_filter_mode becomes off", (int)p->compositor_filter_mode, (int)RENDER3D_POST_FILTER_OFF);

    glr_config_set(GLR_CONFIG_POST_FX_EFFECT, GLR_POST_FX_EFFECT_SCANLINES);
    ASSERT_INT("post_filter_mode becomes scanlines", (int)p->post_filter_mode, (int)RENDER3D_POST_FILTER_SCANLINES);
    ASSERT_INT("compositor_filter_mode becomes off", (int)p->compositor_filter_mode, (int)RENDER3D_POST_FILTER_OFF);

    glr_ctrl_special(GLUT_KEY_F10, 0, 0);
    ASSERT_INT("scope becomes Frame for scanlines",
               glr_config_get(GLR_CONFIG_POST_FX_SCOPE), GLR_POST_FX_SCOPE_FRAME);
    ASSERT_INT("post_filter_mode becomes off", (int)p->post_filter_mode, (int)RENDER3D_POST_FILTER_OFF);
    ASSERT_INT("compositor_filter_mode becomes scanlines", (int)p->compositor_filter_mode, (int)RENDER3D_POST_FILTER_SCANLINES);

    glr_config_set(GLR_CONFIG_POST_FX_EFFECT, GLR_POST_FX_EFFECT_FILM_GRAIN);
    ASSERT_INT("post_filter_mode remains off with frame scope", (int)p->post_filter_mode, (int)RENDER3D_POST_FILTER_OFF);
    ASSERT_INT("compositor_filter_mode becomes film grain", (int)p->compositor_filter_mode, (int)RENDER3D_POST_FILTER_FILM_GRAIN);

    /* Ctrl+Shift+D now cycles the Depth view config row; it must still
     * leave the post-FX state untouched. */
    glr_state_presentation_mut()->depth_viz = 0;
    g_simulated_mods = GLUT_ACTIVE_SHIFT;
    glr_ctrl_keyboard(KEY_CTRL_D, 0, 0);
    ASSERT_INT("Ctrl+Shift+D does not touch post_filter_mode", (int)p->post_filter_mode, (int)RENDER3D_POST_FILTER_OFF);
    ASSERT_INT("Ctrl+Shift+D does not touch compositor_filter_mode", (int)p->compositor_filter_mode, (int)RENDER3D_POST_FILTER_FILM_GRAIN);
    ASSERT_INT("Ctrl+Shift+D cycles Depth view", p->depth_viz, 1);
    glr_state_presentation_mut()->depth_viz = 0;

    g_simulated_mods = GLUT_ACTIVE_SHIFT;
    glr_ctrl_special(GLUT_KEY_F10, 0, 0);
    ASSERT_INT("Shift+F10 steps from Frame to 3D View",
               glr_config_get(GLR_CONFIG_POST_FX_SCOPE), GLR_POST_FX_SCOPE_VIEW_3D);
    ASSERT_INT("Shift+F10 applies film grain to the scene",
               (int)p->post_filter_mode, (int)RENDER3D_POST_FILTER_FILM_GRAIN);
    ASSERT_INT("Shift+F10 clears compositor side",
               (int)p->compositor_filter_mode, (int)RENDER3D_POST_FILTER_OFF);

    glr_ctrl_special(GLUT_KEY_F10, 0, 0);
    ASSERT_INT("Shift+F10 steps from 3D View to Off",
               glr_config_get(GLR_CONFIG_POST_FX_SCOPE), GLR_POST_FX_SCOPE_OFF);
    ASSERT_INT("Shift+F10 clears scene side",
               (int)p->post_filter_mode, (int)RENDER3D_POST_FILTER_OFF);
    ASSERT_INT("Shift+F10 leaves compositor side off",
               (int)p->compositor_filter_mode, (int)RENDER3D_POST_FILTER_OFF);

    glr_ctrl_special(GLUT_KEY_F10, 0, 0);
    ASSERT_INT("Shift+F10 wraps backward from Off to Frame",
               glr_config_get(GLR_CONFIG_POST_FX_SCOPE), GLR_POST_FX_SCOPE_FRAME);
    ASSERT_INT("Shift+F10 wrap keeps scene side off",
               (int)p->post_filter_mode, (int)RENDER3D_POST_FILTER_OFF);
    ASSERT_INT("Shift+F10 wrap applies film grain to compositor side",
               (int)p->compositor_filter_mode, (int)RENDER3D_POST_FILTER_FILM_GRAIN);

    g_simulated_mods = 0;
    editor_input_set_modifier_provider_for_test(NULL);
}

int main(void) {
    printf("--- imrepl_ctrl tests ---\n");

    test_display_frame_builds_config_and_restores_live_state();
    test_reshape_clamps_height();
    test_display_frame_profile_coverage();
    test_frame_total_spans_host_stages();
    test_variable_panel_motion_routes_through_compile_and_coalesces_undo();
    test_variable_panel_shift_left_drag_uses_fine_scale();
    test_variable_panel_motion_preserves_reset_assignment_without_declaration();
    test_variable_panel_motion_initializes_uninitialized_declaration();
    test_variable_panel_drag_motion_never_marks_source_dirty();
    test_variable_panel_drag_release_without_motion_is_a_noop();
    test_variable_panel_drag_release_without_declaration_is_a_noop();
    test_variable_drag_snapshot_wiring();
    test_variable_panel_written_snapshot_wiring();
    test_pointer_state_tracks_controller_mouse_routes();
    test_right_click_code_panel_does_not_start_camera_pan();
    test_right_click_assignment_opens_value_plot();
    test_right_click_gl_command_description_popup();
    test_right_click_empty_line_toggles_gl_state_report();
    test_divider_hover_yields_to_front_panel();
    test_right_click_uncommitted_empty_line_opens_gl_state_report();
    test_gl_state_popup_scroll_geometry();
    test_gl_state_popup_modelview_uses_four_lines();
    test_gl_state_popup_details_toggle();
    test_gl_state_popup_setup_fold();
    test_gl_state_popup_source_line_tracks_gutter();
    test_editor_input_dismisses_gl_state_report();
    test_gl_state_popup_defers_to_front_overlay();
    test_left_click_code_panel_exits_search_and_places_cursor();
    test_tick_per_frame_scheduling();
    test_overlay_transition_machine_wiring();
    test_view_mode_projection_transition_wiring();
    test_view_mode_3d_to_2d_uses_faster_decay();
    test_projection_toggle_free_camera();
    test_view_mode_2d_honors_pending_camera_ease();
    test_view_mode_restore_honors_pending_camera_reset();
    test_import_camera_block_becomes_scene_default();
    test_import_without_camera_block_keeps_global_default();
    test_view_mode_3d_restore_tracks_example_loaded_midblend();
    test_view_record_external_3d_pose_tracks_in_ortho();
    test_view_record_external_3d_pose_noop_in_perspective();
    test_quit_recovery_file();
    test_build_ui_snapshot_is_idempotent();
    test_display_frame_scene_config_is_stable_across_frames();
    test_display_frame_no_replay_means_no_fade_plumbing();
    test_display_frame_follows_replay_line_after_tick();
    test_push_attrib_bit_token_highlights();
    test_begin_end_bracket_highlights();
    test_replay_call_site_highlights_are_pushed();
    test_replay_focus_vertex_affecting_transforms();
    test_replay_focus_glut_solid_affecting_transforms();
    test_overlay_scope_last_instance_affecting_transforms();
    test_numeric_swatch_step_commits_line_and_undoes();
    test_numeric_swatch_step_commits_bare_assignment();
    test_numeric_swatch_visible_inside_loop_expr();
    test_numeric_swatch_step_commits_loop_expr_literal();
    test_numeric_swatch_no_op_outside_numeric_arg();
    test_numeric_swatch_no_op_in_insert_mode();
    test_numeric_swatch_scale_coarse_and_fine();
    test_variable_panel_t_change_reflattens_when_time_paused();
    test_auxiliary_scene_pass_side_effects();
    test_wireframe_renderer_ignores_user_draw_state();
    test_example_reset_reapplies_light_theme();
    test_display_frame_merges_light_theme_and_enable_mask();
    test_export_light_bridge_reads_app_state();
    test_mouse_routing_and_hit_testing();
    test_color_picker_materialfv();
    test_special_key_shortcuts();
    test_scripted_chord_reaches_shift_shortcuts();
    test_post_filter_key_cycling();
    test_app_lifecycle_bootstrap_shutdown();
    test_init_gl_requires_loaded_point_parameter_proc();
    test_code_panel_scroll_clamping_and_follow();

    printf("\n");
    return test_harness_report(&g_harness, "test_imrepl_ctrl");
}
