#include "glr_ctrl.h"
#include "editor/input.h"
#include "editor/search.h"
#include "repl_core.h"

#define g_search_active    (editor_state_search_mut()->active)
#define g_search_query     (editor_state_search_mut()->query)
#define g_search_query_len (editor_state_search_mut()->query_len)
#define g_search_cursor_pos (editor_state_search_mut()->cursor_pos)
#define g_search_match_count (editor_state_search_mut()->match_count)
#define g_search_hit_line  (editor_state_search_mut()->hit_line_idx)
#define g_search_hit_char  (editor_state_search_mut()->hit_char_idx)
#define g_search_hit_ordinal (editor_state_search_mut()->hit_ordinal)

#include "support/test_harness.h"
#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

static void open_search(void) {
    editor_handle_key(6, 0, 0); /* Ctrl+F */
}

static void type_search_text(const char *text) {
    while (*text) {
        editor_handle_key((unsigned char)*text, 0, 0);
        text++;
    }
}

static void declare_test_vars(void) {
    char err[128];
    repl_eval_declare_predef_var("x", err, sizeof(err));
    repl_eval_declare_predef_var("y", err, sizeof(err));
    repl_eval_declare_predef_var("z", err, sizeof(err));
    repl_eval_declare_predef_var("i", err, sizeof(err));
    repl_eval_declare_predef_var("j", err, sizeof(err));
    repl_eval_declare_predef_var("k", err, sizeof(err));
    repl_eval_declare_predef_var("n", err, sizeof(err));
}

static void set_live_input(const char *text) {
    ReplEditorInputState *inp = editor_state_input_mut();
    strncpy(inp->input, text, MAX_INPUT_LEN - 1);
    inp->input[MAX_INPUT_LEN - 1] = '\0';
    inp->input_len = (int)strlen(inp->input);
    editor_cursor_pos_set(inp->input_len);
}

int main(void) {
    repl_eval_init_predef_vars();

    {
        ASSERT_TRUE("find prev empty query",
                    repl_search_find_prev_in_text("abc", "", 1) == -1);
        ASSERT_TRUE("find prev empty text",
                    repl_search_find_prev_in_text("", "a", 0) == -1);
        ASSERT_TRUE("find prev start clamped",
                    repl_search_find_prev_in_text("abc abc", "abc", 999) == 4);
        ASSERT_TRUE("find prev single char",
                    repl_search_find_prev_in_text("xyz", "z", 2) == 2);
        ASSERT_TRUE("find prev last occurrence",
                    repl_search_find_prev_in_text("one two one", "one", 999) == 8);
        ASSERT_TRUE("find prev start position match",
                    repl_search_find_prev_in_text("abcabc", "abc", 3) == 3);
        ASSERT_TRUE("find prev case insensitive",
                    repl_search_find_prev_in_text("AbCaBc", "abc", 99) == 3);
        ASSERT_TRUE("find prev no match",
                    repl_search_find_prev_in_text("abc", "zzz", 2) == -1);
    }

    glr_app_reset_all(); declare_test_vars();
    set_live_input("NeedleLine");
    open_search();
    type_search_text("needle");
    ASSERT_TRUE("search state active before clear", g_search_active == 1);
    ASSERT_TRUE("search state has query before clear", g_search_query_len == 6);
    ASSERT_TRUE("search state has hit line before clear", g_search_hit_line >= 0);
    ASSERT_TRUE("search state has hit char before clear", g_search_hit_char >= 0);
    search_clear_all();
    ASSERT_TRUE("search clear resets active", g_search_active == 0);
    ASSERT_TRUE("search clear resets query", g_search_query_len == 0);
    ASSERT_TRUE("search clear resets query string", g_search_query[0] == '\0');
    ASSERT_TRUE("search clear resets cursor", g_search_cursor_pos == 0);
    ASSERT_TRUE("search clear resets hit line", g_search_hit_line == -1);
    ASSERT_TRUE("search clear resets hit char", g_search_hit_char == -1);
    ASSERT_TRUE("search clear resets hit ordinal", g_search_hit_ordinal == 0);
    ASSERT_TRUE("search clear resets match count", g_search_match_count == 0);

    glr_app_reset_all(); declare_test_vars();
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_feed_line_public("glEnd();");
    open_search();
    type_search_text("definitelynomatch");
    ASSERT_TRUE("search no match count zero", g_search_match_count == 0);
    ASSERT_TRUE("search no match hit line", g_search_hit_line == -1);

    printf("repl_core_search_extra: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.run == g_harness.passed) ? 0 : 1;
}
