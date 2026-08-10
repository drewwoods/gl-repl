/*
 * test_glr_capture_env.c - headless-capture environment hooks.
 *
 * glr_capture_env_apply reads a family of GLR_* env vars at bootstrap and
 * pushes them into the controller. The contract worth pinning is that each
 * hook fires only when its variable is set, and lands on the value asked for
 * rather than a default. Hooks are asserted through the state they write:
 * GLR_ACCUM_PASSES -> glr_config_get(GLR_CONFIG_ACCUM_PASSES),
 * GLR_EDIT_LINE    -> editor_state_edit_line() + follow-scroll request.
 *
 * The state-named hooks (GLR_SYNTAX_HIGHLIGHT, GLR_ACCUM_EFFECT) share one
 * matcher against the Config row's own labels, so their cases also pin that a
 * value naming no state is refused rather than falling through to one.
 *
 * Line-valued hooks take the code panel's own 1-based numbering and convert at
 * the env boundary, so every expectation here is one lower than what is set.
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
#include "app/boot/glr_capture_env.h"
#include "app/glr_ctrl.h"
#include "app/glr_config.h"
#include "editor/state.h"
#include "editor/input.h"   /* editor_feed_line */
#include "editor/help_session.h"
#include "repl/state_owners.h"
#include "repl/state_views.h"
#include "app/boot/splash.h"
#include "subsystems/color_picker/color_picker_state.h"
#include "app/glr_color_picker_bridge.h"
#include "ui/app/repl_code_panel.h"
#include "ui/app/state.h"

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
    unsetenv("GLR_ACCUM_EFFECT");
    unsetenv("GLR_SYNTAX_HIGHLIGHT");
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

/* One-shot frame hooks must be tested combined as the very first test in the process,
 * because their implementation uses internal file-scope static 'done' latches
 * that cannot be reset between tests. */
static void test_frame_hooks_combined(void) {
    UiRenderSnapshot snap;
    UiPointerState pointer;
    UiHit hit;
    clear_capture_env();
    int initial_view = glr_config_get(GLR_CONFIG_ORTHO_MODE);

    /* Set up target state for the four one-shot frame hooks. The line-valued
     * hooks are spelled the way the code panel numbers rows (1-based), so the
     * document indices they land on are one lower. */
    setenv("GLR_VIEW_TOGGLE_AT", "0.0", 1);
    setenv("GLR_OPEN_COLOR_PICKER", "1", 1);
    setenv("GLR_OPEN_GL_STATE", "5", 1);
    setenv("GLR_OPEN_HELP", "2", 1);

    glr_capture_env_apply(NULL);

    ASSERT_INT("picker initially closed", color_picker_active_line(), -1);
    ASSERT_INT("inspector initially closed", ui_state_gl_state_inspector().visible, 0);
    ASSERT_INT("help overlay initially closed", ui_state_help().visible, 0);

    /* First call to frame hook triggers all 4 hooks */
    glr_capture_env_frame_hook();

    ASSERT_TRUE("view mode toggled", glr_config_get(GLR_CONFIG_ORTHO_MODE) != initial_view);
    ASSERT_INT("picker opened on the panel's line 1", color_picker_active_line(), 0);
    ASSERT_INT("inspector opened", ui_state_gl_state_inspector().visible, 1);
    ASSERT_INT("inspector line is the panel's 5, index 4",
               ui_state_gl_state_inspector().source_line_idx, 4);
    pointer = ui_state_pointer();
    ASSERT_INT("inspector anchor follows routed pointer x",
               ui_state_gl_state_inspector().anchor_px, pointer.mouse_x);
    ASSERT_INT("inspector anchor follows routed pointer y",
               ui_state_gl_state_inspector().anchor_py, pointer.mouse_y);
    glr_ctrl_build_ui_snapshot(&snap);
    hit = ui_repl_code_panel_hit_test(&snap, pointer.mouse_x, pointer.mouse_y);
    ASSERT_TRUE("routed pointer lands on requested blank row",
                (hit.kind == UI_HIT_CODE_TEXT ||
                 hit.kind == UI_HIT_CODE_INSERT_LINE) &&
                hit.line_idx == 4);
    ASSERT_INT("help opened", ui_state_help().visible, 1);
    ASSERT_INT("help tab", editor_help_session_tab_idx(), 2);
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

/* GLR_SYNTAX_HIGHLIGHT names a Config-row state rather than an integer, so the
 * variable spells what the menu spells. It exists because the highlight
 * default is renderer-dependent (off on Mesa) - a capture that wants a
 * particular look has to be able to say so regardless of the box it runs on,
 * which is also why the hook runs after the file/@cfg load.
 *
 * Every expectation resolves its state index by name through the same
 * glr_config_state_name table the hook reads, so renumbering the enum or
 * inserting a state cannot silently pass this. */
static int syntax_state_index(const char *name) {
    int n = glr_config_state_count(GLR_CONFIG_SYNTAX_HIGHLIGHT);
    for (int i = 0; i < n; i++) {
        const char *s = glr_config_state_name(GLR_CONFIG_SYNTAX_HIGHLIGHT, i);
        if (s && strcmp(s, name) == 0)
            return i;
    }
    return -1;
}

static void test_syntax_highlight_hook(void) {
    clear_capture_env();

    int off = syntax_state_index("Off");
    int on = syntax_state_index("On");
    int shadow = syntax_state_index("On+Shadow");
    ASSERT_TRUE("syntax-highlight states resolve by menu name",
                off >= 0 && on >= 0 && shadow >= 0);

    /* Seed the opposite of what each case asks for, so a hook that does
     * nothing cannot pass by landing on a state that was already there. */
    glr_config_set(GLR_CONFIG_SYNTAX_HIGHLIGHT, off);
    setenv("GLR_SYNTAX_HIGHLIGHT", "on", 1);
    glr_capture_env_apply(NULL);
    ASSERT_INT("GLR_SYNTAX_HIGHLIGHT=on applied",
               glr_config_get(GLR_CONFIG_SYNTAX_HIGHLIGHT), on);

    /* Labels are user-facing; the variable matches case- and separator-
     * insensitively so a shell does not have to reproduce "On+Shadow". */
    setenv("GLR_SYNTAX_HIGHLIGHT", "on+shadow", 1);
    glr_capture_env_apply(NULL);
    ASSERT_INT("state names match case-insensitively",
               glr_config_get(GLR_CONFIG_SYNTAX_HIGHLIGHT), shadow);

    setenv("GLR_SYNTAX_HIGHLIGHT", "OFF", 1);
    glr_capture_env_apply(NULL);
    ASSERT_INT("GLR_SYNTAX_HIGHLIGHT=OFF applied",
               glr_config_get(GLR_CONFIG_SYNTAX_HIGHLIGHT), off);

    /* An unrecognized value is refused rather than falling back to a state:
     * a capture that mistypes the mode should keep whatever it had (and gets
     * a stderr line naming the real vocabulary). */
    glr_config_set(GLR_CONFIG_SYNTAX_HIGHLIGHT, shadow);
    setenv("GLR_SYNTAX_HIGHLIGHT", "sparkly", 1);
    glr_capture_env_apply(NULL);
    ASSERT_INT("an unknown state name leaves the setting alone",
               glr_config_get(GLR_CONFIG_SYNTAX_HIGHLIGHT), shadow);

    /* Empty is treated as unset, not as the first state. */
    setenv("GLR_SYNTAX_HIGHLIGHT", "", 1);
    glr_capture_env_apply(NULL);
    ASSERT_INT("empty GLR_SYNTAX_HIGHLIGHT is ignored",
               glr_config_get(GLR_CONFIG_SYNTAX_HIGHLIGHT), shadow);

    clear_capture_env();
}

static void test_edit_line_hook(void) {
    clear_capture_env();

    editor_state_edit_line_set(0);
    editor_scroll_follow_cursor_set(0);
    ASSERT_INT("edit line seeded at 0", editor_state_edit_line(), 0);

    setenv("GLR_EDIT_LINE", "3", 1);
    glr_capture_env_apply(NULL);
    ASSERT_INT("GLR_EDIT_LINE parks the cursor on the panel's line 3",
               editor_state_edit_line(), 2);
    ASSERT_INT("GLR_EDIT_LINE requests scroll follow",
               editor_scroll_follow_cursor(), 1);

    /* The numbering is the code panel's, so 0 names no row at all: refused,
     * rather than quietly parking on the first line. */
    setenv("GLR_EDIT_LINE", "0", 1);
    glr_capture_env_apply(NULL);
    ASSERT_INT("GLR_EDIT_LINE=0 is refused, not treated as line 1",
               editor_state_edit_line(), 2);

    setenv("GLR_EDIT_LINE", "-1", 1);
    glr_capture_env_apply(NULL);
    ASSERT_INT("a negative GLR_EDIT_LINE is refused too",
               editor_state_edit_line(), 2);

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

static void test_glr_time_hook(void) {
    clear_capture_env();

    setenv("GLR_TIME", "10.5", 1);
    glr_capture_env_apply(NULL);
    ASSERT_TRUE("GLR_TIME anim_time applied",
                (float)repl_state_variables().anim_time == 10.5f);

    /* time_arg override wins over GLR_TIME */
    setenv("GLR_TIME", "5.0", 1);
    glr_capture_env_apply("12.0");
    ASSERT_TRUE("time_arg overrides GLR_TIME",
                (float)repl_state_variables().anim_time == 12.0f);
}

static void test_no_splash_hook(void) {
    clear_capture_env();
    ASSERT_INT("splash initially active", splash_active(), 1);

    setenv("GLR_NO_SPLASH", "1", 1);
    glr_capture_env_apply(NULL);
    ASSERT_INT("GLR_NO_SPLASH skips splash screen", splash_active(), 0);
}

static void test_tick_per_frame_hook(void) {
    clear_capture_env();
    repl_state_time_set_playing(1);
    repl_state_time_set(0.0f);

    /* Under normal mode, frame timer advances time */
    glr_ctrl_on_frame_timer();
    float t_after_timer = repl_state_variables().anim_time;
    ASSERT_TRUE("normal timer advances anim_time", t_after_timer > 0.0f);

    /* reset time */
    repl_state_time_set(0.0f);

    /* Enable tick-per-frame */
    setenv("GLR_TICK_PER_FRAME", "1", 1);
    glr_capture_env_apply(NULL);

    /* Frame timer should no longer advance time */
    glr_ctrl_on_frame_timer();
    ASSERT_TRUE("tick-per-frame timer does not advance anim_time",
                repl_state_variables().anim_time == 0.0f);

    /* Presented frame should advance time */
    glr_frame_ended();
    ASSERT_TRUE("presented frame advances anim_time in tick-per-frame mode",
                repl_state_variables().anim_time > 0.0f);
}

static void test_type_keys_hook(void) {
    clear_capture_env();
    editor_input_clear();
    ASSERT_INT("input buffer initially empty", (int)strlen(editor_input_text()), 0);

    setenv("GLR_TYPE_KEYS", "xyz", 1);
    glr_capture_env_apply(NULL);
    ASSERT_TRUE("GLR_TYPE_KEYS typed into editor buffer",
                strcmp(editor_input_text(), "xyz") == 0);
}

int main(void) {
    /* GL-free REPL bootstrap: same path the --dump-* CLI flags use. */
    glr_ctrl_bootstrap_repl(NULL);
    glr_color_picker_install_host();

    /* Clear the loaded initial commands so our fed lines start at index 0. */
    glr_ctrl_reset_all();

    /* Set up viewport size for layouts. */
    ui_state_viewport_set_size(1200, 800);

    /* Give the document enough committed lines for tests. */
    editor_feed_line("glColor3f(1,0,0);");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0,0,0);");
    editor_feed_line("glEnd();");
    editor_feed_line("");

    /* One-shot frame hook tests must run first */
    test_frame_hooks_combined();

    test_apply_is_inert_when_unset();
    test_accum_passes_hook();
    test_syntax_highlight_hook();
    test_edit_line_hook();
    test_frame_hook_inert_when_unset();

    test_glr_time_hook();
    test_no_splash_hook();
    test_tick_per_frame_hook();
    test_type_keys_hook();

    clear_capture_env();
    return test_harness_report(&g_harness, "glr_capture_env");
}
