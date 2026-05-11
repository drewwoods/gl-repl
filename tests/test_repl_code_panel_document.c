#include "editor/state.h"
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "ui/gl_2d.h"
#include "editor/input.h"
#include <stdio.h>
#include <string.h>

#include "ui/state.h"
#include "editor/code_panel_document.h"
#include "repl/core.h"
#include "repl/state.h"
#include "ui/layout.h"
#include "ui/metrics.h"
#include "support/test_harness.h"

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(name, cond) \
    TEST_ASSERT_TRUE(&g_harness, name, cond)

static int code_panel_text_x(void) {
    int linenum_w = 4 * FONT_W;
    int idx_col_w = glr_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    return idx_x + idx_col_w;
}

static void reset_doc_fixture(void) {
    glr_app_reset_all();
    ui_state_viewport_set_size(800, 260);
    ui_state_code_panel_mut()->panel_frac = 0.45f;
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
    glr_state_presentation_mut()->show_vertex_indices = 0; glr_ctrl_sync_ui_chrome();
    editor_scroll_set(0);
    editor_scroll_follow_cursor_set(0);
    repl_state_refresh_workspace_header_lines();
}

static void build_doc(CodePanelDocumentLayout *layout) {
    int cp_w, cp_h;
    ui_layout_code_panel_rect(NULL, NULL, &cp_w, &cp_h);
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
    editor_cursor_pos_set(0);
    editor_handle_key('\r', 0, 0);
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
    editor_scroll_set(0);
    editor_scroll_follow_cursor_set(1);
    build_doc(&layout);
    repl_code_panel_document_apply_follow_scroll(&layout);
    ASSERT_TRUE("follow line visible after apply",
                layout.follow_doc_line >= editor_scroll() &&
                layout.follow_doc_line < editor_scroll() + layout.visible_lines);

    return test_harness_report(&g_harness, "repl_code_panel_document");
}
