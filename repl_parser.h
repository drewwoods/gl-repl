/*
 * repl_parser.h - Public parser entrypoints for REPL source lines.
 *
 * Responsible for translating user input (free-form text) into structured
 * GLCmd records. The parser is a cascade of specialized handlers:
 *
 *   1. Float declarations:     float x, y, z;
 *   2. Variable assignments:   x = sin(t);
 *   3. Control flow:           if (...) { } for (...) { } func0() { }
 *   4. Close braces:           }
 *   5. GL commands:            glVertex3f(1, 2, 3), glBegin(GL_TRIANGLES), etc.
 *
 * Each handler tries to match its pattern; if successful, it populates a GLCmd
 * record with parsed type, normalized source text, and evaluated arguments.
 * If all handlers fail, the parser reports "Unknown cmd." in the status.
 *
 * Parse context allows internal callers (repl_flatten.c during expansion,
 * repl_replay.c during step-back) to parse lines outside the active edit position.
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
 * and test paths that re-parse already-committed lines. */
typedef struct {
    int source_line_idx;
    ExprVar *vars;
    int num_vars;
    int strict_refs;
} ReplParseContext;

/* Context-aware parser entrypoint. Pass a ReplParseContext from internal
 * callers when the parse position is not the active editor line (e.g.,
 * repl_flatten.c parsing a source command at a different index, or
 * repl_replay.c doing a step-back parse). Pass NULL to fall back to the
 * legacy g_edit_line behavior used by command-entry wrappers.
 * Returns 1 on success, 0 on parse error (status message set). */
int repl_parser_parse_command_ctx(const char *line, GLCmd *cmd,
                                  const ReplParseContext *ctx);

/* Parse a single REPL line into cmd. Returns 1 on success, 0 on parse error
 * (status message set). The _with_vars variant makes loop/function locals
 * visible to the expression evaluator (required for flattening contexts where
 * expressions may reference the loop counter or function parameters). */
int repl_parser_parse_command(const char *line, GLCmd *cmd);
int repl_parser_parse_command_with_vars(const char *line, GLCmd *cmd,
                                        ExprVar *vars, int num_vars);

#endif
