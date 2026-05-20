/*
 * tools/editor_demo/input.c - Generic editor demo input dispatcher.
 *
 * Translates GLUT key events into edit_ops primitive calls. No REPL
 * coupling: commit semantics, autocomplete, tutorials, comment
 * toggle, reformat, etc. are all REPL-feature concerns and stay
 * out of this dispatcher.
 *
 * v1 surface (intentionally narrow — see plans/active/editor-demo.md
 * "Generic editor demo — locked scope"):
 *   - Printable ASCII -> edit_op_type_char.
 *   - Backspace / Delete -> edit_op_backspace.
 *   - Cursor Left / Right / Home / End -> EditorState primitives.
 *   - Escape -> exit.
 *
 * Out of v1 scope: arrow up/down between lines, word jumps,
 * Shift+arrow selection, Ctrl+A/C/X/V, scroll wheel, find overlay,
 * undo/redo. Each is its own follow-up step once the corresponding
 * edit_ops primitive (or cross-line navigation primitive) lands.
 */

#include "input.h"

#include "editor/edit_ops.h"
#include "editor/state.h"

#include <gl_includes.h>

#include <ctype.h>
#include <stdlib.h>

static int key_is_printable_ascii(unsigned char key) {
    return key >= 32 && key < 127;
}

void demo_input_handle_key(unsigned char key, int x, int y) {
    (void)x; (void)y;

    if (key == 27) {  /* Escape */
        exit(0);
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

    /* Other control bytes (Tab, Enter, Ctrl+letter) intentionally
     * fall through as no-ops in v1. Adding bindings here is the
     * natural extension point. */
}

void demo_input_handle_special(int key, int x, int y) {
    (void)x; (void)y;

    EditorInputView input = editor_state_input();
    int cur = input.cursor_pos;
    int len = input.input_len;

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
    default:
        /* Other special keys ignored in v1. */
        break;
    }
}
