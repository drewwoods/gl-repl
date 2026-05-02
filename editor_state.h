#ifndef EDITOR_STATE_H
#define EDITOR_STATE_H

/* EditorState owns editable text, cursor, selection, navigation, undo
 * transactions, and the rest of the code-editor session per the
 * three-layer ownership contract in MODULES.md and
 * feature/editor-owns-text-completion.md.
 *
 * Phase 1 scaffold (commit 3): the struct is intentionally a placeholder
 * until commit 4 lands the editor_buffer slice. Slice migration order:
 *
 *   commit 4: editor_buffer
 *   commit 5: editor_input / cursor / edit_line / insert_mode / pending_newline
 *   commit 6: selection + clipboard + search + autocomplete
 *   commit 7: status / help / panel / viewport / pointer (those move to
 *             UiState — listed here only because the slice-by-slice
 *             migration in editor-owns-text-completion.md §1.2 walks
 *             editor + UI slices together).
 *
 * capture/restore/reset symmetry mirrors repl_state_capture / restore /
 * reset_all. repl_state_reset_all() now drains all three structs so
 * every test reset clears the full app state.
 */

typedef struct {
    /* Placeholder field. Removed in commit 4 when editor_buffer is the
     * first real slice to land here. C disallows zero-sized aggregates
     * in standard mode, and using a placeholder keeps the layout stable
     * while the struct is still empty. */
    int _phase1_scaffold_placeholder;
} EditorState;

void editor_state_capture(EditorState *snapshot);
void editor_state_restore(const EditorState *snapshot);
void editor_state_reset(void);

#endif /* EDITOR_STATE_H */
