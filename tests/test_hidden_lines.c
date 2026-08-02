/*
 * test_hidden_lines.c - the hidden-line wireframe walk's glPushAttrib/
 * glPopAttrib scoping, and which of its three passes runs the program's
 * glClear.
 *
 * The visible-lines / hidden-lines passes drive push/pop through the executor
 * cursor with ReplExecutionOptions.suppress_attrib_gl = 1: no glPushAttrib/
 * glPopAttrib reach GL (the pass owns its own depth/colour/polygon state), but
 * the REPL render-state bookkeeping mirror (clear colour, light-enable mask)
 * still scopes to the push/pop pair. This test proves scoped clear-colour and
 * light-enable changes do not leak out of the walk, including an unmatched push
 * that only the end-of-cursor unwind can balance.
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

/* Feed the current document through one wireframe walk. */
static void run_walk(Render3dExecutePurpose purpose) {
    char status[256] = "";
    repl_flatten_commands(editor_state_edit_line());
    FlatProgramView prog = repl_state_flat_program_view();
    HiddenLinesRenderContext ctx = {
        .flat_cmd_count = prog.cmd_count,
        .program        = prog,
        .status_out     = status,
        .status_out_sz  = (int)sizeof(status),
    };
    hidden_lines_execute(&ctx, purpose);
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
 * be reverted by the matching pop, so it does not leak past the walk. */
static void test_scoped_clear_color_does_not_leak(void) {
    printf("--- hidden-lines: scoped clear colour ---\n");
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glClearColor(0.02, 0.04, 0.06, 1);");     /* baseline V1 */
    editor_feed_line("glPushAttrib(GL_COLOR_BUFFER_BIT);");
    editor_feed_line("glClearColor(0.09, 0.08, 0.07, 1);");     /* scoped V2 */
    editor_feed_line("glPopAttrib();");
    editor_feed_line("glutSolidCube(0.5);");

    run_visible_lines_walk();

    ReplRenderState r = repl_state_render();
    ASSERT_TRUE("scoped clear colour reverted to V1 after walk",
                r.clear_color[0] == 0.02f && r.clear_color[1] == 0.04f &&
                r.clear_color[2] == 0.06f && r.clear_color[3] == 1.0f);
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

/* An unmatched glPushAttrib is unwound at cursor end, so its scoped clear
 * colour is still reverted even with no explicit glPopAttrib. */
static void test_unmatched_push_unwound_at_end(void) {
    printf("--- hidden-lines: unmatched push unwound ---\n");
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glClearColor(0.02, 0.04, 0.06, 1);");     /* baseline V1 */
    editor_feed_line("glPushAttrib(GL_COLOR_BUFFER_BIT);");     /* never popped */
    editor_feed_line("glClearColor(0.09, 0.08, 0.07, 1);");     /* scoped V2 */
    editor_feed_line("glutSolidCube(0.5);");

    run_visible_lines_walk();

    ReplRenderState r = repl_state_render();
    ASSERT_TRUE("unmatched push: scoped clear colour reverted at cursor end",
                r.clear_color[0] == 0.02f && r.clear_color[3] == 1.0f);
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
    test_no_clear_line_emits_no_clear();

    hidden_lines_destroy_resources();

    printf("test_hidden_lines: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
