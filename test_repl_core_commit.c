#include "repl_core_internal.h"
#include "repl_export.h"
#include "repl_clipboard.h"
#include "repl_state.h"
#include "ui_panels.h"

#define g_status  (repl_state_status_mut()->text)
#define g_scroll  (*repl_state_code_panel_mut()->scroll)
#define g_t_playing (*repl_state_variables_mut()->time_playing)
#define g_ac_ghost  (repl_state_autocomplete_mut()->ghost)
#define g_ac_hint   (repl_state_autocomplete_mut()->hint)
#define g_ac_matches (repl_state_autocomplete_mut()->matches)

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s] (line %d)\n", label, __LINE__); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    g_run++; \
    if (strcmp((got), (exp)) == 0) g_pass++; \
    else printf("FAIL [%s] got \"%s\", expected \"%s\" (line %d)\n", label, (got), (exp), __LINE__); \
} while (0)

#define replay_active        (*repl_state_replay_mut()->active)
#define replay_state         (*repl_state_replay_mut()->state)
#define replay_pc            (*repl_state_replay_mut()->pc)
#define replay_src_line      (*repl_state_replay_mut()->src_line_idx)

#define IMPORT_EXPORT_STATE (repl_state_import_export())
#define g_workspace_header_lines (IMPORT_EXPORT_STATE->workspace_header_lines)
#define g_workspace_header_line_count (*IMPORT_EXPORT_STATE->workspace_header_line_count)
#define g_render_state_lines (IMPORT_EXPORT_STATE->render_state_lines)
#define g_cam_lines (IMPORT_EXPORT_STATE->cam_lines)

static void declare_test_vars(void) {
    char err[128];
    repl_eval_declare_predef_var("x", err, sizeof(err));
    repl_eval_declare_predef_var("y", err, sizeof(err));
    repl_eval_declare_predef_var("z", err, sizeof(err));
    repl_eval_declare_predef_var("i", err, sizeof(err));
    repl_eval_declare_predef_var("j", err, sizeof(err));
    repl_eval_declare_predef_var("k", err, sizeof(err));
    repl_eval_declare_predef_var("a", err, sizeof(err));
    repl_eval_declare_predef_var("b", err, sizeof(err));
    repl_eval_declare_predef_var("c", err, sizeof(err));
    repl_eval_declare_predef_var("n", err, sizeof(err));
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

            if (!*repl_state_presentation()->wrap_at_comma || width_chars < 1 || remaining <= width_chars) {
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
    int idx_col_w = *repl_state_presentation()->show_vertex_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int rows = 0;

    ui_panels_code_panel_rect(NULL, NULL, &panel_w, NULL);
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
    int idx_col_w = *repl_state_presentation()->show_vertex_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int doc_line = code_panel_header_row_count();

    ui_panels_code_panel_rect(NULL, &cp_y, &panel_w, &cp_h);
    for (int i = 0; i < cmd_idx && i < repl_state_document_count(); i++) {
        doc_line += test_code_panel_row_count_for_text(repl_state_document_cmds_mut()[i].source,
                                                       text_x, panel_w);
    }

    int vis = doc_line - g_scroll;
    int line_y_start = cp_y + cp_h - CODE_MARGIN_Y - 2 * LINE_H;
    int gl_y = line_y_start - vis * LINE_H + 1;
    return *repl_state_viewport()->window_h - gl_y;
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

    while (pc < repl_state_flat_program_count()) {
        if (!repl_state_flat_program_cmds_mut()[pc].valid) {
            pc++;
            continue;
        }

        switch (repl_state_flat_program_cmds_mut()[pc].type) {
        case CMD_VAR_ASSIGN: {
            int vi = repl_state_flat_program_cmds_mut()[pc].num_args;
            float value = repl_state_flat_program_cmds_mut()[pc].args[0];
            if (repl_state_flat_program_cmds_mut()[pc].has_vars) {
                char rhs[256];
                if (repl_extract_assignment_parts(repl_state_flat_program_cmds_mut()[pc].source,
                                                  NULL, 0,
                                                  rhs, sizeof(rhs))) {
                    ExprVar *eval_vars = g_predef_vars;
                    int eval_num_vars = g_num_predef_vars;
                    if (repl_state_flat_program_local_vars_mut()[pc].num_vars > 0) {
                        eval_vars = repl_state_flat_program_local_vars_mut()[pc].vars;
                        eval_num_vars = repl_state_flat_program_local_vars_mut()[pc].num_vars;
                    }
                    ExprCtx ctx = { rhs, eval_vars, eval_num_vars };
                    value = repl_eval_expr(&ctx);
                }
            }
            if (vi >= 0 && vi < g_num_predef_vars)
                g_predef_vars[vi].value = value;
            break;
        }
        case CMD_IF_BEGIN: {
            float cond = repl_state_flat_program_cmds_mut()[pc].args[0];
            if (repl_state_flat_program_cmds_mut()[pc].has_vars) {
                char cond_text[256];
                if (repl_extract_paren_payload(repl_state_flat_program_cmds_mut()[pc].source,
                                               cond_text, sizeof(cond_text))) {
                    ExprVar *eval_vars = g_predef_vars;
                    int eval_num_vars = g_num_predef_vars;
                    if (repl_state_flat_program_local_vars_mut()[pc].num_vars > 0) {
                        eval_vars = repl_state_flat_program_local_vars_mut()[pc].vars;
                        eval_num_vars = repl_state_flat_program_local_vars_mut()[pc].num_vars;
                    }
                    ExprCtx ctx = { cond_text, eval_vars, eval_num_vars };
                    cond = repl_eval_expr(&ctx);
                }
            }
            if (cond == 0.0f) {
                int depth = 1;
                while (depth > 0 && ++pc < repl_state_flat_program_count()) {
                    if (repl_state_flat_program_cmds_mut()[pc].type == CMD_IF_BEGIN) depth++;
                    else if (repl_state_flat_program_cmds_mut()[pc].type == CMD_IF_END) depth--;
                }
            }
            break;
        }
        case CMD_GOTO: {
            char label[64];
            if (!repl_extract_goto_label(repl_state_flat_program_cmds_mut()[pc].source, label, sizeof(label)))
                break;
            if (goto_count++ > 100000)
                return;
            for (int li = 0; li < repl_state_flat_program_count(); li++) {
                char target_label[64];
                if (repl_state_flat_program_cmds_mut()[li].valid &&
                    repl_state_flat_program_cmds_mut()[li].type == CMD_LABEL &&
                    repl_extract_label_name(repl_state_flat_program_cmds_mut()[li].source,
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
    repl_eval_init_predef_vars();

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
    ASSERT_TRUE("var assign cmd count", repl_state_document_count() == 1);
    ASSERT_TRUE("var assign type", repl_state_document_cmds_mut()[0].type == CMD_VAR_ASSIGN);
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
    ASSERT_TRUE("expr assign cmd count", repl_state_document_count() == 6);
    ASSERT_TRUE("expr assign preserves source", strstr(repl_state_document_cmds_mut()[2].source, "n + 1") != NULL);
    ASSERT_TRUE("expr assign marked has_vars", repl_state_document_cmds_mut()[2].has_vars == 1);
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
    ASSERT_TRUE("goto geom cmd count", repl_state_document_count() == 10);
    ASSERT_TRUE("goto geom first vertex keeps expr",
                strstr(repl_state_document_cmds_mut()[3].source, "0.42*n") != NULL);
    ASSERT_TRUE("goto geom first vertex has vars", repl_state_document_cmds_mut()[3].has_vars == 1);
    repl_flatten_commands();
    ASSERT_TRUE("goto geom flat first vertex has vars", repl_state_flat_program_cmds_mut()[3].has_vars == 1);
    ASSERT_TRUE("goto geom flat second vertex has vars", repl_state_flat_program_cmds_mut()[4].has_vars == 1);
    run_flat_control_flow_only();
    {
        int n_idx = predef_idx("n");
        ASSERT_TRUE("goto geom n predef exists", n_idx >= 0);
        if (n_idx >= 0)
            ASSERT_TRUE("goto geom loop increments n to 3", fabsf(g_predef_vars[n_idx].value - 3.0f) < 1e-6f);
    }
    ASSERT_TRUE("goto geom flat first vertex still at initial x",
                fabsf(repl_state_flat_program_cmds_mut()[3].args[0] - (-1.5f)) < 1e-5f);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public(":walk");
    ASSERT_TRUE("label cmd count", repl_state_document_count() == 1);
    ASSERT_TRUE("label stored as C label", strcmp(repl_state_document_cmds_mut()[0].source, "walk:") == 0);
    repl_navigate_to_line(0);
    ASSERT_TRUE("label loads back into editor as repl syntax",
                strcmp(repl_state_editor_input()->input, ":walk") == 0);
    repl_keyboard_func(';', 0, 0);
    ASSERT_TRUE("recommitting loaded label keeps label type", repl_state_document_cmds_mut()[0].type == CMD_LABEL);
    ASSERT_TRUE("recommitting loaded label keeps source", strcmp(repl_state_document_cmds_mut()[0].source, "walk:") == 0);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("for(i, 0, 3) {");
    repl_feed_line_public("glVertex3f(i, 0, 0);");
    repl_feed_line_public("}");
    ASSERT_TRUE("for block cmd count", repl_state_document_count() == 3);
    ASSERT_TRUE("for begin", repl_state_document_cmds_mut()[0].type == CMD_FOR_BEGIN);
    ASSERT_TRUE("for body", repl_state_document_cmds_mut()[1].type == CMD_VERTEX3F);
    ASSERT_TRUE("for end", repl_state_document_cmds_mut()[2].type == CMD_FOR_END);
    ASSERT_TRUE("for body keeps i", strstr(repl_state_document_cmds_mut()[1].source, "i") != NULL);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_feed_line_public("glEnd();");
    repl_navigate_to_line(1);
    repl_state_cursor_pos_set(0);
    repl_keyboard_func('\r', 0, 0);
    ASSERT_TRUE("enter at line start enters insert mode", repl_state_insert_mode() == 1);
    ASSERT_TRUE("enter at line start keeps insertion index", repl_state_edit_line() == 1);
    {
        ReplEditorInputState *inp = repl_state_editor_input_mut();
        strcpy(inp->input, "glColor3f(1, 0, 0)");
        *inp->input_len = (int)strlen(inp->input);
        repl_state_cursor_pos_set(*inp->input_len);
    }
    repl_keyboard_func('\r', 0, 0);
    ASSERT_TRUE("inserted line before current cmd count", repl_state_document_count() == 3);
    ASSERT_TRUE("inserted line before current type", repl_state_document_cmds_mut()[1].type == CMD_COLOR3F);
    ASSERT_TRUE("original current line shifted down", repl_state_document_cmds_mut()[2].type == CMD_END);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_feed_line_public("glEnd();");
    repl_navigate_to_line(0);
    repl_state_cursor_pos_set(repl_state_input_len());
    repl_keyboard_func('\r', 0, 0);
    ASSERT_TRUE("enter away from line start still inserts after", repl_state_insert_mode() == 1 && repl_state_edit_line() == 1);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_feed_line_public("glColor3f(1, 0, 0);");
    repl_feed_line_public("glEnd();");
    ui_panels_handle_code_panel_press(CODE_MARGIN_X + 1, code_panel_mouse_y_for_cmd(0));
    ASSERT_TRUE("mouse press selects current line for edit", repl_state_edit_line() == 0);
    ASSERT_TRUE("mouse press starts with no selection", !repl_clipboard_sel_active());
    ui_panels_handle_code_panel_drag(CODE_MARGIN_X + 1, code_panel_mouse_y_for_cmd(2));
    ASSERT_TRUE("mouse drag activates selection", repl_clipboard_sel_active());
    ASSERT_TRUE("mouse drag selection low", repl_clipboard_sel_lo() == 0);
    ASSERT_TRUE("mouse drag selection high", repl_clipboard_sel_hi() == 2);
    ASSERT_TRUE("mouse drag navigates to drag end", repl_state_edit_line() == 2);
    ui_panels_handle_code_panel_release();
    repl_keyboard_func(8, 0, 0);
    ASSERT_TRUE("backspace deletes selected lines", repl_state_document_count() == 0);
    ASSERT_TRUE("backspace clears selection after delete", !repl_clipboard_sel_active());
    ASSERT_TRUE("backspace keeps edit line at start after delete", repl_state_edit_line() == 0);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("if(x > 0) {");
    repl_feed_line_public("glColor3f(1, 0, 0);");
    repl_feed_line_public("}");
    {
        int linenum_w = 4 * FONT_W;
        int idx_col_w = *repl_state_presentation()->show_vertex_indices ? (6 * FONT_W) : 0;
        int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
        int indent = test_leading_ws_chars(repl_state_document_cmds_mut()[1].source);
        ui_panels_handle_code_panel_click(text_x + indent * FONT_W + 1,
                                code_panel_mouse_y_for_cmd(1));
        ASSERT_TRUE("clicking indented active line keeps cursor at first char",
                    repl_state_cursor_pos() == 0);
        ASSERT_TRUE("clicking indented active line selects correct line",
                    repl_state_edit_line() == 1);
    }

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_navigate_to_line(0);
    repl_state_cursor_pos_set(4);
    repl_keyboard_func(1, 0, 0);
    ASSERT_TRUE("ctrl-a moves to line start", repl_state_cursor_pos() == 0);
    repl_keyboard_func(5, 0, 0);
    ASSERT_TRUE("ctrl-e moves to line end", repl_state_cursor_pos() == repl_state_input_len());
    {
        int before = *repl_state_presentation()->code_panel_layout;
        repl_keyboard_func(2, 0, 0);
        ASSERT_TRUE("ctrl-b toggles code panel layout", *repl_state_presentation()->code_panel_layout != before);
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
                strcmp(repl_state_editor_input()->input, "glVertex3f(") == 0);
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
    ASSERT_TRUE("if block cmd count", repl_state_document_count() == 3);
    ASSERT_TRUE("if begin", repl_state_document_cmds_mut()[0].type == CMD_IF_BEGIN);
    ASSERT_TRUE("if body", repl_state_document_cmds_mut()[1].type == CMD_COLOR3F);
    ASSERT_TRUE("if end", repl_state_document_cmds_mut()[2].type == CMD_IF_END);
    repl_flatten_commands();
    ASSERT_TRUE("top-level if flat count", repl_state_flat_program_count() == 3);
    ASSERT_TRUE("top-level if flat begin", repl_state_flat_program_cmds_mut()[0].type == CMD_IF_BEGIN);
    ASSERT_TRUE("top-level if flat body", repl_state_flat_program_cmds_mut()[1].type == CMD_COLOR3F);
    ASSERT_TRUE("top-level if flat end", repl_state_flat_program_cmds_mut()[2].type == CMD_IF_END);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0 {");
    repl_feed_line_public("glVertex3f(1, 2, 3);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0()");
    ASSERT_TRUE("func cmd count", repl_state_document_count() == 4);
    ASSERT_TRUE("func def", repl_state_document_cmds_mut()[0].type == CMD_FUNC_DEF);
    ASSERT_TRUE("func body", repl_state_document_cmds_mut()[1].type == CMD_VERTEX3F);
    ASSERT_TRUE("func end", repl_state_document_cmds_mut()[2].type == CMD_FUNC_END);
    ASSERT_TRUE("func call", repl_state_document_cmds_mut()[3].type == CMD_CALL);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0(radius, yoff) {");
    repl_feed_line_public("glVertex3f(radius, yoff, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(1.5, x + 2);");
    ASSERT_TRUE("param func cmd count", repl_state_document_count() == 4);
    ASSERT_TRUE("param func def", repl_state_document_cmds_mut()[0].type == CMD_FUNC_DEF);
    ASSERT_TRUE("param func header keeps names",
                strstr(repl_state_document_cmds_mut()[0].source, "radius") != NULL &&
                strstr(repl_state_document_cmds_mut()[0].source, "yoff") != NULL);
    ASSERT_TRUE("param func body keeps radius",
                strstr(repl_state_document_cmds_mut()[1].source, "radius") != NULL);
    ASSERT_TRUE("param func body keeps yoff",
                strstr(repl_state_document_cmds_mut()[1].source, "yoff") != NULL);
    ASSERT_TRUE("param func call type", repl_state_document_cmds_mut()[3].type == CMD_CALL);
    ASSERT_TRUE("param func call keeps expr",
                strstr(repl_state_document_cmds_mut()[3].source, "x + 2") != NULL);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glClearColor(0.1, 0.1, 0.1, 1);");
    repl_feed_line_public("glEnable(GL_DEPTH_TEST);");
    repl_feed_line_public("func0 {");
    repl_feed_line_public("glVertex3f(1, 2, 3);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0();");
    ASSERT_TRUE("promoted func keeps cmd count", repl_state_document_count() == 6);
    ASSERT_TRUE("promoted func def before commands", repl_state_document_cmds_mut()[0].type == CMD_FUNC_DEF);
    ASSERT_TRUE("promoted func body before commands", repl_state_document_cmds_mut()[1].type == CMD_VERTEX3F);
    ASSERT_TRUE("promoted func end before commands", repl_state_document_cmds_mut()[2].type == CMD_FUNC_END);
    ASSERT_TRUE("promoted prior clear command preserved", repl_state_document_cmds_mut()[3].type == CMD_CLEAR_COLOR);
    ASSERT_TRUE("promoted prior enable command preserved", repl_state_document_cmds_mut()[4].type == CMD_ENABLE);
    ASSERT_TRUE("promoted following call appends after prior commands", repl_state_document_cmds_mut()[5].type == CMD_CALL);

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
    ASSERT_TRUE("nested func flatten count", repl_state_flat_program_count() == 6);
    ASSERT_TRUE("nested func flatten first begin", repl_state_flat_program_cmds_mut()[0].type == CMD_BEGIN);
    ASSERT_TRUE("nested func flatten first vertex", repl_state_flat_program_cmds_mut()[1].type == CMD_VERTEX3F);
    ASSERT_TRUE("nested func flatten first x", fabsf(repl_state_flat_program_cmds_mut()[1].args[0] - 2.0f) < 1e-6f);
    ASSERT_TRUE("nested func flatten first y", fabsf(repl_state_flat_program_cmds_mut()[1].args[1] - 3.0f) < 1e-6f);
    ASSERT_TRUE("nested func flatten second x", fabsf(repl_state_flat_program_cmds_mut()[4].args[0] - 4.0f) < 1e-6f);
    ASSERT_TRUE("nested func flatten second y", fabsf(repl_state_flat_program_cmds_mut()[4].args[1] - 5.0f) < 1e-6f);
    ASSERT_TRUE("nested func call provenance immediate", repl_state_flat_program_cmds_mut()[1].call_src_cmd_idx == 6);
    ASSERT_TRUE("nested func call provenance root first", repl_state_flat_program_cmds_mut()[1].root_call_src_cmd_idx == 8);
    ASSERT_TRUE("nested func call provenance root second", repl_state_flat_program_cmds_mut()[4].root_call_src_cmd_idx == 9);
    ASSERT_TRUE("nested func scope mask includes both", (repl_state_flat_program_cmds_mut()[1].func_scope_mask & 0x3u) == 0x3u);
    {
        int matched = 0;
        repl_state_edit_line_set(8);
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("nested func call line highlights one invocation", matched == 3);
    }
    {
        int matched = 0;
        repl_state_edit_line_set(6);
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("nested inner call line highlights all invocations", matched == 6);
    }
    {
        int matched = 0;
        repl_state_edit_line_set(2);
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("nested function body highlights all invocations", matched == 6);
    }
    {
        int matched = 0;
        repl_state_edit_line_set(5);
        for (int i = 0; i < repl_state_flat_program_count(); i++)
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
    ASSERT_TRUE("nested block trailing cmd count", repl_state_document_count() == 6);
    ASSERT_TRUE("nested block trailing cmd order body", repl_state_document_cmds_mut()[4].type == CMD_COLOR3F);
    ASSERT_TRUE("nested block trailing cmd order end", repl_state_document_cmds_mut()[5].type == CMD_FUNC_END);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0(n) {");
    repl_feed_line_public("for(i, 0, n) glVertex3f(i, n, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(3);");
    ASSERT_TRUE("local for begin type", repl_state_document_cmds_mut()[1].type == CMD_FOR_BEGIN);
    ASSERT_TRUE("local for body type", repl_state_document_cmds_mut()[2].type == CMD_VERTEX3F);
    ASSERT_TRUE("local for end type", repl_state_document_cmds_mut()[3].type == CMD_FOR_END);
    ASSERT_TRUE("local for header keeps n", strstr(repl_state_document_cmds_mut()[1].source, "n") != NULL);
    ASSERT_TRUE("local for body keeps n", strstr(repl_state_document_cmds_mut()[2].source, "n") != NULL);
    repl_flatten_commands();
    ASSERT_TRUE("local for flatten count", repl_state_flat_program_count() == 3);
    ASSERT_TRUE("local for first x", fabsf(repl_state_flat_program_cmds_mut()[0].args[0] - 0.0f) < 1e-6f);
    ASSERT_TRUE("local for second x", fabsf(repl_state_flat_program_cmds_mut()[1].args[0] - 1.0f) < 1e-6f);
    ASSERT_TRUE("local for third x", fabsf(repl_state_flat_program_cmds_mut()[2].args[0] - 2.0f) < 1e-6f);
    ASSERT_TRUE("local for body uses param", fabsf(repl_state_flat_program_cmds_mut()[2].args[1] - 3.0f) < 1e-6f);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0(scale) {");
    repl_feed_line_public("if(scale > 1) {");
    repl_feed_line_public("glVertex3f(scale, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(2);");
    repl_feed_line_public("func0(0.5);");
    ASSERT_TRUE("local if header keeps scale", strstr(repl_state_document_cmds_mut()[1].source, "scale > 1") != NULL);
    repl_flatten_commands();
    ASSERT_TRUE("local if flatten count", repl_state_flat_program_count() == 1);
    ASSERT_TRUE("local if flatten type", repl_state_flat_program_cmds_mut()[0].type == CMD_VERTEX3F);
    ASSERT_TRUE("local if flatten x", fabsf(repl_state_flat_program_cmds_mut()[0].args[0] - 2.0f) < 1e-6f);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0(cols) {");
    repl_feed_line_public("for(j, 0, cols + 1) {");
    repl_feed_line_public("x = -1.5 + 3.0*j/cols;");
    repl_feed_line_public("glVertex3f(x, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(4);");
    repl_flatten_commands();
    ASSERT_TRUE("local assign flatten count", repl_state_flat_program_count() == 10);
    ASSERT_TRUE("local assign first type", repl_state_flat_program_cmds_mut()[0].type == CMD_VAR_ASSIGN);
    ASSERT_TRUE("local assign first value", fabsf(repl_state_flat_program_cmds_mut()[0].args[0] - (-1.5f)) < 1e-6f);
    ASSERT_TRUE("local assign second value", fabsf(repl_state_flat_program_cmds_mut()[2].args[0] - (-0.75f)) < 1e-6f);
    ASSERT_TRUE("local assign third value", fabsf(repl_state_flat_program_cmds_mut()[4].args[0] - 0.0f) < 1e-6f);
    ASSERT_TRUE("local assign last value", fabsf(repl_state_flat_program_cmds_mut()[8].args[0] - 1.5f) < 1e-6f);
    ASSERT_TRUE("local assign vertex last x", fabsf(repl_state_flat_program_cmds_mut()[9].args[0] - 1.5f) < 1e-6f);
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
        repl_replay_start();
        replay_state = REPLAY_PAUSED;

        replay_pc = 1;
        replay_src_line = 0;
        ASSERT_TRUE("replay display assignment text",
                    repl_replay_code_panel_get_command_display_text(0, display, sizeof(display)));
        ASSERT_STR("replay display assignment inline comment",
                   display,
                   "  i = i + j + 3; // i = 3.2 + 1.2 + 3 = 7.4");

        replay_pc = 2;
        replay_src_line = 1;
        ASSERT_TRUE("replay display prior assignment still visible",
                    repl_replay_code_panel_get_command_display_text(0, display, sizeof(display)));
        ASSERT_STR("replay display prior assignment inline comment",
                   display,
                   "  i = i + j + 3; // i = 3.2 + 1.2 + 3 = 7.4");
        ASSERT_TRUE("replay display vertex text",
                    repl_replay_code_panel_get_command_display_text(1, display, sizeof(display)));
        ASSERT_STR("replay display vertex source unchanged",
                   display,
                   "  glVertex3f(i, j, 0);");

        replay_active = 0;
        replay_state = REPLAY_OFF;
        replay_src_line = -1;
        replay_pc = 0;
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
        repl_replay_start();
        replay_state = REPLAY_PAUSED;

        replay_pc = 1;
        replay_src_line = 0;
        ASSERT_TRUE("replay chain assignment text",
                    repl_replay_code_panel_get_command_display_text(0, display, sizeof(display)));
        ASSERT_STR("replay chain assignment inline comment",
                   display,
                   "  i = i + k; // i = 0.23 + 0.5 = 0.73");

        replay_active = 0;
        replay_state = REPLAY_OFF;
        replay_src_line = -1;
        replay_pc = 0;
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
        repl_replay_start();
        replay_state = REPLAY_PAUSED;

        replay_pc = repl_state_flat_program_count();
        replay_src_line = 5;
        ASSERT_TRUE("replay goto skipped assignment text",
                    repl_replay_code_panel_get_command_display_text(3, display, sizeof(display)));
        ASSERT_TRUE("replay goto skipped assignment uses pre-jump value",
                    strstr(display, "// x = 1 + 1 = 2") != NULL);
        ASSERT_TRUE("replay goto skipped assignment ignores skipped overwrite",
                    strstr(display, "100 + 1") == NULL);

        replay_active = 0;
        replay_state = REPLAY_OFF;
        replay_src_line = -1;
        replay_pc = 0;
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
        repl_replay_start();
        replay_state = REPLAY_PAUSED;

        replay_pc = 1;
        replay_src_line = 1;
        ASSERT_TRUE("replay scientific assignment text",
                    repl_replay_code_panel_get_command_display_text(1, display, sizeof(display)));
        ASSERT_TRUE("replay scientific inline comment keeps expanded rhs",
                    strstr(display, "// i = 1 * 1e-06 = 1e-06") != NULL);

        replay_active = 0;
        replay_state = REPLAY_OFF;
        replay_src_line = -1;
        replay_pc = 0;
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
        repl_state_edit_line_set(0);
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            if (repl_state_flat_program_cmds_mut()[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("top-level color before block matches first vertices", matched == 2);
    }
    {
        int matched = 0;
        repl_state_edit_line_set(5);
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            if (repl_state_flat_program_cmds_mut()[i].type == CMD_VERTEX3F)
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
        repl_state_edit_line_set(0);
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            if (repl_state_flat_program_cmds_mut()[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("top-level normal before block matches first vertices", matched == 2);
    }
    {
        int matched = 0;
        repl_state_edit_line_set(5);
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            if (repl_state_flat_program_cmds_mut()[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("top-level second normal before block matches second vertices", matched == 2);
    }
    ASSERT_TRUE("feeding normal for first block vertex", repl_find_feeding_normal_cmd(2) == 0);
    ASSERT_TRUE("feeding normal for second block vertex", repl_find_feeding_normal_cmd(7) == 5);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("gluColor(1, 0, 0);");
    repl_feed_line_public("gluVertex(0, 0, 0);");
    ASSERT_TRUE("tess color feeds tess vertex", repl_find_feeding_color_cmd(1) == 0);
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("gluVertex(0, 0, 0);");
    ASSERT_TRUE("no tess color → -1", repl_find_feeding_color_cmd(0) == -1);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("gluNormal(0, 0, 1);");
    repl_feed_line_public("gluVertex(0, 0, 0);");
    ASSERT_TRUE("tess normal feeds tess vertex", repl_find_feeding_normal_cmd(1) == 0);
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("gluVertex(0, 0, 0);");
    ASSERT_TRUE("no tess normal → -1", repl_find_feeding_normal_cmd(0) == -1);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glColor3f(1, 0, 0);");
    repl_feed_line_public("glNormal3f(0, 0, 1);");
    ASSERT_TRUE("non-vertex color → -1", repl_find_feeding_color_cmd(0) == -1);
    ASSERT_TRUE("non-vertex normal → -1", repl_find_feeding_normal_cmd(1) == -1);
    ASSERT_TRUE("oob color → -1", repl_find_feeding_color_cmd(-1) == -1);
    ASSERT_TRUE("oob normal → -1", repl_find_feeding_normal_cmd(99) == -1);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glEnd();");
    repl_flatten_commands();
    repl_navigate_to_line(1);
    {
        int matched = 0;
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            if (repl_state_flat_program_cmds_mut()[i].type == CMD_VERTEX3F)
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
    ASSERT_TRUE("recursive flatten count", repl_state_flat_program_count() == 4);
    ASSERT_TRUE("recursive first x", fabsf(repl_state_flat_program_cmds_mut()[0].args[0] - 3.0f) < 1e-6f);
    ASSERT_TRUE("recursive second x", fabsf(repl_state_flat_program_cmds_mut()[1].args[0] - 2.0f) < 1e-6f);
    ASSERT_TRUE("recursive third x", fabsf(repl_state_flat_program_cmds_mut()[2].args[0] - 1.0f) < 1e-6f);
    ASSERT_TRUE("recursive base x", fabsf(repl_state_flat_program_cmds_mut()[3].args[0] - 0.0f) < 1e-6f);

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
    ASSERT_TRUE("mutual recursion flatten count", repl_state_flat_program_count() == 3);
    ASSERT_TRUE("mutual recursion first x", fabsf(repl_state_flat_program_cmds_mut()[0].args[0] - 2.0f) < 1e-6f);
    ASSERT_TRUE("mutual recursion second x", fabsf(repl_state_flat_program_cmds_mut()[1].args[0] - (-1.0f)) < 1e-6f);
    ASSERT_TRUE("mutual recursion base x", fabsf(repl_state_flat_program_cmds_mut()[2].args[0] - 0.0f) < 1e-6f);

    repl_reset_state(); declare_test_vars();
    g_status[0] = '\0';
    repl_feed_line_public("func0(n) {");
    repl_feed_line_public("func0(n + 1);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(0);");
    repl_flatten_commands();
    ASSERT_TRUE("runaway recursion emits no flat cmds", repl_state_flat_program_count() == 0);
    ASSERT_TRUE("runaway recursion depth guard",
                strstr(g_status, "depth limit") != NULL ||
                strstr(g_status, "visit budget") != NULL);

    /* Regression: a second definition of func<N> is rejected with a
     * status message instead of silently creating a duplicate that the
     * flattener would ignore. */
    repl_reset_state(); declare_test_vars();
    g_status[0] = '\0';
    repl_feed_line_public("func0 {");
    repl_feed_line_public("glVertex3f(1, 2, 3);");
    repl_feed_line_public("}");
    int dupe_cmd_count_before = repl_state_document_count();
    repl_feed_line_public("func0(y) {");
    ASSERT_TRUE("duplicate func def rejected: cmd count unchanged",
                repl_state_document_count() == dupe_cmd_count_before);
    ASSERT_TRUE("duplicate func def rejected: status names func0",
                strstr(g_status, "func0 already defined") != NULL);

    /* Cursor on the existing def: "overwrite" path, still allowed. */
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0 {");
    repl_feed_line_public("glVertex3f(1, 2, 3);");
    repl_feed_line_public("}");
    repl_state_edit_line_set(0);
    repl_state_insert_mode_set(0);
    g_status[0] = '\0';
    repl_feed_line_public("func0(z) {");
    ASSERT_TRUE("func def overwrite: still CMD_FUNC_DEF",
                repl_state_document_cmds_mut()[0].type == CMD_FUNC_DEF);
    ASSERT_TRUE("func def overwrite: header keeps new param",
                strstr(repl_state_document_cmds_mut()[0].source, "z") != NULL);
    ASSERT_TRUE("func def overwrite: status not a duplicate rejection",
                strstr(g_status, "already defined") == NULL);

    /* Regression: calling func<N> with no definition is rejected at
     * commit time, the same way `x = 1;` is rejected before `float x;`
     * is declared.  Call is never added to repl_state_document_cmds_mut() and status names the
     * function. */
    repl_reset_state(); declare_test_vars();
    g_status[0] = '\0';
    int pre_call_cmd_count = repl_state_document_count();
    repl_feed_line_public("func5();");
    ASSERT_TRUE("undefined top-level call: not added to repl_state_document_cmds_mut()",
                repl_state_document_count() == pre_call_cmd_count);
    ASSERT_TRUE("undefined top-level call: status names func5",
                strstr(g_status, "func5") != NULL);
    ASSERT_TRUE("undefined top-level call: status says undefined",
                strstr(g_status, "undefined function") != NULL);

    /* Same call accepted once the definition exists. */
    repl_feed_line_public("func5 {");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("}");
    int pre_retry_cmd_count = repl_state_document_count();
    g_status[0] = '\0';
    repl_feed_line_public("func5();");
    ASSERT_TRUE("defined top-level call: commit adds a CMD_CALL",
                repl_state_document_count() == pre_retry_cmd_count + 1);
    ASSERT_TRUE("defined top-level call: last cmd is CMD_CALL",
                repl_state_document_cmds_mut()[repl_state_document_count() - 1].type == CMD_CALL);
    repl_flatten_commands();
    ASSERT_TRUE("defined top-level call: flat body expands",
                repl_state_flat_program_count() == 1 && repl_state_flat_program_cmds_mut()[0].type == CMD_VERTEX3F);

    /* Forward references inside a function body are still allowed so
     * mutual recursion keeps working.  func0's body calls func1 before
     * func1 is defined; top-level `func0(2)` only commits after both
     * defs exist. */
    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("func0(n) {");
    repl_feed_line_public("if(n > 0) {");
    repl_feed_line_public("glVertex3f(n, 0, 0);");
    repl_feed_line_public("func1(n - 1);");   /* forward ref inside body */
    repl_feed_line_public("}");
    repl_feed_line_public("}");
    repl_feed_line_public("func1(n) {");
    repl_feed_line_public("if(n > 0) {");
    repl_feed_line_public("glVertex3f(-n, 0, 0);");
    repl_feed_line_public("func0(n - 1);");
    repl_feed_line_public("}");
    repl_feed_line_public("}");
    int pre_mutual_call_count = repl_state_document_count();
    g_status[0] = '\0';
    repl_feed_line_public("func0(2);");
    ASSERT_TRUE("mutual recursion: top-level call accepted",
                repl_state_document_count() == pre_mutual_call_count + 1 &&
                repl_state_document_cmds_mut()[repl_state_document_count() - 1].type == CMD_CALL);
    repl_flatten_commands();
    ASSERT_TRUE("mutual recursion: flatten reaches base case",
                repl_state_flat_program_count() > 0);

    printf("repl_core_commit: %d/%d passed\n", g_pass, g_run);
    return (g_run == g_pass) ? 0 : 1;
}
