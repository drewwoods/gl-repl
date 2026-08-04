/*
 * test_hidden_lines.c - the hidden-line wireframe walk's glPushAttrib/
 * glPopAttrib scoping, and which of its three passes runs the program's
 * glClear.
 *
 * All three passes drive push/pop through the executor cursor with
 * ReplExecutionOptions.suppress_attrib_gl = 1: no glPushAttrib/glPopAttrib
 * reach GL (the pass owns its own depth/colour/polygon state), but the
 * executor-side state the push saves - the cursor's clear colour / colour
 * mask, and the light-enable mask - still scopes to the push/pop pair. This
 * test proves scoped changes do not leak out of the walk, including an
 * unmatched push that only the end-of-cursor unwind can balance, and that the
 * background observation the depth fill publishes is NOT rewound by either.
 *
 * Clear-colour channels are clamped to REPL_CLEAR_COLOR_MAX_V (0.1) to keep the
 * background dark, so the test values stay below that ceiling to remain
 * distinct after the clamp.
 */
#include "editor/state.h"
#include "editor/input.h"
#include "repl/state_views.h"
#include "repl/state_owners.h"
#include "repl/pipeline.h"
#include "repl/flatten.h"
#include "app/glr_ctrl.h"
#include "subsystems/hidden_lines/hidden_lines.h"
#include "support/repl_test_support.h"
#include "support/test_harness.h"
#include <GL/gl_stub_counts.h>
#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define ASSERT_INT(label, got, want) do { \
    TEST_ASSERT_INT(&g_harness, label, got, want); \
} while (0)

static void declare_test_vars(void) {
    char err[128];
    static const char *const names[] = { "x", "y", "z" };
    (void)repl_test_declare_predef_vars(names, 3, err, sizeof(err));
}

/* The baseline these walks start from - what a glClear with no preceding
 * glClearColor would use. Distinct from every value the tests set, so an
 * observation that fell back to it is visible as a failure. */
static const float k_baseline_rgba[4] = { 0.01f, 0.01f, 0.01f, 1.0f };

/* Feed the current document through one wireframe walk. `obs_out` is the
 * background-observation sink; the subsystem only lets the depth-fill pass
 * publish through it. */
static void run_walk_observed(Render3dExecutePurpose purpose,
                              ReplBackgroundObservation *obs_out) {
    char status[256] = "";
    repl_flatten_commands(editor_state_edit_line());
    FlatProgramView prog = repl_state_flat_program_view();
    HiddenLinesRenderContext ctx = {
        .flat_cmd_count  = prog.cmd_count,
        .program         = prog,
        .status_out      = status,
        .status_out_sz   = (int)sizeof(status),
        .observation_out = obs_out,
    };
    memcpy(ctx.baseline_clear_rgba, k_baseline_rgba,
           sizeof(ctx.baseline_clear_rgba));
    hidden_lines_execute(&ctx, purpose);
}

static void run_walk(Render3dExecutePurpose purpose) {
    run_walk_observed(purpose, NULL);
}

/* Poisoned sink, so a missing publication fails rather than reads as zero -
 * and so "this pass published nothing" is assertable as the poison surviving
 * rather than as a value that a real publication could also produce. */
#define OBS_UNPUBLISHED (-424242)

static ReplBackgroundObservation observe_walk(Render3dExecutePurpose purpose) {
    ReplBackgroundObservation obs;
    memset(&obs, 0x5A, sizeof(obs));
    obs.known = OBS_UNPUBLISHED;
    run_walk_observed(purpose, &obs);
    return obs;
}

static void run_visible_lines_walk(void) {
    run_walk(RENDER3D_EXEC_WIREFRAME_VISIBLE_LINES);
}

static int clears_emitted_by(Render3dExecutePurpose purpose) {
    gl_stub_counts_reset();
    run_walk(purpose);
    return (int)gl_stub_counts[GL_STUB_glClear];
}

/* A clear colour changed inside a glPushAttrib(GL_COLOR_BUFFER_BIT) scope must
 * be reverted by the matching pop. The observable is the clear that follows
 * the pop: it takes the pre-push value, exactly as GL's own would. */
static void test_scoped_clear_color_does_not_leak(void) {
    printf("--- hidden-lines: scoped clear colour ---\n");
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glClearColor(0.02, 0.04, 0.06, 1);");     /* baseline V1 */
    editor_feed_line("glPushAttrib(GL_COLOR_BUFFER_BIT);");
    editor_feed_line("glClearColor(0.09, 0.08, 0.07, 1);");     /* scoped V2 */
    editor_feed_line("glPopAttrib();");
    editor_feed_line("glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);");
    editor_feed_line("glutSolidCube(0.5);");

    ReplBackgroundObservation obs =
        observe_walk(RENDER3D_EXEC_WIREFRAME_DEPTH_FILL);
    ASSERT_TRUE("clear after the pop uses the pre-push clear colour",
                obs.known == 1 &&
                obs.rgba[0] == 0.02f && obs.rgba[1] == 0.04f &&
                obs.rgba[2] == 0.06f && obs.rgba[3] == 1.0f);
}

/* A light enabled inside a glPushAttrib(GL_ENABLE_BIT) scope must be disabled
 * again by the pop; the pre-push light stays enabled. */
static void test_scoped_light_enable_does_not_leak(void) {
    printf("--- hidden-lines: scoped light enable ---\n");
    repl_state_render_clear_light_enabled_mask();
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glEnable(GL_LIGHT0);");                   /* pre-push */
    editor_feed_line("glPushAttrib(GL_ENABLE_BIT);");
    editor_feed_line("glEnable(GL_LIGHT1);");                   /* scoped */
    editor_feed_line("glPopAttrib();");
    editor_feed_line("glutSolidCube(0.5);");

    run_visible_lines_walk();

    ReplRenderState r = repl_state_render();
    ASSERT_TRUE("pre-push LIGHT0 stays enabled after walk",
                repl_light_enabled(r.light_enabled_mask, 0) == 1);
    ASSERT_TRUE("scoped LIGHT1 reverted after walk",
                repl_light_enabled(r.light_enabled_mask, 1) == 0);
    repl_state_render_clear_light_enabled_mask();
}

/* An unmatched glPushAttrib is unwound at cursor end. The unwind restores GL
 * state - here the light-enable mask, which outlives the walk - but it must
 * never rewind the background observation: a pop rewinds state, not pixels
 * that were already written. */
static void test_unmatched_push_unwound_at_end(void) {
    printf("--- hidden-lines: unmatched push unwound ---\n");
    repl_state_render_clear_light_enabled_mask();
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glClearColor(0.02, 0.04, 0.06, 1);");     /* baseline V1 */
    editor_feed_line("glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);");
    editor_feed_line("glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT);");
    editor_feed_line("glEnable(GL_LIGHT1);");                   /* scoped */
    editor_feed_line("glClearColor(0.09, 0.08, 0.07, 1);");     /* scoped V2 */
    editor_feed_line("glutSolidCube(0.5);");

    ReplBackgroundObservation obs =
        observe_walk(RENDER3D_EXEC_WIREFRAME_DEPTH_FILL);
    ASSERT_TRUE("unmatched push: cursor-end unwind reverts the light enable",
                repl_light_enabled(repl_state_render().light_enabled_mask, 1) == 0);
    ASSERT_TRUE("unmatched push: the unwind does not revert the observation",
                obs.known == 1 &&
                obs.rgba[0] == 0.02f && obs.rgba[3] == 1.0f);
    repl_state_render_clear_light_enabled_mask();
}

/* The depth-fill pass masks colour writes to seed depth only and lifts the
 * mask for its one clear, so the background it reports is that synthetic
 * full-mask clear - including when the program's own glColorMask would have
 * masked the channels off. It is also the only pass that clears, so the
 * hidden- and visible-line redraws that follow cannot publish over it. */
static void test_depth_fill_reports_its_forced_full_mask_clear(void) {
    printf("--- hidden-lines: depth-fill observes its forced clear ---\n");
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glClearColor(0.03, 0.05, 0.07, 1);");
    editor_feed_line("glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);");
    editor_feed_line("glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);");
    editor_feed_line("glutSolidCube(0.5);");

    ReplBackgroundObservation obs =
        observe_walk(RENDER3D_EXEC_WIREFRAME_DEPTH_FILL);
    ASSERT_TRUE("depth fill reports its own forced full-mask background",
                obs.known == 1 &&
                obs.rgba[0] == 0.03f && obs.rgba[1] == 0.05f &&
                obs.rgba[2] == 0.07f && obs.rgba[3] == 1.0f);

    ASSERT_INT("the hidden-lines redraw publishes nothing",
               observe_walk(RENDER3D_EXEC_WIREFRAME_HIDDEN_LINES).known,
               OBS_UNPUBLISHED);
    ASSERT_INT("the visible-lines redraw publishes nothing",
               observe_walk(RENDER3D_EXEC_WIREFRAME_VISIBLE_LINES).known,
               OBS_UNPUBLISHED);
}

/* The program's glClear is the frame's clear for the scene rect, and this walk
 * replaces the main fill in hidden-line mode - so exactly one of the three
 * passes must run it. The depth-fill pass (first) owns it; re-running it in
 * the hidden- or visible-line pass would wipe the depth seed and the lines
 * already drawn. */
static void test_only_depth_fill_pass_clears(void) {
    printf("--- hidden-lines: glClear runs once, in the depth fill ---\n");
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);");
    editor_feed_line("glutSolidCube(0.5);");

    ASSERT_INT("depth-fill pass emits the program's glClear",
               clears_emitted_by(RENDER3D_EXEC_WIREFRAME_DEPTH_FILL), 1);
    ASSERT_INT("hidden-lines pass does not clear",
               clears_emitted_by(RENDER3D_EXEC_WIREFRAME_HIDDEN_LINES), 0);
    ASSERT_INT("visible-lines pass does not clear",
               clears_emitted_by(RENDER3D_EXEC_WIREFRAME_VISIBLE_LINES), 0);
}

/* The depth-fill pass masks colour writes to seed depth only, and glClear
 * obeys glColorMask - so the clear has to be bracketed by a mask lift that
 * hands the pass's own colour state back afterwards. */
static void test_depth_fill_clear_lifts_color_mask(void) {
    printf("--- hidden-lines: depth-fill clear lifts the colour mask ---\n");
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);");
    editor_feed_line("glutSolidCube(0.5);");

    gl_stub_counts_reset();
    run_walk(RENDER3D_EXEC_WIREFRAME_DEPTH_FILL);
    ASSERT_INT("clear re-enables colour writes", (int)gl_stub_counts[GL_STUB_glColorMask], 1);
    ASSERT_INT("mask lift is scoped by a balanced push",
               (int)gl_stub_counts[GL_STUB_glPushAttrib], 1);
    ASSERT_INT("mask lift is scoped by a balanced pop",
               (int)gl_stub_counts[GL_STUB_glPopAttrib], 1);
}

/* A program with no glClear must not gain one: the walk emits what the source
 * says, so a deleted clear smears here exactly as it does in the main fill. */
static void test_no_clear_line_emits_no_clear(void) {
    printf("--- hidden-lines: no glClear line, no clear ---\n");
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glutSolidCube(0.5);");

    ASSERT_INT("depth-fill pass adds no clear of its own",
               clears_emitted_by(RENDER3D_EXEC_WIREFRAME_DEPTH_FILL), 0);
}

int main(void) {
    repl_eval_init_predef_vars();
    hidden_lines_init_resources();

    test_scoped_clear_color_does_not_leak();
    test_scoped_light_enable_does_not_leak();
    test_unmatched_push_unwound_at_end();
    test_only_depth_fill_pass_clears();
    test_depth_fill_clear_lifts_color_mask();
    test_depth_fill_reports_its_forced_full_mask_clear();
    test_no_clear_line_emits_no_clear();

    hidden_lines_destroy_resources();

    printf("test_hidden_lines: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
