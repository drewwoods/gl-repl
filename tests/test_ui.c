#include "editor/state.h"
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#define STATUSBAR_H 22
#include "ui/core/metrics.h"
#include "ui/app/state.h"
#include "repl/state_owners.h"
#include "repl/color_limits.h"
#include "subsystems/replay/replay_state.h"
#include "repl/example_loader.h"
#include "editor/input.h"
#include "editor/help_session.h"
#include "ui/core/tabbed_overlay.h"
#include "support/cpuprof.h"
#include "ui/support/cpuprof.h"
#include "subsystems/color_picker/color_picker_state.h"
#include "ui/subsystems/color_picker.h"
#include "ui/app/color_swatch.h"             /* ui_color_picker_render_swatch */
#include "app/glr_color_picker_bridge.h"     /* glr_color_picker_install_host */
#include "ui/app/autocomplete_panel.h"
#include "ui/subsystems/variable_panel.h"
#include "ui/app/variable_panel_view.h"
#include "app/glr_actions.h"
#include "ui/app/menu_bar.h"
#include "ui/app/panels.h"
#include "ui/app/snapshot.h"
#include "ui/app/layout.h"
#include "ui/app/repl_code_panel.h"
#include "subsystems/variable_panel/variable_panel_state.h"
#include "subsystems/variable_panel/variable_panel_drag.h"
#include "subsystems/tutorial/tutorial_animation.h"
#include "subsystems/tutorial/tutorial.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "support/test_harness.h"
#include <GL/gl_stub_counts.h>
#include "ui/core/gl_2d.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

/* Build the variable-panel view from live app state (mirrors the
 * pre-narrowing NULL-snapshot path) so these tests drive rect/hit by count. */
static void vp_rect(int count, int *px, int *py, int *pw, int *ph) {
    UiVariablePanelView v = ui_app_variable_panel_view_live(count);
    ui_variable_panel_rect(&v, px, py, pw, ph);
}
static int vp_hit_row(int count, int gx, int gy, int *row) {
    UiVariablePanelView v = ui_app_variable_panel_view_live(count);
    return ui_variable_panel_hit_row(&v, gx, gy, row);
}
static UiHit vp_hit_test(int count, int mx, int my) {
    UiVariablePanelView v = ui_app_variable_panel_view_live(count);
    return ui_variable_panel_hit_test(&v, mx, my);
}

/* CPU profile panel view at a fixed anchor (the tests only count GL calls,
 * which the panel emits unconditionally when mode != OFF). */
static UiProfilePanelView pp_view(UiProfilePanelMode mode) {
    UiProfilePanelView v = { 800, 600, mode, 0, 100, 400 };
    return v;
}

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define ASSERT_INT(label, got, expected) do { \
    TEST_ASSERT_INT(&g_harness, label, got, expected); \
} while (0)

#define ASSERT_GL_CALLS(label, counter, min_calls) do { \
    TEST_ASSERT_TRUE(&g_harness, label, gl_stub_counts[counter] >= (min_calls)); \
} while (0)

#define STATUSBAR_TRACE_PATH "build/test_ui_statusbar_trace.txt"

static int statusbar_trace_count_line(const char *exact) {
    FILE *trace = fopen(STATUSBAR_TRACE_PATH, "r");
    char line[128];
    int count = 0;

    if (!trace)
        return 0;
    while (fgets(line, sizeof(line), trace)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (strcmp(line, exact) == 0)
            count++;
    }
    fclose(trace);
    return count;
}

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

static void test_statusbar_command_count_overflow_color(void) {
    UiRenderSnapshot snap;
    int normal_error_colors;
    int normal_primary_colors;
    int overflow_error_colors;
    int overflow_primary_colors;

    printf("Testing statusbar command overflow color...\n");
    glr_ctrl_reset_all();
    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.45f;
    make_test_ui_snapshot(&snap);

    snap.flat_program_overflow_count = 0;
    gl_stub_trace_open(STATUSBAR_TRACE_PATH);
    ui_repl_code_panel_render_with_chrome(&snap, NULL);
    gl_stub_trace_close();
    normal_error_colors = statusbar_trace_count_line(
        "glColor4f 1 0.557 0.494 1");
    normal_primary_colors = statusbar_trace_count_line(
        "glColor4f 0.847 0.847 0.847 1");

    snap.flat_program_overflow_count = MAX_FLAT_COMMANDS + 7;
    gl_stub_trace_open(STATUSBAR_TRACE_PATH);
    ui_repl_code_panel_render_with_chrome(&snap, NULL);
    gl_stub_trace_close();
    overflow_error_colors = statusbar_trace_count_line(
        "glColor4f 1 0.557 0.494 1");
    overflow_primary_colors = statusbar_trace_count_line(
        "glColor4f 0.847 0.847 0.847 1");

    ASSERT_INT("overflow adds one error-text color for command count",
               overflow_error_colors, normal_error_colors + 1);
    ASSERT_INT("overflow replaces one primary command-count color",
               overflow_primary_colors + 1, normal_primary_colors);
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
    UiProfileCollapseMask all_collapsed = ui_profile_panel_toggle_mask(
        0, UI_PROFILE_PANEL_TOGGLE_ALL);

    printf("Testing Profile Panel...\n");
    ASSERT_TRUE("profile tree has collapsible branches", all_collapsed != 0);
    ASSERT_TRUE("profile tree starts with every branch collapsed",
                (UiProfileCollapseMask)
                    ui_state_profile_panel().collapsed_sections ==
                all_collapsed);
    gl_stub_counts_reset();

    { UiProfilePanelView v = pp_view(PROFILE_PANEL_OFF); ui_profile_panel_render(&v); }
    ASSERT_TRUE("profile hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    prof_frame_tick();
    prof_begin(PROF_FRAME_TOTAL);
    prof_end(PROF_FRAME_TOTAL);

    gl_stub_counts_reset();
    { UiProfilePanelView v = pp_view(PROFILE_PANEL_SECTIONS); ui_profile_panel_render(&v); }
    ASSERT_GL_CALLS("profile visible -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("profile visible -> draws text", GL_STUB_glRasterPos2f, 5);
    ASSERT_GL_CALLS("profile visible -> calls glColor4f", GL_STUB_glColor4f, 1);

    /* Histogram mode keeps the same detailed section listing. */
    gl_stub_counts_reset();
    { UiProfilePanelView v = pp_view(PROFILE_PANEL_HISTOGRAM); ui_profile_panel_render(&v); }
    ASSERT_GL_CALLS("profile histogram -> draws detailed text", GL_STUB_glRasterPos2f, 10);
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
    color_picker_start(0, 300);
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
    color_picker_start(0, 300);

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
    color_picker_start(0, 300);
    view = color_picker_view();
    int sv_mx2 = view.rects.sv_x + 10;
    int sv_my2 = ui_state_viewport().window_h - (view.rects.sv_y + 10); // High V
    color_picker_handle_press(sv_mx2, sv_my2);
    color_picker_handle_release();

    /* Test CMD_TESS_COLOR writeback */
    repl_state_document_cmds_mut()[0].type = CMD_TESS_COLOR;
    color_picker_start(0, 300);
    view = color_picker_view();
    color_picker_handle_press(view.rects.hue_x + 5, ui_state_viewport().window_h - (view.rects.hue_y + 10));
    color_picker_handle_release();

    /* Test RGB->HSV branches */
    color_picker_stop();
    repl_state_document_cmds_mut()[0].type = CMD_COLOR3F;
    repl_state_document_cmds_mut()[0].args[0] = 0.0f;
    repl_state_document_cmds_mut()[0].args[1] = 1.0f;
    repl_state_document_cmds_mut()[0].args[2] = 0.0f;
    color_picker_start(0, 300);

    repl_state_document_cmds_mut()[0].args[0] = 0.0f;
    repl_state_document_cmds_mut()[0].args[1] = 0.0f;
    repl_state_document_cmds_mut()[0].args[2] = 1.0f;
    color_picker_start(0, 300);

    /* Open an uneditable command to hit coverage */
    repl_state_document_cmds_mut()[0].type = CMD_BEGIN;
    color_picker_start(0, 300);

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
    color_picker_start(0, 300);
    color_picker_handle_press(0, 0);

    /* Test clicking inside picker bounds but outside slider rects (consumed-no-op) */
    color_picker_start(0, 300);
    view = color_picker_view();
    int inside_mx = view.rects.sv_x + 5;
    int inside_my = ui_state_viewport().window_h - (view.rects.sv_y - 4);
    ColorPickerInputResult res_inside = color_picker_handle_press(inside_mx, inside_my);
    ASSERT_TRUE("inside click is consumed", res_inside.consumed == 1);
    ASSERT_TRUE("inside click does not close", res_inside.closed == 0);
    ASSERT_TRUE("picker stays open after inside click", color_picker_active_line() == 0);

    /* Test writeback short-circuit (mutated command type underneath) */
    color_picker_start(0, 300);
    view = color_picker_view();
    repl_state_document_cmds_mut()[0].type = CMD_BEGIN;
    color_picker_handle_press(view.rects.sv_x + 10, ui_state_viewport().window_h - (view.rects.sv_y + 10));
    color_picker_handle_release();

    /* Test writeback short-circuit (command deleted underneath) */
    color_picker_start(0, 300);
    view = color_picker_view();
    repl_state_document_count_set(0);
    color_picker_handle_press(view.rects.sv_x + 10, ui_state_viewport().window_h - (view.rects.sv_y + 10));
    color_picker_handle_release();

    color_picker_stop();
    ASSERT_TRUE("picker closed", color_picker_active_line() == -1);

    /* --- Palettes ------------------------------------------------------ */
    color_picker_state_reset();   /* back to CP_TAB_BASIC */
    repl_state_document_count_set(1);
    repl_state_document_cmds_mut()[0].type = CMD_COLOR3F;
    repl_state_document_cmds_mut()[0].args[0] = 0.0f;
    repl_state_document_cmds_mut()[0].args[1] = 0.0f;
    repl_state_document_cmds_mut()[0].args[2] = 1.0f;   /* blue */
    repl_state_document_cmds_mut()[0].valid = 1;
    repl_state_document_cmds_mut()[0].has_vars = 0;
    color_picker_start(0, 300);

    view = color_picker_view();
    ASSERT_TRUE("default tab is Basic", view.palette_tab == CP_TAB_BASIC);
    ASSERT_INT("basic swatch count", view.swatch_count, 10);

    /* Click the Harmony tab segment (rightmost third of the strip). */
    int harm_seg_x = view.tab_x + view.tab_w * 5 / 6;
    int tab_my = ui_state_viewport().window_h - (view.tab_y - view.tab_h / 2);
    ColorPickerInputResult tab_res = color_picker_handle_press(harm_seg_x, tab_my);
    ASSERT_TRUE("tab click consumed", tab_res.consumed == 1);
    ASSERT_TRUE("tab click no writeback", tab_res.changed == 0);
    view = color_picker_view();
    ASSERT_TRUE("switched to Harmony tab", view.palette_tab == CP_TAB_HARMONY);
    ASSERT_INT("harmony swatch count", view.swatch_count, 4);
    /* Swatch 0 of the harmony set is the chosen color (current hue/sat/val). */
    {
        float r0, g0, b0;
        color_picker_hsv_to_rgb(view.hue, view.sat, view.val, &r0, &g0, &b0);
        TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "harmony[0].r == chosen", view.swatches[0][0], r0);
        TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "harmony[0].b == chosen", view.swatches[0][2], b0);
    }
    /* Hit-test reports a swatch hit over the first harmony cell. */
    {
        int sw_mx = view.pal_x + view.pal_cell / 2;
        int sw_my = ui_state_viewport().window_h - (view.pal_y - view.pal_cell / 2);
        UiHit hh = ui_color_picker_hit_test_helper(sw_mx, sw_my);
        ASSERT_TRUE("hit-test over swatch", hh.kind == UI_HIT_COLOR_SWATCH);
    }

    /* Switch back to Basic and click the red swatch (index 3); writeback
     * should land red into the document command. */
    color_picker_state_reset();
    color_picker_start(0, 300);
    view = color_picker_view();
    {
        int idx = 3;                       /* CP_BASIC red */
        int c = idx % view.pal_cols, r = idx / view.pal_cols;
        int sw_mx = view.pal_x + c * (view.pal_cell + view.pal_gap) + view.pal_cell / 2;
        int sw_top = view.pal_y - r * (view.pal_cell + view.pal_gap);
        int sw_my = ui_state_viewport().window_h - (sw_top - view.pal_cell / 2);
        ColorPickerInputResult sr = color_picker_handle_press(sw_mx, sw_my);
        ASSERT_TRUE("basic swatch click consumed", sr.consumed == 1);
        const GLCmd *cmd = repl_state_document_cmd_at(0);
        ASSERT_TRUE("red swatch -> r dominant", cmd->args[0] > 0.6f);
        ASSERT_TRUE("red swatch -> g small", cmd->args[1] < 0.4f);
        ASSERT_TRUE("red swatch -> b small", cmd->args[2] < 0.4f);
    }

    /* glClearColor clamps a bright (white) swatch to REPL_CLEAR_COLOR_MAX_V. */
    color_picker_state_reset();
    repl_state_document_cmds_mut()[0].type = CMD_CLEAR_COLOR;
    repl_state_document_cmds_mut()[0].args[3] = 1.0f;
    color_picker_start(0, 300);
    view = color_picker_view();
    {
        int idx = 1;                       /* CP_BASIC white */
        int sw_mx = view.pal_x + idx * (view.pal_cell + view.pal_gap) + view.pal_cell / 2;
        int sw_my = ui_state_viewport().window_h - (view.pal_y - view.pal_cell / 2);
        color_picker_handle_press(sw_mx, sw_my);
        const GLCmd *cmd = repl_state_document_cmd_at(0);
        ASSERT_TRUE("clearcolor swatch clamps r",
                    cmd->args[0] <= REPL_CLEAR_COLOR_MAX_V + 0.001f);
    }
    color_picker_stop();
    color_picker_state_reset();

    /* --- Hex readout + sticky harmony key ------------------------------ */
    repl_state_document_count_set(1);
    repl_state_document_cmds_mut()[0].type = CMD_COLOR3F;
    repl_state_document_cmds_mut()[0].args[0] = 1.0f;   /* pure red */
    repl_state_document_cmds_mut()[0].args[1] = 0.0f;
    repl_state_document_cmds_mut()[0].args[2] = 0.0f;
    repl_state_document_cmds_mut()[0].valid = 1;
    repl_state_document_cmds_mut()[0].has_vars = 0;
    color_picker_start(0, 300);
    view = color_picker_view();
    ASSERT_TRUE("hex readout for pure red", strcmp(view.hex, "#FF0000FF") == 0);

    /* Enter the Harmony tab -> key auto-seeds from the current (red) color. */
    {
        int harm_x = view.tab_x + view.tab_w * 5 / 6;
        int tab_my = ui_state_viewport().window_h - (view.tab_y - view.tab_h / 2);
        color_picker_handle_press(harm_x, tab_my);
    }
    view = color_picker_view();
    ASSERT_TRUE("harmony key seeded", view.key_set == 1);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "key swatch == red r", view.swatches[0][0], 1.0f);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "key swatch == red g", view.swatches[0][1], 0.0f);

    /* Drag the hue bar to change the current color; the key (swatch 0) must
     * NOT move — the tetrad stays anchored. */
    {
        int hue_mx = view.rects.hue_x + 5;
        int hue_my = ui_state_viewport().window_h - (view.rects.hue_y + 70);
        color_picker_handle_press(hue_mx, hue_my);
        color_picker_handle_release();
    }
    view = color_picker_view();
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "key unchanged after hue drag", view.swatches[0][0], 1.0f);

    /* Close and reopen on a *different* command color: the key persists
     * across instances (tab stays Harmony, key not re-seeded). */
    color_picker_stop();
    repl_state_document_cmds_mut()[0].args[0] = 0.0f;   /* green */
    repl_state_document_cmds_mut()[0].args[1] = 1.0f;
    repl_state_document_cmds_mut()[0].args[2] = 0.0f;
    color_picker_start(0, 300);
    view = color_picker_view();
    ASSERT_TRUE("tab persisted as Harmony", view.palette_tab == CP_TAB_HARMONY);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "key carried across instances", view.swatches[0][0], 1.0f);

    /* Hit-test reports the Set-key button, and pressing it re-anchors the
     * key to the current (green) color. */
    {
        int kb_mx = view.key_btn_x + view.key_btn_w / 2;
        int kb_my = ui_state_viewport().window_h - (view.key_btn_y - view.key_btn_h / 2);
        UiHit kh = ui_color_picker_hit_test_helper(kb_mx, kb_my);
        ASSERT_TRUE("hit-test over set-key button", kh.kind == UI_HIT_COLOR_SWATCH);
        ColorPickerInputResult kr = color_picker_handle_press(kb_mx, kb_my);
        ASSERT_TRUE("set-key consumed", kr.consumed == 1);
        ASSERT_TRUE("set-key no writeback", kr.changed == 0);
    }
    view = color_picker_view();
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "key re-anchored to green g", view.swatches[0][1], 1.0f);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "key re-anchored to green r", view.swatches[0][0], 0.0f);

    color_picker_stop();
    color_picker_state_reset();

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
                .has_alpha = 0,
            },
        };
        ui_color_picker_render_swatch(&t, 100, 100, color_picker_active_line());
    }
    ASSERT_GL_CALLS("swatch render -> draws quads", GL_STUB_glBegin, 1);
}

/* Regression: when the color picker edits the line the cursor is parked
 * on, the editor input buffer is a live shadow of that line. The
 * writeback used to update the command store + editor buffer text but
 * not the shadow, so the code panel kept showing the pre-edit color and
 * a later navigation re-committed the stale input over the picker's
 * change (commit_before_navigation), silently reverting it. */
static void test_color_picker_active_line_no_revert(void) {
    printf("Testing Color Picker active-line writeback...\n");

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(800, 600);
    repl_load_example(0); /* Lit cube — has glColor3f(1, 1, 1); */

    int ci = -1;
    for (int i = 0; i < repl_state_document_count(); i++)
        if (repl_state_document_cmd_at(i)->type == CMD_COLOR3F) { ci = i; break; }
    ASSERT_TRUE("found a glColor3f line", ci >= 0);

    /* Park the cursor on the color line so its text is loaded into the
     * input buffer (the shadow). This is the precondition for the bug. */
    editor_navigate_to_line(ci);
    ASSERT_INT("cursor parked on color line", editor_state_edit_line(), ci);

    color_picker_start(ci, 300);
    ColorPickerView view = color_picker_view();
    int vp_h = ui_state_viewport().window_h;
    int mx = view.rects.sv_x + 40;
    int my = vp_h - (view.rects.sv_y + 100);
    color_picker_handle_press(mx, my);
    color_picker_handle_motion(mx + 5, my - 5);
    color_picker_handle_release();

    const GLCmd *c = repl_state_document_cmd_at(ci);
    float wr = c->args[0], wg = c->args[1], wb = c->args[2];
    ASSERT_TRUE("picker actually changed the color",
                wr != 1.0f || wg != 1.0f || wb != 1.0f);
    /* The input shadow must track the writeback, not stay at (1,1,1). */
    ASSERT_TRUE("input buffer shadow refreshed (not stale)",
                strstr(editor_state_input().input, "1, 1, 1") == NULL);

    /* Click away: navigate elsewhere, which runs commit_before_navigation.
     * The picker's color must survive (not be reverted by a stale input). */
    color_picker_stop();
    editor_navigate_to_line(ci + 2);
    const GLCmd *c2 = repl_state_document_cmd_at(ci);
    ASSERT_TRUE("color persists after navigating away (r)", c2->args[0] == wr);
    ASSERT_TRUE("color persists after navigating away (g)", c2->args[1] == wg);
    ASSERT_TRUE("color persists after navigating away (b)", c2->args[2] == wb);
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

    variable_panel_state_mut()->visible = 0;
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); UiVariablePanelView v = ui_app_variable_panel_view(&s); ui_variable_panel_render(&v); }
    ASSERT_TRUE("var panel hidden -> no GL calls", gl_stub_counts[GL_STUB_glBegin] == 0);

    variable_panel_state_mut()->visible = 1;
    g_num_predef_vars_mut = 1;
    strcpy(g_predef_vars_mut[0].name, "x");
    g_predef_vars_mut[0].value = 1.0f;

    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); UiVariablePanelView v = ui_app_variable_panel_view(&s); ui_variable_panel_render(&v); }
    ASSERT_GL_CALLS("var panel visible -> draws quads", GL_STUB_glBegin, 1);
    ASSERT_GL_CALLS("var panel visible -> draws text", GL_STUB_glRasterPos2f, 2);
    ASSERT_GL_CALLS("var panel visible -> calls glColor4f", GL_STUB_glColor4f, 1);

    /* Test hit testing */
    int row = -1;
    ui_state_viewport_set_size(800, 600);
    int px, py, pw, ph;
    vp_rect(1, &px, &py, &pw, &ph);
    ASSERT_TRUE("hit test in panel",
                vp_hit_row(1, px + 10,
                           ui_state_viewport().window_h - (py + 10), &row));
    ASSERT_TRUE("hit test outside panel",
                !vp_hit_row(1, 0, 0, &row));
}

static void test_ui_clamp_panel_y_honors_edge_pad(void) {
    ASSERT_INT("4px edge pad clamps above status inset",
               ui_clamp_panel_y(0, 120, 20, 0, 0,
                                STATUSBAR_H, 4),
               STATUSBAR_H + 4);
    ASSERT_INT("custom edge pad is preserved",
               ui_clamp_panel_y(0, 120, 20, 0, 0,
                                STATUSBAR_H, 12),
               STATUSBAR_H + 12);
    ASSERT_INT("top layout overflow parks at scene top with pad",
               ui_clamp_panel_y(0, 40, 30, 100, 1,
                                STATUSBAR_H, 8),
               40 - 30 - 8);
    ASSERT_INT("non-top overflow parks above bottom inset",
               ui_clamp_panel_y(0, 40, 30, 100, 0,
                                STATUSBAR_H, 8),
               STATUSBAR_H + 8);
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
    ASSERT_TRUE("menu hit", ui_menu_bar_hit_test(20, 10).kind == UI_HIT_MENU_BUTTON);
    /* Pins are on the right side of the code panel.
     * With CP LEFT and frac 0.5, cp_w = 400.
     */
    ASSERT_TRUE("pin hit", ui_menu_bar_hit_test(380, 10).kind == UI_HIT_PIN_BUTTON);

    /* Test dropdown */
    ui_menu_bar_set_open_menu(0, 0.0f); // File menu
    ASSERT_TRUE("dropdown open", ui_menu_bar_menu_dropdown_is_open());

    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_menu_bar_render_example_dropdown(&s); }
    ASSERT_GL_CALLS("dropdown render -> draws quads", GL_STUB_glBegin, 1);

    /* Dropdown item hit */
    ASSERT_TRUE("dropdown item hit", ui_menu_bar_hit_test(20, 100).kind == UI_HIT_MENU_ITEM);

    /* Test config menu */
    ui_menu_bar_open_config(0.0f);
    ASSERT_TRUE("config menu open", ui_menu_bar_open_menu_id() == GLR_MENU_CONFIG);

    /* Test search overlay */
    editor_state_search_mut()->active = 1;
    gl_stub_counts_reset();
    { UiRenderSnapshot s; make_test_ui_snapshot(&s); ui_menu_bar_render_search_overlay(&s); }
    ASSERT_GL_CALLS("search overlay -> draws quads", GL_STUB_glBegin, 1);

    ui_menu_bar_close();
    ASSERT_TRUE("menu closed", !ui_menu_bar_menu_dropdown_is_open());
}

/* Phase E commit 28: ui_panels_hit_test classifies pointer location
 * as a UiHit without mutating state.
 *
 * Layout-agnostic: every probe point is derived from the live
 * ui_layout_*_rect() geometry (the code-text row is found by scanning,
 * not hand-computed), and the layout + panel fraction are pinned
 * explicitly. The test therefore does not depend on
 * CFG_DEFAULT_CODE_PANEL_LAYOUT / CFG_DEFAULT_PANEL_FRAC and is run
 * across all three split layouts so a defaults change can't silently
 * shift the regions out from under it. */
static void test_ui_panels_hit_test(void) {
    const int layouts[3] = {
        CODE_PANEL_LAYOUT_TOP,
        CODE_PANEL_LAYOUT_BOTTOM,
        CODE_PANEL_LAYOUT_LEFT,
    };

    for (int li = 0; li < 3; li++) {
        const char *tag = layouts[li] == CODE_PANEL_LAYOUT_TOP    ? " [top]"
                        : layouts[li] == CODE_PANEL_LAYOUT_BOTTOM ? " [bottom]"
                                                                  : " [left]";
        char lbl[80];
        int linenum_w = 4 * FONT_W;
        int idx_col_w = glr_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
        int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
        int sx, sy, sw, sh;
        int cp_x, cp_y, cp_w, cp_h;
        int win_h;
        int row_my = -1;
        UiReplCodePanelLayout layout;
        UiHit h;

        glr_ctrl_reset_all();
        glr_state_presentation_mut()->code_panel_layout = layouts[li];
        glr_ctrl_sync_ui_chrome();
        ui_state_viewport_set_size(800, 600);
        ui_state_code_panel_mut()->panel_frac = 0.45f;
        editor_scroll_follow_cursor_set(0);
        editor_feed_line("glBegin(GL_POINTS);");

        win_h = ui_state_viewport().window_h;
        ui_layout_scene_rect(&sx, &sy, &sw, &sh);
        ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
        build_test_code_panel_layout(&layout, cp_w, text_x, cp_h);
        editor_scroll_set(layout.header_rows);     /* skip chrome */

        /* Scene: centre of the scene rect (GL -> GLUT my). */
        snprintf(lbl, sizeof lbl, "scene hit kind%s", tag);
        h = ui_panels_hit_test_current_snapshot(sx + sw / 2,
                                                win_h - (sy + sh / 2), 0);
        ASSERT_TRUE(lbl, h.kind == UI_HIT_SCENE);

        /* Code text: first my (top-down) at the text column that the
         * panel classifies as CODE_TEXT — found by scan so no row-y
         * math leaks layout/scroll/chrome assumptions in. */
        {
            int mx = cp_x + text_x + 2 * FONT_W;
            for (int my = 0; my < win_h; my++) {
                h = ui_panels_hit_test_current_snapshot(mx, my, 0);
                if (h.kind == UI_HIT_CODE_TEXT) { row_my = my; break; }
            }
        }
        snprintf(lbl, sizeof lbl, "code-panel text hit kind%s", tag);
        ASSERT_TRUE(lbl, row_my >= 0);

        if (row_my >= 0) {
            /* Gutter: same row, mx left of text_x. */
            snprintf(lbl, sizeof lbl, "code-panel gutter hit kind%s", tag);
            h = ui_panels_hit_test_current_snapshot(cp_x + CODE_MARGIN_X + 1,
                                                    row_my, 0);
            ASSERT_TRUE(lbl, h.kind == UI_HIT_CODE_GUTTER);

            /* Help overlay takes priority over the same panel point. */
            ui_state_help_mut()->visible = 1;
            snprintf(lbl, sizeof lbl, "help overlay takes priority%s", tag);
            h = ui_panels_hit_test_current_snapshot(cp_x + text_x + 2 * FONT_W,
                                                    row_my, 0);
            ASSERT_TRUE(lbl, h.kind == UI_HIT_HELP_PANEL);
            ui_state_help_mut()->visible = 0;
        }

        /* Statusbar strip (bottom STATUSBAR_H of the panel) consumes
         * clicks as panel chrome. */
        snprintf(lbl, sizeof lbl, "statusbar consumes clicks%s", tag);
        h = ui_panels_hit_test_current_snapshot(
                cp_x + cp_w / 2, win_h - (cp_y + STATUSBAR_H / 2), 0);
        ASSERT_TRUE(lbl, h.kind == UI_HIT_CODE_PANEL_CHROME);

        if (cp_w >= 700) {
            UiRenderSnapshot status_snap;
            int saw_clear = 0;
            int saw_copy = 0;
            int saw_cut = 0;
            int saw_paste = 0;
            int saw_undo = 0;
            int saw_redo = 0;
            int status_my = win_h - (cp_y + STATUSBAR_H / 2);
            int mx;

            make_test_ui_snapshot(&status_snap);
            for (mx = cp_x; mx < cp_x + cp_w; mx++) {
                h = ui_panels_hit_test(&status_snap, mx, status_my, 0);
                if (h.kind == UI_HIT_CODE_CLEAR_ALL) saw_clear = 1;
                if (h.kind == UI_HIT_CODE_COPY)      saw_copy = 1;
                if (h.kind == UI_HIT_CODE_CUT)       saw_cut = 1;
                if (h.kind == UI_HIT_CODE_PASTE)     saw_paste = 1;
                if (h.kind == UI_HIT_CODE_UNDO)      saw_undo = 1;
                if (h.kind == UI_HIT_CODE_REDO)      saw_redo = 1;
            }
            snprintf(lbl, sizeof lbl, "statusbar trash hit kind%s", tag);
            ASSERT_TRUE(lbl, saw_clear);
            snprintf(lbl, sizeof lbl, "statusbar copy hit kind%s", tag);
            ASSERT_TRUE(lbl, saw_copy);
            snprintf(lbl, sizeof lbl, "statusbar cut hit kind%s", tag);
            ASSERT_TRUE(lbl, saw_cut);
            snprintf(lbl, sizeof lbl, "statusbar paste hit kind%s", tag);
            ASSERT_TRUE(lbl, saw_paste);
            snprintf(lbl, sizeof lbl, "statusbar undo hit kind%s", tag);
            ASSERT_TRUE(lbl, saw_undo);
            snprintf(lbl, sizeof lbl, "statusbar redo hit kind%s", tag);
            ASSERT_TRUE(lbl, saw_redo);
        }
    }
}

/* Phase E commit 29: hit-test entry points for ui_menu_bar,
 * ui_color_picker, ui_variable_panel return passive UiHit
 * classifications without mutating state. ui_panels_hit_test
 * dispatches to them in priority order. */
static void test_ui_menu_bar_hit_test(void) {
    glr_ctrl_reset_all();
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
    glr_ctrl_reset_all();
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
    color_picker_start(0, 300);

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
    color_picker_stop();
}

static void test_ui_variable_panel_hit_test(void) {
    glr_ctrl_reset_all();
    ui_state_viewport_set_size(800, 600);

    /* Panel hidden -> always miss. */
    variable_panel_state_mut()->visible = 0;
    UiHit h_off = vp_hit_test(0, 700, 100);
    ASSERT_TRUE("hidden panel -> NONE", h_off.kind == UI_HIT_NONE);

    /* Visible panel with one declared variable. */
    variable_panel_state_mut()->visible = 1;
    g_num_predef_vars_mut = 1;
    strcpy(g_predef_vars_mut[0].name, "x");
    g_predef_vars_mut[0].value = 1.0f;

    int px, py, pw, ph;
    vp_rect(1, &px, &py, &pw, &ph);
    int my = ui_state_viewport().window_h - (py + 10);

    UiHit h_row = vp_hit_test(1, px + 10, my);
    ASSERT_TRUE("row hit kind", h_row.kind == UI_HIT_VARIABLE_SLIDER);
    ASSERT_TRUE("row item_idx populated",
                h_row.item_idx >= 0 && h_row.item_idx < g_num_predef_vars);

    /* Click outside the panel rect -> NONE. */
    UiHit h_out = vp_hit_test(1, 0, 0);
    ASSERT_TRUE("outside panel -> NONE", h_out.kind == UI_HIT_NONE);
}

/* Verify ui_panels_hit_test routes to the floating-overlay
 * hit-testers in priority order before falling through to the
 * code panel and scene. */
static void test_ui_panels_hit_test_dispatch(void) {
    glr_ctrl_reset_all();
    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.5f;

    /* Variable panel should win over the scene-region fallback when
     * a click lands on its rect. */
    variable_panel_state_mut()->visible = 1;
    g_num_predef_vars_mut = 1;
    strcpy(g_predef_vars_mut[0].name, "x");
    g_predef_vars_mut[0].value = 1.0f;
    int px, py, pw, ph;
    vp_rect(1, &px, &py, &pw, &ph);
    int my_var = ui_state_viewport().window_h - (py + 10);
    UiHit h_var = ui_panels_hit_test_current_snapshot(px + 10, my_var, 1);
    ASSERT_TRUE("var panel routed via panels_hit_test",
                h_var.kind == UI_HIT_VARIABLE_SLIDER);

    /* The open recent-messages list grows upward from the bell and overlaps
     * the panel's lower-right rows. Because it renders after the variable
     * panel, the overlay should win that shared hit area. */
    ui_state_status_set("seed");
    ui_state_status_history_set_open(1);
    {
        UiRenderSnapshot snap;
        int bx, by, bw, bh;
        int overlap_mx;
        int overlap_my;
        UiHit h_overlap_panel;
        UiHit h_overlap_status;

        make_test_ui_snapshot(&snap);
        ASSERT_TRUE("status history button exists",
                    ui_panels_status_history_button_rect(&snap, &bx, &by, &bw, &bh));
        (void)by;
        (void)bh;

        overlap_mx = bx + bw / 2;
        overlap_my = ui_state_viewport().window_h - (py + 10);

        h_overlap_panel = vp_hit_test(1, overlap_mx, overlap_my);
        ASSERT_TRUE("overlap probe reaches variable row",
                    h_overlap_panel.kind == UI_HIT_VARIABLE_SLIDER);

        h_overlap_status = ui_panels_hit_test(&snap, overlap_mx, overlap_my, 1);
        ASSERT_TRUE("status history overlay wins over variable panel",
                    h_overlap_status.kind == UI_HIT_STATUS_HISTORY);
        ASSERT_TRUE("overlap click targets open list body",
                    h_overlap_status.item_idx == 0);
    }
    ui_state_status_history_set_open(0);
    variable_panel_state_mut()->visible = 0;

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
    color_picker_start(0, 300);

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
    color_picker_stop();
}

/* J2.1: ui_panels_hit_test emits UI_HIT_PANEL_DIVIDER for the
 * code-panel splitter strip. */
static void test_ui_panels_hit_test_panel_divider(void) {
    glr_ctrl_reset_all();
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
 * if it cares (the per-test glr_ctrl_reset_all() calls do this). */
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
    glr_ctrl_reset_all();
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
    glr_ctrl_reset_all();
    ui_state_viewport_set_size(800, 600);

    /* Commit one line so insert mode at edit_line=0 produces an
     * insertion ghost row above it. */
    editor_feed_line("glBegin(GL_POINTS);");
    editor_state_edit_line_set(0);
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
               h.line_idx, editor_state_edit_line());

    editor_insert_mode_set(0);
}

static void test_ui_panels_hit_test_overwrite_row_kind(void) {
    glr_ctrl_reset_all();
    ui_state_viewport_set_size(800, 600);

    editor_feed_line("glBegin(GL_POINTS);");
    editor_state_edit_line_set(0);
    editor_insert_mode_set(0);

    int mx, my;
    code_panel_first_row_text_click(&mx, &my);

    UiHit h = ui_panels_hit_test_current_snapshot(mx, my, 0);
    ASSERT_TRUE("overwrite active row hit kind",
                h.kind == UI_HIT_CODE_TEXT);
}

/* Scan the text column for the first on-screen row that hit-tests to
 * UI_HIT_CODE_TEXT for `want_line`. When `want_char_neg1` is set the
 * row must also report char_idx == -1 (the virtual-row marker, which
 * distinguishes a virtual row from the real text row of the same
 * source line). Scanning instead of hand-computing the target row's y
 * makes the assertion independent of chrome height and the scroll
 * clamp, so it holds whether code-focus is on or off. */
static int find_code_panel_text_row(int want_line, int want_char_neg1,
                                     UiHit *out, int *out_mx, int *out_my) {
    int cp_x, cp_y, cp_w, cp_h;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = glr_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int mx;

    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    mx = cp_x + text_x + 2 * FONT_W;          /* mid-text: crosses every row */
    for (int my = 0; my < ui_state_viewport().window_h; my++) {
        UiHit h = ui_panels_hit_test_current_snapshot(mx, my, 0);
        if (h.kind == UI_HIT_CODE_TEXT && h.line_idx == want_line &&
            (!want_char_neg1 || h.char_idx == -1)) {
            if (out)    *out = h;
            if (out_mx) *out_mx = mx;
            if (out_my) *out_my = my;
            return 1;
        }
    }
    return 0;
}

static void test_ui_panels_hit_test_trailing_blank_row_kind(void) {
    /* Both code-focus modes, pinned explicitly: the trailing blank row
     * must resolve to UI_HIT_CODE_TEXT @ document_count regardless of
     * CFG_DEFAULT_CODE_FOCUS. b472592 flipped that default and broke
     * this — the row's y was hand-computed and the scroll clamps
     * differently once focus collapses the header chrome. */
    for (int cf = 0; cf <= 1; cf++) {
        const char *mode = cf ? " [focus]" : " [full]";
        char lbl[96];
        int linenum_w = 4 * FONT_W;
        int idx_col_w = glr_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
        int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
        int cp_x, cp_y, cp_w, cp_h;
        UiReplCodePanelLayout layout;

        glr_ctrl_reset_all();
        glr_state_presentation_mut()->code_focus = cf; glr_ctrl_sync_ui_chrome();
        ui_state_viewport_set_size(800, 600);
        editor_scroll_follow_cursor_set(0);

        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glEnd();");
        editor_state_edit_line_set(0);
        editor_insert_mode_set(0);

        ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
        build_test_code_panel_layout(&layout, cp_w, text_x, cp_h);
        editor_scroll_set(layout.header_rows);   /* skip chrome (0 in focus) */

        snprintf(lbl, sizeof lbl,
                 "trailing blank row -> CODE_TEXT @ document_count%s", mode);
        ASSERT_TRUE(lbl,
            find_code_panel_text_row(repl_state_document_count(), 0,
                                     NULL, NULL, NULL));
    }
}

static void test_ui_panels_hit_test_virtual_row_routes_to_source(void) {
    /* Both code-focus modes, pinned explicitly (same b472592 hazard as
     * the trailing-blank-row test above). */
    for (int cf = 0; cf <= 1; cf++) {
        const char *mode = cf ? " [focus]" : " [full]";
        char lbl[96];
        int linenum_w = 4 * FONT_W;
        int idx_col_w = glr_state_presentation().show_vertex_indices ? (6 * FONT_W) : 0;
        int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
        int cp_x, cp_y, cp_w, cp_h;
        UiReplCodePanelLayout layout;
        UiHit h;
        int mx = 0, my = 0;
        int found;

        glr_ctrl_reset_all();
        glr_state_presentation_mut()->code_focus = cf; glr_ctrl_sync_ui_chrome();
        ui_state_viewport_set_size(800, 600);
        editor_scroll_follow_cursor_set(0);

        editor_feed_line("glBegin(GL_POINTS);");
        editor_feed_line("glEnd();");
        editor_state_virtual_lines_append(0, VIRTUAL_STYLE_REPLAY_EVAL,
                                          "replay eval", "");
        editor_state_edit_line_set(1);

        ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
        build_test_code_panel_layout(&layout, cp_w, text_x, cp_h);
        editor_scroll_set(layout.header_rows);   /* skip chrome (0 in focus) */

        /* line 0 + char_idx == -1 uniquely identifies the virtual row;
         * the real glBegin row is line 0 with char_idx >= 0. This one
         * predicate covers the old "hit kind", "rewrites to source
         * line", and "clears char index" assertions. */
        found = find_code_panel_text_row(0, 1, &h, &mx, &my);
        snprintf(lbl, sizeof lbl,
                 "virtual row -> CODE_TEXT, src line 0, char -1%s", mode);
        ASSERT_TRUE(lbl, found);

        if (found) {
            glr_ctrl_router_handle_code_panel_hit(h, mx, my);
            snprintf(lbl, sizeof lbl,
                     "virtual row click navigates to source line%s", mode);
            ASSERT_TRUE(lbl, editor_state_edit_line() == 0);
        }
    }
}

/* Regression: glVertex2f commands should generate gutter vertex-index labels
 * (v0, v1, ...) when show_vertex_indices is on, just like glVertex3f. */
static void test_vertex2f_gutter_labels(void) {
    printf("Testing glVertex2f gutter labels...\n");

    /* Use a wide viewport (4000px) so that the available character width
     * (>165 chars) stays above every header line in both the idx_col_w=0 and
     * idx_col_w=54 states.  Toggling show_vertex_indices then causes no row
     * wrapping changes, so the only glRasterPos2f delta is the "v0" label. */
    glr_ctrl_reset_all();
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
    editor_state_edit_line_set(2);

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
    glr_ctrl_reset_all();
    ui_state_viewport_set_size(4000, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.4f;

    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(1, 2, 0);");
    editor_feed_line("glEnd();");
    editor_state_edit_line_set(2);

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

static void test_vertex_gutter_labels_follow_cursor_begin_block(void) {
    UiRenderSnapshot snap;
    char label[8];

    printf("Testing cursor-scoped vertex gutter labels...\n");

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(4000, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_state_presentation_mut()->show_vertex_indices = 1;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.4f;

    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glEnd();");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glEnd();");

    editor_state_edit_line_set(4);
    ui_repl_code_panel_invalidate_row_cache_for_test();
    make_test_ui_snapshot(&snap);
    ui_repl_code_panel_render_with_chrome(&snap, NULL);
    ASSERT_TRUE("first block vertex row found",
                ui_repl_code_panel_row_aux_label_for_test(1, label));
    ASSERT_TRUE("first block vertex not labelled while cursor is in second block",
                label[0] == '\0');
    ASSERT_TRUE("second block vertex row found",
                ui_repl_code_panel_row_aux_label_for_test(4, label));
    ASSERT_TRUE("second block vertex labelled",
                strcmp(label, "v0") == 0);

    editor_state_edit_line_set(2);
    ui_repl_code_panel_invalidate_row_cache_for_test();
    make_test_ui_snapshot(&snap);
    ui_repl_code_panel_render_with_chrome(&snap, NULL);
    ASSERT_TRUE("first block vertex row found at glEnd cursor",
                ui_repl_code_panel_row_aux_label_for_test(1, label));
    ASSERT_TRUE("first block vertex labelled when cursor is on its glEnd",
                strcmp(label, "v0") == 0);
    ASSERT_TRUE("second block vertex row found at glEnd cursor",
                ui_repl_code_panel_row_aux_label_for_test(4, label));
    ASSERT_TRUE("second block vertex not labelled when cursor is in first block",
                label[0] == '\0');
}

static void test_statusbar_action_tooltips(void) {
    static const int kinds[] = {
        UI_HIT_CODE_UNDO,
        UI_HIT_CODE_REDO,
        UI_HIT_CODE_COPY,
        UI_HIT_CODE_CUT,
        UI_HIT_CODE_PASTE,
        UI_HIT_CODE_CLEAR_ALL,
        UI_HIT_CODE_FOCUS_TOGGLE,
        UI_HIT_HELP_TOGGLE,
    };
    static const char *const names[] = {
        "undo", "redo", "copy", "cut", "paste", "clear", "focus", "help",
    };
    UiRenderSnapshot snap;
    int hit_x[sizeof(kinds) / sizeof(kinds[0])];
    int cp_x, cp_y, cp_w;
    int status_my;
    int i;
    int mx;
    unsigned long long base_raster_calls;

    printf("Testing statusbar action tooltips...\n");

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(2000, 400);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.5f;
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glEnd();");
    make_test_ui_snapshot(&snap);
    ui_repl_code_panel_invalidate_row_cache_for_test();
    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, NULL);
    status_my = snap.viewport.window_h - (cp_y + STATUSBAR_H / 2);

    for (i = 0; i < (int)(sizeof(hit_x) / sizeof(hit_x[0])); i++)
        hit_x[i] = -1;
    for (mx = cp_x; mx < cp_x + cp_w; mx++) {
        UiHit hit = ui_repl_code_panel_hit_test(&snap, mx, status_my);
        for (i = 0; i < (int)(sizeof(kinds) / sizeof(kinds[0])); i++) {
            if (hit.kind == kinds[i] && hit_x[i] < 0)
                hit_x[i] = mx;
        }
    }

    for (i = 0; i < (int)(sizeof(kinds) / sizeof(kinds[0])); i++) {
        char label[80];
        snprintf(label, sizeof label, "%s statusbar icon is hittable", names[i]);
        ASSERT_TRUE(label, hit_x[i] >= 0);
    }

    /* A non-action statusbar pixel is the no-tooltip control render. */
    snap.pointer.mouse_x = cp_x + CODE_MARGIN_X;
    snap.pointer.mouse_y = status_my;
    ASSERT_INT("tooltip control point is inert statusbar chrome",
               ui_repl_code_panel_hit_test(&snap,
                                            snap.pointer.mouse_x,
                                            snap.pointer.mouse_y).kind,
               UI_HIT_CODE_PANEL_CHROME);
    gl_stub_counts_reset();
    ui_repl_code_panel_render_with_chrome(&snap, NULL);
    base_raster_calls = gl_stub_counts[GL_STUB_glRasterPos2f];

    /* Each icon hover contributes exactly one tooltip text draw. The
     * assertion uses > rather than == so bitmap-font implementation
     * details cannot make the test brittle. */
    for (i = 0; i < (int)(sizeof(kinds) / sizeof(kinds[0])); i++) {
        char label[80];
        snap.pointer.mouse_x = hit_x[i];
        snap.pointer.mouse_y = status_my;
        gl_stub_counts_reset();
        ui_repl_code_panel_render_with_chrome(&snap, NULL);
        snprintf(label, sizeof label, "%s hover draws tooltip text", names[i]);
        ASSERT_TRUE(label,
                    gl_stub_counts[GL_STUB_glRasterPos2f] > base_raster_calls);
    }
}

static void test_tutorial_fade_math_uses_snapshot_view(void) {
    UiRenderSnapshot s;
    const char *line;
    int line_idx;
    int line_len;

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.4f;

    tutorial_start(0);
    make_test_ui_snapshot(&s);

    line_idx = s.tutorial_fade.fade_line_idx;
    line = editor_buffer_view_line(s.editor_buffer, line_idx);
    line_len = line ? (int)strlen(line) : 0;

    ASSERT_INT("snapshot fade line is first row", line_idx, 0);
    ASSERT_TRUE("snapshot fade line text present", line_len > 0);
    ASSERT_TRUE("snapshot fade active just after start",
                tutorial_fade_line_active(&s.tutorial_fade, line_idx,
                                          s.tutorial_fade.fade_start_t + 0.01f));
    ASSERT_INT("snapshot fade front starts at first char",
               tutorial_fade_front(&s.tutorial_fade, line_idx, line_len,
                                   s.tutorial_fade.fade_start_t),
               0);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "snapshot char zero starts transparent",
                              tutorial_fade_alpha(&s.tutorial_fade, line_idx, 0,
                                                  line_len,
                                                  s.tutorial_fade.fade_start_t),
                              0.0f);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "snapshot last char finishes opaque",
                              tutorial_fade_alpha(&s.tutorial_fade, line_idx,
                                                  line_len - 1, line_len,
                                                  s.tutorial_fade.fade_start_t +
                                                      s.tutorial_fade.fade_duration),
                              1.0f);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "snapshot settle starts at white",
                              tutorial_fade_settle(&s.tutorial_fade, line_idx, 0,
                                                   line_len,
                                                   s.tutorial_fade.fade_start_t),
                              0.0f);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "snapshot last char settles to base",
                              tutorial_fade_settle(&s.tutorial_fade, line_idx,
                                                   line_len - 1, line_len,
                                                   s.tutorial_fade.fade_start_t +
                                                       s.tutorial_fade.fade_duration),
                              1.0f);
    ASSERT_TRUE("snapshot fade stops after duration",
                !tutorial_fade_line_active(&s.tutorial_fade, line_idx,
                                           s.tutorial_fade.fade_start_t +
                                               s.tutorial_fade.fade_duration));
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

    glr_ctrl_reset_all();
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
        /* Sample mid-fade so the reveal head, the white→base settle
         * window, and any fully-settled prefix all contribute
         * per-char segments. Sampling at +0.01s would only show the
         * actively-revealing head char on the slower default
         * fade_duration. */
        s.anim_time = tutorial.fade_start_t + tutorial.fade_duration * 0.5f;
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

    glr_ctrl_reset_all();
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
        s.anim_time = tutorial.fade_start_t + tutorial.fade_duration * 0.5f;
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

/* Audit #49 (Tier C, commit f9abd33) regression: ui_repl_code_panel_hit_test
 * caches the row builder from the prior ui_repl_code_panel_render_with_chrome
 * call, keyed on the snapshot pointer. A stale cache (or a cache that
 * doesn't refresh after the document mutates) would silently miss-route
 * clicks. Sweep clicks across the panel after render(A), record which
 * source lines are hit-testable, then add a new line + re-render + re-sweep
 * — the new line must be hit-testable (proves the cache refreshed). */
static int hit_lines_bitmap(const UiRenderSnapshot *snap, int mx) {
    int found_mask = 0;
    for (int my = 0; my < ui_state_viewport().window_h; my++) {
        UiHit h = ui_repl_code_panel_hit_test(snap, mx, my);
        if (h.kind == UI_HIT_CODE_TEXT && h.line_idx >= 0 && h.line_idx < 16)
            found_mask |= (1 << h.line_idx);
    }
    return found_mask;
}

static void test_render_then_hit_test_row_consistency(void) {
    UiRenderSnapshot snap;
    int mx, my;
    int mask_a, mask_b;

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.45f;
    editor_scroll_follow_cursor_set(0);
    /* Force the row builder cache empty so render(A) is the first call
     * that populates it — without this, prior tests may leave
     * g_builder_cache.snap pointing at a stale stack address that
     * happens to mismatch &snap, causing every hit-test below to take
     * the rebuild path instead of exercising the cache. */
    ui_repl_code_panel_invalidate_row_cache_for_test();

    /* State A: two short source lines. Few rows means the cache from A
     * leaves later row indices unreachable if it doesn't refresh on
     * render B. */
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glEnd();");

    /* Use the existing helper to find a known text-row click; that sets
     * editor_scroll to skip chrome and returns mx in the text column. */
    code_panel_first_row_text_click(&mx, &my);
    (void)my;

    /* Render populates the row builder cache for &snap. */
    make_test_ui_snapshot(&snap);
    ui_repl_code_panel_render_with_chrome(&snap, NULL);

    /* Hit-test sweeps reach the same builder via the cache. Both source
     * lines (0, 1) must appear. */
    mask_a = hit_lines_bitmap(&snap, mx);
    ASSERT_TRUE("render-then-hit-test: line 0 hit-testable after A",
                (mask_a & (1 << 0)) != 0);
    ASSERT_TRUE("render-then-hit-test: line 1 hit-testable after A",
                (mask_a & (1 << 1)) != 0);

    /* State B: insert several new source lines between the existing
     * ones so the total row count grows by enough that a stale cache
     * (row_count frozen at A's value) would miss the later lines. */
    editor_navigate_to_line(1);
    editor_feed_line("glColor3f(0.2, 0.4, 0.6);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glVertex3f(0, 0, 1);");
    editor_feed_line("glVertex3f(1, 1, 0);");
    editor_feed_line("glVertex3f(0, 1, 1);");
    editor_feed_line("glColor3f(0.9, 0.1, 0.1);");

    /* Reuse the same `snap` variable so the cache compares the same
     * pointer — render must overwrite the cache with B's larger row
     * set or hit-test will miss the later rows. */
    make_test_ui_snapshot(&snap);
    ui_repl_code_panel_render_with_chrome(&snap, NULL);

    /* The newly-inserted source lines occupy indices 1..8. If the
     * cache is stale (row_count frozen at A's count, which was 2
     * source + chrome), hit-test won't walk rows past that count and
     * the later lines (say line 8) will not appear. */
    mask_b = hit_lines_bitmap(&snap, mx);
    ASSERT_TRUE("render-then-hit-test: last new line hit-testable after B",
                (mask_b & (1 << 8)) != 0);
    ASSERT_TRUE("render-then-hit-test: middle new line hit-testable after B",
                (mask_b & (1 << 5)) != 0);
}

/* Audit #6 (Bug, fd70b4e) regression: the gutter left-marker color used to
 * depend on the silent ORDER of `if (highlight_*) marker = color;` blocks.
 * The fix introduced an explicit MarkerPriority enum
 * (TUTORIAL_INSERTION > FEEDING_COLOR > FEEDING_NORMAL > REPLAY). Walk
 * the priority matrix: for each pair of overlapping signals on the same
 * line, the marker color must match the higher-priority signal. A revert
 * to assignment-order would silently flip pairs and break this. */
static int rgba_eq(const float a[4], float r, float g, float b, float al) {
    const float eps = 1e-3f;
    return (a[0] > r - eps && a[0] < r + eps) &&
           (a[1] > g - eps && a[1] < g + eps) &&
           (a[2] > b - eps && a[2] < b + eps) &&
           (a[3] > al - eps && a[3] < al + eps);
}

static void marker_for_line(int source_line_idx, int *active, float rgba[4]) {
    UiRenderSnapshot snap;
    ui_repl_code_panel_invalidate_row_cache_for_test();
    make_test_ui_snapshot(&snap);
    ui_repl_code_panel_render_with_chrome(&snap, NULL);
    *active = 0;
    ui_repl_code_panel_row_marker_for_test(source_line_idx, active, rgba);
}

static void test_marker_priority_cascade(void) {
    int active;
    float rgba[4];

    /* The four marker colors from repl_code_panel_apply_command_overlays. */
    const float c_replay[4]    = {0.20f, 0.90f, 0.30f, 0.85f};
    const float c_normal[4]    = {0.40f, 0.80f, 0.95f, 0.85f};
    const float c_color[4]     = {0.95f, 0.85f, 0.30f, 0.85f};
    const float c_tutorial[4]  = {0.95f, 0.45f, 0.85f, 0.90f};

    glr_ctrl_reset_all();
    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.45f;
    editor_scroll_follow_cursor_set(0);
    editor_feed_line("glBegin(GL_POINTS);");
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("glEnd();");

    /* Each sub-case clears highlights, sets exactly the signals under
     * test, renders, and asserts the resulting marker color. */

    /* REPLAY alone → green marker. */
    editor_state_highlights_clear();
    replay_state_mut()->active = 1;
    replay_state_mut()->src_line_idx = 1;
    marker_for_line(1, &active, rgba);
    ASSERT_TRUE("REPLAY alone: marker active", active);
    ASSERT_TRUE("REPLAY alone: green color",
                rgba_eq(rgba, c_replay[0], c_replay[1], c_replay[2], c_replay[3]));

    /* REPLAY + FEEDING_NORMAL → blue (NORMAL wins over REPLAY). */
    editor_state_highlights_clear();
    editor_state_highlights_append(1, -1, -1, HIGHLIGHT_FEEDING_NORMAL);
    replay_state_mut()->active = 1;
    replay_state_mut()->src_line_idx = 1;
    marker_for_line(1, &active, rgba);
    ASSERT_TRUE("REPLAY+NORMAL: marker active", active);
    ASSERT_TRUE("REPLAY+NORMAL: NORMAL wins (blue)",
                rgba_eq(rgba, c_normal[0], c_normal[1], c_normal[2], c_normal[3]));

    /* REPLAY + FEEDING_COLOR → yellow (COLOR wins). NORMAL is
     * exclusive-or with COLOR in apply_command_overlays (an
     * `else if`), so we set COLOR alone here. */
    editor_state_highlights_clear();
    editor_state_highlights_append(1, -1, -1, HIGHLIGHT_FEEDING_COLOR);
    replay_state_mut()->active = 1;
    replay_state_mut()->src_line_idx = 1;
    marker_for_line(1, &active, rgba);
    ASSERT_TRUE("REPLAY+COLOR: marker active", active);
    ASSERT_TRUE("REPLAY+COLOR: COLOR wins (yellow)",
                rgba_eq(rgba, c_color[0], c_color[1], c_color[2], c_color[3]));

    /* All three (REPLAY + COLOR + TUTORIAL_INSERTION) → pink. */
    editor_state_highlights_clear();
    editor_state_highlights_append(1, -1, -1, HIGHLIGHT_FEEDING_COLOR);
    editor_state_highlights_append(1, -1, -1, HIGHLIGHT_TUTORIAL_INSERTION);
    replay_state_mut()->active = 1;
    replay_state_mut()->src_line_idx = 1;
    marker_for_line(1, &active, rgba);
    ASSERT_TRUE("REPLAY+COLOR+TUTORIAL: marker active", active);
    ASSERT_TRUE("REPLAY+COLOR+TUTORIAL: TUTORIAL wins (pink)",
                rgba_eq(rgba, c_tutorial[0], c_tutorial[1], c_tutorial[2], c_tutorial[3]));

    /* TUTORIAL alone (no REPLAY/FEEDING) → pink. Confirms tutorial wins
     * even without lower-priority signals also being set. */
    editor_state_highlights_clear();
    editor_state_highlights_append(1, -1, -1, HIGHLIGHT_TUTORIAL_INSERTION);
    replay_state_mut()->active = 0;
    replay_state_mut()->src_line_idx = -1;
    marker_for_line(1, &active, rgba);
    ASSERT_TRUE("TUTORIAL alone: marker active", active);
    ASSERT_TRUE("TUTORIAL alone: pink color",
                rgba_eq(rgba, c_tutorial[0], c_tutorial[1], c_tutorial[2], c_tutorial[3]));

    /* No signals → marker inactive. */
    editor_state_highlights_clear();
    replay_state_mut()->active = 0;
    replay_state_mut()->src_line_idx = -1;
    marker_for_line(1, &active, rgba);
    ASSERT_TRUE("no signals: marker inactive", !active);

    /* Reset for follow-on tests. */
    editor_state_highlights_clear();
    replay_state_mut()->active = 0;
    replay_state_mut()->src_line_idx = -1;
}

int main(void) {
#ifndef GL_STUBS
    printf("This test requires GL stubs (USE_GL_STUBS=1)\n");
    return 0;
#endif

    glr_ctrl_reset_all();
    /* The color-picker peer reads/writes the document through an installed
     * host; wire the REPL/editor-backed one so the picker tests can open. */
    glr_color_picker_install_host();

    test_help_overlay();
    test_profile_panel();
    test_color_picker();
    test_color_picker_active_line_no_revert();
    test_autocomplete_panel();
    test_variable_panel();
    test_ui_clamp_panel_y_honors_edge_pad();
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
    test_statusbar_command_count_overflow_color();
    test_vertex2f_gutter_labels();
    test_vertex_gutter_labels_follow_cursor_begin_block();
    test_statusbar_action_tooltips();
    test_tutorial_fade_math_uses_snapshot_view();
    test_tutorial_fade_render_uses_per_char_path();
    test_tutorial_fade_handles_wrapped_lines();
    test_render_then_hit_test_row_consistency();
    test_marker_priority_cascade();

    printf("\nUI Tests: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
