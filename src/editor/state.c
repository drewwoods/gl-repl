#include "state.h"

#include "repl/compile.h"   /* ReplCompiledChange struct definition */

#include <stddef.h>
#include <string.h>



static EditorState g_editor_state;   /* zero-initialised by BSS */

/* Writes the handful of non-zero sentinel values that distinguish a
 * valid default EditorState from a raw zero-fill. */
static void editor_state_apply_sentinels(EditorState *s) {
    s->input.anchor_pos               = -1;
    s->selection.anchor_idx           = -1;
    s->selection.end_idx              = -1;
    s->clipboard.kind                 = EDITOR_CLIPBOARD_EMPTY;
    s->cursor_blink.cursor_visible    = 1;
    s->cursor_blink.blink_tick        = 0;
}

/* Lazily-initialised defaults — avoids embedding a 3.2 MB const copy
 * of EditorState in the object file.  Also patches the live state on
 * first call so callers that run before an explicit reset() still see
 * correct sentinels (anchor_pos = -1, etc.). */
static const EditorState *editor_state_get_defaults(void) {
    static EditorState defaults;
    static int inited;
    if (!inited) {
        /* defaults is BSS-zeroed; add the non-zero sentinels. */
        editor_state_apply_sentinels(&defaults);
        /* Patch live state once — it's also BSS-zeroed at this point. */
        editor_state_apply_sentinels(&g_editor_state);
        inited = 1;
    }
    return &defaults;
}

/* Bounded copy: writes src into dst (capacity sz, NUL-terminated). If
 * src is too long, dst is cleared to "" — same surrender behavior as
 * the legacy repl_copy_string_fits helper this slice depended on
 * before the migration. Inlined locally so editor_state.c does not
 * depend on src/repl/core_internal.h. */
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
    /* Ensure sentinels are initialized before capturing. */
    editor_state_get_defaults();
    *snapshot = g_editor_state;
}

void editor_state_restore(const EditorState *snapshot) {
    if (!snapshot)
        return;
    g_editor_state = *snapshot;
}

void editor_state_reset(void) {
    g_editor_state = *editor_state_get_defaults();
}

const EditorBuffer *editor_state_buffer(void) {
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
    EditorBuffer *buf = &g_editor_state.buffer;
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
    EditorBuffer *buf = &g_editor_state.buffer;
    /* Reject pos > line_count: replacing past the end would extend
     * line_count over never-written, zeroed gap lines
     * [line_count, pos-1]. The insert path (editor_buffer_insert_lines)
     * already forecloses the same gap by clamping. pos == line_count is
     * still allowed (a contiguous append of one line, no gap). */
    if (pos < 0 || pos >= MAX_COMMANDS || pos > buf->line_count) return 0;
    editor_buffer_write_slot(buf->lines[pos], line ? line : "");
    if (buf->line_count <= pos)
        buf->line_count = pos + 1;
    return 1;
}

int editor_buffer_delete_range(int start, int count) {
    EditorBuffer *buf = &g_editor_state.buffer;
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
    EditorBuffer *buf = &g_editor_state.buffer;
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
    case REPL_COMPILED_INSERT_MANY: {
        /* Build a const char *[] view of the change's text array — the
         * editor_buffer_* mutators take a list of pointers. We clamp to
         * MAX_COMMIT_CMDS when copying to avoid out-of-bounds reads. */
        const char *line_ptrs[MAX_COMMIT_CMDS] = {NULL};
        int insert_count = change->count;
        if (insert_count > MAX_COMMIT_CMDS)
            insert_count = MAX_COMMIT_CMDS;
        for (int i = 0; i < insert_count; i++)
            line_ptrs[i] = change->text[i];
        return editor_buffer_insert_lines(change->pos, line_ptrs, insert_count);
    }
    case REPL_COMPILED_REPLACE_ONE:
        return editor_buffer_replace_line(change->pos, change->text[0]);
    case REPL_COMPILED_DELETE_RANGE:
        return editor_buffer_delete_range(change->pos, change->count);
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

/* The edit-line cursor lives on g_editor_state.document.edit_line_idx;
 * the editor accessors read and write the field directly. REPL
 * pipeline code receives the value as an explicit function parameter
 * or via the repl_dispatch_edit_line_get/_set sink — never by
 * linking to editor_state_edit_line directly (β invariant;
 * storage moved here in Phase 4 of plans/done/edit-line-ownership.md). */
int editor_state_edit_line(void) {
    return g_editor_state.document.edit_line_idx;
}

void editor_state_edit_line_set(int line) {
    /* Clamp negatives but leave the upper bound to explicit
     * _clamp() calls. Test fixtures that populate document /
     * buffer counts out of order (count set before lines are
     * inserted) rely on _set storing the requested value verbatim;
     * the earlier REPL-side implementation only clamped against
     * the document count it happened to see, which gave the same
     * effective "store verbatim" behavior in the lockstep case. */
    if (line < 0) line = 0;
    g_editor_state.document.edit_line_idx = line;
}

void editor_state_edit_line_clamp(void) {
    int *p = &g_editor_state.document.edit_line_idx;
    if (*p < 0) *p = 0;
    /* Clamp upper bound from the editor's own buffer, not from
     * REPL state, so the demo (which doesn't link src/repl/state.c)
     * resolves cleanly. The REPL document and editor buffer track
     * the same line count in lockstep under the editor commit
     * pipeline; this clamp is conservative either way. */
    int count = g_editor_state.buffer.line_count;
    if (*p > count) *p = count;
}


EditorInputView editor_state_input(void) {
    const EditorInputState *in = &g_editor_state.input;
    return (EditorInputView){
        .input = in->input,
        .input_len = in->input_len,
        .cursor_pos = in->cursor_pos,
        .anchor_pos = in->anchor_pos,
        .pending_newline = in->pending_newline,
        .pending_newline_len = in->pending_newline_len,
        .insert_mode = in->insert_mode,
    };
}

EditorInputState *editor_state_input_mut(void) {
    return &g_editor_state.input;
}

void editor_state_input_reset(void) {
    editor_input_clear();
    editor_pending_newline_clear();
    editor_insert_mode_set(0);
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
    /* The trailing default-policy cursor_pos_set clears the anchor as
     * a side effect, which also satisfies the
     * "anchor_pos in [0, input_len]" invariant when the buffer shrinks.
     * Anchor-preserving callers drive the buffer through narrower
     * primitives and route through
     * editor_cursor_pos_set_keep_anchor themselves (for example, the
     * shift handlers added in Phase E). */
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
    g_editor_state.input.anchor_pos = -1;
}

int editor_cursor_pos(void) {
    return g_editor_state.input.cursor_pos;
}

/* Single home for "move the input cursor." `keep_anchor` distinguishes
 * the default move (which clears the input-selection anchor) from the
 * shift-extended move (which leaves the anchor alone so selection
 * grows / shrinks). The plain entry points below are thin wrappers so
 * the anchor-clear policy lives in exactly one place; the 60+ existing
 * cursor-set call sites stay on the default and inherit auto-clear. */
static void cursor_pos_set_internal(int cursor_pos, int keep_anchor) {
    if (cursor_pos < 0)
        cursor_pos = 0;
    if (cursor_pos > g_editor_state.input.input_len)
        cursor_pos = g_editor_state.input.input_len;
    g_editor_state.input.cursor_pos = cursor_pos;
    if (!keep_anchor) {
        g_editor_state.input.anchor_pos = -1;
        return;
    }
    /* keep_anchor: empty selections still collapse — if the anchor
     * just collided with the cursor, drop it. */
    if (g_editor_state.input.anchor_pos == cursor_pos)
        g_editor_state.input.anchor_pos = -1;
}

void editor_cursor_pos_set(int cursor_pos) {
    cursor_pos_set_internal(cursor_pos, /*keep_anchor=*/0);
}

void editor_cursor_pos_set_keep_anchor(int cursor_pos) {
    cursor_pos_set_internal(cursor_pos, /*keep_anchor=*/1);
}

void editor_cursor_pos_extend_selection(int new_pos) {
    EditorInputState *in = &g_editor_state.input;
    int old = in->cursor_pos;

    if (new_pos < 0) new_pos = 0;
    if (new_pos > in->input_len) new_pos = in->input_len;

    /* Pin the pre-move cursor as the anchor *before* the move so a
     * previously inactive selection becomes [old, new). If the anchor
     * is already set, leave it alone — the existing range just extends
     * or contracts. Using anchor_pos_set here would collapse the
     * anchor on (anchor == cursor) before we get the chance to move,
     * which is exactly the footgun this helper avoids. */
    if (in->anchor_pos < 0 && new_pos != old)
        in->anchor_pos = old;

    cursor_pos_set_internal(new_pos, /*keep_anchor=*/1);

    /* The input-buffer character selection and the whole-line range
     * selection are mutually exclusive views of "what is selected".
     * Whenever a character selection is active, drop any line-range so
     * the two highlight bands never paint at once — e.g. shift+click a
     * multi-line range, then hold shift and press Left/Right, which
     * starts a char selection on the current row. This is the single
     * chokepoint every char-selection path flows through (shift+arrows,
     * shift+Home/End, shift+click same row, click-drag, double-click). */
    if (in->anchor_pos >= 0)
        editor_state_selection_clear();
}

int editor_input_anchor(void) {
    return g_editor_state.input.anchor_pos;
}

void editor_input_anchor_set(int pos) {
    if (pos < 0) {
        g_editor_state.input.anchor_pos = -1;
        return;
    }
    if (pos > g_editor_state.input.input_len)
        pos = g_editor_state.input.input_len;
    /* Empty selection collapses immediately: keep "has selection" a
     * single test (anchor_pos >= 0) rather than two. */
    if (pos == g_editor_state.input.cursor_pos) {
        g_editor_state.input.anchor_pos = -1;
        return;
    }
    g_editor_state.input.anchor_pos = pos;
}

void editor_input_anchor_clear(void) {
    g_editor_state.input.anchor_pos = -1;
}

int editor_input_selection_active(void) {
    return g_editor_state.input.anchor_pos >= 0;
}

int editor_input_selection_lo(void) {
    int a = g_editor_state.input.anchor_pos;
    int c = g_editor_state.input.cursor_pos;
    if (a < 0) return -1;
    return a < c ? a : c;
}

int editor_input_selection_hi(void) {
    int a = g_editor_state.input.anchor_pos;
    int c = g_editor_state.input.cursor_pos;
    if (a < 0) return -1;
    return a > c ? a : c;
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

EditorSelectionState editor_state_selection(void) {
    return g_editor_state.selection;
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

const EditorClipboardState *editor_state_clipboard(void) {
    return &g_editor_state.clipboard;
}

EditorClipboardState *editor_state_clipboard_mut(void) {
    return &g_editor_state.clipboard;
}

void editor_state_clipboard_clear(void) {
    g_editor_state.clipboard.kind = EDITOR_CLIPBOARD_EMPTY;
    g_editor_state.clipboard.line_count = 0;
    g_editor_state.clipboard.input_text_len = 0;
    g_editor_state.clipboard.input_text[0] = '\0';
}

int editor_state_clipboard_count(void) {
    return g_editor_state.clipboard.line_count;
}

EditorClipboardKind editor_state_clipboard_kind(void) {
    return g_editor_state.clipboard.kind;
}

/* Callers must populate clipboard->lines[0..n) via editor_state_clipboard_mut()
 * BEFORE calling with n > 0. This setter stamps the kind and count but does
 * not validate line content. */
void editor_state_clipboard_count_set(int line_count) {
    if (line_count < 0)
        line_count = 0;
    if (line_count > MAX_COMMANDS)
        line_count = MAX_COMMANDS;
    /* _count_set(0) is a full clipboard reset, not a "drop the lines
     * but keep stale input_text" partial — otherwise a previous
     * INPUT_TEXT payload would survive as EMPTY-kind with non-zero
     * input_text_len, breaking the kind/payload invariant. */
    if (line_count == 0) {
        editor_state_clipboard_clear();
        return;
    }
    g_editor_state.clipboard.line_count = line_count;
    g_editor_state.clipboard.kind = EDITOR_CLIPBOARD_LINES;
    /* Lines and input-text payloads are mutually exclusive. */
    g_editor_state.clipboard.input_text_len = 0;
    g_editor_state.clipboard.input_text[0] = '\0';
}

void editor_clipboard_set_input_text(const char *text, int len) {
    EditorClipboardState *cb = &g_editor_state.clipboard;
    if (!text || len <= 0) {
        cb->input_text_len = 0;
        cb->input_text[0] = '\0';
        cb->kind = (cb->line_count > 0) ? EDITOR_CLIPBOARD_LINES
                                        : EDITOR_CLIPBOARD_EMPTY;
        return;
    }
    if (len >= MAX_INPUT_LEN)
        len = MAX_INPUT_LEN - 1;
    memcpy(cb->input_text, text, (size_t)len);
    cb->input_text[len] = '\0';
    cb->input_text_len = len;
    cb->kind = EDITOR_CLIPBOARD_INPUT_TEXT;
    /* Mutually exclusive with the line payload. */
    cb->line_count = 0;
}

int editor_clipboard_has_input_text(void) {
    return g_editor_state.clipboard.kind == EDITOR_CLIPBOARD_INPUT_TEXT
        && g_editor_state.clipboard.input_text_len > 0;
}

const char *editor_clipboard_input_text(void) {
    return g_editor_state.clipboard.input_text;
}

int editor_clipboard_input_text_len(void) {
    return g_editor_state.clipboard.input_text_len;
}

const EditorSearchState *editor_state_search(void) {
    return &g_editor_state.search;
}

EditorSearchState *editor_state_search_mut(void) {
    return &g_editor_state.search;
}

const EditorAutocompleteState *editor_state_autocomplete(void) {
    return &g_editor_state.autocomplete;
}

EditorAutocompleteState *editor_state_autocomplete_mut(void) {
    return &g_editor_state.autocomplete;
}

void editor_state_autocomplete_clear(void) {
    g_editor_state.autocomplete = editor_state_get_defaults()->autocomplete;
}

const UiTransformerList *editor_state_transformers(void) {
    return &g_editor_state.transformers;
}

void editor_state_transformers_clear(void) {
    g_editor_state.transformers.count = 0;
}

int editor_state_transformers_append(const UiTransformer *transformer) {
    UiTransformerList *list = &g_editor_state.transformers;
    if (!transformer || list->count >= MAX_TRANSFORMERS)
        return 0;
    list->items[list->count++] = *transformer;
    return 1;
}

const UiHighlightList *editor_state_highlights(void) {
    return &g_editor_state.highlights;
}

void editor_state_highlights_clear(void) {
    g_editor_state.highlights.count = 0;
}

int editor_state_highlights_append(int line_idx, int char_start,
                                   int char_end, UiHighlightKind kind) {
    UiHighlightList *list = &g_editor_state.highlights;
    if (list->count >= MAX_HIGHLIGHTS)
        return 0;
    list->items[list->count++] = (UiHighlight){
        .line_idx = line_idx,
        .char_start = char_start,
        .char_end = char_end,
        .kind = kind,
    };
    return 1;
}

const UiVirtualLineList *editor_state_virtual_lines(void) {
    return &g_editor_state.virtual_lines;
}

void editor_state_virtual_lines_clear(void) {
    g_editor_state.virtual_lines.count = 0;
}

int editor_state_virtual_lines_append(int after_line_idx,
                                      UiVirtualLineStyle style,
                                      const char *text,
                                      const char *aux) {
    UiVirtualLineList *list = &g_editor_state.virtual_lines;
    if (list->count >= MAX_VIRTUAL_LINES)
        return 0;
    UiVirtualLine *vl = &list->items[list->count++];
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


const UiLineOverrideList *editor_state_line_overrides(void) {
    return &g_editor_state.line_overrides;
}

void editor_state_line_overrides_clear(void) {
    g_editor_state.line_overrides.count = 0;
}

int editor_state_line_overrides_append(int line_idx, const char *text) {
    UiLineOverrideList *list = &g_editor_state.line_overrides;
    UiLineOverride *o;
    if (list->count >= MAX_LINE_OVERRIDES)
        return 0;
    o = &list->items[list->count++];
    o->line_idx = line_idx;
    if (text) {
        strncpy(o->text, text, MAX_LINE_OVERRIDE_TEXT - 1);
        o->text[MAX_LINE_OVERRIDE_TEXT - 1] = '\0';
    } else {
        o->text[0] = '\0';
    }
    return 1;
}

const char *editor_state_line_override_for(int line_idx) {
    const UiLineOverrideList *list = &g_editor_state.line_overrides;
    if (line_idx < 0)
        return NULL;
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].line_idx == line_idx)
            return list->items[i].text;
    }
    return NULL;
}

/* The legacy `editor_state_variable_drag` / `_mut` / `_reset`
 * forwarders are gone. Callers use `variable_panel_drag` / `_mut` /
 * `variable_panel_handle_drag_reset` directly (removed in Phase J7). */

EditorCursorBlinkState editor_state_cursor_blink(void) {
    return g_editor_state.cursor_blink;
}

EditorCursorBlinkState *editor_state_cursor_blink_mut(void) {
    return &g_editor_state.cursor_blink;
}

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
 * (e.g., "// " from glr_ctrl). NULL / empty disables comment
 * toggle. The pointer is borrowed; callers pass a string literal or
 * static buffer. */
static const char *g_line_comment_prefix = NULL;

void editor_set_line_comment_prefix(const char *prefix) {
    g_line_comment_prefix = prefix;
}

const char *editor_line_comment_prefix(void) {
    return g_line_comment_prefix;
}
