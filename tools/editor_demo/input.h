/*
 * tools/editor_demo/input.h - Generic editor demo input dispatcher.
 *
 * Plain-text key bindings on top of src/editor/edit_ops primitives,
 * without any REPL or REPL-flavored controller (input.c, commit.c,
 * clipboard.c, undo.c, etc.) coupling. The demo replaces the REPL
 * editor's editor_handle_key / editor_handle_special with these
 * two entry points; the underlying EditorState is the same struct,
 * just driven by a different controller.
 *
 * Mirror of the layering claim:
 *   src/editor/input.c        = REPL editor input dispatcher.
 *   tools/editor_demo/input.c = generic editor input dispatcher.
 *   Both call src/editor/edit_ops for primitive text edits.
 */
#ifndef EDITOR_DEMO_INPUT_H
#define EDITOR_DEMO_INPUT_H

/* Route a printable / control ASCII key (the GLUT keyboard
 * callback's `key` argument). Mouse coordinates carried for
 * symmetry; not used in v1. */
void demo_input_handle_key(unsigned char key, int x, int y);

/* Route a GLUT special key (arrows, Home, End, F-keys). */
void demo_input_handle_special(int key, int x, int y);

/* Cross-line navigation: commit the current input row to the
 * buffer at the active edit_line, advance the edit_line to
 * `target`, and load that buffer line's text into the input row
 * (cursor parked at end of loaded text). `target` is clamped to
 * [0, line_count] so callers can park on the "virtual" line one
 * past the last committed buffer entry (the next Enter creates a
 * fresh row there). Exposed so the demo's mouse-click handler can
 * drive the same navigation primitive the Up / Down arrow keys
 * use. */
void demo_input_navigate_to(int target);

/* Commit the current input row's text into the buffer at the
 * active edit_line (without changing edit_line). Exposed for the
 * Save menu action so on-disk content matches what the user sees,
 * including any in-progress typed text not yet committed via Enter
 * or Up/Down. */
void demo_input_commit_to_buffer(void);

#endif /* EDITOR_DEMO_INPUT_H */
