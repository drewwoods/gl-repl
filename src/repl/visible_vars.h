/*
 * src/repl/visible_vars.h - Loop/function-local variable lookup for parse contexts.
 */
#ifndef REPL_VISIBLE_VARS_H
#define REPL_VISIBLE_VARS_H

#include "repl/eval.h"

/* Populate `vars` with every loop/function-local visible at source line
 * `pos`. Returns the count (capped at max_vars). If total_out is non-NULL,
 * receives the uncapped total (for truncation detection at commit sites). */
int collect_visible_vars(int pos, ExprVar *vars, int max_vars, int *total_out);

#endif /* REPL_VISIBLE_VARS_H */
