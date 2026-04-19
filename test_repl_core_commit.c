#include "repl_core_internal.h"
#include "ui_panels.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s]\n", label); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    g_run++; \
    if (strcmp((got), (exp)) == 0) g_pass++; \
    else printf("FAIL [%s] got \"%s\", expected \"%s\"\n", label, (got), (exp)); \
} while (0)

static void declare_test_vars(void) {
    char err[128];
    declare_predef_var("x", err, sizeof(err));
    declare_predef_var("y", err, sizeof(err));
    declare_predef_var("z", err, sizeof(err));
    declare_predef_var("i", err, sizeof(err));
    declare_predef_var("j", err, sizeof(err));
    declare_predef_var("k", err, sizeof(err));
    declare_predef_var("a", err, sizeof(err));
    declare_predef_var("b", err, sizeof(err));
    declare_predef_var("c", err, sizeof(err));
    declare_predef_var("n", err, sizeof(err));
}

#define TEST_CODE_PANEL_MAX_HANG_INDENT_CHARS 12

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
    for (int i = 0; i < g_workspace_header_line_count; i++)
        rows += test_code_panel_row_count_for_text(g_workspace_header_lines[i], text_x, panel_w);
    for (int i = 0; g_header_pre[i]; i++)
        rows += test_code_panel_row_count_for_text(g_header_pre[i], text_x, panel_w);
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++)
        rows += test_code_panel_row_count_for_text(g_render_state_lines[i], text_x, panel_w);
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

static int predef_idx(const char *name) {
    for (int i = 0; i < g_num_predef_vars; i++) {
        if (strcmp(g_predef_vars[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int test_leading_ws_chars(const char *text) {
    int n = 0;

    while (text && text[n] && isspace((unsigned char)text[n]))
        n++;
    return n;
}

/*
 * Mirror only the execute-time control-flow pieces that affect variables.
 * This lets the tests exercise goto/if/assignment replay rules without
 * needing a live GL context for the geometry commands in the same flat stream.
 */
static void run_flat_control_flow_only(void) {
    int pc = 0;
    int goto_count = 0;

    while (pc < g_num_flat_cmds) {
        if (!g_flat_cmds[pc].valid) {
            pc++;
            continue;
        }

        switch (g_flat_cmds[pc].type) {
        case CMD_VAR_ASSIGN: {
            int vi = g_flat_cmds[pc].num_args;
            float value = g_flat_cmds[pc].args[0];
            if (g_flat_cmds[pc].has_vars) {
                char rhs[256];
                if (repl_extract_assignment_parts(g_flat_cmds[pc].source,
                                                  NULL, 0,
                                                  rhs, sizeof(rhs))) {
                    ExprVar *eval_vars = g_predef_vars;
                    int eval_num_vars = g_num_predef_vars;
                    if (g_flat_cmd_local_vars[pc].num_vars > 0) {
                        eval_vars = g_flat_cmd_local_vars[pc].vars;
                        eval_num_vars = g_flat_cmd_local_vars[pc].num_vars;
                    }
                    ExprCtx ctx = { rhs, eval_vars, eval_num_vars };
                    value = eval_expr(&ctx);
                }
            }
            if (vi >= 0 && vi < g_num_predef_vars)
                g_predef_vars[vi].value = value;
            break;
        }
        case CMD_IF_BEGIN: {
            float cond = g_flat_cmds[pc].args[0];
            if (g_flat_cmds[pc].has_vars) {
                char cond_text[256];
                if (repl_extract_paren_payload(g_flat_cmds[pc].source,
                                               cond_text, sizeof(cond_text))) {
                    ExprVar *eval_vars = g_predef_vars;
                    int eval_num_vars = g_num_predef_vars;
                    if (g_flat_cmd_local_vars[pc].num_vars > 0) {
                        eval_vars = g_flat_cmd_local_vars[pc].vars;
                        eval_num_vars = g_flat_cmd_local_vars[pc].num_vars;
                    }
                    ExprCtx ctx = { cond_text, eval_vars, eval_num_vars };
                    cond = eval_expr(&ctx);
                }
            }
            if (cond == 0.0f) {
                int depth = 1;
                while (depth > 0 && ++pc < g_num_flat_cmds) {
                    if (g_flat_cmds[pc].type == CMD_IF_BEGIN) depth++;
                    else if (g_flat_cmds[pc].type == CMD_IF_END) depth--;
                }
            }
            break;
        }
        case CMD_GOTO: {
            char label[64];
            if (!repl_extract_goto_label(g_flat_cmds[pc].source, label, sizeof(label)))
                break;
            if (goto_count++ > 100000)
                return;
            for (int li = 0; li < g_num_flat_cmds; li++) {
                char target_label[64];
                if (g_flat_cmds[li].valid &&
                    g_flat_cmds[li].type == CMD_LABEL &&
                    repl_extract_label_name(g_flat_cmds[li].source,
                                            target_label,
                                            sizeof(target_label)) &&
                    strcmp(target_label, label) == 0) {
                    pc = li;
                    goto next_pc;
                }
            }
            break;
        }
        default:
            break;
        }

next_pc:
        pc++;
    }
}

int main(void) {
    init_predef_vars();

    repl_reset_state(); declare_test_vars();
    {
        int t_idx = predef_idx("t");
        ASSERT_TRUE("t predef exists", t_idx >= 0);
        if (t_idx >= 0) {
            g_predef_vars[t_idx].value = 3.0f;
            g_t_playing = 1;
            repl_advance_time(0.25f);
            ASSERT_TRUE("t advances from current value",
                        fabsf(g_predef_vars[t_idx].value - 3.25f) < 1e-6f);
            g_t_playing = 0;
            repl_advance_time(0.50f);
            ASSERT_TRUE("paused t does not advance",
                        fabsf(g_predef_vars[t_idx].value - 3.25f) < 1e-6f);
            repl_reset_time_to_zero();
            ASSERT_TRUE("reset time sets t to zero",
                        fabsf(g_predef_vars[t_idx].value) < 1e-6f);
        }
    }

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("z = -0.55;");
    ASSERT_TRUE("var assign cmd count", g_num_cmds == 1);
    ASSERT_TRUE("var assign type", g_cmds[0].type == CMD_VAR_ASSIGN);
    {
        int z_idx = -1;
        for (int i = 0; i < g_num_predef_vars; i++) {
            if (strcmp(g_predef_vars[i].name, "z") == 0) {
                z_idx = i;
                break;
            }
        }
        ASSERT_TRUE("z predef exists", z_idx >= 0);
        if (z_idx >= 0)
            ASSERT_TRUE("z updated", fabsf(g_predef_vars[z_idx].value - (-0.55f)) < 1e-6f);
    }

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("n = 0;");
    repl_feed_line_public(":walk");
    repl_feed_line_public("n = n + 1;");
    repl_feed_line_public("if(n < 3) {");
    repl_feed_line_public("goto walk;");
    repl_feed_line_public("}");
    ASSERT_TRUE("expr assign cmd count", g_num_cmds == 6);
    ASSERT_TRUE("expr assign preserves source", strstr(g_cmds[2].source, "n + 1") != NULL);
    ASSERT_TRUE("expr assign marked has_vars", g_cmds[2].has_vars == 1);
    repl_flatten_commands();
    execute_commands();
    {
        int n_idx = -1;
        for (int i = 0; i < g_num_predef_vars; i++) {
            if (strcmp(g_predef_vars[i].name, "n") == 0) {
                n_idx = i;
                break;
            }
        }
        ASSERT_TRUE("n predef exists", n_idx >= 0);
        if (n_idx >= 0)
            ASSERT_TRUE("goto loop increments n to 3", fabsf(g_predef_vars[n_idx].value - 3.0f) < 1e-6f);
    }

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("n = 0;");
    repl_feed_line_public("glBegin(GL_LINES);");
    repl_feed_line_public(":stripe");
    repl_feed_line_public("glVertex3f(-1.5 + 0.42*n, -0.9, 0);");
    repl_feed_line_public("glVertex3f(-1.5 + 0.42*n, 0.9, 0);");
    repl_feed_line_public("n = n + 1;");
    repl_feed_line_public("if(n < 3) {");
    repl_feed_line_public("goto stripe;");
    repl_feed_line_public("}");
    repl_feed_line_public("glEnd();");
    ASSERT_TRUE("goto geom cmd count", g_num_cmds == 10);
    ASSERT_TRUE("goto geom first vertex keeps expr",
                strstr(g_cmds[3].source, "0.42*n") != NULL);
    ASSERT_TRUE("goto geom first vertex has vars", g_cmds[3].has_vars == 1);
    repl_flatten_commands();
    ASSERT_TRUE("goto geom flat first vertex has vars", g_flat_cmds[3].has_vars == 1);
    ASSERT_TRUE("goto geom flat second vertex has vars", g_flat_cmds[4].has_vars == 1);
    run_flat_control_flow_only();
    {
        int n_idx = predef_idx("n");
        ASSERT_TRUE("goto geom n predef exists", n_idx >= 0);
        if (n_idx >= 0)
            ASSERT_TRUE("goto geom loop increments n to 3", fabsf(g_predef_vars[n_idx].value - 3.0f) < 1e-6f);
    }
    ASSERT_TRUE("goto geom flat first vertex still at initial x",
                fabsf(g_flat_cmds[3].args[0] - (-1.5f)) < 1e-5f);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public(":walk");
    ASSERT_TRUE("label cmd count", g_num_cmds == 1);
    ASSERT_TRUE("label stored as C label", strcmp(g_cmds[0].source, "walk:") == 0);
    repl_navigate_to_line(0);
    ASSERT_TRUE("label loads back into editor as repl syntax",
                strcmp(g_input, ":walk") == 0);
    repl_keyboard_func(';', 0, 0);
    ASSERT_TRUE("recommitting loaded label keeps label type", g_cmds[0].type == CMD_LABEL);
    ASSERT_TRUE("recommitting loaded label keeps source", strcmp(g_cmds[0].source, "walk:") == 0);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("for(i, 0, 3) {");
    repl_feed_line_public("glVertex3f(i, 0, 0);");
    repl_feed_line_public("}");
    ASSERT_TRUE("for block cmd count", g_num_cmds == 3);
    ASSERT_TRUE("for begin", g_cmds[0].type == CMD_FOR_BEGIN);
    ASSERT_TRUE("for body", g_cmds[1].type == CMD_VERTEX3F);
    ASSERT_TRUE("for end", g_cmds[2].type == CMD_FOR_END);
    ASSERT_TRUE("for body keeps i", strstr(g_cmds[1].source, "i") != NULL);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_feed_line_public("glEnd();");
    repl_navigate_to_line(1);
    g_cursor_pos = 0;
    repl_keyboard_func('\r', 0, 0);
    ASSERT_TRUE("enter at line start enters insert mode", g_inserting == 1);
    ASSERT_TRUE("enter at line start keeps insertion index", g_edit_line == 1);
    strcpy(g_input, "glColor3f(1, 0, 0)");
    g_input_len = (int)strlen(g_input);
    g_cursor_pos = g_input_len;
    repl_keyboard_func('\r', 0, 0);
    ASSERT_TRUE("inserted line before current cmd count", g_num_cmds == 3);
    ASSERT_TRUE("inserted line before current type", g_cmds[1].type == CMD_COLOR3F);
    ASSERT_TRUE("original current line shifted down", g_cmds[2].type == CMD_END);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_feed_line_public("glEnd();");
    repl_navigate_to_line(0);
    g_cursor_pos = g_input_len;
    repl_keyboard_func('\r', 0, 0);
    ASSERT_TRUE("enter away from line start still inserts after", g_inserting == 1 && g_edit_line == 1);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_feed_line_public("glColor3f(1, 0, 0);");
    repl_feed_line_public("glEnd();");
    handle_code_panel_press(CODE_MARGIN_X + 1, code_panel_mouse_y_for_cmd(0));
    ASSERT_TRUE("mouse press selects current line for edit", g_edit_line == 0);
    ASSERT_TRUE("mouse press starts with no selection", !sel_active());
    handle_code_panel_drag(CODE_MARGIN_X + 1, code_panel_mouse_y_for_cmd(2));
    ASSERT_TRUE("mouse drag activates selection", sel_active());
    ASSERT_TRUE("mouse drag selection low", sel_lo() == 0);
    ASSERT_TRUE("mouse drag selection high", sel_hi() == 2);
    ASSERT_TRUE("mouse drag navigates to drag end", g_edit_line == 2);
    handle_code_panel_release();
    repl_keyboard_func(8, 0, 0);
    ASSERT_TRUE("backspace deletes selected lines", g_num_cmds == 0);
    ASSERT_TRUE("backspace clears selection after delete", !sel_active());
    ASSERT_TRUE("backspace keeps edit line at start after delete", g_edit_line == 0);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("if(x > 0) {");
    repl_feed_line_public("glColor3f(1, 0, 0);");
    repl_feed_line_public("}");
    {
        int linenum_w = 4 * FONT_W;
        int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
        int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
        int indent = test_leading_ws_chars(g_cmds[1].source);
        handle_code_panel_click(text_x + indent * FONT_W + 1,
                                code_panel_mouse_y_for_cmd(1));
        ASSERT_TRUE("clicking indented active line keeps cursor at first char",
                    g_cursor_pos == 0);
        ASSERT_TRUE("clicking indented active line selects correct line",
                    g_edit_line == 1);
    }

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_navigate_to_line(0);
    g_cursor_pos = 4;
    repl_keyboard_func(1, 0, 0);
    ASSERT_TRUE("ctrl-a moves to line start", g_cursor_pos == 0);
    repl_keyboard_func(5, 0, 0);
    ASSERT_TRUE("ctrl-e moves to line end", g_cursor_pos == g_input_len);
    {
        int before = g_accum_aa_enabled;
        repl_keyboard_func(2, 0, 0);
        ASSERT_TRUE("ctrl-b toggles accum aa", g_accum_aa_enabled != before);
    }

    repl_reset_state(); declare_test_vars();
    {
        const char *prefix = "glVer";
        for (int i = 0; prefix[i]; i++)
            repl_keyboard_func((unsigned char)prefix[i], 0, 0);
    }
    ASSERT_TRUE("autocomplete popup shows signature text",
                strcmp(g_ac_matches[0], "glVertex3f(x, y, z)") == 0);
    ASSERT_TRUE("autocomplete ghost inserts callable suffix",
                strcmp(g_ac_ghost, "tex3f(") == 0);
    ASSERT_TRUE("autocomplete hint shows parameter names",
                strcmp(g_ac_hint, "x, y, z)") == 0);
    repl_keyboard_func('\t', 0, 0);
    ASSERT_TRUE("tab inserts function call prefix only",
                strcmp(g_input, "glVertex3f(") == 0);
    ASSERT_TRUE("function call hint starts at first parameter",
                strcmp(g_ac_hint, "x, y, z)") == 0);
    repl_keyboard_func('1', 0, 0);
    ASSERT_TRUE("function call hint advances after first arg text",
                strcmp(g_ac_hint, ", y, z)") == 0);
    repl_keyboard_func(',', 0, 0);
    repl_keyboard_func(' ', 0, 0);
    ASSERT_TRUE("function call hint shows second parameter at comma",
                strcmp(g_ac_hint, "y, z)") == 0);
    repl_keyboard_func('2', 0, 0);
    ASSERT_TRUE("function call hint advances to remaining args",
                strcmp(g_ac_hint, ", z)") == 0);
    repl_keyboard_func(',', 0, 0);
    repl_keyboard_func(' ', 0, 0);
    repl_keyboard_func('3', 0, 0);
    ASSERT_TRUE("function call hint ends with closing paren",
                strcmp(g_ac_hint, ")") == 0);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0(radius, yoff) {");
    repl_feed_line_public("}");
    {
        const char *call = "func0(";
        for (int i = 0; call[i]; i++)
            repl_keyboard_func((unsigned char)call[i], 0, 0);
    }
    ASSERT_TRUE("user function hint uses declared parameter names",
                strcmp(g_ac_hint, "radius, yoff)") == 0);
    ASSERT_TRUE("user function hint suppresses stale no-arg ghost",
                g_ac_ghost[0] == '\0');

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("if(x > 0) {");
    repl_feed_line_public("glColor3f(1, 0, 0);");
    repl_feed_line_public("}");
    ASSERT_TRUE("if block cmd count", g_num_cmds == 3);
    ASSERT_TRUE("if begin", g_cmds[0].type == CMD_IF_BEGIN);
    ASSERT_TRUE("if body", g_cmds[1].type == CMD_COLOR3F);
    ASSERT_TRUE("if end", g_cmds[2].type == CMD_IF_END);
    repl_flatten_commands();
    ASSERT_TRUE("top-level if flat count", g_num_flat_cmds == 3);
    ASSERT_TRUE("top-level if flat begin", g_flat_cmds[0].type == CMD_IF_BEGIN);
    ASSERT_TRUE("top-level if flat body", g_flat_cmds[1].type == CMD_COLOR3F);
    ASSERT_TRUE("top-level if flat end", g_flat_cmds[2].type == CMD_IF_END);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0 {");
    repl_feed_line_public("glVertex3f(1, 2, 3);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0()");
    ASSERT_TRUE("func cmd count", g_num_cmds == 4);
    ASSERT_TRUE("func def", g_cmds[0].type == CMD_FUNC_DEF);
    ASSERT_TRUE("func body", g_cmds[1].type == CMD_VERTEX3F);
    ASSERT_TRUE("func end", g_cmds[2].type == CMD_FUNC_END);
    ASSERT_TRUE("func call", g_cmds[3].type == CMD_CALL);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0(radius, yoff) {");
    repl_feed_line_public("glVertex3f(radius, yoff, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(1.5, x + 2);");
    ASSERT_TRUE("param func cmd count", g_num_cmds == 4);
    ASSERT_TRUE("param func def", g_cmds[0].type == CMD_FUNC_DEF);
    ASSERT_TRUE("param func header keeps names",
                strstr(g_cmds[0].source, "radius") != NULL &&
                strstr(g_cmds[0].source, "yoff") != NULL);
    ASSERT_TRUE("param func body keeps radius",
                strstr(g_cmds[1].source, "radius") != NULL);
    ASSERT_TRUE("param func body keeps yoff",
                strstr(g_cmds[1].source, "yoff") != NULL);
    ASSERT_TRUE("param func call type", g_cmds[3].type == CMD_CALL);
    ASSERT_TRUE("param func call keeps expr",
                strstr(g_cmds[3].source, "x + 2") != NULL);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func1(a, b) {");
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_feed_line_public("glVertex3f(a, b, 0);");
    repl_feed_line_public("glEnd();");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(scale) {");
    repl_feed_line_public("func1(scale, scale + 1);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(2);");
    repl_feed_line_public("func0(4);");
    repl_flatten_commands();
    ASSERT_TRUE("nested func flatten count", g_num_flat_cmds == 6);
    ASSERT_TRUE("nested func flatten first begin", g_flat_cmds[0].type == CMD_BEGIN);
    ASSERT_TRUE("nested func flatten first vertex", g_flat_cmds[1].type == CMD_VERTEX3F);
    ASSERT_TRUE("nested func flatten first x", fabsf(g_flat_cmds[1].args[0] - 2.0f) < 1e-6f);
    ASSERT_TRUE("nested func flatten first y", fabsf(g_flat_cmds[1].args[1] - 3.0f) < 1e-6f);
    ASSERT_TRUE("nested func flatten second x", fabsf(g_flat_cmds[4].args[0] - 4.0f) < 1e-6f);
    ASSERT_TRUE("nested func flatten second y", fabsf(g_flat_cmds[4].args[1] - 5.0f) < 1e-6f);
    ASSERT_TRUE("nested func call provenance immediate", g_flat_cmds[1].call_src_cmd_idx == 6);
    ASSERT_TRUE("nested func call provenance root first", g_flat_cmds[1].root_call_src_cmd_idx == 8);
    ASSERT_TRUE("nested func call provenance root second", g_flat_cmds[4].root_call_src_cmd_idx == 9);
    ASSERT_TRUE("nested func scope mask includes both", (g_flat_cmds[1].func_scope_mask & 0x3u) == 0x3u);
    {
        int matched = 0;
        g_edit_line = 8;
        for (int i = 0; i < g_num_flat_cmds; i++)
            matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("nested func call line highlights one invocation", matched == 3);
    }
    {
        int matched = 0;
        g_edit_line = 6;
        for (int i = 0; i < g_num_flat_cmds; i++)
            matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("nested inner call line highlights all invocations", matched == 6);
    }
    {
        int matched = 0;
        g_edit_line = 2;
        for (int i = 0; i < g_num_flat_cmds; i++)
            matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("nested function body highlights all invocations", matched == 6);
    }
    {
        int matched = 0;
        g_edit_line = 5;
        for (int i = 0; i < g_num_flat_cmds; i++)
            matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("outer function header highlights nested invocations", matched == 6);
    }

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0(n) {");
    repl_feed_line_public("for(i, 0, n) {");
    repl_feed_line_public("glVertex3f(i, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("glColor3f(1, 0, 0);");
    repl_feed_line_public("}");
    ASSERT_TRUE("nested block trailing cmd count", g_num_cmds == 6);
    ASSERT_TRUE("nested block trailing cmd order body", g_cmds[4].type == CMD_COLOR3F);
    ASSERT_TRUE("nested block trailing cmd order end", g_cmds[5].type == CMD_FUNC_END);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0(n) {");
    repl_feed_line_public("for(i, 0, n) glVertex3f(i, n, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(3);");
    ASSERT_TRUE("local for begin type", g_cmds[1].type == CMD_FOR_BEGIN);
    ASSERT_TRUE("local for body type", g_cmds[2].type == CMD_VERTEX3F);
    ASSERT_TRUE("local for end type", g_cmds[3].type == CMD_FOR_END);
    ASSERT_TRUE("local for header keeps n", strstr(g_cmds[1].source, "n") != NULL);
    ASSERT_TRUE("local for body keeps n", strstr(g_cmds[2].source, "n") != NULL);
    repl_flatten_commands();
    ASSERT_TRUE("local for flatten count", g_num_flat_cmds == 3);
    ASSERT_TRUE("local for first x", fabsf(g_flat_cmds[0].args[0] - 0.0f) < 1e-6f);
    ASSERT_TRUE("local for second x", fabsf(g_flat_cmds[1].args[0] - 1.0f) < 1e-6f);
    ASSERT_TRUE("local for third x", fabsf(g_flat_cmds[2].args[0] - 2.0f) < 1e-6f);
    ASSERT_TRUE("local for body uses param", fabsf(g_flat_cmds[2].args[1] - 3.0f) < 1e-6f);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0(scale) {");
    repl_feed_line_public("if(scale > 1) {");
    repl_feed_line_public("glVertex3f(scale, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(2);");
    repl_feed_line_public("func0(0.5);");
    ASSERT_TRUE("local if header keeps scale", strstr(g_cmds[1].source, "scale > 1") != NULL);
    repl_flatten_commands();
    ASSERT_TRUE("local if flatten count", g_num_flat_cmds == 1);
    ASSERT_TRUE("local if flatten type", g_flat_cmds[0].type == CMD_VERTEX3F);
    ASSERT_TRUE("local if flatten x", fabsf(g_flat_cmds[0].args[0] - 2.0f) < 1e-6f);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0(cols) {");
    repl_feed_line_public("for(j, 0, cols + 1) {");
    repl_feed_line_public("x = -1.5 + 3.0*j/cols;");
    repl_feed_line_public("glVertex3f(x, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(4);");
    repl_flatten_commands();
    ASSERT_TRUE("local assign flatten count", g_num_flat_cmds == 10);
    ASSERT_TRUE("local assign first type", g_flat_cmds[0].type == CMD_VAR_ASSIGN);
    ASSERT_TRUE("local assign first value", fabsf(g_flat_cmds[0].args[0] - (-1.5f)) < 1e-6f);
    ASSERT_TRUE("local assign second value", fabsf(g_flat_cmds[2].args[0] - (-0.75f)) < 1e-6f);
    ASSERT_TRUE("local assign third value", fabsf(g_flat_cmds[4].args[0] - 0.0f) < 1e-6f);
    ASSERT_TRUE("local assign last value", fabsf(g_flat_cmds[8].args[0] - 1.5f) < 1e-6f);
    ASSERT_TRUE("local assign vertex last x", fabsf(g_flat_cmds[9].args[0] - 1.5f) < 1e-6f);
    run_flat_control_flow_only();
    {
        int x_idx = predef_idx("x");
        ASSERT_TRUE("x predef exists", x_idx >= 0);
        if (x_idx >= 0)
            ASSERT_TRUE("local assign replay updates x", fabsf(g_predef_vars[x_idx].value - 1.5f) < 1e-6f);
    }

    repl_reset_state(); declare_test_vars();
    {
        char display[MAX_INPUT_LEN];
        int i_idx = predef_idx("i");
        int j_idx = predef_idx("j");

        ASSERT_TRUE("replay display i predef exists", i_idx >= 0);
        ASSERT_TRUE("replay display j predef exists", j_idx >= 0);
        if (i_idx >= 0) g_predef_vars[i_idx].value = 3.2f;
        if (j_idx >= 0) g_predef_vars[j_idx].value = 1.2f;

        repl_feed_line_public("i = i + j + 3;");
        repl_feed_line_public("glVertex3f(i, j, 0);");
        if (i_idx >= 0) g_predef_vars[i_idx].value = 3.2f;
        if (j_idx >= 0) g_predef_vars[j_idx].value = 1.2f;
        replay_start();
        g_replay_state = REPLAY_PAUSED;

        g_replay_pc = 1;
        g_replay_src_line = 0;
        ASSERT_TRUE("replay display assignment text",
                    code_panel_get_command_display_text(0, display, sizeof(display)));
        ASSERT_STR("replay display assignment inline comment",
                   display,
                   "  i = i + j + 3; // i = 3.2 + 1.2 + 3 = 7.4");

        g_replay_pc = 2;
        g_replay_src_line = 1;
        ASSERT_TRUE("replay display prior assignment still visible",
                    code_panel_get_command_display_text(0, display, sizeof(display)));
        ASSERT_STR("replay display prior assignment inline comment",
                   display,
                   "  i = i + j + 3; // i = 3.2 + 1.2 + 3 = 7.4");
        ASSERT_TRUE("replay display vertex text",
                    code_panel_get_command_display_text(1, display, sizeof(display)));
        ASSERT_STR("replay display vertex source unchanged",
                   display,
                   "  glVertex3f(i, j, 0);");

        g_replay_active = 0;
        g_replay_state = REPLAY_OFF;
        g_replay_src_line = -1;
        g_replay_pc = 0;
    }

    repl_reset_state(); declare_test_vars();
    {
        char display[MAX_INPUT_LEN];
        int i_idx = predef_idx("i");
        int k_idx = predef_idx("k");

        ASSERT_TRUE("replay chain i predef exists", i_idx >= 0);
        ASSERT_TRUE("replay chain k predef exists", k_idx >= 0);
        if (i_idx >= 0) g_predef_vars[i_idx].value = 0.23f;
        if (k_idx >= 0) g_predef_vars[k_idx].value = 0.5f;

        repl_feed_line_public("i = i + k;");
        if (i_idx >= 0) g_predef_vars[i_idx].value = 0.23f;
        if (k_idx >= 0) g_predef_vars[k_idx].value = 0.5f;
        replay_start();
        g_replay_state = REPLAY_PAUSED;

        g_replay_pc = 1;
        g_replay_src_line = 0;
        ASSERT_TRUE("replay chain assignment text",
                    code_panel_get_command_display_text(0, display, sizeof(display)));
        ASSERT_STR("replay chain assignment inline comment",
                   display,
                   "  i = i + k; // i = 0.23 + 0.5 = 0.73");

        g_replay_active = 0;
        g_replay_state = REPLAY_OFF;
        g_replay_src_line = -1;
        g_replay_pc = 0;
    }

    repl_reset_state(); declare_test_vars();
    {
        char display[MAX_INPUT_LEN];
        int i_idx = predef_idx("i");
        int x_idx = predef_idx("x");

        ASSERT_TRUE("replay goto i predef exists", i_idx >= 0);
        ASSERT_TRUE("replay goto x predef exists", x_idx >= 0);

        repl_feed_line_public("i = 1;");
        repl_feed_line_public("goto after;");
        repl_feed_line_public("i = 100;");
        repl_feed_line_public("x = i + 1;");
        repl_feed_line_public(":after");
        repl_feed_line_public("glVertex3f(0, 0, 0);");
        replay_start();
        g_replay_state = REPLAY_PAUSED;

        g_replay_pc = g_num_flat_cmds;
        g_replay_src_line = 5;
        ASSERT_TRUE("replay goto skipped assignment text",
                    code_panel_get_command_display_text(3, display, sizeof(display)));
        ASSERT_TRUE("replay goto skipped assignment uses pre-jump value",
                    strstr(display, "// x = 1 + 1 = 2") != NULL);
        ASSERT_TRUE("replay goto skipped assignment ignores skipped overwrite",
                    strstr(display, "100 + 1") == NULL);

        g_replay_active = 0;
        g_replay_state = REPLAY_OFF;
        g_replay_src_line = -1;
        g_replay_pc = 0;
    }

    repl_reset_state(); declare_test_vars();
    {
        char display[MAX_INPUT_LEN];
        int i_idx = predef_idx("i");
        int j_idx = predef_idx("j");

        ASSERT_TRUE("replay scientific i predef exists", i_idx >= 0);
        ASSERT_TRUE("replay scientific j predef exists", j_idx >= 0);
        if (j_idx >= 0) g_predef_vars[j_idx].value = 1.0f;

        repl_feed_line_public("for(e, 0, 1) {");
        repl_feed_line_public("i = j * 1e-06;");
        repl_feed_line_public("}");
        if (j_idx >= 0) g_predef_vars[j_idx].value = 1.0f;
        replay_start();
        g_replay_state = REPLAY_PAUSED;

        g_replay_pc = 1;
        g_replay_src_line = 1;
        ASSERT_TRUE("replay scientific assignment text",
                    code_panel_get_command_display_text(1, display, sizeof(display)));
        ASSERT_TRUE("replay scientific inline comment keeps expanded rhs",
                    strstr(display, "// i = 1 * 1e-06 = 1e-06") != NULL);

        g_replay_active = 0;
        g_replay_state = REPLAY_OFF;
        g_replay_src_line = -1;
        g_replay_pc = 0;
    }

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glColor3f(1, 0, 0);");
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glEnd();");
    repl_feed_line_public("glColor3f(0, 1, 0);");
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0, 1, 0);");
    repl_feed_line_public("glVertex3f(1, 1, 0);");
    repl_feed_line_public("glEnd();");
    repl_flatten_commands();
    {
        int matched = 0;
        g_edit_line = 0;
        for (int i = 0; i < g_num_flat_cmds; i++)
            if (g_flat_cmds[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("top-level color before block matches first vertices", matched == 2);
    }
    {
        int matched = 0;
        g_edit_line = 5;
        for (int i = 0; i < g_num_flat_cmds; i++)
            if (g_flat_cmds[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("top-level second color before block matches second vertices", matched == 2);
    }
    ASSERT_TRUE("feeding color for first block vertex", repl_find_feeding_color_cmd(2) == 0);
    ASSERT_TRUE("feeding color for second block vertex", repl_find_feeding_color_cmd(7) == 5);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glNormal3f(0, 0, 1);");
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glEnd();");
    repl_feed_line_public("glNormal3f(0, 1, 0);");
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0, 1, 0);");
    repl_feed_line_public("glVertex3f(1, 1, 0);");
    repl_feed_line_public("glEnd();");
    repl_flatten_commands();
    {
        int matched = 0;
        g_edit_line = 0;
        for (int i = 0; i < g_num_flat_cmds; i++)
            if (g_flat_cmds[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("top-level normal before block matches first vertices", matched == 2);
    }
    {
        int matched = 0;
        g_edit_line = 5;
        for (int i = 0; i < g_num_flat_cmds; i++)
            if (g_flat_cmds[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("top-level second normal before block matches second vertices", matched == 2);
    }
    ASSERT_TRUE("feeding normal for first block vertex", repl_find_feeding_normal_cmd(2) == 0);
    ASSERT_TRUE("feeding normal for second block vertex", repl_find_feeding_normal_cmd(7) == 5);

    repl_reset_state(); declare_test_vars();
    g_autonormal = 1;
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glVertex3f(0, 1, 0);");
    repl_feed_line_public("glEnd();");
    repl_recompute_autonormals();
    ASSERT_TRUE("autonormal inserts before each triangle vertex", g_num_cmds == 8);
    ASSERT_TRUE("autonormal default front-face first cmd type", g_cmds[1].type == CMD_NORMAL3F);
    ASSERT_TRUE("autonormal default front-face first cmd auto", g_cmds[1].is_auto == 1);
    ASSERT_TRUE("autonormal default front-face keeps +z", fabsf(g_cmds[1].args[2] - 1.0f) < 1e-6f);

    repl_reset_state(); declare_test_vars();
    g_autonormal = 1;
    repl_feed_line_public("glFrontFace(GL_CW);");
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glVertex3f(0, 1, 0);");
    repl_feed_line_public("glEnd();");
    repl_recompute_autonormals();
    ASSERT_TRUE("autonormal front-face cw inserts before each triangle vertex", g_num_cmds == 9);
    ASSERT_TRUE("autonormal front-face cw first cmd type", g_cmds[2].type == CMD_NORMAL3F);
    ASSERT_TRUE("autonormal front-face cw first cmd auto", g_cmds[2].is_auto == 1);
    ASSERT_TRUE("autonormal front-face cw flips z", fabsf(g_cmds[2].args[2] - (-1.0f)) < 1e-6f);

    g_cmds[0].mode = GL_CCW;
    repl_recompute_autonormals();
    ASSERT_TRUE("autonormal front-face update flips auto normal back", fabsf(g_cmds[2].args[2] - 1.0f) < 1e-6f);

    repl_reset_state(); declare_test_vars();
    g_autonormal = 0;
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glEnd();");
    repl_flatten_commands();
    repl_navigate_to_line(1);
    {
        int matched = 0;
        for (int i = 0; i < g_num_flat_cmds; i++)
            if (g_flat_cmds[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("navigate refreshes current block highlight", matched == 2);
    }

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0(depth) {");
    repl_feed_line_public("if(depth <= 0) {");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("if(depth > 0) {");
    repl_feed_line_public("glVertex3f(depth, 0, 0);");
    repl_feed_line_public("func0(depth - 1);");
    repl_feed_line_public("}");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(3);");
    repl_flatten_commands();
    ASSERT_TRUE("recursive flatten count", g_num_flat_cmds == 4);
    ASSERT_TRUE("recursive first x", fabsf(g_flat_cmds[0].args[0] - 3.0f) < 1e-6f);
    ASSERT_TRUE("recursive second x", fabsf(g_flat_cmds[1].args[0] - 2.0f) < 1e-6f);
    ASSERT_TRUE("recursive third x", fabsf(g_flat_cmds[2].args[0] - 1.0f) < 1e-6f);
    ASSERT_TRUE("recursive base x", fabsf(g_flat_cmds[3].args[0] - 0.0f) < 1e-6f);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0(n) {");
    repl_feed_line_public("if(n <= 0) {");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("if(n > 0) {");
    repl_feed_line_public("glVertex3f(n, 0, 0);");
    repl_feed_line_public("func1(n - 1);");
    repl_feed_line_public("}");
    repl_feed_line_public("}");
    repl_feed_line_public("func1(n) {");
    repl_feed_line_public("if(n <= 0) {");
    repl_feed_line_public("glVertex3f(-10, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("if(n > 0) {");
    repl_feed_line_public("glVertex3f(-n, 0, 0);");
    repl_feed_line_public("func0(n - 1);");
    repl_feed_line_public("}");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(2);");
    repl_flatten_commands();
    ASSERT_TRUE("mutual recursion flatten count", g_num_flat_cmds == 3);
    ASSERT_TRUE("mutual recursion first x", fabsf(g_flat_cmds[0].args[0] - 2.0f) < 1e-6f);
    ASSERT_TRUE("mutual recursion second x", fabsf(g_flat_cmds[1].args[0] - (-1.0f)) < 1e-6f);
    ASSERT_TRUE("mutual recursion base x", fabsf(g_flat_cmds[2].args[0] - 0.0f) < 1e-6f);

    repl_reset_state(); declare_test_vars();
    g_status[0] = '\0';
    repl_feed_line_public("func0(n) {");
    repl_feed_line_public("func0(n + 1);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(0);");
    repl_flatten_commands();
    ASSERT_TRUE("runaway recursion emits no flat cmds", g_num_flat_cmds == 0);
    ASSERT_TRUE("runaway recursion depth guard",
                strstr(g_status, "depth limit") != NULL ||
                strstr(g_status, "visit budget") != NULL);

    printf("repl_core_commit: %d/%d passed\n", g_pass, g_run);
    return (g_run == g_pass) ? 0 : 1;
}
