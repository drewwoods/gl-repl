/*
 * src/repl/visible_vars.h - Loop/function-local variable lookup for parse contexts.
 */
#ifndef REPL_VISIBLE_VARS_H
#define REPL_VISIBLE_VARS_H

#include "repl/command.h"
#include "repl/eval.h"
#include "source_document.h"

/* Populate `vars` with every loop/function-local visible at source line
 * `pos`, walking the explicitly-supplied document (text + command array).
 * Returns the count (capped at max_vars); total_out (if non-NULL) receives the
 * uncapped total for truncation detection. Context-driven: reads no live REPL
 * state, so compile passes its ReplCompileContext document view. */
int collect_visible_vars_in(SourceTextView text, const GLCmd *document_cmds,
                            int document_count, int pos,
                            ExprVar *vars, int max_vars, int *total_out);

/* Live-document convenience wrapper over collect_visible_vars_in (current REPL
 * document). Editor / loader / reformat callers parse against live state
 * through this. */
int collect_visible_vars(int pos, ExprVar *vars, int max_vars, int *total_out);

#endif /* REPL_VISIBLE_VARS_H */
