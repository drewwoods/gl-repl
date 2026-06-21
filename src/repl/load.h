/*
 * src/repl/load.h - Line-at-a-time loader used outside editor input dispatch.
 *
 * repl_load_apply_line is the non-editor load path: the caller chooses the
 * insertion index, this helper compiles the line, applies the resulting change,
 * and updates REPL-owned state without touching editor input widgets. Typical
 * callers are the save-file importer, built-in example loader, tutorial comment
 * injector, and integration tests.
 *
 * The split from src/repl/compile.c matters because compile.c is the pure
 * validator layer: it returns ReplCompiledChange descriptors and must not write
 * command-store, source-text, or predefined-variable state. This module owns
 * the apply orchestration.
 */
#ifndef REPL_LOAD_H
#define REPL_LOAD_H

#include "repl/compile.h"

typedef struct ReplLoadTransactionResult {
    int applied;
    int wrote_local;
    int next_cursor;
} ReplLoadTransactionResult;

/* Apply a precompiled structured change through the non-editor loader
 * transaction:
 *   1. preflight command-store feasibility before any mutation,
 *   2. write source text through source_document,
 *   3. apply predef/scratch side effects,
 *   4. apply the command-store change,
 *   5. publish function-alias changes after command-store success.
 *
 * `wrote_local` is set once the source-document mutation has committed.
 * `next_cursor` is the post-transaction cursor value. The command-store apply
 * honors `change->adjust_edit_line`; the apply is asserted after preflight, so
 * a failure there indicates the live store changed during the transaction or
 * the preflight/apply contracts drifted apart. */
int repl_load_apply_compiled_change_transaction(
    const ReplCompiledChange *change,
    int cursor,
    ReplLoadTransactionResult *out);

/* Compile + apply a single source line at a caller-chosen index.
 *
 * Replaces editor_feed_line() for callers that don't want editor input
 * dispatch (cursor mutations, insert mode toggle, input buffer
 * writes). Dispatch order matches editor_feed_line / try_commit_*:
 *   float decl → var assign (via repl_compile_dispatch)
 *   close_brace → for_loop → func_def → if_block (block validators)
 *   plain GL command (via repl_parse_and_normalize_strict)
 *
 * Returns 1 if the line was consumed, 0 if nothing matched. On parse error
 * fills `err` with a diagnostic and returns 0. Callers usually loop this over
 * imported/example lines, then mark the flat program and auto-normal state dirty
 * once at the end of the batch.
 *
 * `edit_line_inout` supplies the insertion index; the function
 * advances it as the document grows so successive calls in a loop
 * see the canonical insert position. Passing NULL falls back to
 * operating on the ambient host cursor through
 * repl_dispatch_edit_line_get / _set: the value is read at entry,
 * advanced across the per-line apply, and written back on success.
 * Ad-hoc callers / tests get the same visible cursor advance as callers that
 * pass an explicit cursor pointer.
 *
 * Caller responsibilities:
 *   - Initialize *edit_line_inout to the desired insertion index, which
 *     must be in [0, repl_state_document_count()]. The line is
 *     inserted at that index; document_count means append-at-end and
 *     any smaller value inserts in the middle of the document.
 *     (Mid-document inserts are used by the tutorial runner to place
 *     instruction comments above earlier labeled commands; the
 *     append case is still what example/workspace loaders use.)
 *   - Clear editor_insert_mode (the loader does not consult it).
 *   - Call repl_state_mark_flat_dirty / repl_state_mark_source_dirty
 *     after the load loop completes. */
int repl_load_apply_line(const char *line, char *err, int err_size,
                         int *edit_line_inout);

#endif /* REPL_LOAD_H */
