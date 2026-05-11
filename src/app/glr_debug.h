/*
 * repl_debug.h - Diagnostic dumps for CLI flags and tests.
 */
#ifndef GLR_DEBUG_H
#define GLR_DEBUG_H

#include <stdio.h>

#include "editor/state.h"    /* EditorBufferView */
#include "source_document.h" /* SourceTextView */

void glr_debug_dump_editor(FILE *out, SourceTextView text);
void glr_debug_dump_flat_commands(FILE *out, EditorBufferView text);
void glr_debug_dump_runtime_state_layout(FILE *out);

#endif /* GLR_DEBUG_H */
