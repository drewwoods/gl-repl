/*
 * editor_completion.h - Editor completion provider seam.
 *
 * The editor owns the autocomplete popup state on EditorState, but it does
 * not own the grammar-specific matching logic. This seam lets a provider
 * register the three operations the editor needs: recompute matches, refresh
 * the selected preview, and clear popup state.
 *
 * In the full app build the provider is installed from glr_completion during
 * startup, so editor input stays generic while REPL-specific completion logic
 * lives outside the editor module. If no provider is registered, the
 * editor_completion_* entry points are harmless no-ops.
 */
#ifndef EDITOR_COMPLETION_H
#define EDITOR_COMPLETION_H

typedef struct {
    /* Recompute the match list (and ghost/hint) from the current
     * editor input. Reads input via editor_state_*; writes to
     * editor_state_autocomplete_mut. */
    void (*update)(void);

    /* Refresh the ghost/hint preview to match the selected entry
     * after a user navigation. */
    void (*update_selected_preview)(void);

    /* Drop all matches and clear ghost/hint. */
    void (*clear)(void);

    /* Accept the currently selected match: append the ghost-text
     * suffix to the input, advance the cursor, and clear popup
     * state. Called by the Tab and Enter routes when a match is
     * active. */
    void (*accept)(void);
} EditorCompletionProvider;

void editor_completion_register(const EditorCompletionProvider *provider);

/* Returns the currently registered provider, or NULL if none. */
const EditorCompletionProvider *editor_completion_provider(void);

/* Editor-side entry points. Each delegates to the registered
 * provider; if no provider is registered the call is a no-op. */
void editor_completion_update(void);
void editor_completion_update_selected_preview(void);
void editor_completion_clear(void);
void editor_completion_accept(void);

#endif /* EDITOR_COMPLETION_H */
