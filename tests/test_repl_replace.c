/*
 * test_repl_replace.c - Find-bar replace: matching, rename transactions,
 * rollback, undo, and the replace-row key routing.
 *
 * The headline case is the one the incremental commit path cannot do:
 * renaming a variable or a funcN alias, where every intermediate document
 * is invalid and only the whole rewrite parses.
 */
#include "editor/state.h"
#include "editor/input.h"
#include "editor/replace.h"
#include "editor/search.h"
#include "editor/undo.h"
#include "app/glr_ctrl.h"
#include "keys.h"

#include "repl/state.h"
#include "repl/pipeline.h"
#include "ui/core/text_search.h"
#include "ui/app/menu_bar.h"
#include "ui/app/state.h"
#include "ui/app/layout.h"
#include "ui/app/snapshot.h"

#include "support/test_harness.h"
#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

static void declare_test_vars(void) {
    char err[128];
    repl_eval_declare_predef_var("i", err, sizeof(err));
}

static void open_search(void) {
    editor_handle_key(KEY_CTRL_F, 0, 0);
}

static void type_keys(const char *text) {
    while (*text) {
        editor_handle_key((unsigned char)*text, 0, 0);
        text++;
    }
}

/* Drive the find and replace fields the way the user does: Ctrl+F, type
 * the query, Tab into the replace row, type the replacement. */
static void set_find_and_replace(const char *find, const char *replace) {
    open_search();
    type_keys(find);
    editor_handle_key('\t', 0, 0);
    type_keys(replace);
}

static int buffer_has_line(const char *needle) {
    for (int i = 0; i < editor_buffer_count(); i++) {
        const char *line = editor_buffer_line(i);
        if (line && strstr(line, needle))
            return 1;
    }
    return 0;
}

static int buffer_line_count_containing(const char *needle) {
    int n = 0;
    for (int i = 0; i < editor_buffer_count(); i++) {
        const char *line = editor_buffer_line(i);
        if (line && strstr(line, needle))
            n++;
    }
    return n;
}

/* Sweep a band of the find bar for a hit of `kind` (and `item_idx`, or
 * -1 for "any"). Coarse on purpose: it pins the routing contract without
 * hard-coding widget pixel math that the renderer owns.
 *
 * `band` picks FIND_ROW (the menu-bar row itself) or BOTH_ROWS (adding
 * the replace row hanging below it) - the distinction matters because
 * the replace row's own surface also reports "focus the replace field". */
#define BAND_FIND_ROW  0
#define BAND_BOTH_ROWS 1

static int find_search_hit_in(int band, int kind, int item_idx, UiHit *out) {
    UiRenderSnapshot snap;
    int win_h = ui_state_viewport().window_h;
    int bar_x, bar_y, bar_w, bar_h;
    int y_lo;

    glr_ctrl_build_ui_snapshot(&snap);
    ui_layout_menu_bar_rect(&bar_x, &bar_y, &bar_w, &bar_h);
    y_lo = (band == BAND_BOTH_ROWS) ? bar_y - bar_h : bar_y;

    for (int gl_y = y_lo; gl_y < bar_y + bar_h; gl_y++) {
        int my = win_h - gl_y;
        for (int mx = bar_x; mx < bar_x + bar_w; mx++) {
            UiHit hit = ui_menu_bar_search_hit_test(&snap, mx, my);
            if (hit.kind != kind)
                continue;
            if (item_idx >= 0 && hit.item_idx != item_idx)
                continue;
            if (out) *out = hit;
            return 1;
        }
    }
    return 0;
}

int main(void) {
    repl_eval_init_predef_vars();

    /* ---- whole-word matching primitives ---- */
    ASSERT_TRUE("substring matches inside identifier",
                ui_text_find_next_in_text_opts("glColor3f(1, 0, 0);", "r", 0, 0) == 6);
    ASSERT_TRUE("whole word skips identifier interior",
                ui_text_find_next_in_text_opts("glColor3f(1, 0, 0);", "r", 0, 1) < 0);
    ASSERT_TRUE("whole word matches standalone identifier",
                ui_text_find_next_in_text_opts("glVertex3f(r, 0, 0);", "r", 0, 1) == 11);
    ASSERT_TRUE("whole word rejects prefix of longer name",
                ui_text_find_next_in_text_opts("radius = 2;", "rad", 0, 1) < 0);
    /* Only the ends that carry identifier characters are bounded: "(GL_FOG"
     * starts with punctuation, so nothing constrains its left edge, while
     * the bare "(GL" is still rejected by the '_' that follows it. */
    ASSERT_TRUE("whole word allows punctuation-flanked query",
                ui_text_find_next_in_text_opts("glEnable(GL_FOG);", "(GL_FOG", 0, 1) == 8);
    ASSERT_TRUE("whole word still bounds the identifier end",
                ui_text_find_next_in_text_opts("glEnable(GL_FOG);", "(GL", 0, 1) < 0);
    ASSERT_TRUE("whole word prev scan honors boundaries",
                ui_text_find_prev_in_text_opts("r + glColor3f", "r", 99, 1) == 0);
    ASSERT_TRUE("whole word matches_at is boundary aware",
                ui_text_matches_at_opts("glColor3f", "r", 6, 1) == 0 &&
                ui_text_matches_at_opts("glColor3f", "r", 6, 0) == 1);

    /* ---- the flag drives the live match count ---- */
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("float r;");
    editor_feed_line("r = 2;");
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_navigate_to_line(0);
    open_search();
    type_keys("r");
    {
        int loose = editor_state_search()->match_count;
        editor_search_toggle_whole_word();
        ASSERT_TRUE("whole word flag set", editor_state_search()->whole_word == 1);
        ASSERT_TRUE("whole word narrows the match count",
                    editor_state_search()->match_count < loose);
        /* Loose: the declaration, the assignment, and the r inside
         * glColor3f. Whole word drops only the last one. */
        ASSERT_TRUE("loose count includes the identifier interior", loose == 3);
        ASSERT_TRUE("whole word keeps the real variable uses",
                    editor_state_search()->match_count == 2);
        editor_search_toggle_whole_word();
        ASSERT_TRUE("toggle restores loose count",
                    editor_state_search()->match_count == loose);
    }

    /* ---- rename a declared variable: the case per-line commits reject ---- */
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("float r;");
    editor_feed_line("r = 0.5;");
    editor_feed_line("glVertex3f(r, r, 0);");
    editor_navigate_to_line(0);
    set_find_and_replace("r", "radius");
    editor_search_toggle_whole_word();
    {
        int replaced = editor_replace_all();
        ASSERT_TRUE("rename replaced every whole-word use", replaced == 4);
        ASSERT_TRUE("rename rewrote the declaration",
                    buffer_has_line("float radius;"));
        ASSERT_TRUE("rename rewrote the assignment",
                    buffer_has_line("radius = 0.5;"));
        ASSERT_TRUE("rename rewrote the use site",
                    buffer_has_line("glVertex3f(radius, radius, 0);"));
        ASSERT_TRUE("renamed variable is registered",
                    repl_eval_find_predef_var_idx("radius") >= 0);
        ASSERT_TRUE("old variable name is gone",
                    repl_eval_find_predef_var_idx("r") < 0);
        ASSERT_TRUE("rename left no stray glColor damage",
                    buffer_line_count_containing("radius") == 3);
    }

    /* Undo restores the pre-replace document in one step. */
    editor_undo_pop_snapshot();
    ASSERT_TRUE("undo restores the old declaration",
                buffer_has_line("float r;"));
    ASSERT_TRUE("undo restores the old use site",
                buffer_has_line("glVertex3f(r, r, 0);"));
    ASSERT_TRUE("undo restores the old variable",
                repl_eval_find_predef_var_idx("r") >= 0);

    /* ---- the variable's live value survives the rename ---- */
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("float spin;");
    editor_feed_line("glRotatef(spin, 0, 1, 0);");
    {
        int idx = repl_eval_find_predef_var_idx("spin");
        ASSERT_TRUE("spin declared", idx >= 0);
        repl_eval_predef_vars_mut()[idx].value = 42.0f;
    }
    editor_navigate_to_line(0);
    set_find_and_replace("spin", "angle");
    editor_search_toggle_whole_word();
    ASSERT_TRUE("rename with live value applied", editor_replace_all() == 2);
    {
        int idx = repl_eval_find_predef_var_idx("angle");
        ASSERT_TRUE("renamed variable exists", idx >= 0);
        ASSERT_TRUE("renamed variable kept its value",
                    idx >= 0 && repl_eval_predef_vars()[idx].value == 42.0f);
    }

    /* ---- rename a funcN alias plus its call site ---- */
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("blob() {");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("blob();");
    editor_navigate_to_line(0);
    set_find_and_replace("blob", "petal");
    editor_search_toggle_whole_word();
    {
        int replaced = editor_replace_all();
        ASSERT_TRUE("function rename applied", replaced == 2);
        ASSERT_TRUE("function definition renamed", buffer_has_line("petal("));
        ASSERT_TRUE("function call renamed", buffer_has_line("petal();"));
        ASSERT_TRUE("old function name gone", !buffer_has_line("blob"));
        ASSERT_TRUE("alias table follows the rename",
                    repl_func_alias_lookup_slot("petal") >= 0 &&
                    repl_func_alias_lookup_slot("blob") < 0);
    }

    /* ---- literal value edit re-parses into the command args ---- */
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glVertex3f(0.5, 0, 0);");
    editor_navigate_to_line(0);
    set_find_and_replace("0.5", "0.75");
    ASSERT_TRUE("literal replace applied", editor_replace_all() == 1);
    ASSERT_TRUE("literal replace rewrote the text",
                buffer_has_line("glVertex3f(0.75, 0, 0);"));
    ASSERT_TRUE("literal replace re-parsed the argument",
                repl_state_document_cmds()[0].args[0] > 0.74f &&
                repl_state_document_cmds()[0].args[0] < 0.76f);

    /* ---- a replace that breaks the document rolls back whole ---- */
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_navigate_to_line(0);
    {
        int before_count = repl_state_document_count();
        set_find_and_replace("glVertex3f", "glNotACommand");
        ASSERT_TRUE("invalid replace reports failure", editor_replace_all() == -1);
        ASSERT_TRUE("invalid replace kept the command count",
                    repl_state_document_count() == before_count);
        ASSERT_TRUE("invalid replace kept the original text",
                    buffer_has_line("glVertex3f(0, 0, 0);") &&
                    buffer_has_line("glVertex3f(1, 0, 0);"));
        ASSERT_TRUE("invalid replace left nothing to undo",
                    !editor_undo_can_undo());
    }

    /* ---- replace only the current match ---- */
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glVertex3f(2, 0, 0);");
    editor_feed_line("glVertex3f(2, 1, 0);");
    editor_navigate_to_line(0);
    set_find_and_replace("2", "9");
    ASSERT_TRUE("replace current reports one", editor_replace_current() == 1);
    ASSERT_TRUE("replace current rewrote the first match",
                buffer_has_line("glVertex3f(9, 0, 0);"));
    ASSERT_TRUE("replace current left the other match alone",
                buffer_has_line("glVertex3f(2, 1, 0);"));

    /* ---- auto-generated normals stay auto across a rebuild ---- */
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glColor3f(1, 0, 0);");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glVertex3f(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 1, 0);");
    editor_feed_line("glEnd();");
    repl_recompute_autonormals(1, NULL);
    {
        int auto_before = 0;
        int auto_after = 0;

        for (int i = 0; i < repl_state_document_count(); i++)
            if (repl_state_document_cmds()[i].is_auto)
                auto_before++;
        ASSERT_TRUE("fixture produced auto normals", auto_before > 0);

        editor_navigate_to_line(0);
        set_find_and_replace("glColor3f(1, 0, 0)", "glColor3f(0, 1, 0)");
        ASSERT_TRUE("color replace applied", editor_replace_all() == 1);
        for (int i = 0; i < repl_state_document_count(); i++)
            if (repl_state_document_cmds()[i].is_auto)
                auto_after++;
        ASSERT_TRUE("auto normals survive the rebuild as auto",
                    auto_after == auto_before);
        ASSERT_TRUE("replace did not duplicate rows",
                    buffer_line_count_containing("glNormal3f") == auto_before);
    }

    /* ---- guards ---- */
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_navigate_to_line(0);
    set_find_and_replace("glVertex3f", "glVertex3f");
    ASSERT_TRUE("identical find/replace is refused", editor_replace_all() == 0);
    editor_search_clear_all();
    ASSERT_TRUE("replace without a search is refused", editor_replace_all() == 0);

    /* ---- key routing: Tab ring and Enter in the replace field ---- */
    glr_ctrl_reset_all(); declare_test_vars();
    editor_feed_line("glVertex3f(0.25, 0, 0);");
    editor_navigate_to_line(0);
    open_search();
    ASSERT_TRUE("search opens focused on the find field",
                editor_state_search()->focus == EDITOR_SEARCH_FOCUS_FIND);
    ASSERT_TRUE("replace row starts closed",
                editor_state_search()->replace_open == 0);
    type_keys("0.25");
    editor_handle_key('\t', 0, 0);
    ASSERT_TRUE("tab opens the replace row",
                editor_state_search()->replace_open == 1);
    ASSERT_TRUE("tab focuses the replace field",
                editor_state_search()->focus == EDITOR_SEARCH_FOCUS_REPLACE);
    type_keys("0.75");
    ASSERT_TRUE("typing lands in the replace field",
                strcmp(editor_state_search()->replace, "0.75") == 0);
    ASSERT_TRUE("typing did not touch the query",
                strcmp(editor_state_search()->query, "0.25") == 0);
    editor_handle_key('\t', 0, 0);
    ASSERT_TRUE("tab reaches the word chip",
                editor_state_search()->focus == EDITOR_SEARCH_FOCUS_WORD);
    editor_handle_key(' ', 0, 0);
    ASSERT_TRUE("space toggles the chip",
                editor_state_search()->whole_word == 1);
    editor_handle_key(' ', 0, 0);
    ASSERT_TRUE("space toggles the chip back",
                editor_state_search()->whole_word == 0);
    editor_handle_key('\t', 0, 0);
    ASSERT_TRUE("tab wraps to the find field",
                editor_state_search()->focus == EDITOR_SEARCH_FOCUS_FIND);
    editor_handle_key('\t', 0, 0);
    editor_handle_key('\r', 0, 0);   /* Enter in the replace field */
    ASSERT_TRUE("enter in replace field runs replace all",
                buffer_has_line("glVertex3f(0.75, 0, 0);"));

    /* Esc closes the whole bar but keeps the matching preference. */
    editor_search_toggle_whole_word();
    editor_handle_key(KEY_ESC, 0, 0);
    ASSERT_TRUE("escape closes the replace row",
                editor_state_search()->replace_open == 0);
    ASSERT_TRUE("escape clears the replacement text",
                editor_state_search()->replace_len == 0);
    ASSERT_TRUE("escape resets focus",
                editor_state_search()->focus == EDITOR_SEARCH_FOCUS_FIND);
    ASSERT_TRUE("escape keeps the whole-word preference",
                editor_state_search()->whole_word == 1);

    /* ---- the find bar's mouse surface ---- */
    glr_ctrl_reset_all(); declare_test_vars();
    ui_state_reset();
    ui_state_viewport_set_size(1200, 800);
    glr_ctrl_sync_ui_chrome();
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_navigate_to_line(0);
    open_search();
    type_keys("0");
    {
        /* Replace row closed: the find row offers a "replace" affordance
         * that focuses the replace field - the discovery path for users
         * who never press Tab. */
        UiHit hit;
        ASSERT_TRUE("find row exposes a replace affordance",
                    find_search_hit_in(BAND_FIND_ROW, UI_HIT_SEARCH_FOCUS,
                                       EDITOR_SEARCH_FOCUS_REPLACE, &hit));

        editor_search_set_focus(EDITOR_SEARCH_FOCUS_REPLACE);
        ASSERT_TRUE("replace row buttons are clickable",
                    find_search_hit_in(BAND_BOTH_ROWS, UI_HIT_SEARCH_REPLACE, 1, &hit) &&
                    find_search_hit_in(BAND_BOTH_ROWS, UI_HIT_SEARCH_REPLACE, 0, &hit));
        ASSERT_TRUE("word chip is clickable",
                    find_search_hit_in(BAND_BOTH_ROWS, UI_HIT_SEARCH_WORD_TOGGLE,
                                       -1, &hit));
        ASSERT_TRUE("find box takes focus back",
                    find_search_hit_in(BAND_FIND_ROW, UI_HIT_SEARCH_FOCUS,
                                       EDITOR_SEARCH_FOCUS_FIND, &hit));
        /* Once the row is open the find row drops the now-redundant
         * affordance and reads as "focus the find field" throughout. */
        ASSERT_TRUE("no replace affordance once the row is open",
                    !find_search_hit_in(BAND_FIND_ROW, UI_HIT_SEARCH_FOCUS,
                                        EDITOR_SEARCH_FOCUS_REPLACE, &hit));
        /* The replace row itself still routes clicks into the field. */
        ASSERT_TRUE("replace row surface focuses the replace field",
                    find_search_hit_in(BAND_BOTH_ROWS, UI_HIT_SEARCH_FOCUS,
                                       EDITOR_SEARCH_FOCUS_REPLACE, &hit));
    }

    printf("repl_replace: %d/%d passed\n", g_harness.passed, g_harness.run);
    return (g_harness.run == g_harness.passed) ? 0 : 1;
}
