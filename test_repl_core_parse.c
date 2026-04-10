#include "repl_core_internal.h"

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

static int leading_spaces(const char *s) {
    int n = 0;
    while (s[n] == ' ') n++;
    return n;
}

int main(void) {
    init_predef_vars();
    repl_reset_state();

    {
        char payload[128];
        memset(payload, 0, sizeof(payload));
        ASSERT_TRUE("extract nested paren payload",
                    repl_extract_paren_payload("  if(max(x, y + 1) > sin(z)) {",
                                               payload, sizeof(payload)) == 1);
        ASSERT_TRUE("extract nested paren payload text",
                    strcmp(payload, "max(x, y + 1) > sin(z)") == 0);
    }

    {
        char name[32];
        char rhs[128];
        memset(name, 0, sizeof(name));
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
    }

    {
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("glVertex3f(x + 1, 0, 0)", &cmd);
        ASSERT_TRUE("public parse_command detects predef vars", ok == 1);
        ASSERT_TRUE("public parse_command has_vars for predef", cmd.has_vars == 1);
    }

    {
        ExprVar vars[1] = { { "radius", 2.0f } };
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command_with_vars("glVertex3f(1, 2, 3)", &cmd, vars, 1);
        ASSERT_TRUE("parse with locals constant ok", ok == 1);
        ASSERT_TRUE("parse with locals constant has_vars off", cmd.has_vars == 0);
    }

    {
        ExprVar vars[1] = { { "radius", 2.0f } };
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command_with_vars("glVertex3f(radius, 0, 0)", &cmd, vars, 1);
        ASSERT_TRUE("parse with locals referenced ok", ok == 1);
        ASSERT_TRUE("parse with locals referenced has_vars on", cmd.has_vars == 1);
    }

    {
        ExprVar vars[1] = { { "radius", 2.0f } };
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command_with_vars("func0(1, 2)", &cmd, vars, 1);
        ASSERT_TRUE("func call with locals constant ok", ok == 1);
        ASSERT_TRUE("func call with locals constant has_vars off", cmd.has_vars == 0);
    }

    {
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_and_normalize("glVertex3f(x+1, y, z)", 0,
                                          NULL, 0, 1, &cmd);
        ASSERT_TRUE("parse normalize vars ok", ok == 1);
        ASSERT_TRUE("cmd has vars", cmd.has_vars == 1);
        ASSERT_TRUE("source keeps x+1", strstr(cmd.source, "x+1") != NULL);
        ASSERT_TRUE("source keeps y", strstr(cmd.source, "y") != NULL);
        ASSERT_TRUE("source keeps z", strstr(cmd.source, "z") != NULL);
    }

    {
        repl_reset_state();
        repl_feed_line_public("gluBegin(GLU_POLYGON);");
        repl_feed_line_public("gluBegin(GLU_CONTOUR);");
        repl_feed_line_public("glBegin(GL_TRIANGLES);");

        GLCmd tess_cmd;
        GLCmd gl_cmd;
        memset(&tess_cmd, 0, sizeof(tess_cmd));
        memset(&gl_cmd, 0, sizeof(gl_cmd));

        int ok1 = repl_parse_and_normalize("gluNormal(0, 0, 1)", g_num_cmds,
                                           NULL, 0, 0, &tess_cmd);
        int ok2 = repl_parse_and_normalize("glVertex3f(1, 2, 3)", g_num_cmds,
                                           NULL, 0, 0, &gl_cmd);
        ASSERT_TRUE("tess parse ok", ok1 == 1);
        ASSERT_TRUE("gl parse ok", ok2 == 1);

        int tess_indent = leading_spaces(tess_cmd.source);
        int gl_indent = leading_spaces(gl_cmd.source);
        ASSERT_TRUE("gl indent > tess indent", gl_indent > tess_indent);
        ASSERT_TRUE("indent delta is 2", gl_indent - tess_indent == 2);
    }

    {
        repl_reset_state();
        g_edit_line = 0;
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("glBegin(GL_TRIANGLES)", &cmd);
        ASSERT_TRUE("public parse_command", ok == 1);
        ASSERT_TRUE("public parse_command type", cmd.type == CMD_BEGIN);
    }

    {
        repl_reset_state();
        g_edit_line = 0;
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("func0(x + 1, 2)", &cmd);
        ASSERT_TRUE("public parse_command func call", ok == 1);
        ASSERT_TRUE("public parse_command func type", cmd.type == CMD_CALL);
        ASSERT_TRUE("func call keeps raw expr", strstr(cmd.source, "x + 1") != NULL);
    }

    printf("repl_core_parse: %d/%d passed\n", g_pass, g_run);
    return (g_run == g_pass) ? 0 : 1;
}
