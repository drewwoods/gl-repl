/*
 * test_ui_panels.c - Regression tests for ui_panels_render_scene_status
 * (Tier C #55) and repl_code_panel_newline_rows (Tier C #54).
 *
 * GL_STUBS-only: exercises the render path via stub call counts.
 */
#ifdef GL_STUBS
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "ui/app/panels.h"
#include "ui/app/state.h"
#include "ui/app/layout.h"
#include "ui/app/overlay_layout.h"
#include "ui/app/snapshot.h"
#include "ui/app/repl_code_panel.h"
#include "ui/app/variable_panel_view.h"
#include "ui/core/metrics.h"
#include "ui/support/cpuprof.h"
#include "ui/support/memprof.h"

#include "support/test_harness.h"
#include <GL/gl_stub_counts.h>
#endif

#include <stdio.h>
#include <string.h>

#ifdef GL_STUBS
static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT_EQ(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

static void make_snap(UiRenderSnapshot *snap) {
    memset(snap, 0, sizeof(*snap));
    snap->viewport = ui_state_viewport();
    snap->anim_time = 1.0f;
}

/* #55 regression: scene status strip has three near-identical rendering
 * blocks (rename modal, file prompt, status banner). Pin that each mode
 * emits GL calls so the extraction refactor doesn't lose a path. */

static void test_scene_status_rename_mode(void) {
    UiRenderSnapshot snap;

    glr_ctrl_reset_all();
    ui_state_reset();
    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.4f;

    make_snap(&snap);
    snap.rename_active = 1;
    snprintf(snap.rename_text, sizeof(snap.rename_text), "my-scene");

    gl_stub_counts_reset();
    ui_panels_render_scene_status(&snap);

    ASSERT_TRUE("rename mode draws background rect",
                gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("rename mode draws text",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0 ||
                gl_stub_counts[GL_STUB_glutBitmapCharacter] > 0);
    ASSERT_TRUE("rename mode enables blend",
                gl_stub_counts[GL_STUB_glEnable] > 0);
}

static void test_scene_status_file_prompt_mode(void) {
    UiRenderSnapshot snap;

    glr_ctrl_reset_all();
    ui_state_reset();
    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.4f;

    make_snap(&snap);
    snap.file_prompt_active = 1;
    snprintf(snap.file_prompt_text, sizeof(snap.file_prompt_text),
             "output.c");
    snap.file_prompt_error[0] = '\0';

    gl_stub_counts_reset();
    ui_panels_render_scene_status(&snap);

    ASSERT_TRUE("file prompt mode draws background rect",
                gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("file prompt mode draws text",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0 ||
                gl_stub_counts[GL_STUB_glutBitmapCharacter] > 0);
    ASSERT_TRUE("file prompt mode enables blend",
                gl_stub_counts[GL_STUB_glEnable] > 0);
}

static void test_scene_status_file_prompt_error_mode(void) {
    UiRenderSnapshot snap;

    glr_ctrl_reset_all();
    ui_state_reset();
    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.4f;

    make_snap(&snap);
    snap.file_prompt_active = 1;
    snprintf(snap.file_prompt_text, sizeof(snap.file_prompt_text),
             "missing.c");
    snprintf(snap.file_prompt_error, sizeof(snap.file_prompt_error),
             "file not found");

    gl_stub_counts_reset();
    ui_panels_render_scene_status(&snap);

    ASSERT_TRUE("file prompt error draws background rect",
                gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("file prompt error draws text",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0 ||
                gl_stub_counts[GL_STUB_glutBitmapCharacter] > 0);
}

static void test_scene_status_banner_mode(void) {
    UiRenderSnapshot snap;

    glr_ctrl_reset_all();
    ui_state_reset();
    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.4f;

    make_snap(&snap);
    /* A shown message always has at least one history entry (the push that
     * set it), so the bell the banner telescopes out of exists. */
    snap.status_history.count = 1;
    snap.status.ttl = 120;
    snap.status.age = 120;   /* fully telescoped out (past the open ramp) */
    snap.status.kind = UI_STATUS_INFO;
    snprintf(snap.status.text, sizeof(snap.status.text), "Saved to output.c");

    gl_stub_counts_reset();
    ui_panels_render_scene_status(&snap);

    ASSERT_TRUE("status banner draws filled geometry (bell + bar)",
                gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("status banner emits primitive batches",
                gl_stub_counts[GL_STUB_glBegin] > 0);
    ASSERT_TRUE("status banner draws text",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0 ||
                gl_stub_counts[GL_STUB_glutBitmapCharacter] > 0);
    ASSERT_TRUE("status banner enables blend",
                gl_stub_counts[GL_STUB_glEnable] > 0);
}

static void test_scene_status_error_banner(void) {
    UiRenderSnapshot snap;
    unsigned long long info_color4f;
    unsigned long long err_color4f;

    glr_ctrl_reset_all();
    ui_state_reset();
    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.4f;

    make_snap(&snap);
    snap.status_history.count = 1;
    snap.status.ttl = 120;
    snap.status.age = 120;
    snap.status.kind = UI_STATUS_INFO;
    snprintf(snap.status.text, sizeof(snap.status.text), "info msg");

    gl_stub_counts_reset();
    ui_panels_render_scene_status(&snap);
    info_color4f = gl_stub_counts[GL_STUB_glColor4f];

    snap.status.kind = UI_STATUS_ERROR;
    snprintf(snap.status.text, sizeof(snap.status.text), "error msg");

    gl_stub_counts_reset();
    ui_panels_render_scene_status(&snap);
    err_color4f = gl_stub_counts[GL_STUB_glColor4f];

    ASSERT_TRUE("error banner draws (color calls exist)",
                err_color4f > 0);
    /* Error renders the info structure plus exactly one extra color set:
     * the UI_TOK_STATUS_ERR leading-edge stripe on the neutral band. */
    ASSERT_INT_EQ("error adds exactly the edge-stripe color call",
                  (int)err_color4f, (int)info_color4f + 1);
}

static void test_scene_status_no_render_when_inactive(void) {
    UiRenderSnapshot snap;

    glr_ctrl_reset_all();
    ui_state_reset();
    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.4f;

    make_snap(&snap);
    snap.rename_active = 0;
    snap.file_prompt_active = 0;
    snap.status.ttl = 0;
    snap.status.text[0] = '\0';

    gl_stub_counts_reset();
    ui_panels_render_scene_status(&snap);

    ASSERT_INT_EQ("inactive status draws no rects",
                  (int)gl_stub_counts[GL_STUB_glRectf], 0);
    ASSERT_INT_EQ("inactive status draws no text",
                  (int)gl_stub_counts[GL_STUB_glRasterPos2f], 0);
}

static void prepare_overlay_hit_snap(UiRenderSnapshot *snap) {
    glr_ctrl_reset_all();
    ui_state_reset();
    ui_overlay_layout_reset();
    ui_state_viewport_set_size(1000, 800);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.3f;
    make_snap(snap);
}

static void test_front_overlay_hit_surfaces(void) {
    UiRenderSnapshot snap;
    UiOverlayLayoutIn layout_in;
    UiVariablePanelView var_view;
    UiHit hit;
    int px, py, pw, ph;
    int my;

    prepare_overlay_hit_snap(&snap);
    snap.variable_panel.visible = 1;
    snap.variable_panel_vars.count = 2;
    var_view = ui_app_variable_panel_view(&snap);
    ui_variable_panel_rect(&var_view, &px, &py, &pw, &ph);
    my = snap.viewport.window_h - (py + ph - 2);
    hit = ui_panels_hit_test_above_gl_state(&snap, px + 4, my, 2);
    ASSERT_INT_EQ("variable title is front overlay chrome",
                  hit.kind, UI_HIT_OVERLAY_CHROME);

    /* A real row remains actionable through the same front-layer pass. */
    hit = ui_hit_none();
    for (int y = py; y < py + ph && hit.kind == UI_HIT_NONE; y++) {
        UiHit candidate = ui_panels_hit_test_above_gl_state(
            &snap, px + pw / 2, snap.viewport.window_h - y, 2);
        if (candidate.kind == UI_HIT_VARIABLE_SLIDER)
            hit = candidate;
    }
    ASSERT_INT_EQ("variable row keeps slider hit",
                  hit.kind, UI_HIT_VARIABLE_SLIDER);

    prepare_overlay_hit_snap(&snap);
    snap.profile_panel.mode = PROFILE_PANEL_SECTIONS;
    layout_in = ui_overlay_layout_inputs((UiOverlayLayoutInputs){
        .profile_mode               = snap.profile_panel.mode,
        .profile_collapsed_sections = snap.profile_panel.collapsed_sections,
        .memory_mode                = MEMORY_PANEL_OFF,
    });
    ui_overlay_layout_panel_pos(&layout_in, UI_OVERLAY_PANEL_PROFILE,
                                &px, &py);
    hit = ui_panels_hit_test_above_gl_state(
        &snap, px + ui_profile_panel_width() / 2,
        snap.viewport.window_h - (py + 2), 0);
    ASSERT_INT_EQ("profile body is front overlay chrome",
                  hit.kind, UI_HIT_OVERLAY_CHROME);

    prepare_overlay_hit_snap(&snap);
    snap.profile_panel.mode = PROFILE_PANEL_HISTOGRAM;
    layout_in = ui_overlay_layout_inputs((UiOverlayLayoutInputs){
        .profile_mode = snap.profile_panel.mode,
        .memory_mode  = MEMORY_PANEL_OFF,
    });
    ui_overlay_layout_panel_pos(&layout_in, UI_OVERLAY_PANEL_HISTOGRAM,
                                &px, &py);
    hit = ui_panels_hit_test_above_gl_state(
        &snap, px + ui_histogram_panel_width() / 2,
        snap.viewport.window_h -
            (py + ui_histogram_panel_height() / 2), 0);
    ASSERT_INT_EQ("histogram panel is front overlay chrome",
                  hit.kind, UI_HIT_OVERLAY_CHROME);

    /* The header-right "[reset]" control stays actionable through the same
     * front-layer pass. */
    hit = ui_panels_hit_test_above_gl_state(
        &snap, px + ui_histogram_panel_width() - 4,
        snap.viewport.window_h -
            (py + ui_histogram_panel_height() - 6), 0);
    ASSERT_INT_EQ("histogram header control hits reset",
                  hit.kind, UI_HIT_HISTOGRAM_RESET);

    prepare_overlay_hit_snap(&snap);
    snap.profile_panel.mode = PROFILE_PANEL_FPS;
    layout_in = ui_overlay_layout_inputs((UiOverlayLayoutInputs){
        .profile_mode = snap.profile_panel.mode,
        .memory_mode  = MEMORY_PANEL_OFF,
    });
    ui_overlay_layout_panel_pos(&layout_in, UI_OVERLAY_PANEL_FPS,
                                &px, &py);
    hit = ui_panels_hit_test_above_gl_state(
        &snap, px + ui_fps_panel_width() / 2,
        snap.viewport.window_h - (py + ui_fps_panel_height() / 2), 0);
    ASSERT_INT_EQ("FPS panel is front overlay chrome",
                  hit.kind, UI_HIT_OVERLAY_CHROME);

    prepare_overlay_hit_snap(&snap);
    snap.memory_panel.mode = MEMORY_PANEL_ON;
    layout_in = ui_overlay_layout_inputs((UiOverlayLayoutInputs){
        .profile_mode = PROFILE_PANEL_OFF,
        .memory_mode  = snap.memory_panel.mode,
    });
    ui_overlay_layout_panel_pos(&layout_in, UI_OVERLAY_PANEL_MEMORY,
                                &px, &py);
    hit = ui_panels_hit_test_above_gl_state(
        &snap, px + ui_memory_panel_width() / 2,
        snap.viewport.window_h - (py + ui_memory_panel_height() / 2), 0);
    ASSERT_INT_EQ("memory panel is front overlay chrome",
                  hit.kind, UI_HIT_OVERLAY_CHROME);

    prepare_overlay_hit_snap(&snap);
    snap.rename_active = 1;
    ui_layout_scene_rect(&px, &py, &pw, &ph);
    hit = ui_panels_hit_test_above_gl_state(
        &snap, px + pw / 2,
        snap.viewport.window_h - (py + STATUSBAR_H / 2), 0);
    ASSERT_INT_EQ("modal status strip is front overlay chrome",
                  hit.kind, UI_HIT_OVERLAY_CHROME);
}

/* The messages list is painted after every floating telemetry surface. Keep
 * the hit-test order in lockstep so a click on visible list text cannot reach
 * a CPU-profile or assignment-plot panel beneath it. */
static void test_status_history_over_telemetry(void) {
    UiRenderSnapshot snap;
    UiHit hit;
    int bx, by, bw, bh;
    int mx, my;

    prepare_overlay_hit_snap(&snap);
    snap.status_history.count = 1;
    snap.status_history.open = 1;
    ASSERT_TRUE("status history button exists above telemetry",
                ui_panels_status_history_button_rect(&snap,
                                                     &bx, &by, &bw, &bh));

    /* The list grows upward from the bell. This point sits in the first list
     * row and in the bottom-most telemetry panel, which is where the two
     * surfaces naturally overlap. */
    mx = bx + bw / 2;
    my = snap.viewport.window_h - (by + bh + 8);

    snap.assign_plot.open = 1;
    hit = ui_panels_hit_test_above_gl_state(&snap, mx, my, 0);
    ASSERT_INT_EQ("history list wins over assignment plot",
                  hit.kind, UI_HIT_STATUS_HISTORY);

    snap.assign_plot.open = 0;
    snap.profile_panel.mode = PROFILE_PANEL_FPS;
    hit = ui_panels_hit_test_above_gl_state(&snap, mx, my, 0);
    ASSERT_INT_EQ("history list wins over CPU profile panel",
                  hit.kind, UI_HIT_STATUS_HISTORY);
}

int main(void) {
    printf("--- ui_panels tests ---\n");

    test_scene_status_rename_mode();
    test_scene_status_file_prompt_mode();
    test_scene_status_file_prompt_error_mode();
    test_scene_status_banner_mode();
    test_scene_status_error_banner();
    test_scene_status_no_render_when_inactive();
    test_front_overlay_hit_surfaces();
    test_status_history_over_telemetry();

    printf("\n");
    return test_harness_report(&g_harness, "test_ui_panels");
}

#else

int main(void) {
    printf("This test requires GL stubs (USE_GL_STUBS=1)\n");
    return 0;
}

#endif
