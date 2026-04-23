#include <stdio.h>
#include <string.h>

#include "sample.h"
#include "repl_code_panel_document.h"
#include "repl_core.h"
#include "ui_panels.h"

static int g_tests = 0;
static int g_failed = 0;

#define ASSERT_TRUE(name, cond) do {                                      \
    g_tests++;                                                            \
    if (!(cond)) {                                                        \
        g_failed++;                                                       \
        printf("FAIL [%s] line %d\n", (name), __LINE__);                  \
    }                                                                     \
} while (0)

static int code_panel_text_x(void) {
    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    return idx_x + idx_col_w;
}

static void reset_doc_fixture(void) {
    repl_reset_state();
    repl_state_viewport_set_size(800, 260);
    g_panel_frac = 0.45f;
    g_code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    g_show_indices = 0;
    g_scroll = 0;
    g_scroll_follow_cursor = 0;
    refresh_workspace_header_lines();
}

static void build_doc(CodePanelDocumentLayout *layout) {
    int cp_w, cp_h;
    code_panel_rect(NULL, NULL, &cp_w, &cp_h);
    repl_code_panel_document_build(layout, cp_w, code_panel_text_x(), cp_h);
}

int main(void) {
    CodePanelDocumentLayout layout;
    int target = -99;
    int on_insert = -99;
    int row_offset = -99;

    reset_doc_fixture();
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_feed_line_public("glColor3f(1, 0, 0);");
    repl_feed_line_public("glEnd();");
    build_doc(&layout);

    ASSERT_TRUE("document counts commands",
                layout.total_lines >= layout.header_rows + layout.footer_rows + 4);
    ASSERT_TRUE("visible lines nonzero", layout.visible_lines > 0);
    ASSERT_TRUE("first command has one row", layout.cmd_main_rows[0] == 1);

    {
        int doc_line = layout.header_rows + layout.cmd_main_rows[0];
        ASSERT_TRUE("target lookup succeeds",
                    repl_code_panel_document_target_for_doc_line(
                        doc_line, &layout, &target, &on_insert, &row_offset));
        ASSERT_TRUE("target lookup command index", target == 1);
        ASSERT_TRUE("target lookup is source row", on_insert == 0);
        ASSERT_TRUE("target lookup row offset", row_offset == 0);
    }

    repl_navigate_to_line(1);
    g_cursor_pos = 0;
    repl_keyboard_func('\r', 0, 0);
    build_doc(&layout);
    {
        int doc_line = layout.header_rows + layout.cmd_main_rows[0];
        ASSERT_TRUE("insert row lookup succeeds",
                    repl_code_panel_document_target_for_doc_line(
                        doc_line, &layout, &target, &on_insert, &row_offset));
        ASSERT_TRUE("insert row reports virtual line", target == -1);
        ASSERT_TRUE("insert row flag", on_insert == 1);
    }

    reset_doc_fixture();
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_feed_line_public("glColor3f(1, 0, 0);");
    repl_feed_line_public("glEnd();");
    repl_navigate_to_line(2);
    g_scroll = 0;
    g_scroll_follow_cursor = 1;
    build_doc(&layout);
    repl_code_panel_document_apply_follow_scroll(&layout);
    ASSERT_TRUE("follow line visible after apply",
                layout.follow_doc_line >= g_scroll &&
                layout.follow_doc_line < g_scroll + layout.visible_lines);

    if (g_failed) {
        printf("repl_code_panel_document: %d/%d passed\n",
               g_tests - g_failed, g_tests);
        return 1;
    }
    printf("repl_code_panel_document: %d/%d passed\n", g_tests, g_tests);
    return 0;
}
