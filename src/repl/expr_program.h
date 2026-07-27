/*
 * src/repl/expr_program.h - Compiled expression programs and the line cache.
 *
 * Postfix compiler/evaluator for the REPL expression grammar (the exact
 * grammar of repl_eval_expr in src/repl/eval.c), plus the per-source-line
 * cache the flatten pipeline reads on its warm path
 * (docs/plans/in-review/flatten-performance-without-vm.md, Phase 2).
 *
 * This is not a control-flow VM: flatten still owns loops, calls, branches,
 * assignments, output materialization, and resource budgets. A program here
 * is one expression — a GL-command argument, a loop-header bound, an
 * if-condition, a call argument, or an assignment RHS/index — compiled once
 * and evaluated many times.
 *
 * Parity contract: evaluation performs the same float operations in the same
 * order as the recursive-descent text evaluator (same builtin function
 * pointers, same guarded divide/modulo, same epsilon compares), so compiled
 * and text results are bit-identical (NaN compares as the NaN parity case).
 * Anything the compiler cannot express fails compilation, and the caller
 * keeps using the text evaluator for that expression — behavior differences
 * are only ever "fall back", never "diverge".
 *
 * The cache is ephemeral REPL-owned state: never captured into undo
 * snapshots, user scenes, or workspace files. It is invalidated wholesale
 * from the source-mutation seam (repl_state_mark_source_dirty) and rebuilt
 * lazily per source line on the next full flatten. Heap use is capped by
 * REPL_EXPR_CACHE_MAX_BYTES; on allocation/cap/compile failure the affected
 * line simply stays on the text path. Warm evaluation performs no allocation.
 *
 * Dependency masks (Phase 3): every evaluation also returns the set of
 * predefined-variable roots the value depends on, as one bit per predef
 * slot. Constants contribute no bits; a predef read contributes its current
 * dependency mask (supplied by the caller, so assignment dataflow like
 * `n = t * 2` can thread through); a local read contributes the binding's
 * mask; operators and builtins union their operand masks. rand/rand2 add no
 * dependency beyond their arguments.
 */
#ifndef REPL_EXPR_PROGRAM_H
#define REPL_EXPR_PROGRAM_H

#include <stddef.h>

#include "repl/eval.h"
#include "c_compat.h"

/* One bit per predefined-variable slot. */
typedef unsigned int ReplExprDepMask;

STATIC_ASSERT(MAX_PREDEF_VARS <= (int)(sizeof(ReplExprDepMask) * 8),
              repl_expr_dep_mask_must_cover_all_predef_slots);

#define REPL_EXPR_DEP_ALL (~(ReplExprDepMask)0)

/* Value + dependency result of one compiled evaluation. */
typedef struct {
    float           value;
    ReplExprDepMask deps;
} ReplExprValue;

/* The expression-role and capture-sink contract (ReplExprRole,
 * ReplExprCaptureSink) lives in repl/eval.h: the capture points sit inside
 * the shared parse helpers (parser argument lists, the for-header parser,
 * the if-condition kernel), which must not depend on this module. */

/* Total live heap budget for one cache (all arenas together). Exceeding it
 * fails the current compile; the line stays text-evaluated. */
#ifndef REPL_EXPR_CACHE_MAX_BYTES
#define REPL_EXPR_CACHE_MAX_BYTES (16u * 1024u * 1024u)
#endif

/* Opaque cache handle. One live instance backs the REPL pipeline
 * (repl_expr_cache_live); tests can create private instances. */
typedef struct ReplExprCache ReplExprCache;

/* Per-line cache lifecycle. EMPTY lines have never been built since the
 * last invalidation; READY lines carry compiled programs; FAILED lines hit
 * a compile/capture/allocation failure and stay on the text path until the
 * next invalidation. */
typedef enum {
    REPL_EXPR_LINE_EMPTY = 0,
    REPL_EXPR_LINE_READY,
    REPL_EXPR_LINE_FAILED
} ReplExprLineState;

/* Evaluation environment. `locals` are the active flatten bindings (loop
 * counters / function params), resolved by name, checked before predefs —
 * same shadowing order as the text evaluator. `local_deps` optionally
 * carries one dependency mask per local binding (NULL: locals contribute no
 * bits). `predef_vars` is the predefined-variable table the values are read
 * from; `predef_deps` optionally carries the current per-slot dependency
 * masks (NULL: slot i contributes 1u << i). `used_local` is an output flag:
 * set to 1 when any identifier resolved to a local binding (the compiled
 * twin of input_has_expr_vars for has_vars bookkeeping). */
typedef struct {
    const ExprVar         *locals;
    const ReplExprDepMask *local_deps;
    int                    num_locals;
    const ExprVar         *predef_vars;
    const ReplExprDepMask *predef_deps;
    int                    predef_count;
    int                    used_local;   /* out */
} ReplExprEvalEnv;

/* ---- Cache lifecycle ---------------------------------------------------- */

ReplExprCache *repl_expr_cache_live(void);
ReplExprCache *repl_expr_cache_create(void);
void           repl_expr_cache_destroy(ReplExprCache *cache);
/* Drop every compiled program and line entry (arena capacity is retained so
 * a rebuild does not re-grow from zero). The single invalidation entry —
 * called from repl_state_mark_source_dirty and state restore. */
void           repl_expr_cache_invalidate(ReplExprCache *cache);

/* ---- Compilation -------------------------------------------------------- */

/* Compile-time bindings. Predef slots are resolved at compile time
 * (declare/undeclare invalidates the cache, so resolved slots cannot go
 * stale). `locals` are the flatten bindings visible where the expression
 * lives; they are used only for a shadow check — the text evaluator resolves
 * locals before builtin function calls, so an expression whose call target
 * name is shadowed by a local binding fails compilation instead of
 * mis-compiling to the builtin. Local *values* are never baked in; identifier
 * reads resolve locals by name at evaluation time. Binding names at one
 * source position are fixed (loop iterator / function parameter names), so a
 * check against the first visit's names holds for every later visit. */
typedef struct {
    ReplPredefView predef;
    const ExprVar *locals;
    int            num_locals;
} ReplExprCompileEnv;

/* Compile one expression (NUL-terminated / [begin,end) span). Returns a
 * program handle >= 0, or -1 on any unsupported construct or resource
 * failure. Compilation consumes input exactly as far as the text evaluator
 * would; trailing text beyond one expression is ignored. */
int repl_expr_cache_compile(ReplExprCache *cache, const char *src,
                            const ReplExprCompileEnv *env);
int repl_expr_cache_compile_span(ReplExprCache *cache,
                                 const char *begin, const char *end,
                                 const ReplExprCompileEnv *env);

/* Compile a comma-separated expression list. `strict` selects which text
 * splitter the list must mirror: nonzero follows parse_expr_list_exact
 * (whole input must be a well-formed list; returns -1 on any violation),
 * zero follows repl_eval_parse_exprs (lenient: stops at ')' / end / no
 * progress). On success writes up to `max` program handles into prog_out
 * and returns the count (0..max). Returns -1 on failure (strict violation,
 * a sub-expression that fails to compile, or more than `max` entries). */
int repl_expr_cache_compile_list(ReplExprCache *cache, const char *src,
                                 int strict, int max, int *prog_out,
                                 const ReplExprCompileEnv *env);

/* 1 when the program contains at least one scratch-array read. Structural
 * users (loop headers, conditions, call args) treat that as "depends on
 * everything" until scratch cells carry persistent dependency metadata. */
int repl_expr_program_has_scratch_read(const ReplExprCache *cache, int prog);

/* ---- Evaluation --------------------------------------------------------- */

/* Evaluate a compiled program. env is in/out (used_local). Never allocates.
 * A stale/invalid handle returns { 0.0f, REPL_EXPR_DEP_ALL }. */
ReplExprValue repl_expr_program_eval(const ReplExprCache *cache, int prog,
                                     ReplExprEvalEnv *env);

/* ---- Per-line entries (the flatten-facing index) ------------------------ */

ReplExprLineState repl_expr_cache_line_state(const ReplExprCache *cache,
                                             int line_idx);
/* Open a build for line_idx (state -> building; previous programs for the
 * line are dropped from the index). Returns 1 on success, 0 on allocation
 * failure (the line is marked FAILED). */
int  repl_expr_cache_line_begin(ReplExprCache *cache, int line_idx);
/* Attach a compiled program to the line being built. */
int  repl_expr_cache_line_add(ReplExprCache *cache, int line_idx,
                              ReplExprRole role, int ordinal, int prog);
/* Close the build: ok != 0 marks the line READY, ok == 0 marks it FAILED. */
void repl_expr_cache_line_finish(ReplExprCache *cache, int line_idx, int ok);
/* Program handle for (line, role, ordinal), or -1. */
int  repl_expr_cache_line_find(const ReplExprCache *cache, int line_idx,
                               ReplExprRole role, int ordinal);
/* Number of programs recorded for (line, role). */
int  repl_expr_cache_line_role_count(const ReplExprCache *cache, int line_idx,
                                     ReplExprRole role);

/* ---- Per-line assignment-LHS memo --------------------------------------- */
/*
 * The destination name of `x = expr` is a pure function of the row's text,
 * and flatten re-derives it on every visit of every assignment (a persisted
 * var_idx must not be able to defeat a later legal shadow — see
 * flatten_resolve_assign_target). Memoising it here buys that back for free:
 * the memo inherits this cache's single invalidation seam, and
 * repl_state_mark_source_dirty's contract is that every source mutation goes
 * through it.
 *
 * Deliberately independent of the line's program state. The LHS is well
 * defined on an EMPTY line (first visit), on a FAILED one, and on a line
 * whose RHS never compiled — none of which the program index can represent.
 * The memo also stores "this row has no scalar assignment target" so a
 * non-assignment row is parsed at most once.
 *
 * Get returns 1 (memoised, name copied to `out`), 0 (memoised as "no LHS",
 * `out` emptied), or -1 (not memoised — the caller must derive and set it).
 * Set with lhs == NULL or "" records the "no LHS" verdict. Storage failure
 * is silent: the line stays unmemoised and the caller keeps deriving it. */
int  repl_expr_cache_line_lhs_get(const ReplExprCache *cache, int line_idx,
                                  char *out, int out_sz);
void repl_expr_cache_line_lhs_set(ReplExprCache *cache, int line_idx,
                                  const char *lhs);

#endif /* REPL_EXPR_PROGRAM_H */
