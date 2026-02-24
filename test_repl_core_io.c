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

    repl_save_output(path);

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

    printf("repl_core_io: %d/%d passed\n", g_pass, g_run);
    return (g_run == g_pass) ? 0 : 1;
}
