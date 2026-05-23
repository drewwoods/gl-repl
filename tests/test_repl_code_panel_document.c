#include "editor/state.h"
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "ui/core/gl_2d.h"
#include "editor/input.h"
#include <stdio.h>
#include <string.h>

#include "ui/app/state.h"
#include "repl/core.h"
#include "repl/state.h"
#include "ui/core/layout.h"
#include "ui/core/metrics.h"
#include "ui/app/repl_code_panel.h"
#include "ui/app/panels.h"
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

static void build_doc(UiRenderSnapshot *snap, UiReplCodePanelLayout *layout) {
    int cp_w, cp_h;
    ui_layout_code_panel_rect(NULL, NULL, &cp_w, &cp_h);
    glr_ctrl_build_ui_snapshot(snap);
    ui_repl_code_panel_build_layout(snap, layout, cp_w, code_panel_text_x(), cp_h);
}

/* Mirror glr_ctrl_push_color_transformers() (which only runs inside a
 * full display frame, not glr_ctrl_build_ui_snapshot) so build_doc's
 * snapshot carries a color transformer for `line`. Without this the
 * snapshot has no transformers, no inline swatch is ever produced, and
 * the swatch row-builder path is untested. */
static void seed_color_transformer(int line, float r, float g, float b) {
    UiTransformer t = {
        .line_idx   = line,
        .char_start = -1,
        .char_end   = -1,
        .kind       = TRANSFORMER_COLOR_PICKER,
        .state.color = { .r = r, .g = g, .b = b, .a = 1.0f,
                         .has_alpha = 0, .is_clear = 0 },
    };
    editor_state_transformers_clear();
    editor_state_transformers_append(&t);
}

int main(void) {
    UiRenderSnapshot snap;
    UiReplCodePanelLayout layout;
    int target = -99;
    int on_insert = -99;
    int row_offset = -99;

    reset_doc_fixture();
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("glEnd();");
    build_doc(&snap, &layout);

    ASSERT_TRUE("document counts commands",
                layout.total_lines >= layout.header_rows + layout.footer_rows + 4);
    ASSERT_TRUE("visible lines nonzero", layout.visible_lines > 0);
    ASSERT_TRUE("first command has one row", layout.cmd_main_rows[0] == 1);

    {
        int doc_line = layout.header_rows + layout.cmd_main_rows[0];
        ASSERT_TRUE("target lookup succeeds",
                    ui_repl_code_panel_target_for_doc_line(
                        &snap, doc_line, &layout, &target, &on_insert, &row_offset));
        ASSERT_TRUE("target lookup command index", target == 1);
        ASSERT_TRUE("target lookup is source row", on_insert == 0);
        ASSERT_TRUE("target lookup row offset", row_offset == 0);
    }

    editor_navigate_to_line(1);
    editor_cursor_pos_set(0);
    editor_handle_key('\r', 0, 0);
    build_doc(&snap, &layout);
    {
        int doc_line = layout.header_rows + layout.cmd_main_rows[0];
        ASSERT_TRUE("insert row lookup succeeds",
                    ui_repl_code_panel_target_for_doc_line(
                        &snap, doc_line, &layout, &target, &on_insert, &row_offset));
        ASSERT_TRUE("insert row reports virtual line", target == -1);
        ASSERT_TRUE("insert row flag", on_insert == 1);
    }

    reset_doc_fixture();
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("glEnd();");
    editor_navigate_to_line(2);
    editor_scroll_set(0);
    editor_scroll_follow_cursor_set(1);
    build_doc(&snap, &layout);
    glr_ctrl_apply_code_panel_follow_scroll(&layout);
    ASSERT_TRUE("follow line visible after apply",
                layout.follow_doc_line >= editor_scroll() &&
                layout.follow_doc_line < editor_scroll() + layout.visible_lines);

    /* "Code focus" hides the derived C boilerplate stanzas: header and
     * footer row counts collapse to 0, document rows stay, and the
     * row-count/emit gating stays consistent so doc-line mapping holds
     * (review findings P1/P2b). */
    {
        int base_header, base_footer;

        reset_doc_fixture();
        editor_feed_line("float r;");
        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glColor3f(1, 0, 0);");
        editor_feed_line("glEnd();");

        glr_state_presentation_mut()->code_focus = 0;
        build_doc(&snap, &layout);
        base_header = layout.header_rows;
        base_footer = layout.footer_rows;
        ASSERT_TRUE("baseline has header chrome", base_header > 0);
        ASSERT_TRUE("baseline has footer chrome", base_footer > 0);

        glr_state_presentation_mut()->code_focus = 1;
        build_doc(&snap, &layout);
        ASSERT_TRUE("focus hides header chrome", layout.header_rows == 0);
        ASSERT_TRUE("focus hides footer chrome", layout.footer_rows == 0);
        ASSERT_TRUE("focus keeps document rows",
                    layout.total_lines >= 4 && layout.cmd_main_rows[0] == 1);
        {
            /* Command 1 starts right after command 0, at doc line
             * header_rows(==0) + cmd_main_rows[0]. */
            int doc_line = layout.header_rows + layout.cmd_main_rows[0];
            ASSERT_TRUE("focus doc-line mapping succeeds",
                        ui_repl_code_panel_target_for_doc_line(
                            &snap, doc_line, &layout, &target, &on_insert,
                            &row_offset));
            ASSERT_TRUE("focus maps to command 1", target == 1);
            ASSERT_TRUE("focus maps to source row", on_insert == 0);
        }

        glr_state_presentation_mut()->code_focus = 0;
        build_doc(&snap, &layout);
        ASSERT_TRUE("toggling focus off restores header rows",
                    layout.header_rows == base_header);
        ASSERT_TRUE("toggling focus off restores footer rows",
                    layout.footer_rows == base_footer);
    }

    /* P1: glr_ctrl_toggle_code_focus() (shared by Ctrl+Shift+F and the
     * status-bar keycap click) flips code_focus, syncs chrome, and
     * requests follow-scroll. After the toggle the header rows collapse
     * from many to 0; assert the real toggle path keeps the active edit
     * row on screen, and that it actually flips the flag. */
    {
        reset_doc_fixture();
        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glColor3f(1, 0, 0);");
        editor_feed_line("glEnd();");
        editor_navigate_to_line(2);

        glr_state_presentation_mut()->code_focus = 0;
        editor_scroll_set(0);
        build_doc(&snap, &layout);

        glr_ctrl_toggle_code_focus();
        ASSERT_TRUE("toggle flips code_focus on",
                    glr_state_presentation().code_focus == 1);
        build_doc(&snap, &layout);
        glr_ctrl_apply_code_panel_follow_scroll(&layout);
        ASSERT_TRUE("follow keeps cursor visible after focus toggle",
                    layout.follow_doc_line >= editor_scroll() &&
                    layout.follow_doc_line < editor_scroll() + layout.visible_lines);

        glr_ctrl_toggle_code_focus();
        ASSERT_TRUE("toggle flips code_focus off",
                    glr_state_presentation().code_focus == 0);
    }

    /* Regression: at the default left layout (800x260, panel_frac
     * 0.45) the right cluster would collide with the left status text,
     * so the focus keycap must be SUPPRESSED — no hit-test pixel
     * returns UI_HIT_CODE_FOCUS_TOGGLE (no overlapping draw). The help
     * chip sits further right and still fits. */
    {
        int found_focus = 0;
        int found_help = 0;

        reset_doc_fixture();              /* 800x260, LEFT, 0.45 */
        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glEnd();");
        build_doc(&snap, &layout);

        for (int my = 259; my >= 0; my--) {
            for (int mx = 799; mx >= 0; mx--) {
                UiHit hk = ui_repl_code_panel_hit_test(&snap, mx, my);
                if (hk.kind == UI_HIT_CODE_FOCUS_TOGGLE) found_focus = 1;
                else if (hk.kind == UI_HIT_HELP_TOGGLE)  found_help = 1;
            }
        }
        ASSERT_TRUE("focus keycap suppressed at narrow default width",
                    found_focus == 0);
        ASSERT_TRUE("help keycap still fits at narrow default width",
                    found_help == 1);
    }

    /* On a wide panel both keycaps render; a click on each routes
     * THROUGH glr_ctrl_router_handle_code_panel_hit (the real dispatch
     * switch, not the toggle helper directly) and flips the matching
     * session state. */
    {
        int fx = -1, fy = -1, hx = -1, hy = -1;
        int help_was;

        reset_doc_fixture();
        ui_state_viewport_set_size(1600, 300);   /* wide -> cluster fits */
        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glEnd();");
        build_doc(&snap, &layout);

        for (int my = 299; my >= 0 && (fx < 0 || hx < 0); my--) {
            for (int mx = 1599; mx >= 0; mx--) {
                UiHit hk = ui_repl_code_panel_hit_test(&snap, mx, my);
                if (hk.kind == UI_HIT_CODE_FOCUS_TOGGLE && fx < 0) {
                    fx = mx; fy = my;
                } else if (hk.kind == UI_HIT_HELP_TOGGLE && hx < 0) {
                    hx = mx; hy = my;
                }
            }
        }
        ASSERT_TRUE("focus keycap hit-testable on wide panel", fx >= 0);
        ASSERT_TRUE("help keycap hit-testable on wide panel", hx >= 0);

        glr_state_presentation_mut()->code_focus = 0;
        glr_ctrl_sync_ui_chrome();
        {
            UiHit fh = ui_hit_none();
            fh.kind = UI_HIT_CODE_FOCUS_TOGGLE;
            glr_ctrl_router_handle_code_panel_hit(fh, fx, fy);
        }
        ASSERT_TRUE("focus click routes through dispatch -> toggles on",
                    glr_state_presentation().code_focus == 1);
        glr_ctrl_toggle_code_focus();    /* restore */

        help_was = ui_state_help().visible;
        {
            UiHit hh = ui_hit_none();
            hh.kind = UI_HIT_HELP_TOGGLE;
            glr_ctrl_router_handle_code_panel_hit(hh, hx, hy);
        }
        ASSERT_TRUE("help click routes through dispatch -> toggles overlay",
                    ui_state_help().visible == !help_was);
        glr_ctrl_toggle_help();          /* restore */
        ASSERT_TRUE("help overlay restored",
                    ui_state_help().visible == help_was);

        /* While help is open it is modal, but the "F1 help" keycap
         * stays clickable so a second click dismisses it: panel
         * hit-test passes the keycap pixel through as
         * UI_HIT_HELP_TOGGLE while every other pixel stays
         * UI_HIT_HELP_PANEL. */
        ui_state_help_mut()->visible = 1;
        build_doc(&snap, &layout);
        ASSERT_TRUE("help keycap stays clickable through modal",
                    ui_panels_hit_test(&snap, hx, hy, 0).kind
                        == UI_HIT_HELP_TOGGLE);
        ASSERT_TRUE("rest of screen stays modal under help",
                    ui_panels_hit_test(&snap, 20, hy, 0).kind
                        == UI_HIT_HELP_PANEL);
        ui_state_help_mut()->visible = 0;
    }

    /* Regression: the inline color swatch must stay visible AND
     * hittable whether or not the cursor sits on the glColor line, in
     * BOTH code-focus modes. Before the add_input_row fix the
     * edit-in-place input row never set right_action, so moving the
     * cursor onto a color line made the swatch (and its click target)
     * silently vanish; a draw-order-only fix looked plausible but did
     * nothing. code_focus is pinned explicitly so a CFG_DEFAULT_CODE_FOCUS
     * flip can't mask this. Scroll is set past the (focus-aware)
     * header so the color row is on screen in full mode too. */
    for (int cf = 0; cf <= 1; cf++) {
        const char *mode = cf ? " [focus]" : " [full]";

        for (int cursor_on = 0; cursor_on <= 1; cursor_on++) {
            const char *cur = cursor_on ? "/cursor-on" : "/cursor-off";
            char lbl[96];
            int cp_x, cp_w, win_h, found = 0, line = -1;

            reset_doc_fixture();
            glr_state_presentation_mut()->code_focus = cf;
            glr_ctrl_sync_ui_chrome();
            editor_feed_line("glBegin(GL_POINTS);");   /* cmd 0 */
            editor_feed_line("glColor3f(1, 0, 0);");   /* cmd 1 (color) */
            editor_feed_line("glEnd();");              /* cmd 2 */
            seed_color_transformer(1, 1.0f, 0.0f, 0.0f);

            /* cursor_on: edit-in-place input row vs plain command row. */
            editor_navigate_to_line(cursor_on ? 1 : 0);

            /* Build once for the focus-aware header_rows, scroll past
             * the chrome (0 in focus), rebuild so the color row is at
             * the top of the visible area in both modes. */
            build_doc(&snap, &layout);
            editor_scroll_set(layout.header_rows);
            build_doc(&snap, &layout);

            ui_layout_code_panel_rect(&cp_x, NULL, &cp_w, NULL);
            win_h = ui_state_viewport().window_h;
            for (int my = 0; my < win_h && !found; my++)
                for (int mx = cp_x + cp_w - 1;
                     mx >= cp_x + cp_w - CODE_MARGIN_X - 2 * FONT_W && !found;
                     mx--) {
                    UiHit hk = ui_repl_code_panel_hit_test(&snap, mx, my);
                    if (hk.kind == UI_HIT_INLINE_COLOR_SWATCH) {
                        found = 1;
                        line  = hk.line_idx;
                    }
                }
            snprintf(lbl, sizeof lbl, "swatch hittable%s%s", cur, mode);
            ASSERT_TRUE(lbl, found);
            snprintf(lbl, sizeof lbl,
                     "swatch targets the glColor line%s%s", cur, mode);
            ASSERT_TRUE(lbl, line == 1);
        }
    }

    return test_harness_report(&g_harness, "repl_code_panel_document");
}
