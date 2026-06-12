/*
 * test_ui_cpuprof.c
 */
#ifdef GL_STUBS
#include "ui/support/cpuprof.h"
#include "support/cpuprof.h"
#include "app/glr_prof.h"
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

static void test_cpuprof_metrics(void) {
    ASSERT_TRUE("width is positive", ui_profile_panel_width() > 0);
    ASSERT_TRUE("height is positive on", ui_profile_panel_height(PROFILE_PANEL_ON) > 0);
    ASSERT_TRUE("height is positive details", ui_profile_panel_height(PROFILE_PANEL_DETAILS) > 0);
}

static void test_gpu_section_policy(void) {
    /* GL-emitting sections are GPU-bracketed... */
    ASSERT_INT_EQ("scene 3d is gpu", glr_prof_section_is_gpu(PROF_SCENE_3D), 1);
    ASSERT_INT_EQ("code panel is gpu", glr_prof_section_is_gpu(PROF_CODE_PANEL), 1);
    ASSERT_INT_EQ("frame total is gpu", glr_prof_section_is_gpu(PROF_FRAME_TOTAL), 1);
    /* ...pure-CPU sections and the per-fade-batch budget exclusions are not. */
    ASSERT_INT_EQ("flatten is cpu-only", glr_prof_section_is_gpu(PROF_FLATTEN), 0);
    ASSERT_INT_EQ("snapshot is cpu-only", glr_prof_section_is_gpu(PROF_SNAPSHOT), 0);
    ASSERT_INT_EQ("frame restore is cpu-only",
                  glr_prof_section_is_gpu(PROF_FRAME_RESTORE), 0);
    ASSERT_INT_EQ("fade batch exec excluded",
                  glr_prof_section_is_gpu(PROF_SCENE_3D_FADE_BATCH_EXEC), 0);
    ASSERT_INT_EQ("code panel build rows excluded",
                  glr_prof_section_is_gpu(PROF_CODE_PANEL_ROWS), 0);
    ASSERT_INT_EQ("code panel text layout excluded",
                  glr_prof_section_is_gpu(PROF_CODE_PANEL_TEXT_LAYOUT), 0);
    ASSERT_INT_EQ("out-of-range section rejected",
                  glr_prof_section_is_gpu((ProfSection)-1), 0);
}

static void test_cpuprof_render_off(void) {
    UiProfilePanelView view = {
        .window_w = 800,
        .window_h = 600,
        .mode = PROFILE_PANEL_OFF,
        .panel_x = 10,
        .panel_y = 10
    };
    gl_stub_counts_reset();
    ui_profile_panel_render(&view);
    ASSERT_INT_EQ("off mode draws nothing",
                  (int)gl_stub_counts[GL_STUB_glRectf], 0);
}

static void test_cpuprof_render_on(void) {
    UiProfilePanelView view = {
        .window_w = 800,
        .window_h = 600,
        .mode = PROFILE_PANEL_ON,
        .panel_x = 10,
        .panel_y = 10
    };

    prof_frame_tick();

    gl_stub_counts_reset();
    ui_profile_panel_render(&view);
    ASSERT_TRUE("on mode draws frame",
                gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("on mode draws text",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0 ||
                gl_stub_counts[GL_STUB_glutBitmapCharacter] > 0);
}

static void test_cpuprof_render_details(void) {
    UiProfilePanelView view = {
        .window_w = 800,
        .window_h = 600,
        .mode = PROFILE_PANEL_DETAILS,
        .panel_x = 10,
        .panel_y = 10
    };

    prof_frame_tick();

    gl_stub_counts_reset();
    ui_profile_panel_render(&view);
    ASSERT_TRUE("details mode draws frame",
                gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("details mode draws text",
                gl_stub_counts[GL_STUB_glRasterPos2f] > 0 ||
                gl_stub_counts[GL_STUB_glutBitmapCharacter] > 0);
}

int main(void) {
    printf("--- ui_cpuprof tests ---\n");
    test_cpuprof_metrics();
    test_gpu_section_policy();
    test_cpuprof_render_off();
    test_cpuprof_render_on();
    test_cpuprof_render_details();
    printf("\n");
    return test_harness_report(&g_harness, "test_ui_cpuprof");
}
#else
int main(void) {
    printf("This test requires GL stubs (USE_GL_STUBS=1)\n");
    return 0;
}
#endif
