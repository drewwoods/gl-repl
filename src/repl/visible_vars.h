/*
 * src/repl/visible_vars.h - Loop/function-local variable lookup for parse contexts.
 */
#ifndef REPL_VISIBLE_VARS_H
#define REPL_VISIBLE_VARS_H

#include "repl/command.h"
#include "repl/eval.h"
#include "source_document.h"

/* What binds a visible variable. The shared compile/flatten binding tag:
 * name resolution is innermost-first and the *first* match decides, so the
 * kind of that first match is what makes an assignment legal (LOCAL) or not
 * (PARAM / LOOP). Kept here rather than in a compile-private header because
 * flatten tags its own scope arrays with the same enum. */
typedef enum {
    REPL_VISIBLE_VAR_LOOP = 0,  /* for(i, ...) iterator      - not writable */
    REPL_VISIBLE_VAR_PARAM,     /* funcN(a, b) parameter     - not writable */
    REPL_VISIBLE_VAR_LOCAL      /* `float x;` in a func body - writable */
} ReplVisibleVarKind;

/* Populate `vars` with every loop/function-local visible at source line
 * `pos`, walking the explicitly-supplied document (text + command array).
 * Returns the count (capped at max_vars); total_out (if non-NULL) receives the
 * uncapped total for truncation detection. `kinds_out` (if non-NULL) receives
 * one ReplVisibleVarKind per returned entry, parallel to `vars`.
 *
 * Ordering is innermost scope first, and within a function frame the
 * parameters precede that body's locals - flatten builds its call frames the
 * same way, so a resolver written against either sees the same winner.
 * Context-driven: reads no live REPL state, so compile passes its
 * ReplCompileContext document view. */
int collect_visible_vars_in(SourceTextView text, const GLCmd *document_cmds,
                            int document_count, int pos,
                            ExprVar *vars, int max_vars, int *total_out,
                            ReplVisibleVarKind *kinds_out);

/* Live-document convenience wrapper over collect_visible_vars_in (current REPL
 * document). Editor / loader / reformat callers parse against live state
 * through this. */
int collect_visible_vars(int pos, ExprVar *vars, int max_vars, int *total_out);

#endif /* REPL_VISIBLE_VARS_H */
