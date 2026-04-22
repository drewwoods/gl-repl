/*
 * repl_parser.h — Public parser entrypoints for REPL source lines.
 */
#ifndef REPL_PARSER_H
#define REPL_PARSER_H

#include "sample.h"

typedef struct {
    int source_line_idx;
    ExprVar *vars;
    int num_vars;
    /* When strict_refs != 0, the parser rejects a top-level CMD_CALL
     * whose target funcN has no matching CMD_FUNC_DEF in g_cmds[],
     * the same way it rejects an assignment to an undeclared variable.
     * Default 0 for back-compat with unit tests / reformat paths that
     * re-parse already-committed commands. */
    int strict_refs;
} ReplParseContext;

/* Context-aware parser entrypoint. Pass this from internal callers when the
 * parse position is not necessarily the active editor line. NULL falls back to
 * the legacy `g_edit_line` behavior used by compatibility wrappers. */
int repl_parse_command_ctx(const char *line, GLCmd *cmd,
                           const ReplParseContext *ctx);

/* Parse a single REPL line into `cmd`. Returns 1 on success, 0 otherwise.
 * The _with_vars variant makes loop/function locals visible to the
 * expression evaluator (for flattening contexts). */
int repl_parse_command(const char *line, GLCmd *cmd);
int repl_parse_command_with_vars(const char *line, GLCmd *cmd,
                                 ExprVar *vars, int num_vars);

#endif
