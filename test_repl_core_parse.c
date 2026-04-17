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

static void declare_test_vars(void) {
    char err[128];
    declare_predef_var("x", err, sizeof(err));
    declare_predef_var("y", err, sizeof(err));
    declare_predef_var("z", err, sizeof(err));
}

static void assert_status_contains(const char *label, const char *needle) {
    ASSERT_TRUE(label, strstr(g_status, needle) != NULL);
}

int main(void) {
    init_predef_vars();
    repl_reset_state();
    declare_test_vars();

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
        declare_test_vars();
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
        declare_test_vars();
        g_edit_line = 0;
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("glBegin(GL_TRIANGLES)", &cmd);
        ASSERT_TRUE("public parse_command", ok == 1);
        ASSERT_TRUE("public parse_command type", cmd.type == CMD_BEGIN);
    }

    {
        repl_reset_state();
        declare_test_vars();
        g_edit_line = 0;
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("func0(x + 1, 2)", &cmd);
        ASSERT_TRUE("public parse_command func call", ok == 1);
        ASSERT_TRUE("public parse_command func type", cmd.type == CMD_CALL);
        ASSERT_TRUE("func call keeps raw expr", strstr(cmd.source, "x + 1") != NULL);
    }

    /* 4-arg commands (glRotatef, gluDisk, glutSolidTorus) — exercise case 4 in fmt switch */
    {
        repl_reset_state();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("glRotatef(45, 0, 1, 0)", &cmd);
        ASSERT_TRUE("glRotatef parse ok", ok == 1);
        ASSERT_TRUE("glRotatef type", cmd.type == CMD_ROTATEF);
        ASSERT_TRUE("glRotatef source has 45", strstr(cmd.source, "45") != NULL);
    }

    {
        repl_reset_state();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("gluDisk(0.2, 0.5, 16, 2)", &cmd);
        ASSERT_TRUE("gluDisk parse ok", ok == 1);
        ASSERT_TRUE("gluDisk type", cmd.type == CMD_GLU_DISK);
    }

    {
        repl_reset_state();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("glutSolidTorus(0.1, 0.4, 8, 16)", &cmd);
        ASSERT_TRUE("glutSolidTorus parse ok", ok == 1);
        ASSERT_TRUE("glutSolidTorus type", cmd.type == CMD_GLUT_TORUS);
    }

    /* 5-arg command: gluCylinder — exercise case 5 */
    {
        repl_reset_state();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("gluCylinder(0.3, 0.3, 1.0, 12, 4)", &cmd);
        ASSERT_TRUE("gluCylinder parse ok", ok == 1);
        ASSERT_TRUE("gluCylinder type", cmd.type == CMD_GLU_CYLINDER);
        ASSERT_TRUE("gluCylinder source has height", cmd.args[2] == 1.0f);
    }

    /* 6-arg command: gluPartialDisk — exercise case 6 */
    {
        repl_reset_state();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("gluPartialDisk(0.1, 0.5, 16, 2, 0, 180)", &cmd);
        ASSERT_TRUE("gluPartialDisk parse ok", ok == 1);
        ASSERT_TRUE("gluPartialDisk type", cmd.type == CMD_GLU_PARTIAL_DISK);
    }

    /* glMaterialf — scalar and vector (4-value) forms */
    {
        repl_reset_state();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("glMaterialf(GL_FRONT, GL_SHININESS, 64)", &cmd);
        ASSERT_TRUE("glMaterialf scalar parse ok", ok == 1);
        ASSERT_TRUE("glMaterialf scalar type", cmd.type == CMD_MATERIALF);
        ASSERT_TRUE("glMaterialf scalar source has SHININESS",
                    strstr(cmd.source, "GL_SHININESS") != NULL);
    }

    {
        repl_reset_state();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("glMaterialf(GL_FRONT, GL_DIFFUSE, 0.8, 0.2, 0.2, 1.0)", &cmd);
        ASSERT_TRUE("glMaterialf vector parse ok", ok == 1);
        ASSERT_TRUE("glMaterialf vector type", cmd.type == CMD_MATERIALF);
        ASSERT_TRUE("glMaterialf vector num_args", cmd.num_args == 5); /* pname + 4 vals */
    }

    /* glMaterialf — bad face name */
    {
        repl_reset_state();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("glMaterialf(FRONT, GL_DIFFUSE, 0.5)", &cmd);
        ASSERT_TRUE("glMaterialf bad face returns 0", ok == 0);
    }

    /* glPointParameterfv */
    {
        repl_reset_state();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 1, 0, 0)", &cmd);
        ASSERT_TRUE("glPointParameterfv parse ok", ok == 1);
        ASSERT_TRUE("glPointParameterfv type", cmd.type == CMD_POINT_PARAMETER_FV);
    }

    /* Incomplete commands should not be reported as unknown commands. */
    {
        repl_reset_state();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("glColor3f(1, 1", &cmd);
        ASSERT_TRUE("incomplete glColor3f returns 0", ok == 0);
        assert_status_contains("incomplete glColor3f status", "Incomplete command");
        assert_status_contains("incomplete glColor3f missing paren", "missing ')'");
        ASSERT_TRUE("incomplete glColor3f not unknown",
                    strstr(g_status, "Unknown cmd") == NULL);
    }

    {
        repl_reset_state();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("glVertex(1,", &cmd);
        ASSERT_TRUE("incomplete glVertex returns 0", ok == 0);
        assert_status_contains("incomplete glVertex status", "Incomplete command");
        assert_status_contains("incomplete glVertex missing paren", "missing ')'");
        ASSERT_TRUE("incomplete glVertex not unknown",
                    strstr(g_status, "Unknown cmd") == NULL);
    }

    {
        repl_reset_state();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("glColor3f(1, 1)", &cmd);
        ASSERT_TRUE("short glColor3f returns 0", ok == 0);
        assert_status_contains("short glColor3f status", "Incomplete command");
        assert_status_contains("short glColor3f expected count", "expects 3 arguments");
        ASSERT_TRUE("short glColor3f not unknown",
                    strstr(g_status, "Unknown cmd") == NULL);
    }

    {
        repl_reset_state();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("glTotallyUnknown(1, 2, 3)", &cmd);
        ASSERT_TRUE("complete unknown command returns 0", ok == 0);
        assert_status_contains("complete unknown command status", "Unknown cmd");
    }

    /* gluBegin/gluEnd via parse_command */
    {
        repl_reset_state();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("gluBegin(GLU_POLYGON)", &cmd);
        ASSERT_TRUE("gluBegin POLYGON parse ok", ok == 1);
        ASSERT_TRUE("gluBegin POLYGON type", cmd.type == CMD_TESS_BEGIN_POLYGON);
    }

    {
        repl_reset_state();
        GLCmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        int ok = repl_parse_command("gluBegin(GLU_CONTOUR)", &cmd);
        ASSERT_TRUE("gluBegin CONTOUR parse ok", ok == 1);
        ASSERT_TRUE("gluBegin CONTOUR type", cmd.type == CMD_TESS_BEGIN_CONTOUR);
    }

    printf("repl_core_parse: %d/%d passed\n", g_pass, g_run);
    return (g_run == g_pass) ? 0 : 1;
}
