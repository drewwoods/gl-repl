#define _DEFAULT_SOURCE  /* mkdtemp() */
#include "app/glr_actions.h"
#include "app/glr_ctrl.h"
#include "editor/clipboard.h"
#include "config.h"
#include "editor/completion.h"
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
    repl_state_edit_line_set(1);
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
    ReplAutocompleteState ac;

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
    ReplAutocompleteState ac;

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
    ReplAutocompleteState ac;

    reset_fixture();
    tutorial_start(0);
    set_input_text(tutorial_current_expected_text());
    (void)editor_handle_key(';', 0, 0);

    ac = editor_state_autocomplete();
    ASSERT_STR("ghost shows next step's expected after advance",
               ac.ghost, "glVertex3f(0, 0.8, 0)");
}

static void test_shadow_text_clears_on_exit(void) {
    /* Regression: tutorial_exit must refresh autocomplete so the
     * ghost from the in-progress step clears immediately. */
    ReplAutocompleteState ac;

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
               status_text(), "Tutorial: step 3/5");
}

static void test_enter_on_locked_line_shows_position_hint(void) {
    /* Regression: an earlier review flagged that
     * commit_current_input's unmodified+enter_mode branch could toggle
     * insert mode at a locked line. With the Phase 3 precheck,
     * landing on a locked line first triggers load_line_to_input,
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
    /* Filling the document via feed_line (which intentionally
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
    editor_feed_line(expected);
    tutorial_advance_after_successful_commit();
    ASSERT_TRUE("restart menu item restarts tutorial",
                glr_action_menu_item_activate(GLR_MENU_TUTORIALS, tutorial_count + 1) == 1);
    ASSERT_INT("restart resets step", tutorial_state_view().step, 0);

    while (tutorial_active()) {
        expected = tutorial_current_expected_text();
        ASSERT_TRUE("expected exists while tutorial active", expected != NULL);
        editor_feed_line(expected);
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
    /* Phase 2: walk the depth-test triangle tutorial up to the
     * label-targeted step and confirm the new instruction comment
     * lands above the row holding glBegin(GL_TRIANGLES), not at the
     * tail of the document. Use editor_feed_line +
     * tutorial_advance_after_successful_commit since the Phase 3
     * precheck has not landed yet and feed_line is the existing
     * way these tests step through append commits. */
    int t_idx = depth_tutorial_idx();
    ASSERT_TRUE("Depth Test Triangle present in catalog", t_idx >= 0);
    if (t_idx < 0)
        return;

    reset_fixture();
    tutorial_start(t_idx);

    /* The labeled step (index 0) is glBegin(GL_TRIANGLES). Step 5
     * is the label-targeted glEnable insert. Walk the five append
     * steps; after each, record where the just-committed source row
     * landed so we can assert the splice landed above it. */
    int begin_row = -1;
    for (int s = 0; s < 5; s++) {
        const char *expected = tutorial_current_expected_text();
        editor_feed_line(expected);
        if (s == 0) {
            /* The first append commit landed at the row immediately
             * below the instruction-comment for step 0. The doc
             * currently has [instruction, command], so begin_row is
             * doc_count - 1. */
            begin_row = repl_state_document_count() - 1;
        }
        tutorial_advance_after_successful_commit();
    }

    /* We are now on the label-targeted step. expected_commit_line
     * should sit *above* begin_row (specifically begin_row + 1,
     * since the new instruction comment inserted at begin_row and
     * pushed everything below it down by one). */
    int t_count = repl_tutorial_step_count(t_idx);
    ASSERT_INT("walk advanced to label-targeted step",
               tutorial_state_view().step, 5);
    ASSERT_TRUE("more steps left after walking through append",
                t_count >= 6);

    /* Read out the current document; the instruction comment for
     * step 5 should be at begin_row, and glBegin(GL_TRIANGLES);
     * should have shifted to begin_row + 1. */
    SourceTextView doc = source_document_view();
    const char *instruction_line = source_text_line(doc, begin_row);
    const char *labeled_line = source_text_line(doc, begin_row + 1);
    ASSERT_TRUE("instruction comment present at the splice row",
                instruction_line &&
                strstr(instruction_line,
                       "Enable depth testing before the triangle") != NULL);
    ASSERT_TRUE("originally-labeled command moved one row down",
                labeled_line &&
                strstr(labeled_line, "glBegin(GL_TRIANGLES)") != NULL);

    ASSERT_TRUE("new instruction row is locked",
                tutorial_line_is_locked(begin_row));
    ASSERT_INT("expected_commit_line lands directly below instruction",
               tutorial_state_view().expected_commit_line, begin_row + 1);
    ASSERT_TRUE("editor cursor moved to expected commit line",
                repl_state_edit_line() ==
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
     * (which exercises the precheck) rather than feed_line. */
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
    const char *glbegin   = source_text_line(doc, target_line + 1);
    ASSERT_TRUE("committed glEnable lands at the expected commit line",
                committed && strstr(committed, "glEnable(GL_DEPTH_TEST)") != NULL);
    ASSERT_TRUE("originally-labeled glBegin is now directly below",
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
    repl_state_edit_line_set(repl_state_document_count());
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

    ReplEditorInputState *inp = editor_state_input_mut();
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
    repl_state_edit_line_set(expected_line);
    editor_insert_mode_set(expected_line < repl_state_document_count());
    set_input_text(tutorial_current_expected_text());
    (void)editor_handle_key(';', 0, 0);

    ASSERT_INT("pending.step_idx reset after rejected commit",
               tutorial_state_view().pending.step_idx, -1);
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

    repl_state_edit_line_set(1);
    editor_clipboard_copy_current();
    repl_state_edit_line_set(0);
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

    /* committed_line_for_step entries that were set during the walk
     * should still resolve to the matching expected text. step 0
     * (the labeled step) was glBegin(GL_TRIANGLES). */
    int begin_step_line = state.committed_line_for_step[0];
    ASSERT_TRUE("committed_line_for_step[0] points at glBegin",
                begin_step_line >= 0 &&
                begin_step_line < doc.line_count &&
                source_text_line(doc, begin_step_line) &&
                strstr(source_text_line(doc, begin_step_line),
                       "glBegin(GL_TRIANGLES)") != NULL);
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

int main(void) {
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
    test_shadow_text_clears_on_exit();
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
    test_catalog_starter_steps_are_append();
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
    return test_harness_report(&g_harness, "test_tutorial_runner");
}
