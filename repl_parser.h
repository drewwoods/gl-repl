/*
 * repl_parser.h - Public parser entrypoints for REPL source lines.
 *
 * Translates user input into structured GLCmd records for the parser-owned
 * command forms: comments, table-driven GL commands, and the other syntax that
 * feeds the general command model. Float declarations, variable assignments,
 * and block control-flow statements are handled by the commit pipeline before
 * the parser sees the line.
 *
 * Parse context allows internal callers (repl_flatten.c during expansion,
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

#include "sample.h"

/* Parse context: allows internal callers to parse lines outside the active
 * editor position. source_line_idx is used for error reporting and line-number
 * tracking across nested parse calls (e.g., inside repl_flatten_program).
 * vars/num_vars make local loop/function variables visible to expression
 * evaluation. strict_refs enforces that function calls reference declared
 * functions (CMD_FUNC_DEF); default 0 for back-compat with reformatting
 * and test paths that re-parse already-committed lines.
 *
 * Phase H.5 commit 40: err_buf / err_sz invert the diagnostic flow.
 * When err_buf is non-NULL, the parser writes any error message into
 * the buffer instead of calling set_status() directly, so the editor
 * (or whatever orchestrates the commit) decides whether and how to
 * surface the diagnostic. Legacy callers that leave err_buf NULL keep
 * the old set_status() side effect during the migration. */
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

/* Context-aware parser entrypoint. Pass a ReplParseContext from
 * internal callers when the parse position is not the active editor
 * line (e.g., flatten.c parsing a source command at a different
 * index, or replay.c doing a step-back parse). Pass NULL to fall
 * back to the legacy edit-line behavior used by the no-ctx wrappers.
 *
 * Returns 1 on success, 0 on parse error. On success, out->cmd holds
 * the parsed command and out->text holds the editor-buffer form of
 * the normalized source. On failure, parse diagnostics are written
 * to `ctx->err_buf` when provided; the parser core never calls
 * set_status itself. */
int repl_parser_parse_command_ctx(const char *line, ReplParsedLine *out,
                                  const ReplParseContext *ctx);

/* Legacy no-context parse: surfaces diagnostics via set_status as a
 * back-compat bridge for the test harness. New code should use
 * `repl_parser_parse_command_ctx` with its own err_buf and decide
 * how to surface parse failures. The bridge is tracked by
 * `check-no-set-status-in-repl-parser` and ratchets toward zero. */
int repl_parser_parse_command(const char *line, GLCmd *cmd);

#endif
