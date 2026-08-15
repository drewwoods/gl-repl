/*
 * src/repl/line_scan.h - Where one statement ends and the next begins.
 *
 * Two primitives, shared rather than reimplemented, because two places have to
 * agree about the answer or they disagree about the document:
 *
 *   - `src/repl/import.c`'s physical-line accumulator, which glues continuation
 *     lines together until a statement is complete;
 *   - `src/app/glr_extedit.c`'s incomplete-final-row heuristic, which decides
 *     whether a watched file ends in a half-typed command that belongs in the
 *     live input row rather than in the document.
 *
 * The second cannot be a fresh implementation over the first: the depth is not
 * a naive paren count (string and char literals are skipped, an unquoted `//`
 * ends the code, `()` and `[]` count but `{}` do not - those delimit blocks),
 * and a reimplementation that drifts would park a row the importer would have
 * accepted, or accept one it cannot parse.
 */
#ifndef REPL_LINE_SCAN_H
#define REPL_LINE_SCAN_H

/* Scan one physical line's *code* portion - everything up to an unquoted `//`
 * line comment - advancing the running bracket-nesting depth while skipping
 * string and char literals so brackets and slashes inside them cannot confuse
 * the scan.
 *
 * `*depth` is in/out and must persist across the lines of one statement.
 * `*code_len` receives the trimmed code length (trailing whitespace removed),
 * which is 0 for a blank line, a pure comment, or a directive - so "the last
 * row with code" falls out of it without a second classifier.
 *
 * Returns the last non-space code character, or '\0' when the line has no
 * code. Together, depth and that character tell a continued statement (an open
 * bracket, or no terminator yet) from a complete one. */
char repl_scan_code_line(const char *line, int *depth, int *code_len);

/* Every REPL statement ends in one of `; { } :`.
 *
 * `:` is the odd one out and is kept for error containment, not for a live
 * construct: no valid statement ends in a colon now that labels are gone, so
 * the case can only ever fire on invalid input - a `loop:` in a legacy file.
 * There its effect is to keep that line self-contained. Drop it and the
 * accumulator reads `loop:` as an unfinished statement, glues the next
 * physical line onto it, and reports one joined parse error - losing the
 * following row from the document. */
int repl_is_stmt_terminator(char c);

#endif /* REPL_LINE_SCAN_H */
