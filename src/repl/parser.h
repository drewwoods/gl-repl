/*
 * src/repl/parser.h - Public parser entrypoints for REPL source lines.
 *
 * Translates user input into structured GLCmd records for the parser-owned
 * command forms: comments, table-driven GL commands, and the other syntax that
 * feeds the general command model. Float declarations, variable assignments,
 * and block control-flow statements are handled by the commit pipeline before
 * the parser sees the line.
 *
 * Parse context allows internal callers (src/repl/flatten.c during expansion,
 * replay.c during step-back) to parse lines outside the active edit position.
 * Each parse is stateless and immutable: parsing the same line twice yields
 * identical results.
 *
 * Expression validation: identifiers in expressions are validated against
 * the predefined-variable table (float x, y, z, t, etc.) and reserved function
 * names (sin, cos, sqrt, etc.). Function references in CMD_CALL can be validated
 * against existing CMD_FUNC_DEF if strict_refs is enabled.
 */
#ifndef REPL_PARSER_H
#define REPL_PARSER_H

#include "repl/command.h"
#include "repl/eval.h"

/* Parse context: allows internal callers to parse lines outside the active
 * editor position. source_line_idx is used for error reporting and line-number
 * tracking across nested parse calls (e.g., inside repl_flatten_program).
 * vars/num_vars make local loop/function variables visible to expression
 * evaluation. strict_refs enforces that function calls reference declared
 * functions (CMD_FUNC_DEF); default 0 for back-compat with reformatting
 * and test paths that re-parse already-committed lines.
 *
 * err_buf / err_sz invert the diagnostic flow. When err_buf is
 * non-NULL, the parser writes any error message into
 * the buffer instead of calling set_status() directly, so the editor
 * (or whatever orchestrates the commit) decides whether and how to
 * surface the diagnostic (introduced in phase H.5 commit 40). */
typedef struct {
    int source_line_idx;
    ExprVar *vars;
    int num_vars;
    int strict_refs;
    char *err_buf;
    int   err_sz;
} ReplParseContext;

/* Parser output struct. On success, cmd holds the parsed command and text
 * holds the canonical source form (indented, trailing semicolon), which is
 * also the form the editor buffer stores for display/re-edit. */
typedef struct {
    GLCmd cmd;
    char  text[MAX_LINE_LEN];
} ReplParsedLine;

/* Context-aware parser entrypoint. The ReplParseContext is required
 * (NULL returns 0 immediately): every caller — flatten.c parsing
 * source commands at a known index, replay.c doing a step-back
 * parse, editor commit, the lean loader, tests — already
 * constructs a context. The former NULL
 * fallback (which read repl_state_edit_line() for source_line_idx)
 * since β forbids REPL pipeline code from reaching back into the
 * cursor accessor; the fallback was confirmed dead code, not just
 * a defensive vestige (implemented in phase 3.6.3; see the
 * edit-line-ownership plan doc).
 *
 * Returns 1 on success, 0 on parse error or NULL ctx. On success,
 * out->cmd holds the parsed command and out->text holds the
 * editor-buffer form of the normalized source. On failure, parse
 * diagnostics are written to `ctx->err_buf` when provided; the
 * parser core never calls set_status itself. */
int repl_parser_parse_command_ctx(const char *line, ReplParsedLine *out,
                                  const ReplParseContext *ctx);

/* Split a `label(...)` arg payload into the format string body
 * (no quotes) and post-string args (substitution exprs).
 *
 * `args` is the raw text between the outer parens — caller is
 * responsible for paren stripping. Format string must be the first
 * arg (leading whitespace tolerated, but no preceding tokens).
 *
 * On success: writes fmt / post (each NUL-terminated) and returns 1.
 * On a syntax error (missing opening or closing quote, forbidden
 * char inside string, oversize buffers): writes a description into
 * `err` and returns 0. The function never mutates global state.
 *
 * Forbidden inside the format-string body: backslash, '/'+'/', '(',
 * ')', ','. These keep string-unaware scanners elsewhere in the
 * codebase honest. */
int repl_label_split_args(const char *args,
                          char *fmt, int fmt_sz,
                          char *post, int post_sz,
                          char *err, int err_sz);

#endif
