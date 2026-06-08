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
#include "ui/app/snapshot.h"
#include "ui/app/repl_code_panel.h"
#include "ui/core/metrics.h"
#include "repl/core.h"
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
    ASSERT_INT_EQ("error and info use same number of color calls",
                  (int)err_color4f, (int)info_color4f);
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

int main(void) {
    printf("--- ui_panels tests ---\n");

    test_scene_status_rename_mode();
    test_scene_status_file_prompt_mode();
    test_scene_status_file_prompt_error_mode();
    test_scene_status_banner_mode();
    test_scene_status_error_banner();
    test_scene_status_no_render_when_inactive();

    printf("\n");
    return test_harness_report(&g_harness, "test_ui_panels");
}

#else

int main(void) {
    printf("This test requires GL stubs (USE_GL_STUBS=1)\n");
    return 0;
}

#endif
