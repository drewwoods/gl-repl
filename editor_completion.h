/*
 * editor_completion.h - Editor completion provider seam.
 *
 * Phase G commit 36 entry. The editor owns the autocomplete popup
 * state (ReplAutocompleteState lives on EditorState). What it does
 * NOT own is the semantic completion logic — that depends on the
 * REPL's grammar, command spec, and predefined-variable table.
 *
 * The seam: a provider registers a small set of callbacks; the
 * editor invokes them through editor_completion_update /
 * _update_selected_preview / _clear instead of calling
 * `update_autocomplete()` etc. directly. `repl_autocomplete` is the
 * default provider today. A future host could register a different
 * provider (Lisp REPL, language-server-driven, etc.) without
 * touching editor input dispatch.
 *
 * Lifecycle: registration must happen before the first input event.
 * `repl_autocomplete_register_provider()` runs from startup so
 * existing call sites keep working.
 *
 * No-op safety: if no provider is registered, the editor's
 * editor_completion_* calls are no-ops — popup state stays empty.
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
} EditorCompletionProvider;

void editor_completion_register(const EditorCompletionProvider *provider);

/* Returns the currently registered provider, or NULL if none. */
const EditorCompletionProvider *editor_completion_provider(void);

/* Editor-side entry points. Each delegates to the registered
 * provider; if no provider is registered the call is a no-op. */
void editor_completion_update(void);
void editor_completion_update_selected_preview(void);
void editor_completion_clear(void);

#endif /* EDITOR_COMPLETION_H */
