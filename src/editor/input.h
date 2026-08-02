/*
 * editor_input.h - Editor input dispatch entry points.
 *
 * The editor owns text-document concerns only: cursor, selection, scroll,
 * search, autocomplete, clipboard, undo, and the commit chain. Cross-domain
 * routes such as replay, config, save/load, scene switching, camera, and
 * variable-panel drag stay in glr_ctrl and are filtered before these entry
 * points run.
 *
 * This header therefore exposes the editor dispatch surface plus the hit-test
 * predicates and effect plumbing the controller uses when it needs to hand an
 * event to the editor or replay editor-generated GLUT effects.
 */
#ifndef EDITOR_INPUT_H
#define EDITOR_INPUT_H

/* Test seam: editor input reads modifiers through this hook so tests
 * can simulate Ctrl/Shift/Alt without a real GLUT context. Production
 * code installs no provider — `editor_input_active_modifiers()` falls back to
 * glutGetModifiers() once `editor_input_enable_glut_modifier_reads()`
 * has been called. */
typedef int (*EditorModifierProvider)(void);

/* Production hook: the controller installs a reader so the editor
 * can query the current code-panel layout without depending on
 * src/app/glr_state.h. If no provider is registered the layout
 * defaults to CODE_PANEL_LAYOUT_LEFT (the "everything is visible"
 * case), which keeps tests that don't install a provider in the
 * normal-layout fallback. */
typedef int (*EditorCodePanelLayoutProvider)(void);

/* Side-effects accumulated by editor input dispatch and replayed by
 * glr_ctrl_apply_input_effects (request_redraw → glutPostRedisplay,
 * set_cursor → glutSetCursor,
 * restore_hidden_code_panel → glr_ctrl_restore_hidden_code_panel). */
typedef struct EditorInputDispatchEffects {
    int request_redraw;
    int set_cursor;
    int cursor;
    int restore_hidden_code_panel;
    int close_help_overlay;
} EditorInputDispatchEffects;

/* Editor dispatch entry points. Each handles editor-text-model
 * concerns only — text edit / cursor / selection / autocomplete /
 * commit / code-panel resize+drag+scroll. Non-editor concerns are
 * already filtered out by the controller before these run. Direct
 * callers (test fixtures) get the same shape: only editor routes
 * fire when these are invoked.
 *
 * Effects contract: each runs its route, then drains and returns the
 * pending effects accumulator (including anything the caller
 * accumulated before delegating — no reset at entry). The controller
 * folds the snapshot back in via editor_merge_input_effects and
 * flushes once per GLUT event; tests inspect the snapshot directly. */
EditorInputDispatchEffects editor_handle_key(unsigned char key, int x, int y);
EditorInputDispatchEffects editor_handle_special(int key, int x, int y);
EditorInputDispatchEffects editor_handle_mouse(int button, int state, int x, int y);
EditorInputDispatchEffects editor_handle_motion(int x, int y);
EditorInputDispatchEffects editor_handle_passive_motion(int x, int y);
#ifndef USE_GLUT
EditorInputDispatchEffects editor_handle_mousewheel(int wheel, int direction, int x, int y);
#endif

/* Effect accumulation API used by the editor dispatch entry points and by the
 * controller's router helpers so both produce and consume the same
 * EditorInputDispatchEffects struct. */
void                     editor_reset_input_effects(void);
EditorInputDispatchEffects editor_take_and_reset_input_effects(void);
/* Fold a drained snapshot back into the accumulator (flags OR; a set
 * cursor wins last-writer). Used by the controller's dispatch bodies to
 * keep the one-flush-per-event model when delegating to editor_handle_*. */
void                     editor_merge_input_effects(EditorInputDispatchEffects fx);
void                     editor_request_redraw(void);
void                     editor_set_cursor(int cursor);
void                     editor_request_close_help(void);
int                      editor_input_active_modifiers(void);

/* Test seam: drive editor input with a mocked modifier source. */
void editor_input_set_modifier_provider_for_test(EditorModifierProvider provider);

/* Script-side modifier override. The controller brackets a single scripted
 * keyboard/special dispatch with these
 * (see glr_ctrl_scripted_keyboard_with_modifiers)
 * so a scripted chord's declared Shift/Ctrl/Alt is visible to shortcut
 * matching even though no physical modifier key is held. Push sets the mask
 * and marks it active (authoritative over the test provider and
 * glutGetModifiers); pop clears it. Not nestable — one dispatch at a time. */
void editor_input_push_scripted_modifiers(int mods);
void editor_input_pop_scripted_modifiers(void);

/* Production hook: the controller installs this from glr_ctrl_init_gl
 * so editor_input_code_panel_layout() can read the live layout
 * without including src/app/glr_state.h. Tests that need a non-
 * default layout install their own provider. */
void editor_input_set_code_panel_layout_provider(EditorCodePanelLayoutProvider provider);

/* Production hook: glr_ctrl calls this from glr_ctrl_init_gl
 * after glutInit so editor_input_active_modifiers() may safely call
 * glutGetModifiers(). Tests that don't install a modifier provider
 * skip this hook; modifier reads return 0 instead of aborting
 * freeglut for being called pre-init. */
void editor_input_enable_glut_modifier_reads(void);

/* macOS Cmd+letter translation. On freeglut's Cocoa backend, Cmd+letter is
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

/* Hit-test predicates that the controller uses to decide which subsystem
 * owns a click. A click is editor's concern when it lands on the code
 * panel proper, on the divider, or while the example dropdown is open
 * (which conceptually extends the code panel). */
int editor_input_point_in_code_panel(int x, int y);
int editor_input_point_on_code_panel_divider(int x, int y);
int editor_input_code_panel_resize_cursor(void);

/* Code-panel layout query. Reads through the provider hook installed
 * by editor_input_set_code_panel_layout_provider; defaults to
 * CODE_PANEL_LAYOUT_LEFT when no provider is installed. The
 * actual hidden-panel restoration lives on the controller side
 * (glr_ctrl_restore_hidden_code_panel); editor input requests the
 * restore via the restore_hidden_code_panel effect flag. */
int editor_input_code_panel_layout(void);
int editor_input_code_panel_hidden(void);

/* Editor-side scroll handler invoked by the controller when a scroll
 * wheel event lands inside the code panel rect. Other scroll-wheel
 * destinations (camera zoom, help overlay scroll) live in
 * glr_ctrl.c. */
void editor_input_code_panel_scroll(int direction);

/* Move the active edit-line cursor to a source line and sync the input buffer
 * to that line's text. */
void editor_navigate_to_line(int target);

/* Rename-capture predicate. The inline rename overlay is a hard modal:
 * when active, every keystroke must land in the rename buffer ahead of
 * the controller-side router. glr_ctrl_keyboard / _special invoke
 * these BEFORE any other dispatch so backtick / Ctrl+G / F1 / F12 etc.
 * cannot leak out of the rename buffer. */
int editor_input_rename_capture_key(unsigned char key);
int editor_input_rename_capture_special(int key);

/* File-prompt capture predicate. Same hard-modal contract as rename:
 * when the inline file prompt is open every keystroke must land in
 * the prompt buffer ahead of the controller-side router. */
int editor_input_file_prompt_capture_key(unsigned char key);
int editor_input_file_prompt_capture_special(int key);

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
void editor_delete_cmd_range(int start, int count, const char *what);

/* Clear ALL commands unconditionally (same behavior as Ctrl+L). */
void editor_clear_all_cmds(void);

/* Programmatic document reset for callers that already own undo/status
 * policy and scene-slot lifecycle. For interactive Ctrl+L, use
 * editor_clear_all_cmds(). */
void editor_reset_for_new_scene(void);

/* Sync the input buffer to the source line at `idx` (strips trailing
 * `;` and whitespace). Used by the editor when navigating to an
 * existing line and by reformat/scene-switch paths. */
void editor_load_line_to_input(int idx);

/* Programmatic entry point equivalent to typing `line` and pressing
 * `;`. Used by tests, the clipboard paste path, and editor-side
 * file/example loaders. Pipeline TUs use `repl_load_apply_line()` in
 * `src/repl/load.h` instead. */
int editor_feed_line(const char *line);

/* Walk left/right from char_idx in (text, len) over the word-character
 * class [A-Za-z0-9_] and return the half-open range [out_start, out_end)
 * covering the word. If text[char_idx] is not a word character, or
 * char_idx is out of range, both outputs are clamped to char_idx (a
 * zero-width range). Pure helper — no globals, no GL — so the
 * double-click word-selection path can be tested without faking the
 * click-timing dispatch. */
void editor_input_word_bounds_at(const char *text, int len, int char_idx,
                                 int *out_start, int *out_end);

#endif /* EDITOR_INPUT_H */
