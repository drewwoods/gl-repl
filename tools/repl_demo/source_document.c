/*
 * tools/repl_demo/source_document.c -- Standalone source-document
 * backend for repl_demo.
 *
 * Phase 6 of feature/source-document-port.md: prove the REPL pipeline
 * works without the editor by linking the demo against this tiny
 * static line store instead of glr_source_document.c (which forwards
 * to EditorState).
 *
 * The store is a fixed MAX_COMMANDS x MAX_LINE_LEN array. The
 * full-app adapter and this demo backend implement the same
 * source_document_* surface; pipeline TUs cannot tell them apart.
 */

#include "source_document.h"

#include <string.h>

static char g_demo_lines[MAX_COMMANDS][MAX_LINE_LEN];
static int  g_demo_line_count;

SourceTextView source_document_view(void) {
    SourceTextView v = {
        .lines      = g_demo_lines,
        .line_count = g_demo_line_count,
    };
    return v;
}

static void demo_copy_into(int dst, const char *line) {
    if (!line) line = "";
    size_t n = strlen(line);
    if (n >= MAX_LINE_LEN) n = MAX_LINE_LEN - 1;
    memcpy(g_demo_lines[dst], line, n);
    g_demo_lines[dst][n] = '\0';
}

int source_document_insert_line(int pos, const char *line) {
    if (pos < 0 || pos > g_demo_line_count) return 0;
    if (g_demo_line_count >= MAX_COMMANDS) return 0;
    for (int i = g_demo_line_count; i > pos; i--)
        memcpy(g_demo_lines[i], g_demo_lines[i - 1], sizeof(g_demo_lines[i]));
    demo_copy_into(pos, line);
    g_demo_line_count++;
    return 1;
}

int source_document_replace_line(int pos, const char *line) {
    if (pos < 0 || pos >= g_demo_line_count) return 0;
    demo_copy_into(pos, line);
    return 1;
}

int source_document_load_lines(const char *const *lines, int count) {
    if (count < 0) count = 0;
    if (count > MAX_COMMANDS) count = MAX_COMMANDS;
    for (int i = 0; i < count; i++)
        demo_copy_into(i, lines ? lines[i] : "");
    g_demo_line_count = count;
    return 1;
}

void source_document_clear(void) {
    g_demo_line_count = 0;
}

static int demo_delete_range(int pos, int count) {
    if (pos < 0 || count < 0) return 0;
    if (pos + count > g_demo_line_count) return 0;
    int tail = g_demo_line_count - (pos + count);
    for (int i = 0; i < tail; i++)
        memcpy(g_demo_lines[pos + i], g_demo_lines[pos + count + i],
               sizeof(g_demo_lines[pos + i]));
    g_demo_line_count -= count;
    return 1;
}

int source_document_apply_change(const SourceTextChange *change) {
    if (!change) return 0;
    if (change->delete_count > 0 && change->delete_pos >= 0) {
        if (!demo_delete_range(change->delete_pos, change->delete_count))
            return 0;
    }
    switch (change->kind) {
    case SOURCE_TEXT_NO_CHANGE:
        return 1;
    case SOURCE_TEXT_INSERT_ONE:
        return source_document_insert_line(change->pos, change->text[0]);
    case SOURCE_TEXT_REPLACE_ONE:
        return source_document_replace_line(change->pos, change->text[0]);
    case SOURCE_TEXT_INSERT_MANY: {
        for (int i = 0; i < change->count; i++) {
            if (!source_document_insert_line(change->pos + i, change->text[i]))
                return 0;
        }
        return 1;
    }
    case SOURCE_TEXT_DELETE_RANGE:
        return demo_delete_range(change->pos, change->count);
    case SOURCE_TEXT_LOAD_ALL: {
        const char *ptrs[MAX_COMMIT_CMDS] = {0};
        int n = change->count;
        if (n < 0) n = 0;
        if (n > MAX_COMMIT_CMDS) n = MAX_COMMIT_CMDS;
        for (int i = 0; i < n; i++)
            ptrs[i] = change->text[i];
        return source_document_load_lines(ptrs, n);
    }
    }
    return 0;
}
