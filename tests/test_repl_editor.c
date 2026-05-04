#include "editor_input.h"
#include "repl_core_internal.h"
#include "editor_commit.h"
#include "repl_actions.h"
#include "repl_camera_controls.h"
#include "editor_clipboard.h"
#include "ui_code_panel_layout.h"
#include "repl_config.h"
#include "repl_export.h"
#include "replay.h"
#include "replay_state.h"
#include "repl_keys.h"
#include "repl_state.h"
#include "editor_help_session.h"
#include "ui_state.h"
#include "prof.h"
#include "ui_panels.h"
#include "ui_layout.h"
#include "ui_variable_panel.h"
#include "editor_inline_rename.h"
#include "imrepl_ctrl.h"
#include "./include/gl_2d.h"
#include "ui_metrics.h"

#define g_status     (ui_state_status_mut()->text)
#define g_scroll     (editor_state_scroll_mut()->scroll)
#define g_panel_frac (ui_state_code_panel_mut()->panel_frac)
#define g_ac_count   (editor_state_autocomplete_mut()->match_count)
#define g_ac_sel     (editor_state_autocomplete_mut()->selected_idx)
#define g_ac_ghost   (editor_state_autocomplete_mut()->ghost)
#define g_ac_hint    (editor_state_autocomplete_mut()->hint)
#define g_search_active    (editor_state_search_mut()->active)
#define g_search_query     (editor_state_search_mut()->query)
#define g_search_query_len (editor_state_search_mut()->query_len)
#define g_search_cursor_pos (editor_state_search_mut()->cursor_pos)
#define g_show_help          (ui_state_help_mut()->visible)
#define g_show_profile_panel (ui_state_profile_panel_mut()->mode)
#define g_scroll_follow_cursor (editor_state_scroll_mut()->scroll_follow_cursor)
#define refresh_workspace_header_lines repl_state_refresh_workspace_header_lines
#define parse_workspace_header_line    repl_state_parse_workspace_header_line

#include "support/test_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static TestHarness g_harness = TEST_HARNESS_INIT;
static int g_mock_modifiers = 0;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    TEST_ASSERT_INT(&g_harness, label, got, exp); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    TEST_ASSERT_STR(&g_harness, label, got, exp); \
} while (0)

#define g_workspace_header_lines (repl_state_import_export().workspace_header_lines)
#define g_workspace_header_line_count (repl_state_import_export().workspace_header_line_count)
#define g_render_state_lines (repl_state_import_export().render_state_lines)
#define g_cam_lines (repl_state_import_export().cam_lines)

#define replay_active        (replay_state_mut()->active)
#define replay_state         (replay_state_mut()->state)
#define replay_pc            (replay_state_mut()->pc)
#define replay_mode          (replay_state_mut()->mode)
#define replay_src_line      (replay_state_mut()->src_line_idx)
#define replay_expand_args   (replay_state_mut()->expand_args)

#define ASSERT_DECL_OK(label, cond, err) do { \
    (void)(err); \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

static void declare_test_vars(void) {
    char err[128];
    ASSERT_DECL_OK("declare_predef_var x", repl_eval_declare_predef_var("x", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var y", repl_eval_declare_predef_var("y", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var z", repl_eval_declare_predef_var("z", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var i", repl_eval_declare_predef_var("i", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var j", repl_eval_declare_predef_var("j", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var k", repl_eval_declare_predef_var("k", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var n", repl_eval_declare_predef_var("n", err, sizeof(err)), err);
}

#include "variable_panel.h"
#include "variable_panel_drag.h"

#define VAR_PANEL_PAD_INTERNAL   6
#define VAR_TITLE_H_INTERNAL    20

static void set_editor_input(const char *text) {
    ReplEditorInputState *inp = editor_state_input_mut();
    strncpy(inp->input, text, MAX_INPUT_LEN - 1);
    inp->input[MAX_INPUT_LEN - 1] = '\0';
    inp->input_len = (int)strlen(inp->input);
    editor_cursor_pos_set(inp->input_len);
}

static int mock_get_modifiers(void) {
    return g_mock_modifiers;
}

static void assert_status_contains(const char *label, const char *needle) {
    ASSERT_TRUE(label, strstr(g_status, needle) != NULL);
}

static int cfg_row_for_key(ReplConfigKey key) {
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        if (g_cfg_items[i].key == key)
            return i;
    }
    return -1;
}

static int test_code_panel_row_count_for_text(const char *text, int first_x,
                                              int panel_w) {
    CodePanelTextLayout layout =
        repl_code_panel_layout_make(panel_w, first_x, FONT_W, repl_state_presentation().wrap_at_comma);
    return repl_code_panel_row_count_for_text(text, &layout);
}

static int code_panel_header_row_count(void) {
    int panel_w;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = repl_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int rows = 0;

    repl_layout_code_panel_rect(NULL, NULL, &panel_w, NULL);
    refresh_workspace_header_lines();
    for (int i = 0; i < g_workspace_header_line_count; i++)
        rows += test_code_panel_row_count_for_text(g_workspace_header_lines[i],
                                                   text_x, panel_w);
    for (int i = 0; g_header_pre[i]; i++)
        rows += test_code_panel_row_count_for_text(g_header_pre[i], text_x, panel_w);
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++)
        rows += test_code_panel_row_count_for_text(g_render_state_lines[i],
                                                   text_x, panel_w);
    for (int i = 0; i < CAM_LINE_COUNT; i++)
        rows += test_code_panel_row_count_for_text(g_cam_lines[i], text_x, panel_w);
    for (int i = 0; g_header_post[i]; i++)
        rows += test_code_panel_row_count_for_text(g_header_post[i], text_x, panel_w);
    return rows;
}

static int code_panel_mouse_y_for_cmd(int cmd_idx) {
    int cp_y, cp_h, panel_w;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = repl_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int doc_line = code_panel_header_row_count();

    repl_layout_code_panel_rect(NULL, &cp_y, &panel_w, &cp_h);
    for (int i = 0; i < cmd_idx && i < repl_state_document_count(); i++) {
        const char *line_text = editor_buffer_line(i);
        doc_line += test_code_panel_row_count_for_text(line_text ? line_text : "",
                                                       text_x, panel_w);
    }

    int vis = doc_line - g_scroll;
    int line_y_start = cp_y + cp_h - CODE_MARGIN_Y - 2 * LINE_H;
    int gl_y = line_y_start - vis * LINE_H + 1;
    return ui_state_viewport().window_h - gl_y;
}

static void assert_float_decl_rejected_atomic(const char *label,
                                              const char *src,
                                              const char *rejected_name,
                                              const char *status_part) {
    char detail[160];
    int old_num_cmds = repl_state_document_count();
    int old_num_predef_vars = g_num_predef_vars;

    set_editor_input(src);
    repl_state_edit_line_set(repl_state_document_count());
    editor_insert_mode_set(0);

    int result = try_commit_float_decl();

    snprintf(detail, sizeof(detail), "%s: handler consumed input", label);
    ASSERT_INT(detail, result, 1);
    snprintf(detail, sizeof(detail), "%s: cmd count unchanged", label);
    ASSERT_INT(detail, repl_state_document_count(), old_num_cmds);
    snprintf(detail, sizeof(detail), "%s: var count unchanged", label);
    ASSERT_INT(detail, g_num_predef_vars, old_num_predef_vars);
    snprintf(detail, sizeof(detail), "%s: rejected name not registered", label);
    ASSERT_TRUE(detail, repl_eval_find_predef_var_idx(rejected_name) < 0);
    snprintf(detail, sizeof(detail), "%s: status", label);
    assert_status_contains(detail, status_part);
    ASSERT_TRUE("atomic failure preserves anchor",
                old_num_predef_vars <= 1 || repl_eval_find_predef_var_idx("anchor") >= 0);
}

int main() {
    repl_eval_init_predef_vars();
    editor_input_set_modifier_provider_for_test(mock_get_modifiers);
    printf("--- repl_editor tests ---\n");

    /* Commit chain ordering: float declarations must run before assignment
     * parsing, otherwise `float name` is misclassified as an assignment. */
    {
        repl_reset_state();
        set_editor_input("float chain_order;");
        repl_state_edit_line_set(0);
        editor_insert_mode_set(0);

        int result = try_commit_any();

        ASSERT_INT("commit chain float decl consumed", result, 1);
        ASSERT_INT("commit chain float decl inserted one cmd", repl_state_document_count(), 1);
        ASSERT_INT("commit chain float decl cmd type",
                   repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_TRUE("commit chain float decl registered var",
                    repl_eval_find_predef_var_idx("chain_order") >= 0);
    }

    /* 0. Code/scene panel geometry supports left, top, bottom, and hidden layouts */
    {
        int x, y, w, h;

        ui_state_viewport_set_size(1000, 800);
        g_panel_frac = 0.25f;

        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        repl_layout_code_panel_rect(&x, &y, &w, &h);
        ASSERT_INT("left code x", x, 0);
        ASSERT_INT("left code y", y, 0);
        ASSERT_INT("left code w", w, 250);
        ASSERT_INT("left code h", h, 800);
        repl_layout_scene_rect(&x, &y, &w, &h);
        ASSERT_INT("left scene x", x, 250);
        ASSERT_INT("left scene y", y, 0);
        ASSERT_INT("left scene w", w, 750);
        ASSERT_INT("left scene h", h, 800);

        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_TOP;
        repl_layout_code_panel_rect(&x, &y, &w, &h);
        ASSERT_INT("top code x", x, 0);
        ASSERT_INT("top code y", y, 600);
        ASSERT_INT("top code w", w, 1000);
        ASSERT_INT("top code h", h, 200);
        repl_layout_scene_rect(&x, &y, &w, &h);
        ASSERT_INT("top scene x", x, 0);
        ASSERT_INT("top scene y", y, 0);
        ASSERT_INT("top scene w", w, 1000);
        ASSERT_INT("top scene h", h, 600);

        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_BOTTOM;
        repl_layout_code_panel_rect(&x, &y, &w, &h);
        ASSERT_INT("bottom code x", x, 0);
        ASSERT_INT("bottom code y", y, 0);
        ASSERT_INT("bottom code w", w, 1000);
        ASSERT_INT("bottom code h", h, 200);
        repl_layout_scene_rect(&x, &y, &w, &h);
        ASSERT_INT("bottom scene x", x, 0);
        ASSERT_INT("bottom scene y", y, 200);
        ASSERT_INT("bottom scene w", w, 1000);
        ASSERT_INT("bottom scene h", h, 600);

        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN;
        repl_layout_code_panel_rect(&x, &y, &w, &h);
        ASSERT_INT("hidden code x", x, 0);
        ASSERT_INT("hidden code y", y, 0);
        ASSERT_INT("hidden code w", w, 0);
        ASSERT_INT("hidden code h", h, 0);
        repl_layout_scene_rect(&x, &y, &w, &h);
        ASSERT_INT("hidden scene x", x, 0);
        ASSERT_INT("hidden scene y", y, 0);
        ASSERT_INT("hidden scene w", w, 1000);
        ASSERT_INT("hidden scene h", h, 800);

        ui_state_viewport_set_size(1200, 800);
        g_panel_frac = CFG_DEFAULT_PANEL_FRAC;
        repl_state_presentation_mut()->code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;
    }

    /* 0b. Code panel config cycles Left -> Top -> Bottom -> Hidden and imports legacy top layout */
    {
        int row = cfg_row_for_key(REPL_CONFIG_CODE_PANEL_LAYOUT);
        ASSERT_TRUE("code panel cfg row exists", row >= 0);
        if (row >= 0) {
            repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
            repl_cfg_cycle_row(row, +1);
            ASSERT_INT("code panel cfg cycles to top",
                       repl_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_TOP);
            repl_cfg_cycle_row(row, +1);
            ASSERT_INT("code panel cfg cycles to bottom",
                       repl_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_BOTTOM);
            g_ac_count = 2;
            g_ac_sel = 1;
            strcpy(g_ac_ghost, "glVertex3f");
            strcpy(g_ac_hint, "vertex");
            repl_cfg_cycle_row(row, +1);
            ASSERT_INT("code panel cfg cycles to hidden",
                       repl_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);
            ASSERT_INT("hide clears autocomplete count", g_ac_count, 0);
            ASSERT_INT("hide clears autocomplete selection", g_ac_sel, 0);
            ASSERT_STR("hide clears autocomplete ghost", g_ac_ghost, "");
            ASSERT_STR("hide clears autocomplete hint", g_ac_hint, "");
            repl_cfg_cycle_row(row, +1);
            ASSERT_INT("code panel cfg wraps to left",
                       repl_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_LEFT);
        }

        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        ASSERT_INT("parse code_panel cfg",
                   parse_workspace_header_line("// @cfg code_panel = 2"), 1);
        ASSERT_INT("parse code_panel bottom",
                   repl_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_BOTTOM);

        ASSERT_INT("parse code_panel hidden cfg",
                   parse_workspace_header_line("// @cfg code_panel = 3"), 1);
        ASSERT_INT("parse code_panel hidden",
                   repl_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);

        {
            int found_hidden_export = 0;
            refresh_workspace_header_lines();
            for (int i = 0; i < g_workspace_header_line_count; i++) {
                if (strcmp(g_workspace_header_lines[i],
                           "// @cfg code_panel = 3") == 0)
                    found_hidden_export = 1;
            }
            ASSERT_TRUE("workspace header exports hidden code panel",
                        found_hidden_export);
        }

        ASSERT_INT("parse legacy top_code_panel cfg",
                   parse_workspace_header_line("// @cfg top_code_panel = 1"), 1);
        ASSERT_INT("legacy top_code_panel maps to top",
                   repl_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_TOP);

        ASSERT_INT("parse legacy top_code_panel off cfg",
                   parse_workspace_header_line("// @cfg top_code_panel = 0"), 1);
        ASSERT_INT("legacy top_code_panel off maps to left",
                   repl_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_LEFT);

        g_panel_frac = CFG_DEFAULT_PANEL_FRAC;
        repl_state_presentation_mut()->code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;
    }

    /* 0b2. Config/action module owns shortcut and menu row dispatch. */
    {
        repl_reset_state();

        repl_state_presentation_mut()->wireframe = 0;
        ASSERT_INT("config special shortcut consumed",
                   repl_cfg_handle_special_shortcut(GLUT_KEY_F2), 1);
        ASSERT_INT("config special shortcut toggles wireframe",
                   repl_state_presentation().wireframe, 1);

        repl_state_presentation_mut()->grid_major_idx = 0;
        ASSERT_INT("config ascii shortcut consumed",
                   repl_cfg_handle_ascii_shortcut(KEY_CTRL_O), 1);
        ASSERT_INT("config ascii shortcut cycles grid major",
                   repl_state_presentation().grid_major_idx, 1);

        int row = cfg_row_for_key(REPL_CONFIG_CODE_PANEL_LAYOUT);
        ASSERT_TRUE("config menu action row exists", row >= 0);
        if (row >= 0) {
            repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
            int close_menu = repl_action_menu_item_activate(REPL_MENU_CONFIG, row);
            ASSERT_INT("config menu action keeps menu open", close_menu, 0);
            ASSERT_INT("config menu action cycles code panel",
                       repl_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_TOP);
        }
    }

    /* 0c. Hidden code panel returns to the editor on ordinary input */
    {
        repl_reset_state();
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN;
        editor_handle_key('v', 0, 0);
        ASSERT_INT("typing restores hidden code panel",
                   repl_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_LEFT);
        ASSERT_STR("typing after restore still reaches input", editor_state_input().input, "v");

        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN;
        editor_handle_key('`', 0, 0);
        ASSERT_INT("config shortcut restores hidden code panel",
                   repl_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_LEFT);
        editor_handle_key('`', 0, 0);

        repl_state_presentation_mut()->code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;
    }

    /* 0c2. Keyboard mode routing keeps rename ahead of config, replay, and search. */
    {
        repl_reset_state();
        repl_load_example(0);
        int slot = repl_promote_example_if_needed();
        ASSERT_TRUE("rename route setup slot", slot >= 0);
        ASSERT_INT("rename route begin", repl_inline_rename_begin(slot), 1);

        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN;
        editor_handle_key('`', 0, 0);
        ASSERT_INT("rename swallows config hidden restore",
                   repl_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);
        ASSERT_INT("rename remains active after config key",
                   repl_inline_rename_active(), 1);

        editor_handle_key(KEY_CTRL_R, 0, 0);
        ASSERT_INT("rename swallows replay toggle", replay_active, 0);

        editor_handle_key(KEY_CTRL_F, 0, 0);
        ASSERT_INT("rename swallows search open", g_search_active, 0);

        repl_inline_rename_cancel();
        repl_state_presentation_mut()->code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;
    }

    /* 0c3. Search mode captures printable editing keys before text editing. */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        set_editor_input("");

        editor_handle_key(KEY_CTRL_F, 0, 0);
        ASSERT_INT("search route opens", g_search_active, 1);

        editor_handle_key('v', 0, 0);
        ASSERT_STR("search route receives printable key", g_search_query, "v");
        ASSERT_TRUE("search route does not type into editor input",
                    strcmp(editor_state_input().input, "v") != 0);
        ASSERT_INT("search route does not commit", repl_state_document_count(), 1);

        search_clear_all();
    }

    /* 0c4. Semicolon keyboard route still enters the commit handler chain. */
    {
        repl_reset_state();
        set_editor_input("float keyboard_route");

        editor_handle_key(';', 0, 0);

        ASSERT_INT("semicolon route committed one command", repl_state_document_count(), 1);
        ASSERT_INT("semicolon route used float declaration handler",
                   repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_TRUE("semicolon route registered declared variable",
                    repl_eval_find_predef_var_idx("keyboard_route") >= 0);
    }

    /* 0c5. Special-key routing keeps rename ahead of replay/search/navigation. */
    {
        repl_reset_state();
        repl_load_example(0);
        int slot = repl_promote_example_if_needed();
        ASSERT_TRUE("rename special route setup slot", slot >= 0);
        ASSERT_INT("rename special route begin", repl_inline_rename_begin(slot), 1);

        set_editor_input("abc");
        editor_cursor_pos_set(2);
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN;
        g_show_help = 0;
        replay_active = 1;
        replay_state = REPLAY_PAUSED;
        replay_pc = 0;
        g_search_active = 1;
        snprintf(g_search_query, MAX_INPUT_LEN, "abc");
        g_search_query_len = 3;
        g_search_cursor_pos = 2;

        editor_handle_special(GLUT_KEY_LEFT, 0, 0);
        ASSERT_INT("rename special swallows hidden restore",
                   repl_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);
        ASSERT_INT("rename special keeps editor cursor", editor_cursor_pos(), 2);
        ASSERT_INT("rename special keeps search cursor", g_search_cursor_pos, 2);
        ASSERT_INT("rename special keeps replay pc", replay_pc, 0);

        editor_handle_special(GLUT_KEY_F1, 0, 0);
        ASSERT_INT("rename special swallows help toggle", g_show_help, 0);
        ASSERT_INT("rename still active after special keys",
                   repl_inline_rename_active(), 1);

        repl_inline_rename_cancel();
        search_clear_all();
        replay_active = 0;
        repl_state_presentation_mut()->code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;
    }

    /* 0c6. Search mode captures special arrows before editor/help navigation. */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        navigate_to_line(0);
        set_editor_input("abc");
        editor_cursor_pos_set(2);
        g_show_help = 1;
        editor_help_session_set_tab(1);
        g_search_active = 1;
        snprintf(g_search_query, MAX_INPUT_LEN, "abc");
        g_search_query_len = 3;
        g_search_cursor_pos = 2;

        editor_handle_special(GLUT_KEY_LEFT, 0, 0);

        ASSERT_INT("search special moves search cursor", g_search_cursor_pos, 1);
        ASSERT_INT("search special leaves editor cursor", editor_cursor_pos(), 2);
        ASSERT_INT("search special leaves help tab", editor_help_session_tab_idx(), 1);

        search_clear_all();
        g_show_help = 0;
    }

    /* 0c7. F12 special route still cycles built-in examples. */
    {
        repl_reset_state();
        if (repl_example_count() > 1) {
            repl_load_example(0);
            imrepl_ctrl_router_handle_scene_cycle_special(GLUT_KEY_F12);
            ASSERT_INT("f12 special route advances example",
                    repl_state_scenes().active_example_idx, 1);
        }
    }

    /* 0d. Cramped variable-panel fallback still preserves scene status strip */
    {
        int x, y, w, h;
        ui_state_viewport_set_size(320, 80);
        g_panel_frac = 0.25f;
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        replay_active = 0;

        ui_variable_panel_rect(&x, &y, &w, &h);
        ASSERT_INT("cramped var panel clears status strip",
                   y, STATUSBAR_H + 4);

        ui_state_viewport_set_size(1200, 800);
        g_panel_frac = CFG_DEFAULT_PANEL_FRAC;
        repl_state_presentation_mut()->code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;
    }

    /* 0e. Camera control module owns scene drag and momentum behavior. */
    {
        repl_reset_state();
        ui_state_camera_set_orbit(0.0f, 0.0f);
        ui_state_camera_set_distance(10.0f);
        repl_camera_mouse_event(GLUT_LEFT_BUTTON, GLUT_DOWN, 10, 10, 0);
        repl_camera_drag_motion(20, 14);
        ASSERT_TRUE("camera left drag yaws",
                    fabsf(ui_state_camera().ry - 5.0f) < 1e-6f);
        ASSERT_TRUE("camera left drag pitches",
                    fabsf(ui_state_camera().rx - 2.0f) < 1e-6f);
        repl_camera_mouse_event(GLUT_LEFT_BUTTON, GLUT_UP, 20, 14, 0);
        repl_camera_tick();
        ASSERT_TRUE("camera release keeps yaw momentum",
                    fabsf(ui_state_camera().ry - 7.5f) < 1e-6f);

        repl_camera_controls_reset();
        ui_state_camera_set_orbit(0.0f, 0.0f);
        ui_state_camera_set_pan(0.0f, 0.0f, 0.0f);
        ui_state_camera_set_distance(10.0f);
        repl_camera_mouse_event(GLUT_RIGHT_BUTTON, GLUT_DOWN, 0, 0,
                                GLUT_ACTIVE_SHIFT);
        repl_camera_drag_motion(0, 20);
        ASSERT_TRUE("camera shift-right drag pans y",
                    fabsf(ui_state_camera().ty - 1.0f) < 1e-6f);
        ASSERT_TRUE("camera y pan lights motion glow",
                    fabsf(ui_state_camera().motion_glow - 1.0f) < 1e-6f);

        repl_camera_controls_reset();
        ui_state_camera_set_distance(0.6f);
        repl_camera_mouse_event(GLUT_MIDDLE_BUTTON, GLUT_DOWN, 0, 0, 0);
        repl_camera_drag_motion(0, -100);
        ASSERT_TRUE("camera middle drag clamps near zoom",
                    fabsf(ui_state_camera().dist - 0.5f) < 1e-6f);
    }

    /* 1. Undo when nothing to undo - must run before any undo push */
    {
        /* g_undo_count starts at 0 (global zero-init); repl_reset_state() does
         * NOT clear the undo buffer, so run this before touching undo at all. */
        repl_reset_state();
        repl_undo_pop_snapshot();
        /* Should survive without crashing; state unchanged */
        ASSERT_INT("undo-nothing: num_cmds still 0", repl_state_document_count(), 0);
    }

    /* 2. Redo when nothing to redo - similarly must be before any undo activity */
    {
        repl_reset_state();
        repl_undo_do_redo();
        ASSERT_INT("redo-nothing: num_cmds still 0", repl_state_document_count(), 0);
    }

    /* 3. Undo/Redo basic */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("glVertex3f(1,1,1)");
        ASSERT_INT("num_cmds 1", repl_state_document_count(), 1);

        repl_undo_push_snapshot();
        repl_feed_line_public("glVertex3f(2,2,2)");
        ASSERT_INT("num_cmds 2", repl_state_document_count(), 2);

        repl_undo_pop_snapshot();
        ASSERT_INT("num_cmds after undo", repl_state_document_count(), 1);
        ASSERT_STR("cmd 0 source", editor_buffer_line(0), "  glVertex3f(1, 1, 1);");

        repl_undo_do_redo();
        ASSERT_INT("num_cmds after redo", repl_state_document_count(), 2);
        ASSERT_STR("cmd 1 source", editor_buffer_line(1), "  glVertex3f(2, 2, 2);");
    }

    /* 4. Deleting commands - basic */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        ASSERT_INT("num_cmds 3", repl_state_document_count(), 3);

        delete_cmd_range(1, 1, "test");
        ASSERT_INT("num_cmds after delete", repl_state_document_count(), 2);
        ASSERT_STR("line 0 still there", editor_buffer_line(0), "  glVertex3f(0, 0, 0);");
        ASSERT_STR("line 1 is now old line 2", editor_buffer_line(1), "  glVertex3f(2, 2, 2);");
    }

    /* 5. delete_cmd_range - count=0 (no-op) */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        delete_cmd_range(0, 0, "noop");
        ASSERT_INT("delete count=0: no change", repl_state_document_count(), 1);
    }

    /* 6. delete_cmd_range - start=repl_state_document_count() (out of bounds, no-op) */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        delete_cmd_range(1, 1, "oob");  /* start == repl_state_document_count() (1) */
        ASSERT_INT("delete oob start: no change", repl_state_document_count(), 1);
    }

    /* 7. delete_cmd_range - count clamped when over-count */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        /* ask to delete 5 but only 1 remains at start=1 */
        delete_cmd_range(1, 5, "clamp");
        ASSERT_INT("delete clamp: leaves 1 cmd", repl_state_document_count(), 1);
        ASSERT_STR("delete clamp: cmd 0 remains", editor_buffer_line(0), "  glVertex3f(0, 0, 0);");
    }

    /* 8. delete_cmd_range - edit_line clamped when it would exceed repl_state_document_count() */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_state_edit_line_set(2);  /* beyond last cmd */
        delete_cmd_range(1, 1, "elclamp");
        /* repl_state_edit_line() should be clamped to repl_state_document_count() (1) */
        ASSERT_INT("delete edit_line clamp: edit_line<=num_cmds", repl_state_edit_line() <= repl_state_document_count(), 1);
    }

    /* 8a. delete_cmd_range - interior multi-line block */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        repl_feed_line_public("glVertex3f(3,3,3)");
        repl_feed_line_public("glVertex3f(4,4,4)");
        editor_state_selection_set(1, 3);

        delete_cmd_range(1, 3, "Deleted");

        ASSERT_INT("delete block: leaves 2 cmds", repl_state_document_count(), 2);
        ASSERT_STR("delete block: first survives", editor_buffer_line(0), "  glVertex3f(0, 0, 0);");
        ASSERT_STR("delete block: last survives", editor_buffer_line(1), "  glVertex3f(4, 4, 4);");
        ASSERT_INT("delete block: edit_line at deletion start", repl_state_edit_line(), 1);
        ASSERT_STR("delete block: input reloaded", editor_state_input().input, "glVertex3f(4, 4, 4)");
        ASSERT_TRUE("delete block: selection cleared", !repl_clipboard_sel_active());
        assert_status_contains("delete block: status line count", "Deleted 3 lines");
    }

    /* 8b. Backspace deletes a reversed selection */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        repl_feed_line_public("glVertex3f(3,3,3)");
        repl_feed_line_public("glVertex3f(4,4,4)");
        editor_state_selection_set(4, 2);
        repl_state_edit_line_set(4);

        editor_handle_key(8, 0, 0);

        ASSERT_INT("backspace reversed selection: leaves 2 cmds", repl_state_document_count(), 2);
        ASSERT_STR("backspace reversed selection: first survives", editor_buffer_line(0), "  glVertex3f(0, 0, 0);");
        ASSERT_STR("backspace reversed selection: second survives", editor_buffer_line(1), "  glVertex3f(1, 1, 1);");
        ASSERT_INT("backspace reversed selection: edit_line", repl_state_edit_line(), 2);
        ASSERT_TRUE("backspace reversed selection: selection cleared", !repl_clipboard_sel_active());
    }

    /* 8b2. Ctrl+D deletes a selected range through its own key path */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        repl_feed_line_public("glVertex3f(3,3,3)");
        repl_feed_line_public("glVertex3f(4,4,4)");
        editor_state_selection_set(3, 1);
        repl_state_edit_line_set(3);

        editor_handle_key(4, 0, 0);

        ASSERT_INT("ctrl-d selection: leaves 2 cmds", repl_state_document_count(), 2);
        ASSERT_STR("ctrl-d selection: first survives", editor_buffer_line(0), "  glVertex3f(0, 0, 0);");
        ASSERT_STR("ctrl-d selection: last survives", editor_buffer_line(1), "  glVertex3f(4, 4, 4);");
        ASSERT_INT("ctrl-d selection: edit_line", repl_state_edit_line(), 1);
        ASSERT_TRUE("ctrl-d selection: selection cleared", !repl_clipboard_sel_active());
        assert_status_contains("ctrl-d selection: status", "Deleted 3 lines");
    }

    /* 8c. Undo/redo after a multi-line delete */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        repl_feed_line_public("glVertex3f(3,3,3)");
        repl_feed_line_public("glVertex3f(4,4,4)");

        delete_cmd_range(1, 3, "Deleted");
        ASSERT_INT("delete undo setup: leaves 2 cmds", repl_state_document_count(), 2);

        repl_undo_pop_snapshot();
        ASSERT_INT("delete undo: restores 5 cmds", repl_state_document_count(), 5);
        ASSERT_STR("delete undo: middle restored", editor_buffer_line(2), "  glVertex3f(2, 2, 2);");

        repl_undo_do_redo();
        ASSERT_INT("delete redo: leaves 2 cmds", repl_state_document_count(), 2);
        ASSERT_STR("delete redo: last survives", editor_buffer_line(1), "  glVertex3f(4, 4, 4);");
    }

    /* 8d. Copy selected block and paste inside a later block */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("glBegin(GL_POINTS);");
        repl_feed_line_public("glVertex3f(0,0,0);");
        repl_feed_line_public("glVertex3f(1,1,1);");
        repl_feed_line_public("glEnd();");
        repl_feed_line_public("if(x > 0) {");
        repl_feed_line_public("glColor3f(1,0,0);");
        repl_feed_line_public("}");
        editor_state_selection_set(1, 2);

        editor_handle_key(3, 0, 0);
        ASSERT_INT("copy block: clipboard count", editor_state_clipboard_count(), 2);
        ASSERT_STR("copy block: first copied", editor_state_clipboard_mut()->lines[0], "    glVertex3f(0, 0, 0);");
        ASSERT_STR("copy block: second copied", editor_state_clipboard_mut()->lines[1], "    glVertex3f(1, 1, 1);");
        ASSERT_TRUE("copy block: selection cleared", !repl_clipboard_sel_active());

        repl_state_edit_line_set(5);
        editor_insert_mode_set(0);
        editor_handle_key(22, 0, 0);
        ASSERT_INT("paste block: cmd count", repl_state_document_count(), 9);
        ASSERT_STR("paste block: if preserved", editor_buffer_line(4), "  if(x > 0) {");
        ASSERT_STR("paste block: first inserted", editor_buffer_line(5), "    glVertex3f(0, 0, 0);");
        ASSERT_STR("paste block: second inserted", editor_buffer_line(6), "    glVertex3f(1, 1, 1);");
        ASSERT_STR("paste block: original body shifted", editor_buffer_line(7), "    glColor3f(1, 0, 0);");
        ASSERT_STR("paste block: close brace preserved", editor_buffer_line(8), "  }");
        ASSERT_INT("paste block: edit_line after pasted block", repl_state_edit_line(), 7);
        ASSERT_STR("paste block: input reloaded after paste", editor_state_input().input, "glColor3f(1, 0, 0)");
        assert_status_contains("paste block: status", "Pasted 2 lines");

        repl_undo_pop_snapshot();
        ASSERT_INT("paste undo: restores original count", repl_state_document_count(), 7);
        ASSERT_STR("paste undo: original body restored", editor_buffer_line(5), "    glColor3f(1, 0, 0);");
        ASSERT_STR("paste undo: close brace restored", editor_buffer_line(6), "  }");

        repl_undo_do_redo();
        ASSERT_INT("paste redo: restores pasted count", repl_state_document_count(), 9);
        ASSERT_STR("paste redo: first pasted line", editor_buffer_line(5), "    glVertex3f(0, 0, 0);");
        ASSERT_STR("paste redo: original body shifted again", editor_buffer_line(7), "    glColor3f(1, 0, 0);");
    }

    /* 8e. Cut selected block, then undo/redo */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        repl_feed_line_public("glVertex3f(3,3,3)");
        editor_state_selection_set(1, 2);

        editor_handle_key(24, 0, 0);
        ASSERT_INT("cut block: clipboard count", editor_state_clipboard_count(), 2);
        ASSERT_STR("cut block: first copied", editor_state_clipboard_mut()->lines[0], "  glVertex3f(1, 1, 1);");
        ASSERT_STR("cut block: second copied", editor_state_clipboard_mut()->lines[1], "  glVertex3f(2, 2, 2);");
        ASSERT_INT("cut block: leaves 2 cmds", repl_state_document_count(), 2);
        ASSERT_STR("cut block: first survivor", editor_buffer_line(0), "  glVertex3f(0, 0, 0);");
        ASSERT_STR("cut block: second survivor", editor_buffer_line(1), "  glVertex3f(3, 3, 3);");
        ASSERT_TRUE("cut block: selection cleared", !repl_clipboard_sel_active());

        repl_undo_pop_snapshot();
        ASSERT_INT("cut undo: restores 4 cmds", repl_state_document_count(), 4);
        ASSERT_STR("cut undo: first cut line restored", editor_buffer_line(1), "  glVertex3f(1, 1, 1);");

        repl_undo_do_redo();
        ASSERT_INT("cut redo: leaves 2 cmds", repl_state_document_count(), 2);
        ASSERT_STR("cut redo: second survivor", editor_buffer_line(1), "  glVertex3f(3, 3, 3);");
    }

    /* 8f. Copy/cut from a for-begin line captures the whole block */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("for(i, 0, 2) {");
        repl_feed_line_public("glVertex3f(i,0,0);");
        repl_feed_line_public("}");
        repl_state_edit_line_set(0);

        editor_handle_key(3, 0, 0);
        ASSERT_INT("copy for block: clipboard count", editor_state_clipboard_count(), 3);
        ASSERT_INT("copy for block: first type", editor_state_clipboard_cmds_mut()[0].type, CMD_FOR_BEGIN);
        ASSERT_INT("copy for block: last type", editor_state_clipboard_cmds_mut()[2].type, CMD_FOR_END);
        ASSERT_INT("copy for block: source unchanged", repl_state_document_count(), 3);

        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("for(i, 0, 2) {");
        repl_feed_line_public("glVertex3f(i,0,0);");
        repl_feed_line_public("}");
        repl_state_edit_line_set(0);

        editor_handle_key(24, 0, 0);
        ASSERT_INT("cut for block: clipboard count", editor_state_clipboard_count(), 3);
        ASSERT_INT("cut for block: first type", editor_state_clipboard_cmds_mut()[0].type, CMD_FOR_BEGIN);
        ASSERT_INT("cut for block: last type", editor_state_clipboard_cmds_mut()[2].type, CMD_FOR_END);
        ASSERT_INT("cut for block: buffer empty", repl_state_document_count(), 0);
        ASSERT_INT("cut for block: edit line at start", repl_state_edit_line(), 0);
    }

    /* 8g. Paste with empty clipboard */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        editor_state_clipboard_count_set(0);

        editor_handle_key(22, 0, 0);

        ASSERT_INT("paste empty: no mutation", repl_state_document_count(), 1);
        ASSERT_STR("paste empty: source unchanged", editor_buffer_line(0), "  glVertex3f(1, 1, 1);");
        assert_status_contains("paste empty: status", "Clipboard empty");
    }

    /* 8h. Paste refuses to overflow the command buffer */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        editor_state_clipboard_cmds_mut()[0] = repl_state_document_cmds()[0];
        editor_state_clipboard_count_set(1);
        repl_state_document_count_set(MAX_COMMANDS);

        editor_handle_key(22, 0, 0);

        ASSERT_INT("paste full: no mutation", repl_state_document_count(), MAX_COMMANDS);
        ASSERT_INT("paste full: clipboard preserved", editor_state_clipboard_count(), 1);
        assert_status_contains("paste full: status", "Command buffer full");
    }

    /* 8i. Clipboard operations reject float declarations */
    {
        repl_reset_state();
        repl_feed_line_public("float n;");
        repl_feed_line_public("n = 5;");
        editor_state_clipboard_cmds_mut()[0] = repl_state_document_cmds()[1];
        repl_copy_string_fits(editor_state_clipboard_mut()->lines[0], MAX_LINE_LEN,
                              editor_buffer_line(1));
        editor_state_clipboard_count_set(1);
        repl_state_edit_line_set(0);

        editor_handle_key(3, 0, 0);

        ASSERT_INT("copy decl line: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_INT("copy decl line: clipboard count preserved", editor_state_clipboard_count(), 1);
        ASSERT_STR("copy decl line: clipboard source preserved", editor_state_clipboard_mut()->lines[0], "  n = 5;");
        assert_status_contains("copy decl line: status", "Cannot copy float declarations");

        editor_state_clipboard_cmds_mut()[0] = repl_state_document_cmds()[0];
        editor_state_clipboard_count_set(1);
        repl_state_edit_line_set(repl_state_document_count());

        editor_handle_key(22, 0, 0);

        ASSERT_INT("paste decl line: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_INT("paste decl line: clipboard count preserved", editor_state_clipboard_count(), 1);
        ASSERT_INT("paste decl line: first still decl", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_INT("paste decl line: second still assign", repl_state_document_cmds_mut()[1].type, CMD_VAR_ASSIGN);
        assert_status_contains("paste decl line: status", "Cannot paste float declarations");
    }

    /* 8j. Copy/cut in insert mode clear selection without touching clipboard */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        editor_state_clipboard_cmds_mut()[0] = repl_state_document_cmds()[1];
        repl_copy_string_fits(editor_state_clipboard_mut()->lines[0], MAX_LINE_LEN,
                              editor_buffer_line(1));
        editor_state_clipboard_count_set(1);
        editor_state_selection_set(0, 1);
        repl_state_edit_line_set(1);
        editor_insert_mode_set(1);

        editor_handle_key(3, 0, 0);
        ASSERT_INT("copy insert mode: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_INT("copy insert mode: clipboard unchanged", editor_state_clipboard_count(), 1);
        ASSERT_STR("copy insert mode: clipboard source unchanged", editor_state_clipboard_mut()->lines[0], "  glVertex3f(2, 2, 2);");
        ASSERT_TRUE("copy insert mode: selection cleared", !repl_clipboard_sel_active());

        editor_state_selection_set(0, 1);
        editor_insert_mode_set(1);
        editor_handle_key(24, 0, 0);
        ASSERT_INT("cut insert mode: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_INT("cut insert mode: clipboard unchanged", editor_state_clipboard_count(), 1);
        ASSERT_STR("cut insert mode: clipboard source unchanged", editor_state_clipboard_mut()->lines[0], "  glVertex3f(2, 2, 2);");
        ASSERT_TRUE("cut insert mode: selection cleared", !repl_clipboard_sel_active());
    }

    /* 8k. Backspace in insert mode edits input instead of selected source lines */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        editor_state_selection_set(0, 1);
        repl_state_edit_line_set(1);
        editor_insert_mode_set(1);
        set_editor_input("abcd");
        editor_cursor_pos_set(3);

        editor_handle_key(8, 0, 0);

        ASSERT_INT("backspace insert mode: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_STR("backspace insert mode: input edited", editor_state_input().input, "abd");
        ASSERT_INT("backspace insert mode: cursor moved", editor_cursor_pos(), 2);
        ASSERT_INT("backspace insert mode: still inserting", editor_insert_mode(), 1);
    }

    /* 8k. Committing incomplete commands reports incomplete, not unknown */
    {
        repl_reset_state();
        set_editor_input("glColor3f(1, 1");

        editor_handle_key(';', 0, 0);

        ASSERT_INT("commit incomplete glColor3f: no cmd added", repl_state_document_count(), 0);
        assert_status_contains("commit incomplete glColor3f: status", "Incomplete command");
        assert_status_contains("commit incomplete glColor3f: missing paren", "missing ')'");
        ASSERT_TRUE("commit incomplete glColor3f: not unknown",
                    strstr(g_status, "Unknown cmd") == NULL);
    }

    {
        repl_reset_state();
        set_editor_input("glTotallyUnknown(1, 2, 3)");

        editor_handle_key(';', 0, 0);

        ASSERT_INT("commit unknown command: no cmd added", repl_state_document_count(), 0);
        assert_status_contains("commit unknown command: status", "Unknown cmd");
    }

    /* 9. load_line_to_input - CMD_LABEL path */
    {
        repl_reset_state();
        /* Feed a label command to create a CMD_LABEL entry */
        repl_feed_line_public(":myloop");
        ASSERT_INT("label cmd created", repl_state_document_count(), 1);
        ASSERT_INT("label cmd type", repl_state_document_cmds_mut()[0].type, CMD_LABEL);

        /* Now load it back into input */
        load_line_to_input(0);
        /* Input should start with ':' and contain the label name */
        ASSERT_TRUE("label load: starts with colon", editor_state_input().input[0] == ':');
        ASSERT_TRUE("label load: contains name", strstr(editor_state_input().input, "myloop") != NULL);
    }

    /* 10. navigate_to_line - clamp target < 0 */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        navigate_to_line(-5);
        ASSERT_INT("navigate_to_line neg: edit_line=0", repl_state_edit_line(), 0);
    }

    /* 11. navigate_to_line - clamp target > repl_state_document_count() */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        navigate_to_line(999);
        ASSERT_INT("navigate_to_line over: edit_line=repl_state_document_count()", repl_state_edit_line(), repl_state_document_count());
    }

    /* 12. try_assign_variable - append to end (existing tests cover this) */
    {
        repl_reset_state(); declare_test_vars();
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "n = 10.5");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = try_assign_variable();
        ASSERT_INT("try_assign_variable returns 1", r, 1);
        ASSERT_INT("num_cmds 1 after assign", repl_state_document_count(), 1);
        ASSERT_STR("assigned cmd source", editor_buffer_line(0), "  n = 10.5;");
    }

    /* 13. try_assign_variable - inserting mode (inserts before cursor) */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        /* Put cursor at line 1, inserting mode */
        repl_state_edit_line_set(1);
        editor_insert_mode_set(1);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "x = 3.0");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = try_assign_variable();
        ASSERT_INT("insert assign returns 1", r, 1);
        ASSERT_INT("insert assign: 3 cmds", repl_state_document_count(), 3);
        /* The assignment should appear at index 1 */
        ASSERT_INT("insert assign: cmd 1 is VAR_ASSIGN", repl_state_document_cmds_mut()[1].type, CMD_VAR_ASSIGN);
    }

    /* 14. try_assign_variable - overwrite existing cmd */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("n = 1.0");
        repl_feed_line_public("glVertex3f(1,1,1)");
        /* Navigate to the assignment line and overwrite */
        repl_state_edit_line_set(0);
        editor_insert_mode_set(0);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "n = 7.0");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = try_assign_variable();
        ASSERT_INT("overwrite assign returns 1", r, 1);
        ASSERT_STR("overwrite assign: new source", editor_buffer_line(0), "  n = 7.0;");
        ASSERT_INT("overwrite assign: edit_line advanced", repl_state_edit_line(), 1);
    }

    /* 15. Committing for loops - basic open brace form */
    {
        repl_reset_state(); declare_test_vars();
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "for(i, 0, 5) {");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = try_commit_for_loop();
        ASSERT_INT("try_commit_for_loop returns 1", r, 1);
        ASSERT_INT("num_cmds 2 after for", repl_state_document_count(), 2);
        ASSERT_STR("for loop source", editor_buffer_line(0), "  for(i, 0, 5) {");
        ASSERT_STR("for end source", editor_buffer_line(1), "  }");
    }

    /* 16. try_commit_for_loop - with explicit step */
    {
        repl_reset_state();
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "for(i, 0, 10, 2) {");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = try_commit_for_loop();
        ASSERT_INT("for with step returns 1", r, 1);
        ASSERT_INT("for with step: 2 cmds", repl_state_document_count(), 2);
        /* Source should contain step value */
        ASSERT_TRUE("for with step: source has 2 step",
                    strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "", "2") != NULL);
    }

    /* 17. try_commit_for_loop - update existing for-begin header */
    {
        repl_reset_state();
        /* Create an existing for-loop */
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "for(i, 0, 5) {");
            inp->input_len = (int)strlen(inp->input);
        }
        try_commit_for_loop();
        ASSERT_INT("setup: 2 cmds", repl_state_document_count(), 2);

        /* Navigate back to line 0 and update the for header */
        repl_state_edit_line_set(0);
        editor_insert_mode_set(0);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "for(i, 0, 10) {");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = try_commit_for_loop();
        ASSERT_INT("update for-begin returns 1", r, 1);
        /* Should update in place, not add more commands */
        ASSERT_INT("update for-begin: still 2 cmds", repl_state_document_count(), 2);
        ASSERT_TRUE("update for-begin: source updated",
                    strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "", "10") != NULL);
    }

    /* 18. try_commit_for_loop - inline form (for with body on same line) */
    {
        repl_reset_state();
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "for(i, 0, 3) glVertex3f(i,0,0);");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = try_commit_for_loop();
        ASSERT_INT("for inline returns 1", r, 1);
        /* Should create 3 cmds: for-begin, body, for-end */
        ASSERT_INT("for inline: 3 cmds", repl_state_document_count(), 3);
        ASSERT_INT("for inline: cmd 0 is FOR_BEGIN", repl_state_document_cmds_mut()[0].type, CMD_FOR_BEGIN);
        ASSERT_INT("for inline: cmd 2 is FOR_END", repl_state_document_cmds_mut()[2].type, CMD_FOR_END);
    }

    /* 19. Committing func defs - basic */
    {
        repl_reset_state(); declare_test_vars();
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "func0(x, y) {");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = try_commit_func_def();
        ASSERT_INT("try_commit_func_def returns 1", r, 1);
        ASSERT_INT("num_cmds 2 after func", repl_state_document_count(), 2);
        ASSERT_STR("func def source", editor_buffer_line(0), "  func0(x, y) {");
        ASSERT_STR("func end source", editor_buffer_line(1), "  }");
    }

    /* 20. try_commit_func_def - update existing func-def header */
    {
        repl_reset_state();
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "func1(a) {");
            inp->input_len = (int)strlen(inp->input);
        }
        try_commit_func_def();
        ASSERT_INT("func update setup: 2 cmds", repl_state_document_count(), 2);

        /* Navigate back and update the func-def header */
        repl_state_edit_line_set(0);
        editor_insert_mode_set(0);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "func1(a, b) {");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = try_commit_func_def();
        ASSERT_INT("update func-def returns 1", r, 1);
        ASSERT_INT("update func-def: still 2 cmds", repl_state_document_count(), 2);
        ASSERT_TRUE("update func-def: source has b param",
                    strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "", "b") != NULL);
    }

    /* 21. Committing if blocks - basic */
    {
        repl_reset_state(); declare_test_vars();
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "if(x > 0) {");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = try_commit_if_block();
        ASSERT_INT("try_commit_if_block returns 1", r, 1);
        ASSERT_INT("num_cmds 2 after if", repl_state_document_count(), 2);
        ASSERT_STR("if block source", editor_buffer_line(0), "  if(x > 0) {");
        ASSERT_STR("if end source", editor_buffer_line(1), "  }");
    }

    /* 22. try_commit_if_block - update existing if-begin */
    {
        repl_reset_state(); declare_test_vars();
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "if(x > 0) {");
            inp->input_len = (int)strlen(inp->input);
        }
        try_commit_if_block();

        /* Navigate back and update the if condition */
        repl_state_edit_line_set(0);
        editor_insert_mode_set(0);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "if(x < 0) {");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = try_commit_if_block();
        ASSERT_INT("update if-begin returns 1", r, 1);
        ASSERT_INT("update if-begin: still 2 cmds", repl_state_document_count(), 2);
        ASSERT_STR("update if-begin: source updated", editor_buffer_line(0), "  if(x < 0) {");
    }

    /* 23. Committing close brace - for-loop */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("for(i, 0, 1) {");
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "}");
            inp->input_len = (int)strlen(inp->input);
        }
        repl_state_edit_line_set(1);

        int r = try_commit_close_brace();
        ASSERT_INT("try_commit_close_brace returns 1", r, 1);
        ASSERT_INT("num_cmds after brace", repl_state_document_count(), 2);
        ASSERT_STR("close brace source", editor_buffer_line(1), "  }");
    }

    /* 26. Committing close brace - while in inserting mode closes existing end */
    {
        repl_reset_state();
        /* Set up: for-loop with begin+end, enter inserting mode inside */
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "for(i, 0, 3) {");
            inp->input_len = (int)strlen(inp->input);
        }
        try_commit_for_loop();
        /* repl_state_edit_line()=1, editor_insert_mode()=1 - we're inside the loop */
        ASSERT_INT("insert-close setup: in inserting", editor_insert_mode(), 1);
        ASSERT_INT("insert-close setup: edit_line=1", repl_state_edit_line(), 1);

        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "}");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = try_commit_close_brace();
        ASSERT_INT("insert-close brace returns 1", r, 1);
        ASSERT_INT("insert-close: no longer inserting", editor_insert_mode(), 0);
    }

    /* 27. Committing close brace - func-def */
    {
        repl_reset_state();
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "func2() {");
            inp->input_len = (int)strlen(inp->input);
        }
        try_commit_func_def();
        /* Now close the func with '}' */
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "}");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = try_commit_close_brace();
        ASSERT_INT("func close brace returns 1", r, 1);
        ASSERT_INT("func close: no longer inserting", editor_insert_mode(), 0);
    }

    /* 28. Committing close brace - if-block */
    {
        repl_reset_state(); declare_test_vars();
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "if(x > 0) {");
            inp->input_len = (int)strlen(inp->input);
        }
        try_commit_if_block();
        /* Inserting inside if-block, close it */
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "}");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = try_commit_close_brace();
        ASSERT_INT("if close brace returns 1", r, 1);
        ASSERT_INT("if close: no longer inserting", editor_insert_mode(), 0);
    }

    /* 29. try_commit_for_loop - empty body emits error */
    {
        repl_reset_state();
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "for(i, 0, 3) ;");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = try_commit_for_loop();
        ASSERT_INT("for empty body returns 1 (error)", r, 1);
        /* No commands committed on bad body */
        ASSERT_INT("for empty body: no cmds added", repl_state_document_count(), 0);
    }

    /* 30. try_commit_func_def - func with no params */
    {
        repl_reset_state();
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strcpy(inp->input, "func3() {");
            inp->input_len = (int)strlen(inp->input);
        }

        int r = try_commit_func_def();
        ASSERT_INT("func no-params returns 1", r, 1);
        ASSERT_INT("func no-params: 2 cmds", repl_state_document_count(), 2);
        ASSERT_TRUE("func no-params: source correct",
                    strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "", "func3") != NULL);
    }

    /* 24. prof_frame_tick - increments staleness counters */
    {
        /* Call begin/end to prime a section, then tick several frames */
        prof_begin(PROF_SCENE_3D);
        prof_end(PROF_SCENE_3D);
        /* Frame tick - sections that didn't run this frame become stale */
        prof_frame_tick();
        prof_frame_tick();
        /* Should not crash; calling multiple times is fine */
        ASSERT_TRUE("prof_frame_tick survives", 1);
    }

    /* ---- Float declaration parsing (regression tests) ---- */

    /* 26. float decl without trailing semicolon (interactive ';' key path) */
    {
        repl_reset_state();

        /* Simulate the interactive ';' key handler: editor_state_input().input has no ';' */
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strncpy(inp->input, "float tmp", MAX_INPUT_LEN - 1);
            inp->input[MAX_INPUT_LEN - 1] = '\0';
            inp->input_len = (int)strlen(inp->input);
            editor_cursor_pos_set(inp->input_len);
        }
        repl_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(0);

        int result = try_commit_float_decl();
        ASSERT_INT("float_decl no-semi: accepted", result, 1);
        ASSERT_INT("float_decl no-semi: cmd added", repl_state_document_count(), 1);
        ASSERT_INT("float_decl no-semi: type", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_INT("float_decl no-semi: var_decl_count", repl_state_document_cmds_mut()[0].var_decl_count, 1);
        ASSERT_STR("float_decl no-semi: var name", repl_state_document_cmds_mut()[0].var_names[0], "tmp");
        ASSERT_TRUE("float_decl no-semi: predef registered",
                     repl_eval_find_predef_var_idx("tmp") >= 0);
    }

    /* 27. float decl WITH trailing semicolon (feed_line path) */
    {
        repl_reset_state();
        repl_feed_line_public("float abc;");
        ASSERT_INT("float_decl with-semi: cmd added", repl_state_document_count(), 1);
        ASSERT_INT("float_decl with-semi: type", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_STR("float_decl with-semi: var name", repl_state_document_cmds_mut()[0].var_names[0], "abc");
        ASSERT_TRUE("float_decl with-semi: predef registered",
                     repl_eval_find_predef_var_idx("abc") >= 0);
    }

    /* 28. float decl with initializer, no trailing semicolon */
    {
        repl_reset_state();

        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strncpy(inp->input, "float tmp = 0", MAX_INPUT_LEN - 1);
            inp->input[MAX_INPUT_LEN - 1] = '\0';
            inp->input_len = (int)strlen(inp->input);
            editor_cursor_pos_set(inp->input_len);
        }
        repl_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(0);

        int result = try_commit_float_decl();
        ASSERT_INT("float_decl init no-semi: accepted", result, 1);
        ASSERT_INT("float_decl init no-semi: cmd added", repl_state_document_count(), 1);
        ASSERT_INT("float_decl init no-semi: type", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_STR("float_decl init no-semi: var name", repl_state_document_cmds_mut()[0].var_names[0], "tmp");
        int idx = repl_eval_find_predef_var_idx("tmp");
        ASSERT_TRUE("float_decl init no-semi: predef registered", idx >= 0);
        if (idx >= 0)
            ASSERT_TRUE("float_decl init no-semi: value is 0",
                         g_predef_vars[idx].value == 0.0f);
    }

    /* 29. float decl with initializer expression */
    {
        repl_reset_state();
        repl_feed_line_public("float radius = 2.5;");
        ASSERT_INT("float_decl init expr: cmd added", repl_state_document_count(), 1);
        ASSERT_INT("float_decl init expr: type", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_STR("float_decl init expr: var name", repl_state_document_cmds_mut()[0].var_names[0], "radius");
        int idx = repl_eval_find_predef_var_idx("radius");
        ASSERT_TRUE("float_decl init expr: registered", idx >= 0);
        if (idx >= 0)
            ASSERT_TRUE("float_decl init expr: value is 2.5",
                         g_predef_vars[idx].value == 2.5f);
    }

    /* 30. multi-name float decl without semicolon */
    {
        repl_reset_state();

        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strncpy(inp->input, "float a, b, c", MAX_INPUT_LEN - 1);
            inp->input[MAX_INPUT_LEN - 1] = '\0';
            inp->input_len = (int)strlen(inp->input);
            editor_cursor_pos_set(inp->input_len);
        }
        repl_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(0);

        int result = try_commit_float_decl();
        ASSERT_INT("float_decl multi no-semi: accepted", result, 1);
        ASSERT_INT("float_decl multi no-semi: cmd added", repl_state_document_count(), 1);
        ASSERT_INT("float_decl multi no-semi: var_decl_count",
                   repl_state_document_cmds_mut()[0].var_decl_count, 3);
        ASSERT_TRUE("float_decl multi no-semi: a registered",
                     repl_eval_find_predef_var_idx("a") >= 0);
        ASSERT_TRUE("float_decl multi no-semi: b registered",
                     repl_eval_find_predef_var_idx("b") >= 0);
        ASSERT_TRUE("float_decl multi no-semi: c registered",
                     repl_eval_find_predef_var_idx("c") >= 0);
    }

    /* 31. multi-name float decl with initializers */
    {
        repl_reset_state();
        repl_feed_line_public("float x = 1, y = 2;");
        ASSERT_INT("float_decl multi init: cmd added", repl_state_document_count(), 1);
        ASSERT_INT("float_decl multi init: var_decl_count",
                   repl_state_document_cmds_mut()[0].var_decl_count, 2);
        int xi = repl_eval_find_predef_var_idx("x");
        int yi = repl_eval_find_predef_var_idx("y");
        ASSERT_TRUE("float_decl multi init: x registered", xi >= 0);
        ASSERT_TRUE("float_decl multi init: y registered", yi >= 0);
        if (xi >= 0)
            ASSERT_TRUE("float_decl multi init: x value is 1",
                         g_predef_vars[xi].value == 1.0f);
        if (yi >= 0)
            ASSERT_TRUE("float_decl multi init: y value is 2",
                         g_predef_vars[yi].value == 2.0f);
    }

    /* 32. float decl inserted above existing non-decl commands.
     * Regression: previously `float tmp = 40;` committed after
     * `n = tmp;` could land below its reference, producing
     *     n = tmp;
     *     float tmp = 40;
     * which is semantically wrong. New behavior pushes decls to
     * the top of non-decl code. */
    {
        repl_reset_state();
        repl_feed_line_public("float n;");
        repl_feed_line_public("n = 5;");
        repl_feed_line_public("glBegin(GL_POINTS);");
        ASSERT_INT("decl-top baseline: 3 cmds", repl_state_document_count(), 3);

        repl_feed_line_public("float tmp = 40;");
        ASSERT_INT("decl-top: 4 cmds after float tmp", repl_state_document_count(), 4);
        ASSERT_INT("decl-top: repl_state_document_cmds_mut()[0] is float n", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_STR("decl-top: repl_state_document_cmds_mut()[0] name", repl_state_document_cmds_mut()[0].var_names[0], "n");
        ASSERT_INT("decl-top: repl_state_document_cmds_mut()[1] is float tmp", repl_state_document_cmds_mut()[1].type, CMD_VAR_DECLARE);
        ASSERT_STR("decl-top: repl_state_document_cmds_mut()[1] name", repl_state_document_cmds_mut()[1].var_names[0], "tmp");
        ASSERT_INT("decl-top: repl_state_document_cmds_mut()[2] is assign", repl_state_document_cmds_mut()[2].type, CMD_VAR_ASSIGN);
        ASSERT_INT("decl-top: repl_state_document_cmds_mut()[3] is glBegin", repl_state_document_cmds_mut()[3].type, CMD_BEGIN);
    }

    /* 33. float decl from edit position in middle of code still
     * lands at the top (not at the cursor). */
    {
        repl_reset_state();
        repl_feed_line_public("glBegin(GL_LINES);");
        repl_feed_line_public("glEnd();");
        ASSERT_INT("decl-top mid: 2 cmds", repl_state_document_count(), 2);

        /* Simulate being on line 1 in overwrite mode when committing
         * a float decl through the ';' key (no trailing ';'). */
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strncpy(inp->input, "float radius = 3", MAX_INPUT_LEN - 1);
            inp->input[MAX_INPUT_LEN - 1] = '\0';
            inp->input_len = (int)strlen(inp->input);
            editor_cursor_pos_set(inp->input_len);
        }
        repl_state_edit_line_set(1);
        editor_insert_mode_set(0);

        int result = try_commit_float_decl();
        ASSERT_INT("decl-top mid: accepted", result, 1);
        ASSERT_INT("decl-top mid: 3 cmds", repl_state_document_count(), 3);
        ASSERT_INT("decl-top mid: repl_state_document_cmds_mut()[0] is decl",
                   repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_STR("decl-top mid: repl_state_document_cmds_mut()[0] name",
                   repl_state_document_cmds_mut()[0].var_names[0], "radius");
        ASSERT_INT("decl-top mid: repl_state_document_cmds_mut()[1] preserved glBegin",
                   repl_state_document_cmds_mut()[1].type, CMD_BEGIN);
        ASSERT_INT("decl-top mid: repl_state_document_cmds_mut()[2] preserved glEnd",
                   repl_state_document_cmds_mut()[2].type, CMD_END);
    }

    /* 34. Overwriting a multi-name decl that shares a name with the new
     * decl must not fail with "already declared". Regression: previously
     * `declare_predef_var` ran before `undeclare_predef_var`, so shared
     * names (still registered from the old decl) collided. */
    {
        repl_reset_state();
        repl_feed_line_public("float a, b;");
        ASSERT_INT("overwrite shared: baseline cmd count", repl_state_document_count(), 1);
        ASSERT_TRUE("overwrite shared: a registered",
                    repl_eval_find_predef_var_idx("a") >= 0);
        ASSERT_TRUE("overwrite shared: b registered",
                    repl_eval_find_predef_var_idx("b") >= 0);

        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strncpy(inp->input, "float a, c", MAX_INPUT_LEN - 1);
            inp->input[MAX_INPUT_LEN - 1] = '\0';
            inp->input_len = (int)strlen(inp->input);
            editor_cursor_pos_set(inp->input_len);
        }
        repl_state_edit_line_set(0);
        editor_insert_mode_set(0);

        int result = try_commit_float_decl();
        ASSERT_INT("overwrite shared: accepted", result, 1);
        ASSERT_INT("overwrite shared: still 1 cmd", repl_state_document_count(), 1);
        ASSERT_INT("overwrite shared: var_decl_count", repl_state_document_cmds_mut()[0].var_decl_count, 2);
        ASSERT_STR("overwrite shared: slot 0 is a", repl_state_document_cmds_mut()[0].var_names[0], "a");
        ASSERT_STR("overwrite shared: slot 1 is c", repl_state_document_cmds_mut()[0].var_names[1], "c");
        ASSERT_TRUE("overwrite shared: a still registered",
                    repl_eval_find_predef_var_idx("a") >= 0);
        ASSERT_TRUE("overwrite shared: c registered",
                    repl_eval_find_predef_var_idx("c") >= 0);
        ASSERT_TRUE("overwrite shared: b unregistered",
                    repl_eval_find_predef_var_idx("b") < 0);
    }

    /* 35. Overwriting `float n, b;` → `float n;` with `n` referenced
     * elsewhere must succeed (only `b` is being dropped, and `b` is not
     * referenced). Regression: previously the check iterated ALL old
     * names and errored on `n is in use` even though `n` is being kept. */
    {
        repl_reset_state();
        repl_feed_line_public("float n, b;");
        repl_feed_line_public("n = 5;");
        ASSERT_INT("drop b: baseline cmd count", repl_state_document_count(), 2);

        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strncpy(inp->input, "float n", MAX_INPUT_LEN - 1);
            inp->input[MAX_INPUT_LEN - 1] = '\0';
            inp->input_len = (int)strlen(inp->input);
            editor_cursor_pos_set(inp->input_len);
        }
        repl_state_edit_line_set(0);
        editor_insert_mode_set(0);

        int result = try_commit_float_decl();
        ASSERT_INT("drop b: accepted", result, 1);
        ASSERT_INT("drop b: var_decl_count now 1", repl_state_document_cmds_mut()[0].var_decl_count, 1);
        ASSERT_STR("drop b: slot 0 is n", repl_state_document_cmds_mut()[0].var_names[0], "n");
        ASSERT_TRUE("drop b: n still registered",
                    repl_eval_find_predef_var_idx("n") >= 0);
        ASSERT_TRUE("drop b: b unregistered",
                    repl_eval_find_predef_var_idx("b") < 0);
    }

    /* 36. Attempting to drop a name that IS referenced elsewhere must
     * fail, and the error must name the dropped variable (not a
     * different name in the same decl). */
    {
        repl_reset_state();
        repl_feed_line_public("float n, b;");
        repl_feed_line_public("b = 7;");
        ASSERT_INT("drop referenced b: baseline", repl_state_document_count(), 2);

        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strncpy(inp->input, "float n", MAX_INPUT_LEN - 1);
            inp->input[MAX_INPUT_LEN - 1] = '\0';
            inp->input_len = (int)strlen(inp->input);
            editor_cursor_pos_set(inp->input_len);
        }
        repl_state_edit_line_set(0);
        editor_insert_mode_set(0);

        int result = try_commit_float_decl();
        ASSERT_INT("drop referenced b: handler consumed input", result, 1);
        /* Overwrite must have been rejected: decl still has both names */
        ASSERT_INT("drop referenced b: decl unchanged",
                   repl_state_document_cmds_mut()[0].var_decl_count, 2);
        ASSERT_STR("drop referenced b: b preserved",
                   repl_state_document_cmds_mut()[0].var_names[1], "b");
        ASSERT_TRUE("drop referenced b: status mentions 'b'",
                    strstr(g_status, "'b'") != NULL);
        ASSERT_TRUE("drop referenced b: status does not mention 'n'",
                    strstr(g_status, "'n'") == NULL);
    }

    /* 37. Deleting a declaration line is rejected */
    {
        repl_reset_state();
        repl_feed_line_public("float n;");
        repl_feed_line_public("n = 5;");
        ASSERT_TRUE("delete referenced decl setup: n registered",
                    repl_eval_find_predef_var_idx("n") >= 0);

        delete_cmd_range(0, 1, "Deleted");

        ASSERT_INT("delete referenced decl: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_INT("delete referenced decl: first still decl", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_INT("delete referenced decl: second still assign", repl_state_document_cmds_mut()[1].type, CMD_VAR_ASSIGN);
        ASSERT_TRUE("delete referenced decl: n still registered",
                    repl_eval_find_predef_var_idx("n") >= 0);
        assert_status_contains("delete referenced decl: status", "Cannot remove float declarations");
    }

    /* 37a. Ctrl+X rejects cutting declaration lines */
    {
        repl_reset_state();
        repl_feed_line_public("float n;");
        repl_feed_line_public("n = 5;");
        editor_state_clipboard_cmds_mut()[0] = repl_state_document_cmds()[1];
        repl_copy_string_fits(editor_state_clipboard_mut()->lines[0], MAX_LINE_LEN,
                              editor_buffer_line(1));
        editor_state_clipboard_count_set(1);
        repl_state_edit_line_set(0);

        editor_handle_key(24, 0, 0);

        ASSERT_INT("cut referenced decl: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_INT("cut referenced decl: first still decl", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_INT("cut referenced decl: second still assign", repl_state_document_cmds_mut()[1].type, CMD_VAR_ASSIGN);
        ASSERT_TRUE("cut referenced decl: n still registered",
                    repl_eval_find_predef_var_idx("n") >= 0);
        ASSERT_INT("cut referenced decl: clipboard count preserved", editor_state_clipboard_count(), 1);
        ASSERT_STR("cut referenced decl: clipboard source preserved", editor_state_clipboard_mut()->lines[0], "  n = 5;");
        assert_status_contains("cut referenced decl: status", "Cannot remove float declarations");
    }

    /* 37b. Ctrl+X rejects blocks that include a declaration and all uses */
    {
        repl_reset_state();
        repl_feed_line_public("float n;");
        repl_feed_line_public("n = 5;");
        ASSERT_TRUE("cut decl block setup: n registered",
                    repl_eval_find_predef_var_idx("n") >= 0);
        editor_state_clipboard_cmds_mut()[0] = repl_state_document_cmds()[1];
        repl_copy_string_fits(editor_state_clipboard_mut()->lines[0], MAX_LINE_LEN,
                              editor_buffer_line(1));
        editor_state_clipboard_count_set(1);
        editor_state_selection_set(0, 1);

        editor_handle_key(24, 0, 0);

        ASSERT_INT("cut decl block: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_TRUE("cut decl block: n still registered",
                    repl_eval_find_predef_var_idx("n") >= 0);
        ASSERT_INT("cut decl block: clipboard count preserved", editor_state_clipboard_count(), 1);
        ASSERT_STR("cut decl block: clipboard source preserved", editor_state_clipboard_mut()->lines[0], "  n = 5;");
        assert_status_contains("cut decl block: status", "Cannot remove float declarations");
    }

    /* 38. Deleting a declaration with all its uses is still rejected */
    {
        repl_reset_state();
        repl_feed_line_public("float n;");
        repl_feed_line_public("n = 5;");
        ASSERT_TRUE("delete decl block setup: n registered",
                    repl_eval_find_predef_var_idx("n") >= 0);

        delete_cmd_range(0, 2, "Deleted");

        ASSERT_INT("delete decl block: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_TRUE("delete decl block: n still registered",
                    repl_eval_find_predef_var_idx("n") >= 0);
        assert_status_contains("delete decl block: status", "Cannot remove float declarations");
    }

    /* 39. Per-declaration name limit rejects atomically */
    {
        repl_reset_state();
        set_editor_input("float v0, v1, v2, v3, v4, v5, v6, v7, v8");
        repl_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(0);

        int result = try_commit_float_decl();

        ASSERT_INT("decl per-line overflow: handler consumed input", result, 1);
        ASSERT_INT("decl per-line overflow: no cmd added", repl_state_document_count(), 0);
        ASSERT_INT("decl per-line overflow: only t registered", g_num_predef_vars, 1);
        for (int i = 0; i <= MAX_NAMES_PER_DECL; i++) {
            char name[16];
            char label[128];
            snprintf(name, sizeof(name), "v%d", i);
            snprintf(label, sizeof(label), "decl per-line overflow: %s not registered", name);
            ASSERT_TRUE(label, repl_eval_find_predef_var_idx(name) < 0);
        }
        assert_status_contains("decl per-line overflow: status count", "too many names per declaration");
        assert_status_contains("decl per-line overflow: status max", "max 8");
    }

    /* 40. Total variable table limit rejects atomically */
    {
        repl_reset_state();
        repl_feed_line_public("float v0, v1, v2, v3, v4, v5, v6, v7;");
        repl_feed_line_public("float v8, v9, v10, v11, v12, v13, v14;");
        ASSERT_INT("decl table full setup: two decl cmds", repl_state_document_count(), 2);
        ASSERT_INT("decl table full setup: all slots used", g_num_predef_vars, MAX_PREDEF_VARS);

        set_editor_input("float overflow");
        repl_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(0);
        int result = try_commit_float_decl();

        ASSERT_INT("decl table full: handler consumed input", result, 1);
        ASSERT_INT("decl table full: cmd count unchanged", repl_state_document_count(), 2);
        ASSERT_INT("decl table full: var count unchanged", g_num_predef_vars, MAX_PREDEF_VARS);
        ASSERT_TRUE("decl table full: overflow not registered",
                    repl_eval_find_predef_var_idx("overflow") < 0);
        ASSERT_TRUE("decl table full: first existing var remains",
                    repl_eval_find_predef_var_idx("v0") >= 0);
        ASSERT_TRUE("decl table full: last existing var remains",
                    repl_eval_find_predef_var_idx("v14") >= 0);
        assert_status_contains("decl table full: status full", "variable table full");
        assert_status_contains("decl table full: status max", "max 16");
    }

    /* 41. Declaration validation failures are atomic */
    {
        repl_reset_state();
        repl_feed_line_public("float anchor;");
        ASSERT_INT("decl atomic setup: one decl", repl_state_document_count(), 1);
        ASSERT_TRUE("decl atomic setup: anchor registered",
                    repl_eval_find_predef_var_idx("anchor") >= 0);

        assert_float_decl_rejected_atomic("decl duplicate", "float dup, dup",
                                          "dup", "duplicate name 'dup'");
        assert_float_decl_rejected_atomic("decl reserved", "float sin",
                                          "sin", "'sin' is reserved");
        assert_float_decl_rejected_atomic("decl invalid", "float 1bad",
                                          "1bad", "expected identifier");
        assert_float_decl_rejected_atomic("decl overlong", "float abcdefghijklmnop",
                                          "abcdefghijklmnop", "max 15");

        ASSERT_INT("decl atomic: only anchor cmd remains", repl_state_document_count(), 1);
        ASSERT_INT("decl atomic: only t plus anchor registered", g_num_predef_vars, 2);
        ASSERT_TRUE("decl atomic: anchor still registered",
                    repl_eval_find_predef_var_idx("anchor") >= 0);
    }

    /* 42. Expanding a multi-name decl (float a,b,c → float a,b,c,d) must
     * preserve the live values of kept variables. Regression: the old
     * overwrite path undeclared ALL names then re-declared them with
     * value 0.0f, silently zeroing a, b, and c. */
    {
        repl_reset_state();
        repl_feed_line_public("float a, b, c;");
        repl_feed_line_public("a = 1;");
        repl_feed_line_public("b = 2;");
        repl_feed_line_public("c = 3;");
        ASSERT_INT("expand decl: baseline cmd count", repl_state_document_count(), 4);

        int ai = repl_eval_find_predef_var_idx("a");
        int bi = repl_eval_find_predef_var_idx("b");
        int ci = repl_eval_find_predef_var_idx("c");
        ASSERT_TRUE("expand decl: a registered before", ai >= 0);
        ASSERT_TRUE("expand decl: b registered before", bi >= 0);
        ASSERT_TRUE("expand decl: c registered before", ci >= 0);
        if (ai >= 0) ASSERT_TRUE("expand decl: a value is 1 before",
                                  g_predef_vars[ai].value == 1.0f);
        if (bi >= 0) ASSERT_TRUE("expand decl: b value is 2 before",
                                  g_predef_vars[bi].value == 2.0f);
        if (ci >= 0) ASSERT_TRUE("expand decl: c value is 3 before",
                                  g_predef_vars[ci].value == 3.0f);

        /* Overwrite decl to add d */
        extern int try_commit_float_decl(void);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            strncpy(inp->input, "float a, b, c, d", MAX_INPUT_LEN - 1);
            inp->input[MAX_INPUT_LEN - 1] = '\0';
            inp->input_len = (int)strlen(inp->input);
            editor_cursor_pos_set(inp->input_len);
        }
        repl_state_edit_line_set(0);
        editor_insert_mode_set(0);

        int result = try_commit_float_decl();
        ASSERT_INT("expand decl: accepted", result, 1);
        ASSERT_INT("expand decl: still 4 cmds", repl_state_document_count(), 4);
        ASSERT_INT("expand decl: var_decl_count is 4", repl_state_document_cmds_mut()[0].var_decl_count, 4);

        ai = repl_eval_find_predef_var_idx("a");
        bi = repl_eval_find_predef_var_idx("b");
        ci = repl_eval_find_predef_var_idx("c");
        int di = repl_eval_find_predef_var_idx("d");
        ASSERT_TRUE("expand decl: a still registered", ai >= 0);
        ASSERT_TRUE("expand decl: b still registered", bi >= 0);
        ASSERT_TRUE("expand decl: c still registered", ci >= 0);
        ASSERT_TRUE("expand decl: d newly registered", di >= 0);

        if (ai >= 0)
            ASSERT_TRUE("expand decl: a value preserved (1)",
                        g_predef_vars[ai].value == 1.0f);
        if (bi >= 0)
            ASSERT_TRUE("expand decl: b value preserved (2)",
                        g_predef_vars[bi].value == 2.0f);
        if (ci >= 0)
            ASSERT_TRUE("expand decl: c value preserved (3)",
                        g_predef_vars[ci].value == 3.0f);
        if (di >= 0)
            ASSERT_TRUE("expand decl: d value is 0",
                        g_predef_vars[di].value == 0.0f);

        /* CMD_VAR_ASSIGN slot refs must still point to the right variables */
        ASSERT_INT("expand decl: assign a slot correct", repl_state_document_cmds_mut()[1].num_args, ai);
        ASSERT_INT("expand decl: assign b slot correct", repl_state_document_cmds_mut()[2].num_args, bi);
        ASSERT_INT("expand decl: assign c slot correct", repl_state_document_cmds_mut()[3].num_args, ci);
    }

    /* try_assign_variable - overlong formatted source is rejected with
     * "Command too long" and does not mutate repl_state_document_cmds_mut() / repl_state_document_count(). */
    {
        repl_reset_state(); declare_test_vars();
        int old_num_cmds = repl_state_document_count();
        g_status[0] = '\0';

        /* Build rhs padded out with parenthesized zeros so the final
         * "  n = <rhs>;" comfortably exceeds MAX_LINE_LEN (256). */
        char big_rhs[MAX_INPUT_LEN];
        int off = snprintf(big_rhs, sizeof(big_rhs), "0");
        while (off < 400 && off + 4 < (int)sizeof(big_rhs)) {
            off += snprintf(big_rhs + off, sizeof(big_rhs) - off, "+(0)");
        }

        char input[MAX_INPUT_LEN];
        ASSERT_TRUE("overlong assign: test input fits",
                    repl_format_fits(input, sizeof(input), "n = %s", big_rhs));
        set_editor_input(input);
        repl_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(0);

        int r = try_assign_variable();
        ASSERT_INT("overlong assign: handler consumed input", r, 1);
        ASSERT_INT("overlong assign: num_cmds unchanged",
                   repl_state_document_count(), old_num_cmds);
        assert_status_contains("overlong assign: status", "Command too long");
    }

    /* Replay keyboard dispatch is owned by repl_replay.c, not the editor. */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("glVertex3f(0, 0, 0);");
        repl_feed_line_public("glVertex3f(1, 0, 0);");
        repl_flatten_commands();

        ASSERT_INT("replay key ctrl-r consumed",
                   repl_replay_handle_key(KEY_CTRL_R), 1);
        ASSERT_INT("replay key ctrl-r starts replay",
                   replay_active, 1);
        ASSERT_INT("replay space consumed",
                   repl_replay_handle_key(' '), 1);
        ASSERT_INT("replay space pauses",
                   replay_state, REPLAY_PAUSED);
        ASSERT_INT("replay space resumes",
                   repl_replay_handle_key(' '), 1);
        ASSERT_INT("replay resumed playing",
                   replay_state, REPLAY_PLAYING);

        replay_state = REPLAY_PAUSED;
        ASSERT_INT("replay right consumed",
                   repl_replay_handle_special_key(GLUT_KEY_RIGHT), 1);
        ASSERT_INT("replay right advances one step",
                   replay_pc, 1);
        ASSERT_INT("replay left consumed",
                   repl_replay_handle_special_key(GLUT_KEY_LEFT), 1);
        ASSERT_INT("replay left steps back",
                   replay_pc, 0);

        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN;
        replay_state = REPLAY_PLAYING;
        imrepl_ctrl_router_handle_active_replay_key(' ');
        ASSERT_INT("replay space keeps hidden code panel",
                   repl_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);
        ASSERT_INT("replay space still pauses through router",
                   replay_state, REPLAY_PAUSED);

        replay_state = REPLAY_PAUSED;
        replay_pc = 0;
        imrepl_ctrl_router_handle_replay_special(GLUT_KEY_RIGHT);
        ASSERT_INT("replay right keeps hidden code panel",
                   repl_state_presentation().code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);
        ASSERT_INT("replay right still advances through router",
                   replay_pc, 1);
        repl_state_presentation_mut()->code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;

        ASSERT_INT("replay unknown key unconsumed",
                   repl_replay_handle_key('x'), 0);
        ASSERT_INT("replay unknown key stops replay",
                   replay_active, 0);
    }

    /* Replay scroll-follow should keep the replayed source command visible
     * without moving the user's editor cursor/input. */
    {
        int follow_doc_line = -1;
        int visible_lines = -1;

        repl_reset_state(); declare_test_vars();
        for (int i = 0; i < 30; i++) {
            char line[64];
            snprintf(line, sizeof(line), "glVertex3f(%d, 0, 0);", i);
            repl_feed_line_public(line);
        }
        repl_flatten_commands();


        ui_state_viewport_set_size(800, 230);
        g_panel_frac = 0.5f;
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        repl_state_presentation_mut()->show_vertex_indices = 0;
        navigate_to_line(0);

        replay_active = 1;
        replay_state = REPLAY_PAUSED;
        replay_pc = repl_state_flat_program_count();
        replay_src_line = 25;
        g_scroll = 0;
        g_scroll_follow_cursor = 0;

        (void)ui_panels_code_panel_apply_scroll_follow_for_test(&follow_doc_line,
                                                      &visible_lines);
        ASSERT_TRUE("replay follow helper computes target",
                    follow_doc_line >= 0);
        ASSERT_INT("code panel visible rows match rendered rows",
                   visible_lines, 9);

        g_scroll = follow_doc_line - 9;
        g_scroll_follow_cursor = 1;

        ASSERT_TRUE("replay follow helper reports visible",
                    ui_panels_code_panel_apply_scroll_follow_for_test(&follow_doc_line,
                                                            &visible_lines));
        ASSERT_INT("replay follow scrolls row above status bar",
                   g_scroll, follow_doc_line - visible_lines + 1);
        ASSERT_TRUE("replay follow line after scroll",
                    follow_doc_line >= g_scroll &&
                    follow_doc_line < g_scroll + visible_lines);
        ASSERT_INT("replay follow leaves edit line alone", repl_state_edit_line(), 0);
        ASSERT_STR("replay follow leaves input alone", editor_state_input().input, "glVertex3f(0, 0, 0)");

        replay_active = 0;
        replay_state = REPLAY_OFF;
        replay_src_line = -1;
        replay_pc = 0;
    }

    /* Replay source focus should follow GLU tessellation vertices, not the
     * structural gluBegin/gluEnd commands that wrap them. */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("gluBegin(GLU_POLYGON);");
        repl_feed_line_public("gluBegin(GLU_CONTOUR);");
        repl_feed_line_public("gluVertex(0, 0, 0);");
        repl_feed_line_public("gluVertex(1, 0, 0);");
        repl_feed_line_public("gluVertex(0, 1, 0);");
        repl_feed_line_public("gluEnd();");
        repl_feed_line_public("gluEnd();");
        repl_flatten_commands();

        replay_mode = REPLAY_MODE_VERTEX;
        repl_replay_start();
        repl_replay_advance();
        ASSERT_INT("replay vertex mode focuses first gluVertex",
                   replay_src_line, 2);
        repl_replay_advance();
        ASSERT_INT("replay vertex mode focuses next gluVertex",
                   replay_src_line, 3);
        repl_replay_stop();

        replay_mode = REPLAY_MODE_POLYGON;
        repl_replay_start();
        repl_replay_advance();
        ASSERT_INT("replay polygon mode focuses tess vertex",
                   replay_src_line, 4);
        repl_replay_stop();
        replay_mode = REPLAY_MODE_VERTEX;
    }

    /* Replay follow rows must match whether variable expansion rows are
     * actually rendered. Collapsed replay should follow the command row. */
    {
        int collapsed_follow = -1;
        int expanded_follow = -1;
        int visible_lines = -1;

        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("glVertex3f(0, 0, 0);");
        repl_feed_line_public("glVertex3f(x, y, z);");
        repl_feed_line_public("glVertex3f(2, 0, 0);");
        repl_flatten_commands();


        ui_state_viewport_set_size(800, 230);
        g_panel_frac = 0.5f;
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        repl_state_presentation_mut()->show_vertex_indices = 0;
        navigate_to_line(0);

        replay_active = 1;
        replay_state = REPLAY_PAUSED;
        replay_pc = repl_state_flat_program_count();
        replay_src_line = 1;
        g_scroll = 0;
        g_scroll_follow_cursor = 0;

        replay_expand_args = 0;
        (void)ui_panels_code_panel_apply_scroll_follow_for_test(&collapsed_follow,
                                                      &visible_lines);
        ASSERT_TRUE("collapsed replay follow resolves command row",
                    collapsed_follow >= 0);

        replay_expand_args = 1;
        (void)ui_panels_code_panel_apply_scroll_follow_for_test(&expanded_follow,
                                                      &visible_lines);
        ASSERT_INT("expanded replay follows final annotation row",
                   expanded_follow, collapsed_follow + 2);

        replay_expand_args = 0;
        expanded_follow = -1;
        (void)ui_panels_code_panel_apply_scroll_follow_for_test(&expanded_follow,
                                                      &visible_lines);
        ASSERT_INT("collapsed replay removes annotation rows from follow",
                   expanded_follow, collapsed_follow);

        replay_expand_args = 1;
        replay_active = 0;
        replay_state = REPLAY_OFF;
        replay_src_line = -1;
        replay_pc = 0;
    }

    /* Regression: pressing Enter on the last existing command must enter
     * insert mode at repl_state_edit_line() == repl_state_document_count().  Before the fix the
     * cursor was invisible because the renderer guarded the active-input
     * row with !editor_insert_mode(), so the slot drew a dimmed placeholder instead
     * of the cursor. */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        navigate_to_line(2);
        ASSERT_INT("enter-at-last: setup edit_line", repl_state_edit_line(), 2);
        ASSERT_INT("enter-at-last: setup not inserting", editor_insert_mode(), 0);

        editor_handle_key('\r', 0, 0);

        ASSERT_INT("enter-at-last: now inserting", editor_insert_mode(), 1);
        ASSERT_INT("enter-at-last: edit_line == num_cmds", repl_state_edit_line(), repl_state_document_count());
    }

    /* Regression: scroll follow for insert-at-end must track the cursor
     * identically to overwrite-at-end.  Both were already computing the
     * same cursor_doc_line, but total_lines differed for non-empty input
     * before the code_panel_newline_rows fix. */
    {
        int follow_insert = -1, follow_overwrite = -1;
        int visible_lines = -1;

        repl_reset_state();
        for (int i = 0; i < 20; i++) {
            char line[64];
            snprintf(line, sizeof(line), "glVertex3f(%d, 0, 0);", i);
            repl_feed_line_public(line);
        }


        ui_state_viewport_set_size(800, 230);
        g_panel_frac = 0.5f;
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        repl_state_presentation_mut()->show_vertex_indices = 0;
        replay_active = 0;

        repl_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(1);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            inp->input[0] = '\0';
            inp->input_len = 0;
        }
        editor_cursor_pos_set(0);
        g_scroll = 0;
        g_scroll_follow_cursor = 1;
        ui_panels_code_panel_apply_scroll_follow_for_test(&follow_insert, &visible_lines);
        ASSERT_TRUE("insert-at-end cursor in visible region",
                    follow_insert >= g_scroll &&
                    follow_insert < g_scroll + visible_lines);

        repl_state_edit_line_set(repl_state_document_count());
        editor_insert_mode_set(0);
        g_scroll = 0;
        g_scroll_follow_cursor = 1;
        ui_panels_code_panel_apply_scroll_follow_for_test(&follow_overwrite, &visible_lines);

        ASSERT_INT("insert-at-end follow matches overwrite-at-end follow",
                   follow_insert, follow_overwrite);
    }

    /* Regression: special_func (arrow keys) must set g_scroll_follow_cursor
     * so the viewport tracks cursor movement on each keypress.  Before the
     * fix only keyboard_func set the flag, so Up/Down left the view stale. */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        navigate_to_line(0);

        g_scroll_follow_cursor = 0;
        editor_handle_special(GLUT_KEY_UP, 0, 0);
        ASSERT_INT("up key sets scroll_follow_cursor", g_scroll_follow_cursor, 1);

        g_scroll_follow_cursor = 0;
        editor_handle_special(GLUT_KEY_DOWN, 0, 0);
        ASSERT_INT("down key sets scroll_follow_cursor", g_scroll_follow_cursor, 1);

        /* Page Up/Down scroll the view manually and must NOT trigger cursor
         * follow - that would snap the view back to the cursor position,
         * defeating the purpose of the scroll. */
        g_scroll_follow_cursor = 0;
        editor_handle_special(GLUT_KEY_PAGE_UP, 0, 0);
        ASSERT_INT("page up cancels scroll_follow_cursor", g_scroll_follow_cursor, 0);

        g_scroll_follow_cursor = 0;
        editor_handle_special(GLUT_KEY_PAGE_DOWN, 0, 0);
        ASSERT_INT("page down cancels scroll_follow_cursor", g_scroll_follow_cursor, 0);
    }

    /* Regression: pressing Up when the cursor is below the visible area
     * must scroll the viewport to reveal it. */
    {
        int follow_doc_line = -1;
        int visible_lines = -1;

        repl_reset_state();
        for (int i = 0; i < 20; i++) {
            char line[64];
            snprintf(line, sizeof(line), "glVertex3f(%d, 0, 0);", i);
            repl_feed_line_public(line);
        }


        ui_state_viewport_set_size(800, 230);
        g_panel_frac = 0.5f;
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        repl_state_presentation_mut()->show_vertex_indices = 0;
        replay_active = 0;

        navigate_to_line(repl_state_document_count() - 1);
        g_scroll = 0;
        g_scroll_follow_cursor = 0;

        editor_handle_special(GLUT_KEY_UP, 0, 0);
        ASSERT_INT("up nav: scroll_follow_cursor set", g_scroll_follow_cursor, 1);

        ASSERT_TRUE("up nav: cursor visible after follow",
                    ui_panels_code_panel_apply_scroll_follow_for_test(&follow_doc_line,
                                                            &visible_lines));
    }

    /* Auto-commit modified lines before keyboard navigation. */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        navigate_to_line(0);
        set_editor_input("glVertex3f(9, 0, 0)");

        editor_handle_special(GLUT_KEY_DOWN, 0, 0);

        ASSERT_INT("nav auto-commit valid edit: cursor moved", repl_state_edit_line(), 1);
        ASSERT_INT("nav auto-commit valid edit: first cmd type",
                   repl_state_document_cmds_mut()[0].type, CMD_VERTEX3F);
        ASSERT_TRUE("nav auto-commit valid edit: x arg",
                    fabsf(repl_state_document_cmds_mut()[0].args[0] - 9.0f) < 1e-6f);
        ASSERT_STR("nav auto-commit valid edit: input loaded next", editor_state_input().input, "glVertex3f(1, 1, 1)");
    }

    /* Invalid auto-commit reverts the edited line and still navigates. */
    {
        char old_source[MAX_LINE_LEN];
        int old_num_cmds;

        repl_reset_state();
        repl_feed_line_public("glColor3f(1,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        navigate_to_line(0);
        snprintf(old_source, sizeof(old_source), "%s",
                 editor_buffer_line(0) ? editor_buffer_line(0) : "");
        old_num_cmds = repl_state_document_count();
        set_editor_input("glColor3f(1, 0");

        editor_handle_special(GLUT_KEY_DOWN, 0, 0);

        ASSERT_INT("nav auto-commit invalid edit: cmd count unchanged",
                   repl_state_document_count(), old_num_cmds);
        ASSERT_STR("nav auto-commit invalid edit: source reverted",
                   editor_buffer_line(0), old_source);
        ASSERT_INT("nav auto-commit invalid edit: cursor moved", repl_state_edit_line(), 1);
        ASSERT_STR("nav auto-commit invalid edit: input loaded next", editor_state_input().input, "glVertex3f(1, 1, 1)");
        assert_status_contains("nav auto-commit invalid edit: status",
                               "Incomplete command");
    }

    /* Auto-commit new end-of-buffer input before moving away. */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        navigate_to_line(repl_state_document_count());
        set_editor_input("glVertex3f(3, 3, 3)");

        editor_handle_special(GLUT_KEY_UP, 0, 0);

        ASSERT_INT("nav auto-commit append: cmd count", repl_state_document_count(), 3);
        ASSERT_INT("nav auto-commit append: cursor moved up", repl_state_edit_line(), 1);
        ASSERT_INT("nav auto-commit append: new cmd type",
                   repl_state_document_cmds_mut()[2].type, CMD_VERTEX3F);
        ASSERT_TRUE("nav auto-commit append: x arg",
                    fabsf(repl_state_document_cmds_mut()[2].args[0] - 3.0f) < 1e-6f);
    }

    /* Invalid new end-of-buffer input is discarded before moving away. */
    {
        int old_num_cmds;

        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        old_num_cmds = repl_state_document_count();
        navigate_to_line(repl_state_document_count());
        set_editor_input("glVertex3f(");

        editor_handle_special(GLUT_KEY_UP, 0, 0);

        ASSERT_INT("nav auto-commit invalid append: cmd count unchanged",
                   repl_state_document_count(), old_num_cmds);
        ASSERT_INT("nav auto-commit invalid append: cursor moved up",
                   repl_state_edit_line(), 1);
        ASSERT_STR("nav auto-commit invalid append: input loaded target", editor_state_input().input, "glVertex3f(1, 1, 1)");
        assert_status_contains("nav auto-commit invalid append: status",
                               "Incomplete command");
        ASSERT_STR("nav auto-commit invalid append: newline buffer discarded",
                   editor_pending_newline_buffer_mut(), "");

        navigate_to_line(repl_state_document_count());
        ASSERT_STR("nav auto-commit invalid append: return to end is empty", editor_state_input().input, "");
    }

    /* Code-panel clicks use the same auto-commit path. */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");

        ui_state_viewport_set_size(800, 600);
        g_panel_frac = 0.5f;
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        repl_state_presentation_mut()->show_vertex_indices = 0;
        g_scroll = code_panel_header_row_count();
        navigate_to_line(0);
        set_editor_input("glVertex3f(8, 0, 0)");

        {
            int mx = CODE_MARGIN_X + 1;
            int my = code_panel_mouse_y_for_cmd(2);
            UiHit hit = ui_panels_hit_test(mx, my);
            imrepl_ctrl_router_handle_code_panel_hit(hit, mx, my);
        }

        ASSERT_INT("mouse auto-commit valid edit: cursor moved", repl_state_edit_line(), 2);
        ASSERT_TRUE("mouse auto-commit valid edit: x arg",
                    fabsf(repl_state_document_cmds_mut()[0].args[0] - 8.0f) < 1e-6f);
        ASSERT_STR("mouse auto-commit valid edit: input loaded clicked", editor_state_input().input, "glVertex3f(2, 2, 2)");
    }

    /* Autocomplete Up/Down keeps selection behavior and does not navigate. */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        navigate_to_line(0);
        set_editor_input("glVertex3f(7, 0, 0)");
        g_ac_count = 2;
        g_ac_sel = 0;

        editor_handle_special(GLUT_KEY_DOWN, 0, 0);

        ASSERT_INT("autocomplete down changes selection", g_ac_sel, 1);
        ASSERT_INT("autocomplete down does not navigate", repl_state_edit_line(), 0);
        ASSERT_TRUE("autocomplete down does not commit edit",
                    fabsf(repl_state_document_cmds_mut()[0].args[0] - 0.0f) < 1e-6f);
    }

    /* repl_clear_all_cmds - clears scene including float declarations. */
    {
        int base_num_predef_vars;
        int i;
        int found_tmp_before_clear = 0;
        int found_tmp_after_clear = 0;

        repl_reset_state();
        base_num_predef_vars = g_num_predef_vars;
        repl_feed_line_public("float tmp;");
        repl_feed_line_public("glVertex3f(1, 0, 0)");
        ASSERT_INT("clear_all: setup two cmds", repl_state_document_count(), 2);
        ASSERT_INT("clear_all: first is var decl", repl_state_document_cmds_mut()[0].type, CMD_VAR_DECLARE);
        ASSERT_INT("clear_all: decl registers one predef var",
                   g_num_predef_vars, base_num_predef_vars + 1);
        for (i = 0; i < g_num_predef_vars; i++) {
            if (strcmp(g_predef_vars[i].name, "tmp") == 0) {
                found_tmp_before_clear = 1;
                break;
            }
        }
        ASSERT_TRUE("clear_all: tmp is registered before clear", found_tmp_before_clear);

        repl_clear_all_cmds();
        ASSERT_INT("clear_all: num_cmds is 0", repl_state_document_count(), 0);
        ASSERT_INT("clear_all: edit_line is 0", repl_state_edit_line(), 0);
        ASSERT_INT("clear_all: inserting is 0", editor_insert_mode(), 0);
        ASSERT_INT("clear_all: input is empty", editor_state_input().input[0], 0);
        ASSERT_INT("clear_all: predef var count restored",
                   g_num_predef_vars, base_num_predef_vars);
        for (i = 0; i < g_num_predef_vars; i++) {
            if (strcmp(g_predef_vars[i].name, "tmp") == 0) {
                found_tmp_after_clear = 1;
                break;
            }
        }
        ASSERT_TRUE("clear_all: tmp is no longer registered", !found_tmp_after_clear);
    }

    /* Regression: Ctrl+/ on a commented-out variable assignment should
     * uncomment it into a CMD_VAR_ASSIGN.  Before the fix the GL-command
     * parser would reject the stripped "x = 1;" and the user would see
     * "Cannot uncomment: not a valid command". */
    {
        int saved_mods = g_mock_modifiers;

        repl_reset_state();
        repl_feed_line_public("float x;");
        repl_feed_line_public("x = 1;");
        ASSERT_INT("uncomment setup: two cmds", repl_state_document_count(), 2);
        ASSERT_INT("uncomment setup: line 1 is assign",
                   repl_state_document_cmds_mut()[1].type, CMD_VAR_ASSIGN);

        /* Comment the assignment with Ctrl+/. */
        repl_state_edit_line_set(1);
        editor_insert_mode_set(0);
        g_status[0] = '\0';
        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key('/', 0, 0);
        ASSERT_INT("comment assignment: type became comment",
                   repl_state_document_cmds_mut()[1].type, CMD_COMMENT);
        ASSERT_TRUE("comment assignment: source keeps x = 1",
                    strstr(editor_buffer_line(1) ? editor_buffer_line(1) : "", "x = 1") != NULL);
        assert_status_contains("comment assignment: status", "Commented");

        /* Uncomment with Ctrl+/ again.  Fallback path must rebuild the
         * CMD_VAR_ASSIGN in place, not reject with "not a valid command". */
        repl_state_edit_line_set(1);
        editor_insert_mode_set(0);
        g_status[0] = '\0';
        editor_handle_key('/', 0, 0);
        ASSERT_INT("uncomment assignment: type back to assign",
                   repl_state_document_cmds_mut()[1].type, CMD_VAR_ASSIGN);
        ASSERT_TRUE("uncomment assignment: no error status",
                    strstr(g_status, "Cannot uncomment") == NULL);
        assert_status_contains("uncomment assignment: status", "Uncommented");
        ASSERT_TRUE("uncomment assignment: value restored",
                    repl_state_document_cmds_mut()[1].num_args == repl_eval_find_predef_var_idx("x"));

        g_mock_modifiers = saved_mods;
    }

    /* Regression: uncomment still errors on genuinely unparseable lines.
     * Comment a valid line, mangle the source so neither the GL parser
     * nor the assignment fallback can rebuild it, then Ctrl+/ again. */
    {
        int saved_mods = g_mock_modifiers;

        repl_reset_state();
        repl_feed_line_public("glVertex3f(0, 0, 0);");
        repl_state_edit_line_set(0);
        editor_insert_mode_set(0);
        g_status[0] = '\0';
        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key('/', 0, 0);
        ASSERT_INT("mangled setup: commented", repl_state_document_cmds_mut()[0].type, CMD_COMMENT);

        /* Replace the comment body with nonsense so the re-parse fails. */
        editor_buffer_set_line(0, "// !@#$not a command$@#!");
        g_status[0] = '\0';
        repl_state_edit_line_set(0);
        editor_handle_key('/', 0, 0);
        ASSERT_INT("uncomment genuinely bad: stays a comment",
                   repl_state_document_cmds_mut()[0].type, CMD_COMMENT);
        assert_status_contains("uncomment genuinely bad: error status",
                               "Cannot uncomment");

        g_mock_modifiers = saved_mods;
    }

    /* Extra coverage: accumulation samples toggle (Ctrl++ / Ctrl+-) */
    {
        int saved_mods = g_mock_modifiers;
        ReplRenderState *rs = repl_state_render_mut();

        repl_reset_state();
        rs->use_accum = 1;
        rs->accum_samples = 2;

        g_mock_modifiers = GLUT_ACTIVE_CTRL;

        /* Increment */
        imrepl_ctrl_router_handle_accum_samples_key('+');
        ASSERT_INT("accum toggle +: samples increased to 4", rs->accum_samples, 4);

        imrepl_ctrl_router_handle_accum_samples_key('='); /* some keyboards use = for + without shift */
        ASSERT_INT("accum toggle =: samples increased to 8", rs->accum_samples, 8);

        /* Decrement */
        imrepl_ctrl_router_handle_accum_samples_key('-');
        ASSERT_INT("accum toggle -: samples decreased to 4", rs->accum_samples, 4);

        /* Test limits */
        rs->accum_samples = 16;
        imrepl_ctrl_router_handle_accum_samples_key('+');
        ASSERT_INT("accum toggle +: samples capped at 16", rs->accum_samples, 16);

        rs->accum_samples = 1;
        imrepl_ctrl_router_handle_accum_samples_key('-');
        ASSERT_INT("accum toggle -: samples floored at 1", rs->accum_samples, 1);

        g_mock_modifiers = saved_mods;
    }

    /* Extra coverage: Ctrl+L (Clear All) */
    {
        int saved_mods = g_mock_modifiers;
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        ASSERT_INT("Ctrl+L setup: 1 cmd", repl_state_document_count(), 1);

        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key(12, 0, 0); /* Ctrl+L is 12 */
        ASSERT_INT("Ctrl+L: cleared all", repl_state_document_count(), 0);

        g_mock_modifiers = saved_mods;
    }

    /* Extra coverage: F12 cycling with user scenes */
    {
        repl_reset_state();
        int example_count = repl_example_count();
        if (example_count > 0) {
            /* Load example 0 */
            repl_load_example(0);

            /* Promote to user scene */
            int slot = repl_promote_example_if_needed();
            ASSERT_TRUE("F12 test: promoted example to slot", slot >= 0);

            /* Cycle from user scene 0 -> should go to example 0 */
            imrepl_ctrl_router_handle_scene_cycle_special(GLUT_KEY_F12);
            ASSERT_INT("F12: user scene 0 -> example 0", repl_state_scenes().active_example_idx, 0);
            ASSERT_INT("F12: active user scene now -1", repl_active_user_scene(), -1);

            /* Cycle through all examples to reach user scenes again */
            for (int i = 0; i < example_count; i++) {
                 imrepl_ctrl_router_handle_scene_cycle_special(GLUT_KEY_F12);
            }
            /* After all examples, it should hit the first used user scene slot */
            ASSERT_INT("F12: cycled back to user scene 0", repl_active_user_scene(), 0);
        }
    }

    /* Extra coverage: panel resizing via mouse motion */
    {
        repl_reset_state();
        ui_state_viewport_set_size(1000, 1000);
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        ui_state_code_panel_mut()->resizing_panel = 1;

        editor_handle_motion(300, 500);
        ASSERT_TRUE("panel resize left: frac updated", fabsf(ui_state_code_panel().panel_frac - 0.3f) < 1e-6f);

        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_TOP;
        editor_handle_motion(500, 400);
        ASSERT_TRUE("panel resize top: frac updated", fabsf(ui_state_code_panel().panel_frac - 0.4f) < 1e-6f);

        ui_state_code_panel_mut()->resizing_panel = 0;
    }

    /* Extra coverage: editor_point_on_code_panel_divider */
    {
        repl_reset_state();
        ui_state_viewport_set_size(1000, 1000);
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        ui_state_code_panel_mut()->panel_frac = 0.3f;

        /* Divider should be at x = 300. Test hit at 305. */
        editor_handle_passive_motion(305, 500);
        /* We can't easily assert the cursor change without more mocking,
         * but we are hitting the branch. */

        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_TOP;
        /* Divider should be at gl_y = 300. window_h = 1000, so y = 700. Test hit at 705. */
        editor_handle_passive_motion(500, 705);
    }

    /* Extra coverage: Audio track navigation (Ctrl+Left/Right) */
    {
        int saved_mods = g_mock_modifiers;
        g_mock_modifiers = GLUT_ACTIVE_CTRL;

        /* Just trigger the routes to ensure they are covered.
         * Actual track change depends on audio assets. */
        imrepl_ctrl_router_handle_horizontal_audio_special(GLUT_KEY_LEFT);
        imrepl_ctrl_router_handle_horizontal_audio_special(GLUT_KEY_RIGHT);

        g_mock_modifiers = saved_mods;
    }

    /* Extra coverage: Help overlay scrolling */
    {
        ui_state_help_mut()->visible = 1;
        editor_help_session_set_scroll(0);
        editor_help_session_set_tab(0);

        imrepl_ctrl_router_handle_help_scroll_special(GLUT_KEY_DOWN);
        ASSERT_INT("help scroll down", editor_help_session_scroll(), 1);

        imrepl_ctrl_router_handle_help_scroll_special(GLUT_KEY_UP);
        ASSERT_INT("help scroll up", editor_help_session_scroll(), 0);

        imrepl_ctrl_router_handle_help_tab_special(GLUT_KEY_RIGHT);
        ASSERT_INT("help tab right", editor_help_session_tab_idx(), 1);

        imrepl_ctrl_router_handle_help_tab_special(GLUT_KEY_LEFT);
        ASSERT_INT("help tab left", editor_help_session_tab_idx(), 0);

        imrepl_ctrl_router_handle_help_scroll_special(GLUT_KEY_PAGE_DOWN);
        ASSERT_INT("help page down", editor_help_session_scroll(), 5);

        imrepl_ctrl_router_handle_help_scroll_special(GLUT_KEY_PAGE_UP);
        ASSERT_INT("help page up", editor_help_session_scroll(), 0);

        ui_state_help_mut()->visible = 0;
    }

    /* Extra coverage: Escape key routes */
    {
        /* 1. Help visible */
        ui_state_help_mut()->visible = 1;
        editor_handle_key(27, 0, 0);
        ASSERT_TRUE("Esc: help closed", !ui_state_help().visible);

        /* 2. Autocomplete active */
        editor_state_autocomplete_mut()->match_count = 1;
        editor_handle_key(27, 0, 0);
        ASSERT_INT("Esc: autocomplete cleared", editor_state_autocomplete().match_count, 0);

        /* 3. Insert mode */
        editor_insert_mode_set(1);
        editor_handle_key(27, 0, 0);
        ASSERT_TRUE("Esc: insert mode exited", !editor_insert_mode());

        /* 4. Input clear */
        set_editor_input("some text");
        editor_handle_key(27, 0, 0);
        ASSERT_INT("Esc: input cleared", editor_state_input().input_len, 0);
    }

    /* Extra coverage: timer_func logic */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0);");
        repl_feed_line_public("glVertex3f(1,1,1);");
        repl_flatten_commands();

        /* 1. Status TTL decrement */
        ui_state_status_mut()->ttl = 10;
        imrepl_ctrl_tick();
        ASSERT_INT("timer: status ttl decremented", ui_state_status().ttl, 9);

        /* 2. Cursor blink */
        ui_state_code_panel_mut()->blink_tick = 29;
        ui_state_code_panel_mut()->cursor_visible = 1;
        imrepl_ctrl_tick();
        ASSERT_INT("timer: blink tick reset", ui_state_code_panel().blink_tick, 0);
        ASSERT_TRUE("timer: cursor visibility toggled", !ui_state_code_panel().cursor_visible);

        /* 3. Replay advance */
        replay_state_mut()->active = 1;
        replay_state_mut()->state = REPLAY_PLAYING;
        replay_state_mut()->speed = 1.0f;
        replay_state_mut()->accum = 0.99f;
        /* This should trigger at least one repl_replay_advance */
        imrepl_ctrl_tick();
        ASSERT_TRUE("timer: replay accum advanced", replay_state_view().accum < 0.5f);

        replay_state_mut()->active = 0;
    }

    /* Extra coverage: variable dragging via mouse */
    {
        repl_reset_state();
        ui_state_viewport_set_size(1000, 1000);
        variable_panel_view_mut()->visible = 1;
        repl_feed_line_public("float testvar = 5.0;");

        /* Variable panel is usually at bottom-right of scene.
         * Scene is to the right of code panel (0.3 frac).
         * Code panel = 0..300. Scene = 300..1000.
         * Variable panel rect is roughly [800..1000, 8..60] in GL coords.
         * GL y=8 is window y=992.
         */

        /* Attempt to hit the variable row. */
        /* ui_variable_panel_rect will place it at the right edge of the scene. */
        int px, py, pw, ph;
        ui_variable_panel_rect(&px, &py, &pw, &ph);

        /* Click in the middle of the first row. */
        int click_x = px + pw / 2;
        int click_y = 1000 - (py + ph - VAR_PANEL_PAD_INTERNAL - VAR_TITLE_H_INTERNAL / 2);

        imrepl_ctrl_router_handle_variable_panel_drag_begin(GLUT_LEFT_BUTTON, GLUT_DOWN, click_x, click_y);
        ASSERT_TRUE("mouse: variable drag active", variable_panel_drag_active());

        imrepl_ctrl_router_handle_variable_panel_motion(click_x + 100, click_y);
        /* drag motion should have changed the variable value. */

        imrepl_ctrl_router_handle_variable_panel_drag_release(GLUT_UP);
        ASSERT_TRUE("mouse: variable drag inactive after release", !variable_panel_drag_active());
    }

    /* Extra coverage: Undo/Redo keys */
    {
        int saved_mods = g_mock_modifiers;
        repl_reset_state();
        repl_undo_push_snapshot();
        repl_feed_line_public("glVertex3f(1,1,1)");
        ASSERT_INT("undo setup: 1 cmd", repl_state_document_count(), 1);

        /* Undo */
        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key(26, 0, 0); /* Ctrl+Z */
        ASSERT_INT("Ctrl+Z: undo works", repl_state_document_count(), 0);

        /* Redo via Ctrl+Shift+Z */
        g_mock_modifiers = GLUT_ACTIVE_CTRL | GLUT_ACTIVE_SHIFT;
        editor_handle_key(26, 0, 0);
        ASSERT_INT("Ctrl+Shift+Z: redo works", repl_state_document_count(), 1);

        /* Undo again */
        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key(26, 0, 0);
        ASSERT_INT("undo again", repl_state_document_count(), 0);

        /* Redo via Ctrl+Y */
        g_mock_modifiers = GLUT_ACTIVE_CTRL;
        editor_handle_key(25, 0, 0); /* Ctrl+Y */
        ASSERT_INT("Ctrl+Y: redo works", repl_state_document_count(), 1);

        g_mock_modifiers = saved_mods;
    }

    /* Extra coverage: Mouse wheel */
#ifndef USE_GLUT
    {
        const int layouts[] = {
            CODE_PANEL_LAYOUT_LEFT,
            CODE_PANEL_LAYOUT_TOP,
            CODE_PANEL_LAYOUT_BOTTOM
        };
        const char *const layout_names[] = {
            "left",
            "top",
            "bottom"
        };

        for (int layout_idx = 0; layout_idx < 3; layout_idx++) {
            int cp_x, cp_y, cp_w, cp_h;
            int scene_x, scene_y, scene_w, scene_h;
            int code_x, code_y;
            int layout = layouts[layout_idx];
            char label[128];

            repl_reset_state();
            ui_state_viewport_set_size(1000, 1000);
            editor_scroll_set(0);
            repl_state_presentation_mut()->code_panel_layout = layout;

            repl_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
            repl_layout_scene_rect(&scene_x, &scene_y, &scene_w, &scene_h);

            code_x = cp_x + cp_w / 2;
            code_y = ui_state_viewport().window_h - (cp_y + cp_h / 2);
            scene_x += scene_w / 2;
            scene_y = ui_state_viewport().window_h - (scene_y + scene_h / 2);

            editor_handle_mousewheel(0, 1, code_x, code_y);
            snprintf(label, sizeof(label),
                     "mousewheel: %s code panel scrolled down",
                     layout_names[layout_idx]);
            ASSERT_INT(label, editor_scroll(), -1);

            editor_handle_mousewheel(0, -1, code_x, code_y);
            snprintf(label, sizeof(label),
                     "mousewheel: %s code panel scrolled up",
                     layout_names[layout_idx]);
            ASSERT_INT(label, editor_scroll(), 0);

            editor_handle_mousewheel(0, 1, scene_x, scene_y);
            snprintf(label, sizeof(label),
                     "mousewheel: %s scene wheel leaves code scroll unchanged",
                     layout_names[layout_idx]);
            ASSERT_INT(label, editor_scroll(), 0);
        }

        repl_reset_state();
        ui_state_viewport_set_size(1000, 1000);
        editor_scroll_set(0);
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN;

        editor_handle_mousewheel(0, 1, 500, 500);
        ASSERT_INT("mousewheel: hidden layout leaves code scroll unchanged",
                   editor_scroll(), 0);
    }
#endif

    /* Extra coverage: handle_buffer_command_key_route remaining keys */
    {
        int saved_mods = g_mock_modifiers;
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");

        g_mock_modifiers = GLUT_ACTIVE_CTRL;

        /* Ctrl+\ (Reformat) */
        editor_handle_key(28, 0, 0);
        assert_status_contains("Ctrl+\\ reformat", "Reformatted");

        /* Ctrl+P (Dump) */
        /* We can't easily check stdout, but we trigger the branch. */
        imrepl_ctrl_router_handle_debug_dump_key(16);
        assert_status_contains("Ctrl+P dump", "Dumped");

        /* Ctrl+S (Save) */
        {
            /* Keep the default output path inside a throwaway directory so
             * the test never touches repo-root output.c. */
            char cwd[1024];
            char temp_dir[] = "/tmp/test_repl_editor_output.XXXXXX";
            char *made_dir = mkdtemp(temp_dir);
            int have_cwd = getcwd(cwd, sizeof(cwd)) != NULL;

            ASSERT_TRUE("mkdtemp default output dir", made_dir != NULL);
            ASSERT_TRUE("getcwd before Ctrl+S save", have_cwd);
            if (made_dir && have_cwd) {
                int cd_ok = chdir(made_dir);
                ASSERT_INT("chdir default output dir", cd_ok, 0);
                if (cd_ok == 0) {
                    imrepl_ctrl_router_handle_save_key(19);
                    assert_status_contains("Ctrl+S save", "Saved");
                    ASSERT_INT("default output saved in temp dir",
                               access("output.c", F_OK), 0);
                    unlink("output.c");
                    ASSERT_INT("restore cwd after Ctrl+S save",
                               chdir(cwd), 0);
                }
                rmdir(made_dir);
            }
        }

        g_mock_modifiers = saved_mods;
    }

    /* Extra coverage: editor_restore_hidden_code_panel from keys */
    {
        repl_reset_state();
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN;

        /* Pressing a printable key should restore it. */
        editor_handle_key('a', 0, 0);
        ASSERT_INT("key restores hidden panel",
                   repl_state_presentation().code_panel_layout,
                   CODE_PANEL_LAYOUT_LEFT);

        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN;
        /* Pressing a special key should restore it. */
        editor_handle_special(GLUT_KEY_UP, 0, 0);
        ASSERT_INT("special key restores hidden panel",
                   repl_state_presentation().code_panel_layout,
                   CODE_PANEL_LAYOUT_LEFT);
    }

    /* Extra coverage: Commenting Func/If blocks */
    {
        repl_reset_state();
        repl_feed_line_public("if(1) {");
        repl_feed_line_public("  glVertex3f(0,0,0);");
        repl_feed_line_public("}");

        int saved_mods = g_mock_modifiers;
        g_mock_modifiers = GLUT_ACTIVE_CTRL;

        repl_state_edit_line_set(0);
        editor_insert_mode_set(0);
        editor_handle_key('/', 0, 0);
        ASSERT_INT("comment if-begin", repl_state_document_cmds_mut()[0].type, CMD_COMMENT);

        repl_state_edit_line_set(2);
        editor_handle_key('/', 0, 0);
        ASSERT_INT("comment if-end", repl_state_document_cmds_mut()[2].type, CMD_COMMENT);

        g_mock_modifiers = saved_mods;
    }

    /* Extra coverage: BOTTOM layout resizing */
    {
        repl_reset_state();
        ui_state_viewport_set_size(1000, 1000);
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_BOTTOM;
        ui_state_code_panel_mut()->panel_frac = 0.3f;

        /* Divider should be at gl_y = 300 (y = 700). */
        editor_handle_passive_motion(500, 695);

        ui_state_code_panel_mut()->resizing_panel = 1;
        editor_handle_motion(500, 600);
        ASSERT_TRUE("panel resize bottom: frac updated", fabsf(ui_state_code_panel().panel_frac - 0.4f) < 1e-6f);
        ui_state_code_panel_mut()->resizing_panel = 0;
    }

    /* Extra coverage: Right-click variable drag */
    {
        repl_reset_state();
        ui_state_viewport_set_size(1000, 1000);
        variable_panel_view_mut()->visible = 1;
        repl_feed_line_public("float testvar = 5.0;");

        int px, py, pw, ph;
        ui_variable_panel_rect(&px, &py, &pw, &ph);
        int click_x = px + pw / 2;
        int click_y = 1000 - (py + ph - VAR_PANEL_PAD_INTERNAL - VAR_TITLE_H_INTERNAL / 2);

        imrepl_ctrl_router_handle_variable_panel_drag_begin(GLUT_RIGHT_BUTTON, GLUT_DOWN, click_x, click_y);
        ASSERT_TRUE("mouse: right-click variable drag active", variable_panel_drag_active());
        imrepl_ctrl_router_handle_variable_panel_drag_release(GLUT_UP);
    }

    printf("\n%d / %d tests passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
