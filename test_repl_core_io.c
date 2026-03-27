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

    printf("repl_core_io: %d/%d passed\n", g_pass, g_run);
    return (g_run == g_pass) ? 0 : 1;
}
