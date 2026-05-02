#include "editor_state.h"

#include <stddef.h>

static EditorState g_editor_state;
static const EditorState g_editor_state_defaults = {0};

void editor_state_capture(EditorState *snapshot) {
    if (!snapshot)
        return;
    *snapshot = g_editor_state;
}

void editor_state_restore(const EditorState *snapshot) {
    if (!snapshot)
        return;
    g_editor_state = *snapshot;
}

void editor_state_reset(void) {
    g_editor_state = g_editor_state_defaults;
}
