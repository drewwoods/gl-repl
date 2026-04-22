/*
 * repl_parser.h — Public parser entrypoints for REPL source lines.
 */
#ifndef REPL_PARSER_H
#define REPL_PARSER_H

#include "sample.h"

/* Parse a single REPL line into `cmd`. Returns 1 on success, 0 otherwise.
 * The _with_vars variant makes loop/function locals visible to the
 * expression evaluator (for flattening contexts). */
int repl_parse_command(const char *line, GLCmd *cmd);
int repl_parse_command_with_vars(const char *line, GLCmd *cmd,
                                 ExprVar *vars, int num_vars);

#endif
