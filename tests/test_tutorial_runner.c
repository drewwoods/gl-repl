#define _DEFAULT_SOURCE  /* mkdtemp() */
#include "app/glr_actions.h"
#include "app/glr_ctrl.h"
#include "editor/clipboard.h"
#include "config.h"
#include "editor/input.h"
#include "editor/state.h"
#include "keys.h"
#include "repl/core.h"
#include "repl/state_owners.h"
#include "repl/state_views.h"
#include "repl/tutorials.h"
#include "source_document.h"
#include "support/test_harness.h"
#include "ui/state.h"
#include "widgets/tutorial.h"
#include "widgets/tutorial_state.h"

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
    ReplEditorInputState *inp = editor_state_input_mut();
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
               status_text(), "Tutorial: step 1/11");

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
    repl_feed_line_public(expected);
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
    repl_state_edit_line_set(1);
    expected = tutorial_current_expected_text();
    set_input_text(expected);
    (void)editor_handle_key(';', 0, 0);

    doc = source_document_view();
    ASSERT_INT("step unchanged after non-append commit",
               tutorial_state_view().step, 1);
    ASSERT_STR("non-append commit status",
               status_text(),
               "Move cursor to the end of the buffer before committing");
    ASSERT_STR("non-append commit preserves input",
               editor_state_input().input, expected);
    ASSERT_INT("non-append commit does not append a new line",
               doc.line_count, 3);
}

static void test_tutorial_start_sets_step_progress_status(void) {
    reset_fixture();
    tutorial_start(0);
    ASSERT_STR("start sets step 1 status",
               status_text(), "Tutorial: step 1/5");
}

static void test_tutorial_advance_updates_step_progress_status(void) {
    const char *expected;

    reset_fixture();
    tutorial_start(0);
    expected = tutorial_current_expected_text();
    set_input_text(expected);
    (void)editor_handle_key(';', 0, 0);
    ASSERT_STR("advance sets step 2 status",
               status_text(), "Tutorial: step 2/5");
}

static void test_locked_comment_load_is_read_only(void) {
    reset_fixture();
    tutorial_start(0);
    set_input_text("glEnd()");

    load_line_to_input(0);

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
    repl_state_edit_line_set(0);

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

    reset_fixture();
    tutorial_start(0);
    expected = tutorial_current_expected_text();
    set_input_text(expected);
    (void)editor_handle_key(';', 0, 0);

    repl_state_edit_line_set(1);
    editor_clipboard_copy_current();
    repl_state_edit_line_set(0);
    editor_clipboard_paste_current();

    ASSERT_INT("paste before locked prefix keeps line count",
               repl_state_document_count(), 3);
    ASSERT_STR("paste before locked prefix status",
               status_text(), "Tutorial comment is read-only");
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
     * landing on a locked instruction would have load_line_to_input
     * overwrite the precheck status. */
    set_input_text(tutorial_current_expected_text());
    (void)editor_handle_key(';', 0, 0);
    doc_before = source_document_view();

    /* Type a parseable but non-matching line at the trailing edit row,
     * then navigate to the non-locked user line. Without the navigation-
     * path tutorial gate, commit_before_navigation would slip the line in
     * without advancing the step. */
    set_input_text("glPointSize(1)");
    repl_navigate_to_line(1);

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
    repl_navigate_to_line(1);

    doc = source_document_view();
    ASSERT_INT("navigation advance committed user line + next instruction",
               doc.line_count, 5);
    ASSERT_INT("step advanced via navigation", tutorial_state_view().step, 2);
    ASSERT_STR("navigation advance sets step 3 status",
               status_text(), "Tutorial: step 3/5");
}

static void test_enter_on_locked_line_shows_position_hint(void) {
    /* Regression: an earlier review flagged that
     * commit_current_input's unmodified+enter_mode branch could toggle
     * insert mode at a locked line. The Phase 4 precheck's position
     * guard actually catches this before commit_current_input runs;
     * lock that down so a future refactor doesn't re-open the gap. */
    reset_fixture();
    tutorial_start(0);
    set_input_text(tutorial_current_expected_text());
    (void)editor_handle_key(';', 0, 0);

    repl_navigate_to_line(0);
    (void)editor_handle_key('\n', 0, 0);

    ASSERT_STR("enter on locked line shows position hint",
               status_text(),
               "Move cursor to the end of the buffer before committing");
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
    repl_navigate_to_line(0);

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
    reset_fixture();
    tutorial_start(0);

    for (int i = repl_state_document_count(); i < MAX_COMMANDS; i++)
        repl_feed_line_public("glPointSize(1);");

    ASSERT_INT("document filled to capacity",
               repl_state_document_count(), MAX_COMMANDS);

    set_input_text(tutorial_current_expected_text());
    (void)editor_handle_key(';', 0, 0);

    ASSERT_INT("step unchanged after rejected commit",
               tutorial_state_view().step, 0);
    ASSERT_STR("capacity failure status preserved",
               status_text(), "Command buffer full!");
    ASSERT_STR("rejected commit keeps expected input",
               editor_state_input().input,
               repl_tutorial_step_expected(0, 0));
}

static void test_feed_line_alone_does_not_advance_tutorial(void) {
    const char *expected;

    reset_fixture();
    tutorial_start(0);

    expected = tutorial_current_expected_text();
    repl_feed_line_public(expected);

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
    ASSERT_INT("fade duration half second", (int)(state.fade_duration * 10.0f), 5);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "char zero starts transparent",
                              tutorial_step_fade_alpha(0, 0, line_len, state.fade_start_t),
                              0.0f);
    TEST_ASSERT_FLOAT_DEFAULT(&g_harness, "last char finishes opaque",
                              tutorial_step_fade_alpha(0, line_len - 1, line_len,
                                                       state.fade_start_t + state.fade_duration),
                              1.0f);
    ASSERT_TRUE("line stops fading after duration",
                !tutorial_line_is_fading(0, state.fade_start_t + state.fade_duration));
}

static void test_complete_and_menu_actions(void) {
    const char *expected;
    int tutorial_count;

    reset_fixture();
    tutorial_count = repl_tutorial_count();
    ASSERT_TRUE("tutorial menu action starts tutorial",
                glr_action_menu_item_activate(GLR_MENU_TUTORIALS, 0) == 1);
    ASSERT_TRUE("tutorial active from menu action", tutorial_active());

    expected = tutorial_current_expected_text();
    repl_feed_line_public(expected);
    tutorial_advance_after_successful_commit();
    ASSERT_TRUE("restart menu item restarts tutorial",
                glr_action_menu_item_activate(GLR_MENU_TUTORIALS, tutorial_count + 1) == 1);
    ASSERT_INT("restart resets step", tutorial_state_view().step, 0);

    while (tutorial_active()) {
        expected = tutorial_current_expected_text();
        ASSERT_TRUE("expected exists while tutorial active", expected != NULL);
        repl_feed_line_public(expected);
        tutorial_advance_after_successful_commit();
    }

    ASSERT_STR("completion status set", status_text(), "Tutorial complete");

    ASSERT_TRUE("exit menu item accepted when inactive",
                glr_action_menu_item_activate(GLR_MENU_TUTORIALS, tutorial_count + 2) == 1);
}

static void test_start_captures_home_for_unsaved_buffer(void) {
    reset_fixture();
    /* Type a line into a fresh buffer (no example, no user scene) so the
     * pre-tutorial state lives only in the live document. */
    repl_feed_line_public("glBegin(GL_TRIANGLES);");
    ASSERT_INT("user typed line is in document",
               repl_state_document_count(), 1);

    tutorial_start(0);
    ASSERT_TRUE("home slot captured before transient discard",
                repl_user_scene_slot_used(0));
    ASSERT_TRUE("tutorial active after start", tutorial_active());
}

static void test_start_rejects_out_of_range_idx(void) {
    reset_fixture();
    tutorial_start(repl_tutorial_count());

    ASSERT_TRUE("tutorial inactive after bad index",
                !tutorial_active());
    ASSERT_STR("bad index status set",
               status_text(), "Tutorial index out of range");
}

int main(void) {
    test_start_enters_transient_tutorial_scene();
    test_catalog_includes_color_transform_tutorial();
    test_color_transform_walkthrough();
    test_runner_match_and_advance();
    test_semicolon_route_rejects_mismatch_and_preserves_input();
    test_replace_existing_line_does_not_advance();
    test_tutorial_start_sets_step_progress_status();
    test_tutorial_advance_updates_step_progress_status();
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
    return test_harness_report(&g_harness, "test_tutorial_runner");
}
