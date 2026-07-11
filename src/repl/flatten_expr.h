/*
 * src/repl/flatten_expr.h -- Internal compiled-expression boundary for
 * source flattening.
 *
 * flatten.c owns the source/control-flow walk. This module owns every
 * flatten-facing cache detail: line lifecycle, capture-time compilation,
 * program lookup, and warm evaluation.
 */
#ifndef REPL_FLATTEN_EXPR_H
#define REPL_FLATTEN_EXPR_H

#include "repl/expr_program.h"

typedef struct {
    ReplExprCache      *cache;
    int                 force_text;
    int                 build_line;
    int                 build_failed;
    ReplExprCompileEnv  build_env;
} ReplFlattenExprEngine;

typedef struct {
    float value;
    int   found;
    int   used_local;
} ReplFlattenExprValue;

void repl_flatten_expr_init(ReplFlattenExprEngine *engine,
                            ReplExprCache *cache, int force_text);
int  repl_flatten_expr_line_ready(const ReplFlattenExprEngine *engine,
                                  int line_idx);
int  repl_flatten_expr_build_begin(ReplFlattenExprEngine *engine,
                                   int line_idx,
                                   const ExprVar *locals, int num_locals);
void repl_flatten_expr_build_finish(ReplFlattenExprEngine *engine,
                                    int line_idx, int parsed_ok);
ReplExprCaptureSink repl_flatten_expr_capture_sink(
    ReplFlattenExprEngine *engine);
int repl_flatten_expr_capture_span(ReplFlattenExprEngine *engine,
                                   ReplExprRole role, int ordinal,
                                   const char *begin, const char *end);
int repl_flatten_expr_compile_active_list(ReplFlattenExprEngine *engine,
                                          ReplExprRole role,
                                          int base_ordinal,
                                          const char *text, int strict,
                                          int max_programs);
int repl_flatten_expr_role_count(const ReplFlattenExprEngine *engine,
                                 int line_idx, ReplExprRole role);
ReplFlattenExprValue repl_flatten_expr_eval(
    const ReplFlattenExprEngine *engine, int line_idx,
    ReplExprRole role, int ordinal,
    const ExprVar *locals, int num_locals);

#endif /* REPL_FLATTEN_EXPR_H */
