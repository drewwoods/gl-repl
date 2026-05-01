#include "repl_code_panel_layout.h"
#include "support/test_harness.h"

#include <stdio.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_INT(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

static CodePanelTextLayout test_layout(int panel_w, int first_x,
                                       int char_w, int wrap) {
    CodePanelTextLayout layout =
        repl_code_panel_layout_make(panel_w, first_x, char_w, wrap);
    layout.right_pad_px = 0;
    return layout;
}

int main(void) {
    printf("--- repl_code_panel_layout tests ---\n");

    {
        CodePanelTextLayout default_layout =
            repl_code_panel_layout_make(12, 0, 1, 1);
        CodePanelTextLayout layout = test_layout(8, 0, 1, 1);
        ASSERT_INT("available chars uses default right pad",
                   repl_code_panel_available_chars(&default_layout, 0), 8);
        ASSERT_INT("available chars honors custom right pad",
                   repl_code_panel_available_chars(&layout, 0), 8);
        ASSERT_INT("available chars clamps too-narrow width",
                   repl_code_panel_available_chars(&layout, 8), 0);
    }

    {
        ASSERT_INT("continuation indent aligns to function args",
                   repl_code_panel_cont_indent_chars("  glVertex3f(1, 2, 3)",
                                                     12),
                   13);
        ASSERT_INT("continuation indent caps long names",
                   repl_code_panel_cont_indent_chars(
                       "  veryLongFunctionName(1, 2)", 12),
                   14);
        ASSERT_INT("continuation indent without paren",
                   repl_code_panel_cont_indent_chars("  // comment", 12),
                   6);
    }

    {
        const char *text = "abc, def, ghi";
        int len = 13;
        ASSERT_INT("wrap break prefers comma in window",
                   repl_code_panel_find_wrap_break(text, 0, 8, len), 3);
        ASSERT_INT("wrap break falls back to secondary char",
                   repl_code_panel_find_wrap_break("abcdef ghij", 0, 9, 11), 6);
        ASSERT_INT("wrap break extends to next comma",
                   repl_code_panel_find_wrap_break("abcdef,ghi", 0, 3, 10), 6);
    }

    {
        CodePanelTextLayout layout = test_layout(4, 0, 1, 1);
        int start = -1, len = -1, x = -1;
        ASSERT_INT("row count wraps at comma",
                   repl_code_panel_row_count_for_text("abc,def,ghi", &layout),
                   2);
        ASSERT_TRUE("segment row 0 exists",
                    repl_code_panel_segment_for_row("abc,def,ghi", &layout,
                                                    0, &start, &len, &x));
        ASSERT_INT("segment row 0 start", start, 0);
        ASSERT_INT("segment row 0 len", len, 4);
        ASSERT_INT("segment row 0 x", x, 0);
        ASSERT_TRUE("segment row 1 exists",
                    repl_code_panel_segment_for_row("abc,def,ghi", &layout,
                                                    1, &start, &len, &x));
        ASSERT_INT("segment row 1 start", start, 4);
        ASSERT_INT("segment row 1 len", len, 7);
        ASSERT_INT("segment row 1 continuation x", x, 4);
    }

    {
        CodePanelTextLayout layout = test_layout(4, 0, 1, 1);
        int seg_start = -1, seg_len = -1, seg_x = -1;
        ASSERT_INT("cursor before wrap stays row 0",
                   repl_code_panel_cursor_row_for_text("abc,def,ghi", &layout,
                                                       2, &seg_start,
                                                       &seg_len, &seg_x),
                   0);
        ASSERT_INT("cursor row 0 seg start", seg_start, 0);
        ASSERT_INT("cursor after wrap moves to row 1",
                   repl_code_panel_cursor_row_for_text("abc,def,ghi", &layout,
                                                       5, &seg_start,
                                                       &seg_len, &seg_x),
                   1);
        ASSERT_INT("cursor row 1 seg x", seg_x, 4);
    }

    {
        CodePanelTextLayout layout = test_layout(4, 0, 1, 0);
        ASSERT_INT("wrap disabled keeps one row",
                   repl_code_panel_row_count_for_text("abc,def,ghi", &layout),
                   1);
        ASSERT_INT("empty text still has one visual row",
                   repl_code_panel_row_count_for_text("", &layout), 1);
    }

    return test_harness_report(&g_harness, "test_repl_code_panel_layout");
}
