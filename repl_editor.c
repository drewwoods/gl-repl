/*
 * repl_editor.c — Transitional public wrappers.
 *
 * Phase J1 commits 44–46 migrated keyboard / special / mouse / motion /
 * mousewheel dispatch into editor_input.c. Commit 48b inlined the timer
 * dispatch into imrepl_ctrl_timer. What remains here:
 *  - repl_*_func public wrappers used by imrepl_ctrl + tests
 *    (commit 49a deletes the wrappers; tests migrate to editor_handle_*)
 *  - repl_editor_active_modifiers + repl_set_modifier_provider_for_test
 *    legacy forwarders (commit 49a will rename)
 */
#include "repl_core_internal.h"
#include "editor_input.h"

void repl_set_modifier_provider_for_test(ReplModifierProvider provider) {
    editor_input_set_modifier_provider_for_test(provider);
}

int repl_editor_active_modifiers(void) {
    return editor_input_active_modifiers();
}

ReplInputDispatchEffects repl_keyboard_func(unsigned char key, int x, int y) {
    return editor_handle_key(key, x, y);
}

ReplInputDispatchEffects repl_special_func(int key, int x, int y) {
    return editor_handle_special(key, x, y);
}

ReplInputDispatchEffects repl_mouse_func(int button, int state, int x, int y) {
    return editor_handle_mouse(button, state, x, y);
}

ReplInputDispatchEffects repl_motion_func(int x, int y) {
    return editor_handle_motion(x, y);
}

ReplInputDispatchEffects repl_passive_motion_func(int x, int y) {
    return editor_handle_passive_motion(x, y);
}

#ifndef USE_GLUT
ReplInputDispatchEffects repl_mousewheel_func(int wheel, int direction, int x, int y) {
    return editor_handle_mousewheel(wheel, direction, x, y);
}
#endif
