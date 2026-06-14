#define _DEFAULT_SOURCE  /* mkdtemp() */
#include "editor/state.h"
#include "app/glr_camera.h"
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "app/glr_defaults.h"
#include "config.h"      /* QUIT_RECOVERY_FILE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "editor/undo.h"
#include "repl/core.h"
#include "editor/input.h"
#include "ui/app/layout.h"   /* CODE_PANEL_LAYOUT_* */
#include "support/test_harness.h"

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
#define scene_render_3d_scene              test_scene_render_3d_scene
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
#define glutPostRedisplay                  test_glutPostRedisplay
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
void test_glutPostRedisplay(void);
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

#undef scene_render_3d_scene
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
#undef glutPostRedisplay
#undef glutSetCursor
#undef glr_export_mesh_ply

static SceneRenderConfig g_last_scene_config;
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

int test_scene_render_3d_scene(SceneRendererState *state,
                               const SceneRenderConfig *config) {
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
void test_glutPostRedisplay(void) {}
void test_glutSetCursor(int cursor) { (void)cursor; }
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
    glr_state_render_mut()->accum_effect = SCENE_ACCUM_EFFECT_OFF;
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
    presentation->xform_guide_mode = (SceneXformGuideMode)9; /* out-of-range: exercises clamp path */
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
    /* viewport_w/h removed from SceneRenderConfig — scene helpers use
     * scene_w/scene_h (the active GL viewport) instead. The HUD asserts
     * below read the window viewport from the snapshot directly. */
    ASSERT_FLOAT("camera distance forwarded", g_last_scene_config.cam_dist, 7.5f);
    ASSERT_FLOAT("camera tx forwarded", g_last_scene_config.cam_tx, 0.5f);
    ASSERT_FLOAT("camera glow forwarded", g_last_scene_config.cam_motion_glow, 0.9f);
    ASSERT_INT("user lighting copied", g_last_scene_config.user_lighting_enabled, 1);
    ASSERT_INT("light indicators copied", g_last_scene_config.show_light_indicators, 1);
    /* The replay-fade plan moved out of SceneRenderConfig and is now a
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
    ASSERT_INT("HUD replaying on snap", g_last_replay_hud_snap.replay.active, 1);

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
    repl_mark_source_dirty();
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
        PROF_MEMORY_PANEL,
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
        prof_section_last_us(PROF_MEMORY_PANEL) +
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
                 1.0f + 5.0f * GLR_ADJUST_FINE_SCALE);
    ASSERT_STR("fine drag rewrites declaration source",
               editor_buffer_line(0), "  static float testvar = 2;");

    ASSERT_INT("fine drag release handled",
               glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP),
               1);
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
    ASSERT_FLOAT("reset drag updates live t value", g_predef_vars[var_idx].value, 5.0f);

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

/* Audit #18 (Tier B, commit 783d7e3) regression: variable_drag must arrive
 * in UiRenderSnapshot from the controller's snapshot-build phase, not be
 * re-fetched from peer file-statics inside ui_variable_panel_render. Pin
 * that glr_ctrl_build_ui_snapshot copies both fields (active_var, log_mode)
 * — a revert to live peer reads would silently still pass every existing
 * variable_drag test but would re-introduce the snapshot-purity violation. */
static void test_variable_drag_snapshot_wiring(void) {
    printf("--- imrepl_ctrl variable_drag snapshot wiring ---\n");

    prepare_display_fixture();
    ASSERT_INT("no drag active before begin",
               variable_panel_drag_active(), 0);

    variable_panel_handle_drag_begin(0, /*log_mode=*/1, /*x=*/100);
    ASSERT_INT("drag active after begin",
               variable_panel_drag_active(), 1);
    ASSERT_INT("active var seeded", variable_panel_drag_active_var(), 0);
    ASSERT_INT("log mode seeded", variable_panel_drag_log_mode(), 1);

    glr_ctrl_display_frame();

    ASSERT_INT("snap.variable_drag.active_var arrives in snapshot",
               g_last_replay_hud_snap.variable_drag.active_var, 0);
    ASSERT_INT("snap.variable_drag.log_mode arrives in snapshot",
               g_last_replay_hud_snap.variable_drag.log_mode, 1);

    /* Release: the next frame's snapshot must reflect the cleared state
     * (proves the snapshot path is re-evaluated, not stale-cached). */
    variable_panel_handle_drag_reset();
    glr_ctrl_display_frame();
    ASSERT_INT("snap.variable_drag.active_var clears after release",
               g_last_replay_hud_snap.variable_drag.active_var, -1);

    /* Reset for next test. */
    glr_ctrl_reset_all();
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
     * scene_transition.h doesn't break this test. +6 ticks of margin. */
    const int settle = (int)((GRID_FADE_OUT_SECS + GRID_FADE_IN_SECS)
                             / 0.016f) + 6;

    /* 1. First frame is a SNAP — rule 8 seeding (in glr_ctrl_reset_all)
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

    /* 5. glr_ctrl_reset_all() re-seeds both machines to the post-reset
     *    presentation at full opacity, STEADY — no post-reset animation. */
    glr_ctrl_reset_all();
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

    glr_state_presentation_mut()->ortho_mode = SCENE_VIEW_2D;
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

    glr_state_presentation_mut()->ortho_mode = SCENE_VIEW_3D;
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
    glr_state_presentation_mut()->ortho_mode = SCENE_VIEW_2D;
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
    glr_state_presentation_mut()->ortho_mode = SCENE_VIEW_3D;
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
    glr_state_presentation_mut()->ortho_mode = SCENE_VIEW_2D;
    tick_until_view_settled(400);
    cam = glr_camera();
    ASSERT_FLOAT("camera flattened to rx=0", cam.rx, 0.0f);
    ASSERT_FLOAT("camera flattened to ry=0", cam.ry, 0.0f);

    /* 2) An example load (via the camera bridge) reports the new 3D
     *    target pose while we're still in 2D. */
    glr_ctrl_view_record_external_3d_pose(35.0f, 70.0f, 0.0f);

    /* 3) Returning to 3D should ease toward the reported pose, not the
     *    snapshot captured on 2D entry. */
    glr_state_presentation_mut()->ortho_mode = SCENE_VIEW_3D;
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
    glr_state_presentation_mut()->ortho_mode = SCENE_VIEW_2D;
    tick_until_view_settled(400);
    glr_state_presentation_mut()->ortho_mode = SCENE_VIEW_3D;
    tick_until_view_settled(400);
    cam_before = glr_camera();

    /* Fire the bridge while in 3D — the saved snapshot must be ignored. */
    glr_ctrl_view_record_external_3d_pose(99.0f, 99.0f, 99.0f);

    glr_state_presentation_mut()->ortho_mode = SCENE_VIEW_2D;
    tick_until_view_settled(400);
    glr_state_presentation_mut()->ortho_mode = SCENE_VIEW_3D;
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
 * is what fields the controller must populate on SceneRenderConfig so
 * the (post-refactor) scene module can drive overlay passes directly.
 *
 * Most fields are already pinned by
 * test_display_frame_builds_config_and_restores_live_state; this test
 * adds the invariants that travel ACROSS frames — repeated frames must
 * not introduce hysteresis, and the post-overlays hook's user_data
 * must point at the config that hosts the guides (so the future
 * scene-side overlay code can rely on that handle). */
static void test_display_frame_scene_config_is_stable_across_frames(void) {
    SceneRenderConfig frame1;
    SceneRenderConfig frame2;

    printf("--- imrepl_ctrl scene config purity ---\n");
    prepare_display_fixture();
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;

    glr_ctrl_display_frame();
    frame1 = g_last_scene_config;

    glr_ctrl_display_frame();
    frame2 = g_last_scene_config;

    /* Stable inputs across frames. Note: anim_time advances inside
     * scene_render (via the replay tick), so don't pin that here —
     * the rest of the config is steady-state. */
    ASSERT_INT("scene_w stable across frames",
               frame2.scene_w, frame1.scene_w);
    ASSERT_INT("scene_h stable across frames",
               frame2.scene_h, frame1.scene_h);
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
     * the hook reads guides off SceneRenderConfig. Pin that pointer
     * identity — a refactor that switches user_data to NULL or to a
     * different pointer would break the guide overlay. */
    ASSERT_TRUE("post_overlays_fn wired in frame 1",
                frame1.post_overlays_fn != NULL);
    ASSERT_TRUE("post_overlays_fn wired in frame 2",
                frame2.post_overlays_fn != NULL);
    /* user_data IS the live config the controller passed into
     * scene_render_3d_scene (not the cached frame1/frame2 copies).
     * Catch it by reading g_last_scene_config.post_overlays_user_data
     * after the second frame and asserting it equals &g_last_scene_config
     * is intentionally NOT done — the controller hands the scene module
     * its OWN local SceneRenderConfig. We pin the looser invariant: the
     * hook always carries a non-NULL user_data alongside the fn. */
    ASSERT_TRUE("post_overlays_user_data non-NULL",
                frame2.post_overlays_user_data != NULL);
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

    /* Off replay the edit-cursor set returns: cursor on line 2 unions both
     * expansions, so all three transform lines light up. */
    replay_stop();
    glr_ctrl_push_highlights();
    ASSERT_INT("post-replay cursor union shows first translate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 5), 1);
    ASSERT_INT("post-replay cursor union shows the rotate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 7), 1);
    ASSERT_INT("post-replay cursor union shows second translate",
               count_highlight_kind_on_line(HIGHLIGHT_AFFECTING_TRANSFORM, 8), 1);
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
    ASSERT_FLOAT("variable panel changed t", g_predef_vars[t_idx].value, 2.5f);
    ASSERT_INT("panel t change marks flat dirty",
               repl_state_flat_program_dirty(), 1);

    glr_ctrl_display_frame();

    ASSERT_TRUE("post-frame flat vertex exists", first_flat_vertex_x(&x));
    ASSERT_FLOAT("post-frame flat vertex uses panel t", x, 2.5f);
    ASSERT_FLOAT("time remains paused at panel value",
                 g_predef_vars[t_idx].value, 2.5f);
    ASSERT_INT("auto time remains off",
               repl_state_variables().time_playing, 0);

    repl_state_flat_program_clear_dirty();
    variable_panel_handle_drag_begin(t_idx, 0, 0);
    ASSERT_TRUE("same-value variable-panel t motion consumed",
                glr_ctrl_router_handle_variable_panel_motion(0, 0));
    ASSERT_FLOAT("same-value variable-panel leaves t unchanged",
                 g_predef_vars[t_idx].value, 2.5f);
    ASSERT_INT("same-value variable-panel leaves flat clean",
               repl_state_flat_program_dirty(), 0);
}

/* scene_execute_adapter is called by render.c on both the main fill
 * pass and the scene_probe_eye_dist feedback pass. The probe pass
 * runs every frame in ortho/projection-transition mode; before the
 * SceneExecutePurpose wiring its execute_fn invocation mutated REPL
 * state (predef vars, scratch arrays, light enables, clear_color)
 * the same as the main fill, so the user's `t = t + 1` style code
 * advanced twice per frame and the probe's glEnable(GL_LIGHT0) /
 * glClearColor() leaked across frames (the frame-end restore in
 * glr_ctrl_display_frame only snapshots predef + scratch, not the
 * persistent render state).
 *
 * This test exercises the adapter directly with both purposes and
 * pins the invariant: DEPTH_PROBE doesn't mutate predef vars or
 * scratch arrays; MAIN_FILL still does (would-be-regression for the
 * fix accidentally suppressing the real path). Clear-color and
 * light-enable side effects are intentionally excluded here — the
 * parser clamps glClearColor channels to 0.15 max, which complicates
 * a clean test signal, but the snapshot/restore path covers them the
 * same way it covers the predef/scratch state. */
static void test_depth_probe_does_not_mutate_repl_state(void) {
    printf("--- depth probe does not mutate REPL state (#2 P1 review) ---\n");

#ifndef GL_STUBS
    printf("Run `make test_glr_ctrl USE_GL_STUBS=1` for depth-probe adapter coverage.\n");
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
    SceneExecuteContext probe_ctx = { .purpose = SCENE_EXEC_DEPTH_PROBE };
    scene_execute_adapter(&probe_ctx, NULL);

    float predef_after_probe = g_predef_vars[probevar_idx].value;
    float scratch_after_probe;
    repl_eval_scratch_get(scratch_a_idx, 0, &scratch_after_probe);

    ASSERT_FLOAT("probe: probevar unchanged", predef_after_probe, predef_before);
    ASSERT_FLOAT("probe: A[0] unchanged", scratch_after_probe, scratch_before);

    /* Main fill call: state SHOULD mutate. */
    SceneExecuteContext fill_ctx = { .purpose = SCENE_EXEC_MAIN_FILL };
    scene_execute_adapter(&fill_ctx, NULL);

    float predef_after_fill = g_predef_vars[probevar_idx].value;
    float scratch_after_fill;
    repl_eval_scratch_get(scratch_a_idx, 0, &scratch_after_fill);

    ASSERT_FLOAT("fill: probevar advanced once",
                 predef_after_fill, predef_before + 1.0f);
    ASSERT_FLOAT("fill: A[0] advanced once",
                 scratch_after_fill, scratch_before + 1.0f);
}

/* Regression: loading an example resets the light_theme *name* to the
 * default, and that reset must ALSO re-apply the theme to the app-state
 * lights[] (positions / colors / eye-space). Before the fix
 * glr_ctrl_reset_example_chrome reset the cfg field directly — bypassing
 * the scene_lights_apply_theme hook in glr_config_set — so the lights[]
 * array (and the light indicators that read it) stayed on the *previous*
 * theme: the name said default but the geometry stayed e.g. SOLAR.
 *
 * The dimensional light table is app-owned (GlrRenderState.lights); the
 * REPL pipeline owns only the enable bitmask, so this checks app state. */
static void test_example_reset_reapplies_light_theme(void) {
    printf("--- glr_ctrl example reset re-applies light theme to lights[] ---\n");

    SceneLight expected_default[MAX_LIGHTS];
    scene_lights_apply_theme(expected_default, LIGHT_THEME_DEFAULT);

    /* Switch to a non-default theme through the real setter, which fires
     * the scene_lights_apply_theme hook, so lights[] holds the SOLAR preset. */
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

/* The light-split contract: SceneRenderConfig.lights[] is assembled per frame
 * from two owners — the app-owned theme-seeded dimensional data
 * (GlrRenderState.lights: position / color / id / eye-space) merged with the
 * REPL-owned enable bitmask (ReplRenderState.light_enabled_mask). This drives
 * a frame (scene render is stubbed, so the executor never re-derives the mask)
 * and checks each slot's `.enabled` tracks the mask while the rest tracks the
 * theme. */
static void test_display_frame_merges_light_theme_and_enable_mask(void) {
    printf("--- imrepl_ctrl light merge (theme + enable mask) ---\n");
    prepare_display_fixture();

    SceneLight theme[MAX_LIGHTS];
    scene_lights_apply_theme(theme, LIGHT_THEME_SOLAR);

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
        const SceneLight *l = &render.lights[slot];
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
    glr_ctrl_special(GLUT_KEY_F3, 0, 0);
    ASSERT_TRUE("F3 toggled grid config", glr_config_get(GLR_CONFIG_GRID_THEME) != old_grid);

    /* 3. Audio special shortcuts (Ctrl+Left / Ctrl+Right) */
    g_simulated_mods = GLUT_ACTIVE_CTRL;
    int rc = glr_ctrl_router_handle_horizontal_audio_special(GLUT_KEY_LEFT);
    ASSERT_INT("audio prev handled", rc, 1);
    rc = glr_ctrl_router_handle_horizontal_audio_special(GLUT_KEY_RIGHT);
    ASSERT_INT("audio next handled", rc, 1);
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

    /* 6. Export special (F11) */
    rc = glr_ctrl_router_handle_export_special(GLUT_KEY_F11);
    ASSERT_INT("export special handled", rc, 1);

    editor_input_set_modifier_provider_for_test(NULL);
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
    printf("Run `make test_glr_ctrl USE_GL_STUBS=1` for init_gl point-parameter loader coverage.\n");
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
    editor_scroll_follow_cursor_set(1);
    editor_state_edit_line_set(80);

    glr_ctrl_display_frame();

    glr_ctrl_code_panel_apply_scroll_follow_for_test(
        &g_last_ui_snapshot, &follow_doc_line, &visible_lines);

    ASSERT_TRUE("scroll moved down to follow cursor", editor_scroll() > 0);
    ASSERT_TRUE("cursor is inside visible range",
                follow_doc_line >= editor_scroll() &&
                follow_doc_line < editor_scroll() + visible_lines);
}

int main(void) {
    printf("--- imrepl_ctrl tests ---\n");

    test_display_frame_builds_config_and_restores_live_state();
    test_reshape_clamps_height();
    test_display_frame_profile_coverage();
    test_variable_panel_motion_routes_through_compile_and_coalesces_undo();
    test_variable_panel_shift_left_drag_uses_fine_scale();
    test_variable_panel_motion_preserves_reset_assignment_without_declaration();
    test_variable_panel_motion_initializes_uninitialized_declaration();
    test_variable_drag_snapshot_wiring();
    test_pointer_state_tracks_controller_mouse_routes();
    test_overlay_transition_machine_wiring();
    test_view_mode_projection_transition_wiring();
    test_view_mode_3d_to_2d_uses_faster_decay();
    test_view_record_external_3d_pose_tracks_in_ortho();
    test_view_record_external_3d_pose_noop_in_perspective();
    test_quit_recovery_file();
    test_build_ui_snapshot_is_idempotent();
    test_display_frame_scene_config_is_stable_across_frames();
    test_display_frame_no_replay_means_no_fade_plumbing();
    test_display_frame_follows_replay_line_after_tick();
    test_replay_call_site_highlights_are_pushed();
    test_replay_focus_vertex_affecting_transforms();
    test_replay_focus_glut_solid_affecting_transforms();
    test_numeric_swatch_step_commits_line_and_undoes();
    test_numeric_swatch_no_op_outside_numeric_arg();
    test_numeric_swatch_no_op_in_insert_mode();
    test_numeric_swatch_scale_coarse_and_fine();
    test_variable_panel_t_change_reflattens_when_time_paused();
    test_depth_probe_does_not_mutate_repl_state();
    test_example_reset_reapplies_light_theme();
    test_display_frame_merges_light_theme_and_enable_mask();
    test_export_light_bridge_reads_app_state();
    test_mouse_routing_and_hit_testing();
    test_special_key_shortcuts();
    test_app_lifecycle_bootstrap_shutdown();
    test_init_gl_requires_loaded_point_parameter_proc();
    test_code_panel_scroll_clamping_and_follow();

    printf("\n");
    return test_harness_report(&g_harness, "test_imrepl_ctrl");
}
