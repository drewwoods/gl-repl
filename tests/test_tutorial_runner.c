#define _DEFAULT_SOURCE  /* mkdtemp() */
#include "app/glr_actions.h"
#include "app/glr_camera.h"       /* tutorial-start camera reset assertions */
#include "app/glr_ctrl.h"
#include "app/glr_defaults.h"     /* CFG_DEFAULT_* / CFG_DEFAULT_TUTORIAL_* */
#include "editor/clipboard.h"
#include "config.h"
#include "editor/completion.h"
#include "editor/input.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "keymap.h"
#include "keys.h"
#include <stdio.h>
#include "repl/example_loader.h"
#include "repl/cfg_baseline.h"
#include "repl/host_effects.h"
#include "repl/scenes.h"
#include "repl/eval.h"            /* REQUIRE_VAR tests: predef-var declare/lookup */
#include "repl/state_owners.h"
#include "repl/state_views.h"
#include "repl/tutorials.h"
#include "render3d/themes.h"        /* GRID_THEME_*, AXES_THEME_*, RENDER3D_BACKDROP_* */
#include "source_document.h"
#include "subsystems/variable_panel/variable_panel_drag.h"   /* slider-drag plumbing for REQUIRE_VAR test */
#include "subsystems/variable_panel/variable_panel_state.h"
#include "support/test_harness.h"
#include "ui/app/state.h"
#include "subsystems/tutorial/tutorial.h"
#include "subsystems/tutorial/tutorial_state.h"

#include <math.h>     /* fabsf for REQUIRE_VAR epsilon checks */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static TestHarness g_harness = TEST_HARNESS_INIT;
static int g_mock_modifiers = 0;

static int mock_get_modifiers(void) {
    return g_mock_modifiers;
}

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

#define ASSERT_STR(label, got, exp) \
    TEST_ASSERT_STR(&g_harness, label, got, exp)

static const char *status_text(void) {
    return ui_state_status_mut()->text;
}

static void get_expected_hint(int tutorial_idx, int step, int total, int is_commit, char *out, size_t out_size) {
    const char *name = repl_tutorial_name(tutorial_idx);
    int curr = tutorial_idx + 1;
    int tot_tuts = repl_tutorial_count();
    if (is_commit) {
        snprintf(out, out_size, "%s Tutorial [%d/%d]: step %d/%d - press Enter or ';' to commit", name, curr, tot_tuts, step, total);
    } else {
        snprintf(out, out_size, "%s Tutorial [%d/%d]: step %d/%d - type the command or press Tab to autocomplete", name, curr, tot_tuts, step, total);
    }
}

static void get_expected_completion_status(char *out, size_t out_size) {
    char shortcut[KEYMAP_SHORTCUT_LABEL_MAX];
    keymap_binding_to_string(shortcut, sizeof(shortcut),
                             KM_KEY(GLR_NEXT_TUTORIAL),
                             KM_MODS(GLR_NEXT_TUTORIAL), 1);
    snprintf(out, out_size,
             "Tutorial complete - press %s to advance or edit to continue", shortcut);
}

static const char *trim_leading_ws(const char *text) {
    while (text && *text == ' ')
        text++;
    while (text && *text == '\t')
        text++;
    return text ? text : "";
}

static void set_input_text(const char *text) {
    EditorInputState *inp = editor_state_input_mut();
    size_t len = text ? strlen(text) : 0;

    if (len >= MAX_INPUT_LEN)
        len = MAX_INPUT_LEN - 1;
    if (text && len > 0)
        memcpy(inp->input, text, len);
    inp->input[len] = '\0';
    inp->input_len = (int)len;
    editor_cursor_pos_set(inp->input_len);
}

static void reset_fixture(void) {
    glr_ctrl_reset_all();
}

static void test_start_enters_transient_tutorial_scene(void) {
    SourceTextView doc;

    reset_fixture();
    repl_load_example(0);
    tutorial_start(0);

    doc = source_document_view();
    ASSERT_TRUE("tutorial active after start", tutorial_active());
    ASSERT_INT("tutorial index stored", tutorial_state_view().tutorial_idx, 0);
    ASSERT_INT("tutorial step starts at zero", tutorial_state_view().step, 0);
    ASSERT_INT("example detached", repl_state_scenes().active_example_idx, -1);
    ASSERT_INT("user scene detached", repl_active_user_scene(), -1);
    /* Rows 0-1 are the injected scene-clear prelude (a comment then its
     * glClear, both locked); row 2 is step 0's instruction comment. */
    ASSERT_INT("tutorial doc line count", doc.line_count, 3);
    ASSERT_TRUE("prelude comment is first",
                strncmp(trim_leading_ws(source_text_line(doc, 0)), "//", 2) == 0);
    ASSERT_TRUE("second line is the scene-clearing glClear",
                strstr(source_text_line(doc, 1), "glClear") != NULL);
    ASSERT_TRUE("prelude comment locked", tutorial_line_is_locked(0));
    ASSERT_TRUE("glClear line locked", tutorial_line_is_locked(1));
    ASSERT_TRUE("instruction line is comment",
                strncmp(trim_leading_ws(source_text_line(doc, 2)), "//", 2) == 0);
    ASSERT_TRUE("instruction line locked", tutorial_line_is_locked(2));
    ASSERT_STR("current expected text",
               tutorial_current_expected_text(),
               repl_tutorial_step_expected(0, 0));
}

/* A tutorial start is a fresh transient scene, so it must not inherit the
 * previous scene's view: presentation chrome goes back to CFG_DEFAULT_*,
 * the camera eases back to the built-in pose (examples deliberately
 * inherit it; tutorials do not), the grid narrows to CLOSE to frame
 * unit-scale lesson geometry, and the vertex overlays go off so the
 * lesson starts on bare geometry. "Color & Transform" ships no leading
 * `@cfg`, so nothing layers over the reset. */
static void test_start_resets_view_to_tutorial_defaults(void) {
    reset_fixture();

    /* Dirty every slug the reset owns, plus the camera. */
    glr_config_set(GLR_CONFIG_GRID_THEME, GRID_THEME_RADAR);
    glr_config_set(GLR_CONFIG_GRID_EXTENT, GRID_EXTENT_MID);
    glr_config_set(GLR_CONFIG_PROJECTION, PROJ_ORTHO);
    glr_config_set(GLR_CONFIG_BACKDROP, RENDER3D_BACKDROP_SUNSET);
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 1);
    glr_config_set(GLR_CONFIG_VERTEX_POINTS, 1);
    glr_camera_set(72.0f, -140.0f, 31.0f, 3.0f, -2.0f, 1.5f, 0.0f);
    /* Read the dirtied values back rather than assuming the writes stuck
     * verbatim — the backdrop/grid pairing policy in glr_config.c can
     * force a companion grid theme on top of what we asked for. */
    int pre_grid     = repl_cfg_get_int("grid", -1);
    int pre_extent   = repl_cfg_get_int("grid_extent", -1);
    int pre_outlines = repl_cfg_get_int("vertex_outlines", -1);
    ASSERT_TRUE("pre-tutorial grid extent is not the tutorial default",
                pre_extent != CFG_DEFAULT_TUTORIAL_GRID_EXTENT_IDX);
    ASSERT_INT("pre-tutorial vertex outlines on", pre_outlines, 1);

    tutorial_start(1);
    ASSERT_INT("no-cfg tutorial active", tutorial_active(), 1);
    ASSERT_INT("grid theme back to default",
               repl_cfg_get_int("grid", -1), CFG_DEFAULT_GRID_THEME);
    ASSERT_INT("projection back to perspective",
               repl_cfg_get_int("projection", -1), CFG_DEFAULT_PROJECTION);
    ASSERT_INT("backdrop back to default",
               repl_cfg_get_int("backdrop", -1), CFG_DEFAULT_BACKDROP_MODE);
    /* The three slugs that deliberately differ from CFG_DEFAULT_*. */
    ASSERT_INT("grid extent narrowed to close",
               repl_cfg_get_int("grid_extent", -1),
               CFG_DEFAULT_TUTORIAL_GRID_EXTENT_IDX);
    ASSERT_INT("vertex outlines off for the lesson",
               repl_cfg_get_int("vertex_outlines", -1),
               CFG_DEFAULT_TUTORIAL_VERTEX_OUTLINES);
    ASSERT_INT("vertex points off for the lesson",
               repl_cfg_get_int("vertex_points", -1),
               CFG_DEFAULT_TUTORIAL_VERTEX_POINTS);

    /* The camera eases rather than snaps, so the target pose shows up as
     * the ease destination, not (yet) the live pose: the built-in orbit
     * and target, pulled back to the tutorial distance. */
    GlrCameraState dest = glr_camera_destination();
    ASSERT_TRUE("camera eases back to default orbit",
                fabsf(dest.rx - 20.0f) < 0.001f &&
                fabsf(dest.ry - 30.0f) < 0.001f);
    ASSERT_TRUE("camera eases out to the tutorial distance",
                fabsf(dest.dist - CFG_DEFAULT_TUTORIAL_CAMERA_DIST) < 0.001f);
    ASSERT_TRUE("camera eases back to origin target",
                fabsf(dest.tx) < 0.001f && fabsf(dest.ty) < 0.001f &&
                fabsf(dest.tz) < 0.001f);
    ASSERT_INT("camera back in 3D control mode",
               (int)glr_camera_control_mode(), (int)GLR_CAMERA_CONTROL_3D);

    /* Exit leaves the lesson's view alone — the learner is still looking
     * at the tutorial's scene, so nothing should snap out from under it. */
    tutorial_stop();
    ASSERT_INT("exit keeps the tutorial's grid extent",
               repl_cfg_get_int("grid_extent", -1),
               CFG_DEFAULT_TUTORIAL_GRID_EXTENT_IDX);

    /* Every overridden slug rides the scene-subset baseline, so the
     * deferred flush puts the user's own values back rather than leaving
     * the tutorial's CLOSE grid and bare geometry behind. */
    tutorial_teardown();
    ASSERT_INT("flush restores the pre-tutorial grid extent",
               repl_cfg_get_int("grid_extent", -1), pre_extent);
    ASSERT_INT("flush restores the pre-tutorial grid theme",
               repl_cfg_get_int("grid", -1), pre_grid);
    ASSERT_INT("flush restores the pre-tutorial vertex outlines",
               repl_cfg_get_int("vertex_outlines", -1), pre_outlines);
}

static void test_catalog_includes_color_transform_tutorial(void) {
    /* "Color & Transform" exercises push/pop matrix, color, translate,
     * rotate and quads — a richer set of GL commands than the starter
     * "First Triangle". Pin both tutorials in the catalog so a future
     * catalog rewrite has to keep them. */
    ASSERT_TRUE("at least two starter tutorials available",
                repl_tutorial_count() >= 2);
    ASSERT_STR("first tutorial is First Triangle",
               repl_tutorial_name(0), "First Triangle");
    ASSERT_STR("second tutorial is Color & Transform",
               repl_tutorial_name(1), "Color & Transform");
    ASSERT_INT("color-transform tutorial has 11 steps",
               repl_tutorial_step_count(1), 11);
    ASSERT_STR("color-transform first expected command",
               repl_tutorial_step_expected(1, 0), "glPushMatrix()");
    ASSERT_STR("color-transform last expected command",
               repl_tutorial_step_expected(1, 10), "glPopMatrix()");
}

static void test_color_transform_walkthrough(void) {
    const char *expected;
    int total_steps;
    int idx;

    reset_fixture();
    tutorial_start(1);
    ASSERT_TRUE("color-transform tutorial active", tutorial_active());
    ASSERT_INT("color-transform tutorial idx",
               tutorial_state_view().tutorial_idx, 1);
    char expected_status[256];
    get_expected_hint(1, 1, 11, 0, expected_status, sizeof(expected_status));
    ASSERT_STR("color-transform start status",
               status_text(),
               expected_status);

    total_steps = repl_tutorial_step_count(1);
    for (idx = 0; idx < total_steps; idx++) {
        expected = tutorial_current_expected_text();
        ASSERT_TRUE("expected exists mid-tutorial", expected != NULL);
        set_input_text(expected);
        (void)editor_handle_key(';', 0, 0);
    }

    ASSERT_TRUE("color-transform tutorial completed", !tutorial_active());
    char expected_comp[256];
    get_expected_completion_status(expected_comp, sizeof(expected_comp));
    ASSERT_STR("color-transform completion status",
               status_text(), expected_comp);
}

static void test_runner_match_and_advance(void) {
    const char *expected;
    SourceTextView doc;
    TutorialMatchResult result;

    reset_fixture();
    tutorial_start(0);

    ASSERT_TRUE("wrong command rejected",
                !tutorial_handle_commit_attempt("glEnd()", &result));
    ASSERT_INT("wrong command mismatch kind", result.kind, TUT_MISMATCH_SHAPE);
    ASSERT_INT("step unchanged after wrong command", tutorial_state_view().step, 0);
    ASSERT_TRUE("wrong command message populated", result.message[0] != '\0');

    expected = tutorial_current_expected_text();
    ASSERT_TRUE("matching command accepted",
                tutorial_handle_commit_attempt(expected, &result));
    editor_feed_line(expected);
    tutorial_advance_after_successful_commit();

    /* Doc rows: 0-1 clear prelude, 2 step-0 comment, 3 committed glBegin,
     * 4 step-1 comment. */
    doc = source_document_view();
    ASSERT_INT("step advanced after success", tutorial_state_view().step, 1);
    ASSERT_INT("next instruction appended", doc.line_count, 5);
    ASSERT_STR("new instruction text", trim_leading_ws(source_text_line(doc, 4)),
               repl_tutorial_step_comment(0, 1));
    ASSERT_TRUE("new instruction locked", tutorial_line_is_locked(4));
    ASSERT_STR("expected text advanced",
               tutorial_current_expected_text(),
               repl_tutorial_step_expected(0, 1));
}

static void test_semicolon_route_rejects_mismatch_and_preserves_input(void) {
    SourceTextView doc;

    reset_fixture();
    tutorial_start(0);
    set_input_text("glEnd()");

    (void)editor_handle_key(';', 0, 0);

    doc = source_document_view();
    ASSERT_INT("step unchanged after semicolon mismatch",
               tutorial_state_view().step, 0);
    ASSERT_STR("semicolon mismatch status",
               status_text(), "expected: glBegin(GL_TRIANGLES)");
    ASSERT_STR("semicolon mismatch preserves input",
               editor_state_input().input, "glEnd()");
    ASSERT_INT("semicolon mismatch does not commit line",
               doc.line_count, 3);
}

static void test_enter_route_advances_after_match(void) {
    SourceTextView doc;

    reset_fixture();
    tutorial_start(0);
    set_input_text(tutorial_current_expected_text());

    (void)editor_handle_key('\n', 0, 0);

    doc = source_document_view();
    ASSERT_INT("enter route advanced step", tutorial_state_view().step, 1);
    ASSERT_INT("enter route appended instruction", doc.line_count, 5);
    ASSERT_STR("enter route next instruction text",
               trim_leading_ws(source_text_line(doc, 4)),
               repl_tutorial_step_comment(0, 1));
}

static void test_replace_existing_line_does_not_advance(void) {
    const char *expected;
    SourceTextView doc;

    reset_fixture();
    tutorial_start(0);

    /* Commit step 0 cleanly so there is an existing user line to land on. */
    set_input_text(tutorial_current_expected_text());
    (void)editor_handle_key(';', 0, 0);
    ASSERT_INT("step advanced after first commit", tutorial_state_view().step, 1);

    /* Navigate back to the previously-committed user line (row 3: rows 0-1
     * are the injected clear prelude, row 2 is step 0's comment) and try to
     * commit the current step's expected text. The precheck must reject the
     * non-append commit so the user does not overwrite prior progress while
     * also advancing. */
    editor_state_edit_line_set(3);
    expected = tutorial_current_expected_text();
    set_input_text(expected);
    (void)editor_handle_key(';', 0, 0);

    doc = source_document_view();
    ASSERT_INT("step unchanged after non-append commit",
               tutorial_state_view().step, 1);
    ASSERT_STR("non-append commit status",
               status_text(),
               "Move cursor to the tutorial insertion line");
    ASSERT_STR("non-append commit preserves input",
               editor_state_input().input, expected);
    ASSERT_INT("non-append commit does not append a new line",
               doc.line_count, 5);
}

static void test_shadow_suffix_strict_prefix(void) {
    char buf[128];

    reset_fixture();
    tutorial_start(0);

    /* Empty input is the empty prefix - shadow returns the full expected
     * line so the user can see the full hint before typing anything. */
    ASSERT_TRUE("empty input yields full expected as shadow",
                tutorial_shadow_suffix("", buf, sizeof(buf)) == 1);
    ASSERT_STR("empty-input shadow equals full expected",
               buf, "glBegin(GL_TRIANGLES)");

    /* Typed prefix - shadow returns only the untyped suffix. */
    ASSERT_TRUE("prefix input yields untyped suffix",
                tutorial_shadow_suffix("glBegin", buf, sizeof(buf)) == 1);
    ASSERT_STR("prefix shadow is suffix",
               buf, "(GL_TRIANGLES)");

    /* Fully-typed input - shadow returns an empty suffix but still
     * reports success so the caller can clear any prior ghost. */
    ASSERT_TRUE("fully-typed input yields empty suffix",
                tutorial_shadow_suffix("glBegin(GL_TRIANGLES)",
                                       buf, sizeof(buf)) == 1);
    ASSERT_STR("fully-typed shadow is empty", buf, "");

    /* Non-prefix input - shadow returns 0 and clears `out`. The user
     * has gone off-script; Tab autofill is the recovery path. */
    ASSERT_TRUE("non-prefix input yields no shadow",
                tutorial_shadow_suffix("glEnd", buf, sizeof(buf)) == 0);
    ASSERT_STR("non-prefix shadow is empty after rejection", buf, "");
}

static void test_shadow_suffix_inactive_returns_zero(void) {
    char buf[128];

    reset_fixture();
    ASSERT_TRUE("inactive tutorial yields no shadow",
                tutorial_shadow_suffix("anything", buf, sizeof(buf)) == 0);
    ASSERT_STR("inactive shadow is empty", buf, "");
}

static void test_shadow_text_populates_autocomplete_ghost(void) {
    /* The autocomplete provider mirrors tutorial shadow text into
     * autocomplete.ghost so the existing input-row ghost render path
     * draws it dimmed after the cursor. */
    const EditorAutocompleteState *ac;

    reset_fixture();
    tutorial_start(0);
    set_input_text("glBe");
    editor_completion_update();

    ac = editor_state_autocomplete();
    ASSERT_STR("autocomplete ghost carries tutorial suffix",
               ac->ghost, "gin(GL_TRIANGLES)");
    ASSERT_INT("autocomplete suppresses match list during tutorial",
               ac->match_count, 0);
    ASSERT_STR("autocomplete suppresses param hint during tutorial",
               ac->hint, "");
}

static void test_shadow_text_appears_immediately_on_start(void) {
    /* Regression: tutorial_start must poke editor_completion_update()
     * itself so the shadow ghost appears on the first frame instead
     * of waiting for the user's next keystroke. */
    const EditorAutocompleteState *ac;

    reset_fixture();
    tutorial_start(0);
    /* No keystroke, no manual editor_completion_update() — just read. */
    ac = editor_state_autocomplete();
    ASSERT_STR("ghost populated immediately after tutorial_start",
               ac->ghost, "glBegin(GL_TRIANGLES)");
}

static void test_shadow_text_refreshes_on_advance(void) {
    /* Regression: tutorial_advance_after_successful_commit must
     * refresh autocomplete so the next step's shadow appears on the
     * very next frame, not after the user types again. */
    const EditorAutocompleteState *ac;

    reset_fixture();
    tutorial_start(0);
    set_input_text(tutorial_current_expected_text());
    (void)editor_handle_key(';', 0, 0);

    ac = editor_state_autocomplete();
    /* Derived from the catalog: this asserts the ghost tracks the step,
     * not what the lesson's second command happens to be. */
    ASSERT_STR("ghost shows next step's expected after advance",
               ac->ghost, repl_tutorial_step_expected(0, 1));
}

static void test_shadow_ghost_falls_through_off_expected_line(void) {
    /* On the expected commit line, the tutorial shadow suffix takes
     * over autocomplete. On any other line the user is editing
     * unrelated code, so normal autocomplete should run. */
    const EditorAutocompleteState *ac;
    int expected_line;

    reset_fixture();
    tutorial_start(0);
    expected_line = tutorial_expected_commit_line();
    ASSERT_TRUE("expected_commit_line off line zero", expected_line != 0);

    editor_state_edit_line_set(0);
    set_input_text("glC");
    editor_completion_update();

    ac = editor_state_autocomplete();
    ASSERT_TRUE("normal autocomplete produces matches off-line",
                ac->match_count > 0);
    ASSERT_TRUE("ghost is not the tutorial expected text",
                strcmp(ac->ghost, "glBegin(GL_TRIANGLES)") != 0);
}

static void test_ghost_reappears_on_return_to_expected_line(void) {
    /* Navigating to a non-tutorial line clears the shadow ghost so
     * stale text doesn't follow the cursor. Navigating back to the
     * expected commit line should restore the ghost without
     * requiring the user to type. */
    const EditorAutocompleteState *ac;
    int expected_line;

    reset_fixture();
    tutorial_start(0);
    expected_line = tutorial_expected_commit_line();
    ASSERT_TRUE("expected_commit_line off line zero", expected_line != 0);

    ac = editor_state_autocomplete();
    ASSERT_STR("ghost shows on the expected line at start",
               ac->ghost, "glBegin(GL_TRIANGLES)");

    editor_navigate_to_line(0);
    ac = editor_state_autocomplete();
    ASSERT_STR("ghost clears after navigating off-line",
               ac->ghost, "");

    editor_navigate_to_line(expected_line);
    ac = editor_state_autocomplete();
    ASSERT_STR("ghost restored after returning to expected line",
               ac->ghost, "glBegin(GL_TRIANGLES)");
}

static void test_tab_skips_tutorial_autofill_off_expected_line(void) {
    /* Tab on the expected commit line autofills the next expected
     * step. On any other line Tab should defer to normal autocomplete
     * instead of overwriting the user's unrelated input. */
    int expected_line;

    reset_fixture();
    tutorial_start(0);
    expected_line = tutorial_expected_commit_line();
    ASSERT_TRUE("expected_commit_line off line zero", expected_line != 0);

    editor_state_edit_line_set(0);
    set_input_text("glC");

    (void)editor_handle_key('\t', 0, 0);

    ASSERT_TRUE("tab did not autofill expected on off-line",
                strcmp(editor_state_input().input,
                       "glBegin(GL_TRIANGLES)") != 0);
}

static void test_shadow_text_clears_on_exit(void) {
    /* Regression: tutorial_stop must refresh autocomplete so the
     * ghost from the in-progress step clears immediately. */
    const EditorAutocompleteState *ac;

    reset_fixture();
    tutorial_start(0);
    tutorial_stop();

    ac = editor_state_autocomplete();
    ASSERT_STR("ghost clears on tutorial_stop", ac->ghost, "");
}

static void test_tutorial_start_sets_step_progress_status(void) {
    char expected_status[256];
    reset_fixture();
    tutorial_start(0);
    get_expected_hint(0, 1, 5, 0, expected_status, sizeof(expected_status));
    ASSERT_STR("start sets step 1 status",
               status_text(),
               expected_status);
}

static void test_tutorial_advance_updates_step_progress_status(void) {
    const char *expected;
    char expected_status[256];

    reset_fixture();
    tutorial_start(0);
    expected = tutorial_current_expected_text();
    set_input_text(expected);
    (void)editor_handle_key(';', 0, 0);
    get_expected_hint(0, 2, 5, 0, expected_status, sizeof(expected_status));
    ASSERT_STR("advance sets step 2 status",
               status_text(),
               expected_status);
}

/* tutorial_status_hint computes which COMMAND-step hint variant the
 * controller's per-frame refresh should keep visible. Inactive +
 * non-COMMAND steps return 0 (the slot is free for other status
 * messages); COMMAND steps return 1 with the entry hint by default and
 * the commit reminder when the input on the expected commit line is a
 * full match. tutorial_status_is_hint recognises any "Tutorial: step "
 * prefix so the controller can distinguish "my hint" from foreign
 * status writes. */
static void test_tutorial_status_hint_variants(void) {
    char buf[REPL_STATUS_TEXT_MAX];
    char expected_entry[256];
    char expected_commit[256];
    int got;

    get_expected_hint(0, 1, 5, 0, expected_entry, sizeof(expected_entry));
    get_expected_hint(0, 1, 5, 1, expected_commit, sizeof(expected_commit));

    /* Inactive: returns 0 with empty out. */
    reset_fixture();
    buf[0] = 'x';
    got = tutorial_status_hint(buf, sizeof buf);
    ASSERT_INT("inactive returns 0", got, 0);
    ASSERT_STR("inactive clears out", buf, "");

    /* COMMAND step entry: returns the entry hint variant. */
    tutorial_start(0);
    got = tutorial_status_hint(buf, sizeof buf);
    ASSERT_INT("COMMAND entry returns 1", got, 1);
    ASSERT_STR("COMMAND entry uses 'type or Tab' variant",
               buf,
               expected_entry);

    /* COMMAND step, full match on expected line: commit reminder. */
    set_input_text(tutorial_current_expected_text());
    got = tutorial_status_hint(buf, sizeof buf);
    ASSERT_INT("COMMAND match returns 1", got, 1);
    ASSERT_STR("COMMAND match uses commit reminder",
               buf,
               expected_commit);

    /* tutorial_status_is_hint recognises the prefix on either variant
     * and rejects unrelated text. */
    ASSERT_INT("entry hint recognised",
               tutorial_status_is_hint(expected_entry),
               1);
    ASSERT_INT("commit hint recognised",
               tutorial_status_is_hint(expected_commit),
               1);
    ASSERT_INT("non-tutorial status not recognised",
               tutorial_status_is_hint("Saved to output.c"),
               0);
    ASSERT_INT("NULL not recognised",
               tutorial_status_is_hint(NULL),
               0);
}

/* tutorial_refresh_input_hint flips the status to the "ready to commit"
 * reminder when the input fully matches the expected command, and is a
 * no-op for inactive / partial input. The completion provider calls it
 * on every input-change refresh while the cursor is on the expected
 * commit line; here we exercise it directly. */
static void test_tutorial_refresh_input_hint_on_full_match(void) {
    const char *expected;
    char expected_entry[256];
    char expected_commit[256];

    get_expected_hint(0, 1, 5, 0, expected_entry, sizeof(expected_entry));
    get_expected_hint(0, 1, 5, 1, expected_commit, sizeof(expected_commit));

    /* Active: empty input is a no-op (status keeps the entry hint). */
    reset_fixture();
    tutorial_start(0);
    tutorial_refresh_input_hint("");
    ASSERT_STR("empty input does not overwrite entry status",
               status_text(),
               expected_entry);

    /* Active: a strict prefix of expected is also a no-op. */
    expected = tutorial_current_expected_text();
    ASSERT_TRUE("expected exists", expected != NULL);
    set_input_text("glBeg");
    tutorial_refresh_input_hint("glBeg");
    ASSERT_STR("partial input does not overwrite entry status",
               status_text(),
               expected_entry);

    /* Active: full match refreshes status with the commit reminder. */
    set_input_text(expected);
    tutorial_refresh_input_hint(expected);
    ASSERT_STR("full match sets commit reminder",
               status_text(),
               expected_commit);

    /* Inactive: never writes. Park a sentinel status, exit the tutorial,
     * and confirm the call leaves it intact. */
    tutorial_stop();
    repl_set_status("sentinel");
    tutorial_refresh_input_hint("glBegin(GL_TRIANGLES)");
    ASSERT_STR("inactive call is a no-op", status_text(), "sentinel");
}

static void test_locked_comment_load_is_read_only(void) {
    reset_fixture();
    tutorial_start(0);
    set_input_text("glEnd()");

    editor_load_line_to_input(0);

    ASSERT_INT("locked line load clears input len",
               editor_state_input().input_len, 0);
    ASSERT_STR("locked line load clears input text",
               editor_state_input().input, "");
    ASSERT_STR("locked line load status",
               status_text(), "Tutorial line is read-only");
}

static void test_locked_comment_mutations_are_blocked(void) {
    reset_fixture();
    tutorial_start(0);
    editor_state_edit_line_set(0);

    (void)editor_handle_key(KEY_CTRL_D, 0, 0);
    ASSERT_INT("ctrl-d keeps locked comment row",
               repl_state_document_count(), 3);
    ASSERT_STR("ctrl-d read-only status",
               status_text(), "Tutorial line is read-only");

    (void)editor_handle_key(KEY_CTRL_L, 0, 0);
    ASSERT_INT("ctrl-l keeps tutorial rows",
               repl_state_document_count(), 3);
    ASSERT_STR("ctrl-l read-only status",
               status_text(), "Tutorial line is read-only");

    (void)editor_handle_key(KEY_CTRL_BACKSLASH, 0, 0);
    ASSERT_INT("ctrl-backslash keeps tutorial rows",
               repl_state_document_count(), 3);
    ASSERT_STR("ctrl-backslash read-only status",
               status_text(), "Tutorial line is read-only");
}

static void test_paste_before_locked_prefix_is_blocked(void) {
    const char *expected;
    EditorUndoRingState ring_before, ring_after;

    reset_fixture();
    tutorial_start(0);
    expected = tutorial_current_expected_text();
    set_input_text(expected);
    (void)editor_handle_key(';', 0, 0);

    /* Copy the committed user line (row 3, after the 2-row clear prelude
     * and step 0's comment), then aim the paste at the locked row 0. */
    editor_state_edit_line_set(3);
    editor_clipboard_copy_current();
    editor_state_edit_line_set(0);

    /* Regression: a guard-rejected paste must not touch the undo/redo
     * rings. editor_undo_push_snapshot() (saves a snapshot, bumps
     * undo_count, zeroes the redo ring) used to run BEFORE the
     * read-only guard in editor_clipboard_paste_current(), so a blocked
     * paste silently pushed a phantom undo and destroyed the redo
     * stack with the document unchanged. */
    editor_undo_ring_state_capture(&ring_before);
    editor_clipboard_paste_current();
    editor_undo_ring_state_capture(&ring_after);

    ASSERT_INT("paste before locked prefix keeps line count",
               repl_state_document_count(), 5);
    ASSERT_STR("paste before locked prefix status",
               status_text(), "Tutorial line is read-only");
    ASSERT_INT("blocked paste pushes no phantom undo",
               ring_after.undo_count, ring_before.undo_count);
    ASSERT_INT("blocked paste does not clear redo ring",
               ring_after.redo_count, ring_before.redo_count);
}

static void test_undo_redo_blocked_during_tutorial(void) {
    reset_fixture();
    tutorial_start(0);

    (void)editor_handle_key(KEY_CTRL_Z, 0, 0);
    ASSERT_STR("undo blocked status",
               status_text(), "Undo disabled during tutorial");

    (void)editor_handle_key(KEY_CTRL_Y, 0, 0);
    ASSERT_STR("redo blocked status",
               status_text(), "Undo disabled during tutorial");
}

static void test_navigation_rejects_non_matching_input(void) {
    SourceTextView doc_before;
    SourceTextView doc_after;

    reset_fixture();
    tutorial_start(0);
    /* Commit step 0 first so there's a non-locked user line to navigate to;
     * landing on a locked instruction would have editor_load_line_to_input
     * overwrite the precheck status. */
    set_input_text(tutorial_current_expected_text());
    (void)editor_handle_key(';', 0, 0);
    doc_before = source_document_view();

    /* Type a parseable but non-matching line at the trailing edit row,
     * then navigate to the non-locked user line (row 3, after the 2-row
     * clear prelude and step 0's comment). Without the navigation-path
     * tutorial gate, commit_before_navigation would slip the line in
     * without advancing the step. */
    set_input_text("glPointSize(1)");
    editor_navigate_to_line(3);

    doc_after = source_document_view();
    ASSERT_INT("navigation does not commit non-matching line",
               doc_after.line_count, doc_before.line_count);
    ASSERT_INT("step unchanged after rejected navigation",
               tutorial_state_view().step, 1);
    /* Derived from the catalog: the assertion is that the hint quotes the
     * step's expected text, not what that lesson's step 1 happens to be. */
    {
        char want[MAX_LINE_LEN];
        snprintf(want, sizeof want, "expected: %s",
                 repl_tutorial_step_expected(0, 1));
        ASSERT_STR("navigation rejection surfaces hint status",
                   status_text(), want);
    }
}

static void test_navigation_advances_on_matching_input(void) {
    SourceTextView doc;
    const char *expected;

    reset_fixture();
    tutorial_start(0);
    /* Commit step 0 cleanly so navigation lands on a non-locked user line. */
    set_input_text(tutorial_current_expected_text());
    (void)editor_handle_key(';', 0, 0);

    /* Type the current expected text at trailing edit row, navigate up to
     * the committed user line (row 3, after the 2-row clear prelude and
     * step 0's comment). The navigation commit should advance the tutorial. */
    expected = tutorial_current_expected_text();
    set_input_text(expected);
    editor_navigate_to_line(3);

    char expected_status[256];
    doc = source_document_view();
    ASSERT_INT("navigation advance committed user line + next instruction",
               doc.line_count, 7);
    ASSERT_INT("step advanced via navigation", tutorial_state_view().step, 2);
    get_expected_hint(0, 3, 5, 0, expected_status, sizeof(expected_status));
    ASSERT_STR("navigation advance sets step 3 status",
               status_text(),
               expected_status);
}

static void test_enter_on_locked_line_shows_position_hint(void) {
    /* Regression: an earlier review flagged that
     * commit_current_input's unmodified+enter_mode branch could toggle
     * insert mode at a locked line. With the Phase 3 precheck,
     * landing on a locked line first triggers editor_load_line_to_input,
     * which clears the input and sets the read-only status; the
     * Enter that follows hits the precheck's empty-input silent
     * reject, so the read-only status stays visible. The step
     * neither advances nor enters insert mode. */
    reset_fixture();
    tutorial_start(0);
    set_input_text(tutorial_current_expected_text());
    (void)editor_handle_key(';', 0, 0);

    editor_navigate_to_line(0);
    (void)editor_handle_key('\n', 0, 0);

    ASSERT_STR("enter on locked line shows read-only status",
               status_text(),
               "Tutorial line is read-only");
    ASSERT_TRUE("enter on locked line did not enter insert mode",
                !editor_insert_mode());
    ASSERT_INT("enter on locked line did not advance step",
               tutorial_state_view().step, 1);
}

static void test_ctrl_slash_on_locked_line_is_blocked(void) {
    int saved_modifiers = g_mock_modifiers;

    editor_input_set_modifier_provider_for_test(mock_get_modifiers);
    editor_set_line_comment_prefix("// ");
    reset_fixture();
    tutorial_start(0);
    editor_navigate_to_line(0);

    g_mock_modifiers = GLUT_ACTIVE_CTRL;
    (void)editor_handle_key('/', 0, 0);
    g_mock_modifiers = saved_modifiers;
    editor_input_set_modifier_provider_for_test(NULL);

    ASSERT_INT("ctrl-/ keeps tutorial comment row",
               repl_state_document_count(), 3);
    ASSERT_STR("ctrl-/ read-only status",
               status_text(), "Tutorial line is read-only");
}

static void test_tab_autofill_then_semicolon_advances(void) {
    const char *expected;
    SourceTextView doc;

    reset_fixture();
    tutorial_start(0);
    set_input_text("glEnd()");
    expected = tutorial_current_expected_text();

    (void)editor_handle_key('\t', 0, 0);

    ASSERT_STR("tab autofill replaces input",
               editor_state_input().input, expected);
    ASSERT_INT("tab autofill updates input len",
               editor_state_input().input_len, (int)strlen(expected));
    ASSERT_STR("tab autofill status",
               status_text(),
               "Replaced input with expected tutorial command; press ; to commit");

    (void)editor_handle_key(';', 0, 0);

    doc = source_document_view();
    ASSERT_INT("tab then semicolon advances step",
               tutorial_state_view().step, 1);
    ASSERT_INT("tab then semicolon appends instruction",
               doc.line_count, 5);
}

static void test_rejected_commit_does_not_advance_tutorial(void) {
    /* Filling the document via editor_feed_line (which intentionally
     * bypasses the tutorial precheck) leaves the editor cursor at
     * the trailing row while expected_commit_line still points at
     * the row immediately below the first instruction. The Phase 3
     * precheck rejects the mismatched cursor, so the step doesn't
     * advance and the user's typed input is preserved — the same
     * guarantee the original capacity-rejection test was after,
     * achieved one rung earlier in the pipeline. */
    reset_fixture();
    tutorial_start(0);

    for (int i = repl_state_document_count(); i < MAX_EDITOR_COMMANDS; i++)
        editor_feed_line("glPointSize(1);");

    ASSERT_INT("document filled to capacity",
               repl_state_document_count(), MAX_EDITOR_COMMANDS);

    set_input_text(tutorial_current_expected_text());
    (void)editor_handle_key(';', 0, 0);

    ASSERT_INT("step unchanged after rejected commit",
               tutorial_state_view().step, 0);
    ASSERT_STR("position-mismatch status surfaces from precheck",
               status_text(),
               "Move cursor to the tutorial insertion line");
    ASSERT_STR("rejected commit keeps expected input",
               editor_state_input().input,
               repl_tutorial_step_expected(0, 0));
}

static void test_feed_line_alone_does_not_advance_tutorial(void) {
    const char *expected;

    reset_fixture();
    tutorial_start(0);

    expected = tutorial_current_expected_text();
    editor_feed_line(expected);

    ASSERT_INT("step unchanged after direct feed line", tutorial_state_view().step, 0);
    ASSERT_TRUE("tutorial still active after direct feed line", tutorial_active());
}

static void test_loading_example_exits_tutorial(void) {
    reset_fixture();
    tutorial_start(0);

    repl_load_example(0);

    ASSERT_TRUE("example load exits tutorial", !tutorial_active());
}

static void test_loading_workspace_exits_tutorial(void) {
    char temp_dir[] = "/tmp/test_tutorial_workspace.XXXXXX";
    char *made_dir;

    reset_fixture();
    tutorial_start(0);

    made_dir = mkdtemp(temp_dir);
    ASSERT_TRUE("mkdtemp tutorial workspace", made_dir != NULL);
    if (!made_dir)
        return;

    ASSERT_INT("empty workspace load succeeds",
               repl_load_workspace(made_dir), 0);
    ASSERT_TRUE("workspace load exits tutorial", !tutorial_active());

    rmdir(made_dir);
}

static void test_invalid_workspace_load_does_not_exit_tutorial(void) {
    reset_fixture();
    tutorial_start(0);

    ASSERT_INT("null workspace load fails", repl_load_workspace(NULL), 0);
    ASSERT_TRUE("tutorial survives invalid workspace load", tutorial_active());
}

static void test_invalid_user_scene_load_does_not_exit_tutorial(void) {
    reset_fixture();
    tutorial_start(0);

    ASSERT_INT("invalid scene load fails", repl_load_user_scene_idx(-1), 0);
    ASSERT_TRUE("tutorial survives invalid scene load", tutorial_active());
}

static void test_fade_duration_math(void) {
    TutorialRuntimeState state;

    reset_fixture();
    tutorial_start(0);
    state = tutorial_state_view();

    ASSERT_TRUE("tutorial active after start", state.active);
    /* Step 0's instruction comment lands at row 2, below the 2-row
     * scene-clear prelude; the fade animates that freshly-inserted row. */
    ASSERT_INT("fade line idx is the step-0 instruction row",
               state.fade_line_idx, 2);
    /* Duration is now derived from the comment's length at a fixed
     * chars-per-second rate. Use the catalog's comment string (the same
     * input the emit code measures) — `source_text_line` may differ by
     * a couple chars after loader normalisation, which would skew the
     * comparison. Round to milliseconds for a stable integer check. */
    {
        const char *cmt = repl_tutorial_step_comment(0, 0);
        int n = cmt ? (int)strlen(cmt) : 1;
        float expected = (float)(n + TUTORIAL_FADE_SETTLE_CHARS) /
                         TUTORIAL_FADE_CHARS_PER_SEC;
        ASSERT_INT("fade duration follows chars-per-sec rate",
                   (int)(state.fade_duration * 1000.0f),
                   (int)(expected * 1000.0f));
    }
}

static void test_complete_and_menu_actions(void) {
    const char *expected;
    int tag_count;

    reset_fixture();
    /* After Phase B's hierarchical menu, top-level MENU_TUTORIALS rows
     * are tag rows (inert) + trailing Restart/Exit; tutorial activation
     * itself flows through route_submenu_item_hit → tutorial_start,
     * which is REPL-side and tested directly here. The menu-action
     * route only owns Restart/Exit at the top level. */
    tutorial_start(0);
    ASSERT_TRUE("tutorial active after tutorial_start", tutorial_active());

    expected = tutorial_current_expected_text();
    editor_feed_line(expected);
    tutorial_advance_after_successful_commit();
    tag_count = repl_tutorial_visible_tag_count();
    ASSERT_TRUE("restart menu item restarts tutorial",
                glr_action_menu_item_activate(GLR_MENU_TUTORIALS, tag_count + GLR_TUTORIAL_OFF_RESTART) == 1);
    ASSERT_INT("restart resets step", tutorial_state_view().step, 0);

    while (tutorial_active()) {
        expected = tutorial_current_expected_text();
        ASSERT_TRUE("expected exists while tutorial active", expected != NULL);
        editor_feed_line(expected);
        tutorial_advance_after_successful_commit();
    }

    char expected_comp[256];
    get_expected_completion_status(expected_comp, sizeof(expected_comp));
    ASSERT_STR("completion status set", status_text(), expected_comp);

    /* After completion no tutorial is active, so the trailing Restart/Exit
     * rows don't exist. The MENU_TUTORIALS activation handler still has a
     * catch-all `return 1` at the bottom, so out-of-range indices report
     * "handled" (the original behavior — preserved for non-regression). */
    ASSERT_TRUE("exit menu item accepted when inactive",
                glr_action_menu_item_activate(
                    GLR_MENU_TUTORIALS,
                    repl_tutorial_visible_tag_count() + GLR_TUTORIAL_OFF_EXIT) == 1);
}

static void test_start_leaves_unsaved_buffer_transient(void) {
    reset_fixture();
    /* Type a line into a fresh buffer (no example, no user scene) so the
     * pre-tutorial state lives only in the live document. */
    editor_feed_line("glBegin(GL_TRIANGLES);");
    ASSERT_INT("user typed line is in document",
               repl_state_document_count(), 1);

    tutorial_start(0);
    ASSERT_INT("tutorial start does not create a user scene",
               repl_user_scene_count(), 0);
    ASSERT_TRUE("tutorial active after start", tutorial_active());
}

static void test_catalog_starter_steps_are_append(void) {
    /* Phase 1: the original append-only starter tutorials migrated
     * to TutorialStep records should still report append placement
     * across all their steps with no label or target_label. The
     * Depth Test Triangle tutorial added in Phase 2 is intentionally
     * excluded — it carries a label and a label-targeted step. */
    const char *const append_only[] = { "First Triangle", "Color & Transform" };
    for (size_t k = 0; k < sizeof(append_only)/sizeof(append_only[0]); k++) {
        int t_idx = -1;
        for (int i = 0; i < repl_tutorial_count(); i++) {
            const char *name = repl_tutorial_name(i);
            if (name && strcmp(name, append_only[k]) == 0) {
                t_idx = i;
                break;
            }
        }
        ASSERT_TRUE("append-only starter tutorial is in catalog", t_idx >= 0);
        if (t_idx < 0)
            continue;
        int n = repl_tutorial_step_count(t_idx);
        for (int s = 0; s < n; s++) {
            ASSERT_INT("starter step is append placement",
                       repl_tutorial_step_placement(t_idx, s),
                       TUTORIAL_STEP_APPEND);
            ASSERT_TRUE("starter step has no label",
                        repl_tutorial_step_label(t_idx, s) == NULL ||
                        repl_tutorial_step_label(t_idx, s)[0] == '\0');
            ASSERT_TRUE("starter step has no target_label",
                        repl_tutorial_step_target_label(t_idx, s) == NULL ||
                        repl_tutorial_step_target_label(t_idx, s)[0] == '\0');
        }
    }
}

static void test_catalog_cfg_lines(void) {
    /* First Triangle ships a leading `@cfg view_mode = RENDER3D_VIEW_2D` so the
     * flat triangle renders in true 2D. First Animation pauses auto_time so
     * Ctrl+T visibly starts its cube. Phase C adds cfg carriers for its 2D,
     * backdrop, and auto-time demonstrations. All other shipped tutorials
     * omit cfg (NULL = no presentation overrides). Out-of-range idx → NULL. */
    int first = -1, first_animation = -1;
    for (int i = 0; i < repl_tutorial_count(); i++) {
        const char *name = repl_tutorial_name(i);
        if (name && strcmp(name, "First Triangle") == 0) { first = i; break; }
    }
    ASSERT_TRUE("First Triangle is in catalog", first >= 0);
    if (first >= 0) {
        const char *const *cfg = repl_tutorial_cfg_lines(first);
        ASSERT_TRUE("First Triangle has cfg lines", cfg != NULL);
        if (cfg) {
            ASSERT_TRUE("First Triangle cfg first line is view_mode = RENDER3D_VIEW_2D",
                        cfg[0] != NULL &&
                        strstr(cfg[0], "view_mode") != NULL &&
                        strstr(cfg[0], "RENDER3D_VIEW_2D") != NULL);
            ASSERT_TRUE("First Triangle cfg is NULL-terminated after 1 line",
                        cfg[1] == NULL);
        }
    }

    for (int i = 0; i < repl_tutorial_count(); i++) {
        const char *name = repl_tutorial_name(i);
        if (name && strcmp(name, "First Animation") == 0) {
            first_animation = i;
            break;
        }
    }
    ASSERT_TRUE("First Animation is in catalog", first_animation >= 0);
    if (first_animation >= 0) {
        const char *const *cfg = repl_tutorial_cfg_lines(first_animation);
        ASSERT_TRUE("First Animation has cfg lines", cfg != NULL);
        if (cfg) {
            ASSERT_TRUE("First Animation cfg pauses auto_time",
                        cfg[0] != NULL &&
                        strstr(cfg[0], "auto_time = 0") != NULL);
            ASSERT_TRUE("First Animation cfg is NULL-terminated after 1 line",
                        cfg[1] == NULL);
        }
    }

    /* Tutorials that opt into entry-level @cfg list them by name here; the
     * rest must leave cfg NULL so the field stays genuinely opt-in. */
    for (int i = 0; i < repl_tutorial_count(); i++) {
        const char *name = repl_tutorial_name(i);
        if (name && (strcmp(name, "First Triangle") == 0 ||
                     strcmp(name, "Feature Tour") == 0 ||
                     strcmp(name, "First Animation") == 0 ||
                     strcmp(name, "Points & Lines") == 0 ||
                     strcmp(name, "Line Stipple") == 0 ||
                     strcmp(name, "Blending & Transparency") == 0 ||
                     strcmp(name, "Fog") == 0 ||
                     strcmp(name, "Bitmap Text") == 0 ||
                     strcmp(name, "If & Conditionals") == 0))
            continue;
        ASSERT_TRUE("tutorials without an entry-level @cfg have NULL cfg",
                    repl_tutorial_cfg_lines(i) == NULL);
    }

    ASSERT_TRUE("out-of-range tutorial cfg is NULL",
                repl_tutorial_cfg_lines(repl_tutorial_count()) == NULL);
    ASSERT_TRUE("negative tutorial cfg is NULL",
                repl_tutorial_cfg_lines(-1) == NULL);
}

/* Mirror of test_example_tag_metadata in tests/test_repl_core_examples.c.
 * Sanity-checks the tutorial tag system end-to-end: tag count, label table,
 * mask/has_tag/count_for_tag/index_for_tag mutual agreement, bounds, the
 * visible-tag count, and that every shipped catalog entry carries a
 * non-zero mask whose bits all map to known tags. Also asserts that the
 * known multi-tag entry ("Depth Test Triangle" → GEOMETRY|DEPTH_LIGHTING)
 * is reachable under each of its tags via index_for_tag — the equivalent
 * of examples' Stress-test multi-tag assertion. */
static void test_catalog_tag_metadata(void) {
    int tag_count = repl_tutorial_tag_count();
    int tutorial_count = repl_tutorial_count();
    unsigned int known_tag_bits = 0u;

    ASSERT_TRUE("tutorial tag count positive", tag_count > 0);
    for (int tag_idx = 0; tag_idx < tag_count; tag_idx++) {
        char label[128];
        const char *tag_label = repl_tutorial_tag_label(tag_idx);
        int count;

        snprintf(label, sizeof(label), "tutorial tag %d label", tag_idx);
        ASSERT_TRUE(label, tag_label != NULL && tag_label[0] != '\0');
        known_tag_bits |= repl_tutorial_tag_bit(tag_idx);

        count = repl_tutorial_count_for_tag(tag_idx);
        for (int ordinal = 0; ordinal < count; ordinal++) {
            int tutorial_idx = repl_tutorial_index_for_tag(tag_idx, ordinal);
            snprintf(label, sizeof(label), "tag %d ordinal %d maps valid",
                     tag_idx, ordinal);
            ASSERT_TRUE(label,
                        tutorial_idx >= 0 && tutorial_idx < tutorial_count);
            snprintf(label, sizeof(label), "tag %d ordinal %d has tag",
                     tag_idx, ordinal);
            ASSERT_TRUE(label, repl_tutorial_has_tag(tutorial_idx, tag_idx));
        }
    }

    ASSERT_TRUE("invalid negative tag bit", repl_tutorial_tag_bit(-1) == 0u);
    ASSERT_TRUE("invalid high tag bit",
                repl_tutorial_tag_bit(repl_tutorial_tag_count()) == 0u);
    ASSERT_TRUE("visible tag count within tag count",
                repl_tutorial_visible_tag_count() <= tag_count);
    for (int dense_idx = 0;
         dense_idx < repl_tutorial_visible_tag_count();
         dense_idx++) {
        char label[128];
        int tag_idx = repl_tutorial_visible_tag_at(dense_idx);
        snprintf(label, sizeof(label), "visible tag %d maps valid", dense_idx);
        ASSERT_TRUE(label, tag_idx >= 0 && tag_idx < tag_count);
        snprintf(label, sizeof(label), "visible tag %d has tutorials",
                 dense_idx);
        ASSERT_TRUE(label, repl_tutorial_count_for_tag(tag_idx) > 0);
    }

    for (int idx = 0; idx < tutorial_count; idx++) {
        char label[160];
        unsigned int mask = repl_tutorial_tag_mask(idx);

        /* Every entry has at least the synthetic ALL bit folded in, so
         * an accidentally zero-mask entry (the most likely regression
         * when adding new tutorials) still surfaces here. */
        snprintf(label, sizeof(label), "tutorial %d tag mask nonzero", idx);
        ASSERT_TRUE(label, mask != 0u);
        snprintf(label, sizeof(label), "tutorial %d tag mask known bits", idx);
        ASSERT_TRUE(label, (mask & ~known_tag_bits) == 0u);
        for (int tag_idx = 0; tag_idx < tag_count; tag_idx++) {
            int expected = (mask & repl_tutorial_tag_bit(tag_idx)) != 0u;
            snprintf(label, sizeof(label), "tutorial %d tag %d agreement",
                     idx, tag_idx);
            ASSERT_TRUE(label,
                        repl_tutorial_has_tag(idx, tag_idx) == expected);
        }
    }

    /* Known multi-tag entry: Depth Test Triangle ships under both
     * GEOMETRY and DEPTH_LIGHTING. Must be discoverable from each. */
    int depth_idx = -1;
    for (int i = 0; i < tutorial_count; i++) {
        const char *name = repl_tutorial_name(i);
        if (name && strcmp(name, "Depth Test Triangle") == 0) {
            depth_idx = i;
            break;
        }
    }
    ASSERT_TRUE("known multi-tag tutorial found", depth_idx >= 0);
    if (depth_idx >= 0) {
        int hits = 0;
        for (int tag_idx = 0; tag_idx < tag_count; tag_idx++) {
            int found_under_tag = 0;
            if (!repl_tutorial_has_tag(depth_idx, tag_idx))
                continue;
            hits++;
            for (int ordinal = 0;
                 ordinal < repl_tutorial_count_for_tag(tag_idx);
                 ordinal++) {
                if (repl_tutorial_index_for_tag(tag_idx, ordinal) == depth_idx) {
                    found_under_tag = 1;
                    break;
                }
            }
            ASSERT_TRUE("multi-tag tutorial discoverable under assigned tag",
                        found_under_tag);
        }
        /* hits counts ALL + GEOMETRY + DEPTH_LIGHTING = 3 minimum. */
        ASSERT_TRUE("multi-tag tutorial has multiple tags", hits > 1);
    }
}

/* Subheading axis: each tutorial declares an optional free-form section
 * label (`TutorialEntry.subheading`); the Tutorials menu groups
 * consecutive tutorials sharing a subheading under a `### subheading`
 * chrome row in the per-tag flyout. Test invariants:
 *   - Every subheading is either NULL or a non-empty string (the menu
 *     stripping logic would render an empty string as zero-width chrome).
 *   - The getter returns NULL for out-of-range indices.
 *   - Per tag, every non-NULL subheading appears in a single contiguous
 *     run of tutorials (matches the menu walker's emit rule — one
 *     header per group; interleaving would render duplicate headers).
 *   - At least one shipped tutorial has a non-NULL subheading so the
 *     menu walker's HEADER path is actually exercised in production.
 *   - The known multi-tag entry (Depth Test Triangle) shows up in
 *     every one of its tag flyouts under the same subheading. */
static void test_catalog_subheading_metadata(void) {
    int tag_count = repl_tutorial_tag_count();
    int tutorial_count = repl_tutorial_count();

    /* Every subheading is either NULL or non-empty. */
    for (int idx = 0; idx < tutorial_count; idx++) {
        char label[128];
        const char *sub = repl_tutorial_subheading(idx);
        snprintf(label, sizeof(label),
                 "tutorial %d subheading is NULL or non-empty", idx);
        ASSERT_TRUE(label, !sub || sub[0] != '\0');
    }
    ASSERT_TRUE("out-of-range tutorial subheading is NULL",
                repl_tutorial_subheading(tutorial_count) == NULL);
    ASSERT_TRUE("negative tutorial subheading is NULL",
                repl_tutorial_subheading(-1) == NULL);

    /* At least one tutorial in the shipped catalog declares a
     * subheading — otherwise the menu's HEADER path is dead code. */
    int has_any_subheading = 0;
    for (int idx = 0; idx < tutorial_count; idx++) {
        if (repl_tutorial_subheading(idx)) { has_any_subheading = 1; break; }
    }
    ASSERT_TRUE("catalog ships at least one subheading", has_any_subheading);

    /* Per tag: walk in catalog order, count distinct non-NULL
     * subheadings (set semantics) vs the number of subheading-change
     * transitions the menu walker would emit. Equality means each
     * distinct subheading appears in a single contiguous run; a
     * mismatch means an interleaved subheading would render its
     * header twice (e.g. catalog order "Beginner, Intermediate,
     * Beginner" would emit two "Beginner" rows). */
    for (int t = 0; t < tag_count; t++) {
        char label[128];
        const char *seen[16];
        int seen_count = 0;
        const char *prev = NULL;
        int transitions = 0;
        int n = repl_tutorial_count_for_tag(t);
        for (int o = 0; o < n; o++) {
            int tut_idx = repl_tutorial_index_for_tag(t, o);
            const char *sub = repl_tutorial_subheading(tut_idx);
            if (!sub)
                continue;
            int already_seen = 0;
            for (int s = 0; s < seen_count; s++) {
                if (strcmp(seen[s], sub) == 0) { already_seen = 1; break; }
            }
            if (!already_seen &&
                seen_count < (int)(sizeof(seen) / sizeof(seen[0]))) {
                seen[seen_count++] = sub;
            }
            int header_here = !prev || strcmp(prev, sub) != 0;
            if (header_here)
                transitions++;
            prev = sub;
        }
        snprintf(label, sizeof(label),
                 "tag %d subheadings are contiguous (no interleaving)", t);
        ASSERT_INT(label, transitions, seen_count);
    }

    /* Known multi-tag entry: Depth Test Triangle. Its subheading must
     * be non-NULL (we currently ship "Intermediate") and the same
     * value must surface under every tag it carries. */
    int depth_idx = -1;
    for (int i = 0; i < tutorial_count; i++) {
        const char *name = repl_tutorial_name(i);
        if (name && strcmp(name, "Depth Test Triangle") == 0) {
            depth_idx = i;
            break;
        }
    }
    ASSERT_TRUE("Depth Test Triangle in catalog", depth_idx >= 0);
    if (depth_idx >= 0) {
        const char *expected_sub = repl_tutorial_subheading(depth_idx);
        ASSERT_TRUE("Depth Test Triangle has a subheading",
                    expected_sub != NULL);
        for (int t = 0; t < tag_count; t++) {
            if (!repl_tutorial_has_tag(depth_idx, t))
                continue;
            int n = repl_tutorial_count_for_tag(t);
            int found = 0;
            for (int o = 0; o < n; o++) {
                int tut_idx = repl_tutorial_index_for_tag(t, o);
                if (tut_idx != depth_idx)
                    continue;
                const char *sub = repl_tutorial_subheading(tut_idx);
                if (sub && expected_sub && strcmp(sub, expected_sub) == 0)
                    found = 1;
                break;
            }
            ASSERT_TRUE("multi-tag entry has same subheading in every tag",
                        found);
        }
    }
}

static void test_catalog_validation_passes_for_all_tutorials(void) {
    /* Phase 1: every shipped catalog entry must validate. */
    for (int t = 0; t < repl_tutorial_count(); t++) {
        char err[160] = "";
        int ok = repl_tutorial_validate(t, err, sizeof(err));
        ASSERT_TRUE("shipped tutorial validates", ok);
        if (!ok) {
            /* surface the diagnostic so test output explains the
             * failure even when the assert collapses to 0/1. */
            ASSERT_STR("validation err empty on success", err, "");
        }
    }
}

static void test_catalog_rejects_out_of_range_index(void) {
    char err[160] = "";
    int ok = repl_tutorial_validate(repl_tutorial_count(), err, sizeof(err));
    ASSERT_TRUE("out-of-range tutorial idx fails validation", !ok);
    ASSERT_TRUE("out-of-range tutorial idx has diagnostic",
                err[0] != '\0');
}

static void test_validate_rejects_duplicate_label(void) {
    static const TutorialStep dup_steps[] = {
        { "lbl", "// one", "glPointSize(1)", TUTORIAL_STEP_APPEND, NULL },
        { "lbl", "// two", "glPointSize(2)", TUTORIAL_STEP_APPEND, NULL },
        { NULL,  NULL,     NULL,             TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry entry = { .name = "dup_labels", .steps = dup_steps };
    char err[160] = "";
    int ok = repl_tutorial_validate_entry(&entry, err, sizeof(err));
    ASSERT_TRUE("duplicate label rejected", !ok);
    ASSERT_TRUE("duplicate label diagnostic mentions 'duplicate'",
                strstr(err, "duplicate") != NULL);
}

static void test_validate_rejects_missing_target_label(void) {
    static const TutorialStep missing_steps[] = {
        { NULL, "// one", "glPointSize(1)",
          TUTORIAL_STEP_LABEL, "no_such_label" },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry entry = { .name = "missing_target", .steps = missing_steps };
    char err[160] = "";
    int ok = repl_tutorial_validate_entry(&entry, err, sizeof(err));
    ASSERT_TRUE("missing target_label rejected", !ok);
    ASSERT_TRUE("missing target_label diagnostic mentions the label",
                strstr(err, "no_such_label") != NULL);
}

static void test_validate_rejects_forward_reference(void) {
    static const TutorialStep fwd_steps[] = {
        { NULL,    "// targets the later step",
                   "glPointSize(1)", TUTORIAL_STEP_LABEL, "later" },
        { "later", "// gets labeled later",
                   "glPointSize(2)", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry entry = { .name = "forward_ref", .steps = fwd_steps };
    char err[160] = "";
    int ok = repl_tutorial_validate_entry(&entry, err, sizeof(err));
    ASSERT_TRUE("forward reference rejected", !ok);
}

static void test_validate_rejects_multi_row_expected(void) {
    /* Semicolons inside `expected` are interpreted as statement
     * separators and would expand the catalog row into multiple
     * source rows on commit. */
    static const TutorialStep semi_steps[] = {
        { NULL, "// double commit",
                "glPointSize(1); glPointSize(2)",
                TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry entry_semi = { .name = "multi_stmt", .steps = semi_steps };
    char err[160] = "";
    ASSERT_TRUE("expected with ';' rejected",
                !repl_tutorial_validate_entry(&entry_semi, err, sizeof(err)));

    /* Block opens are now a legal shape (TUTORIAL_EXPECTED_BLOCK_OPEN),
     * but only in balanced sequences: an open with no matching close
     * step still rejects (tutorial would end inside the block). Braces
     * in ORDINARY-shaped text stay rejected outright — see
     * test_validate_block_step_rules for the full block matrix. */
    static const TutorialStep brace_steps[] = {
        { NULL, "// opens a block",
                "for(i, 0, 3) {",
                TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry entry_brace = { .name = "block_open", .steps = brace_steps };
    err[0] = '\0';
    ASSERT_TRUE("unclosed block open rejected",
                !repl_tutorial_validate_entry(&entry_brace, err, sizeof(err)));
    ASSERT_TRUE("unclosed block diagnostic mentions the open block",
                strstr(err, "open block") != NULL);

    /* Multi-name float decls expand into one CMD_VAR_DECLARE per
     * name, which is several source rows. */
    static const TutorialStep multi_decl_steps[] = {
        { NULL, "// declares multiple",
                "float a, b, c",
                TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry entry_decl = { .name = "multi_decl",
                                 .steps = multi_decl_steps };
    err[0] = '\0';
    ASSERT_TRUE("multi-name float decl rejected",
                !repl_tutorial_validate_entry(&entry_decl, err, sizeof(err)));

    /* Single-name float decls also rejected: even though they
     * parse to one source command, the commit path relocates
     * CMD_VAR_DECLARE rows to the top of non-decl code, so the
     * pending bookkeeping cannot trust pending.commit_line for
     * label resolution. */
    static const TutorialStep single_decl_steps[] = {
        { NULL, "// declares one",
                "float x",
                TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry entry_single_decl = { .name = "single_decl",
                                        .steps = single_decl_steps };
    err[0] = '\0';
    ASSERT_TRUE("single-name float decl rejected",
                !repl_tutorial_validate_entry(&entry_single_decl,
                                              err, sizeof(err)));
    ASSERT_TRUE("single-name float decl diagnostic mentions 'float'",
                strstr(err, "float") != NULL);
}

static void test_append_first_expected_commit_line_is_trailing_row(void) {
    /* Phase 2: append tutorials should set expected_commit_line to
     * the trailing row of the document immediately after start. */
    reset_fixture();
    tutorial_start(0);

    TutorialRuntimeState state = tutorial_state_view();
    ASSERT_INT("expected_commit_line is trailing row",
               state.expected_commit_line,
               repl_state_document_count());
}

static int g_depth_tutorial_idx_cached = -1;

static int depth_tutorial_idx(void) {
    if (g_depth_tutorial_idx_cached >= 0)
        return g_depth_tutorial_idx_cached;
    for (int i = 0; i < repl_tutorial_count(); i++) {
        const char *name = repl_tutorial_name(i);
        if (name && strcmp(name, "Depth Test Triangle") == 0) {
            g_depth_tutorial_idx_cached = i;
            return i;
        }
    }
    return -1;
}

static void test_depth_tutorial_label_targeted_step_inserts_above_label(void) {
    /* Phase 2 (post-fix): the label-targeted splice for step 5
     * should land ABOVE the original (instruction, command) pair
     * for step 0 — keeping the original step-0 instruction comment
     * directly above its glBegin command rather than orphaning
     * them. Use editor_feed_line +
     * tutorial_advance_after_successful_commit since the Phase 3
     * precheck has not landed yet and editor_feed_line is the existing
     * way these tests step through append commits. */
    int t_idx = depth_tutorial_idx();
    ASSERT_TRUE("Depth Test Triangle present in catalog", t_idx >= 0);
    if (t_idx < 0)
        return;

    reset_fixture();
    tutorial_start(t_idx);

    /* The labeled step (index 0) emits its instruction at row 2, below
     * the 2-row scene-clear prelude; record that anchor before the
     * appends. */
    int instruction_row = tutorial_state_view().instruction_line_for_step[0];
    ASSERT_INT("step 0 instruction recorded below the clear prelude",
               instruction_row, 2);

    for (int s = 0; s < 5; s++) {
        const char *expected = tutorial_current_expected_text();
        editor_feed_line(expected);
        tutorial_advance_after_successful_commit();
    }

    /* We are now on the label-targeted step. */
    int t_count = repl_tutorial_step_count(t_idx);
    ASSERT_INT("walk advanced to label-targeted step",
               tutorial_state_view().step, 5);
    ASSERT_TRUE("more steps left after walking through append",
                t_count >= 6);

    /* The new step-5 instruction should land at the recorded
     * instruction row (which has stayed at 2 because every prior
     * append went strictly below it). The original step-0
     * instruction shifted to row 3 and the originally-labeled
     * glBegin shifted to row 4 — keeping the (instruction,
     * command) pair adjacent, still below the row 0-1 clear prelude. */
    SourceTextView doc = source_document_view();
    const char *new_instruction = source_text_line(doc, 2);
    const char *orig_instruction = source_text_line(doc, 3);
    const char *labeled_line = source_text_line(doc, 4);
    ASSERT_TRUE("new instruction comment lands at the splice row",
                new_instruction &&
                strstr(new_instruction,
                       "Enable depth testing before the triangle") != NULL);
    ASSERT_TRUE("original step-0 instruction shifted but stays adjacent",
                orig_instruction &&
                strstr(orig_instruction,
                       "Start the triangle batch") != NULL);
    ASSERT_TRUE("originally-labeled command kept directly below its instruction",
                labeled_line &&
                strstr(labeled_line, "glBegin(GL_TRIANGLES)") != NULL);

    ASSERT_TRUE("new instruction row is locked",
                tutorial_line_is_locked(2));
    ASSERT_INT("expected_commit_line lands directly below new instruction",
               tutorial_state_view().expected_commit_line, 3);
    ASSERT_TRUE("editor cursor moved to expected commit line",
                editor_state_edit_line() ==
                    tutorial_state_view().expected_commit_line);
    ASSERT_TRUE("editor in insert mode since expected row is mid-document",
                editor_insert_mode() != 0);
}

static void walk_depth_tutorial_to_label_step(void) {
    int t_idx = depth_tutorial_idx();
    if (t_idx < 0)
        return;
    reset_fixture();
    tutorial_start(t_idx);
    for (int s = 0; s < 5; s++) {
        const char *expected = tutorial_current_expected_text();
        set_input_text(expected);
        (void)editor_handle_key(';', 0, 0);
    }
}

static void test_phase3_label_targeted_commit_inserts_above_label(void) {
    /* Phase 3: with the precheck + guard exception in place, the
     * user can actually commit the label-targeted step's expected
     * command at expected_commit_line and the runner advances.
     * Walks the depth-test tutorial via the real keyboard route
     * (which exercises the precheck) rather than editor_feed_line. */
    int t_idx = depth_tutorial_idx();
    if (t_idx < 0)
        return;

    walk_depth_tutorial_to_label_step();
    ASSERT_INT("walked to label-targeted step",
               tutorial_state_view().step, 5);

    int target_line = tutorial_state_view().expected_commit_line;
    ASSERT_TRUE("expected_commit_line is set on label step",
                target_line >= 0);

    const char *expected = tutorial_current_expected_text();
    ASSERT_STR("expected for label step is glEnable",
               expected, "glEnable(GL_DEPTH_TEST)");
    set_input_text(expected);
    (void)editor_handle_key(';', 0, 0);

    /* Step advanced past the label-targeted step (or completed). */
    ASSERT_TRUE("step advanced or tutorial completed",
                tutorial_state_view().step != 5 || !tutorial_active());

    SourceTextView doc = source_document_view();
    const char *committed = source_text_line(doc, target_line);
    const char *orig_instr = source_text_line(doc, target_line + 1);
    const char *glbegin   = source_text_line(doc, target_line + 2);
    ASSERT_TRUE("committed glEnable lands at the expected commit line",
                committed && strstr(committed, "glEnable(GL_DEPTH_TEST)") != NULL);
    ASSERT_TRUE("original step-0 instruction stays adjacent to its command",
                orig_instr &&
                strstr(orig_instr, "Start the triangle batch") != NULL);
    ASSERT_TRUE("originally-labeled glBegin sits directly below its instruction",
                glbegin && strstr(glbegin, "glBegin(GL_TRIANGLES)") != NULL);
}

static void test_phase3_wrong_input_at_label_step_does_not_insert(void) {
    int t_idx = depth_tutorial_idx();
    if (t_idx < 0)
        return;

    walk_depth_tutorial_to_label_step();
    int target_line = tutorial_state_view().expected_commit_line;
    SourceTextView doc_before = source_document_view();
    int step_before = tutorial_state_view().step;

    set_input_text("glPointSize(2)");
    (void)editor_handle_key(';', 0, 0);

    SourceTextView doc_after = source_document_view();
    ASSERT_INT("wrong input on label step does not insert",
               doc_after.line_count, doc_before.line_count);
    ASSERT_INT("wrong input does not advance step",
               tutorial_state_view().step, step_before);
    ASSERT_STR("wrong input keeps user text",
               editor_state_input().input, "glPointSize(2)");
    /* Status surfaces the expected hint. */
    ASSERT_TRUE("wrong-input status starts with 'expected:'",
                strncmp(status_text(), "expected:", 9) == 0);
    (void)target_line;  /* keep variable for symmetry with happy-path test */
}

static void test_phase3_correct_input_at_wrong_line_does_not_insert(void) {
    int t_idx = depth_tutorial_idx();
    if (t_idx < 0)
        return;

    walk_depth_tutorial_to_label_step();
    int target_line = tutorial_state_view().expected_commit_line;
    SourceTextView doc_before = source_document_view();
    int step_before = tutorial_state_view().step;

    /* Move the cursor off the expected line (to the trailing row)
     * but keep input correct. The precheck should reject. */
    editor_state_edit_line_set(repl_state_document_count());
    editor_insert_mode_set(0);
    set_input_text("glEnable(GL_DEPTH_TEST)");
    (void)editor_handle_key(';', 0, 0);

    SourceTextView doc_after = source_document_view();
    ASSERT_INT("correct input at wrong line does not insert",
               doc_after.line_count, doc_before.line_count);
    ASSERT_INT("correct input at wrong line does not advance",
               tutorial_state_view().step, step_before);
    ASSERT_STR("position-mismatch status surfaces",
               status_text(),
               "Move cursor to the tutorial insertion line");
    (void)target_line;
}

static void test_phase3_empty_input_silent_reject(void) {
    /* Phase 3: pressing ; or Enter with an empty input on the
     * expected line should silently reject — no status update. */
    reset_fixture();
    tutorial_start(0);
    repl_set_status("baseline");

    EditorInputState *inp = editor_state_input_mut();
    inp->input[0] = '\0';
    inp->input_len = 0;

    (void)editor_handle_key(';', 0, 0);
    ASSERT_STR("empty ;-commit leaves status untouched",
               status_text(), "baseline");

    (void)editor_handle_key('\n', 0, 0);
    ASSERT_STR("empty Enter-commit leaves status untouched",
               status_text(), "baseline");
}

static void test_phase3_pending_clears_after_match_failure(void) {
    /* Phase 3 invariant: every begin pairs with exactly one note/
     * cancel. Trigger a precheck match-pass (so _begin runs) then
     * force the editor commit to fail — pending must reset to -1. */
    reset_fixture();
    tutorial_start(0);

    /* Fill the buffer to capacity so the eventual commit fails. */
    for (int i = repl_state_document_count(); i < MAX_EDITOR_COMMANDS; i++)
        editor_feed_line("glPointSize(1);");

    /* Move cursor to expected_commit_line so the position check
     * passes; the matcher should accept the expected text. The
     * capacity check in the editor commit will then fail, and
     * tutorial_cancel_pending in commit_before_navigation's
     * REJECTED branch (or the ;-route's REJECTED case) is supposed
     * to clear the pending record. */
    int expected_line = tutorial_state_view().expected_commit_line;
    editor_state_edit_line_set(expected_line);
    editor_insert_mode_set(expected_line < repl_state_document_count());
    set_input_text(tutorial_current_expected_text());
    (void)editor_handle_key(';', 0, 0);

    ASSERT_INT("pending.step_idx reset after rejected commit",
               tutorial_state_view().pending.step_idx, -1);
}

static void test_review_guard_blocks_expected_commit_line_without_pending(void) {
    /* Review fix: tutorial_guard_source_change must block any
     * mutation at expected_commit_line that did NOT come from the
     * matched precheck (i.e., did not stamp pending). The original
     * locked-line-prefix check only catches positions <= a locked
     * row, so a label-targeted step whose anchor sits below every
     * locked instruction would let an untracked paste through.
     *
     * Drive the guard directly with a crafted state: locked at row
     * 0, expected_commit_line at row 5, no later locked rows. This
     * isolates the new guard branch from the rest of the runner. */
    reset_fixture();
    tutorial_start(0);

    TutorialRuntimeState *state = tutorial_state_mut();
    state->locked_line_count = 1;
    state->locked_lines[0] = 0;
    state->expected_commit_line = 5;
    state->pending.step_idx = -1;
    state->pending.commit_line = -1;

    /* Without the new guard branch this would return 1 (allow): pos
     * 5 is greater than the only locked row 0 and pending is
     * inactive, so the historical locked-line-prefix scan accepted
     * it. The new check rejects pos == expected_commit_line when
     * pending isn't matched. */
    int allowed = tutorial_guard_source_change(/*pos=*/5,
                                               /*delete_count=*/0,
                                               /*insert_count=*/1);
    ASSERT_TRUE("guard rejects insert at expected_commit_line without pending",
                !allowed);

    /* Sanity: with pending stamped and pos == pending.commit_line,
     * the guard's existing exception still allows the matched
     * commit through. */
    state->pending.step_idx = state->step;
    state->pending.commit_line = 5;
    state->pending.doc_count_before = repl_state_document_count();
    int allowed_with_pending =
        tutorial_guard_source_change(/*pos=*/5,
                                     /*delete_count=*/0,
                                     /*insert_count=*/1);
    ASSERT_TRUE("guard allows pending-matched insert at the same row",
                allowed_with_pending);

    /* And inserting at a different row with pending still set
     * (pos != pending.commit_line) reverts to the same blocked
     * behavior — pending only authorizes one specific row. */
    int allowed_other = tutorial_guard_source_change(/*pos=*/6,
                                                     /*delete_count=*/0,
                                                     /*insert_count=*/1);
    ASSERT_TRUE("pending does not authorize a different row",
                /* row 6 is not == expected_commit_line (5) and not
                 * <= any locked row (0); allowed before, allowed now */
                allowed_other);
}

static void test_phase4_depth_tutorial_catalog_shape(void) {
    /* Phase 4: pin the worked label-targeted tutorial's catalog
     * shape so future catalog edits can't silently drop the
     * label-targeted step or rename the label. */
    int t_idx = depth_tutorial_idx();
    ASSERT_TRUE("Depth Test Triangle is in the catalog", t_idx >= 0);
    if (t_idx < 0)
        return;

    ASSERT_STR("Depth tutorial name",
               repl_tutorial_name(t_idx), "Depth Test Triangle");
    ASSERT_INT("Depth tutorial step count",
               repl_tutorial_step_count(t_idx), 6);

    /* Step 0: labeled append, the glBegin batch opener. */
    ASSERT_STR("Step 0 expected is glBegin(GL_TRIANGLES)",
               repl_tutorial_step_expected(t_idx, 0),
               "glBegin(GL_TRIANGLES)");
    ASSERT_STR("Step 0 label is 'triangle_begin'",
               repl_tutorial_step_label(t_idx, 0), "triangle_begin");
    ASSERT_INT("Step 0 placement is append",
               repl_tutorial_step_placement(t_idx, 0),
               TUTORIAL_STEP_APPEND);

    /* Step 5: label-targeted insertion of glEnable(GL_DEPTH_TEST). */
    ASSERT_INT("Step 5 placement is label-targeted",
               repl_tutorial_step_placement(t_idx, 5),
               TUTORIAL_STEP_LABEL);
    ASSERT_STR("Step 5 target_label is 'triangle_begin'",
               repl_tutorial_step_target_label(t_idx, 5),
               "triangle_begin");
    ASSERT_STR("Step 5 expected is glEnable(GL_DEPTH_TEST)",
               repl_tutorial_step_expected(t_idx, 5),
               "glEnable(GL_DEPTH_TEST)");
}

static void test_phase4_full_walk_places_setup_before_batch(void) {
    /* Phase 4: walk the full Depth Test Triangle tutorial through
     * the real keyboard route and assert the inserted setup
     * command (glEnable) sits BEFORE the originally-committed
     * glBegin row in the final source order. */
    int t_idx = depth_tutorial_idx();
    if (t_idx < 0)
        return;

    reset_fixture();
    tutorial_start(t_idx);

    int total = repl_tutorial_step_count(t_idx);
    for (int s = 0; s < total; s++) {
        const char *expected = tutorial_current_expected_text();
        ASSERT_TRUE("expected exists during walk", expected != NULL);
        if (!expected)
            return;
        set_input_text(expected);
        (void)editor_handle_key(';', 0, 0);
    }

    ASSERT_TRUE("tutorial completed", !tutorial_active());
    char expected_comp[256];
    get_expected_completion_status(expected_comp, sizeof(expected_comp));
    ASSERT_STR("tutorial completion status",
               status_text(), expected_comp);

    /* Now scan the document and assert the glEnable line sits at a
     * lower index than the glBegin line — the whole point of the
     * label-targeted step. */
    SourceTextView doc = source_document_view();
    int glenable_row = -1;
    int glbegin_row  = -1;
    for (int i = 0; i < doc.line_count; i++) {
        const char *line = source_text_line(doc, i);
        if (!line) continue;
        if (glenable_row < 0 && strstr(line, "glEnable(GL_DEPTH_TEST)"))
            glenable_row = i;
        if (glbegin_row < 0 && strstr(line, "glBegin(GL_TRIANGLES)"))
            glbegin_row = i;
    }
    ASSERT_TRUE("glEnable row found", glenable_row >= 0);
    ASSERT_TRUE("glBegin row found", glbegin_row >= 0);
    ASSERT_TRUE("glEnable lands before glBegin in the final document",
                glenable_row >= 0 && glbegin_row >= 0 &&
                glenable_row < glbegin_row);

    /* The original step-0 (instruction, command) pair must remain
     * adjacent — the label-targeted splice anchors above the
     * instruction comment, not between the comment and its
     * command. */
    int begin_instr_row = -1;
    for (int i = 0; i < doc.line_count; i++) {
        const char *line = source_text_line(doc, i);
        if (line && strstr(line, "Start the triangle batch")) {
            begin_instr_row = i;
            break;
        }
    }
    ASSERT_TRUE("step-0 instruction comment still in document",
                begin_instr_row >= 0);
    ASSERT_TRUE("step-0 instruction sits directly above its glBegin",
                begin_instr_row >= 0 && glbegin_row == begin_instr_row + 1);
}

static void test_phase3_paste_above_locked_still_blocked(void) {
    /* Phase 3 guard must keep paste / Ctrl-D / Ctrl-/ blocked
     * above locked comments, since the guard exception is scoped
     * to the in-flight matched expected commit. */
    reset_fixture();
    tutorial_start(0);
    /* Commit step 0 so there's a non-locked user line we can copy. */
    set_input_text(tutorial_current_expected_text());
    (void)editor_handle_key(';', 0, 0);

    editor_state_edit_line_set(1);
    editor_clipboard_copy_current();
    editor_state_edit_line_set(0);
    editor_clipboard_paste_current();
    ASSERT_STR("paste above locked still rejected",
               status_text(), "Tutorial line is read-only");
}

static void test_depth_tutorial_label_targeted_emit_shifts_prior_locked_lines(void) {
    /* Phase 2: the locked instruction comments for steps 0-4 should
     * shift to keep pointing at the same source content after the
     * step-5 label-targeted insertion shoves rows down by one. */
    int t_idx = depth_tutorial_idx();
    if (t_idx < 0)
        return;

    reset_fixture();
    tutorial_start(t_idx);

    /* Walk the first five append commits, recording the document
     * snapshot before the label-targeted insert fires. */
    for (int s = 0; s < 5; s++) {
        const char *expected = tutorial_current_expected_text();
        editor_feed_line(expected);
        tutorial_advance_after_successful_commit();
    }

    /* Every previously locked instruction row should still point at
     * a comment line that starts with "//". */
    TutorialRuntimeState state = tutorial_state_view();
    SourceTextView doc = source_document_view();
    for (int i = 0; i < state.locked_line_count; i++) {
        int line = state.locked_lines[i];
        ASSERT_TRUE("locked line index in range",
                    line >= 0 && line < doc.line_count);
        if (line < 0 || line >= doc.line_count)
            continue;
        const char *text = source_text_line(doc, line);
        /* Locked rows are the instruction comments plus the injected
         * scene-clear prelude (its comment and the glClear it describes);
         * the glClear is the one locked row that isn't a `//` comment. */
        ASSERT_TRUE("locked line still points at a tutorial comment or the clear",
                    text != NULL &&
                    (strstr(text, "//") != NULL ||
                     strstr(text, "glClear") != NULL));
    }

    /* instruction_line_for_step now records each step's INSTRUCTION
     * COMMENT row (not the committed command row), so a label
     * target resolves above the original (instruction, command)
     * pair and keeps it adjacent. Step 0's instruction starts with
     * "// Start the triangle batch...". */
    int begin_step_instruction = state.instruction_line_for_step[0];
    ASSERT_TRUE("instruction_line_for_step[0] points at the comment",
                begin_step_instruction >= 0 &&
                begin_step_instruction < doc.line_count &&
                source_text_line(doc, begin_step_instruction) &&
                strstr(source_text_line(doc, begin_step_instruction),
                       "Start the triangle batch") != NULL);
}

static void test_validate_accepts_well_formed_label_tutorial(void) {
    static const TutorialStep ok_steps[] = {
        { "open_batch", "// open the batch",
                        "glBegin(GL_TRIANGLES)",
                        TUTORIAL_STEP_APPEND, NULL },
        { NULL,         "// add a vertex",
                        "glVertex3f(0, 0.5, 0)",
                        TUTORIAL_STEP_APPEND, NULL },
        { NULL,         "// later: enable depth testing before open_batch",
                        "glEnable(GL_DEPTH_TEST)",
                        TUTORIAL_STEP_LABEL, "open_batch" },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry entry = { .name = "label_ok", .steps = ok_steps };
    char err[160] = "";
    ASSERT_TRUE("well-formed label-targeted tutorial validates",
                repl_tutorial_validate_entry(&entry, err, sizeof(err)));
    ASSERT_STR("err empty on success", err, "");
}

static void test_start_rejects_out_of_range_idx(void) {
    reset_fixture();
    tutorial_start(repl_tutorial_count());

    ASSERT_TRUE("tutorial inactive after bad index",
                !tutorial_active());
    ASSERT_STR("bad index status set",
               status_text(), "Tutorial index out of range");
}

/* ------------------------------------------------------------------------- */
/* New step kinds (SET / REQUIRE), cfg notify hook, mutation guard,          */
/* restore-on-teardown.                                                      */
/* ------------------------------------------------------------------------- */

static int find_tutorial_idx(const char *name) {
    for (int i = 0; i < repl_tutorial_count(); i++) {
        const char *n = repl_tutorial_name(i);
        if (n && strcmp(n, name) == 0) return i;
    }
    return -1;
}

static int commit_command_step(int idx, int step);

/* First Animation keeps the built-in `t` clock paused while its two
 * commands are entered, then enables Auto time only when Ctrl+T is pressed.
 * This protects the tutorial's core visual promise: the shortcut starts the
 * motion rather than pausing an already moving cube. */
static void test_first_animation_starts_on_ctrl_t(void) {
    reset_fixture();

    int idx = find_tutorial_idx("First Animation");
    ASSERT_TRUE("First Animation is in catalog", idx >= 0);
    if (idx < 0) return;

    ASSERT_TRUE("First Animation is tagged Animation",
                repl_tutorial_has_tag(idx, REPL_TUTORIAL_TAG_ANIMATION));
    ASSERT_INT("First Animation has intro, two commands, a REQUIRE, and a closing NOTE",
               repl_tutorial_step_count(idx), 5);
    const char *const *cfg = repl_tutorial_cfg_lines(idx);
    ASSERT_TRUE("First Animation pauses Auto time in its cfg",
                cfg != NULL && cfg[0] != NULL &&
                strstr(cfg[0], "auto_time = 0") != NULL);

    tutorial_start(idx);
    ASSERT_TRUE("First Animation starts", tutorial_active());
    ASSERT_INT("Auto time starts paused", repl_state_variables().time_playing, 0);
    ASSERT_INT("First Animation opens with a NOTE",
               (int)tutorial_current_step_kind(),
               (int)TUTORIAL_STEP_KIND_NOTE);

    glr_ctrl_keyboard('\r', 0, 0);
    ASSERT_TRUE("intro acknowledgement reaches rotation command",
                tutorial_current_expected_text() != NULL);
    ASSERT_STR("rotation command uses the t clock",
               tutorial_current_expected_text(),
               "glRotatef(t * 45, 0, 1, 0)");
    ASSERT_TRUE("rotation command commits",
                commit_command_step(idx, tutorial_state_view().step));
    ASSERT_STR("cube command follows rotation",
               tutorial_current_expected_text(), "glutSolidCube(1)");
    ASSERT_TRUE("cube command commits",
                commit_command_step(idx, tutorial_state_view().step));

    const TutorialStep *step = repl_tutorial_step_get(
        idx, tutorial_state_view().step);
    ASSERT_TRUE("motion-start step is present", step != NULL);
    if (!step) return;
    ASSERT_INT("motion-start step requires a config action", (int)step->kind,
               (int)TUTORIAL_STEP_KIND_REQUIRE);
    ASSERT_STR("motion-start step watches Auto time", step->cfg_slug, "auto_time");
    ASSERT_INT("motion-start step waits for Auto time on", step->cfg_value, 1);
    ASSERT_INT("motion-start step advertises Ctrl+T", step->comment_binding_key,
               KEY_CTRL_T);

    glr_ctrl_keyboard(KEY_CTRL_T, 0, 0);
    ASSERT_TRUE("Ctrl+T leaves the tutorial open to show the motion",
                tutorial_active());
    ASSERT_INT("Ctrl+T leaves time playing", repl_state_variables().time_playing, 1);
    ASSERT_INT("Ctrl+T advances to the closing NOTE",
               (int)tutorial_current_step_kind(),
               (int)TUTORIAL_STEP_KIND_NOTE);
    int time_idx = repl_eval_find_predef_var_idx("t");
    ASSERT_TRUE("the built-in t variable exists", time_idx >= 0);
    if (time_idx >= 0) {
        float time_before_tick = repl_eval_predef_view().vars[time_idx].value;
        glr_ctrl_tick();
        ASSERT_TRUE("the clock advances while the final NOTE is displayed",
                    repl_eval_predef_view().vars[time_idx].value > time_before_tick);
    }

    glr_ctrl_keyboard('\r', 0, 0);
    ASSERT_TRUE("closing acknowledgement completes First Animation",
                !tutorial_active());
}

/* Drive one COMMAND step's commit via the ; key route. Returns 1 on success
 * (step advanced), 0 otherwise. */
static int commit_command_step(int idx, int step) {
    const char *expected = repl_tutorial_step_expected(idx, step);
    if (!expected) return 0;
    set_input_text(expected);
    editor_handle_key(';', 0, 0);
    return tutorial_state_view().step > step;
}

/* Phase C ships 15 catalog entries spanning ordinary commands, setup
 * scaffolds, label-targeted inserts, block opens/branches/closes, SET and
 * REQUIRE steps, named function calls, and scratch-array assignments. Walk
 * every one through the real controller/editor routes so authoring mistakes
 * cannot leave a visible tutorial that validates structurally but fails when
 * the learner types the advertised command. */
static int phase_c_complete_current_step(void) {
    TutorialRuntimeState before = tutorial_state_view();
    const TutorialStep *step = repl_tutorial_step_get(before.tutorial_idx,
                                                      before.step);
    char input[128];

    if (!step)
        return 0;
    if (step->kind == TUTORIAL_STEP_KIND_COMMAND) {
        set_input_text(step->expected);
        editor_handle_key(';', 0, 0);
    } else if (step->kind == TUTORIAL_STEP_KIND_NOTE ||
               step->kind == TUTORIAL_STEP_KIND_SET) {
        glr_ctrl_keyboard('\r', 0, 0);
    } else if (step->kind == TUTORIAL_STEP_KIND_REQUIRE) {
        int target = step->cfg_value;
        if (step->cfg_value_name &&
            !repl_cfg_resolve_text(step->cfg_slug, step->cfg_value_name,
                                   &target))
            return 0;
        repl_cfg_set_int(step->cfg_slug, target);
        tutorial_notify_state_changed();
    } else if (step->kind == TUTORIAL_STEP_KIND_REQUIRE_VAR) {
        if (repl_eval_find_predef_var_idx(step->var_name) < 0) {
            snprintf(input, sizeof(input), "float %s = %g",
                     step->var_name, (double)step->var_target);
        } else {
            snprintf(input, sizeof(input), "%s = %g",
                     step->var_name, (double)step->var_target);
        }
        set_input_text(input);
        editor_handle_key(';', 0, 0);
    } else {
        return 0;
    }

    return !tutorial_active() ||
           tutorial_state_view().step != before.step;
}

static void test_phase_c_catalog_full_walk(void) {
    static const char *const names[] = {
        "Points & Lines",
        "GLUT Solids Tour",
        "First Loop",
        "Line Stipple",
        "Blending & Transparency",
        "Depth Mask & Draw Order",
        "Fog",
        "Clip Planes",
        "Materials & Shininess",
        "Normals & Shade Model",
        "Culling & Winding",
        "Bitmap Text",
        "Functions",
        "If & Conditionals",
        "Scratch Arrays",
    };

    ASSERT_INT("Phase C expands the catalog from 8 to 23 tutorials",
               repl_tutorial_count(), 23);

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char label[160];
        int idx = find_tutorial_idx(names[i]);
        snprintf(label, sizeof(label), "Phase C tutorial exists: %s", names[i]);
        ASSERT_TRUE(label, idx >= 0);
        if (idx < 0)
            continue;

        reset_fixture();
        tutorial_start(idx);
        snprintf(label, sizeof(label), "Phase C tutorial starts: %s", names[i]);
        ASSERT_TRUE(label, tutorial_active());
        if (!tutorial_active())
            continue;

        int guard = 0;
        int limit = repl_tutorial_step_count(idx) + 4;
        while (tutorial_active() && guard++ < limit) {
            int before_step = tutorial_state_view().step;
            int advanced = phase_c_complete_current_step();
            snprintf(label, sizeof(label), "%s step %d advances",
                     names[i], before_step);
            ASSERT_TRUE(label, advanced);
            if (!advanced)
                break;
        }

        snprintf(label, sizeof(label), "Phase C tutorial completes: %s", names[i]);
        ASSERT_TRUE(label, !tutorial_active());
        if (tutorial_active())
            tutorial_stop();
    }
}

/* Block-step adversarial runtime coverage (the post-Phase-C test pass the
 * plan defers from A4). The full walk above proves every Phase C step is
 * committable; these pin the block machinery's edges: the locked open/close
 * frame, in-block parking, close-row shifting, mismatch rejection, paste
 * guarding, close-commit bookkeeping, and Esc-recovery. */

static int block_step_of_shape(int idx, TutorialExpectedShape shape) {
    int total = repl_tutorial_step_count(idx);
    for (int s = 0; s < total; s++) {
        const char *expected = repl_tutorial_step_expected(idx, s);
        if (expected && repl_tutorial_expected_shape(expected) == shape)
            return s;
    }
    return -1;
}

/* Start `name` fresh and walk it to `target_step` through the real routes
 * (NOTE/SET ack via the controller keyboard path, COMMAND commits via the
 * ';' key). Returns the tutorial idx, or -1 when a step fails to advance. */
static int walk_block_tutorial_to_step(const char *name, int target_step) {
    int idx = find_tutorial_idx(name);
    if (idx < 0)
        return -1;
    reset_fixture();
    tutorial_start(idx);
    while (tutorial_active() && tutorial_state_view().step < target_step) {
        int step = tutorial_state_view().step;
        if (repl_tutorial_step_kind(idx, step) == TUTORIAL_STEP_KIND_COMMAND) {
            set_input_text(repl_tutorial_step_expected(idx, step));
            (void)editor_handle_key(';', 0, 0);
        } else {
            glr_ctrl_keyboard('\r', 0, 0);
        }
        if (tutorial_state_view().step <= step)
            return -1;
    }
    return idx;
}

static void test_block_open_commit_locks_header_and_parks_in_block(void) {
    int idx = find_tutorial_idx("First Loop");
    int open_step = idx >= 0
        ? block_step_of_shape(idx, TUTORIAL_EXPECTED_BLOCK_OPEN) : -1;
    ASSERT_TRUE("First Loop ships a block-open step", open_step >= 0);
    if (open_step < 0)
        return;
    ASSERT_TRUE("walk reaches the open step",
                walk_block_tutorial_to_step("First Loop", open_step) == idx);

    int before = repl_state_document_count();
    int open_row = tutorial_state_view().expected_commit_line;
    /* The whitespace-free variant must match the canonical expected. */
    set_input_text("for(i,0,8){");
    (void)editor_handle_key(';', 0, 0);

    TutorialRuntimeState st = tutorial_state_view();
    ASSERT_INT("open commit advances to the first body step",
               st.step, open_step + 1);
    /* +2 from the block frame (header + auto `}`) and +1 from the next
     * body step's instruction comment, emitted on entry at the block row. */
    ASSERT_INT("open commit inserts header, auto close, body instruction",
               repl_state_document_count(), before + 3);
    ASSERT_INT("block depth is one inside the loop", st.block_depth, 1);
    ASSERT_INT("innermost tracked close sits below the body instruction",
               st.block_end_lines[0], open_row + 2);
    ASSERT_INT("body step's commit row is the tracked close row",
               st.expected_commit_line, st.block_end_lines[0]);
    ASSERT_TRUE("in-block body step parks in insert mode",
                editor_insert_mode() != 0);

    SourceTextView doc = source_document_view();
    ASSERT_TRUE("header row holds the canonical for header",
                strstr(source_text_line(doc, open_row), "for(i, 0, 8)") != NULL);
    ASSERT_TRUE("tracked close row holds the auto brace",
                trim_leading_ws(source_text_line(doc, st.block_end_lines[0]))[0]
                    == '}');
    ASSERT_TRUE("header row locked", tutorial_line_is_locked(open_row));
    ASSERT_TRUE("auto close row locked",
                tutorial_line_is_locked(st.block_end_lines[0]));
}

static void test_block_body_commit_shifts_locked_close(void) {
    int idx = find_tutorial_idx("First Loop");
    int open_step = idx >= 0
        ? block_step_of_shape(idx, TUTORIAL_EXPECTED_BLOCK_OPEN) : -1;
    ASSERT_TRUE("walk reaches the first body step",
                open_step >= 0 &&
                walk_block_tutorial_to_step("First Loop", open_step + 1) == idx);
    if (open_step < 0)
        return;

    int close_row = tutorial_state_view().block_end_lines[0];
    ASSERT_TRUE("tracked close row valid before the body commit",
                close_row >= 0);
    set_input_text(repl_tutorial_step_expected(idx, open_step + 1));
    (void)editor_handle_key(';', 0, 0);

    TutorialRuntimeState st = tutorial_state_view();
    ASSERT_INT("body commit advances", st.step, open_step + 2);
    /* The next step is comment-less, so the close shifts by exactly the
     * one committed body row. */
    ASSERT_INT("close row shifted by the body insert",
               st.block_end_lines[0], close_row + 1);
    ASSERT_TRUE("shifted close row still locked",
                tutorial_line_is_locked(st.block_end_lines[0]));
    ASSERT_TRUE("shifted close row still holds the brace",
                trim_leading_ws(source_text_line(source_document_view(),
                                                 st.block_end_lines[0]))[0]
                    == '}');
}

static void test_block_wrong_body_input_rejected_and_preserved(void) {
    int idx = find_tutorial_idx("First Loop");
    int open_step = idx >= 0
        ? block_step_of_shape(idx, TUTORIAL_EXPECTED_BLOCK_OPEN) : -1;
    ASSERT_TRUE("walk reaches a comment-less body step",
                open_step >= 0 &&
                walk_block_tutorial_to_step("First Loop", open_step + 2) == idx);
    if (open_step < 0)
        return;

    int before_doc = repl_state_document_count();
    int before_step = tutorial_state_view().step;
    set_input_text("glScalef(2, 2, 2)");
    (void)editor_handle_key(';', 0, 0);

    ASSERT_INT("mismatched body input does not advance",
               tutorial_state_view().step, before_step);
    ASSERT_INT("mismatched body input does not mutate the document",
               repl_state_document_count(), before_doc);
    ASSERT_STR("mismatched input preserved for editing",
               editor_state_input().input, "glScalef(2, 2, 2)");
}

static void test_block_paste_at_close_row_blocked(void) {
    int idx = find_tutorial_idx("First Loop");
    int open_step = idx >= 0
        ? block_step_of_shape(idx, TUTORIAL_EXPECTED_BLOCK_OPEN) : -1;
    ASSERT_TRUE("walk reaches a step with a committed body row",
                open_step >= 0 &&
                walk_block_tutorial_to_step("First Loop", open_step + 2) == idx);
    if (open_step < 0)
        return;

    int close_row = tutorial_state_view().block_end_lines[0];
    int body_row = close_row - 1;   /* the just-committed (unlocked) body row */
    ASSERT_TRUE("body row above the close holds the committed command",
                strstr(source_text_line(source_document_view(), body_row),
                       "glPushMatrix") != NULL);

    int before_doc = repl_state_document_count();
    /* Copy is a no-op in insert mode; Esc out first (the stranding
     * scenario), then copy from the body row — the block-extent
     * expansion makes this a whole-block copy, which is fine: the
     * point is the paste attempt at the locked close row. */
    (void)editor_handle_key(KEY_ESC, 0, 0);
    editor_state_edit_line_set(body_row);
    editor_clipboard_copy_current();
    editor_state_edit_line_set(close_row);
    editor_clipboard_paste_current();
    ASSERT_STR("paste at the locked close row rejected",
               status_text(), "Tutorial line is read-only");
    ASSERT_INT("document unchanged after blocked paste",
               repl_state_document_count(), before_doc);
}

static void test_block_close_commit_returns_to_depth_zero(void) {
    int idx = find_tutorial_idx("First Loop");
    int close_step = idx >= 0
        ? block_step_of_shape(idx, TUTORIAL_EXPECTED_BLOCK_CLOSE) : -1;
    ASSERT_TRUE("walk reaches the close step",
                close_step >= 0 &&
                walk_block_tutorial_to_step("First Loop", close_step) == idx);
    if (close_step < 0)
        return;
    ASSERT_INT("depth one arriving at the close step",
               tutorial_state_view().block_depth, 1);

    int before_doc = repl_state_document_count();
    set_input_text("}");
    (void)editor_handle_key(';', 0, 0);

    TutorialRuntimeState st = tutorial_state_view();
    ASSERT_INT("close commit advances", st.step, close_step + 1);
    /* The close matched the existing auto brace (no new block rows); the
     * only insert is the next NOTE step's comment, appended at trailing. */
    ASSERT_INT("close commit adds no block rows",
               repl_state_document_count(), before_doc + 1);
    ASSERT_INT("block depth returns to zero", st.block_depth, 0);
    ASSERT_TRUE("insert mode off after leaving the block",
                !editor_insert_mode());
}

static void test_post_block_append_returns_to_trailing_row(void) {
    int idx = find_tutorial_idx("Functions");
    int close_step = idx >= 0
        ? block_step_of_shape(idx, TUTORIAL_EXPECTED_BLOCK_CLOSE) : -1;
    ASSERT_TRUE("walk reaches the post-block append step",
                close_step >= 0 &&
                walk_block_tutorial_to_step("Functions", close_step + 1) == idx);
    if (close_step < 0)
        return;

    TutorialRuntimeState st = tutorial_state_view();
    ASSERT_INT("block depth cleared after the func close", st.block_depth, 0);
    ASSERT_INT("post-block append parks at the trailing row",
               st.expected_commit_line, repl_state_document_count());
}

static void test_block_esc_then_navigate_back_reenters_insert_mode(void) {
    int idx = find_tutorial_idx("First Loop");
    int open_step = idx >= 0
        ? block_step_of_shape(idx, TUTORIAL_EXPECTED_BLOCK_OPEN) : -1;
    ASSERT_TRUE("walk parks on an in-block body step",
                open_step >= 0 &&
                walk_block_tutorial_to_step("First Loop", open_step + 2) == idx);
    if (open_step < 0)
        return;

    int expected_line = tutorial_state_view().expected_commit_line;
    ASSERT_TRUE("body step starts in insert mode", editor_insert_mode() != 0);
    (void)editor_handle_key(KEY_ESC, 0, 0);
    ASSERT_TRUE("Esc leaves insert mode", !editor_insert_mode());

    editor_navigate_to_line(0);
    editor_navigate_to_line(expected_line);
    ASSERT_TRUE("navigating back to the commit row re-enters insert mode",
                editor_insert_mode() != 0);
    ASSERT_INT("re-park clears the loaded locked-row text",
               editor_state_input().input_len, 0);
    ASSERT_TRUE("shadow ghost re-teaches the expected command",
                strstr(editor_state_autocomplete()->ghost, "glRotatef")
                    != NULL);
}

/* Walk the Feature Tour through its leading NOTE + COMMAND steps so the
 * runner lands on the REQUIRE vertex_outlines step. NOTE steps are acked
 * through the controller keyboard route; COMMAND steps (comment-full or
 * comment-less) commit their expected text. Returns the tutorial idx on
 * success, -1 if the catalog doesn't contain Feature Tour or a step
 * fails to advance. */
static int start_feature_tour_and_walk_commands(void) {
    int idx = find_tutorial_idx("Feature Tour");
    if (idx < 0) return -1;
    tutorial_start(idx);
    int total = repl_tutorial_step_count(idx);
    for (int guard = 0; guard < total && tutorial_active(); guard++) {
        int step = tutorial_state_view().step;
        TutorialStepKind kind = repl_tutorial_step_kind(idx, step);
        if (kind == TUTORIAL_STEP_KIND_NOTE) {
            glr_ctrl_keyboard('\r', 0, 0);
            if (tutorial_state_view().step <= step) return -1;
        } else if (kind == TUTORIAL_STEP_KIND_COMMAND) {
            if (!commit_command_step(idx, step)) return -1;
        } else {
            return idx;  /* landed on REQUIRE/SET — walk complete */
        }
    }
    return tutorial_active() ? idx : -1;
}

/* The catalog index of Feature Tour's REQUIRE vertex_outlines step —
 * the step start_feature_tour_and_walk_commands stops on. Derived, not
 * hardcoded, so catalog edits (added NOTE / comment-less steps) don't
 * invalidate the tests that assert step positions. */
static int feature_tour_require_step(int idx) {
    int total = repl_tutorial_step_count(idx);
    for (int s = 0; s < total; s++) {
        if (repl_tutorial_step_kind(idx, s) == TUTORIAL_STEP_KIND_REQUIRE)
            return s;
    }
    return -1;
}

/* Regression for the cursor-park bug the user hit in interactive testing:
 * after a COMMAND commit advances into a SET or REQUIRE step, the runner
 * must park the editor cursor PAST the just-emitted locked instruction
 * comment. Otherwise the editor renders the empty input-buffer overlay on
 * top of the comment row and the instruction is invisible until any key
 * moves the cursor. */
static void test_set_and_require_step_park_cursor_past_comment(void) {
    reset_fixture();
    int idx = start_feature_tour_and_walk_commands();
    ASSERT_TRUE("feature tour walks its NOTE + COMMAND steps", idx >= 0);
    ASSERT_TRUE("tutorial still active at REQUIRE", tutorial_active());

    int require_step = feature_tour_require_step(idx);
    ASSERT_TRUE("Feature Tour has a REQUIRE step", require_step >= 0);
    TutorialRuntimeState st = tutorial_state_view();
    ASSERT_INT("on the REQUIRE step", st.step, require_step);
    ASSERT_INT("REQUIRE step kind",
               (int)tutorial_current_step_kind(),
               (int)TUTORIAL_STEP_KIND_REQUIRE);
    int instr_line = st.instruction_line_for_step[require_step];
    ASSERT_TRUE("REQUIRE instruction line recorded", instr_line >= 0);
    /* Cursor must NOT be on the instruction-comment row; the runner
     * parks it at instruction_line + 1 (the virtual trailing row). */
    ASSERT_INT("cursor parked past REQUIRE comment row",
               editor_state_edit_line(), instr_line + 1);

    /* Advance past REQUIRE by setting vertex_outlines (the notify hook
     * in glr_config_set drives this). The next step is SET grid=Radar. */
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 1);
    st = tutorial_state_view();
    ASSERT_INT("REQUIRE advanced to SET grid=Radar", st.step,
               require_step + 1);
    ASSERT_INT("SET step kind",
               (int)tutorial_current_step_kind(),
               (int)TUTORIAL_STEP_KIND_SET);
    int set_instr = st.instruction_line_for_step[require_step + 1];
    ASSERT_TRUE("SET instruction line recorded", set_instr >= 0);
    ASSERT_INT("cursor parked past SET comment row",
               editor_state_edit_line(), set_instr + 1);
}

/* SET step applies its cfg on entry, then advances on Enter via the
 * controller-level ack router. Covers both the cfg-write side and the
 * keyboard-router wiring in glr_ctrl. */
static void test_set_step_applies_cfg_and_advances_on_ack(void) {
    reset_fixture();
    int idx = start_feature_tour_and_walk_commands();
    ASSERT_TRUE("walked into REQUIRE", idx >= 0);
    int require_step = feature_tour_require_step(idx);
    /* Drive through REQUIRE to reach the first SET (grid = Radar). */
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 1);
    ASSERT_INT("entered SET grid=Radar", tutorial_state_view().step,
               require_step + 1);
    ASSERT_INT("cfg grid applied to Radar",
               repl_cfg_get_int("grid", -1), GRID_THEME_RADAR);

    /* Ack via the controller router; SET advances to next SET (Aurora). */
    glr_ctrl_keyboard('\r', 0, 0);
    ASSERT_INT("ack key advanced to SET grid=Aurora",
               tutorial_state_view().step, require_step + 2);
    ASSERT_INT("cfg grid applied to Aurora",
               repl_cfg_get_int("grid", -1), GRID_THEME_AURORA);

    /* One more ack: past the final SET → tutorial completes. */
    glr_ctrl_keyboard('\r', 0, 0);
    ASSERT_TRUE("final ack completes the tutorial", !tutorial_active());
}

/* REQUIRE advances when the watched slug reaches its target, but NOT when
 * an unrelated config is toggled. Covers the slug-scoped predicate inside
 * the notify hook. */
static void test_require_ignores_unrelated_config_changes(void) {
    reset_fixture();
    int idx = start_feature_tour_and_walk_commands();
    ASSERT_TRUE("walked into REQUIRE", idx >= 0);
    int step_before = tutorial_state_view().step;

    /* Toggle backdrop — REQUIRE step watches vertex_outlines, must not
     * advance. */
    glr_config_set(GLR_CONFIG_BACKDROP, 1);
    ASSERT_INT("unrelated config change does not advance REQUIRE",
               tutorial_state_view().step, step_before);

    /* The actual target advances. */
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 1);
    ASSERT_TRUE("matching config change advances",
                tutorial_state_view().step > step_before);
}

/* Validator: SET / REQUIRE shape rules. The STEP_* macros live in
 * src/repl/tutorials.c (file-scope, catalog-only); test fixtures use
 * designated initializers — same convention as the existing label tests. */
static void test_validate_accepts_set_and_require_steps(void) {
    static const TutorialStep steps[] = {
        { NULL, "// type the begin", "glBegin(GL_TRIANGLES)",
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
        { NULL, "// turn on outlines", NULL,
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_REQUIRE, "vertex_outlines", 1 },
        { NULL, "// showcase radar", NULL,
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_SET, "grid", 10 },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
    };
    TutorialEntry entry = { .name = "mixed_ok", .steps = steps };
    char err[160] = "";
    ASSERT_TRUE("mixed COMMAND+SET+REQUIRE validates",
                repl_tutorial_validate_entry(&entry, err, sizeof(err)));
    ASSERT_STR("err empty on success", err, "");
    /* Step count must include the non-command steps too — the sentinel
     * needs BOTH comment and expected NULL, so SET/REQUIRE rows (NULL
     * expected, non-NULL comment) don't terminate the walk. */
    int n = 0;
    while (!repl_tutorial_step_is_sentinel(&steps[n])) n++;
    ASSERT_INT("mixed entry has 3 steps", n, 3);
}

static void test_validate_rejects_set_with_empty_slug(void) {
    static const TutorialStep steps[] = {
        { NULL, "// missing slug", NULL,
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_SET, "", 0 },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
    };
    TutorialEntry entry = { .name = "set_bad_slug", .steps = steps };
    char err[160] = "";
    ASSERT_TRUE("SET with empty slug rejected",
                !repl_tutorial_validate_entry(&entry, err, sizeof(err)));
    ASSERT_TRUE("error mentions cfg_slug",
                err[0] != '\0' && strstr(err, "cfg_slug") != NULL);
}

static void test_validate_rejects_require_with_expected(void) {
    /* Hand-build a malformed REQUIRE step: macros enforce NULL
     * `expected`, so use a designated initializer to bypass them. */
    static const TutorialStep steps[] = {
        { NULL, "// require with bogus expected", "glBegin(GL_TRIANGLES)",
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_REQUIRE, "vertex_outlines", 1 },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
    };
    TutorialEntry entry = { .name = "require_with_expected", .steps = steps };
    char err[160] = "";
    ASSERT_TRUE("REQUIRE with non-NULL expected rejected",
                !repl_tutorial_validate_entry(&entry, err, sizeof(err)));
    ASSERT_TRUE("error mentions expected",
                err[0] != '\0' && strstr(err, "expected") != NULL);
}

/* Relaxed COMMAND shape: `comment` is optional. A comment-less COMMAND
 * step (NULL comment, non-NULL expected) must validate AND must not be
 * mistaken for the array sentinel (which needs BOTH fields NULL). */
static void test_validate_accepts_comment_less_command_step(void) {
    static const TutorialStep steps[] = {
        { NULL, "// open the batch", "glBegin(GL_TRIANGLES)",
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
        { NULL, NULL, "glVertex3f(0, 0.7, 0)",
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
        { NULL, NULL, "glEnd()",
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
    };
    TutorialEntry entry = { .name = "comment_less_ok", .steps = steps };
    char err[160] = "";
    ASSERT_TRUE("comment-less COMMAND steps validate",
                repl_tutorial_validate_entry(&entry, err, sizeof(err)));
    ASSERT_STR("err empty on success", err, "");
    int n = 0;
    while (!repl_tutorial_step_is_sentinel(&steps[n])) n++;
    ASSERT_INT("comment-less steps counted, sentinel still terminates",
               n, 3);
    /* A comment-less COMMAND still needs a real expected: the pair-NULL
     * shape is reserved for the sentinel, and expected content rules
     * (single command, no ';', ...) still apply. */
    static const TutorialStep bad_steps[] = {
        { NULL, NULL, "glPointSize(1); glPointSize(2)",
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
    };
    TutorialEntry bad = { .name = "comment_less_multi", .steps = bad_steps };
    err[0] = '\0';
    ASSERT_TRUE("comment-less step still rejects multi-statement expected",
                !repl_tutorial_validate_entry(&bad, err, sizeof(err)));
}

/* NOTE step shape rules: expected must stay NULL and the comment must be
 * non-empty (a NOTE is nothing BUT its comment). */
static void test_validate_note_step_shapes(void) {
    static const TutorialStep ok_steps[] = {
        { NULL, "// welcome to the tour", NULL,
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_NOTE, NULL, 0 },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
    };
    TutorialEntry ok = { .name = "note_ok", .steps = ok_steps };
    char err[160] = "";
    ASSERT_TRUE("comment-only NOTE step validates",
                repl_tutorial_validate_entry(&ok, err, sizeof(err)));

    static const TutorialStep expected_steps[] = {
        { NULL, "// note with a command", "glEnd()",
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_NOTE, NULL, 0 },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
    };
    TutorialEntry with_expected = { .name = "note_with_expected",
                                    .steps = expected_steps };
    err[0] = '\0';
    ASSERT_TRUE("NOTE with non-NULL expected rejected",
                !repl_tutorial_validate_entry(&with_expected,
                                              err, sizeof(err)));
    ASSERT_TRUE("error mentions expected",
                err[0] != '\0' && strstr(err, "expected") != NULL);

    static const TutorialStep empty_steps[] = {
        { NULL, "", NULL,
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_NOTE, NULL, 0 },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
    };
    TutorialEntry empty = { .name = "note_empty_comment",
                            .steps = empty_steps };
    err[0] = '\0';
    ASSERT_TRUE("NOTE with empty comment rejected",
                !repl_tutorial_validate_entry(&empty, err, sizeof(err)));
    ASSERT_TRUE("error mentions comment",
                err[0] != '\0' && strstr(err, "comment") != NULL);
}

/* Block-structure steps: the expected-shape classifier. */
static void test_expected_shape_classifier(void) {
    ASSERT_INT("for header is OPEN",
               (int)repl_tutorial_expected_shape("for(i, 0, 8) {"),
               (int)TUTORIAL_EXPECTED_BLOCK_OPEN);
    ASSERT_INT("if header is OPEN",
               (int)repl_tutorial_expected_shape("if(sin(t) > 0) {"),
               (int)TUTORIAL_EXPECTED_BLOCK_OPEN);
    ASSERT_INT("named func header is OPEN",
               (int)repl_tutorial_expected_shape("spoke(a) {"),
               (int)TUTORIAL_EXPECTED_BLOCK_OPEN);
    ASSERT_INT("close brace is CLOSE",
               (int)repl_tutorial_expected_shape("  }  "),
               (int)TUTORIAL_EXPECTED_BLOCK_CLOSE);
    ASSERT_INT("else branch is BRANCH",
               (int)repl_tutorial_expected_shape("} else {"),
               (int)TUTORIAL_EXPECTED_BLOCK_BRANCH);
    ASSERT_INT("else-if branch is BRANCH",
               (int)repl_tutorial_expected_shape("} else if(t > 1) {"),
               (int)TUTORIAL_EXPECTED_BLOCK_BRANCH);
    ASSERT_INT("plain command is ORDINARY",
               (int)repl_tutorial_expected_shape("glVertex3f(0, 0.8, 0)"),
               (int)TUTORIAL_EXPECTED_ORDINARY);
    ASSERT_INT("header without paren close is ORDINARY",
               (int)repl_tutorial_expected_shape("for i {"),
               (int)TUTORIAL_EXPECTED_ORDINARY);
    ASSERT_INT("brace-less header is ORDINARY",
               (int)repl_tutorial_expected_shape("for(i, 0, 8)"),
               (int)TUTORIAL_EXPECTED_ORDINARY);
    ASSERT_INT("NULL is ORDINARY",
               (int)repl_tutorial_expected_shape(NULL),
               (int)TUTORIAL_EXPECTED_ORDINARY);

    ASSERT_TRUE("named func open is func-shaped",
                repl_tutorial_expected_is_func_open("spoke(a) {"));
    ASSERT_TRUE("funcN open is func-shaped",
                repl_tutorial_expected_is_func_open("func0() {"));
    ASSERT_TRUE("for open is not func-shaped",
                !repl_tutorial_expected_is_func_open("for(i, 0, 8) {"));
    ASSERT_TRUE("if open is not func-shaped",
                !repl_tutorial_expected_is_func_open("if(t > 0) {"));
    ASSERT_TRUE("ordinary call is not func-shaped",
                !repl_tutorial_expected_is_func_open("spoke(0)"));
}

/* Block-structure steps: balanced sequences validate — a simple for
 * body, a nested block, an if with an else branch, and a func def
 * followed by ordinary calls. */
static void test_validate_accepts_balanced_block_steps(void) {
    static const TutorialStep loop_steps[] = {
        { NULL, "// repeat eight times", "for(i, 0, 8) {",
          TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "glRotatef(i * 45, 0, 0, 1)",
          TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "glutSolidCube(0.15)",
          TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "}", TUTORIAL_STEP_APPEND, NULL },
        { NULL, "// after the loop", "glColor3f(1, 1, 1)",
          TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry loop = { .name = "block_loop", .steps = loop_steps };
    char err[160] = "";
    ASSERT_TRUE("balanced for block validates",
                repl_tutorial_validate_entry(&loop, err, sizeof(err)));
    ASSERT_STR("err empty on success", err, "");

    static const TutorialStep nested_steps[] = {
        { NULL, NULL, "for(i, 0, 3) {", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "for(j, 0, 3) {", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "glVertex3f(i, j, 0)", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "}", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "}", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry nested = { .name = "block_nested", .steps = nested_steps };
    err[0] = '\0';
    ASSERT_TRUE("nested for blocks validate",
                repl_tutorial_validate_entry(&nested, err, sizeof(err)));

    static const TutorialStep branch_steps[] = {
        { NULL, NULL, "if(sin(t) > 0) {", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "glColor3f(0.2, 1, 0.4)", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "} else {", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "glColor3f(1, 0.3, 0.2)", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "}", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry branch = { .name = "block_branch", .steps = branch_steps };
    err[0] = '\0';
    ASSERT_TRUE("if/else block validates",
                repl_tutorial_validate_entry(&branch, err, sizeof(err)));

    static const TutorialStep func_steps[] = {
        { NULL, "// define a helper", "spoke(a) {",
          TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "glRotatef(a, 0, 0, 1)", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "}", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "spoke(0)", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "spoke(120)", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry func = { .name = "block_func", .steps = func_steps };
    err[0] = '\0';
    ASSERT_TRUE("func def then calls validates",
                repl_tutorial_validate_entry(&func, err, sizeof(err)));
}

/* Block-structure steps: the rejection matrix. */
static void test_validate_block_step_rules(void) {
    char err[160];

    /* Close without any open. */
    static const TutorialStep close_only[] = {
        { NULL, NULL, "}", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry e1 = { .name = "close_only", .steps = close_only };
    err[0] = '\0';
    ASSERT_TRUE("close without open rejected",
                !repl_tutorial_validate_entry(&e1, err, sizeof(err)));
    ASSERT_TRUE("unmatched close diagnostic",
                strstr(err, "unmatched") != NULL);

    /* Branch at depth 0. */
    static const TutorialStep branch_depth0[] = {
        { NULL, NULL, "} else {", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry e2 = { .name = "branch_depth0", .steps = branch_depth0 };
    err[0] = '\0';
    ASSERT_TRUE("else at depth 0 rejected",
                !repl_tutorial_validate_entry(&e2, err, sizeof(err)));

    /* Branch inside a for block (needs an enclosing IF open). */
    static const TutorialStep branch_in_for[] = {
        { NULL, NULL, "for(i, 0, 3) {", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "} else {", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "}", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry e3 = { .name = "branch_in_for", .steps = branch_in_for };
    err[0] = '\0';
    ASSERT_TRUE("else inside for rejected",
                !repl_tutorial_validate_entry(&e3, err, sizeof(err)));
    ASSERT_TRUE("else-in-for diagnostic mentions if",
                strstr(err, "if") != NULL);

    /* Label placement inside an open block. */
    static const TutorialStep label_in_block[] = {
        { "anchor", "// anchored", "glPointSize(1)",
          TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "for(i, 0, 3) {", TUTORIAL_STEP_APPEND, NULL },
        { NULL, "// splice above the anchor", "glPointSize(2)",
          TUTORIAL_STEP_LABEL, "anchor" },
        { NULL, NULL, "}", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry e4 = { .name = "label_in_block", .steps = label_in_block };
    err[0] = '\0';
    ASSERT_TRUE("label placement inside block rejected",
                !repl_tutorial_validate_entry(&e4, err, sizeof(err)));
    ASSERT_TRUE("label-in-block diagnostic mentions block",
                strstr(err, "block") != NULL);

    /* REQUIRE_VAR inside an open block. */
    static const TutorialStep require_var_in_block[] = {
        { NULL, NULL, "for(i, 0, 3) {", TUTORIAL_STEP_APPEND, NULL },
        { NULL, "// set n", NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_REQUIRE_VAR, NULL, 0, NULL, "n", 5.0f },
        { NULL, NULL, "}", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry e5 = { .name = "require_var_in_block",
                         .steps = require_var_in_block };
    err[0] = '\0';
    ASSERT_TRUE("REQUIRE_VAR inside block rejected",
                !repl_tutorial_validate_entry(&e5, err, sizeof(err)));

    /* ORDINARY-shaped for header (no trailing '{') — the block kernels
     * would still claim it and commit two rows. */
    static const TutorialStep braceless_for[] = {
        { NULL, NULL, "for(i, 0, 3)", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry e6 = { .name = "braceless_for", .steps = braceless_for };
    err[0] = '\0';
    ASSERT_TRUE("brace-less for header rejected",
                !repl_tutorial_validate_entry(&e6, err, sizeof(err)));
    ASSERT_TRUE("brace-less header diagnostic mentions '{'",
                strstr(err, "{") != NULL);

    /* Func-open after non-func top-level content (relocation hazard). */
    static const TutorialStep func_after_cmd[] = {
        { NULL, NULL, "glPointSize(1)", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "spoke(a) {", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "}", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry e7 = { .name = "func_after_cmd", .steps = func_after_cmd };
    err[0] = '\0';
    ASSERT_TRUE("func open after top-level command rejected",
                !repl_tutorial_validate_entry(&e7, err, sizeof(err)));
    ASSERT_TRUE("func-relocation diagnostic mentions precede",
                strstr(err, "precede") != NULL);

    /* Func-open with a setup scaffold (same relocation hazard). */
    static const char *const scaffold[] = {
        "glPointSize(1)",
        NULL,
    };
    static const TutorialStep func_with_setup[] = {
        { NULL, NULL, "spoke(a) {", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "}", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry e8 = { .name = "func_with_setup",
                         .steps = func_with_setup, .setup = scaffold };
    err[0] = '\0';
    ASSERT_TRUE("func open with setup scaffold rejected",
                !repl_tutorial_validate_entry(&e8, err, sizeof(err)));
    ASSERT_TRUE("func-scaffold diagnostic mentions scaffold",
                strstr(err, "scaffold") != NULL);

    /* Func-open nested inside another block. */
    static const TutorialStep func_nested[] = {
        { NULL, NULL, "for(i, 0, 3) {", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "spoke(a) {", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "}", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "}", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry e9 = { .name = "func_nested", .steps = func_nested };
    err[0] = '\0';
    ASSERT_TRUE("func open nested in a block rejected",
                !repl_tutorial_validate_entry(&e9, err, sizeof(err)));
    ASSERT_TRUE("nested-func diagnostic mentions nest",
                strstr(err, "nest") != NULL);

    /* A second func def AFTER the first closed is fine (completed funcs
     * are relocation-safe prefixes), but an ordinary command between
     * them poisons the third. */
    static const TutorialStep two_funcs[] = {
        { NULL, NULL, "spoke(a) {", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "}", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "rim(b) {", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, "}", TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry e10 = { .name = "two_funcs", .steps = two_funcs };
    err[0] = '\0';
    ASSERT_TRUE("second func def after first closes validates",
                repl_tutorial_validate_entry(&e10, err, sizeof(err)));
}

/* Feature Tour is the catalog's showcase for the relaxed shapes: it must
 * open with a NOTE step and carry comment-less COMMAND steps, and
 * step_count must include them (they'd vanish if any walker still used
 * the old `comment == NULL` sentinel). */
static void test_catalog_feature_tour_uses_relaxed_step_shapes(void) {
    int idx = find_tutorial_idx("Feature Tour");
    ASSERT_TRUE("Feature Tour is in catalog", idx >= 0);
    if (idx < 0) return;

    ASSERT_INT("Feature Tour opens with a NOTE step",
               (int)repl_tutorial_step_kind(idx, 0),
               (int)TUTORIAL_STEP_KIND_NOTE);
    ASSERT_TRUE("NOTE step has a comment",
                repl_tutorial_step_comment(idx, 0) != NULL);
    ASSERT_TRUE("NOTE step has no expected",
                repl_tutorial_step_expected(idx, 0) == NULL);

    int comment_less = 0;
    int total = repl_tutorial_step_count(idx);
    for (int s = 0; s < total; s++) {
        if (repl_tutorial_step_kind(idx, s) == TUTORIAL_STEP_KIND_COMMAND &&
            repl_tutorial_step_comment(idx, s) == NULL) {
            ASSERT_TRUE("comment-less step still has expected",
                        repl_tutorial_step_expected(idx, s) != NULL);
            comment_less++;
        }
    }
    ASSERT_INT("Feature Tour has two comment-less COMMAND steps",
               comment_less, 2);
}

/* Runtime NOTE behavior: the instruction comment is emitted and locked,
 * the document is frozen (typed commits reject with the ack hint), a
 * non-ack key is not consumed, and an ack key advances. */
static void test_note_step_waits_for_ack_and_freezes_document(void) {
    reset_fixture();
    int idx = find_tutorial_idx("Feature Tour");
    ASSERT_TRUE("Feature Tour found", idx >= 0);
    if (idx < 0) return;

    tutorial_start(idx);
    ASSERT_TRUE("tutorial active", tutorial_active());
    ASSERT_INT("on the NOTE step", tutorial_state_view().step, 0);
    /* Rows 0-1 are the scene-clear prelude; the NOTE comment lands at
     * row 2. */
    ASSERT_INT("NOTE comment emitted below the clear prelude",
               repl_state_document_count(), 3);
    ASSERT_TRUE("NOTE comment row is locked", tutorial_line_is_locked(2));
    ASSERT_INT("no expected commit row during NOTE",
               tutorial_state_view().expected_commit_line, -1);

    /* Typed commit is rejected with the ack hint, not committed. */
    set_input_text("glPointSize(2)");
    editor_handle_key(';', 0, 0);
    ASSERT_INT("step unchanged after rejected commit during NOTE",
               tutorial_state_view().step, 0);
    ASSERT_INT("document unchanged after rejected commit",
               repl_state_document_count(), 3);
    char expected_status[256];
    snprintf(expected_status, sizeof(expected_status), "%s Tutorial [%d/%d]: Press Enter / Tab / Space to continue", repl_tutorial_name(idx), idx + 1, repl_tutorial_count());
    ASSERT_STR("ack hint shown for NOTE",
               status_text(), expected_status);

    /* Non-ack keys are not consumed by the ack router. */
    ASSERT_INT("non-ack key not consumed", tutorial_handle_ack_key('x'), 0);
    ASSERT_INT("still on the NOTE step", tutorial_state_view().step, 0);

    /* Ack via the controller keyboard route advances to the first
     * COMMAND step. */
    glr_ctrl_keyboard(' ', 0, 0);
    ASSERT_INT("ack advanced past the NOTE step",
               tutorial_state_view().step, 1);
    ASSERT_INT("next step is COMMAND",
               (int)tutorial_current_step_kind(),
               (int)TUTORIAL_STEP_KIND_COMMAND);
}

/* Runtime comment-less COMMAND behavior: no instruction row is emitted
 * (the document grows by exactly one row — the committed command), the
 * commit lands at the parked cursor row, and after the commit that row
 * is recorded as the step's anchor and locked. */
static void test_comment_less_command_commits_without_instruction_row(void) {
    reset_fixture();
    int idx = find_tutorial_idx("Feature Tour");
    ASSERT_TRUE("Feature Tour found", idx >= 0);
    if (idx < 0) return;

    tutorial_start(idx);
    glr_ctrl_keyboard('\r', 0, 0);                      /* ack the NOTE   */
    ASSERT_TRUE("commit glBegin step",
                commit_command_step(idx, tutorial_state_view().step));
    ASSERT_TRUE("commit top-vertex step",
                commit_command_step(idx, tutorial_state_view().step));

    /* Now paused on the first comment-less vertex step. */
    int step = tutorial_state_view().step;
    ASSERT_TRUE("current step is comment-less COMMAND",
                repl_tutorial_step_kind(idx, step) ==
                    TUTORIAL_STEP_KIND_COMMAND &&
                repl_tutorial_step_comment(idx, step) == NULL);

    int doc_before = repl_state_document_count();
    TutorialRuntimeState st = tutorial_state_view();
    ASSERT_INT("commit row is the trailing row (no comment inserted)",
               st.expected_commit_line, doc_before);
    ASSERT_INT("cursor parked on the commit row",
               editor_state_edit_line(), doc_before);
    ASSERT_INT("no anchor recorded before the commit",
               st.instruction_line_for_step[step], -1);

    ASSERT_TRUE("commit the comment-less step",
                commit_command_step(idx, step));
    ASSERT_INT("document grew by exactly the committed command",
               repl_state_document_count(), doc_before + 1);
    st = tutorial_state_view();
    ASSERT_INT("committed row recorded as the step's anchor",
               st.instruction_line_for_step[step], doc_before);
    ASSERT_TRUE("committed row is locked",
                tutorial_line_is_locked(doc_before));

    /* The committed row holds the expected command (no comment row was
     * inserted above it). */
    SourceTextView doc = source_document_view();
    const char *row = source_text_line(doc, doc_before);
    ASSERT_TRUE("committed row holds the expected command",
                row && strstr(row, repl_tutorial_step_expected(idx, step)) != NULL);
}

/* --- Setup scaffold (Option A) ------------------------------------------- */
/* A tutorial can preload starting code (TutorialEntry.setup) so it builds
 * on what an earlier tutorial taught without the learner re-typing it.
 * See docs/plans/active/tutorial-setup-scaffold.md. */

static void test_catalog_color_interp_uses_setup_scaffold(void) {
    int idx = find_tutorial_idx("Color Interpolation");
    ASSERT_TRUE("Color Interpolation is in catalog", idx >= 0);
    if (idx < 0) return;

    const char *const *setup = repl_tutorial_setup_lines(idx);
    ASSERT_TRUE("Color Interpolation has a setup scaffold", setup != NULL);
    ASSERT_TRUE("setup leads with an @cfg header line",
                setup && setup[0] && strstr(setup[0], "@cfg") != NULL);

    ASSERT_INT("step 0 is a NOTE",
               (int)repl_tutorial_step_kind(idx, 0),
               (int)TUTORIAL_STEP_KIND_NOTE);
    int label_targeted = 0;
    int total = repl_tutorial_step_count(idx);
    for (int s = 0; s < total; s++) {
        if (repl_tutorial_step_placement(idx, s) == TUTORIAL_STEP_LABEL) {
            /* The targets anchor on setup goto labels, not step labels. */
            const char *target = repl_tutorial_step_target_label(idx, s);
            ASSERT_TRUE("label-targeted step has a target",
                        target && target[0]);
            label_targeted++;
        }
    }
    ASSERT_INT("two steps splice into the scaffold", label_targeted, 2);

    /* Setup stays genuinely opt-in. */
    int first = find_tutorial_idx("First Triangle");
    ASSERT_TRUE("First Triangle has no setup scaffold",
                first >= 0 && repl_tutorial_setup_lines(first) == NULL);
    ASSERT_TRUE("out-of-range setup is NULL",
                repl_tutorial_setup_lines(repl_tutorial_count()) == NULL);
}

static void test_setup_scaffold_preloads_locked_rows_and_cfg(void) {
    reset_fixture();
    int idx = find_tutorial_idx("Color Interpolation");
    ASSERT_TRUE("Color Interpolation found", idx >= 0);
    if (idx < 0) return;

    int expected_2d = -1;
    ASSERT_TRUE("RENDER3D_VIEW_2D resolves through the bridge",
                repl_cfg_resolve_text("view_mode", "RENDER3D_VIEW_2D",
                                      &expected_2d));
    int view_baseline = repl_cfg_get_int("view_mode", -1);

    tutorial_start(idx);
    ASSERT_TRUE("tutorial active", tutorial_active());

    /* The 2-row scene-clear prelude loads ahead of the scaffold body
     * (9 rows: comment, glBegin, glColor3f, vertex, left:, vertex,
     * right:, vertex, glEnd); the step-0 NOTE comment appends one more. */
    int scaffold_rows = 9;
    int prelude_rows = TUTORIAL_SCENE_PRELUDE_ROWS;
    ASSERT_INT("clear prelude + scaffold + NOTE instruction rows loaded",
               repl_state_document_count(), prelude_rows + scaffold_rows + 1);
    for (int r = 0; r < prelude_rows + scaffold_rows; r++)
        ASSERT_TRUE("prelude/scaffold row is locked", tutorial_line_is_locked(r));
    ASSERT_INT("setup @cfg header applied (2D view)",
               repl_cfg_get_int("view_mode", -1), expected_2d);

    /* Setup @cfg slugs join the restore baseline like entry @cfg — but
     * the restore is deferred past exit, so flush before asserting it. */
    tutorial_stop();
    ASSERT_TRUE("tutorial inactive after exit", !tutorial_active());
    tutorial_teardown();
    ASSERT_INT("flush restores view_mode to pre-tutorial baseline",
               repl_cfg_get_int("view_mode", -1), view_baseline);
}

static void test_setup_label_targeted_steps_splice_into_scaffold(void) {
    reset_fixture();
    int idx = find_tutorial_idx("Color Interpolation");
    ASSERT_TRUE("Color Interpolation found", idx >= 0);
    if (idx < 0) return;

    tutorial_start(idx);
    glr_ctrl_keyboard('\r', 0, 0);                     /* ack the NOTE */
    TutorialRuntimeState st = tutorial_state_view();
    ASSERT_INT("on the first label-targeted step", st.step, 1);
    ASSERT_TRUE("commit row pinned inside the scaffold",
                st.expected_commit_line >= 0);

    ASSERT_TRUE("commit the green corner color",
                commit_command_step(idx, 1));

    /* Final step: commit directly — commit_command_step's step-advance
     * check can't observe completion (teardown resets the step). */
    const char *blue_expected = repl_tutorial_step_expected(idx, 2);
    ASSERT_TRUE("final step has expected text", blue_expected != NULL);
    set_input_text(blue_expected ? blue_expected : "");
    editor_handle_key(';', 0, 0);
    ASSERT_TRUE("tutorial completes after the final splice",
                !tutorial_active());

    /* Final document order: each color lands immediately above its
     * anchor label, so it executes before that corner's vertex. */
    SourceTextView doc = source_document_view();
    int green = -1, blue = -1, left = -1, right = -1;
    int n = repl_state_document_count();
    for (int r = 0; r < n; r++) {
        const char *line = source_text_line(doc, r);
        if (!line) continue;
        if (strstr(line, "glColor3f(0.2, 1, 0.3)"))  green = r;
        if (strstr(line, "glColor3f(0.2, 0.3, 1)"))  blue  = r;
        if (strcmp(trim_leading_ws(line), "left:") == 0)  left  = r;
        if (strcmp(trim_leading_ws(line), "right:") == 0) right = r;
    }
    ASSERT_TRUE("green color row committed", green >= 0);
    ASSERT_TRUE("blue color row committed", blue >= 0);
    ASSERT_TRUE("left anchor label present", left >= 0);
    ASSERT_TRUE("right anchor label present", right >= 0);
    ASSERT_TRUE("green splice sits above the left anchor",
                green >= 0 && left >= 0 && green < left);
    ASSERT_TRUE("blue splice sits between the anchors",
                blue >= 0 && right >= 0 && left < blue && blue < right);
}

static void test_validate_setup_label_rules(void) {
    static const char *const setup_lines[] = {
        "glBegin(GL_TRIANGLES)",
        ":anchor",
        "glEnd()",
        NULL,
    };

    /* A target_label resolving to a setup goto label validates. */
    static const TutorialStep at_setup_steps[] = {
        { NULL, "// splice above the anchor", "glPointSize(2)",
          TUTORIAL_STEP_LABEL, "anchor",
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
    };
    TutorialEntry ok = { .name = "setup_anchor_ok",
                         .steps = at_setup_steps,
                         .setup = setup_lines };
    char err[160] = "";
    ASSERT_TRUE("target_label may name a setup goto label",
                repl_tutorial_validate_entry(&ok, err, sizeof(err)));

    /* A step label shadowing a setup goto label is ambiguous. */
    static const TutorialStep collide_steps[] = {
        { "anchor", "// shadows the setup label", "glPointSize(2)",
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
    };
    TutorialEntry collide = { .name = "setup_anchor_collision",
                              .steps = collide_steps,
                              .setup = setup_lines };
    err[0] = '\0';
    ASSERT_TRUE("step label colliding with setup goto label rejected",
                !repl_tutorial_validate_entry(&collide, err, sizeof(err)));
    ASSERT_TRUE("collision diagnostic mentions 'collides'",
                err[0] != '\0' && strstr(err, "collides") != NULL);

    /* Unknown targets stay rejected even with a setup present. */
    static const TutorialStep missing_steps[] = {
        { NULL, "// no such anchor", "glPointSize(2)",
          TUTORIAL_STEP_LABEL, "nope",
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
    };
    TutorialEntry missing = { .name = "setup_anchor_missing",
                              .steps = missing_steps,
                              .setup = setup_lines };
    err[0] = '\0';
    ASSERT_TRUE("unknown target_label still rejected",
                !repl_tutorial_validate_entry(&missing, err, sizeof(err)));
}

static void test_validate_setup_capacity(void) {
    /* setup rows are all locked, so setup lines + steps must fit the
     * locked-line table. */
    static const char *big_setup[TUTORIAL_LOCKED_LINE_MAX + 2];
    for (int i = 0; i < TUTORIAL_LOCKED_LINE_MAX + 1; i++)
        big_setup[i] = "glPointSize(1)";
    big_setup[TUTORIAL_LOCKED_LINE_MAX + 1] = NULL;

    static const TutorialStep one_step[] = {
        { NULL, "// one step", "glEnd()", TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0 },
    };
    TutorialEntry entry = { .name = "setup_too_big",
                            .steps = one_step,
                            .setup = (const char *const *)big_setup };
    char err[160] = "";
    ASSERT_TRUE("oversized setup rejected",
                !repl_tutorial_validate_entry(&entry, err, sizeof(err)));
    ASSERT_TRUE("capacity diagnostic mentions locked-line capacity",
                err[0] != '\0' && strstr(err, "locked-line") != NULL);
}

/* SET/REQUIRE steps must reject typed commits with a kind-appropriate hint
 * — not the misleading "Move cursor to the tutorial insertion line". */
static void test_commit_blocked_with_hint_during_set_step(void) {
    reset_fixture();
    int idx = start_feature_tour_and_walk_commands();
    ASSERT_TRUE("walked into REQUIRE", idx >= 0);
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 1);
    int step_before = tutorial_state_view().step;
    ASSERT_INT("at SET step", step_before, feature_tour_require_step(idx) + 1);

    /* Attempt to commit arbitrary text — the precheck must reject with
     * the SET hint. */
    set_input_text("glPointSize(2)");
    editor_handle_key(';', 0, 0);

    ASSERT_INT("step unchanged after rejected SET commit",
               tutorial_state_view().step, step_before);
    char expected_status[256];
    snprintf(expected_status, sizeof(expected_status), "%s Tutorial [%d/%d]: Press Enter / Tab / Space to continue", repl_tutorial_name(idx), idx + 1, repl_tutorial_count());
    ASSERT_STR("SET hint shown (not the cursor-position hint)",
               status_text(), expected_status);
}

/* Load-bearing regression: workspace load during an active tutorial must
 * restore the tutorial's cfg baseline BEFORE the workspace stash captures
 * the pre-load cfg. Otherwise the tutorial-mutated cfg gets enshrined as
 * the new "pre-workspace" baseline. */
static void test_workspace_load_during_tutorial_restores_baseline(void) {
    char temp_dir[] = "/tmp/test_tutorial_restore.XXXXXX";

    reset_fixture();
    /* Establish a baseline OUTSIDE the tutorial: grid OFF. */
    glr_config_set(GLR_CONFIG_GRID_THEME, 0);
    int baseline = repl_cfg_get_int("grid", -1);
    ASSERT_INT("baseline grid is OFF", baseline, 0);

    int idx = start_feature_tour_and_walk_commands();
    ASSERT_TRUE("walked into REQUIRE", idx >= 0);
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 1);  /* advances past REQUIRE */
    ASSERT_INT("now on SET grid=Radar", tutorial_state_view().step,
               feature_tour_require_step(idx) + 1);
    ASSERT_INT("grid is RADAR mid-tutorial",
               repl_cfg_get_int("grid", -1), GRID_THEME_RADAR);

    /* Trigger the workspace-load teardown path — empty dir, but the
     * teardown helper must run the cfg restore before the load. */
    char *made_dir = mkdtemp(temp_dir);
    ASSERT_TRUE("mkdtemp restore-test workspace", made_dir != NULL);
    if (!made_dir) return;
    (void)repl_load_workspace(made_dir);
    ASSERT_TRUE("workspace load exits tutorial", !tutorial_active());
    /* The whole point: grid is back to baseline, not stuck at RADAR. */
    ASSERT_INT("grid restored to baseline after workspace load",
               repl_cfg_get_int("grid", -1), baseline);
    rmdir(made_dir);
}

/* Regression for finding 1: exiting on a REQUIRE step must not let the
 * baseline restore advance the tutorial. The notify hook in
 * glr_config_set fires per slug; if the runtime state is still
 * active=1 when restore writes the slug to a value matching the
 * REQUIRE target, the tutorial advances mid-teardown and runs the
 * next step's SET side effects. The fix resets state BEFORE the
 * restore. */
static void test_exit_on_require_does_not_autoadvance(void) {
    reset_fixture();

    /* Pre-tutorial baseline that COINCIDES with the REQUIRE target
     * (vertex_outlines == 1) — the worst-case input that exposed the
     * bug. Grid baseline is OFF so the post-fix path leaves grid at
     * 0, not the SET-step's RADAR theme. */
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 1);
    glr_config_set(GLR_CONFIG_GRID_THEME, 0);
    int outlines_baseline = repl_cfg_get_int("vertex_outlines", -1);
    int grid_baseline     = repl_cfg_get_int("grid", -1);
    ASSERT_INT("vertex_outlines baseline 1", outlines_baseline, 1);
    ASSERT_INT("grid baseline OFF", grid_baseline, 0);

    int idx = start_feature_tour_and_walk_commands();
    ASSERT_TRUE("walked into REQUIRE", idx >= 0);
    ASSERT_INT("on REQUIRE step", tutorial_state_view().step,
               feature_tour_require_step(idx));
    ASSERT_INT("REQUIRE kind",
               (int)tutorial_current_step_kind(),
               (int)TUTORIAL_STEP_KIND_REQUIRE);
    /* presentation_reset wiped vertex_outlines back to its default
     * (0) — the REQUIRE is unsatisfied, which is the precondition for
     * the bug: a restore write of 1 (the baseline) would match. */
    ASSERT_INT("REQUIRE unsatisfied at exit", repl_cfg_get_int("vertex_outlines", -1), 0);

    tutorial_stop();
    ASSERT_TRUE("tutorial inactive after exit", !tutorial_active());
    /* Exit keeps the lesson's view; the baseline restore is deferred to
     * the next teardown flush, which is where the restore writes land. */
    ASSERT_INT("exit keeps the tutorial's vertex_outlines",
               repl_cfg_get_int("vertex_outlines", -1), 0);

    tutorial_teardown();
    ASSERT_INT("flush restores vertex_outlines to baseline",
               repl_cfg_get_int("vertex_outlines", -1), outlines_baseline);
    /* The whole point: the next-step SET (grid = RADAR) must NOT
     * have fired during the restore. Pre-fix, the notify hook saw a
     * matching REQUIRE on the vertex_outlines write and auto-advanced,
     * stamping the RADAR theme after grid had already been restored. */
    ASSERT_INT("next-step SET did NOT fire during teardown",
               repl_cfg_get_int("grid", -1), grid_baseline);
}

/* Regression for finding 2: starting a new tutorial while one is
 * active must restore the prior tutorial's baseline before capturing
 * a new one. Otherwise the just-mutated cfg gets enshrined as the
 * "pre-tutorial" state, and the final exit restores tutorial-mutated
 * values instead of the user's true original. */
static void test_restart_during_tutorial_preserves_original_baseline(void) {
    reset_fixture();

    /* True pre-tutorial baseline: grid OFF. */
    glr_config_set(GLR_CONFIG_GRID_THEME, 0);
    int baseline = repl_cfg_get_int("grid", -1);
    ASSERT_INT("baseline grid OFF", baseline, 0);

    /* Walk Feature Tour past REQUIRE so the SET grid=RADAR fires. */
    int idx1 = start_feature_tour_and_walk_commands();
    ASSERT_TRUE("walked tour 1 into REQUIRE", idx1 >= 0);
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 1); /* advances REQUIRE → SET */
    ASSERT_INT("grid mutated to RADAR mid-tutorial 1",
               repl_cfg_get_int("grid", -1), GRID_THEME_RADAR);
    ASSERT_TRUE("tutorial 1 still active", tutorial_active());

    /* Start tutorial 2 without exiting tutorial 1. Pre-fix: the new
     * tutorial's baseline capture saw the RADAR theme and enshrined it;
     * the final exit would restore that mutated value. Post-fix: the
     * inner teardown restores grid=0 first, the new baseline captures
     * grid=0. */
    int idx2 = find_tutorial_idx("First Triangle");
    ASSERT_TRUE("found First Triangle", idx2 >= 0);
    tutorial_start(idx2);
    ASSERT_TRUE("tutorial 2 active", tutorial_active());
    ASSERT_INT("tutorial 2 idx", tutorial_state_view().tutorial_idx, idx2);

    /* Exit tutorial 2, then flush: its baseline (captured after teardown
     * of tour 1) must equal the original user baseline. */
    tutorial_stop();
    ASSERT_TRUE("tutorial 2 inactive after exit", !tutorial_active());
    tutorial_teardown();
    ASSERT_INT("grid restored to ORIGINAL baseline (not tour-1 mutated)",
               repl_cfg_get_int("grid", -1), baseline);
}

/* Companion to the above, for the deferred-restore contract: a
 * *finished* tutorial's pending baseline must be flushed by the next
 * tutorial_start too, not just by an active-tutorial teardown. Chaining
 * lesson B off a completed lesson A would otherwise capture A's
 * presentation as "the user's" and strand the real pre-tutorial config. */
static void test_start_after_finish_flushes_pending_baseline(void) {
    reset_fixture();

    glr_config_set(GLR_CONFIG_GRID_THEME, 0);
    int baseline = repl_cfg_get_int("grid", -1);
    ASSERT_INT("baseline grid OFF", baseline, 0);

    int idx1 = start_feature_tour_and_walk_commands();
    ASSERT_TRUE("walked tour into REQUIRE", idx1 >= 0);
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 1); /* advances REQUIRE → SET */
    ASSERT_INT("grid mutated to RADAR mid-tutorial",
               repl_cfg_get_int("grid", -1), GRID_THEME_RADAR);

    /* Exit, and confirm the mutated view is kept (nothing flushed yet). */
    tutorial_stop();
    ASSERT_INT("exit keeps the tutorial's grid theme",
               repl_cfg_get_int("grid", -1), GRID_THEME_RADAR);

    int idx2 = find_tutorial_idx("First Triangle");
    ASSERT_TRUE("found First Triangle", idx2 >= 0);
    tutorial_start(idx2);
    tutorial_stop();
    tutorial_teardown();
    ASSERT_INT("second lesson's baseline is the ORIGINAL user grid",
               repl_cfg_get_int("grid", -1), baseline);
}

/* Regression for finding 3: the baseline must capture view_mode
 * (GLR_CONFIG_ORTHO_MODE) unconditionally. presentation_reset always
 * touches it, but it's deliberately outside fill_scene_subset (it's a
 * global, not per-scene). A tutorial whose @cfg / SET / REQUIRE steps
 * never name view_mode would otherwise leak the
 * presentation_reset(→3D) past teardown. */
static void test_baseline_captures_view_mode_even_when_unreferenced(void) {
    reset_fixture();

    /* Pre-tutorial: 2D (ortho_mode = 1). */
    glr_config_set(GLR_CONFIG_ORTHO_MODE, 1);
    ASSERT_INT("baseline view_mode 2D", repl_cfg_get_int("view_mode", -1), 1);

    /* Color & Transform has no .cfg block and no SET/REQUIRE steps —
     * it never names view_mode, so the bug case is exactly this. */
    int idx = find_tutorial_idx("Color & Transform");
    ASSERT_TRUE("found Color & Transform", idx >= 0);
    tutorial_start(idx);
    ASSERT_TRUE("color-transform active", tutorial_active());
    /* presentation_reset → CFG_DEFAULT_ORTHO_MODE = 0 (3D) */
    ASSERT_INT("view_mode reset to 3D inside tutorial",
               repl_cfg_get_int("view_mode", -1), 0);

    tutorial_stop();
    ASSERT_TRUE("color-transform inactive after exit", !tutorial_active());
    ASSERT_INT("exit keeps the tutorial's 3D view",
               repl_cfg_get_int("view_mode", -1), 0);
    tutorial_teardown();
    ASSERT_INT("flush restores view_mode to pre-tutorial 2D",
               repl_cfg_get_int("view_mode", -1), 1);
}

/* Audit #41: catalog grid-theme SET steps must use symbolic value
 * names (via STEP_SET_SYM) so reordering `Render3dGridTheme` in
 * src/scene/themes.h cannot silently shift the showcase to a
 * different theme. Locate the Feature Tour by name, find the two
 * grid SET steps in catalog order, and assert each is symbolic
 * with the expected name. (The bridge's resolve_text — pinned by
 * `test_apply_cfg_text_resolves_symbolic_grid_theme` — separately
 * locks "GRID_THEME_RADAR" → GRID_THEME_RADAR, etc.) */
static void test_feature_tour_grid_steps_use_symbolic_names(void) {
    int n = repl_tutorial_count();
    int tour_idx = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(repl_tutorial_name(i), "Feature Tour") == 0) {
            tour_idx = i;
            break;
        }
    }
    ASSERT_TRUE("Feature Tour exists in tutorial catalog", tour_idx >= 0);
    if (tour_idx < 0) return;

    int step_count = repl_tutorial_step_count(tour_idx);
    int grid_step_idx[8];
    int grid_step_count = 0;
    for (int i = 0; i < step_count; i++) {
        const char *slug = repl_tutorial_step_cfg_slug(tour_idx, i);
        if (slug && strcmp(slug, "grid") == 0 &&
            repl_tutorial_step_kind(tour_idx, i) == TUTORIAL_STEP_KIND_SET) {
            if (grid_step_count < 8)
                grid_step_idx[grid_step_count++] = i;
        }
    }
    ASSERT_INT("Feature Tour has two grid SET steps", grid_step_count, 2);
    if (grid_step_count < 2) return;

    const char *first_name = repl_tutorial_step_cfg_value_name(
            tour_idx, grid_step_idx[0]);
    const char *second_name = repl_tutorial_step_cfg_value_name(
            tour_idx, grid_step_idx[1]);
    ASSERT_TRUE("first grid SET step uses cfg_value_name", first_name != NULL);
    ASSERT_TRUE("second grid SET step uses cfg_value_name", second_name != NULL);
    ASSERT_STR("first grid SET step == GRID_THEME_RADAR",
               first_name ? first_name : "(null)",
               "GRID_THEME_RADAR");
    ASSERT_STR("second grid SET step == GRID_THEME_AURORA",
               second_name ? second_name : "(null)",
               "GRID_THEME_AURORA");
}

static void test_feature_tour_vertex_outline_hint_uses_keymap(void) {
    int tour_idx = find_tutorial_idx("Feature Tour");
    ASSERT_TRUE("Feature Tour exists for shortcut hint test", tour_idx >= 0);
    if (tour_idx < 0) return;

    int outline_step = -1;
    int step_count = repl_tutorial_step_count(tour_idx);
    for (int i = 0; i < step_count; i++) {
        const char *slug = repl_tutorial_step_cfg_slug(tour_idx, i);
        if (slug && strcmp(slug, "vertex_outlines") == 0 &&
            repl_tutorial_step_kind(tour_idx, i) == TUTORIAL_STEP_KIND_REQUIRE) {
            outline_step = i;
            break;
        }
    }
    ASSERT_TRUE("Feature Tour has vertex_outlines REQUIRE step", outline_step >= 0);
    if (outline_step < 0) return;

    char shortcut[KEYMAP_SHORTCUT_LABEL_MAX];
    const char *comment = repl_tutorial_step_comment(tour_idx, outline_step);
    keymap_binding_to_string(shortcut, (int)sizeof(shortcut),
                             KM_KEY(GLR_VERTEX_OUTLINES),
                             KM_MODS(GLR_VERTEX_OUTLINES), 0);
    ASSERT_TRUE("vertex outline hint contains keymap shortcut",
                comment && strstr(comment, shortcut) != NULL);
    ASSERT_TRUE("vertex outline hint does not carry stale F7 shortcut",
                !comment || strstr(comment, "F7") == NULL);
}

/* Audit #41 follow-up: the runtime validator must reject a tutorial
 * whose SET / REQUIRE step or entry-level @cfg line carries a
 * symbolic value name that the bridge doesn't recognise — otherwise
 * the strtol-fallback path would silently land the showcase at the
 * default (`*_OFF` = 0), and any REQUIRE pointing at that same
 * default would auto-advance incorrectly. Exercises
 * tutorial_validate_entry_against_bridge directly so the test
 * doesn't need a typo'd entry in the shipped catalog. */
static void test_validate_rejects_typo_symbolic_value_name(void) {
    /* glr_ctrl_reset_all installs g_glr_export_cfg_bridge — the
     * bridge that resolve_text and the resolver rules live behind.
     * Without it repl_cfg_known returns 0 for every slug. */
    glr_ctrl_reset_all();

    /* Sanity: shipped Feature Tour passes the bridge validator
     * (catches a regression where someone breaks the validator
     * itself or adds a typo to the catalog). */
    int n = repl_tutorial_count();
    int tour_idx = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(repl_tutorial_name(i), "Feature Tour") == 0) {
            tour_idx = i;
            break;
        }
    }
    ASSERT_TRUE("Feature Tour exists", tour_idx >= 0);
    if (tour_idx >= 0) {
        const TutorialEntry *e = repl_tutorial_entry(tour_idx);
        ASSERT_TRUE("repl_tutorial_entry returns shipped entry", e != NULL);
        char err[160] = "";
        ASSERT_TRUE("shipped Feature Tour passes bridge validator",
                    tutorial_validate_entry_against_bridge(e, err, sizeof err));
    }

    /* Synthetic entry: SET step with typo'd value name. */
    static const TutorialStep typo_set_steps[] = {
        { NULL, "// SET typo step", NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_SET, "grid", 0, "GRID_THEME_RADRA" },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0, NULL },
    };
    TutorialEntry typo_set_entry = {
        .name = "typo_set", .steps = typo_set_steps,
        .cfg = NULL, .tags = 0, .subheading = NULL,
    };
    char err[160] = "";
    ASSERT_TRUE("SET typo rejected",
                !tutorial_validate_entry_against_bridge(&typo_set_entry,
                                                       err, sizeof err));
    ASSERT_TRUE("SET typo diagnostic names the bad symbol",
                strstr(err, "GRID_THEME_RADRA") != NULL);

    /* Synthetic entry: REQUIRE step with typo'd value name. */
    static const TutorialStep typo_req_steps[] = {
        { NULL, "// REQUIRE typo step", NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_REQUIRE, "axes", 0, "AXES_THEME_COMPSS" },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0, NULL },
    };
    TutorialEntry typo_req_entry = {
        .name = "typo_req", .steps = typo_req_steps,
        .cfg = NULL, .tags = 0, .subheading = NULL,
    };
    err[0] = '\0';
    ASSERT_TRUE("REQUIRE typo rejected",
                !tutorial_validate_entry_against_bridge(&typo_req_entry,
                                                       err, sizeof err));
    ASSERT_TRUE("REQUIRE typo diagnostic names the bad symbol",
                strstr(err, "AXES_THEME_COMPSS") != NULL);

    /* Synthetic entry: entry-level @cfg line with typo'd value name. */
    static const TutorialStep cmd_only_steps[] = {
        { NULL, "// trivial", "glPointSize(1)", TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0, NULL },
    };
    static const char *const typo_cfg_lines[] = {
        "// @cfg backdrop = SCENE_BACKDROP_CITISCAPE",   /* typo */
        NULL,
    };
    TutorialEntry typo_cfg_entry = {
        .name = "typo_cfg", .steps = cmd_only_steps,
        .cfg = typo_cfg_lines, .tags = 0, .subheading = NULL,
    };
    err[0] = '\0';
    ASSERT_TRUE("@cfg typo rejected",
                !tutorial_validate_entry_against_bridge(&typo_cfg_entry,
                                                       err, sizeof err));
    ASSERT_TRUE("@cfg typo diagnostic names the bad symbol",
                strstr(err, "SCENE_BACKDROP_CITISCAPE") != NULL);

    /* Symmetric: an entry with valid symbolic + numeric @cfg values
     * passes. Locks the back-compat path: integer literals still
     * resolve through the strtol fallback. */
    static const char *const valid_cfg_lines[] = {
        "// @cfg grid = GRID_THEME_RADAR",
        "// @cfg axes = 4",   /* legacy integer-form, must still pass */
        NULL,
    };
    TutorialEntry valid_entry = {
        .name = "valid", .steps = cmd_only_steps,
        .cfg = valid_cfg_lines, .tags = 0, .subheading = NULL,
    };
    err[0] = '\0';
    ASSERT_TRUE("clean symbolic + legacy integer @cfg passes",
                tutorial_validate_entry_against_bridge(&valid_entry,
                                                      err, sizeof err));
}

/* ------------------------------------------------------------------------- */
/* REQUIRE_VAR step kind: predef-variable target with either-path                  */
/* (typed `name = expr;` commit OR variable-panel slider drag).             */
/* ------------------------------------------------------------------------- */

static void test_validate_accepts_require_var_step(void) {
    static const TutorialStep steps[] = {
        { NULL, "// set n to 5", NULL,
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_REQUIRE_VAR, NULL, 0, NULL, "n", 5.0f },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0, NULL, NULL, 0.0f },
    };
    TutorialEntry entry = { .name = "require_var_ok", .steps = steps };
    char err[160] = "";
    ASSERT_TRUE("REQUIRE_VAR with valid var_name validates",
                repl_tutorial_validate_entry(&entry, err, sizeof(err)));
    ASSERT_STR("err empty on success", err, "");
}

static void test_validate_rejects_require_var_with_expected(void) {
    static const TutorialStep steps[] = {
        { NULL, "// require with bogus expected", "glBegin(GL_TRIANGLES)",
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_REQUIRE_VAR, NULL, 0, NULL, "n", 5.0f },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0, NULL, NULL, 0.0f },
    };
    TutorialEntry entry = { .name = "require_var_with_expected", .steps = steps };
    char err[160] = "";
    ASSERT_TRUE("REQUIRE_VAR with non-NULL expected rejected",
                !repl_tutorial_validate_entry(&entry, err, sizeof(err)));
    ASSERT_TRUE("error mentions expected",
                err[0] != '\0' && strstr(err, "expected") != NULL);
}

static void test_validate_rejects_require_var_empty_name(void) {
    static const TutorialStep steps[] = {
        { NULL, "// missing var name", NULL,
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_REQUIRE_VAR, NULL, 0, NULL, "", 5.0f },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0, NULL, NULL, 0.0f },
    };
    TutorialEntry entry = { .name = "require_var_empty_name", .steps = steps };
    char err[160] = "";
    ASSERT_TRUE("REQUIRE_VAR with empty var_name rejected",
                !repl_tutorial_validate_entry(&entry, err, sizeof(err)));
    ASSERT_TRUE("error mentions var_name",
                err[0] != '\0' && strstr(err, "var_name") != NULL);
}

static void test_validate_rejects_require_var_reserved_name(void) {
    /* `t` is the predefined animation variable — REQUIRE_VAR var_name
     * must not collide with the reserved-ident set. */
    static const TutorialStep steps[] = {
        { NULL, "// reserved name", NULL,
          TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_REQUIRE_VAR, NULL, 0, NULL, "t", 5.0f },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL,
          TUTORIAL_STEP_KIND_COMMAND, NULL, 0, NULL, NULL, 0.0f },
    };
    TutorialEntry entry = { .name = "require_var_reserved", .steps = steps };
    char err[160] = "";
    ASSERT_TRUE("REQUIRE_VAR with reserved var_name rejected",
                !repl_tutorial_validate_entry(&entry, err, sizeof(err)));
    ASSERT_TRUE("error mentions reserved",
                err[0] != '\0' && strstr(err, "reserved") != NULL);
}

/* The catalog's "Variable Slider" tutorial is author-editable (it now
 * also draws a triangle whose vertices use `n`), so these tests read its
 * targets/structure dynamically rather than pinning literals. The
 * load-bearing invariants: step 0 is a REQUIRE_VAR declaration step on
 * `n`, and the LAST step is a REQUIRE_VAR slider step on `n`. */
static int variable_slider_last_step(int idx) {
    return repl_tutorial_step_count(idx) - 1;
}

/* Format the declaration line the user types for a REQUIRE_VAR step whose
 * var does not exist yet: `float <name> = <target>` (matches the
 * declaration form of the synthesized ghost, minus the trailing comment). */
static void format_decl_line(int idx, int step, char *out, size_t out_sz) {
    const char *name = repl_tutorial_step_var_name(idx, step);
    float target = repl_tutorial_step_var_target(idx, step);
    snprintf(out, out_sz, "float %s = %g", name ? name : "n", (double)target);
}

static void test_catalog_includes_variable_slider_tutorial(void) {
    int idx = find_tutorial_idx("Variable Slider");
    ASSERT_TRUE("Variable Slider tutorial is in catalog", idx >= 0);
    if (idx < 0) return;
    int last = variable_slider_last_step(idx);
    ASSERT_TRUE("Variable Slider has at least two steps", last >= 1);
    ASSERT_INT("step 0 is REQUIRE_VAR",
               (int)repl_tutorial_step_kind(idx, 0),
               (int)TUTORIAL_STEP_KIND_REQUIRE_VAR);
    ASSERT_STR("step 0 watches var n",
               repl_tutorial_step_var_name(idx, 0), "n");
    ASSERT_INT("final step is REQUIRE_VAR (the slider step)",
               (int)repl_tutorial_step_kind(idx, last),
               (int)TUTORIAL_STEP_KIND_REQUIRE_VAR);
    ASSERT_STR("final step watches var n",
               repl_tutorial_step_var_name(idx, last), "n");
    /* The variable starts low and the slider raises it. */
    ASSERT_TRUE("final target is greater than the declared target",
                repl_tutorial_step_var_target(idx, last) >
                repl_tutorial_step_var_target(idx, 0));
}

/* No pre-declaration: the watched variable does NOT exist when the
 * tutorial starts. Step 0 is a declaration step — the user creates `n`
 * themselves. Because the satisfying `float n = ...;` relocates to the
 * document top, the runner emits no separate locked instruction comment
 * for it (the instruction rides the ghost as a trailing comment), so the
 * document holds only the injected scene-clear prelude until the user
 * declares the variable. */
static void test_require_var_does_not_predeclare(void) {
    reset_fixture();

    int idx = find_tutorial_idx("Variable Slider");
    ASSERT_TRUE("Variable Slider in catalog", idx >= 0);
    if (idx < 0) return;
    tutorial_start(idx);

    ASSERT_TRUE("n is NOT pre-declared at tutorial start",
                repl_eval_find_predef_var_idx("n") < 0);
    ASSERT_INT("declaration step emits no instruction row beyond the prelude",
               repl_state_document_count(), TUTORIAL_SCENE_PRELUDE_ROWS);
}

/* A typed `float n = <target>;` declaration satisfies the step-0
 * REQUIRE_VAR on `n`: the decl-with-initializer carries a DECLARE predef
 * op through repl_apply_predef_ops, which the chokepoint in
 * editor_commit_apply_plan notifies AFTER the commit's post-effects
 * settle. `n` does not exist until the user declares it. */
static void test_require_var_advances_on_typed_assignment(void) {
    reset_fixture();

    int idx = find_tutorial_idx("Variable Slider");
    ASSERT_TRUE("Variable Slider in catalog", idx >= 0);
    if (idx < 0) return;
    tutorial_start(idx);
    ASSERT_TRUE("tutorial active", tutorial_active());
    ASSERT_INT("on REQUIRE_VAR step 0", tutorial_state_view().step, 0);

    char decl[64];
    format_decl_line(idx, 0, decl, sizeof decl);
    float target0 = repl_tutorial_step_var_target(idx, 0);
    set_input_text(decl);
    editor_handle_key(';', 0, 0);

    ASSERT_INT("REQUIRE_VAR advanced exactly one step after declaring n",
               tutorial_state_view().step, 1);
    int n_idx = repl_eval_find_predef_var_idx("n");
    ASSERT_TRUE("n now declared", n_idx >= 0);
    if (n_idx >= 0)
        ASSERT_TRUE("n holds the declared value",
                    fabsf(g_predef_vars[n_idx].value - target0) <= TUTORIAL_VAR_EPS);
}

/* Regression for the skipped-glBegin bug: the declaration commit's
 * predef-writeback notify advances step 0 -> step 1 (a COMMAND step,
 * whose instruction comment is emitted). The commit-side advance must
 * then stay a no-op — otherwise it advances AGAIN onto step 2, skipping
 * step 1's command entirely (comment shown, command never typed). After
 * declaring we must be paused ON step 1, with step 1's expected command
 * still waiting to be typed, and the cursor below the locked comment. */
static void test_require_var_declaration_does_not_skip_next_step(void) {
    reset_fixture();

    int idx = find_tutorial_idx("Variable Slider");
    ASSERT_TRUE("Variable Slider in catalog", idx >= 0);
    if (idx < 0) return;
    /* Only meaningful when a COMMAND step follows the declaration. */
    if (repl_tutorial_step_kind(idx, 1) != TUTORIAL_STEP_KIND_COMMAND) return;
    tutorial_start(idx);

    char decl[64];
    format_decl_line(idx, 0, decl, sizeof decl);
    set_input_text(decl);
    editor_handle_key(';', 0, 0);

    ASSERT_INT("advanced exactly one step (step 1 not skipped)",
               tutorial_state_view().step, 1);
    ASSERT_TRUE("still paused on step 1's command, awaiting the user",
                tutorial_active() &&
                tutorial_current_expected_text() != NULL);
    ASSERT_STR("paused on step 1's expected command",
               tutorial_current_expected_text(),
               repl_tutorial_step_expected(idx, 1));
    /* Cursor parks on the trailing input row, below step 1's locked
     * instruction comment — never on a locked line. */
    int edit_line = editor_state_edit_line();
    ASSERT_TRUE("cursor is not on a locked line",
                !tutorial_line_is_locked(edit_line));
    ASSERT_TRUE("cursor parked on the trailing input row",
                edit_line == repl_state_document_count());
}

/* Regression for the Tab-accept bug: the REQUIRE_VAR ghost is written
 * straight into autocomplete.ghost with NO match-list entry, so the old
 * match_count-only gate made Tab a silent no-op. The declaration ghost is
 * `float n = <target>; <catalog comment>` (the comment rides along as a
 * trailing comment). Tab must fill the input with it, and a following ';'
 * must declare `n` and advance. */
static void test_require_var_tab_accepts_ghost(void) {
    reset_fixture();

    int idx = find_tutorial_idx("Variable Slider");
    ASSERT_TRUE("Variable Slider in catalog", idx >= 0);
    if (idx < 0) return;
    tutorial_start(idx);

    /* Build the expected declaration ghost: `float n = <target>` plus the
     * catalog comment as a trailing comment. tutorial_start pokes
     * editor_completion_update(), so the ghost is populated before any
     * keystroke. */
    char decl[64];
    format_decl_line(idx, 0, decl, sizeof decl);
    const char *cmt = repl_tutorial_step_comment(idx, 0);
    char expect_ghost[MAX_LINE_LEN];
    if (cmt && cmt[0])
        snprintf(expect_ghost, sizeof expect_ghost, "%s; %s", decl, cmt);
    else
        snprintf(expect_ghost, sizeof expect_ghost, "%s", decl);

    ASSERT_STR("ghost teaches the declaration with its trailing comment",
               editor_state_autocomplete()->ghost, expect_ghost);

    (void)editor_handle_key('\t', 0, 0);
    ASSERT_STR("Tab fills the input with the declaration line",
               editor_state_input().input, expect_ghost);

    (void)editor_handle_key(';', 0, 0);
    ASSERT_INT("Tab-accept then ; declares n and advances",
               tutorial_state_view().step, 1);
    ASSERT_TRUE("n declared after Tab-accept commit",
                repl_eval_find_predef_var_idx("n") >= 0);
    /* The instruction comment is PRESENT — it committed as a trailing
     * comment on the declaration line (the user's "missing comment" fix),
     * not as a separate stranded line. */
    if (cmt && cmt[0]) {
        SourceTextView doc = source_document_view();
        const char *decl_line = source_text_line(doc, 0);
        ASSERT_TRUE("declaration line carries the trailing comment",
                    decl_line && strstr(decl_line, cmt) != NULL);
    }
}

/* Regression: Enter must commit a REQUIRE_VAR declaration just like ';'.
 * The declaration parks on the trailing row with insert mode OFF; the
 * Enter route's commit_current_input previously only tried block_structs
 * there, so `float n = 1;` fell through to the GL-command parser and was
 * rejected — ';' committed it but Enter did not. Tab-accept the ghost,
 * then commit with Enter (\r). */
static void test_require_var_enter_commits_declaration(void) {
    reset_fixture();

    int idx = find_tutorial_idx("Variable Slider");
    ASSERT_TRUE("Variable Slider in catalog", idx >= 0);
    if (idx < 0) return;
    tutorial_start(idx);

    (void)editor_handle_key('\t', 0, 0);   /* fill the declaration ghost */
    (void)editor_handle_key('\r', 0, 0);   /* commit with Enter, not ';' */

    ASSERT_INT("Enter commits the declaration and advances one step",
               tutorial_state_view().step, 1);
    ASSERT_TRUE("n declared after the Enter commit",
                repl_eval_find_predef_var_idx("n") >= 0);
}

/* Complete whatever step the tutorial is paused on: type a COMMAND step's
 * expected command, or declare an undeclared REQUIRE_VAR var. Returns 0
 * (without acting) when the current step is a DECLARED REQUIRE_VAR (a
 * slider step the caller should drive via the slider) or there is nothing
 * to do. */
static int tut_complete_current_step(void) {
    if (!tutorial_active()) return 0;
    TutorialRuntimeState st = tutorial_state_view();
    TutorialStepKind k = tutorial_current_step_kind();
    if (k == TUTORIAL_STEP_KIND_COMMAND) {
        const char *exp = tutorial_current_expected_text();
        if (!exp) return 0;
        set_input_text(exp);
        editor_handle_key(';', 0, 0);
        return 1;
    }
    if (k == TUTORIAL_STEP_KIND_REQUIRE_VAR) {
        const char *name = repl_tutorial_step_var_name(st.tutorial_idx, st.step);
        if (!name || repl_eval_find_predef_var_idx(name) >= 0)
            return 0;  /* declared REQUIRE_VAR -> slider step */
        char decl[64];
        format_decl_line(st.tutorial_idx, st.step, decl, sizeof decl);
        set_input_text(decl);
        editor_handle_key(';', 0, 0);
        return 1;
    }
    return 0;
}

/* Slider drag satisfies the final REQUIRE_VAR step end-to-end. Walk the
 * whole tutorial (declaration + the triangle COMMAND steps) up to the
 * final slider step, then drag `n` to its target. The variable-panel
 * writeback flows through the same repl_apply_predef_ops chokepoint;
 * driving the controller-level router exercises the wiring end-to-end.
 * Reaching the final target completes the tutorial. */
static void test_require_var_advances_on_slider_drag(void) {
    reset_fixture();

    int idx = find_tutorial_idx("Variable Slider");
    ASSERT_TRUE("Variable Slider in catalog", idx >= 0);
    if (idx < 0) return;
    tutorial_start(idx);

    int guard = 0;
    while (tut_complete_current_step() && guard++ < 64) { /* walk steps */ }

    ASSERT_TRUE("reached the final slider step", tutorial_active());
    ASSERT_INT("final step is REQUIRE_VAR",
               (int)tutorial_current_step_kind(),
               (int)TUTORIAL_STEP_KIND_REQUIRE_VAR);
    int n_idx = repl_eval_find_predef_var_idx("n");
    ASSERT_TRUE("n declared by the time we reach the slider step", n_idx >= 0);
    if (n_idx < 0) return;

    float cur = g_predef_vars[n_idx].value;
    float target = repl_tutorial_step_var_target(tutorial_state_view().tutorial_idx,
                                                 tutorial_state_view().step);
    /* Linear drag: 1 px = 0.1 units, so dx px maps to dx*0.1 units. */
    int dx = (int)((target - cur) / 0.1f + 0.5f);
    variable_panel_handle_drag_begin(n_idx, /*coarse=*/0, /*x=*/0);
    glr_ctrl_router_handle_variable_panel_motion(/*x=*/dx, /*y=*/0);

    ASSERT_TRUE("n value reflects drag to the final target",
                fabsf(g_predef_vars[n_idx].value - target) <= TUTORIAL_VAR_EPS);
    ASSERT_TRUE("slider drag to the final target completed the tutorial",
                !tutorial_active());
    variable_panel_handle_drag_reset();
}

/* An assignment to an unrelated variable must NOT advance a REQUIRE_VAR
 * step that watches `n`. The notify hook (not the commit-side advance) is
 * the authority, and it only matches the watched var. `n` is never
 * declared here, so the step stays put. */
static void test_require_var_ignores_unrelated_assignment(void) {
    reset_fixture();

    int idx = find_tutorial_idx("Variable Slider");
    ASSERT_TRUE("Variable Slider in catalog", idx >= 0);
    if (idx < 0) return;
    tutorial_start(idx);
    /* Declare an unrelated variable so the test commits an assignment
     * the parser can resolve. `n` remains undeclared. */
    editor_feed_line("float m;");
    int step_before = tutorial_state_view().step;

    set_input_text("m = 5");
    editor_handle_key(';', 0, 0);

    ASSERT_INT("unrelated assignment does not advance REQUIRE_VAR",
               tutorial_state_view().step, step_before);
}

/* The notify advances at most one step at a time: after the declaration
 * commit the runner enters step 1 but must NOT also advance past it.
 * Pins the "commit-side no-op for free-form commits" + "notify-side
 * single advance" pairing. */
static void test_require_var_pauses_on_next_step_after_match(void) {
    reset_fixture();

    int idx = find_tutorial_idx("Variable Slider");
    ASSERT_TRUE("Variable Slider in catalog", idx >= 0);
    if (idx < 0) return;
    tutorial_start(idx);
    ASSERT_TRUE("tutorial active", tutorial_active());

    char decl[64];
    format_decl_line(idx, 0, decl, sizeof decl);
    set_input_text(decl);
    editor_handle_key(';', 0, 0);

    ASSERT_INT("advanced exactly one step", tutorial_state_view().step, 1);
    ASSERT_TRUE("tutorial still active (step 1 didn't auto-advance)",
                tutorial_active());
}

/* Epsilon boundary: a value just inside TUTORIAL_VAR_EPS counts as a
 * match; just outside doesn't. Declare `n` off-target so it exists for
 * the direct poke without advancing step 0, then drive the boundary check
 * through the notify path so the live comparison in
 * tutorial_var_matches_target is exercised. */
static void test_require_var_epsilon_boundary(void) {
    reset_fixture();

    int idx = find_tutorial_idx("Variable Slider");
    ASSERT_TRUE("Variable Slider in catalog", idx >= 0);
    if (idx < 0) return;
    tutorial_start(idx);

    float target0 = repl_tutorial_step_var_target(idx, 0);

    /* Declare n at a value that does NOT hit the step-0 target, so n
     * exists for the poke below without advancing the step. */
    char decl[64];
    snprintf(decl, sizeof decl, "float n = %g;", (double)(target0 + 1.0f));
    editor_feed_line(decl);
    int n_idx = repl_eval_find_predef_var_idx("n");
    ASSERT_TRUE("n declared", n_idx >= 0);
    if (n_idx < 0) return;
    ASSERT_INT("still on step 0 (off-target value)", tutorial_state_view().step, 0);

    /* Just outside epsilon -> no match. Poke the value directly and fire
     * the notify; step must stay on 0. (Direct poke isolates the matcher
     * from drag arithmetic precision.) */
    g_predef_vars_mut[n_idx].value = target0 - 2 * TUTORIAL_VAR_EPS;
    tutorial_notify_state_changed();
    ASSERT_INT("just-outside-epsilon does not advance",
               tutorial_state_view().step, 0);

    /* Just inside epsilon -> matches and the step advances. */
    g_predef_vars_mut[n_idx].value = target0 - 0.5f * TUTORIAL_VAR_EPS;
    tutorial_notify_state_changed();
    ASSERT_INT("just-inside-epsilon advances",
               tutorial_state_view().step, 1);
}

/* The ghost-suffix helper synthesizes the line the user should type for a
 * REQUIRE_VAR step. While `n` is undeclared (step 0) it teaches the
 * DECLARATION `float n = <target>;` carrying the catalog comment as a
 * trailing comment; once declared it teaches the bare assignment
 * `n = <target>`. */
static void test_require_var_shadow_suffix_synthesizes_assignment(void) {
    reset_fixture();
    char ghost[MAX_LINE_LEN];

    int idx = find_tutorial_idx("Variable Slider");
    ASSERT_TRUE("Variable Slider in catalog", idx >= 0);
    if (idx < 0) return;
    tutorial_start(idx);

    char decl[64];
    format_decl_line(idx, 0, decl, sizeof decl);
    const char *cmt = repl_tutorial_step_comment(idx, 0);
    char expect_decl_ghost[MAX_LINE_LEN];
    if (cmt && cmt[0])
        snprintf(expect_decl_ghost, sizeof expect_decl_ghost, "%s; %s", decl, cmt);
    else
        snprintf(expect_decl_ghost, sizeof expect_decl_ghost, "%s", decl);

    /* Undeclared n, empty input -> full declaration ghost (with trailing
     * comment). */
    ghost[0] = '\0';
    ASSERT_INT("shadow returns 1 on empty input",
               tutorial_shadow_suffix("", ghost, sizeof ghost), 1);
    ASSERT_STR("ghost is the full synthesized declaration line",
               ghost, expect_decl_ghost);

    /* Strict-prefix `float n =` -> the remainder of the declaration line. */
    {
        char prefix[64];
        snprintf(prefix, sizeof prefix, "float n =");
        char expect_suffix[MAX_LINE_LEN];
        /* Remainder after "float n =" is " <target>; <comment>". */
        float t0 = repl_tutorial_step_var_target(idx, 0);
        if (cmt && cmt[0])
            snprintf(expect_suffix, sizeof expect_suffix, " %g; %s", (double)t0, cmt);
        else
            snprintf(expect_suffix, sizeof expect_suffix, " %g", (double)t0);
        ghost[0] = '\0';
        ASSERT_INT("shadow returns 1 for valid prefix",
                   tutorial_shadow_suffix(prefix, ghost, sizeof ghost), 1);
        ASSERT_STR("ghost is the remainder after the prefix",
                   ghost, expect_suffix);
    }

    /* Once n is declared, the ghost teaches the bare assignment form.
     * Declare it off-target so the step does not advance, then re-check. */
    float t0 = repl_tutorial_step_var_target(idx, 0);
    char decl2[64];
    snprintf(decl2, sizeof decl2, "float n = %g;", (double)(t0 + 1.0f));
    editor_feed_line(decl2);
    char expect_assign[64];
    snprintf(expect_assign, sizeof expect_assign, "n = %g", (double)t0);
    ghost[0] = '\0';
    ASSERT_INT("shadow returns 1 once declared",
               tutorial_shadow_suffix("", ghost, sizeof ghost), 1);
    ASSERT_STR("ghost switches to bare assignment form once declared",
               ghost, expect_assign);
}

int main(void) {
    tutorial_state_init_explicit();
    test_feature_tour_grid_steps_use_symbolic_names();
    test_feature_tour_vertex_outline_hint_uses_keymap();
    test_validate_rejects_typo_symbolic_value_name();
    test_exit_on_require_does_not_autoadvance();
    test_restart_during_tutorial_preserves_original_baseline();
    test_start_after_finish_flushes_pending_baseline();
    test_baseline_captures_view_mode_even_when_unreferenced();
    test_start_enters_transient_tutorial_scene();
    test_start_resets_view_to_tutorial_defaults();
    test_catalog_includes_color_transform_tutorial();
    test_color_transform_walkthrough();
    test_runner_match_and_advance();
    test_semicolon_route_rejects_mismatch_and_preserves_input();
    test_replace_existing_line_does_not_advance();
    test_shadow_suffix_strict_prefix();
    test_shadow_suffix_inactive_returns_zero();
    test_shadow_text_populates_autocomplete_ghost();
    test_shadow_text_appears_immediately_on_start();
    test_shadow_text_refreshes_on_advance();
    test_shadow_ghost_falls_through_off_expected_line();
    test_ghost_reappears_on_return_to_expected_line();
    test_tab_skips_tutorial_autofill_off_expected_line();
    test_shadow_text_clears_on_exit();
    test_tutorial_start_sets_step_progress_status();
    test_tutorial_advance_updates_step_progress_status();
    test_tutorial_status_hint_variants();
    test_tutorial_refresh_input_hint_on_full_match();
    test_locked_comment_load_is_read_only();
    test_locked_comment_mutations_are_blocked();
    test_paste_before_locked_prefix_is_blocked();
    test_undo_redo_blocked_during_tutorial();
    test_enter_route_advances_after_match();
    test_navigation_rejects_non_matching_input();
    test_navigation_advances_on_matching_input();
    test_enter_on_locked_line_shows_position_hint();
    test_ctrl_slash_on_locked_line_is_blocked();
    test_tab_autofill_then_semicolon_advances();
    test_rejected_commit_does_not_advance_tutorial();
    test_feed_line_alone_does_not_advance_tutorial();
    test_loading_example_exits_tutorial();
    test_loading_workspace_exits_tutorial();
    test_invalid_workspace_load_does_not_exit_tutorial();
    test_invalid_user_scene_load_does_not_exit_tutorial();
    test_fade_duration_math();
    test_complete_and_menu_actions();
    test_start_leaves_unsaved_buffer_transient();
    test_start_rejects_out_of_range_idx();
    test_catalog_starter_steps_are_append();
    test_catalog_cfg_lines();
    test_catalog_tag_metadata();
    test_catalog_subheading_metadata();
    test_catalog_validation_passes_for_all_tutorials();
    test_phase_c_catalog_full_walk();
    test_block_open_commit_locks_header_and_parks_in_block();
    test_block_body_commit_shifts_locked_close();
    test_block_wrong_body_input_rejected_and_preserved();
    test_block_paste_at_close_row_blocked();
    test_block_close_commit_returns_to_depth_zero();
    test_post_block_append_returns_to_trailing_row();
    test_block_esc_then_navigate_back_reenters_insert_mode();
    test_catalog_rejects_out_of_range_index();
    test_validate_rejects_duplicate_label();
    test_validate_rejects_missing_target_label();
    test_validate_rejects_forward_reference();
    test_validate_rejects_multi_row_expected();
    test_validate_accepts_well_formed_label_tutorial();
    test_append_first_expected_commit_line_is_trailing_row();
    test_depth_tutorial_label_targeted_step_inserts_above_label();
    test_depth_tutorial_label_targeted_emit_shifts_prior_locked_lines();
    test_phase3_label_targeted_commit_inserts_above_label();
    test_phase3_wrong_input_at_label_step_does_not_insert();
    test_phase3_correct_input_at_wrong_line_does_not_insert();
    test_phase3_empty_input_silent_reject();
    test_phase3_pending_clears_after_match_failure();
    test_phase3_paste_above_locked_still_blocked();
    test_phase4_depth_tutorial_catalog_shape();
    test_phase4_full_walk_places_setup_before_batch();
    test_review_guard_blocks_expected_commit_line_without_pending();
    /* New step kinds (SET / REQUIRE), cfg notify, mutation guard, restore. */
    test_set_and_require_step_park_cursor_past_comment();
    test_set_step_applies_cfg_and_advances_on_ack();
    test_require_ignores_unrelated_config_changes();
    test_validate_accepts_set_and_require_steps();
    test_validate_rejects_set_with_empty_slug();
    test_validate_rejects_require_with_expected();
    test_first_animation_starts_on_ctrl_t();
    /* Relaxed step shapes: NOTE kind + comment-less COMMAND steps. */
    test_validate_accepts_comment_less_command_step();
    test_validate_note_step_shapes();
    test_catalog_feature_tour_uses_relaxed_step_shapes();
    test_note_step_waits_for_ack_and_freezes_document();
    test_comment_less_command_commits_without_instruction_row();
    /* Setup scaffold (Option A — preloaded starting code). */
    test_catalog_color_interp_uses_setup_scaffold();
    test_setup_scaffold_preloads_locked_rows_and_cfg();
    test_setup_label_targeted_steps_splice_into_scaffold();
    test_validate_setup_label_rules();
    test_validate_setup_capacity();
    test_commit_blocked_with_hint_during_set_step();
    test_workspace_load_during_tutorial_restores_baseline();
    /* REQUIRE_VAR step kind. */
    test_validate_accepts_require_var_step();
    test_validate_rejects_require_var_with_expected();
    test_validate_rejects_require_var_empty_name();
    test_validate_rejects_require_var_reserved_name();
    test_catalog_includes_variable_slider_tutorial();
    test_require_var_does_not_predeclare();
    test_require_var_advances_on_typed_assignment();
    test_require_var_declaration_does_not_skip_next_step();
    test_require_var_tab_accepts_ghost();
    test_require_var_enter_commits_declaration();
    test_require_var_advances_on_slider_drag();
    test_require_var_ignores_unrelated_assignment();
    test_require_var_pauses_on_next_step_after_match();
    test_require_var_epsilon_boundary();
    test_require_var_shadow_suffix_synthesizes_assignment();
    /* Block-structure steps (shape classifier + validator depth walk). */
    test_expected_shape_classifier();
    test_validate_accepts_balanced_block_steps();
    test_validate_block_step_rules();
    return test_harness_report(&g_harness, "test_tutorial_runner");
}
