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

int main(void) {
    const char *path = "/tmp/repl_core_roundtrip_output.c";
    const char *func_path = "/tmp/repl_core_func_output.c";
    const char *quadric_path = "/tmp/repl_core_quadric_output.c";
    const char *tess_path = "/tmp/repl_core_tess_output.c";

    init_predef_vars();
    repl_reset_state();

    repl_feed_line_public("x = 1.25;");
    repl_feed_line_public("glBegin(GL_LINE_STRIP);");
    repl_feed_line_public("for(i, 0, 3) {");
    repl_feed_line_public("glVertex3f(i + x, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("glEnd();");

    ASSERT_TRUE("pre-save cmds", g_num_cmds > 0);

    int before_n = g_num_cmds;
    CmdType before_types[MAX_COMMANDS];
    for (int i = 0; i < before_n; i++) before_types[i] = g_cmds[i].type;

    g_multisample_enabled = 0;
    g_line_smooth_enabled = 1;
    repl_save_output(path);
    {
        FILE *saved = fopen(path, "r");
        char buf[8192];
        size_t nread = saved ? fread(buf, 1, sizeof(buf) - 1, saved) : 0;
        if (saved) fclose(saved);
        buf[nread] = '\0';
        ASSERT_TRUE("saved t uses elapsed time",
                    strstr(buf, "t = 0.001f * (float)glutGet(GLUT_ELAPSED_TIME)") != NULL);
        ASSERT_TRUE("saved multisample header state",
                    strstr(buf, "glDisable(GL_MULTISAMPLE);") != NULL);
        ASSERT_TRUE("saved line smooth header state",
                    strstr(buf, "glEnable(GL_LINE_SMOOTH);") != NULL);
    }

    repl_reset_state();
    ASSERT_TRUE("load saved output", repl_load_from_file(path) == 1);
    ASSERT_TRUE("roundtrip cmd count", g_num_cmds == before_n);

    for (int i = 0; i < before_n; i++) {
        char label[64];
        snprintf(label, sizeof(label), "roundtrip type %d", i);
        ASSERT_TRUE(label, g_cmds[i].type == before_types[i]);
    }

    repl_flatten_commands();
    ASSERT_TRUE("flatten produced cmds", g_num_flat_cmds > 0);

    repl_reset_state();
    repl_feed_line_public("x = 1;");
    repl_feed_line_public("func0(radius, yoff) {");
    repl_feed_line_public("glVertex3f(radius, yoff, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(1.5, x + 2);");

    before_n = g_num_cmds;
    for (int i = 0; i < before_n; i++) before_types[i] = g_cmds[i].type;

    repl_save_output(func_path);
    {
        FILE *saved = fopen(func_path, "r");
        char buf[8192];
        size_t nread = saved ? fread(buf, 1, sizeof(buf) - 1, saved) : 0;
        if (saved) fclose(saved);
        buf[nread] = '\0';
        ASSERT_TRUE("saved func signature",
                    strstr(buf, "static void func0(float radius, float yoff)") != NULL);
        ASSERT_TRUE("saved func call",
                    strstr(buf, "func0(1.5, x + 2);") != NULL);
    }

    repl_reset_state();
    ASSERT_TRUE("load saved param func output", repl_load_from_file(func_path) == 1);
    ASSERT_TRUE("param func roundtrip cmd count", g_num_cmds == before_n);
    {
        int have_var = 0, have_def = 0, have_body = 0, have_end = 0, have_call = 0;
        for (int i = 0; i < g_num_cmds; i++) {
            if (g_cmds[i].type == CMD_VAR_ASSIGN) have_var++;
            if (g_cmds[i].type == CMD_FUNC_DEF) have_def++;
            if (g_cmds[i].type == CMD_VERTEX3F) have_body++;
            if (g_cmds[i].type == CMD_FUNC_END) have_end++;
            if (g_cmds[i].type == CMD_CALL) have_call++;
        }
        ASSERT_TRUE("param func roundtrip has var assign", have_var == 1);
        ASSERT_TRUE("param func roundtrip has func def", have_def == 1);
        ASSERT_TRUE("param func roundtrip has body", have_body == 1);
        ASSERT_TRUE("param func roundtrip has func end", have_end == 1);
        ASSERT_TRUE("param func roundtrip has func call", have_call == 1);
    }

    repl_flatten_commands();
    ASSERT_TRUE("param func flatten count", g_num_flat_cmds >= 2);
    ASSERT_TRUE("param func flatten vertex type", g_flat_cmds[g_num_flat_cmds - 1].type == CMD_VERTEX3F);
    ASSERT_TRUE("param func flatten x",
                fabsf(g_flat_cmds[g_num_flat_cmds - 1].args[0] - 1.5f) < 1e-6f);
    ASSERT_TRUE("param func flatten y",
                fabsf(g_flat_cmds[g_num_flat_cmds - 1].args[1] - 3.0f) < 1e-6f);

    repl_reset_state();
    repl_feed_line_public("x = 0.25;");
    repl_feed_line_public("gluSphere(x, 16, 12);");
    repl_feed_line_public("gluCylinder(0.15, 0.05, 1.5, 8, 1);");
    repl_feed_line_public("gluDisk(0, 0.35, 12, 1);");
    repl_feed_line_public("gluPartialDisk(0.1, 0.5, 12, 4, 30, 180);");
    repl_save_output(quadric_path);
    {
        FILE *saved = fopen(quadric_path, "r");
        char buf[16384];
        size_t nread = saved ? fread(buf, 1, sizeof(buf) - 1, saved) : 0;
        if (saved) fclose(saved);
        buf[nread] = '\0';
        ASSERT_TRUE("saved quadric sphere includes g_quadric",
                    strstr(buf, "gluSphere(g_quadric, x, 16, 12);") != NULL);
        ASSERT_TRUE("saved quadric cylinder includes g_quadric",
                    strstr(buf, "gluCylinder(g_quadric, 0.15, 0.05, 1.5, 8, 1);") != NULL);
        ASSERT_TRUE("saved quadric disk includes g_quadric",
                    strstr(buf, "gluDisk(g_quadric, 0, 0.35, 12, 1);") != NULL);
        ASSERT_TRUE("saved quadric partial disk includes g_quadric",
                    strstr(buf, "gluPartialDisk(g_quadric, 0.1, 0.5, 12, 4, 30, 180);") != NULL);
    }

    repl_reset_state();
    ASSERT_TRUE("load saved quadric output", repl_load_from_file(quadric_path) == 1);
    {
        int sphere_seen = 0;
        int quadric_cmds = 0;
        for (int i = 0; i < g_num_cmds; i++) {
            if (!g_cmds[i].valid)
                continue;
            if (g_cmds[i].type == CMD_GLU_SPHERE ||
                g_cmds[i].type == CMD_GLU_CYLINDER ||
                g_cmds[i].type == CMD_GLU_DISK ||
                g_cmds[i].type == CMD_GLU_PARTIAL_DISK) {
                quadric_cmds++;
                ASSERT_TRUE("loaded quadric source omits g_quadric",
                            strstr(g_cmds[i].source, "g_quadric") == NULL);
                if (g_cmds[i].type == CMD_GLU_SPHERE &&
                    strstr(g_cmds[i].source, "gluSphere(x, 16, 12);") != NULL)
                    sphere_seen = 1;
            }
        }
        ASSERT_TRUE("loaded quadric cmd count", quadric_cmds == 4);
        ASSERT_TRUE("loaded quadric sphere keeps expr source", sphere_seen == 1);
    }

    repl_reset_state();
    repl_feed_line_public("func0(radius) {");
    repl_feed_line_public("gluBegin(GLU_POLYGON);");
    repl_feed_line_public("gluBegin(GLU_CONTOUR);");
    repl_feed_line_public("for(i, 0, 4) {");
    repl_feed_line_public("gluColor(0.25 + 0.15*sin(i), 0.3, 0.4);");
    repl_feed_line_public("gluVertex(radius*cos(i), 0, radius*sin(i));");
    repl_feed_line_public("}");
    repl_feed_line_public("gluEnd();");
    repl_feed_line_public("gluEnd();");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(2.0);");
    repl_save_output(tess_path);
    {
        FILE  *saved = fopen(tess_path, "r");
        char   buf[16384];
        size_t nread = saved ? fread(buf, 1, sizeof(buf) - 1, saved) : 0;
        if (saved)
            fclose(saved);
        buf[nread] = '\0';
        ASSERT_TRUE("saved tess color keeps loop expr",
                    strstr(buf, "_tc[0]=0.25 + 0.15*sinf(i);") != NULL);
        ASSERT_TRUE("saved tess vertex keeps param expr",
                    strstr(buf, "_v->pos[0]=radius*cosf(i);") != NULL);
        ASSERT_TRUE("saved tess vertex keeps z expr",
                    strstr(buf, "_v->pos[2]=radius*sinf(i);") != NULL);
    }

    printf("repl_core_io: %d/%d passed\n", g_pass, g_run);
    return (g_run == g_pass) ? 0 : 1;
}
