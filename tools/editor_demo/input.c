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
 *   - Backspace / Delete -> edit_op_backspace.
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
 *   - Escape -> exit.
 *
 * Out of scope (deferred): word jumps, scroll wheel, undo/redo,
 * find overlay, multi-line / line-range copy, system clipboard
 * (NSPasteboard / X11 selections).
 */

#include "input.h"

#include "editor/edit_ops.h"
#include "editor/state.h"

#include <gl_includes.h>

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
 * commit. */
static void demo_commit_input_to_buffer(void) {
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

    demo_commit_input_to_buffer();
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

    if (key == 27) {  /* Escape */
        exit(0);
    }

    if (key == '\r' || key == '\n') {
        demo_handle_enter();
        return;
    }

    /* Backspace (8) and Delete (127). Both map to backspace
     * semantics in v1: delete one character to the left of the
     * cursor (or consume the active input selection). Forward
     * delete is a separate primitive — deferred. */
    if (key == 8 || key == 127) {
        (void)edit_op_backspace();
        return;
    }

    /* Clipboard shortcuts: both Ctrl+letter (Linux / Windows /
     * X11 GLUT) and Cmd+letter (macOS GLUT) trip the same paths.
     * Two code paths arrive here:
     *
     *   - real Ctrl: GLUT delivers the ASCII control byte
     *     (Ctrl+A = 0x01, Ctrl+C = 0x03, Ctrl+V = 0x16,
     *     Ctrl+X = 0x18). glutGetModifiers reports CTRL.
     *   - macOS Cmd: Apple's GLUT maps Cmd to CTRL in
     *     glutGetModifiers but delivers the *letter* byte
     *     ('a'/'c'/'v'/'x'), so the modifier check is what
     *     distinguishes Cmd+letter from a plain typed letter.
     *
     * Gating the letter form on the CTRL modifier prevents a bare
     * 'c' / 'x' from triggering the shortcut. Ctrl+H (0x08) is
     * already handled above as backspace. */
    int ctrl_or_cmd = (glutGetModifiers() & GLUT_ACTIVE_CTRL) != 0;
    if (key == 1  || (ctrl_or_cmd && (key == 'a' || key == 'A'))) {
        demo_handle_select_all(); return;
    }
    if (key == 3  || (ctrl_or_cmd && (key == 'c' || key == 'C'))) {
        demo_handle_copy(); return;
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
    default:
        /* Other special keys ignored in v1. */
        break;
    }
}
