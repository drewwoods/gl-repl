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
#include "repl_var_drag.h"
#include <GL/gl_stub_counts.h>

#include <stdio.h>
#include <string.h>
#include <assert.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s] (line %d)\n", label, __LINE__); \
} while (0)

#define ASSERT_GL_CALLS(label, counter, min_calls) do { \
    g_run++; \
    if (gl_stub_counts[counter] >= (min_calls)) g_pass++; \
    else printf("FAIL [%s] GL call %s: got %llu, expected >= %d (line %d)\n", \
                label, gl_stub_count_name(counter), gl_stub_counts[counter], (min_calls), __LINE__); \
} while (0)

static void test_help_overlay(void) {
    printf("Testing Help Overlay...\n");
    gl_stub_counts_reset();
    
    repl_state_help_mut()->visible = 0;
    ui_help_overlay_render();
    ASSERT_TRUE("help hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    repl_state_help_mut()->visible = 1;
    repl_state_viewport_set_size(800, 600);
    repl_state_help_mut()->tab_idx = 0;
    repl_state_help_mut()->scroll = 0;

    gl_stub_counts_reset();
    ui_help_overlay_render();
    ASSERT_GL_CALLS("help visible -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("help visible -> calls glVertex2f", GL_STUB_glVertex2f, 4);
    ASSERT_GL_CALLS("help visible -> draws text", GL_STUB_glRasterPos2f, 10);
    ASSERT_GL_CALLS("help visible -> enables blending", GL_STUB_glEnable, 1);

    /* Switch tabs */
    repl_state_help_mut()->tab_idx = 1;
    gl_stub_counts_reset();
    ui_help_overlay_render();
    ASSERT_GL_CALLS("help tab 1 -> draws text", GL_STUB_glRasterPos2f, 10);
}

static void test_profile_panel(void) {
    printf("Testing Profile Panel...\n");
    gl_stub_counts_reset();
    
    repl_state_profile_panel_mut()->mode = PROFILE_PANEL_OFF;
    ui_profile_panel_render();
    ASSERT_TRUE("profile hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    repl_state_profile_panel_mut()->mode = PROFILE_PANEL_ON;
    prof_frame_tick();
    prof_begin(PROF_FRAME_TOTAL);
    prof_end(PROF_FRAME_TOTAL);

    gl_stub_counts_reset();
    ui_profile_panel_render();
    ASSERT_GL_CALLS("profile visible -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("profile visible -> draws text", GL_STUB_glRasterPos2f, 5);
    ASSERT_GL_CALLS("profile visible -> calls glColor4f", GL_STUB_glColor4f, 1);

    /* Test details mode */
    repl_state_profile_panel_mut()->mode = PROFILE_PANEL_DETAILS;
    gl_stub_counts_reset();
    ui_profile_panel_render();
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
    strcpy(repl_state_document_cmds_mut()[0].source, "glColor3f(1, 0, 0);");
    
    ASSERT_TRUE("can edit color cmd", ui_color_picker_can_edit_cmd(0));
    
    repl_state_viewport_set_size(800, 600);
    ui_color_picker_open(0, 300);
    ASSERT_TRUE("picker is active", ui_color_picker_active_line() == 0);
    
    gl_stub_counts_reset();
    ui_color_picker_render();
    ASSERT_GL_CALLS("picker render -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("picker render -> calls glVertex2f", GL_STUB_glVertex2f, 4);
    
    /* Test interactions */
    int px = 400;
    int py = 300;
    
    /* Press SV square (CP_SV_SZ = 150) */
    ui_color_picker_press(px + 10, *repl_state_viewport()->window_h - (py - 10));
    ui_color_picker_motion(px + 20, *repl_state_viewport()->window_h - (py - 20));
    ui_color_picker_release();

    /* Press Hue bar (Offset by CP_SV_SZ + CP_GAP = 150 + 6 = 156) */
    ui_color_picker_press(px + 156 + 10, *repl_state_viewport()->window_h - (py - 10));
    ui_color_picker_motion(px + 156 + 10, *repl_state_viewport()->window_h - (py - 20));
    ui_color_picker_release();

    /* Test Alpha support */
    repl_state_document_cmds_mut()[0].type = CMD_COLOR4F;
    repl_state_document_cmds_mut()[0].args[3] = 0.5f;
    strcpy(repl_state_document_cmds_mut()[0].source, "glColor4f(1, 0, 0, 0.5);");
    ui_color_picker_open(0, 300);
    
    gl_stub_counts_reset();
    ui_color_picker_render();
    ASSERT_GL_CALLS("picker render with alpha -> draws quads", GL_STUB_glBegin, 1);
    
    /* Press Alpha bar (alp_x = hue_x + 18 + 6 = px + 156 + 24 = 180) */
    ui_color_picker_press(px + 185, *repl_state_viewport()->window_h - (py - 10));
    ui_color_picker_motion(px + 185, *repl_state_viewport()->window_h - (py - 20));
    ui_color_picker_release();

    /* Test glClearColor limits */
    repl_state_document_cmds_mut()[0].type = CMD_CLEAR_COLOR;
    ui_color_picker_open(0, 300);
    ui_color_picker_press(px + 10, *repl_state_viewport()->window_h - (py - 5)); // High V
    ui_color_picker_release();
    
    ui_color_picker_close();
    ASSERT_TRUE("picker closed", ui_color_picker_active_line() == -1);

    /* Test swatch rendering */
    gl_stub_counts_reset();
    ui_color_picker_render_swatch(0, 100, 100);
    ASSERT_GL_CALLS("swatch render -> draws quads", GL_STUB_glBegin, 1);
}

static void test_autocomplete_panel(void) {
    printf("Testing Autocomplete Panel...\n");
    gl_stub_counts_reset();
    
    *repl_state_autocomplete_mut()->match_count = 0;
    ui_autocomplete_panel_render();
    ASSERT_TRUE("ac hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    *repl_state_autocomplete_mut()->match_count = 2;
    repl_state_autocomplete_mut()->matches[0] = "glVertex3f";
    repl_state_autocomplete_mut()->matches[1] = "glVertex2f";
    *repl_state_autocomplete_mut()->selected_idx = 0;
    *repl_state_code_panel_mut()->cursor_px = 100;
    *repl_state_code_panel_mut()->cursor_py = 100;
    
    gl_stub_counts_reset();
    ui_autocomplete_panel_render();
    ASSERT_GL_CALLS("ac visible -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("ac visible -> draws text", GL_STUB_glRasterPos2f, 2);
    ASSERT_GL_CALLS("ac visible -> calls glVertex2f", GL_STUB_glVertex2f, 4);
}

static void test_variable_panel(void) {
    printf("Testing Variable Panel...\n");
    gl_stub_counts_reset();
    
    repl_state_variable_panel_mut()->visible = 0;
    ui_variable_panel_render();
    ASSERT_TRUE("var panel hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    repl_state_variable_panel_mut()->visible = 1;
    g_num_predef_vars = 1;
    strcpy(g_predef_vars[0].name, "x");
    g_predef_vars[0].value = 1.0f;

    gl_stub_counts_reset();
    ui_variable_panel_render();
    ASSERT_GL_CALLS("var panel visible -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("var panel visible -> draws text", GL_STUB_glRasterPos2f, 2);
    ASSERT_GL_CALLS("var panel visible -> calls glColor4f", GL_STUB_glColor4f, 1);
    
    /* Test hit testing */
    int row = -1;
    repl_state_viewport_set_size(800, 600);
    int px, py, pw, ph;
    ui_variable_panel_rect(&px, &py, &pw, &ph);
    ASSERT_TRUE("hit test in panel", ui_variable_panel_hit(px + 10, *repl_state_viewport()->window_h - (py + 10), &row));
    ASSERT_TRUE("hit test outside panel", !ui_variable_panel_hit(0, 0, &row));
}

static void test_menu_bar(void) {
    printf("Testing Menu Bar...\n");
    gl_stub_counts_reset();
    
    repl_state_viewport_set_size(800, 600);
    *repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    *repl_state_code_panel_mut()->panel_frac = 0.5f;
    
    gl_stub_counts_reset();
    ui_menu_bar_render();
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
    ui_menu_bar_render_example_dropdown();
    ASSERT_GL_CALLS("dropdown render -> draws quads", GL_STUB_glBegin, 1);
    
    /* Dropdown item hit */
    ASSERT_TRUE("dropdown item hit", ui_menu_bar_dropdown_item_hit(20, 100) >= 0);
    
    /* Test config menu */
    ui_menu_bar_open_config();
    ASSERT_TRUE("config menu open", ui_menu_bar_open_menu_id() == 2); // MENU_CONFIG

    /* Test search overlay */
    repl_state_search_mut()->active = 1;
    gl_stub_counts_reset();
    ui_menu_bar_render_search_overlay(0, 400, 600);
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
    
    printf("\nUI Tests: %d/%d passed\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
