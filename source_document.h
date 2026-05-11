/*
 * source_document.h - Neutral source-document port.
 *
 * Decouples the REPL pipeline from the editor text model. REPL TUs use
 * SourceTextView (read) and source_document_* (mutate) to access the
 * live source text. Hosts (full app, demo, tests) provide implementations
 * that adapt their underlying storage:
 *
 *   Full app:     glr_source_document.c forwards to EditorState
 *   Demo:         tools/repl_demo/source_document.c (Phase 6) backs with
 *                 a tiny static line store, no editor link
 *   Tests:        link the full-app adapter OR a fixture under tests/support/
 *
 * The header includes no REPL or editor headers. MAX_COMMANDS and
 * MAX_LINE_LEN come from repl_command.h, which itself is a neutral REPL
 * grammar header (no editor coupling). */
#ifndef SOURCE_DOCUMENT_H
#define SOURCE_DOCUMENT_H

#include "repl_command.h"  /* MAX_COMMANDS, MAX_LINE_LEN */

/* Read-only view over the live source document. Pointer + count;
 * passed by value to REPL consumers that need source text (replay
 * annotations, export, executor display text, flatten reparse, etc.).
 * Non-owning; stays valid as long as the underlying storage does. */
typedef struct {
    const char (*lines)[MAX_LINE_LEN];
    int          line_count;
} SourceTextView;

/* Slice-style index: bounded by MAX_COMMANDS (the underlying storage),
 * not by view.line_count. Matches the editor_buffer_view_line() it
 * replaces — readers that only care about populated lines should guard
 * with view.line_count themselves. */
static inline const char *source_text_line(SourceTextView view, int idx) {
    if (!view.lines || idx < 0 || idx >= MAX_COMMANDS)
        return "";
    return view.lines[idx];
}

/* Mutation port (Phase 2). REPL_DEMO/test hosts that don't exercise
 * mutation may leave these undefined; the linker will flag any reach
 * that actually calls them. */

typedef enum {
    SOURCE_TEXT_NO_CHANGE = 0,
    SOURCE_TEXT_INSERT_ONE,
    SOURCE_TEXT_REPLACE_ONE,
    SOURCE_TEXT_INSERT_MANY,
    SOURCE_TEXT_DELETE_RANGE,
    SOURCE_TEXT_LOAD_ALL,
} SourceTextChangeKind;

#ifndef SOURCE_TEXT_CHANGE_MAX_LINES
#define SOURCE_TEXT_CHANGE_MAX_LINES 16
#endif

typedef struct {
    SourceTextChangeKind kind;
    int pos;
    int count;

    /* Optional pre-insert delete, matching the existing compiled-change
     * combined shape. delete_pos = -1 / delete_count = 0 means none. */
    int delete_pos;
    int delete_count;

    char text[SOURCE_TEXT_CHANGE_MAX_LINES][MAX_LINE_LEN];
} SourceTextChange;

/* Read accessor. Implemented by the host. */
SourceTextView source_document_view(void);

/* Mutation accessors. Implemented by the host. Phase 2 wires them in. */
int  source_document_apply_change(const SourceTextChange *change);
int  source_document_insert_line(int pos, const char *line);
int  source_document_replace_line(int pos, const char *line);
int  source_document_load_lines(const char *const *lines, int count);
void source_document_clear(void);

#endif /* SOURCE_DOCUMENT_H */
