#include "editor_state.h"

#include <stddef.h>
#include <string.h>

static EditorState g_editor_state = {
    .input = {
        .input_capacity = MAX_INPUT_LEN,
        .pending_newline_capacity = MAX_INPUT_LEN,
    },
};
static const EditorState g_editor_state_defaults = {
    .input = {
        .input_capacity = MAX_INPUT_LEN,
        .pending_newline_capacity = MAX_INPUT_LEN,
    },
};

/* Bounded copy: writes src into dst (capacity sz, NUL-terminated). If
 * src is too long, dst is cleared to "" — same surrender behavior as
 * the legacy repl_copy_string_fits helper this slice depended on
 * before the migration. Inlined locally so editor_state.c does not
 * depend on repl_core_internal.h. */
static void editor_input_copy_str(char *dst, size_t sz, const char *src) {
    if (!dst || sz == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= sz) {
        dst[0] = '\0';
        return;
    }
    memcpy(dst, src, len + 1);
}

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

/* The canonical edit-line cursor lives on ReplDocumentState. The input
 * slice copies it into the view for callers that consume the input as
 * one bundle. Forward-declared rather than including repl_state_views.h
 * to keep editor_state.h independent of the REPL state facade.
 *
 * The other forward decls below cover entry points editor_state.c
 * implements that get called from sibling impls in this file (e.g.
 * editor_state_input_reset reuses repl_state_input_clear). They are
 * the same symbols declared in repl_state_views.h / repl_state_owners.h;
 * the headers are not included here to keep editor_state.c free of the
 * REPL state facade. Phase 5's rename will fold these into the
 * editor_* namespace. */
int  repl_state_edit_line(void);
void repl_state_input_clear(void);
void repl_state_pending_newline_clear(void);
void repl_state_cursor_pos_set(int cursor_pos);

ReplEditorInputView editor_state_input(void) {
    const ReplEditorInputState *in = &g_editor_state.input;
    return (ReplEditorInputView){
        .input = in->input,
        .input_capacity = MAX_INPUT_LEN,
        .input_len = in->input_len,
        .cursor_pos = in->cursor_pos,
        .edit_line_idx = repl_state_edit_line(),
        .pending_newline = in->pending_newline,
        .pending_newline_capacity = MAX_INPUT_LEN,
        .pending_newline_len = in->pending_newline_len,
        .insert_mode = in->insert_mode,
    };
}

ReplEditorInputState *editor_state_input_mut(void) {
    return &g_editor_state.input;
}

void editor_state_input_reset(void) {
    repl_state_input_clear();
    repl_state_pending_newline_clear();
    g_editor_state.input.insert_mode = 0;
}

const char *repl_state_input_text(void) {
    return g_editor_state.input.input;
}

char *repl_state_input_buffer_mut(void) {
    return g_editor_state.input.input;
}

int repl_state_input_len(void) {
    return g_editor_state.input.input_len;
}

void repl_state_input_len_set(int input_len) {
    if (input_len < 0)
        input_len = 0;
    if (input_len >= MAX_INPUT_LEN)
        input_len = MAX_INPUT_LEN - 1;
    g_editor_state.input.input_len = input_len;
    g_editor_state.input.input[input_len] = '\0';
    repl_state_cursor_pos_set(g_editor_state.input.cursor_pos);
}

void repl_state_input_set_text(const char *text) {
    editor_input_copy_str(g_editor_state.input.input, MAX_INPUT_LEN,
                          text ? text : "");
    repl_state_input_len_set((int)strlen(g_editor_state.input.input));
    repl_state_cursor_pos_set(g_editor_state.input.input_len);
}

void repl_state_input_clear(void) {
    g_editor_state.input.input[0] = '\0';
    g_editor_state.input.input_len = 0;
    g_editor_state.input.cursor_pos = 0;
}

int repl_state_cursor_pos(void) {
    return g_editor_state.input.cursor_pos;
}

void repl_state_cursor_pos_set(int cursor_pos) {
    if (cursor_pos < 0)
        cursor_pos = 0;
    if (cursor_pos > g_editor_state.input.input_len)
        cursor_pos = g_editor_state.input.input_len;
    g_editor_state.input.cursor_pos = cursor_pos;
}

int repl_state_insert_mode(void) {
    return g_editor_state.input.insert_mode;
}

void repl_state_insert_mode_set(int insert_mode) {
    g_editor_state.input.insert_mode = insert_mode ? 1 : 0;
}

char *repl_state_pending_newline_buffer_mut(void) {
    return g_editor_state.input.pending_newline;
}

int repl_state_pending_newline_len(void) {
    return g_editor_state.input.pending_newline_len;
}

void repl_state_pending_newline_len_set(int newline_len) {
    if (newline_len < 0)
        newline_len = 0;
    if (newline_len >= MAX_INPUT_LEN)
        newline_len = MAX_INPUT_LEN - 1;
    g_editor_state.input.pending_newline_len = newline_len;
    g_editor_state.input.pending_newline[newline_len] = '\0';
}

void repl_state_pending_newline_set_text(const char *text) {
    editor_input_copy_str(g_editor_state.input.pending_newline, MAX_INPUT_LEN,
                          text ? text : "");
    repl_state_pending_newline_len_set(
        (int)strlen(g_editor_state.input.pending_newline));
}

void repl_state_pending_newline_clear(void) {
    g_editor_state.input.pending_newline[0] = '\0';
    g_editor_state.input.pending_newline_len = 0;
}
