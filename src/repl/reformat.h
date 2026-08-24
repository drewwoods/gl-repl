/*
 * src/repl/reformat.h - Whole-document REPL source reformatter.
 */
#ifndef REPL_REFORMAT_H
#define REPL_REFORMAT_H

/* Pure REPL pass: walks every command and rewrites the canonical
 * line text + GLCmd in place. Does not save/restore editor input;
 * the editor's Ctrl+\ wrapper (`editor_reformat_commands`) layers
 * that on top. */
void repl_reformat_program(void);

struct ReplCompiledChange_s;

/* Re-derive ONLY the base indentation of rows that changed side when the
 * display-body boundary moved (see repl_source_scope_display_body_start).
 * `at_before` is the boundary before the edit. Leaves every other byte of
 * every row untouched - unlike repl_reformat_program(), which
 * re-canonicalizes text and must not be used as an incidental fixup.
 * `change` provides index translation for structural insertions and
 * deletions; if NULL, a 1-to-1 index mapping is assumed. */
void repl_reindent_after_change(int at_before, const struct ReplCompiledChange_s *change);
void repl_reindent_after_boundary_move(int at_before);

#endif /* REPL_REFORMAT_H */
