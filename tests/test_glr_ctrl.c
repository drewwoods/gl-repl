#define _DEFAULT_SOURCE  /* mkdtemp() */
#include "editor/state.h"
#include "app/glr_camera.h"
#include "app/glr_camera_export.h"
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "app/glr_defaults.h"
#include "app/glr_pointer_script.h"
#include "config.h"      /* QUIT_RECOVERY_FILE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ui/app/repl_code_panel.h"
#include "subsystems/assign_plot/assign_plot.h"
#include "subsystems/console/console.h"
#include "editor/undo.h"
#include "repl/example_loader.h"
#include "repl/examples.h"
#include "repl/export.h"
#include "repl/flatten.h"
#include "repl/command_store.h"
#include "repl/state_owners.h"
#include "repl/workspace_io.h"  /* recovery-workspace cleanup */
#include "repl/time.h"
#include "repl/pipeline.h"
#include "repl/state_notify.h"
#include "repl/cfg_baseline.h"  /* scene-local roster bags via the cfg bridge */
#include "app/glr_actions.h"    /* glr_actions_install_export_cfg_bridge */
#include "editor/input.h"
#include "editor/search.h"
#include "keys.h"
#include "app/glr_prof.h"    /* glr_prof_section_is_gpu (summary-row policy) */
#include "ui/app/layout.h"   /* CODE_PANEL_LAYOUT_* */
#include "ui/core/metrics.h"
#include "support/gl_state_cell.h"
#include "support/test_harness.h"
#ifdef GL_STUBS
#include <GL/gl_stub_counts.h>
#include "gl_includes.h"  /* GL_INVALID_OPERATION, stub error seam */
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

/* Value cells compare by value, not by typesetting - see
 * tests/support/gl_state_cell.h. */
static void assert_cell_impl(const char *label, const char *got,
                             const char *exp) {
    char msg[256];
    snprintf(msg, sizeof(msg), "%s (got \"%s\", expected \"%s\")",
             label, got ? got : "(null)", exp ? exp : "(null)");
    ASSERT_TRUE(msg, gl_state_cell_matches(got, exp));
}

#define ASSERT_CELL(label, got, exp) assert_cell_impl(label, got, exp)

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
#define glutSetWindowTitle                 test_glutSetWindowTitle
/* The controller makes exactly one glClearColor call of its own - the chrome
 * strips - so intercepting it textually here says which background the strips
 * took, and when. Only glr_ctrl.c's own calls are redirected; the executor is
 * a separate TU, so the program's glClearColor still reaches the GL stubs. */
#define glClearColor                       test_chrome_glClearColor

/* glr_camera.h was already pulled in at line 3 (before the #define),
 * so its `glr_camera_load_modelview` declaration is preserved as the
 * real name. Forward-declare the test stub explicitly so glr_ctrl.c's
 * macro-substituted call has a visible prototype. The other stubs
 * are reached only through glr_ctrl.c's includes, which the macros
 * cover. */
void test_glr_camera_load_modelview(const GlrCameraPose *pose);
static float g_chrome_clear_rgba[4];
static int   g_chrome_clear_calls;
static void test_chrome_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    g_chrome_clear_rgba[0] = r; g_chrome_clear_rgba[1] = g;
    g_chrome_clear_rgba[2] = b; g_chrome_clear_rgba[3] = a;
    g_chrome_clear_calls++;
}
/* ui/subsystems/variable_panel.h is pulled in before the #define (via
 * ui/app/snapshot.h), so its real-name prototype is preserved too;
 * forward-declare this stub the same way. */
void test_ui_variable_panel_render(const UiVariablePanelView *view);
void test_glutSetCursor(int cursor);
int test_glr_export_mesh_ply(const char *path, int srgb_decode);
static char g_last_window_title[256];
static void test_glutSetWindowTitle(const char *title) {
    if (title)
        snprintf(g_last_window_title, sizeof(g_last_window_title), "%s", title);
    else
        g_last_window_title[0] = '\0';
}

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
#undef glutSetWindowTitle
#undef glClearColor

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

static int get_highlight_aux_on_line(UiHighlightKind kind, int line_idx, int match_index) {
    const UiHighlightList *list = editor_state_highlights();
    int match = 0;

    if (!list)
        return -1;
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].kind == kind &&
            list->items[i].line_idx == line_idx) {
            if (match == match_index)
                return list->items[i].aux;
            match++;
        }
    }
    return -1;
}

static int band_matches_aux(const float rgba[4], int aux) {
    float r = (float)((aux >> 16) & 0xFF) / 255.0f;
    float g = (float)((aux >> 8) & 0xFF) / 255.0f;
    float b = (float)(aux & 0xFF) / 255.0f;
    return (float)fabs(rgba[0] - r) < 0.02f &&
           (float)fabs(rgba[1] - g) < 0.02f &&
           (float)fabs(rgba[2] - b) < 0.02f;
}

/* Off (-1) by default: most display-frame tests want the config, not a
 * geometry walk. Set to a Render3dExecutePurpose to make the stub drive the
 * controller's real execute_fn the way render.c does, which is the only way
 * this frame's background observation gets produced. */
static int g_scene_stub_execute_purpose = -1;
/* Non-zero makes the stub reject the config: no callback runs, so nothing
 * publishes a background. */
static int g_scene_stub_reject = 0;
/* Chrome-clear count seen from inside the scene render, to pin ordering. */
static int g_chrome_clear_calls_at_scene = -1;

int test_scene_render_3d_scene(Render3dState *state,
                               const Render3dRenderConfig *config) {
    (void)state;
    g_scene_render_calls++;
    g_last_scene_config = *config;
    g_chrome_clear_calls_at_scene = g_chrome_clear_calls;

    if (g_scene_stub_reject)
        return -1;

    if (g_scene_stub_execute_purpose >= 0 && config->execute_fn) {
        Render3dExecuteContext ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.purpose = (Render3dExecutePurpose)g_scene_stub_execute_purpose;
        config->execute_fn(&ctx, config->execute_user_data);
    }

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

/* Chrome-clear count seen from the replay HUD, to pin the other side of the
 * ordering: the strips must be painted before any 2D overlay draws over
 * them. */
static int g_chrome_clear_calls_at_hud = -1;

void test_replay_ui_hud_render(const struct UiRenderSnapshot *snap) {
    g_replay_hud_calls++;
    g_last_replay_hud_snap = *snap;
    g_chrome_clear_calls_at_hud = g_chrome_clear_calls;

    if (g_t_idx >= 0)
        g_predef_value_seen_in_hud = g_predef_vars[g_t_idx].value;
    g_flat_count_seen_in_hud = repl_state_flat_program_count();
}

void test_ui_panels_render_code_panel(const UiRenderSnapshot *snap,
                                      UiCodePanelOutput *out) {
    if (out) {
        out->cursor_px = 100;
        out->cursor_py = snap && snap->viewport.window_h > 0 ? snap->viewport.window_h - 100 : 400;
        out->cursor_valid = 1;
    }
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
/* -1 = no clock row, so the frame stepper is absent and these helpers see the
 * plain row geometry. Tests that want the stepper build the view themselves. */
#define VP_NO_CLOCK_ROW (-1)

static void vp_rect(int count, int *px, int *py, int *pw, int *ph) {
    UiVariablePanelView v = ui_app_variable_panel_view_live(count, VP_NO_CLOCK_ROW, 0);
    ui_variable_panel_rect(&v, px, py, pw, ph);
}
static int vp_hit_row(int count, int gx, int gy, int *row) {
    UiVariablePanelView v = ui_app_variable_panel_view_live(count, VP_NO_CLOCK_ROW, 0);
    return ui_variable_panel_hit_row(&v, gx, gy, row);
}

/* Pixel step for the two exhaustive code-panel probes below. A code row is
 * LINE_H (18) tall and a character cell FONT_SMALL_W (8) wide, so a step of 4
 * cannot straddle either without landing in it. Probing every pixel instead
 * ran ~100k hit-tests per call - which was most of this binary's runtime,
 * since ui_panels_hit_test re-solves the overlay layout on every call. */
#define HIT_PROBE_STEP 4

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

    for (int gl_y = cp_y + 1; gl_y < cp_y + cp_h - 1; gl_y += HIT_PROBE_STEP) {
        int my = win_h - gl_y;
        for (int mx = cp_x + 1; mx < cp_x + cp_w - 1; mx += HIT_PROBE_STEP) {
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

    for (int gl_y = cp_y + 1; gl_y < cp_y + cp_h - 1; gl_y += HIT_PROBE_STEP) {
        int my = win_h - gl_y;
        for (int mx = cp_x + 1; mx < cp_x + cp_w - 1; mx += HIT_PROBE_STEP) {
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
    g_scene_stub_execute_purpose = -1;
    g_scene_stub_reject = 0;
    g_chrome_clear_calls = 0;
    g_chrome_clear_calls_at_scene = -1;
    g_chrome_clear_calls_at_hud = -1;
    memset(g_chrome_clear_rgba, 0, sizeof(g_chrome_clear_rgba));
    /* The retained presentation background deliberately has no production
     * reset - not even glr_ctrl_reset_all - because snapping to the default is
     * exactly wrong on the frame the value is least trustworthy. Tests need
     * isolation from each other, though, so the fixture reseeds the static
     * directly (this TU includes glr_ctrl.c). */
    g_presentation_rgba[0] = CFG_DEFAULT_CLEAR_R;
    g_presentation_rgba[1] = CFG_DEFAULT_CLEAR_G;
    g_presentation_rgba[2] = CFG_DEFAULT_CLEAR_B;
    g_presentation_rgba[3] = CFG_DEFAULT_CLEAR_A;
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
    repl_state_flat_program_set_user_lighting_enabled(1);

    presentation = glr_state_presentation_mut();
    presentation->show_vertex_points = 1;
    presentation->show_light_indicators = 1;
    presentation->highlight_current_poly = 1;
    presentation->xform_guide_mode = (Render3dXformGuideMode)9; /* out-of-range: exercises clamp path */
    presentation->code_panel_layout = CODE_PANEL_LAYOUT_BOTTOM;

    repl_state_variables_mut()->anim_time = 4.25f;
    editor_insert_mode_set(1);

    doc_cmds = repl_command_store_live().cmds;
    flat_cmds = repl_state_flat_program_writable()->cmds;
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
    replay_state_mut()->expand_args = REPLAY_EXPAND_VERBOSE;
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
    /* viewport_w/h removed from Render3dRenderConfig - scene helpers use
     * render3d_w/render3d_h (the active GL viewport) instead. The HUD asserts
     * below read the window viewport from the snapshot directly. */
    ASSERT_FLOAT("camera distance forwarded", g_last_scene_config.cam_dist, 7.5f);
    ASSERT_FLOAT("camera tx forwarded", g_last_scene_config.cam_tx, 0.5f);
    ASSERT_FLOAT("camera glow forwarded", g_last_scene_config.cam_motion_glow, 0.9f);
    ASSERT_INT("light indicators copied", g_last_scene_config.show_light_indicators, 1);
    /* Replay-fade data is a controller-private static
     * (g_replay_fade_plan), rather than part of Render3dRenderConfig.
     * Inspect it directly since this TU includes imrepl_ctrl.c. */
    ASSERT_INT("replay fade plan inactive without active fades",
               g_replay_fade_plan.active, 0);
    ASSERT_INT("replay fade base limit zero without fades",
               g_replay_fade_plan.base_limit, 0);
    ASSERT_INT("replay fade batches empty",
               g_replay_fade_plan.batch_count, 0);
    ASSERT_FLOAT("replay baseline scratch copied",
                 g_replay_fade_plan.baseline_scratch_arrays[0][0], 4.0f);
    /* The fixture has replay_mode == VERTEX, which gates the tess-preview
     * wireframe overlay - so post_fill_fn is wired even though no fade
     * batches are active. */
    ASSERT_TRUE("post_fill_fn wired when a replay overlay is active",
                g_last_scene_config.post_fill_fn != NULL);
    ASSERT_TRUE("post_overlays_fn wired",
                g_last_scene_config.post_overlays_fn != NULL);
    ASSERT_TRUE("post_resolve_overlays_fn wired (label passes)",
                g_last_scene_config.post_resolve_overlays_fn != NULL);
    ASSERT_INT("tess preview marked active for VERTEX replay mode",
               g_replay_fade_plan.tess_preview_active, 1);
    ASSERT_FLOAT("baseline clear color r",
                 g_last_scene_config.baseline_clear_color[0], CFG_DEFAULT_CLEAR_R);
    ASSERT_FLOAT("baseline clear color g",
                 g_last_scene_config.baseline_clear_color[1], CFG_DEFAULT_CLEAR_G);
    ASSERT_FLOAT("baseline clear color b",
                 g_last_scene_config.baseline_clear_color[2], CFG_DEFAULT_CLEAR_B);
    ASSERT_FLOAT("baseline clear color a",
                 g_last_scene_config.baseline_clear_color[3], CFG_DEFAULT_CLEAR_A);
    ASSERT_FLOAT("presentation background default r",
                 g_last_scene_config.presentation_rgba[0], CFG_DEFAULT_CLEAR_R);
    ASSERT_FLOAT("presentation background default g",
                 g_last_scene_config.presentation_rgba[1], CFG_DEFAULT_CLEAR_G);
    ASSERT_FLOAT("presentation background default b",
                 g_last_scene_config.presentation_rgba[2], CFG_DEFAULT_CLEAR_B);
    ASSERT_FLOAT("presentation background default a",
                 g_last_scene_config.presentation_rgba[3], CFG_DEFAULT_CLEAR_A);
    ASSERT_FLOAT("alpha scale uses default background",
                 g_last_scene_config.alpha_scale, 1.0f);
    ASSERT_INT("focus vertex valid", g_last_scene_config.focus.valid, 1);
    ASSERT_FLOAT("focus vertex x", g_last_scene_config.focus.pos[0], 1.0f);
    ASSERT_FLOAT("focus vertex y", g_last_scene_config.focus.pos[1], 2.0f);
    ASSERT_FLOAT("focus vertex z", g_last_scene_config.focus.pos[2], 3.0f);

    /* Scene/viewport rect: replay_ui_hud_render now derives the scene
     * rect via ui_layout_scene_rect() and reads the viewport from
     * snap->viewport - neither field travels on the snapshot we capture
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
    ASSERT_INT("HUD replay expand args on snap",
               g_last_replay_hud_snap.replay.expand_args,
               REPLAY_EXPAND_VERBOSE);
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

/* Plant a bare flat command at `idx` over the display fixture's program. */
static GLCmd *plant_flat_cmd(int idx, CmdType type, const char *text) {
    GLCmd *cmds = repl_state_flat_program_writable()->cmds;
    memset(&cmds[idx], 0, sizeof(cmds[idx]));
    cmds[idx].type = type;
    cmds[idx].valid = 1;
    cmds[idx].src_cmd_idx = idx;
    cmds[idx].call_src_cmd_idx = -1;
    cmds[idx].root_call_src_cmd_idx = -1;
    editor_buffer_set_line(idx, text);
    return &cmds[idx];
}

static void plant_flat_clear(int idx) {
    GLCmd *c = plant_flat_cmd(idx, CMD_CLEAR, "glClear(GL_COLOR_BUFFER_BIT);");
    c->num_args = 1;
    c->args[0] = GL_COLOR_BUFFER_BIT;
}

static void plant_flat_clear_color(int idx, float r, float g, float b) {
    GLCmd *c = plant_flat_cmd(idx, CMD_CLEAR_COLOR, "glClearColor(0.05, 0.06, 0.08, 1);");
    c->num_args = 4;
    c->args[0] = r; c->args[1] = g; c->args[2] = b; c->args[3] = 1.0f;
}

/* Set up a two-frame background fixture: replay off, the stub driving the
 * controller's real geometry callback so the frame's background is produced
 * by actual execution. */
static void prepare_background_fixture(void) {
    prepare_display_fixture();
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;
    g_scene_stub_execute_purpose = RENDER3D_EXEC_MAIN_FILL;
}

/* A glClearColor that lands *after* the clear reaches no pixel this frame -
 * source order is what executes - so the background stays the baseline the
 * bare clear took. Nothing predicts that: the executor observes it while
 * emitting the two commands. */
static void test_display_frame_clear_color_after_clear_is_ignored(void) {
    printf("--- imrepl_ctrl clear color source order (late) ---\n");
    prepare_background_fixture();

    plant_flat_clear(2);
    plant_flat_clear_color(3, 0.05f, 0.06f, 0.08f);
    repl_state_document_count_set(4);
    repl_state_flat_program_set_count(4);

    glr_ctrl_display_frame();

    ASSERT_FLOAT("late clear color does not reach this frame's chrome r",
                 g_chrome_clear_rgba[0], CFG_DEFAULT_CLEAR_R);
    ASSERT_FLOAT("late clear color does not reach this frame's chrome g",
                 g_chrome_clear_rgba[1], CFG_DEFAULT_CLEAR_G);
    ASSERT_FLOAT("late clear color does not reach this frame's chrome b",
                 g_chrome_clear_rgba[2], CFG_DEFAULT_CLEAR_B);
    ASSERT_FLOAT("late clear color does not reach this frame's chrome a",
                 g_chrome_clear_rgba[3], CFG_DEFAULT_CLEAR_A);

    /* Second frame: the retained value the fog and contrast read is the same
     * baseline, so a late glClearColor cannot leak forward either. */
    glr_ctrl_display_frame();
    ASSERT_FLOAT("late clear color does not leak into the next frame r",
                 g_last_scene_config.presentation_rgba[0], CFG_DEFAULT_CLEAR_R);
    ASSERT_FLOAT("late clear color does not leak into the next frame b",
                 g_last_scene_config.presentation_rgba[2], CFG_DEFAULT_CLEAR_B);
}

/* The other half of the same rule, and the one a user actually authors: a
 * glClearColor *before* the clear is the background the rect took. Chrome
 * follows it on the frame it happens; the fog / overlay-contrast consumers
 * read the retained value, so they follow on the next one. Both vintages are
 * asserted here rather than worked around - the lag is the design. */
static void test_display_frame_clear_color_before_clear_applies(void) {
    printf("--- imrepl_ctrl clear color source order (early) ---\n");
    prepare_background_fixture();

    plant_flat_clear_color(2, 0.05f, 0.06f, 0.08f);
    plant_flat_clear(3);
    repl_state_document_count_set(4);
    repl_state_flat_program_set_count(4);

    glr_ctrl_display_frame();

    ASSERT_FLOAT("chrome takes this frame's observed background r",
                 g_chrome_clear_rgba[0], 0.05f);
    ASSERT_FLOAT("chrome takes this frame's observed background g",
                 g_chrome_clear_rgba[1], 0.06f);
    ASSERT_FLOAT("chrome takes this frame's observed background b",
                 g_chrome_clear_rgba[2], 0.08f);
    ASSERT_FLOAT("chrome takes this frame's observed background a",
                 g_chrome_clear_rgba[3], 1.0f);
    /* Same frame, the other vintage: this config was built before the walk
     * that observed the new background, so it still carries the previous one. */
    ASSERT_FLOAT("fog/contrast lag one frame behind a background change",
                 g_last_scene_config.presentation_rgba[0], CFG_DEFAULT_CLEAR_R);

    glr_ctrl_display_frame();

    ASSERT_FLOAT("the retained background reaches the next frame's fog r",
                 g_last_scene_config.presentation_rgba[0], 0.05f);
    ASSERT_FLOAT("the retained background reaches the next frame's fog g",
                 g_last_scene_config.presentation_rgba[1], 0.06f);
    ASSERT_FLOAT("the retained background reaches the next frame's fog b",
                 g_last_scene_config.presentation_rgba[2], 0.08f);
    /* alpha_scale is derived from that background, not from a constant: a
     * background darker than the design point boosts overlay alpha. */
    ASSERT_TRUE("darker background boosts overlay alpha_scale",
                g_last_scene_config.alpha_scale > 1.0f);
    /* The baseline is configuration, not history: it must not follow the
     * observation, or the program's own clear would start from whatever the
     * last frame happened to show. */
    ASSERT_FLOAT("baseline clear color stays the configured default",
                 g_last_scene_config.baseline_clear_color[0], CFG_DEFAULT_CLEAR_R);
}

/* Chrome is painted after the scene render - so it can consume the frame's own
 * observation - and before the 2D overlays that draw over the strips. */
static void test_display_frame_chrome_clears_after_the_scene(void) {
    printf("--- imrepl_ctrl chrome clear ordering ---\n");
    prepare_display_fixture();
    g_scene_stub_execute_purpose = RENDER3D_EXEC_MAIN_FILL;

    plant_flat_clear_color(2, 0.05f, 0.06f, 0.08f);
    plant_flat_clear(3);
    repl_state_document_count_set(4);
    repl_state_flat_program_set_count(4);

    glr_ctrl_display_frame();

    ASSERT_INT("chrome not yet cleared when the scene renders",
               g_chrome_clear_calls_at_scene, 0);
    ASSERT_INT("chrome cleared exactly once per frame", g_chrome_clear_calls, 1);
    ASSERT_INT("chrome cleared before the replay HUD paints over it",
               g_chrome_clear_calls_at_hud, 1);
}

/* Retention is the whole unknown-background policy: a frame that establishes
 * no background keeps the last honest answer rather than snapping to the
 * default, and that is also what holds replay steady. */
static void test_display_frame_background_retention(void) {
    printf("--- imrepl_ctrl background retention ---\n");
    prepare_background_fixture();

    plant_flat_clear_color(2, 0.05f, 0.06f, 0.08f);
    plant_flat_clear(3);
    repl_state_document_count_set(4);
    repl_state_flat_program_set_count(4);
    glr_ctrl_display_frame();
    ASSERT_FLOAT("frame 1 establishes a background", g_chrome_clear_rgba[0], 0.05f);

    /* Delete the clear: the program now establishes nothing, and the host
     * must NOT clear the rect on its behalf - so the last known background is
     * what chrome and the retained presentation keep showing. */
    plant_flat_cmd(3, CMD_COMMENT, "// no clear");
    glr_ctrl_display_frame();
    ASSERT_FLOAT("an unknown frame keeps the retained background",
                 g_chrome_clear_rgba[0], 0.05f);
    glr_ctrl_display_frame();
    ASSERT_FLOAT("the retained background still feeds fog/contrast",
                 g_last_scene_config.presentation_rgba[0], 0.05f);

    /* A clear under a fully-disabled color mask writes no color pixels. The
     * observation is unknown, so retention holds - the defect a source-only
     * resolver could not see. */
    {
        GLCmd *c = plant_flat_cmd(3, CMD_COLOR_MASK,
                                  "glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);");
        c->num_args = 4;
        c->args[0] = 0.0f; c->args[1] = 0.0f; c->args[2] = 0.0f; c->args[3] = 0.0f;
        plant_flat_clear_color(4, 0.90f, 0.10f, 0.10f);
        plant_flat_clear(5);
        repl_state_document_count_set(6);
        repl_state_flat_program_set_count(6);
    }
    glr_ctrl_display_frame();
    ASSERT_FLOAT("a fully masked clear does not become the background",
                 g_chrome_clear_rgba[0], 0.05f);

    /* A rejected config runs no callback at all: nothing publishes, so the
     * retained value stands untouched. */
    g_scene_stub_reject = 1;
    glr_ctrl_display_frame();
    ASSERT_FLOAT("a rejected render leaves the retained background alone",
                 g_chrome_clear_rgba[0], 0.05f);
    g_scene_stub_reject = 0;
}

/* Retention is written per walk, not folded per frame, so a frame with more
 * than one authoritative pass - accumulation, where every sample runs the fill
 * - resolves by "the last sample that knew a background wins". That is an
 * endpoint approximation, not the resolved background: glAccum presents the
 * weighted average of the samples, so a clear colour that moves within a frame
 * shows as the mean while this retains the final sample (they coincide unless
 * time blur animates glClearColor). The endpoint is taken deliberately - it is
 * the sample baked at the true frame time - and this test pins that rule. A
 * sample that established nothing is dropped rather than stored, so it can
 * neither clobber an earlier known sample nor reset the retained value.
 *
 * Drives scene_execute_adapter directly: the same entry render3d calls once
 * per accumulation sample, without needing an accumulation-capable context. */
static void test_background_retention_across_passes(void) {
    printf("--- imrepl_ctrl background retention across passes ---\n");
    prepare_display_fixture();
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;

    Render3dExecuteContext fill = { .purpose = RENDER3D_EXEC_MAIN_FILL };

    /* Sample 1 establishes a background. */
    plant_flat_clear_color(2, 0.05f, 0.06f, 0.08f);
    plant_flat_clear(3);
    repl_state_document_count_set(4);
    repl_state_flat_program_set_count(4);
    scene_execute_adapter(&fill, NULL);
    ASSERT_FLOAT("first known sample is retained", g_presentation_rgba[0], 0.05f);

    /* Sample 2 establishes none (the clear is gone): dropped, not stored - so
     * an unknown sample cannot pull the frame back to the default. */
    plant_flat_cmd(3, CMD_COMMENT, "// no clear");
    scene_execute_adapter(&fill, NULL);
    ASSERT_FLOAT("an unknown sample does not clobber a known one",
                 g_presentation_rgba[0], 0.05f);

    /* Sample 3 knows again, with a different colour: the last known wins. */
    plant_flat_clear_color(2, 0.11f, 0.12f, 0.13f);
    plant_flat_clear(3);
    scene_execute_adapter(&fill, NULL);
    ASSERT_FLOAT("the last known sample wins r", g_presentation_rgba[0], 0.11f);
    ASSERT_FLOAT("the last known sample wins g", g_presentation_rgba[1], 0.12f);
    ASSERT_FLOAT("the last known sample wins b", g_presentation_rgba[2], 0.13f);

    /* A non-authoritative purpose publishes nothing whatever it executes. */
    Render3dExecuteContext probe = { .purpose = RENDER3D_EXEC_DEPTH_PROBE };
    plant_flat_clear_color(2, 0.02f, 0.02f, 0.02f);
    scene_execute_adapter(&probe, NULL);
    ASSERT_FLOAT("a depth probe cannot speak for the background",
                 g_presentation_rgba[0], 0.11f);
}

/* Shipped scenes whose first flat command before the clear is real work
 * (glEnable(GL_DEPTH_TEST), an assignment, ...) get replay_frame_setup_limit()
 * == 0, so an early replay prefix executes no clear at all. Retention is what
 * keeps those from flickering to the default until the PC crosses the clear. */
static void test_display_frame_replay_prefix_holds_background(void) {
    printf("--- imrepl_ctrl replay prefix background ---\n");
    prepare_background_fixture();

    {
        GLCmd *c = plant_flat_cmd(0, CMD_ENABLE, "glEnable(GL_DEPTH_TEST);");
        c->num_args = 1;
        c->args[0] = (float)GL_DEPTH_TEST;
    }
    plant_flat_clear_color(1, 0.05f, 0.06f, 0.08f);
    plant_flat_clear(2);
    plant_flat_cmd(3, CMD_COMMENT, "// geometry follows");
    repl_state_document_count_set(4);
    repl_state_flat_program_set_count(4);

    /* Establish a known background first, so "holds" is distinguishable from
     * "happens to equal the default". */
    glr_ctrl_display_frame();
    ASSERT_FLOAT("full run establishes the program's background",
                 g_chrome_clear_rgba[0], 0.05f);
    memcpy(g_presentation_rgba,
           (float[4]){0.42f, 0.43f, 0.44f, 1.0f}, sizeof(g_presentation_rgba));

    /* This is the shape the audit found: work before the clear, so the frame
     * setup limit cannot rescue an early prefix. */
    ASSERT_INT("a scene with work before its clear keeps no setup limit",
               replay_frame_setup_limit(repl_state_flat_program_view()), 0);

    replay_state_mut()->active = 1;
    replay_state_mut()->state = REPLAY_PAUSED;
    replay_state_mut()->pc = 1;             /* stops short of the clear */
    glr_ctrl_display_frame();
    ASSERT_FLOAT("a prefix short of the clear holds the retained background",
                 g_chrome_clear_rgba[0], 0.42f);

    replay_state_mut()->pc = 3;             /* the clear has now executed */
    glr_ctrl_display_frame();
    ASSERT_FLOAT("crossing the clear releases the background to it",
                 g_chrome_clear_rgba[0], 0.05f);
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
 * sections should approximate PROF_FRAME_WORK (no section big
 * enough to matter goes unprofiled). The test drives
 * glr_ctrl_display_frame() and inspects the profile state.
 *
 * The "all major sections non-stale" half is deterministic. The
 * "sum approximately equals total" half uses a generous lower bound
 * (50% of PROF_FRAME_WORK) to avoid flake from OS scheduling noise
 * - any future regression that drops a major section entirely will
 * blow the lower bound. A tighter upper bound is not asserted
 * because per-section start/end overhead can stack to a real but
 * harmless gap.
 *
 * Even with the loose bound a single frame can be unlucky (a
 * scheduler preemption inside PROF_FRAME_WORK but outside every
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
     * active so PROF_REPLAY_HUD lands. The glr_frame_* trio is the host's
     * frame bracket - without it neither span ever opens. Coverage is measured
     * against PROF_FRAME_WORK: every section below runs inside it, while
     * PROF_FRAME_TOTAL additionally carries a present this test never
     * performs. */
    repl_mark_source_dirty();
    repl_state_mark_flat_dirty();
    glr_frame_begin();
    glr_ctrl_display_frame();
    glr_frame_work_end();
    glr_frame_ended();

    /* Sections that should have landed a sample this frame. */
    ProfSection major[] = {
        PROF_FRAME_TOTAL,
        PROF_FRAME_WORK,
        PROF_PRESENT,
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
            glr_frame_begin();
            glr_ctrl_display_frame();
            glr_frame_work_end();
            glr_frame_ended();
        }
        attempts_run = attempt;

        /* Sum of disjoint top-level sections should be a substantial
         * fraction of PROF_FRAME_WORK. PROF_SNAPSHOT / PROF_RENDER3D
         * are themselves aggregates, so summing them with the leaves
         * outside (autonormal, flatten, replay_hud, code_panel,
         * ui_panels, profile_panel, memory_panel, compositor,
         * frame_restore) covers the
         * controller's whole frame body. */
        total_us = prof_section_last_us(PROF_FRAME_WORK);
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

        printf("    attempt %d/%d: FRAME_WORK %.1fus, major sum %.1fus (%.1f%%); "
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
    ASSERT_TRUE("frame work positive", best_total_us > 0.0);
    ASSERT_TRUE("major-section sum positive", best_sum_us > 0.0);

    /* Sum should cover at least half the frame; missing a major
     * section drops it well below this threshold on every attempt. */
    if (best_total_us > 0.0) {
        char label[96];
        snprintf(label, sizeof(label),
                 "major sections cover >=50%% of FRAME_WORK (best %.1f%%)",
                 best_major_coverage * 100.0);
        ASSERT_TRUE(label, best_major_coverage >= PROFILE_MAJOR_COVERAGE_MIN);
    }

    if (snapshot_measured) {
        char label[96];
        snprintf(label, sizeof(label),
                 "SNAPSHOT subs cover >=70%% of parent (best %.1f%%)",
                 best_snapshot_coverage * 100.0);
        ASSERT_TRUE(label, best_snapshot_coverage >= PROFILE_SNAPSHOT_COVERAGE_MIN);
    }
}

/* The frame boundary is the application's, and it produces the callback's four
 * numbers from three spans: Frame Time (the whole display callback), Frame Work
 * (up to the present), Depth Snapshot (the post-work occlusion capture, when it
 * ran), and Present as what is left of the total. The next begin also derives
 * Frame Wait from the start-to-start interval minus that completed callback.
 *
 * Each piece is a regression that happened. The stages gl_repl.c runs on
 * either side of glr_ctrl_display_frame() - scripted input, the post-composite
 * splash / tour overlays - are real per-frame cost: a guided tour's caption
 * overlay alone measured ~10 ms/frame while the frame reported ~1.5 ms,
 * because the bracket lived inside the controller and the overlay draws after
 * it returns. The present is the vsync wait: counted as work it pinned the
 * number at the refresh interval, so a 5 ms frame and a 15 ms one both read
 * ~16 ms in permanent red. And deriving Present rather than bracketing the
 * swap is what keeps the two spans exhaustive - anything the callback does
 * after glr_frame_work_end() lands in Present instead of nowhere.
 *
 * Driven on the profiler's test clock so the arithmetic is exact and no GL is
 * needed: each host stage is stood in for by its own prof bracket, opened in
 * the order display_func() reaches it. The tour overlay self-times in
 * production but is stood in the same way, since it needs a live GL context
 * and a window to do anything. */
static void test_frame_spans_host_stages(void) {
    double frame_us, work_us, overlay_us, tour_us, present_us, wait_us;

    printf("--- imrepl_ctrl frame spans host stages ---\n");

    prof_test_set_now_us(0.0);
    glr_frame_begin();          /* host: first statement of the callback */

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

    glr_frame_work_end();       /* host: before the present */

    /* ... glFinish + swap (2 ms), inside the frame but outside its work ... */
    prof_test_set_now_us(17000.0);

    glr_frame_ended();          /* host: last statement of the callback */

    frame_us   = prof_section_last_us(PROF_FRAME_TOTAL);
    work_us    = prof_section_last_us(PROF_FRAME_WORK);
    overlay_us = prof_section_last_us(PROF_HOST_OVERLAYS);
    tour_us    = prof_section_last_us(PROF_TOUR_OVERLAY);
    present_us = prof_section_last_us(PROF_PRESENT);

    ASSERT_TRUE("host overlays not stale", !prof_section_is_stale(PROF_HOST_OVERLAYS));
    ASSERT_TRUE("tour overlay not stale", !prof_section_is_stale(PROF_TOUR_OVERLAY));
    ASSERT_TRUE("present not stale", !prof_section_is_stale(PROF_PRESENT));
    ASSERT_TRUE("scripted input not stale", !prof_section_is_stale(PROF_SCRIPTED_INPUT));

    ASSERT_TRUE("tour overlay measured 10ms", tour_us == 10000.0);
    /* The whole callback, end to end. */
    ASSERT_TRUE("frame time is the whole callback (17ms)", frame_us == 17000.0);
    /* Every host stage that does frame work, and not the present. */
    ASSERT_TRUE("frame work covers the host work stages (15ms)",
                work_us == 15000.0);
    ASSERT_TRUE("frame work contains the host overlays", work_us >= overlay_us);
    /* Derived, and exhaustive: the two parts account for the whole frame. */
    ASSERT_TRUE("present is the difference (2ms)", present_us == 2000.0);
    ASSERT_TRUE("work + present is the frame", work_us + present_us == frame_us);

    /* If the host omits work_end, do not subtract the preceding frame's stale
     * Work sample. None of the new frame was accounted as work, so all of it
     * belongs to Present, and ending it must clear both open flags. */
    prof_test_set_now_us(20000.0);
    glr_frame_begin();
    prof_test_set_now_us(25000.0);
    glr_frame_ended();
    wait_us = prof_section_last_us(PROF_FRAME_WAIT);
    ASSERT_TRUE("time between callbacks is attributed (3ms)", wait_us == 3000.0);
    ASSERT_TRUE("frame without work_end still records its total",
                prof_section_last_us(PROF_FRAME_TOTAL) == 5000.0);
    ASSERT_TRUE("frame without work_end attributes the full frame to present",
                prof_section_last_us(PROF_PRESENT) == 5000.0);

    /* An unpaired end must not close a span that was never opened -
     * glr_ctrl_display_frame() is called bare by tests and tools. */
    prof_test_set_now_us(99000.0);
    glr_frame_work_end();
    glr_frame_ended();
    ASSERT_TRUE("unpaired ends leave the frame alone",
                prof_section_last_us(PROF_FRAME_TOTAL) == 5000.0);
    ASSERT_TRUE("unpaired ends leave the work alone",
                prof_section_last_us(PROF_FRAME_WORK) == 15000.0);
    ASSERT_TRUE("unpaired ends leave the present alone",
                prof_section_last_us(PROF_PRESENT) == 5000.0);

    /* A frame that took the depth snapshot: the capture runs after work_end
     * (after the glFinish, before the swap), so it is inside Frame Time but
     * outside Frame Work. Present must not keep it - that row is drawn as
     * headroom, and a 3 ms pixel transfer is not headroom. */
    prof_test_set_now_us(100000.0);
    glr_frame_begin();
    prof_test_set_now_us(104000.0);   /* ... 4 ms of frame work ... */
    glr_frame_work_end();
    prof_test_set_now_us(106000.0);   /* ... 2 ms glFinish drain ... */
    prof_begin(PROF_DEPTH_SNAPSHOT);  /* ... 3 ms depth readback ... */
    prof_test_set_now_us(109000.0);
    prof_end(PROF_DEPTH_SNAPSHOT);
    prof_test_set_now_us(110000.0);   /* ... 1 ms swap ... */
    glr_frame_ended();

    ASSERT_TRUE("frame with a capture records the whole callback (10ms)",
                prof_section_last_us(PROF_FRAME_TOTAL) == 10000.0);
    ASSERT_TRUE("the capture is outside frame work (4ms)",
                prof_section_last_us(PROF_FRAME_WORK) == 4000.0);
    ASSERT_TRUE("the capture measured 3ms",
                prof_section_last_us(PROF_DEPTH_SNAPSHOT) == 3000.0);
    ASSERT_TRUE("present excludes the capture (3ms)",
                prof_section_last_us(PROF_PRESENT) == 3000.0);
    ASSERT_TRUE("work + capture + present is the frame",
                prof_section_last_us(PROF_FRAME_WORK)
                    + prof_section_last_us(PROF_DEPTH_SNAPSHOT)
                    + prof_section_last_us(PROF_PRESENT)
                        == prof_section_last_us(PROF_FRAME_TOTAL));

    /* The next frame skips the capture (labels out of scope). Its last_us still
     * holds the 3 ms above, and subtracting that would hand Present a shortfall
     * no part of this frame paid for. Freshness, not staleness, is the gate. */
    prof_test_set_now_us(120000.0);
    glr_frame_begin();
    prof_test_set_now_us(124000.0);
    glr_frame_work_end();
    prof_test_set_now_us(130000.0);
    glr_frame_ended();

    ASSERT_TRUE("a skipped capture keeps its previous sample",
                prof_section_last_us(PROF_DEPTH_SNAPSHOT) == 3000.0);
    ASSERT_TRUE("a frame without a capture subtracts nothing (6ms present)",
                prof_section_last_us(PROF_PRESENT) == 6000.0);
    ASSERT_TRUE("work + present is the frame again",
                prof_section_last_us(PROF_FRAME_WORK)
                    + prof_section_last_us(PROF_PRESENT)
                        == prof_section_last_us(PROF_FRAME_TOTAL));

    prof_test_clear_now_us();
}

/* The nesting guard: a child section bracketed outside its catalog parent's
 * span. This is the failure the Depth Snapshot row shipped with - a 2.17 ms row
 * indented under a 9 us "Host Overlays", because the capture moved after that
 * aggregate closed while the row stayed a child of it. Nothing static can catch
 * it (parent and child are bracketed in different TUs), and no timing assertion
 * can either under the stubs, where every span is ~0 us. What is checkable is
 * the structural claim: at the moment a nested section begins, its parent must
 * be open.
 *
 * Driven on the real catalog with a real parent/child pair, so the test also
 * pins that the parent lookup resolves the depth column the way the panel's
 * branch walk does. */
static void test_prof_nesting_guard(void) {
    printf("--- imrepl_ctrl profile nesting guard ---\n");

    /* The app installs this from init_gl; do it here too rather than inheriting
     * whatever earlier tests left, since an uninstalled guard reports zero
     * violations and would pass every case below. prof_test_reset() uninstalls
     * it again along with the hooks, so the order matters. */
    prof_test_reset();
    glr_prof_install_nesting_guard();
    ASSERT_INT("a fresh profiler has no violations",
               prof_nesting_violations(), 0);

    /* Properly nested: parent open across the child. */
    prof_begin(PROF_HOST_OVERLAYS);
    prof_begin(PROF_TOUR_OVERLAY);
    prof_end(PROF_TOUR_OVERLAY);
    prof_end(PROF_HOST_OVERLAYS);
    ASSERT_INT("a child inside its parent is not a violation",
               prof_nesting_violations(), 0);

    /* The regression: same child, begun after the parent closed. */
    prof_begin(PROF_HOST_OVERLAYS);
    prof_end(PROF_HOST_OVERLAYS);
    prof_begin(PROF_TOUR_OVERLAY);
    prof_end(PROF_TOUR_OVERLAY);
    ASSERT_INT("a child begun after its parent closed is a violation",
               prof_nesting_violations(), 1);
    ASSERT_INT("the guard names the offending child",
               (int)prof_first_nesting_violation(), (int)PROF_TOUR_OVERLAY);

    /* The other half of containment: the child starts inside its parent but
     * outlives it. Its time is just as misattributed - part of the row lies
     * outside the total it is drawn under - so checking only the start would
     * pass this. */
    prof_test_reset();
    glr_prof_install_nesting_guard();
    prof_begin(PROF_HOST_OVERLAYS);
    prof_begin(PROF_TOUR_OVERLAY);
    prof_end(PROF_HOST_OVERLAYS);
    prof_end(PROF_TOUR_OVERLAY);
    ASSERT_INT("a child that outlives its parent is a violation",
               prof_nesting_violations(), 1);
    ASSERT_INT("the overlong child is the one named",
               (int)prof_first_nesting_violation(), (int)PROF_TOUR_OVERLAY);

    /* Wrong at both ends is still one mis-nested span, not two events. */
    prof_test_reset();
    glr_prof_install_nesting_guard();
    prof_begin(PROF_TOUR_OVERLAY);
    prof_end(PROF_TOUR_OVERLAY);
    ASSERT_INT("a span outside its parent at both ends counts once",
               prof_nesting_violations(), 1);

    /* Top-level rows have no parent to be inside of - including the capture,
     * which is exactly why it had to become one. */
    prof_test_reset();
    glr_prof_install_nesting_guard();
    prof_begin(PROF_DEPTH_SNAPSHOT);
    prof_end(PROF_DEPTH_SNAPSHOT);
    prof_begin(PROF_RENDER3D);
    prof_end(PROF_RENDER3D);
    ASSERT_INT("a top-level section is never an orphan",
               prof_nesting_violations(), 0);

    /* An accumulated parent needs no exemption: it brackets each of its legs,
     * so a child inside one sees it open. The fade pass is the live example -
     * one leg per accumulation sample, each with its own per-batch children. */
    prof_begin(PROF_RENDER3D);          /* the fade pass's own parent */
    prof_accum_reset(PROF_RENDER3D_FADE);
    prof_begin(PROF_RENDER3D_FADE);
    prof_begin(PROF_RENDER3D_FADE_BATCH_PREP);
    prof_end(PROF_RENDER3D_FADE_BATCH_PREP);
    prof_accum_end(PROF_RENDER3D_FADE);
    prof_begin(PROF_RENDER3D_FADE);
    prof_begin(PROF_RENDER3D_FADE_BATCH_EXEC);
    prof_end(PROF_RENDER3D_FADE_BATCH_EXEC);
    prof_accum_end(PROF_RENDER3D_FADE);
    prof_accum_commit(PROF_RENDER3D_FADE);
    ASSERT_INT("an accumulated parent's legs nest cleanly",
               prof_nesting_violations(), 0);

    /* ... and the gap between its legs is still guarded. */
    prof_begin(PROF_RENDER3D_FADE_BATCH_EXEC);
    prof_end(PROF_RENDER3D_FADE_BATCH_EXEC);
    ASSERT_INT("a child in the gap between accum legs is a violation",
               prof_nesting_violations(), 1);
    prof_end(PROF_RENDER3D);

    prof_test_reset();
}

/* The guard over a real frame, which is what the synthetic cases above cannot
 * check: every catalog parent claimed by the depth column has to be open across
 * its children in the frame the controller actually runs. The 2D UI band is the
 * reason this exists - it is a direct parent whose bracket stays open around
 * the assignment-plot and console panel draws, so both of those and the whole
 * run of panels between them have to land on the right side of it. Opened plot
 * and console, because those children only run when they draw. */
static void test_prof_nesting_guard_over_a_display_frame(void) {
    printf("--- imrepl_ctrl profile nesting over a frame ---\n");

    prepare_display_fixture();
    /* One frame first: glr_assign_plot_sync_tags() closes a plot whose
     * document was replaced wholesale without a `// @plot` row, and building
     * the fixture is such a replacement. Opening after it has run is what
     * makes the plot's scan and panel actually execute in the frame below. */
    glr_ctrl_display_frame();
    assign_plot_open(1);
    console_set_open(1);

    prof_test_reset();
    glr_prof_install_nesting_guard();

    glr_ctrl_display_frame();
    ASSERT_INT("a real display frame nests cleanly",
               prof_nesting_violations(), 0);
    ASSERT_INT("the 2D UI band sampled this frame",
               prof_section_sampled_this_frame(PROF_UI_2D), 1);

    /* Every row names a place, so the plot's and the console's phases are
     * scattered across three parents rather than gathered under a row named
     * for the feature: the scans sit where they run, the panels in the band. */
    ASSERT_INT("the UI band is a root row",
               prof_section_info(PROF_UI_2D).depth, 0);
    ASSERT_INT("the code panel hangs off it",
               prof_section_info(PROF_CODE_PANEL).depth, 1);
    ASSERT_INT("so does the profile panel",
               prof_section_info(PROF_PROFILE_PANEL).depth, 1);
    ASSERT_INT("the plot panel joined the band",
               prof_section_info(PROF_ASSIGN_PLOT_PANEL).depth, 1);
    ASSERT_INT("and so did the console panel",
               prof_section_info(PROF_CONSOLE_PANEL).depth, 1);
    /* Depth 2: both scans run inside the snapshot's prep bracket, which is
     * where they used to be double-counted from. */
    ASSERT_INT("the replay-marker scan hangs off snapshot prep",
               prof_section_info(PROF_ASSIGN_PLOT_MARKERS).depth, 2);
    ASSERT_INT("so does the console scan",
               prof_section_info(PROF_CONSOLE_CAPTURE).depth, 2);
    ASSERT_INT("the plot's own scan predates every bracket, so it is a root",
               prof_section_info(PROF_ASSIGN_PLOT_CAPTURE).depth, 0);
    ASSERT_INT("the plot scan sampled this frame",
               prof_section_sampled_this_frame(PROF_ASSIGN_PLOT_CAPTURE), 1);
    ASSERT_INT("and so did the console scan",
               prof_section_sampled_this_frame(PROF_CONSOLE_CAPTURE), 1);
    /* A panel that draws inside the band, so the band is checked with a child
     * in it and not just around empty space. (The plot's panel needs a real
     * assignment row to stay open through a capture, which this fixture's
     * two-command document has not got; its scan row above is what pins the
     * plot's side of the split.) */
    ASSERT_INT("the console panel drew inside the band",
               prof_section_sampled_this_frame(PROF_CONSOLE_PANEL), 1);
    ASSERT_INT("as did the code panel",
               prof_section_sampled_this_frame(PROF_CODE_PANEL), 1);
    /* The popup leaves: seven brackets inside PROF_UI_PANELS, so the guard has
     * a depth-2 run to check under the band as well as a depth-1 one. Two of
     * them are unconditional draws that no-op when their surface is hidden. */
    ASSERT_INT("the popup leaves are nested under popups",
               prof_section_info(PROF_UI_PANELS_HELP).depth, 2);
    ASSERT_INT("the variable panel leaf ran",
               prof_section_sampled_this_frame(PROF_UI_PANELS_VARIABLES), 1);
    ASSERT_INT("and the help overlay leaf ran",
               prof_section_sampled_this_frame(PROF_UI_PANELS_HELP), 1);
    /* The status-history popup is bracketed outside PROF_UI_PANELS, so it is a
     * sibling of it rather than an eighth leaf. */
    ASSERT_INT("status history is a band child, not a popup leaf",
               prof_section_info(PROF_UI_STATUS_HISTORY).depth, 1);
    ASSERT_INT("and it drew this frame",
               prof_section_sampled_this_frame(PROF_UI_STATUS_HISTORY), 1);

    prof_test_reset();
    assign_plot_reset_all();
    console_set_open(0);
}

/* The summary rows the panel draws under its divider: the callback total first,
 * then the parts it decomposes into, then the wait outside it. Row order is
 * catalog order, so the run PROF_FRAME_TOTAL through PROF_FRAME_WAIT is what
 * puts them under the rule and in that order. */
static void test_summary_row_metadata(void) {
    int section_idx;
    int total_rows = 0, slack_rows = 0;

    printf("--- imrepl_ctrl profile summary rows ---\n");

    ASSERT_TRUE("frame work is the row after the frame total",
                (int)PROF_FRAME_WORK == (int)PROF_FRAME_TOTAL + 1);
    /* The capture is a summary row, not a host-overlay one: it is a third part
     * of the frame total (Work + Depth Snapshot + Present), so it sits inside
     * the run rather than above the divider. */
    ASSERT_TRUE("depth snapshot is the row after the frame work",
                (int)PROF_DEPTH_SNAPSHOT == (int)PROF_FRAME_WORK + 1);
    ASSERT_TRUE("present is the row after the depth snapshot",
                (int)PROF_PRESENT == (int)PROF_DEPTH_SNAPSHOT + 1);
    ASSERT_TRUE("frame wait is the row after present",
                (int)PROF_FRAME_WAIT == (int)PROF_PRESENT + 1);
    ASSERT_INT("depth snapshot is not a total row",
               prof_section_info(PROF_DEPTH_SNAPSHOT).is_total, 0);
    ASSERT_INT("depth snapshot is not slack",
               prof_section_info(PROF_DEPTH_SNAPSHOT).is_slack, 0);
    ASSERT_INT("frame total row is the total",
               prof_section_info(PROF_FRAME_TOTAL).is_total, 1);
    ASSERT_INT("frame total row is not slack",
               prof_section_info(PROF_FRAME_TOTAL).is_slack, 0);
    ASSERT_INT("frame total row has refresh tolerance",
               prof_section_info(PROF_FRAME_TOTAL).is_frame_total, 1);
    ASSERT_INT("frame work row is not a second total",
               prof_section_info(PROF_FRAME_WORK).is_total, 0);
    ASSERT_INT("frame work row has no refresh tolerance",
               prof_section_info(PROF_FRAME_WORK).is_frame_total, 0);
    ASSERT_INT("present row is slack",
               prof_section_info(PROF_PRESENT).is_slack, 1);
    ASSERT_INT("present row is not a second total",
               prof_section_info(PROF_PRESENT).is_total, 0);
    ASSERT_INT("present row has no refresh tolerance",
               prof_section_info(PROF_PRESENT).is_frame_total, 0);
    ASSERT_INT("frame wait is informational",
               prof_section_info(PROF_FRAME_WAIT).is_slack, 1);
    ASSERT_INT("frame wait is not a second total",
               prof_section_info(PROF_FRAME_WAIT).is_total, 0);
    ASSERT_STR("frame total row is labeled Frame Time",
               prof_section_info(PROF_FRAME_TOTAL).label, "Frame Time");
    ASSERT_STR("frame work row is labeled Frame Work",
               prof_section_info(PROF_FRAME_WORK).label, "Frame Work");
    ASSERT_STR("present row is labeled Present",
               prof_section_info(PROF_PRESENT).label, "Present");
#if defined(__EMSCRIPTEN__)
    ASSERT_STR("web wait row names the browser",
               prof_section_info(PROF_FRAME_WAIT).label, "Browser Wait");
#else
    ASSERT_STR("native wait row names the frame gap",
               prof_section_info(PROF_FRAME_WAIT).label, "Frame Wait");
#endif

    /* Frame Work carries the GPU query, not the two rows either side of it:
     * Frame Time's span runs past a glFinish and Present has no span at all. */
    ASSERT_INT("frame work is gpu-bracketed",
               glr_prof_section_is_gpu(PROF_FRAME_WORK), 1);
    ASSERT_INT("frame time is not gpu-bracketed",
               glr_prof_section_is_gpu(PROF_FRAME_TOTAL), 0);
    ASSERT_INT("present is not gpu-bracketed",
               glr_prof_section_is_gpu(PROF_PRESENT), 0);
    ASSERT_INT("frame wait is not gpu-bracketed",
               glr_prof_section_is_gpu(PROF_FRAME_WAIT), 0);

    /* Exactly one of each across the catalog: the divider and the inverted
     * scale are both single-row affordances. */
    for (section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        ProfSectionInfo info = prof_section_info((ProfSection)section_idx);
        if (info.label == NULL) continue;
        if (info.is_total) total_rows++;
        if (info.is_slack) slack_rows++;
    }
    ASSERT_INT("exactly one total row", total_rows, 1);
    ASSERT_INT("present and frame wait are the two informational rows",
               slack_rows, 2);
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
               editor_buffer_line(0), "static float testvar = 1;");
    ASSERT_FLOAT("drag updates live predef value", g_predef_vars[var_idx].value, 11.0f);

    ASSERT_INT("drag release handled",
               glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP),
               1);
    ASSERT_STR("release rewrites declaration source through compiler",
               editor_buffer_line(0), "static float testvar = 11;");
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
               editor_buffer_line(0), "static float testvar = 1;");
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
               editor_buffer_line(0), "static float testvar = 1;");

    ASSERT_INT("fine drag release handled",
               glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP),
               1);
    ASSERT_STR("fine drag release rewrites declaration source",
               editor_buffer_line(0), "static float testvar = 3;");
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
               editor_buffer_line(0), "static float testvar;");
    ASSERT_FLOAT("uninitialized drag updates live predef value",
                 g_predef_vars[var_idx].value, 10.0f);

    ASSERT_INT("uninitialized drag release handled",
               glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP),
               1);
    ASSERT_STR("uninitialized drag release adds explicit initializer",
               editor_buffer_line(0), "static float testvar = 10;");
    ASSERT_TRUE("uninitialized drag inactive after release",
                !variable_panel_drag_active());
    ASSERT_INT("uninitialized undo flag cleared after release",
               variable_panel_drag_undo_snapshot_pushed(), 0);

    editor_undo_pop_snapshot();
    ASSERT_STR("undo restores bare declaration",
               editor_buffer_line(0), "static float testvar;");
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

/* During a variable-panel drag, motion applies the live value only.
 * repl_state_mark_source_dirty() is the single seam every source-derived
 * cache invalidates from - autonormals, the flat program, and the
 * source-scope depth cache - so motion cannot rebuild those caches per
 * pointer event. Mouse-up trips it exactly once. */
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

    /* Use 100 motion events to cover a representative drag workload. */
    repl_state_normals_dirty_clear();
    for (int step = 1; step <= 100; step++)
        ASSERT_INT("deferred-write motion handled",
                   glr_ctrl_router_handle_variable_panel_motion(
                       click_x + step, click_y),
                   1);

    ASSERT_INT("100 motions mark the source dirty zero times",
               repl_state_normals_dirty(), 0);
    ASSERT_STR("100 motions leave the declaration text alone",
               editor_buffer_line(0), "static float testvar = 1;");
    ASSERT_FLOAT("100 motions update the live value",
                 g_predef_vars[var_idx].value, 1.0f + 100.0f * 0.1f);

    ASSERT_INT("deferred-write release handled",
               glr_ctrl_router_handle_variable_panel_drag_release(GLUT_UP),
               1);
    ASSERT_INT("release marks the source dirty once",
               repl_state_normals_dirty(), 1);
    ASSERT_STR("release persists the settled value",
               editor_buffer_line(0), "static float testvar = 11;");
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
               editor_buffer_line(0), "static float testvar = 1;");
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
 * - a revert to live peer reads would silently still pass every existing
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

/* Press-drag-release on the code panel's scrollbar scrolls the document.
 * The press must not be read as a cursor move or a selection drag, and the
 * release must disarm the drag so later motion goes back to the camera. */
static void test_scrollbar_drag_scrolls_code_panel(void) {
    UiHit hit;
    int x = -1;
    int y = -1;
    int win_h;
    int cp_x, cp_y, cp_w, cp_h;
    int press_line;
    int scroll_bottom;

    printf("--- imrepl_ctrl scrollbar drag ---\n");

    prepare_code_panel_mouse_fixture();
    /* Enough rows that the panel overflows and grows a scrollbar. */
    for (int i = 0; i < 80; i++)
        editor_feed_line("glVertex3f(1, 2, 3);");
    editor_navigate_to_line(0);
    editor_scroll_set(0);

    ASSERT_TRUE("code panel grew a scrollbar",
                find_hit_point_in_code_panel(UI_HIT_CODE_SCROLLBAR,
                                             &hit, &x, &y));
    ASSERT_TRUE("scrollbar press reports a grab offset", hit.item_idx >= 0);

    win_h = ui_state_viewport().window_h;
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    press_line = editor_state_edit_line();

    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_DOWN, x, y);
    ASSERT_INT("scrollbar press marks the drag active",
               ui_state_code_panel().scrollbar_drag, 1);
    ASSERT_INT("scrollbar press does not move the edit cursor",
               editor_state_edit_line(), press_line);

    /* Drag to the panel's bottom edge: the document scrolls to its end. */
    glr_ctrl_motion(x, win_h - (cp_y + 1));
    scroll_bottom = editor_scroll();
    ASSERT_TRUE("dragging to the bottom scrolls forward", scroll_bottom > 0);
    ASSERT_INT("dragging does not move the edit cursor",
               editor_state_edit_line(), press_line);

    /* And back to the top. */
    glr_ctrl_motion(x, win_h - (cp_y + cp_h - 1));
    ASSERT_INT("dragging back to the top scrolls home", editor_scroll(), 0);

    glr_ctrl_mouse(GLUT_LEFT_BUTTON, GLUT_UP, x, win_h - (cp_y + cp_h - 1));
    ASSERT_INT("release disarms the scrollbar drag",
               ui_state_code_panel().scrollbar_drag, 0);

    /* Motion after the release is no longer a scroll gesture. */
    glr_ctrl_motion(x, win_h - (cp_y + 1));
    ASSERT_INT("post-release motion leaves scroll alone", editor_scroll(), 0);
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
                   snap.assign_plot_titles[0], "angle");
        ASSERT_INT("snapshot carries the open flag", snap.assign_plot.open, 1);
        ASSERT_INT("one series to start", snap.assign_plot.series_count, 1);
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

    /* The capture entry point (GLR_OPEN_ASSIGN_PLOT) drives the same
     * synthetic right-click, so the "row is not an assignment" rejection is
     * the routing's, not a second copy of it. */
    ASSERT_INT("capture hook opens the plot on the assignment row",
               glr_ctrl_open_assign_plot(1), 1);
    ASSERT_INT("capture hook targets that row", assign_plot_source_line(), 1);
    ASSERT_INT("capture hook refuses a GL command row",
               glr_ctrl_open_assign_plot(2), 0);
    ui_state_command_description_close();
    assign_plot_reset_all();
}

/* Shift held: the modifier provider is the test seam the router reads through
 * (glutGetModifiers is not callable here). */
static int shift_mods_provider(void) {
    return GLUT_ACTIVE_SHIFT;
}

/* Shift+right-click adds a row to the open plot instead of retargeting, and
 * the status line reports the count - plus, for a frozen one-shot, the fact
 * that the snapshot the user deliberately froze has been re-armed. */
static void test_shift_right_click_adds_plot_series(void) {
    UiHit hit;
    int x = -1, y = -1;

    printf("--- imrepl_ctrl shift+right-click adds a plot series ---\n");

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
    editor_feed_line("float speed;");
    editor_feed_line("angle = t * 30;");
    editor_feed_line("speed = t * 2;");
    editor_navigate_to_line(0);

    /* Plain right-click opens on the first assignment. */
    ASSERT_TRUE("found the first assignment row",
                find_code_text_hit_for_line(2, &hit, &x, &y));
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
    ASSERT_INT("plot opened on one series", assign_plot_series_count(), 1);

    /* Shift+right-click on the second adds rather than retargets. */
    editor_input_set_modifier_provider_for_test(shift_mods_provider);
    ASSERT_TRUE("found the second assignment row",
                find_code_text_hit_for_line(3, &hit, &x, &y));
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
    ASSERT_INT("shift+right-click added a series",
               assign_plot_series_count(), 2);
    ASSERT_INT("and kept the original primary", assign_plot_source_line(), 2);
    ASSERT_TRUE("the status line reports the count",
                strstr(ui_state_status().text, "2 series") != NULL);
    ASSERT_TRUE("and says nothing about recapturing on a live plot",
                strstr(ui_state_status().text, "recapturing") == NULL);

    /* Shift+right-click again removes it. */
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
    ASSERT_INT("shift+right-click again removed it",
               assign_plot_series_count(), 1);

    /* A frozen one-shot: adding discards that snapshot, and says so. */
    assign_plot_set_rate(ASSIGN_PLOT_RATE_ONCE);
    repl_flatten_commands(0);
    repl_state_flat_program_clear_dirty();
    assign_plot_capture(0.0);
    ASSERT_INT("the one-shot froze", assign_plot_view().captured, 1);

    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
    ASSERT_INT("the series joined", assign_plot_series_count(), 2);
    ASSERT_INT("and the one-shot re-armed", assign_plot_view().captured, 0);
    ASSERT_TRUE("which the status line says",
                strstr(ui_state_status().text, "recapturing") != NULL);

    editor_input_set_modifier_provider_for_test(NULL);
    assign_plot_reset_all();
}

/* A document can open the plot on itself: `// @plot` on an assignment row.
 * Resolved in the frame path (after the flat program is current), keyed on
 * the undo generation, so a load applies the tags and an ordinary edit does
 * not re-apply them. */
static void test_plot_tag_opens_the_plot_on_load(void) {
    printf("--- imrepl_ctrl @plot tag sync ---\n");

    glr_ctrl_reset_all();
    assign_plot_reset_all();
    editor_feed_line("static float w;   // @plot");
    editor_feed_line("static float h;");
    editor_feed_line("w = sin(t);       // @plot");
    editor_feed_line("h = cos(t);       // @plot");
    editor_feed_line("glVertex3f(w, h, 0);");

    ASSERT_INT("no tag is honored before a frame runs", assign_plot_is_open(), 0);
    glr_ctrl_display_frame();
    ASSERT_INT("the tagged rows opened the plot", assign_plot_is_open(), 1);
    ASSERT_INT("one series per tagged assignment", assign_plot_series_count(), 2);
    /* Row 0's decl carries a tag too and is deliberately not a series: only an
     * assignment has values to plot. So the primary is the first tagged
     * assignment, row 2. */
    ASSERT_INT("first tagged assignment is the primary",
               assign_plot_source_line(), 2);
    ASSERT_INT("the second tagged row joined", assign_plot_has_series(3), 1);
    ASSERT_INT("an untagged row stays out", assign_plot_has_series(0), 0);

    /* An ordinary edit does not bump the undo generation, so the sync stays
     * out of the way of a manual retarget. */
    assign_plot_open(3);
    glr_ctrl_display_frame();
    ASSERT_INT("a later frame does not re-apply the tags",
               assign_plot_series_count(), 1);
    ASSERT_INT("leaving the manual target alone", assign_plot_source_line(), 3);

    /* A wholesale replacement re-reads the tags. Row identity does not survive
     * one, so an untagged document closes the plot rather than keeping indices
     * that now address someone else's rows. */
    glr_ctrl_reset_all();
    editor_feed_line("static float w;");
    editor_feed_line("w = sin(t);");
    glr_ctrl_display_frame();
    ASSERT_INT("replacing with an untagged document closes the plot",
               assign_plot_is_open(), 0);

    glr_ctrl_reset_all();
    assign_plot_reset_all();
}

static void test_right_click_gl_command_description_popup(void) {
    UiCommandDescriptionPanelView view;
    UiHit hit;
    GLCmd probe;
    ReplCommandDescription description;
    int enable_line;
    int anchor_px;
    int anchor_py;
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

    /* The capture entry point (GLR_OPEN_COMMAND_HELP) takes the same routing,
     * then slides the opened card along x. The click still has to land on the
     * row being explained, so the offset is the only way a scripted capture
     * can move the card off whatever it would cover. */
    ASSERT_INT("capture hook opens the card on a GL row",
               glr_ctrl_open_command_description(0, 0), 1);
    anchor_px = ui_state_command_description().anchor_px;
    anchor_py = ui_state_command_description().anchor_py;
    ASSERT_INT("capture hook re-opens with a right offset",
               glr_ctrl_open_command_description(0, 90), 1);
    ASSERT_INT("offset slides the anchor right",
               ui_state_command_description().anchor_px, anchor_px + 90);
    ASSERT_INT("offset leaves the row's y alone",
               ui_state_command_description().anchor_py, anchor_py);
    ASSERT_INT("offset keeps the card on its source row",
               ui_state_command_description().source_line_idx, 0);
    ASSERT_INT("a negative offset slides it back left",
               glr_ctrl_open_command_description(0, -40), 1);
    ASSERT_INT("left offset applies to the click anchor, not the last card",
               ui_state_command_description().anchor_px, anchor_px - 40);
    ui_state_command_description_close();
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
    ui_state_command_description_close();

    /* The capture entry point (GLR_OPEN_GL_STATE) poses the popup through the
     * same synthetic right-click a user makes, so it inherits the row policy
     * rather than restating it: a blank row opens the report, a code row does
     * not. */
    ASSERT_INT("capture hook opens the report on the blank row",
               glr_ctrl_open_gl_state_popup(blank_line), 1);
    ASSERT_INT("capture hook anchors the report to that row",
               ui_state_gl_state_inspector().source_line_idx, blank_line);
    ASSERT_INT("capture hook reports failure on a code row",
               glr_ctrl_open_gl_state_popup(0), 0);
    ui_state_command_description_close();

    /* The retry contract the shared helper owns: a row that exists but is
     * scrolled out of the panel must fail *without clicking*, because
     * GLR_EDIT_LINE's follow-scroll only lands during a display pass - the
     * capture hook is expected to try the same row again next frame, not to
     * click whatever is on screen instead. Grow the document past a
     * panel-full (same shape as the scroll-clamp fixture below) and park the
     * view away from the blank row. */
    {
        int doc_count = repl_state_document_count();
        int i;

        for (i = doc_count; i < 120; i++)
            editor_buffer_set_line(i, "glVertex3f(1, 2, 3);");
        repl_state_document_count_set(120);
        editor_scroll_follow_cursor_set(0);
        editor_scroll_set(110);

        ASSERT_INT("capture hook reports failure for a scrolled-out row",
                   glr_ctrl_open_gl_state_popup(blank_line), 0);
        ASSERT_INT("and opens nothing while it waits for the row",
                   ui_state_gl_state_inspector().visible, 0);

        /* Scrolled back, the same row is targetable again - so the failure
         * above was the row's position, not the row. */
        editor_scroll_set(0);
        ASSERT_INT("the same row opens once it is back on screen",
                   glr_ctrl_open_gl_state_popup(blank_line), 1);
    }
    ui_state_gl_state_inspector_close();
    ui_state_command_description_close();
}

/* The resize cursor must promise exactly what a press delivers. A panel
 * painted in front of the divider owns the pixel - the OpenGL-state popup is
 * classified ahead of the canonical hit-test in mouse_dispatch, so a press
 * there never reaches the divider - and the hover treatment, which the editor
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

/* Shift+right-click on a second blank row pins it as the report's comparison
 * basis, so the popup reads as the differential between the two probe points.
 * The color rows below bracket the pinned line: unchanged between the probes
 * on one side, changed on the other. */
static void test_shift_right_click_pins_gl_state_comparison_basis(void) {
    UiHit hit;
    UiGlStatePanelView view;
    int basis_line, anchor_line;
    int basis_x = -1, basis_y = -1;
    int anchor_x = -1, anchor_y = -1;
    const ReplGlStateReportRow *row = NULL;
    int i;

    printf("--- imrepl_ctrl OpenGL state report comparison basis ---\n");

    prepare_code_panel_mouse_fixture();
    editor_input_set_modifier_provider_for_test(simulated_mods_provider);
    g_simulated_mods = 0;
    editor_insert_mode_set(0);

    editor_state_edit_line_set(repl_state_document_count());
    ASSERT_INT("append first color", editor_feed_line("glColor3f(1, 0, 0);"), 1);
    basis_line = repl_state_document_count();
    editor_state_edit_line_set(basis_line);
    ASSERT_INT("append basis blank row", editor_feed_line(""), 1);
    editor_state_edit_line_set(repl_state_document_count());
    ASSERT_INT("append second color", editor_feed_line("glColor3f(0, 1, 0);"), 1);
    anchor_line = repl_state_document_count();
    editor_state_edit_line_set(anchor_line);
    ASSERT_INT("append anchor blank row", editor_feed_line(""), 1);

    ASSERT_TRUE("found basis row hit",
                find_code_text_hit_for_line(basis_line, &hit,
                                            &basis_x, &basis_y));
    ASSERT_TRUE("found anchor row hit",
                find_code_text_hit_for_line(anchor_line, &hit,
                                            &anchor_x, &anchor_y));

    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, anchor_x, anchor_y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, anchor_x, anchor_y);
    ASSERT_INT("plain right-click opens the report",
               ui_state_gl_state_inspector().visible, 1);
    ASSERT_INT("report opens on the GL defaults",
               ui_state_gl_state_inspector().basis_line_idx, -1);

    g_simulated_mods = GLUT_ACTIVE_SHIFT;
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, basis_x, basis_y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, basis_x, basis_y);
    ASSERT_INT("shift+right-click leaves the popup open",
               ui_state_gl_state_inspector().visible, 1);
    ASSERT_INT("shift+right-click keeps the original anchor",
               ui_state_gl_state_inspector().source_line_idx, anchor_line);
    ASSERT_INT("shift+right-click pins the basis",
               ui_state_gl_state_inspector().basis_line_idx, basis_line);
    ASSERT_INT("pinning a basis reveals the column it lives in",
               ui_state_gl_state_inspector().details_expanded, 1);

#ifdef GL_STUBS
    glr_ctrl_display_frame();
#endif
    view = glr_ctrl_build_gl_state_panel_view(NULL);
    ASSERT_INT("compare-mode view visible", view.visible, 1);
    ASSERT_INT("report carries the basis line",
               view.report ? view.report->basis_line_idx : -2, basis_line);
    for (i = 0; view.report && i < view.report->count; i++) {
        if (strcmp(view.report->rows[i].name, "GL_CURRENT_COLOR") == 0)
            row = &view.report->rows[i];
    }
    ASSERT_TRUE("color row present in compare mode", row != NULL);
    if (row) {
        ASSERT_CELL("color current is the value at the anchor",
                   row->current, "(0, 1, 0, 1)");
        ASSERT_CELL("color basis is the value at the pinned line",
                   row->basis_value, "(1, 0, 0, 1)");
        ASSERT_INT("color differs between the probe points",
                   row->differs_from_basis, 1);
    }
    /* The generated setup ran before both probes, so compare mode reports it
     * as unchanged - the point of the mode, since against the GL defaults
     * every one of those rows differs. */
    for (i = 0; view.report && i < view.report->count; i++) {
        const ReplGlStateReportRow *setup = &view.report->rows[i];
        if (setup->source.kind != REPL_GL_STATE_SOURCE_INIT)
            continue;
        ASSERT_INT("generated setup row is unchanged between the probes",
                   setup->differs_from_basis, 0);
        break;
    }

    /* Re-pinning the same line is the clear gesture. */
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, basis_x, basis_y);
    glr_ctrl_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, basis_x, basis_y);
    ASSERT_INT("re-pinning the basis line clears the comparison",
               ui_state_gl_state_inspector().basis_line_idx, -1);
    view = glr_ctrl_build_gl_state_panel_view(NULL);
    ASSERT_INT("cleared report is back on the GL defaults",
               view.report ? view.report->basis_line_idx : -2, -1);

    g_simulated_mods = 0;
    editor_input_set_modifier_provider_for_test(NULL);
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
        snprintf(report.rows[i].basis_value,
                 sizeof(report.rows[i].basis_value), "0");
        report.rows[i].differs_from_basis = 1;
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
    snprintf(report.rows[report.count - 1].basis_value,
             sizeof(report.rows[report.count - 1].basis_value),
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
    snprintf(report.rows[0].basis_value,
             sizeof(report.rows[0].basis_value), "(1, 1, 1, 1)");
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
    snprintf(report.rows[0].basis_value,
             sizeof(report.rows[0].basis_value),
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
 * when nothing hits. Coarse scan is fine - we only compare widths. */
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
        snprintf(report.rows[i].basis_value,
                 sizeof(report.rows[i].basis_value),
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

/* The basis column's header names the pinned line, so it is solved text
 * rather than a constant. The chip cell is sized from that same string in
 * both the collapsed and expanded forms - if the two ever diverged the chip
 * would draw in one place and hit-test in another. */
static void test_gl_state_popup_basis_header_sizes_the_chip(void) {
    static ReplGlStateReport report;
    UiGlStatePanelView view;
    int x = -1, y = -1;
    int default_right, basis_right;
    int i;

    printf("--- imrepl_ctrl OpenGL-state popup basis header ---\n");

    memset(&report, 0, sizeof(report));
    report.count = 4;
    report.user_row_count = report.count;
    report.basis_line_idx = -1;
    for (i = 0; i < report.count; i++) {
        snprintf(report.rows[i].name, sizeof(report.rows[i].name),
                 "GL_ROW_%02d", i);
        snprintf(report.rows[i].current, sizeof(report.rows[i].current), "1");
        snprintf(report.rows[i].basis_value,
                 sizeof(report.rows[i].basis_value), "0");
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
    view.basis_gutter_label = -1;

    ASSERT_TRUE("collapsed default-basis popup has a chip",
                gl_state_popup_find_details_toggle(&view, &x, &y));
    default_right = gl_state_popup_rightmost_hit_x(&view);
    view.details_expanded = 1;
    ASSERT_TRUE("expanded default-basis popup has a chip",
                gl_state_popup_find_details_toggle(&view, &x, &y));

    /* Same rows, now compared against a pinned line. Both header forms are
     * shorter than the "default (GL 2.1)" ones, so the popup narrows - which
     * is exactly the case a stale constant width would miss. */
    report.basis_line_idx = 11;
    view.basis_gutter_label = 12;
    view.details_expanded = 0;
    ASSERT_TRUE("collapsed compare-mode popup has a chip",
                gl_state_popup_find_details_toggle(&view, &x, &y));
    basis_right = gl_state_popup_rightmost_hit_x(&view);
    ASSERT_TRUE("compare-mode header resizes the popup",
                basis_right != default_right);
    view.details_expanded = 1;
    ASSERT_TRUE("expanded compare-mode popup has a chip",
                gl_state_popup_find_details_toggle(&view, &x, &y));
#ifdef GL_STUBS
    gl_stub_counts_reset();
    ui_gl_state_panel_render(&view);
    ASSERT_TRUE("compare-mode popup renders",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0);
#endif
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
     * delivering Ctrl+G through the keyboard callback. The controller must
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
    glr_ctrl_keyboard(KEY_CTRL_G, 0, 0);
    ASSERT_TRUE("Ctrl+G remains a controller-owned profile shortcut",
                glr_config_get(GLR_CONFIG_CPU_PROFILE) != old_cpu_profile);
    ASSERT_INT("controller-owned Ctrl+G preserves state",
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
        snprintf(report.rows[i].basis_value,
                 sizeof(report.rows[i].basis_value), "0");
        report.rows[i].differs_from_basis = 1;
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
 * number is good for is finding the line in the code panel - so it has to be
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
    /* Wider than the shared fixture: this case measures the popup growing
     * with its source column, which only says anything while the table still
     * has room to grow. At 800px a four-column report is already against the
     * window cap, and the solver answers by shrinking the value columns - a
     * correct response that would make the assertion below vacuous. */
    ui_state_viewport_set_size(1280, 600);
    glr_ctrl_sync_ui_chrome();
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
    /* Focus on: the derived-C chrome is hidden except the one row that
     * frames the body - `display() {` - so the gutter is the 1-based
     * document line shifted by that single row. (There is no blank spacer
     * above it here: this document has no file-scope prologue, so the
     * boundary is row 0.) */
    ASSERT_INT("focused gutter label clears the display() frame row",
               focused_label, enable_line + 2);
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

/* The popup is a reading surface parked over an animating scene: it stays open
 * across frames while `t` runs, and every value in it is recomputed each one.
 * Its geometry must not follow the numbers. Before the report printed in a
 * fixed field the widest cell changed length several times a second, so the
 * value column breathed, the columns right of it slid, and the popup - pinned
 * to the window's right edge - walked sideways under the pointer.
 *
 * So: same program, same probe, nine values of `t`, one solved frame each. The
 * cells have to change (otherwise this proves nothing) and the frame must
 * not. */
static void test_gl_state_popup_geometry_is_stable_over_time(void) {
    UiGlStatePanelView view;
    UiHit hit;
    int blank_line;
    int x = -1, y = -1;
    int right0 = -1, i, pass;
    int values_changed = 0;
    char first_cell[REPL_GL_STATE_VALUE_MAX];

    printf("--- imrepl_ctrl OpenGL-state popup geometry vs time ---\n");

    first_cell[0] = '\0';
    prepare_code_panel_mouse_fixture();
    /* Wide enough that the table is sized by its content: against the window
     * cap the solver pins the popup to the maximum width and nothing can move,
     * which would make this case pass for the wrong reason. */
    ui_state_viewport_set_size(1280, 600);
    glr_ctrl_sync_ui_chrome();

    /* An animated colour and nothing else that writes state. Under "%g" this
     * cell is the width of the table: its components run through 0.5, then
     * 0.973848, then 0.146447 - three characters one frame and eight the next,
     * because %g drops trailing zeros. Deliberately no transform: a modelview
     * row would print in the matrix's own fixed field and pin the column
     * width, hiding the very thing under test. */
    editor_state_edit_line_set(repl_state_document_count());
    ASSERT_INT("append an animated colour",
               editor_feed_line("glColor3f(0.5+0.5*sin(t), 0.5*cos(t), 0.25);"),
               1);
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

    /* Both folds. The default one is where the colour cell decides the table
     * width, so it is the pass that can actually catch a breathing column;
     * the expanded one brings in the generated setup rows - including the
     * camera's modelview matrix, whose fixed field is wider than anything the
     * colour can reach - and holds the width steady for a different reason.
     * Asserting both is what says the popup is stable in the view the reader
     * gets by default *and* in the one they expand into. */
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            ui_state_gl_state_inspector_toggle_details();
            ui_state_gl_state_inspector_toggle_setup();
            right0 = -1;
        }
        for (i = 0; i < 9; i++) {
            const ReplGlStateReportRow *row = NULL;
            int right, j;
            char label[128];

            repl_set_time((float)i * 0.37f);
            glr_ctrl_display_frame();
            view = glr_ctrl_build_gl_state_panel_view(NULL);
            ASSERT_TRUE("popup still visible while time runs",
                        view.visible != 0);
            if (!view.report)
                return;

            for (j = 0; j < view.report->count; j++)
                if (strcmp(view.report->rows[j].name, "GL_CURRENT_COLOR") == 0)
                    row = &view.report->rows[j];
            ASSERT_TRUE("colour row present", row != NULL);
            if (row) {
                if (first_cell[0] == '\0')
                    snprintf(first_cell, sizeof(first_cell), "%s",
                             row->current);
                else if (strcmp(first_cell, row->current) != 0)
                    values_changed = 1;
            }

            right = gl_state_popup_rightmost_hit_x(&view);
            ASSERT_TRUE("popup answers hits", right >= 0);
            if (right0 < 0)
                right0 = right;
            snprintf(label, sizeof(label),
                     "popup right edge is unmoved at t step %d (fold %d)",
                     i, pass);
            ASSERT_INT(label, right, right0);
        }
    }

    ASSERT_TRUE("the animated cell really did change", values_changed);
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

    /* 1. First frame is a SNAP - rule 8 seeding (in glr_ctrl_reset_all)
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
     *    presentation at full opacity, STEADY - no post-reset animation. */
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
    glr_frame_ended();
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
    glr_frame_ended();
    ASSERT_FLOAT("frame mode: first frame advances once",
                 repl_state_variables().anim_time, GLR_FRAME_DT_SECS);
    glr_frame_ended();
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
     * the moment projection settles - that's the sample to measure
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
 * flight, and (2) the combine is min() - 3D view + Ortho toggle still yields
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
    /* min(view_mix=1, toggle_mix=0) == 0 - the combine picks the ortho
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
 * leaves the snapshot pointing at the pose captured on 2D entry - so
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
 * (dist=7.5 here) - otherwise the ortho zoom locks onto the old camera
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
 * the projection MIX reaches the target - tick_until_view_settled returns
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
 * pose - not to the global built-in defaults. Import streams the block through
 * the camera bridge line by line; the end-of-import hook is what turns the
 * result into the scene default. */
static void test_import_camera_block_becomes_scene_default(void) {
    static const char *const k_lines[] = {
        "// camera",
        "glTranslatef(0.0f, 0.0f, -15.0f);   // @camera dist",
        "glRotatef(26.0f, 1.0f, 0.0f, 0.0f);   // @camera rx",
        "glRotatef(-20.0f, 0.0f, 1.0f, 0.0f);   // @camera ry",
        "glTranslatef(-1.0f, -2.0f, -3.0f);   // @camera pan",
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

static void test_new_scene_from_2d_pose_does_not_poison_reset(void) {
    static const char *const k_lines[] = {
        "// @cfg view_mode = RENDER3D_VIEW_2D",
        "glBegin(GL_POINTS);",
        "glVertex3f(0, 0, 0);",
        "glEnd();",
        NULL
    };
    GlrCameraState cam;
    int slot;

    printf("--- imrepl_ctrl new scene from 2d does not poison reset ---\n");
    glr_ctrl_reset_all();
    glr_camera_set(11.0f, 22.0f, 7.5f, 0.5f, -0.25f, 1.75f, 0.0f);
    glr_ctrl_display_frame();
    repl_scenes_capture_pre_example_cfg_if_entering();
    ASSERT_TRUE("2d example loads", repl_load_example_lines(k_lines) > 0);
    glr_ctrl_display_frame();
    for (int i = 0; i < 400; i++) {
        glr_ctrl_tick();
        glr_ctrl_display_frame();
        if (!glr_camera_target_active() &&
            g_last_scene_config.projection_mix == 0.0f)
            break;
    }

    ASSERT_INT("new scene action succeeds",
               glr_action_menu_item_activate(GLR_MENU_FILE,
                                              GLR_FILE_ITEM_NEW_SCENE), 1);
    slot = repl_active_user_scene();
    ASSERT_TRUE("new scene created from 2d example", slot >= 0);

    for (int i = 0; i < 400; i++) {
        glr_ctrl_tick();
        glr_ctrl_display_frame();
        if (!glr_camera_target_active() &&
            g_last_scene_config.projection_mix == 1.0f)
            break;
    }

    repl_scenes_detach_active_user_scene();
    ASSERT_TRUE("new scene reloads", repl_load_user_scene_idx(slot));
    glr_ctrl_display_frame();
    glr_camera_ease_to_default();
    for (int i = 0; i < 600 && glr_camera_target_active(); i++)
        glr_camera_tick();
    cam = glr_camera();
    ASSERT_TRUE("reset does not return to flattened 2d rx",
                fabsf(cam.rx - 20.0f) < 0.05f);
    ASSERT_TRUE("reset does not return to flattened 2d ry",
                fabsf(cam.ry - 30.0f) < 0.05f);
}

static void test_new_scene_real_2d_reset_during_transition(void) {
    int example = -1;
    int slot;
    GlrCameraState cam;

    printf("--- imrepl_ctrl real 2d new scene reset during transition ---\n");
    for (int i = 0; i < glr_scene_example_count(); i++) {
        const char *name = glr_scene_example_name(i);
        if (name && strstr(name, "2D assignment sketch") != NULL) {
            example = i;
            break;
        }
    }
    ASSERT_TRUE("real 2d example found", example >= 0);
    if (example < 0)
        return;

    glr_ctrl_reset_all();
    glr_ctrl_display_frame();
    glr_scene_load_example(example);
    glr_ctrl_display_frame();
    for (int i = 0; i < 500; i++) {
        glr_ctrl_tick();
        glr_ctrl_display_frame();
        if (!glr_camera_target_active() &&
            g_last_scene_config.projection_mix == 0.0f)
            break;
    }
    cam = glr_camera();
    ASSERT_INT("real 2d example settles in ortho",
               glr_state_presentation().ortho_mode, 1);
    ASSERT_FLOAT("real 2d example settles projection",
                 g_last_scene_config.projection_mix, 0.0f);
    ASSERT_TRUE("real 2d example looks down z",
                fabsf(cam.rx) < 0.05f && fabsf(cam.ry) < 0.05f);

    ASSERT_INT("real 2d new scene action succeeds",
               glr_action_menu_item_activate(GLR_MENU_FILE,
                                              GLR_FILE_ITEM_NEW_SCENE), 1);
    slot = repl_active_user_scene();
    ASSERT_TRUE("real 2d new scene is active", slot >= 0);

    /* The action changes the scene cfg to 3D, but the transition tick has not
     * run yet. Reset here: this is the race that used to let the old 2D saved
     * orbit overwrite the reset target at the end of the projection leg. */
    editor_input_set_modifier_provider_for_test(simulated_mods_provider);
    g_simulated_mods = GLUT_ACTIVE_SHIFT;
    glr_ctrl_keyboard(KEY_CTRL_C, 0, 0);
    ASSERT_TRUE("reset during new-scene transition starts an ease",
                glr_camera_target_active());
    for (int i = 0; i < 500; i++) {
        glr_ctrl_tick();
        glr_ctrl_display_frame();
        if (!glr_camera_target_active() &&
            g_last_scene_config.projection_mix == 1.0f)
            break;
    }
    cam = glr_camera();
    ASSERT_FLOAT("new scene returns to 3D",
                 g_last_scene_config.projection_mix, 1.0f);
    ASSERT_TRUE("reset during new-scene transition keeps default rx",
                fabsf(cam.rx - 20.0f) < 0.05f);
    ASSERT_TRUE("reset during new-scene transition keeps default ry",
                fabsf(cam.ry - 30.0f) < 0.05f);
    g_simulated_mods = 0;
    editor_input_set_modifier_provider_for_test(NULL);
}

/* Cycling 2D-example -> 3D-example-A -> 3D-example-B fast: example A
 * starts a 2D->3D projection blend; example B loads mid-blend. The
 * saved-3D snapshot (consumed by start_camera_to_3d when the blend ends)
 * must refresh to B's pose, not stay stuck on A's - and the carried
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

/* Carpet -> sponge can switch before the 3D->2D camera leg has finished,
 * while projection_mix is therefore still 1. The sponge camera bridge queues
 * its authored 3D orbit for the next display frame. A timer tick in between
 * must not see the already-perspective projection, immediately consume the
 * carpet's saved orbit, and make the later sponge record too late to apply. */
static void test_view_mode_quick_2d_to_3d_waits_for_pending_example_pose(void) {
    static const char *const carpet[] = {
        "// @cfg view_mode = RENDER3D_VIEW_2D",
        "",
        "glTranslatef(0.0f, 0.0f, -6.0f);   // @camera dist",
        "glRotatef(0.0f, 1.0f, 0.0f, 0.0f);   // @camera rx",
        "glRotatef(0.0f, 0.0f, 1.0f, 0.0f);   // @camera ry",
        "glTranslatef(0.0f, 0.0f, 0.0f);   // @camera pan",
        "",
        "glBegin(GL_POINTS);",
        "glVertex3f(0, 0, 0);",
        "glEnd();",
        NULL
    };
    static const char *const sponge[] = {
        "glTranslatef(0.0f, 0.0f, -9.0f);   // @camera dist",
        "glRotatef(18.0f, 1.0f, 0.0f, 0.0f);   // @camera rx",
        "glRotatef(30.0f, 0.0f, 1.0f, 0.0f);   // @camera ry",
        "glTranslatef(0.0f, 0.0f, 0.0f);   // @camera pan",
        "",
        "glBegin(GL_POINTS);",
        "glVertex3f(0, 0, 0);",
        "glEnd();",
        NULL
    };
    GlrCameraState cam;

    printf("--- imrepl_ctrl quick carpet-to-sponge camera handoff ---\n");
    prepare_display_fixture();
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;
    glr_ctrl_display_frame();

    glr_ctrl_reset_transients();
    ASSERT_TRUE("2D carpet fixture loads", repl_load_example_lines(carpet) > 0);
    /* Drain the carpet record, then let one timer tick enter CAMERA_TO_2D.
     * Its projection leg has not begun, so the mix remains perspective. */
    glr_ctrl_display_frame();
    glr_ctrl_tick();
    glr_ctrl_display_frame();
    ASSERT_TRUE("carpet camera flatten is still active",
                glr_camera_target_active());
    ASSERT_FLOAT("projection is still at the perspective endpoint",
                 g_last_scene_config.projection_mix, 1.0f);

    glr_ctrl_reset_transients();
    ASSERT_TRUE("3D sponge fixture loads", repl_load_example_lines(sponge) > 0);
    ASSERT_TRUE("sponge 3D pose is pending the display-frame handoff",
                glr_camera_export_has_pending_3d_pose());

    /* Reproduce the problematic callback ordering: timer before display. */
    glr_ctrl_tick();
    glr_ctrl_display_frame();
    ASSERT_TRUE("display consumed the sponge pose",
                !glr_camera_export_has_pending_3d_pose());

    for (int i = 0; i < 400; i++) {
        glr_ctrl_tick();
        glr_ctrl_display_frame();
        if (g_last_scene_config.projection_mix == 1.0f &&
            !glr_camera_target_active() &&
            !glr_ctrl_view_transition_active())
            break;
    }

    cam = glr_camera();
    ASSERT_TRUE("sponge rx wins over the carpet's flat orbit",
                fabsf(cam.rx - 18.0f) < 0.05f);
    ASSERT_TRUE("sponge ry wins over the carpet's flat orbit",
                fabsf(cam.ry - 30.0f) < 0.05f);
    ASSERT_TRUE("sponge distance reaches its authored target",
                fabsf(cam.dist - 9.0f) < 0.05f);
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

    /* Fire the bridge while in 3D - the saved snapshot must be ignored. */
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

/* Viewing a built-in example is not user work: the live document is the
 * shipped example verbatim, so quitting from it must NOT write (and
 * thereby clobber) QUIT_RECOVERY_FILE. The first edit promotes the
 * example into a user-scene slot, and from then on it is worth saving. */
static void test_recovery_skips_unpromoted_example(void) {
    char cwd[1024];
    char temp_dir[] = "/tmp/test_glr_ctrl_recovery_ex.XXXXXX";
    char *made_dir;
    int have_cwd;

    printf("--- imrepl_ctrl recovery skips unpromoted example ---\n");

    glr_ctrl_reset_all();
    repl_load_example(0);
    ASSERT_INT("example active, no scene slot", repl_active_user_scene(), -1);
    ASSERT_INT("unpromoted example is not user work",
               glr_ctrl_recovery_has_user_work(), 0);

    made_dir = mkdtemp(temp_dir);
    have_cwd = getcwd(cwd, sizeof(cwd)) != NULL;
    ASSERT_TRUE("mkdtemp example-recovery dir", made_dir != NULL);
    ASSERT_TRUE("getcwd before example-recovery save", have_cwd);
    if (!made_dir || !have_cwd)
        return;

    ASSERT_INT("chdir example-recovery dir", chdir(made_dir), 0);
    glr_ctrl_save_quit_recovery();
    ASSERT_TRUE("quit from example wrote no recovery file",
                access(QUIT_RECOVERY_FILE, F_OK) != 0);

    /* Promote (what any edit does) -> the document is now the user's. */
    ASSERT_TRUE("example promoted to a slot",
                repl_promote_transient_if_needed() >= 0);
    ASSERT_INT("promoted example is user work",
               glr_ctrl_recovery_has_user_work(), 1);
    glr_ctrl_save_quit_recovery();
    ASSERT_INT("quit after promotion wrote recovery file",
               access(QUIT_RECOVERY_FILE, F_OK), 0);

    unlink(QUIT_RECOVERY_FILE);
    ASSERT_INT("restore cwd after example-recovery save", chdir(cwd), 0);
    rmdir(made_dir);
    glr_ctrl_reset_all();
}

/* Remove a workspace directory and every file its manifest names. */
static void remove_recovery_workspace(const char *dir) {
    WorkspaceManifest manifest;
    char err[REPL_STATUS_TEXT_MAX];
    char path[REPL_WORKSPACE_DIR_MAX + WORKSPACE_IO_FILE_MAX + 8];
    if (workspace_io_manifest_read(dir, &manifest, err, sizeof(err))) {
        for (int i = 0; i < manifest.scene_count; i++) {
            if (workspace_io_path_join(dir, manifest.scene_files[i],
                                       path, sizeof(path)))
                unlink(path);
        }
    }
    if (workspace_io_path_join(dir, WORKSPACE_IO_MANIFEST_FILE,
                               path, sizeof(path)))
        unlink(path);
    rmdir(dir);
}

/* Quitting while an example is on screen must still rescue the user's
 * in-memory scene slots - they die with the process and the single-file
 * recovery copy can only hold the (uninteresting) visible document. They
 * go to a recovery WORKSPACE instead, never to the bound workspace dir. */
static void test_recovery_workspace_rescues_scene_slots(void) {
    char cwd[1024];
    char temp_dir[] = "/tmp/test_glr_ctrl_recovery_ws.XXXXXX";
    char *made_dir;
    int have_cwd;

    printf("--- imrepl_ctrl recovery workspace rescues scene slots ---\n");

    glr_ctrl_reset_all();
    ASSERT_TRUE("created a user scene", repl_scenes_create_empty_user_scene() >= 0);
    editor_feed_line("glVertex3f(1, 2, 3);");
    repl_load_example(0);
    ASSERT_INT("example is live, not the scene", repl_active_user_scene(), -1);
    ASSERT_INT("scene slot survives the example load", repl_user_scene_count(), 1);

    made_dir = mkdtemp(temp_dir);
    have_cwd = getcwd(cwd, sizeof(cwd)) != NULL;
    ASSERT_TRUE("mkdtemp recovery-workspace dir", made_dir != NULL);
    ASSERT_TRUE("getcwd before recovery-workspace save", have_cwd);
    if (!made_dir || !have_cwd)
        return;

    ASSERT_INT("chdir recovery-workspace dir", chdir(made_dir), 0);
    ASSERT_INT("quit rescued the scene slots",
               glr_ctrl_save_quit_recovery(), 1);
    ASSERT_TRUE("no recovery.c written for the example",
                access(QUIT_RECOVERY_FILE, F_OK) != 0);
    ASSERT_INT("recovery workspace manifest written",
               access("recovery-workspace/" WORKSPACE_IO_MANIFEST_FILE, F_OK), 0);
    ASSERT_STR("workspace binding left unbound", repl_workspace_dir(), "");

    remove_recovery_workspace("recovery-workspace");
    ASSERT_INT("restore cwd after recovery-workspace save", chdir(cwd), 0);
    rmdir(made_dir);
    glr_ctrl_reset_all();
}

/* UI snapshot idempotency: glr_ctrl_build_ui_snapshot is called twice per
 * display frame because the second build picks up post-follow-scroll offsets.
 * Building it twice with no intervening state change must preserve all
 * observable fields.
 *
 * We don't memcmp() the whole struct because it contains pointers
 * (document_cmds, snapshots of subsystem state) that may legitimately
 * vary across builds in some refactors. Instead spot-check the fields
 * a renderer would actually consume - viewport, code-panel, replay,
 * camera-derived selection, autocomplete - so the test pins the
 * observable contract rather than internal layout. */
static void test_build_ui_snapshot_is_idempotent(void) {
    UiRenderSnapshot snap_a;
    UiRenderSnapshot snap_b;

    printf("--- imrepl_ctrl snapshot idempotence ---\n");
    prepare_display_fixture();
    repl_state_flat_program_writable()->overflow_cmd_count = MAX_FLAT_COMMANDS + 7;

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

/* Render3d config invariants over the per-frame display path. The contract
 * is the set of fields the controller must populate on Render3dRenderConfig
 * so the render3d module can drive overlay passes directly.
 *
 * Most fields are already pinned by
 * test_display_frame_builds_config_and_restores_live_state; this test adds
 * the invariants that travel ACROSS frames - repeated frames must not
 * introduce hysteresis, and the post-overlays hook's user_data must point
 * at the config that hosts the guides. */
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
     * render3d_render (via the replay tick), so don't pin that here -
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
    ASSERT_INT("scene show_light_indicators stable across frames",
               frame2.show_light_indicators, frame1.show_light_indicators);
    ASSERT_FLOAT("scene alpha_scale stable across frames",
                 frame2.alpha_scale, frame1.alpha_scale);

    /* The post_overlays_fn hook is wired with config-as-user_data so
     * the hook reads guides off Render3dRenderConfig. Pin that pointer
     * identity - a refactor that switches user_data to NULL or to a
     * different pointer would break the guide overlay. */
    ASSERT_TRUE("post_overlays_fn wired in frame 1",
                frame1.post_overlays_fn != NULL);
    ASSERT_TRUE("post_overlays_fn wired in frame 2",
                frame2.post_overlays_fn != NULL);
    /* user_data is the live config the controller passed into
     * render3d_draw_scene (not the cached frame1/frame2 copies). The test
     * deliberately does not assert identity with g_last_scene_config: the
     * controller hands the render3d module its own local Render3dRenderConfig.
     * We pin the looser invariant that the hook carries non-NULL user_data. */
    ASSERT_TRUE("post_overlays_user_data non-NULL",
                frame2.post_overlays_user_data != NULL);
    /* Same contract for the once-per-frame label hook. */
    ASSERT_TRUE("post_resolve_overlays_fn wired in frame 2",
                frame2.post_resolve_overlays_fn != NULL);
    ASSERT_TRUE("post_resolve_overlays_user_data non-NULL",
                frame2.post_resolve_overlays_user_data != NULL);
}

/* Replay-fade is the largest of the controller's overlay integrations.
 * When replay is OFF, no fade plumbing should be wired (no post_fill_fn
 * for fades, base_limit at zero, empty batch ring). Keep the inactive-
 * replay path costless. */
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
 * tokens, confined to the (...) argument range - a bit token mentioned in a
 * trailing comment is never coloured - and the saved setter, reverted scoped
 * setter, and matching pop bracket get their highlights. */
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
    ASSERT_INT("scoped setter reverted by the matching pop gets the marker",
               count_highlight_kind_on_line(HIGHLIGHT_ATTRIB_STATE, 2), 1);
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
    ASSERT_INT("first nested replay highlights immediate call site on chain",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 6), 1);
    ASSERT_INT("first nested replay highlights root call site on chain",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 8), 1);
    ASSERT_INT("first nested replay has no root marker on second outer call",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 9), 0);
    ASSERT_INT("indexed frame suppresses legacy call site marker",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_SITE, 6), 0);
    ASSERT_INT("indexed frame suppresses legacy root call site marker",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_ROOT_CALL_SITE, 8), 0);

    replay_advance(repl_state_flat_program_view());
    glr_ctrl_push_highlights();

    ASSERT_INT("second nested replay still highlights immediate call site",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 6), 1);
    ASSERT_INT("second nested replay moves root marker to second outer call",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 9), 1);
    ASSERT_INT("second nested replay clears prior root marker",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 8), 0);

    replay_stop();
    glr_ctrl_push_highlights();
    ASSERT_INT("inactive replay clears call-chain highlights",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 6), 0);
}

static void test_replay_call_chain_ramp_colors(void) {
    printf("--- imrepl_ctrl replay call-chain ramp colors ---\n");

    glr_ctrl_reset_all();
    editor_feed_line("func2() {");               /* 0 */
    editor_feed_line("glBegin(GL_POINTS);");      /* 1 */
    editor_feed_line("glVertex3f(0, 0, 0);");     /* 2 */
    editor_feed_line("glEnd();");                 /* 3 */
    editor_feed_line("}");                        /* 4 */
    editor_feed_line("func1() {");               /* 5 */
    editor_feed_line("func2();");                 /* 6 */
    editor_feed_line("}");                        /* 7 */
    editor_feed_line("func0() {");               /* 8 */
    editor_feed_line("func1();");                 /* 9 */
    editor_feed_line("}");                        /* 10 */
    editor_feed_line("func0();");                 /* 11 */
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    replay_state_mut()->state = REPLAY_PAUSED;
    replay_state_mut()->mode = REPLAY_MODE_VERTEX;

    replay_advance(repl_state_flat_program_view());
    glr_ctrl_push_highlights();

    ASSERT_INT("chain highlights root call site",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 11), 1);
    ASSERT_INT("chain highlights middle call site",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 9), 1);
    ASSERT_INT("chain highlights immediate call site",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 6), 1);

    int aux_root = get_highlight_aux_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 11, 0);
    int aux_mid  = get_highlight_aux_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 9, 0);
    int aux_leaf = get_highlight_aux_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 6, 0);

    ASSERT_TRUE("root and mid colors are distinct", aux_root != aux_mid);
    ASSERT_TRUE("mid and leaf colors are distinct", aux_mid != aux_leaf);
    ASSERT_TRUE("root and leaf colors are distinct", aux_root != aux_leaf);

    /* Unpack RGB to assert monotonic warmth (red rises, blue falls) */
    int r_root = (aux_root >> 16) & 0xFF, b_root = aux_root & 0xFF;
    int r_mid  = (aux_mid >> 16) & 0xFF,  b_mid  = aux_mid & 0xFF;
    int r_leaf = (aux_leaf >> 16) & 0xFF, b_leaf = aux_leaf & 0xFF;

    ASSERT_TRUE("red channel rises monotonically along chain",
                r_root <= r_mid && r_mid <= r_leaf && r_root < r_leaf);
    ASSERT_TRUE("blue channel falls monotonically along chain",
                b_root >= b_mid && b_mid >= b_leaf && b_root > b_leaf);

    replay_stop();
}

static void test_replay_call_chain_recursive_same_line(void) {
    printf("--- imrepl_ctrl replay recursive same-line call chain ---\n");

    glr_ctrl_reset_all();
    editor_feed_line("rec(d) {");                 /* 0 */
    editor_feed_line("if (d > 0) {");             /* 1 */
    editor_feed_line("rec(d - 1);");              /* 2 */
    editor_feed_line("}");                        /* 3 */
    editor_feed_line("glBegin(GL_POINTS);");      /* 4 */
    editor_feed_line("glVertex3f(d, 0, 0);");     /* 5 */
    editor_feed_line("glEnd();");                 /* 6 */
    editor_feed_line("}");                        /* 7 */
    editor_feed_line("rec(2);");                  /* 8 */
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    replay_state_mut()->state = REPLAY_PAUSED;
    replay_state_mut()->mode = REPLAY_MODE_VERTEX;

    /* First vertex is from deepest recursion: d = 0, chain depth 3 (root @8, rec(1) @2, rec(0) @2) */
    replay_advance(repl_state_flat_program_view());
    glr_ctrl_push_highlights();

    ASSERT_INT("root call site has 1 chain highlight",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 8), 1);
    ASSERT_INT("recursive call line 2 has 2 chain highlights",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 2), 2);

    int aux_rec0 = get_highlight_aux_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 2, 0);
    int aux_rec1 = get_highlight_aux_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 2, 1);
    ASSERT_TRUE("recursive same-line rungs have distinct ramp colours",
                aux_rec0 != aux_rec1);

    /* Verify code panel produces 2 distinct left marker bands on line 2 */
    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.45f;

    UiRenderSnapshot snap;
    glr_ctrl_build_ui_snapshot(&snap);
    ui_repl_code_panel_render_with_chrome(&snap, NULL);

    int active = 0, band_count = 0;
    float band_rgba[4][4];
    ASSERT_TRUE("code panel row marker bands available for line 2",
                ui_repl_code_panel_row_marker_bands_for_test(2, &active, &band_count, band_rgba, 4));
    ASSERT_INT("line 2 marker active", active, 1);
    ASSERT_INT("line 2 has 2 marker bands", band_count, 2);
    ASSERT_TRUE("band 0 and band 1 have distinct colors",
                band_rgba[0][0] != band_rgba[1][0] ||
                band_rgba[0][1] != band_rgba[1][1] ||
                band_rgba[0][2] != band_rgba[1][2]);

    replay_stop();
}

static void test_replay_call_chain_unindexed_fallback(void) {
    printf("--- imrepl_ctrl replay unindexed call chain fallback ---\n");

    glr_ctrl_reset_all();
    editor_feed_line("func1() {");                /* 0 */
    editor_feed_line("glBegin(GL_POINTS);");      /* 1 */
    editor_feed_line("glVertex3f(0, 0, 0);");     /* 2 */
    editor_feed_line("glEnd();");                 /* 3 */
    editor_feed_line("}");                        /* 4 */
    editor_feed_line("func0() {");                /* 5 */
    editor_feed_line("func1();");                 /* 6 */
    editor_feed_line("}");                        /* 7 */
    editor_feed_line("func0();");                 /* 8 */
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    replay_state_mut()->state = REPLAY_PAUSED;
    replay_state_mut()->mode = REPLAY_MODE_VERTEX;

    replay_advance(repl_state_flat_program_view());

    /* Temporarily simulate unindexed frame table */
    ReplFlatProgramState *flat_state = repl_state_flat_program_writable();
    int focus = replay_focus_flat_idx();
    int saved_frame = flat_state->call_frame_idx[focus];
    flat_state->call_frame_idx[focus] = REPL_CALL_FRAME_NONE;

    glr_ctrl_push_highlights();

    ASSERT_INT("unindexed frame falls back to legacy immediate call site",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_SITE, 6), 1);
    ASSERT_INT("unindexed frame falls back to legacy root call site",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_ROOT_CALL_SITE, 8), 1);
    ASSERT_INT("unindexed frame suppresses multi-entry chain highlight",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 6), 0);

    /* Restore */
    flat_state->call_frame_idx[focus] = saved_frame;

    replay_stop();
}

static void test_replay_call_chain_gutter_bands_capping(void) {
    printf("--- imrepl_ctrl replay 4-band capping policy ---\n");

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.45f;

    for (int i = 0; i < 5; i++) {
        editor_feed_line("glVertex3f(0, 0, 0);");
    }

    /* Test that if 6 chain highlights land on one line, the UI retains
     * outermost (idx 0) + 3 innermost (idx 3, 4, 5). */
    editor_state_highlights_clear();
    for (int i = 0; i < 6; i++) {
        int r = (i + 1) * 30;
        int aux = (r << 16) | (r << 8) | r;
        editor_state_highlights_append_aux(2, -1, -1, HIGHLIGHT_REPLAY_CALL_CHAIN, aux);
    }

    UiRenderSnapshot snap;
    glr_ctrl_build_ui_snapshot(&snap);
    ui_repl_code_panel_render_with_chrome(&snap, NULL);

    int active = 0, band_count = 0;
    float band_rgba[4][4];
    ASSERT_TRUE("code panel row marker bands available for line 2",
                ui_repl_code_panel_row_marker_bands_for_test(2, &active, &band_count, band_rgba, 4));
    ASSERT_INT("capped band count is 4", band_count, 4);

    /* Check that band 0 is from i=0 (r = 30 / 255) */
    float r0 = 30.0f / 255.0f;
    float r3 = 120.0f / 255.0f;
    float r4 = 150.0f / 255.0f;
    float r5 = 180.0f / 255.0f;

    ASSERT_TRUE("band 0 is outermost", (float)fabs(band_rgba[0][0] - r0) < 0.02f);
    ASSERT_TRUE("band 1 is innermost-2 (i=3)", (float)fabs(band_rgba[1][0] - r3) < 0.02f);
    ASSERT_TRUE("band 2 is innermost-1 (i=4)", (float)fabs(band_rgba[2][0] - r4) < 0.02f);
    ASSERT_TRUE("band 3 is innermost-0 (i=5)", (float)fabs(band_rgba[3][0] - r5) < 0.02f);

    editor_state_highlights_clear();
}

/* Publication must start from replay_focus_flat_idx(), not the vertex-only
 * replay_focus_anchor_flat_idx(). Polygon mode and a draw-free step both
 * leave the anchor at -1; the chain must still publish. */
static void test_replay_call_chain_focus_not_anchor(void) {
    printf("--- imrepl_ctrl replay call-chain focus vs vertex-only anchor ---\n");

    glr_ctrl_reset_all();
    editor_feed_line("func1() {");                /* 0 */
    editor_feed_line("glBegin(GL_POINTS);");      /* 1 */
    editor_feed_line("glVertex3f(0, 0, 0);");     /* 2 */
    editor_feed_line("glEnd();");                 /* 3 */
    editor_feed_line("}");                        /* 4 */
    editor_feed_line("func0() {");                /* 5 */
    editor_feed_line("func1();");                 /* 6 */
    editor_feed_line("}");                        /* 7 */
    editor_feed_line("func0();");                 /* 8 */
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    replay_state_mut()->state = REPLAY_PAUSED;
    replay_state_mut()->mode = REPLAY_MODE_POLYGON;
    replay_advance(repl_state_flat_program_view());
    glr_ctrl_push_highlights();

    ASSERT_TRUE("polygon step has a focused command",
                replay_focus_flat_idx() >= 0);
    ASSERT_INT("polygon step has no vertex-mode draw anchor",
               replay_focus_anchor_flat_idx(), -1);
    ASSERT_INT("polygon step still publishes immediate call-chain rung",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 6), 1);
    ASSERT_INT("polygon step still publishes root call-chain rung",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 8), 1);
    replay_stop();

    glr_ctrl_reset_all();
    editor_feed_line("func1() {");                /* 0 */
    editor_feed_line("glTranslatef(1, 0, 0);");   /* 1 */
    editor_feed_line("}");                        /* 2 */
    editor_feed_line("func0() {");                /* 3 */
    editor_feed_line("func1();");                 /* 4 */
    editor_feed_line("}");                        /* 5 */
    editor_feed_line("func0();");                 /* 6 */
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    replay_state_mut()->state = REPLAY_PAUSED;
    replay_state_mut()->mode = REPLAY_MODE_VERTEX;
    replay_advance(repl_state_flat_program_view());
    glr_ctrl_push_highlights();

    ASSERT_TRUE("draw-free vertex step has a focused command",
                replay_focus_flat_idx() >= 0);
    ASSERT_INT("draw-free vertex step has no draw anchor",
               replay_focus_anchor_flat_idx(), -1);
    ASSERT_INT("draw-free step still publishes immediate call-chain rung",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 4), 1);
    ASSERT_INT("draw-free step still publishes root call-chain rung",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 6), 1);
    replay_stop();
}

/* Live rec(5) publishes every same-line ancestor (five rungs) and the
 * gutter keeps outermost + 3 innermost. walk_chain(..., 4) would drop
 * the leaf and never reach five published entries on that line. */
static void test_replay_call_chain_recursive_deep_cap(void) {
    printf("--- imrepl_ctrl replay recursive call-chain live 4-band cap ---\n");

    glr_ctrl_reset_all();
    editor_feed_line("rec(d) {");                 /* 0 */
    editor_feed_line("if (d > 0) {");             /* 1 */
    editor_feed_line("rec(d - 1);");              /* 2 */
    editor_feed_line("}");                        /* 3 */
    editor_feed_line("glBegin(GL_POINTS);");      /* 4 */
    editor_feed_line("glVertex3f(d, 0, 0);");     /* 5 */
    editor_feed_line("glEnd();");                 /* 6 */
    editor_feed_line("}");                        /* 7 */
    editor_feed_line("rec(5);");                  /* 8 */
    repl_flatten_commands(editor_state_edit_line());

    replay_start();
    replay_state_mut()->state = REPLAY_PAUSED;
    replay_state_mut()->mode = REPLAY_MODE_VERTEX;
    replay_advance(repl_state_flat_program_view());
    glr_ctrl_push_highlights();

    ASSERT_INT("root call site has 1 chain highlight",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 8), 1);
    ASSERT_INT("recursive call line publishes all 5 ancestor rungs",
               count_highlight_kind_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 2), 5);

    int aux0 = get_highlight_aux_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 2, 0);
    int aux2 = get_highlight_aux_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 2, 2);
    int aux3 = get_highlight_aux_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 2, 3);
    int aux4 = get_highlight_aux_on_line(HIGHLIGHT_REPLAY_CALL_CHAIN, 2, 4);
    ASSERT_TRUE("outermost same-line rung has a colour", aux0 >= 0);
    ASSERT_TRUE("innermost same-line rung has a colour", aux4 >= 0);

    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.45f;

    UiRenderSnapshot snap;
    glr_ctrl_build_ui_snapshot(&snap);
    ui_repl_code_panel_render_with_chrome(&snap, NULL);

    int active = 0, band_count = 0;
    float band_rgba[4][4];
    ASSERT_TRUE("code panel row marker bands available for line 2",
                ui_repl_code_panel_row_marker_bands_for_test(2, &active, &band_count, band_rgba, 4));
    ASSERT_INT("line 2 marker active", active, 1);
    ASSERT_INT("line 2 gutter keeps outermost + 3 innermost", band_count, 4);
    ASSERT_TRUE("band 0 is the outermost same-line rung",
                band_matches_aux(band_rgba[0], aux0));
    ASSERT_TRUE("band 1 is the third-innermost same-line rung",
                band_matches_aux(band_rgba[1], aux2));
    ASSERT_TRUE("band 2 is the second-innermost same-line rung",
                band_matches_aux(band_rgba[2], aux3));
    ASSERT_TRUE("band 3 is the innermost same-line rung",
                band_matches_aux(band_rgba[3], aux4));

    replay_stop();
}

/* Req 5: during replay the affecting-transform highlight tracks the
 * replay-focused vertex (via the req-4 exact-flat resolver), not the edit
 * cursor - and each expansion shows only its own in-scope transforms. */
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
     * flat resolver would union BOTH expansions -> {5, 7, 8}; this lets the
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
     * resolves the LAST flat expansion of the cursor line - here the second
     * func0() call - so it shows that expansion's transform set: all three. */
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

/* Last-instance scope resolves the cursor line's LAST flat expansion - the
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

/* Numeric swatch routing owns compile + parse + ReplCompiledChange
 * construction + apply + reload. Pin its observable contract:
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
    /* The new arg is greater than 1.5 - pin the direction without
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

    /* Each swatch step is a discrete undo unit - three steps total
     * (one up, two down) so three Ctrl+Z restores the original. */
    editor_undo_pop_snapshot();
    editor_undo_pop_snapshot();
    editor_undo_pop_snapshot();
    buf = editor_buffer_view();
    ASSERT_STR("undox3 restores the original line",
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
    /* Cursor mid-comment - no numeric arg available. */
    editor_cursor_pos_set(6);
    buf = editor_buffer_view();
    snprintf(before, sizeof(before), "%s", editor_buffer_view_line(buf, 0));

    hit_up.kind     = UI_HIT_NUMERIC_SWATCH;
    hit_up.item_idx = 1;
    int rc = route_numeric_swatch_hit(&hit_up, 1.0f);
    /* Consumed (returns 1) - the swatch hit is the router's
     * responsibility - but no change to the line. */
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
    /* Flip to insert mode - swatch should refuse. */
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

static void test_variable_time_step_replay_policy(void) {
    UiHit hit = ui_hit_none();

    printf("--- variable-panel time step replay policy ---\n");
    hit.kind = UI_HIT_VARIABLE_TIME_STEP;
    hit.item_idx = 1;

    glr_ctrl_reset_all();
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glVertex3f(t, 0, 0);");
    editor_feed_line("glEnd();");
    repl_flatten_commands(editor_state_edit_line());
    repl_state_flat_program_clear_dirty();

    replay_start();
    ASSERT_TRUE("value-only t scene starts replay", replay_active());
    ASSERT_INT("value-only time step is consumed",
               glr_ctrl_router_handle_code_panel_hit(hit, 0, 0), 1);
    ASSERT_TRUE("value-only t step keeps replay active", replay_active());
    ASSERT_TRUE("value-only t step advances the clock",
                g_predef_vars[repl_state_variables().time_var_idx].value > 0.0f);
    replay_stop();

    glr_ctrl_reset_all();
    repl_state_time_set(1.0f);
    editor_feed_line("for(i, 0, t) {");
    editor_feed_line("glVertex3f(i, 0, 0);");
    editor_feed_line("}");
    repl_flatten_commands(editor_state_edit_line());
    repl_state_flat_program_clear_dirty();

    replay_start();
    ASSERT_TRUE("structural t scene starts replay", replay_active());
    ASSERT_INT("structural time step is consumed",
               glr_ctrl_router_handle_code_panel_hit(hit, 0, 0), 1);
    ASSERT_INT("structural t step stops replay", replay_active(), 0);
    ASSERT_STR("structural t step explains replay stop",
               ui_state_status_mut()->text,
               "Replay stopped: t changes this scene's structure");
}

/* scene_execute_adapter is called by render.c on both the main fill
 * pass and the render3d_probe_eye_dist feedback pass. The probe pass
 * runs every frame in ortho/projection-transition mode; before the
 * Render3dExecutePurpose wiring its execute_fn invocation mutated REPL
 * state (predef vars, scratch arrays, light enables) the same as the
 * main fill, so the user's `t = t + 1` style code advanced twice per
 * frame and the probe's glEnable(GL_LIGHT0) leaked across frames (the
 * frame-end restore in glr_ctrl_display_frame only snapshots predef +
 * scratch, not the persistent render state).
 *
 * This test exercises the adapter directly and pins the invariant:
 * DEPTH_PROBE plus the hidden/depth wireframe passes don't mutate
 * predef vars or scratch arrays; MAIN_FILL and the visible wireframe
 * pass still do (would-be-regression for accidentally suppressing the
 * real render path, including wireframe mode). The light-enable side
 * effect is excluded here for a clean signal, but the snapshot/restore
 * path covers it the same way it covers predef/scratch state. */
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

    /* Force a known starting state *before* re-flattening - the
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
 * glr_ctrl_reset_example_chrome reset the cfg field directly - bypassing
 * the render3d_lights_apply_theme hook in glr_config_set - so the lights[]
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

/* ---- Modal kind contract ------------------------------------------------
 *
 * A GlrModalKind is only usable when all three of its dispatches are wired:
 * glr_modal.c admits characters, this controller's snapshot builder formats
 * the prompt, and glr_actions.c commits. The switches are exhaustive under
 * -Werror=switch, which catches a missing *arm* - but not an arm that is
 * present and empty, which still produces a modal that captures the keyboard
 * with nothing on screen. This test is that second half: every kind renders a
 * prompt, follows its typing policy, and reaches its commit. */
typedef struct {
    GlrModalKind kind;
    int          accepts_typing;   /* 0 = confirmation prompt, not a field */
    unsigned char confirm_key;     /* key that commits it */
} ModalKindPolicy;

static const ModalKindPolicy k_modal_policies[] = {
    { GLR_MODAL_WORKSPACE_NEW,           1, '\r' },
    { GLR_MODAL_WORKSPACE_SAVE_AS,       1, '\r' },
    { GLR_MODAL_WORKSPACE_OPEN_PATH,     1, '\r' },
    { GLR_MODAL_SCENE_SAVE_AS,           1, '\r' },
    { GLR_MODAL_CONFIRM_DELETE_SCENE,    0, 'y'  },
    { GLR_MODAL_CONFIRM_WIP_RECOVER,     0, 'y'  },
};

static GlrModalKind g_modal_commit_kind;
static int g_modal_commit_calls;

static int test_modal_commit(GlrModalKind kind, const char *text, int context) {
    (void)text;
    (void)context;
    g_modal_commit_kind = kind;
    g_modal_commit_calls++;
    return 1;   /* accept, so the modal closes */
}

static const ModalKindPolicy *modal_policy_for(GlrModalKind kind) {
    for (int i = 0; i < (int)(sizeof(k_modal_policies) /
                              sizeof(k_modal_policies[0])); i++)
        if (k_modal_policies[i].kind == kind)
            return &k_modal_policies[i];
    return NULL;
}

static void test_every_modal_kind_is_wired(void) {
    printf("--- every modal kind has a prompt, a typing policy, a commit ---\n");

    ASSERT_INT("modal kind NONE does not open",
               glr_modal_begin(GLR_MODAL_NONE, "", 0, test_modal_commit), 0);
    ASSERT_INT("modal kind COUNT does not open",
               glr_modal_begin(GLR_MODAL_COUNT, "", 0, test_modal_commit), 0);
    ASSERT_INT("out-of-range modal kind does not open",
               glr_modal_begin((GlrModalKind)(GLR_MODAL_COUNT + 7), "", 0,
                               test_modal_commit), 0);
    ASSERT_INT("modal with no commit callback does not open",
               glr_modal_begin(GLR_MODAL_SCENE_SAVE_AS, "", 0, NULL), 0);
    ASSERT_TRUE("no modal is active after the refused opens",
                !glr_modal_active());

    for (int k = GLR_MODAL_NONE + 1; k < GLR_MODAL_COUNT; k++) {
        GlrModalKind kind = (GlrModalKind)k;
        const ModalKindPolicy *policy = modal_policy_for(kind);
        UiRenderSnapshot snap;
        char label[96];

        snprintf(label, sizeof(label),
                 "modal kind %d has a declared typing policy", k);
        ASSERT_TRUE(label, policy != NULL);
        if (!policy)
            continue;

        ASSERT_INT("modal opens", glr_modal_begin(kind, "seed", 0,
                                                  test_modal_commit), 1);

        glr_ctrl_build_ui_snapshot(&snap);
        snprintf(label, sizeof(label), "modal kind %d reports active", k);
        ASSERT_INT(label, snap.app_modal_active, 1);
        snprintf(label, sizeof(label), "modal kind %d renders a prompt", k);
        ASSERT_TRUE(label, snap.app_modal_message[0] != '\0');
        snprintf(label, sizeof(label), "modal kind %d offers a way out", k);
        ASSERT_TRUE(label, strstr(snap.app_modal_message, "[Esc]") != NULL);

        /* Typing policy: a name/path field takes printable characters; a
         * confirmation prompt takes none. */
        glr_modal_handle_key('a');
        snprintf(label, sizeof(label), "modal kind %d typing policy", k);
        ASSERT_INT(label, strcmp(glr_modal_text(), "seeda") == 0,
                   policy->accepts_typing);

        g_modal_commit_calls = 0;
        g_modal_commit_kind = GLR_MODAL_NONE;
        glr_modal_handle_key(policy->confirm_key);
        snprintf(label, sizeof(label), "modal kind %d reaches its commit", k);
        ASSERT_INT(label, g_modal_commit_calls, 1);
        snprintf(label, sizeof(label), "modal kind %d commits as itself", k);
        ASSERT_INT(label, (int)g_modal_commit_kind, k);
        snprintf(label, sizeof(label), "modal kind %d closes on commit", k);
        ASSERT_INT(label, glr_modal_active(), 0);
    }

    glr_modal_cancel();
}

static const GlrConfigItem *cfg_item_for_slug(const char *slug) {
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        const GlrConfigItem *item = glr_config_item_at(i);
        const char *item_slug = glr_config_item_slug(item);
        if (item_slug && strcmp(item_slug, slug) == 0)
            return item;
    }
    return NULL;
}

/* Bag values are symbolic where the key has a symbol table ("GRID_THEME_OFF")
 * and decimal otherwise. */
static int cfg_bag_value_int(const char *slug, const char *text) {
    int value = 0;
    if (repl_cfg_resolve_text(slug, text, &value))
        return value;
    return atoi(text);
}

/* The scene-local config contract, end to end. `k_cfg_scene_defaults[]` in
 * glr_actions.c is the roster of settings a *scene* owns; this reset is its
 * other half - the direct GlrPresentationState writes in
 * glr_state_presentation_reset_example_defaults() plus the camera-autorotate
 * and variable-panel peer writes here. A key added to the roster but missed in
 * the reset leaks across an F12 example switch (the next scene inherits the
 * previous scene's grid, backdrop, view mode, ...), which is exactly the
 * failure no compiler and no single-table guard can see.
 *
 * So: drive every roster key off its default through the real setter, then run
 * the example-load chrome reset and require all of them back at the default
 * the .glr writer diffs against. */
static void test_scene_local_reset_covers_whole_roster(void) {
    printf("--- example chrome reset covers the whole scene-local roster ---\n");

    glr_actions_install_export_cfg_bridge();
    const ReplConfigBridge *bridge = repl_config_bridge();
    ASSERT_TRUE("cfg bridge exposes the scene defaults",
                bridge && bridge->fill_scene_defaults && bridge->apply);
    if (!bridge || !bridge->fill_scene_defaults || !bridge->apply)
        return;

    ReplConfigBag defaults;
    repl_config_bag_clear(&defaults);
    bridge->fill_scene_defaults(&defaults);
    ASSERT_TRUE("scene-local roster is non-empty", defaults.count > 0);

    /* Start from the defaults so the mutation pass below is deterministic
     * (notably: a paired backdrop would otherwise pin grid_theme). */
    bridge->apply(&defaults);

    for (int i = 0; i < defaults.count; i++) {
        const char *slug = defaults.items[i].key;
        int def = cfg_bag_value_int(slug, defaults.items[i].value);
        const GlrConfigItem *item = cfg_item_for_slug(slug);
        char label[REPL_CFG_KEY_MAX + 64];
        int moved = 0;

        snprintf(label, sizeof(label), "%s has a descriptor row", slug);
        ASSERT_TRUE(label, item != NULL);
        if (!item)
            continue;

        for (int v = 0; v < item->state_count && !moved; v++) {
            if (v == def)
                continue;
            /* A backdrop that owns a grid would drag grid_theme along with
             * it; keep the two keys independently mutated. */
            if (item->key == GLR_CONFIG_BACKDROP &&
                glr_config_backdrop_forces_grid((Render3dBackdropMode)v, NULL))
                continue;
            glr_config_set(item->key, v);
            moved = glr_config_get(item->key) != def;
        }

        snprintf(label, sizeof(label), "%s moved off its default", slug);
        ASSERT_TRUE(label, moved);
    }

    /* Mask 0 applies no example tag defaults, so the built-in baseline is
     * the whole expectation. */
    glr_ctrl_reset_example_chrome(0u);

    for (int i = 0; i < defaults.count; i++) {
        const char *slug = defaults.items[i].key;
        const GlrConfigItem *item = cfg_item_for_slug(slug);
        char label[REPL_CFG_KEY_MAX + 64];

        if (!item)
            continue;
        snprintf(label, sizeof(label), "%s reset to its scene default", slug);
        ASSERT_INT(label, glr_config_get(item->key),
                   cfg_bag_value_int(slug, defaults.items[i].value));
    }
}

/* Render3dRenderConfig.lights[] is assembled per frame
 * from two owners - the app-owned theme-seeded dimensional data
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

/* The exporter is render3d/app-free and reads the dimensional light data through
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

/* Drive the active tutorial to completion. A lesson mixes commit steps with
 * showcase steps (NOTE / SET), which carry no expected text and advance on an
 * ack key instead - so a caller that only commits expected text stalls on the
 * first note. */
static void complete_active_tutorial(const char *label) {
    int guard = 0;
    while (tutorial_active() && guard++ < 256) {
        TutorialMatchResult match;
        const char *expected;
        TutorialStepKind kind = tutorial_current_step_kind();
        if (kind == TUTORIAL_STEP_KIND_NOTE || kind == TUTORIAL_STEP_KIND_SET) {
            ASSERT_TRUE(label, tutorial_handle_ack_key('\r') == 1);
            continue;
        }
        expected = tutorial_current_expected_text();
        ASSERT_TRUE(label, expected != NULL);
        ASSERT_TRUE(label, tutorial_handle_commit_attempt(expected, &match));
        tutorial_advance_after_successful_commit();
    }
    ASSERT_TRUE(label, !tutorial_active());
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
#if defined(__EMSCRIPTEN__)
    /* menu_visible() hides MENU_FILE on the web build, so routing a hit at its
     * button stays inert rather than opening a menu the shell replaced. */
    ASSERT_INT("File menu stays closed on the web build",
               ui_menu_bar_open_menu_id(), -1);
#else
    ASSERT_INT("menu opened", ui_menu_bar_open_menu_id(), GLR_MENU_FILE);
#endif

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
    glr_ctrl_reshape(1000, 620);
    glr_ctrl_set_depth_readback_supported_for_test(1);
    glr_ctrl_set_depth_snapshot_wanted(1);
    glr_ctrl_capture_depth_snapshot();
    ASSERT_INT("depth valid before submenu tutorial start",
               glr_ctrl_depth_snapshot_view().valid, 1);
    rc = route_submenu_item_hit(&hit);
    ASSERT_INT("submenu tutorial item consumed", rc, 1);
    ASSERT_INT("submenu tutorial start drops outgoing document depth",
               glr_ctrl_depth_snapshot_view().valid, 0);

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

    /* Prev / Next steppers: the example / user-scene cycle with no tutorial
     * running, the lesson cycle while one is active. */
    {
        int count = repl_example_count();
        tutorial_stop();
        repl_load_example(0);
        hit.kind = UI_HIT_PIN_BUTTON;
        hit.item_idx = UI_MENU_BAR_PIN_NEXT;
        rc = route_pin_button_hit(&hit);
        ASSERT_INT("next pin hit consumed", rc, 1);
        ASSERT_INT("next pin steps the example catalog",
                   repl_state_scenes().active_example_idx, count > 1 ? 1 : 0);

        /* The stepper tooltip predicts each destination by name; on the
         * first example the back leg has no earlier entry, so it reports
         * where the cycle actually wraps to (a user scene if one exists,
         * else the last example). */
        {
            GlrCycleTarget fwd, back;
            repl_load_example(0);
            fwd = glr_ctrl_cycle_peek(1);
            back = glr_ctrl_cycle_peek(-1);
            if (count > 1) {
                ASSERT_INT("peek forward reports an example",
                           fwd.kind, GLR_CYCLE_TARGET_EXAMPLE);
                ASSERT_STR("peek forward names the next example",
                           fwd.name ? fwd.name : "", repl_example_name(1));
            }
            ASSERT_TRUE("peek backward from the first example has a target",
                        back.kind != GLR_CYCLE_TARGET_NONE);
            ASSERT_INT("peek of a zero direction is empty",
                       glr_ctrl_cycle_peek(0).kind, GLR_CYCLE_TARGET_NONE);
        }

        hit.item_idx = UI_MENU_BAR_PIN_PREV;
        repl_load_example(count > 1 ? 1 : 0);
        rc = route_pin_button_hit(&hit);
        ASSERT_INT("prev pin hit consumed", rc, 1);
        ASSERT_INT("prev pin steps back to the first example",
                   repl_state_scenes().active_example_idx, 0);
    }
    if (repl_tutorial_count() > 1) {
        tutorial_start(0);
        ASSERT_TRUE("tutorial active for stepper test", tutorial_active());
        hit.item_idx = UI_MENU_BAR_PIN_NEXT;
        rc = route_pin_button_hit(&hit);
        ASSERT_INT("next pin hit consumed under tutorial", rc, 1);
        ASSERT_INT("next pin steps the lesson catalog",
                   tutorial_state_view().tutorial_idx, 1);

        hit.item_idx = UI_MENU_BAR_PIN_PREV;
        rc = route_pin_button_hit(&hit);
        ASSERT_INT("prev pin hit consumed under tutorial", rc, 1);
        /* The tooltip's prediction follows the same rule the click does. */
        {
            GlrCycleTarget peek = glr_ctrl_cycle_peek(1);
            ASSERT_INT("peek under tutorial reports a lesson",
                       peek.kind, GLR_CYCLE_TARGET_TUTORIAL);
            ASSERT_STR("peek names the lesson the click lands on",
                       peek.name ? peek.name : "", repl_tutorial_name(1));
        }
        ASSERT_INT("prev pin steps back to the first lesson",
                   tutorial_state_view().tutorial_idx, 0);
        tutorial_stop();

        /* A completed tutorial is inactive but retains its index so F11 can
         * continue the lesson catalog. The mouse steppers must make the same
         * choice, including both directions. */
        tutorial_start(0);
        complete_active_tutorial("completed-pin setup drives the lesson");
        ASSERT_TRUE("tutorial inactive after completed-pin setup",
                    !tutorial_active());
        ASSERT_INT("completed-pin setup retains lesson index",
                   tutorial_state_view().tutorial_idx, 0);
        {
            GlrCycleTarget next_peek = glr_ctrl_cycle_peek(1);
            GlrCycleTarget prev_peek = glr_ctrl_cycle_peek(-1);
            ASSERT_INT("completed-pin next peek is a lesson",
                       next_peek.kind, GLR_CYCLE_TARGET_TUTORIAL);
            ASSERT_INT("completed-pin prev peek is a lesson",
                       prev_peek.kind, GLR_CYCLE_TARGET_TUTORIAL);
        }
        hit.item_idx = UI_MENU_BAR_PIN_NEXT;
        rc = route_pin_button_hit(&hit);
        ASSERT_INT("next pin after completion hit consumed", rc, 1);
        ASSERT_INT("next pin after completion steps lesson",
                   tutorial_state_view().tutorial_idx, 1);
        tutorial_stop();

        /* Re-complete lesson 0 to exercise the opposite direction from the
         * same inactive-but-retained state. */
        tutorial_start(0);
        complete_active_tutorial("completed-prev setup drives the lesson");
        ASSERT_TRUE("tutorial inactive before completed prev pin",
                    !tutorial_active());
        hit.item_idx = UI_MENU_BAR_PIN_PREV;
        rc = route_pin_button_hit(&hit);
        ASSERT_INT("prev pin after completion hit consumed", rc, 1);
        ASSERT_INT("prev pin after completion steps lesson",
                   tutorial_state_view().tutorial_idx,
                   repl_tutorial_count() - 1);
        tutorial_stop();
    }

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
 * behind two baked enum tokens - so both the read (swatch color) and the
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

static void test_func_nav_special_beats_replay_and_help(void) {
    printf("--- imrepl_ctrl Ctrl+Up/Down function nav beats replay/help ---\n");
    float speed_before;
    int scroll_before;

    glr_ctrl_reset_all();
    editor_feed_line("func0() {");
    editor_feed_line("  glColor3f(1,0,0);");
    editor_feed_line("}");
    editor_feed_line("func1() {");
    editor_feed_line("  glColor3f(0,1,0);");
    editor_feed_line("}");
    ASSERT_INT("func-nav router setup: 6 rows", repl_state_document_count(), 6);
    ASSERT_INT("func-nav router setup: row 0 is func0",
               repl_state_document_cmds()[0].type, CMD_FUNC_DEF);

    editor_input_set_modifier_provider_for_test(simulated_mods_provider);
    g_simulated_mods = GLUT_ACTIVE_CTRL;
    editor_navigate_to_line(0);

    replay_state_mut()->active = 1;
    replay_state_mut()->state = REPLAY_PLAYING;
    replay_state_mut()->speed = 1.0f;
    speed_before = replay_state_mut()->speed;
    glr_ctrl_special(GLUT_KEY_DOWN, 0, 0);
    ASSERT_INT("ctrl-down during replay jumps to next func",
               editor_state_edit_line(), 3);
    ASSERT_TRUE("ctrl-down during replay leaves speed unchanged",
                replay_state_mut()->speed == speed_before);
    replay_state_mut()->active = 0;
    replay_state_mut()->state = REPLAY_OFF;

    editor_navigate_to_line(0);
    ui_state_help_mut()->visible = 1;
    editor_help_session_set_scroll(10);
    scroll_before = editor_help_session_scroll();
    glr_ctrl_special(GLUT_KEY_DOWN, 0, 0);
    ASSERT_INT("ctrl-down with help open jumps to next func",
               editor_state_edit_line(), 3);
    ASSERT_INT("ctrl-down with help open leaves help scroll",
               editor_help_session_scroll(), scroll_before);

    g_simulated_mods = 0;
    editor_navigate_to_line(0);
    glr_ctrl_special(GLUT_KEY_DOWN, 0, 0);
    ASSERT_INT("plain down with help open still scrolls help, not func nav",
               editor_state_edit_line(), 0);
    ASSERT_TRUE("plain down with help open scrolled help",
                editor_help_session_scroll() != scroll_before);

    ui_state_help_mut()->visible = 0;
    editor_input_set_modifier_provider_for_test(NULL);
}

static unsigned int s_ctrl_click_clock_ms = 1000;
static unsigned int ctrl_click_clock_provider(void) {
    s_ctrl_click_clock_ms += 1000;
    return s_ctrl_click_clock_ms;
}

static void test_ctrl_click_go_to_func_def(void) {
    printf("--- imrepl_ctrl Ctrl+Click go to function definition ---\n");
    UiHit hit;
    memset(&hit, 0, sizeof(hit));
    hit.kind = UI_HIT_CODE_TEXT;

    glr_ctrl_reset_all();
    s_ctrl_click_clock_ms = 1000;
    glr_ctrl_router_set_double_click_clock_for_test(ctrl_click_clock_provider);
    repl_func_alias_clear_all();
    repl_func_alias_set(1, "my_star");
    editor_feed_line("func0() {");            /* 0: func0 def */
    editor_feed_line("  glColor3f(1,0,0);");  /* 1: func0 body */
    editor_feed_line("}");                    /* 2: func0 end */
    editor_feed_line("func1() {");            /* 3: func1 def (alias: my_star) */
    editor_feed_line("  glColor3f(0,1,0);");  /* 4: func1 body */
    editor_feed_line("}");                    /* 5: func1 end */
    editor_feed_line("func0();");             /* 6: standalone call to func0 */
    editor_feed_line("my_star();");           /* 7: aliased call to func1 */
    editor_feed_line("glVertex3f(0, 0, 0); // func2 callsite"); /* 8: comment call to func2 */
    editor_feed_line("glVertex3f(0, 0, 0); // func0 callsite"); /* 9: comment call to func0 */

    ASSERT_INT("document has 10 lines", repl_state_document_count(), 10);

    editor_input_set_modifier_provider_for_test(simulated_mods_provider);

    /* 1. Standalone CMD_CALL: Ctrl+Click on func0(); -> lands on line 0 */
    g_simulated_mods = GLUT_ACTIVE_CTRL;
    hit.line_idx = 6; hit.char_idx = 2;
    int consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("ctrl-click func0() consumed", consumed, 1);
    ASSERT_INT("ctrl-click func0() lands on line 0", editor_state_edit_line(), 0);
    ASSERT_INT("scroll follow cursor armed after func jump", editor_scroll_follow_cursor(), 1);
    ASSERT_INT("drag disarmed after jump", g_code_panel_drag_active, 0);

    /* 2. macOS Cmd (GLUT_ACTIVE_SUPER) normalization */
    g_simulated_mods = GLUT_ACTIVE_SUPER;
    hit.line_idx = 6; hit.char_idx = 2;
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("cmd-click func0() consumed", consumed, 1);
    ASSERT_INT("cmd-click func0() lands on line 0", editor_state_edit_line(), 0);

    /* 3. Aliased call: Ctrl+Click on my_star(); -> lands on line 3 */
    g_simulated_mods = GLUT_ACTIVE_CTRL;
    hit.line_idx = 7; hit.char_idx = 3;
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("ctrl-click my_star() consumed", consumed, 1);
    ASSERT_INT("ctrl-click my_star() lands on line 3", editor_state_edit_line(), 3);
    ASSERT_INT("drag disarmed after my_star jump", g_code_panel_drag_active, 0);

    /* 4. Search and Autocomplete cleanup on successful navigation */
    editor_state_search_mut()->active = 1;
    editor_state_search_mut()->query_len = 3;
    editor_state_autocomplete_mut()->match_count = 2;
    hit.line_idx = 6; hit.char_idx = 2;
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("ctrl-click with active search/ac consumed", consumed, 1);
    ASSERT_INT("search dismissed on jump", editor_state_search()->active, 0);
    ASSERT_INT("search query cleared on jump", editor_state_search()->query_len, 0);
    ASSERT_INT("autocomplete dismissed on jump", editor_state_autocomplete()->match_count, 0);

    /* 5. Call inside comment: Ctrl+Click on func0 in // func0 callsite -> lands on line 0 */
    hit.line_idx = 9; hit.char_idx = 26;
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("ctrl-click func0 in comment consumed", consumed, 1);
    ASSERT_INT("ctrl-click func0 in comment lands on line 0", editor_state_edit_line(), 0);

    /* 6. Ctrl+Shift precedence: Shift wins, triggers selection, does NOT jump */
    editor_navigate_to_line(8);
    g_simulated_mods = GLUT_ACTIVE_CTRL | GLUT_ACTIVE_SHIFT;
    hit.line_idx = 6; hit.char_idx = 2;
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("ctrl+shift+click consumed by selection route", consumed, 1);
    ASSERT_TRUE("ctrl+shift+click active selection", editor_clipboard_sel_active());
    ASSERT_INT("ctrl+shift+click anchor line", editor_state_selection_anchor(), 8);
    ASSERT_INT("ctrl+shift+click navigated to clicked line 6", editor_state_edit_line(), 6);
    editor_selection_clear_line_range();

    /* 7. Invalid hit positions fall back to ordinary click */
    g_simulated_mods = GLUT_ACTIVE_CTRL;
    hit.line_idx = 6; hit.char_idx = -1;
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("invalid char_idx falls back to ordinary click (consumed)", consumed, 1);
    ASSERT_INT("edit line stays at 6", editor_state_edit_line(), 6);

    hit.line_idx = -1; hit.char_idx = 2;
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("negative line_idx falls back to ordinary click", consumed, 1);

    hit.line_idx = 999; hit.char_idx = 2;
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("out-of-bounds line_idx falls back to ordinary click", consumed, 1);

    /* 8. Click on definition line itself: consumed, drag disarmed */
    editor_navigate_to_line(0);
    hit.line_idx = 0; hit.char_idx = 2;
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("ctrl-click on definition line consumed", consumed, 1);
    ASSERT_INT("stays on line 0", editor_state_edit_line(), 0);
    ASSERT_INT("drag disarmed on self click", g_code_panel_drag_active, 0);

    /* 9. Function definition created by pending edit:
     * User types func2() { on row 9, Ctrl-clicks func2 callsite at row 8.
     * Preflight recognizes pending func2 definition -> commits -> jumps to new definition! */
    editor_navigate_to_line(9);
    editor_input_set_text("func2() {");
    ASSERT_TRUE("dirty edit is pending", editor_input_has_uncommitted_change());
    ASSERT_INT("func2 not yet in AST", repl_source_scope_find_func_def_line(2), -1);
    hit.line_idx = 8; hit.char_idx = 26; /* "func2" in comment on line 8 */
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("ctrl-click on func2 callsite while defining func2 consumed", consumed, 1);
    int func2_def = repl_source_scope_find_func_def_line(2);
    ASSERT_TRUE("func2 def created in AST", func2_def >= 0);
    ASSERT_INT("navigated to newly created func2 definition", editor_state_edit_line(), func2_def);

    /* 9b. Definition removed by pending edit:
     * User renames my_star() { (slot 1 at line 3) into func3() { (slot 3).
     * Ctrl-clicking my_star(); callsite at line 10 auto-commits the rename,
     * finds slot 1 gone post-commit, consumes the event, and safely keeps
     * cursor on the committed edit line without misnavigating to stale row. */
    editor_navigate_to_line(3);
    editor_load_line_to_input(3);
    editor_input_set_text("func3() {");
    ASSERT_TRUE("rename edit is pending", editor_input_has_uncommitted_change());
    hit.line_idx = 10; hit.char_idx = 3; /* my_star(); callsite at line 10 */
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("ctrl-click with removed definition consumed", consumed, 1);
    ASSERT_INT("my_star slot 1 no longer exists post-commit", repl_source_scope_find_func_def_line(1), -1);
    ASSERT_TRUE("func3 slot 3 created post-commit", repl_source_scope_find_func_def_line(3) >= 0);
    ASSERT_INT("cursor advances to func3 body line 4", editor_state_edit_line(), 4);
    editor_load_line_to_input(editor_state_edit_line());

    /* 10. Undefined function does NOT commit pending edit when clicked on same row */
    editor_navigate_to_line(repl_state_document_count());
    editor_feed_line("// func5 func10 12345 { ; }"); /* test row at end of doc */
    int test_row = repl_state_document_count() - 1;

    editor_navigate_to_line(test_row);
    editor_load_line_to_input(test_row);
    editor_input_set_text("// func5 dirty edit");
    ASSERT_TRUE("dirty edit pending at test_row", editor_input_has_uncommitted_change());
    hit.line_idx = test_row; hit.char_idx = 4; /* "func5" on current edit line */
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("undefined func5 falls back to ordinary click", consumed, 1);
    ASSERT_TRUE("pending edit on current line was NOT committed", editor_input_has_uncommitted_change());
    ASSERT_STR("pending edit text preserved", editor_state_input().input, "// func5 dirty edit");
    editor_load_line_to_input(test_row); /* restore clean line */

    /* 10b. A CALL to an undefined slot typed in the input row is not a
     * definition. The preflight must not mistake it for one: committing
     * would be rejected and the rollback discards the typed row, so a
     * Ctrl+click would destroy work an ordinary same-row click preserves.
     *
     * All three parenthesized call forms, because they fail the definition
     * test for two different reasons and only one is caught by the
     * signature grammar: `func6(1)` has a non-identifier parameter list, but
     * `func6()` and `func6(a)` parse as brace-less signatures and are
     * separated from a definition only by the absent `{`
     * (repl_text_is_func_call_shaped). */
    {
        const char *call_forms[3] = { "func6(1)", "func6()", "func6(a)" };
        for (int ci = 0; ci < 3; ci++) {
            editor_navigate_to_line(test_row);
            editor_load_line_to_input(test_row);
            editor_input_set_text(call_forms[ci]);
            ASSERT_TRUE("call-in-progress pending at test_row",
                        editor_input_has_uncommitted_change());
            ASSERT_INT("func6 undefined before the click",
                       repl_source_scope_find_func_def_line(6), -1);
            hit.line_idx = test_row; hit.char_idx = 1; /* "func6" in the input row */
            consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
            ASSERT_INT("call to undefined func falls back to ordinary click",
                       consumed, 1);
            ASSERT_TRUE("call-in-progress was NOT committed",
                        editor_input_has_uncommitted_change());
            ASSERT_STR("call-in-progress text preserved",
                       editor_state_input().input, call_forms[ci]);
            ASSERT_INT("still no func6 definition",
                       repl_source_scope_find_func_def_line(6), -1);
            ASSERT_INT("document unchanged by refused preflight",
                       repl_state_document_count(), test_row + 1);
            editor_load_line_to_input(test_row); /* restore clean line */
        }
    }

    /* 10c. The definition form of the same slot still arms the preflight -
     * the same text as the refused `func6(a)` above, plus the `{` that is
     * what actually makes it a definition. */
    editor_navigate_to_line(test_row);
    editor_load_line_to_input(test_row);
    editor_input_set_text("func6(a) {");
    hit.line_idx = test_row; hit.char_idx = 1;
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("definition-in-progress consumes the click", consumed, 1);
    ASSERT_TRUE("func6 definition committed",
                repl_source_scope_find_func_def_line(6) >= 0);
    editor_load_line_to_input(editor_state_edit_line());
    test_row = repl_state_document_count() - 1;

    /* 11. Individual non-function tokens fall back to ordinary click */
    /* 11a. func10 (not a slot 0..9) */
    editor_navigate_to_line(0);
    editor_load_line_to_input(0);
    glr_ctrl_router_reset_code_panel_drag();
    hit.line_idx = test_row; hit.char_idx = 10; /* "func10" */
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("func10 falls back to ordinary click", consumed, 1);
    ASSERT_INT("navigated to test_row", editor_state_edit_line(), test_row);
    editor_load_line_to_input(editor_state_edit_line());

    /* 11b. Number token "12345" */
    editor_navigate_to_line(0);
    editor_load_line_to_input(0);
    glr_ctrl_router_reset_code_panel_drag();
    hit.line_idx = test_row; hit.char_idx = 18; /* "12345" */
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("number token falls back to ordinary click", consumed, 1);
    ASSERT_INT("navigated to test_row", editor_state_edit_line(), test_row);
    editor_load_line_to_input(editor_state_edit_line());

    /* 11c. Punctuation "{" */
    editor_navigate_to_line(0);
    editor_load_line_to_input(0);
    glr_ctrl_router_reset_code_panel_drag();
    hit.line_idx = test_row; hit.char_idx = 22; /* "{" */
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("punctuation '{' falls back to ordinary click", consumed, 1);
    ASSERT_INT("navigated to test_row", editor_state_edit_line(), test_row);
    editor_load_line_to_input(editor_state_edit_line());

    /* 11d. Punctuation ";" */
    editor_navigate_to_line(0);
    editor_load_line_to_input(0);
    glr_ctrl_router_reset_code_panel_drag();
    hit.line_idx = test_row; hit.char_idx = 24; /* ";" */
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("punctuation ';' falls back to ordinary click", consumed, 1);
    ASSERT_INT("navigated to test_row", editor_state_edit_line(), test_row);
    editor_load_line_to_input(editor_state_edit_line());

    /* 11e. Whitespace */
    editor_navigate_to_line(0);
    editor_load_line_to_input(0);
    glr_ctrl_router_reset_code_panel_drag();
    hit.line_idx = test_row; hit.char_idx = 2; /* whitespace */
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("whitespace falls back to ordinary click", consumed, 1);
    ASSERT_INT("navigated to test_row", editor_state_edit_line(), test_row);
    editor_load_line_to_input(editor_state_edit_line());

    /* 12. Pending edit rejection preserves error status and aborts navigation */
    g_simulated_mods = GLUT_ACTIVE_CTRL;
    editor_navigate_to_line(0);
    editor_load_line_to_input(0);
    editor_input_set_text("func0(bad syntax !!!");
    EditorAutocompleteState *ac = editor_state_autocomplete_mut();
    ac->match_count = 3;
    glr_ctrl_router_reset_code_panel_drag();
    hit.line_idx = 10; hit.char_idx = 2; /* func0(); callsite at line 10 */
    consumed = glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
    ASSERT_INT("rejected edit consumes event", consumed, 1);
    ASSERT_INT("rejected edit keeps cursor on dirty line 0", editor_state_edit_line(), 0);
    ASSERT_INT("autocomplete cleared on commit rejection", editor_state_autocomplete()->match_count, 0);
    editor_load_line_to_input(0); /* restore clean line */

    g_simulated_mods = 0;
    glr_ctrl_router_set_double_click_clock_for_test(NULL);
    editor_input_set_modifier_provider_for_test(NULL);
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
        complete_active_tutorial("completed-cycle setup drives the lesson");
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

static int write_cycle_fixture(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    if (!file)
        return 0;
    if (fputs(text, file) == EOF) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static void test_scene_cycle_skips_failed_examples(void) {
    char root_template[] = "/tmp/glr_ctrl_cycle_XXXXXX";
    char *root = mkdtemp(root_template);
    char scenes_dir[512];
    char catalog_path[512];
    char good0_path[512];
    char bad_path[512];
    char good2_path[512];
    char error[REPL_DIAG_TEXT_MAX];
    int catalog_loaded = 0;

    printf("--- imrepl_ctrl scene cycle skips failed examples ---\n");
    ASSERT_TRUE("cycle fixture directory created", root != NULL);
    if (!root)
        return;

    snprintf(scenes_dir, sizeof(scenes_dir), "%s/scenes", root);
    snprintf(catalog_path, sizeof(catalog_path), "%s/catalog.ini", root);
    snprintf(good0_path, sizeof(good0_path), "%s/good-0.glr", scenes_dir);
    snprintf(bad_path, sizeof(bad_path), "%s/bad-1.glr", scenes_dir);
    snprintf(good2_path, sizeof(good2_path), "%s/good-2.glr", scenes_dir);

    ASSERT_INT("cycle fixture scenes directory created",
               mkdir(scenes_dir, 0700), 0);
    ASSERT_TRUE("cycle fixture first example written",
                write_cycle_fixture(good0_path,
                                    "glBegin(GL_POINTS);\n"
                                    "glVertex3f(0, 0, 0);\n"
                                    "glEnd();\n"));
    ASSERT_TRUE("cycle fixture broken example written",
                write_cycle_fixture(bad_path,
                                    "glBegin(GL_POINTS);\n"
                                    "notACommand(1, 2, 3);\n"
                                    "glEnd();\n"));
    ASSERT_TRUE("cycle fixture last example written",
                write_cycle_fixture(good2_path,
                                    "glBegin(GL_LINES);\n"
                                    "glVertex3f(0, 0, 0);\n"
                                    "glVertex3f(1, 0, 0);\n"
                                    "glEnd();\n"));
    ASSERT_TRUE("cycle fixture catalog written",
                write_cycle_fixture(catalog_path,
                                    "[good-0]\n"
                                    "file = scenes/good-0.glr\n"
                                    "name = Cycle good 0\n"
                                    "tags = 2D\n"
                                    "group = Cycle\n\n"
                                    "[bad-1]\n"
                                    "file = scenes/bad-1.glr\n"
                                    "name = Cycle broken 1\n"
                                    "tags = 2D\n"
                                    "group = Cycle\n\n"
                                    "[good-2]\n"
                                    "file = scenes/good-2.glr\n"
                                    "name = Cycle good 2\n"
                                    "tags = 2D\n"
                                    "group = Cycle\n"));

    error[0] = '\0';
    catalog_loaded = repl_examples_load_dir(root, error, sizeof(error));
    ASSERT_TRUE("cycle fixture catalog loads", catalog_loaded);
    if (catalog_loaded) {
        glr_ctrl_reset_all();
        ASSERT_INT("cycle fixture has three examples", repl_example_count(), 3);
        ASSERT_TRUE("cycle fixture starts at first example",
                    repl_load_example(0) > 0);

        glr_ctrl_scene_cycle_next();
        ASSERT_INT("F12 skips broken middle example",
                   repl_state_scenes().active_example_idx, 2);
        ASSERT_TRUE("F12 reports skipped example",
                    strstr(ui_state_status().text,
                           "skipped 1 unavailable example") != NULL);
        ASSERT_TRUE("F12 reports loaded destination",
                    strstr(ui_state_status().text, "Cycle good 2") != NULL);
        ASSERT_INT("successful fallback is informational",
                   ui_state_status().kind, UI_STATUS_INFO);

        glr_ctrl_scene_cycle_prev();
        ASSERT_INT("Shift+F12 skips broken middle example",
                   repl_state_scenes().active_example_idx, 0);
        ASSERT_TRUE("Shift+F12 reports skipped example",
                    strstr(ui_state_status().text,
                           "skipped 1 unavailable example") != NULL);

        ASSERT_TRUE("all-fail catalog rewrite succeeds",
                    write_cycle_fixture(catalog_path,
                                        "[good-0]\n"
                                        "file = scenes/good-0.glr\n"
                                        "name = Cycle good 0\n"
                                        "tags = 2D\n"
                                        "group = Cycle\n\n"
                                        "[bad-1]\n"
                                        "file = scenes/bad-1.glr\n"
                                        "name = Cycle broken 1\n"
                                        "tags = 2D\n"
                                        "group = Cycle\n"));
        error[0] = '\0';
        ASSERT_TRUE("all-fail catalog reloads",
                    repl_examples_load_dir(root, error, sizeof(error)));
        glr_ctrl_scene_cycle_next();
        ASSERT_INT("all-fail cycle preserves active example",
                   repl_state_scenes().active_example_idx, 0);
        ASSERT_INT("all-fail cycle leaves an error status",
                   ui_state_status().kind, UI_STATUS_ERROR);
        ASSERT_TRUE("all-fail cycle explains the failure",
                    strstr(ui_state_status().text, "F12 cycle failed") != NULL);

        ASSERT_TRUE("user-origin broken example rewrite succeeds",
                    write_cycle_fixture(good0_path,
                                        "glBegin(GL_POINTS);\n"
                                        "notACommand(1, 2, 3);\n"
                                        "glEnd();\n"));
        error[0] = '\0';
        ASSERT_TRUE("user-origin all-fail catalog reloads",
                    repl_examples_load_dir(root, error, sizeof(error)));
        glr_ctrl_reset_all();
        int user_slot = repl_scenes_create_empty_user_scene();
        ASSERT_TRUE("user-origin scene created", user_slot >= 0);
        int expected_user_rows = repl_state_document_count() + 4;
        editor_feed_line("glVertex3f(0, 0, 0);");
        editor_feed_line("glVertex3f(1, 0, 0);");
        editor_feed_line("glVertex3f(0, 1, 0);");
        editor_feed_line("glVertex3f(1, 1, 0);");
        ASSERT_INT("user-origin scene has expected rows",
                   repl_state_document_count(), expected_user_rows);

        glr_ctrl_scene_cycle_prev();
        ASSERT_INT("all-fail user-origin cycle restores active slot",
                   repl_active_user_scene(), user_slot);
        ASSERT_INT("all-fail user-origin cycle preserves rows",
                   repl_state_document_count(), expected_user_rows);
        ASSERT_INT("all-fail user-origin cycle remains an error",
                   ui_state_status().kind, UI_STATUS_ERROR);
        repl_scenes_detach_active_user_scene();
        ASSERT_TRUE("preserved user slot reloads after detaching",
                    repl_load_user_scene_idx(user_slot));
        ASSERT_INT("reloaded user slot still has expected rows",
                   repl_state_document_count(), expected_user_rows);

        glr_ctrl_reset_all();
        glr_ctrl_scene_cycle_next();
        ASSERT_TRUE("transient all-fail cycle scans each entry once",
                    strstr(ui_state_status().text,
                           "skipped 2 unavailable examples") != NULL);
        ASSERT_TRUE("transient all-fail cycle does not double-scan",
                    strstr(ui_state_status().text,
                           "skipped 4 unavailable examples") == NULL);
    }

    repl_examples_clear_runtime_catalog();
    unlink(good0_path);
    unlink(bad_path);
    unlink(good2_path);
    unlink(catalog_path);
    rmdir(scenes_dir);
    rmdir(root);
    glr_ctrl_reset_all();
}

/* Promotion must not cost the user their place in the example catalog.
 * Editing a viewed example promotes it into a user-scene slot, which ends the
 * example tab (active_example_idx goes to -1) - and the F12 leg that walks out
 * of the user scenes used to restart the catalog at example 1 as a result. The
 * parked place is the example twin of a completed tutorial's retained index. */
static void test_promotion_keeps_example_catalog_place(void) {
    int count;

    printf("--- imrepl_ctrl promotion keeps the example catalog place ---\n");

    glr_ctrl_reset_all();
    count = repl_example_count();
    ASSERT_TRUE("catalog has at least three examples", count >= 3);
    if (count < 3)
        return;

    ASSERT_TRUE("origin example loads", repl_load_example(1) > 0);
    ASSERT_TRUE("example promoted to a slot",
                repl_promote_transient_if_needed() >= 0);
    ASSERT_INT("promotion ends the example tab",
               repl_state_scenes().active_example_idx, -1);
    ASSERT_INT("promotion parks the catalog place",
               repl_state_scenes().example_place_idx, 1);

    glr_ctrl_scene_cycle_next();
    ASSERT_INT("F12 after promotion continues past the origin example",
               repl_state_scenes().active_example_idx, 2);
    ASSERT_INT("loading an example drops the parked place",
               repl_state_scenes().example_place_idx, -1);

    /* Shift+F12 resumes in its own direction from the same parked place. */
    glr_ctrl_reset_all();
    ASSERT_TRUE("origin example loads for the reverse leg",
                repl_load_example(1) > 0);
    ASSERT_TRUE("example promoted for the reverse leg",
                repl_promote_transient_if_needed() >= 0);
    glr_ctrl_scene_cycle_prev();
    ASSERT_INT("Shift+F12 after promotion steps back from the origin example",
               repl_state_scenes().active_example_idx, 0);

    /* Scope pin: only a promotion parks a place. A user scene that never came
     * out of the catalog still enters it at the end the direction implies. */
    glr_ctrl_reset_all();
    ASSERT_TRUE("standalone user scene created",
                repl_scenes_create_empty_user_scene() >= 0);
    ASSERT_INT("a scene with no example origin parks nothing",
               repl_state_scenes().example_place_idx, -1);
    glr_ctrl_scene_cycle_next();
    ASSERT_INT("F12 from an unpromoted scene starts the catalog at example 0",
               repl_state_scenes().active_example_idx, 0);

    glr_ctrl_reset_all();
}

/* Drive one untimed pointer-script line and fire its single event. The
 * scripted `chord` verb is the only production path that synthesizes a Shift
 * modifier, so these tests install NO modifier provider - the scripted override
 * pushed by glr_ctrl_scripted_*_with_modifiers is the sole modifier source,
 * exactly as in a real capture/tour run. */
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

/* Syntax highlighting is the one presentation setting whose default is
 * resolved at runtime rather than compiled in: glr_ctrl_init_gl turns it off
 * on a Mesa context, where each colored span forces a raster-state
 * revalidation. The regression that matters is not the probe (the stub
 * renderer string is never Mesa) but the *durability* of its verdict - a
 * default installed only into the live value would be silently undone by the
 * next whole-world reset, which is exactly what F12 and Ctrl+N do.
 *
 * Values here are read back from the installed default rather than compared to
 * a literal, so retuning CFG_DEFAULT_SYNTAX_HIGHLIGHT cannot break this. */
static void test_syntax_highlight_default_survives_resets(void) {
    printf("--- imrepl_ctrl syntax-highlight default ---\n");

    int original = glr_state_default_syntax_highlight();
    ASSERT_INT("default starts at the compile-time value",
               original, CFG_DEFAULT_SYNTAX_HIGHLIGHT);

    /* Any state other than the compiled-in one, so a reset that ignores the
     * installed default lands somewhere visibly different. */
    int installed = (original == SYNTAX_HIGHLIGHT_OFF) ? SYNTAX_HIGHLIGHT_ON
                                                       : SYNTAX_HIGHLIGHT_OFF;
    glr_state_set_default_syntax_highlight(installed);
    ASSERT_INT("installing the default moves the live value too",
               glr_state_presentation().syntax_highlight, installed);

    glr_state_presentation_reset_defaults();
    ASSERT_INT("presentation reset lands on the installed default",
               glr_state_presentation().syntax_highlight, installed);
    ASSERT_INT("neighbouring presentation fields still reset to their macros",
               glr_state_presentation().code_focus, CFG_DEFAULT_CODE_FOCUS);

    glr_ctrl_reset_all();
    ASSERT_INT("a whole-world reset keeps the installed default",
               glr_state_presentation().syntax_highlight, installed);

    /* The default is a starting point, not a lock: the Config row still
     * cycles away from it, and doing so must not rewrite the default. */
    glr_state_presentation_mut()->syntax_highlight = original;
    ASSERT_INT("changing the setting leaves the default alone",
               glr_state_default_syntax_highlight(), installed);

    glr_state_set_default_syntax_highlight(original);
    glr_state_presentation_reset_defaults();
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

    /* 3. Scrolloff margin test: cursor maintains GLR_CODE_PANEL_SCROLLOFF (2) lines margin */
    {
        UiReplCodePanelLayout l;
        memset(&l, 0, sizeof(l));
        l.visible_lines = 10;
        l.total_lines = 50;

        /* Move cursor down past bottom margin (visible_lines - 1 - scrolloff = 7) */
        editor_scroll_set(0);
        editor_scroll_follow_cursor_set(1);
        l.follow_doc_line = 8;
        glr_ctrl_apply_code_panel_follow_scroll(&l);
        /* follow_doc_line (8) - visible_lines (10) + 1 + scrolloff (2) = 1 */
        ASSERT_INT("scrolloff scrolls down when past bottom margin", editor_scroll(), 1);

        /* Move cursor up past top margin (scroll + scrolloff = 1 + 2 = 3) */
        l.follow_doc_line = 2;
        editor_scroll_follow_cursor_set(1);
        glr_ctrl_apply_code_panel_follow_scroll(&l);
        /* follow_doc_line (2) - scrolloff (2) = 0 */
        ASSERT_INT("scrolloff scrolls up when above top margin", editor_scroll(), 0);
    }
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

static void test_refresh_window_title(void) {
#if defined(__EMSCRIPTEN__)
    /* glr_ctrl_refresh_window_title() returns early under __EMSCRIPTEN__:
     * Emscripten's glutSetWindowTitle is a no-op, so the formatting is skipped
     * on purpose and there is no title to assert on. */
    return;
#else
    g_last_window_title[0] = '\0';
    repl_state_scenes_set_active_example_idx(0);
    glr_ctrl_refresh_window_title();
    g_last_window_title[0] = '\0';
    repl_state_scenes_set_active_example_idx(1);
    glr_ctrl_refresh_window_title();
    ASSERT_TRUE("window title is set", g_last_window_title[0] != '\0');
    ASSERT_TRUE("window title omits '(no workspace)' when no workspace is active",
                strstr(g_last_window_title, "(no workspace)") == NULL);
#endif  /* !__EMSCRIPTEN__ */
}

/* ---------------------------------------------------------------------------
 * Vertex-label depth snapshot: controller-side lifecycle.
 *
 * The expensive, stateful half of the label occlusion cull lives here rather
 * than in the pass (see glr_ctrl_capture_depth_snapshot and
 * docs/plans/in-review/vertex-label-depth-readback-stall.md), so this is where
 * it has to be tested. The hazard the design introduces is a RETAINED buffer
 * that still reads as valid after the scene it describes is gone - so most of
 * what follows is about invalidation rather than about capture.
 * ------------------------------------------------------------------------- */
static void test_depth_snapshot_gate_and_capture(void) {
    printf("--- depth snapshot: gate and capture ---\n");

    /* The profile row is top-level, not a child of the host-overlay aggregate
     * it happens to sit beside in the catalog: the capture runs after both
     * PROF_HOST_OVERLAYS and PROF_FRAME_WORK have closed, so an indented row
     * would read as a milliseconds-long child of a microseconds-long parent. */
    ASSERT_INT("depth snapshot is a top-level profile row",
               prof_section_info(PROF_DEPTH_SNAPSHOT).depth, 0);

    /* The capture reads the scene rect from the layout, so each of these
     * needs a window of its own rather than whatever the previous test left. */
    glr_ctrl_reshape(1000, 620);

    glr_ctrl_set_depth_readback_supported_for_test(1);
    glr_ctrl_invalidate_depth_snapshot();

    /* Gate closed: no capture, and - the part that matters - nothing left
     * published either. */
    glr_ctrl_set_depth_snapshot_wanted(0);
    gl_stub_counts_reset();
    glr_ctrl_capture_depth_snapshot();
    ASSERT_INT("gate closed takes no readback",
               gl_stub_counts[GL_STUB_glReadPixels], 0);
    ASSERT_INT("gate closed publishes no snapshot",
               glr_ctrl_depth_snapshot_view().valid, 0);

    /* Gate open: one readback, and a usable view carrying the viewport size so
     * the consumer can refuse it if the scene rect changes shape. */
    glr_ctrl_set_depth_snapshot_wanted(1);
    gl_stub_counts_reset();
    glr_ctrl_capture_depth_snapshot();
    ASSERT_INT("gate open takes exactly one readback",
               gl_stub_counts[GL_STUB_glReadPixels], 1);
    {
        OverlayDepthSnapshot view = glr_ctrl_depth_snapshot_view();
        ASSERT_INT("captured snapshot is valid", view.valid, 1);
        ASSERT_TRUE("captured snapshot has pixels", view.pixels != NULL);
        ASSERT_TRUE("captured snapshot carries its viewport size",
                    view.vw > 0 && view.vh > 0);
    }

    /* An unsupported context must neither read nor keep what it read earlier:
     * losing the capability drops the buffer rather than merely stopping the
     * refresh. */
    glr_ctrl_set_depth_readback_supported_for_test(0);
    ASSERT_INT("losing depth capability drops the retained snapshot",
               glr_ctrl_depth_snapshot_view().valid, 0);
    gl_stub_counts_reset();
    glr_ctrl_capture_depth_snapshot();
    ASSERT_INT("unsupported context takes no readback",
               gl_stub_counts[GL_STUB_glReadPixels], 0);
    glr_ctrl_set_depth_readback_supported_for_test(1);
}

/* The case review caught: labels off then on again must not hand the first
 * pass back the pre-off buffer. The capture gate alone does not give this -
 * "should we capture" and "is what we hold still usable" are different
 * questions, and answering only the first leaves a stale buffer published. */
static void test_depth_snapshot_invalidation(void) {
    printf("--- depth snapshot: invalidation ---\n");

    /* The capture reads the scene rect from the layout, so each of these
     * needs a window of its own rather than whatever the previous test left. */
    glr_ctrl_reshape(1000, 620);

    glr_ctrl_set_depth_readback_supported_for_test(1);
    glr_ctrl_set_depth_snapshot_wanted(1);
    glr_ctrl_capture_depth_snapshot();
    ASSERT_INT("snapshot captured before label-off",
               glr_ctrl_depth_snapshot_view().valid, 1);

    /* Labels off -> gate closes -> the retained buffer goes with it. */
    glr_ctrl_set_depth_snapshot_wanted(0);
    glr_ctrl_capture_depth_snapshot();
    ASSERT_INT("labels off drops the snapshot",
               glr_ctrl_depth_snapshot_view().valid, 0);

    /* Labels back on: the first frame must see NO snapshot - it captures at
     * the END of that frame, for the next one. A stale-publish bug shows up
     * right here, as a valid view before any capture has run. */
    glr_ctrl_set_depth_snapshot_wanted(1);
    ASSERT_INT("first frame after re-enabling has no snapshot yet",
               glr_ctrl_depth_snapshot_view().valid, 0);
    glr_ctrl_capture_depth_snapshot();
    ASSERT_INT("second frame after re-enabling has one",
               glr_ctrl_depth_snapshot_view().valid, 1);

    /* A reshape moves the scene rect, so the retained depth describes a region
     * that no longer exists. */
    glr_ctrl_reshape(900, 700);
    ASSERT_INT("reshape drops the snapshot",
               glr_ctrl_depth_snapshot_view().valid, 0);

    /* A wholesale document replacement likewise: those pixels are the old
     * scene's, and culling the next frame's labels against them would hide
     * labels behind geometry that no longer exists. */
    glr_ctrl_set_depth_snapshot_wanted(1);
    glr_ctrl_capture_depth_snapshot();
    ASSERT_INT("snapshot captured before reset",
               glr_ctrl_depth_snapshot_view().valid, 1);
    glr_ctrl_reset_all();
    ASSERT_INT("reset_all drops the snapshot",
               glr_ctrl_depth_snapshot_view().valid, 0);
}

/* A capture that fails must publish nothing rather than let the previous
 * buffer stand in as current. The startup capability probe reads a single
 * pixel and cannot promise every later full-viewport read succeeds. */
static void test_depth_snapshot_failed_read(void) {
    printf("--- depth snapshot: failed read ---\n");

    /* The capture reads the scene rect from the layout, so each of these
     * needs a window of its own rather than whatever the previous test left. */
    glr_ctrl_reshape(1000, 620);

    glr_ctrl_set_depth_readback_supported_for_test(1);
    glr_ctrl_set_depth_snapshot_wanted(1);
    glr_ctrl_capture_depth_snapshot();
    ASSERT_INT("good capture is valid", glr_ctrl_depth_snapshot_view().valid, 1);

    /* Make the read itself fail. The buffer then holds indeterminate contents;
     * culling against those would drop labels at random, so valid must clear
     * rather than the previous capture standing in as current. */
    g_gl_stub_fail_read_pixels = 1;
    glr_ctrl_capture_depth_snapshot();
    ASSERT_INT("failed read clears the snapshot",
               glr_ctrl_depth_snapshot_view().valid, 0);
    g_gl_stub_fail_read_pixels = 0;

    /* ...and recovers on the next good frame. */
    glr_ctrl_capture_depth_snapshot();
    ASSERT_INT("capture recovers after a failed read",
               glr_ctrl_depth_snapshot_view().valid, 1);

    /* Freeing is the shutdown path; ASan/LSan in the debug build is what
     * actually proves the allocation is released. */
    glr_ctrl_depth_snapshot_free();
    ASSERT_INT("freeing drops the snapshot",
               glr_ctrl_depth_snapshot_view().valid, 0);
}

/* The capture must read the SCENE rect, not whatever viewport happens to be
 * current. It runs at end of frame, after the code panel, HUDs and compositor
 * have each set the viewport to the whole window for their 2D passes - so
 * ambient GL state there is the window, not the scene.
 *
 * This was shipped broken once and neither the profiler nor a screenshot would
 * have shown it: the stall was gone (the point of the change), the labels still
 * drew, and the only symptom was that every snapshot got rejected next frame
 * for disagreeing with the projection rect - so the app paid for a LARGER
 * readback and silently did no occlusion culling at all. Hence a test that
 * pins the rect rather than just the absence of a stall.
 *
 * The GL stub answers GL_VIEWPORT with a fixed 1024x768 that has nothing to do
 * with the layout, which is exactly what makes the assertion sharp: reading
 * ambient state produces that, reading the layout produces the scene rect. */
static void test_depth_snapshot_uses_scene_rect(void) {
    printf("--- depth snapshot: captures the scene rect ---\n");

    int sx = 0, sy = 0, sw = 0, sh = 0;

    glr_ctrl_reshape(1000, 620);
    ui_layout_scene_rect(&sx, &sy, &sw, &sh);

    /* Simulate what the code panel / HUDs / compositor do before the capture
     * runs: reset the viewport to the whole window for their own 2D passes. */
    glViewport(0, 0, 1000, 620);

    glr_ctrl_set_depth_readback_supported_for_test(1);
    glr_ctrl_set_depth_snapshot_wanted(1);
    glr_ctrl_capture_depth_snapshot();

    {
        OverlayDepthSnapshot view = glr_ctrl_depth_snapshot_view();
        ASSERT_INT("capture is valid", view.valid, 1);
        ASSERT_INT("capture width is the scene rect's", view.vw, sw);
        ASSERT_INT("capture height is the scene rect's", view.vh, sh);
        /* Guards the test itself: if the layout ever made the scene rect equal
         * the stub's ambient viewport, the assertions above would pass for the
         * wrong reason and the regression would slip through again. */
        ASSERT_TRUE("scene rect differs from the ambient GL viewport, so the "
                    "assertion above can actually fail",
                    !(sw == 1024 && sh == 768));
    }
}

/* Generation-bumping paths that replace the live document wholesale must
 * invalidate retained depth - example load, user scene, workspace, tutorial
 * teardown, F12 cycling. Staleness is keyed off the undo generation for this
 * ordinary family; generation-preserving tutorial/tour paths have their own
 * app-boundary regressions above and in test_glr_tour_snapshot. */
static void test_depth_snapshot_stale_across_document_replacement(void) {
    printf("--- depth snapshot: document replacement ---\n");

    glr_ctrl_reshape(1000, 620);
    glr_ctrl_set_depth_readback_supported_for_test(1);
    glr_ctrl_set_depth_snapshot_wanted(1);
    glr_ctrl_capture_depth_snapshot();
    ASSERT_INT("snapshot captured before the document changes",
               glr_ctrl_depth_snapshot_view().valid, 1);

    /* This is what every scene/example/workspace/tutorial load calls. */
    editor_undo_note_wholesale_replacement();
    ASSERT_INT("document replacement drops the snapshot",
               glr_ctrl_depth_snapshot_view().valid, 0);

    /* ...and the next capture under the new document is usable again. */
    glr_ctrl_capture_depth_snapshot();
    ASSERT_INT("capture under the new document is valid",
               glr_ctrl_depth_snapshot_view().valid, 1);
}

static void test_autocomplete_wheel_routing(void) {
    printf("--- autocomplete popup wheel routing ---\n");
    prepare_display_fixture();
    glr_ctrl_reset_all();
    glr_ctrl_reshape(800, 600);
    ui_state_code_panel_mut()->panel_frac = 0.45f;
    ui_state_code_panel_mut()->layout_mode = CODE_PANEL_LAYOUT_LEFT;

    /* Add dummy lines so the editor has document content to scroll */
    for (int i = 0; i < 20; i++) {
        editor_feed_line("// comment line");
    }
    ASSERT_INT("document has 20 lines", repl_state_document_count(), 20);

    /* 1. Set input text to trigger 12 glPushAttrib matches */
    editor_load_line_to_input(0);
    strcpy(editor_state_input_mut()->input, "glPushAttrib(GL_");
    editor_state_input_mut()->input_len = (int)strlen(editor_state_input().input);
    editor_state_input_mut()->cursor_pos = editor_state_input().input_len;
    editor_completion_update();

    ASSERT_INT("match_count is 12", editor_state_autocomplete()->match_count, 12);
    ASSERT_INT("initial selected_idx is 0", editor_state_autocomplete()->selected_idx, 0);
    ASSERT_INT("initial scroll_top is 0", editor_state_autocomplete()->scroll_top, 0);

    /* Render frame to calculate and cache cursor anchor */
    glr_ctrl_display_frame();

    /* Find coordinates inside the popup */
    int win_h = ui_state_viewport().window_h;
    int pop_mx = 0;
    int pop_my = 0;
    int found_hit = 0;
    for (int y = 10; y < win_h && !found_hit; y += 10) {
        for (int x = 10; x < 400 && !found_hit; x += 10) {
            if (glr_ctrl_autocomplete_popup_hit_test(x, y)) {
                pop_mx = x;
                pop_my = y;
                found_hit = 1;
            }
        }
    }
    ASSERT_TRUE("found hit-test coordinates on popup", found_hit);
    ASSERT_TRUE("popup hit-test succeeds at (pop_mx, pop_my)",
                glr_ctrl_autocomplete_popup_hit_test(pop_mx, pop_my));

    int base_scroll = editor_scroll();

    /* 2. Wheel inside popup changes selection and does NOT scroll editor */
    glr_ctrl_mousewheel(0, -1, pop_mx, pop_my);
    ASSERT_INT("wheel +1 inside popup moves selected_idx to 1",
               editor_state_autocomplete()->selected_idx, 1);
    ASSERT_INT("wheel inside popup preserves editor_scroll",
               editor_scroll(), base_scroll);

    /* Scroll down to the end */
    glr_ctrl_mousewheel(0, -15, pop_mx, pop_my);
    ASSERT_INT("wheel to end clamps selected_idx to 11",
               editor_state_autocomplete()->selected_idx, 11);
    ASSERT_INT("wheel to end advances scroll_top to 2",
               editor_state_autocomplete()->scroll_top, 2);
    ASSERT_INT("editor_scroll still unchanged",
               editor_scroll(), base_scroll);

    /* 3. Wheel outside popup (over code panel) scrolls code panel */
    int cpx, cpy, cpw, cph;
    ui_layout_code_panel_rect(&cpx, &cpy, &cpw, &cph);
    int outside_x = cpx + 20;
    int outside_y = win_h - (cpy + 20);
    if (glr_ctrl_autocomplete_popup_hit_test(outside_x, outside_y)) {
        outside_y = win_h - (cpy + cph - 20);
    }
    ASSERT_TRUE("outside coord is not on popup",
                !glr_ctrl_autocomplete_popup_hit_test(outside_x, outside_y));
    ASSERT_TRUE("outside coord is on code panel",
                editor_input_point_in_code_panel(outside_x, outside_y));

    glr_ctrl_mousewheel(0, -2, outside_x, outside_y);
    ASSERT_INT("wheel outside popup scrolls editor",
               editor_scroll(), base_scroll + 2);
    ASSERT_INT("wheel outside popup leaves autocomplete selected_idx intact",
               editor_state_autocomplete()->selected_idx, 11);

    /* 4. Non-scrollable popup (<= 10 matches) consumes wheel without wrapping */
    strcpy(editor_state_input_mut()->input, "glBegin(gl_tri");
    editor_state_input_mut()->input_len = (int)strlen(editor_state_input().input);
    editor_state_input_mut()->cursor_pos = editor_state_input().input_len;
    editor_completion_update();
    ASSERT_INT("glBegin(gl_tri has 3 matches",
               editor_state_autocomplete()->match_count, 3);
    glr_ctrl_display_frame();

    int short_pop_mx = 0;
    int short_pop_my = 0;
    int found_short_hit = 0;
    for (int y = 10; y < win_h && !found_short_hit; y += 10) {
        for (int x = 10; x < 400 && !found_short_hit; x += 10) {
            if (glr_ctrl_autocomplete_popup_hit_test(x, y)) {
                short_pop_mx = x;
                short_pop_my = y;
                found_short_hit = 1;
            }
        }
    }
    ASSERT_TRUE("found hit-test coordinates on short popup", found_short_hit);

    int scroll_before = editor_scroll();
    /* Scroll down */
    glr_ctrl_mousewheel(0, -1, short_pop_mx, short_pop_my);
    ASSERT_INT("short popup selects item 1",
               editor_state_autocomplete()->selected_idx, 1);
    ASSERT_INT("short popup preserves scroll_top 0",
               editor_state_autocomplete()->scroll_top, 0);
    ASSERT_INT("short popup consumes wheel (no editor scroll)",
               editor_scroll(), scroll_before);

    /* Scroll down again: clamps at index 2 without wrap */
    glr_ctrl_mousewheel(0, -5, short_pop_mx, short_pop_my);
    ASSERT_INT("short popup clamps at item 2",
               editor_state_autocomplete()->selected_idx, 2);
    ASSERT_INT("short popup still preserves editor scroll",
               editor_scroll(), scroll_before);
}

int main(void) {
    printf("--- imrepl_ctrl tests ---\n");

    test_display_frame_builds_config_and_restores_live_state();
    test_display_frame_clear_color_after_clear_is_ignored();
    test_display_frame_clear_color_before_clear_applies();
    test_display_frame_chrome_clears_after_the_scene();
    test_display_frame_background_retention();
    test_background_retention_across_passes();
    test_display_frame_replay_prefix_holds_background();
    test_reshape_clamps_height();
    test_display_frame_profile_coverage();
    test_frame_spans_host_stages();
    test_prof_nesting_guard();
    test_prof_nesting_guard_over_a_display_frame();
    test_summary_row_metadata();
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
    test_scrollbar_drag_scrolls_code_panel();
    test_right_click_assignment_opens_value_plot();
    test_plot_tag_opens_the_plot_on_load();
    test_shift_right_click_adds_plot_series();
    test_right_click_gl_command_description_popup();
    test_right_click_empty_line_toggles_gl_state_report();
    test_divider_hover_yields_to_front_panel();
    test_right_click_uncommitted_empty_line_opens_gl_state_report();
    test_shift_right_click_pins_gl_state_comparison_basis();
    test_gl_state_popup_scroll_geometry();
    test_gl_state_popup_modelview_uses_four_lines();
    test_gl_state_popup_details_toggle();
    test_gl_state_popup_basis_header_sizes_the_chip();
    test_gl_state_popup_setup_fold();
    test_gl_state_popup_source_line_tracks_gutter();
    test_gl_state_popup_geometry_is_stable_over_time();
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
    test_new_scene_from_2d_pose_does_not_poison_reset();
    test_new_scene_real_2d_reset_during_transition();
    test_view_mode_3d_restore_tracks_example_loaded_midblend();
    test_view_mode_quick_2d_to_3d_waits_for_pending_example_pose();
    test_view_record_external_3d_pose_tracks_in_ortho();
    test_view_record_external_3d_pose_noop_in_perspective();
    test_quit_recovery_file();
    test_recovery_skips_unpromoted_example();
    test_recovery_workspace_rescues_scene_slots();
    test_build_ui_snapshot_is_idempotent();
    test_display_frame_scene_config_is_stable_across_frames();
    test_display_frame_no_replay_means_no_fade_plumbing();
    test_display_frame_follows_replay_line_after_tick();
    test_push_attrib_bit_token_highlights();
    test_begin_end_bracket_highlights();
    test_replay_call_site_highlights_are_pushed();
    test_replay_call_chain_ramp_colors();
    test_replay_call_chain_recursive_same_line();
    test_replay_call_chain_unindexed_fallback();
    test_replay_call_chain_gutter_bands_capping();
    test_replay_call_chain_focus_not_anchor();
    test_replay_call_chain_recursive_deep_cap();
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
    test_variable_time_step_replay_policy();
    test_auxiliary_scene_pass_side_effects();
    test_wireframe_renderer_ignores_user_draw_state();
    test_example_reset_reapplies_light_theme();
    test_scene_local_reset_covers_whole_roster();
    test_every_modal_kind_is_wired();
    test_display_frame_merges_light_theme_and_enable_mask();
    test_export_light_bridge_reads_app_state();
    test_mouse_routing_and_hit_testing();
    test_color_picker_materialfv();
    test_special_key_shortcuts();
    test_func_nav_special_beats_replay_and_help();
    test_ctrl_click_go_to_func_def();
    test_scene_cycle_skips_failed_examples();
    test_promotion_keeps_example_catalog_place();
    test_scripted_chord_reaches_shift_shortcuts();
    test_post_filter_key_cycling();
    test_app_lifecycle_bootstrap_shutdown();
    test_init_gl_requires_loaded_point_parameter_proc();
    test_syntax_highlight_default_survives_resets();
    test_code_panel_scroll_clamping_and_follow();
    test_refresh_window_title();
    test_depth_snapshot_gate_and_capture();
    test_depth_snapshot_invalidation();
    test_depth_snapshot_failed_read();
    test_depth_snapshot_uses_scene_rect();
    test_depth_snapshot_stale_across_document_replacement();
    test_autocomplete_wheel_routing();

    printf("\n");
    return test_harness_report(&g_harness, "test_imrepl_ctrl");
}
