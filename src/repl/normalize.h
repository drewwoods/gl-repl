/*
 * src/repl/normalize.h - REPL parse-and-normalize entry points.
 */
#ifndef REPL_NORMALIZE_H
#define REPL_NORMALIZE_H

#include "repl/command.h"
#include "repl/eval.h"
#include "repl/source_scope.h"

void repl_normalize_from_parsed(const char *parsed_source,
                                const char *raw_expr,
                                int ensure_semicolon,
                                char *out, int out_sz);

/* Parse `line` into `out_cmd` and optionally write the canonical (normalized)
 * source text into text_out (capacity text_sz; pass NULL/0 to ignore).
 * `preserve_expr` keeps the raw argument expressions instead of re-emitting
 * them from evaluated values. */
int repl_parse_and_normalize(const char *line, int pos,
                             ExprVar *vars, int num_vars,
                             int preserve_expr, GLCmd *out_cmd,
                             char *text_out, int text_sz);

/* Same as repl_parse_and_normalize() but rejects top-level CMD_CALL
 * whose target funcN has no matching CMD_FUNC_DEF. Used by the commit
 * path so undefined calls surface at typing time like undeclared
 * variables do; reformat/flatten/test paths keep the permissive
 * variant above. */
int repl_parse_and_normalize_strict(const char *line, int pos,
                                    ExprVar *vars, int num_vars,
                                    int preserve_expr, GLCmd *out_cmd,
                                    char *text_out, int text_sz);

int repl_parse_and_normalize_strict_with_scope(
        const char *line, int pos,
        ExprVar *vars, int num_vars,
        int preserve_expr, GLCmd *out_cmd,
        char *text_out, int text_sz,
        const ReplSourceScopeView *source_scope,
        ReplFuncAliasView func_aliases);

#endif /* REPL_NORMALIZE_H */
