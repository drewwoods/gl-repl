#include "editor/state.h"
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "ui/metrics.h"
#include "ui/state.h"
#include "repl/state.h"
#include "widgets/replay_state.h"
#include "repl/core.h"
#include "editor/input.h"
#include "editor/help_session.h"
#include "ui/tabbed_overlay.h"
#include "prof.h"
#include "ui/profile_panel.h"
#include "widgets/color_picker_state.h"
#include "ui/color_picker.h"
#include "ui/autocomplete_panel.h"
#include "ui/variable_panel.h"
#include "app/glr_actions.h"
#include "ui/menu_bar.h"
#include "ui/panels.h"
#include "ui/snapshot.h"
#include "ui/layout.h"
#include "ui/repl_code_panel.h"
#include "widgets/variable_panel_state.h"
#include "widgets/variable_panel_drag.h"
#include "widgets/tutorial.h"
#include "widgets/tutorial_state.h"
#include "support/test_harness.h"
#include <GL/gl_stub_counts.h>
#include "ui/gl_2d.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define ASSERT_INT(label, got, expected) do { \
    TEST_ASSERT_INT(&g_harness, label, got, expected); \
} while (0)

#define ASSERT_GL_CALLS(label, counter, min_calls) do { \
    TEST_ASSERT_TRUE(&g_harness, label, gl_stub_counts[counter] >= (min_calls)); \
} while (0)

/* Build a UiRenderSnapshot from the current REPL state for renderer tests. */
static void make_test_ui_snapshot(UiRenderSnapshot *snap) {
    glr_ctrl_build_ui_snapshot(snap);
    glr_ctrl_code_panel_apply_scroll_follow_for_test(snap, NULL, NULL);
    glr_ctrl_build_ui_snapshot(snap);
}

static UiHit ui_panels_hit_test_current_snapshot(int mx, int my,
                                                 int variable_count) {
    UiRenderSnapshot snap;

    make_test_ui_snapshot(&snap);
    return ui_panels_hit_test(&snap, mx, my, variable_count);
}

static void build_test_code_panel_layout(UiReplCodePanelLayout *layout,
                                         int cp_w, int text_x, int cp_h) {
    UiRenderSnapshot snap;

    make_test_ui_snapshot(&snap);
    ui_repl_code_panel_build_layout(&snap, layout, cp_w, text_x, cp_h);
}

/* Test-side helper: build a fresh ColorPickerView and hit-test against
 * the live viewport height. Mirrors ui_panels_hit_test's inline pattern
 * so legacy tests keep their `(mx, my)` shape. */
static UiHit ui_color_picker_hit_test_helper(int mx, int my) {
    ColorPickerView pv = color_picker_view();
    return ui_color_picker_hit_test(&pv, mx, my, ui_state_viewport().window_h);
}

static UiOverlayState build_help_overlay_input(void) {
    UiOverlayState in = {
        .visible    = ui_state_help().visible,
        .tab_idx    = editor_help_session_view().tab_idx,
        .scroll     = editor_help_session_view().scroll,
        .viewport_w = ui_state_viewport().window_w,
        .viewport_h = ui_state_viewport().window_h,
        .content    = glr_ctrl_help_overlay_content(),
    };
    return in;
}

static void test_help_overlay(void) {
    printf("Testing Help Overlay...\n");
    gl_stub_counts_reset();

    ui_state_help_mut()->visible = 0;
    { UiOverlayState in = build_help_overlay_input(); ui_tabbed_overlay_render(&in); }
    ASSERT_TRUE("help hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    ui_state_help_mut()->visible = 1;
    ui_state_viewport_set_size(800, 600);
    editor_help_session_set_tab(0);
    editor_help_session_set_scroll(0);

    gl_stub_counts_reset();
    { UiOverlayState in = build_help_overlay_input(); ui_tabbed_overlay_render(&in); }
    ASSERT_GL_CALLS("help visible -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("help visible -> calls glVertex2f", GL_STUB_glVertex2f, 4);
    ASSERT_GL_CALLS("help visible -> draws text", GL_STUB_glRasterPos2f, 10);
    ASSERT_GL_CALLS("help visible -> enables blending", GL_STUB_glEnable, 1);

    /* Switch tabs */
    editor_help_session_set_tab(1);
    gl_stub_counts_reset();
    { UiOverlayState in = build_help_overlay_input(); ui_tabbed_overlay_render(&in); }
    ASSERT_GL_CALLS("help tab 1 -> draws text", GL_STUB_glRasterPos2f, 10);
}

static void test_profile_panel(void) {
    printf("Testing Profile Panel...\n");
    gl_stub_counts_reset();

    ui_state_profile_panel_mut()->mode = PROFILE_PANEL_OFF;
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_profile_panel_render(&s); }
    ASSERT_TRUE("profile hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    ui_state_profile_panel_mut()->mode = PROFILE_PANEL_ON;
    prof_frame_tick();
    prof_begin(PROF_FRAME_TOTAL);
    prof_end(PROF_FRAME_TOTAL);

    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_profile_panel_render(&s); }
    ASSERT_GL_CALLS("profile visible -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("profile visible -> draws text", GL_STUB_glRasterPos2f, 5);
    ASSERT_GL_CALLS("profile visible -> calls glColor4f", GL_STUB_glColor4f, 1);

    /* Test details mode */
    ui_state_profile_panel_mut()->mode = PROFILE_PANEL_DETAILS;
    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_profile_panel_render(&s); }
    ASSERT_GL_CALLS("profile details -> draws more text", GL_STUB_glRasterPos2f, 10);
}

static void test_color_picker(void) {
    printf("Testing Color Picker...\n");

    /* Setup a command to edit */
    repl_state_document_count_set(1);
    repl_state_document_cmds_mut()[0].type = CMD_COLOR3F;
    repl_state_document_cmds_mut()[0].args[0] = 1.0f;
    repl_state_document_cmds_mut()[0].args[1] = 0.0f;
    repl_state_document_cmds_mut()[0].args[2] = 0.0f;
    repl_state_document_cmds_mut()[0].valid = 1;
    repl_state_document_cmds_mut()[0].has_vars = 0;

    ASSERT_TRUE("can edit color cmd", color_picker_can_edit_cmd(0));

    ui_state_viewport_set_size(800, 600);
    color_picker_open(0, 300);
    ASSERT_TRUE("picker is active", color_picker_active_line() == 0);

    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_color_picker_render(&s.color_picker, s.viewport.window_w, s.viewport.window_h); }
    ASSERT_GL_CALLS("picker render -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("picker render -> calls glVertex2f", GL_STUB_glVertex2f, 4);

    ColorPickerView view = color_picker_view();
    int sv_mx = view.rects.sv_x + 10;
    int sv_my = ui_state_viewport().window_h - (view.rects.sv_y + 10);

    /* Press SV square (CP_SV_SZ = 150) */
    color_picker_handle_press(sv_mx, sv_my);
    color_picker_handle_motion(sv_mx + 10, sv_my - 10);
    color_picker_handle_release();

    /* Press Hue bar */
    int hue_mx = view.rects.hue_x + 5;
    int hue_my = ui_state_viewport().window_h - (view.rects.hue_y + 10);
    color_picker_handle_press(hue_mx, hue_my);
    color_picker_handle_motion(hue_mx, hue_my - 10);
    color_picker_handle_release();

    /* Test Alpha support */
    repl_state_document_cmds_mut()[0].type = CMD_COLOR4F;
    repl_state_document_cmds_mut()[0].args[3] = 0.5f;
    color_picker_open(0, 300);

    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_color_picker_render(&s.color_picker, s.viewport.window_w, s.viewport.window_h); }
    ASSERT_GL_CALLS("picker render with alpha -> draws quads", GL_STUB_glBegin, 1);

    view = color_picker_view();
    /* Press Alpha bar */
    int alp_mx = view.rects.alp_x + 5;
    int alp_my = ui_state_viewport().window_h - (view.rects.alp_y + 10);
    color_picker_handle_press(alp_mx, alp_my);
    color_picker_handle_motion(alp_mx, alp_my - 10);
    color_picker_handle_release();

    /* Test glClearColor limits */
    repl_state_document_cmds_mut()[0].type = CMD_CLEAR_COLOR;
    color_picker_open(0, 300);
    view = color_picker_view();
    int sv_mx2 = view.rects.sv_x + 10;
    int sv_my2 = ui_state_viewport().window_h - (view.rects.sv_y + 10); // High V
    color_picker_handle_press(sv_mx2, sv_my2);
    color_picker_handle_release();

    /* Test CMD_TESS_COLOR writeback */
    repl_state_document_cmds_mut()[0].type = CMD_TESS_COLOR;
    color_picker_open(0, 300);
    view = color_picker_view();
    color_picker_handle_press(view.rects.hue_x + 5, ui_state_viewport().window_h - (view.rects.hue_y + 10));
    color_picker_handle_release();

    /* Test RGB->HSV branches */
    color_picker_close();
    repl_state_document_cmds_mut()[0].type = CMD_COLOR3F;
    repl_state_document_cmds_mut()[0].args[0] = 0.0f;
    repl_state_document_cmds_mut()[0].args[1] = 1.0f;
    repl_state_document_cmds_mut()[0].args[2] = 0.0f;
    color_picker_open(0, 300);

    repl_state_document_cmds_mut()[0].args[0] = 0.0f;
    repl_state_document_cmds_mut()[0].args[1] = 0.0f;
    repl_state_document_cmds_mut()[0].args[2] = 1.0f;
    color_picker_open(0, 300);

    /* Open an uneditable command to hit coverage */
    repl_state_document_cmds_mut()[0].type = CMD_BEGIN;
    color_picker_open(0, 300);

    /* Test invalid/vars constraints */
    repl_state_document_cmds_mut()[0].type = CMD_COLOR3F;
    repl_state_document_cmds_mut()[0].valid = 0;
    color_picker_can_edit_cmd(0);
    repl_state_document_cmds_mut()[0].valid = 1;
    repl_state_document_cmds_mut()[0].has_vars = 1;
    color_picker_can_edit_cmd(0);
    repl_state_document_cmds_mut()[0].has_vars = 0;

    /* Out of bounds cmd_idx */
    color_picker_can_edit_cmd(-1);

    /* Test clicking outside picker */
    color_picker_open(0, 300);
    color_picker_handle_press(0, 0);

    /* Test writeback short-circuit (mutated command type underneath) */
    color_picker_open(0, 300);
    view = color_picker_view();
    repl_state_document_cmds_mut()[0].type = CMD_BEGIN;
    color_picker_handle_press(view.rects.sv_x + 10, ui_state_viewport().window_h - (view.rects.sv_y + 10));
    color_picker_handle_release();

    /* Test writeback short-circuit (command deleted underneath) */
    color_picker_open(0, 300);
    view = color_picker_view();
    repl_state_document_count_set(0);
    color_picker_handle_press(view.rects.sv_x + 10, ui_state_viewport().window_h - (view.rects.sv_y + 10));
    color_picker_handle_release();

    color_picker_close();
    ASSERT_TRUE("picker closed", color_picker_active_line() == -1);

    /* Test swatch rendering — render reads from a transformer entry, not
     * from the document, so build a minimal one for the test. */
    gl_stub_counts_reset();
    {
        UiTransformer t = {
            .line_idx = 0,
            .char_start = -1,
            .char_end = -1,
            .kind = TRANSFORMER_COLOR_PICKER,
            .state.color = {
                .r = 0.5f, .g = 0.5f, .b = 0.5f, .a = 1.0f,
                .has_alpha = 0, .is_clear = 0,
            },
        };
        ui_color_picker_render_swatch(&t, 100, 100, color_picker_active_line());
    }
    ASSERT_GL_CALLS("swatch render -> draws quads", GL_STUB_glBegin, 1);
}

static void test_autocomplete_panel(void) {
    printf("Testing Autocomplete Panel...\n");
    gl_stub_counts_reset();

    editor_state_autocomplete_mut()->match_count = 0;
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_autocomplete_panel_render(&s, 100, 100); }
    ASSERT_TRUE("ac hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    editor_state_autocomplete_mut()->match_count = 2;
    editor_state_autocomplete_mut()->matches[0] = "glVertex3f";
    editor_state_autocomplete_mut()->matches[1] = "glVertex2f";
    editor_state_autocomplete_mut()->selected_idx = 0;

    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_autocomplete_panel_render(&s, 100, 100); }
    ASSERT_GL_CALLS("ac visible -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("ac visible -> draws text", GL_STUB_glRasterPos2f, 2);
    ASSERT_GL_CALLS("ac visible -> calls glVertex2f", GL_STUB_glVertex2f, 4);
}

static void test_variable_panel(void) {
    printf("Testing Variable Panel...\n");
    gl_stub_counts_reset();

    variable_panel_view_mut()->visible = 0;
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_variable_panel_render(&s); }
    ASSERT_TRUE("var panel hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    variable_panel_view_mut()->visible = 1;
    g_num_predef_vars = 1;
    strcpy(g_predef_vars[0].name, "x");
    g_predef_vars[0].value = 1.0f;

    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_variable_panel_render(&s); }
    ASSERT_GL_CALLS("var panel visible -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("var panel visible -> draws text", GL_STUB_glRasterPos2f, 2);
    ASSERT_GL_CALLS("var panel visible -> calls glColor4f", GL_STUB_glColor4f, 1);

    /* Test hit testing */
    int row = -1;
    ui_state_viewport_set_size(800, 600);
    int px, py, pw, ph;
    ui_variable_panel_rect_for_count(1, &px, &py, &pw, &ph);
    ASSERT_TRUE("hit test in panel",
                ui_variable_panel_hit_for_count(
                    px + 10, ui_state_viewport().window_h - (py + 10),
                    1, &row));
    ASSERT_TRUE("hit test outside panel",
                !ui_variable_panel_hit_for_count(0, 0, 1, &row));
}

static void test_menu_bar(void) {
    printf("Testing Menu Bar...\n");
    gl_stub_counts_reset();

    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.5f;

    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_menu_bar_render(&s); }
    ASSERT_GL_CALLS("menu bar render -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("menu bar render -> draws text", GL_STUB_glRasterPos2f, 2);

    /* Test hits */
    ASSERT_TRUE("menu hit", ui_menu_bar_menu_hit(20, 10) >= 0);
    /* Pins are on the right side of the code panel.
     * With CP LEFT and frac 0.5, cp_w = 400.
     */
    ASSERT_TRUE("pin hit", ui_menu_bar_pin_hit(380, 10) >= 0);

    /* Test dropdown */
    ui_menu_bar_set_open_menu(0, 0.0f); // File menu
    ASSERT_TRUE("dropdown open", ui_menu_bar_menu_dropdown_is_open());

    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_menu_bar_render_example_dropdown(&s); }
    ASSERT_GL_CALLS("dropdown render -> draws quads", GL_STUB_glBegin, 1);

    /* Dropdown item hit */
    ASSERT_TRUE("dropdown item hit", ui_menu_bar_dropdown_item_hit(20, 100) >= 0);

    /* Test config menu */
    ui_menu_bar_open_config(0.0f);
    ASSERT_TRUE("config menu open", ui_menu_bar_open_menu_id() == GLR_MENU_CONFIG);

    /* Test search overlay */
    editor_state_search_mut()->active = 1;
    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_menu_bar_render_search_overlay(&s, 0, 400, 600); }
    ASSERT_GL_CALLS("search overlay -> draws quads", GL_STUB_glBegin, 1);

    ui_menu_bar_close();
    ASSERT_TRUE("menu closed", !ui_menu_bar_menu_dropdown_is_open());
}

/* Phase E commit 28: ui_panels_hit_test classifies pointer location
 * as a UiHit without mutating state.
 *
 * Default code-panel layout is TOP at fraction 0.45 — at 800x600
 * that puts the panel at OpenGL rect (0, 330, 800, 270). In GLUT
 * (top-down) coords the panel occupies my in [0, 270]. */
static void test_ui_panels_hit_test(void) {
    glr_app_reset_all();
    ui_state_viewport_set_size(800, 600);

    /* Click in the scene region (below the panel, my > 270). */
    UiHit h_scene = ui_panels_hit_test_current_snapshot(400, 400, 0);
    ASSERT_TRUE("scene hit kind", h_scene.kind == UI_HIT_SCENE);

    /* Click inside the code panel's text area. mx > gutter end
     * (CODE_MARGIN_X + 4*FONT_W = ~40) and my within the panel. */
    UiHit h_text = ui_panels_hit_test_current_snapshot(150, 100, 0);
    ASSERT_TRUE("code-panel text hit kind",
                h_text.kind == UI_HIT_CODE_TEXT);

    /* Click in the gutter (line numbers, mx < ~40). */
    UiHit h_gutter = ui_panels_hit_test_current_snapshot(20, 100, 0);
    ASSERT_TRUE("code-panel gutter hit kind",
                h_gutter.kind == UI_HIT_CODE_GUTTER);

    {
        int cp_x, cp_y, cp_w, cp_h;
        int my_status;

        ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
        my_status = ui_state_viewport().window_h - (cp_y + STATUSBAR_H / 2);
        UiHit h_status = ui_panels_hit_test_current_snapshot(cp_x + cp_w / 2,
                                                             my_status, 0);
        ASSERT_TRUE("code-panel statusbar consumes clicks",
                    h_status.kind == UI_HIT_CODE_PANEL_CHROME);
    }

    /* Help overlay takes priority over everything. */
    ui_state_help_mut()->visible = 1;
    UiHit h_help = ui_panels_hit_test_current_snapshot(150, 100, 0);
    ASSERT_TRUE("help overlay takes priority",
                h_help.kind == UI_HIT_HELP_PANEL);
    ui_state_help_mut()->visible = 0;
}

/* Phase E commit 29: hit-test entry points for ui_menu_bar,
 * ui_color_picker, ui_variable_panel return passive UiHit
 * classifications without mutating state. ui_panels_hit_test
 * dispatches to them in priority order. */
static void test_ui_menu_bar_hit_test(void) {
    glr_app_reset_all();
    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.5f;

    /* Outside menu bar entirely (well below it). */
    UiHit h_none = ui_menu_bar_hit_test(20, 400);
    ASSERT_TRUE("menu bar miss -> NONE", h_none.kind == UI_HIT_NONE);

    /* Top-level menu button (File at left edge, my within bar). The
     * J2.1 split emits UI_HIT_MENU_BUTTON for top-level labels with
     * cmd_idx = menu_id; the controller no longer reads ui_menu_bar
     * state to distinguish a top-level click from a dropdown row. */
    ui_menu_bar_close();
    UiHit h_menu = ui_menu_bar_hit_test(20, 10);
    ASSERT_TRUE("menu top-level hit kind",
                h_menu.kind == UI_HIT_MENU_BUTTON);
    ASSERT_TRUE("menu top-level cmd_idx is menu_id",
                h_menu.cmd_idx >= 0 && h_menu.cmd_idx < 3);

    /* Pin button (right side, mx near right edge). */
    UiHit h_pin = ui_menu_bar_hit_test(380, 10);
    ASSERT_TRUE("pin hit kind", h_pin.kind == UI_HIT_PIN_BUTTON);
    ASSERT_TRUE("pin item_idx populated", h_pin.item_idx >= 0);

    /* Open a menu, then a click on a dropdown row reports
     * UI_HIT_MENU_ITEM with cmd_idx = open_menu_id and item_idx =
     * row. */
    ui_menu_bar_set_open_menu(0, 0.0f); /* File menu */
    UiHit h_row = ui_menu_bar_hit_test(20, 100);
    ASSERT_TRUE("dropdown row hit kind",
                h_row.kind == UI_HIT_MENU_ITEM);
    ASSERT_TRUE("dropdown row cmd_idx is menu_id",
                h_row.cmd_idx == 0);
    ASSERT_TRUE("dropdown row item_idx populated", h_row.item_idx >= 0);
    ui_menu_bar_close();
}

static void test_ui_color_picker_hit_test(void) {
    glr_app_reset_all();
    ui_state_viewport_set_size(800, 600);

    /* No picker active -> miss. */
    UiHit h_closed = ui_color_picker_hit_test_helper(400, 300);
    ASSERT_TRUE("closed picker -> NONE", h_closed.kind == UI_HIT_NONE);

    /* Open a picker on a color command. The picker positions itself
     * relative to the code-panel rect, so probe the actual rect by
     * sweeping mx / my for the click coordinate that hits SV (region 1). */
    repl_state_document_count_set(1);
    repl_state_document_cmds_mut()[0].type = CMD_COLOR3F;
    repl_state_document_cmds_mut()[0].args[0] = 1.0f;
    repl_state_document_cmds_mut()[0].args[1] = 0.0f;
    repl_state_document_cmds_mut()[0].args[2] = 0.0f;
    repl_state_document_cmds_mut()[0].valid = 1;
    repl_state_document_cmds_mut()[0].has_vars = 0;
    color_picker_open(0, 300);

    int win_h = ui_state_viewport().window_h;
    int sv_mx = -1, sv_my = -1;
    int hue_mx = -1, hue_my = -1;
    for (int my = 0; my < win_h; my += 4) {
        for (int mx = 0; mx < ui_state_viewport().window_w; mx += 4) {
            UiHit h = ui_color_picker_hit_test_helper(mx, my);
            if (h.kind == UI_HIT_COLOR_SWATCH && h.item_idx == 1 && sv_mx < 0) {
                sv_mx = mx; sv_my = my;
            }
            if (h.kind == UI_HIT_COLOR_SWATCH && h.item_idx == 2 && hue_mx < 0) {
                hue_mx = mx; hue_my = my;
            }
        }
    }
    ASSERT_TRUE("SV rect probed", sv_mx >= 0);
    ASSERT_TRUE("hue rect probed", hue_mx >= 0);

    UiHit h_sv = ui_color_picker_hit_test_helper(sv_mx, sv_my);
    ASSERT_TRUE("SV hit kind", h_sv.kind == UI_HIT_COLOR_SWATCH);
    ASSERT_TRUE("SV cmd_idx is 0", h_sv.cmd_idx == 0);
    ASSERT_TRUE("SV item_idx is 1", h_sv.item_idx == 1);

    UiHit h_hue = ui_color_picker_hit_test_helper(hue_mx, hue_my);
    ASSERT_TRUE("hue hit kind", h_hue.kind == UI_HIT_COLOR_SWATCH);
    ASSERT_TRUE("hue item_idx is 2", h_hue.item_idx == 2);

    /* Click far outside picker -> NONE. */
    UiHit h_miss = ui_color_picker_hit_test_helper(0, 0);
    ASSERT_TRUE("picker miss -> NONE", h_miss.kind == UI_HIT_NONE);
    color_picker_close();
}

static void test_ui_variable_panel_hit_test(void) {
    glr_app_reset_all();
    ui_state_viewport_set_size(800, 600);

    /* Panel hidden -> always miss. */
    variable_panel_view_mut()->visible = 0;
    UiHit h_off = ui_variable_panel_hit_test(700, 100, 0);
    ASSERT_TRUE("hidden panel -> NONE", h_off.kind == UI_HIT_NONE);

    /* Visible panel with one declared variable. */
    variable_panel_view_mut()->visible = 1;
    g_num_predef_vars = 1;
    strcpy(g_predef_vars[0].name, "x");
    g_predef_vars[0].value = 1.0f;

    int px, py, pw, ph;
    ui_variable_panel_rect_for_count(1, &px, &py, &pw, &ph);
    int my = ui_state_viewport().window_h - (py + 10);

    UiHit h_row = ui_variable_panel_hit_test(px + 10, my, 1);
    ASSERT_TRUE("row hit kind", h_row.kind == UI_HIT_VARIABLE_SLIDER);
    ASSERT_TRUE("row item_idx populated",
                h_row.item_idx >= 0 && h_row.item_idx < g_num_predef_vars);

    /* Click outside the panel rect -> NONE. */
    UiHit h_out = ui_variable_panel_hit_test(0, 0, 1);
    ASSERT_TRUE("outside panel -> NONE", h_out.kind == UI_HIT_NONE);
}

/* Verify ui_panels_hit_test routes to the floating-overlay
 * hit-testers in priority order before falling through to the
 * code panel and scene. */
static void test_ui_panels_hit_test_dispatch(void) {
    glr_app_reset_all();
    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.5f;

    /* Variable panel should win over the scene-region fallback when
     * a click lands on its rect. */
    variable_panel_view_mut()->visible = 1;
    g_num_predef_vars = 1;
    strcpy(g_predef_vars[0].name, "x");
    g_predef_vars[0].value = 1.0f;
    int px, py, pw, ph;
    ui_variable_panel_rect_for_count(1, &px, &py, &pw, &ph);
    int my_var = ui_state_viewport().window_h - (py + 10);
    UiHit h_var = ui_panels_hit_test_current_snapshot(px + 10, my_var, 1);
    ASSERT_TRUE("var panel routed via panels_hit_test",
                h_var.kind == UI_HIT_VARIABLE_SLIDER);
    variable_panel_view_mut()->visible = 0;

    /* Menu pin button click should resolve as UI_HIT_PIN_BUTTON. */
    ui_menu_bar_close();
    UiHit h_pin = ui_panels_hit_test_current_snapshot(380, 10, 0);
    ASSERT_TRUE("pin routed via panels_hit_test",
                h_pin.kind == UI_HIT_PIN_BUTTON);

    /* Color picker, when open, beats menu / variable / code panel.
     * Probe for an SV-rect coordinate so we don't depend on the
     * picker's internal placement math. */
    repl_state_document_count_set(1);
    repl_state_document_cmds_mut()[0].type = CMD_COLOR3F;
    repl_state_document_cmds_mut()[0].args[0] = 1.0f;
    repl_state_document_cmds_mut()[0].args[1] = 0.0f;
    repl_state_document_cmds_mut()[0].args[2] = 0.0f;
    repl_state_document_cmds_mut()[0].valid = 1;
    repl_state_document_cmds_mut()[0].has_vars = 0;
    color_picker_open(0, 300);

    int sv_mx = -1, sv_my = -1;
    for (int my = 0; my < ui_state_viewport().window_h && sv_mx < 0; my += 4) {
        for (int mx = 0; mx < ui_state_viewport().window_w; mx += 4) {
            UiHit h = ui_color_picker_hit_test_helper(mx, my);
            if (h.kind == UI_HIT_COLOR_SWATCH && h.item_idx == 1) {
                sv_mx = mx; sv_my = my;
                break;
            }
        }
    }
    ASSERT_TRUE("dispatch SV rect probed", sv_mx >= 0);
    UiHit h_pick = ui_panels_hit_test_current_snapshot(sv_mx, sv_my, 0);
    ASSERT_TRUE("picker routed via panels_hit_test",
                h_pick.kind == UI_HIT_COLOR_SWATCH);
    color_picker_close();
}

/* J2.1: ui_panels_hit_test emits UI_HIT_PANEL_DIVIDER for the
 * code-panel splitter strip. */
static void test_ui_panels_hit_test_panel_divider(void) {
    glr_app_reset_all();
    ui_state_viewport_set_size(800, 600);
    ui_state_code_panel_mut()->panel_frac = 0.5f;

    /* LEFT layout: divider sits at x = panel_w. */
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
    int cp_x, cp_y, cp_w, cp_h;
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    UiHit h_left = ui_panels_hit_test_current_snapshot(cp_x + cp_w, cp_y + 20, 0);
    ASSERT_TRUE("LEFT divider hit kind",
                h_left.kind == UI_HIT_PANEL_DIVIDER);

    /* TOP layout: divider sits at gl_y = cp_y. The hit is at
     * my == window_h - cp_y. */
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_TOP; glr_ctrl_sync_ui_chrome();
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    int my_top = ui_state_viewport().window_h - cp_y;
    UiHit h_top = ui_panels_hit_test_current_snapshot(cp_x + 100, my_top, 0);
    ASSERT_TRUE("TOP divider hit kind",
                h_top.kind == UI_HIT_PANEL_DIVIDER);

    /* BOTTOM layout: divider sits at gl_y = cp_y + cp_h. */
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_BOTTOM; glr_ctrl_sync_ui_chrome();
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    int my_bot = ui_state_viewport().window_h - (cp_y + cp_h);
    UiHit h_bot = ui_panels_hit_test_current_snapshot(cp_x + 100, my_bot, 0);
    ASSERT_TRUE("BOTTOM divider hit kind",
                h_bot.kind == UI_HIT_PANEL_DIVIDER);

    /* HIDDEN layout: no divider. */
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_HIDDEN; glr_ctrl_sync_ui_chrome();
    UiHit h_hidden = ui_panels_hit_test_current_snapshot(400, 300, 0);
    ASSERT_TRUE("HIDDEN layout has no divider hit",
                h_hidden.kind != UI_HIT_PANEL_DIVIDER);

    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
}

/* Compute the GLUT-space (mx, my) for the first code-panel text row
 * that resolves to a real document line, derived from the same layout
 * helpers production uses (no baked-in magic coordinates).
 *
 * Production math:
 *   vis = (line_y_start + LINE_H - 3 - gl_y) / LINE_H, where
 *   line_y_start = panel_top - CODE_MARGIN_Y - 2*LINE_H.
 *   doc_line = editor_scroll() + vis.
 *   target_for_doc_line(doc_line) requires doc_line >= header_rows
 *   to resolve a row.
 *
 * The header section (workspace metadata, render-state setup, camera
 * setup) typically takes more rows than fit in the panel at the
 * default panel_frac. Setting editor_scroll() = layout.header_rows
 * makes the first user / insert-line row land at vis=0, regardless
 * of how the header section grows.
 *
 * Caller is responsible for restoring editor_scroll() after the test
 * if it cares (the per-test glr_app_reset_all() calls do this). */
static void code_panel_first_row_text_click(int *out_mx, int *out_my) {
    int cp_x, cp_y, cp_w, cp_h;
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);

    int linenum_w = 4 * FONT_W;
    int idx_col_w = glr_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    UiReplCodePanelLayout layout;
    build_test_code_panel_layout(&layout, cp_w, text_x, cp_h);

    /* Scroll past the header so vis=0 maps to a resolvable doc row. */
    editor_scroll_set(layout.header_rows);

    int line_y_start = (cp_y + cp_h) - CODE_MARGIN_Y - 2 * LINE_H;
    /* gl_y for vis=0 lives in (line_y_start - 3, line_y_start - 3 + LINE_H);
     * pick the midpoint so off-by-one shifts in CODE_MARGIN_Y don't
     * leak into the test. */
    int gl_y_mid = line_y_start - 3 + LINE_H / 2;
    int my = ui_state_viewport().window_h - gl_y_mid;

    /* mx in the text-area column (past the gutter), past the indent
     * so the segment math has a real seg_x to clamp against. */
    int mx = cp_x + CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w + 2 * FONT_W;
    if (out_mx) *out_mx = mx;
    if (out_my) *out_my = my;
}

/* J2.1: ui_panels_hit_test populates char_idx for code-text clicks
 * with the input-cursor target derived from the wrap / indent /
 * segment math the legacy press helper used. */
static void test_ui_panels_hit_test_code_text_cursor(void) {
    glr_app_reset_all();
    ui_state_viewport_set_size(800, 600);

    int mx, my;
    code_panel_first_row_text_click(&mx, &my);

    /* Empty input → click resolves char_idx == 0 regardless of mx. */
    {
        EditorInputState *inp = editor_state_input_mut();
        inp->input[0] = '\0';
        inp->input_len = 0;
    }
    UiHit h_empty = ui_panels_hit_test_current_snapshot(mx, my, 0);
    ASSERT_TRUE("empty input code-text kind",
                h_empty.kind == UI_HIT_CODE_TEXT ||
                h_empty.kind == UI_HIT_CODE_INSERT_LINE);
    ASSERT_INT("empty input char_idx == 0", h_empty.char_idx, 0);

    /* Populate input "abcdef"; click 4 chars right of mx → char_idx
     * is non-zero and within the input length. Exact column depends
     * on segment / indent math but the property holds: clicking
     * further right yields a higher char_idx (capped at input_len). */
    {
        EditorInputState *inp = editor_state_input_mut();
        strcpy(inp->input, "abcdef");
        inp->input_len = 6;
    }
    UiHit h_mid = ui_panels_hit_test_current_snapshot(mx + 4 * FONT_W, my, 0);
    ASSERT_TRUE("mid-input code-text kind",
                h_mid.kind == UI_HIT_CODE_TEXT ||
                h_mid.kind == UI_HIT_CODE_INSERT_LINE);
    ASSERT_TRUE("mid-input char_idx in range",
                h_mid.char_idx >= 0 && h_mid.char_idx <= 6);
    ASSERT_TRUE("mid-input char_idx past start", h_mid.char_idx > 0);
}

/* J2.1: in insert mode, clicking on the insertion virtual rows
 * (the extra "ghost" row above the edit-line where the user is
 * typing the next command) surfaces UI_HIT_CODE_INSERT_LINE — the
 * controller's route_code_insert_line_hit skips editor_navigate_to_line
 * for these clicks, matching the legacy on_insert_line=1 path. */
static void test_ui_panels_hit_test_insert_line(void) {
    glr_app_reset_all();
    ui_state_viewport_set_size(800, 600);

    /* Commit one line so insert mode at edit_line=0 produces an
     * insertion ghost row above it. */
    editor_feed_line("glBegin(GL_POINTS);");
    repl_state_edit_line_set(0);
    editor_insert_mode_set(1);

    /* The insert ghost row sits at doc_line == header_rows (the
     * first row inside the document section). Use the same helper
     * that scrolls past the header so vis=0 maps there. */
    int mx, my;
    code_panel_first_row_text_click(&mx, &my);

    UiHit h = ui_panels_hit_test_current_snapshot(mx, my, 0);
    ASSERT_TRUE("insert-mode ghost row hit kind",
                h.kind == UI_HIT_CODE_INSERT_LINE);
    ASSERT_INT("insert-line line_idx == edit_line",
               h.line_idx, repl_state_edit_line());

    editor_insert_mode_set(0);
}

static void test_ui_panels_hit_test_overwrite_row_kind(void) {
    glr_app_reset_all();
    ui_state_viewport_set_size(800, 600);

    editor_feed_line("glBegin(GL_POINTS);");
    repl_state_edit_line_set(0);
    editor_insert_mode_set(0);

    int mx, my;
    code_panel_first_row_text_click(&mx, &my);

    UiHit h = ui_panels_hit_test_current_snapshot(mx, my, 0);
    ASSERT_TRUE("overwrite active row hit kind",
                h.kind == UI_HIT_CODE_TEXT);
}

static void test_ui_panels_hit_test_trailing_blank_row_kind(void) {
    int cp_x, cp_y, cp_w, cp_h;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = glr_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int trailing_doc_row;
    int line_y_start;
    int gl_y_mid;
    int mx;
    int my;
    UiReplCodePanelLayout layout;

    glr_app_reset_all();
    ui_state_viewport_set_size(800, 600);

    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glEnd();");
    repl_state_edit_line_set(0);
    editor_insert_mode_set(0);

    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    build_test_code_panel_layout(&layout, cp_w, text_x, cp_h);

    trailing_doc_row = layout.header_rows;
    for (int i = 0; i < repl_state_document_count(); i++)
        trailing_doc_row += layout.cmd_main_rows[i] + layout.replay_extra_rows[i];

    editor_scroll_set(trailing_doc_row);

    line_y_start = (cp_y + cp_h) - CODE_MARGIN_Y - 2 * LINE_H;
    gl_y_mid = line_y_start - 3 + LINE_H / 2;
    my = ui_state_viewport().window_h - gl_y_mid;
    mx = cp_x + text_x + 2 * FONT_W;

    UiHit h = ui_panels_hit_test_current_snapshot(mx, my, 0);
    ASSERT_TRUE("trailing blank row hit kind",
                h.kind == UI_HIT_CODE_TEXT);
    ASSERT_INT("trailing blank row line_idx == document_count",
               h.line_idx, repl_state_document_count());
}

static void test_ui_panels_hit_test_virtual_row_routes_to_source(void) {
    int cp_x, cp_y, cp_w, cp_h;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = glr_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int virtual_row;
    int line_y_start;
    int gl_y_mid;
    int mx;
    int my;
    UiReplCodePanelLayout layout;

    glr_app_reset_all();
    ui_state_viewport_set_size(800, 600);

    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glEnd();");
    editor_state_virtual_lines_append(0, VIRTUAL_STYLE_REPLAY_EVAL,
                                      "replay eval", "");
    repl_state_edit_line_set(1);

    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    build_test_code_panel_layout(&layout, cp_w, text_x, cp_h);

    virtual_row = layout.header_rows + layout.cmd_main_rows[0];
    editor_scroll_set(virtual_row);

    line_y_start = (cp_y + cp_h) - CODE_MARGIN_Y - 2 * LINE_H;
    gl_y_mid = line_y_start - 3 + LINE_H / 2;
    my = ui_state_viewport().window_h - gl_y_mid;
    mx = cp_x + text_x + 2 * FONT_W;

    UiHit h = ui_panels_hit_test_current_snapshot(mx, my, 0);
    ASSERT_TRUE("virtual row hit kind", h.kind == UI_HIT_CODE_TEXT);
    ASSERT_TRUE("virtual row hit rewrites to source line", h.line_idx == 0);
    ASSERT_TRUE("virtual row hit clears char index", h.char_idx == -1);

    glr_ctrl_router_handle_code_panel_hit(h, mx, my);
    ASSERT_TRUE("virtual row click navigates to source line",
                repl_state_edit_line() == 0);
}

/* Regression: glVertex2f commands should generate gutter vertex-index labels
 * (v0, v1, ...) when show_vertex_indices is on, just like glVertex3f. */
static void test_vertex2f_gutter_labels(void) {
    printf("Testing glVertex2f gutter labels...\n");

    /* Use a wide viewport (4000px) so that the available character width
     * (>165 chars) stays above every header line in both the idx_col_w=0 and
     * idx_col_w=54 states.  Toggling show_vertex_indices then causes no row
     * wrapping changes, so the only glRasterPos2f delta is the "v0" label. */
    glr_app_reset_all();
    ui_state_viewport_set_size(4000, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.4f;

    /* Commit a real program so both the document array and editor buffer
     * are populated — the wrap iterator needs non-empty display text to
     * produce rows, which is required for the gutter to draw. */
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex2f(1, 2);");
    editor_feed_line("glEnd();");
    /* Cursor on glEnd so glVertex2f is a non-edit row with a visible gutter */
    repl_state_edit_line_set(2);

    /* The workspace header can be many rows long in builds with full REPL
     * state (e.g. stubs with bootstrap commands), so scroll=0 may not show
     * user code at all.  Run a warm-up render with follow_cursor set so that
     * the scroll settles to the cursor's row; both measurement renders then
     * see the same window and the vertex label is the only delta. */
    editor_scroll_follow_cursor_set(1);
    {
        UiRenderSnapshot s; make_test_ui_snapshot(&s);
        ui_panels_render_code_panel(&s, NULL);   /* scroll now follows cursor */
    }

    glr_state_presentation_mut()->show_vertex_indices = 0; glr_ctrl_sync_ui_chrome();
    unsigned long long base;
    {
        UiRenderSnapshot s; make_test_ui_snapshot(&s);
        gl_stub_counts_reset();
        ui_panels_render_code_panel(&s, NULL);
        base = gl_stub_counts[GL_STUB_glRasterPos2f];
    }

    glr_state_presentation_mut()->show_vertex_indices = 1; glr_ctrl_sync_ui_chrome();
    {
        UiRenderSnapshot s; make_test_ui_snapshot(&s);
        gl_stub_counts_reset();
        ui_panels_render_code_panel(&s, NULL);
    }
    ASSERT_TRUE("vertex2f gutter label drawn when show_vertex_indices=1",
                gl_stub_counts[GL_STUB_glRasterPos2f] > base);

    /* Confirm vertex3f has the same behaviour */
    glr_app_reset_all();
    ui_state_viewport_set_size(4000, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.4f;

    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(1, 2, 0);");
    editor_feed_line("glEnd();");
    repl_state_edit_line_set(2);

    editor_scroll_follow_cursor_set(1);
    {
        UiRenderSnapshot s; make_test_ui_snapshot(&s);
        ui_panels_render_code_panel(&s, NULL);
    }

    glr_state_presentation_mut()->show_vertex_indices = 0; glr_ctrl_sync_ui_chrome();
    unsigned long long v3f_base;
    {
        UiRenderSnapshot s; make_test_ui_snapshot(&s);
        gl_stub_counts_reset();
        ui_panels_render_code_panel(&s, NULL);
        v3f_base = gl_stub_counts[GL_STUB_glRasterPos2f];
    }
    glr_state_presentation_mut()->show_vertex_indices = 1; glr_ctrl_sync_ui_chrome();
    {
        UiRenderSnapshot s; make_test_ui_snapshot(&s);
        gl_stub_counts_reset();
        ui_panels_render_code_panel(&s, NULL);
    }
    ASSERT_TRUE("vertex3f gutter label consistent with vertex2f",
                gl_stub_counts[GL_STUB_glRasterPos2f] > v3f_base);
}

static void test_tutorial_fade_render_uses_per_char_path(void) {
    unsigned long long fading_raster_calls;
    unsigned long long settled_raster_calls;
    unsigned long long fading_alpha_calls;
    unsigned long long settled_alpha_calls;
    unsigned long long fading_solid_color_calls;
    unsigned long long settled_solid_color_calls;
    unsigned long long fading_blend_func_calls;
    unsigned long long settled_blend_func_calls;
    unsigned long long fading_enable_calls;
    unsigned long long settled_enable_calls;
    TutorialRuntimeState tutorial;

    printf("Testing tutorial fade render...\n");

    glr_app_reset_all();
    ui_state_viewport_set_size(4000, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.4f;

    tutorial_start(0);
    tutorial = tutorial_state_view();

    editor_scroll_follow_cursor_set(1);
    {
        UiRenderSnapshot s;
        make_test_ui_snapshot(&s);
        ui_panels_render_code_panel(&s, NULL);
    }

    {
        UiRenderSnapshot s;
        make_test_ui_snapshot(&s);
        s.anim_time = tutorial.fade_start_t + 0.01f;
        gl_stub_counts_reset();
        ui_panels_render_code_panel(&s, NULL);
        fading_raster_calls = gl_stub_counts[GL_STUB_glRasterPos2f];
        fading_alpha_calls = gl_stub_counts[GL_STUB_glColor4f];
        fading_solid_color_calls = gl_stub_counts[GL_STUB_glColor3f];
        fading_blend_func_calls = gl_stub_counts[GL_STUB_glBlendFunc];
        fading_enable_calls = gl_stub_counts[GL_STUB_glEnable];
    }

    {
        UiRenderSnapshot s;
        make_test_ui_snapshot(&s);
        s.anim_time = tutorial.fade_start_t + tutorial.fade_duration + 0.1f;
        gl_stub_counts_reset();
        ui_panels_render_code_panel(&s, NULL);
        settled_raster_calls = gl_stub_counts[GL_STUB_glRasterPos2f];
        settled_alpha_calls = gl_stub_counts[GL_STUB_glColor4f];
        settled_solid_color_calls = gl_stub_counts[GL_STUB_glColor3f];
        settled_blend_func_calls = gl_stub_counts[GL_STUB_glBlendFunc];
        settled_enable_calls = gl_stub_counts[GL_STUB_glEnable];
    }

    ASSERT_TRUE("tutorial fade increases raster ops",
                fading_raster_calls > settled_raster_calls);
    ASSERT_TRUE("tutorial fade increases alpha color ops",
                fading_alpha_calls > settled_alpha_calls);
    /* Settled path uses glColor3f for the body row; fade path replaces
     * it with per-char glColor4f. Pin both to confirm neither path
     * silently drifts into the other. */
    ASSERT_TRUE("settled path uses solid color body draw",
                settled_solid_color_calls > fading_solid_color_calls);
    /* Without an extra glBlendFunc / glEnable call inside the fade
     * path, glColor4f's alpha is dropped on the floor because the
     * panel chrome disables GL_BLEND before body rows render. */
    ASSERT_TRUE("tutorial fade enables blending",
                fading_blend_func_calls > settled_blend_func_calls);
    ASSERT_TRUE("tutorial fade re-enables GL_BLEND",
                fading_enable_calls > settled_enable_calls);
}

static void test_tutorial_fade_handles_wrapped_lines(void) {
    /* Force the comment row to wrap by shrinking the code panel until
     * the instruction text spans multiple wrap segments. The fade path
     * must still emit per-char raster ops across every visible segment
     * — line_len is computed from the full display_text so the per-
     * char delay spans the wrapped line, but every visible char on
     * every wrap row should still get a raster + alpha call. */
    unsigned long long fading_raster_calls;
    unsigned long long fading_alpha_calls;
    TutorialRuntimeState tutorial;
    int code_panel_w;

    printf("Testing tutorial fade render across wrap rows...\n");

    glr_app_reset_all();
    ui_state_viewport_set_size(220, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 1.0f;

    tutorial_start(0);
    tutorial = tutorial_state_view();

    {
        int cp_x;
        int cp_y;
        int cp_h;
        ui_layout_code_panel_rect(&cp_x, &cp_y, &code_panel_w, &cp_h);
    }

    editor_scroll_follow_cursor_set(1);
    {
        UiRenderSnapshot s;
        make_test_ui_snapshot(&s);
        ui_panels_render_code_panel(&s, NULL);
    }

    {
        UiRenderSnapshot s;
        make_test_ui_snapshot(&s);
        s.anim_time = tutorial.fade_start_t + 0.01f;
        gl_stub_counts_reset();
        ui_panels_render_code_panel(&s, NULL);
        fading_raster_calls = gl_stub_counts[GL_STUB_glRasterPos2f];
        fading_alpha_calls = gl_stub_counts[GL_STUB_glColor4f];
    }

    /* The first tutorial comment is ~25 chars and the panel is narrow
     * enough that it must wrap. The per-char fade emits a raster +
     * glColor4f per visible glyph across every wrap row. */
    ASSERT_TRUE("wrap-row fade emits raster ops",
                fading_raster_calls > 0);
    ASSERT_TRUE("wrap-row fade emits alpha color ops",
                fading_alpha_calls > 0);
}

int main(void) {
#ifndef GL_STUBS
    printf("This test requires GL stubs (USE_GL_STUBS=1)\n");
    return 0;
#endif

    glr_app_reset_all();

    test_help_overlay();
    test_profile_panel();
    test_color_picker();
    test_autocomplete_panel();
    test_variable_panel();
    test_menu_bar();
    test_ui_panels_hit_test();
    test_ui_menu_bar_hit_test();
    test_ui_color_picker_hit_test();
    test_ui_variable_panel_hit_test();
    test_ui_panels_hit_test_dispatch();
    test_ui_panels_hit_test_panel_divider();
    test_ui_panels_hit_test_code_text_cursor();
    test_ui_panels_hit_test_insert_line();
    test_ui_panels_hit_test_overwrite_row_kind();
    test_ui_panels_hit_test_trailing_blank_row_kind();
    test_ui_panels_hit_test_virtual_row_routes_to_source();
    test_vertex2f_gutter_labels();
    test_tutorial_fade_render_uses_per_char_path();
    test_tutorial_fade_handles_wrapped_lines();

    printf("\nUI Tests: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
