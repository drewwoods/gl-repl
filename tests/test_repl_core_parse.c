#include "app/glr_ctrl.h"
#include "repl/core_internal.h"
#include "editor/input.h"
#include "repl/parser.h"
#include "repl/state.h"
#include "ui/state.h"
#include "support/repl_test_support.h"
#include "support/test_harness.h"

#define g_status (ui_state_status_mut()->text)

#include <stdio.h>
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
    glr_app_reset_all();
    declare_test_vars();

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
        glr_app_reset_all();
        declare_test_vars();
        editor_feed_line("glBegin(GL_TRIANGLES);");
        editor_state_edit_line_set(0);

        GLCmd cmd;
        ReplParseContext ctx = { repl_state_document_count(), NULL, 0, 0 };
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
        glr_app_reset_all();
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
        glr_app_reset_all();
        declare_test_vars();
        editor_state_edit_line_set(0);
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glBegin(GL_TRIANGLES)", &cmd);
        ASSERT_TRUE("public parse_command", ok == 1);
        ASSERT_TRUE("public parse_command type", cmd.type == CMD_BEGIN);
    }

    {
        glr_app_reset_all();
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

    {
        glr_app_reset_all();
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
        glr_app_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glRotatef(45, 0, 1, 0)", &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glRotatef parse ok", ok == 1);
        ASSERT_TRUE("glRotatef type", cmd.type == CMD_ROTATEF);
        ASSERT_TRUE("glRotatef source has 45", strstr(cmd_text, "45") != NULL);
    }

    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glutSolidTorus(0.1, 0.4, 8, 16)", &cmd);
        ASSERT_TRUE("glutSolidTorus parse ok", ok == 1);
        ASSERT_TRUE("glutSolidTorus type", cmd.type == CMD_GLUT_TORUS);
    }

    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glutSolidCone(0.25, 1.0, 12, 4)", &cmd);
        ASSERT_TRUE("glutSolidCone parse ok", ok == 1);
        ASSERT_TRUE("glutSolidCone type", cmd.type == CMD_GLUT_CONE);
    }

    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glutSolidSphere(0.25, 16, 12)", &cmd);
        ASSERT_TRUE("glutSolidSphere parse ok", ok == 1);
        ASSERT_TRUE("glutSolidSphere type", cmd.type == CMD_GLUT_SPHERE);
    }

    {
        glr_app_reset_all();
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
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glRasterPos3f(0, 1.5, 0)", &cmd);
        ASSERT_TRUE("glRasterPos3f parse ok", ok == 1);
        ASSERT_TRUE("glRasterPos3f type", cmd.type == CMD_RASTER_POS3F);
        ASSERT_TRUE("glRasterPos3f args[1]", cmd.args[1] == 1.5f);
    }

    {
        glr_app_reset_all();
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
                    strcmp(cmd.text, "hello") == 0);
        ASSERT_TRUE("label canonical text",
                    strstr(text, "\"hello\"") != NULL);
    }

    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"v=%f\", 3.5)", &cmd);
        ASSERT_TRUE("label one %f parse ok", ok == 1);
        ASSERT_TRUE("label one %f num_args", cmd.num_args == 1);
        ASSERT_TRUE("label one %f sub arg", cmd.args[0] == 3.5f);
        ASSERT_TRUE("label one %f fmt stored",
                    strcmp(cmd.text, "v=%f") == 0);
    }

    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test(
            "label(\"a=%f b=%f c=%f d=%f\", 1, 2, 3, 4)", &cmd);
        ASSERT_TRUE("label four %f parse ok", ok == 1);
        ASSERT_TRUE("label four %f num_args", cmd.num_args == 4);
        ASSERT_TRUE("label four %f arg[3]", cmd.args[3] == 4.0f);
    }

    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"100%% done\")", &cmd);
        ASSERT_TRUE("label %% literal parse ok", ok == 1);
        ASSERT_TRUE("label %% literal num_args", cmd.num_args == 0);
        ASSERT_TRUE("label %% literal fmt stored",
                    strcmp(cmd.text, "100%% done") == 0);
    }

    /* label: error cases. Each must surface a graceful
     * status message and reject the line (no GLCmd committed). */
    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"a // b\")", &cmd);
        ASSERT_TRUE("label // forbidden", ok == 0);
        ASSERT_TRUE("label // forbidden status",
                    strstr(g_status, "'//'") != NULL);
    }
    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"a , b\")", &cmd);
        ASSERT_TRUE("label , forbidden", ok == 0);
        ASSERT_TRUE("label , forbidden status",
                    strstr(g_status, "','") != NULL);
    }
    {
        glr_app_reset_all();
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
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"unterminated)", &cmd);
        ASSERT_TRUE("label missing close quote", ok == 0);
        ASSERT_TRUE("label missing close quote status",
                    strstr(g_status, "closing") != NULL);
    }
    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"%f\", 1, 2)", &cmd);
        ASSERT_TRUE("label arg-count mismatch", ok == 0);
        ASSERT_TRUE("label arg-count status",
                    strstr(g_status, "format expects") != NULL);
    }
    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("label(\"%d\", 1)", &cmd);
        ASSERT_TRUE("label %d rejected", ok == 0);
        ASSERT_TRUE("label %d status",
                    strstr(g_status, "only %f") != NULL);
    }
    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test(
            "label(\"%f %f %f %f %f\", 1, 2, 3, 4, 5)", &cmd);
        ASSERT_TRUE("label >4 sub args rejected", ok == 0);
        ASSERT_TRUE("label >4 sub args status",
                    strstr(g_status, "max 4") != NULL);
    }

    /* Removed GLU quadric shapes should no longer parse. */
    {
        glr_app_reset_all();
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
        glr_app_reset_all();
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
        glr_app_reset_all();
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
        glr_app_reset_all();
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
        glr_app_reset_all();
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
        glr_app_reset_all();
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
        glr_app_reset_all();
        declare_test_vars();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        /* The one intentional change: a var expression in the ENUM_OR_EXPR
         * slot now sets has_vars (re-evaluation) where it did not before. */
        int ok = parse_for_test("glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, x)", &cmd);
        ASSERT_TRUE("glLightModeli var ok", ok == 1);
        ASSERT_TRUE("glLightModeli var sets has_vars", cmd.has_vars == 1);
    }

    /* glMaterialf - scalar and vector (4-value) forms */
    {
        glr_app_reset_all();
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_cmd_with_text("glMaterialf(GL_FRONT, GL_SHININESS, 64)",
                                      &cmd, cmd_text, sizeof(cmd_text));
        ASSERT_TRUE("glMaterialf scalar parse ok", ok == 1);
        ASSERT_TRUE("glMaterialf scalar type", cmd.type == CMD_MATERIALF);
        ASSERT_TRUE("glMaterialf scalar source has SHININESS",
                    strstr(cmd_text, "GL_SHININESS") != NULL);
    }

    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glMaterialf(GL_FRONT, GL_DIFFUSE, 0.8, 0.2, 0.2, 1.0)", &cmd);
        ASSERT_TRUE("glMaterialf vector parse ok", ok == 1);
        ASSERT_TRUE("glMaterialf vector type", cmd.type == CMD_MATERIALF);
        ASSERT_TRUE("glMaterialf vector num_args", cmd.num_args == 6); /* face + pname + 4 vals */
    }

    /* glMaterialf - bad face name */
    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glMaterialf(FRONT, GL_DIFFUSE, 0.5)", &cmd);
        ASSERT_TRUE("glMaterialf bad face returns 0", ok == 0);
    }

    /* glPointParameterfv */
    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 1, 0, 0)", &cmd);
        ASSERT_TRUE("glPointParameterfv parse ok", ok == 1);
        ASSERT_TRUE("glPointParameterfv type", cmd.type == CMD_POINT_PARAMETER_FV);
    }

    /* Incomplete commands should not be reported as unknown commands. */
    {
        glr_app_reset_all();
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
        glr_app_reset_all();
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
        glr_app_reset_all();
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
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glColor3f(1,, 3)", &cmd);
        ASSERT_TRUE("malformed glColor3f returns 0", ok == 0);
        assert_status_contains("malformed glColor3f incomplete", "Incomplete command");
        ASSERT_TRUE("malformed glColor3f not unknown",
                    strstr(g_status, "Unknown cmd") == NULL);
    }

    {
        glr_app_reset_all();
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
        glr_app_reset_all();
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
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("sin(t)", &cmd);
        ASSERT_TRUE("sin(t) returns 0", ok == 0);
        assert_status_contains("sin(t) status names function", "sin");
        assert_status_contains("sin(t) status hints assign", "x = sin");
    }

    {
        glr_app_reset_all();
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
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("gluBegin(GLU_POLYGON)", &cmd);
        ASSERT_TRUE("gluBegin POLYGON parse ok", ok == 1);
        ASSERT_TRUE("gluBegin POLYGON type", cmd.type == CMD_TESS_BEGIN_POLYGON);
    }

    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("gluBegin(GLU_CONTOUR)", &cmd);
        ASSERT_TRUE("gluBegin CONTOUR parse ok", ok == 1);
        ASSERT_TRUE("gluBegin CONTOUR type", cmd.type == CMD_TESS_BEGIN_CONTOUR);
    }

    /* glDepthMask - bool-enum state command */
    {
        glr_app_reset_all();
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
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glDepthMask(GL_TRUE)", &cmd);
        ASSERT_TRUE("glDepthMask(GL_TRUE) parse ok", ok == 1);
        ASSERT_TRUE("glDepthMask GL_TRUE mode", (GLenum)cmd.args[0] == GL_TRUE);
    }
    {
        glr_app_reset_all();
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
        glr_app_reset_all();
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
        glr_app_reset_all();
        declare_test_vars();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        /* Runtime vars are rejected in bool-mask slots (would silently
         * collapse under flatten/replay/export). */
        int ok = parse_for_test("glDepthMask(x)", &cmd);
        ASSERT_TRUE("glDepthMask(var) rejected", ok == 0);
    }
    {
        glr_app_reset_all();
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
            glr_app_reset_all();
            GLCmd c;
            memset(&c, 0, sizeof(c));
            int r = parse_for_test(bad[bi], &c);
            ASSERT_TRUE("bool-slot rejects malformed numeric", r == 0);
        }
    }
    {
        /* Well-formed literals still accepted and canonicalized. */
        glr_app_reset_all();
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
        glr_app_reset_all();
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
        glr_app_reset_all();
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
        glr_app_reset_all();
        declare_test_vars();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        /* Runtime var in a bool-mask slot is rejected. */
        int ok = parse_for_test("glColorMask(x, GL_TRUE, GL_TRUE, GL_TRUE)", &cmd);
        ASSERT_TRUE("glColorMask(var,...) rejected", ok == 0);
    }
    {
        glr_app_reset_all();
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
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glEnable(3553)", &cmd);
        ASSERT_TRUE("glEnable(3553) still rejected", ok == 0);
    }
    {
        glr_app_reset_all();
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
            glr_app_reset_all();
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
        glr_app_reset_all();
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
        glr_app_reset_all();
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
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glPushMatrix", &cmd);
        ASSERT_TRUE("glPushMatrix without parens parse ok", ok == 1);
        ASSERT_TRUE("glPushMatrix without parens type", cmd.type == CMD_PUSH_MATRIX);
    }

    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glPopMatrix()", &cmd);
        ASSERT_TRUE("glPopMatrix with parens parse ok", ok == 1);
        ASSERT_TRUE("glPopMatrix with parens type", cmd.type == CMD_POP_MATRIX);
    }

    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glLoadIdentity()", &cmd);
        ASSERT_TRUE("glLoadIdentity with parens parse ok", ok == 1);
        ASSERT_TRUE("glLoadIdentity with parens type", cmd.type == CMD_LOAD_IDENTITY);
    }

    /* fixed-arity command with extra args should be rejected predictably */
    {
        glr_app_reset_all();
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
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("func2()", &cmd);
        ASSERT_TRUE("func2 empty arglist parse ok", ok == 1);
        ASSERT_TRUE("func2 empty arglist type", cmd.type == CMD_CALL);
        ASSERT_TRUE("func2 empty arglist num_args", cmd.num_args == 0);
    }

    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("func2(1,)", &cmd);
        ASSERT_TRUE("func2 malformed arglist returns 0", ok == 0);
        ASSERT_TRUE("func2 malformed arglist status",
                    strstr(g_status, "Invalid function call arguments") != NULL);
    }

    {
        glr_app_reset_all();
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
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("func2(1 2)", &cmd);
        ASSERT_TRUE("func2 missing comma invalid", ok == 0);
        ASSERT_TRUE("func2 missing comma status",
                    strstr(g_status, "Invalid function call arguments") != NULL);
    }

    /* supported arities for specialized commands */
    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glMaterialf(GL_FRONT, GL_AMBIENT, 0.1, 0.2, 0.3)", &cmd);
        ASSERT_TRUE("glMaterialf 3-float vector invalid", ok == 0);
        ASSERT_TRUE("glMaterialf 3-float status", strstr(g_status, "Expected 1 or 4 float values") != NULL);
    }

    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 1, 0)", &cmd);
        ASSERT_TRUE("glPointParameterfv short arglist invalid", ok == 0);
        ASSERT_TRUE("glPointParameterfv short arglist status",
                    strstr(g_status, "Expected 3 floats") != NULL);
    }

    {
        glr_app_reset_all();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = parse_for_test("glPointParameterfv(GL_POINT_FADE_THRESHOLD_SIZE, 1, 0, 0)", &cmd);
        ASSERT_TRUE("glPointParameterfv bad pname invalid", ok == 0);
        ASSERT_TRUE("glPointParameterfv bad pname status",
                    strstr(g_status, "GL_POINT_DISTANCE_ATTENUATION") != NULL);
    }

    /* known command prefix + missing close paren remains "incomplete", even with ';' */
    {
        glr_app_reset_all();
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
        glr_app_reset_all();
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

    return test_harness_report(&g_harness, "repl_core_parse");
}
