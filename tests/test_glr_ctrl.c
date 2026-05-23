#define _DEFAULT_SOURCE  /* mkdtemp() */
#include "editor/state.h"
#include "app/glr_camera.h"
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "config.h"      /* QUIT_RECOVERY_FILE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "editor/undo.h"
#include "repl/core.h"
#include "editor/input.h"
#include "ui/core/layout.h"   /* CODE_PANEL_LAYOUT_* */
#include "support/test_harness.h"

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
#define scene_render_3d_scene              test_scene_render_3d_scene
#define scene_apply_camera                 test_scene_apply_camera
#define replay_ui_hud_render               test_replay_ui_hud_render
#define ui_panels_render_code_panel        test_ui_panels_render_code_panel
#define ui_autocomplete_panel_render       test_ui_autocomplete_panel_render
#define ui_menu_bar_render_example_dropdown test_ui_menu_bar_render_example_dropdown
#define ui_variable_panel_render           test_ui_variable_panel_render
#define ui_panels_render_scene_status      test_ui_panels_render_scene_status
#define ui_tabbed_overlay_render           test_ui_tabbed_overlay_render
#define ui_profile_panel_render            test_ui_profile_panel_render

#include "app/glr_ctrl.c"

#undef scene_render_3d_scene
#undef scene_apply_camera
#undef replay_ui_hud_render
#undef ui_panels_render_code_panel
#undef ui_autocomplete_panel_render
#undef ui_menu_bar_render_example_dropdown
#undef ui_variable_panel_render
#undef ui_panels_render_scene_status
#undef ui_tabbed_overlay_render
#undef ui_profile_panel_render

static SceneRenderConfig g_last_scene_config;
static UiReplayHudState  g_last_replay_hud_state;
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

int test_scene_render_3d_scene(const SceneRenderConfig *config) {
    g_scene_render_calls++;
    g_last_scene_config = *config;

    if (g_t_idx >= 0) {
        g_predef_value_seen_in_scene = g_predef_vars[g_t_idx].value;
        g_predef_vars[g_t_idx].value = g_mutated_predef_value;
    }
    repl_eval_scratch_get(0, 0, &g_scratch_value_seen_in_scene);
    repl_eval_scratch_set(0, 0, g_mutated_scratch_value);

    repl_state_flat_program_set_count(g_mutated_flat_count);
    return 0;
}

void test_scene_apply_camera(float rx, float ry, float dist,
                             float tx, float ty, float tz) {
    (void)rx; (void)ry; (void)dist; (void)tx; (void)ty; (void)tz;
}

void test_replay_ui_hud_render(const UiReplayHudState *state) {
    g_replay_hud_calls++;
    g_last_replay_hud_state = *state;

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
void test_ui_variable_panel_render(const UiRenderSnapshot *snap) { (void)snap; }
void test_ui_panels_render_scene_status(const UiRenderSnapshot *snap) { (void)snap; }
void test_ui_tabbed_overlay_render(const UiOverlayState *in) { (void)in; }
void test_ui_profile_panel_render(const UiRenderSnapshot *snap) { (void)snap; }

static void prepare_display_fixture(void) {
    GLCmd *doc_cmds;
    GLCmd *flat_cmds;
    GlrPresentationState *presentation;

    memset(&g_last_scene_config, 0, sizeof(g_last_scene_config));
    memset(&g_last_replay_hud_state, 0, sizeof(g_last_replay_hud_state));
    g_scene_render_calls = 0;
    g_replay_hud_calls = 0;
    g_predef_value_seen_in_scene = 0.0f;
    g_predef_value_seen_in_hud = 0.0f;
    g_flat_count_seen_in_hud = -1;
    g_scratch_value_seen_in_scene = 0.0f;
    g_t_idx = -1;

    glr_app_reset_all();
    repl_eval_init_predef_vars();

    ui_state_viewport_set_size(800, 600);
    glr_camera_set(11.0f, 22.0f, 7.5f, 0.5f, -0.25f, 1.75f, 0.9f);
    glr_state_render_mut()->use_accum = 0;
    glr_state_render_mut()->accum_aa_enabled = 0;
    glr_state_render_mut()->accum_samples = 1;
    glr_state_render_mut()->multisample_enabled = 0;
    glr_state_render_mut()->line_smooth_enabled = 1;
    repl_state_render_mut()->clear_color[0] = 0.0f;
    repl_state_render_mut()->clear_color[1] = 0.0f;
    repl_state_render_mut()->clear_color[2] = 0.0f;
    repl_state_render_mut()->clear_color[3] = 1.0f;

    repl_state_flat_program_set_user_lighting_enabled(1);

    presentation = glr_state_presentation_mut();
    presentation->show_vertex_guides = 1;
    presentation->show_vertex_points = 1;
    presentation->show_light_indicators = 1;
    presentation->highlight_current_poly = 1;
    presentation->xform_guide_mode = 2;
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
        g_predef_vars[g_t_idx].value = 9.0f;
    repl_eval_scratch_set(0, 0, 4.0f);

    replay_start();
    repl_state_flat_program_set_user_lighting_enabled(1);

    replay_state_mut()->active = 1;
    replay_state_mut()->state = REPLAY_PLAYING;
    replay_state_mut()->pc = 1;
    replay_state_mut()->mode = REPLAY_MODE_VERTEX;
    replay_state_mut()->speed = 2.5f;
    replay_state_mut()->expand_args = 1;
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
    ASSERT_INT("viewport width forwarded", g_last_scene_config.viewport_w, 800);
    ASSERT_INT("viewport height forwarded", g_last_scene_config.viewport_h, 600);
    ASSERT_FLOAT("camera distance forwarded", g_last_scene_config.cam_dist, 7.5f);
    ASSERT_FLOAT("camera tx forwarded", g_last_scene_config.cam_tx, 0.5f);
    ASSERT_FLOAT("camera glow forwarded", g_last_scene_config.cam_motion_glow, 0.9f);
    ASSERT_INT("user lighting copied", g_last_scene_config.user_lighting_enabled, 1);
    ASSERT_INT("light indicators copied", g_last_scene_config.show_light_indicators, 1);
    /* The replay-fade plan moved out of SceneRenderConfig and is now a
     * controller-private static (g_replay_fade_plan). Inspect it directly
     * since this TU includes imrepl_ctrl.c as a compilation unit. */
    ASSERT_INT("replay fade plan inactive without active fades",
               g_replay_fade_plan_active, 0);
    ASSERT_INT("replay fade base limit zero without fades",
               g_replay_fade_plan_base_limit, 0);
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
    ASSERT_INT("tess preview marked active for VERTEX replay mode",
               g_replay_tess_preview_active, 1);
    ASSERT_FLOAT("alpha scale boosted for black background", g_last_scene_config.alpha_scale, 3.0f);
    ASSERT_INT("focus vertex valid", g_last_scene_config.focus.valid, 1);
    ASSERT_FLOAT("focus vertex x", g_last_scene_config.focus.pos[0], 1.0f);
    ASSERT_FLOAT("focus vertex y", g_last_scene_config.focus.pos[1], 2.0f);
    ASSERT_FLOAT("focus vertex z", g_last_scene_config.focus.pos[2], 3.0f);

    ASSERT_INT("HUD scene x matches config", g_last_replay_hud_state.scene_x, g_last_scene_config.scene_x);
    ASSERT_INT("HUD scene y matches config", g_last_replay_hud_state.scene_y, g_last_scene_config.scene_y);
    ASSERT_INT("HUD scene w matches config", g_last_replay_hud_state.scene_w, g_last_scene_config.scene_w);
    ASSERT_INT("HUD scene h matches config", g_last_replay_hud_state.scene_h, g_last_scene_config.scene_h);
    ASSERT_INT("HUD viewport w matches config", g_last_replay_hud_state.viewport_w, g_last_scene_config.viewport_w);
    ASSERT_INT("HUD viewport h matches config", g_last_replay_hud_state.viewport_h, g_last_scene_config.viewport_h);
    ASSERT_INT("HUD layout copied", g_last_replay_hud_state.code_panel_layout, CODE_PANEL_LAYOUT_BOTTOM);
    ASSERT_INT("HUD replay mode copied", g_last_replay_hud_state.replay_mode, REPLAY_MODE_VERTEX);
    ASSERT_INT("HUD replay pc copied", g_last_replay_hud_state.replay_pc, 1);
    ASSERT_INT("HUD replay total cmds copied", g_last_replay_hud_state.replay_total_cmds, 777);
    ASSERT_INT("HUD replay state copied", g_last_replay_hud_state.replay_state_val, REPLAY_PLAYING);
    ASSERT_FLOAT("HUD replay speed copied", g_last_replay_hud_state.replay_speed, 2.5f);
    ASSERT_INT("HUD replay expand args copied", g_last_replay_hud_state.replay_expand_args, 1);
    ASSERT_INT("HUD replaying copied", g_last_replay_hud_state.replaying, 1);

    ASSERT_FLOAT("scene saw live predef before mutation", g_predef_value_seen_in_scene, 9.0f);
    ASSERT_FLOAT("scene saw live scratch before mutation", g_scratch_value_seen_in_scene, 4.0f);
    ASSERT_FLOAT("HUD observed mutated predef during frame", g_predef_value_seen_in_hud, g_mutated_predef_value);
    ASSERT_INT("HUD observed mutated flat count during frame", g_flat_count_seen_in_hud,
               g_mutated_flat_count);

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
 * enough to matter goes unprofiled). The test runs one
 * glr_ctrl_display_frame() and inspects the profile state.
 *
 * The "all major sections non-stale" half is deterministic. The
 * "sum approximately equals total" half uses a generous lower bound
 * (50% of PROF_FRAME_TOTAL) to avoid flake from OS scheduling noise
 * — any future regression that drops a major section entirely will
 * blow the lower bound. A tighter upper bound is not asserted
 * because per-section start/end overhead can stack to a real but
 * harmless gap. */
static void test_display_frame_profile_coverage(void) {
    printf("--- imrepl_ctrl profile coverage ---\n");
    prepare_display_fixture();

    /* Drive one frame after marking both dirty so PROF_AUTONORMAL
     * and PROF_FLATTEN both run. The fixture also leaves replay
     * active so PROF_REPLAY_HUD lands. */
    repl_mark_normals_dirty();
    repl_state_mark_flat_dirty();
    glr_ctrl_display_frame();

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
        PROF_SCENE_3D,
        PROF_REPLAY_HUD,    /* fixture has replay active */
        PROF_CODE_PANEL,
        PROF_UI_PANELS,
        PROF_PROFILE_PANEL,
        PROF_FRAME_RESTORE,
    };
    for (size_t i = 0; i < sizeof(major) / sizeof(major[0]); i++) {
        char label[64];
        snprintf(label, sizeof(label), "section %d not stale", (int)major[i]);
        ASSERT_TRUE(label, !prof_section_is_stale(major[i]));
    }

    /* Sum of disjoint top-level sections should be a substantial
     * fraction of PROF_FRAME_TOTAL. PROF_SNAPSHOT / PROF_SCENE_3D
     * are themselves aggregates, so summing them with the leaves
     * outside (autonormal, flatten, replay_hud, code_panel,
     * ui_panels, profile_panel, frame_restore) covers the
     * controller's whole frame body. */
    double total_us = prof_section_last_us(PROF_FRAME_TOTAL);
    double sum_us =
        prof_section_last_us(PROF_AUTONORMAL) +
        prof_section_last_us(PROF_FLATTEN) +
        prof_section_last_us(PROF_SNAPSHOT) +
        prof_section_last_us(PROF_SCENE_3D) +
        prof_section_last_us(PROF_REPLAY_HUD) +
        prof_section_last_us(PROF_CODE_PANEL) +
        prof_section_last_us(PROF_UI_PANELS) +
        prof_section_last_us(PROF_PROFILE_PANEL) +
        prof_section_last_us(PROF_FRAME_RESTORE);

    /* Both should be positive (frame did real work). */
    ASSERT_TRUE("frame total positive", total_us > 0.0);
    ASSERT_TRUE("major-section sum positive", sum_us > 0.0);

    /* Sum should cover at least half the frame; missing a major
     * section drops it well below this threshold. */
    if (total_us > 0.0) {
        double coverage = sum_us / total_us;
        char label[96];
        snprintf(label, sizeof(label),
                 "major sections cover ≥50%% of FRAME_TOTAL (got %.1f%%)",
                 coverage * 100.0);
        ASSERT_TRUE(label, coverage >= 0.5);
    }

    /* PROF_SNAPSHOT subsections should sum to near the parent
     * (they are disjoint and exhaustive). */
    double snapshot_us = prof_section_last_us(PROF_SNAPSHOT);
    double snapshot_sub_us =
        prof_section_last_us(PROF_SNAPSHOT_TRANSFORMERS) +
        prof_section_last_us(PROF_SNAPSHOT_HIGHLIGHTS) +
        prof_section_last_us(PROF_SNAPSHOT_VIRTUAL_LINES) +
        prof_section_last_us(PROF_SNAPSHOT_PREP) +
        prof_section_last_us(PROF_SNAPSHOT_SCENE_CONFIG) +
        prof_section_last_us(PROF_SNAPSHOT_UI);
    if (snapshot_us > 0.0) {
        double coverage = snapshot_sub_us / snapshot_us;
        char label[96];
        snprintf(label, sizeof(label),
                 "SNAPSHOT subs cover ≥70%% of parent (got %.1f%%)",
                 coverage * 100.0);
        ASSERT_TRUE(label, coverage >= 0.7);
    }
}

static void test_variable_panel_motion_routes_through_compile_and_coalesces_undo(void) {
    int px, py, pw, ph;
    int click_x, click_y;
    int hit_row;
    int window_h;
    int var_idx;
    EditorUndoRingState undo_state;

    printf("--- imrepl_ctrl variable panel drag route ---\n");

    glr_app_reset_all();
    ui_state_viewport_set_size(1000, 1000);
    variable_panel_set_visible(1);
    editor_feed_line("float testvar = 1.0;");

    var_idx = repl_eval_find_predef_var_idx("testvar");
    ASSERT_TRUE("testvar declared", var_idx >= 0);
    ASSERT_FLOAT("testvar starts at 1", g_predef_vars[var_idx].value, 1.0f);

    ui_variable_panel_rect_for_count(g_num_predef_vars, &px, &py, &pw, &ph);
    window_h = ui_state_viewport().window_h;
    click_x = px + pw / 2;
    click_y = -1;
    for (int gl_y = py; gl_y < py + ph; gl_y++) {
        int candidate_y = window_h - gl_y;
        hit_row = -1;
        if (ui_variable_panel_hit_for_count(click_x, candidate_y,
                                            g_num_predef_vars, &hit_row)
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
    ASSERT_STR("drag rewrites declaration source through compiler",
               editor_buffer_line(0), "  static float testvar = 6;");
    ASSERT_FLOAT("drag updates live predef value", g_predef_vars[var_idx].value, 6.0f);

    ASSERT_INT("drag release handled",
               glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP),
               1);
    ASSERT_TRUE("drag inactive after release", !variable_panel_drag_active());
    ASSERT_INT("undo flag cleared after release",
               variable_panel_drag_undo_snapshot_pushed(), 0);

    editor_undo_pop_snapshot();
    ASSERT_STR("undo restores declaration source",
               editor_buffer_line(0), "  static float testvar = 1;");
    ASSERT_FLOAT("undo restores live predef value", g_predef_vars[var_idx].value, 1.0f);
}

static void test_variable_panel_motion_initializes_uninitialized_declaration(void) {
    int px, py, pw, ph;
    int click_x, click_y;
    int hit_row;
    int window_h;
    int var_idx;
    EditorUndoRingState undo_state;

    printf("--- imrepl_ctrl variable panel initializes decl ---\n");

    glr_app_reset_all();
    ui_state_viewport_set_size(1000, 1000);
    variable_panel_set_visible(1);
    editor_feed_line("float testvar;");

    var_idx = repl_eval_find_predef_var_idx("testvar");
    ASSERT_TRUE("uninitialized testvar declared", var_idx >= 0);
    ASSERT_FLOAT("uninitialized testvar starts at 0",
                 g_predef_vars[var_idx].value, 0.0f);

    ui_variable_panel_rect_for_count(g_num_predef_vars, &px, &py, &pw, &ph);
    window_h = ui_state_viewport().window_h;
    click_x = px + pw / 2;
    click_y = -1;
    for (int gl_y = py; gl_y < py + ph; gl_y++) {
        int candidate_y = window_h - gl_y;
        hit_row = -1;
        if (ui_variable_panel_hit_for_count(click_x, candidate_y,
                                            g_num_predef_vars, &hit_row)
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
    ASSERT_STR("uninitialized drag adds explicit initializer",
               editor_buffer_line(0), "  static float testvar = 5;");
    ASSERT_FLOAT("uninitialized drag updates live predef value",
                 g_predef_vars[var_idx].value, 5.0f);

    ASSERT_INT("uninitialized drag release handled",
               glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP),
               1);
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

static void test_pointer_state_tracks_controller_mouse_routes(void) {
    printf("--- imrepl_ctrl pointer state routing ---\n");

    glr_app_reset_all();
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

/* Grid/axes in-out transition wiring: the controller diffs the
 * presentation theme, ticks g_grid_xn/g_axes_xn, and writes the
 * effective {theme, opacity, phase} into SceneRenderConfig. Drives the
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
     * config.h doesn't break this test. +6 ticks of margin. */
    const int settle = (int)((GRID_FADE_OUT_SECS + GRID_FADE_IN_SECS)
                             / 0.016f) + 6;

    /* 1. First frame is a SNAP — rule 8 seeding (in glr_app_reset_all)
     *    means the non-off default grid does NOT animate in at startup. */
    glr_ctrl_display_frame();
    ASSERT_INT("snap grid theme = default",
               g_last_scene_config.grid_theme, CFG_DEFAULT_GRID_THEME);
    ASSERT_FLOAT("snap grid opacity 1", g_last_scene_config.grid_opacity, 1.0f);
    ASSERT_INT("snap grid phase STEADY",
               g_last_scene_config.grid_xn_phase, SCENE_XN_STEADY);
    ASSERT_FLOAT("snap axes opacity 1", g_last_scene_config.axes_opacity, 1.0f);
    ASSERT_INT("snap axes phase STEADY",
               g_last_scene_config.axes_xn_phase, SCENE_XN_STEADY);

    /* 2. A theme change drives FADE_OUT (old theme still drawn) then,
     *    past the opacity-0 crossing, FADE_IN of the adopted theme. */
    glr_state_presentation_mut()->grid_theme = GRID_THEME_CLASSIC;
    glr_ctrl_tick();
    glr_ctrl_display_frame();
    ASSERT_INT("OUT keeps old theme",
               g_last_scene_config.grid_theme, CFG_DEFAULT_GRID_THEME);
    ASSERT_INT("phase FADE_OUT",
               g_last_scene_config.grid_xn_phase, SCENE_XN_FADE_OUT);
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
               g_last_scene_config.grid_xn_phase, SCENE_XN_STEADY);

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
     *    scene_xn_show, so the new theme fades IN from 0 with NO
     *    preceding FADE_OUT (current jumps straight to the theme). */
    glr_state_presentation_mut()->grid_theme = GRID_THEME_OFF;
    for (int i = 0; i < settle; i++) glr_ctrl_tick();   /* settle to off */
    glr_state_presentation_mut()->grid_theme = GRID_THEME_CLASSIC;
    glr_ctrl_tick();
    glr_ctrl_display_frame();
    ASSERT_INT("show: current jumps straight to theme",
               g_last_scene_config.grid_theme, GRID_THEME_CLASSIC);
    ASSERT_INT("show: phase FADE_IN (no dead OUT)",
               g_last_scene_config.grid_xn_phase, SCENE_XN_FADE_IN);
    ASSERT_TRUE("show: fading up from 0",
                g_last_scene_config.grid_opacity > 0.0f &&
                g_last_scene_config.grid_opacity < 1.0f);

    /* 5. glr_app_reset_all() re-seeds both machines to the post-reset
     *    presentation at full opacity, STEADY — no post-reset animation. */
    glr_app_reset_all();
    glr_ctrl_display_frame();
    ASSERT_INT("reset snaps grid to default",
               g_last_scene_config.grid_theme, CFG_DEFAULT_GRID_THEME);
    ASSERT_FLOAT("reset grid opacity 1",
                 g_last_scene_config.grid_opacity, 1.0f);
    ASSERT_INT("reset grid STEADY",
               g_last_scene_config.grid_xn_phase, SCENE_XN_STEADY);
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

    glr_state_presentation_mut()->ortho_mode = 1;
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

    glr_state_presentation_mut()->ortho_mode = 0;
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
    glr_state_presentation_mut()->ortho_mode = 1;
    tick_until_view_settled(400);
    cam = glr_camera();
    ASSERT_FLOAT("camera flattened to rx=0", cam.rx, 0.0f);
    ASSERT_FLOAT("camera flattened to ry=0", cam.ry, 0.0f);

    /* 2) An example load (via the camera bridge) reports the new 3D
     *    target pose while we're still in 2D. */
    glr_ctrl_view_record_external_3d_pose(35.0f, 70.0f, 0.0f);

    /* 3) Returning to 3D should ease toward the reported pose, not the
     *    snapshot captured on 2D entry. */
    glr_state_presentation_mut()->ortho_mode = 0;
    tick_until_view_settled(400);
    cam = glr_camera();
    ASSERT_FLOAT("camera-to-3d uses refreshed rx", cam.rx, 35.0f);
    ASSERT_FLOAT("camera-to-3d uses refreshed ry", cam.ry, 70.0f);
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
    glr_state_presentation_mut()->ortho_mode = 1;
    tick_until_view_settled(400);
    glr_state_presentation_mut()->ortho_mode = 0;
    tick_until_view_settled(400);
    cam_before = glr_camera();

    /* Fire the bridge while in 3D — the saved snapshot must be ignored. */
    glr_ctrl_view_record_external_3d_pose(99.0f, 99.0f, 99.0f);

    glr_state_presentation_mut()->ortho_mode = 1;
    tick_until_view_settled(400);
    glr_state_presentation_mut()->ortho_mode = 0;
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
               "quit-recovery.c");

    glr_app_reset_all();
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

int main(void) {
    printf("--- imrepl_ctrl tests ---\n");

    test_display_frame_builds_config_and_restores_live_state();
    test_reshape_clamps_height();
    test_display_frame_profile_coverage();
    test_variable_panel_motion_routes_through_compile_and_coalesces_undo();
    test_variable_panel_motion_initializes_uninitialized_declaration();
    test_pointer_state_tracks_controller_mouse_routes();
    test_overlay_transition_machine_wiring();
    test_view_mode_projection_transition_wiring();
    test_view_record_external_3d_pose_tracks_in_ortho();
    test_view_record_external_3d_pose_noop_in_perspective();
    test_quit_recovery_file();

    printf("\n");
    return test_harness_report(&g_harness, "test_imrepl_ctrl");
}
