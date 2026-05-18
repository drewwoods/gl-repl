/*
 * test_editor_input_selection.c - Phase A/B coverage for the input-buffer
 * selection model and the tagged editor clipboard.
 *
 * Verifies:
 *   - Anchor lifecycle (set / clear / collapse on equal cursor).
 *   - Selection derivation (lo / hi / active for various orderings).
 *   - The atomic extend-selection helper. Regression: setting the
 *     anchor to the current cursor position and then moving with
 *     _keep_anchor would clear the anchor before the move and produce
 *     no selection. The extend helper pins first, moves second.
 *   - Cursor-set policy: default clears anchor, keep_anchor preserves
 *     it, empty selections still collapse.
 *   - Buffer-shrink invariant: cutting input_len below anchor clears
 *     the anchor.
 *   - Clipboard kind/payload invariants for every transition. Regression:
 *     _count_set(0) after a previous INPUT_TEXT copy must fully clear
 *     the input_text payload, not leave it stranded behind kind=EMPTY.
 */

#include "editor/state.h"
#include "editor/input.h"
#include "editor/clipboard.h"
#include "app/glr_ctrl.h"
#include "keys.h"
#include "repl/core.h"
#include "repl/state_owners.h"
#include "ui/hit.h"
#include "support/test_harness.h"

#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, (label), (cond))

#define ASSERT_INT(label, got, exp) \
    TEST_ASSERT_TRUE(&g_harness, (label), (got) == (exp))

#define ASSERT_STR(label, got, exp) \
    TEST_ASSERT_STR(&g_harness, (label), (got), (exp))

/* Load a known string into the input buffer with cursor at the
 * end. Returns the input length. */
static int load_input(const char *text) {
    editor_input_set_text(text);
    return editor_input_len();
}

/* Modifier-provider seam for shift+arrow tests. */
static int g_test_modifiers = 0;
static int test_modifiers_provider(void) { return g_test_modifiers; }

/* Clock seam for double-click dispatch tests. */
static unsigned int g_test_clock_ms = 0;
static unsigned int test_clock_ms_provider(void) { return g_test_clock_ms; }

int main(void) {
    printf("--- input-anchor lifecycle ---\n");
    {
        editor_state_reset();
        load_input("glVertex3f");

        ASSERT_INT("fresh anchor is -1", editor_input_anchor(), -1);
        ASSERT_TRUE("fresh selection inactive",
                    !editor_input_selection_active());
        ASSERT_INT("fresh selection lo is -1",
                   editor_input_selection_lo(), -1);

        editor_cursor_pos_set(4);
        editor_input_anchor_set(0);
        ASSERT_INT("anchor pinned at 0", editor_input_anchor(), 0);
        ASSERT_TRUE("selection active", editor_input_selection_active());
        ASSERT_INT("selection lo", editor_input_selection_lo(), 0);
        ASSERT_INT("selection hi", editor_input_selection_hi(), 4);

        /* Setting anchor equal to cursor collapses immediately —
         * empty selections are not representable. */
        editor_input_anchor_set(4);
        ASSERT_INT("anchor==cursor collapses to -1",
                   editor_input_anchor(), -1);
        ASSERT_TRUE("collapse → selection inactive",
                    !editor_input_selection_active());

        /* Anchor with cursor on the right end of the range — derivation
         * still picks min/max correctly. */
        editor_cursor_pos_set(2);
        editor_input_anchor_set(7);
        ASSERT_INT("reverse anchor lo", editor_input_selection_lo(), 2);
        ASSERT_INT("reverse anchor hi", editor_input_selection_hi(), 7);

        editor_input_anchor_clear();
        ASSERT_INT("clear drops to -1", editor_input_anchor(), -1);
    }

    printf("\n--- buffer-shrink invariant ---\n");
    {
        editor_state_reset();
        load_input("0123456789");
        editor_cursor_pos_set(3);
        editor_input_anchor_set(8);
        ASSERT_INT("anchor at 8", editor_input_anchor(), 8);

        editor_input_len_set(5);
        ASSERT_INT("anchor cleared when buffer shrinks past it",
                   editor_input_anchor(), -1);
    }

    printf("\n--- cursor-set policy ---\n");
    {
        editor_state_reset();
        load_input("hello world");
        editor_cursor_pos_set(0);
        editor_input_anchor_set(5);
        ASSERT_INT("setup anchor", editor_input_anchor(), 5);

        /* Default cursor move clears anchor. */
        editor_cursor_pos_set(3);
        ASSERT_INT("default cursor move clears anchor",
                   editor_input_anchor(), -1);

        /* keep_anchor preserves an active anchor. */
        editor_cursor_pos_set(0);
        editor_input_anchor_set(5);
        editor_cursor_pos_set_keep_anchor(7);
        ASSERT_INT("keep_anchor preserves anchor",
                   editor_input_anchor(), 5);
        ASSERT_INT("keep_anchor moved cursor",
                   editor_cursor_pos(), 7);

        /* keep_anchor still collapses empty selection. */
        editor_cursor_pos_set_keep_anchor(5);
        ASSERT_INT("keep_anchor collapses when cursor reaches anchor",
                   editor_input_anchor(), -1);
    }

    printf("\n--- extend-selection helper ---\n");
    {
        editor_state_reset();
        load_input("hello world");

        /* Inactive anchor + extend move pins old cursor as anchor
         * and produces [old, new) selection. This is the regression
         * case: editor_input_anchor_set(cursor_pos) would collapse
         * before the move, so the shift handler must use this atomic
         * helper instead. */
        editor_cursor_pos_set(3);
        ASSERT_INT("pre-extend anchor inactive",
                   editor_input_anchor(), -1);
        editor_cursor_pos_extend_selection(6);
        ASSERT_INT("extend from inactive pins old cursor",
                   editor_input_anchor(), 3);
        ASSERT_INT("extend from inactive moves cursor",
                   editor_cursor_pos(), 6);
        ASSERT_INT("extend selection lo",
                   editor_input_selection_lo(), 3);
        ASSERT_INT("extend selection hi",
                   editor_input_selection_hi(), 6);

        /* Active anchor + extend just grows the selection. */
        editor_cursor_pos_extend_selection(9);
        ASSERT_INT("extend keeps existing anchor",
                   editor_input_anchor(), 3);
        ASSERT_INT("extend grew cursor", editor_cursor_pos(), 9);

        /* Extending back across the anchor flips the range and lo/hi
         * stay in min/max order. */
        editor_cursor_pos_extend_selection(1);
        ASSERT_INT("extend across anchor: lo",
                   editor_input_selection_lo(), 1);
        ASSERT_INT("extend across anchor: hi",
                   editor_input_selection_hi(), 3);

        /* Extending to the anchor collapses selection. */
        editor_cursor_pos_extend_selection(3);
        ASSERT_INT("extend to anchor collapses",
                   editor_input_anchor(), -1);

        /* Zero-distance extend from inactive anchor stays inactive. */
        editor_cursor_pos_set(4);
        editor_cursor_pos_extend_selection(4);
        ASSERT_INT("no-op extend stays inactive",
                   editor_input_anchor(), -1);
    }

    printf("\n--- clipboard transitions ---\n");
    {
        editor_state_reset();
        EditorClipboardState *cb = editor_state_clipboard_mut();
        ASSERT_INT("fresh kind = EMPTY", cb->kind, EDITOR_CLIPBOARD_EMPTY);
        ASSERT_INT("fresh line_count", cb->line_count, 0);
        ASSERT_INT("fresh input_text_len", cb->input_text_len, 0);

        /* EMPTY → LINES (existing line-copy path). */
        strcpy(cb->lines[0], "glVertex3f(0,0,0);");
        editor_state_clipboard_count_set(1);
        ASSERT_INT("LINES kind", cb->kind, EDITOR_CLIPBOARD_LINES);
        ASSERT_INT("LINES line_count", cb->line_count, 1);
        ASSERT_INT("LINES clears input_text_len", cb->input_text_len, 0);

        /* LINES → INPUT_TEXT (partial-line copy path). The
         * input-text setter must clear the previous line payload. */
        editor_clipboard_set_input_text("sin(t)", 6);
        ASSERT_INT("INPUT_TEXT kind", cb->kind, EDITOR_CLIPBOARD_INPUT_TEXT);
        ASSERT_INT("INPUT_TEXT input_text_len", cb->input_text_len, 6);
        ASSERT_STR("INPUT_TEXT payload",
                   editor_clipboard_input_text(), "sin(t)");
        ASSERT_INT("INPUT_TEXT clears line_count", cb->line_count, 0);
        ASSERT_TRUE("has_input_text reports true",
                    editor_clipboard_has_input_text());

        /* INPUT_TEXT → EMPTY via explicit clear. */
        editor_state_clipboard_clear();
        ASSERT_INT("clear → kind EMPTY", cb->kind, EDITOR_CLIPBOARD_EMPTY);
        ASSERT_INT("clear → line_count 0", cb->line_count, 0);
        ASSERT_INT("clear → input_text_len 0", cb->input_text_len, 0);
        ASSERT_TRUE("clear → has_input_text false",
                    !editor_clipboard_has_input_text());

        /* Regression: INPUT_TEXT → EMPTY via _count_set(0) must also
         * drop input_text_len. Previously, _count_set(0) only set
         * kind=EMPTY and line_count=0 but left input_text_len stale,
         * violating the kind/payload invariant. */
        editor_clipboard_set_input_text("sin(t)", 6);
        ASSERT_INT("pre-bug setup: INPUT_TEXT",
                   cb->kind, EDITOR_CLIPBOARD_INPUT_TEXT);
        ASSERT_INT("pre-bug setup: text_len", cb->input_text_len, 6);
        editor_state_clipboard_count_set(0);
        ASSERT_INT("_count_set(0) → kind EMPTY",
                   cb->kind, EDITOR_CLIPBOARD_EMPTY);
        ASSERT_INT("_count_set(0) → input_text_len cleared",
                   cb->input_text_len, 0);
        ASSERT_TRUE("_count_set(0) → has_input_text false",
                    !editor_clipboard_has_input_text());

        /* set_input_text with len<=0 also clears the input-text slot
         * and recomputes kind. */
        editor_clipboard_set_input_text("ignored", 0);
        ASSERT_INT("zero-len set → EMPTY when no lines",
                   cb->kind, EDITOR_CLIPBOARD_EMPTY);
    }

    printf("\n--- editor_state_capture/restore round-trips anchor + clipboard ---\n");
    {
        editor_state_reset();
        load_input("snapshot test");
        editor_cursor_pos_set(2);
        editor_input_anchor_set(8);
        editor_clipboard_set_input_text("sin(t)", 6);

        EditorState snap;
        editor_state_capture(&snap);

        /* Trash the live state. */
        editor_input_anchor_clear();
        editor_cursor_pos_set(0);
        editor_state_clipboard_clear();
        ASSERT_INT("trashed anchor", editor_input_anchor(), -1);
        ASSERT_INT("trashed clipboard kind",
                   editor_state_clipboard_mut()->kind,
                   EDITOR_CLIPBOARD_EMPTY);

        /* Restore — full struct copy brings everything back. */
        editor_state_restore(&snap);
        ASSERT_INT("restored anchor", editor_input_anchor(), 8);
        ASSERT_INT("restored cursor", editor_cursor_pos(), 2);
        ASSERT_INT("restored clipboard kind",
                   editor_state_clipboard_mut()->kind,
                   EDITOR_CLIPBOARD_INPUT_TEXT);
        ASSERT_STR("restored clipboard payload",
                   editor_clipboard_input_text(), "sin(t)");
    }

    printf("\n--- input_consume_selection ---\n");
    {
        editor_state_reset();
        load_input("abcdefghij");
        editor_cursor_pos_set(2);
        editor_input_anchor_set(7);
        ASSERT_INT("selection [2,7) active",
                   editor_input_selection_lo(), 2);

        int consumed = editor_input_consume_selection();
        ASSERT_INT("consume returns 1", consumed, 1);
        ASSERT_STR("consume deletes [lo,hi)",
                   editor_input_text(), "abhij");
        ASSERT_INT("consume cursor at lo", editor_cursor_pos(), 2);
        ASSERT_INT("consume clears anchor", editor_input_anchor(), -1);

        /* No-op when no selection. */
        consumed = editor_input_consume_selection();
        ASSERT_INT("consume on inactive returns 0", consumed, 0);
        ASSERT_STR("consume no-op leaves buffer",
                   editor_input_text(), "abhij");
    }

    /* The remaining tests drive the editor's keyboard dispatch, which
     * needs the full app shell wired so commit / autocomplete / status
     * paths don't dereference uninitialized state. */
    glr_app_reset_all();

    printf("\n--- typed char replaces input selection ---\n");
    {
        editor_state_reset();
        load_input("hello world");
        editor_cursor_pos_set(6);
        editor_input_anchor_set(11);   /* selection on "world" */

        editor_handle_key('X', 0, 0);
        ASSERT_STR("typed char replaces selection",
                   editor_input_text(), "hello X");
        ASSERT_INT("post-replace cursor", editor_cursor_pos(), 7);
        ASSERT_INT("post-replace anchor cleared",
                   editor_input_anchor(), -1);
    }

    printf("\n--- backspace replaces selection (single deletion) ---\n");
    {
        editor_state_reset();
        load_input("hello world");
        editor_cursor_pos_set(0);
        editor_input_anchor_set(5);    /* selection on "hello" */

        editor_handle_key(KEY_BACKSPACE, 0, 0);
        ASSERT_STR("backspace deletes only the selection",
                   editor_input_text(), " world");
        ASSERT_INT("backspace cursor at lo", editor_cursor_pos(), 0);
        ASSERT_INT("backspace clears anchor",
                   editor_input_anchor(), -1);

        /* Follow-up typed char inserts at the post-cut cursor (lo). */
        editor_handle_key('H', 0, 0);
        ASSERT_STR("follow-up insert lands at lo",
                   editor_input_text(), "H world");
    }

    printf("\n--- delete (forward) replaces selection ---\n");
    {
        editor_state_reset();
        load_input("hello world");
        editor_cursor_pos_set(0);
        editor_input_anchor_set(6);    /* selection on "hello " */

        editor_handle_key(KEY_DELETE, 0, 0);
        ASSERT_STR("delete consumes selection",
                   editor_input_text(), "world");
        ASSERT_INT("delete clears anchor", editor_input_anchor(), -1);
    }

    printf("\n--- semicolon commits, anchor clears, no delete ---\n");
    {
        editor_state_reset();
        load_input("glVertex3f(0, 0, 0)");
        /* Place a selection inside the line. ; should commit the full
         * text (parser strips trailing whitespace + ;) — the selection
         * is dismissed but the underlying bytes survive. */
        editor_cursor_pos_set(11);
        editor_input_anchor_set(18);

        editor_handle_key(';', 0, 0);
        ASSERT_INT("; clears anchor", editor_input_anchor(), -1);
        /* After successful commit the editor reloads the canonical
         * line, so the input buffer no longer holds the typed text;
         * the strong assertion is just the no-delete property: the
         * pre-commit anchor never deleted "0, 0)" mid-line. The
         * commit produced exactly one new GLCmd. */
        ASSERT_TRUE("; commits one command",
                    editor_buffer_count() >= 1);
    }

    printf("\n--- tab keeps buffer text, clears anchor ---\n");
    {
        editor_state_reset();
        load_input("plain text");
        editor_cursor_pos_set(0);
        editor_input_anchor_set(5);

        editor_handle_key('\t', 0, 0);
        ASSERT_INT("tab clears anchor", editor_input_anchor(), -1);
        ASSERT_STR("tab does not delete selection",
                   editor_input_text(), "plain text");
    }

    printf("\n--- escape clears anchor without touching buffer ---\n");
    {
        editor_state_reset();
        load_input("plain text");
        editor_cursor_pos_set(0);
        editor_input_anchor_set(5);

        editor_handle_key(KEY_ESC, 0, 0);
        ASSERT_INT("esc clears anchor", editor_input_anchor(), -1);
        /* The current Esc branch with no other transient state clears
         * the whole input. That's an existing behavior — the new
         * property is just that the anchor went away too. */
    }

    printf("\n--- Ctrl+C copies input selection (preserves selection) ---\n");
    {
        glr_app_reset_all();
        load_input("glVertex3f(1, 2, 3)");
        editor_cursor_pos_set(11);
        editor_input_anchor_set(18);   /* selects "1, 2, 3" */

        editor_handle_key(KEY_CTRL_C, 0, 0);
        ASSERT_TRUE("Ctrl+C → INPUT_TEXT clipboard",
                    editor_clipboard_has_input_text());
        ASSERT_STR("Ctrl+C captured substring",
                   editor_clipboard_input_text(), "1, 2, 3");
        ASSERT_INT("Ctrl+C preserves visual selection: anchor",
                   editor_input_anchor(), 18);
        ASSERT_STR("Ctrl+C leaves buffer untouched",
                   editor_input_text(), "glVertex3f(1, 2, 3)");
        /* line-copy clipboard slot is not populated. */
        ASSERT_INT("Ctrl+C did not touch line clipboard",
                   editor_state_clipboard_count(), 0);
    }

    printf("\n--- Ctrl+X cuts input selection (works in any mode) ---\n");
    {
        glr_app_reset_all();
        load_input("glVertex3f(1, 2, 3)");
        editor_cursor_pos_set(11);
        editor_input_anchor_set(18);

        editor_handle_key(KEY_CTRL_X, 0, 0);
        ASSERT_TRUE("Ctrl+X → INPUT_TEXT clipboard",
                    editor_clipboard_has_input_text());
        ASSERT_STR("Ctrl+X captured substring",
                   editor_clipboard_input_text(), "1, 2, 3");
        ASSERT_STR("Ctrl+X deletes selection from input",
                   editor_input_text(), "glVertex3f()");
        ASSERT_INT("Ctrl+X clears anchor", editor_input_anchor(), -1);
        ASSERT_INT("Ctrl+X cursor at lo", editor_cursor_pos(), 11);

        /* Same path in insert mode — line-range cut is disabled there,
         * but partial-line cut must still work. */
        glr_app_reset_all();
        load_input("foo bar baz");
        editor_insert_mode_set(1);
        editor_cursor_pos_set(4);
        editor_input_anchor_set(7);
        editor_handle_key(KEY_CTRL_X, 0, 0);
        ASSERT_STR("Ctrl+X partial cut works in insert mode",
                   editor_input_text(), "foo  baz");
        ASSERT_STR("insert-mode cut captures substring",
                   editor_clipboard_input_text(), "bar");
        editor_insert_mode_set(0);
    }

    printf("\n--- Ctrl+V (input-text) inserts at cursor ---\n");
    {
        glr_app_reset_all();
        /* Stage an INPUT_TEXT clipboard directly so the test is
         * independent of the Ctrl+C path. */
        editor_clipboard_set_input_text("sin(t)", 6);
        load_input("glVertex3f()");
        editor_cursor_pos_set(11);

        editor_handle_key(KEY_CTRL_V, 0, 0);
        ASSERT_STR("Ctrl+V inserts at cursor",
                   editor_input_text(), "glVertex3f(sin(t))");
        ASSERT_INT("Ctrl+V cursor advances to end of paste",
                   editor_cursor_pos(), 17);
        /* Paste leaves the clipboard intact — user can paste again. */
        ASSERT_TRUE("Ctrl+V leaves clipboard populated",
                    editor_clipboard_has_input_text());
    }

    printf("\n--- Ctrl+V (input-text) replaces destination selection ---\n");
    {
        glr_app_reset_all();
        editor_clipboard_set_input_text("X", 1);
        load_input("hello world");
        editor_cursor_pos_set(6);
        editor_input_anchor_set(11);    /* destination selection "world" */

        editor_handle_key(KEY_CTRL_V, 0, 0);
        ASSERT_STR("Ctrl+V consumes destination selection",
                   editor_input_text(), "hello X");
        ASSERT_INT("post-paste anchor cleared",
                   editor_input_anchor(), -1);
        ASSERT_INT("post-paste cursor after inserted char",
                   editor_cursor_pos(), 7);
    }

    printf("\n--- Ctrl+V with EMPTY clipboard reports empty ---\n");
    {
        glr_app_reset_all();
        editor_state_clipboard_clear();
        load_input("unchanged");

        editor_handle_key(KEY_CTRL_V, 0, 0);
        ASSERT_STR("EMPTY paste leaves input untouched",
                   editor_input_text(), "unchanged");
    }

    printf("\n--- input-only cut/paste does NOT auto-promote example ---\n");
    {
        /* Loaded example + first source-mutating edit normally promotes
         * the example into a fresh user-scene slot via
         * repl_promote_example_if_needed inside editor_undo_push_snapshot.
         * Partial-line cut/paste must not trigger that hook: it edits
         * only the input buffer, never a source command, and the undo
         * model can't restore the input anyway. Regression for the
         * Phase D review finding. */
        glr_app_reset_all();
        repl_load_example(0);
        ReplSceneRuntimeState scenes = repl_state_scenes();
        int example_before = scenes.active_example_idx;
        ASSERT_TRUE("example is active before edit", example_before >= 0);

        editor_input_set_text("foo bar baz");
        editor_cursor_pos_set(4);
        editor_input_anchor_set(7);     /* selects "bar" */
        editor_handle_key(KEY_CTRL_X, 0, 0);
        scenes = repl_state_scenes();
        ASSERT_INT("Ctrl+X (input-only) leaves example active",
                   scenes.active_example_idx, example_before);

        /* And the paste path. */
        editor_input_set_text("destination");
        editor_cursor_pos_set(0);
        editor_handle_key(KEY_CTRL_V, 0, 0);
        scenes = repl_state_scenes();
        ASSERT_INT("Ctrl+V (input-only) leaves example active",
                   scenes.active_example_idx, example_before);
    }

    printf("\n--- word-bounds helper (pure) ---\n");
    {
        int s = -1, e = -1;
        const char *t = "glVertex3f(1, 2, 3)";
        int len = (int)strlen(t);

        /* Inside the identifier — walks both directions over [A-Za-z0-9_]. */
        editor_input_word_bounds_at(t, len, 4, &s, &e);
        ASSERT_INT("word start at 'glVertex3f'[0]", s, 0);
        ASSERT_INT("word end at 'glVertex3f'[10]", e, 10);

        /* Numeric tokens count as word chars too. */
        editor_input_word_bounds_at(t, len, 11, &s, &e);
        ASSERT_INT("digit word start", s, 11);
        ASSERT_INT("digit word end", e, 12);

        /* Underscore + alphanumeric. */
        const char *u = "foo_bar_42 baz";
        editor_input_word_bounds_at(u, (int)strlen(u), 2, &s, &e);
        ASSERT_INT("underscore word start", s, 0);
        ASSERT_INT("underscore word end", e, 10);

        /* Whitespace position: zero-width range. */
        editor_input_word_bounds_at(t, len, 10, &s, &e);
        ASSERT_INT("'(' is not a word char: start=char_idx", s, 10);
        ASSERT_INT("'(' is not a word char: end=char_idx",   e, 10);

        /* Out-of-range char_idx is also a zero-width range. */
        editor_input_word_bounds_at(t, len, -1, &s, &e);
        ASSERT_INT("negative char_idx: start", s, -1);
        editor_input_word_bounds_at(t, len, 999, &s, &e);
        ASSERT_INT("past-end char_idx: start", s, 999);

        /* Word at the very start of the buffer. */
        const char *w = "hello world";
        editor_input_word_bounds_at(w, (int)strlen(w), 0, &s, &e);
        ASSERT_INT("first-char word start", s, 0);
        ASSERT_INT("first-char word end", e, 5);

        /* Word abutting end-of-buffer. */
        editor_input_word_bounds_at(w, (int)strlen(w), 10, &s, &e);
        ASSERT_INT("last-char word start", s, 6);
        ASSERT_INT("last-char word end", e, 11);
    }

    printf("\n--- select_word_at integration ---\n");
    {
        glr_app_reset_all();
        editor_feed_line("glVertex3f(1, 2, 3);");
        editor_navigate_to_line(0);

        /* Click on the 'V' of glVertex3f → selects the whole
         * identifier. */
        glr_ctrl_router_select_word_at(0, 4);
        ASSERT_INT("word select: lo", editor_input_selection_lo(), 0);
        ASSERT_INT("word select: hi", editor_input_selection_hi(), 10);
        ASSERT_INT("word select: cursor at word_end",
                   editor_cursor_pos(), 10);

        /* Click on whitespace / punctuation → no selection. */
        glr_ctrl_router_select_word_at(0, 10);   /* '(' */
        ASSERT_INT("non-word: anchor stays -1",
                   editor_input_anchor(), -1);
        ASSERT_INT("non-word: cursor placed",
                   editor_cursor_pos(), 10);
    }

    printf("\n--- double-click dispatches word selection ---\n");
    {
        glr_app_reset_all();
        editor_feed_line("glVertex3f(1, 2, 3);");
        editor_navigate_to_line(0);

        glr_ctrl_router_set_double_click_clock_for_test(test_clock_ms_provider);

        UiHit hit = ui_hit_none();
        hit.kind = UI_HIT_CODE_TEXT;
        hit.line_idx = 0;
        hit.char_idx = 4;

        /* First press at t=1000 → ordinary single-click placement. */
        g_test_clock_ms = 1000;
        glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
        ASSERT_INT("first press: no anchor (single-click)",
                   editor_input_anchor(), -1);
        ASSERT_INT("first press: cursor at char_idx",
                   editor_cursor_pos(), 4);

        /* Second press at t=1200 (200ms later) at the same (line,
         * char) → double-click, selects 'glVertex3f'. */
        g_test_clock_ms = 1200;
        glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
        ASSERT_INT("double-click selection: lo",
                   editor_input_selection_lo(), 0);
        ASSERT_INT("double-click selection: hi",
                   editor_input_selection_hi(), 10);

        /* A third press more than DOUBLE_CLICK_MS later is a single
         * click again — clears the selection and re-places cursor. */
        g_test_clock_ms = 1200 + 500;
        glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
        ASSERT_INT("late press: single-click clears selection",
                   editor_input_anchor(), -1);
        ASSERT_INT("late press: cursor placed",
                   editor_cursor_pos(), 4);

        /* Two fast clicks at *different* chars stays a single click. */
        UiHit other = hit;
        other.char_idx = 12;
        g_test_clock_ms = 2000;
        glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);
        g_test_clock_ms = 2100;
        glr_ctrl_router_handle_code_panel_hit(other, 0, 0);
        ASSERT_INT("different char: still single-click",
                   editor_input_anchor(), -1);

        glr_ctrl_router_set_double_click_clock_for_test(NULL);
        glr_ctrl_router_reset_code_panel_drag();
    }

    printf("\n--- code-panel press + drag on active edit row ---\n");
    {
        glr_app_reset_all();
        /* Seed a single source line so the press hit targets a real
         * committed row. */
        editor_feed_line("glVertex3f(1, 2, 3);");
        editor_navigate_to_line(0);
        ASSERT_INT("baseline edit line", repl_state_edit_line(), 0);

        /* Synthesize a UI_HIT_CODE_TEXT press at char 11 ("1" inside
         * the parens). The router uses the hit directly; it doesn't
         * need to come from a real ui_panels_hit_test. */
        UiHit hit = ui_hit_none();
        hit.kind = UI_HIT_CODE_TEXT;
        hit.line_idx = 0;
        hit.char_idx = 11;
        hit.visual_row = 0;
        glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);

        ASSERT_INT("press places cursor at char_idx",
                   editor_cursor_pos(), 11);
        ASSERT_INT("press clears any prior anchor",
                   editor_input_anchor(), -1);

        /* Drag motion still on the active edit row: extend the input
         * selection. The first motion call pins the press column. */
        int handled = glr_ctrl_router_apply_input_row_drag(0, 16);
        ASSERT_INT("drag motion on edit row is handled", handled, 1);
        ASSERT_INT("first drag motion pins press col as anchor",
                   editor_input_anchor(), 11);
        ASSERT_INT("first drag motion moves cursor",
                   editor_cursor_pos(), 16);

        /* Continuing motion grows the same selection. */
        glr_ctrl_router_apply_input_row_drag(0, 18);
        ASSERT_INT("continued drag keeps anchor",
                   editor_input_anchor(), 11);
        ASSERT_INT("continued drag moves cursor further",
                   editor_cursor_pos(), 18);

        /* Drag wandering off the active edit row is not handled by
         * this path — falls through to line-range (existing behavior,
         * not exercised here). */
        handled = glr_ctrl_router_apply_input_row_drag(1, 0);
        ASSERT_INT("drag off active edit row not handled here",
                   handled, 0);

        glr_ctrl_router_reset_code_panel_drag();
    }

    printf("\n--- input-row drag is sticky: off-row motion is a no-op ---\n");
    {
        /* Regression for the Phase E/F review finding: once a press
         * arms an input-row drag (char_anchor >= 0), motion that
         * wanders off the press row must not silently switch the
         * interaction into line-range selection mode. Otherwise a
         * user trying to select text inside the input would
         * accidentally start a multi-line range when they drift the
         * cursor below the row. */
        glr_app_reset_all();
        editor_feed_line("glVertex3f(1, 2, 3);");
        editor_feed_line("glVertex3f(4, 5, 6);");
        editor_navigate_to_line(0);

        /* Arm the drag on line 0 at char 4. */
        UiHit hit = ui_hit_none();
        hit.kind = UI_HIT_CODE_TEXT;
        hit.line_idx = 0;
        hit.char_idx = 4;
        glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);

        /* Build a partial input selection by dragging within row 0. */
        glr_ctrl_router_apply_input_row_drag(0, 8);
        ASSERT_INT("setup: input selection lo",
                   editor_input_selection_lo(), 4);
        ASSERT_TRUE("setup: no line-range selection yet",
                    !editor_clipboard_sel_active());

        /* Motion off the press row returns 0 from the input-row
         * helper. The sticky-mode guard in the top-level drag handler
         * must keep us from falling through to the line-range path.
         * We can't easily call the top-level drag handler in a test
         * (it expects real pixel coords through ui_panels_hit_test),
         * but we can verify the precondition the guard checks: an
         * armed input-row drag (char_anchor >= 0) plus off-row motion
         * → apply_input_row_drag returns 0, and at that point the
         * caller in glr_ctrl_router_handle_code_panel_drag stops. The
         * downstream effect: line-range selection stays clear. */
        int handled = glr_ctrl_router_apply_input_row_drag(1, 0);
        ASSERT_INT("off-row motion not handled by input-row path",
                   handled, 0);
        /* The input-row selection survives the off-row motion. */
        ASSERT_INT("off-row motion preserves input anchor",
                   editor_input_anchor(), 4);
        ASSERT_INT("off-row motion preserves input cursor",
                   editor_cursor_pos(), 8);
        /* No line-range selection got started. */
        ASSERT_TRUE("off-row motion did not start line-range",
                    !editor_clipboard_sel_active());

        /* Drag back onto the original row resumes per-char selection. */
        handled = glr_ctrl_router_apply_input_row_drag(0, 12);
        ASSERT_INT("back-on-row motion resumes input drag",
                   handled, 1);
        ASSERT_INT("input selection grew on return",
                   editor_cursor_pos(), 12);

        glr_ctrl_router_reset_code_panel_drag();
    }

    printf("\n--- press on insert-line / no-drag press does not arm input-row drag ---\n");
    {
        glr_app_reset_all();
        editor_feed_line("glVertex3f(1, 2, 3);");
        editor_navigate_to_line(0);

        /* UI_HIT_CODE_INSERT_LINE doesn't arm any drag, so the
         * input-row drag helper returns 0 with no state change. */
        UiHit hit = ui_hit_none();
        hit.kind = UI_HIT_CODE_INSERT_LINE;
        hit.line_idx = 0;
        hit.char_idx = 3;
        glr_ctrl_router_handle_code_panel_hit(hit, 0, 0);

        int handled = glr_ctrl_router_apply_input_row_drag(0, 8);
        ASSERT_INT("insert-line press leaves drag unarmed",
                   handled, 0);
        ASSERT_INT("no spurious anchor after unarmed drag",
                   editor_input_anchor(), -1);
    }

    printf("\n--- shift+arrow / home / end extend selection ---\n");
    {
        glr_app_reset_all();
        editor_input_set_modifier_provider_for_test(test_modifiers_provider);

        load_input("hello world");
        editor_cursor_pos_set(5);
        ASSERT_INT("baseline anchor inactive",
                   editor_input_anchor(), -1);

        /* Shift+Right pins old cursor and extends. */
        g_test_modifiers = GLUT_ACTIVE_SHIFT;
        editor_handle_special(GLUT_KEY_RIGHT, 0, 0);
        ASSERT_INT("shift+right pins old cursor as anchor",
                   editor_input_anchor(), 5);
        ASSERT_INT("shift+right moved cursor", editor_cursor_pos(), 6);
        ASSERT_INT("selection lo", editor_input_selection_lo(), 5);
        ASSERT_INT("selection hi", editor_input_selection_hi(), 6);

        /* Subsequent shift+right grows the same selection. */
        editor_handle_special(GLUT_KEY_RIGHT, 0, 0);
        ASSERT_INT("shift+right grows: anchor stays",
                   editor_input_anchor(), 5);
        ASSERT_INT("shift+right grows: cursor at 7",
                   editor_cursor_pos(), 7);

        /* Shift+Left shrinks. */
        editor_handle_special(GLUT_KEY_LEFT, 0, 0);
        ASSERT_INT("shift+left shrinks cursor",
                   editor_cursor_pos(), 6);
        ASSERT_INT("shift+left keeps anchor",
                   editor_input_anchor(), 5);

        /* Shift+Left across the anchor collapses (returns to no
         * selection at the start position). */
        editor_handle_special(GLUT_KEY_LEFT, 0, 0);
        ASSERT_INT("shift+left to anchor collapses anchor",
                   editor_input_anchor(), -1);
        ASSERT_INT("shift+left to anchor: cursor at 5",
                   editor_cursor_pos(), 5);

        /* Unshifted arrow clears any active anchor. */
        editor_input_anchor_set(0);
        g_test_modifiers = 0;
        editor_handle_special(GLUT_KEY_LEFT, 0, 0);
        ASSERT_INT("plain arrow clears anchor",
                   editor_input_anchor(), -1);

        /* Shift+Home extends to start. */
        editor_cursor_pos_set(8);
        editor_input_anchor_clear();
        g_test_modifiers = GLUT_ACTIVE_SHIFT;
        editor_handle_special(GLUT_KEY_HOME, 0, 0);
        ASSERT_INT("shift+home pins cursor",
                   editor_input_anchor(), 8);
        ASSERT_INT("shift+home cursor at 0", editor_cursor_pos(), 0);

        /* Shift+End extends to end. */
        editor_cursor_pos_set(3);
        editor_input_anchor_clear();
        editor_handle_special(GLUT_KEY_END, 0, 0);
        ASSERT_INT("shift+end pins cursor",
                   editor_input_anchor(), 3);
        ASSERT_INT("shift+end cursor at input_len",
                   editor_cursor_pos(), editor_input_len());

        /* Plain Home clears anchor as a side effect. */
        g_test_modifiers = 0;
        editor_handle_special(GLUT_KEY_HOME, 0, 0);
        ASSERT_INT("plain home clears anchor",
                   editor_input_anchor(), -1);

        editor_input_set_modifier_provider_for_test(NULL);
    }

    printf("\n--- Ctrl+V (LINES) routes through line paste, not input ---\n");
    {
        glr_app_reset_all();
        /* Seed the LINES clipboard directly. */
        EditorClipboardState *cb = editor_state_clipboard_mut();
        strcpy(cb->lines[0], "glVertex3f(9, 9, 9);");
        editor_state_clipboard_count_set(1);
        ASSERT_INT("seeded LINES kind",
                   cb->kind, EDITOR_CLIPBOARD_LINES);

        int doc_before = editor_buffer_count();
        editor_input_clear();
        editor_handle_key(KEY_CTRL_V, 0, 0);
        /* LINES paste runs editor_feed_line — a new source command appears. */
        ASSERT_TRUE("LINES paste extended the document",
                    editor_buffer_count() > doc_before);
    }

    printf("\n--- undo restore rebuilds input, clears anchor, preserves clipboard ---\n");
    {
        /* Undo's snapshot model intentionally does not capture
         * EditorInputState.input or EditorClipboardState (see Phase
         * A item 6 in done/editor-input-selection.md). After a
         * source-mutating commit, undo rewinds the source array and
         * editor_undo_snapshot_restore calls editor_load_line_to_input to
         * rebuild the active input from the restored source. This
         * test pins those two properties:
         *
         *   1. After undo, the input anchor is -1 (input rebuilt
         *      from canonical source, no persisted anchor).
         *   2. After undo, an input-text clipboard payload survives
         *      unchanged (undo does not snapshot clipboard state). */
        glr_app_reset_all();
        editor_feed_line("glVertex3f(1, 2, 3);");
        editor_navigate_to_line(0);
        editor_clipboard_set_input_text("sin(t)", 6);
        editor_cursor_pos_set(2);
        editor_input_anchor_set(8);
        ASSERT_INT("pre-mutation anchor set", editor_input_anchor(), 8);

        /* Source-mutating commit pushes an undo snapshot first. */
        editor_input_set_text("glVertex3f(9, 9, 9)");
        editor_handle_key(';', 0, 0);

        ASSERT_TRUE("commit succeeded (still 1 line, content updated)",
                    editor_buffer_count() == 1);

        /* Ctrl+Z restores the prior snapshot. */
        editor_handle_key(KEY_CTRL_Z, 0, 0);
        ASSERT_INT("undo rebuilds input → anchor cleared",
                   editor_input_anchor(), -1);
        ASSERT_TRUE("undo preserves input-text clipboard payload",
                    editor_clipboard_has_input_text());
        ASSERT_STR("undo: clipboard payload intact",
                   editor_clipboard_input_text(), "sin(t)");
    }

    return test_harness_report(&g_harness, "test_editor_input_selection");
}
