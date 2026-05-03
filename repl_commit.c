/*
 * repl_commit.c -- transitional placeholder.
 *
 * Phase H.5 commit 39 moved every translation-unit-private body
 * formerly hosted here (try_commit_* dispatchers, apply_*_change
 * helpers, the func-decl-resume editor-orchestration scratch, and
 * repl_commit_resolve_insert_exit_target / repl_commit_reset_transients)
 * into editor_commit.c. The corrected M/V/C contract has the editor
 * attempt commits and the REPL act as a pure parser/validator;
 * commit dispatch is editor-side orchestration, not REPL grammar.
 *
 * Commit 41 deletes this file once the public symbol decls in
 * repl_core_internal.h migrate to editor_commit.h and the
 * `try_commit_*` symbol names rename to their `editor_*`
 * destinations. Keeping the file empty (rather than deleting it
 * mid-phase) gives `make sample` a clean object to skip without
 * Makefile churn until the rename pass.
 */
