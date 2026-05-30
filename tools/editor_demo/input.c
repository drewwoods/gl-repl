/*
 * tools/editor_demo/input.c - Generic editor demo input dispatcher.
 *
 * Translates GLUT key events into edit_ops primitive calls plus
 * cross-line navigation. No REPL coupling: commit semantics,
 * autocomplete, tutorials, comment toggle, reformat, etc. are all
 * REPL-feature concerns and stay out of this dispatcher.
 *
 * v1.1 surface (Phase 8b):
 *   - Printable ASCII -> edit_op_type_char.
 *   - Backspace / Delete -> edit_op_backspace; at cursor
 *     column 0 with no selection, merges the input row into the
 *     end of the previous buffer line (inverse of Enter's split).
 *   - Cursor Left / Right / Home / End within the active line ->
 *     EditorState primitives. Shift extends a character-range
 *     selection in the input row via editor_cursor_pos_extend_selection.
 *   - Enter (\r or \n) -> split current input at cursor; commit
 *     left half to the buffer at edit_line, push right half into
 *     a new line at edit_line+1, advance edit_line, load the new
 *     line's text into the input row.
 *   - Up / Down -> commit current input, change edit_line, load
 *     target line into the input row. Clamped to [0, line_count]
 *     so callers can park on the "virtual" line one past the last
 *     committed buffer entry (where the next Enter creates a fresh
 *     row).
 *   - Ctrl+A / Ctrl+C / Ctrl+X / Ctrl+V (and Cmd+letter on
 *     macOS) -> select-all input, copy / cut / paste the input
 *     row's character-range selection through EditorClipboardState
 *     (input-text payload). Demo-local only; no system clipboard
 *     integration.
 *   - Ctrl+F / Cmd+F -> open find bar; typing edits the query,
 *     Esc cancels, Enter accepts (navigates cursor to current
 *     hit). Ctrl+G / Cmd+G / F3 = find next; Cmd+Shift+G /
 *     Shift+F3 = find prev. Uses ui_text_search primitives;
 *     match highlights are drawn by text_panel via snap->search.
 *   - Escape -> exit (unless search mode is active, in which case
 *     Esc cancels the find bar — the search dispatcher intercepts
 *     before the exit path).
 *
 * Out of scope (deferred): word jumps, scroll wheel, undo/redo,
 * multi-line / line-range copy, system clipboard
 * (NSPasteboard / X11 selections), find-and-replace.
 */

#include "input.h"

#include "editor/edit_ops.h"
#include "editor/state.h"
#include "ui/core/text_search.h"

#include "gl_includes.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Edit-line cursor lives on EditorState.document.edit_line_idx
 * (Phase 4 of plans/done/edit-line-ownership.md). The demo reads /
 * writes it through editor_state_edit_line / _set — the same API
 * the REPL editor uses; the shim that previously backed this is
 * gone (Phase 5). */

static int key_is_printable_ascii(unsigned char key) {
    return key >= 32 && key < 127;
}

/* ---- Cross-line navigation helpers ----------------------------- */

/* Commit the input buffer's current text into the document buffer
 * at the active edit line. editor_buffer_replace_line auto-extends
 * line_count if edit_line was past the end, so a freshly-navigated
 * "virtual" line (edit_line == line_count) becomes a real entry on
 * commit. Non-static — exposed as demo_input_commit_to_buffer so
 * the Save menu action can flush in-progress input before writing
 * the buffer to disk. */
void demo_input_commit_to_buffer(void) {
    int line = editor_state_edit_line();
    if (line < 0) line = 0;
    (void)editor_buffer_replace_line(line, editor_input_text());
}

/* Replace the active input row's text with the buffer entry at
 * `line`, or clear it if `line` is past the last committed entry. */
static void demo_load_buffer_line(int line) {
    if (line < 0 || line >= editor_buffer_count()) {
        editor_input_clear();
        return;
    }
    EditorBufferView buf = editor_buffer_view();
    const char *text = editor_buffer_view_line(buf, line);
    editor_input_set_text(text ? text : "");
}

/* Navigate to a target edit-line. Commits the current input first,
 * loads the target's text, parks the cursor at the end of the
 * loaded text. Target is clamped to [0, line_count] so the
 * "virtual" line one past the last committed entry is reachable
 * (Down at the last line lands there). Non-static: the click
 * handler in editor_demo.c drives this through the public
 * declaration in input.h. */
void demo_input_navigate_to(int target) {
    int count = editor_buffer_count();
    if (target < 0)     target = 0;
    if (target > count) target = count;

    demo_input_commit_to_buffer();
    editor_state_edit_line_set(target);
    demo_load_buffer_line(target);
    editor_cursor_pos_set(editor_input_len());
}

/* Enter: split the input row at the cursor. Left half replaces the
 * current line; right half becomes a new line at edit_line+1; the
 * cursor lands at the start of the new line. */
static void demo_handle_enter(void) {
    int line = editor_state_edit_line();
    if (line < 0) line = 0;

    const char *text = editor_input_text();
    int len     = editor_input_len();
    int cur     = editor_cursor_pos();
    if (cur < 0) cur = 0;
    if (cur > len) cur = len;

    /* Capture both halves before mutating EditorState — the input
     * buffer is shared storage, so the right half has to be copied
     * out before we overwrite the row with the left half. */
    char left[MAX_LINE_LEN];
    char right[MAX_LINE_LEN];
    int left_len  = cur;
    int right_len = len - cur;
    if (left_len  >= (int)sizeof(left))  left_len  = (int)sizeof(left)  - 1;
    if (right_len >= (int)sizeof(right)) right_len = (int)sizeof(right) - 1;
    memcpy(left,  text,       (size_t)left_len);
    memcpy(right, text + cur, (size_t)right_len);
    left[left_len]   = '\0';
    right[right_len] = '\0';

    (void)editor_buffer_replace_line(line, left);
    (void)editor_buffer_insert_line(line + 1, right);

    editor_state_edit_line_set(line + 1);
    editor_input_set_text(right);
    editor_cursor_pos_set(0);
}

/* Backspace pressed at column 0: merge the input row's content
 * into the end of the previous buffer line and remove the current
 * row. Inverse of demo_handle_enter (which splits a line at the
 * cursor). No-op at the top of the buffer (no previous line to
 * merge into). Caller has already verified cursor == 0 and that
 * no input-row selection is active — otherwise the regular
 * edit_op_backspace handles it. */
static void demo_handle_backspace_at_line_start(void) {
    int line = editor_state_edit_line();
    if (line <= 0) return;

    int prev = line - 1;
    int count = editor_buffer_count();
    EditorBufferView buf = editor_buffer_view();

    /* Capture current input text first — the input buffer is
     * shared storage with the input row, and the merge below
     * overwrites that storage when it sets the merged text back. */
    char curr_text[MAX_LINE_LEN];
    int curr_len = editor_input_len();
    if (curr_len < 0) curr_len = 0;
    if (curr_len >= (int)sizeof(curr_text)) curr_len = (int)sizeof(curr_text) - 1;
    memcpy(curr_text, editor_input_text(), (size_t)curr_len);
    curr_text[curr_len] = '\0';

    /* Read previous line's text from the buffer view; null-safe. */
    const char *prev_text = editor_buffer_view_line(buf, prev);
    int prev_len = prev_text ? (int)strlen(prev_text) : 0;

    /* Build merged = prev + curr, clamped to MAX_LINE_LEN - 1 so
     * the null terminator fits. The join point (where the cursor
     * will land) is prev_len, clamped to whatever survived the
     * clamp on the merged buffer. */
    char merged[MAX_LINE_LEN];
    int prev_copy = prev_len;
    if (prev_copy >= (int)sizeof(merged)) prev_copy = (int)sizeof(merged) - 1;
    memcpy(merged, prev_text, (size_t)prev_copy);
    int curr_capacity = (int)sizeof(merged) - 1 - prev_copy;
    int curr_copy = curr_len < curr_capacity ? curr_len : curr_capacity;
    memcpy(merged + prev_copy, curr_text, (size_t)curr_copy);
    int merged_len = prev_copy + curr_copy;
    merged[merged_len] = '\0';

    /* Write the merged line back. Only delete the current row from
     * the buffer if it's a real committed row (line < count); a
     * virtual past-the-end row has no buffer entry to remove. */
    (void)editor_buffer_replace_line(prev, merged);
    if (line < count)
        (void)editor_buffer_delete_range(line, 1);

    editor_state_edit_line_set(prev);
    editor_input_set_text(merged);
    editor_cursor_pos_set(prev_copy);
}

/* ---- Find / search --------------------------------------------- */

/* Demo-local search state. Lives in input.c rather than on
 * EditorState so the demo can prove the generic ui_text_search
 * primitives + text_panel's highlight rendering work without
 * linking the REPL editor's src/editor/search.c controller. The
 * editor's own EditorSearchState slot on EditorState stays empty
 * in the demo. DEMO_SEARCH_QUERY_MAX is in input.h so the
 * editor_demo render-bar code can size its formatted-buffer the
 * same way. */

static struct {
    int  active;
    char query[DEMO_SEARCH_QUERY_MAX];
    int  query_len;
    int  hit_line;  /* -1 = no match */
    int  hit_char;
} g_demo_search;

int demo_search_active(void) {
    return g_demo_search.active;
}
const char *demo_search_query(void) {
    return g_demo_search.query;
}
int demo_search_query_len(void) {
    return g_demo_search.query_len;
}
int demo_search_hit_line(void) {
    return g_demo_search.hit_line;
}
int demo_search_hit_char(void) {
    return g_demo_search.hit_char;
}

/* Scan committed buffer lines for the first occurrence of the
 * current query at or after (start_line, start_char), wrapping
 * past end-of-buffer back to (0, 0). Stores the result in
 * g_demo_search.hit_* (or clears them if no match exists).
 * `direction` is +1 for next, -1 for previous; the prev path
 * uses ui_text_find_prev_in_text on each line in reverse order
 * starting at start_char. */
static void demo_search_step(int start_line, int start_char, int direction) {
    g_demo_search.hit_line = -1;
    g_demo_search.hit_char = -1;
    if (g_demo_search.query_len <= 0) return;

    int n = editor_buffer_count();
    if (n <= 0) return;
    EditorBufferView buf = editor_buffer_view();

    /* Normalize start_line into [0, n) so the modulo arithmetic
     * for wrap-around stays positive. */
    if (start_line < 0)  start_line = 0;
    if (start_line >= n) start_line = n - 1;

    for (int step = 0; step <= n; step++) {
        int li = direction > 0
            ? (start_line + step) % n
            : ((start_line - step) % n + n) % n;
        const char *line = editor_buffer_view_line(buf, li);
        int from = (step == 0) ? start_char : (direction > 0 ? 0 : -1);
        int pos = direction > 0
            ? ui_text_find_next_in_text(line, g_demo_search.query, from)
            : ui_text_find_prev_in_text(line, g_demo_search.query,
                                        from < 0 ? (int)strlen(line)
                                                 : from);
        if (pos >= 0) {
            g_demo_search.hit_line = li;
            g_demo_search.hit_char = pos;
            return;
        }
    }
}

/* On every query mutation, rescan from the top of the buffer so
 * the highlight tracks the live query. */
static void demo_search_rescan(void) {
    demo_search_step(0, 0, 1);
}

static void demo_search_begin(void) {
    /* Flush any in-progress input row to the buffer first. Without
     * this, content the user typed on the active edit_line is
     * invisible to the find scan (which walks committed buffer
     * lines only). Same rationale as the Save action — what the
     * user sees on screen is what they expect Find to search. */
    demo_input_commit_to_buffer();

    g_demo_search.active = 1;
    g_demo_search.query[0] = '\0';
    g_demo_search.query_len = 0;
    g_demo_search.hit_line = -1;
    g_demo_search.hit_char = -1;
}

static void demo_search_cancel(void) {
    g_demo_search.active = 0;
    g_demo_search.query[0] = '\0';
    g_demo_search.query_len = 0;
    g_demo_search.hit_line = -1;
    g_demo_search.hit_char = -1;
}

/* Accept the current hit: navigate the edit-line cursor to it and
 * exit search mode. No-op on no-match (the bar just closes). */
static void demo_search_accept(void) {
    if (g_demo_search.hit_line >= 0 && g_demo_search.hit_char >= 0) {
        demo_input_navigate_to(g_demo_search.hit_line);
        editor_cursor_pos_set(g_demo_search.hit_char);
    }
    g_demo_search.active = 0;
    /* Keep the query in g_demo_search.query so a subsequent F3 can
     * resume cycling from the new cursor without retyping. */
}

/* F3 / Cmd+G: find next from one char past the current hit (or
 * from the cursor position if no current hit). Re-arms search
 * mode if it had been closed via Enter on a prior match so the
 * highlight comes back. */
static void demo_search_next(int direction) {
    if (g_demo_search.query_len <= 0) return;
    int line = g_demo_search.hit_line;
    int chr  = g_demo_search.hit_char;
    if (line < 0) {
        line = editor_state_edit_line();
        chr  = editor_cursor_pos();
    } else {
        chr += (direction > 0) ? 1 : -1;
    }
    g_demo_search.active = 1;
    demo_search_step(line, chr, direction);
}

/* Search-mode keystroke handler. Returns 1 if the key was
 * consumed, 0 if the regular dispatch should continue (e.g. for
 * keys that aren't meaningful in search mode and should fall
 * through). */
static int demo_search_handle_key(unsigned char key) {
    if (!g_demo_search.active) return 0;
    if (key == 27) {           /* Esc */
        demo_search_cancel();
        return 1;
    }
    if (key == '\r' || key == '\n') {
        demo_search_accept();
        return 1;
    }
    if (key == 8 || key == 127) {  /* Backspace / Delete */
        if (g_demo_search.query_len > 0) {
            g_demo_search.query[--g_demo_search.query_len] = '\0';
            demo_search_rescan();
        }
        return 1;
    }
    if (key_is_printable_ascii(key) &&
        g_demo_search.query_len < DEMO_SEARCH_QUERY_MAX - 1) {
        g_demo_search.query[g_demo_search.query_len++] = (char)key;
        g_demo_search.query[g_demo_search.query_len]   = '\0';
        demo_search_rescan();
        return 1;
    }
    /* Other keys (modifier-only events, control bytes besides those
     * above) are swallowed while search is up — typing in the
     * editor body while a search bar is open would be confusing. */
    return 1;
}

/* ---- Clipboard ------------------------------------------------- */

/* Select-all: anchor at start, extend cursor to end. Sets up the
 * input row so Ctrl+C immediately copies the whole row. */
static void demo_handle_select_all(void) {
    int len = editor_input_len();
    if (len <= 0) return;
    editor_cursor_pos_set(0);
    editor_cursor_pos_extend_selection(len);
}

/* Copy: stash the active selection into EditorClipboardState's
 * input-text payload. No-op if no selection. */
static void demo_handle_copy(void) {
    if (!editor_input_selection_active()) return;
    int lo = editor_input_selection_lo();
    int hi = editor_input_selection_hi();
    editor_clipboard_set_input_text(editor_input_text() + lo, hi - lo);
}

/* Cut: copy, then drop the selection from the input buffer. */
static void demo_handle_cut(void) {
    if (!editor_input_selection_active()) return;
    demo_handle_copy();
    (void)edit_op_consume_input_selection();
}

/* Paste: drop any active selection, then splice the clipboard's
 * input-text payload in at the (now collapsed) cursor. Insert one
 * char at a time so the existing cursor/anchor invariants on the
 * buffer-only primitive stay intact; stop early on buffer-full. */
static void demo_handle_paste(void) {
    if (!editor_clipboard_has_input_text()) return;
    (void)edit_op_consume_input_selection();
    const char *clip = editor_clipboard_input_text();
    int n = editor_clipboard_input_text_len();
    for (int i = 0; i < n; i++) {
        if (!edit_op_buffer_insert_char_at_cursor(clip[i])) break;
    }
}

/* ---- Key dispatch ---------------------------------------------- */

void demo_input_handle_key(unsigned char key, int x, int y) {
    (void)x; (void)y;

    /* Search mode intercepts the keyboard whole — typing edits
     * the query, Enter accepts, Esc cancels. Stays first so a
     * stray Esc doesn't exit the demo while the user thinks
     * they're cancelling a find. */
    if (g_demo_search.active) {
        (void)demo_search_handle_key(key);
        return;
    }

    if (key == 27) {  /* Escape */
        exit(0);
    }

    if (key == '\r' || key == '\n') {
        demo_handle_enter();
        return;
    }

    /* Backspace (8) and Delete (127). Selection-active or
     * non-zero cursor: defer to edit_op_backspace (consumes
     * selection / deletes one char left). Cursor at column 0
     * with no selection: merge the input row into the end of
     * the previous buffer line — inverse of demo_handle_enter.
     * Forward delete is a separate primitive — deferred. */
    if (key == 8 || key == 127) {
        if (editor_cursor_pos() == 0 && !editor_input_selection_active())
            demo_handle_backspace_at_line_start();
        else
            (void)edit_op_backspace();
        return;
    }

    /* Clipboard / find shortcuts: both Ctrl+letter (Linux /
     * Windows / X11 GLUT) and Cmd+letter (macOS freeglut) trip
     * the same paths.
     *
     *   - real Ctrl: GLUT delivers the ASCII control byte
     *     (Ctrl+A = 0x01, Ctrl+C = 0x03, Ctrl+F = 0x06,
     *     Ctrl+G = 0x07, Ctrl+V = 0x16, Ctrl+X = 0x18).
     *     glutGetModifiers reports GLUT_ACTIVE_CTRL.
     *   - macOS Cmd: freeglut's Cocoa backend reports Cmd as
     *     GLUT_ACTIVE_SUPER (NOT CTRL) and delivers the *letter*
     *     byte ('a'/'c'/'f'/'g'/'v'/'x'). Check both bits so
     *     either real Ctrl or Cmd fires the shortcut; gate on
     *     the modifier so a bare typed letter isn't treated as
     *     a shortcut. The production editor's editor_input_active_modifiers
     *     in src/editor/input.c uses the same SUPER-as-CTRL alias.
     *
     * Ctrl+H (0x08) is already handled above as backspace. */
    int mods = glutGetModifiers();
#ifdef GLUT_ACTIVE_SUPER
    int ctrl_or_cmd = (mods & (GLUT_ACTIVE_CTRL | GLUT_ACTIVE_SUPER)) != 0;
#else
    int ctrl_or_cmd = (mods & GLUT_ACTIVE_CTRL) != 0;
#endif
    if (key == 1  || (ctrl_or_cmd && (key == 'a' || key == 'A'))) {
        demo_handle_select_all(); return;
    }
    if (key == 3  || (ctrl_or_cmd && (key == 'c' || key == 'C'))) {
        demo_handle_copy(); return;
    }
    if (key == 6  || (ctrl_or_cmd && (key == 'f' || key == 'F'))) {
        demo_search_begin(); return;
    }
    if (key == 7  || (ctrl_or_cmd && (key == 'g' || key == 'G'))) {
        /* Cmd+G next, Cmd+Shift+G prev — mainstream macOS find-
         * again convention. Cmd+Shift+G arrives as 'G' (capital)
         * via the shift modifier, hence the case-letter split. */
        demo_search_next(key == 'G' ? -1 : +1); return;
    }
    if (key == 22 || (ctrl_or_cmd && (key == 'v' || key == 'V'))) {
        demo_handle_paste(); return;
    }
    if (key == 24 || (ctrl_or_cmd && (key == 'x' || key == 'X'))) {
        demo_handle_cut(); return;
    }

    if (key_is_printable_ascii(key)) {
        (void)edit_op_type_char((char)key);
        return;
    }

    /* Other control bytes (Tab, remaining Ctrl+letter) intentionally
     * fall through as no-ops. Adding bindings here is the natural
     * extension point. */
}

void demo_input_handle_special(int key, int x, int y) {
    (void)x; (void)y;

    EditorInputView input = editor_state_input();
    int cur = input.cursor_pos;
    int len = input.input_len;
    int line = editor_state_edit_line();
    /* Shift extends a character-range selection via the editor's
     * extend-selection primitive (pins the anchor on first
     * extension; subsequent moves grow / shrink in place). Plain
     * arrow moves clear any active anchor — both behaviors are
     * what mainstream editors do. */
    int shift = (glutGetModifiers() & GLUT_ACTIVE_SHIFT) != 0;

    switch (key) {
    case GLUT_KEY_LEFT:
        if (cur > 0) {
            if (shift) editor_cursor_pos_extend_selection(cur - 1);
            else       editor_cursor_pos_set(cur - 1);
        }
        break;
    case GLUT_KEY_RIGHT:
        if (cur < len) {
            if (shift) editor_cursor_pos_extend_selection(cur + 1);
            else       editor_cursor_pos_set(cur + 1);
        }
        break;
    case GLUT_KEY_HOME:
        if (shift) editor_cursor_pos_extend_selection(0);
        else       editor_cursor_pos_set(0);
        break;
    case GLUT_KEY_END:
        if (shift) editor_cursor_pos_extend_selection(len);
        else       editor_cursor_pos_set(len);
        break;
    case GLUT_KEY_UP:
        demo_input_navigate_to(line - 1);
        break;
    case GLUT_KEY_DOWN:
        demo_input_navigate_to(line + 1);
        break;
    case GLUT_KEY_F3:
        /* F3 / Shift+F3: cycle through search matches. Mirrors the
         * Ctrl+G / Cmd+Shift+G shortcuts in the regular keyboard
         * dispatcher for users used to the F3 convention. */
        demo_search_next((glutGetModifiers() & GLUT_ACTIVE_SHIFT) ? -1 : +1);
        break;
    default:
        /* Other special keys ignored in v1. */
        break;
    }
}
