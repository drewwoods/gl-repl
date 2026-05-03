/*
 * editor_input.c -- Editor input dispatch entry points.
 *
 * Phase D commit 26a thin shims. Each entry point delegates to the
 * legacy repl_*_func body in repl_editor.c so this commit doesn't
 * touch the keyboard / mouse / scroll dispatch internals — only
 * the *boundary* between imrepl_ctrl and the input handlers.
 *
 * Phase D commits 26b-26e migrate handler bodies into this file
 * one structured-commit handler at a time as the
 * compile/apply migration lands; once they have all moved, the
 * shims and the repl_*_func declarations can be deleted (Phase H
 * rename pass).
 */

#include "editor_input.h"
#include "repl_core.h"  /* repl_keyboard_func etc. — legacy bodies */

ReplInputDispatchEffects editor_handle_key(unsigned char key, int x, int y) {
    return repl_keyboard_func(key, x, y);
}

ReplInputDispatchEffects editor_handle_special(int key, int x, int y) {
    return repl_special_func(key, x, y);
}

ReplInputDispatchEffects editor_handle_mouse(int button, int state, int x, int y) {
    return repl_mouse_func(button, state, x, y);
}

ReplInputDispatchEffects editor_handle_motion(int x, int y) {
    return repl_motion_func(x, y);
}

ReplInputDispatchEffects editor_handle_passive_motion(int x, int y) {
    return repl_passive_motion_func(x, y);
}
