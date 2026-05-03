/*
 * editor_input.h - Editor input dispatch entry points.
 *
 * Phase D commit 26a carves out the editor input layer with the
 * five GLUT-callback entry points the controller dispatches on.
 * Today these are thin shims that delegate to the legacy
 * repl_*_func bodies in repl_editor.c — Phase D commits 26b-26e
 * move the bodies (and their file-local helpers) over here as
 * the structured-commit migration lands.
 *
 * The shim form establishes the boundary check
 * (`check-imrepl-not-editor-mirror` in commit 27 verifies
 * `imrepl_ctrl` calls only `editor_handle_*` for input dispatch).
 */
#ifndef EDITOR_INPUT_H
#define EDITOR_INPUT_H

#include "repl_core.h"  /* ReplInputDispatchEffects */

ReplInputDispatchEffects editor_handle_key(unsigned char key, int x, int y);
ReplInputDispatchEffects editor_handle_special(int key, int x, int y);
ReplInputDispatchEffects editor_handle_mouse(int button, int state, int x, int y);
ReplInputDispatchEffects editor_handle_motion(int x, int y);
ReplInputDispatchEffects editor_handle_passive_motion(int x, int y);

#endif /* EDITOR_INPUT_H */
