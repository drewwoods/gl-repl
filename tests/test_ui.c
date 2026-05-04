#include "sample.h"
#include "ui_state.h"
#include "repl_state.h"
#include "repl_core.h"
#include "editor_help_session.h"
#include "ui_help_overlay.h"
#include "prof.h"
#include "ui_profile_panel.h"
#include "ui_color_picker.h"
#include "ui_autocomplete_panel.h"
#include "ui_variable_panel.h"
#include "ui_menu_bar.h"
#include "ui_panels.h"
#include "ui_snapshot.h"
#include "variable_panel_drag.h"
#include "support/test_harness.h"
#include <GL/gl_stub_counts.h>

#include <stdio.h>
#include <string.h>
#include <assert.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define ASSERT_GL_CALLS(label, counter, min_calls) do { \
    TEST_ASSERT_TRUE(&g_harness, label, gl_stub_counts[counter] >= (min_calls)); \
} while (0)

/* Build a UiRenderSnapshot from the current REPL state for renderer tests.
 * Mirrors imrepl_ctrl_build_ui_snapshot(); the test harness can't link the
 * controller TU, so we duplicate the relevant slice population here. */
static void make_test_ui_snapshot(UiRenderSnapshot *snap) {
    memset(snap, 0, sizeof(*snap));
    snap->viewport       = ui_state_viewport();
    snap->presentation   = repl_state_presentation();
    snap->code_panel     = ui_state_code_panel();
    snap->help           = ui_state_help();
    snap->help_session   = editor_help_session_view();
    snap->variable_panel = ui_state_variable_panel();
    snap->profile_panel  = ui_state_profile_panel();
    snap->status         = ui_state_status();
    snap->search         = editor_state_search();
    snap->autocomplete   = editor_state_autocomplete();
    snap->camera         = ui_state_camera();
    snap->pointer        = ui_state_pointer();
    snap->render         = repl_state_render();
    snap->replay         = repl_state_replay();
    snap->scenes         = repl_state_scenes();
    snap->variable_drag  = editor_state_variable_drag();
    snap->selection      = editor_state_selection();
    snap->scroll         = editor_state_scroll();
    snap->variables      = repl_state_variables();
    snap->editor_input   = editor_state_input();
    snap->import_export  = repl_state_import_export();
    snap->flat_program   = repl_state_flat_program_view();
    snap->predef         = repl_eval_predef_view();
    snap->document_cmds  = repl_state_document_cmds();
    snap->document_count = repl_state_document_count();
    snap->edit_line      = repl_state_edit_line();
    snap->insert_mode    = snap->editor_input.insert_mode;
    snap->cursor_pos     = snap->editor_input.cursor_pos;
    snap->input_len      = snap->editor_input.input_len;
    snap->flat_program_count = snap->flat_program.cmd_count;
    snap->user_scene_active_idx = -1;
}

static void test_help_overlay(void) {
    printf("Testing Help Overlay...\n");
    gl_stub_counts_reset();
    
    ui_state_help_mut()->visible = 0;
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_help_overlay_render(&s); }
    ASSERT_TRUE("help hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    ui_state_help_mut()->visible = 1;
    ui_state_viewport_set_size(800, 600);
    editor_help_session_set_tab(0);
    editor_help_session_set_scroll(0);

    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_help_overlay_render(&s); }
    ASSERT_GL_CALLS("help visible -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("help visible -> calls glVertex2f", GL_STUB_glVertex2f, 4);
    ASSERT_GL_CALLS("help visible -> draws text", GL_STUB_glRasterPos2f, 10);
    ASSERT_GL_CALLS("help visible -> enables blending", GL_STUB_glEnable, 1);

    /* Switch tabs */
    editor_help_session_set_tab(1);
    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_help_overlay_render(&s); }
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
    
    ASSERT_TRUE("can edit color cmd", ui_color_picker_can_edit_cmd(0));
    
    ui_state_viewport_set_size(800, 600);
    ui_color_picker_open(0, 300);
    ASSERT_TRUE("picker is active", ui_color_picker_active_line() == 0);
    
    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_color_picker_render(&s); }
    ASSERT_GL_CALLS("picker render -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("picker render -> calls glVertex2f", GL_STUB_glVertex2f, 4);
    
    /* Test interactions */
    int px = 400;
    int py = 300;
    
    /* Press SV square (CP_SV_SZ = 150) */
    ui_color_picker_press(px + 10, ui_state_viewport().window_h - (py - 10));
    ui_color_picker_motion(px + 20, ui_state_viewport().window_h - (py - 20));
    ui_color_picker_release();

    /* Press Hue bar (Offset by CP_SV_SZ + CP_GAP = 150 + 6 = 156) */
    ui_color_picker_press(px + 156 + 10, ui_state_viewport().window_h - (py - 10));
    ui_color_picker_motion(px + 156 + 10, ui_state_viewport().window_h - (py - 20));
    ui_color_picker_release();

    /* Test Alpha support */
    repl_state_document_cmds_mut()[0].type = CMD_COLOR4F;
    repl_state_document_cmds_mut()[0].args[3] = 0.5f;
    ui_color_picker_open(0, 300);
    
    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_color_picker_render(&s); }
    ASSERT_GL_CALLS("picker render with alpha -> draws quads", GL_STUB_glBegin, 1);
    
    /* Press Alpha bar (alp_x = hue_x + 18 + 6 = px + 156 + 24 = 180) */
    ui_color_picker_press(px + 185, ui_state_viewport().window_h - (py - 10));
    ui_color_picker_motion(px + 185, ui_state_viewport().window_h - (py - 20));
    ui_color_picker_release();

    /* Test glClearColor limits */
    repl_state_document_cmds_mut()[0].type = CMD_CLEAR_COLOR;
    ui_color_picker_open(0, 300);
    ui_color_picker_press(px + 10, ui_state_viewport().window_h - (py - 5)); // High V
    ui_color_picker_release();
    
    ui_color_picker_close();
    ASSERT_TRUE("picker closed", ui_color_picker_active_line() == -1);

    /* Test swatch rendering — render reads from a transformer entry, not
     * from the document, so build a minimal one for the test. */
    gl_stub_counts_reset();
    {
        EditorTransformer t = {
            .line_idx = 0,
            .char_start = -1,
            .char_end = -1,
            .kind = TRANSFORMER_COLOR_PICKER,
            .state.color = {
                .r = 0.5f, .g = 0.5f, .b = 0.5f, .a = 1.0f,
                .has_alpha = 0, .is_clear = 0,
            },
        };
        ui_color_picker_render_swatch(&t, 100, 100);
    }
    ASSERT_GL_CALLS("swatch render -> draws quads", GL_STUB_glBegin, 1);
}

static void test_autocomplete_panel(void) {
    printf("Testing Autocomplete Panel...\n");
    gl_stub_counts_reset();
    
    editor_state_autocomplete_mut()->match_count = 0;
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_autocomplete_panel_render(&s); }
    ASSERT_TRUE("ac hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    editor_state_autocomplete_mut()->match_count = 2;
    editor_state_autocomplete_mut()->matches[0] = "glVertex3f";
    editor_state_autocomplete_mut()->matches[1] = "glVertex2f";
    editor_state_autocomplete_mut()->selected_idx = 0;
    ui_state_code_panel_mut()->cursor_px = 100;
    ui_state_code_panel_mut()->cursor_py = 100;
    
    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_autocomplete_panel_render(&s); }
    ASSERT_GL_CALLS("ac visible -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("ac visible -> draws text", GL_STUB_glRasterPos2f, 2);
    ASSERT_GL_CALLS("ac visible -> calls glVertex2f", GL_STUB_glVertex2f, 4);
}

static void test_variable_panel(void) {
    printf("Testing Variable Panel...\n");
    gl_stub_counts_reset();
    
    ui_state_variable_panel_mut()->visible = 0;
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_variable_panel_render(&s); }
    ASSERT_TRUE("var panel hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    ui_state_variable_panel_mut()->visible = 1;
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
    ui_variable_panel_rect(&px, &py, &pw, &ph);
    ASSERT_TRUE("hit test in panel", ui_variable_panel_hit(px + 10, ui_state_viewport().window_h - (py + 10), &row));
    ASSERT_TRUE("hit test outside panel", !ui_variable_panel_hit(0, 0, &row));
}

static void test_menu_bar(void) {
    printf("Testing Menu Bar...\n");
    gl_stub_counts_reset();
    
    ui_state_viewport_set_size(800, 600);
    repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
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
    ui_menu_bar_set_open_menu(0); // File menu
    ASSERT_TRUE("dropdown open", ui_menu_bar_menu_dropdown_is_open());
    
    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_menu_bar_render_example_dropdown(&s); }
    ASSERT_GL_CALLS("dropdown render -> draws quads", GL_STUB_glBegin, 1);
    
    /* Dropdown item hit */
    ASSERT_TRUE("dropdown item hit", ui_menu_bar_dropdown_item_hit(20, 100) >= 0);
    
    /* Test config menu */
    ui_menu_bar_open_config();
    ASSERT_TRUE("config menu open", ui_menu_bar_open_menu_id() == 2); // MENU_CONFIG

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
    repl_reset_state();
    ui_state_viewport_set_size(800, 600);

    /* Click in the scene region (below the panel, my > 270). */
    UiHit h_scene = ui_panels_hit_test(400, 400);
    ASSERT_TRUE("scene hit kind", h_scene.kind == UI_HIT_SCENE);

    /* Click inside the code panel's text area. mx > gutter end
     * (CODE_MARGIN_X + 4*FONT_W = ~40) and my within the panel. */
    UiHit h_text = ui_panels_hit_test(150, 100);
    ASSERT_TRUE("code-panel text hit kind",
                h_text.kind == UI_HIT_CODE_TEXT);

    /* Click in the gutter (line numbers, mx < ~40). */
    UiHit h_gutter = ui_panels_hit_test(20, 100);
    ASSERT_TRUE("code-panel gutter hit kind",
                h_gutter.kind == UI_HIT_CODE_GUTTER);

    /* Help overlay takes priority over everything. */
    ui_state_help_mut()->visible = 1;
    UiHit h_help = ui_panels_hit_test(150, 100);
    ASSERT_TRUE("help overlay takes priority",
                h_help.kind == UI_HIT_HELP_PANEL);
    ui_state_help_mut()->visible = 0;
}

/* Phase E commit 29: hit-test entry points for ui_menu_bar,
 * ui_color_picker, ui_variable_panel return passive UiHit
 * classifications without mutating state. ui_panels_hit_test
 * dispatches to them in priority order. */
static void test_ui_menu_bar_hit_test(void) {
    repl_reset_state();
    ui_state_viewport_set_size(800, 600);
    repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    ui_state_code_panel_mut()->panel_frac = 0.5f;

    /* Outside menu bar entirely (well below it). */
    UiHit h_none = ui_menu_bar_hit_test(20, 400);
    ASSERT_TRUE("menu bar miss -> NONE", h_none.kind == UI_HIT_NONE);

    /* Top-level menu button (File at left edge, my within bar). */
    ui_menu_bar_close();
    UiHit h_menu = ui_menu_bar_hit_test(20, 10);
    ASSERT_TRUE("menu top-level hit kind",
                h_menu.kind == UI_HIT_MENU_ITEM);
    ASSERT_TRUE("menu top-level item_idx is menu_id",
                h_menu.item_idx >= 0 && h_menu.item_idx < 3);

    /* Pin button (right side, mx near right edge). */
    UiHit h_pin = ui_menu_bar_hit_test(380, 10);
    ASSERT_TRUE("pin hit kind", h_pin.kind == UI_HIT_PIN_BUTTON);
    ASSERT_TRUE("pin item_idx populated", h_pin.item_idx >= 0);

    /* Open a menu, then a click on a dropdown row should report
     * UI_HIT_MENU_ITEM with item_idx = row. The disambiguator from
     * top-level vs row is ui_menu_bar_open_menu_id() != -1. */
    ui_menu_bar_set_open_menu(0); /* File menu */
    UiHit h_row = ui_menu_bar_hit_test(20, 100);
    ASSERT_TRUE("dropdown row hit kind",
                h_row.kind == UI_HIT_MENU_ITEM);
    ASSERT_TRUE("dropdown row item_idx populated", h_row.item_idx >= 0);
    ASSERT_TRUE("open menu disambiguates row",
                ui_menu_bar_open_menu_id() == 0);
    ui_menu_bar_close();
}

static void test_ui_color_picker_hit_test(void) {
    repl_reset_state();
    ui_state_viewport_set_size(800, 600);

    /* No picker active -> miss. */
    UiHit h_closed = ui_color_picker_hit_test(400, 300);
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
    ui_color_picker_open(0, 300);

    int win_h = ui_state_viewport().window_h;
    int sv_mx = -1, sv_my = -1;
    int hue_mx = -1, hue_my = -1;
    for (int my = 0; my < win_h; my += 4) {
        for (int mx = 0; mx < ui_state_viewport().window_w; mx += 4) {
            UiHit h = ui_color_picker_hit_test(mx, my);
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

    UiHit h_sv = ui_color_picker_hit_test(sv_mx, sv_my);
    ASSERT_TRUE("SV hit kind", h_sv.kind == UI_HIT_COLOR_SWATCH);
    ASSERT_TRUE("SV cmd_idx is 0", h_sv.cmd_idx == 0);
    ASSERT_TRUE("SV item_idx is 1", h_sv.item_idx == 1);

    UiHit h_hue = ui_color_picker_hit_test(hue_mx, hue_my);
    ASSERT_TRUE("hue hit kind", h_hue.kind == UI_HIT_COLOR_SWATCH);
    ASSERT_TRUE("hue item_idx is 2", h_hue.item_idx == 2);

    /* Click far outside picker -> NONE. */
    UiHit h_miss = ui_color_picker_hit_test(0, 0);
    ASSERT_TRUE("picker miss -> NONE", h_miss.kind == UI_HIT_NONE);
    ui_color_picker_close();
}

static void test_ui_variable_panel_hit_test(void) {
    repl_reset_state();
    ui_state_viewport_set_size(800, 600);

    /* Panel hidden -> always miss. */
    ui_state_variable_panel_mut()->visible = 0;
    UiHit h_off = ui_variable_panel_hit_test(700, 100);
    ASSERT_TRUE("hidden panel -> NONE", h_off.kind == UI_HIT_NONE);

    /* Visible panel with one declared variable. */
    ui_state_variable_panel_mut()->visible = 1;
    g_num_predef_vars = 1;
    strcpy(g_predef_vars[0].name, "x");
    g_predef_vars[0].value = 1.0f;

    int px, py, pw, ph;
    ui_variable_panel_rect(&px, &py, &pw, &ph);
    int my = ui_state_viewport().window_h - (py + 10);

    UiHit h_row = ui_variable_panel_hit_test(px + 10, my);
    ASSERT_TRUE("row hit kind", h_row.kind == UI_HIT_VARIABLE_SLIDER);
    ASSERT_TRUE("row item_idx populated",
                h_row.item_idx >= 0 && h_row.item_idx < g_num_predef_vars);

    /* Click outside the panel rect -> NONE. */
    UiHit h_out = ui_variable_panel_hit_test(0, 0);
    ASSERT_TRUE("outside panel -> NONE", h_out.kind == UI_HIT_NONE);
}

/* Verify ui_panels_hit_test routes to the floating-overlay
 * hit-testers in priority order before falling through to the
 * code panel and scene. */
static void test_ui_panels_hit_test_dispatch(void) {
    repl_reset_state();
    ui_state_viewport_set_size(800, 600);
    repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    ui_state_code_panel_mut()->panel_frac = 0.5f;

    /* Variable panel should win over the scene-region fallback when
     * a click lands on its rect. */
    ui_state_variable_panel_mut()->visible = 1;
    g_num_predef_vars = 1;
    strcpy(g_predef_vars[0].name, "x");
    g_predef_vars[0].value = 1.0f;
    int px, py, pw, ph;
    ui_variable_panel_rect(&px, &py, &pw, &ph);
    int my_var = ui_state_viewport().window_h - (py + 10);
    UiHit h_var = ui_panels_hit_test(px + 10, my_var);
    ASSERT_TRUE("var panel routed via panels_hit_test",
                h_var.kind == UI_HIT_VARIABLE_SLIDER);
    ui_state_variable_panel_mut()->visible = 0;

    /* Menu pin button click should resolve as UI_HIT_PIN_BUTTON. */
    ui_menu_bar_close();
    UiHit h_pin = ui_panels_hit_test(380, 10);
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
    ui_color_picker_open(0, 300);

    int sv_mx = -1, sv_my = -1;
    for (int my = 0; my < ui_state_viewport().window_h && sv_mx < 0; my += 4) {
        for (int mx = 0; mx < ui_state_viewport().window_w; mx += 4) {
            UiHit h = ui_color_picker_hit_test(mx, my);
            if (h.kind == UI_HIT_COLOR_SWATCH && h.item_idx == 1) {
                sv_mx = mx; sv_my = my;
                break;
            }
        }
    }
    ASSERT_TRUE("dispatch SV rect probed", sv_mx >= 0);
    UiHit h_pick = ui_panels_hit_test(sv_mx, sv_my);
    ASSERT_TRUE("picker routed via panels_hit_test",
                h_pick.kind == UI_HIT_COLOR_SWATCH);
    ui_color_picker_close();
}

/* Regression: glVertex2f commands should generate gutter vertex-index labels
 * (v0, v1, ...) when show_vertex_indices is on, just like glVertex3f. */
static void test_vertex2f_gutter_labels(void) {
    printf("Testing glVertex2f gutter labels...\n");

    /* Use a wide viewport (4000px) so that the available character width
     * (>165 chars) stays above every header line in both the idx_col_w=0 and
     * idx_col_w=54 states.  Toggling show_vertex_indices then causes no row
     * wrapping changes, so the only glRasterPos2f delta is the "v0" label. */
    repl_reset_state();
    ui_state_viewport_set_size(4000, 600);
    repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    ui_state_code_panel_mut()->panel_frac = 0.4f;

    /* Commit a real program so both the document array and editor buffer
     * are populated — the wrap iterator needs non-empty display text to
     * produce rows, which is required for the gutter to draw. */
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex2f(1, 2);");
    repl_feed_line_public("glEnd();");
    /* Cursor on glEnd so glVertex2f is a non-edit row with a visible gutter */
    repl_state_edit_line_set(2);

    repl_state_presentation_mut()->show_vertex_indices = 0;
    unsigned long long base;
    {
        UiRenderSnapshot s; make_test_ui_snapshot(&s);
        gl_stub_counts_reset();
        ui_panels_render_code_panel(&s);
        base = gl_stub_counts[GL_STUB_glRasterPos2f];
    }

    repl_state_presentation_mut()->show_vertex_indices = 1;
    {
        UiRenderSnapshot s; make_test_ui_snapshot(&s);
        gl_stub_counts_reset();
        ui_panels_render_code_panel(&s);
    }
    ASSERT_TRUE("vertex2f gutter label drawn when show_vertex_indices=1",
                gl_stub_counts[GL_STUB_glRasterPos2f] > base);

    /* Confirm vertex3f has the same behaviour */
    repl_reset_state();
    ui_state_viewport_set_size(4000, 600);
    repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    ui_state_code_panel_mut()->panel_frac = 0.4f;

    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    repl_feed_line_public("glVertex3f(1, 2, 0);");
    repl_feed_line_public("glEnd();");
    repl_state_edit_line_set(2);

    repl_state_presentation_mut()->show_vertex_indices = 0;
    unsigned long long v3f_base;
    {
        UiRenderSnapshot s; make_test_ui_snapshot(&s);
        gl_stub_counts_reset();
        ui_panels_render_code_panel(&s);
        v3f_base = gl_stub_counts[GL_STUB_glRasterPos2f];
    }
    repl_state_presentation_mut()->show_vertex_indices = 1;
    {
        UiRenderSnapshot s; make_test_ui_snapshot(&s);
        gl_stub_counts_reset();
        ui_panels_render_code_panel(&s);
    }
    ASSERT_TRUE("vertex3f gutter label consistent with vertex2f",
                gl_stub_counts[GL_STUB_glRasterPos2f] > v3f_base);
}

int main(void) {
#ifndef OPENGL_VIBE_USE_GL_STUBS
    printf("This test requires GL stubs (USE_GL_STUBS=1)\n");
    return 0;
#endif

    repl_reset_state();

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
    test_vertex2f_gutter_labels();

    printf("\nUI Tests: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
