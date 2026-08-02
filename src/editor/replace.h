/*
 * editor/replace.h - Find-bar replace operations.
 *
 * The editor owns what a "match" is (the search session's query, case
 * folding, and whole-word flag) and therefore owns the substitution. It
 * builds the rewritten document text and hands it to
 * repl_document_rebuild(), which validates and applies it as one
 * transaction - see src/repl/replace.h for why a replace cannot be a
 * sequence of per-line commits.
 *
 * Both entry points push exactly one undo snapshot, so Ctrl+Z reverses a
 * whole replace-all, and roll the undo ring back if the rebuild is
 * rejected. Status text reports the outcome either way.
 */
#ifndef EDITOR_REPLACE_H
#define EDITOR_REPLACE_H

/* Replace every match of the live search query with the replace-field
 * text. Returns the number of occurrences replaced, 0 when there was
 * nothing to do, or -1 when the rewritten document was rejected (the
 * document is unchanged in that case). */
int editor_replace_all(void);

/* Replace only the match the find bar is currently sitting on, then step
 * to the next one. Same return convention as editor_replace_all().
 *
 * Note that renaming a variable or function one match at a time cannot
 * succeed - the intermediate document is invalid and gets rejected. That
 * is what Replace All exists for; this entry point is for editing literal
 * values and one-off text. */
int editor_replace_current(void);

#endif /* EDITOR_REPLACE_H */
