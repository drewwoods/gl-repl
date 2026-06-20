/*
 * src/repl/core_internal.h - Shared internal parse / normalize helpers.
 *
 * The narrow internal surface that several REPL TUs and tests share:
 *
 *   - the parse-and-normalize entry points (the commit pipeline's front door),
 *   - the parse / extract / canonical-text helpers from text_helpers.h, and
 *   - visible-variable collection from visible_vars.h used by parse callers.
 *
 * It owns none of scene loading, export, editor-input shims, or controller
 * hooks; those live behind their own headers (repl/scenes.h, repl/export.h,
 * src/editor/input.h, ...) which callers include directly when they need them.
 */
#ifndef REPL_CORE_INTERNAL_H
#define REPL_CORE_INTERNAL_H

#include "repl/command.h"      /* GLCmd */
#include "repl/eval.h"         /* ExprVar */
#include "repl/text_helpers.h" /* parse / extract / canonical-text helpers */
#include "repl/visible_vars.h" /* visible-variable collection */
#include "source_document.h"   /* SourceTextView document view */

/* ---- Normalize / commit pipeline -------------------------------------- */

/* Parse `line` into `out_cmd` and optionally write the canonical (normalized)
 * source text into text_out (capacity text_sz; pass NULL/0 to ignore).
 * `preserve_expr` keeps the raw argument expressions instead of re-emitting
 * them from evaluated values. */
int  repl_parse_and_normalize(const char *line, int pos,
                              ExprVar *vars, int num_vars,
                              int preserve_expr, GLCmd *out_cmd,
                              char *text_out, int text_sz);

/* Same as repl_parse_and_normalize() but rejects top-level CMD_CALL
 * whose target funcN has no matching CMD_FUNC_DEF.  Used by the commit
 * path so undefined calls surface at typing time like undeclared
 * variables do; reformat/flatten/test paths keep the permissive
 * variant above. */
int  repl_parse_and_normalize_strict(const char *line, int pos,
                                     ExprVar *vars, int num_vars,
                                     int preserve_expr, GLCmd *out_cmd,
                                     char *text_out, int text_sz);

#endif /* REPL_CORE_INTERNAL_H */
