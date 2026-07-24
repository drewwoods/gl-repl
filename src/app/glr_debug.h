/*
 * glr_debug.h - Debug dump helpers for the full app build.
 *
 * These helpers format editor text, flattened command state, and runtime
 * layout details for CLI flags and tests. They are intentionally narrow:
 * dump-only utilities that read the existing state models without owning
 * any part of the runtime themselves.
 */
#ifndef GLR_DEBUG_H
#define GLR_DEBUG_H

#include <stdio.h>

#include "source_document.h" /* SourceTextView */
#include "app/glr_cli.h"     /* GlrCliOptions */

void glr_debug_dump_editor(FILE *out, SourceTextView text);
void glr_debug_dump_flat_commands_sync(FILE *out, SourceTextView text);
void glr_debug_dump_flat_histogram(FILE *out, SourceTextView text);
void glr_debug_dump_current_editor(FILE *out);
void glr_debug_dump_current_flat_commands_sync(FILE *out);
void glr_debug_dump_current_flat_histogram(FILE *out);
void glr_debug_dump_runtime_state_layout(FILE *out);

/* CLI dump dispatch. If opts selects any --dump-* / --flat-histogram flag,
 * bootstrap the REPL (GL-free: no context, no window), load --example if
 * given, run the requested dumps to `out` (NULL -> stdout), and return 1 so
 * the caller exits 0. Returns 0 when no dump flag is set (proceed to the
 * windowed run). */
int glr_debug_run_dumps(const GlrCliOptions *opts, FILE *out);

#endif /* GLR_DEBUG_H */
