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
 * command-store, editor-buffer, or predefined-variable state. This module owns
 * the apply orchestration. That separation became explicit in step 5b of
 * feature/decouple-repl-from-gl-repl-alt.md after a review finding on compile
 * purity.
 */
#ifndef REPL_LOAD_H
#define REPL_LOAD_H

/* Compile + apply a single source line at a caller-chosen index.
 *
 * Replaces feed_line() for callers that don't want editor input
 * dispatch (cursor mutations, insert mode toggle, input buffer
 * writes). Dispatch order matches feed_line / try_commit_*:
 *   float decl → var assign (via repl_compile_dispatch)
 *   close_brace → for_loop → func_def → if_block (block validators)
 *   plain GL command (via repl_parse_and_normalize_strict)
 *
 * Returns 1 if the line was consumed, 0 if nothing matched. On parse error
 * fills `err` with a diagnostic and returns 0. Callers usually loop this over
 * imported/example lines, then mark the flat program and auto-normal state dirty
 * once at the end of the batch.
 *
 * Caller responsibilities:
 *   - Set repl_state_edit_line to the desired insertion index, which
 *     must be in [0, repl_state_document_count()]. The line is
 *     inserted at that index; document_count means append-at-end and
 *     any smaller value inserts in the middle of the document.
 *     (Mid-document inserts are used by the tutorial runner to place
 *     instruction comments above earlier labeled commands; the
 *     append case is still what example/workspace loaders use.)
 *   - Clear editor_insert_mode (the loader does not consult it).
 *   - Call repl_state_mark_flat_dirty / repl_state_mark_normals_dirty
 *     after the load loop completes. */
int repl_load_apply_line(const char *line, char *err, int err_size);

#endif /* REPL_LOAD_H */
