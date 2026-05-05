/*
 * editor_completion.c - Editor completion provider registry.
 */
#include "editor_completion.h"
#include "editor_state.h"

#include <stddef.h>

static const EditorCompletionProvider *g_provider = NULL;

void editor_completion_register(const EditorCompletionProvider *provider) {
    g_provider = provider;
}

const EditorCompletionProvider *editor_completion_provider(void) {
    return g_provider;
}

void editor_completion_update(void) {
    if (g_provider && g_provider->update)
        g_provider->update();
}

void editor_completion_update_selected_preview(void) {
    if (g_provider && g_provider->update_selected_preview)
        g_provider->update_selected_preview();
}

void editor_completion_clear(void) {
    editor_state_autocomplete_clear();
    if (g_provider && g_provider->clear)
        g_provider->clear();
}
