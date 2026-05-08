#include "editor_input.h"
#include "editor_search.h"
#include "repl_core.h"
#include "repl_state.h"

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

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glColor3f(1, 0, 0);");
    repl_feed_line_public("glEnd();");
    {
        char before0[MAX_LINE_LEN];
        int before_num = repl_state_document_count();

        const char *buf_line0 = editor_buffer_line(0);
        strncpy(before0, buf_line0 ? buf_line0 : "", sizeof(before0) - 1);
        before0[sizeof(before0) - 1] = '\0';

        open_search();
        ASSERT_TRUE("ctrl-f activates search", g_search_active == 1);
        ASSERT_TRUE("ctrl-f leaves query empty", g_search_query_len == 0);
        ASSERT_TRUE("ctrl-f leaves cmd count unchanged", repl_state_document_count() == before_num);
        ASSERT_TRUE("ctrl-f leaves source unchanged",
                    strcmp(editor_buffer_line(0), before0) == 0);
    }

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("// color COLOR");
    repl_feed_line_public("glColor3f(1, 0, 0);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    open_search();
    type_search_text("CoLoR");
    {
        int first = repl_search_find_next_in_text(repl_search_row_text(0),
                                                  g_search_query, 0);
        int second = repl_search_find_next_in_text(repl_search_row_text(0),
                                                   g_search_query, first + 1);

        ASSERT_TRUE("search matches case-insensitively", g_search_match_count == 3);
        ASSERT_TRUE("search jumps to first hit line", repl_state_edit_line() == 0);
        ASSERT_TRUE("search first hit line", g_search_hit_line == 0);
        ASSERT_TRUE("search first hit char", g_search_hit_char == first);
        ASSERT_TRUE("search first ordinal", g_search_hit_ordinal == 1);

        editor_handle_key('\n', 0, 0);
        ASSERT_TRUE("enter navigates next same-line hit", repl_state_edit_line() == 0);
        ASSERT_TRUE("enter next hit char", g_search_hit_char == second);
        ASSERT_TRUE("enter next ordinal", g_search_hit_ordinal == 2);

        editor_handle_special(GLUT_KEY_DOWN, 0, 0);
        ASSERT_TRUE("down navigates next line", repl_state_edit_line() == 1);
        ASSERT_TRUE("down hit line", g_search_hit_line == 1);
        ASSERT_TRUE("down hit ordinal", g_search_hit_ordinal == 3);
        ASSERT_TRUE("down hit char",
                    g_search_hit_char ==
                    repl_search_find_next_in_text(repl_search_row_text(1),
                                                  g_search_query, 0));

        editor_handle_special(GLUT_KEY_DOWN, 0, 0);
        ASSERT_TRUE("down wraps to first hit", repl_state_edit_line() == 0);
        ASSERT_TRUE("down wrap line", g_search_hit_line == 0);
        ASSERT_TRUE("down wrap char", g_search_hit_char == first);
        ASSERT_TRUE("down wrap ordinal", g_search_hit_ordinal == 1);

        editor_handle_special(GLUT_KEY_UP, 0, 0);
        ASSERT_TRUE("up wraps to last hit", repl_state_edit_line() == 1);
        ASSERT_TRUE("up wrap line", g_search_hit_line == 1);
        ASSERT_TRUE("up wrap ordinal", g_search_hit_ordinal == 3);
    }

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_feed_line_public("glEnd();");
    repl_navigate_to_line(1);
    open_search();
    type_search_text("#include");
    ASSERT_TRUE("header text is not searched", g_search_match_count == 0);
    ASSERT_TRUE("header search has no current hit", g_search_hit_line == -1);
    ASSERT_TRUE("zero-hit search leaves line unchanged", repl_state_edit_line() == 1);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_navigate_to_line(0);
    set_live_input("NeedleLine");
    open_search();
    type_search_text("needle");
    ASSERT_TRUE("active edit line buffer is searchable", g_search_match_count == 1);
    ASSERT_TRUE("active edit line hit line", g_search_hit_line == 0);
    ASSERT_TRUE("active edit line hit char", g_search_hit_char == 0);
    ASSERT_TRUE("active edit line search uses live input",
                strcmp(repl_search_row_text(0), "NeedleLine") == 0);

    repl_reset_state(); declare_test_vars();
    set_live_input("ColorProbe");
    open_search();
    type_search_text("color");
    ASSERT_TRUE("newline buffer is searchable", g_search_match_count == 1);
    ASSERT_TRUE("newline hit line", g_search_hit_line == 0);
    ASSERT_TRUE("newline hit char", g_search_hit_char == 0);
    ASSERT_TRUE("newline search keeps current line", repl_state_edit_line() == 0);

    editor_handle_key(27, 0, 0);
    ASSERT_TRUE("escape closes search", g_search_active == 0);
    ASSERT_TRUE("escape clears search query", g_search_query_len == 0);
    ASSERT_TRUE("escape clears search hit", g_search_hit_line == -1);
    ASSERT_TRUE("escape clears match count", g_search_match_count == 0);

    {
        const char *text = "abcabc";
        ASSERT_TRUE("find next empty query",
                    repl_search_find_next_in_text(text, "", 0) == -1);
        ASSERT_TRUE("find next empty text",
                    repl_search_find_next_in_text("", "a", 0) == -1);
        ASSERT_TRUE("find next start at strlen",
                    repl_search_find_next_in_text(text, "a", (int)strlen(text)) == -1);
        ASSERT_TRUE("find next query longer than text",
                    repl_search_find_next_in_text("ab", "abcd", 0) == -1);
        ASSERT_TRUE("find next single char at zero",
                    repl_search_find_next_in_text("z", "z", 0) == 0);
        ASSERT_TRUE("find next match at start pos",
                    repl_search_find_next_in_text(text, "abc", 3) == 3);
    }

    {
        ASSERT_TRUE("find prev empty query",
                    repl_search_find_prev_in_text("abc", "", 0) == -1);
        ASSERT_TRUE("find prev empty text",
                    repl_search_find_prev_in_text("", "a", 0) == -1);
        ASSERT_TRUE("find prev start larger than strlen",
                    repl_search_find_prev_in_text("abc abc", "abc", 999) == 4);
        ASSERT_TRUE("find prev single character",
                    repl_search_find_prev_in_text("xyz", "y", 2) == 1);
        ASSERT_TRUE("find prev last occurrence with past start",
                    repl_search_find_prev_in_text("abaaba", "aba", 99) == 3);
    }

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("abc");
    open_search();
    type_search_text("abc");
    ASSERT_TRUE("search cursor starts at query len", g_search_cursor_pos == 3);
    editor_handle_special(GLUT_KEY_LEFT, 0, 0);
    ASSERT_TRUE("search cursor left", g_search_cursor_pos == 2);
    editor_handle_special(GLUT_KEY_HOME, 0, 0);
    ASSERT_TRUE("search cursor home", g_search_cursor_pos == 0);
    editor_handle_special(GLUT_KEY_RIGHT, 0, 0);
    ASSERT_TRUE("search cursor right", g_search_cursor_pos == 1);
    editor_handle_special(GLUT_KEY_END, 0, 0);
    ASSERT_TRUE("search cursor end", g_search_cursor_pos == g_search_query_len);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("abcd");
    open_search();
    type_search_text("abcd");
    ASSERT_TRUE("search backspace initial len", g_search_query_len == 4);
    editor_handle_key(127, 0, 0);
    ASSERT_TRUE("search backspace decrements len", g_search_query_len == 3);
    ASSERT_TRUE("search backspace removes last char",
                strcmp(g_search_query, "abc") == 0);

    repl_reset_state(); declare_test_vars();
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_feed_line_public("glEnd();");
    editor_insert_mode_set(1);
    repl_state_edit_line_set(1);
    ASSERT_TRUE("row count inserting includes input row",
                repl_search_row_count() == repl_state_document_count() + 1);
    ASSERT_TRUE("row map inserting shifts at edit line",
                repl_search_row_for_cmd_index(1) == 2);
    ASSERT_TRUE("row map inserting keeps prior rows",
                repl_search_row_for_cmd_index(0) == 0);

    printf("repl_core_search: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.run == g_harness.passed) ? 0 : 1;
}
