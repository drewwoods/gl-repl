#include "editor_state.h"

#include <stddef.h>
#include <string.h>

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

const ReplEditorBuffer *editor_state_buffer(void) {
    return &g_editor_state.buffer;
}

ReplEditorBuffer *editor_state_buffer_mut(void) {
    return &g_editor_state.buffer;
}

const char *editor_buffer_line(int idx) {
    if (idx < 0 || idx >= MAX_COMMANDS)
        return "";
    return g_editor_state.buffer.lines[idx];
}

void editor_buffer_set_line(int idx, const char *text) {
    if (idx < 0 || idx >= MAX_COMMANDS)
        return;
    if (!text) text = "";
    char *dst = g_editor_state.buffer.lines[idx];
    int n = (int)strlen(text);
    if (n >= MAX_LINE_LEN) n = MAX_LINE_LEN - 1;
    memcpy(dst, text, (size_t)n);
    dst[n] = '\0';
}

int editor_buffer_count(void) {
    return g_editor_state.buffer.line_count;
}

void editor_buffer_set_count(int count) {
    if (count < 0) count = 0;
    if (count > MAX_COMMANDS) count = MAX_COMMANDS;
    g_editor_state.buffer.line_count = count;
}
