/*
 * glr_source_document.c -- Full-app adapter for the neutral source-document port.
 *
 * Bridges source_document.h (used by REPL pipeline TUs) to the
 * EditorState text buffer (owned by src/editor/state.c). Exposes both
 * the read view and the full mutation port (insert/replace/load/clear
 * and the combined apply_change variant) so REPL pipeline TUs never
 * reach into editor state directly. The full app composes both layers
 * through this file; the standalone repl_demo links the same adapter
 * today (a tiny editor-free implementation ships separately).
 * (Bridges the REPL parser and compiler text needs to the editor.) */

#include "source_document.h"

#include <string.h>

#include "editor/state.h"

SourceTextView source_document_view(void) {
    EditorBufferView v = editor_buffer_view();
    SourceTextView out = { .lines = v.lines, .line_count = v.line_count };
    return out;
}

int source_document_insert_line(int pos, const char *line) {
    return editor_buffer_insert_line(pos, line);
}

int source_document_replace_line(int pos, const char *line) {
    return editor_buffer_replace_line(pos, line);
}

int source_document_load_lines(const char *const *lines, int count) {
    return editor_buffer_load_lines(lines, count);
}

void source_document_clear(void) {
    editor_buffer_clear();
}

int source_document_apply_change(const SourceTextChange *change) {
    if (!change) return 0;

    /* Validate any pre-delete + follow-up operation first so we never
     * mutate the buffer unless the full change is admissible. */
    int line_count = editor_buffer_count();
    int effective_delete = 0;
    if (change->delete_count > 0 && change->delete_pos >= 0) {
        int start = change->delete_pos;
        int count = change->delete_count;
        if (start < 0) start = 0;
        if (start < line_count) {
            if (start + count > line_count)
                count = line_count - start;
            effective_delete = count;
        }
    }
    int post_delete_count = line_count - effective_delete;

    switch (change->kind) {
    case SOURCE_TEXT_INSERT_ONE:
        if (post_delete_count + 1 > MAX_COMMANDS)
            return 0;
        break;
    case SOURCE_TEXT_INSERT_MANY:
        if (change->count <= 0 || change->count > MAX_COMMIT_CMDS)
            return 0;
        if (post_delete_count + change->count > MAX_COMMANDS)
            return 0;
        break;
    case SOURCE_TEXT_REPLACE_ONE:
        if (change->pos < 0 ||
            change->pos >= MAX_COMMANDS ||
            change->pos > post_delete_count)
            return 0;
        break;
    case SOURCE_TEXT_LOAD_ALL:
        if (change->count > MAX_COMMIT_CMDS)
            return 0;
        break;
    case SOURCE_TEXT_NO_CHANGE:
    case SOURCE_TEXT_DELETE_RANGE:
        break;
    }

    /* Pre-insert delete (matches the combined ReplCompiledChange shape:
     * delete a range, then INSERT at a post-delete position). */
    if (change->delete_count > 0 && change->delete_pos >= 0) {
        if (!editor_buffer_delete_range(change->delete_pos, change->delete_count))
            return 0;
    }

    switch (change->kind) {
    case SOURCE_TEXT_NO_CHANGE:
        return 1;
    case SOURCE_TEXT_INSERT_ONE:
        return editor_buffer_insert_line(change->pos, change->text[0]);
    case SOURCE_TEXT_REPLACE_ONE:
        return editor_buffer_replace_line(change->pos, change->text[0]);
    case SOURCE_TEXT_INSERT_MANY: {
        for (int i = 0; i < change->count; i++) {
            if (!editor_buffer_insert_line(change->pos + i, change->text[i]))
                return 0;
        }
        return 1;
    }
    case SOURCE_TEXT_DELETE_RANGE:
        return editor_buffer_delete_range(change->pos, change->count);
    case SOURCE_TEXT_LOAD_ALL: {
        const char *ptrs[MAX_COMMIT_CMDS];
        int n = change->count;
        if (n < 0) n = 0;
        if (n > MAX_COMMIT_CMDS) return 0;
        for (int i = 0; i < n; i++)
            ptrs[i] = change->text[i];
        return editor_buffer_load_lines(ptrs, n);
    }
    }
    return 0;
}
