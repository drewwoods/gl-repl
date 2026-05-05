/*
 * repl_debug.h - Diagnostic dumps for CLI flags and tests.
 */
#ifndef REPL_DEBUG_H
#define REPL_DEBUG_H

#include <stdio.h>

#include "editor_state.h"  /* EditorBufferView */

void repl_debug_dump_editor(FILE *out, EditorBufferView text);
void repl_debug_dump_flat_commands(FILE *out, EditorBufferView text);
void repl_debug_dump_runtime_state_layout(FILE *out);

#endif /* REPL_DEBUG_H */
