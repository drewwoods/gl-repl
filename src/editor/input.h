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

/* Test seam: editor input reads modifiers through this hook so tests
 * can simulate Ctrl/Shift/Alt without a real GLUT context. Production
 * code installs no provider — `editor_get_modifiers()` falls back to
 * glutGetModifiers() once `editor_input_enable_glut_modifier_reads()`
 * has been called. */
typedef int (*ReplModifierProvider)(void);

/* Side-effects accumulated by editor input dispatch and replayed by
 * glr_ctrl_apply_input_effects (request_redraw → glutPostRedisplay,
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

/* Production hook: imrepl_ctrl calls this from glr_ctrl_init_gl
 * after glutInit so editor_get_modifiers() may safely call
 * glutGetModifiers(). Tests that don't install a modifier provider
 * skip this hook; modifier reads return 0 instead of aborting
 * freeglut for being called pre-init. */
void editor_input_enable_glut_modifier_reads(void);

/* macOS Cmd+letter translation. On the freeglut-fork, Cmd+letter is
 * delivered as the raw letter byte (e.g. 'b' = 0x62) rather than the
 * control-character that real Ctrl+letter produces (e.g. KEY_CTRL_B
 * = 0x02). Callers that dispatch on the control-character form must
 * translate before their lookup or they'll miss the shortcut.
 *
 * Returns the translated key when SUPER is held and the input is a
 * letter; otherwise returns key unchanged. The controller calls this
 * at the top of its keyboard route so every downstream handler
 * (cfg-shortcut chain, replay, save, editor) sees the already-
 * translated form. */
unsigned char editor_input_normalize_super_to_ctrl(unsigned char key);

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

/* Move the active edit-line cursor to a source line and sync the input buffer
 * to that line's text. */
void navigate_to_line(int target);

/* Rename-capture predicate. The inline rename overlay is a hard modal:
 * when active, every keystroke must land in the rename buffer ahead of
 * the controller-side router. glr_ctrl_keyboard / _special invoke
 * these BEFORE any other dispatch so backtick / Ctrl+G / F1 / F12 etc.
 * cannot leak out of the rename buffer. */
int editor_input_rename_capture_key(unsigned char key);
int editor_input_rename_capture_special(int key);

/* Editor-side command-store / input-buffer operations.
 *
 * These mutate editor state directly (command store, input buffer,
 * cursor, transient state). They are NOT REPL-pipeline helpers and
 * must not be called from repl_*.c pipeline TUs — the hard guards
 * `check-no-feed-line-in-pipeline` and
 * `check-no-load-line-to-input-in-pipeline` enforce that. */

/* Delete cmds[start..start+count) with a status-bar message describing
 * what was removed. Guards against removing a `float` decl whose
 * variable is still referenced elsewhere. */
void delete_cmd_range(int start, int count, const char *what);

/* Clear ALL commands unconditionally (same behavior as Ctrl+L). */
void repl_clear_all_cmds(void);

/* Sync the input buffer to the source line at `idx` (strips trailing
 * `;` and whitespace). Used by the editor when navigating to an
 * existing line and by reformat/scene-switch paths. */
void load_line_to_input(int idx);

/* Drop camera / menu / picker / code-panel-drag transient state in
 * addition to the editor commit transients. Called from
 * glr_app_reset_all() and from controller paths that switch examples /
 * scenes so the editor returns to a clean idle posture. */
void repl_editor_reset_transients(void);

/* Programmatic entry point equivalent to typing `line` and pressing
 * `;`. Used by tests, the clipboard paste path, and editor-side
 * file/example loaders. Pipeline TUs use `repl_load_apply_line()` in
 * `src/repl/load.h` instead. */
int feed_line(const char *line);

/* If an input-buffer selection is active, delete [lo, hi) from input[],
 * place the cursor at lo, and clear the anchor. Returns 1 if anything
 * was deleted, 0 if no selection was active. Used as the pre-step for
 * typed-char insertion, backspace/delete, and input-text paste so all
 * three "replace selection" cases share one mutation primitive. */
int editor_input_consume_selection(void);

#endif /* EDITOR_INPUT_H */
