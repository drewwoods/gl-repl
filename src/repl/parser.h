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
 * replay.c during step-back) to parse lines outside the active edit position
 * and against an explicit source-scope view. A parse is deterministic for a
 * stable context: parsing the same line twice yields identical results. The
 * parser does not read the live editor, command list, source-scope cache, or
 * function-alias table; callers that intentionally parse against the live
 * document pass those views in the context.
 *
 * Expression validation: identifiers in expressions are validated against
 * the predefined-variable table (float x, y, z, t, etc.) and reserved function
 * names (sin, cos, sqrt, etc.). Function references in CMD_CALL can be
 * validated against existing CMD_FUNC_DEF rows in source_scope if strict_refs
 * is enabled.
 */
#ifndef REPL_PARSER_H
#define REPL_PARSER_H

#include "repl/command.h"
#include "repl/eval.h"
#include "repl/source_scope.h"

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
 * surface the diagnostic. */
typedef struct {
    int source_line_idx;
    ExprVar *vars;
    int num_vars;
    int strict_refs;
    char *err_buf;
    int   err_sz;
    /* When set, the parser skips emitting the canonical source text into
     * out->text (out->text is left as ""). Callers that only consume the
     * parsed GLCmd - notably src/repl/flatten.c, which re-parses every
     * command on each frame and discards the text - set this to avoid the
     * per-arg %g/snprintf rendering and trailing-comment scan. Default 0
     * (emit text) preserves the behavior for commit / reformat / export. */
    int skip_text;
    /* Function aliases visible to this parse. A zero-initialized view means no
     * aliases; callers parsing against the live REPL should pass
     * repl_func_alias_view(). */
    ReplFuncAliasView func_aliases;
    /* Source-scope view for indentation / block-depth / begin-block queries
     * and strict function-reference checks. NULL means no scope information:
     * the parser uses top-level/default indentation and does not read live REPL
     * state. */
    const ReplSourceScopeView *source_scope;
    /* The parsed command is the body of a loop being assembled in the same
     * compile transaction, so its CMD_FOR_BEGIN is not present in source_scope
     * yet. This permits break/continue to bind to that pending loop while
     * retaining the ambient scope for indentation and all other validation. */
    int pending_loop_body;
    /* Optional expression-span capture (see ReplExprCaptureSink in
     * repl/eval.h). When set, the parser fires the sink at every point it
     * text-evaluates an expression, so the compiled-expression cache can
     * compile exactly the spans this parse evaluated. NULL (the default for
     * commit/reformat/tests) is today's behavior. Every parser branch that
     * evaluates expressions must fire it - a branch that doesn't would make
     * the compiled path bake stale args for that command family. */
    const ReplExprCaptureSink *capture;
} ReplParseContext;

/* Parser output struct. On success, cmd holds the parsed command and text
 * holds the canonical source form (indented, trailing semicolon), which is
 * also the form the editor buffer stores for display/re-edit. */
typedef struct {
    GLCmd cmd;
    char  text[MAX_LINE_LEN];
} ReplParsedLine;

/* Context-aware parser entrypoint. The ReplParseContext is required
 * (NULL returns 0 immediately): every caller - flatten.c parsing
 * source commands at a known index, replay.c doing a step-back
 * parse, editor commit, the lean loader, tests - already
 * constructs a context. The former NULL
 * fallback (which read repl_state_edit_line() for source_line_idx)
 * is gone because parser code does not reach back into REPL cursor
 * accessors; the fallback was confirmed dead code, not just a
 * defensive vestige.
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
 * `args` is the raw text between the outer parens - caller is
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

int repl_label_split_args_named(const char *args,
                                char *fmt, int fmt_sz,
                                char *post, int post_sz,
                                char *err, int err_sz,
                                const char *func_name);

#endif
