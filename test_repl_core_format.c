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
    const char *line = "glVertex3f(x+1, y, z);";
    const char *tmp_path = "/tmp/repl_core_format_input.c";
    const char *tmp_loop_path = "/tmp/repl_core_format_loop_input.c";
    const char *tmp_dump_path = "/tmp/repl_core_format_dump.txt";

    init_predef_vars();
    repl_reset_state();

    GLCmd interactive_cmd;
    memset(&interactive_cmd, 0, sizeof(interactive_cmd));
    ASSERT_TRUE("interactive parse normalize",
                repl_parse_and_normalize(line, 0, NULL, 0, 1, &interactive_cmd) == 1);

    repl_reset_state();
    repl_feed_line_public(line);
    ASSERT_TRUE("feed inserted one", g_num_cmds == 1);
    ASSERT_TRUE("feed matches interactive",
                strcmp(g_cmds[0].source, interactive_cmd.source) == 0);

    {
        FILE *f = fopen(tmp_path, "w");
        ASSERT_TRUE("open tmp file", f != NULL);
        if (f) {
            fprintf(f, "// Snippet start\n");
            fprintf(f, "%s\n", line);
            fprintf(f, "// Snippet end\n");
            fclose(f);
        }
    }

    repl_reset_state();
    ASSERT_TRUE("load from file", repl_load_from_file(tmp_path) == 1);
    ASSERT_TRUE("load inserted one", g_num_cmds == 1);
    ASSERT_TRUE("load matches interactive",
                strcmp(g_cmds[0].source, interactive_cmd.source) == 0);

    {
        char out[256];
        repl_normalize_from_parsed("      glVertex3f(1,2,3);",
                                   "  glVertex3f(x, y, z)   ",
                                   1, out, sizeof(out));
        ASSERT_TRUE("normalize preserves vars", strstr(out, "x") != NULL);
        ASSERT_TRUE("normalize has semicolon", out[strlen(out) - 1] == ';');
        ASSERT_TRUE("normalize keeps indent", out[0] == ' ' && out[1] == ' ');
    }

    {
        FILE *f = fopen(tmp_loop_path, "w");
        ASSERT_TRUE("open tmp loop file", f != NULL);
        if (f) {
            fprintf(f, "// Snippet start\n");
            fprintf(f, "for (float i = 0; i < 3; i += 1.0f) {\n");
            fprintf(f, "glVertex3f(i + x, 0, 0);\n");
            fprintf(f, "}\n");
            fprintf(f, "// Snippet end\n");
            fclose(f);
        }
    }

    repl_reset_state();
    ASSERT_TRUE("load loop file", repl_load_from_file(tmp_loop_path) == 1);
    ASSERT_TRUE("loop imported 3 cmds", g_num_cmds == 3);
    ASSERT_TRUE("loop header type", g_cmds[0].type == CMD_FOR_BEGIN);
    ASSERT_TRUE("loop body type", g_cmds[1].type == CMD_VERTEX3F);
    ASSERT_TRUE("loop body has vars", g_cmds[1].has_vars == 1);
    ASSERT_TRUE("loop body keeps expression", strstr(g_cmds[1].source, "i + x") != NULL);
    ASSERT_TRUE("loop body indented", strncmp(g_cmds[1].source, "    ", 4) == 0);

    {
        strncpy(g_cmds[1].source, "glVertex3f(i+x,0,0)", sizeof(g_cmds[1].source) - 1);
        g_cmds[1].source[sizeof(g_cmds[1].source) - 1] = '\0';
        strncpy(g_cmds[2].source, "}", sizeof(g_cmds[2].source) - 1);
        g_cmds[2].source[sizeof(g_cmds[2].source) - 1] = '\0';
        repl_reformat_commands();
        ASSERT_TRUE("reformat body semicolon", g_cmds[1].source[strlen(g_cmds[1].source) - 1] == ';');
        ASSERT_TRUE("reformat body indent", strncmp(g_cmds[1].source, "    ", 4) == 0);
        ASSERT_TRUE("reformat body comma spacing",
                    strstr(g_cmds[1].source, ", 0, 0") != NULL);
        ASSERT_TRUE("reformat close brace indent", strcmp(g_cmds[2].source, "  }") == 0);
    }

    {
        char dump_buf[8192];
        FILE *dump_f = fopen(tmp_dump_path, "w");
        ASSERT_TRUE("open dump file", dump_f != NULL);
        if (dump_f) {
            repl_debug_dump_editor(dump_f);
            fclose(dump_f);
        }

        dump_f = fopen(tmp_dump_path, "r");
        ASSERT_TRUE("read dump file", dump_f != NULL);
        if (dump_f) {
            size_t n = fread(dump_buf, 1, sizeof(dump_buf) - 1, dump_f);
            dump_buf[n] = '\0';
            fclose(dump_f);
            ASSERT_TRUE("dump has source section",
                        strstr(dump_buf, "--- source ---") != NULL);
            ASSERT_TRUE("dump includes formatted body",
                        strstr(dump_buf, "    glVertex3f(i+x, 0, 0);") != NULL);
        }
    }

    printf("repl_core_format: %d/%d passed\n", g_pass, g_run);
    return (g_run == g_pass) ? 0 : 1;
}
