#include <stdio.h>
#include <string.h>
#include "editor_undo.h"
#include "ui/layout.h"   /* CODE_PANEL_LAYOUT_* */
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
#define replay_ui_hud_render               test_replay_ui_hud_render
#define ui_panels_render_code_panel        test_ui_panels_render_code_panel
#define ui_autocomplete_panel_render       test_ui_autocomplete_panel_render
#define ui_menu_bar_render_example_dropdown test_ui_menu_bar_render_example_dropdown
#define ui_variable_panel_render           test_ui_variable_panel_render
#define ui_panels_render_scene_status      test_ui_panels_render_scene_status
#define ui_tabbed_overlay_render           test_ui_tabbed_overlay_render
#define ui_profile_panel_render            test_ui_profile_panel_render

#include "imrepl_ctrl.c"

#undef scene_render_3d_scene
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
    ReplPresentationState *presentation;

    memset(&g_last_scene_config, 0, sizeof(g_last_scene_config));
    memset(&g_last_replay_hud_state, 0, sizeof(g_last_replay_hud_state));
    g_scene_render_calls = 0;
    g_replay_hud_calls = 0;
    g_predef_value_seen_in_scene = 0.0f;
    g_predef_value_seen_in_hud = 0.0f;
    g_flat_count_seen_in_hud = -1;
    g_scratch_value_seen_in_scene = 0.0f;
    g_t_idx = -1;

    repl_reset_state();
    repl_eval_init_predef_vars();

    ui_state_viewport_set_size(800, 600);
    ui_state_camera_set(11.0f, 22.0f, 7.5f, 0.5f, -0.25f, 1.75f, 0.9f);
    repl_state_render_mut()->use_accum = 0;
    repl_state_render_mut()->accum_aa_enabled = 0;
    repl_state_render_mut()->accum_samples = 1;
    repl_state_render_mut()->multisample_enabled = 0;
    repl_state_render_mut()->line_smooth_enabled = 1;
    repl_state_render_mut()->clear_color[0] = 0.0f;
    repl_state_render_mut()->clear_color[1] = 0.0f;
    repl_state_render_mut()->clear_color[2] = 0.0f;
    repl_state_render_mut()->clear_color[3] = 1.0f;

    repl_state_flat_program_set_user_lighting_enabled(1);

    presentation = repl_state_presentation_mut();
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
    repl_state_edit_line_set(repl_state_document_count());
    editor_input_set_text("glColor3f(0.2, 0.4, 0.6);");
    editor_cursor_pos_set(7);

    g_t_idx = repl_eval_find_predef_var_idx("t");
    ASSERT_TRUE("t predef exists", g_t_idx >= 0);
    if (g_t_idx >= 0)
        g_predef_vars[g_t_idx].value = 9.0f;
    repl_eval_scratch_set(0, 0, 4.0f);

    repl_replay_start();
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

    imrepl_ctrl_display_frame();

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
    imrepl_ctrl_reshape(640, 0);

    ASSERT_INT("reshape forwards width", ui_state_viewport().window_w, 640);
    ASSERT_INT("reshape clamps height", ui_state_viewport().window_h, 1);
}

/* Regression test for the profile-coverage bug: a frame should land
 * profile samples in every major section, and the sum of major
 * sections should approximate PROF_FRAME_TOTAL (no section big
 * enough to matter goes unprofiled). The test runs one
 * imrepl_ctrl_display_frame() and inspects the profile state.
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
    mark_normals_dirty();
    repl_state_mark_flat_dirty();
    imrepl_ctrl_display_frame();

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
    ReplUndoRingState undo_state;

    printf("--- imrepl_ctrl variable panel drag route ---\n");

    repl_reset_state();
    ui_state_viewport_set_size(1000, 1000);
    variable_panel_set_visible(1);
    repl_feed_line_public("float testvar = 1.0;");

    var_idx = repl_eval_find_predef_var_idx("testvar");
    ASSERT_TRUE("testvar declared", var_idx >= 0);
    ASSERT_FLOAT("testvar starts at 1", g_predef_vars[var_idx].value, 1.0f);

    ui_variable_panel_rect(&px, &py, &pw, &ph);
    window_h = ui_state_viewport().window_h;
    click_x = px + pw / 2;
    click_y = -1;
    for (int gl_y = py; gl_y < py + ph; gl_y++) {
        int candidate_y = window_h - gl_y;
        hit_row = -1;
        if (ui_variable_panel_hit(click_x, candidate_y, &hit_row)
                && hit_row == var_idx) {
            click_y = candidate_y;
            break;
        }
    }
    ASSERT_TRUE("found click target for testvar row", click_y >= 0);

    ASSERT_INT("drag begin handled",
               imrepl_ctrl_router_handle_variable_panel_drag_begin(
                   GLUT_LEFT_BUTTON, GLUT_DOWN, click_x, click_y),
               1);
    ASSERT_TRUE("drag active after begin", variable_panel_drag_active());

    for (int step = 1; step <= 10; step++) {
        ASSERT_INT("drag motion handled",
                   imrepl_ctrl_router_handle_variable_panel_motion(
                       click_x + step * 10, click_y),
                   1);
    }

    editor_undo_ring_state_capture(&undo_state);
    ASSERT_INT("drag motions coalesce to one undo snapshot",
               undo_state.undo_count, 1);
    ASSERT_STR("drag rewrites declaration source through compiler",
               editor_buffer_line(0), "  float testvar = 6;");
    ASSERT_FLOAT("drag updates live predef value", g_predef_vars[var_idx].value, 6.0f);

    ASSERT_INT("drag release handled",
               imrepl_ctrl_router_handle_variable_panel_drag_release(GLUT_UP),
               1);
    ASSERT_TRUE("drag inactive after release", !variable_panel_drag_active());
    ASSERT_INT("undo flag cleared after release",
               variable_panel_drag_undo_snapshot_pushed(), 0);

    editor_undo_pop_snapshot();
    ASSERT_STR("undo restores declaration source",
               editor_buffer_line(0), "  float testvar = 1;");
    ASSERT_FLOAT("undo restores live predef value", g_predef_vars[var_idx].value, 1.0f);
}

static void test_variable_panel_motion_initializes_uninitialized_declaration(void) {
    int px, py, pw, ph;
    int click_x, click_y;
    int hit_row;
    int window_h;
    int var_idx;
    ReplUndoRingState undo_state;

    printf("--- imrepl_ctrl variable panel initializes decl ---\n");

    repl_reset_state();
    ui_state_viewport_set_size(1000, 1000);
    variable_panel_set_visible(1);
    repl_feed_line_public("float testvar;");

    var_idx = repl_eval_find_predef_var_idx("testvar");
    ASSERT_TRUE("uninitialized testvar declared", var_idx >= 0);
    ASSERT_FLOAT("uninitialized testvar starts at 0",
                 g_predef_vars[var_idx].value, 0.0f);

    ui_variable_panel_rect(&px, &py, &pw, &ph);
    window_h = ui_state_viewport().window_h;
    click_x = px + pw / 2;
    click_y = -1;
    for (int gl_y = py; gl_y < py + ph; gl_y++) {
        int candidate_y = window_h - gl_y;
        hit_row = -1;
        if (ui_variable_panel_hit(click_x, candidate_y, &hit_row)
                && hit_row == var_idx) {
            click_y = candidate_y;
            break;
        }
    }
    ASSERT_TRUE("found click target for uninitialized testvar row", click_y >= 0);

    ASSERT_INT("uninitialized drag begin handled",
               imrepl_ctrl_router_handle_variable_panel_drag_begin(
                   GLUT_LEFT_BUTTON, GLUT_DOWN, click_x, click_y),
               1);
    ASSERT_TRUE("uninitialized drag active after begin",
                variable_panel_drag_active());

    for (int step = 1; step <= 10; step++) {
        ASSERT_INT("uninitialized drag motion handled",
                   imrepl_ctrl_router_handle_variable_panel_motion(
                       click_x + step * 10, click_y),
                   1);
    }

    editor_undo_ring_state_capture(&undo_state);
    ASSERT_INT("uninitialized drag motions coalesce to one undo snapshot",
               undo_state.undo_count, 1);
    ASSERT_STR("uninitialized drag adds explicit initializer",
               editor_buffer_line(0), "  float testvar = 5;");
    ASSERT_FLOAT("uninitialized drag updates live predef value",
                 g_predef_vars[var_idx].value, 5.0f);

    ASSERT_INT("uninitialized drag release handled",
               imrepl_ctrl_router_handle_variable_panel_drag_release(GLUT_UP),
               1);
    ASSERT_TRUE("uninitialized drag inactive after release",
                !variable_panel_drag_active());
    ASSERT_INT("uninitialized undo flag cleared after release",
               variable_panel_drag_undo_snapshot_pushed(), 0);

    editor_undo_pop_snapshot();
    ASSERT_STR("undo restores bare declaration",
               editor_buffer_line(0), "  float testvar;");
    ASSERT_FLOAT("undo restores live value to zero",
                 g_predef_vars[var_idx].value, 0.0f);
}

int main(void) {
    printf("--- imrepl_ctrl tests ---\n");

    test_display_frame_builds_config_and_restores_live_state();
    test_reshape_clamps_height();
    test_display_frame_profile_coverage();
    test_variable_panel_motion_routes_through_compile_and_coalesces_undo();
    test_variable_panel_motion_initializes_uninitialized_declaration();

    printf("\n");
    return test_harness_report(&g_harness, "test_imrepl_ctrl");
}
