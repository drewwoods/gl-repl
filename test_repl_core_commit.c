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
    init_predef_vars();

    repl_reset_state();
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

    repl_reset_state();
    repl_feed_line_public("for(i, 0, 3) {");
    repl_feed_line_public("glVertex3f(i, 0, 0);");
    repl_feed_line_public("}");
    ASSERT_TRUE("for block cmd count", g_num_cmds == 3);
    ASSERT_TRUE("for begin", g_cmds[0].type == CMD_FOR_BEGIN);
    ASSERT_TRUE("for body", g_cmds[1].type == CMD_VERTEX3F);
    ASSERT_TRUE("for end", g_cmds[2].type == CMD_FOR_END);
    ASSERT_TRUE("for body keeps i", strstr(g_cmds[1].source, "i") != NULL);

    repl_reset_state();
    repl_feed_line_public("if(x > 0) {");
    repl_feed_line_public("glColor3f(1, 0, 0);");
    repl_feed_line_public("}");
    ASSERT_TRUE("if block cmd count", g_num_cmds == 3);
    ASSERT_TRUE("if begin", g_cmds[0].type == CMD_IF_BEGIN);
    ASSERT_TRUE("if body", g_cmds[1].type == CMD_COLOR3F);
    ASSERT_TRUE("if end", g_cmds[2].type == CMD_IF_END);

    repl_reset_state();
    repl_feed_line_public("func0 {");
    repl_feed_line_public("glVertex3f(1, 2, 3);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0()");
    ASSERT_TRUE("func cmd count", g_num_cmds == 4);
    ASSERT_TRUE("func def", g_cmds[0].type == CMD_FUNC_DEF);
    ASSERT_TRUE("func body", g_cmds[1].type == CMD_VERTEX3F);
    ASSERT_TRUE("func end", g_cmds[2].type == CMD_FUNC_END);
    ASSERT_TRUE("func call", g_cmds[3].type == CMD_CALL);

    printf("repl_core_commit: %d/%d passed\n", g_pass, g_run);
    return (g_run == g_pass) ? 0 : 1;
}
