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
    repl_flatten_commands();
    ASSERT_TRUE("top-level if flat count", g_num_flat_cmds == 3);
    ASSERT_TRUE("top-level if flat begin", g_flat_cmds[0].type == CMD_IF_BEGIN);
    ASSERT_TRUE("top-level if flat body", g_flat_cmds[1].type == CMD_COLOR3F);
    ASSERT_TRUE("top-level if flat end", g_flat_cmds[2].type == CMD_IF_END);

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

    repl_reset_state();
    repl_feed_line_public("func0(radius, yoff) {");
    repl_feed_line_public("glVertex3f(radius, yoff, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(1.5, x + 2);");
    ASSERT_TRUE("param func cmd count", g_num_cmds == 4);
    ASSERT_TRUE("param func def", g_cmds[0].type == CMD_FUNC_DEF);
    ASSERT_TRUE("param func header keeps names",
                strstr(g_cmds[0].source, "radius") != NULL &&
                strstr(g_cmds[0].source, "yoff") != NULL);
    ASSERT_TRUE("param func body keeps radius",
                strstr(g_cmds[1].source, "radius") != NULL);
    ASSERT_TRUE("param func body keeps yoff",
                strstr(g_cmds[1].source, "yoff") != NULL);
    ASSERT_TRUE("param func call type", g_cmds[3].type == CMD_CALL);
    ASSERT_TRUE("param func call keeps expr",
                strstr(g_cmds[3].source, "x + 2") != NULL);

    repl_reset_state();
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
    ASSERT_TRUE("nested func flatten count", g_num_flat_cmds == 6);
    ASSERT_TRUE("nested func flatten first begin", g_flat_cmds[0].type == CMD_BEGIN);
    ASSERT_TRUE("nested func flatten first vertex", g_flat_cmds[1].type == CMD_VERTEX3F);
    ASSERT_TRUE("nested func flatten first x", fabsf(g_flat_cmds[1].args[0] - 2.0f) < 1e-6f);
    ASSERT_TRUE("nested func flatten first y", fabsf(g_flat_cmds[1].args[1] - 3.0f) < 1e-6f);
    ASSERT_TRUE("nested func flatten second x", fabsf(g_flat_cmds[4].args[0] - 4.0f) < 1e-6f);
    ASSERT_TRUE("nested func flatten second y", fabsf(g_flat_cmds[4].args[1] - 5.0f) < 1e-6f);
    ASSERT_TRUE("nested func call provenance immediate", g_flat_cmds[1].call_src_cmd_idx == 6);
    ASSERT_TRUE("nested func call provenance root first", g_flat_cmds[1].root_call_src_cmd_idx == 8);
    ASSERT_TRUE("nested func call provenance root second", g_flat_cmds[4].root_call_src_cmd_idx == 9);
    ASSERT_TRUE("nested func scope mask includes both", (g_flat_cmds[1].func_scope_mask & 0x3u) == 0x3u);
    {
        int matched = 0;
        g_edit_line = 8;
        for (int i = 0; i < g_num_flat_cmds; i++)
            matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("nested func call line highlights one invocation", matched == 3);
    }
    {
        int matched = 0;
        g_edit_line = 6;
        for (int i = 0; i < g_num_flat_cmds; i++)
            matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("nested inner call line highlights all invocations", matched == 6);
    }
    {
        int matched = 0;
        g_edit_line = 2;
        for (int i = 0; i < g_num_flat_cmds; i++)
            matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("nested function body highlights all invocations", matched == 6);
    }
    {
        int matched = 0;
        g_edit_line = 5;
        for (int i = 0; i < g_num_flat_cmds; i++)
            matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("outer function header highlights nested invocations", matched == 6);
    }

    repl_reset_state();
    repl_feed_line_public("func0(n) {");
    repl_feed_line_public("for(i, 0, n) {");
    repl_feed_line_public("glVertex3f(i, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("glColor3f(1, 0, 0);");
    repl_feed_line_public("}");
    ASSERT_TRUE("nested block trailing cmd count", g_num_cmds == 6);
    ASSERT_TRUE("nested block trailing cmd order body", g_cmds[4].type == CMD_COLOR3F);
    ASSERT_TRUE("nested block trailing cmd order end", g_cmds[5].type == CMD_FUNC_END);

    repl_reset_state();
    repl_feed_line_public("func0(n) {");
    repl_feed_line_public("for(i, 0, n) glVertex3f(i, n, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(3);");
    ASSERT_TRUE("local for begin type", g_cmds[1].type == CMD_FOR_BEGIN);
    ASSERT_TRUE("local for body type", g_cmds[2].type == CMD_VERTEX3F);
    ASSERT_TRUE("local for end type", g_cmds[3].type == CMD_FOR_END);
    ASSERT_TRUE("local for header keeps n", strstr(g_cmds[1].source, "n") != NULL);
    ASSERT_TRUE("local for body keeps n", strstr(g_cmds[2].source, "n") != NULL);
    repl_flatten_commands();
    ASSERT_TRUE("local for flatten count", g_num_flat_cmds == 3);
    ASSERT_TRUE("local for first x", fabsf(g_flat_cmds[0].args[0] - 0.0f) < 1e-6f);
    ASSERT_TRUE("local for second x", fabsf(g_flat_cmds[1].args[0] - 1.0f) < 1e-6f);
    ASSERT_TRUE("local for third x", fabsf(g_flat_cmds[2].args[0] - 2.0f) < 1e-6f);
    ASSERT_TRUE("local for body uses param", fabsf(g_flat_cmds[2].args[1] - 3.0f) < 1e-6f);

    repl_reset_state();
    repl_feed_line_public("func0(scale) {");
    repl_feed_line_public("if(scale > 1) {");
    repl_feed_line_public("glVertex3f(scale, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(2);");
    repl_feed_line_public("func0(0.5);");
    ASSERT_TRUE("local if header keeps scale", strstr(g_cmds[1].source, "scale > 1") != NULL);
    repl_flatten_commands();
    ASSERT_TRUE("local if flatten count", g_num_flat_cmds == 1);
    ASSERT_TRUE("local if flatten type", g_flat_cmds[0].type == CMD_VERTEX3F);
    ASSERT_TRUE("local if flatten x", fabsf(g_flat_cmds[0].args[0] - 2.0f) < 1e-6f);

    repl_reset_state();
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
        g_edit_line = 0;
        for (int i = 0; i < g_num_flat_cmds; i++)
            if (g_flat_cmds[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("top-level color before block matches first vertices", matched == 2);
    }
    {
        int matched = 0;
        g_edit_line = 5;
        for (int i = 0; i < g_num_flat_cmds; i++)
            if (g_flat_cmds[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("top-level second color before block matches second vertices", matched == 2);
    }
    ASSERT_TRUE("feeding color for first block vertex", repl_find_feeding_color_cmd(2) == 0);
    ASSERT_TRUE("feeding color for second block vertex", repl_find_feeding_color_cmd(7) == 5);

    repl_reset_state();
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
        g_edit_line = 0;
        for (int i = 0; i < g_num_flat_cmds; i++)
            if (g_flat_cmds[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("top-level normal before block matches first vertices", matched == 2);
    }
    {
        int matched = 0;
        g_edit_line = 5;
        for (int i = 0; i < g_num_flat_cmds; i++)
            if (g_flat_cmds[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("top-level second normal before block matches second vertices", matched == 2);
    }
    ASSERT_TRUE("feeding normal for first block vertex", repl_find_feeding_normal_cmd(2) == 0);
    ASSERT_TRUE("feeding normal for second block vertex", repl_find_feeding_normal_cmd(7) == 5);

    repl_reset_state();
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glVertex3f(1, 0, 0);");
    repl_feed_line_public("glEnd();");
    repl_flatten_commands();
    repl_navigate_to_line(1);
    {
        int matched = 0;
        for (int i = 0; i < g_num_flat_cmds; i++)
            if (g_flat_cmds[i].type == CMD_VERTEX3F)
                matched += repl_flat_cmd_matches_cursor(i);
        ASSERT_TRUE("navigate refreshes current block highlight", matched == 2);
    }

    repl_reset_state();
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
    ASSERT_TRUE("recursive flatten count", g_num_flat_cmds == 4);
    ASSERT_TRUE("recursive first x", fabsf(g_flat_cmds[0].args[0] - 3.0f) < 1e-6f);
    ASSERT_TRUE("recursive second x", fabsf(g_flat_cmds[1].args[0] - 2.0f) < 1e-6f);
    ASSERT_TRUE("recursive third x", fabsf(g_flat_cmds[2].args[0] - 1.0f) < 1e-6f);
    ASSERT_TRUE("recursive base x", fabsf(g_flat_cmds[3].args[0] - 0.0f) < 1e-6f);

    repl_reset_state();
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
    ASSERT_TRUE("mutual recursion flatten count", g_num_flat_cmds == 3);
    ASSERT_TRUE("mutual recursion first x", fabsf(g_flat_cmds[0].args[0] - 2.0f) < 1e-6f);
    ASSERT_TRUE("mutual recursion second x", fabsf(g_flat_cmds[1].args[0] - (-1.0f)) < 1e-6f);
    ASSERT_TRUE("mutual recursion base x", fabsf(g_flat_cmds[2].args[0] - 0.0f) < 1e-6f);

    repl_reset_state();
    g_status[0] = '\0';
    repl_feed_line_public("func0(n) {");
    repl_feed_line_public("func0(n + 1);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(0);");
    repl_flatten_commands();
    ASSERT_TRUE("runaway recursion emits no flat cmds", g_num_flat_cmds == 0);
    ASSERT_TRUE("runaway recursion depth guard",
                strstr(g_status, "depth limit") != NULL ||
                strstr(g_status, "visit budget") != NULL);

    printf("repl_core_commit: %d/%d passed\n", g_pass, g_run);
    return (g_run == g_pass) ? 0 : 1;
}
