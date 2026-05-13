#include "ui/text_layout.h"
#include "support/test_harness.h"

#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_INT(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

static CodeLayout test_layout(int panel_w, int first_x,
                                       int char_w, int wrap) {
    CodeLayout layout =
        code_layout_make(panel_w, first_x, char_w, wrap);
    layout.right_pad_px = 0;
    return layout;
}

/* ---- realistic panel helpers ----
 * Match the real code-panel geometry so wrap tests exercise the same
 * parameters the renderer uses at runtime.
 *
 * FONT_W = 9              (include/ui/gl_2d.h)
 * CODE_MARGIN_X = 10      (src/ui/metrics.h)
 * linenum column = 4 chars
 * index column   = 6 chars (when vertex indices enabled)
 * 1-char gap between linenum and index/text columns              */
#define PANEL_FONT_W       9
#define PANEL_MARGIN_X    10
#define PANEL_LINENUM_W   (4 * PANEL_FONT_W)
#define PANEL_IDX_COL_W   (6 * PANEL_FONT_W)
#define PANEL_IDX_X       (PANEL_MARGIN_X + PANEL_LINENUM_W + PANEL_FONT_W)

static int panel_text_x(int show_indices) {
    return PANEL_IDX_X + (show_indices ? PANEL_IDX_COL_W : 0);
}

static CodeLayout panel_layout(int panel_w, int show_indices, int wrap) {
    return code_layout_make(panel_w, panel_text_x(show_indices),
                            PANEL_FONT_W, wrap);
}

static int seg_text_eq(const char *text, const CodeLayout *layout,
                       int row, const char *expected) {
    int start, len, x;
    if (!code_layout_segment_for_row(text, layout, row, &start, &len, &x))
        return 0;
    int exp_len = (int)strlen(expected);
    return (len == exp_len && strncmp(text + start, expected, (size_t)len) == 0);
}

int main(void) {
    printf("--- code_layout tests ---\n");

    /* ---- unit-level tests (char_w = 1, no padding) ---- */

    {
        CodeLayout default_layout =
            code_layout_make(12, 0, 1, 1);
        CodeLayout layout = test_layout(8, 0, 1, 1);
        ASSERT_INT("available chars uses default right pad",
                   code_layout_available_chars(&default_layout, 0), 8);
        ASSERT_INT("available chars honors custom right pad",
                   code_layout_available_chars(&layout, 0), 8);
        ASSERT_INT("available chars clamps too-narrow width",
                   code_layout_available_chars(&layout, 8), 0);
    }

    {
        ASSERT_INT("continuation indent aligns to function args",
                   code_layout_cont_indent_chars("  glVertex3f(1, 2, 3)",
                                                     12),
                   13);
        ASSERT_INT("continuation indent caps long names",
                   code_layout_cont_indent_chars(
                       "  veryLongFunctionName(1, 2)", 12),
                   14);
        ASSERT_INT("continuation indent without paren",
                   code_layout_cont_indent_chars("  // comment", 12),
                   6);
    }

    {
        const char *text = "abc, def, ghi";
        int len = 13;
        ASSERT_INT("wrap break prefers comma in window",
                   code_layout_find_wrap_break(text, 0, 8, len), 3);
        ASSERT_INT("wrap break falls back to secondary char",
                   code_layout_find_wrap_break("abcdef ghij", 0, 9, 11), 6);
        ASSERT_INT("wrap break extends to next comma",
                   code_layout_find_wrap_break("abcdef,ghi", 0, 3, 10), 6);
    }

    {
        CodeLayout layout = test_layout(4, 0, 1, 1);
        int start = -1, len = -1, x = -1;
        ASSERT_INT("row count wraps at comma",
                   code_layout_row_count_for_text("abc,def,ghi", &layout),
                   2);
        ASSERT_TRUE("segment row 0 exists",
                    code_layout_segment_for_row("abc,def,ghi", &layout,
                                                    0, &start, &len, &x));
        ASSERT_INT("segment row 0 start", start, 0);
        ASSERT_INT("segment row 0 len", len, 4);
        ASSERT_INT("segment row 0 x", x, 0);
        ASSERT_TRUE("segment row 1 exists",
                    code_layout_segment_for_row("abc,def,ghi", &layout,
                                                    1, &start, &len, &x));
        ASSERT_INT("segment row 1 start", start, 4);
        ASSERT_INT("segment row 1 len", len, 7);
        ASSERT_INT("segment row 1 continuation x", x, 4);
    }

    {
        CodeLayout layout = test_layout(4, 0, 1, 1);
        int seg_start = -1, seg_len = -1, seg_x = -1;
        ASSERT_INT("cursor before wrap stays row 0",
                   code_layout_cursor_row_for_text("abc,def,ghi", &layout,
                                                       2, &seg_start,
                                                       &seg_len, &seg_x),
                   0);
        ASSERT_INT("cursor row 0 seg start", seg_start, 0);
        ASSERT_INT("cursor after wrap moves to row 1",
                   code_layout_cursor_row_for_text("abc,def,ghi", &layout,
                                                       5, &seg_start,
                                                       &seg_len, &seg_x),
                   1);
        ASSERT_INT("cursor row 1 seg x", seg_x, 4);
    }

    {
        CodeLayout layout = test_layout(4, 0, 1, 0);
        ASSERT_INT("wrap disabled keeps one row",
                   code_layout_row_count_for_text("abc,def,ghi", &layout),
                   1);
        ASSERT_INT("empty text still has one visual row",
                   code_layout_row_count_for_text("", &layout), 1);
    }

    /* ---- realistic panel wrap tests ----
     * Use the same font, margin, and panel geometry the live code panel
     * renderer sees.  Panel width 270 = (int)(360 * 0.75) (left layout
     * at 75% frac on a 360-px window).  text_x = 55 without vertex
     * indices.                                                         */

    {
        const char *text = "  glColor4f(0.125, 0.250, 0.500, 1.000);";
        CodeLayout layout = panel_layout(270, 0, 1);

        ASSERT_INT("color4f wraps to 4 rows",
                   code_layout_row_count_for_text(text, &layout), 4);
        ASSERT_TRUE("color4f row 0 breaks after first comma",
                    seg_text_eq(text, &layout, 0, "  glColor4f(0.125,"));
        ASSERT_TRUE("color4f row 1 is second arg",
                    seg_text_eq(text, &layout, 1, " 0.250,"));
        ASSERT_TRUE("color4f row 2 is third arg",
                    seg_text_eq(text, &layout, 2, " 0.500,"));
        ASSERT_TRUE("color4f row 3 is last arg + close",
                    seg_text_eq(text, &layout, 3, " 1.000);"));

        int start, len, x;
        code_layout_segment_for_row(text, &layout, 0, &start, &len, &x);
        int first_x = x;
        code_layout_segment_for_row(text, &layout, 1, &start, &len, &x);
        ASSERT_TRUE("color4f continuation x > first row x", x > first_x);
        int hang = (x - first_x) / PANEL_FONT_W;
        ASSERT_INT("color4f hang indent = 12 (paren at col 11 + 1, capped)",
                   hang, 12);
    }

    {
        const char *text =
            "  glVertex3f(123456789012345678901234567890, 0.250, 0.500);";
        CodeLayout layout = panel_layout(270, 0, 1);

        ASSERT_INT("overflow wraps to 3 rows",
                   code_layout_row_count_for_text(text, &layout), 3);
        ASSERT_TRUE("overflow row 0 extends to first comma past budget",
                    seg_text_eq(text, &layout, 0,
                                "  glVertex3f(123456789012345678901234567890,"));
        ASSERT_TRUE("overflow row 1 is second arg",
                    seg_text_eq(text, &layout, 1, " 0.250,"));
        ASSERT_TRUE("overflow row 2 is last arg + close",
                    seg_text_eq(text, &layout, 2, " 0.500);"));
    }

    {
        const char *text =
            "  glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION,"
            " (GLfloat[]){0.2, 0, 0.15});";
        CodeLayout layout = panel_layout(270, 0, 1);
        int rows = code_layout_row_count_for_text(text, &layout);

        ASSERT_TRUE("point-param wraps to multiple rows", rows >= 3);
        ASSERT_TRUE("point-param row 0 breaks after enum constant",
                    seg_text_eq(text, &layout, 0,
                        "  glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION,"));
        ASSERT_TRUE("point-param row 1 is compound literal",
                    seg_text_eq(text, &layout, 1, " (GLfloat[])"));

        int start, len, x;
        code_layout_segment_for_row(text, &layout, 1, &start, &len, &x);
        int hang = (x - panel_text_x(0)) / PANEL_FONT_W;
        ASSERT_TRUE("point-param hang indent capped (not uncapped 21)",
                    hang < 21);
        ASSERT_INT("point-param hang indent = 2 leading + 12 max cap",
                   hang, 14);
    }

    /* Narrow panel: first arg itself overflows the available width, but
     * the iterator must still emit the first row with real content
     * (extending to the first reachable comma), not a blank row.       */
    {
        const char *text =
            "  glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION,"
            " (GLfloat[]){0.2, 0, 0.15});";
        CodeLayout layout = panel_layout(195, 0, 1);   /* 260 * 0.75 */
        int start, len, x;

        code_layout_segment_for_row(text, &layout, 0, &start, &len, &x);
        ASSERT_TRUE("narrow panel row 0 has content (not blank)",
                    len > 2);
        ASSERT_TRUE("narrow panel row 0 starts at beginning",
                    start == 0);
    }

    /* Full-width panel (top / bottom / hidden layouts give the whole
     * window width). A short line that wraps in a side panel must fit
     * on a single row at full width.                                   */
    {
        const char *text = "  glVertex3f(1, 2, 3);";
        CodeLayout layout = panel_layout(360, 0, 1);

        ASSERT_INT("full-width panel keeps short line on one row",
                   code_layout_row_count_for_text(text, &layout), 1);
    }

    /* Vertex index column shifts text_x, changing the wrap budget. The
     * same text that wraps to N rows without indices must wrap to more
     * rows with the index column enabled (text_x jumps by 54 px).      */
    {
        const char *text = "  glColor4f(0.125, 0.250, 0.500, 1.000);";
        CodeLayout no_idx = panel_layout(270, 0, 1);
        CodeLayout with_idx = panel_layout(270, 1, 1);

        int rows_off = code_layout_row_count_for_text(text, &no_idx);
        int rows_on  = code_layout_row_count_for_text(text, &with_idx);
        ASSERT_TRUE("vertex indices column increases row count",
                    rows_on > rows_off);
    }

    /* Wrap disabled: even a long line stays on one row. */
    {
        const char *text = "  glColor4f(0.125, 0.250, 0.500, 1.000);";
        CodeLayout layout = panel_layout(270, 0, 0);
        ASSERT_INT("wrap disabled keeps long line on one row",
                   code_layout_row_count_for_text(text, &layout), 1);
    }

    return test_harness_report(&g_harness, "test_code_layout");
}
