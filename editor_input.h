/*
 * editor_input.h - Editor input dispatch entry points.
 *
 * The editor owns text-document concerns only: cursor / selection /
 * scroll / search / autocomplete / clipboard / undo / commit chain.
 * Non-editor concerns (replay, audio, config, save, scene cycle,
 * variable panel drag, camera, scene press, scroll-wheel zoom) route
 * through imrepl_ctrl directly to their owning subsystem; the helper
 * bodies live in imrepl_ctrl.c. This file's surface is just the editor
 * dispatch entry points + the predicates the controller uses to decide
 * which subsystem owns a click.
 */
#ifndef EDITOR_INPUT_H
#define EDITOR_INPUT_H

#include "repl_core_internal.h"  /* ReplModifierProvider (test seam) */

/* Side-effects accumulated by editor input dispatch and replayed by
 * imrepl_ctrl_apply_input_effects (request_redraw → glutPostRedisplay,
 * set_cursor → glutSetCursor, schedule_timer → glutTimerFunc). */
typedef struct ReplInputDispatchEffects {
    int request_redraw;
    int set_cursor;
    int cursor;
    int schedule_timer;
    unsigned int timer_millis;
    int timer_value;
} ReplInputDispatchEffects;

/* Editor dispatch entry points. Each handles editor-text-model
 * concerns only — text edit / cursor / selection / autocomplete /
 * commit / code-panel resize+drag+scroll. Non-editor concerns are
 * already filtered out by imrepl_ctrl before these run. Direct
 * callers (test fixtures) get the same shape: only editor routes
 * fire when these are invoked. */
ReplInputDispatchEffects editor_handle_key(unsigned char key, int x, int y);
ReplInputDispatchEffects editor_handle_special(int key, int x, int y);
ReplInputDispatchEffects editor_handle_mouse(int button, int state, int x, int y);
ReplInputDispatchEffects editor_handle_motion(int x, int y);
ReplInputDispatchEffects editor_handle_passive_motion(int x, int y);
#ifndef USE_GLUT
ReplInputDispatchEffects editor_handle_mousewheel(int wheel, int direction, int x, int y);
#endif

/* Effect accumulation API used by the editor dispatch entry points
 * and by the controller's router helpers (declared in imrepl_ctrl.h)
 * so both produce/consume the same ReplInputDispatchEffects struct. */
void                     editor_reset_input_effects(void);
ReplInputDispatchEffects editor_take_input_effects(void);
void                     editor_request_redraw(void);
void                     editor_set_cursor(int cursor);
void                     editor_schedule_timer(unsigned int millis, int value);
int                      editor_get_modifiers(void);

/* Test seam: drive editor input with a mocked modifier source. */
void editor_input_set_modifier_provider_for_test(ReplModifierProvider provider);
int  editor_input_active_modifiers(void);

/* Hit-test predicates that imrepl_ctrl uses to decide which subsystem
 * owns a click. A click is editor's concern when it lands on the code
 * panel proper, on the divider, or while the example dropdown is open
 * (which conceptually extends the code panel). */
int editor_input_point_in_code_panel(int x, int y);
int editor_input_point_on_code_panel_divider(int x, int y);
int editor_input_code_panel_resize_cursor(void);

/* Code-panel layout query + hidden-panel restore. The controller uses
 * the latter when opening the config menu via backtick (the menu
 * needs the panel visible to render). */
int editor_input_code_panel_layout(void);
int editor_input_code_panel_hidden(void);
int editor_input_restore_hidden_code_panel(void);

/* Editor-side scroll handler invoked by the controller when a scroll
 * wheel event lands inside the code panel rect. Other scroll-wheel
 * destinations (camera zoom, help overlay scroll) live in
 * imrepl_ctrl.c. */
void editor_input_code_panel_scroll(int direction);

/* Rename-capture predicate. The inline rename overlay is a hard modal:
 * when active, every keystroke must land in the rename buffer ahead of
 * the controller-side router. imrepl_ctrl_keyboard / _special invoke
 * these BEFORE any other dispatch so backtick / Ctrl+G / F1 / F12 etc.
 * cannot leak out of the rename buffer. */
int editor_input_rename_capture_key(unsigned char key);
int editor_input_rename_capture_special(int key);

#endif /* EDITOR_INPUT_H */
