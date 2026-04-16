#include "repl_core_internal.h"
#include "sample.h"
#include "profile_panel.h"
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

static void declare_test_vars(void) {
    char err[128];
    declare_predef_var("x", err, sizeof(err));
    declare_predef_var("y", err, sizeof(err));
    declare_predef_var("z", err, sizeof(err));
    declare_predef_var("i", err, sizeof(err));
    declare_predef_var("j", err, sizeof(err));
    declare_predef_var("k", err, sizeof(err));
    declare_predef_var("n", err, sizeof(err));
}

int main() {
    init_predef_vars();
    printf("--- repl_editor tests ---\n");

    /* 1. Undo when nothing to undo — must be first, before any push */
    {
        /* g_undo_count starts at 0 (global zero-init); repl_reset_state() does
         * NOT clear the undo buffer, so run this before touching undo at all. */
        repl_reset_state();
        pop_undo_snapshot();
        /* Should survive without crashing; state unchanged */
        ASSERT_INT("undo-nothing: num_cmds still 0", g_num_cmds, 0);
    }

    /* 2. Redo when nothing to redo — similarly must be before any undo activity */
    {
        repl_reset_state();
        do_redo();
        ASSERT_INT("redo-nothing: num_cmds still 0", g_num_cmds, 0);
    }

    /* 3. Undo/Redo basic */
    {
        repl_reset_state(); declare_test_vars();
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

    /* 4. Deleting commands — basic */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        ASSERT_INT("num_cmds 3", g_num_cmds, 3);

        delete_cmd_range(1, 1, "test");
        ASSERT_INT("num_cmds after delete", g_num_cmds, 2);
        ASSERT_STR("line 0 still there", g_cmds[0].source, "  glVertex3f(0, 0, 0);");
        ASSERT_STR("line 1 is now old line 2", g_cmds[1].source, "  glVertex3f(2, 2, 2);");
    }

    /* 5. delete_cmd_range — count=0 (no-op) */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        delete_cmd_range(0, 0, "noop");
        ASSERT_INT("delete count=0: no change", g_num_cmds, 1);
    }

    /* 6. delete_cmd_range — start=g_num_cmds (out of bounds, no-op) */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        delete_cmd_range(1, 1, "oob");  /* start == g_num_cmds (1) */
        ASSERT_INT("delete oob start: no change", g_num_cmds, 1);
    }

    /* 7. delete_cmd_range — count clamped when over-count */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        /* ask to delete 5 but only 1 remains at start=1 */
        delete_cmd_range(1, 5, "clamp");
        ASSERT_INT("delete clamp: leaves 1 cmd", g_num_cmds, 1);
        ASSERT_STR("delete clamp: cmd 0 remains", g_cmds[0].source, "  glVertex3f(0, 0, 0);");
    }

    /* 8. delete_cmd_range — edit_line clamped when it would exceed g_num_cmds */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(0,0,0)");
        repl_feed_line_public("glVertex3f(1,1,1)");
        g_edit_line = 2;  /* beyond last cmd */
        delete_cmd_range(1, 1, "elclamp");
        /* g_edit_line should be clamped to g_num_cmds (1) */
        ASSERT_INT("delete edit_line clamp: edit_line<=num_cmds", g_edit_line <= g_num_cmds, 1);
    }

    /* 9. load_line_to_input — CMD_LABEL path */
    {
        repl_reset_state();
        /* Feed a label command to create a CMD_LABEL entry */
        repl_feed_line_public(":myloop");
        ASSERT_INT("label cmd created", g_num_cmds, 1);
        ASSERT_INT("label cmd type", g_cmds[0].type, CMD_LABEL);

        /* Now load it back into input */
        load_line_to_input(0);
        /* Input should start with ':' and contain the label name */
        ASSERT_TRUE("label load: starts with colon", g_input[0] == ':');
        ASSERT_TRUE("label load: contains name", strstr(g_input, "myloop") != NULL);
    }

    /* 10. navigate_to_line — clamp target < 0 */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        navigate_to_line(-5);
        ASSERT_INT("navigate_to_line neg: edit_line=0", g_edit_line, 0);
    }

    /* 11. navigate_to_line — clamp target > g_num_cmds */
    {
        repl_reset_state();
        repl_feed_line_public("glVertex3f(1,1,1)");
        navigate_to_line(999);
        ASSERT_INT("navigate_to_line over: edit_line=g_num_cmds", g_edit_line, g_num_cmds);
    }

    /* 12. try_assign_variable — append to end (existing tests cover this) */
    {
        repl_reset_state(); declare_test_vars();
        strcpy(g_input, "n = 10.5");
        g_input_len = (int)strlen(g_input);

        int r = try_assign_variable();
        ASSERT_INT("try_assign_variable returns 1", r, 1);
        ASSERT_INT("num_cmds 1 after assign", g_num_cmds, 1);
        ASSERT_STR("assigned cmd source", g_cmds[0].source, "  n = 10.5;");
    }

    /* 13. try_assign_variable — inserting mode (inserts before cursor) */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("glVertex3f(1,1,1)");
        repl_feed_line_public("glVertex3f(2,2,2)");
        /* Put cursor at line 1, inserting mode */
        g_edit_line = 1;
        g_inserting = 1;
        strcpy(g_input, "x = 3.0");
        g_input_len = (int)strlen(g_input);

        int r = try_assign_variable();
        ASSERT_INT("insert assign returns 1", r, 1);
        ASSERT_INT("insert assign: 3 cmds", g_num_cmds, 3);
        /* The assignment should appear at index 1 */
        ASSERT_INT("insert assign: cmd 1 is VAR_ASSIGN", g_cmds[1].type, CMD_VAR_ASSIGN);
    }

    /* 14. try_assign_variable — overwrite existing cmd */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("n = 1.0");
        repl_feed_line_public("glVertex3f(1,1,1)");
        /* Navigate to the assignment line and overwrite */
        g_edit_line = 0;
        g_inserting = 0;
        strcpy(g_input, "n = 7.0");
        g_input_len = (int)strlen(g_input);

        int r = try_assign_variable();
        ASSERT_INT("overwrite assign returns 1", r, 1);
        ASSERT_STR("overwrite assign: new source", g_cmds[0].source, "  n = 7.0;");
        ASSERT_INT("overwrite assign: edit_line advanced", g_edit_line, 1);
    }

    /* 15. Committing for loops — basic open brace form */
    {
        repl_reset_state(); declare_test_vars();
        strcpy(g_input, "for(i, 0, 5) {");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_for_loop();
        ASSERT_INT("try_commit_for_loop returns 1", r, 1);
        ASSERT_INT("num_cmds 2 after for", g_num_cmds, 2);
        ASSERT_STR("for loop source", g_cmds[0].source, "  for(i, 0, 5) {");
        ASSERT_STR("for end source", g_cmds[1].source, "  }");
    }

    /* 16. try_commit_for_loop — with explicit step */
    {
        repl_reset_state();
        strcpy(g_input, "for(i, 0, 10, 2) {");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_for_loop();
        ASSERT_INT("for with step returns 1", r, 1);
        ASSERT_INT("for with step: 2 cmds", g_num_cmds, 2);
        /* Source should contain step value */
        ASSERT_TRUE("for with step: source has 2 step",
                    strstr(g_cmds[0].source, "2") != NULL);
    }

    /* 17. try_commit_for_loop — update existing for-begin header */
    {
        repl_reset_state();
        /* Create an existing for-loop */
        strcpy(g_input, "for(i, 0, 5) {");
        g_input_len = (int)strlen(g_input);
        try_commit_for_loop();
        ASSERT_INT("setup: 2 cmds", g_num_cmds, 2);

        /* Navigate back to line 0 and update the for header */
        g_edit_line = 0;
        g_inserting = 0;
        strcpy(g_input, "for(i, 0, 10) {");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_for_loop();
        ASSERT_INT("update for-begin returns 1", r, 1);
        /* Should update in place, not add more commands */
        ASSERT_INT("update for-begin: still 2 cmds", g_num_cmds, 2);
        ASSERT_TRUE("update for-begin: source updated",
                    strstr(g_cmds[0].source, "10") != NULL);
    }

    /* 18. try_commit_for_loop — inline form (for with body on same line) */
    {
        repl_reset_state();
        strcpy(g_input, "for(i, 0, 3) glVertex3f(i,0,0);");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_for_loop();
        ASSERT_INT("for inline returns 1", r, 1);
        /* Should create 3 cmds: for-begin, body, for-end */
        ASSERT_INT("for inline: 3 cmds", g_num_cmds, 3);
        ASSERT_INT("for inline: cmd 0 is FOR_BEGIN", g_cmds[0].type, CMD_FOR_BEGIN);
        ASSERT_INT("for inline: cmd 2 is FOR_END", g_cmds[2].type, CMD_FOR_END);
    }

    /* 19. Committing func defs — basic */
    {
        repl_reset_state(); declare_test_vars();
        strcpy(g_input, "func0(x, y) {");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_func_def();
        ASSERT_INT("try_commit_func_def returns 1", r, 1);
        ASSERT_INT("num_cmds 2 after func", g_num_cmds, 2);
        ASSERT_STR("func def source", g_cmds[0].source, "  func0(x, y) {");
        ASSERT_STR("func end source", g_cmds[1].source, "  }");
    }

    /* 20. try_commit_func_def — update existing func-def header */
    {
        repl_reset_state();
        strcpy(g_input, "func1(a) {");
        g_input_len = (int)strlen(g_input);
        try_commit_func_def();
        ASSERT_INT("func update setup: 2 cmds", g_num_cmds, 2);

        /* Navigate back and update the func-def header */
        g_edit_line = 0;
        g_inserting = 0;
        strcpy(g_input, "func1(a, b) {");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_func_def();
        ASSERT_INT("update func-def returns 1", r, 1);
        ASSERT_INT("update func-def: still 2 cmds", g_num_cmds, 2);
        ASSERT_TRUE("update func-def: source has b param",
                    strstr(g_cmds[0].source, "b") != NULL);
    }

    /* 21. Committing if blocks — basic */
    {
        repl_reset_state(); declare_test_vars();
        strcpy(g_input, "if(x > 0) {");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_if_block();
        ASSERT_INT("try_commit_if_block returns 1", r, 1);
        ASSERT_INT("num_cmds 2 after if", g_num_cmds, 2);
        ASSERT_STR("if block source", g_cmds[0].source, "  if(x > 0) {");
        ASSERT_STR("if end source", g_cmds[1].source, "  }");
    }

    /* 22. try_commit_if_block — update existing if-begin */
    {
        repl_reset_state(); declare_test_vars();
        strcpy(g_input, "if(x > 0) {");
        g_input_len = (int)strlen(g_input);
        try_commit_if_block();

        /* Navigate back and update the if condition */
        g_edit_line = 0;
        g_inserting = 0;
        strcpy(g_input, "if(x < 0) {");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_if_block();
        ASSERT_INT("update if-begin returns 1", r, 1);
        ASSERT_INT("update if-begin: still 2 cmds", g_num_cmds, 2);
        ASSERT_STR("update if-begin: source updated", g_cmds[0].source, "  if(x < 0) {");
    }

    /* 23. Committing close brace — for-loop */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("for(i, 0, 1) {");
        strcpy(g_input, "}");
        g_input_len = (int)strlen(g_input);
        g_edit_line = 1;

        int r = try_commit_close_brace();
        ASSERT_INT("try_commit_close_brace returns 1", r, 1);
        ASSERT_INT("num_cmds after brace", g_num_cmds, 2);
        ASSERT_STR("close brace source", g_cmds[1].source, "  }");
    }

    /* 26. Committing close brace — while in inserting mode closes existing end */
    {
        repl_reset_state();
        /* Set up: for-loop with begin+end, enter inserting mode inside */
        strcpy(g_input, "for(i, 0, 3) {");
        g_input_len = (int)strlen(g_input);
        try_commit_for_loop();
        /* g_edit_line=1, g_inserting=1 — we're inside the loop */
        ASSERT_INT("insert-close setup: in inserting", g_inserting, 1);
        ASSERT_INT("insert-close setup: edit_line=1", g_edit_line, 1);

        strcpy(g_input, "}");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_close_brace();
        ASSERT_INT("insert-close brace returns 1", r, 1);
        ASSERT_INT("insert-close: no longer inserting", g_inserting, 0);
    }

    /* 27. Committing close brace — func-def */
    {
        repl_reset_state();
        strcpy(g_input, "func2() {");
        g_input_len = (int)strlen(g_input);
        try_commit_func_def();
        /* Now close the func with '}' */
        strcpy(g_input, "}");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_close_brace();
        ASSERT_INT("func close brace returns 1", r, 1);
        ASSERT_INT("func close: no longer inserting", g_inserting, 0);
    }

    /* 28. Committing close brace — if-block */
    {
        repl_reset_state(); declare_test_vars();
        strcpy(g_input, "if(x > 0) {");
        g_input_len = (int)strlen(g_input);
        try_commit_if_block();
        /* Inserting inside if-block, close it */
        strcpy(g_input, "}");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_close_brace();
        ASSERT_INT("if close brace returns 1", r, 1);
        ASSERT_INT("if close: no longer inserting", g_inserting, 0);
    }

    /* 29. try_commit_for_loop — empty body emits error */
    {
        repl_reset_state();
        strcpy(g_input, "for(i, 0, 3) ;");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_for_loop();
        ASSERT_INT("for empty body returns 1 (error)", r, 1);
        /* No commands committed on bad body */
        ASSERT_INT("for empty body: no cmds added", g_num_cmds, 0);
    }

    /* 30. try_commit_func_def — func with no params */
    {
        repl_reset_state();
        strcpy(g_input, "func3() {");
        g_input_len = (int)strlen(g_input);

        int r = try_commit_func_def();
        ASSERT_INT("func no-params returns 1", r, 1);
        ASSERT_INT("func no-params: 2 cmds", g_num_cmds, 2);
        ASSERT_TRUE("func no-params: source correct",
                    strstr(g_cmds[0].source, "func3") != NULL);
    }

    /* 24. prof_frame_tick — increments staleness counters */
    {
        /* Call begin/end to prime a section, then tick several frames */
        prof_begin(PROF_SCENE_3D);
        prof_end(PROF_SCENE_3D);
        /* Frame tick — sections that didn't run this frame become stale */
        prof_frame_tick();
        prof_frame_tick();
        /* Should not crash; calling multiple times is fine */
        ASSERT_TRUE("prof_frame_tick survives", 1);
    }

    /* 25. prof_code_panel_details_enabled — reflects g_show_profile_panel */
    {
        g_show_profile_panel = PROFILE_PANEL_OFF;
        ASSERT_INT("details disabled when OFF", prof_code_panel_details_enabled(), 0);

        g_show_profile_panel = PROFILE_PANEL_DETAILS;
        ASSERT_INT("details enabled when DETAILS", prof_code_panel_details_enabled(), 1);

        g_show_profile_panel = PROFILE_PANEL_OFF;  /* restore */
    }

    printf("\n%d / %d tests passed\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
