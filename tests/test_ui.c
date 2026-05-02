#include "sample.h"
#include "repl_state.h"
#include "repl_core.h"
#include "ui_help_overlay.h"
#include "prof.h"
#include "ui_profile_panel.h"
#include "ui_color_picker.h"
#include "ui_autocomplete_panel.h"
#include "ui_variable_panel.h"
#include "ui_menu_bar.h"
#include "ui_snapshot.h"
#include "repl_var_drag.h"
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
    snap->viewport       = repl_state_viewport();
    snap->presentation   = repl_state_presentation();
    snap->code_panel     = repl_state_code_panel();
    snap->help           = repl_state_help();
    snap->variable_panel = repl_state_variable_panel();
    snap->profile_panel  = repl_state_profile_panel();
    snap->status         = repl_state_status();
    snap->search         = repl_state_search();
    snap->autocomplete   = repl_state_autocomplete();
    snap->camera         = repl_state_camera();
    snap->pointer        = repl_state_pointer();
    snap->render         = repl_state_render();
    snap->replay         = repl_state_replay();
    snap->scenes         = repl_state_scenes();
    snap->variable_drag  = repl_state_variable_drag();
    snap->selection      = repl_state_selection();
    snap->variables      = repl_state_variables();
    snap->editor_input   = repl_state_editor_input();
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
    
    repl_state_help_mut()->visible = 0;
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_help_overlay_render(&s); }
    ASSERT_TRUE("help hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    repl_state_help_mut()->visible = 1;
    repl_state_viewport_set_size(800, 600);
    repl_state_help_mut()->tab_idx = 0;
    repl_state_help_mut()->scroll = 0;

    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_help_overlay_render(&s); }
    ASSERT_GL_CALLS("help visible -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("help visible -> calls glVertex2f", GL_STUB_glVertex2f, 4);
    ASSERT_GL_CALLS("help visible -> draws text", GL_STUB_glRasterPos2f, 10);
    ASSERT_GL_CALLS("help visible -> enables blending", GL_STUB_glEnable, 1);

    /* Switch tabs */
    repl_state_help_mut()->tab_idx = 1;
    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_help_overlay_render(&s); }
    ASSERT_GL_CALLS("help tab 1 -> draws text", GL_STUB_glRasterPos2f, 10);
}

static void test_profile_panel(void) {
    printf("Testing Profile Panel...\n");
    gl_stub_counts_reset();
    
    repl_state_profile_panel_mut()->mode = PROFILE_PANEL_OFF;
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_profile_panel_render(&s); }
    ASSERT_TRUE("profile hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    repl_state_profile_panel_mut()->mode = PROFILE_PANEL_ON;
    prof_frame_tick();
    prof_begin(PROF_FRAME_TOTAL);
    prof_end(PROF_FRAME_TOTAL);

    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_profile_panel_render(&s); }
    ASSERT_GL_CALLS("profile visible -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("profile visible -> draws text", GL_STUB_glRasterPos2f, 5);
    ASSERT_GL_CALLS("profile visible -> calls glColor4f", GL_STUB_glColor4f, 1);

    /* Test details mode */
    repl_state_profile_panel_mut()->mode = PROFILE_PANEL_DETAILS;
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
    
    repl_state_viewport_set_size(800, 600);
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
    ui_color_picker_press(px + 10, repl_state_viewport().window_h - (py - 10));
    ui_color_picker_motion(px + 20, repl_state_viewport().window_h - (py - 20));
    ui_color_picker_release();

    /* Press Hue bar (Offset by CP_SV_SZ + CP_GAP = 150 + 6 = 156) */
    ui_color_picker_press(px + 156 + 10, repl_state_viewport().window_h - (py - 10));
    ui_color_picker_motion(px + 156 + 10, repl_state_viewport().window_h - (py - 20));
    ui_color_picker_release();

    /* Test Alpha support */
    repl_state_document_cmds_mut()[0].type = CMD_COLOR4F;
    repl_state_document_cmds_mut()[0].args[3] = 0.5f;
    ui_color_picker_open(0, 300);
    
    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_color_picker_render(&s); }
    ASSERT_GL_CALLS("picker render with alpha -> draws quads", GL_STUB_glBegin, 1);
    
    /* Press Alpha bar (alp_x = hue_x + 18 + 6 = px + 156 + 24 = 180) */
    ui_color_picker_press(px + 185, repl_state_viewport().window_h - (py - 10));
    ui_color_picker_motion(px + 185, repl_state_viewport().window_h - (py - 20));
    ui_color_picker_release();

    /* Test glClearColor limits */
    repl_state_document_cmds_mut()[0].type = CMD_CLEAR_COLOR;
    ui_color_picker_open(0, 300);
    ui_color_picker_press(px + 10, repl_state_viewport().window_h - (py - 5)); // High V
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
    
    repl_state_autocomplete_mut()->match_count = 0;
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_autocomplete_panel_render(&s); }
    ASSERT_TRUE("ac hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    repl_state_autocomplete_mut()->match_count = 2;
    repl_state_autocomplete_mut()->matches[0] = "glVertex3f";
    repl_state_autocomplete_mut()->matches[1] = "glVertex2f";
    repl_state_autocomplete_mut()->selected_idx = 0;
    repl_state_code_panel_mut()->cursor_px = 100;
    repl_state_code_panel_mut()->cursor_py = 100;
    
    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_autocomplete_panel_render(&s); }
    ASSERT_GL_CALLS("ac visible -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("ac visible -> draws text", GL_STUB_glRasterPos2f, 2);
    ASSERT_GL_CALLS("ac visible -> calls glVertex2f", GL_STUB_glVertex2f, 4);
}

static void test_variable_panel(void) {
    printf("Testing Variable Panel...\n");
    gl_stub_counts_reset();
    
    repl_state_variable_panel_mut()->visible = 0;
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_variable_panel_render(&s); }
    ASSERT_TRUE("var panel hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    repl_state_variable_panel_mut()->visible = 1;
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
    repl_state_viewport_set_size(800, 600);
    int px, py, pw, ph;
    ui_variable_panel_rect(&px, &py, &pw, &ph);
    ASSERT_TRUE("hit test in panel", ui_variable_panel_hit(px + 10, repl_state_viewport().window_h - (py + 10), &row));
    ASSERT_TRUE("hit test outside panel", !ui_variable_panel_hit(0, 0, &row));
}

static void test_menu_bar(void) {
    printf("Testing Menu Bar...\n");
    gl_stub_counts_reset();
    
    repl_state_viewport_set_size(800, 600);
    repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    repl_state_code_panel_mut()->panel_frac = 0.5f;
    
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
    repl_state_search_mut()->active = 1;
    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_menu_bar_render_search_overlay(&s, 0, 400, 600); }
    ASSERT_GL_CALLS("search overlay -> draws quads", GL_STUB_glBegin, 1);
    
    ui_menu_bar_close();
    ASSERT_TRUE("menu closed", !ui_menu_bar_menu_dropdown_is_open());
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
    
    printf("\nUI Tests: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
