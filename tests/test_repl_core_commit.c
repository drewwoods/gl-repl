#include "editor/state.h"
#include "app/glr_state.h"
#include "editor/input.h"
#include "app/glr_ctrl.h"
#include "repl/core.h"
#include "repl/core_internal.h"
#include "repl/executor.h"
#include "editor/clipboard.h"
#include "editor/commit.h"
#include "repl/state.h"
#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_state.h"
#include "ui/app/state.h"
#include "repl/replay_annotations.h"
#include "ui/app/panels.h"
#include "ui/app/layout.h"
#include "ui/core/metrics.h"
#include "ui/core/gl_2d.h"
#include "ui/app/repl_code_panel.h"

#define g_status  (ui_state_status_mut()->text)
#define g_scroll  (editor_state_scroll_mut()->scroll)
#define g_t_playing (repl_state_variables_mut()->time_playing)
#define g_ac_ghost  (editor_state_autocomplete_mut()->ghost)
#define g_ac_hint   (editor_state_autocomplete_mut()->hint)
#define g_ac_matches (editor_state_autocomplete_mut()->matches)

#include "support/test_harness.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    TEST_ASSERT_STR(&g_harness, label, got, exp); \
} while (0)

#define replay_active        (replay_state_mut()->active)
#define replay_state         (replay_state_mut()->state)
#define replay_pc            (replay_state_mut()->pc)
#define replay_src_line      (replay_state_mut()->src_line_idx)

#define g_workspace_header_lines (repl_state_import_export().workspace_header_lines)
#define g_workspace_header_line_count (repl_state_import_export().workspace_header_line_count)
#define g_render_state_lines (repl_state_import_export().render_state_lines)
#define g_cam_lines (repl_state_import_export().cam_lines)

static void declare_test_vars(void) {
    ui_state_viewport_set_size(1200, 800);
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

static void build_test_ui_snapshot(UiRenderSnapshot *snap) {
    glr_ctrl_build_ui_snapshot(snap);
}

static int apply_code_panel_follow_for_test(int *out_follow_doc_line,
                                            int *out_visible_lines) {
    UiRenderSnapshot snap;

    build_test_ui_snapshot(&snap);
    return glr_ctrl_code_panel_apply_scroll_follow_for_test(
        &snap, out_follow_doc_line, out_visible_lines);
}

static UiHit code_panel_hit_test_current_snapshot(int mx, int my,
                                                  int variable_count) {
    UiRenderSnapshot snap;

    build_test_ui_snapshot(&snap);
    return ui_panels_hit_test(&snap, mx, my, variable_count);
}

static int code_panel_mouse_y_for_cmd(int cmd_idx) {
    UiRenderSnapshot snap;
    UiReplCodePanelLayout layout;
    int cp_y, cp_h, panel_w;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = glr_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int doc_line;

    /* Mirror the frame path: scroll-follow is applied before the code panel is
     * rendered or hit-tested, so synthetic mouse targets need the same step. */
    (void)apply_code_panel_follow_for_test(NULL, NULL);

    ui_layout_code_panel_rect(NULL, &cp_y, &panel_w, &cp_h);
    build_test_ui_snapshot(&snap);
    ui_repl_code_panel_build_layout(&snap, &layout, panel_w, text_x, cp_h);

    doc_line = layout.header_rows;
    for (int i = 0; i < cmd_idx && i < repl_state_document_count(); i++)
        doc_line += layout.cmd_main_rows[i] + layout.replay_extra_rows[i];

    {
        int visible_lines = layout.visible_lines;
        if (doc_line < g_scroll)
            g_scroll = doc_line;
        else if (doc_line >= g_scroll + visible_lines)
            g_scroll = doc_line - visible_lines + 1;
    }

    int vis = doc_line - g_scroll;
    int line_y_start = cp_y + cp_h - CODE_MARGIN_Y - 2 * LINE_H;
    int gl_y = line_y_start - vis * LINE_H + 1;
    return ui_state_viewport().window_h - gl_y;
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

/* Resolve source text for a flat command using the editor buffer, mirroring
 * the real executor's execution_flat_text() logic. */
static const char *flat_cmd_text(int pc) {
    int src = repl_state_flat_program_cmds_mut()[pc].src_cmd_idx;
    if (src < 0 || src >= repl_state_document_count())
        return "";
    const char *t = editor_buffer_line(src);
    return (t && t[0]) ? t : "";
}

/*
 * Mirror only the execute-time control-flow pieces that affect variables.
 * This lets the tests exercise goto/if/assignment replay rules without
            UiHit hit = code_panel_hit_test_current_snapshot(mx, my,
                                                             g_num_predef_vars);
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
            /* Match the real executor: apply pre-computed args[0]
             * without re-evaluating. Flatten owns the eval. */
            int vi = repl_state_flat_program_cmds_mut()[pc].num_args;
            float value = repl_state_flat_program_cmds_mut()[pc].args[0];
            if (vi >= 0 && vi < g_num_predef_vars)
                g_predef_vars[vi].value = value;
            break;
        }
        case CMD_IF_BEGIN: {
            float cond = repl_state_flat_program_cmds_mut()[pc].args[0];
            if (repl_state_flat_program_cmds_mut()[pc].has_vars) {
                char cond_text[256];
                if (repl_extract_paren_payload(flat_cmd_text(pc),
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
            if (!repl_extract_goto_label(flat_cmd_text(pc), label, sizeof(label)))
                break;
            if (goto_count++ > 100000)
                return;
            for (int li = 0; li < repl_state_flat_program_count(); li++) {
                char target_label[64];
                if (repl_state_flat_program_cmds_mut()[li].valid &&
                    repl_state_flat_program_cmds_mut()[li].type == CMD_GOTO_LABEL &&
                    repl_extract_label_name(flat_cmd_text(li),
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

    glr_app_reset_all(); declare_test_vars();
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

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("z = -0.55;");
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

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("A[0] = 1;");
    ASSERT_TRUE("scratch assign cmd count", repl_state_document_count() == 1);
    ASSERT_TRUE("scratch assign type",
                repl_state_document_cmds_mut()[0].type == CMD_SCRATCH_ASSIGN);
    ASSERT_TRUE("scratch assign normalized text",
              strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "",
                  "A[0] = 1;") != NULL);
    {
        float scratch = -1.0f;
        ASSERT_TRUE("scratch assign applied live",
                    repl_eval_scratch_get(0, 0, &scratch) && fabsf(scratch - 1.0f) < 1e-6f);
        ASSERT_TRUE("scratch assign cached array idx",
                    fabsf(repl_state_document_cmds_mut()[0].args[0]) < 1e-6f);
        ASSERT_TRUE("scratch assign cached elem idx",
                    fabsf(repl_state_document_cmds_mut()[0].args[1]) < 1e-6f);
        ASSERT_TRUE("scratch assign cached value",
                    fabsf(repl_state_document_cmds_mut()[0].args[2] - 1.0f) < 1e-6f);
    }

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("for(i, 0, 2) {");
    editor_feed_line("A[i] = A[i] + 1;");
    editor_feed_line("}");
    ASSERT_TRUE("scratch assign in loop cmd count", repl_state_document_count() == 3);
    ASSERT_TRUE("scratch assign in loop type",
                repl_state_document_cmds_mut()[1].type == CMD_SCRATCH_ASSIGN);
    ASSERT_TRUE("scratch assign in loop has vars",
                repl_state_document_cmds_mut()[1].has_vars == 1);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("A[0] = 1;");
    editor_feed_line("func0(depth) {");
    editor_feed_line("if(depth > 0) {");
    editor_feed_line("func0(depth - 1);");
    editor_feed_line("A[depth] = A[depth - 1] + 1;");
    editor_feed_line("}");
    editor_feed_line("}");
    editor_feed_line("func0(3);");
    repl_flatten_commands(editor_state_edit_line());
    repl_execute_commands();
    {
        float scratch = 0.0f;
        ASSERT_TRUE("recursive scratch A[1]",
                    repl_eval_scratch_get(0, 1, &scratch) && fabsf(scratch - 2.0f) < 1e-6f);
        ASSERT_TRUE("recursive scratch A[2]",
                    repl_eval_scratch_get(0, 2, &scratch) && fabsf(scratch - 3.0f) < 1e-6f);
        ASSERT_TRUE("recursive scratch A[3]",
                    repl_eval_scratch_get(0, 3, &scratch) && fabsf(scratch - 4.0f) < 1e-6f);
    }

    /* Var-decl-overwrite cascade: replacing a CMD_VAR_DECLARE with an
     * assign must reject when one of the dropped names is referenced by
     * another line. The diagnostic comes through the compile-error path
     * (commit 43 absorbed the cascade into repl_compile_var_assign), so
     * apply never runs and the document is unchanged. */
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("float foo;");
    editor_feed_line("y = foo;");
    ASSERT_TRUE("cascade fixture cmd count", repl_state_document_count() == 2);
    ASSERT_TRUE("cascade fixture line 0 is decl",
                repl_state_document_cmds_mut()[0].type == CMD_VAR_DECLARE);
    ASSERT_TRUE("cascade fixture line 1 is assign",
                repl_state_document_cmds_mut()[1].type == CMD_VAR_ASSIGN);
    {
        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);
        EditorInputState *inp = editor_state_input_mut();
        strcpy(inp->input, "foo = 5");
        inp->input_len = (int)strlen(inp->input);
        editor_cursor_pos_set(inp->input_len);
        g_status[0] = '\0';
        int consumed = editor_try_assign_variable();
        ASSERT_TRUE("cascade in-use rejection consumed", consumed == 1);
        ASSERT_TRUE("cascade in-use status names variable",
                    strstr(g_status, "foo") != NULL &&
                    strstr(g_status, "in use") != NULL);
        ASSERT_TRUE("cascade in-use leaves doc unchanged",
                    repl_state_document_count() == 2 &&
                    repl_state_document_cmds_mut()[0].type == CMD_VAR_DECLARE);
        ASSERT_TRUE("cascade in-use leaves foo declared",
                    repl_eval_find_predef_var_idx("foo") >= 0);
    }

    /* Goto loops can no longer drive a counter through an if-block
     * gate: flatten resolves the if-condition once (eval-at-flatten),
     * so each goto iteration re-runs the same flat command with the
     * same args. With self-referential `n = n + 1;`, the executor
     * applies args[0] = 1 every iteration and `n` plateaus. Goto
     * remains documented as partial; the test now exercises the
     * single-pass path and verifies n stops at 1. */
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("n = 0;");
    editor_feed_line(":walk");
    editor_feed_line("n = n + 1;");
    editor_feed_line("if(n < 3) {");
    editor_feed_line("goto walk;");
    editor_feed_line("}");
    ASSERT_TRUE("expr assign cmd count", repl_state_document_count() == 6);
    ASSERT_TRUE("expr assign preserves source", strstr(editor_buffer_line(2) ? editor_buffer_line(2) : "", "n + 1") != NULL);
    ASSERT_TRUE("expr assign marked has_vars", repl_state_document_cmds_mut()[2].has_vars == 1);
    repl_flatten_commands(editor_state_edit_line());
    repl_execute_commands();
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
            ASSERT_TRUE("goto-with-if no longer iterates self-ref counter",
                        fabsf(g_predef_vars[n_idx].value - 1.0f) < 1e-6f);
    }

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("n = 0;");
    editor_feed_line("glBegin(GL_LINES);");
    editor_feed_line(":stripe");
    editor_feed_line("glVertex3f(-1.5 + 0.42*n, -0.9, 0);");
    editor_feed_line("glVertex3f(-1.5 + 0.42*n, 0.9, 0);");
    editor_feed_line("n = n + 1;");
    editor_feed_line("if(n < 3) {");
    editor_feed_line("goto stripe;");
    editor_feed_line("}");
    editor_feed_line("glEnd();");
    ASSERT_TRUE("goto geom cmd count", repl_state_document_count() == 10);
    ASSERT_TRUE("goto geom first vertex keeps expr",
                strstr(editor_buffer_line(3) ? editor_buffer_line(3) : "", "0.42*n") != NULL);
    ASSERT_TRUE("goto geom first vertex has vars", repl_state_document_cmds_mut()[3].has_vars == 1);
    repl_flatten_commands(editor_state_edit_line());
    ASSERT_TRUE("goto geom flat first vertex has vars", repl_state_flat_program_cmds_mut()[3].has_vars == 1);
    ASSERT_TRUE("goto geom flat second vertex has vars", repl_state_flat_program_cmds_mut()[4].has_vars == 1);
    run_flat_control_flow_only();
    {
        int n_idx = predef_idx("n");
        ASSERT_TRUE("goto geom n predef exists", n_idx >= 0);
        /* Same partial-goto behaviour as above: n stops at 1. */
        if (n_idx >= 0)
            ASSERT_TRUE("goto geom no longer iterates self-ref counter",
                        fabsf(g_predef_vars[n_idx].value - 1.0f) < 1e-6f);
    }
    ASSERT_TRUE("goto geom flat first vertex still at initial x",
                fabsf(repl_state_flat_program_cmds_mut()[3].args[0] - (-1.5f)) < 1e-5f);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line(":walk");
    ASSERT_TRUE("label cmd count", repl_state_document_count() == 1);
    ASSERT_TRUE("label stored as C label", strcmp(editor_buffer_line(0) ? editor_buffer_line(0) : "", "walk:") == 0);
    editor_navigate_to_line(0);
    ASSERT_TRUE("label loads back into editor as repl syntax",
                strcmp(editor_state_input().input, ":walk") == 0);
    editor_handle_key(';', 0, 0);
    ASSERT_TRUE("recommitting loaded label keeps label type", repl_state_document_cmds_mut()[0].type == CMD_GOTO_LABEL);
    ASSERT_TRUE("recommitting loaded label keeps source", strcmp(editor_buffer_line(0) ? editor_buffer_line(0) : "", "walk:") == 0);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("for(i, 0, 3) {");
    editor_feed_line("glVertex3f(i, 0, 0);");
    editor_feed_line("}");
    ASSERT_TRUE("for block cmd count", repl_state_document_count() == 3);
    ASSERT_TRUE("for begin", repl_state_document_cmds_mut()[0].type == CMD_FOR_BEGIN);
    ASSERT_TRUE("for body", repl_state_document_cmds_mut()[1].type == CMD_VERTEX3F);
    ASSERT_TRUE("for end", repl_state_document_cmds_mut()[2].type == CMD_FOR_END);
    ASSERT_TRUE("for body keeps i", strstr(editor_buffer_line(1) ? editor_buffer_line(1) : "", "i") != NULL);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glEnd();");
    editor_navigate_to_line(1);
    editor_cursor_pos_set(0);
    editor_handle_key('\r', 0, 0);
    ASSERT_TRUE("enter at line start enters insert mode", editor_insert_mode() == 1);
    ASSERT_TRUE("enter at line start keeps insertion index", editor_state_edit_line() == 1);
    {
        EditorInputState *inp = editor_state_input_mut();
        strcpy(inp->input, "glColor3f(1, 0, 0)");
        inp->input_len = (int)strlen(inp->input);
        editor_cursor_pos_set(inp->input_len);
    }
    editor_handle_key('\r', 0, 0);
    ASSERT_TRUE("inserted line before current cmd count", repl_state_document_count() == 3);
    ASSERT_TRUE("inserted line before current type", repl_state_document_cmds_mut()[1].type == CMD_COLOR3F);
    ASSERT_TRUE("original current line shifted down", repl_state_document_cmds_mut()[2].type == CMD_END);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glEnd();");
    editor_navigate_to_line(0);
    editor_cursor_pos_set(editor_input_len());
    editor_handle_key('\r', 0, 0);
    ASSERT_TRUE("enter away from line start still inserts after", editor_insert_mode() == 1 && editor_state_edit_line() == 1);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("glEnd();");
    /* J2.2: code-panel mouse press / drag dispatch through the
     * controller's UiHit router. Press resolves the click via
     * ui_panels_hit_test and dispatches via
     * glr_ctrl_router_handle_code_panel_hit; subsequent motion
     * calls glr_ctrl_router_handle_code_panel_drag with the new
     * coords. */
    {
        UiHit hit = code_panel_hit_test_current_snapshot(
            CODE_MARGIN_X + 1,
            code_panel_mouse_y_for_cmd(0),
            g_num_predef_vars);
        glr_ctrl_router_handle_code_panel_hit(hit, CODE_MARGIN_X + 1,
                                                 code_panel_mouse_y_for_cmd(0));
    }
    ASSERT_TRUE("mouse press selects current line for edit", editor_state_edit_line() == 0);
    ASSERT_TRUE("mouse press starts with no selection", !editor_clipboard_sel_active());
    glr_ctrl_router_handle_code_panel_drag(CODE_MARGIN_X + 1,
                                              code_panel_mouse_y_for_cmd(2));
    ASSERT_TRUE("mouse drag activates selection", editor_clipboard_sel_active());
    ASSERT_TRUE("mouse drag selection low", editor_clipboard_sel_lo() == 0);
    ASSERT_TRUE("mouse drag selection high", editor_clipboard_sel_hi() == 2);
    ASSERT_TRUE("mouse drag navigates to drag end", editor_state_edit_line() == 2);
    glr_ctrl_router_reset_code_panel_drag();
    editor_handle_key(8, 0, 0);
    ASSERT_TRUE("backspace deletes selected lines", repl_state_document_count() == 0);
    ASSERT_TRUE("backspace clears selection after delete", !editor_clipboard_sel_active());
    ASSERT_TRUE("backspace keeps edit line at start after delete", editor_state_edit_line() == 0);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glEnd();");
    editor_navigate_to_line(0);
    {
        int linenum_w = 4 * FONT_W;
        int idx_col_w = glr_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
        int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
        int mx = text_x + 1;
        int my = code_panel_mouse_y_for_cmd(repl_state_document_count());
        UiHit hit = code_panel_hit_test_current_snapshot(mx, my,
                                 g_num_predef_vars);
        ASSERT_TRUE("trailing blank row hit kind", hit.kind == UI_HIT_CODE_TEXT);
        ASSERT_TRUE("trailing blank row targets document end",
                    hit.line_idx == repl_state_document_count());
        glr_ctrl_router_handle_code_panel_hit(hit, mx, my);
        ASSERT_TRUE("clicking trailing blank row navigates to document end",
                    editor_state_edit_line() == repl_state_document_count());
    }

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("if(x > 0) {");
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("}");
    {
        int linenum_w = 4 * FONT_W;
        int idx_col_w = glr_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
        int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
        int indent = test_leading_ws_chars(editor_buffer_line(1) ? editor_buffer_line(1) : "");
        int mx = text_x + indent * FONT_W + 1;
        int my = code_panel_mouse_y_for_cmd(1);
        UiHit hit = code_panel_hit_test_current_snapshot(mx, my,
                                 g_num_predef_vars);
        glr_ctrl_router_handle_code_panel_hit(hit, mx, my);
        ASSERT_TRUE("clicking indented active line keeps cursor at first char",
                    editor_cursor_pos() == 0);
        ASSERT_TRUE("clicking indented active line selects correct line",
                    editor_state_edit_line() == 1);
    }

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glBegin(GL_POINTS);");
    editor_navigate_to_line(0);
    editor_cursor_pos_set(4);
    editor_handle_key(1, 0, 0);
    ASSERT_TRUE("ctrl-a moves to line start", editor_cursor_pos() == 0);
    editor_handle_key(5, 0, 0);
    ASSERT_TRUE("ctrl-e moves to line end", editor_cursor_pos() == editor_input_len());
    {
        int before = glr_state_presentation().code_panel_layout;
        glr_ctrl_router_handle_cfg_shortcut_key(2);
        ASSERT_TRUE("ctrl-b toggles code panel layout", glr_state_presentation().code_panel_layout != before);
    }

    glr_app_reset_all(); declare_test_vars();
    {
        const char *prefix = "glVer";
        for (int i = 0; prefix[i]; i++)
            editor_handle_key((unsigned char)prefix[i], 0, 0);
    }
    ASSERT_TRUE("autocomplete popup shows signature text",
                strcmp(g_ac_matches[0], "glVertex3f(x, y, z)") == 0);
    ASSERT_TRUE("autocomplete ghost inserts callable suffix",
                strcmp(g_ac_ghost, "tex3f(") == 0);
    ASSERT_TRUE("autocomplete hint shows parameter names",
                strcmp(g_ac_hint, "x, y, z)") == 0);
    editor_handle_key('\t', 0, 0);
    ASSERT_TRUE("tab inserts function call prefix only",
                strcmp(editor_state_input().input, "glVertex3f(") == 0);
    ASSERT_TRUE("function call hint starts at first parameter",
                strcmp(g_ac_hint, "x, y, z)") == 0);
    editor_handle_key('1', 0, 0);
    ASSERT_TRUE("function call hint advances after first arg text",
                strcmp(g_ac_hint, ", y, z)") == 0);
    editor_handle_key(',', 0, 0);
    editor_handle_key(' ', 0, 0);
    ASSERT_TRUE("function call hint shows second parameter at comma",
                strcmp(g_ac_hint, "y, z)") == 0);
    editor_handle_key('2', 0, 0);
    ASSERT_TRUE("function call hint advances to remaining args",
                strcmp(g_ac_hint, ", z)") == 0);
    editor_handle_key(',', 0, 0);
    editor_handle_key(' ', 0, 0);
    editor_handle_key('3', 0, 0);
    ASSERT_TRUE("function call hint ends with closing paren",
                strcmp(g_ac_hint, ")") == 0);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("func0(radius, yoff) {");
    editor_feed_line("}");
    {
        const char *call = "func0(";
        for (int i = 0; call[i]; i++)
            editor_handle_key((unsigned char)call[i], 0, 0);
    }
    ASSERT_TRUE("user function hint uses declared parameter names",
                strcmp(g_ac_hint, "radius, yoff)") == 0);
    ASSERT_TRUE("user function hint suppresses stale no-arg ghost",
                g_ac_ghost[0] == '\0');

    /* Top-level if-blocks are now resolved at flatten time: the
     * IF_BEGIN/IF_END markers are compiled away, and the body is
     * included iff the condition evaluates non-zero. With x == 0
     * (declare_test_vars only declares, doesn't init) the cond is
     * false and the body is skipped entirely. */
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("if(x > 0) {");
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("}");
    ASSERT_TRUE("if block cmd count", repl_state_document_count() == 3);
    ASSERT_TRUE("if begin", repl_state_document_cmds_mut()[0].type == CMD_IF_BEGIN);
    ASSERT_TRUE("if body", repl_state_document_cmds_mut()[1].type == CMD_COLOR3F);
    ASSERT_TRUE("if end", repl_state_document_cmds_mut()[2].type == CMD_IF_END);
    repl_flatten_commands(editor_state_edit_line());
    ASSERT_TRUE("top-level if (false cond) compiled away",
                repl_state_flat_program_count() == 0);

    /* When the cond is true at flatten time, the body lands in the
     * flat array without IF_BEGIN/IF_END markers. */
    {
        char err[128];
        glr_app_reset_all();
        repl_eval_declare_predef_var("xx", err, sizeof(err));
        int xx_idx = repl_eval_find_predef_var_idx("xx");
        repl_state_variables_mut()->predef_vars[xx_idx].value = 1.0f;
        editor_feed_line("if(xx > 0) {");
        editor_feed_line("glColor3f(1, 0, 0);");
        editor_feed_line("}");
        repl_flatten_commands(editor_state_edit_line());
        ASSERT_TRUE("top-level if (true cond) body in flat",
                    repl_state_flat_program_count() == 1);
        ASSERT_TRUE("top-level if (true cond) body type",
                    repl_state_flat_program_cmds_mut()[0].type == CMD_COLOR3F);
    }

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("func0 {");
    editor_feed_line("glVertex3f(1, 2, 3);");
    editor_feed_line("}");
    editor_feed_line("func0()");
    ASSERT_TRUE("func cmd count", repl_state_document_count() == 4);
    ASSERT_TRUE("func def", repl_state_document_cmds_mut()[0].type == CMD_FUNC_DEF);
    ASSERT_TRUE("func body", repl_state_document_cmds_mut()[1].type == CMD_VERTEX3F);
    ASSERT_TRUE("func end", repl_state_document_cmds_mut()[2].type == CMD_FUNC_END);
    ASSERT_TRUE("func call", repl_state_document_cmds_mut()[3].type == CMD_CALL);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("func0(radius, yoff) {");
    editor_feed_line("glVertex3f(radius, yoff, 0);");
    editor_feed_line("}");
    editor_feed_line("func0(1.5, x + 2);");
    ASSERT_TRUE("param func cmd count", repl_state_document_count() == 4);
    ASSERT_TRUE("param func def", repl_state_document_cmds_mut()[0].type == CMD_FUNC_DEF);
    ASSERT_TRUE("param func header keeps names",
                strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "", "radius") != NULL &&
                strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "", "yoff") != NULL);
    ASSERT_TRUE("param func body keeps radius",
                strstr(editor_buffer_line(1) ? editor_buffer_line(1) : "", "radius") != NULL);
    ASSERT_TRUE("param func body keeps yoff",
                strstr(editor_buffer_line(1) ? editor_buffer_line(1) : "", "yoff") != NULL);
    ASSERT_TRUE("param func call type", repl_state_document_cmds_mut()[3].type == CMD_CALL);
    ASSERT_TRUE("param func call keeps expr",
                strstr(editor_buffer_line(3) ? editor_buffer_line(3) : "", "x + 2") != NULL);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glClearColor(0.1, 0.1, 0.1, 1);");
    editor_feed_line("glEnable(GL_DEPTH_TEST);");
    editor_feed_line("func0 {");
    editor_feed_line("glVertex3f(1, 2, 3);");
    editor_feed_line("}");
    editor_feed_line("func0();");
    ASSERT_TRUE("promoted func keeps cmd count", repl_state_document_count() == 6);
    ASSERT_TRUE("promoted func def before commands", repl_state_document_cmds_mut()[0].type == CMD_FUNC_DEF);
    ASSERT_TRUE("promoted func body before commands", repl_state_document_cmds_mut()[1].type == CMD_VERTEX3F);
    ASSERT_TRUE("promoted func end before commands", repl_state_document_cmds_mut()[2].type == CMD_FUNC_END);
    ASSERT_TRUE("promoted prior clear command preserved", repl_state_document_cmds_mut()[3].type == CMD_CLEAR_COLOR);
    ASSERT_TRUE("promoted prior enable command preserved", repl_state_document_cmds_mut()[4].type == CMD_ENABLE);
    ASSERT_TRUE("promoted following call appends after prior commands", repl_state_document_cmds_mut()[5].type == CMD_CALL);

    glr_app_reset_all(); declare_test_vars();
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
        editor_state_edit_line_set(8);
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            matched += repl_flat_cmd_matches_cursor(i, editor_state_edit_line());
        ASSERT_TRUE("nested func call line highlights one invocation", matched == 3);
    }
    {
        int matched = 0;
        editor_state_edit_line_set(6);
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            matched += repl_flat_cmd_matches_cursor(i, editor_state_edit_line());
        ASSERT_TRUE("nested inner call line highlights all invocations", matched == 6);
    }
    {
        int matched = 0;
        editor_state_edit_line_set(2);
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            matched += repl_flat_cmd_matches_cursor(i, editor_state_edit_line());
        ASSERT_TRUE("nested function body highlights all invocations", matched == 6);
    }
    {
        int matched = 0;
        editor_state_edit_line_set(5);
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            matched += repl_flat_cmd_matches_cursor(i, editor_state_edit_line());
        ASSERT_TRUE("outer function header highlights nested invocations", matched == 6);
    }

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("func0(n) {");
    editor_feed_line("for(i, 0, n) {");
    editor_feed_line("glVertex3f(i, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("}");
    ASSERT_TRUE("nested block trailing cmd count", repl_state_document_count() == 6);
    ASSERT_TRUE("nested block trailing cmd order body", repl_state_document_cmds_mut()[4].type == CMD_COLOR3F);
    ASSERT_TRUE("nested block trailing cmd order end", repl_state_document_cmds_mut()[5].type == CMD_FUNC_END);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("func0(n) {");
    editor_feed_line("for(i, 0, n) glVertex3f(i, n, 0);");
    editor_feed_line("}");
    editor_feed_line("func0(3);");
    ASSERT_TRUE("local for begin type", repl_state_document_cmds_mut()[1].type == CMD_FOR_BEGIN);
    ASSERT_TRUE("local for body type", repl_state_document_cmds_mut()[2].type == CMD_VERTEX3F);
    ASSERT_TRUE("local for end type", repl_state_document_cmds_mut()[3].type == CMD_FOR_END);
    ASSERT_TRUE("local for header keeps n", strstr(editor_buffer_line(1) ? editor_buffer_line(1) : "", "n") != NULL);
    ASSERT_TRUE("local for body keeps n", strstr(editor_buffer_line(2) ? editor_buffer_line(2) : "", "n") != NULL);
    repl_flatten_commands(editor_state_edit_line());
    ASSERT_TRUE("local for flatten count", repl_state_flat_program_count() == 3);
    ASSERT_TRUE("local for first x", fabsf(repl_state_flat_program_cmds_mut()[0].args[0] - 0.0f) < 1e-6f);
    ASSERT_TRUE("local for second x", fabsf(repl_state_flat_program_cmds_mut()[1].args[0] - 1.0f) < 1e-6f);
    ASSERT_TRUE("local for third x", fabsf(repl_state_flat_program_cmds_mut()[2].args[0] - 2.0f) < 1e-6f);
    ASSERT_TRUE("local for body uses param", fabsf(repl_state_flat_program_cmds_mut()[2].args[1] - 3.0f) < 1e-6f);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("// top-level note");
    editor_feed_line("for(i, 0, 3) {");
    editor_feed_line("// loop note");
    editor_feed_line("glVertex3f(i, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("// trailing note");
    ASSERT_TRUE("comments remain source rows",
                repl_state_document_count() == 6 &&
                repl_state_document_cmds_mut()[0].type == CMD_COMMENT &&
                repl_state_document_cmds_mut()[2].type == CMD_COMMENT &&
                repl_state_document_cmds_mut()[5].type == CMD_COMMENT);
    repl_flatten_commands(editor_state_edit_line());
    ASSERT_TRUE("comments omitted from flattened loop count",
                repl_state_flat_program_count() == 3);
    for (int flat_idx = 0; flat_idx < repl_state_flat_program_count(); flat_idx++) {
        ASSERT_TRUE("flattened loop command is executable vertex",
                    repl_state_flat_program_cmds_mut()[flat_idx].type == CMD_VERTEX3F);
        ASSERT_TRUE("flattened loop command maps to source body",
                    repl_state_flat_program_cmds_mut()[flat_idx].src_cmd_idx == 3);
    }

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("func0(scale) {");
    editor_feed_line("if(scale > 1) {");
    editor_feed_line("glVertex3f(scale, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("}");
    editor_feed_line("func0(2);");
    editor_feed_line("func0(0.5);");
    ASSERT_TRUE("local if header keeps scale", strstr(editor_buffer_line(1) ? editor_buffer_line(1) : "", "scale > 1") != NULL);
    repl_flatten_commands(editor_state_edit_line());
    ASSERT_TRUE("local if flatten count", repl_state_flat_program_count() == 1);
    ASSERT_TRUE("local if flatten type", repl_state_flat_program_cmds_mut()[0].type == CMD_VERTEX3F);
    ASSERT_TRUE("local if flatten x", fabsf(repl_state_flat_program_cmds_mut()[0].args[0] - 2.0f) < 1e-6f);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("func0(cols) {");
    editor_feed_line("for(j, 0, cols + 1) {");
    editor_feed_line("x = -1.5 + 3.0*j/cols;");
    editor_feed_line("glVertex3f(x, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("}");
    editor_feed_line("func0(4);");
    repl_flatten_commands(editor_state_edit_line());
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

    glr_app_reset_all(); declare_test_vars();
    {
        char display[MAX_INPUT_LEN];
        int i_idx = predef_idx("i");
        int j_idx = predef_idx("j");

        ASSERT_TRUE("replay display i predef exists", i_idx >= 0);
        ASSERT_TRUE("replay display j predef exists", j_idx >= 0);
        if (i_idx >= 0) g_predef_vars[i_idx].value = 3.2f;
        if (j_idx >= 0) g_predef_vars[j_idx].value = 1.2f;

        editor_feed_line("i = i + j + 3;");
        editor_feed_line("glVertex3f(i, j, 0);");
        if (i_idx >= 0) g_predef_vars[i_idx].value = 3.2f;
        if (j_idx >= 0) g_predef_vars[j_idx].value = 1.2f;
        replay_start();
        replay_state = REPLAY_PAUSED;

        replay_pc = 1;
        replay_src_line = 0;
        ASSERT_TRUE("replay display assignment text",
                    replay_code_panel_get_command_display_text(source_document_view(), 0, display, sizeof(display)));
        ASSERT_STR("replay display assignment inline comment",
                   display,
                   "  i = i + j + 3; // i = 3.2 + 1.2 + 3 = 7.4");

        replay_pc = 2;
        replay_src_line = 1;
        ASSERT_TRUE("replay display prior assignment still visible",
                    replay_code_panel_get_command_display_text(source_document_view(), 0, display, sizeof(display)));
        ASSERT_STR("replay display prior assignment inline comment",
                   display,
                   "  i = i + j + 3; // i = 3.2 + 1.2 + 3 = 7.4");
        ASSERT_TRUE("replay display vertex text",
                    replay_code_panel_get_command_display_text(source_document_view(), 1, display, sizeof(display)));
        ASSERT_STR("replay display vertex source unchanged",
                   display,
                   "  glVertex3f(i, j, 0);");

        replay_active = 0;
        replay_state = REPLAY_OFF;
        replay_src_line = -1;
        replay_pc = 0;
    }

    glr_app_reset_all(); declare_test_vars();
    {
        char display[MAX_INPUT_LEN];
        const UiVirtualLineList *vlines;

        editor_feed_line("A[0] = 1;");
        editor_feed_line("A[0] = A[0] + 3;");
        editor_feed_line("glVertex3f(A[0], 0, 0);");
        replay_start();
        replay_state = REPLAY_PAUSED;

        replay_pc = 2;
        replay_src_line = 1;
        ASSERT_TRUE("scratch replay assignment text",
                    replay_code_panel_get_command_display_text(source_document_view(), 1, display, sizeof(display)));
        ASSERT_STR("scratch replay assignment inline comment",
                   display,
                   "  A[0] = A[0] + 3; // A[0] = 1 + 3 = 4");

        replay_pc = 3;
        replay_src_line = 2;
        {
            ReplReplayAnnotationOutput annotations;
            replay_annotations_prepare(source_document_view(),
                                            &annotations);
            glr_publish_replay_annotations(&annotations);
        }
        vlines = editor_state_virtual_lines();
        ASSERT_TRUE("scratch replay vertex annotation rows exist",
                    vlines != NULL && vlines->count >= 1);
        if (vlines && vlines->count >= 1) {
            const UiVirtualLine *eval_line = &vlines->items[vlines->count - 1];
            ASSERT_TRUE("scratch replay eval row style",
                        eval_line->style == VIRTUAL_STYLE_REPLAY_EVAL);
            ASSERT_STR("scratch replay vertex eval text",
                       eval_line->text,
                       "  glVertex3f(4, 0, 0);");
        }

        replay_active = 0;
        replay_state = REPLAY_OFF;
        replay_src_line = -1;
        replay_pc = 0;
    }

    glr_app_reset_all(); declare_test_vars();
    {
        char display[MAX_INPUT_LEN];
        int i_idx = predef_idx("i");
        int k_idx = predef_idx("k");

        ASSERT_TRUE("replay chain i predef exists", i_idx >= 0);
        ASSERT_TRUE("replay chain k predef exists", k_idx >= 0);
        if (i_idx >= 0) g_predef_vars[i_idx].value = 0.23f;
        if (k_idx >= 0) g_predef_vars[k_idx].value = 0.5f;

        editor_feed_line("i = i + k;");
        if (i_idx >= 0) g_predef_vars[i_idx].value = 0.23f;
        if (k_idx >= 0) g_predef_vars[k_idx].value = 0.5f;
        replay_start();
        replay_state = REPLAY_PAUSED;

        replay_pc = 1;
        replay_src_line = 0;
        ASSERT_TRUE("replay chain assignment text",
                    replay_code_panel_get_command_display_text(source_document_view(), 0, display, sizeof(display)));
        ASSERT_STR("replay chain assignment inline comment",
                   display,
                   "  i = i + k; // i = 0.23 + 0.5 = 0.73");

        replay_active = 0;
        replay_state = REPLAY_OFF;
        replay_src_line = -1;
        replay_pc = 0;
    }

    glr_app_reset_all(); declare_test_vars();
    {
        char display[MAX_INPUT_LEN];
        int i_idx = predef_idx("i");
        int x_idx = predef_idx("x");

        ASSERT_TRUE("replay goto i predef exists", i_idx >= 0);
        ASSERT_TRUE("replay goto x predef exists", x_idx >= 0);

        editor_feed_line("i = 1;");
        editor_feed_line("goto after;");
        editor_feed_line("i = 100;");
        editor_feed_line("x = i + 1;");
        editor_feed_line(":after");
        editor_feed_line("glVertex3f(0, 0, 0);");
        replay_start();
        replay_state = REPLAY_PAUSED;

        replay_pc = repl_state_flat_program_count();
        replay_src_line = 5;
        ASSERT_TRUE("replay goto skipped assignment text",
                    replay_code_panel_get_command_display_text(source_document_view(), 3, display, sizeof(display)));
        ASSERT_TRUE("replay goto skipped assignment uses pre-jump value",
                    strstr(display, "// x = 1 + 1 = 2") != NULL);
        ASSERT_TRUE("replay goto skipped assignment ignores skipped overwrite",
                    strstr(display, "100 + 1") == NULL);

        replay_active = 0;
        replay_state = REPLAY_OFF;
        replay_src_line = -1;
        replay_pc = 0;
    }

    glr_app_reset_all(); declare_test_vars();
    {
        char display[MAX_INPUT_LEN];
        int i_idx = predef_idx("i");
        int j_idx = predef_idx("j");

        ASSERT_TRUE("replay scientific i predef exists", i_idx >= 0);
        ASSERT_TRUE("replay scientific j predef exists", j_idx >= 0);
        if (j_idx >= 0) g_predef_vars[j_idx].value = 1.0f;

        editor_feed_line("for(e, 0, 1) {");
        editor_feed_line("i = j * 1e-06;");
        editor_feed_line("}");
        if (j_idx >= 0) g_predef_vars[j_idx].value = 1.0f;
        replay_start();
        replay_state = REPLAY_PAUSED;

        replay_pc = 1;
        replay_src_line = 1;
        ASSERT_TRUE("replay scientific assignment text",
                    replay_code_panel_get_command_display_text(source_document_view(), 1, display, sizeof(display)));
        ASSERT_TRUE("replay scientific inline comment keeps expanded rhs",
                    strstr(display, "// i = 1 * 1e-06 = 1e-06") != NULL);

        replay_active = 0;
        replay_state = REPLAY_OFF;
        replay_src_line = -1;
        replay_pc = 0;
    }

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glEnd();");
    editor_feed_line("glColor3f(0, 1, 0);");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glVertex3f(1, 1, 0);");
    editor_feed_line("glEnd();");
    repl_flatten_commands(editor_state_edit_line());
    {
        int matched = 0;
        editor_state_edit_line_set(0);
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            if (repl_state_flat_program_cmds_mut()[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i, editor_state_edit_line());
        ASSERT_TRUE("top-level color before block matches first vertices", matched == 2);
    }
    {
        int matched = 0;
        editor_state_edit_line_set(5);
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            if (repl_state_flat_program_cmds_mut()[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i, editor_state_edit_line());
        ASSERT_TRUE("top-level second color before block matches second vertices", matched == 2);
    }
    ASSERT_TRUE("feeding color for first block vertex", repl_find_feeding_color_cmd(2) == 0);
    ASSERT_TRUE("feeding color for second block vertex", repl_find_feeding_color_cmd(7) == 5);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glNormal3f(0, 0, 1);");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glEnd();");
    editor_feed_line("glNormal3f(0, 1, 0);");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glVertex3f(1, 1, 0);");
    editor_feed_line("glEnd();");
    repl_flatten_commands(editor_state_edit_line());
    {
        int matched = 0;
        editor_state_edit_line_set(0);
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            if (repl_state_flat_program_cmds_mut()[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i, editor_state_edit_line());
        ASSERT_TRUE("top-level normal before block matches first vertices", matched == 2);
    }
    {
        int matched = 0;
        editor_state_edit_line_set(5);
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            if (repl_state_flat_program_cmds_mut()[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i, editor_state_edit_line());
        ASSERT_TRUE("top-level second normal before block matches second vertices", matched == 2);
    }
    ASSERT_TRUE("feeding normal for first block vertex", repl_find_feeding_normal_cmd(2) == 0);
    ASSERT_TRUE("feeding normal for second block vertex", repl_find_feeding_normal_cmd(7) == 5);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("gluColor(1, 0, 0);");
    editor_feed_line("gluVertex(0, 0, 0);");
    ASSERT_TRUE("tess color feeds tess vertex", repl_find_feeding_color_cmd(1) == 0);
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("gluVertex(0, 0, 0);");
    ASSERT_TRUE("no tess color → -1", repl_find_feeding_color_cmd(0) == -1);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("gluNormal(0, 0, 1);");
    editor_feed_line("gluVertex(0, 0, 0);");
    ASSERT_TRUE("tess normal feeds tess vertex", repl_find_feeding_normal_cmd(1) == 0);
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("gluVertex(0, 0, 0);");
    ASSERT_TRUE("no tess normal → -1", repl_find_feeding_normal_cmd(0) == -1);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("glNormal3f(0, 0, 1);");
    ASSERT_TRUE("non-vertex color → -1", repl_find_feeding_color_cmd(0) == -1);
    ASSERT_TRUE("non-vertex normal → -1", repl_find_feeding_normal_cmd(1) == -1);
    ASSERT_TRUE("oob color → -1", repl_find_feeding_color_cmd(-1) == -1);
    ASSERT_TRUE("oob normal → -1", repl_find_feeding_normal_cmd(99) == -1);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glEnd();");
    repl_flatten_commands(editor_state_edit_line());
    editor_navigate_to_line(1);
    {
        int matched = 0;
        for (int i = 0; i < repl_state_flat_program_count(); i++)
            if (repl_state_flat_program_cmds_mut()[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i, editor_state_edit_line());
        ASSERT_TRUE("navigate refreshes current block highlight", matched == 2);
    }

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("func0(depth) {");
    editor_feed_line("if(depth <= 0) {");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("if(depth > 0) {");
    editor_feed_line("glVertex3f(depth, 0, 0);");
    editor_feed_line("func0(depth - 1);");
    editor_feed_line("}");
    editor_feed_line("}");
    editor_feed_line("func0(3);");
    repl_flatten_commands(editor_state_edit_line());
    ASSERT_TRUE("recursive flatten count", repl_state_flat_program_count() == 4);
    ASSERT_TRUE("recursive first x", fabsf(repl_state_flat_program_cmds_mut()[0].args[0] - 3.0f) < 1e-6f);
    ASSERT_TRUE("recursive second x", fabsf(repl_state_flat_program_cmds_mut()[1].args[0] - 2.0f) < 1e-6f);
    ASSERT_TRUE("recursive third x", fabsf(repl_state_flat_program_cmds_mut()[2].args[0] - 1.0f) < 1e-6f);
    ASSERT_TRUE("recursive base x", fabsf(repl_state_flat_program_cmds_mut()[3].args[0] - 0.0f) < 1e-6f);

    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("func0(n) {");
    editor_feed_line("if(n <= 0) {");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("if(n > 0) {");
    editor_feed_line("glVertex3f(n, 0, 0);");
    editor_feed_line("func1(n - 1);");
    editor_feed_line("}");
    editor_feed_line("}");
    editor_feed_line("func1(n) {");
    editor_feed_line("if(n <= 0) {");
    editor_feed_line("glVertex3f(-10, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("if(n > 0) {");
    editor_feed_line("glVertex3f(-n, 0, 0);");
    editor_feed_line("func0(n - 1);");
    editor_feed_line("}");
    editor_feed_line("}");
    editor_feed_line("func0(2);");
    repl_flatten_commands(editor_state_edit_line());
    ASSERT_TRUE("mutual recursion flatten count", repl_state_flat_program_count() == 3);
    ASSERT_TRUE("mutual recursion first x", fabsf(repl_state_flat_program_cmds_mut()[0].args[0] - 2.0f) < 1e-6f);
    ASSERT_TRUE("mutual recursion second x", fabsf(repl_state_flat_program_cmds_mut()[1].args[0] - (-1.0f)) < 1e-6f);
    ASSERT_TRUE("mutual recursion base x", fabsf(repl_state_flat_program_cmds_mut()[2].args[0] - 0.0f) < 1e-6f);

    glr_app_reset_all(); declare_test_vars();
    g_status[0] = '\0';
    editor_feed_line("func0(n) {");
    editor_feed_line("func0(n + 1);");
    editor_feed_line("}");
    editor_feed_line("func0(0);");
    repl_flatten_commands(editor_state_edit_line());
    ASSERT_TRUE("runaway recursion emits no flat cmds", repl_state_flat_program_count() == 0);
    ASSERT_TRUE("runaway recursion depth guard",
                strstr(g_status, "depth limit") != NULL ||
                strstr(g_status, "visit budget") != NULL);

    /* Regression: a second definition of func<N> is rejected with a
     * status message instead of silently creating a duplicate that the
     * flattener would ignore. */
    glr_app_reset_all(); declare_test_vars();
    g_status[0] = '\0';
    editor_feed_line("func0 {");
    editor_feed_line("glVertex3f(1, 2, 3);");
    editor_feed_line("}");
    int dupe_cmd_count_before = repl_state_document_count();
    editor_feed_line("func0(y) {");
    ASSERT_TRUE("duplicate func def rejected: cmd count unchanged",
                repl_state_document_count() == dupe_cmd_count_before);
    ASSERT_TRUE("duplicate func def rejected: status names func0",
                strstr(g_status, "func0 already defined") != NULL);

    /* Cursor on the existing def: "overwrite" path, still allowed. */
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("func0 {");
    editor_feed_line("glVertex3f(1, 2, 3);");
    editor_feed_line("}");
    editor_state_edit_line_set(0);
    editor_insert_mode_set(0);
    g_status[0] = '\0';
    editor_feed_line("func0(z) {");
    ASSERT_TRUE("func def overwrite: still CMD_FUNC_DEF",
                repl_state_document_cmds_mut()[0].type == CMD_FUNC_DEF);
    ASSERT_TRUE("func def overwrite: header keeps new param",
                strstr(editor_buffer_line(0) ? editor_buffer_line(0) : "", "z") != NULL);
    ASSERT_TRUE("func def overwrite: status not a duplicate rejection",
                strstr(g_status, "already defined") == NULL);

    /* Regression: calling func<N> with no definition is rejected at
     * commit time, the same way `x = 1;` is rejected before `float x;`
     * is declared.  Call is never added to repl_state_document_cmds_mut() and status names the
     * function. */
    glr_app_reset_all(); declare_test_vars();
    g_status[0] = '\0';
    int pre_call_cmd_count = repl_state_document_count();
    editor_feed_line("func5();");
    ASSERT_TRUE("undefined top-level call: not added to repl_state_document_cmds_mut()",
                repl_state_document_count() == pre_call_cmd_count);
    ASSERT_TRUE("undefined top-level call: status names func5",
                strstr(g_status, "func5") != NULL);
    ASSERT_TRUE("undefined top-level call: status says undefined",
                strstr(g_status, "undefined function") != NULL);

    /* Same call accepted once the definition exists. */
    editor_feed_line("func5 {");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("}");
    int pre_retry_cmd_count = repl_state_document_count();
    g_status[0] = '\0';
    editor_feed_line("func5();");
    ASSERT_TRUE("defined top-level call: commit adds a CMD_CALL",
                repl_state_document_count() == pre_retry_cmd_count + 1);
    ASSERT_TRUE("defined top-level call: last cmd is CMD_CALL",
                repl_state_document_cmds_mut()[repl_state_document_count() - 1].type == CMD_CALL);
    repl_flatten_commands(editor_state_edit_line());
    ASSERT_TRUE("defined top-level call: flat body expands",
                repl_state_flat_program_count() == 1 && repl_state_flat_program_cmds_mut()[0].type == CMD_VERTEX3F);

    /* Forward references inside a function body are still allowed so
     * mutual recursion keeps working.  func0's body calls func1 before
     * func1 is defined; top-level `func0(2)` only commits after both
     * defs exist. */
    glr_app_reset_all(); declare_test_vars();
    editor_feed_line("func0(n) {");
    editor_feed_line("if(n > 0) {");
    editor_feed_line("glVertex3f(n, 0, 0);");
    editor_feed_line("func1(n - 1);");   /* forward ref inside body */
    editor_feed_line("}");
    editor_feed_line("}");
    editor_feed_line("func1(n) {");
    editor_feed_line("if(n > 0) {");
    editor_feed_line("glVertex3f(-n, 0, 0);");
    editor_feed_line("func0(n - 1);");
    editor_feed_line("}");
    editor_feed_line("}");
    int pre_mutual_call_count = repl_state_document_count();
    g_status[0] = '\0';
    editor_feed_line("func0(2);");
    ASSERT_TRUE("mutual recursion: top-level call accepted",
                repl_state_document_count() == pre_mutual_call_count + 1 &&
                repl_state_document_cmds_mut()[repl_state_document_count() - 1].type == CMD_CALL);
    repl_flatten_commands(editor_state_edit_line());
    ASSERT_TRUE("mutual recursion: flatten reaches base case",
                repl_state_flat_program_count() > 0);

    /* Regression: commands marked CMD_TYPE_SPEC_NOT_IN_BEGIN in
     * src/repl/command_spec.c must be rejected at commit time when
     * source-scope says the cursor sits inside a glBegin/glEnd block.
     * Representative CMD_TYPE_SPEC commands (vertex/normal/color/etc.)
     * must still commit. Before this fix the executor defensively
     * glEnd()ed the begin block for not-in-begin commands and never
     * reopened it, silently dropping every subsequent glVertex inside
     * the same flat-stream iteration of a for-loop. The parser is now
     * the gate. */
    {
        static const struct {
            const char *line;
            const char *name_substr;
        } not_in_begin_cases[] = {
            { "glBegin(GL_LINES);",                                          "glBegin" },
            { "glPointSize(2);",                                             "glPointSize" },
            { "glLineWidth(2);",                                             "glLineWidth" },
            { "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 1, 0, 0);", "glPointParameterfv" },
            { "glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);",          "glBlendFunc" },
            { "glClearColor(0, 0, 0, 1);",                                   "glClearColor" },
            { "glDepthMask(GL_TRUE);",                                       "glDepthMask" },
            { "glutSolidTorus(0.1, 0.5, 8, 8);",                             "glutSolidTorus" },
            { "glutSolidCube(1);",                                           "glutSolidCube" },
            { "glutSolidSphere(1, 8, 8);",                                   "glutSolidSphere" },
            { "glutSolidTeapot(1);",                                         "glutSolidTeapot" },
            { "glutSolidCone(1, 1, 8, 8);",                                  "glutSolidCone" },
            { "glRasterPos3f(0, 0, 0);",                                     "glRasterPos3f" },
            { "label(\"hi\");",                                              "label" },
            { "gluBegin(GLU_POLYGON);",                                      "gluTessBeginPolygon" },
        };
        int case_count = (int)(sizeof(not_in_begin_cases) / sizeof(not_in_begin_cases[0]));
        for (int i = 0; i < case_count; i++) {
            glr_app_reset_all(); declare_test_vars();
            editor_feed_line("glBegin(GL_TRIANGLES);");
            int before = repl_state_document_count();
            g_status[0] = '\0';
            editor_feed_line(not_in_begin_cases[i].line);
            char label[160];

            snprintf(label, sizeof(label),
                     "not-in-begin %s: line rejected, doc count unchanged",
                     not_in_begin_cases[i].name_substr);
            ASSERT_TRUE(label, repl_state_document_count() == before);

            snprintf(label, sizeof(label),
                     "not-in-begin %s: status names the command",
                     not_in_begin_cases[i].name_substr);
            ASSERT_TRUE(label,
                strstr(g_status, not_in_begin_cases[i].name_substr) != NULL);

            snprintf(label, sizeof(label),
                     "not-in-begin %s: status mentions glBegin/glEnd",
                     not_in_begin_cases[i].name_substr);
            ASSERT_TRUE(label, strstr(g_status, "glBegin/glEnd") != NULL);
        }
    }

    /* And the inverse: representative CMD_TYPE_SPEC commands (the
     * standard "real GL would actually accept" set plus a few that the
     * REPL just doesn't bother rejecting) still commit cleanly inside
     * a glBegin block. */
    {
        static const struct {
            const char *line;
            const char *name;
            CmdType     expected_type;
        } in_begin_cases[] = {
            { "glVertex3f(0, 0, 0);",                       "glVertex3f",   CMD_VERTEX3F },
            { "glVertex2f(0, 0);",                          "glVertex2f",   CMD_VERTEX2F },
            { "glNormal3f(0, 0, 1);",                       "glNormal3f",   CMD_NORMAL3F },
            { "glColor3f(1, 0, 0);",                        "glColor3f",    CMD_COLOR3F },
            { "glColor4f(1, 0, 0, 1);",                     "glColor4f",    CMD_COLOR4F },
            { "glMaterialfv(GL_FRONT, GL_SHININESS, 32);",  "glMaterialfv", CMD_MATERIALFV },
        };
        int case_count = (int)(sizeof(in_begin_cases) / sizeof(in_begin_cases[0]));
        for (int i = 0; i < case_count; i++) {
            glr_app_reset_all(); declare_test_vars();
            editor_feed_line("glBegin(GL_TRIANGLES);");
            int before = repl_state_document_count();
            g_status[0] = '\0';
            editor_feed_line(in_begin_cases[i].line);
            char label[160];

            snprintf(label, sizeof(label),
                     "in-begin %s: doc grew by one",
                     in_begin_cases[i].name);
            ASSERT_TRUE(label, repl_state_document_count() == before + 1);

            snprintf(label, sizeof(label),
                     "in-begin %s: committed type matches",
                     in_begin_cases[i].name);
            ASSERT_TRUE(label,
                repl_state_document_cmds_mut()[before].type
                    == in_begin_cases[i].expected_type);

            snprintf(label, sizeof(label),
                     "in-begin %s: no begin-scope rejection in status",
                     in_begin_cases[i].name);
            ASSERT_TRUE(label,
                strstr(g_status, "not valid inside glBegin") == NULL);
        }
    }

    /* Regression: pasting into an Enter-created *virtual* blank line
     * (insert mode, no doc mutation) must not leave the input buffer
     * pre-loaded with the *following* line's text while still in
     * insert mode. That made the code panel render a phantom copy of
     * the following line and materialized a real duplicate on the next
     * commit. (clipboard.c paste tail; saved_insert restore.) */
    {
        glr_app_reset_all(); declare_test_vars();
        editor_feed_line("glEnable(GL_BLEND);");
        editor_feed_line("glutSolidSphere(1, 20, 20);");
        editor_feed_line("glDepthMask(GL_TRUE);");
        ASSERT_TRUE("paste-vblank: initial doc count",
                    repl_state_document_count() == 3);

        /* Copy line 0 (glEnable) onto the line clipboard. */
        editor_navigate_to_line(0);
        editor_clipboard_copy_current();

        /* Navigate onto glutSolidSphere (line 1); Enter opens a
         * virtual blank below it (insert mode, no doc mutation). */
        editor_navigate_to_line(1);
        editor_handle_key('\r', 0, 0);
        ASSERT_TRUE("paste-vblank: Enter sets insert mode",
                    editor_insert_mode());
        ASSERT_TRUE("paste-vblank: Enter did not mutate doc",
                    repl_state_document_count() == 3);

        /* Paste the clipboard line into the virtual blank. */
        editor_clipboard_paste_current();

        ASSERT_TRUE("paste-vblank: exactly one line inserted",
                    repl_state_document_count() == 4);
        ASSERT_TRUE("paste-vblank: glDepthMask not duplicated in doc",
                    repl_state_document_cmds_mut()[3].type == CMD_DEPTH_MASK &&
                    repl_state_document_cmds_mut()[2].type != CMD_DEPTH_MASK);
        /* Core fix: if paste left us in insert mode, the input buffer
         * must be empty — not pre-loaded with glDepthMask's text. */
        if (editor_insert_mode())
            ASSERT_TRUE("paste-vblank: insert-mode input buffer is clean",
                        editor_state_input().input_len == 0);

        /* The user's next keystroke is a commit. Pre-fix this
         * materialized the staged glDepthMask into a real duplicate. */
        editor_handle_key('\r', 0, 0);
        ASSERT_TRUE("paste-vblank: commit after paste adds no duplicate",
                    repl_state_document_count() == 4);
    }

    printf("repl_core_commit: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.run == g_harness.passed) ? 0 : 1;
}
