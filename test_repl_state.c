#include "repl_state.h"

#include <stdio.h>
#include <string.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s] (line %d)\n", label, __LINE__); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    g_run++; \
    if ((got) == (exp)) g_pass++; \
    else printf("FAIL [%s] got %d, expected %d (line %d)\n", \
                label, (int)(got), (int)(exp), __LINE__); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    g_run++; \
    if (strcmp((got), (exp)) == 0) g_pass++; \
    else printf("FAIL [%s] got \"%s\", expected \"%s\" (line %d)\n", \
                label, (got), (exp), __LINE__); \
} while (0)

static void test_capture_restore_round_trip(void) {
    char err[128];
    static ReplRuntimeState snapshot;
    int foo_idx;

    repl_state_init_defaults();
    repl_state_input_set_text("glVertex3f(1, 2, 3)");
    repl_state_cursor_pos_set(6);
    repl_state_document_count_set(2);
    repl_state_edit_line_set(1);
    repl_state_help_mut()->visible = 1;
    *repl_state_code_panel_mut()->scroll = 9;
    repl_state_search_mut()->active = 1;
    snprintf(repl_state_search_mut()->query,
             sizeof(repl_state_search_mut()->query),
             "%s", "vertex");
    repl_state_search_mut()->query_len = 6;
    *repl_state_variables_mut()->time_playing = 0;
    repl_state_camera_set_orbit(11.0f, 22.0f);
    repl_state_workspace_set_dir("/tmp/repl-state-stage1");

    repl_eval_declare_predef_var("foo", err, sizeof(err));
    foo_idx = repl_eval_find_predef_var_idx("foo");
    ASSERT_TRUE("foo var declared", foo_idx >= 0);
    if (foo_idx >= 0)
        g_predef_vars[foo_idx].value = 42.0f;

    repl_state_capture(&snapshot);

    repl_state_input_clear();
    repl_state_document_count_set(0);
    repl_state_edit_line_set(0);
    repl_state_help_mut()->visible = 0;
    *repl_state_code_panel_mut()->scroll = 0;
    repl_state_search_clear();
    *repl_state_variables_mut()->time_playing = 1;
    repl_state_camera_set_orbit(99.0f, 88.0f);
    repl_state_workspace_set_dir("/tmp/repl-state-mutated");
    if (foo_idx >= 0)
        g_predef_vars[foo_idx].value = -1.0f;

    repl_state_restore(&snapshot);

    ASSERT_STR("input restored",
               repl_state_input_text(),
               "glVertex3f(1, 2, 3)");
    ASSERT_INT("cursor restored", repl_state_cursor_pos(), 6);
    ASSERT_INT("document count restored", repl_state_document_count(), 2);
    ASSERT_INT("edit line restored", repl_state_edit_line(), 1);
    ASSERT_INT("help restored", repl_state_help()->visible, 1);
    ASSERT_INT("code panel scroll restored", *repl_state_code_panel()->scroll, 9);
    ASSERT_INT("search active restored", repl_state_search()->active, 1);
    ASSERT_STR("search query restored", repl_state_search()->query, "vertex");
    ASSERT_INT("time playing restored", *repl_state_variables()->time_playing, 0);
    ASSERT_TRUE("camera rx restored", *repl_state_camera()->rx == 11.0f);
    ASSERT_TRUE("camera ry restored", *repl_state_camera()->ry == 22.0f);
    ASSERT_STR("workspace restored",
               repl_state_workspace_dir(),
               "/tmp/repl-state-stage1");

    foo_idx = repl_eval_find_predef_var_idx("foo");
    ASSERT_TRUE("foo var restored", foo_idx >= 0);
    if (foo_idx >= 0)
        ASSERT_TRUE("foo value restored", g_predef_vars[foo_idx].value == 42.0f);
}

int main(void) {
    printf("--- repl_state tests ---\n");
    test_capture_restore_round_trip();
    printf("PASS %d/%d\n", g_pass, g_run);
    return g_pass == g_run ? 0 : 1;
}