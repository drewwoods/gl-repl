#include "repl_core_internal.h"
#include "repl_replay.h"
#include "repl_keys.h"
#include "sample.h"
#include "profile_panel.h"
#include "ui_panels.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define TEST_CODE_PANEL_MAX_HANG_INDENT_CHARS 12

static int g_run = 0;
static int g_pass = 0;
static int g_mock_modifiers = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s] (line %d)\n", label, __LINE__); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    g_run++; \
    if ((got) == (exp)) g_pass++; \
    else printf("FAIL [%s] got %d, expected %d (line %d)\n", label, (int)(got), (int)(exp), __LINE__); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    g_run++; \
    if (strcmp(got, exp) == 0) g_pass++; \
    else printf("FAIL [%s] got \"%s\", expected \"%s\" (line %d)\n", label, got, exp, __LINE__); \
} while (0)

#define ASSERT_DECL_OK(label, cond, err) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s] %s (line %d)\n", label, err, __LINE__); \
} while (0)

static void declare_test_vars(void) {
    char err[128];
    ASSERT_DECL_OK("declare_predef_var x", declare_predef_var("x", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var y", declare_predef_var("y", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var z", declare_predef_var("z", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var i", declare_predef_var("i", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var j", declare_predef_var("j", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var k", declare_predef_var("k", err, sizeof(err)), err);
    ASSERT_DECL_OK("declare_predef_var n", declare_predef_var("n", err, sizeof(err)), err);
}

static void set_editor_input(const char *s) {
    strncpy(g_input, s, MAX_INPUT_LEN - 1);
    g_input[MAX_INPUT_LEN - 1] = '\0';
    g_input_len = (int)strlen(g_input);
    g_cursor_pos = g_input_len;
}

static int mock_get_modifiers(void) {
    return g_mock_modifiers;
}

static void assert_status_contains(const char *label, const char *needle) {
    ASSERT_TRUE(label, strstr(g_status, needle) != NULL);
}

static int cfg_row_for_value(int *value) {
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        if (g_cfg_items[i].value == value)
            return i;
    }
    return -1;
}

static int test_code_panel_available_chars(int panel_w, int x) {
    int avail_px = panel_w - x - 4;
    if (avail_px < FONT_W)
        return 0;
    return avail_px / FONT_W;
}

static int test_code_panel_cont_indent_chars(const char *text) {
    const char *src = text ? text : "";
    int leading = 0;

    while (src[leading] && isspace((unsigned char)src[leading]))
        leading++;

    {
        const char *paren = strchr(src, '(');
        if (paren && paren[1] != '\0') {
            int align = (int)(paren - src) + 1;
            int max_align = leading + TEST_CODE_PANEL_MAX_HANG_INDENT_CHARS;
            if (align > max_align)
                align = max_align;
            return align;
        }
    }

    return leading + 4;
}

static int test_code_panel_is_secondary_break(char c) {
    return c == ')' || c == ' ' || c == '+' || c == '*' || c == '-' || c == '/';
}

static int test_code_panel_find_wrap_break(const char *text, int start,
                                           int max_chars, int len) {
    int end = start + max_chars - 1;
    int search_start = start;

    if (end >= len)
        end = len - 1;

    while (search_start < len &&
           isspace((unsigned char)text[search_start]))
        search_start++;

    for (int i = end; i > search_start; i--) {
        if (text[i] == ',')
            return i;
    }

    for (int i = end; i > search_start; i--) {
        if (test_code_panel_is_secondary_break(text[i]))
            return i;
    }

    for (int i = end + 1; i < len; i++) {
        if (text[i] == ',' ||
            (i > search_start && test_code_panel_is_secondary_break(text[i])))
            return i;
    }

    return -1;
}

static int test_code_panel_row_count_for_text(const char *text, int first_x,
                                              int panel_w) {
    const char *src = text ? text : "";
    int len = (int)strlen(src);
    int pos = 0;
    int x = first_x;
    int cont_x = first_x + test_code_panel_cont_indent_chars(src) * FONT_W;
    int rows = 0;
    int done = 0;

    while (!done) {
        rows++;
        if (len == 0) {
            done = 1;
        } else {
            int width_chars = test_code_panel_available_chars(panel_w, x);
            int remaining = len - pos;

            if (!g_wrap_at_comma || width_chars < 1 || remaining <= width_chars) {
                done = 1;
            } else {
                int break_idx = test_code_panel_find_wrap_break(src, pos,
                                                                width_chars, len);
                if (break_idx < 0) {
                    done = 1;
                } else {
                    pos = break_idx + 1;
                    x = cont_x;
                }
            }
        }
    }

    return rows;
}

static int code_panel_header_row_count(void) {
    int panel_w;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int rows = 0;

    code_panel_rect(NULL, NULL, &panel_w, NULL);
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
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int doc_line = code_panel_header_row_count();

    code_panel_rect(NULL, &cp_y, &panel_w, &cp_h);
    for (int i = 0; i < cmd_idx && i < g_num_cmds; i++) {
        doc_line += test_code_panel_row_count_for_text(g_cmds[i].source,
                                                       text_x, panel_w);
    }

    int vis = doc_line - g_scroll;
    int line_y_start = cp_y + cp_h - CODE_MARGIN_Y - 2 * LINE_H;
    int gl_y = line_y_start - vis * LINE_H + 1;
    return g_win_h - gl_y;
}

static void assert_float_decl_rejected_atomic(const char *label,
                                              const char *src,
                                              const char *rejected_name,
                                              const char *status_part) {
    char detail[160];
    int old_num_cmds = g_num_cmds;
    int old_num_predef_vars = g_num_predef_vars;

    set_editor_input(src);
    g_edit_line = g_num_cmds;
    g_inserting = 0;

    int result = try_commit_float_decl();

    snprintf(detail, sizeof(detail), "%s: handler consumed input", label);
    ASSERT_INT(detail, result, 1);
    snprintf(detail, sizeof(detail), "%s: cmd count unchanged", label);
    ASSERT_INT(detail, g_num_cmds, old_num_cmds);
    snprintf(detail, sizeof(detail), "%s: var count unchanged", label);
    ASSERT_INT(detail, g_num_predef_vars, old_num_predef_vars);
    snprintf(detail, sizeof(detail), "%s: rejected name not registered", label);
    ASSERT_TRUE(detail, find_predef_var_idx(rejected_name) < 0);
    snprintf(detail, sizeof(detail), "%s: status", label);
    assert_status_contains(detail, status_part);
    ASSERT_TRUE("atomic failure preserves anchor",
                old_num_predef_vars <= 1 || find_predef_var_idx("anchor") >= 0);
}

int main() {
    init_predef_vars();
    repl_set_modifier_provider_for_test(mock_get_modifiers);
    printf("--- repl_editor tests ---\n");

    /* Commit chain ordering: float declarations must run before assignment
     * parsing, otherwise `float name` is misclassified as an assignment. */
    {
        repl_reset_state();
        set_editor_input("float chain_order;");
        g_edit_line = 0;
        g_inserting = 0;

        int result = try_commit_any();

        ASSERT_INT("commit chain float decl consumed", result, 1);
        ASSERT_INT("commit chain float decl inserted one cmd", g_num_cmds, 1);
        ASSERT_INT("commit chain float decl cmd type",
                   g_cmds[0].type, CMD_VAR_DECLARE);
        ASSERT_TRUE("commit chain float decl registered var",
                    find_predef_var_idx("chain_order") >= 0);
    }

    /* 0. Code/scene panel geometry supports left, top, bottom, and hidden layouts */
    {
        int x, y, w, h;
        g_win_w = 1000;
        g_win_h = 800;
        g_panel_frac = 0.25f;

        g_code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        code_panel_rect(&x, &y, &w, &h);
        ASSERT_INT("left code x", x, 0);
        ASSERT_INT("left code y", y, 0);
        ASSERT_INT("left code w", w, 250);
        ASSERT_INT("left code h", h, 800);
        scene_rect(&x, &y, &w, &h);
        ASSERT_INT("left scene x", x, 250);
        ASSERT_INT("left scene y", y, 0);
        ASSERT_INT("left scene w", w, 750);
        ASSERT_INT("left scene h", h, 800);

        g_code_panel_layout = CODE_PANEL_LAYOUT_TOP;
        code_panel_rect(&x, &y, &w, &h);
        ASSERT_INT("top code x", x, 0);
        ASSERT_INT("top code y", y, 600);
        ASSERT_INT("top code w", w, 1000);
        ASSERT_INT("top code h", h, 200);
        scene_rect(&x, &y, &w, &h);
        ASSERT_INT("top scene x", x, 0);
        ASSERT_INT("top scene y", y, 0);
        ASSERT_INT("top scene w", w, 1000);
        ASSERT_INT("top scene h", h, 600);

        g_code_panel_layout = CODE_PANEL_LAYOUT_BOTTOM;
        code_panel_rect(&x, &y, &w, &h);
        ASSERT_INT("bottom code x", x, 0);
        ASSERT_INT("bottom code y", y, 0);
        ASSERT_INT("bottom code w", w, 1000);
        ASSERT_INT("bottom code h", h, 200);
        scene_rect(&x, &y, &w, &h);
        ASSERT_INT("bottom scene x", x, 0);
        ASSERT_INT("bottom scene y", y, 200);
        ASSERT_INT("bottom scene w", w, 1000);
        ASSERT_INT("bottom scene h", h, 600);

        g_code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN;
        code_panel_rect(&x, &y, &w, &h);
        ASSERT_INT("hidden code x", x, 0);
        ASSERT_INT("hidden code y", y, 0);
        ASSERT_INT("hidden code w", w, 0);
        ASSERT_INT("hidden code h", h, 0);
        scene_rect(&x, &y, &w, &h);
        ASSERT_INT("hidden scene x", x, 0);
        ASSERT_INT("hidden scene y", y, 0);
        ASSERT_INT("hidden scene w", w, 1000);
        ASSERT_INT("hidden scene h", h, 800);

        g_win_w = 1200;
        g_win_h = 800;
        g_panel_frac = CFG_DEFAULT_PANEL_FRAC;
        g_code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;
    }

    /* 0b. Code panel config cycles Left -> Top -> Bottom -> Hidden and imports legacy top layout */
    {
        int row = cfg_row_for_value(&g_code_panel_layout);
        ASSERT_TRUE("code panel cfg row exists", row >= 0);
        if (row >= 0) {
            g_code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
            repl_cfg_cycle_row(row, +1);
            ASSERT_INT("code panel cfg cycles to top",
                       g_code_panel_layout, CODE_PANEL_LAYOUT_TOP);
            repl_cfg_cycle_row(row, +1);
            ASSERT_INT("code panel cfg cycles to bottom",
                       g_code_panel_layout, CODE_PANEL_LAYOUT_BOTTOM);
            g_ac_count = 2;
            g_ac_sel = 1;
            strcpy(g_ac_ghost, "glVertex3f");
            strcpy(g_ac_hint, "vertex");
            repl_cfg_cycle_row(row, +1);
            ASSERT_INT("code panel cfg cycles to hidden",
                       g_code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);
            ASSERT_INT("hide clears autocomplete count", g_ac_count, 0);
            ASSERT_INT("hide clears autocomplete selection", g_ac_sel, 0);
            ASSERT_STR("hide clears autocomplete ghost", g_ac_ghost, "");
            ASSERT_STR("hide clears autocomplete hint", g_ac_hint, "");
            repl_cfg_cycle_row(row, +1);
            ASSERT_INT("code panel cfg wraps to left",
                       g_code_panel_layout, CODE_PANEL_LAYOUT_LEFT);
        }

        g_code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        ASSERT_INT("parse code_panel cfg",
                   parse_workspace_header_line("// @cfg code_panel = 2"), 1);
        ASSERT_INT("parse code_panel bottom",
                   g_code_panel_layout, CODE_PANEL_LAYOUT_BOTTOM);

        ASSERT_INT("parse code_panel hidden cfg",
                   parse_workspace_header_line("// @cfg code_panel = 3"), 1);
        ASSERT_INT("parse code_panel hidden",
                   g_code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);

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
                   g_code_panel_layout, CODE_PANEL_LAYOUT_TOP);

        ASSERT_INT("parse legacy top_code_panel off cfg",
                   parse_workspace_header_line("// @cfg top_code_panel = 0"), 1);
        ASSERT_INT("legacy top_code_panel off maps to left",
                   g_code_panel_layout, CODE_PANEL_LAYOUT_LEFT);

        g_panel_frac = CFG_DEFAULT_PANEL_FRAC;
        g_code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;
    }

    /* 0c. Hidden code panel returns to the editor on ordinary input */
    {
        repl_reset_state();
        g_code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN;
        repl_keyboard_func('v', 0, 0);
        ASSERT_INT("typing restores hidden code panel",
                   g_code_panel_layout, CODE_PANEL_LAYOUT_LEFT);
        ASSERT_STR("typing after restore still reaches input", g_input, "v");

        g_code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN;
        repl_keyboard_func('`', 0, 0);
        ASSERT_INT("config shortcut restores hidden code panel",
                   g_code_panel_layout, CODE_PANEL_LAYOUT_LEFT);
        repl_keyboard_func('`', 0, 0);

        g_code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;
    }

    /* 0c2. Keyboard mode routing keeps rename ahead of config, replay, and search. */
    {
        repl_reset_state();
        repl_load_example(0);
        int slot = repl_promote_example_if_needed();
        ASSERT_TRUE("rename route setup slot", slot >= 0);
        ASSERT_INT("rename route begin", ui_panels_begin_rename(slot), 1);

        g_code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN;
        repl_keyboard_func('`', 0, 0);
        ASSERT_INT("rename swallows config hidden restore",
                   g_code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);
        ASSERT_INT("rename remains active after config key",
                   ui_panels_rename_active(), 1);

        repl_keyboard_func(KEY_CTRL_R, 0, 0);
        ASSERT_INT("rename swallows replay toggle", g_replay_active, 0);

        repl_keyboard_func(KEY_CTRL_F, 0, 0);
        ASSERT_INT("rename swallows search open", g_search_active, 0);

        ui_panels_cancel_rename();
        g_code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;
    }

    /* 0c3. Search mode captures printable editing keys before text editing. */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        set_editor_input("");

        repl_keyboard_func(KEY_CTRL_F, 0, 0);
        ASSERT_INT("search route opens", g_search_active, 1);

        repl_keyboard_func('v', 0, 0);
        ASSERT_STR("search route receives printable key", g_search_query, "v");
        ASSERT_TRUE("search route does not type into editor input",
                    strcmp(g_input, "v") != 0);
        ASSERT_INT("search route does not commit", g_num_cmds, 1);

        search_clear_all();
    }

    /* 0c4. Semicolon keyboard route still enters the commit handler chain. */
    {
        repl_reset_state();
        set_editor_input("float keyboard_route");

        repl_keyboard_func(';', 0, 0);

        ASSERT_INT("semicolon route committed one command", g_num_cmds, 1);
        ASSERT_INT("semicolon route used float declaration handler",
                   g_cmds[0].type, CMD_VAR_DECLARE);
        ASSERT_TRUE("semicolon route registered declared variable",
                    find_predef_var_idx("keyboard_route") >= 0);
    }

    /* 0c5. Special-key routing keeps rename ahead of replay/search/navigation. */
    {
        repl_reset_state();
        repl_load_example(0);
        int slot = repl_promote_example_if_needed();
        ASSERT_TRUE("rename special route setup slot", slot >= 0);
        ASSERT_INT("rename special route begin", ui_panels_begin_rename(slot), 1);

        set_editor_input("abc");
        g_cursor_pos = 2;
        g_code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN;
        g_show_help = 0;
        g_replay_active = 1;
        g_replay_state = REPLAY_PAUSED;
        g_replay_pc = 0;
        g_search_active = 1;
        snprintf(g_search_query, sizeof(g_search_query), "abc");
        g_search_query_len = 3;
        g_search_cursor_pos = 2;

        repl_special_func(GLUT_KEY_LEFT, 0, 0);
        ASSERT_INT("rename special swallows hidden restore",
                   g_code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);
        ASSERT_INT("rename special keeps editor cursor", g_cursor_pos, 2);
        ASSERT_INT("rename special keeps search cursor", g_search_cursor_pos, 2);
        ASSERT_INT("rename special keeps replay pc", g_replay_pc, 0);

        repl_special_func(GLUT_KEY_F1, 0, 0);
        ASSERT_INT("rename special swallows help toggle", g_show_help, 0);
        ASSERT_INT("rename still active after special keys",
                   ui_panels_rename_active(), 1);

        ui_panels_cancel_rename();
        search_clear_all();
        g_replay_active = 0;
        g_code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;
    }

    /* 0c6. Search mode captures special arrows before editor/help navigation. */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        navigate_to_line(0);
        set_editor_input("abc");
        g_cursor_pos = 2;
        g_show_help = 1;
        g_help_tab = 1;
        g_search_active = 1;
        snprintf(g_search_query, sizeof(g_search_query), "abc");
        g_search_query_len = 3;
        g_search_cursor_pos = 2;

        repl_special_func(GLUT_KEY_LEFT, 0, 0);

        ASSERT_INT("search special moves search cursor", g_search_cursor_pos, 1);
        ASSERT_INT("search special leaves editor cursor", g_cursor_pos, 2);
        ASSERT_INT("search special leaves help tab", g_help_tab, 1);

        search_clear_all();
        g_show_help = 0;
    }

    /* 0c7. F12 special route still cycles built-in examples. */
    {
        repl_reset_state();
        if (repl_example_count() > 1) {
            repl_load_example(0);
            repl_special_func(GLUT_KEY_F12, 0, 0);
            ASSERT_INT("f12 special route advances example", g_example_idx, 1);
        }
    }

    /* 0d. Cramped variable-panel fallback still preserves scene status strip */
    {
        int x, y, w, h;
        g_win_w = 320;
        g_win_h = 80;
        g_panel_frac = 0.25f;
        g_code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        g_replay_active = 0;

        var_panel_rect(&x, &y, &w, &h);
        ASSERT_INT("cramped var panel clears status strip",
                   y, STATUSBAR_H + 4);

        g_win_w = 1200;
        g_win_h = 800;
        g_panel_frac = CFG_DEFAULT_PANEL_FRAC;
        g_code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;
    }

    /* 1. Undo when nothing to undo — must run before any undo push */
    {
        /* g_undo_count starts at 0 (global zero-init); repl_reset_state() does
         * NOT clear the undo buffer, so run this before touching undo at all. */
        repl_reset_state();
        pop_undo_snapshot();
        /* Should survive without crashing; state unchanged */
        ASSERT_INT("undo-nothing: num_cmds still 0", g_num_cmds, 0);
    }

    /* 2. Redo when nothing to redo — similarly must be before any undo activity */
    {
        repl_reset_state();
        do_redo();
        ASSERT_INT("redo-nothing: num_cmds still 0", g_num_cmds, 0);
    }

    /* 3. Undo/Redo basic */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("glVertex3f(1,1,1)");
        ASSERT_INT("num_cmds 1", g_num_cmds, 1);

        push_undo_snapshot();
        repl_feed_line_public("glVertex3f(2,2,2)");
        ASSERT_INT("num_cmds 2", g_num_cmds, 2);

        pop_undo_snapshot();
        ASSERT_INT("num_cmds after undo", g_num_cmds, 1);
        ASSERT_STR("cmd 0 source", g_cmds[0].source, "  glVertex3f(1, 1, 1);");

        do_redo();
        ASSERT_INT("num_cmds after redo", g_num_cmds, 2);
        ASSERT_STR("cmd 1 source", g_cmds[1].source, "  glVertex3f(2, 2, 2);");
    }

    /* 4. Deleting commands — basic */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        ASSERT_INT("num_cmds 3", g_num_cmds, 3);

        delete_cmd_range(1, 1, "test");
        ASSERT_INT("num_cmds after delete", g_num_cmds, 2);
        ASSERT_STR("line 0 still there", g_cmds[0].source, "  glVertex3f(0, 0, 0);");
        ASSERT_STR("line 1 is now old line 2", g_cmds[1].source, "  glVertex3f(2, 2, 2);");
    }

    /* 5. delete_cmd_range — count=0 (no-op) */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        delete_cmd_range(0, 0, "noop");
        ASSERT_INT("delete count=0: no change", g_num_cmds, 1);
    }

    /* 6. delete_cmd_range — start=g_num_cmds (out of bounds, no-op) */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        delete_cmd_range(1, 1, "oob");  /* start == g_num_cmds (1) */
        ASSERT_INT("delete oob start: no change", g_num_cmds, 1);
    }

    /* 7. delete_cmd_range — count clamped when over-count */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        /* ask to delete 5 but only 1 remains at start=1 */
        delete_cmd_range(1, 5, "clamp");
        ASSERT_INT("delete clamp: leaves 1 cmd", g_num_cmds, 1);
        ASSERT_STR("delete clamp: cmd 0 remains", g_cmds[0].source, "  glVertex3f(0, 0, 0);");
    }

    /* 8. delete_cmd_range — edit_line clamped when it would exceed g_num_cmds */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        g_edit_line = 2;  /* beyond last cmd */
        delete_cmd_range(1, 1, "elclamp");
        /* g_edit_line should be clamped to g_num_cmds (1) */
        ASSERT_INT("delete edit_line clamp: edit_line<=num_cmds", g_edit_line <= g_num_cmds, 1);
    }

    /* 8a. delete_cmd_range — interior multi-line block */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        repl_feed_line_public("glVertex3f(3,3,3)");
        repl_feed_line_public("glVertex3f(4,4,4)");
        g_sel_anchor = 1;
        g_sel_end = 3;

        delete_cmd_range(1, 3, "Deleted");

        ASSERT_INT("delete block: leaves 2 cmds", g_num_cmds, 2);
        ASSERT_STR("delete block: first survives", g_cmds[0].source, "  glVertex3f(0, 0, 0);");
        ASSERT_STR("delete block: last survives", g_cmds[1].source, "  glVertex3f(4, 4, 4);");
        ASSERT_INT("delete block: edit_line at deletion start", g_edit_line, 1);
        ASSERT_STR("delete block: input reloaded", g_input, "glVertex3f(4, 4, 4)");
        ASSERT_TRUE("delete block: selection cleared", !sel_active());
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
        g_sel_anchor = 4;
        g_sel_end = 2;
        g_edit_line = 4;

        repl_keyboard_func(8, 0, 0);

        ASSERT_INT("backspace reversed selection: leaves 2 cmds", g_num_cmds, 2);
        ASSERT_STR("backspace reversed selection: first survives", g_cmds[0].source, "  glVertex3f(0, 0, 0);");
        ASSERT_STR("backspace reversed selection: second survives", g_cmds[1].source, "  glVertex3f(1, 1, 1);");
        ASSERT_INT("backspace reversed selection: edit_line", g_edit_line, 2);
        ASSERT_TRUE("backspace reversed selection: selection cleared", !sel_active());
    }

    /* 8b2. Ctrl+D deletes a selected range through its own key path */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        repl_feed_line_public("glVertex3f(3,3,3)");
        repl_feed_line_public("glVertex3f(4,4,4)");
        g_sel_anchor = 3;
        g_sel_end = 1;
        g_edit_line = 3;

        repl_keyboard_func(4, 0, 0);

        ASSERT_INT("ctrl-d selection: leaves 2 cmds", g_num_cmds, 2);
        ASSERT_STR("ctrl-d selection: first survives", g_cmds[0].source, "  glVertex3f(0, 0, 0);");
        ASSERT_STR("ctrl-d selection: last survives", g_cmds[1].source, "  glVertex3f(4, 4, 4);");
        ASSERT_INT("ctrl-d selection: edit_line", g_edit_line, 1);
        ASSERT_TRUE("ctrl-d selection: selection cleared", !sel_active());
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
        ASSERT_INT("delete undo setup: leaves 2 cmds", g_num_cmds, 2);

        pop_undo_snapshot();
        ASSERT_INT("delete undo: restores 5 cmds", g_num_cmds, 5);
        ASSERT_STR("delete undo: middle restored", g_cmds[2].source, "  glVertex3f(2, 2, 2);");

        do_redo();
        ASSERT_INT("delete redo: leaves 2 cmds", g_num_cmds, 2);
        ASSERT_STR("delete redo: last survives", g_cmds[1].source, "  glVertex3f(4, 4, 4);");
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
        g_sel_anchor = 1;
        g_sel_end = 2;

        repl_keyboard_func(3, 0, 0);
        ASSERT_INT("copy block: clipboard count", g_clipboard_count, 2);
        ASSERT_STR("copy block: first copied", g_clipboard[0].source, "    glVertex3f(0, 0, 0);");
        ASSERT_STR("copy block: second copied", g_clipboard[1].source, "    glVertex3f(1, 1, 1);");
        ASSERT_TRUE("copy block: selection cleared", !sel_active());

        g_edit_line = 5;
        g_inserting = 0;
        repl_keyboard_func(22, 0, 0);
        ASSERT_INT("paste block: cmd count", g_num_cmds, 9);
        ASSERT_STR("paste block: if preserved", g_cmds[4].source, "  if(x > 0) {");
        ASSERT_STR("paste block: first inserted", g_cmds[5].source, "    glVertex3f(0, 0, 0);");
        ASSERT_STR("paste block: second inserted", g_cmds[6].source, "    glVertex3f(1, 1, 1);");
        ASSERT_STR("paste block: original body shifted", g_cmds[7].source, "    glColor3f(1, 0, 0);");
        ASSERT_STR("paste block: close brace preserved", g_cmds[8].source, "  }");
        ASSERT_INT("paste block: edit_line after pasted block", g_edit_line, 7);
        ASSERT_STR("paste block: input reloaded after paste", g_input, "glColor3f(1, 0, 0)");
        assert_status_contains("paste block: status", "Pasted 2 lines");

        pop_undo_snapshot();
        ASSERT_INT("paste undo: restores original count", g_num_cmds, 7);
        ASSERT_STR("paste undo: original body restored", g_cmds[5].source, "    glColor3f(1, 0, 0);");
        ASSERT_STR("paste undo: close brace restored", g_cmds[6].source, "  }");

        do_redo();
        ASSERT_INT("paste redo: restores pasted count", g_num_cmds, 9);
        ASSERT_STR("paste redo: first pasted line", g_cmds[5].source, "    glVertex3f(0, 0, 0);");
        ASSERT_STR("paste redo: original body shifted again", g_cmds[7].source, "    glColor3f(1, 0, 0);");
    }

    /* 8e. Cut selected block, then undo/redo */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        repl_feed_line_public("glVertex3f(3,3,3)");
        g_sel_anchor = 1;
        g_sel_end = 2;

        repl_keyboard_func(24, 0, 0);
        ASSERT_INT("cut block: clipboard count", g_clipboard_count, 2);
        ASSERT_STR("cut block: first copied", g_clipboard[0].source, "  glVertex3f(1, 1, 1);");
        ASSERT_STR("cut block: second copied", g_clipboard[1].source, "  glVertex3f(2, 2, 2);");
        ASSERT_INT("cut block: leaves 2 cmds", g_num_cmds, 2);
        ASSERT_STR("cut block: first survivor", g_cmds[0].source, "  glVertex3f(0, 0, 0);");
        ASSERT_STR("cut block: second survivor", g_cmds[1].source, "  glVertex3f(3, 3, 3);");
        ASSERT_TRUE("cut block: selection cleared", !sel_active());

        pop_undo_snapshot();
        ASSERT_INT("cut undo: restores 4 cmds", g_num_cmds, 4);
        ASSERT_STR("cut undo: first cut line restored", g_cmds[1].source, "  glVertex3f(1, 1, 1);");

        do_redo();
        ASSERT_INT("cut redo: leaves 2 cmds", g_num_cmds, 2);
        ASSERT_STR("cut redo: second survivor", g_cmds[1].source, "  glVertex3f(3, 3, 3);");
    }

    /* 8f. Copy/cut from a for-begin line captures the whole block */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("for(i, 0, 2) {");
        repl_feed_line_public("glVertex3f(i,0,0);");
        repl_feed_line_public("}");
        g_edit_line = 0;

        repl_keyboard_func(3, 0, 0);
        ASSERT_INT("copy for block: clipboard count", g_clipboard_count, 3);
        ASSERT_INT("copy for block: first type", g_clipboard[0].type, CMD_FOR_BEGIN);
        ASSERT_INT("copy for block: last type", g_clipboard[2].type, CMD_FOR_END);
        ASSERT_INT("copy for block: source unchanged", g_num_cmds, 3);

        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("for(i, 0, 2) {");
        repl_feed_line_public("glVertex3f(i,0,0);");
        repl_feed_line_public("}");
        g_edit_line = 0;

        repl_keyboard_func(24, 0, 0);
        ASSERT_INT("cut for block: clipboard count", g_clipboard_count, 3);
        ASSERT_INT("cut for block: first type", g_clipboard[0].type, CMD_FOR_BEGIN);
        ASSERT_INT("cut for block: last type", g_clipboard[2].type, CMD_FOR_END);
        ASSERT_INT("cut for block: buffer empty", g_num_cmds, 0);
        ASSERT_INT("cut for block: edit line at start", g_edit_line, 0);
    }

    /* 8g. Paste with empty clipboard */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        g_clipboard_count = 0;

        repl_keyboard_func(22, 0, 0);

        ASSERT_INT("paste empty: no mutation", g_num_cmds, 1);
        ASSERT_STR("paste empty: source unchanged", g_cmds[0].source, "  glVertex3f(1, 1, 1);");
        assert_status_contains("paste empty: status", "Clipboard empty");
    }

    /* 8h. Paste refuses to overflow the command buffer */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        g_clipboard[0] = g_cmds[0];
        g_clipboard_count = 1;
        g_num_cmds = MAX_COMMANDS;

        repl_keyboard_func(22, 0, 0);

        ASSERT_INT("paste full: no mutation", g_num_cmds, MAX_COMMANDS);
        ASSERT_INT("paste full: clipboard preserved", g_clipboard_count, 1);
        assert_status_contains("paste full: status", "Command buffer full");
    }

    /* 8i. Clipboard operations reject float declarations */
    {
        repl_reset_state();
        repl_feed_line_public("float n;");
        repl_feed_line_public("n = 5;");
        g_clipboard[0] = g_cmds[1];
        g_clipboard_count = 1;
        g_edit_line = 0;

        repl_keyboard_func(3, 0, 0);

        ASSERT_INT("copy decl line: cmd count unchanged", g_num_cmds, 2);
        ASSERT_INT("copy decl line: clipboard count preserved", g_clipboard_count, 1);
        ASSERT_STR("copy decl line: clipboard source preserved", g_clipboard[0].source, "  n = 5;");
        assert_status_contains("copy decl line: status", "Cannot copy float declarations");

        g_clipboard[0] = g_cmds[0];
        g_clipboard_count = 1;
        g_edit_line = g_num_cmds;

        repl_keyboard_func(22, 0, 0);

        ASSERT_INT("paste decl line: cmd count unchanged", g_num_cmds, 2);
        ASSERT_INT("paste decl line: clipboard count preserved", g_clipboard_count, 1);
        ASSERT_INT("paste decl line: first still decl", g_cmds[0].type, CMD_VAR_DECLARE);
        ASSERT_INT("paste decl line: second still assign", g_cmds[1].type, CMD_VAR_ASSIGN);
        assert_status_contains("paste decl line: status", "Cannot paste float declarations");
    }

    /* 8j. Copy/cut in insert mode clear selection without touching clipboard */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        g_clipboard[0] = g_cmds[1];
        g_clipboard_count = 1;
        g_sel_anchor = 0;
        g_sel_end = 1;
        g_edit_line = 1;
        g_inserting = 1;

        repl_keyboard_func(3, 0, 0);
        ASSERT_INT("copy insert mode: cmd count unchanged", g_num_cmds, 2);
        ASSERT_INT("copy insert mode: clipboard unchanged", g_clipboard_count, 1);
        ASSERT_STR("copy insert mode: clipboard source unchanged", g_clipboard[0].source, "  glVertex3f(2, 2, 2);");
        ASSERT_TRUE("copy insert mode: selection cleared", !sel_active());

        g_sel_anchor = 0;
        g_sel_end = 1;
        g_inserting = 1;
        repl_keyboard_func(24, 0, 0);
        ASSERT_INT("cut insert mode: cmd count unchanged", g_num_cmds, 2);
        ASSERT_INT("cut insert mode: clipboard unchanged", g_clipboard_count, 1);
        ASSERT_STR("cut insert mode: clipboard source unchanged", g_clipboard[0].source, "  glVertex3f(2, 2, 2);");
        ASSERT_TRUE("cut insert mode: selection cleared", !sel_active());
    }

    /* 8k. Backspace in insert mode edits input instead of selected source lines */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        g_sel_anchor = 0;
        g_sel_end = 1;
        g_edit_line = 1;
        g_inserting = 1;
        set_editor_input("abcd");
        g_cursor_pos = 3;

        repl_keyboard_func(8, 0, 0);

        ASSERT_INT("backspace insert mode: cmd count unchanged", g_num_cmds, 2);
        ASSERT_STR("backspace insert mode: input edited", g_input, "abd");
        ASSERT_INT("backspace insert mode: cursor moved", g_cursor_pos, 2);
        ASSERT_INT("backspace insert mode: still inserting", g_inserting, 1);
    }

    /* 8k. Committing incomplete commands reports incomplete, not unknown */
    {
        repl_reset_state();
        set_editor_input("glColor3f(1, 1");

        repl_keyboard_func(';', 0, 0);

        ASSERT_INT("commit incomplete glColor3f: no cmd added", g_num_cmds, 0);
        assert_status_contains("commit incomplete glColor3f: status", "Incomplete command");
        assert_status_contains("commit incomplete glColor3f: missing paren", "missing ')'");
        ASSERT_TRUE("commit incomplete glColor3f: not unknown",
                    strstr(g_status, "Unknown cmd") == NULL);
    }

    {
        repl_reset_state();
        set_editor_input("glTotallyUnknown(1, 2, 3)");

        repl_keyboard_func(';', 0, 0);

        ASSERT_INT("commit unknown command: no cmd added", g_num_cmds, 0);
        assert_status_contains("commit unknown command: status", "Unknown cmd");
    }

    /* 9. load_line_to_input — CMD_LABEL path */
    {
        repl_reset_state();
        /* Feed a label command to create a CMD_LABEL entry */
        repl_feed_line_public(":myloop");
        ASSERT_INT("label cmd created", g_num_cmds, 1);
        ASSERT_INT("label cmd type", g_cmds[0].type, CMD_LABEL);

        /* Now load it back into input */
        load_line_to_input(0);
        /* Input should start with ':' and contain the label name */
        ASSERT_TRUE("label load: starts with colon", g_input[0] == ':');
        ASSERT_TRUE("label load: contains name", strstr(g_input, "myloop") != NULL);
    }

    /* 10. navigate_to_line — clamp target < 0 */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        navigate_to_line(-5);
        ASSERT_INT("navigate_to_line neg: edit_line=0", g_edit_line, 0);
    }

    /* 11. navigate_to_line — clamp target > g_num_cmds */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        navigate_to_line(999);
        ASSERT_INT("navigate_to_line over: edit_line=g_num_cmds", g_edit_line, g_num_cmds);
    }

    /* 12. try_assign_variable — append to end (existing tests cover this) */
    {
        repl_reset_state(); declare_test_vars();
        strcpy(g_input, "n = 10.5");
        g_input_len = (int)strlen(g_input);

        int r = try_assign_variable();
        ASSERT_INT("try_assign_variable returns 1", r, 1);
        ASSERT_INT("num_cmds 1 after assign", g_num_cmds, 1);
        ASSERT_STR("assigned cmd source", g_cmds[0].source, "  n = 10.5;");
    }

    /* 13. try_assign_variable — inserting mode (inserts before cursor) */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        /* Put cursor at line 1, inserting mode */
        g_edit_line = 1;
        g_inserting = 1;
        strcpy(g_input, "x = 3.0");
        g_input_len = (int)strlen(g_input);

        int r = try_assign_variable();
        ASSERT_INT("insert assign returns 1", r, 1);
        ASSERT_INT("insert assign: 3 cmds", g_num_cmds, 3);
        /* The assignment should appear at index 1 */
        ASSERT_INT("insert assign: cmd 1 is VAR_ASSIGN", g_cmds[1].type, CMD_VAR_ASSIGN);
    }

    /* 14. try_assign_variable — overwrite existing cmd */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("n = 1.0");
        repl_feed_line_public("glVertex3f(1,1,1)");
        /* Navigate to the assignment line and overwrite */
        g_edit_line = 0;
        g_inserting = 0;
        strcpy(g_input, "n = 7.0");
        g_input_len = (int)strlen(g_input);

        int r = try_assign_variable();
        ASSERT_INT("overwrite assign returns 1", r, 1);
        ASSERT_STR("overwrite assign: new source", g_cmds[0].source, "  n = 7.0;");
        ASSERT_INT("overwrite assign: edit_line advanced", g_edit_line, 1);
    }

    /* 15. Committing for loops — basic open brace form */
    {
        repl_reset_state(); declare_test_vars();
        strcpy(g_input, "for(i, 0, 5) {");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_for_loop();
        ASSERT_INT("try_commit_for_loop returns 1", r, 1);
        ASSERT_INT("num_cmds 2 after for", g_num_cmds, 2);
        ASSERT_STR("for loop source", g_cmds[0].source, "  for(i, 0, 5) {");
        ASSERT_STR("for end source", g_cmds[1].source, "  }");
    }

    /* 16. try_commit_for_loop — with explicit step */
    {
        repl_reset_state();
        strcpy(g_input, "for(i, 0, 10, 2) {");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_for_loop();
        ASSERT_INT("for with step returns 1", r, 1);
        ASSERT_INT("for with step: 2 cmds", g_num_cmds, 2);
        /* Source should contain step value */
        ASSERT_TRUE("for with step: source has 2 step",
                    strstr(g_cmds[0].source, "2") != NULL);
    }

    /* 17. try_commit_for_loop — update existing for-begin header */
    {
        repl_reset_state();
        /* Create an existing for-loop */
        strcpy(g_input, "for(i, 0, 5) {");
        g_input_len = (int)strlen(g_input);
        try_commit_for_loop();
        ASSERT_INT("setup: 2 cmds", g_num_cmds, 2);

        /* Navigate back to line 0 and update the for header */
        g_edit_line = 0;
        g_inserting = 0;
        strcpy(g_input, "for(i, 0, 10) {");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_for_loop();
        ASSERT_INT("update for-begin returns 1", r, 1);
        /* Should update in place, not add more commands */
        ASSERT_INT("update for-begin: still 2 cmds", g_num_cmds, 2);
        ASSERT_TRUE("update for-begin: source updated",
                    strstr(g_cmds[0].source, "10") != NULL);
    }

    /* 18. try_commit_for_loop — inline form (for with body on same line) */
    {
        repl_reset_state();
        strcpy(g_input, "for(i, 0, 3) glVertex3f(i,0,0);");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_for_loop();
        ASSERT_INT("for inline returns 1", r, 1);
        /* Should create 3 cmds: for-begin, body, for-end */
        ASSERT_INT("for inline: 3 cmds", g_num_cmds, 3);
        ASSERT_INT("for inline: cmd 0 is FOR_BEGIN", g_cmds[0].type, CMD_FOR_BEGIN);
        ASSERT_INT("for inline: cmd 2 is FOR_END", g_cmds[2].type, CMD_FOR_END);
    }

    /* 19. Committing func defs — basic */
    {
        repl_reset_state(); declare_test_vars();
        strcpy(g_input, "func0(x, y) {");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_func_def();
        ASSERT_INT("try_commit_func_def returns 1", r, 1);
        ASSERT_INT("num_cmds 2 after func", g_num_cmds, 2);
        ASSERT_STR("func def source", g_cmds[0].source, "  func0(x, y) {");
        ASSERT_STR("func end source", g_cmds[1].source, "  }");
    }

    /* 20. try_commit_func_def — update existing func-def header */
    {
        repl_reset_state();
        strcpy(g_input, "func1(a) {");
        g_input_len = (int)strlen(g_input);
        try_commit_func_def();
        ASSERT_INT("func update setup: 2 cmds", g_num_cmds, 2);

        /* Navigate back and update the func-def header */
        g_edit_line = 0;
        g_inserting = 0;
        strcpy(g_input, "func1(a, b) {");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_func_def();
        ASSERT_INT("update func-def returns 1", r, 1);
        ASSERT_INT("update func-def: still 2 cmds", g_num_cmds, 2);
        ASSERT_TRUE("update func-def: source has b param",
                    strstr(g_cmds[0].source, "b") != NULL);
    }

    /* 21. Committing if blocks — basic */
    {
        repl_reset_state(); declare_test_vars();
        strcpy(g_input, "if(x > 0) {");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_if_block();
        ASSERT_INT("try_commit_if_block returns 1", r, 1);
        ASSERT_INT("num_cmds 2 after if", g_num_cmds, 2);
        ASSERT_STR("if block source", g_cmds[0].source, "  if(x > 0) {");
        ASSERT_STR("if end source", g_cmds[1].source, "  }");
    }

    /* 22. try_commit_if_block — update existing if-begin */
    {
        repl_reset_state(); declare_test_vars();
        strcpy(g_input, "if(x > 0) {");
        g_input_len = (int)strlen(g_input);
        try_commit_if_block();

        /* Navigate back and update the if condition */
        g_edit_line = 0;
        g_inserting = 0;
        strcpy(g_input, "if(x < 0) {");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_if_block();
        ASSERT_INT("update if-begin returns 1", r, 1);
        ASSERT_INT("update if-begin: still 2 cmds", g_num_cmds, 2);
        ASSERT_STR("update if-begin: source updated", g_cmds[0].source, "  if(x < 0) {");
    }

    /* 23. Committing close brace — for-loop */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("for(i, 0, 1) {");
        strcpy(g_input, "}");
        g_input_len = (int)strlen(g_input);
        g_edit_line = 1;

        int r = try_commit_close_brace();
        ASSERT_INT("try_commit_close_brace returns 1", r, 1);
        ASSERT_INT("num_cmds after brace", g_num_cmds, 2);
        ASSERT_STR("close brace source", g_cmds[1].source, "  }");
    }

    /* 26. Committing close brace — while in inserting mode closes existing end */
    {
        repl_reset_state();
        /* Set up: for-loop with begin+end, enter inserting mode inside */
        strcpy(g_input, "for(i, 0, 3) {");
        g_input_len = (int)strlen(g_input);
        try_commit_for_loop();
        /* g_edit_line=1, g_inserting=1 — we're inside the loop */
        ASSERT_INT("insert-close setup: in inserting", g_inserting, 1);
        ASSERT_INT("insert-close setup: edit_line=1", g_edit_line, 1);

        strcpy(g_input, "}");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_close_brace();
        ASSERT_INT("insert-close brace returns 1", r, 1);
        ASSERT_INT("insert-close: no longer inserting", g_inserting, 0);
    }

    /* 27. Committing close brace — func-def */
    {
        repl_reset_state();
        strcpy(g_input, "func2() {");
        g_input_len = (int)strlen(g_input);
        try_commit_func_def();
        /* Now close the func with '}' */
        strcpy(g_input, "}");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_close_brace();
        ASSERT_INT("func close brace returns 1", r, 1);
        ASSERT_INT("func close: no longer inserting", g_inserting, 0);
    }

    /* 28. Committing close brace — if-block */
    {
        repl_reset_state(); declare_test_vars();
        strcpy(g_input, "if(x > 0) {");
        g_input_len = (int)strlen(g_input);
        try_commit_if_block();
        /* Inserting inside if-block, close it */
        strcpy(g_input, "}");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_close_brace();
        ASSERT_INT("if close brace returns 1", r, 1);
        ASSERT_INT("if close: no longer inserting", g_inserting, 0);
    }

    /* 29. try_commit_for_loop — empty body emits error */
    {
        repl_reset_state();
        strcpy(g_input, "for(i, 0, 3) ;");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_for_loop();
        ASSERT_INT("for empty body returns 1 (error)", r, 1);
        /* No commands committed on bad body */
        ASSERT_INT("for empty body: no cmds added", g_num_cmds, 0);
    }

    /* 30. try_commit_func_def — func with no params */
    {
        repl_reset_state();
        strcpy(g_input, "func3() {");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_func_def();
        ASSERT_INT("func no-params returns 1", r, 1);
        ASSERT_INT("func no-params: 2 cmds", g_num_cmds, 2);
        ASSERT_TRUE("func no-params: source correct",
                    strstr(g_cmds[0].source, "func3") != NULL);
    }

    /* 24. prof_frame_tick — increments staleness counters */
    {
        /* Call begin/end to prime a section, then tick several frames */
        prof_begin(PROF_SCENE_3D);
        prof_end(PROF_SCENE_3D);
        /* Frame tick — sections that didn't run this frame become stale */
        prof_frame_tick();
        prof_frame_tick();
        /* Should not crash; calling multiple times is fine */
        ASSERT_TRUE("prof_frame_tick survives", 1);
    }

    /* 25. prof_code_panel_details_enabled — reflects g_show_profile_panel */
    {
        g_show_profile_panel = PROFILE_PANEL_OFF;
        ASSERT_INT("details disabled when OFF", prof_code_panel_details_enabled(), 0);

        g_show_profile_panel = PROFILE_PANEL_DETAILS;
        ASSERT_INT("details enabled when DETAILS", prof_code_panel_details_enabled(), 1);

        g_show_profile_panel = PROFILE_PANEL_OFF;  /* restore */
    }

    /* ---- Float declaration parsing (regression tests) ---- */

    /* 26. float decl without trailing semicolon (interactive ';' key path) */
    {
        repl_reset_state();

        /* Simulate the interactive ';' key handler: g_input has no ';' */
        strncpy(g_input, "float tmp", MAX_INPUT_LEN - 1);
        g_input_len = (int)strlen(g_input);
        g_cursor_pos = g_input_len;
        g_edit_line = g_num_cmds;
        g_inserting = 0;

        int result = try_commit_float_decl();
        ASSERT_INT("float_decl no-semi: accepted", result, 1);
        ASSERT_INT("float_decl no-semi: cmd added", g_num_cmds, 1);
        ASSERT_INT("float_decl no-semi: type", g_cmds[0].type, CMD_VAR_DECLARE);
        ASSERT_INT("float_decl no-semi: var_decl_count", g_cmds[0].var_decl_count, 1);
        ASSERT_STR("float_decl no-semi: var name", g_cmds[0].var_names[0], "tmp");
        ASSERT_TRUE("float_decl no-semi: predef registered",
                     find_predef_var_idx("tmp") >= 0);
    }

    /* 27. float decl WITH trailing semicolon (feed_line path) */
    {
        repl_reset_state();
        repl_feed_line_public("float abc;");
        ASSERT_INT("float_decl with-semi: cmd added", g_num_cmds, 1);
        ASSERT_INT("float_decl with-semi: type", g_cmds[0].type, CMD_VAR_DECLARE);
        ASSERT_STR("float_decl with-semi: var name", g_cmds[0].var_names[0], "abc");
        ASSERT_TRUE("float_decl with-semi: predef registered",
                     find_predef_var_idx("abc") >= 0);
    }

    /* 28. float decl with initializer, no trailing semicolon */
    {
        repl_reset_state();

        strncpy(g_input, "float tmp = 0", MAX_INPUT_LEN - 1);
        g_input_len = (int)strlen(g_input);
        g_cursor_pos = g_input_len;
        g_edit_line = g_num_cmds;
        g_inserting = 0;

        int result = try_commit_float_decl();
        ASSERT_INT("float_decl init no-semi: accepted", result, 1);
        ASSERT_INT("float_decl init no-semi: cmd added", g_num_cmds, 1);
        ASSERT_INT("float_decl init no-semi: type", g_cmds[0].type, CMD_VAR_DECLARE);
        ASSERT_STR("float_decl init no-semi: var name", g_cmds[0].var_names[0], "tmp");
        int idx = find_predef_var_idx("tmp");
        ASSERT_TRUE("float_decl init no-semi: predef registered", idx >= 0);
        if (idx >= 0)
            ASSERT_TRUE("float_decl init no-semi: value is 0",
                         g_predef_vars[idx].value == 0.0f);
    }

    /* 29. float decl with initializer expression */
    {
        repl_reset_state();
        repl_feed_line_public("float radius = 2.5;");
        ASSERT_INT("float_decl init expr: cmd added", g_num_cmds, 1);
        ASSERT_INT("float_decl init expr: type", g_cmds[0].type, CMD_VAR_DECLARE);
        ASSERT_STR("float_decl init expr: var name", g_cmds[0].var_names[0], "radius");
        int idx = find_predef_var_idx("radius");
        ASSERT_TRUE("float_decl init expr: registered", idx >= 0);
        if (idx >= 0)
            ASSERT_TRUE("float_decl init expr: value is 2.5",
                         g_predef_vars[idx].value == 2.5f);
    }

    /* 30. multi-name float decl without semicolon */
    {
        repl_reset_state();

        strncpy(g_input, "float a, b, c", MAX_INPUT_LEN - 1);
        g_input_len = (int)strlen(g_input);
        g_cursor_pos = g_input_len;
        g_edit_line = g_num_cmds;
        g_inserting = 0;

        int result = try_commit_float_decl();
        ASSERT_INT("float_decl multi no-semi: accepted", result, 1);
        ASSERT_INT("float_decl multi no-semi: cmd added", g_num_cmds, 1);
        ASSERT_INT("float_decl multi no-semi: var_decl_count",
                   g_cmds[0].var_decl_count, 3);
        ASSERT_TRUE("float_decl multi no-semi: a registered",
                     find_predef_var_idx("a") >= 0);
        ASSERT_TRUE("float_decl multi no-semi: b registered",
                     find_predef_var_idx("b") >= 0);
        ASSERT_TRUE("float_decl multi no-semi: c registered",
                     find_predef_var_idx("c") >= 0);
    }

    /* 31. multi-name float decl with initializers */
    {
        repl_reset_state();
        repl_feed_line_public("float x = 1, y = 2;");
        ASSERT_INT("float_decl multi init: cmd added", g_num_cmds, 1);
        ASSERT_INT("float_decl multi init: var_decl_count",
                   g_cmds[0].var_decl_count, 2);
        int xi = find_predef_var_idx("x");
        int yi = find_predef_var_idx("y");
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
        ASSERT_INT("decl-top baseline: 3 cmds", g_num_cmds, 3);

        repl_feed_line_public("float tmp = 40;");
        ASSERT_INT("decl-top: 4 cmds after float tmp", g_num_cmds, 4);
        ASSERT_INT("decl-top: g_cmds[0] is float n", g_cmds[0].type, CMD_VAR_DECLARE);
        ASSERT_STR("decl-top: g_cmds[0] name", g_cmds[0].var_names[0], "n");
        ASSERT_INT("decl-top: g_cmds[1] is float tmp", g_cmds[1].type, CMD_VAR_DECLARE);
        ASSERT_STR("decl-top: g_cmds[1] name", g_cmds[1].var_names[0], "tmp");
        ASSERT_INT("decl-top: g_cmds[2] is assign", g_cmds[2].type, CMD_VAR_ASSIGN);
        ASSERT_INT("decl-top: g_cmds[3] is glBegin", g_cmds[3].type, CMD_BEGIN);
    }

    /* 33. float decl from edit position in middle of code still
     * lands at the top (not at the cursor). */
    {
        repl_reset_state();
        repl_feed_line_public("glBegin(GL_LINES);");
        repl_feed_line_public("glEnd();");
        ASSERT_INT("decl-top mid: 2 cmds", g_num_cmds, 2);

        /* Simulate being on line 1 in overwrite mode when committing
         * a float decl through the ';' key (no trailing ';'). */
        strncpy(g_input, "float radius = 3", MAX_INPUT_LEN - 1);
        g_input_len = (int)strlen(g_input);
        g_cursor_pos = g_input_len;
        g_edit_line = 1;
        g_inserting = 0;

        int result = try_commit_float_decl();
        ASSERT_INT("decl-top mid: accepted", result, 1);
        ASSERT_INT("decl-top mid: 3 cmds", g_num_cmds, 3);
        ASSERT_INT("decl-top mid: g_cmds[0] is decl",
                   g_cmds[0].type, CMD_VAR_DECLARE);
        ASSERT_STR("decl-top mid: g_cmds[0] name",
                   g_cmds[0].var_names[0], "radius");
        ASSERT_INT("decl-top mid: g_cmds[1] preserved glBegin",
                   g_cmds[1].type, CMD_BEGIN);
        ASSERT_INT("decl-top mid: g_cmds[2] preserved glEnd",
                   g_cmds[2].type, CMD_END);
    }

    /* 34. Overwriting a multi-name decl that shares a name with the new
     * decl must not fail with "already declared". Regression: previously
     * `declare_predef_var` ran before `undeclare_predef_var`, so shared
     * names (still registered from the old decl) collided. */
    {
        repl_reset_state();
        repl_feed_line_public("float a, b;");
        ASSERT_INT("overwrite shared: baseline cmd count", g_num_cmds, 1);
        ASSERT_TRUE("overwrite shared: a registered",
                    find_predef_var_idx("a") >= 0);
        ASSERT_TRUE("overwrite shared: b registered",
                    find_predef_var_idx("b") >= 0);

        strncpy(g_input, "float a, c", MAX_INPUT_LEN - 1);
        g_input_len = (int)strlen(g_input);
        g_cursor_pos = g_input_len;
        g_edit_line = 0;
        g_inserting = 0;

        int result = try_commit_float_decl();
        ASSERT_INT("overwrite shared: accepted", result, 1);
        ASSERT_INT("overwrite shared: still 1 cmd", g_num_cmds, 1);
        ASSERT_INT("overwrite shared: var_decl_count", g_cmds[0].var_decl_count, 2);
        ASSERT_STR("overwrite shared: slot 0 is a", g_cmds[0].var_names[0], "a");
        ASSERT_STR("overwrite shared: slot 1 is c", g_cmds[0].var_names[1], "c");
        ASSERT_TRUE("overwrite shared: a still registered",
                    find_predef_var_idx("a") >= 0);
        ASSERT_TRUE("overwrite shared: c registered",
                    find_predef_var_idx("c") >= 0);
        ASSERT_TRUE("overwrite shared: b unregistered",
                    find_predef_var_idx("b") < 0);
    }

    /* 35. Overwriting `float n, b;` → `float n;` with `n` referenced
     * elsewhere must succeed (only `b` is being dropped, and `b` is not
     * referenced). Regression: previously the check iterated ALL old
     * names and errored on `n is in use` even though `n` is being kept. */
    {
        repl_reset_state();
        repl_feed_line_public("float n, b;");
        repl_feed_line_public("n = 5;");
        ASSERT_INT("drop b: baseline cmd count", g_num_cmds, 2);

        strncpy(g_input, "float n", MAX_INPUT_LEN - 1);
        g_input_len = (int)strlen(g_input);
        g_cursor_pos = g_input_len;
        g_edit_line = 0;
        g_inserting = 0;

        int result = try_commit_float_decl();
        ASSERT_INT("drop b: accepted", result, 1);
        ASSERT_INT("drop b: var_decl_count now 1", g_cmds[0].var_decl_count, 1);
        ASSERT_STR("drop b: slot 0 is n", g_cmds[0].var_names[0], "n");
        ASSERT_TRUE("drop b: n still registered",
                    find_predef_var_idx("n") >= 0);
        ASSERT_TRUE("drop b: b unregistered",
                    find_predef_var_idx("b") < 0);
    }

    /* 36. Attempting to drop a name that IS referenced elsewhere must
     * fail, and the error must name the dropped variable (not a
     * different name in the same decl). */
    {
        repl_reset_state();
        repl_feed_line_public("float n, b;");
        repl_feed_line_public("b = 7;");
        ASSERT_INT("drop referenced b: baseline", g_num_cmds, 2);

        strncpy(g_input, "float n", MAX_INPUT_LEN - 1);
        g_input_len = (int)strlen(g_input);
        g_cursor_pos = g_input_len;
        g_edit_line = 0;
        g_inserting = 0;

        int result = try_commit_float_decl();
        ASSERT_INT("drop referenced b: handler consumed input", result, 1);
        /* Overwrite must have been rejected: decl still has both names */
        ASSERT_INT("drop referenced b: decl unchanged",
                   g_cmds[0].var_decl_count, 2);
        ASSERT_STR("drop referenced b: b preserved",
                   g_cmds[0].var_names[1], "b");
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
                    find_predef_var_idx("n") >= 0);

        delete_cmd_range(0, 1, "Deleted");

        ASSERT_INT("delete referenced decl: cmd count unchanged", g_num_cmds, 2);
        ASSERT_INT("delete referenced decl: first still decl", g_cmds[0].type, CMD_VAR_DECLARE);
        ASSERT_INT("delete referenced decl: second still assign", g_cmds[1].type, CMD_VAR_ASSIGN);
        ASSERT_TRUE("delete referenced decl: n still registered",
                    find_predef_var_idx("n") >= 0);
        assert_status_contains("delete referenced decl: status", "Cannot remove float declarations");
    }

    /* 37a. Ctrl+X rejects cutting declaration lines */
    {
        repl_reset_state();
        repl_feed_line_public("float n;");
        repl_feed_line_public("n = 5;");
        g_clipboard[0] = g_cmds[1];
        g_clipboard_count = 1;
        g_edit_line = 0;

        repl_keyboard_func(24, 0, 0);

        ASSERT_INT("cut referenced decl: cmd count unchanged", g_num_cmds, 2);
        ASSERT_INT("cut referenced decl: first still decl", g_cmds[0].type, CMD_VAR_DECLARE);
        ASSERT_INT("cut referenced decl: second still assign", g_cmds[1].type, CMD_VAR_ASSIGN);
        ASSERT_TRUE("cut referenced decl: n still registered",
                    find_predef_var_idx("n") >= 0);
        ASSERT_INT("cut referenced decl: clipboard count preserved", g_clipboard_count, 1);
        ASSERT_STR("cut referenced decl: clipboard source preserved", g_clipboard[0].source, "  n = 5;");
        assert_status_contains("cut referenced decl: status", "Cannot remove float declarations");
    }

    /* 37b. Ctrl+X rejects blocks that include a declaration and all uses */
    {
        repl_reset_state();
        repl_feed_line_public("float n;");
        repl_feed_line_public("n = 5;");
        ASSERT_TRUE("cut decl block setup: n registered",
                    find_predef_var_idx("n") >= 0);
        g_clipboard[0] = g_cmds[1];
        g_clipboard_count = 1;
        g_sel_anchor = 0;
        g_sel_end = 1;

        repl_keyboard_func(24, 0, 0);

        ASSERT_INT("cut decl block: cmd count unchanged", g_num_cmds, 2);
        ASSERT_TRUE("cut decl block: n still registered",
                    find_predef_var_idx("n") >= 0);
        ASSERT_INT("cut decl block: clipboard count preserved", g_clipboard_count, 1);
        ASSERT_STR("cut decl block: clipboard source preserved", g_clipboard[0].source, "  n = 5;");
        assert_status_contains("cut decl block: status", "Cannot remove float declarations");
    }

    /* 38. Deleting a declaration with all its uses is still rejected */
    {
        repl_reset_state();
        repl_feed_line_public("float n;");
        repl_feed_line_public("n = 5;");
        ASSERT_TRUE("delete decl block setup: n registered",
                    find_predef_var_idx("n") >= 0);

        delete_cmd_range(0, 2, "Deleted");

        ASSERT_INT("delete decl block: cmd count unchanged", g_num_cmds, 2);
        ASSERT_TRUE("delete decl block: n still registered",
                    find_predef_var_idx("n") >= 0);
        assert_status_contains("delete decl block: status", "Cannot remove float declarations");
    }

    /* 39. Per-declaration name limit rejects atomically */
    {
        repl_reset_state();
        set_editor_input("float v0, v1, v2, v3, v4, v5, v6, v7, v8");
        g_edit_line = g_num_cmds;
        g_inserting = 0;

        int result = try_commit_float_decl();

        ASSERT_INT("decl per-line overflow: handler consumed input", result, 1);
        ASSERT_INT("decl per-line overflow: no cmd added", g_num_cmds, 0);
        ASSERT_INT("decl per-line overflow: only t registered", g_num_predef_vars, 1);
        for (int i = 0; i <= MAX_NAMES_PER_DECL; i++) {
            char name[16];
            char label[128];
            snprintf(name, sizeof(name), "v%d", i);
            snprintf(label, sizeof(label), "decl per-line overflow: %s not registered", name);
            ASSERT_TRUE(label, find_predef_var_idx(name) < 0);
        }
        assert_status_contains("decl per-line overflow: status count", "too many names per declaration");
        assert_status_contains("decl per-line overflow: status max", "max 8");
    }

    /* 40. Total variable table limit rejects atomically */
    {
        repl_reset_state();
        repl_feed_line_public("float v0, v1, v2, v3, v4, v5, v6, v7;");
        repl_feed_line_public("float v8, v9, v10, v11, v12, v13, v14;");
        ASSERT_INT("decl table full setup: two decl cmds", g_num_cmds, 2);
        ASSERT_INT("decl table full setup: all slots used", g_num_predef_vars, MAX_PREDEF_VARS);

        set_editor_input("float overflow");
        g_edit_line = g_num_cmds;
        g_inserting = 0;
        int result = try_commit_float_decl();

        ASSERT_INT("decl table full: handler consumed input", result, 1);
        ASSERT_INT("decl table full: cmd count unchanged", g_num_cmds, 2);
        ASSERT_INT("decl table full: var count unchanged", g_num_predef_vars, MAX_PREDEF_VARS);
        ASSERT_TRUE("decl table full: overflow not registered",
                    find_predef_var_idx("overflow") < 0);
        ASSERT_TRUE("decl table full: first existing var remains",
                    find_predef_var_idx("v0") >= 0);
        ASSERT_TRUE("decl table full: last existing var remains",
                    find_predef_var_idx("v14") >= 0);
        assert_status_contains("decl table full: status full", "variable table full");
        assert_status_contains("decl table full: status max", "max 16");
    }

    /* 41. Declaration validation failures are atomic */
    {
        repl_reset_state();
        repl_feed_line_public("float anchor;");
        ASSERT_INT("decl atomic setup: one decl", g_num_cmds, 1);
        ASSERT_TRUE("decl atomic setup: anchor registered",
                    find_predef_var_idx("anchor") >= 0);

        assert_float_decl_rejected_atomic("decl duplicate", "float dup, dup",
                                          "dup", "duplicate name 'dup'");
        assert_float_decl_rejected_atomic("decl reserved", "float sin",
                                          "sin", "'sin' is reserved");
        assert_float_decl_rejected_atomic("decl invalid", "float 1bad",
                                          "1bad", "expected identifier");
        assert_float_decl_rejected_atomic("decl overlong", "float abcdefghijklmnop",
                                          "abcdefghijklmnop", "max 15");

        ASSERT_INT("decl atomic: only anchor cmd remains", g_num_cmds, 1);
        ASSERT_INT("decl atomic: only t plus anchor registered", g_num_predef_vars, 2);
        ASSERT_TRUE("decl atomic: anchor still registered",
                    find_predef_var_idx("anchor") >= 0);
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
        ASSERT_INT("expand decl: baseline cmd count", g_num_cmds, 4);

        int ai = find_predef_var_idx("a");
        int bi = find_predef_var_idx("b");
        int ci = find_predef_var_idx("c");
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
        strncpy(g_input, "float a, b, c, d", MAX_INPUT_LEN - 1);
        g_input_len = (int)strlen(g_input);
        g_cursor_pos = g_input_len;
        g_edit_line = 0;
        g_inserting = 0;

        int result = try_commit_float_decl();
        ASSERT_INT("expand decl: accepted", result, 1);
        ASSERT_INT("expand decl: still 4 cmds", g_num_cmds, 4);
        ASSERT_INT("expand decl: var_decl_count is 4", g_cmds[0].var_decl_count, 4);

        ai = find_predef_var_idx("a");
        bi = find_predef_var_idx("b");
        ci = find_predef_var_idx("c");
        int di = find_predef_var_idx("d");
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
        ASSERT_INT("expand decl: assign a slot correct", g_cmds[1].num_args, ai);
        ASSERT_INT("expand decl: assign b slot correct", g_cmds[2].num_args, bi);
        ASSERT_INT("expand decl: assign c slot correct", g_cmds[3].num_args, ci);
    }

    /* try_assign_variable — overlong formatted source is rejected with
     * "Command too long" and does not mutate g_cmds / g_num_cmds. */
    {
        repl_reset_state(); declare_test_vars();
        int old_num_cmds = g_num_cmds;
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
        g_edit_line = g_num_cmds;
        g_inserting = 0;

        int r = try_assign_variable();
        ASSERT_INT("overlong assign: handler consumed input", r, 1);
        ASSERT_INT("overlong assign: num_cmds unchanged",
                   g_num_cmds, old_num_cmds);
        assert_status_contains("overlong assign: status", "Command too long");
    }

    /* Replay keyboard dispatch is owned by repl_replay.c, not the editor. */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("glVertex3f(0, 0, 0);");
        repl_feed_line_public("glVertex3f(1, 0, 0);");
        repl_flatten_commands();

        ASSERT_INT("replay key ctrl-r consumed",
                   replay_handle_key(KEY_CTRL_R), 1);
        ASSERT_INT("replay key ctrl-r starts replay",
                   g_replay_active, 1);
        ASSERT_INT("replay space consumed",
                   replay_handle_key(' '), 1);
        ASSERT_INT("replay space pauses",
                   g_replay_state, REPLAY_PAUSED);
        ASSERT_INT("replay space resumes",
                   replay_handle_key(' '), 1);
        ASSERT_INT("replay resumed playing",
                   g_replay_state, REPLAY_PLAYING);

        g_replay_state = REPLAY_PAUSED;
        ASSERT_INT("replay right consumed",
                   replay_handle_special_key(GLUT_KEY_RIGHT), 1);
        ASSERT_INT("replay right advances one step",
                   g_replay_pc, 1);
        ASSERT_INT("replay left consumed",
                   replay_handle_special_key(GLUT_KEY_LEFT), 1);
        ASSERT_INT("replay left steps back",
                   g_replay_pc, 0);

        g_code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN;
        g_replay_state = REPLAY_PLAYING;
        repl_keyboard_func(' ', 0, 0);
        ASSERT_INT("replay space keeps hidden code panel",
                   g_code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);
        ASSERT_INT("replay space still pauses through editor",
                   g_replay_state, REPLAY_PAUSED);

        g_replay_state = REPLAY_PAUSED;
        g_replay_pc = 0;
        repl_special_func(GLUT_KEY_RIGHT, 0, 0);
        ASSERT_INT("replay right keeps hidden code panel",
                   g_code_panel_layout, CODE_PANEL_LAYOUT_HIDDEN);
        ASSERT_INT("replay right still advances through editor",
                   g_replay_pc, 1);
        g_code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT;

        ASSERT_INT("replay unknown key unconsumed",
                   replay_handle_key('x'), 0);
        ASSERT_INT("replay unknown key stops replay",
                   g_replay_active, 0);
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

        g_win_w = 800;
        g_win_h = 230;
        g_panel_frac = 0.5f;
        g_code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        g_show_indices = 0;
        navigate_to_line(0);

        g_replay_active = 1;
        g_replay_state = REPLAY_PAUSED;
        g_replay_pc = g_num_flat_cmds;
        g_replay_src_line = 25;
        g_scroll = 0;
        g_scroll_follow_cursor = 0;

        (void)code_panel_apply_scroll_follow_for_test(&follow_doc_line,
                                                      &visible_lines);
        ASSERT_TRUE("replay follow helper computes target",
                    follow_doc_line >= 0);
        ASSERT_INT("code panel visible rows match rendered rows",
                   visible_lines, 9);

        g_scroll = follow_doc_line - 9;
        g_scroll_follow_cursor = 1;

        ASSERT_TRUE("replay follow helper reports visible",
                    code_panel_apply_scroll_follow_for_test(&follow_doc_line,
                                                            &visible_lines));
        ASSERT_INT("replay follow scrolls row above status bar",
                   g_scroll, follow_doc_line - visible_lines + 1);
        ASSERT_TRUE("replay follow line after scroll",
                    follow_doc_line >= g_scroll &&
                    follow_doc_line < g_scroll + visible_lines);
        ASSERT_INT("replay follow leaves edit line alone", g_edit_line, 0);
        ASSERT_STR("replay follow leaves input alone",
                   g_input, "glVertex3f(0, 0, 0)");

        g_replay_active = 0;
        g_replay_state = REPLAY_OFF;
        g_replay_src_line = -1;
        g_replay_pc = 0;
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

        g_replay_mode = REPLAY_MODE_VERTEX;
        replay_start();
        replay_advance();
        ASSERT_INT("replay vertex mode focuses first gluVertex",
                   g_replay_src_line, 2);
        replay_advance();
        ASSERT_INT("replay vertex mode focuses next gluVertex",
                   g_replay_src_line, 3);
        replay_stop();

        g_replay_mode = REPLAY_MODE_POLYGON;
        replay_start();
        replay_advance();
        ASSERT_INT("replay polygon mode focuses tess vertex",
                   g_replay_src_line, 4);
        replay_stop();
        g_replay_mode = REPLAY_MODE_VERTEX;
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

        g_win_w = 800;
        g_win_h = 230;
        g_panel_frac = 0.5f;
        g_code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        g_show_indices = 0;
        navigate_to_line(0);

        g_replay_active = 1;
        g_replay_state = REPLAY_PAUSED;
        g_replay_pc = g_num_flat_cmds;
        g_replay_src_line = 1;
        g_scroll = 0;
        g_scroll_follow_cursor = 0;

        g_replay_expand_args = 0;
        (void)code_panel_apply_scroll_follow_for_test(&collapsed_follow,
                                                      &visible_lines);
        ASSERT_TRUE("collapsed replay follow resolves command row",
                    collapsed_follow >= 0);

        g_replay_expand_args = 1;
        (void)code_panel_apply_scroll_follow_for_test(&expanded_follow,
                                                      &visible_lines);
        ASSERT_INT("expanded replay follows final annotation row",
                   expanded_follow, collapsed_follow + 2);

        g_replay_expand_args = 0;
        expanded_follow = -1;
        (void)code_panel_apply_scroll_follow_for_test(&expanded_follow,
                                                      &visible_lines);
        ASSERT_INT("collapsed replay removes annotation rows from follow",
                   expanded_follow, collapsed_follow);

        g_replay_expand_args = 1;
        g_replay_active = 0;
        g_replay_state = REPLAY_OFF;
        g_replay_src_line = -1;
        g_replay_pc = 0;
    }

    /* Regression: pressing Enter on the last existing command must enter
     * insert mode at g_edit_line == g_num_cmds.  Before the fix the
     * cursor was invisible because the renderer guarded the active-input
     * row with !g_inserting, so the slot drew a dimmed placeholder instead
     * of the cursor. */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        navigate_to_line(2);
        ASSERT_INT("enter-at-last: setup edit_line", g_edit_line, 2);
        ASSERT_INT("enter-at-last: setup not inserting", g_inserting, 0);

        repl_keyboard_func('\r', 0, 0);

        ASSERT_INT("enter-at-last: now inserting", g_inserting, 1);
        ASSERT_INT("enter-at-last: edit_line == num_cmds", g_edit_line, g_num_cmds);
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

        g_win_w = 800;
        g_win_h = 230;
        g_panel_frac = 0.5f;
        g_code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        g_show_indices = 0;
        g_replay_active = 0;

        g_edit_line = g_num_cmds;
        g_inserting = 1;
        g_input[0] = '\0';
        g_input_len = 0;
        g_cursor_pos = 0;
        g_scroll = 0;
        g_scroll_follow_cursor = 1;
        code_panel_apply_scroll_follow_for_test(&follow_insert, &visible_lines);
        ASSERT_TRUE("insert-at-end cursor in visible region",
                    follow_insert >= g_scroll &&
                    follow_insert < g_scroll + visible_lines);

        g_edit_line = g_num_cmds;
        g_inserting = 0;
        g_scroll = 0;
        g_scroll_follow_cursor = 1;
        code_panel_apply_scroll_follow_for_test(&follow_overwrite, &visible_lines);

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
        repl_special_func(GLUT_KEY_UP, 0, 0);
        ASSERT_INT("up key sets scroll_follow_cursor", g_scroll_follow_cursor, 1);

        g_scroll_follow_cursor = 0;
        repl_special_func(GLUT_KEY_DOWN, 0, 0);
        ASSERT_INT("down key sets scroll_follow_cursor", g_scroll_follow_cursor, 1);

        /* Page Up/Down scroll the view manually and must NOT trigger cursor
         * follow — that would snap the view back to the cursor position,
         * defeating the purpose of the scroll. */
        g_scroll_follow_cursor = 0;
        repl_special_func(GLUT_KEY_PAGE_UP, 0, 0);
        ASSERT_INT("page up cancels scroll_follow_cursor", g_scroll_follow_cursor, 0);

        g_scroll_follow_cursor = 0;
        repl_special_func(GLUT_KEY_PAGE_DOWN, 0, 0);
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

        g_win_w = 800;
        g_win_h = 230;
        g_panel_frac = 0.5f;
        g_code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        g_show_indices = 0;
        g_replay_active = 0;

        navigate_to_line(g_num_cmds - 1);
        g_scroll = 0;
        g_scroll_follow_cursor = 0;

        repl_special_func(GLUT_KEY_UP, 0, 0);
        ASSERT_INT("up nav: scroll_follow_cursor set", g_scroll_follow_cursor, 1);

        ASSERT_TRUE("up nav: cursor visible after follow",
                    code_panel_apply_scroll_follow_for_test(&follow_doc_line,
                                                            &visible_lines));
    }

    /* Auto-commit modified lines before keyboard navigation. */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        navigate_to_line(0);
        set_editor_input("glVertex3f(9, 0, 0)");

        repl_special_func(GLUT_KEY_DOWN, 0, 0);

        ASSERT_INT("nav auto-commit valid edit: cursor moved", g_edit_line, 1);
        ASSERT_INT("nav auto-commit valid edit: first cmd type",
                   g_cmds[0].type, CMD_VERTEX3F);
        ASSERT_TRUE("nav auto-commit valid edit: x arg",
                    fabsf(g_cmds[0].args[0] - 9.0f) < 1e-6f);
        ASSERT_STR("nav auto-commit valid edit: input loaded next",
                   g_input, "glVertex3f(1, 1, 1)");
    }

    /* Invalid auto-commit reverts the edited line and still navigates. */
    {
        char old_source[MAX_LINE_LEN];
        int old_num_cmds;

        repl_reset_state();
        repl_feed_line_public("glColor3f(1,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        navigate_to_line(0);
        snprintf(old_source, sizeof(old_source), "%s", g_cmds[0].source);
        old_num_cmds = g_num_cmds;
        set_editor_input("glColor3f(1, 0");

        repl_special_func(GLUT_KEY_DOWN, 0, 0);

        ASSERT_INT("nav auto-commit invalid edit: cmd count unchanged",
                   g_num_cmds, old_num_cmds);
        ASSERT_STR("nav auto-commit invalid edit: source reverted",
                   g_cmds[0].source, old_source);
        ASSERT_INT("nav auto-commit invalid edit: cursor moved", g_edit_line, 1);
        ASSERT_STR("nav auto-commit invalid edit: input loaded next",
                   g_input, "glVertex3f(1, 1, 1)");
        assert_status_contains("nav auto-commit invalid edit: status",
                               "Incomplete command");
    }

    /* Auto-commit new end-of-buffer input before moving away. */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        navigate_to_line(g_num_cmds);
        set_editor_input("glVertex3f(3, 3, 3)");

        repl_special_func(GLUT_KEY_UP, 0, 0);

        ASSERT_INT("nav auto-commit append: cmd count", g_num_cmds, 3);
        ASSERT_INT("nav auto-commit append: cursor moved up", g_edit_line, 1);
        ASSERT_INT("nav auto-commit append: new cmd type",
                   g_cmds[2].type, CMD_VERTEX3F);
        ASSERT_TRUE("nav auto-commit append: x arg",
                    fabsf(g_cmds[2].args[0] - 3.0f) < 1e-6f);
    }

    /* Invalid new end-of-buffer input is discarded before moving away. */
    {
        int old_num_cmds;

        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        old_num_cmds = g_num_cmds;
        navigate_to_line(g_num_cmds);
        set_editor_input("glVertex3f(");

        repl_special_func(GLUT_KEY_UP, 0, 0);

        ASSERT_INT("nav auto-commit invalid append: cmd count unchanged",
                   g_num_cmds, old_num_cmds);
        ASSERT_INT("nav auto-commit invalid append: cursor moved up",
                   g_edit_line, 1);
        ASSERT_STR("nav auto-commit invalid append: input loaded target",
                   g_input, "glVertex3f(1, 1, 1)");
        assert_status_contains("nav auto-commit invalid append: status",
                               "Incomplete command");
        ASSERT_STR("nav auto-commit invalid append: newline buffer discarded",
                   g_newline_buf, "");

        navigate_to_line(g_num_cmds);
        ASSERT_STR("nav auto-commit invalid append: return to end is empty",
                   g_input, "");
    }

    /* Code-panel clicks use the same auto-commit path. */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        g_win_w = 800;
        g_win_h = 600;
        g_panel_frac = 0.5f;
        g_code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
        g_show_indices = 0;
        g_scroll = code_panel_header_row_count();
        navigate_to_line(0);
        set_editor_input("glVertex3f(8, 0, 0)");

        handle_code_panel_click(CODE_MARGIN_X + 1, code_panel_mouse_y_for_cmd(2));

        ASSERT_INT("mouse auto-commit valid edit: cursor moved", g_edit_line, 2);
        ASSERT_TRUE("mouse auto-commit valid edit: x arg",
                    fabsf(g_cmds[0].args[0] - 8.0f) < 1e-6f);
        ASSERT_STR("mouse auto-commit valid edit: input loaded clicked",
                   g_input, "glVertex3f(2, 2, 2)");
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

        repl_special_func(GLUT_KEY_DOWN, 0, 0);

        ASSERT_INT("autocomplete down changes selection", g_ac_sel, 1);
        ASSERT_INT("autocomplete down does not navigate", g_edit_line, 0);
        ASSERT_TRUE("autocomplete down does not commit edit",
                    fabsf(g_cmds[0].args[0] - 0.0f) < 1e-6f);
    }

    /* repl_clear_all_cmds — clears scene including float declarations. */
    {
        int base_num_predef_vars;
        int i;
        int found_tmp_before_clear = 0;
        int found_tmp_after_clear = 0;

        repl_reset_state();
        base_num_predef_vars = g_num_predef_vars;
        repl_feed_line_public("float tmp;");
        repl_feed_line_public("glVertex3f(1, 0, 0)");
        ASSERT_INT("clear_all: setup two cmds", g_num_cmds, 2);
        ASSERT_INT("clear_all: first is var decl", g_cmds[0].type, CMD_VAR_DECLARE);
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
        ASSERT_INT("clear_all: num_cmds is 0", g_num_cmds, 0);
        ASSERT_INT("clear_all: edit_line is 0", g_edit_line, 0);
        ASSERT_INT("clear_all: inserting is 0", g_inserting, 0);
        ASSERT_INT("clear_all: input is empty", g_input[0], 0);
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

    printf("\n%d / %d tests passed\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
