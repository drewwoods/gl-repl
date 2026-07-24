/*
 * test_glr_capture_env.c - headless-capture environment hooks.
 *
 * glr_capture_env_apply reads a family of GLR_* env vars at bootstrap and
 * pushes them into the controller. The contract worth pinning is that each
 * hook fires only when its variable is set, and lands on the value asked for
 * rather than a default. Hooks are asserted through the state they write:
 * GLR_ACCUM_PASSES -> glr_config_get(GLR_CONFIG_ACCUM_PASSES),
 * GLR_EDIT_LINE    -> editor_state_edit_line().
 *
 * Values are set explicitly and compared against what was set (never against
 * a shipped default), so retuning the defaults cannot silently pass this.
 *
 * glr_capture_env_frame_hook's four hooks each latch a one-shot static on
 * first call, so the meaningful process-wide assertion is the unset path:
 * with no env vars set it must be an inert no-op.
 *
 * Runs GL-free (bootstrap_repl needs no context, same as the --dump-* path)
 * and links CORE_TEST_OBJS.
 */
#include "app/glr_capture_env.h"
#include "app/glr_ctrl.h"
#include "app/glr_config.h"
#include "editor/state.h"
#include "editor/input.h"   /* editor_feed_line */

#include "support/test_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, g, e)  TEST_ASSERT_INT(&g_harness, label, g, e)

/* Clear every variable glr_capture_env_apply consults so each case starts
 * from a known-unset environment. */
static void clear_capture_env(void) {
    unsetenv("GLR_TIME");
    unsetenv("GLR_NO_SPLASH");
    unsetenv("GLR_TICK_PER_FRAME");
    unsetenv("GLR_VIEW_TOGGLE_AT");
    unsetenv("GLR_EDIT_LINE");
    unsetenv("GLR_TYPE_KEYS");
    unsetenv("GLR_ACCUM_PASSES");
    unsetenv("GLR_POINTER_SCRIPT");
    unsetenv("GLR_OPEN_COLOR_PICKER");
    unsetenv("GLR_OPEN_GL_STATE");
    unsetenv("GLR_OPEN_HELP");
}

/* GLR_CONFIG_ACCUM_PASSES holds a position on the accum-pass ladder, not the
 * raw pass count. Seed through the same controller entry point the env hook
 * uses and read back the resulting position, so expectations track the ladder
 * instead of hardcoding its current shape. */
static int seed_accum_passes(int passes) {
    glr_ctrl_set_accum_passes(passes);
    return glr_config_get(GLR_CONFIG_ACCUM_PASSES);
}

/* With nothing set, apply must not disturb state the user/config already
 * established. Seed a sentinel pass count and prove it survives. */
static void test_apply_is_inert_when_unset(void) {
    clear_capture_env();

    int before = seed_accum_passes(16);
    glr_capture_env_apply(NULL);

    ASSERT_INT("unset GLR_ACCUM_PASSES leaves config alone",
               glr_config_get(GLR_CONFIG_ACCUM_PASSES), before);
}

static void test_accum_passes_hook(void) {
    clear_capture_env();

    /* Learn where "4" lands, then seed a different rung so a no-op
     * implementation cannot accidentally pass. */
    int want = seed_accum_passes(4);
    int other = seed_accum_passes(16);
    ASSERT_TRUE("ladder distinguishes 4 from 16 passes", want != other);

    setenv("GLR_ACCUM_PASSES", "4", 1);
    glr_capture_env_apply(NULL);
    ASSERT_INT("GLR_ACCUM_PASSES applied",
               glr_config_get(GLR_CONFIG_ACCUM_PASSES), want);

    /* An empty value is treated as unset, not as 0. */
    int seeded = seed_accum_passes(16);
    setenv("GLR_ACCUM_PASSES", "", 1);
    glr_capture_env_apply(NULL);
    ASSERT_INT("empty GLR_ACCUM_PASSES is ignored",
               glr_config_get(GLR_CONFIG_ACCUM_PASSES), seeded);

    clear_capture_env();
}

static void test_edit_line_hook(void) {
    clear_capture_env();

    /* Give the document enough committed lines for line 2 to be a real row. */
    editor_feed_line("glColor3f(1,0,0);");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0,0,0);");
    editor_feed_line("glEnd();");

    editor_state_edit_line_set(0);
    ASSERT_INT("edit line seeded at 0", editor_state_edit_line(), 0);

    setenv("GLR_EDIT_LINE", "2", 1);
    glr_capture_env_apply(NULL);
    ASSERT_INT("GLR_EDIT_LINE parks the cursor", editor_state_edit_line(), 2);

    /* Unset leaves the cursor where it was rather than resetting it. */
    clear_capture_env();
    glr_capture_env_apply(NULL);
    ASSERT_INT("unset GLR_EDIT_LINE leaves the cursor",
               editor_state_edit_line(), 2);
}

/* The frame hook's four affordances each latch on first call; with nothing
 * set it must stay inert and leave surrounding state untouched. */
static void test_frame_hook_inert_when_unset(void) {
    clear_capture_env();

    int accum_before = seed_accum_passes(16);
    int line_before = editor_state_edit_line();

    glr_capture_env_frame_hook();
    glr_capture_env_frame_hook();   /* one-shots must not fire on a re-entry */

    ASSERT_INT("frame hook leaves accum passes alone",
               glr_config_get(GLR_CONFIG_ACCUM_PASSES), accum_before);
    ASSERT_INT("frame hook leaves the cursor alone",
               editor_state_edit_line(), line_before);
}

int main(void) {
    /* GL-free REPL bootstrap: same path the --dump-* CLI flags use. */
    glr_ctrl_bootstrap_repl(NULL);

    test_apply_is_inert_when_unset();
    test_accum_passes_hook();
    test_edit_line_hook();
    test_frame_hook_inert_when_unset();

    clear_capture_env();
    return test_harness_report(&g_harness, "glr_capture_env");
}
