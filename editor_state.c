#include "editor_state.h"

#include "repl_compile.h"   /* ReplCompiledChange struct definition */

#include <stddef.h>
#include <string.h>

#define EDITOR_STATE_INITIAL                          \
    {                                                 \
        .input = {                                    \
            .input_capacity = MAX_INPUT_LEN,          \
            .pending_newline_capacity = MAX_INPUT_LEN,\
        },                                            \
        .selection = {                                \
            .anchor_idx = -1,                         \
            .end_idx = -1,                            \
        },                                            \
        .clipboard = {                                \
            .cmd_count = 0,                           \
        },                                            \
        .search = {                                   \
            .active = 0,                              \
            .query = "",                              \
            .query_len = 0,                           \
            .cursor_pos = 0,                          \
            .hit_line_idx = -1,                       \
            .hit_char_idx = -1,                       \
            .hit_ordinal = 0,                         \
            .match_count = 0,                         \
        },                                            \
        .autocomplete = {                             \
            .match_count = 0,                         \
            .selected_idx = 0,                        \
            .ghost = "",                              \
            .hint = "",                               \
        },                                            \
        .scroll = {                                   \
            .scroll = 0,                              \
            .scroll_follow_cursor = 0,                \
        },                                            \
    }

static EditorState g_editor_state = EDITOR_STATE_INITIAL;
static const EditorState g_editor_state_defaults = EDITOR_STATE_INITIAL;

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

static void editor_buffer_write_slot(char dst[MAX_LINE_LEN], const char *text) {
    int n;
    if (!text) text = "";
    n = (int)strlen(text);
    if (n >= MAX_LINE_LEN)
        n = MAX_LINE_LEN - 1;
    memcpy(dst, text, (size_t)n);
    dst[n] = '\0';
}

int editor_buffer_insert_lines(int pos, const char *const *lines, int count) {
    ReplEditorBuffer *buf = &g_editor_state.buffer;
    int old_count = buf->line_count;

    if (count <= 0) return 1;
    if (old_count + count > MAX_COMMANDS) return 0;
    if (pos < 0) pos = 0;
    if (pos > old_count) pos = old_count;

    memmove(&buf->lines[pos + count], &buf->lines[pos],
            (size_t)(old_count - pos) * sizeof(buf->lines[0]));
    for (int i = 0; i < count; i++) {
        const char *text = (lines && lines[i]) ? lines[i] : "";
        editor_buffer_write_slot(buf->lines[pos + i], text);
    }
    buf->line_count = old_count + count;
    return 1;
}

int editor_buffer_insert_line(int pos, const char *line) {
    const char *lines[1] = { line };
    return editor_buffer_insert_lines(pos, line ? lines : NULL, 1);
}

int editor_buffer_replace_line(int pos, const char *line) {
    ReplEditorBuffer *buf = &g_editor_state.buffer;
    if (pos < 0 || pos >= MAX_COMMANDS) return 0;
    editor_buffer_write_slot(buf->lines[pos], line ? line : "");
    if (buf->line_count <= pos)
        buf->line_count = pos + 1;
    return 1;
}

int editor_buffer_delete_range(int start, int count) {
    ReplEditorBuffer *buf = &g_editor_state.buffer;
    int old_count = buf->line_count;

    if (count <= 0) return 1;
    if (start < 0) start = 0;
    if (start >= old_count) return 1;
    if (start + count > old_count) count = old_count - start;

    memmove(&buf->lines[start], &buf->lines[start + count],
            (size_t)(old_count - start - count) * sizeof(buf->lines[0]));
    int new_count = old_count - count;
    for (int i = new_count; i < old_count; i++)
        buf->lines[i][0] = '\0';
    buf->line_count = new_count;
    return 1;
}

int editor_buffer_load_lines(const char *const *lines, int count) {
    ReplEditorBuffer *buf = &g_editor_state.buffer;
    if (count < 0) return 0;
    if (count > MAX_COMMANDS) count = MAX_COMMANDS;

    for (int i = 0; i < count; i++) {
        const char *text = (lines && lines[i]) ? lines[i] : "";
        editor_buffer_write_slot(buf->lines[i], text);
    }
    for (int i = count; i < buf->line_count; i++)
        buf->lines[i][0] = '\0';
    buf->line_count = count;
    return 1;
}

void editor_buffer_clear(void) {
    g_editor_state.buffer.line_count = 0;
}

int editor_buffer_apply_compiled_change(const struct ReplCompiledChange_s *change) {
    if (!change) return 0;

    /* Build a const char *[] view of the change's text array — the
     * editor_buffer_* mutators take a list of pointers. */
    const char *line_ptrs[MAX_COMMIT_CMDS];
    for (int i = 0; i < change->count && i < MAX_COMMIT_CMDS; i++)
        line_ptrs[i] = change->text[i];

    /* Optional pre-insert delete fires first to mirror the cmd-store
     * apply ordering. `change->pos` is interpreted in post-delete
     * coordinates. */
    if (change->delete_count > 0) {
        if (!editor_buffer_delete_range(change->delete_pos,
                                        change->delete_count))
            return 0;
    }

    switch (change->kind) {
    case REPL_COMPILED_NO_CHANGE:
        return 1;
    case REPL_COMPILED_INSERT_ONE:
        return editor_buffer_insert_line(change->pos, change->text[0]);
    case REPL_COMPILED_INSERT_MANY:
        return editor_buffer_insert_lines(change->pos, line_ptrs, change->count);
    case REPL_COMPILED_REPLACE_ONE:
        return editor_buffer_replace_line(change->pos, change->text[0]);
    case REPL_COMPILED_DELETE_RANGE:
        return editor_buffer_delete_range(change->pos, change->count);
    case REPL_COMPILED_LOAD_ALL:
        return editor_buffer_load_lines(line_ptrs, change->count);
    }
    return 0;
}

EditorBufferView editor_buffer_view(void) {
    EditorBufferView view = {
        .lines      = g_editor_state.buffer.lines,
        .line_count = g_editor_state.buffer.line_count,
    };
    return view;
}

const char *editor_buffer_view_line(EditorBufferView view, int idx) {
    if (!view.lines || idx < 0 || idx >= MAX_COMMANDS)
        return "";
    return view.lines[idx];
}

/* The canonical edit-line cursor lives on ReplDocumentState. The input
 * slice copies it into the view for callers that consume the input as
 * one bundle. Forward-declared rather than including repl_state_views.h
 * to keep editor_state.h independent of the REPL state facade.
 *
 * The other forward decls below cover entry points editor_state.c
 * implements that get called from sibling impls in this file (e.g.
 * editor_state_input_reset reuses editor_input_clear). They are
 * the same symbols declared in repl_state_views.h / repl_state_owners.h;
 * the headers are not included here to keep editor_state.c free of the
 * REPL state facade. Phase 5's rename will fold these into the
 * editor_* namespace. */
int  repl_state_edit_line(void);
void editor_input_clear(void);
void editor_pending_newline_clear(void);
void editor_cursor_pos_set(int cursor_pos);

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
    editor_input_clear();
    editor_pending_newline_clear();
    g_editor_state.input.insert_mode = 0;
}

const char *editor_input_text(void) {
    return g_editor_state.input.input;
}

char *editor_input_buffer_mut(void) {
    return g_editor_state.input.input;
}

int editor_input_len(void) {
    return g_editor_state.input.input_len;
}

void editor_input_len_set(int input_len) {
    if (input_len < 0)
        input_len = 0;
    if (input_len >= MAX_INPUT_LEN)
        input_len = MAX_INPUT_LEN - 1;
    g_editor_state.input.input_len = input_len;
    g_editor_state.input.input[input_len] = '\0';
    editor_cursor_pos_set(g_editor_state.input.cursor_pos);
}

void editor_input_set_text(const char *text) {
    editor_input_copy_str(g_editor_state.input.input, MAX_INPUT_LEN,
                          text ? text : "");
    editor_input_len_set((int)strlen(g_editor_state.input.input));
    editor_cursor_pos_set(g_editor_state.input.input_len);
}

void editor_input_clear(void) {
    g_editor_state.input.input[0] = '\0';
    g_editor_state.input.input_len = 0;
    g_editor_state.input.cursor_pos = 0;
}

int editor_cursor_pos(void) {
    return g_editor_state.input.cursor_pos;
}

void editor_cursor_pos_set(int cursor_pos) {
    if (cursor_pos < 0)
        cursor_pos = 0;
    if (cursor_pos > g_editor_state.input.input_len)
        cursor_pos = g_editor_state.input.input_len;
    g_editor_state.input.cursor_pos = cursor_pos;
}

int editor_insert_mode(void) {
    return g_editor_state.input.insert_mode;
}

void editor_insert_mode_set(int insert_mode) {
    g_editor_state.input.insert_mode = insert_mode ? 1 : 0;
}

char *editor_pending_newline_buffer_mut(void) {
    return g_editor_state.input.pending_newline;
}

int editor_pending_newline_len(void) {
    return g_editor_state.input.pending_newline_len;
}

void editor_pending_newline_len_set(int newline_len) {
    if (newline_len < 0)
        newline_len = 0;
    if (newline_len >= MAX_INPUT_LEN)
        newline_len = MAX_INPUT_LEN - 1;
    g_editor_state.input.pending_newline_len = newline_len;
    g_editor_state.input.pending_newline[newline_len] = '\0';
}

void editor_pending_newline_set_text(const char *text) {
    editor_input_copy_str(g_editor_state.input.pending_newline, MAX_INPUT_LEN,
                          text ? text : "");
    editor_pending_newline_len_set(
        (int)strlen(g_editor_state.input.pending_newline));
}

void editor_pending_newline_clear(void) {
    g_editor_state.input.pending_newline[0] = '\0';
    g_editor_state.input.pending_newline_len = 0;
}

ReplSelectionState editor_state_selection(void) {
    return g_editor_state.selection;
}

ReplSelectionState *editor_state_selection_mut(void) {
    return &g_editor_state.selection;
}

void editor_state_selection_clear(void) {
    g_editor_state.selection.anchor_idx = -1;
    g_editor_state.selection.end_idx = -1;
}

int editor_state_selection_anchor(void) {
    return g_editor_state.selection.anchor_idx;
}

int editor_state_selection_end_idx(void) {
    return g_editor_state.selection.end_idx;
}

void editor_state_selection_set(int anchor_idx, int end_idx) {
    g_editor_state.selection.anchor_idx = anchor_idx;
    g_editor_state.selection.end_idx = end_idx;
}

ReplClipboardState editor_state_clipboard(void) {
    return g_editor_state.clipboard;
}

ReplClipboardState *editor_state_clipboard_mut(void) {
    return &g_editor_state.clipboard;
}

void editor_state_clipboard_clear(void) {
    g_editor_state.clipboard.cmd_count = 0;
}

GLCmd *editor_state_clipboard_cmds_mut(void) {
    return g_editor_state.clipboard.cmds;
}

int editor_state_clipboard_count(void) {
    return g_editor_state.clipboard.cmd_count;
}

void editor_state_clipboard_count_set(int cmd_count) {
    if (cmd_count < 0)
        cmd_count = 0;
    if (cmd_count > MAX_COMMANDS)
        cmd_count = MAX_COMMANDS;
    g_editor_state.clipboard.cmd_count = cmd_count;
}

ReplSearchState editor_state_search(void) {
    return g_editor_state.search;
}

ReplSearchState *editor_state_search_mut(void) {
    return &g_editor_state.search;
}

void editor_state_search_clear(void) {
    g_editor_state.search = g_editor_state_defaults.search;
}

ReplAutocompleteState editor_state_autocomplete(void) {
    return g_editor_state.autocomplete;
}

ReplAutocompleteState *editor_state_autocomplete_mut(void) {
    return &g_editor_state.autocomplete;
}

void editor_state_autocomplete_clear(void) {
    g_editor_state.autocomplete = g_editor_state_defaults.autocomplete;
}

const EditorTransformerList *editor_state_transformers(void) {
    return &g_editor_state.transformers;
}

void editor_state_transformers_clear(void) {
    g_editor_state.transformers.count = 0;
}

int editor_state_transformers_append(const EditorTransformer *transformer) {
    EditorTransformerList *list = &g_editor_state.transformers;
    if (!transformer || list->count >= MAX_TRANSFORMERS)
        return 0;
    list->items[list->count++] = *transformer;
    return 1;
}

const EditorHighlightList *editor_state_highlights(void) {
    return &g_editor_state.highlights;
}

void editor_state_highlights_clear(void) {
    g_editor_state.highlights.count = 0;
}

int editor_state_highlights_append(int line_idx, int char_start,
                                   int char_end, HighlightKind kind) {
    EditorHighlightList *list = &g_editor_state.highlights;
    if (list->count >= MAX_HIGHLIGHTS)
        return 0;
    list->items[list->count++] = (EditorHighlight){
        .line_idx = line_idx,
        .char_start = char_start,
        .char_end = char_end,
        .kind = kind,
    };
    return 1;
}

const EditorVirtualLineList *editor_state_virtual_lines(void) {
    return &g_editor_state.virtual_lines;
}

void editor_state_virtual_lines_clear(void) {
    g_editor_state.virtual_lines.count = 0;
}

int editor_state_virtual_lines_append(int after_line_idx,
                                      VirtualLineStyle style,
                                      const char *text,
                                      const char *aux) {
    EditorVirtualLineList *list = &g_editor_state.virtual_lines;
    if (list->count >= MAX_VIRTUAL_LINES)
        return 0;
    EditorVirtualLine *vl = &list->items[list->count++];
    vl->after_line_idx = after_line_idx;
    vl->style = style;
    if (text) {
        strncpy(vl->text, text, MAX_VIRTUAL_LINE_TEXT - 1);
        vl->text[MAX_VIRTUAL_LINE_TEXT - 1] = '\0';
    } else {
        vl->text[0] = '\0';
    }
    if (aux) {
        strncpy(vl->aux, aux, MAX_VIRTUAL_LINE_AUX - 1);
        vl->aux[MAX_VIRTUAL_LINE_AUX - 1] = '\0';
    } else {
        vl->aux[0] = '\0';
    }
    return 1;
}

/* Phase J7: the legacy `editor_state_variable_drag` / `_mut` /
 * `_reset` forwarders are gone. Callers use `variable_panel_drag` /
 * `_mut` / `variable_panel_handle_drag_reset` directly. */

EditorScrollState editor_state_scroll(void) {
    return g_editor_state.scroll;
}

EditorScrollState *editor_state_scroll_mut(void) {
    return &g_editor_state.scroll;
}

int editor_scroll(void) {
    return g_editor_state.scroll.scroll;
}

void editor_scroll_set(int scroll) {
    g_editor_state.scroll.scroll = scroll;
}

int editor_scroll_follow_cursor(void) {
    return g_editor_state.scroll.scroll_follow_cursor;
}

void editor_scroll_follow_cursor_set(int follow) {
    g_editor_state.scroll.scroll_follow_cursor = follow ? 1 : 0;
}

/* Line-comment prefix. Set explicitly by the controller at startup
 * (e.g., "// " from imrepl_ctrl). NULL / empty disables comment
 * toggle. The pointer is borrowed; callers pass a string literal or
 * static buffer. */
static const char *g_line_comment_prefix = NULL;

void editor_set_line_comment_prefix(const char *prefix) {
    g_line_comment_prefix = prefix;
}

const char *editor_line_comment_prefix(void) {
    return g_line_comment_prefix;
}
