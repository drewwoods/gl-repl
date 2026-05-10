/*
 * editor/reformat.h - Editor wrapper for the REPL reformat pass.
 *
 * The pure source-level rewrite (re-emit canonical text per command)
 * lives on the REPL pipeline as `repl_reformat_program()`. The wrapper
 * here adds the editor input save/restore and the post-pass
 * `load_line_to_input` refresh so the typed input buffer survives a
 * Ctrl+\ reformat.
 *
 * Pipeline TUs that need to canonicalize loaded text (post-load
 * normalization) call the pure REPL helper directly; only the editor's
 * Ctrl+\ key path and tests that exercise the editor save/restore use
 * this wrapper.
 */
#ifndef EDITOR_REFORMAT_H
#define EDITOR_REFORMAT_H

void editor_reformat_commands(void);

#endif /* EDITOR_REFORMAT_H */
