/*
 * test_hidden_lines.c - the hidden-line wireframe walk's glPushAttrib/
 * glPopAttrib scoping.
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
#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

static void declare_test_vars(void) {
    char err[128];
    static const char *const names[] = { "x", "y", "z" };
    (void)repl_test_declare_predef_vars(names, 3, err, sizeof(err));
}

/* Feed the current document through a visible-lines wireframe walk. */
static void run_visible_lines_walk(void) {
    char status[256] = "";
    repl_flatten_commands(editor_state_edit_line());
    FlatProgramView prog = repl_state_flat_program_view();
    HiddenLinesRenderContext ctx = {
        .flat_cmd_count = prog.cmd_count,
        .program        = prog,
        .status_out     = status,
        .status_out_sz  = (int)sizeof(status),
    };
    hidden_lines_execute(&ctx, RENDER3D_EXEC_WIREFRAME_VISIBLE_LINES);
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

int main(void) {
    repl_eval_init_predef_vars();
    hidden_lines_init_resources();

    test_scoped_clear_color_does_not_leak();
    test_scoped_light_enable_does_not_leak();
    test_unmatched_push_unwound_at_end();

    hidden_lines_destroy_resources();

    printf("test_hidden_lines: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.passed == g_harness.run) ? 0 : 1;
}
