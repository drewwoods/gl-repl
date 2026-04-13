#include "repl_core_internal.h"
#include "sample.h"
#include <stdio.h>
#include <string.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s]\n", label); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    g_run++; \
    if ((got) == (exp)) g_pass++; \
    else printf("FAIL [%s] got %d, expected %d\n", label, (int)(got), (int)(exp)); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    g_run++; \
    if (strcmp(got, exp) == 0) g_pass++; \
    else printf("FAIL [%s] got \"%s\", expected \"%s\"\n", label, got, exp); \
} while (0)

int main() {
    init_predef_vars();
    printf("--- repl_editor tests ---\n");

    /* 1. Undo/Redo */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        ASSERT_INT("num_cmds 1", g_num_cmds, 1);
        
        push_undo_snapshot();
        repl_feed_line_public("glVertex3f(2,2,2)");
        ASSERT_INT("num_cmds 2", g_num_cmds, 2);
        
        pop_undo_snapshot();
        ASSERT_INT("num_cmds after undo", g_num_cmds, 1);
        ASSERT_STR("cmd 0 source", g_cmds[0].source, "  glVertex3f(1, 1, 1);");
        
        do_redo();
        ASSERT_INT("num_cmds after redo", g_num_cmds, 2);
        ASSERT_STR("cmd 1 source", g_cmds[1].source, "  glVertex3f(2, 2, 2);");
    }

    /* 2. Deleting commands */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        ASSERT_INT("num_cmds 3", g_num_cmds, 3);
        
        delete_cmd_range(1, 1, "test");
        ASSERT_INT("num_cmds after delete", g_num_cmds, 2);
        ASSERT_STR("line 0 still there", g_cmds[0].source, "  glVertex3f(0, 0, 0);");
        ASSERT_STR("line 1 is now old line 2", g_cmds[1].source, "  glVertex3f(2, 2, 2);");
    }

    /* 3. Committing variable assignments */
    {
        repl_reset_state();
        strcpy(g_input, "n = 10.5");
        g_input_len = (int)strlen(g_input);
        
        int r = try_assign_variable();
        ASSERT_INT("try_assign_variable returns 1", r, 1);
        ASSERT_INT("num_cmds 1 after assign", g_num_cmds, 1);
        ASSERT_STR("assigned cmd source", g_cmds[0].source, "  n = 10.5;");
    }

    /* 4. Committing for loops */
    {
        repl_reset_state();
        strcpy(g_input, "for(i, 0, 5) {");
        g_input_len = (int)strlen(g_input);
        
        int r = try_commit_for_loop();
        ASSERT_INT("try_commit_for_loop returns 1", r, 1);
        ASSERT_INT("num_cmds 2 after for", g_num_cmds, 2);
        ASSERT_STR("for loop source", g_cmds[0].source, "  for(i, 0, 5) {");
        ASSERT_STR("for end source", g_cmds[1].source, "  }");
    }

    /* 5. Committing func defs */
    {
        repl_reset_state();
        strcpy(g_input, "func0(x, y) {");
        g_input_len = (int)strlen(g_input);
        
        int r = try_commit_func_def();
        ASSERT_INT("try_commit_func_def returns 1", r, 1);
        ASSERT_INT("num_cmds 2 after func", g_num_cmds, 2);
        ASSERT_STR("func def source", g_cmds[0].source, "  func0(x, y) {");
        ASSERT_STR("func end source", g_cmds[1].source, "  }");
    }

    /* 6. Committing if blocks */
    {
        repl_reset_state();
        strcpy(g_input, "if(x > 0) {");
        g_input_len = (int)strlen(g_input);
        
        int r = try_commit_if_block();
        ASSERT_INT("try_commit_if_block returns 1", r, 1);
        ASSERT_INT("num_cmds 2 after if", g_num_cmds, 2);
        ASSERT_STR("if block source", g_cmds[0].source, "  if(x > 0) {");
        ASSERT_STR("if end source", g_cmds[1].source, "  }");
    }

    /* 7. Committing close brace */
    {
        repl_reset_state();
        repl_feed_line_public("for(i, 0, 1) {");
        strcpy(g_input, "}");
        g_input_len = (int)strlen(g_input);
        g_edit_line = 1;
        
        int r = try_commit_close_brace();
        ASSERT_INT("try_commit_close_brace returns 1", r, 1);
        /* Note: try_commit_close_brace might overwrite the existing closing brace if it's already there, 
         * or add one. If we just did repl_feed_line_public("for...{"), it already added a closing brace! */
        ASSERT_INT("num_cmds after brace", g_num_cmds, 2);
        ASSERT_STR("close brace source", g_cmds[1].source, "  }");
    }

    printf("\n%d / %d tests passed\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
