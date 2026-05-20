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
 *     EditorState primitives.
 *   - Enter (\r or \n) -> split current input at cursor; commit
 *     left half to the buffer at edit_line, push right half into
 *     a new line at edit_line+1, advance edit_line, load the new
 *     line's text into the input row.
 *   - Up / Down -> commit current input, change edit_line, load
 *     target line into the input row. Clamped to [0, line_count]
 *     so callers can park on the "virtual" line one past the last
 *     committed buffer entry (where the next Enter creates a fresh
 *     row).
 *   - Escape -> exit.
 *
 * Out of scope (deferred): word jumps, Shift+arrow selection,
 * Ctrl+A/C/X/V, scroll wheel, undo/redo, find overlay. Each is its
 * own follow-up step once the corresponding edit_ops primitive
 * lands.
 */

#include "input.h"

#include "editor/edit_ops.h"
#include "editor/state.h"

#include <gl_includes.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Demo-local edit-line cursor: read via the shim's
 * repl_state_edit_line(), write via demo_edit_line_set(). Both
 * declared here so we don't have to pull repl/state.h transitively
 * (the shim's whole purpose is keeping repl/ headers out of the
 * demo's compilation unit). Storage lives in
 * tools/editor_demo/repl_shim.c. */
int  repl_state_edit_line(void);
void demo_edit_line_set(int n);

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
    int line = repl_state_edit_line();
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
    demo_edit_line_set(target);
    demo_load_buffer_line(target);
    editor_cursor_pos_set(editor_input_len());
}

/* Enter: split the input row at the cursor. Left half replaces the
 * current line; right half becomes a new line at edit_line+1; the
 * cursor lands at the start of the new line. */
static void demo_handle_enter(void) {
    int line = repl_state_edit_line();
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

    demo_edit_line_set(line + 1);
    editor_input_set_text(right);
    editor_cursor_pos_set(0);
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

    if (key_is_printable_ascii(key)) {
        (void)edit_op_type_char((char)key);
        return;
    }

    /* Other control bytes (Tab, Ctrl+letter) intentionally fall
     * through as no-ops. Adding bindings here is the natural
     * extension point. */
}

void demo_input_handle_special(int key, int x, int y) {
    (void)x; (void)y;

    EditorInputView input = editor_state_input();
    int cur = input.cursor_pos;
    int len = input.input_len;
    int line = repl_state_edit_line();

    switch (key) {
    case GLUT_KEY_LEFT:
        if (cur > 0) editor_cursor_pos_set(cur - 1);
        break;
    case GLUT_KEY_RIGHT:
        if (cur < len) editor_cursor_pos_set(cur + 1);
        break;
    case GLUT_KEY_HOME:
        editor_cursor_pos_set(0);
        break;
    case GLUT_KEY_END:
        editor_cursor_pos_set(len);
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
