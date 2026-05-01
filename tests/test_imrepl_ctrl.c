#include <math.h>
#include <stdio.h>
#include <string.h>

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
    float delta = (float)(got) - (float)(exp); \
    if (delta < 0.0f) delta = -delta; \
    if (delta < 1e-5f) g_pass++; \
    else printf("FAIL [%s] got %.6f, expected %.6f (line %d)\n", \
                label, (float)(got), (float)(exp), __LINE__); \
} while (0)

/* Rename the controller's downstream render delegates so this test can stub
 * them and inspect the per-frame config without a real GL context. */
#define scene_render_3d_scene              test_scene_render_3d_scene
#define ui_replay_hud_render               test_ui_replay_hud_render
#define ui_panels_render_code_panel        test_ui_panels_render_code_panel
#define ui_autocomplete_panel_render       test_ui_autocomplete_panel_render
#define ui_menu_bar_render_example_dropdown test_ui_menu_bar_render_example_dropdown
#define ui_variable_panel_render           test_ui_variable_panel_render
#define ui_panels_render_scene_status      test_ui_panels_render_scene_status
#define ui_help_overlay_render             test_ui_help_overlay_render
#define ui_profile_panel_render            test_ui_profile_panel_render

#include "imrepl_ctrl.c"

#undef scene_render_3d_scene
#undef ui_replay_hud_render
#undef ui_panels_render_code_panel
#undef ui_autocomplete_panel_render
#undef ui_menu_bar_render_example_dropdown
#undef ui_variable_panel_render
#undef ui_panels_render_scene_status
#undef ui_help_overlay_render
#undef ui_profile_panel_render

static SceneRenderConfig g_last_scene_config;
static UiReplayHudState  g_last_replay_hud_state;
static int g_scene_render_calls = 0;
static int g_replay_hud_calls = 0;
static int g_t_idx = -1;
static float g_predef_value_seen_in_scene = 0.0f;
static float g_predef_value_seen_in_hud = 0.0f;
static int g_flat_count_seen_in_hud = -1;

static const float g_mutated_predef_value = 123.0f;
static const int g_mutated_flat_count = 99;

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
    snprintf(cmd->source, sizeof(cmd->source), "%s", source);
}

void test_scene_render_3d_scene(const SceneRenderConfig *config) {
    g_scene_render_calls++;
    g_last_scene_config = *config;

    if (g_t_idx >= 0) {
        g_predef_value_seen_in_scene = g_predef_vars[g_t_idx].value;
        g_predef_vars[g_t_idx].value = g_mutated_predef_value;
    }

    repl_state_flat_program_set_count(g_mutated_flat_count);
}

void test_ui_replay_hud_render(const UiReplayHudState *state) {
    g_replay_hud_calls++;
    g_last_replay_hud_state = *state;

    if (g_t_idx >= 0)
        g_predef_value_seen_in_hud = g_predef_vars[g_t_idx].value;
    g_flat_count_seen_in_hud = repl_state_flat_program_count();
}

void test_ui_panels_render_code_panel(const UiRenderSnapshot *snap) { (void)snap; }
void test_ui_autocomplete_panel_render(const UiRenderSnapshot *snap) { (void)snap; }
void test_ui_menu_bar_render_example_dropdown(const UiRenderSnapshot *snap) { (void)snap; }
void test_ui_variable_panel_render(const UiRenderSnapshot *snap) { (void)snap; }
void test_ui_panels_render_scene_status(const UiRenderSnapshot *snap) { (void)snap; }
void test_ui_help_overlay_render(const UiRenderSnapshot *snap) { (void)snap; }
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
    g_t_idx = -1;

    repl_reset_state();
    repl_eval_init_predef_vars();

    repl_state_viewport_set_size(800, 600);
    repl_state_camera_set(11.0f, 22.0f, 7.5f, 0.5f, -0.25f, 1.75f, 0.9f);
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
    presentation->show_light_indicators = 1;
    presentation->highlight_current_poly = 1;
    presentation->xform_guide_mode = 2;
    presentation->code_panel_layout = CODE_PANEL_LAYOUT_BOTTOM;

    repl_state_variables_mut()->anim_time = 4.25f;
    repl_state_insert_mode_set(1);

    doc_cmds = repl_state_document_cmds_mut();
    flat_cmds = repl_state_flat_program_cmds_mut();
    set_cmd3(&doc_cmds[0], CMD_VERTEX3F, 0, "glVertex3f(1, 2, 3);",
             1.0f, 2.0f, 3.0f);
    set_cmd3(&doc_cmds[1], CMD_COLOR3F, 1, "glColor3f(0.2, 0.4, 0.6);",
             0.2f, 0.4f, 0.6f);
    set_cmd3(&flat_cmds[0], CMD_VERTEX3F, 0, "glVertex3f(1, 2, 3);",
             1.0f, 2.0f, 3.0f);
    set_cmd3(&flat_cmds[1], CMD_COLOR3F, 1, "glColor3f(0.2, 0.4, 0.6);",
             0.2f, 0.4f, 0.6f);
    repl_state_document_count_set(2);
    repl_state_flat_program_set_count(2);
    repl_state_edit_line_set(repl_state_document_count());
    repl_state_input_set_text("glColor3f(0.2, 0.4, 0.6);");
    repl_state_cursor_pos_set(7);

    g_t_idx = repl_eval_find_predef_var_idx("t");
    ASSERT_TRUE("t predef exists", g_t_idx >= 0);
    if (g_t_idx >= 0)
        g_predef_vars[g_t_idx].value = 9.0f;

    repl_state_replay_mut()->active = 1;
    repl_state_replay_mut()->state = REPLAY_PLAYING;
    repl_state_replay_mut()->pc = 1;
    repl_state_replay_mut()->mode = REPLAY_MODE_VERTEX;
    repl_state_replay_mut()->speed = 2.5f;
    repl_state_replay_mut()->expand_args = 1;
    repl_state_replay_mut()->total_flat_cmds = 777;

    repl_state_flat_program_clear_dirty();
    repl_state_normals_dirty_clear();
}

static void test_display_frame_builds_config_and_restores_live_state(void) {
    int saved_flat_count;
    float saved_t_value;
    ReplPredefView predef_view;

    printf("--- imrepl_ctrl display_frame ---\n");
    prepare_display_fixture();
    predef_view = repl_eval_predef_view();

    saved_flat_count = repl_state_flat_program_count();
    saved_t_value = g_predef_vars[g_t_idx].value;

    imrepl_ctrl_display_frame();

    ASSERT_INT("scene render called once", g_scene_render_calls, 1);
    ASSERT_INT("replay HUD called once", g_replay_hud_calls, 1);

    ASSERT_TRUE("scene execute fn wired", g_last_scene_config.execute_fn != NULL);
    ASSERT_TRUE("scene execute reset fn wired", g_last_scene_config.execute_reset_fn != NULL);
    ASSERT_TRUE("scene execute user data null", g_last_scene_config.execute_user_data == NULL);
    ASSERT_INT("viewport width forwarded", g_last_scene_config.viewport_w, 800);
    ASSERT_INT("viewport height forwarded", g_last_scene_config.viewport_h, 600);
    ASSERT_FLOAT("camera distance forwarded", g_last_scene_config.cam_dist, 7.5f);
    ASSERT_FLOAT("camera tx forwarded", g_last_scene_config.cam_tx, 0.5f);
    ASSERT_FLOAT("camera glow forwarded", g_last_scene_config.cam_motion_glow, 0.9f);
    ASSERT_INT("user lighting copied", g_last_scene_config.user_lighting_enabled, 1);
    ASSERT_INT("light indicators copied", g_last_scene_config.show_light_indicators, 1);
    ASSERT_INT("show guides copied", g_last_scene_config.show_guides, 1);
    ASSERT_INT("current poly hidden while replaying", g_last_scene_config.show_current_poly, 0);
    ASSERT_INT("replaying copied", g_last_scene_config.replaying, 1);
    ASSERT_INT("replay mode copied", g_last_scene_config.replay_mode, REPLAY_MODE_VERTEX);
    ASSERT_INT("replay tess preview enabled", g_last_scene_config.replay_tess_preview, 1);
    ASSERT_INT("replay vertex points enabled", g_last_scene_config.replay_vertex_points, 1);
    ASSERT_INT("replay fades absent", g_last_scene_config.replay_has_fades, 0);
    ASSERT_INT("replay base limit zero without fades", g_last_scene_config.replay_base_limit, 0);
    ASSERT_INT("replay fade batches empty", g_last_scene_config.replay_fade_plan.batch_count, 0);
    ASSERT_FLOAT("alpha scale boosted for black background", g_last_scene_config.alpha_scale, 3.0f);
    ASSERT_INT("cursor block begin cleared by refresh", g_last_scene_config.cursor_block_begin_idx, -1);
    ASSERT_INT("cursor block end cleared by refresh", g_last_scene_config.cursor_block_end_idx, -1);
    ASSERT_INT("cursor block source tracks edit line", g_last_scene_config.cursor_block_source_line, 2);
    ASSERT_INT("edit line copied", g_last_scene_config.edit_line_idx, 2);
    ASSERT_INT("cursor func scope mask empty", g_last_scene_config.cursor_func_scope_mask, 0);
    ASSERT_INT("cursor call src idx invalid", g_last_scene_config.cursor_call_src_cmd_idx, -1);
    ASSERT_INT("focus vertex valid", g_last_scene_config.focus.valid, 1);
    ASSERT_FLOAT("focus vertex x", g_last_scene_config.focus.pos[0], 1.0f);
    ASSERT_FLOAT("focus vertex y", g_last_scene_config.focus.pos[1], 2.0f);
    ASSERT_FLOAT("focus vertex z", g_last_scene_config.focus.pos[2], 3.0f);
    ASSERT_INT("flat program clamped for replay", g_last_scene_config.flat_program.cmd_count, 1);
    ASSERT_TRUE("flat program pointer forwarded",
                g_last_scene_config.flat_program.cmds == repl_state_flat_program_cmds_mut());

    ASSERT_TRUE("guide input pointer forwarded",
                g_last_scene_config.guide_snapshot.input == repl_state_input_text());
    ASSERT_INT("guide input length copied", g_last_scene_config.guide_snapshot.input_len,
               (int)strlen(repl_state_input_text()));
    ASSERT_INT("guide cursor copied", g_last_scene_config.guide_snapshot.cursor_pos, 7);
    ASSERT_INT("guide edit line copied", g_last_scene_config.guide_snapshot.edit_line_idx, 2);
    ASSERT_INT("guide insert mode copied", g_last_scene_config.guide_snapshot.inserting, 1);
    ASSERT_TRUE("guide source cmds forwarded",
                g_last_scene_config.guide_snapshot.source_cmds == repl_state_document_cmds_mut());
    ASSERT_INT("guide source count copied", g_last_scene_config.guide_snapshot.source_cmd_count, 2);
    ASSERT_TRUE("guide flat program forwarded",
                g_last_scene_config.guide_snapshot.flat_program.cmds == repl_state_flat_program_cmds_mut());
    ASSERT_INT("guide flat count copied", g_last_scene_config.guide_snapshot.flat_program.cmd_count, 1);
    ASSERT_TRUE("guide predef vars forwarded",
                g_last_scene_config.guide_snapshot.predef_vars == predef_view.vars);
    ASSERT_INT("guide predef count copied", g_last_scene_config.guide_snapshot.predef_var_count,
               predef_view.count);
    ASSERT_FLOAT("guide anim time copied", g_last_scene_config.guide_snapshot.anim_time, 4.25f);
    ASSERT_INT("guide xform mode copied", g_last_scene_config.guide_snapshot.xform_guide_mode, 2);
    ASSERT_INT("guide replaying copied", g_last_scene_config.guide_snapshot.replaying, 1);
    ASSERT_INT("guide show guides copied", g_last_scene_config.guide_snapshot.show_guides, 1);
    ASSERT_INT("guide user lighting copied", g_last_scene_config.guide_snapshot.user_lighting_enabled, 1);
    ASSERT_FLOAT("guide alpha scale copied", g_last_scene_config.guide_snapshot.alpha_scale, 3.0f);

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
    ASSERT_FLOAT("HUD observed mutated predef during frame", g_predef_value_seen_in_hud, g_mutated_predef_value);
    ASSERT_INT("HUD observed mutated flat count during frame", g_flat_count_seen_in_hud,
               g_mutated_flat_count);

    ASSERT_FLOAT("predef restored after frame", g_predef_vars[g_t_idx].value, saved_t_value);
    ASSERT_INT("flat count restored after frame", repl_state_flat_program_count(), saved_flat_count);
}

static void test_reshape_clamps_height(void) {
    printf("--- imrepl_ctrl reshape ---\n");

    repl_state_viewport_set_size(320, 240);
    imrepl_ctrl_reshape(640, 0);

    ASSERT_INT("reshape forwards width", repl_state_viewport().window_w, 640);
    ASSERT_INT("reshape clamps height", repl_state_viewport().window_h, 1);
}

int main(void) {
    printf("--- imrepl_ctrl tests ---\n");

    test_display_frame_builds_config_and_restores_live_state();
    test_reshape_clamps_height();

    printf("\n%d / %d tests passed\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
