/*
 * src/repl/load.h - Non-editor source-load entry point.
 *
 * Step 5b of feature/decouple-repl-from-gl-repl-alt.md split this out
 * of src/repl/compile.c after a [P2] review finding: src/repl/compile.c is the
 * pure-validator module (each entry point reads inputs, returns a
 * ReplCompiledChange, and must not write the command store / editor
 * buffer / predef-var registrations). repl_load_apply_line drives the
 * apply side — it orchestrates compile → predef apply → editor-buffer
 * apply → command-store apply. That orchestration belongs in its own
 * module so the compile module can stay pure.
 *
 * Used by:
 *   - src/repl/export.c importer (single-line apply during file-load)
 *   - src/repl/example_loader.c (built-in example loading; the loader
 *     classifies lines into 3 buckets — decls, func_def blocks,
 *     other — and emits each bucket through repl_load_apply_line
 *     so the layout matches the editor's canonical reorder
 *     without needing to touch the lean loader's append-at-end
 *     contract; see step 7e of the decouple plan)
 *
 * Not used by:
 *   - editor commit chain (editor_commit_apply_plan covers that path
 *     with EditorCommitPlan side effects)
 */
#ifndef REPL_LOAD_H
#define REPL_LOAD_H

/* Compile + apply a single source line at the end of the document.
 *
 * Replaces feed_line() for callers that don't want editor input
 * dispatch (cursor mutations, insert mode toggle, input buffer
 * writes). Dispatch order matches feed_line / try_commit_*:
 *   float decl → var assign (via repl_compile_dispatch)
 *   close_brace → for_loop → func_def → if_block (block validators)
 *   plain GL command (via repl_parse_and_normalize_strict)
 *
 * Returns 1 if the line was consumed, 0 if nothing matched. On parse
 * error fills `err` with a diagnostic and returns 0.
 *
 * Caller responsibilities:
 *   - Set repl_state_edit_line to repl_state_document_count() before
 *     the load loop.
 *   - Clear editor_insert_mode (the loader assumes append-at-end).
 *   - Call repl_state_mark_flat_dirty / repl_state_mark_normals_dirty
 *     after the load loop completes. */
int repl_load_apply_line(const char *line, char *err, int err_size);

#endif /* REPL_LOAD_H */
