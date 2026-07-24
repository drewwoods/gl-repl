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

void glr_debug_dump_editor(FILE *out, SourceTextView text);
void glr_debug_dump_flat_commands_sync(FILE *out, SourceTextView text);
void glr_debug_dump_flat_histogram(FILE *out, SourceTextView text);
void glr_debug_dump_current_editor(FILE *out);
void glr_debug_dump_current_flat_commands_sync(FILE *out);
void glr_debug_dump_current_flat_histogram(FILE *out);
void glr_debug_dump_runtime_state_layout(FILE *out);

/* The CLI dump-and-exit dispatch (glr_boot_run_dumps) lives in the boot band
 * — src/app/boot/glr_boot_dumps.{c,h} — because it consumes GlrCliOptions
 * (glr_cli) and orchestrates a GL-free bootstrap before any frame. It calls
 * the dump primitives above; keeping it out of this TU keeps these formatters
 * free of any boot dependency, so the controller can reuse them at runtime. */

#endif /* GLR_DEBUG_H */
