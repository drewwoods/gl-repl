/*
 * glr_source_document.c -- Full-app adapter for the neutral source-document port.
 *
 * Bridges source_document.h (used by REPL pipeline TUs) to the
 * EditorState text buffer (owned by src/editor/state.c). Only the full
 * app and tests that already link the editor compose them through this
 * file; the standalone repl_demo ships its own implementation.
 *
 * Phase 1 (this commit) implements the read accessor. Phase 2 wires the
 * mutation ports. */

#include "source_document.h"

#include "editor/state.h"

SourceTextView source_document_view(void) {
    EditorBufferView v = editor_buffer_view();
    SourceTextView out = { .lines = v.lines, .line_count = v.line_count };
    return out;
}
