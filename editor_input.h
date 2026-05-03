/*
 * editor_input.h - Editor input dispatch entry points.
 *
 * Phase J1 commit 44 migrates the keyboard dispatch body out of
 * repl_editor.c into editor_input.c. `editor_handle_key` is the
 * canonical entry — no longer a shim. Special / mouse / motion /
 * mousewheel still delegate to repl_*_func bodies in repl_editor.c
 * (commits 45 and 46 migrate those next).
 *
 * The boundary check (`check-no-repl-editor-input-shim`, commit 49b)
 * locks the contract that editor_input.c may not call repl_*_func
 * dispatch bodies once the migration is complete.
 */
#ifndef EDITOR_INPUT_H
#define EDITOR_INPUT_H

#include "repl_core.h"           /* ReplInputDispatchEffects */
#include "repl_core_internal.h"  /* ReplModifierProvider (test seam) */

ReplInputDispatchEffects editor_handle_key(unsigned char key, int x, int y);
ReplInputDispatchEffects editor_handle_special(int key, int x, int y);
ReplInputDispatchEffects editor_handle_mouse(int button, int state, int x, int y);
ReplInputDispatchEffects editor_handle_motion(int x, int y);
ReplInputDispatchEffects editor_handle_passive_motion(int x, int y);
#ifndef USE_GLUT
ReplInputDispatchEffects editor_handle_mousewheel(int wheel, int direction, int x, int y);
#endif

/* Effect accumulation API used by the five public dispatch entry
 * points. During the keyboard / special / mouse / motion migration
 * (commits 44–46) the legacy repl_*_func wrappers in repl_editor.c
 * also call these helpers; once every body has moved into
 * editor_input.c they go file-private again. */
void                     editor_reset_input_effects(void);
ReplInputDispatchEffects editor_take_input_effects(void);
void                     editor_request_redraw(void);
void                     editor_set_cursor(int cursor);
void                     editor_schedule_timer(unsigned int millis, int value);
int                      editor_get_modifiers(void);

/* Audio-gesture coupling. The Web Audio context stays suspended until
 * a user gesture, so the very first key / mouse event fires
 * repl_audio_on_user_gesture once. Native builds make this a no-op.
 * Phase J1 commit 48a relocates this state to imrepl_ctrl. */
void editor_input_notify_audio_gesture_once(void);

/* Test seam: 49a renames repl_set_modifier_provider_for_test ->
 * editor_input_set_modifier_provider_for_test. Today both names
 * resolve to the same storage. */
void editor_input_set_modifier_provider_for_test(ReplModifierProvider provider);
int  editor_input_active_modifiers(void);

/* Router pre-dispatch stubs.
 *
 * These are the non-editor concerns that bled into the keyboard /
 * special dispatch in repl_editor.c. Phase J1 commits 47a / 47b lift
 * each of them up into imrepl_ctrl router pre-dispatch; until then
 * the bodies remain in editor_input.c and dispatch from
 * keyboard_func / special_func calls them inline. Each returns 1 if
 * the key was consumed.
 */
int editor_input_router_handle_save_key(unsigned char key);             /* Ctrl+S */
int editor_input_router_handle_debug_dump_key(unsigned char key);       /* Ctrl+P */
int editor_input_router_handle_quit_key(unsigned char key);             /* Ctrl+Q */
int editor_input_router_handle_config_menu_key(unsigned char key);      /* backtick → config menu */
int editor_input_router_handle_active_replay_key(unsigned char key);    /* replay forwarding when active */
int editor_input_router_handle_replay_toggle_key(unsigned char key);    /* Ctrl+G + replay shortcuts */
int editor_input_router_handle_cfg_shortcut_key(unsigned char key);     /* repl_cfg_handle_ascii_shortcut */
int editor_input_router_handle_accum_samples_key(unsigned char key);    /* Ctrl+= / Ctrl+- */
int editor_input_router_handle_replay_special(int key);                 /* replay-active forwarding */
int editor_input_router_handle_cfg_special_shortcut(int key);           /* cfg shortcut on F-keys */
int editor_input_router_handle_horizontal_audio_special(int key);       /* Ctrl+Left/Right audio */
int editor_input_router_handle_help_tab_special(int key);               /* Left/Right help-tab when help visible */
int editor_input_router_handle_help_scroll_special(int key);            /* Up/Down/PageUp/Down when help visible */
int editor_input_router_handle_help_toggle_special(int key);            /* F1 */
int editor_input_router_handle_scene_cycle_special(int key);            /* F12 */

/* Mouse-side router stubs lifted into imrepl_ctrl pre-dispatch by
 * commit 47c. Each returns 1 if the event was consumed.
 *
 * The variable-panel drag begin / right-config press preempt any
 * code-panel handling because they own UI rects that may overlap
 * the code panel. The variable-panel drag release on UP fires
 * after the editor's UP cleanup so ui_panels_handle_mouse_release
 * still runs first.
 *
 * Camera mouse event + scroll wheel + scene press are still inside
 * editor_handle_mouse's chain pending a `consumed` field on
 * ReplInputDispatchEffects (deferred).
 */
int editor_input_router_handle_variable_panel_drag_begin(int button, int state, int x, int y);
int editor_input_router_handle_right_config_press(int button, int state, int x, int y);

/* Rename-capture predicate. The inline rename overlay is a hard modal:
 * when active, every keystroke must land in the rename buffer ahead of
 * the controller-side router. imrepl_ctrl_keyboard / _special invoke
 * these BEFORE any other dispatch so backtick / Ctrl+G / F1 / F12 etc.
 * cannot leak out of the rename buffer. */
int editor_input_rename_capture_key(unsigned char key);
int editor_input_rename_capture_special(int key);

#endif /* EDITOR_INPUT_H */
