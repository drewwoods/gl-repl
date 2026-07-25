#include "app/glr_ctrl.h"
#include "repl/command.h"
#include "repl/eval.h"
#include "repl/normalize.h"
#include "repl/text_helpers.h"
#include "editor/input.h"
#include "repl/parser.h"
#include "repl/flatten.h"
#include "repl/state_views.h"
#include "ui/app/state.h"
#include "support/repl_test_support.h"
#include "support/test_harness.h"

#define g_status (ui_state_status_mut()->text)

#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include "repl/host_effects.h"
#include <stdlib.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

static int leading_spaces(const char *s) {
    int n = 0;
    if (!s) return 0;
    while (s[n] == ' ') n++;
    return n;
}

/* Helper: parse line with default context, output cmd + canonical text. */
static int parse_cmd_with_text(const char *line, GLCmd *cmd,
                                char *text_out, int text_sz) {
    ReplParseContext ctx = { 0, NULL, 0, 0 };
    ReplParsedLine pl;
    int ok = repl_parser_parse_command_ctx(line, &pl, &ctx);
    if (cmd) *cmd = pl.cmd;
    if (text_out && text_sz > 0) {
        strncpy(text_out, pl.text, (size_t)(text_sz - 1));
        text_out[text_sz - 1] = '\0';
    }
    return ok;
}

/* Test-local replacements for the deleted no-ctx parser wrappers.
 * The wrappers used to surface parser diagnostics through set_status
 * so the test harness could assert against g_status; preserve that
 * by writing the err_buf into the status text on failure. The
 * production parser is set_status-free (check-no-set-status-in-
 * repl-parser baseline 0). */
static int parse_for_test(const char *line, GLCmd *cmd) {
    char err_buf[REPL_STATUS_TEXT_MAX];
    err_buf[0] = '\0';
    ReplParseContext ctx = {
        .source_line_idx = editor_state_edit_line(),
        .err_buf = err_buf,
        .err_sz  = (int)sizeof(err_buf),
    };
    ReplParsedLine pl;
    int ok = repl_parser_parse_command_ctx(line, &pl, &ctx);
    if (cmd) *cmd = pl.cmd;
    if (!ok && err_buf[0]) repl_set_status(err_buf);
    return ok;
}

static int parse_for_test_with_vars(const char *line, GLCmd *cmd,
                                    ExprVar *vars, int num_vars) {
    char err_buf[REPL_STATUS_TEXT_MAX];
    err_buf[0] = '\0';
    ReplParseContext ctx = {
        .source_line_idx = editor_state_edit_line(),
        .vars = vars, .num_vars = num_vars,
        .err_buf = err_buf,
        .err_sz  = (int)sizeof(err_buf),
    };
    ReplParsedLine pl;
    int ok = repl_parser_parse_command_ctx(line, &pl, &ctx);
    if (cmd) *cmd = pl.cmd;
    if (!ok && err_buf[0]) repl_set_status(err_buf);
    return ok;
}

static void test_flat_stencil_clear_predicate(void) {
    GLCmd cmds[3];

    memset(cmds, 0, sizeof(cmds));
    cmds[0].type = CMD_CLEAR;
    cmds[0].valid = 1;
    cmds[0].args[0] = (float)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    ASSERT_TRUE("flat clear predicate ignores color/depth", !repl_flat_clears_stencil(cmds, 1));

    cmds[1].type = CMD_CLEAR;
    cmds[1].valid = 0; /* Deleted source rows must not suppress the warning. */
    cmds[1].args[0] = (float)GL_STENCIL_BUFFER_BIT;
    ASSERT_TRUE("flat clear predicate ignores invalid rows", !repl_flat_clears_stencil(cmds, 2));

    cmds[2].type = CMD_CLEAR;
    cmds[2].valid = 1;
    cmds[2].args[0] = (float)(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    ASSERT_TRUE("flat clear predicate finds stencil bit", repl_flat_clears_stencil(cmds, 3));
}

static void declare_test_vars(void) {
    char err[128];
    static const char *const names[] = { "x", "y", "z" };

    ASSERT_TRUE("declare parser vars",
                repl_test_declare_predef_vars(names, 3, err, sizeof(err)));
}

static void assert_status_contains(const char *label, const char *needle) {
    ASSERT_TRUE(label, strstr(g_status, needle) != NULL);
}

int main(void) {
    repl_eval_init_predef_vars();
    glr_ctrl_reset_all();
    declare_test_vars();
    test_flat_stencil_clear_predicate();

    {
        char payload[128];
        memset(payload, 0, sizeof(payload));
        ASSERT_TRUE("extract nested paren payload",
                    repl_extract_paren_payload("  if(max(x, y + 1) > sin(z)) {",
                                               payload, sizeof(payload)) == 1);
        ASSERT_TRUE("extract nested paren payload text",
                    strcmp(payload, "max(x, y + 1) > sin(z)") == 0);
        ASSERT_TRUE("extract paren payload rejects missing close",
                    repl_extract_paren_payload("glColor3f(1, 2, 3", payload,
                                               sizeof(payload)) == 0);
    }

    {
        char name[32];
        char index_expr[128];
        char rhs[128];
        memset(name, 0, sizeof(name));
        memset(index_expr, 0, sizeof(index_expr));
        memset(rhs, 0, sizeof(rhs));
        ASSERT_TRUE("extract assignment parts",
                    repl_extract_assignment_parts("  radius = sin(x + 1);  ",
                                                  name, sizeof(name),
                                                  rhs, sizeof(rhs)) == 1);
        ASSERT_TRUE("assignment name", strcmp(name, "radius") == 0);
        ASSERT_TRUE("assignment rhs", strcmp(rhs, "sin(x + 1)") == 0);
        ASSERT_TRUE("assignment rejects equality",
                    repl_extract_assignment_parts("x == 1", name, sizeof(name),
                                                  rhs, sizeof(rhs)) == 0);
        ASSERT_TRUE("assignment rejects empty rhs",
                    repl_extract_assignment_parts("radius =", name, sizeof(name),
                                                  rhs, sizeof(rhs)) == 0);
        ASSERT_TRUE("assignment trims trailing comment",
                    repl_extract_assignment_parts("radius = sin(x); // keep",
                                                  name, sizeof(name),
                                                  rhs, sizeof(rhs)) == 1);
        ASSERT_TRUE("assignment rhs strips semicolon and comment",
                    strcmp(rhs, "sin(x)") == 0);
        ASSERT_TRUE("extract scratch assignment target",
                repl_extract_assignment_target_parts("  A[i + 1] = sin(x); // keep",
                                 name, sizeof(name),
                                 index_expr, sizeof(index_expr),
                                 rhs, sizeof(rhs)) == 1);
        ASSERT_TRUE("scratch assignment name", strcmp(name, "A") == 0);
        ASSERT_TRUE("scratch assignment index", strcmp(index_expr, "i + 1") == 0);
        ASSERT_TRUE("scratch assignment rhs", strcmp(rhs, "sin(x)") == 0);
        ASSERT_TRUE("scalar wrapper rejects scratch target",
                repl_extract_assignment_parts("A[0] = 1", name, sizeof(name),
                              rhs, sizeof(rhs)) == 0);
    }

    {
        char label[64];
        memset(label, 0, sizeof(label));
        ASSERT_TRUE("extract label name",
                    repl_extract_label_name("  :walk_2", label, sizeof(label)) == 1);
        ASSERT_TRUE("extract label text", strcmp(label, "walk_2") == 0);
        ASSERT_TRUE("extract goto label",
                    repl_extract_goto_label("  goto walk_2;  ",
                                            label, sizeof(label)) == 1);
        ASSERT_TRUE("extract goto text", strcmp(label, "walk_2") == 0);
        ASSERT_TRUE("extract goto rejects missing target",
                    repl_extract_goto_label("goto   ;", label, sizeof(label)) == 0);
        ASSERT_TRUE("extract label rejects empty identifier",
                    repl_extract_label_name("  :   ", label, sizeof(label)) == 0);
    }

    {
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glVertex3f(x + 1, 0, 0)", &cmd);
        ASSERT_TRUE("public parse_command detects predef vars", ok == 1);
        ASSERT_TRUE("public parse_command has_vars for predef", cmd.has_vars == 1);
    }

    {
        ExprVar vars[1] = { { "radius", 2.0f } };
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test_with_vars("glVertex3f(1, 2, 3)", &cmd, vars, 1);
        ASSERT_TRUE("parse with locals constant ok", ok == 1);
        ASSERT_TRUE("parse with locals constant has_vars off", cmd.has_vars == 0);
    }

    {
        ExprVar vars[1] = { { "radius", 2.0f } };
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test_with_vars("glVertex3f(radius, 0, 0)", &cmd, vars, 1);
        ASSERT_TRUE("parse with locals referenced ok", ok == 1);
        ASSERT_TRUE("parse with locals referenced has_vars on", cmd.has_vars == 1);
    }

    {
        ExprVar vars[1] = { { "radius", 2.0f } };
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test_with_vars("func0(1, 2)", &cmd, vars, 1);
        ASSERT_TRUE("func call with locals constant ok", ok == 1);
        ASSERT_TRUE("func call with locals constant has_vars off", cmd.has_vars == 0);
    }

    {
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_and_normalize("glVertex3f(x+1, y, z)", 0,
                                          NULL, 0, 1, &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("parse normalize vars ok", ok == 1);
        ASSERT_TRUE("cmd has vars", cmd.has_vars == 1);
        ASSERT_TRUE("source keeps x+1", strstr(cmd_text, "x+1") != NULL);
        ASSERT_TRUE("source keeps y", strstr(cmd_text, "y") != NULL);
        ASSERT_TRUE("source keeps z", strstr(cmd_text, "z") != NULL);
    }

    {
        glr_ctrl_reset_all();
        declare_test_vars();
        editor_feed_line("glBegin(GL_TRIANGLES);");
        editor_state_edit_line_set(0);

        GLCmd cmd;
        ReplSourceScopeView source_scope;
        repl_source_scope_view_bind(&source_scope,
                                    repl_state_document_cmds(),
                                    repl_state_document_count());
        ReplParseContext ctx = {
            .source_line_idx = repl_state_document_count(),
            .source_scope = &source_scope,
        };
        ReplParsedLine pl;
        int ok = repl_parser_parse_command_ctx("glVertex3f(1, 2, 3)", &pl, &ctx);
        ASSERT_TRUE("context parse ok", ok == 1);
        ASSERT_TRUE("context parse uses source line indent",
                    leading_spaces(pl.text) == 4);
        ASSERT_TRUE("context parse leaves edit line alone", editor_state_edit_line() == 0);

        memset(&cmd, 0, sizeof(cmd));
        ok = repl_parse_and_normalize("glColor3f(1, 0, 0)", repl_state_document_count(),
                                      NULL, 0, 0, &cmd, NULL, 0);
        ASSERT_TRUE("normalize explicit line ok", ok == 1);
        ASSERT_TRUE("normalize explicit line leaves edit line alone",
                    editor_state_edit_line() == 0);
    }

    {
        GLCmd doc[1];
        memset(doc, 0, sizeof(doc));
        doc[0].valid = 1;
        doc[0].type = CMD_FUNC_DEF;
        doc[0].args[0] = 0.0f;
        ReplSourceScopeView source_scope;
        repl_source_scope_view_bind(&source_scope, doc, 1);

        char aliases[REPL_FUNC_SLOT_COUNT][REPL_FUNC_NAME_MAX];
        memset(aliases, 0, sizeof(aliases));
        snprintf(aliases[0], sizeof(aliases[0]), "%s", "drawBox");

        char err_buf[REPL_STATUS_TEXT_MAX] = "";
        ReplParseContext ctx = {
            .source_line_idx = 1,
            .strict_refs = 1,
            .err_buf = err_buf,
            .err_sz = (int)sizeof(err_buf),
            .func_aliases = { aliases, REPL_FUNC_SLOT_COUNT },
            .source_scope = &source_scope,
        };
        ReplParsedLine pl;
        int ok = repl_parser_parse_command_ctx("drawBox(1);", &pl, &ctx);
        ASSERT_TRUE("parser resolves alias from context", ok == 1);
        ASSERT_TRUE("parser alias call targets slot 0", (int)pl.cmd.args[0] == 0);
        ASSERT_TRUE("parser alias canonical text uses context alias",
                    strstr(pl.text, "drawBox(1)") != NULL);

        repl_func_alias_clear_all();
        ASSERT_TRUE("set global-only alias", repl_func_alias_set(0, "globalOnly") == 1);
        memset(&pl, 0, sizeof(pl));
        err_buf[0] = '\0';
        ReplParseContext no_alias_ctx = {
            .source_line_idx = 1,
            .strict_refs = 1,
            .err_buf = err_buf,
            .err_sz = (int)sizeof(err_buf),
            .source_scope = &source_scope,
        };
        ok = repl_parser_parse_command_ctx("globalOnly();", &pl, &no_alias_ctx);
        ASSERT_TRUE("parser ignores global alias without context", ok == 0);
        repl_func_alias_clear_all();
    }

    {
        glr_ctrl_reset_all();
        declare_test_vars();
        editor_feed_line("func0() {");
        editor_feed_line("}");

        ReplSourceScopeView empty_scope;
        repl_source_scope_view_bind(&empty_scope, NULL, 0);
        char err_buf[REPL_STATUS_TEXT_MAX] = "";
        ReplParseContext ctx = {
            .source_line_idx = 0,
            .strict_refs = 1,
            .err_buf = err_buf,
            .err_sz = (int)sizeof(err_buf),
            .source_scope = &empty_scope,
        };
        ReplParsedLine pl;
        int ok = repl_parser_parse_command_ctx("func0();", &pl, &ctx);
        ASSERT_TRUE("strict refs ignore live document without source view",
                    ok == 0);
        ASSERT_TRUE("strict refs report undefined from explicit empty view",
                    strstr(err_buf, "undefined function 'func0'") != NULL);
    }

    {
        glr_ctrl_reset_all();
        declare_test_vars();
        editor_feed_line("gluBegin(GLU_POLYGON);");
        editor_feed_line("gluBegin(GLU_CONTOUR);");
        editor_feed_line("glBegin(GL_TRIANGLES);");

        GLCmd tess_cmd;
        GLCmd gl_cmd;
        char tess_text[MAX_LINE_LEN] = "";
        char gl_text[MAX_LINE_LEN] = "";
        memset(&tess_cmd, 0, sizeof(tess_cmd));
        memset(&gl_cmd, 0, sizeof(gl_cmd));

        int ok1 = repl_parse_and_normalize("gluNormal(0, 0, 1)", repl_state_document_count(),
                                           NULL, 0, 0, &tess_cmd, tess_text, sizeof(tess_text));
        int ok2 = repl_parse_and_normalize("glVertex3f(1, 2, 3)", repl_state_document_count(),
                                           NULL, 0, 0, &gl_cmd, gl_text, sizeof(gl_text));
        ASSERT_TRUE("tess parse ok", ok1 == 1);
        ASSERT_TRUE("gl parse ok", ok2 == 1);

        int tess_indent = leading_spaces(tess_text);
        int gl_indent = leading_spaces(gl_text);
        ASSERT_TRUE("gl indent > tess indent", gl_indent > tess_indent);
        ASSERT_TRUE("indent delta is 2", gl_indent - tess_indent == 2);
    }

    {
        glr_ctrl_reset_all();
        declare_test_vars();
        editor_state_edit_line_set(0);
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glBegin(GL_TRIANGLES)", &cmd);
        ASSERT_TRUE("public parse_command", ok == 1);
        ASSERT_TRUE("public parse_command type", cmd.type == CMD_BEGIN);
    }

    {
        glr_ctrl_reset_all();
        declare_test_vars();
        editor_state_edit_line_set(0);
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("func0(x + 1, 2)", &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("public parse_command func call", ok == 1);
        ASSERT_TRUE("public parse_command func type", cmd.type == CMD_CALL);
        ASSERT_TRUE("func call keeps raw expr", strstr(cmd_text, "x + 1") != NULL);
    }

    /* Trailing `// ...` comment containing a ')': the arg-list close paren
     * must be located from the command, not the last ')' in the line (which
     * lives inside the comment). Regression for the strrchr(p, ')') bug. */
    {
        glr_ctrl_reset_all();
        declare_test_vars();
        editor_state_edit_line_set(0);
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glColor3f(1, 0, 0); // tint (with paren)",
                                     &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("gl call w/ paren-comment parses", ok == 1);
        ASSERT_TRUE("gl call w/ paren-comment type", cmd.type == CMD_COLOR3F);
        ASSERT_TRUE("gl call w/ paren-comment arg count", cmd.num_args == 3);
        ASSERT_TRUE("gl call w/ paren-comment args intact",
                    cmd.args[0] == 1.0f && cmd.args[1] == 0.0f && cmd.args[2] == 0.0f);
        ASSERT_TRUE("gl call w/ paren-comment keeps comment",
                    strstr(cmd_text, "// tint (with paren)") != NULL);
    }

    /* Same bug on a funcN call whose comment contains ')'. */
    {
        glr_ctrl_reset_all();
        declare_test_vars();
        editor_state_edit_line_set(0);
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("func0(x + 1, 2); // step (i of n)",
                                     &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("func call w/ paren-comment parses", ok == 1);
        ASSERT_TRUE("func call w/ paren-comment type", cmd.type == CMD_CALL);
        ASSERT_TRUE("func call w/ paren-comment keeps raw expr",
                    strstr(cmd_text, "x + 1") != NULL);
        ASSERT_TRUE("func call w/ paren-comment keeps comment",
                    strstr(cmd_text, "// step (i of n)") != NULL);
    }

    /* The fix must not weaken the "unexpected text after ')'" guard: real
     * garbage after the close paren (not a // comment) still fails. */
    {
        glr_ctrl_reset_all();
        g_status[0] = '\0';
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glColor3f(1, 0, 0) garbage", &cmd);
        ASSERT_TRUE("trailing garbage after ) still fails", ok == 0);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char long_cmd[256];
        memset(&cmd, 0, sizeof(cmd));
        memset(long_cmd, 'a', sizeof(long_cmd) - 1);
        long_cmd[sizeof(long_cmd) - 1] = '\0';
        int ok = parse_for_test(long_cmd, &cmd);
        ASSERT_TRUE("long unknown command parse fails", ok == 0);
        ASSERT_TRUE("long unknown command reports status",
                    strstr(g_status, "Unknown cmd") != NULL);
    }

    /* 4-arg commands (glRotatef, glutSolidTorus, glutSolidCone) - exercise case 4 in fmt switch */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glRotatef(45, 0, 1, 0)", &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glRotatef parse ok", ok == 1);
        ASSERT_TRUE("glRotatef type", cmd.type == CMD_ROTATEF);
        ASSERT_TRUE("glRotatef source has 45", strstr(cmd_text, "45") != NULL);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glutSolidTorus(0.1, 0.4, 8, 16)", &cmd);
        ASSERT_TRUE("glutSolidTorus parse ok", ok == 1);
        ASSERT_TRUE("glutSolidTorus type", cmd.type == CMD_GLUT_TORUS);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glutSolidCone(0.25, 1.0, 12, 4)", &cmd);
        ASSERT_TRUE("glutSolidCone parse ok", ok == 1);
        ASSERT_TRUE("glutSolidCone type", cmd.type == CMD_GLUT_CONE);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glutSolidSphere(0.25, 16, 12)", &cmd);
        ASSERT_TRUE("glutSolidSphere parse ok", ok == 1);
        ASSERT_TRUE("glutSolidSphere type", cmd.type == CMD_GLUT_SPHERE);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glutSolidTeapot(0.25)", &cmd);
        ASSERT_TRUE("glutSolidTeapot parse ok", ok == 1);
        ASSERT_TRUE("glutSolidTeapot type", cmd.type == CMD_GLUT_TEAPOT);
    }

    /* label: success cases. */
    /* glRasterPos3f is table-driven, but worth one parse smoke
     * test now that label() depends on a preceding raster pos. */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glRasterPos3f(0, 1.5, 0)", &cmd);
        ASSERT_TRUE("glRasterPos3f parse ok", ok == 1);
        ASSERT_TRUE("glRasterPos3f type", cmd.type == CMD_RASTER_POS3F);
        ASSERT_TRUE("glRasterPos3f args[1]", cmd.args[1] == 1.5f);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text(
            "label(\"hello\")", &cmd, text, sizeof(text));
        ASSERT_TRUE("label no-args parse ok", ok == 1);
        ASSERT_TRUE("label type",
                    cmd.type == CMD_LABEL);
        ASSERT_TRUE("label num_args", cmd.num_args == 0);
        ASSERT_TRUE("label fmt stored",
                    strcmp(cmd.payload.label.fmt, "hello") == 0);
        ASSERT_TRUE("label canonical text",
                    strstr(text, "\"hello\"") != NULL);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"v=%f\", 3.5)", &cmd);
        ASSERT_TRUE("label one %f parse ok", ok == 1);
        ASSERT_TRUE("label one %f num_args", cmd.num_args == 1);
        ASSERT_TRUE("label one %f sub arg", cmd.args[0] == 3.5f);
        ASSERT_TRUE("label one %f fmt stored",
                    strcmp(cmd.payload.label.fmt, "v=%f") == 0);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test(
            "label(\"a=%f b=%f c=%f d=%f\", 1, 2, 3, 4)", &cmd);
        ASSERT_TRUE("label four %f parse ok", ok == 1);
        ASSERT_TRUE("label four %f num_args", cmd.num_args == 4);
        ASSERT_TRUE("label four %f arg[3]", cmd.args[3] == 4.0f);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"100%% done\")", &cmd);
        ASSERT_TRUE("label %% literal parse ok", ok == 1);
        ASSERT_TRUE("label %% literal num_args", cmd.num_args == 0);
        ASSERT_TRUE("label %% literal fmt stored",
                    strcmp(cmd.payload.label.fmt, "100%% done") == 0);
    }

    /* label: error cases. Each must surface a graceful
     * status message and reject the line (no GLCmd committed). */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"a // b\")", &cmd);
        ASSERT_TRUE("label // forbidden", ok == 0);
        ASSERT_TRUE("label // forbidden status",
                    strstr(g_status, "'//'") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"a , b\")", &cmd);
        ASSERT_TRUE("label , forbidden", ok == 0);
        ASSERT_TRUE("label , forbidden status",
                    strstr(g_status, "','") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"hi\\nworld\")", &cmd);
        ASSERT_TRUE("label backslash forbidden", ok == 0);
        ASSERT_TRUE("label backslash forbidden status",
                    strstr(g_status, "backslash") != NULL);
    }
    {
        /* Outer parser must find the closing ')' first; missing
         * close quote is detected by our string-aware helper after
         * the args are extracted between the parens. */
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"unterminated)", &cmd);
        ASSERT_TRUE("label missing close quote", ok == 0);
        ASSERT_TRUE("label missing close quote status",
                    strstr(g_status, "closing") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"%f\", 1, 2)", &cmd);
        ASSERT_TRUE("label arg-count mismatch", ok == 0);
        ASSERT_TRUE("label arg-count status",
                    strstr(g_status, "format expects") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"%d\", 1)", &cmd);
        ASSERT_TRUE("label %d rejected", ok == 0);
        ASSERT_TRUE("label %d status",
                    strstr(g_status, "only %f") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test(
            "label(\"%f %f %f %f %f\", 1, 2, 3, 4, 5)", &cmd);
        ASSERT_TRUE("label >4 sub args rejected", ok == 0);
        ASSERT_TRUE("label >4 sub args status",
                    strstr(g_status, "max 4") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"a ( b\")", &cmd);
        ASSERT_TRUE("label ( forbidden", ok == 0);
        ASSERT_TRUE("label ( forbidden status",
                    strstr(g_status, "'('") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"a ) b\")", &cmd);
        ASSERT_TRUE("label ) forbidden", ok == 0);
        ASSERT_TRUE("label ) forbidden status",
                    strstr(g_status, "')'") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"1234567890123456789012345678901234567890123456789012345678901234\")", &cmd);
        ASSERT_TRUE("label 64-char format rejected", ok == 0);
        ASSERT_TRUE("label 64-char format status",
                    strstr(g_status, "format too long") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"123456789012345678901234567890123456789012345678901234567890123\")", &cmd);
        ASSERT_TRUE("label 63-char format accepted", ok == 1);
        ASSERT_TRUE("label 63-char format matches",
                    strcmp(cmd.payload.label.fmt, "123456789012345678901234567890123456789012345678901234567890123") == 0);
    }

    /* Removed GLU quadric shapes should no longer parse. */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        ASSERT_TRUE("gluSphere parse fails",
                    parse_for_test("gluSphere(0.25, 16, 12)", &cmd) == 0);
        ASSERT_TRUE("gluCylinder parse fails",
                    parse_for_test("gluCylinder(0.3, 0.3, 1.0, 12, 4)", &cmd) == 0);
        ASSERT_TRUE("gluDisk parse fails",
                    parse_for_test("gluDisk(0.2, 0.5, 16, 2)", &cmd) == 0);
        ASSERT_TRUE("gluPartialDisk parse fails",
                    parse_for_test("gluPartialDisk(0.1, 0.5, 16, 2, 0, 180)", &cmd) == 0);
    }

    /* glColorMaterial - face/mode enums */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE)",
                                      &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glColorMaterial parse ok", ok == 1);
        ASSERT_TRUE("glColorMaterial type", cmd.type == CMD_COLOR_MATERIAL);
        ASSERT_TRUE("glColorMaterial face", (GLenum)cmd.args[0] == GL_FRONT_AND_BACK);
        ASSERT_TRUE("glColorMaterial mode", (GLenum)cmd.args[1] == GL_AMBIENT_AND_DIFFUSE);
        ASSERT_TRUE("glColorMaterial source",
                    strstr(cmd_text, "glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);") != NULL);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glColorMaterial(GL_FRONT, GL_SHININESS)", &cmd);
        ASSERT_TRUE("glColorMaterial rejects shininess", ok == 0);
        assert_status_contains("glColorMaterial bad mode status", "GL_AMBIENT_AND_DIFFUSE");
    }

    /* glLightModeli slot 1 is ENUM_OR_EXPR (token, else expression).
     * Strict-compat: GL_TRUE / 1 / 0 / a non-table integer all parse
     * and the typed source token is emitted verbatim (not
     * canonicalized) on the expression-fallback path. */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char t[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE)",
                                     &cmd, t, sizeof(t));
        ASSERT_TRUE("glLightModeli GL_TRUE ok", ok == 1);
        ASSERT_TRUE("glLightModeli pname", (GLenum)cmd.args[0] == GL_LIGHT_MODEL_TWO_SIDE);
        ASSERT_TRUE("glLightModeli GL_TRUE val", (GLenum)cmd.args[1] == GL_TRUE);
        ASSERT_TRUE("glLightModeli GL_TRUE text",
                    strstr(t, "glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char t[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 1)",
                                     &cmd, t, sizeof(t));
        ASSERT_TRUE("glLightModeli 1 ok", ok == 1);
        ASSERT_TRUE("glLightModeli 1 val", cmd.args[1] == 1.0f);
        ASSERT_TRUE("glLightModeli 1 verbatim (not canonicalized)",
                    strstr(t, "glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 1);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char t[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        /* Non-table integer: parsed before the refactor, still parses. */
        int ok = parse_cmd_with_text("glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, 2)",
                                     &cmd, t, sizeof(t));
        ASSERT_TRUE("glLightModeli non-table int ok", ok == 1);
        ASSERT_TRUE("glLightModeli non-table int val", cmd.args[1] == 2.0f);
        ASSERT_TRUE("glLightModeli non-table int verbatim",
                    strstr(t, "glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, 2);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        declare_test_vars();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        /* The one intentional change: a var expression in the ENUM_OR_EXPR
         * slot now sets has_vars (re-evaluation) where it did not before. */
        int ok = parse_for_test("glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, x)", &cmd);
        ASSERT_TRUE("glLightModeli var ok", ok == 1);
        ASSERT_TRUE("glLightModeli var sets has_vars", cmd.has_vars == 1);
    }

    /* glMaterialfv - scalar (1-float) and vector (4-float) forms;
     * accepts flat-float input AND the compound-literal form, canonical
     * emit always wraps the values in (GLfloat[]){...}. */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glMaterialfv(GL_FRONT, GL_SHININESS, 64)",
                                      &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glMaterialfv scalar parse ok (flat)", ok == 1);
        ASSERT_TRUE("glMaterialfv scalar type", cmd.type == CMD_MATERIALFV);
        ASSERT_TRUE("glMaterialfv scalar source has SHININESS",
                    strstr(cmd_text, "GL_SHININESS") != NULL);
        ASSERT_TRUE("glMaterialfv scalar canonical emits compound literal",
                    strstr(cmd_text, "(GLfloat[]){64}") != NULL);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text(
            "glMaterialfv(GL_FRONT, GL_DIFFUSE, 0.8, 0.2, 0.2, 1.0)",
            &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glMaterialfv vector parse ok (flat 4)", ok == 1);
        ASSERT_TRUE("glMaterialfv vector type", cmd.type == CMD_MATERIALFV);
        ASSERT_TRUE("glMaterialfv vector num_args",
                    cmd.num_args == 6); /* face + pname + 4 vals */
        ASSERT_TRUE("glMaterialfv vector canonical emits compound literal",
                    strstr(cmd_text, "(GLfloat[]){0.8, 0.2, 0.2, 1}") != NULL);
    }

    /* Compound-literal input round-trips through the parser without
     * losing args or changing function name. */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text(
            "glMaterialfv(GL_FRONT, GL_DIFFUSE, (GLfloat[]){0.8, 0.2, 0.2, 1.0})",
            &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glMaterialfv compound-literal parse ok", ok == 1);
        ASSERT_TRUE("glMaterialfv compound-literal num_args",
                    cmd.num_args == 6);
        ASSERT_TRUE("glMaterialfv compound-literal source preserves form",
                    strstr(cmd_text, "(GLfloat[]){0.8, 0.2, 0.2, 1}") != NULL);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text(
            "glMaterialfv(GL_FRONT, GL_SHININESS, (GLfloat[]){42.0})",
            &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glMaterialfv scalar compound-literal parse ok", ok == 1);
        ASSERT_TRUE("glMaterialfv scalar compound num_args",
                    cmd.num_args == 3);
        ASSERT_TRUE("glMaterialfv scalar compound canonical form",
                    strstr(cmd_text, "(GLfloat[]){42}") != NULL);
    }

    /* glMaterialfv - bad face name */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glMaterialfv(FRONT, GL_DIFFUSE, 0.5)", &cmd);
        ASSERT_TRUE("glMaterialfv bad face returns 0", ok == 0);
    }

    /* Positive coverage: the four valid input shapes the parser
     * accepts (scalar flat, vector flat, scalar compound literal,
     * vector compound literal) were exercised above for the
     * GL_FRONT × {GL_SHININESS, GL_DIFFUSE} corner. Broaden the
     * positive surface so a future change can't quietly drop a face
     * type, pname, expression evaluator path, or whitespace-handling
     * concern — each pulled out as its own ASSERT so a regression
     * names what it broke. */

    /* Each face token resolves through the enum table. */
    {
        const char *const cases[] = {
            "glMaterialfv(GL_FRONT, GL_DIFFUSE, 1, 0, 0, 1)",
            "glMaterialfv(GL_BACK, GL_DIFFUSE, 1, 0, 0, 1)",
            "glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, 1, 0, 0, 1)",
        };
        for (int i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
            glr_ctrl_reset_all();
            GLCmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            int ok = parse_for_test(cases[i], &cmd);
            ASSERT_TRUE("glMaterialfv: face variant parses", ok == 1);
            ASSERT_TRUE("glMaterialfv: face variant num_args",
                        cmd.num_args == 6);
        }
    }

    /* Each RGBA material pname accepts the 4-float vector form. The
     * scalar-only GL_SHININESS path is covered separately above. */
    {
        const char *const cases[] = {
            "glMaterialfv(GL_FRONT, GL_AMBIENT, 0.1, 0.1, 0.1, 1)",
            "glMaterialfv(GL_FRONT, GL_SPECULAR, 1, 1, 1, 1)",
            "glMaterialfv(GL_FRONT, GL_EMISSION, 0, 0, 0, 1)",
            "glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, 0.5, 0.5, 0.5, 1)",
        };
        for (int i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
            glr_ctrl_reset_all();
            GLCmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            int ok = parse_for_test(cases[i], &cmd);
            ASSERT_TRUE("glMaterialfv: pname variant parses", ok == 1);
            ASSERT_TRUE("glMaterialfv: pname variant num_args",
                        cmd.num_args == 6);
        }
    }

    /* Expressions inside the compound literal exercise both the
     * brace-aware unwrap (don't truncate at the inner ')') and the
     * full expression evaluator. The has_vars flag must propagate so
     * variable-bearing commands can re-evaluate during execution. */
    {
        glr_ctrl_reset_all();
        declare_test_vars();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text(
            "glMaterialfv(GL_FRONT, GL_DIFFUSE, (GLfloat[]){0.5 + 0.5*sin(t), 0.3, 0.4, 1})",
            &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glMaterialfv compound w/ expr: parse ok", ok == 1);
        ASSERT_TRUE("glMaterialfv compound w/ expr: num_args", cmd.num_args == 6);
        ASSERT_TRUE("glMaterialfv compound w/ expr: has_vars (t)", cmd.has_vars == 1);
    }

    /* Variable references on the flat path also flag has_vars. */
    {
        glr_ctrl_reset_all();
        declare_test_vars();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test(
            "glMaterialfv(GL_FRONT, GL_DIFFUSE, x, y, z, 1)", &cmd);
        ASSERT_TRUE("glMaterialfv flat w/ vars: parse ok", ok == 1);
        ASSERT_TRUE("glMaterialfv flat w/ vars: has_vars", cmd.has_vars == 1);
    }

    /* Whitespace tolerance: extra padding inside the call AND inside
     * the compound literal, plus the no-whitespace minimal form,
     * should all canonicalize to the same compound-literal emit. */
    {
        const char *const cases[] = {
            "glMaterialfv(GL_FRONT,GL_DIFFUSE,0.8,0.2,0.2,1)",
            "glMaterialfv(  GL_FRONT  ,  GL_DIFFUSE  ,  0.8 , 0.2 , 0.2 , 1  )",
            "glMaterialfv(GL_FRONT, GL_DIFFUSE, (GLfloat[]){ 0.8, 0.2, 0.2, 1 })",
        };
        for (int i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
            glr_ctrl_reset_all();
            GLCmd cmd;
            char cmd_text[MAX_LINE_LEN] = "";
            memset(&cmd, 0, sizeof(cmd));
            int ok = parse_cmd_with_text(cases[i], &cmd, cmd_text, sizeof(cmd_text));
            ASSERT_TRUE("glMaterialfv whitespace variant parses", ok == 1);
            ASSERT_TRUE("glMaterialfv whitespace variant canonical form",
                        strstr(cmd_text,
                               "glMaterialfv(GL_FRONT, GL_DIFFUSE, (GLfloat[]){0.8, 0.2, 0.2, 1})")
                            != NULL);
        }
    }

    /* Trailing ';' is part of the line text users type interactively;
     * the canonical emit always supplies its own, so an input ';'
     * must not double up or break parsing. */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text(
            "glMaterialfv(GL_FRONT, GL_SHININESS, (GLfloat[]){30});",
            &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glMaterialfv with trailing ';' parses", ok == 1);
        ASSERT_TRUE("glMaterialfv with trailing ';' canonical ends with ');'",
                    strstr(cmd_text, ");") != NULL);
    }

    /* Regression: 6-arg flat input form
     * (`glMaterialfv(face, pname, r, g, b, a)`) must parse, populate
     * args[2..5] from the four RGBA floats, AND canonicalize to the
     * compound-literal emit that re-parses unchanged. This is the
     * shape interactive users type most often, and the original
     * glMaterialf bug silently dropped the round-trip — locking it in. */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text(
            "glMaterialfv(GL_FRONT, GL_DIFFUSE, 0.8, 0.2, 0.2, 1.0)",
            &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glMaterialfv 6-arg flat: parse ok", ok == 1);
        ASSERT_TRUE("glMaterialfv 6-arg flat: type", cmd.type == CMD_MATERIALFV);
        ASSERT_TRUE("glMaterialfv 6-arg flat: num_args 6 (face+pname+4)",
                    cmd.num_args == 6);
        ASSERT_TRUE("glMaterialfv 6-arg flat: r preserved",
                    cmd.args[2] > 0.799f && cmd.args[2] < 0.801f);
        ASSERT_TRUE("glMaterialfv 6-arg flat: g preserved",
                    cmd.args[3] > 0.199f && cmd.args[3] < 0.201f);
        ASSERT_TRUE("glMaterialfv 6-arg flat: b preserved",
                    cmd.args[4] > 0.199f && cmd.args[4] < 0.201f);
        ASSERT_TRUE("glMaterialfv 6-arg flat: a preserved",
                    cmd.args[5] > 0.999f && cmd.args[5] < 1.001f);
        ASSERT_TRUE("glMaterialfv 6-arg flat: canonical emits compound literal",
                    strstr(cmd_text,
                           "glMaterialfv(GL_FRONT, GL_DIFFUSE, (GLfloat[]){0.8, 0.2, 0.2, 1})")
                        != NULL);

        /* Round-trip: feed the canonical emit back through the parser
         * and confirm the resulting GLCmd is identical. */
        glr_ctrl_reset_all();
        GLCmd cmd2;
        char cmd_text2[MAX_LINE_LEN] = "";
        memset(&cmd2, 0, sizeof(cmd2));
        int ok2 = parse_cmd_with_text(cmd_text, &cmd2, cmd_text2, sizeof(cmd_text2));
        ASSERT_TRUE("glMaterialfv 6-arg flat: canonical re-parses", ok2 == 1);
        ASSERT_TRUE("glMaterialfv 6-arg flat: round-trip num_args",
                    cmd2.num_args == 6);
        ASSERT_TRUE("glMaterialfv 6-arg flat: round-trip canonical text matches",
                    strcmp(cmd_text, cmd_text2) == 0);
    }

    /* glMaterialf — scalar-only sibling for GL_SHININESS. Distinct
     * CmdType (CMD_MATERIALF), canonical emit preserves the typed
     * function name (no rewrite to glMaterialfv). */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glMaterialf(GL_FRONT, GL_SHININESS, 64)",
                                      &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glMaterialf parse ok", ok == 1);
        ASSERT_TRUE("glMaterialf type", cmd.type == CMD_MATERIALF);
        ASSERT_TRUE("glMaterialf num_args (face+pname+value)",
                    cmd.num_args == 3);
        ASSERT_TRUE("glMaterialf value preserved",
                    cmd.args[2] > 63.9f && cmd.args[2] < 64.1f);
        ASSERT_TRUE("glMaterialf canonical preserves function name",
                    strstr(cmd_text, "glMaterialf(GL_FRONT, GL_SHININESS, 64);")
                        != NULL);
        ASSERT_TRUE("glMaterialf canonical is not the fv form",
                    strstr(cmd_text, "(GLfloat[]){") == NULL);
    }

    /* glMaterialf rejects RGBA pnames — those need 4 floats, only
     * glMaterialfv accepts them. */
    {
        const char *const cases[] = {
            "glMaterialf(GL_FRONT, GL_DIFFUSE, 0.5)",
            "glMaterialf(GL_FRONT, GL_AMBIENT, 0.5)",
            "glMaterialf(GL_FRONT, GL_SPECULAR, 0.5)",
            "glMaterialf(GL_FRONT, GL_EMISSION, 0.5)",
            "glMaterialf(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, 0.5)",
        };
        for (int i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
            glr_ctrl_reset_all();
            GLCmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            int ok = parse_for_test(cases[i], &cmd);
            ASSERT_TRUE("glMaterialf RGBA pname rejected", ok == 0);
        }
    }

    /* glMaterialf rejects bad face token. */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glMaterialf(FRONT, GL_SHININESS, 64)", &cmd);
        ASSERT_TRUE("glMaterialf bad face returns 0", ok == 0);
    }

    /* glMaterialf accepts an expression in the value slot and
     * propagates has_vars when the expression references a predef. */
    {
        glr_ctrl_reset_all();
        declare_test_vars();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test(
            "glMaterialf(GL_FRONT, GL_SHININESS, x * 64)", &cmd);
        ASSERT_TRUE("glMaterialf expr parse ok", ok == 1);
        ASSERT_TRUE("glMaterialf expr has_vars", cmd.has_vars == 1);
    }

    /* Scalar input is only valid for GL_SHININESS: the RGBA pnames
     * expect 4 floats, and a 1-element compound literal would let the
     * GL driver read past the array in exported standalone C. */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glMaterialfv(GL_FRONT, GL_DIFFUSE, 0.5)", &cmd);
        ASSERT_TRUE("scalar for GL_DIFFUSE rejected", ok == 0);
        ASSERT_TRUE("scalar-for-non-shininess status",
                    strstr(g_status, "Only GL_SHININESS") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test(
            "glMaterialfv(GL_FRONT, GL_AMBIENT, (GLfloat[]){0.5})", &cmd);
        ASSERT_TRUE("scalar compound for GL_AMBIENT rejected", ok == 0);
    }

    /* Trailing tokens after the compound literal's closing brace are
     * silently dropped without this guard — reject so the user sees
     * the typo. */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test(
            "glMaterialfv(GL_FRONT, GL_DIFFUSE, (GLfloat[]){1, 2, 3, 4}, 99)",
            &cmd);
        ASSERT_TRUE("trailing arg after compound literal rejected", ok == 0);
        ASSERT_TRUE("trailing arg status",
                    strstr(g_status, "Trailing content after") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test(
            "glMaterialfv(GL_FRONT, GL_DIFFUSE, (GLfloat[]){1, 2, 3, 4} junk)",
            &cmd);
        ASSERT_TRUE("trailing junk no-comma after compound literal rejected",
                    ok == 0);
    }

    /* glPointParameterfv */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        ExprVar vars[1] = { { "radius", 1.0f } };
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test_with_vars("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 1, 0, 0)",
                                          &cmd, vars, 1);
        ASSERT_TRUE("glPointParameterfv parse ok", ok == 1);
        ASSERT_TRUE("glPointParameterfv type", cmd.type == CMD_POINT_PARAMETER_FV);
        ASSERT_TRUE("glPointParameterfv literal has_vars false", cmd.has_vars == 0);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        GLCmd cmd2;
        char cmd_text[MAX_LINE_LEN] = "";
        char cmd_text2[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        memset(&cmd2, 0, sizeof(cmd2));
        int ok = parse_cmd_with_text(
            "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 0.2, 0, 0.15)",
            &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glPointParameterfv flat parse ok", ok == 1);
        ASSERT_TRUE("glPointParameterfv flat canonical emits compound literal",
                    strstr(cmd_text,
                           "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, (GLfloat[]){0.2, 0, 0.15})")
                        != NULL);

        ok = parse_cmd_with_text(cmd_text, &cmd2, cmd_text2, sizeof(cmd_text2));
        ASSERT_TRUE("glPointParameterfv canonical re-parses", ok == 1);
        ASSERT_TRUE("glPointParameterfv canonical type",
                    cmd2.type == CMD_POINT_PARAMETER_FV);
        ASSERT_TRUE("glPointParameterfv canonical num_args",
                    cmd2.num_args == 4);
        ASSERT_TRUE("glPointParameterfv canonical text stable",
                    strcmp(cmd_text, cmd_text2) == 0);
    }

    /* glMultMatrixf, scratch-array form — the argument is a name, not an
     * expression, so the accept/reject set is unusually narrow. */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        GLCmd cmd2;
        char cmd_text[MAX_LINE_LEN] = "";
        char cmd_text2[MAX_LINE_LEN] = "";
        int ok;

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_cmd_with_text("glMultMatrixf(A)", &cmd,
                                 cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glMultMatrixf parse ok", ok == 1);
        ASSERT_TRUE("glMultMatrixf type", cmd.type == CMD_MULT_MATRIXF);
        ASSERT_TRUE("glMultMatrixf num_args", cmd.num_args == 1);
        ASSERT_TRUE("glMultMatrixf array index", cmd.args[0] == 0.0f);
        ASSERT_TRUE("glMultMatrixf has_vars false", cmd.has_vars == 0);
        /* Identity until flatten snapshots the live cells — never a zero
         * matrix, which would collapse everything after it. */
        ASSERT_TRUE("glMultMatrixf payload starts identity",
                    cmd.payload.matrix.m[0] == 1.0f &&
                    cmd.payload.matrix.m[5] == 1.0f &&
                    cmd.payload.matrix.m[10] == 1.0f &&
                    cmd.payload.matrix.m[15] == 1.0f &&
                    cmd.payload.matrix.m[1] == 0.0f &&
                    cmd.payload.matrix.m[12] == 0.0f);
        ASSERT_TRUE("glMultMatrixf canonical text",
                    strstr(cmd_text, "glMultMatrixf(A);") != NULL);

        ok = parse_cmd_with_text(cmd_text, &cmd2, cmd_text2, sizeof(cmd_text2));
        ASSERT_TRUE("glMultMatrixf canonical re-parses", ok == 1);
        ASSERT_TRUE("glMultMatrixf canonical text stable",
                    strcmp(cmd_text, cmd_text2) == 0);

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_cmd_with_text("glMultMatrixf( C )", &cmd,
                                 cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glMultMatrixf tolerates spacing", ok == 1);
        ASSERT_TRUE("glMultMatrixf picks named array", cmd.args[0] == 2.0f);

        memset(&cmd, 0, sizeof(cmd));
        ASSERT_TRUE("glMultMatrixf rejects unknown array",
                    parse_cmd_with_text("glMultMatrixf(D)", &cmd,
                                        cmd_text, sizeof(cmd_text)) == 0);
        memset(&cmd, 0, sizeof(cmd));
        ASSERT_TRUE("glMultMatrixf rejects a subscript",
                    parse_cmd_with_text("glMultMatrixf(A[0])", &cmd,
                                        cmd_text, sizeof(cmd_text)) == 0);
        memset(&cmd, 0, sizeof(cmd));
        ASSERT_TRUE("glMultMatrixf rejects two arrays",
                    parse_cmd_with_text("glMultMatrixf(A, B)", &cmd,
                                        cmd_text, sizeof(cmd_text)) == 0);
        memset(&cmd, 0, sizeof(cmd));
        ASSERT_TRUE("glMultMatrixf rejects a number",
                    parse_cmd_with_text("glMultMatrixf(1)", &cmd,
                                        cmd_text, sizeof(cmd_text)) == 0);
        memset(&cmd, 0, sizeof(cmd));
        ASSERT_TRUE("glMultMatrixf rejects an empty arg",
                    parse_cmd_with_text("glMultMatrixf()", &cmd,
                                        cmd_text, sizeof(cmd_text)) == 0);
    }

    /* glMultMatrixf, compound-literal form — 16 values inline. Unlike the
     * array form these are ordinary expressions, and they live in the
     * payload rather than args[] (which holds 8). */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        GLCmd cmd2;
        char cmd_text[MAX_LINE_LEN] = "";
        char cmd_text2[MAX_LINE_LEN] = "";
        ExprVar vars[1] = { { "shear", 0.25f } };
        int ok;

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_cmd_with_text(
            "glMultMatrixf((GLfloat[]){1, 0, 0, 0, 0.5, 1, 0, 0, "
            "0, 0, 1, 0, 2, 3, 4, 1})",
            &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glMultMatrixf literal parse ok", ok == 1);
        ASSERT_TRUE("glMultMatrixf literal type", cmd.type == CMD_MULT_MATRIXF);
        /* num_args 0 is the discriminator the flatten/export paths read —
         * there is no array behind this form. */
        ASSERT_TRUE("glMultMatrixf literal num_args", cmd.num_args == 0);
        ASSERT_TRUE("glMultMatrixf literal is not the array form",
                    !repl_cmd_mult_matrix_from_array(&cmd));
        ASSERT_TRUE("glMultMatrixf literal cells land in the payload",
                    cmd.payload.matrix.m[0] == 1.0f &&
                    cmd.payload.matrix.m[4] == 0.5f &&
                    cmd.payload.matrix.m[12] == 2.0f &&
                    cmd.payload.matrix.m[14] == 4.0f &&
                    cmd.payload.matrix.m[15] == 1.0f);
        ASSERT_TRUE("glMultMatrixf literal has_vars false", cmd.has_vars == 0);

        ok = parse_cmd_with_text(cmd_text, &cmd2, cmd_text2, sizeof(cmd_text2));
        ASSERT_TRUE("glMultMatrixf literal canonical re-parses", ok == 1);
        ASSERT_TRUE("glMultMatrixf literal canonical text stable",
                    strcmp(cmd_text, cmd_text2) == 0);

        /* Flat shorthand rewrites to the compound-literal form, matching
         * glClipPlane / glFogfv / glMaterialfv. */
        memset(&cmd, 0, sizeof(cmd));
        ok = parse_cmd_with_text(
            "glMultMatrixf(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)",
            &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glMultMatrixf flat parse ok", ok == 1);
        ASSERT_TRUE("glMultMatrixf flat canonical emits compound literal",
                    strstr(cmd_text,
                           "glMultMatrixf((GLfloat[]){1, 0, 0, 0, 0, 1, 0, 0, "
                           "0, 0, 1, 0, 0, 0, 0, 1});") != NULL);

        /* Cells are expressions: a variable-backed one bakes its value and
         * marks the line for re-evaluation. */
        memset(&cmd, 0, sizeof(cmd));
        ok = parse_for_test_with_vars(
            "glMultMatrixf((GLfloat[]){1, 0, 0, 0, shear, 1, 0, 0, "
            "0, 0, 1, 0, 0, 0, 0, 1})",
            &cmd, vars, 1);
        ASSERT_TRUE("glMultMatrixf literal var parse ok", ok == 1);
        ASSERT_TRUE("glMultMatrixf literal var has_vars", cmd.has_vars == 1);
        ASSERT_TRUE("glMultMatrixf literal var value baked",
                    cmd.payload.matrix.m[4] == 0.25f);

        memset(&cmd, 0, sizeof(cmd));
        ASSERT_TRUE("glMultMatrixf rejects 15 cells",
                    parse_cmd_with_text(
                        "glMultMatrixf((GLfloat[]){1, 0, 0, 0, 0, 1, 0, 0, "
                        "0, 0, 1, 0, 0, 0, 1})",
                        &cmd, cmd_text, sizeof(cmd_text)) == 0);
        memset(&cmd, 0, sizeof(cmd));
        ASSERT_TRUE("glMultMatrixf rejects 17 cells",
                    parse_cmd_with_text(
                        "glMultMatrixf((GLfloat[]){1, 0, 0, 0, 0, 1, 0, 0, "
                        "0, 0, 1, 0, 0, 0, 0, 1, 1})",
                        &cmd, cmd_text, sizeof(cmd_text)) == 0);
        memset(&cmd, 0, sizeof(cmd));
        ASSERT_TRUE("glMultMatrixf rejects an unclosed literal",
                    parse_cmd_with_text(
                        "glMultMatrixf((GLfloat[]){1, 0, 0, 0)",
                        &cmd, cmd_text, sizeof(cmd_text)) == 0);
    }

    /* glClipPlane */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        ExprVar vars[1] = { { "cut", 0.5f } };
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test_with_vars(
            "glClipPlane(GL_CLIP_PLANE0, (GLdouble[]){0, 1, 0, 0.5})",
            &cmd, vars, 1);
        ASSERT_TRUE("glClipPlane parse ok", ok == 1);
        ASSERT_TRUE("glClipPlane type", cmd.type == CMD_CLIP_PLANE);
        ASSERT_TRUE("glClipPlane num_args", cmd.num_args == 5);
        ASSERT_TRUE("glClipPlane plane enum",
                    (GLenum)cmd.args[0] == GL_CLIP_PLANE0);
        ASSERT_TRUE("glClipPlane equation baked",
                    cmd.args[1] == 0.0f && cmd.args[2] == 1.0f &&
                    cmd.args[3] == 0.0f && cmd.args[4] == 0.5f);
        ASSERT_TRUE("glClipPlane literal has_vars false", cmd.has_vars == 0);

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_for_test_with_vars(
            "glClipPlane(GL_CLIP_PLANE1, (GLdouble[]){0, 1, 0, cut})",
            &cmd, vars, 1);
        ASSERT_TRUE("glClipPlane var parse ok", ok == 1);
        ASSERT_TRUE("glClipPlane var has_vars", cmd.has_vars == 1);
        ASSERT_TRUE("glClipPlane var value baked", cmd.args[4] == 0.5f);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        GLCmd cmd2;
        char cmd_text[MAX_LINE_LEN] = "";
        char cmd_text2[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        memset(&cmd2, 0, sizeof(cmd2));
        int ok = parse_cmd_with_text(
            "glClipPlane(GL_CLIP_PLANE0, 0, 1, 0, 0.5)",
            &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glClipPlane flat parse ok", ok == 1);
        ASSERT_TRUE("glClipPlane flat canonical emits compound literal",
                    strstr(cmd_text,
                           "glClipPlane(GL_CLIP_PLANE0, (GLdouble[]){0, 1, 0, 0.5})")
                        != NULL);

        ok = parse_cmd_with_text(cmd_text, &cmd2, cmd_text2, sizeof(cmd_text2));
        ASSERT_TRUE("glClipPlane canonical re-parses", ok == 1);
        ASSERT_TRUE("glClipPlane canonical type", cmd2.type == CMD_CLIP_PLANE);
        ASSERT_TRUE("glClipPlane canonical num_args", cmd2.num_args == 5);
        ASSERT_TRUE("glClipPlane canonical text stable",
                    strcmp(cmd_text, cmd_text2) == 0);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glClipPlane(GL_FRONT, 0, 1, 0, 0)", &cmd);
        ASSERT_TRUE("glClipPlane bad plane token rejected", ok == 0);

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_for_test("glClipPlane(GL_CLIP_PLANE0, 0, 1, 0)", &cmd);
        ASSERT_TRUE("glClipPlane wrong coeff count rejected", ok == 0);

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_for_test(
            "glClipPlane(GL_CLIP_PLANE0, (GLdouble[]){0, 1, 0, 0.5} junk)",
            &cmd);
        ASSERT_TRUE("glClipPlane trailing junk rejected", ok == 0);
    }

    /* glFogi - table-driven pname/mode enums */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glFogi(GL_FOG_MODE, GL_EXP2)",
                                     &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glFogi parse ok", ok == 1);
        ASSERT_TRUE("glFogi type", cmd.type == CMD_FOG_I);
        ASSERT_TRUE("glFogi pname", (GLenum)cmd.args[0] == GL_FOG_MODE);
        ASSERT_TRUE("glFogi mode", (GLenum)cmd.args[1] == GL_EXP2);
        ASSERT_TRUE("glFogi source",
                    strstr(cmd_text, "glFogi(GL_FOG_MODE, GL_EXP2);") != NULL);

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_for_test("glFogi(GL_FOG_MODE, GL_LINEAR)", &cmd);
        ASSERT_TRUE("glFogi GL_LINEAR ok", ok == 1);
        ASSERT_TRUE("glFogi GL_LINEAR mode", (GLenum)cmd.args[1] == GL_LINEAR);

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_for_test("glFogi(GL_FOG_MODE, GL_EXP)", &cmd);
        ASSERT_TRUE("glFogi GL_EXP ok", ok == 1);
        ASSERT_TRUE("glFogi GL_EXP mode", (GLenum)cmd.args[1] == GL_EXP);

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_for_test("glFogi(GL_FOG_MODE, GL_LESS)", &cmd);
        ASSERT_TRUE("glFogi bad mode rejected", ok == 0);

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_for_test("glFogi(GL_FOG_DENSITY, GL_EXP)", &cmd);
        ASSERT_TRUE("glFogi bad pname rejected", ok == 0);
    }

    /* glFogf - enum pname + expression value */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        ExprVar vars[1] = { { "density", 0.25f } };
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glFogf(GL_FOG_DENSITY, 0.12)",
                                     &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glFogf parse ok", ok == 1);
        ASSERT_TRUE("glFogf type", cmd.type == CMD_FOG_F);
        ASSERT_TRUE("glFogf num_args", cmd.num_args == 2);
        ASSERT_TRUE("glFogf pname", (GLenum)cmd.args[0] == GL_FOG_DENSITY);
        ASSERT_TRUE("glFogf value baked", cmd.args[1] == 0.12f);
        ASSERT_TRUE("glFogf source",
                    strstr(cmd_text, "glFogf(GL_FOG_DENSITY, 0.12);") != NULL);

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_for_test("glFogf(GL_FOG_START, 2)", &cmd);
        ASSERT_TRUE("glFogf GL_FOG_START ok", ok == 1);
        ASSERT_TRUE("glFogf GL_FOG_START pname",
                    (GLenum)cmd.args[0] == GL_FOG_START);

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_for_test("glFogf(GL_FOG_END, 30)", &cmd);
        ASSERT_TRUE("glFogf GL_FOG_END ok", ok == 1);
        ASSERT_TRUE("glFogf GL_FOG_END pname",
                    (GLenum)cmd.args[0] == GL_FOG_END);

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_for_test_with_vars("glFogf(GL_FOG_DENSITY, density)",
                                      &cmd, vars, 1);
        ASSERT_TRUE("glFogf var parse ok", ok == 1);
        ASSERT_TRUE("glFogf var has_vars", cmd.has_vars == 1);
        ASSERT_TRUE("glFogf var value baked", cmd.args[1] == 0.25f);

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_for_test("glFogf(GL_FOG_MODE, 1)", &cmd);
        ASSERT_TRUE("glFogf rejects GL_FOG_MODE (glFogi owns it)", ok == 0);

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_for_test("glFogf(GL_FOG_DENSITY, 1, 2)", &cmd);
        ASSERT_TRUE("glFogf extra arg rejected", ok == 0);
    }

    /* glFogfv - compound-literal and flat shorthand forms */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        GLCmd cmd2;
        char cmd_text[MAX_LINE_LEN] = "";
        char cmd_text2[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        memset(&cmd2, 0, sizeof(cmd2));
        int ok = parse_cmd_with_text(
            "glFogfv(GL_FOG_COLOR, (GLfloat[]){0.1, 0.2, 0.3, 1})",
            &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glFogfv parse ok", ok == 1);
        ASSERT_TRUE("glFogfv type", cmd.type == CMD_FOG_FV);
        ASSERT_TRUE("glFogfv num_args", cmd.num_args == 5);
        ASSERT_TRUE("glFogfv pname", (GLenum)cmd.args[0] == GL_FOG_COLOR);
        ASSERT_TRUE("glFogfv color baked",
                    cmd.args[1] == 0.1f && cmd.args[2] == 0.2f &&
                    cmd.args[3] == 0.3f && cmd.args[4] == 1.0f);

        /* Flat shorthand canonicalizes to the compound literal and the
         * canonical text is stable across a re-parse. */
        memset(&cmd, 0, sizeof(cmd));
        ok = parse_cmd_with_text("glFogfv(GL_FOG_COLOR, 0.1, 0.2, 0.3, 1)",
                                 &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glFogfv flat parse ok", ok == 1);
        ASSERT_TRUE("glFogfv flat canonical emits compound literal",
                    strstr(cmd_text,
                           "glFogfv(GL_FOG_COLOR, (GLfloat[]){0.1, 0.2, 0.3, 1})")
                        != NULL);
        ok = parse_cmd_with_text(cmd_text, &cmd2, cmd_text2, sizeof(cmd_text2));
        ASSERT_TRUE("glFogfv canonical re-parses", ok == 1);
        ASSERT_TRUE("glFogfv canonical text stable",
                    strcmp(cmd_text, cmd_text2) == 0);

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_for_test("glFogfv(GL_FOG_DENSITY, 0.1, 0.2, 0.3, 1)", &cmd);
        ASSERT_TRUE("glFogfv bad pname rejected", ok == 0);

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_for_test("glFogfv(GL_FOG_COLOR, 0.1, 0.2, 0.3)", &cmd);
        ASSERT_TRUE("glFogfv wrong channel count rejected", ok == 0);

        memset(&cmd, 0, sizeof(cmd));
        ok = parse_for_test(
            "glFogfv(GL_FOG_COLOR, (GLfloat[]){0.1, 0.2, 0.3, 1} junk)",
            &cmd);
        ASSERT_TRUE("glFogfv trailing junk rejected", ok == 0);
    }

    /* Incomplete commands should not be reported as unknown commands. */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glColor3f(1, 1", &cmd);
        ASSERT_TRUE("incomplete glColor3f returns 0", ok == 0);
        assert_status_contains("incomplete glColor3f status", "Incomplete command");
        assert_status_contains("incomplete glColor3f missing paren", "missing ')'");
        ASSERT_TRUE("incomplete glColor3f not unknown",
                    strstr(g_status, "Unknown cmd") == NULL);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glVertex(1,", &cmd);
        ASSERT_TRUE("incomplete glVertex returns 0", ok == 0);
        assert_status_contains("incomplete glVertex status", "Incomplete command");
        assert_status_contains("incomplete glVertex missing paren", "missing ')'");
        ASSERT_TRUE("incomplete glVertex not unknown",
                    strstr(g_status, "Unknown cmd") == NULL);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glColor3f(1, 1)", &cmd);
        ASSERT_TRUE("short glColor3f returns 0", ok == 0);
        assert_status_contains("short glColor3f status", "Incomplete command");
        assert_status_contains("short glColor3f expected count", "expects 3 arguments");
        ASSERT_TRUE("short glColor3f not unknown",
                    strstr(g_status, "Unknown cmd") == NULL);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glColor3f(1,, 3)", &cmd);
        ASSERT_TRUE("malformed glColor3f returns 0", ok == 0);
        assert_status_contains("malformed glColor3f incomplete", "Incomplete command");
        ASSERT_TRUE("malformed glColor3f not unknown",
                    strstr(g_status, "Unknown cmd") == NULL);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glTotallyUnknown(1, 2, 3)", &cmd);
        ASSERT_TRUE("complete unknown command returns 0", ok == 0);
        assert_status_contains("complete unknown command status", "Unknown cmd");
    }

    /* Top-level math expressions get a tailored error instead of the
     * generic "Unknown cmd" — typing `rand()` or `sin(t)` as a
     * statement is a common mistake (the result has nowhere to go). */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("rand()", &cmd);
        ASSERT_TRUE("rand() returns 0", ok == 0);
        assert_status_contains("rand() status names function", "rand");
        assert_status_contains("rand() status hints expression", "expression");
        ASSERT_TRUE("rand() status not generic",
                    strstr(g_status, "Unknown cmd") == NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("sin(t)", &cmd);
        ASSERT_TRUE("sin(t) returns 0", ok == 0);
        assert_status_contains("sin(t) status names function", "sin");
        assert_status_contains("sin(t) status hints assign", "x = sin");
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glTotallyUnknown(1, 2", &cmd);
        ASSERT_TRUE("unterminated unknown command returns 0", ok == 0);
        assert_status_contains("unterminated unknown command status", "Unknown cmd");
        ASSERT_TRUE("unterminated unknown command not incomplete",
                    strstr(g_status, "Incomplete command") == NULL);
    }

    /* gluBegin/gluEnd via parse_command */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("gluBegin(GLU_POLYGON)", &cmd);
        ASSERT_TRUE("gluBegin POLYGON parse ok", ok == 1);
        ASSERT_TRUE("gluBegin POLYGON type", cmd.type == CMD_TESS_BEGIN_POLYGON);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("gluBegin(GLU_CONTOUR)", &cmd);
        ASSERT_TRUE("gluBegin CONTOUR parse ok", ok == 1);
        ASSERT_TRUE("gluBegin CONTOUR type", cmd.type == CMD_TESS_BEGIN_CONTOUR);
    }

    /* Stencil state — custom mixed enum/expression/literal forms plus the
     * generalized three-enum glStencilOp path. */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glStencilFunc(GL_EQUAL, 3.9, 0xff)",
                                     &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glStencilFunc parse ok", ok == 1);
        ASSERT_TRUE("glStencilFunc type", cmd.type == CMD_STENCIL_FUNC);
        ASSERT_TRUE("glStencilFunc ref truncates", (int)cmd.args[1] == 3);
        ASSERT_TRUE("glStencilFunc hex mask", (int)cmd.args[2] == 255);
        ASSERT_TRUE("glStencilFunc canonical hex", strstr(cmd_text, "0xFF") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glStencilFunc(GL_EQUAL, 256, 0xFF)", &cmd);
        ASSERT_TRUE("glStencilFunc ref over limit rejected", ok == 0);
        assert_status_contains("glStencilFunc ref range message", "0..255");
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glStencilOp(GL_KEEP, GL_ZERO, GL_REPLACE)",
                                     &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glStencilOp parse ok", ok == 1);
        ASSERT_TRUE("glStencilOp type", cmd.type == CMD_STENCIL_OP);
        ASSERT_TRUE("glStencilOp source canonicalized",
                    strstr(cmd_text, "glStencilOp(GL_KEEP, GL_ZERO, GL_REPLACE);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glStencilMask(127)", &cmd,
                                     cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glStencilMask parse ok", ok == 1);
        ASSERT_TRUE("glStencilMask type", cmd.type == CMD_STENCIL_MASK);
        ASSERT_TRUE("glStencilMask canonical hex", strstr(cmd_text, "0x7F") != NULL);
    }

    /* glDepthMask - bool-enum state command */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glDepthMask(GL_FALSE)", &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glDepthMask(GL_FALSE) parse ok", ok == 1);
        ASSERT_TRUE("glDepthMask type", cmd.type == CMD_DEPTH_MASK);
        ASSERT_TRUE("glDepthMask mode GL_FALSE", (GLenum)cmd.args[0] == GL_FALSE);
        ASSERT_TRUE("glDepthMask source canonicalized",
                    strstr(cmd_text, "glDepthMask(GL_FALSE);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glDepthMask(GL_TRUE)", &cmd);
        ASSERT_TRUE("glDepthMask(GL_TRUE) parse ok", ok == 1);
        ASSERT_TRUE("glDepthMask GL_TRUE mode", (GLenum)cmd.args[0] == GL_TRUE);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        /* Bool-slot policy: glDepthMask is ENUM_OR_CONST_VALUE, so a
         * constant 0/1 canonicalizes to GL_FALSE/GL_TRUE instead of
         * being rejected. */
        char cmd_text[MAX_LINE_LEN] = "";
        int ok = parse_cmd_with_text("glDepthMask(1)", &cmd,
                                     cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glDepthMask(1) parse ok", ok == 1);
        ASSERT_TRUE("glDepthMask(1) -> GL_TRUE", (GLenum)cmd.args[0] == GL_TRUE);
        ASSERT_TRUE("glDepthMask(1) canonicalized",
                    strstr(cmd_text, "glDepthMask(GL_TRUE);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glDepthMask(0)", &cmd,
                                     cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glDepthMask(0) parse ok", ok == 1);
        ASSERT_TRUE("glDepthMask(0) -> GL_FALSE", (GLenum)cmd.args[0] == GL_FALSE);
        ASSERT_TRUE("glDepthMask(0) canonicalized",
                    strstr(cmd_text, "glDepthMask(GL_FALSE);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        declare_test_vars();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        /* Runtime vars are rejected in bool-mask slots (would silently
         * collapse under flatten/replay/export). */
        int ok = parse_for_test("glDepthMask(x)", &cmd);
        ASSERT_TRUE("glDepthMask(var) rejected", ok == 0);
    }

    /* glClear - the ENUM_BITFIELD mask slot */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glClear(GL_COLOR_BUFFER_BIT)", &cmd,
                                     cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glClear(one bit) parse ok", ok == 1);
        ASSERT_TRUE("glClear type", cmd.type == CMD_CLEAR);
        ASSERT_TRUE("glClear num_args", cmd.num_args == 1);
        ASSERT_TRUE("glClear single-bit mask",
                    (GLbitfield)cmd.args[0] == GL_COLOR_BUFFER_BIT);
        ASSERT_TRUE("glClear single-bit canonicalized",
                    strstr(cmd_text, "glClear(GL_COLOR_BUFFER_BIT);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)",
                                     &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glClear(both bits) parse ok", ok == 1);
        ASSERT_TRUE("glClear both-bit mask",
                    (GLbitfield)cmd.args[0] ==
                        (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
        ASSERT_TRUE("glClear both-bit canonicalized",
                    strstr(cmd_text,
                           "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);") != NULL);
    }
    {
        /* Emission is table order with duplicates dropped, so any
         * spelling of a mask has exactly one canonical text. */
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glClear(GL_DEPTH_BUFFER_BIT|GL_COLOR_BUFFER_BIT)",
                                     &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glClear(reversed, unspaced) parse ok", ok == 1);
        ASSERT_TRUE("glClear reversed mask",
                    (GLbitfield)cmd.args[0] ==
                        (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
        ASSERT_TRUE("glClear reversed emits in table order",
                    strstr(cmd_text,
                           "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glClear(GL_COLOR_BUFFER_BIT | GL_COLOR_BUFFER_BIT)",
                                     &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glClear(repeated bit) parse ok", ok == 1);
        ASSERT_TRUE("glClear repeated bit deduped",
                    strstr(cmd_text, "glClear(GL_COLOR_BUFFER_BIT);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glClear(GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)",
                                     &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glClear stencil bit parse ok", ok == 1);
        ASSERT_TRUE("glClear stencil bit resolves",
                    ((GLbitfield)cmd.args[0] & GL_STENCIL_BUFFER_BIT) != 0);
        ASSERT_TRUE("glClear stencil bit table order",
                    strstr(cmd_text,
                           "glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        declare_test_vars();
        GLCmd cmd;
        /* A mask is not a quantity: numeric literals (even the right
         * value), unlisted bits, expressions and empty terms all
         * reject rather than resolve. */
        static const char *k_bad[] = {
            "glClear(16640)",
            "glClear(GL_COLOR_BUFFER_BIT + GL_DEPTH_BUFFER_BIT)",
            "glClear(GL_ALL_ATTRIB_BITS)",
            "glClear(GL_ACCUM_BUFFER_BIT)",
            "glClear(GL_LIGHTING)",
            "glClear(x)",
            "glClear()",
            "glClear(GL_COLOR_BUFFER_BIT | )",
            "glClear(| GL_COLOR_BUFFER_BIT)",
            "glClear(GL_COLOR_BUFFER_BIT || GL_DEPTH_BUFFER_BIT)",
        };
        for (int i = 0; i < (int)(sizeof(k_bad) / sizeof(k_bad[0])); i++) {
            memset(&cmd, 0, sizeof(cmd));
            ASSERT_TRUE(k_bad[i], parse_for_test(k_bad[i], &cmd) == 0);
        }
    }

    /* glPushAttrib - the second ENUM_BITFIELD mask slot (shares glClear's
     * parser branch: |-parse, canonical table order, dupe-drop, expression
     * rejection). glPopAttrib is the zero-arg partner. */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glPushAttrib(GL_CURRENT_BIT)", &cmd,
                                     cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glPushAttrib(one bit) parse ok", ok == 1);
        ASSERT_TRUE("glPushAttrib type", cmd.type == CMD_PUSH_ATTRIB);
        ASSERT_TRUE("glPushAttrib num_args", cmd.num_args == 1);
        ASSERT_TRUE("glPushAttrib single-bit mask",
                    (GLbitfield)cmd.args[0] == GL_CURRENT_BIT);
        ASSERT_TRUE("glPushAttrib single-bit canonicalized",
                    strstr(cmd_text, "glPushAttrib(GL_CURRENT_BIT);") != NULL);
    }
    {
        /* GL_ALL_ATTRIB_BITS is a REPL-level alias for the union of every
         * modeled group, not the platform's broader GL value. */
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glPushAttrib(GL_ALL_ATTRIB_BITS)", &cmd,
                                     cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glPushAttrib(all alias) parse ok", ok == 1);
        ASSERT_TRUE("glPushAttrib all alias resolves supported union",
                    (GLbitfield)cmd.args[0] ==
                        (GL_CURRENT_BIT | GL_POINT_BIT | GL_LINE_BIT |
                         GL_POLYGON_BIT | GL_LIGHTING_BIT | GL_FOG_BIT |
                         GL_DEPTH_BUFFER_BIT | GL_TRANSFORM_BIT |
                         GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT));
        ASSERT_TRUE("glPushAttrib all alias canonicalized compactly",
                    strstr(cmd_text,
                           "glPushAttrib(GL_ALL_ATTRIB_BITS);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text(
            "glPushAttrib(GL_ALL_ATTRIB_BITS | GL_FOG_BIT)",
            &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glPushAttrib(all alias OR bit) parse ok", ok == 1);
        ASSERT_TRUE("glPushAttrib all alias OR bit dedupes to alias",
                    strstr(cmd_text,
                           "glPushAttrib(GL_ALL_ATTRIB_BITS);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glPushAttrib(GL_CURRENT_BIT | GL_LINE_BIT)",
                                     &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glPushAttrib(two bits) parse ok", ok == 1);
        ASSERT_TRUE("glPushAttrib two-bit mask",
                    (GLbitfield)cmd.args[0] ==
                        (GL_CURRENT_BIT | GL_LINE_BIT));
        ASSERT_TRUE("glPushAttrib two-bit canonicalized",
                    strstr(cmd_text,
                           "glPushAttrib(GL_CURRENT_BIT | GL_LINE_BIT);") != NULL);
    }
    {
        /* Regression: one enum argument used to have a 63-character text
         * ceiling, so four long, valid bit names failed before resolution. */
        static const char *k_four_long =
            "glPushAttrib(GL_CURRENT_BIT | GL_LIGHTING_BIT | "
            "GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT)";
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text(k_four_long, &cmd,
                                     cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glPushAttrib(four long bits) parse ok", ok == 1);
        ASSERT_TRUE("glPushAttrib four-long-bit mask",
                    (GLbitfield)cmd.args[0] ==
                        (GL_CURRENT_BIT | GL_LIGHTING_BIT |
                         GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT));
    }
    {
        static const char *k_all_ten =
            "glPushAttrib(GL_CURRENT_BIT | GL_POINT_BIT | GL_LINE_BIT | "
            "GL_POLYGON_BIT | GL_LIGHTING_BIT | GL_FOG_BIT | "
            "GL_DEPTH_BUFFER_BIT | GL_TRANSFORM_BIT | GL_ENABLE_BIT | "
            "GL_COLOR_BUFFER_BIT)";
        static const char *k_all_ten_canonical =
            "glPushAttrib(GL_ALL_ATTRIB_BITS);";
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text(k_all_ten, &cmd,
                                     cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glPushAttrib(all ten bits) parse ok", ok == 1);
        ASSERT_TRUE("glPushAttrib all-ten-bit mask",
                    (GLbitfield)cmd.args[0] ==
                        (GL_CURRENT_BIT | GL_POINT_BIT | GL_LINE_BIT |
                         GL_POLYGON_BIT | GL_LIGHTING_BIT | GL_FOG_BIT |
                         GL_DEPTH_BUFFER_BIT | GL_TRANSFORM_BIT |
                         GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT));
        ASSERT_TRUE("glPushAttrib all ten bits canonicalize to alias",
                    strstr(cmd_text, k_all_ten_canonical) != NULL);
    }
    {
        /* Reversed, unspaced input emits in canonical table order (ascending
         * GL value), so a mask has exactly one canonical spelling. */
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glPushAttrib(GL_ENABLE_BIT|GL_LIGHTING_BIT)",
                                     &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glPushAttrib(reversed, unspaced) parse ok", ok == 1);
        ASSERT_TRUE("glPushAttrib reversed mask",
                    (GLbitfield)cmd.args[0] ==
                        (GL_LIGHTING_BIT | GL_ENABLE_BIT));
        ASSERT_TRUE("glPushAttrib reversed emits in table order",
                    strstr(cmd_text,
                           "glPushAttrib(GL_LIGHTING_BIT | GL_ENABLE_BIT);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glPushAttrib(GL_ENABLE_BIT | GL_ENABLE_BIT)",
                                     &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glPushAttrib(repeated bit) parse ok", ok == 1);
        ASSERT_TRUE("glPushAttrib repeated bit deduped",
                    strstr(cmd_text, "glPushAttrib(GL_ENABLE_BIT);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        declare_test_vars();
        GLCmd cmd;
        /* Same mask policy as glClear: numeric literals, unsupported bits,
         * expressions, variables and empty terms reject rather than resolve. */
        static const char *k_bad[] = {
            "glPushAttrib(1)",
            "glPushAttrib(GL_CURRENT_BIT + GL_LINE_BIT)",
            "glPushAttrib(GL_STENCIL_BUFFER_BIT)",
            "glPushAttrib(x)",
            "glPushAttrib()",
            "glPushAttrib(GL_CURRENT_BIT | )",
            "glPushAttrib(GL_CURRENT_BIT || GL_LINE_BIT)",
        };
        for (int i = 0; i < (int)(sizeof(k_bad) / sizeof(k_bad[0])); i++) {
            memset(&cmd, 0, sizeof(cmd));
            ASSERT_TRUE(k_bad[i], parse_for_test(k_bad[i], &cmd) == 0);
        }
    }
    {
        /* glPopAttrib: zero-arg partner (mirrors glEnd / glPopMatrix). */
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glPopAttrib()", &cmd,
                                     cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glPopAttrib() parse ok", ok == 1);
        ASSERT_TRUE("glPopAttrib type", cmd.type == CMD_POP_ATTRIB);
        ASSERT_TRUE("glPopAttrib canonicalized",
                    strstr(cmd_text, "glPopAttrib();") != NULL);
    }
    {
        /* Like glPopMatrix/glEnd, glPopAttrib ignores a stray payload rather
         * than rejecting it: it parses as CMD_POP_ATTRIB and the canonical
         * text drops the argument. */
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glPopAttrib(GL_CURRENT_BIT)", &cmd,
                                     cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glPopAttrib(stray arg) parses as pop", ok == 1);
        ASSERT_TRUE("glPopAttrib(stray arg) type", cmd.type == CMD_POP_ATTRIB);
        ASSERT_TRUE("glPopAttrib(stray arg) drops payload",
                    strstr(cmd_text, "glPopAttrib();") != NULL);
    }

    /* glEdgeFlag - bool-enum state command */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glEdgeFlag(GL_FALSE)", &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glEdgeFlag(GL_FALSE) parse ok", ok == 1);
        ASSERT_TRUE("glEdgeFlag type", cmd.type == CMD_EDGE_FLAG);
        ASSERT_TRUE("glEdgeFlag mode GL_FALSE", (GLenum)cmd.args[0] == GL_FALSE);
        ASSERT_TRUE("glEdgeFlag source canonicalized",
                    strstr(cmd_text, "glEdgeFlag(GL_FALSE);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glEdgeFlag(GL_TRUE)", &cmd);
        ASSERT_TRUE("glEdgeFlag(GL_TRUE) parse ok", ok == 1);
        ASSERT_TRUE("glEdgeFlag GL_TRUE mode", (GLenum)cmd.args[0] == GL_TRUE);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        char cmd_text[MAX_LINE_LEN] = "";
        int ok = parse_cmd_with_text("glEdgeFlag(1)", &cmd,
                                     cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glEdgeFlag(1) parse ok", ok == 1);
        ASSERT_TRUE("glEdgeFlag(1) -> GL_TRUE", (GLenum)cmd.args[0] == GL_TRUE);
        ASSERT_TRUE("glEdgeFlag(1) canonicalized",
                    strstr(cmd_text, "glEdgeFlag(GL_TRUE);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        declare_test_vars();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glEdgeFlag(x)", &cmd);
        ASSERT_TRUE("glEdgeFlag(var) rejected", ok == 0);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glDepthMask(undef_var)", &cmd);
        ASSERT_TRUE("glDepthMask(undef_var) rejected", ok == 0);
        assert_status_contains("glDepthMask(undef_var) undeclared status",
                               "undeclared variable 'undef_var'");
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        /* Unknown magic numbers still rejected (only 0/1 reverse-map). */
        int ok = parse_for_test("glDepthMask(2)", &cmd);
        ASSERT_TRUE("glDepthMask(2) rejected", ok == 0);
    }
    {
        /* Malformed numerics must NOT slip through the bool-slot
         * literal parse (the evaluator would fold "1+" -> 1 and
         * "1abc" -> 1, silently canonicalizing garbage to GL_TRUE). */
        const char *bad[] = {
            "glDepthMask(1+)", "glDepthMask(1abc)", "glDepthMask(1 2)",
            "glDepthMask()",
            "glColorMask(1+, 0, 1, 0)", "glColorMask(1, 0abc, 1, 0)",
        };
        for (size_t bi = 0; bi < sizeof(bad) / sizeof(bad[0]); bi++) {
            glr_ctrl_reset_all();
            GLCmd c;
            memset(&c, 0, sizeof(c));
            int r = parse_for_test(bad[bi], &c);
            ASSERT_TRUE("bool-slot rejects malformed numeric", r == 0);
        }
    }
    {
        /* Well-formed literals still accepted and canonicalized. */
        glr_ctrl_reset_all();
        GLCmd c;
        char t[MAX_LINE_LEN] = "";
        memset(&c, 0, sizeof(c));
        int r = parse_cmd_with_text("glDepthMask(1.0)", &c, t, sizeof(t));
        ASSERT_TRUE("glDepthMask(1.0) ok", r == 1);
        ASSERT_TRUE("glDepthMask(1.0) -> GL_TRUE", (GLenum)c.args[0] == GL_TRUE);
        ASSERT_TRUE("glDepthMask(1.0) canonicalized",
                    strstr(t, "glDepthMask(GL_TRUE);") != NULL);
    }

    /* glColorMask - 4 boolean-mask slots (the original ask) */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE)",
                                     &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glColorMask parse ok", ok == 1);
        ASSERT_TRUE("glColorMask type", cmd.type == CMD_COLOR_MASK);
        ASSERT_TRUE("glColorMask num_args", cmd.num_args == 4);
        ASSERT_TRUE("glColorMask r", (GLenum)cmd.args[0] == GL_TRUE);
        ASSERT_TRUE("glColorMask g", (GLenum)cmd.args[1] == GL_TRUE);
        ASSERT_TRUE("glColorMask b", (GLenum)cmd.args[2] == GL_TRUE);
        ASSERT_TRUE("glColorMask a", (GLenum)cmd.args[3] == GL_FALSE);
        ASSERT_TRUE("glColorMask symbolic round-trip",
                    strstr(cmd_text, "glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        /* Numeric 1/0 canonicalize to GL_TRUE/GL_FALSE (bool-slot policy). */
        int ok = parse_cmd_with_text("glColorMask(1, 0, 1, 0)",
                                     &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glColorMask(1,0,1,0) parse ok", ok == 1);
        ASSERT_TRUE("glColorMask(1,0,1,0) canonicalized",
                    strstr(cmd_text, "glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);") != NULL);
    }
    {
        glr_ctrl_reset_all();
        declare_test_vars();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        /* Runtime var in a bool-mask slot is rejected. */
        int ok = parse_for_test("glColorMask(x, GL_TRUE, GL_TRUE, GL_TRUE)", &cmd);
        ASSERT_TRUE("glColorMask(var,...) rejected", ok == 0);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        /* Wrong arg count is rejected. */
        int ok = parse_for_test("glColorMask(GL_TRUE, GL_TRUE)", &cmd);
        ASSERT_TRUE("glColorMask wrong arg count rejected", ok == 0);
    }

    /* ENUM_ONLY behavior-neutral guard: strict enum slots did NOT get
     * numeric fallback from the refactor. A raw enum number stays
     * rejected exactly as before. */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glEnable(3553)", &cmd);
        ASSERT_TRUE("glEnable(3553) still rejected", ok == 0);
    }
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glShadeModel(7425)", &cmd);
        ASSERT_TRUE("glShadeModel(numeric) still rejected", ok == 0);
    }

    /* Contract: every parseable enum-spec row fills args[0..num_args-1]
     * and sets num_args == abs(spec.num_args). Proves uniform args[]
     * storage across the whole enum table, command-independently. */
    {
        struct { const char *line; int n; } cases[] = {
            { "glBegin(GL_TRIANGLES)",                                  1 },
            { "glEnable(GL_DEPTH_TEST)",                                1 },
            { "glDisable(GL_BLEND)",                                    1 },
            { "glShadeModel(GL_FLAT)",                                  1 },
            { "glFrontFace(GL_CW)",                                     1 },
            { "glDepthMask(GL_TRUE)",                                   1 },
            { "glBlendFunc(GL_SRC_ALPHA, GL_ONE)",                      2 },
            { "glColorMaterial(GL_FRONT, GL_DIFFUSE)",                  2 },
            { "glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE)",        2 },
            { "glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE)",      4 },
        };
        for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
            glr_ctrl_reset_all();
            GLCmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            int ok = parse_for_test(cases[ci].line, &cmd);
            ASSERT_TRUE("enum-spec contract parse ok", ok == 1);
            ASSERT_TRUE("enum-spec contract num_args",
                        cmd.num_args == cases[ci].n);
        }
    }

    /* whitespace + semicolon trimming stays predictable */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("   glColor3f( 1 , 2 , 3 )   ;   ",
                                      &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("trimmed glColor3f parse ok", ok == 1);
        ASSERT_TRUE("trimmed glColor3f type", cmd.type == CMD_COLOR3F);
        ASSERT_TRUE("trimmed glColor3f source normalized",
                    strstr(cmd_text, "glColor3f(1, 2, 3);") != NULL);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glColor3f(1 + 2, sin(0.5), -3)",
                                      &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("expr glColor3f parse ok", ok == 1);
        ASSERT_TRUE("expr glColor3f type", cmd.type == CMD_COLOR3F);
        ASSERT_TRUE("expr glColor3f source canonicalized",
                    strstr(cmd_text, "glColor3f(") != NULL);
        ASSERT_TRUE("expr glColor3f computes first arg", cmd.args[0] == 3.0f);
    }

    /* zero-arg commands can be written with or without () */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glPushMatrix", &cmd);
        ASSERT_TRUE("glPushMatrix without parens parse ok", ok == 1);
        ASSERT_TRUE("glPushMatrix without parens type", cmd.type == CMD_PUSH_MATRIX);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glPopMatrix()", &cmd);
        ASSERT_TRUE("glPopMatrix with parens parse ok", ok == 1);
        ASSERT_TRUE("glPopMatrix with parens type", cmd.type == CMD_POP_MATRIX);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glLoadIdentity()", &cmd);
        ASSERT_TRUE("glLoadIdentity with parens parse ok", ok == 1);
        ASSERT_TRUE("glLoadIdentity with parens type", cmd.type == CMD_LOAD_IDENTITY);
    }

    /* fixed-arity command with extra args should be rejected predictably */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glColor3f(1, 2, 3, 4)", &cmd);
        ASSERT_TRUE("long glColor3f returns 0", ok == 0);
        ASSERT_TRUE("long glColor3f reports usage", strstr(g_status, "Usage:") != NULL);
        ASSERT_TRUE("long glColor3f not incomplete",
                    strstr(g_status, "Incomplete command") == NULL);
    }

    /* undeclared identifier should fail when visible vars are provided */
    {
        ExprVar vars[1] = { { "radius", 2.0f } };
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test_with_vars("glVertex3f(unknown_name, 0, 0)", &cmd, vars, 1);
        ASSERT_TRUE("unknown local var returns 0", ok == 0);
        ASSERT_TRUE("unknown local var message",
                    strstr(g_status, "undeclared variable 'unknown_name'") != NULL);
    }

    /* function calls: empty arglist valid, malformed arglist invalid */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("func2()", &cmd);
        ASSERT_TRUE("func2 empty arglist parse ok", ok == 1);
        ASSERT_TRUE("func2 empty arglist type", cmd.type == CMD_CALL);
        ASSERT_TRUE("func2 empty arglist num_args", cmd.num_args == 0);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("func2(1,)", &cmd);
        ASSERT_TRUE("func2 malformed arglist returns 0", ok == 0);
        ASSERT_TRUE("func2 malformed arglist status",
                    strstr(g_status, "Invalid function call arguments") != NULL);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("func2(1 + sin(2), max(3, 4))",
                                      &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("func2 nested args parse ok", ok == 1);
        ASSERT_TRUE("func2 nested args type", cmd.type == CMD_CALL);
        ASSERT_TRUE("func2 nested args preserves nested expr",
                    strstr(cmd_text, "max(3, 4)") != NULL);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("func2(1 2)", &cmd);
        ASSERT_TRUE("func2 missing comma invalid", ok == 0);
        ASSERT_TRUE("func2 missing comma status",
                    strstr(g_status, "Invalid function call arguments") != NULL);
    }

    /* supported arities for specialized commands */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glMaterialfv(GL_FRONT, GL_AMBIENT, 0.1, 0.2, 0.3)", &cmd);
        ASSERT_TRUE("glMaterialfv 3-float vector invalid", ok == 0);
        ASSERT_TRUE("glMaterialfv 3-float status", strstr(g_status, "Expected 1 or 4 float values") != NULL);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 1, 0)", &cmd);
        ASSERT_TRUE("glPointParameterfv short arglist invalid", ok == 0);
        ASSERT_TRUE("glPointParameterfv short arglist status",
                    strstr(g_status, "Expected 3 floats") != NULL);
    }

    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glPointParameterfv(GL_POINT_FADE_THRESHOLD_SIZE, 1, 0, 0)", &cmd);
        ASSERT_TRUE("glPointParameterfv bad pname invalid", ok == 0);
        ASSERT_TRUE("glPointParameterfv bad pname status",
                    strstr(g_status, "GL_POINT_DISTANCE_ATTENUATION") != NULL);
    }

    /* known command prefix + missing close paren remains "incomplete", even with ';' */
    {
        glr_ctrl_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glTranslatef(1, 2, 3;", &cmd);
        ASSERT_TRUE("unterminated known command returns 0", ok == 0);
        ASSERT_TRUE("unterminated known command incomplete",
                    strstr(g_status, "Incomplete command") != NULL);
        ASSERT_TRUE("unterminated known command not unknown",
                    strstr(g_status, "Unknown cmd") == NULL);
    }

    /* funcN(...) with args whose canonicalized source exceeds MAX_LINE_LEN
     * fails parse with "Command too long" rather than silently truncating.
     *
     * parse_command truncates its input at MAX_LINE_LEN-1 (255) chars, so the
     * line itself must stay under that limit while still producing a
     * canonicalized "  funcN(args);" that overflows GLCmd.source (256). */
    {
        glr_ctrl_reset_all();
        g_status[0] = '\0';

        /* 13 numeric args of 18 chars separated by ',' → 246 chars of args.
         * Full input "func0(<246>)" = 253 chars (fits pre-truncation).
         * Canonical "  func0(<246>);" = 256 chars - one past the buffer. */
        char line[MAX_LINE_LEN];
        int off = snprintf(line, sizeof(line), "func0(");
        for (int i = 0; i < 13; i++) {
            off += snprintf(line + off, sizeof(line) - off,
                            i == 0 ? "123456789012345678"
                                   : ",123456789012345678");
        }
        snprintf(line + off, sizeof(line) - off, ")");

        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test(line, &cmd);
        ASSERT_TRUE("overlong funcN: parse fails", ok == 0);
        assert_status_contains("overlong funcN: status", "Command too long");
    }

    /* Canonical-text float round-trip: a constant expression folds at
     * parse time and the canonical text re-prints the folded value; that
     * text is the command's only source of record (flatten force_reparse
     * re-evaluates it, save/load round-trips it), so re-parsing it must
     * reproduce the committed arg bits exactly. 0.92f*0.55f is a
     * discriminator: its product is NOT the float nearest "0.506", so
     * the old 6-digit "%g" emit broke the round trip. Covers the std
     * table plus every hand-written arg emitter. */
    {
        static const char *const lines[] = {
            "glColor3f(0.92*0.55, 0.95*0.55, 0.98*0.55);",
            "glTranslatef(1.25*cos(PI/2), 0.38, 1.25*sin(PI*7/6));",
            "gluColor(0.92*0.55, 0.2, 0.3);",
            "label(\"x %f\", 0.92*0.55);",
            "glMaterialfv(GL_FRONT, GL_DIFFUSE, 0.92*0.55, 0.2, 0.3, 1);",
            "glMaterialf(GL_FRONT, GL_SHININESS, 0.92*0.55*128);",
            "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 0.92*0.55, 0, 0.01*3);",
            "glClipPlane(GL_CLIP_PLANE0, 0.92*0.55, 0, 1, 0.1*3);",
            /* Shared formatter boundaries: fixed-point 1e38f exceeds the
             * 32-byte source-float buffer, and -0 must keep its sign bit. */
            "glVertex3f(1e38, 0, 0);",
            "glVertex3f(-0, 0, 0);",
        };
        for (size_t li = 0; li < sizeof(lines) / sizeof(lines[0]); li++) {
            GLCmd a, b;
            char text[MAX_LINE_LEN];
            char label_buf[128];
            memset(&a, 0, sizeof(a));
            memset(&b, 0, sizeof(b));

            snprintf(label_buf, sizeof(label_buf),
                     "canonical float round-trip: parse [%s]", lines[li]);
            ASSERT_TRUE(label_buf,
                        parse_cmd_with_text(lines[li], &a, text,
                                            sizeof(text)) == 1);

            snprintf(label_buf, sizeof(label_buf),
                     "canonical float round-trip: reparse [%s]", lines[li]);
            ASSERT_TRUE(label_buf,
                        parse_cmd_with_text(text, &b, NULL, 0) == 1);

            snprintf(label_buf, sizeof(label_buf),
                     "canonical float round-trip: num_args [%s]", lines[li]);
            ASSERT_TRUE(label_buf, a.num_args == b.num_args);

            snprintf(label_buf, sizeof(label_buf),
                     "canonical float round-trip: bits [%s]", lines[li]);
            ASSERT_TRUE(label_buf,
                        a.num_args > 0 &&
                        memcmp(a.args, b.args,
                               (size_t)a.num_args * sizeof(a.args[0])) == 0);
        }
    }

    return test_harness_report(&g_harness, "repl_core_parse");
}
