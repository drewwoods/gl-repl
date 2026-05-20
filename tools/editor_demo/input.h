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

#endif /* EDITOR_DEMO_INPUT_H */
