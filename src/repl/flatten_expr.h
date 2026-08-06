/*
 * src/repl/flatten_expr.h -- Internal expression engine for flatten.c.
 *
 * This boundary contains every flatten-facing detail of the compiled
 * expression cache: per-line build state, capture sinks, warm evaluation,
 * dependency propagation, and rebake lookups.  The source-expansion walk
 * deals in expression roles and values; it never manipulates cache entries
 * or program handles directly.
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
    ReplExprDepMask     predef_deps[MAX_PREDEF_VARS];
    ReplExprDepMask     value_dep_mask;
    ReplExprDepMask     structural_dep_mask;
    int                 rebake_ok;
} ReplFlattenExprEngine;

typedef struct {
    float           value;
    ReplExprDepMask deps;
    int             found;
    int             used_local;
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
int  repl_flatten_expr_capture_span(ReplFlattenExprEngine *engine,
                                    ReplExprRole role, int ordinal,
                                    const char *begin, const char *end);
int  repl_flatten_expr_compile_active_list(ReplFlattenExprEngine *engine,
                                           ReplExprRole role, int base_ordinal,
                                           const char *text, int strict,
                                           int max_programs);

int repl_flatten_expr_role_count(const ReplFlattenExprEngine *engine,
                                 int line_idx, ReplExprRole role);

/* Assignment destination name for a source line, memoised per line in the
 * cache (repl_expr_cache_line_lhs_*). Returns 1 with `out` filled when the
 * row is a scalar assignment, 0 when it is not (`out` emptied). Derives and
 * memoises on the first visit; without a cache - and on the force_text
 * differential path, which stays independent of it by construction - every
 * visit derives. */
int repl_flatten_expr_assign_lhs(ReplFlattenExprEngine *engine, int line_idx,
                                 const char *src_text, char *out, int out_sz);
ReplFlattenExprValue repl_flatten_expr_eval(
    const ReplFlattenExprEngine *engine, int line_idx,
    ReplExprRole role, int ordinal,
    const ExprVar *locals, const ReplExprDepMask *local_deps,
    int num_locals, int structural);
ReplExprDepMask repl_flatten_expr_deps(
    const ReplFlattenExprEngine *engine, int line_idx,
    ReplExprRole role, int ordinal,
    const ExprVar *locals, const ReplExprDepMask *local_deps,
    int num_locals, int structural, int missing_ok);
ReplExprDepMask repl_flatten_expr_header_value(
    const ReplFlattenExprEngine *engine, int line_idx,
    ReplExprRole role, const ExprVar *locals,
    const ReplExprDepMask *local_deps, int num_locals,
    float *value_inout, int missing_all_bits);

void repl_flatten_expr_note_value(ReplFlattenExprEngine *engine,
                                  ReplExprDepMask deps);
void repl_flatten_expr_note_structural(ReplFlattenExprEngine *engine,
                                       ReplExprDepMask deps);
void repl_flatten_expr_set_predef_deps(ReplFlattenExprEngine *engine,
                                       int slot, ReplExprDepMask deps);
void repl_flatten_expr_note_emitted(ReplFlattenExprEngine *engine,
                                    int has_vars, int line_idx);
ReplExprDepMask repl_flatten_expr_value_deps(
    const ReplFlattenExprEngine *engine);
ReplExprDepMask repl_flatten_expr_structural_deps(
    const ReplFlattenExprEngine *engine);
int repl_flatten_expr_rebake_ok(const ReplFlattenExprEngine *engine);

/* Compiled-only lookup used by the in-place rebake walk. */
int repl_flatten_expr_rebake_line_ready(const ReplExprCache *cache,
                                        int line_idx);
int repl_flatten_expr_rebake_eval(const ReplExprCache *cache, int line_idx,
                                  ReplExprRole role, int ordinal,
                                  const ExprVar *locals, int num_locals,
                                  float *value_out);

#endif /* REPL_FLATTEN_EXPR_H */
