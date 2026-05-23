#define _DEFAULT_SOURCE  /* mkdtemp() */
#include "app/glr_actions.h"
#include "app/glr_ctrl.h"
#include "editor/clipboard.h"
#include "config.h"
#include "editor/completion.h"
#include "editor/input.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "keys.h"
#include "repl/core.h"
#include "repl/state_owners.h"
#include "repl/state_views.h"
#include "repl/tutorials.h"
#include "source_document.h"
#include "support/test_harness.h"
#include "ui/app/state.h"
#include "subsystems/tutorial/tutorial.h"
#include "subsystems/tutorial/tutorial_state.h"

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
    glr_app_reset_all();
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
    ASSERT_INT("tutorial doc line count", doc.line_count, 1);
    ASSERT_TRUE("first line is comment",
                strncmp(trim_leading_ws(source_text_line(doc, 0)), "//", 2) == 0);
    ASSERT_TRUE("first line locked", tutorial_line_is_locked(0));
    ASSERT_STR("current expected text",
               tutorial_current_expected_text(),
               repl_tutorial_step_expected(0, 0));
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
    ASSERT_STR("color-transform start status",
               status_text(),
               "Tutorial: step 1/11 - type the command or press Tab to autocomplete");

    total_steps = repl_tutorial_step_count(1);
    for (idx = 0; idx < total_steps; idx++) {
        expected = tutorial_current_expected_text();
        ASSERT_TRUE("expected exists mid-tutorial", expected != NULL);
        set_input_text(expected);
        (void)editor_handle_key(';', 0, 0);
    }

    ASSERT_TRUE("color-transform tutorial completed", !tutorial_active());
    ASSERT_STR("color-transform completion status",
               status_text(), "Tutorial complete");
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

    doc = source_document_view();
    ASSERT_INT("step advanced after success", tutorial_state_view().step, 1);
    ASSERT_INT("next instruction appended", doc.line_count, 3);
    ASSERT_STR("new instruction text", trim_leading_ws(source_text_line(doc, 2)),
               repl_tutorial_step_comment(0, 1));
    ASSERT_TRUE("new instruction locked", tutorial_line_is_locked(2));
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
               doc.line_count, 1);
}

static void test_enter_route_advances_after_match(void) {
    SourceTextView doc;

    reset_fixture();
    tutorial_start(0);
    set_input_text(tutorial_current_expected_text());

    (void)editor_handle_key('\n', 0, 0);

    doc = source_document_view();
    ASSERT_INT("enter route advanced step", tutorial_state_view().step, 1);
    ASSERT_INT("enter route appended instruction", doc.line_count, 3);
    ASSERT_STR("enter route next instruction text",
               trim_leading_ws(source_text_line(doc, 2)),
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

    /* Navigate back to the previously-committed user line (line 1) and try to
     * commit the current step's expected text. The precheck must reject the
     * non-append commit so the user does not overwrite prior progress while
     * also advancing. */
    editor_state_edit_line_set(1);
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
               doc.line_count, 3);
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
    EditorAutocompleteState ac;

    reset_fixture();
    tutorial_start(0);
    set_input_text("glBe");
    editor_completion_update();

    ac = editor_state_autocomplete();
    ASSERT_STR("autocomplete ghost carries tutorial suffix",
               ac.ghost, "gin(GL_TRIANGLES)");
    ASSERT_INT("autocomplete suppresses match list during tutorial",
               ac.match_count, 0);
    ASSERT_STR("autocomplete suppresses param hint during tutorial",
               ac.hint, "");
}

static void test_shadow_text_appears_immediately_on_start(void) {
    /* Regression: tutorial_start must poke editor_completion_update()
     * itself so the shadow ghost appears on the first frame instead
     * of waiting for the user's next keystroke. */
    EditorAutocompleteState ac;

    reset_fixture();
    tutorial_start(0);
    /* No keystroke, no manual editor_completion_update() — just read. */
    ac = editor_state_autocomplete();
    ASSERT_STR("ghost populated immediately after tutorial_start",
               ac.ghost, "glBegin(GL_TRIANGLES)");
}

static void test_shadow_text_refreshes_on_advance(void) {
    /* Regression: tutorial_advance_after_successful_commit must
     * refresh autocomplete so the next step's shadow appears on the
     * very next frame, not after the user types again. */
    EditorAutocompleteState ac;

    reset_fixture();
    tutorial_start(0);
    set_input_text(tutorial_current_expected_text());
    (void)editor_handle_key(';', 0, 0);

    ac = editor_state_autocomplete();
    ASSERT_STR("ghost shows next step's expected after advance",
               ac.ghost, "glVertex3f(0, 0.8, 0)");
}

static void test_shadow_ghost_falls_through_off_expected_line(void) {
    /* On the expected commit line, the tutorial shadow suffix takes
     * over autocomplete. On any other line the user is editing
     * unrelated code, so normal autocomplete should run. */
    EditorAutocompleteState ac;
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
                ac.match_count > 0);
    ASSERT_TRUE("ghost is not the tutorial expected text",
                strcmp(ac.ghost, "glBegin(GL_TRIANGLES)") != 0);
}

static void test_ghost_reappears_on_return_to_expected_line(void) {
    /* Navigating to a non-tutorial line clears the shadow ghost so
     * stale text doesn't follow the cursor. Navigating back to the
     * expected commit line should restore the ghost without
     * requiring the user to type. */
    EditorAutocompleteState ac;
    int expected_line;

    reset_fixture();
    tutorial_start(0);
    expected_line = tutorial_expected_commit_line();
    ASSERT_TRUE("expected_commit_line off line zero", expected_line != 0);

    ac = editor_state_autocomplete();
    ASSERT_STR("ghost shows on the expected line at start",
               ac.ghost, "glBegin(GL_TRIANGLES)");

    editor_navigate_to_line(0);
    ac = editor_state_autocomplete();
    ASSERT_STR("ghost clears after navigating off-line",
               ac.ghost, "");

    editor_navigate_to_line(expected_line);
    ac = editor_state_autocomplete();
    ASSERT_STR("ghost restored after returning to expected line",
               ac.ghost, "glBegin(GL_TRIANGLES)");
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
    /* Regression: tutorial_exit must refresh autocomplete so the
     * ghost from the in-progress step clears immediately. */
    EditorAutocompleteState ac;

    reset_fixture();
    tutorial_start(0);
    tutorial_exit();

    ac = editor_state_autocomplete();
    ASSERT_STR("ghost clears on tutorial_exit", ac.ghost, "");
}

static void test_tutorial_start_sets_step_progress_status(void) {
    reset_fixture();
    tutorial_start(0);
    ASSERT_STR("start sets step 1 status",
               status_text(),
               "Tutorial: step 1/5 - type the command or press Tab to autocomplete");
}

static void test_tutorial_advance_updates_step_progress_status(void) {
    const char *expected;

    reset_fixture();
    tutorial_start(0);
    expected = tutorial_current_expected_text();
    set_input_text(expected);
    (void)editor_handle_key(';', 0, 0);
    ASSERT_STR("advance sets step 2 status",
               status_text(),
               "Tutorial: step 2/5 - type the command or press Tab to autocomplete");
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
    int got;

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
               "Tutorial: step 1/5 - type the command or press Tab to autocomplete");

    /* COMMAND step, full match on expected line: commit reminder. */
    set_input_text(tutorial_current_expected_text());
    got = tutorial_status_hint(buf, sizeof buf);
    ASSERT_INT("COMMAND match returns 1", got, 1);
    ASSERT_STR("COMMAND match uses commit reminder",
               buf,
               "Tutorial: step 1/5 - press Enter or ';' to commit");

    /* tutorial_status_is_hint recognises the prefix on either variant
     * and rejects unrelated text. */
    ASSERT_INT("entry hint recognised",
               tutorial_status_is_hint(
                   "Tutorial: step 1/5 - type the command or press Tab to autocomplete"),
               1);
    ASSERT_INT("commit hint recognised",
               tutorial_status_is_hint(
                   "Tutorial: step 1/5 - press Enter or ';' to commit"),
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

    /* Active: empty input is a no-op (status keeps the entry hint). */
    reset_fixture();
    tutorial_start(0);
    tutorial_refresh_input_hint("");
    ASSERT_STR("empty input does not overwrite entry status",
               status_text(),
               "Tutorial: step 1/5 - type the command or press Tab to autocomplete");

    /* Active: a strict prefix of expected is also a no-op. */
    expected = tutorial_current_expected_text();
    ASSERT_TRUE("expected exists", expected != NULL);
    set_input_text("glBeg");
    tutorial_refresh_input_hint("glBeg");
    ASSERT_STR("partial input does not overwrite entry status",
               status_text(),
               "Tutorial: step 1/5 - type the command or press Tab to autocomplete");

    /* Active: full match refreshes status with the commit reminder. */
    set_input_text(expected);
    tutorial_refresh_input_hint(expected);
    ASSERT_STR("full match sets commit reminder",
               status_text(),
               "Tutorial: step 1/5 - press Enter or ';' to commit");

    /* Inactive: never writes. Park a sentinel status, exit the tutorial,
     * and confirm the call leaves it intact. */
    tutorial_exit();
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
               status_text(), "Tutorial instruction is read-only");
}

static void test_locked_comment_mutations_are_blocked(void) {
    reset_fixture();
    tutorial_start(0);
    editor_state_edit_line_set(0);

    (void)editor_handle_key(KEY_CTRL_D, 0, 0);
    ASSERT_INT("ctrl-d keeps locked comment row",
               repl_state_document_count(), 1);
    ASSERT_STR("ctrl-d read-only status",
               status_text(), "Tutorial comment is read-only");

    (void)editor_handle_key(KEY_CTRL_L, 0, 0);
    ASSERT_INT("ctrl-l keeps tutorial rows",
               repl_state_document_count(), 1);
    ASSERT_STR("ctrl-l read-only status",
               status_text(), "Tutorial comment is read-only");

    (void)editor_handle_key(KEY_CTRL_BACKSLASH, 0, 0);
    ASSERT_INT("ctrl-backslash keeps tutorial rows",
               repl_state_document_count(), 1);
    ASSERT_STR("ctrl-backslash read-only status",
               status_text(), "Tutorial comment is read-only");
}

static void test_paste_before_locked_prefix_is_blocked(void) {
    const char *expected;
    EditorUndoRingState ring_before, ring_after;

    reset_fixture();
    tutorial_start(0);
    expected = tutorial_current_expected_text();
    set_input_text(expected);
    (void)editor_handle_key(';', 0, 0);

    editor_state_edit_line_set(1);
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
               repl_state_document_count(), 3);
    ASSERT_STR("paste before locked prefix status",
               status_text(), "Tutorial comment is read-only");
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
     * then navigate to the non-locked user line. Without the navigation-
     * path tutorial gate, commit_before_navigation would slip the line in
     * without advancing the step. */
    set_input_text("glPointSize(1)");
    editor_navigate_to_line(1);

    doc_after = source_document_view();
    ASSERT_INT("navigation does not commit non-matching line",
               doc_after.line_count, doc_before.line_count);
    ASSERT_INT("step unchanged after rejected navigation",
               tutorial_state_view().step, 1);
    ASSERT_STR("navigation rejection surfaces hint status",
               status_text(), "expected: glVertex3f(0, 0.8, 0)");
}

static void test_navigation_advances_on_matching_input(void) {
    SourceTextView doc;
    const char *expected;

    reset_fixture();
    tutorial_start(0);
    /* Commit step 0 cleanly so navigation lands on a non-locked user line. */
    set_input_text(tutorial_current_expected_text());
    (void)editor_handle_key(';', 0, 0);

    /* Type the current expected text at trailing edit row, navigate up.
     * The navigation commit should advance the tutorial. */
    expected = tutorial_current_expected_text();
    set_input_text(expected);
    editor_navigate_to_line(1);

    doc = source_document_view();
    ASSERT_INT("navigation advance committed user line + next instruction",
               doc.line_count, 5);
    ASSERT_INT("step advanced via navigation", tutorial_state_view().step, 2);
    ASSERT_STR("navigation advance sets step 3 status",
               status_text(),
               "Tutorial: step 3/5 - type the command or press Tab to autocomplete");
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
               "Tutorial instruction is read-only");
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
               repl_state_document_count(), 1);
    ASSERT_STR("ctrl-/ read-only status",
               status_text(), "Tutorial comment is read-only");
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
               doc.line_count, 3);
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

    for (int i = repl_state_document_count(); i < MAX_COMMANDS; i++)
        editor_feed_line("glPointSize(1);");

    ASSERT_INT("document filled to capacity",
               repl_state_document_count(), MAX_COMMANDS);

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

static void test_fade_alpha_math(void) {
    SourceTextView doc;
    TutorialRuntimeState state;
    int line_len;

    reset_fixture();
    tutorial_start(0);
    doc = source_document_view();
    state = tutorial_state_view();
    line_len = (int)strlen(source_text_line(doc, 0));

    ASSERT_TRUE("line is fading at start",
                tutorial_line_is_fading(0, state.fade_start_t + 0.01f));
    ASSERT_INT("fade line idx is first row", state.fade_line_idx, 0);
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
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "char zero starts transparent",
                              tutorial_step_fade_alpha(0, 0, line_len, state.fade_start_t),
                              0.0f);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "last char finishes opaque",
                              tutorial_step_fade_alpha(0, line_len - 1, line_len,
                                                       state.fade_start_t + state.fade_duration),
                              1.0f);
    /* Settle factor lags behind the reveal: at fade_start_t a char is
     * just appearing in bright white (settle=0) and the last char's
     * settle finishes exactly at fade_start_t + fade_duration. */
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "char zero settle starts at white",
                              tutorial_step_fade_settle(0, 0, line_len,
                                                        state.fade_start_t),
                              0.0f);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "last char settle finishes at base",
                              tutorial_step_fade_settle(0, line_len - 1, line_len,
                                                        state.fade_start_t +
                                                            state.fade_duration),
                              1.0f);
    ASSERT_TRUE("line stops fading after duration",
                !tutorial_line_is_fading(0, state.fade_start_t + state.fade_duration));
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
                glr_action_menu_item_activate(GLR_MENU_TUTORIALS, tag_count + 1) == 1);
    ASSERT_INT("restart resets step", tutorial_state_view().step, 0);

    while (tutorial_active()) {
        expected = tutorial_current_expected_text();
        ASSERT_TRUE("expected exists while tutorial active", expected != NULL);
        editor_feed_line(expected);
        tutorial_advance_after_successful_commit();
    }

    ASSERT_STR("completion status set", status_text(), "Tutorial complete");

    /* After completion no tutorial is active, so the trailing Restart/Exit
     * rows don't exist. The MENU_TUTORIALS activation handler still has a
     * catch-all `return 1` at the bottom, so out-of-range indices report
     * "handled" (the original behavior — preserved for non-regression). */
    ASSERT_TRUE("exit menu item accepted when inactive",
                glr_action_menu_item_activate(
                    GLR_MENU_TUTORIALS,
                    repl_tutorial_visible_tag_count() + 2) == 1);
}

static void test_start_captures_home_for_unsaved_buffer(void) {
    reset_fixture();
    /* Type a line into a fresh buffer (no example, no user scene) so the
     * pre-tutorial state lives only in the live document. */
    editor_feed_line("glBegin(GL_TRIANGLES);");
    ASSERT_INT("user typed line is in document",
               repl_state_document_count(), 1);

    tutorial_start(0);
    ASSERT_TRUE("home slot captured before transient discard",
                repl_user_scene_slot_used(0));
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
    /* First Triangle ships a leading `@cfg view_mode = 1` so the flat
     * triangle renders in true 2D; every other shipped tutorial omits
     * cfg (NULL = no presentation overrides). Out-of-range idx → NULL. */
    int first = -1;
    for (int i = 0; i < repl_tutorial_count(); i++) {
        const char *name = repl_tutorial_name(i);
        if (name && strcmp(name, "First Triangle") == 0) { first = i; break; }
    }
    ASSERT_TRUE("First Triangle is in catalog", first >= 0);
    if (first >= 0) {
        const char *const *cfg = repl_tutorial_cfg_lines(first);
        ASSERT_TRUE("First Triangle has cfg lines", cfg != NULL);
        if (cfg) {
            ASSERT_TRUE("First Triangle cfg first line is view_mode = 1",
                        cfg[0] != NULL &&
                        strstr(cfg[0], "view_mode") != NULL &&
                        strstr(cfg[0], "1") != NULL);
            ASSERT_TRUE("First Triangle cfg is NULL-terminated after 1 line",
                        cfg[1] == NULL);
        }
    }

    /* Tutorials that opt into entry-level @cfg list them by name here; the
     * rest must leave cfg NULL so the field stays genuinely opt-in. */
    for (int i = 0; i < repl_tutorial_count(); i++) {
        const char *name = repl_tutorial_name(i);
        if (name && (strcmp(name, "First Triangle") == 0 ||
                     strcmp(name, "Feature Tour") == 0))
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

    /* Block-opening braces ({ }) would commit a CMD_BLOCK and shift
     * the document by more than one row. */
    static const TutorialStep brace_steps[] = {
        { NULL, "// opens a block",
                "for(i, 0, 3) {",
                TUTORIAL_STEP_APPEND, NULL },
        { NULL, NULL, NULL, TUTORIAL_STEP_APPEND, NULL },
    };
    TutorialEntry entry_brace = { .name = "block_open", .steps = brace_steps };
    err[0] = '\0';
    ASSERT_TRUE("expected with '{' rejected",
                !repl_tutorial_validate_entry(&entry_brace, err, sizeof(err)));

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

    /* The labeled step (index 0) emits its instruction at row 0;
     * record that anchor before the appends. */
    int instruction_row = tutorial_state_view().instruction_line_for_step[0];
    ASSERT_INT("step 0 instruction recorded at row 0", instruction_row, 0);

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
     * instruction row (which has stayed at 0 because every prior
     * append went strictly below it). The original step-0
     * instruction shifted to row 1 and the originally-labeled
     * glBegin shifted to row 2 — keeping the (instruction,
     * command) pair adjacent. */
    SourceTextView doc = source_document_view();
    const char *new_instruction = source_text_line(doc, 0);
    const char *orig_instruction = source_text_line(doc, 1);
    const char *labeled_line = source_text_line(doc, 2);
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
                tutorial_line_is_locked(0));
    ASSERT_INT("expected_commit_line lands directly below new instruction",
               tutorial_state_view().expected_commit_line, 1);
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
    for (int i = repl_state_document_count(); i < MAX_COMMANDS; i++)
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
    ASSERT_STR("tutorial completion status",
               status_text(), "Tutorial complete");

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
               status_text(), "Tutorial comment is read-only");
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
        ASSERT_TRUE("locked line still points at a tutorial comment",
                    text != NULL && strstr(text, "//") != NULL);
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

/* Drive one COMMAND step's commit via the ; key route. Returns 1 on success
 * (step advanced), 0 otherwise. */
static int commit_command_step(int idx, int step) {
    const char *expected = repl_tutorial_step_expected(idx, step);
    if (!expected) return 0;
    set_input_text(expected);
    editor_handle_key(';', 0, 0);
    return tutorial_state_view().step > step;
}

/* Walk the Feature Tour up through its 5 COMMAND steps so the runner lands
 * on step 5 (the REQUIRE vertex_outlines step). Returns the tutorial idx
 * on success, -1 if the catalog doesn't contain Feature Tour. */
static int start_feature_tour_and_walk_commands(void) {
    int idx = find_tutorial_idx("Feature Tour");
    if (idx < 0) return -1;
    tutorial_start(idx);
    for (int s = 0; s < 5; s++)
        if (!commit_command_step(idx, s)) return -1;
    return idx;
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
    ASSERT_TRUE("feature tour walks 5 commands", idx >= 0);
    ASSERT_TRUE("tutorial still active at REQUIRE", tutorial_active());

    TutorialRuntimeState st = tutorial_state_view();
    ASSERT_INT("on REQUIRE step (step 5)", st.step, 5);
    ASSERT_INT("REQUIRE step kind",
               (int)tutorial_current_step_kind(),
               (int)TUTORIAL_STEP_KIND_REQUIRE);
    int instr_line = st.instruction_line_for_step[5];
    ASSERT_TRUE("REQUIRE instruction line recorded", instr_line >= 0);
    /* Cursor must NOT be on the instruction-comment row; the runner
     * parks it at instruction_line + 1 (the virtual trailing row). */
    ASSERT_INT("cursor parked past REQUIRE comment row",
               editor_state_edit_line(), instr_line + 1);

    /* Advance past REQUIRE by setting vertex_outlines (the notify hook
     * in glr_config_set drives this). The next step is SET grid=10. */
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 1);
    st = tutorial_state_view();
    ASSERT_INT("REQUIRE advanced to SET grid=Radar", st.step, 6);
    ASSERT_INT("SET step kind",
               (int)tutorial_current_step_kind(),
               (int)TUTORIAL_STEP_KIND_SET);
    int set_instr = st.instruction_line_for_step[6];
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
    /* Drive through REQUIRE to reach the first SET (grid = Radar = 10). */
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 1);
    ASSERT_INT("entered SET grid=Radar", tutorial_state_view().step, 6);
    ASSERT_INT("cfg grid applied to Radar",
               repl_cfg_get_int("grid", -1), 10);

    /* Ack via the controller router; SET advances to next SET (Focus = 6). */
    glr_ctrl_keyboard('\r', 0, 0);
    ASSERT_INT("ack key advanced to SET grid=Focus",
               tutorial_state_view().step, 7);
    ASSERT_INT("cfg grid applied to Focus",
               repl_cfg_get_int("grid", -1), 6);

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
    /* Step count must include the non-command steps too — sentinel is
     * keyed on `comment` alone now. */
    int n = 0;
    while (steps[n].comment) n++;
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

/* SET/REQUIRE steps must reject typed commits with a kind-appropriate hint
 * — not the misleading "Move cursor to the tutorial insertion line". */
static void test_commit_blocked_with_hint_during_set_step(void) {
    reset_fixture();
    int idx = start_feature_tour_and_walk_commands();
    ASSERT_TRUE("walked into REQUIRE", idx >= 0);
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 1);
    int step_before = tutorial_state_view().step;
    ASSERT_INT("at SET step", step_before, 6);

    /* Attempt to commit arbitrary text — the precheck must reject with
     * the SET hint. */
    set_input_text("glPointSize(2)");
    editor_handle_key(';', 0, 0);

    ASSERT_INT("step unchanged after rejected SET commit",
               tutorial_state_view().step, step_before);
    ASSERT_STR("SET hint shown (not the cursor-position hint)",
               status_text(), "Press Enter / Tab / Space to continue");
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
    ASSERT_INT("now on SET grid=Radar", tutorial_state_view().step, 6);
    ASSERT_INT("grid is RADAR mid-tutorial", repl_cfg_get_int("grid", -1), 10);

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
     * 0, not the SET-step's RADAR=10. */
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 1);
    glr_config_set(GLR_CONFIG_GRID_THEME, 0);
    int outlines_baseline = repl_cfg_get_int("vertex_outlines", -1);
    int grid_baseline     = repl_cfg_get_int("grid", -1);
    ASSERT_INT("vertex_outlines baseline 1", outlines_baseline, 1);
    ASSERT_INT("grid baseline OFF", grid_baseline, 0);

    int idx = start_feature_tour_and_walk_commands();
    ASSERT_TRUE("walked into REQUIRE", idx >= 0);
    ASSERT_INT("on REQUIRE step", tutorial_state_view().step, 5);
    ASSERT_INT("REQUIRE kind",
               (int)tutorial_current_step_kind(),
               (int)TUTORIAL_STEP_KIND_REQUIRE);
    /* presentation_reset wiped vertex_outlines back to its default
     * (0) — the REQUIRE is unsatisfied, which is the precondition for
     * the bug: a restore write of 1 (the baseline) would match. */
    ASSERT_INT("REQUIRE unsatisfied at exit", repl_cfg_get_int("vertex_outlines", -1), 0);

    tutorial_exit();
    ASSERT_TRUE("tutorial inactive after exit", !tutorial_active());
    ASSERT_INT("vertex_outlines restored to baseline",
               repl_cfg_get_int("vertex_outlines", -1), outlines_baseline);
    /* The whole point: the next-step SET (grid = RADAR = 10) must NOT
     * have fired during the restore. Pre-fix, the notify hook saw a
     * matching REQUIRE on the vertex_outlines write and auto-advanced,
     * stamping grid=10 after grid had already been restored. */
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

    /* Walk Feature Tour past REQUIRE so the SET grid=RADAR(10) fires. */
    int idx1 = start_feature_tour_and_walk_commands();
    ASSERT_TRUE("walked tour 1 into REQUIRE", idx1 >= 0);
    glr_config_set(GLR_CONFIG_VERTEX_OUTLINES, 1); /* advances REQUIRE → SET */
    ASSERT_INT("grid mutated to RADAR mid-tutorial 1",
               repl_cfg_get_int("grid", -1), 10);
    ASSERT_TRUE("tutorial 1 still active", tutorial_active());

    /* Start tutorial 2 without exiting tutorial 1. Pre-fix: the new
     * tutorial's baseline capture saw grid=10 and enshrined it; the
     * final exit would restore grid=10. Post-fix: the inner teardown
     * restores grid=0 first, the new baseline captures grid=0. */
    int idx2 = find_tutorial_idx("First Triangle");
    ASSERT_TRUE("found First Triangle", idx2 >= 0);
    tutorial_start(idx2);
    ASSERT_TRUE("tutorial 2 active", tutorial_active());
    ASSERT_INT("tutorial 2 idx", tutorial_state_view().tutorial_idx, idx2);

    /* Exit tutorial 2: its baseline (captured after teardown of tour 1)
     * must equal the original user baseline. */
    tutorial_exit();
    ASSERT_TRUE("tutorial 2 inactive after exit", !tutorial_active());
    ASSERT_INT("grid restored to ORIGINAL baseline (not tour-1 mutated)",
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

    tutorial_exit();
    ASSERT_TRUE("color-transform inactive after exit", !tutorial_active());
    ASSERT_INT("view_mode restored to pre-tutorial 2D",
               repl_cfg_get_int("view_mode", -1), 1);
}

int main(void) {
    test_exit_on_require_does_not_autoadvance();
    test_restart_during_tutorial_preserves_original_baseline();
    test_baseline_captures_view_mode_even_when_unreferenced();
    test_start_enters_transient_tutorial_scene();
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
    test_fade_alpha_math();
    test_complete_and_menu_actions();
    test_start_captures_home_for_unsaved_buffer();
    test_start_rejects_out_of_range_idx();
    test_catalog_starter_steps_are_append();
    test_catalog_cfg_lines();
    test_catalog_tag_metadata();
    test_catalog_subheading_metadata();
    test_catalog_validation_passes_for_all_tutorials();
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
    test_commit_blocked_with_hint_during_set_step();
    test_workspace_load_during_tutorial_restores_baseline();
    return test_harness_report(&g_harness, "test_tutorial_runner");
}
