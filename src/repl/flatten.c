/*
 * src/repl/flatten.c -- Source-to-flat command expansion.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "repl/host_effects.h"
#include "repl/pipeline.h"
#include "source_document.h"
#include "repl/color_limits.h"   /* REPL_CLEAR_COLOR_MAX_V (compiled-path clamp) */
#include "repl/stencil_limits.h"
#include "repl/command.h"
#include "repl/command_spec.h"
#include "repl/eval.h"
#include "repl/text_helpers.h"
#include "repl/flatten.h"
#include "repl/flatten_expr.h"
#include "repl/flatten_query.h"
#include "repl/parser.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"
#include "repl/util.h"            /* repl_format_fits / repl_copy_string_fits */
#include "repl/visible_vars.h"    /* ReplVisibleVarKind - the shared binding tag */
#include "support/cpuprof.h"   /* PROF_FLATTEN_* sub-phase timing */
#include "config.h"        /* REPL_STATUS_TEXT_MAX */

#define MAX_FLATTEN_CALL_DEPTH 64
#define MAX_FLATTEN_VISIT_BUDGET 200000
/* Per-for-loop hard stop on unrolled iterations, separate from the
 * whole-program MAX_FLATTEN_VISIT_BUDGET above: a single runaway loop
 * bails at this many iterations even if the global budget has room. */
#define MAX_FLATTEN_LOOP_ITERS 100000
/* Bit width of GLCmd.func_scope_mask (an unsigned int). A function slot
 * index must be < this to be representable as `1u << slot` in the mask. */
#define FUNC_SCOPE_MASK_BITS 32

/* The display controller can issue one refresh at frame top and another for
 * every accumulation-time-blur sample. Keep that frame aggregation behind
 * the live refresh boundary: export/debug/replay callers still publish their
 * one refresh immediately, while the frame commits one total after its last
 * subframe. The flags also keep the existing full-flatten child rows aligned
 * with their now-accumulated parent. */
typedef struct {
    int active;
    int flatten_sampled;
    int rebake_sampled;
} FlatRefreshProfileFrame;

static FlatRefreshProfileFrame g_flat_refresh_profile_frame;

static void flat_refresh_profile_end(ProfSection section) {
    if (g_flat_refresh_profile_frame.active)
        prof_accum_end(section);
    else
        prof_end(section);
}

void repl_flat_refresh_profile_frame_begin(void) {
    g_flat_refresh_profile_frame.active = 1;
    g_flat_refresh_profile_frame.flatten_sampled = 0;
    g_flat_refresh_profile_frame.rebake_sampled = 0;

    prof_accum_reset(PROF_FLATTEN);
    prof_accum_reset(PROF_FLATTEN_REPARSE);
    prof_accum_reset(PROF_FLATTEN_VAR_ASSIGN);
    prof_accum_reset(PROF_FLATTEN_SCRATCH_ASSIGN);
    prof_accum_reset(PROF_REBAKE);
    prof_accum_reset(PROF_REBAKE_EVAL);
}

void repl_flat_refresh_profile_frame_end(void) {
    if (!g_flat_refresh_profile_frame.active)
        return;

    /* Optional refresh sections preserve their last sample and naturally
     * become stale on frames where they do not run. Do not publish an empty
     * zero merely because the display frame opened an accumulation scope. */
    if (g_flat_refresh_profile_frame.flatten_sampled) {
        prof_accum_commit(PROF_FLATTEN);
        prof_accum_commit(PROF_FLATTEN_REPARSE);
        prof_accum_commit(PROF_FLATTEN_VAR_ASSIGN);
        prof_accum_commit(PROF_FLATTEN_SCRATCH_ASSIGN);
    }
    if (g_flat_refresh_profile_frame.rebake_sampled) {
        prof_accum_commit(PROF_REBAKE);
        prof_accum_commit(PROF_REBAKE_EVAL);
    }
    g_flat_refresh_profile_frame.active = 0;
}

/* Read the source text for command index `i` through the editor
 * buffer view threaded into the flatten context, falling back to ""
 * if unset. The view is supplied by `ReplFlattenOptions.text`. */
static const char *flatten_src_text(SourceTextView text, int i) {
    const char *line = source_text_line(text, i);
    return (line && line[0]) ? line : "";
}

typedef struct {
    const GLCmd      *source_cmds;
    int               source_count;
    GLCmd            *flat_cmds;
    FlatCmdLocalVars *flat_local_vars;
    int               flat_capacity;
    int               flat_count;
    int               capacity_exceeded;
    SourceTextView  text;             /* editor source-text view for inline expansion */
    ReplFuncAliasView func_aliases;
    ReplSourceScopeView source_scope;
    int               max_call_depth;
    int               force_reparse;    /* test seam: skip the literal fast path */
    /* Narrow boundary around compiled caching and dependency propagation.
     * The expansion walk sees semantic roles/values, never cache entries or
     * program handles. */
    ReplFlattenExprEngine expr;
    int call_depth;
    int abort;
    int visit_budget;
    char status[REPL_DIAG_TEXT_MAX];
    /* funcN -> source_cmds[] index of the matching CMD_FUNC_DEF, or -1.
     * Built once in repl_flatten_program; CMD_CALL handlers index directly
     * instead of walking the source for each call. */
    int func_def_idx[REPL_FUNC_SLOT_COUNT];
} FlattenContext;

static void flatten_note_status(FlattenContext *ctx, const char *msg) {
    if (!ctx || !msg || ctx->status[0])
        return;
    repl_copy_string_fits(ctx->status, sizeof(ctx->status), msg);
}

static void flatten_fail(FlattenContext *ctx, const char *msg) {
    flatten_note_status(ctx, msg);
    ctx->abort = 1;
}

static void flatten_get_for_var_name(SourceTextView text,
                                     int cmd_idx,
                                     char *var, int var_sz) {
    const char *p = flatten_src_text(text, cmd_idx);
    while (*p && *p != '(') p++;
    if (*p) p++;
    while (*p && isspace((unsigned char)*p)) p++;
    int i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && i < var_sz - 1)
        var[i++] = *p++;
    var[i] = '\0';
}

/* Tag a flat command with its origin so cursor-highlighting, replay, and
 * debug dumps can trace each expanded command back to:
 *   src_cmd_idx          -- the repl_state_document_cmds()[] line this command came from
 *   call_src_cmd_idx     -- the funcN() call site that triggered expansion
 *                          (-1 if top-level)
 *   root_call_src_cmd_idx-- the outermost call site in nested func calls
 *                          (-1 if top-level)
 *   func_scope_mask      -- bitmask of which func bodies this cmd is inside
 *   call_depth           -- funcN call-frame nesting/recursion depth (0 = top) */
static void flat_cmd_set_provenance(GLCmd *cmd, int src_cmd_idx,
                                    int call_src_cmd_idx,
                                    int root_call_src_cmd_idx,
                                    unsigned int func_scope_mask,
                                    int call_depth) {
    cmd->src_cmd_idx = src_cmd_idx;
    cmd->call_src_cmd_idx = call_src_cmd_idx;
    cmd->root_call_src_cmd_idx = root_call_src_cmd_idx;
    cmd->func_scope_mask = func_scope_mask;
    cmd->call_depth = call_depth;
}

static int flatten_repl_source_scope_find_block_end(const FlattenContext *ctx, int begin_idx) {
    int depth = 1;

    for (int j = begin_idx + 1; j < ctx->source_count; j++) {
        CmdType t = ctx->source_cmds[j].type;
        if (repl_cmd_is_block_head(t))
            depth++;
        else if (repl_cmd_is_block_end(t)) {
            depth--;
            if (depth == 0)
                return j;
        }
    }
    return ctx->source_count;
}

static int flatten_append_cmd(FlattenContext *ctx, const GLCmd *cmd,
                              int src_cmd_idx, int call_src_cmd_idx,
                              int root_call_src_cmd_idx,
                              unsigned int func_scope_mask,
                              const ExprVar *vars, int num_vars) {
    int flat_cmd_idx;
    int snap_count = 0;

    /* Once the destination fills, keep counting by reusing it as a circular
     * scratch buffer. A capacity overflow invalidates the whole flattened
     * program, so preserving its command order no longer matters; continuing
     * lets the caller report the exact capacity the program needs. Other
     * flatten failures still abort immediately, and the visit budget keeps
     * this diagnostic pass bounded. */
    flat_cmd_idx = ctx->flat_capacity > 0
                 ? ctx->flat_count % ctx->flat_capacity
                 : 0;
    ctx->flat_count++;
    if (ctx->flat_count > ctx->flat_capacity)
        ctx->capacity_exceeded = 1;
    if (ctx->flat_capacity <= 0)
        return 1;

    ctx->flat_cmds[flat_cmd_idx] = *cmd;
    /* glMultMatrixf(A) resolves here rather than in the executor: scratch
     * writes are applied by flatten in stream order (flatten_scratch_assign
     * calls repl_eval_scratch_set), so the array holds exactly what the
     * lines above this one put there. Baking the 16 cells onto the flat
     * command makes it self-contained for every later walker — including
     * the ones in render3d, which cannot call into the scratch table.
     * The compound-literal form has no array to read: its payload arrives
     * already filled by the parse (or by the expression-slot refresh in
     * flatten_reparse_line), and reading array 0 here would overwrite it. */
    if (cmd->type == CMD_MULT_MATRIXF && repl_cmd_mult_matrix_from_array(cmd)) {
        int array_idx = (int)cmd->args[0];
        for (int k = 0; k < REPL_MATRIX_CELL_COUNT && k < REPL_SCRATCH_ARRAY_LEN; k++)
            repl_eval_scratch_get(array_idx, k,
                                  &ctx->flat_cmds[flat_cmd_idx].payload.matrix.m[k]);
    }
    flat_cmd_set_provenance(&ctx->flat_cmds[flat_cmd_idx],
                            src_cmd_idx, call_src_cmd_idx,
                            root_call_src_cmd_idx, func_scope_mask,
                            ctx->call_depth);

    if (ctx->flat_local_vars) {
        if (vars && num_vars > 0)
            snap_count = num_vars < MAX_EXPR_VARS ? num_vars : MAX_EXPR_VARS;
        ctx->flat_local_vars[flat_cmd_idx].num_vars = snap_count;
        if (snap_count > 0)
            memcpy(ctx->flat_local_vars[flat_cmd_idx].vars, vars,
                   (size_t)snap_count * sizeof(ExprVar));
    }
    return 1;
}

/* The scope array is threaded as three parallel arrays plus a count:
 * `vars` (name + value), `var_deps` (dep mask per binding) and `var_kinds`
 * (what binds it). The kinds are not diagnostics — assignment resolves its
 * target against this array and only a LOCAL is writable, so the tag is
 * what keeps a parameter or a loop iterator constant even when it shadows
 * a writable outer binding. `var_kinds` may be NULL only when nv == 0.
 *
 * `var_deps` is non-const because a local assignment writes its slot;
 * flatten_for_loop then copies the outer entries back out of its
 * per-iteration array. */
static void flatten_range(FlattenContext *ctx,
                          int start, int end_idx,
                          ExprVar *vars, ReplExprDepMask *var_deps,
                          const ReplVisibleVarKind *var_kinds, int nv,
                          int call_src_cmd_idx, int root_call_src_cmd_idx,
                          unsigned int func_scope_mask);

static void flatten_for_loop(FlattenContext *ctx,
                             const GLCmd *src_cmd, int i,
                             ExprVar *vars, ReplExprDepMask *var_deps,
                             const ReplVisibleVarKind *var_kinds, int nv,
                             int call_src_cmd_idx, int root_call_src_cmd_idx,
                             unsigned int func_scope_mask,
                             int loop_end) {
    char var_name[REPL_PREDEF_NAME_MAX];
    float start_val = src_cmd->args[0];
    float end_val   = src_cmd->args[1];
    float step_val  = src_cmd->args[2];
    /* The header bounds decide the unrolled iteration count, so their deps
     * are structural, and the iterator local conservatively carries their
     * union. A constant header contributes nothing. */
    ReplExprDepMask header_deps = 0;
    const char *src_text = flatten_src_text(ctx->text, i);
    flatten_get_for_var_name(ctx->text, i, var_name, sizeof(var_name));

    if (src_cmd->has_vars) {
        if (repl_flatten_expr_line_ready(&ctx->expr, i)) {
            /* Warm path: evaluate the compiled header bounds, taking each
             * bound's deps from the SAME eval (no second dep-only pass).
             * An absent start/end program on a READY line widens to all
             * bits (structural uncertainty); an absent step program means
             * the source omitted it — the committed args[2] already bakes
             * the 1.0 default, so it contributes no deps. */
            header_deps |= repl_flatten_expr_header_value(
                &ctx->expr, i, REPL_EXPR_ROLE_LOOP_START, vars, var_deps, nv,
                &start_val, /*missing_all_bits=*/1);
            header_deps |= repl_flatten_expr_header_value(
                &ctx->expr, i, REPL_EXPR_ROLE_LOOP_END, vars, var_deps, nv,
                &end_val, /*missing_all_bits=*/1);
            header_deps |= repl_flatten_expr_header_value(
                &ctx->expr, i, REPL_EXPR_ROLE_LOOP_STEP, vars, var_deps, nv,
                &step_val, /*missing_all_bits=*/0);
        } else {
            const char *unused_body;
            float re_start, re_end, re_step;
            char rv[16];
            int building = repl_flatten_expr_build_begin(&ctx->expr, i,
                                                         vars, nv);
            ReplExprCaptureSink sink =
                repl_flatten_expr_capture_sink(&ctx->expr);
            int parsed = repl_eval_parse_for_header(
                &(ReplForHeaderParseConfig){
                    .input = src_text,
                    .var_name = rv,
                    .var_sz = (int)sizeof(rv),
                    .start = &re_start,
                    .end = &re_end,
                    .step = &re_step,
                    .body_start = &unused_body,
                    .vars = vars,
                    .num_vars = nv,
                    .capture = building ? &sink : NULL,
                });
            if (building)
                repl_flatten_expr_build_finish(&ctx->expr, i, parsed);
            if (parsed) {
                start_val = re_start;
                end_val   = re_end;
                step_val  = re_step;
            }
            /* Build/FAILED visit: one dep-only pass over the just-built
             * programs (start/end required, step's absence baked; uncached
             * headers widen to all bits). */
            header_deps |= repl_flatten_expr_deps(
                &ctx->expr, i, REPL_EXPR_ROLE_LOOP_START, 0, vars, var_deps,
                nv, /*structural=*/1, /*missing_ok=*/0);
            header_deps |= repl_flatten_expr_deps(
                &ctx->expr, i, REPL_EXPR_ROLE_LOOP_END, 0, vars, var_deps,
                nv, 1, 0);
            header_deps |= repl_flatten_expr_deps(
                &ctx->expr, i, REPL_EXPR_ROLE_LOOP_STEP, 0, vars, var_deps,
                nv, 1, /*missing_ok=*/1);
        }
        repl_flatten_expr_note_structural(&ctx->expr, header_deps);
    }

    if (fabsf(step_val) < 1e-9f ||
        (step_val > 0 && start_val >= end_val) ||
        (step_val < 0 && start_val <= end_val))
        return;

    int max_iters = MAX_FLATTEN_LOOP_ITERS;
    for (float val = start_val;
         (step_val > 0) ? (val < end_val - 1e-6f) : (val > end_val + 1e-6f);
         val += step_val) {
        if (--max_iters < 0) break;
        if (ctx->abort) return;
        ExprVar lvars[MAX_EXPR_VARS];
        ReplExprDepMask ldeps[MAX_EXPR_VARS];
        ReplVisibleVarKind lkinds[MAX_EXPR_VARS];
        int lnv = 0;
        int outer_n = 0;
        repl_copy_string_fits(lvars[lnv].name,
                              sizeof(lvars[lnv].name),
                              var_name);
        lvars[lnv].value = val;
        ldeps[lnv] = header_deps;
        lkinds[lnv] = REPL_VISIBLE_VAR_LOOP;
        lnv++;
        if (vars)
            for (int v = 0; v < nv && lnv < MAX_EXPR_VARS; v++) {
                ldeps[lnv] = var_deps ? var_deps[v] : 0;
                lkinds[lnv] = var_kinds ? var_kinds[v] : REPL_VISIBLE_VAR_LOOP;
                lvars[lnv++] = vars[v];
                outer_n++;
            }
        flatten_range(ctx, i + 1, loop_end, lvars, ldeps, lkinds, lnv,
                      call_src_cmd_idx, root_call_src_cmd_idx,
                      func_scope_mask);
        /* Copy the outer bindings back. `lvars` is rebuilt per iteration,
         * so without this `float acc; acc = 0; for(i,0,n){ acc = acc+i; }`
         * would silently reset every pass. Index 0 is the iterator, which
         * does not survive the body. flatten_if_block needs no equivalent
         * (it shares the caller's arrays) and flatten_call deliberately
         * does not copy back — a callee frame is not the caller's. */
        for (int v = 0; v < outer_n; v++) {
            vars[v] = lvars[1 + v];
            if (var_deps)
                var_deps[v] = ldeps[1 + v];
        }
    }
}

/* Append a callee's declaration prologue to its fresh call frame, each
 * local bound to 0.0f with dep mask 0.
 *
 * The *whole body* is scanned, not a leading prologue run. Commit hoists
 * declarations to the body top, but nothing keeps them there: an insert
 * in the middle of the prologue drops a statement ahead of them, and
 * replacing an unused leading declaration does the same. A scan that
 * stopped at the first non-declaration would silently unbind every local
 * after that point — its readers would then resolve to a same-named
 * global, or fail to reparse and vanish from the flat stream, with no
 * diagnostic anywhere. Row position is a formatting convention; the
 * binding must not depend on it.
 *
 * This also keeps flatten in step with compile: collect_visible_vars_in()
 * already binds a local declaration into the enclosing CMD_FUNC_DEF frame
 * wherever in the body it appears, so a prologue-only scan here made the
 * two disagree about what is in scope.
 *
 * Nested function bodies are a different lexical scope and are skipped
 * whole. The cost is one cheap type-check pass over the body per call,
 * against a flatten_range walk of the same rows that reparses and
 * evaluates every one of them. */
static void flatten_bind_func_locals(FlattenContext *ctx,
                                     int body_start, int body_end,
                                     ExprVar *lvars, ReplExprDepMask *ldeps,
                                     ReplVisibleVarKind *lkinds, int *lnv) {
    for (int j = body_start;
         j >= 0 && j < body_end && j < ctx->source_count; j++) {
        const GLCmd *decl = &ctx->source_cmds[j];
        if (decl->type == CMD_FUNC_DEF) {
            j = flatten_repl_source_scope_find_block_end(ctx, j);
            continue;
        }
        if (!decl->valid || decl->type != CMD_VAR_DECLARE ||
            decl->var_idx != REPL_VAR_IDX_LOCAL)
            continue;
        for (int n = 0; n < decl->payload.decl.count &&
                        *lnv < MAX_EXPR_VARS; n++) {
            repl_copy_string_fits(lvars[*lnv].name, sizeof(lvars[*lnv].name),
                                  decl->payload.decl.names[n]);
            lvars[*lnv].value = 0.0f;
            ldeps[*lnv] = 0;
            lkinds[*lnv] = REPL_VISIBLE_VAR_LOCAL;
            (*lnv)++;
        }
    }
}

static void flatten_call(FlattenContext *ctx,
                         const GLCmd *src_cmd, int i,
                         ExprVar *vars, const ReplExprDepMask *var_deps,
                         int nv,
                         int root_call_src_cmd_idx,
                         unsigned int func_scope_mask) {
    int func_num = (int)src_cmd->args[0];
    if (ctx->call_depth >= ctx->max_call_depth) {
        char msg[REPL_DIAG_TEXT_MAX];
        snprintf(msg, sizeof(msg),
                 "Recursive expansion exceeded depth limit (%d) at func%d",
                 ctx->max_call_depth, func_num);
        flatten_fail(ctx, msg);
        return;
    }

    int k = (func_num >= 0 && func_num < REPL_FUNC_SLOT_COUNT)
            ? ctx->func_def_idx[func_num]
            : -1;
    if (k < 0) {
        char msg[64];
        snprintf(msg, sizeof(msg),
                 "Error: func%d not defined", func_num);
        flatten_note_status(ctx, msg);
        return;
    }
    do {
        int body_end = flatten_repl_source_scope_find_block_end(ctx, k);
        int def_fn = func_num;
        int param_count = 0;
        char param_names[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
        char arg_text[MAX_LINE_LEN];
        float arg_vals[MAX_EXPR_VARS];
        ReplExprDepMask arg_deps[MAX_EXPR_VARS];
        int arg_count = 0;
        const char *def_text = flatten_src_text(ctx->text, k);
        const char *call_text = flatten_src_text(ctx->text, i);

        int warm_call = 0;
        if (!parse_repl_func_signature(def_text, &def_fn,
                                       param_names, MAX_EXPR_VARS,
                                       &param_count))
            break;
        warm_call = repl_flatten_expr_line_ready(&ctx->expr, i);
        if (warm_call) {
            /* Warm path: evaluate the compiled call arguments (one eval per
             * arg, deps collected from the same pass). The count was frozen
             * when the line built; the param-count check below still runs
             * against the current definition. */
            arg_count = repl_flatten_expr_role_count(
                &ctx->expr, i, REPL_EXPR_ROLE_CALL_ARG);
            for (int a = 0; a < arg_count && a < MAX_EXPR_VARS; a++) {
                ReplFlattenExprValue v = repl_flatten_expr_eval(
                    &ctx->expr, i, REPL_EXPR_ROLE_CALL_ARG, a, vars,
                    var_deps, nv, /*structural=*/1);
                arg_vals[a] = v.value;
                arg_deps[a] = v.deps;
            }
        } else {
            int building;
            if (!extract_func_call_args_text(call_text, NULL,
                                             arg_text, sizeof(arg_text)))
                break;
            building = repl_flatten_expr_build_begin(&ctx->expr, i, vars, nv);
            if (!parse_expr_list_exact(arg_text, arg_vals, MAX_EXPR_VARS,
                                       vars, nv, &arg_count)) {
                if (building)
                    repl_flatten_expr_build_finish(&ctx->expr, i, 0);
                break;
            }
            if (building) {
                int compiled = repl_flatten_expr_compile_active_list(
                    &ctx->expr, REPL_EXPR_ROLE_CALL_ARG, 0, arg_text,
                    /*strict=*/1, MAX_EXPR_VARS);
                repl_flatten_expr_build_finish(&ctx->expr, i,
                                               compiled == arg_count);
            }
        }
        /* Call-argument values freeze into per-flat-command local snapshots,
         * so their deps are structural. The warm branch filled arg_deps from
         * its value evals; the build/text branch takes one dep-only pass
         * here (all-bits for anything that didn't compile). */
        if (!warm_call) {
            for (int a = 0; a < arg_count && a < MAX_EXPR_VARS; a++)
                arg_deps[a] = repl_flatten_expr_deps(
                    &ctx->expr, i, REPL_EXPR_ROLE_CALL_ARG, a, vars, var_deps, nv,
                    /*structural=*/1, /*missing_ok=*/0);
        }
        for (int a = 0; a < arg_count && a < MAX_EXPR_VARS; a++)
            repl_flatten_expr_note_structural(&ctx->expr, arg_deps[a]);
        if (arg_count != param_count) {
            char msg[REPL_DIAG_TEXT_MAX];
            const char *alias = NULL;
            if (func_num >= 0 && func_num < ctx->func_aliases.count &&
                func_num < REPL_FUNC_SLOT_COUNT &&
                ctx->func_aliases.names &&
                ctx->func_aliases.names[func_num][0])
                alias = ctx->func_aliases.names[func_num];
            if (alias) {
                snprintf(msg, sizeof(msg),
                         "%s expects %d args, got %d",
                         alias, param_count, arg_count);
            } else {
                snprintf(msg, sizeof(msg),
                         "func%d expects %d args, got %d",
                         func_num, param_count, arg_count);
            }
            flatten_note_status(ctx, msg);
            break;
        }

        ExprVar lvars[MAX_EXPR_VARS];
        ReplExprDepMask ldeps[MAX_EXPR_VARS];
        ReplVisibleVarKind lkinds[MAX_EXPR_VARS];
        int lnv = 0;
        for (int p = 0; p < param_count && lnv < MAX_EXPR_VARS; p++) {
            repl_copy_string_fits(lvars[lnv].name,
                                  sizeof(lvars[lnv].name),
                                  param_names[p]);
            lvars[lnv].value = arg_vals[p];
            ldeps[lnv] = arg_deps[p];   /* params inherit their arg's mask */
            lkinds[lnv] = REPL_VISIBLE_VAR_PARAM;
            lnv++;
        }
        /* Scope is lexical, not dynamic: the frame is the callee's
         * parameters followed by the callee's own locals, and nothing of
         * the caller's. Copying caller bindings in would let a caller
         * local hide a global the callee reads — eval_primary searches
         * this array before the predef table — while the exported C reads
         * the global. The call arguments have already captured every
         * caller value the callee is entitled to receive. Params and
         * locals cannot collide: that is a same-scope redefinition, and
         * compile rejects it. */
        flatten_bind_func_locals(ctx, k + 1, body_end,
                                 lvars, ldeps, lkinds, &lnv);

        unsigned int nested_func_mask = func_scope_mask;
        if (func_num >= 0 && func_num < FUNC_SCOPE_MASK_BITS)
            nested_func_mask |= (1u << func_num);
        int nested_root_call = (root_call_src_cmd_idx >= 0)
                             ? root_call_src_cmd_idx : i;

        ctx->call_depth++;
        flatten_range(ctx, k + 1, body_end, lvars, ldeps, lkinds, lnv,
                      i, nested_root_call, nested_func_mask);
        if (ctx->call_depth > 0) ctx->call_depth--;
    } while (0);
}

static int flatten_if_arm_boundary(const FlattenContext *ctx,
                                   int start, int if_end) {
    int depth = 0;

    for (int j = start; j < if_end && j < ctx->source_count; j++) {
        CmdType t = ctx->source_cmds[j].type;
        if (depth == 0 && repl_cmd_is_if_branch_separator(t))
            return j;
        if (repl_cmd_is_block_head(t))
            depth++;
        else if (repl_cmd_is_block_end(t) && depth > 0)
            depth--;
    }
    return if_end;
}

/* Evaluate an if / else-if condition line. Every evaluated condition's deps
 * are structural (they select which arm's commands exist in the flat
 * stream); conditions that were never evaluated this flatten (short-
 * circuited by an earlier taken arm) contribute nothing — a root that only
 * they read cannot matter until an evaluated condition changes first, and
 * that condition's deps are already in the mask. */
static float flatten_eval_if_line(FlattenContext *ctx,
                                  const GLCmd *src_cmd, int line_idx,
                                  ExprVar *vars,
                                  const ReplExprDepMask *var_deps, int nv) {
    if (repl_flatten_expr_line_ready(&ctx->expr, line_idx)) {
        ReplFlattenExprValue v = repl_flatten_expr_eval(
            &ctx->expr, line_idx, REPL_EXPR_ROLE_CONDITION, 0,
            vars, var_deps, nv, /*structural=*/1);
        /* No condition program on a READY line: the paren payload failed
         * to extract when the line built, which is the text path's
         * fallback-to-args[0] case — a baked constant, no deps. */
        if (!v.found)
            return src_cmd->args[0];
        repl_flatten_expr_note_structural(&ctx->expr, v.deps);
        return v.value;
    }
    {
        int building = repl_flatten_expr_build_begin(&ctx->expr, line_idx,
                                                     vars, nv);
        ReplExprCaptureSink sink =
            repl_flatten_expr_capture_sink(&ctx->expr);
        float v = repl_eval_if_condition_captured(
            flatten_src_text(ctx->text, line_idx),
            vars, nv, src_cmd->args[0], building ? &sink : NULL);
        if (building)
            repl_flatten_expr_build_finish(&ctx->expr, line_idx, 1);
        /* Build/FAILED visit: one dep-only pass over the just-built program
         * (all-bits when it never compiled). */
        repl_flatten_expr_note_structural(
            &ctx->expr,
            repl_flatten_expr_deps(
                &ctx->expr, line_idx, REPL_EXPR_ROLE_CONDITION, 0,
                vars, var_deps, nv, /*structural=*/1, /*missing_ok=*/1));
        return v;
    }
}

/* An if-block shares the caller's scope arrays outright — no fresh frame,
 * so no copy-back and no kind rewriting; it only forwards them. */
static void flatten_if_block(FlattenContext *ctx,
                             const GLCmd *src_cmd, int i,
                             ExprVar *vars, ReplExprDepMask *var_deps,
                             const ReplVisibleVarKind *var_kinds, int nv,
                             int call_src_cmd_idx, int root_call_src_cmd_idx,
                             unsigned int func_scope_mask,
                             int if_end) {
    int arm_start = i + 1;
    int arm_end = flatten_if_arm_boundary(ctx, arm_start, if_end);
    float cond = flatten_eval_if_line(ctx, src_cmd, i, vars, var_deps, nv);

    if (cond != 0.0f) {
        flatten_range(ctx, arm_start, arm_end, vars, var_deps, var_kinds, nv,
                      call_src_cmd_idx, root_call_src_cmd_idx,
                      func_scope_mask);
        return;
    }

    for (int branch_idx = arm_end; branch_idx < if_end; ) {
        const GLCmd *branch = &ctx->source_cmds[branch_idx];
        int next_arm_end = flatten_if_arm_boundary(ctx, branch_idx + 1, if_end);

        if (branch->type == CMD_ELSE_IF) {
            cond = flatten_eval_if_line(ctx, branch, branch_idx, vars,
                                        var_deps, nv);
            if (cond != 0.0f) {
                flatten_range(ctx, branch_idx + 1, next_arm_end, vars,
                              var_deps, var_kinds, nv,
                              call_src_cmd_idx, root_call_src_cmd_idx,
                              func_scope_mask);
                return;
            }
        } else if (branch->type == CMD_ELSE) {
            flatten_range(ctx, branch_idx + 1, next_arm_end, vars, var_deps,
                          var_kinds, nv,
                          call_src_cmd_idx, root_call_src_cmd_idx,
                          func_scope_mask);
            return;
        }

        branch_idx = next_arm_end;
    }
}

static int flatten_cmd_is_source_only_cond_marker(CmdType type) {
    return (type == CMD_IF_END || repl_cmd_is_if_branch_separator(type));
}

/* True for the glMultMatrixf form whose 16 cells are expression slots on
 * the line rather than a scratch array read at bake time. Those slots sit
 * at REPL_EXPR_ROLE_CMD_ARG ordinals 0..15 — past the args[] window every
 * other command re-evaluates — so each of the three refresh paths (warm
 * flatten, dep-note, rebake) needs an extra pass keyed on this. */
static int flatten_cmd_has_matrix_slots(const GLCmd *cmd) {
    return cmd->type == CMD_MULT_MATRIXF &&
           !repl_cmd_mult_matrix_from_array(cmd);
}

/* A committed standard command already records its command type and arity.
 * Re-evaluate only the argument substring instead of redispatching and
 * revalidating the whole source line. Returning 0 is deliberately a soft
 * miss: unusual-but-valid text (for example a trailing comment containing
 * ')') falls back to the general parser below. */
static const ReplStdCommandSpec *flatten_std_spec_for_type(CmdType type) {
    const ReplStdCommandSpec *def;

    for (def = repl_std_command_specs(); def->name; def++)
        if (def->type == type)
            return def;
    return NULL;
}

static int flatten_eval_std_cmd(const GLCmd *src_cmd, const char *text,
                                ExprVar *vars, int nv, GLCmd *out) {
    const ReplStdCommandSpec *def = flatten_std_spec_for_type(src_cmd->type);
    const char *open;
    const char *close;
    char args[MAX_LINE_LEN];
    int arg_len;
    int exact_count = 0;

    if (!def || !text || !out)
        return 0;
    open = strchr(text, '(');
    close = open ? strrchr(open + 1, ')') : NULL;
    if (!open || !close || close <= open)
        return 0;
    arg_len = (int)(close - open - 1);
    if (arg_len < 0 || arg_len >= (int)sizeof(args))
        return 0;
    memcpy(args, open + 1, (size_t)arg_len);
    args[arg_len] = '\0';

    *out = *src_cmd;
    if (!parse_expr_list_exact(args, out->args, def->num_args,
                               vars, nv, &exact_count) ||
        exact_count != def->num_args)
        return 0;
    out->num_args = exact_count;

    /* Preserve the parser's post-evaluation behavior for dynamic colors. */
    if (out->type == CMD_CLEAR_COLOR) {
        for (int ci = 0; ci < 3; ci++)
            if (out->args[ci] > REPL_CLEAR_COLOR_MAX_V)
                out->args[ci] = REPL_CLEAR_COLOR_MAX_V;
    }
    return 1;
}

/* Re-parse a source line into the flat buffer, evaluating expressions
 * against the current variable bindings.  Four paths converge here:
 *   0. src_cmd->has_vars == 0 → literal line: append the committed command
 *      verbatim, no parse (see below)
 *   1. local vars present  → pass vars to the parser, keep src has_vars
 *   2. no local vars but src has predefined-var refs → re-eval, force has_vars=1
 *   3. no vars at all      → re-parse for fresh args, clear has_vars
 * On parse failure in path 3, the original src_cmd is copied through
 * unchanged (the line was already invalid at commit time).
 *
 * Path 0 is the fast path. `has_vars` is decided at commit time against every
 * variable visible at that source position — predefs plus the enclosing
 * loop/function bindings (input_has_any_visible_vars) — and the source array
 * is replaced transactionally on each successful edit. So a `has_vars == 0`
 * command's args and payload are already the parse of its current text under
 * bindings that cannot influence it, and re-parsing only reproduces them.
 * It still records the enclosing local snapshot, which replay's value-tracing
 * annotations read. Set ReplFlattenOptions.force_reparse to route path 0 back
 * through the parser; the flatten differential test compares the two.
 *
 * Returns 1 on success, 0 if the flat buffer overflowed (caller should
 * return immediately). Single-exit so the PROF_FLATTEN_REPARSE probe is
 * one begin/accum_end pair spanning the whole reparse. */
static int flatten_reparse_line(FlattenContext *ctx,
                                const GLCmd *src_cmd, int i,
                                ExprVar *vars,
                                const ReplExprDepMask *var_deps, int nv,
                                int call_src_cmd_idx,
                                int root_call_src_cmd_idx,
                                unsigned int func_scope_mask) {
    char parse_err[REPL_STATUS_TEXT_MAX];
    parse_err[0] = '\0';

    int has_local_vars = (vars && nv > 0);

    if (!src_cmd->has_vars && !ctx->force_reparse) {
        int rv;
        prof_begin(PROF_FLATTEN_REPARSE);
        rv = flatten_append_cmd(ctx, src_cmd, i, call_src_cmd_idx,
                                root_call_src_cmd_idx, func_scope_mask,
                                has_local_vars ? vars : NULL,
                                has_local_vars ? nv : 0);
        prof_accum_end(PROF_FLATTEN_REPARSE);
        return rv;
    }

    if (repl_flatten_expr_line_ready(&ctx->expr, i)) {
        /* Warm compiled path: the committed command already carries the
         * right type / num_args / enum tokens / payload; only the
         * expression-backed arg slots re-evaluate. Slots without a program
         * (enum tokens, omitted defaults like gluColor's alpha) keep their
         * baked value — every expression slot was captured when the line
         * built, or the line would be FAILED. */
        GLCmd tmp = *src_cmd;
        int rv;

        prof_begin(PROF_FLATTEN_REPARSE);
        for (int k = 0; k < tmp.num_args && k < 8; k++) {
            ReplFlattenExprValue v = repl_flatten_expr_eval(
                &ctx->expr, i, REPL_EXPR_ROLE_CMD_ARG, k,
                vars, var_deps, nv, /*structural=*/0);
            if (v.found) {
                tmp.args[k] = v.value;
                repl_flatten_expr_note_value(&ctx->expr, v.deps);
            }
        }
        if (flatten_cmd_has_matrix_slots(&tmp)) {
            for (int k = 0; k < REPL_MATRIX_CELL_COUNT; k++) {
                ReplFlattenExprValue v = repl_flatten_expr_eval(
                    &ctx->expr, i, REPL_EXPR_ROLE_CMD_ARG, k,
                    vars, var_deps, nv, /*structural=*/0);
                if (v.found) {
                    tmp.payload.matrix.m[k] = v.value;
                    repl_flatten_expr_note_value(&ctx->expr, v.deps);
                }
            }
        }
        /* Mirror the parser's post-eval fixup: glClearColor clamps each
         * RGB channel at commit AND on every reparse. */
        if (tmp.type == CMD_CLEAR_COLOR) {
            for (int ci = 0; ci < 3; ci++) {
                if (tmp.args[ci] > REPL_CLEAR_COLOR_MAX_V)
                    tmp.args[ci] = REPL_CLEAR_COLOR_MAX_V;
            }
        }
        if (tmp.type == CMD_STENCIL_FUNC) {
            int ref;
            (void)repl_stencil_clamp_ref(tmp.args[1], &ref);
            tmp.args[1] = (float)ref;
        }
        if (tmp.type == CMD_CLEAR_STENCIL) {
            int clear_value;
            (void)repl_stencil_clamp_ref(tmp.args[0], &clear_value);
            tmp.args[0] = (float)clear_value;
        }
        /* has_vars stays the committed value — identical to the text
         * branch's rules for a has_vars source command (kept under local
         * bindings, forced 1 otherwise; it is 1 here either way). */
        rv = flatten_append_cmd(ctx, &tmp, i, call_src_cmd_idx,
                                root_call_src_cmd_idx, func_scope_mask,
                                has_local_vars ? vars : NULL,
                                has_local_vars ? nv : 0);
        prof_accum_end(PROF_FLATTEN_REPARSE);
        return rv;
    }

    ReplParseContext parse_ctx = {
        .source_line_idx = i,
        .vars = has_local_vars ? vars : NULL,
        .num_vars = has_local_vars ? nv : 0,
        .err_buf = parse_err,
        .err_sz  = (int)sizeof(parse_err),
        /* Flatten only reads tmp_pl.cmd; the canonical text rendering
         * (per-arg %g/snprintf) is pure waste on every frame. */
        .skip_text = 1,
        .func_aliases = ctx->func_aliases,
        .source_scope = &ctx->source_scope,
    };
    const char *text = flatten_src_text(ctx->text, i);
    ReplParsedLine tmp_pl;
    ReplExprCaptureSink sink = repl_flatten_expr_capture_sink(&ctx->expr);
    int building = repl_flatten_expr_build_begin(&ctx->expr, i, vars, nv);
    int parsed;
    int rv = 1;

    if (building)
        parse_ctx.capture = &sink;

    prof_begin(PROF_FLATTEN_REPARSE);
    if (!building && !ctx->force_reparse &&
        flatten_eval_std_cmd(src_cmd, text,
                             has_local_vars ? vars : NULL,
                             has_local_vars ? nv : 0,
                             &tmp_pl.cmd)) {
        parsed = 1;
    } else {
        parsed = repl_parser_parse_command_ctx(text, &tmp_pl, &parse_ctx);
        if (building)
            repl_flatten_expr_build_finish(&ctx->expr, i, parsed);
    }
    if (parsed) {
        GLCmd tmp = tmp_pl.cmd;
        /* Restore is_auto after the parse, not before it: the parser memsets
         * the whole ReplParsedLine as its first statement, so a seed written
         * ahead of the call is erased. The parser never sets is_auto (it only
         * clears it, for CMD_EMPTY and CMD_COMMENT), so without this a
         * synthesized normal that takes the reparse branch loses the flag and
         * stops being recognized as generated. The literal fast path above
         * appends the committed command verbatim and keeps it; restoring here
         * is what makes the two paths agree.
         *
         * Nothing else needs restoring. `valid` is parser-owned: a line that
         * fails to reparse must not inherit a stale valid=1. `var_idx` is
         * commit-time state, but its only carrier (CMD_VAR_ASSIGN) is routed
         * to flatten_var_assign before it can reach here, as is
         * CMD_VAR_DECLARE with payload.decl. The parser's memset already
         * zeroes the payload union (GLCmd's "unused payload is zeroed"
         * contract) and refills payload.label for CMD_LABEL. */
        tmp.is_auto = src_cmd->is_auto;
        if (has_local_vars)
            tmp.has_vars = src_cmd->has_vars;
        else if (src_cmd->has_vars)
            tmp.has_vars = 1;
        else
            tmp.has_vars = 0;
        /* Build/FAILED visit of a var-bearing command: one dep-only pass
         * over the just-built programs (all-bits when the line stayed
         * FAILED; slots without a program on a READY line are baked
         * enum/constant slots). A has_vars command whose line never
         * reached READY can't be re-evaluated in place, so it also
         * forfeits rebake for this flat program. */
        if (tmp.has_vars) {
            int slots = flatten_cmd_has_matrix_slots(&tmp)
                      ? REPL_MATRIX_CELL_COUNT
                      : (tmp.num_args < 8 ? tmp.num_args : 8);
            for (int k = 0; k < slots; k++)
                repl_flatten_expr_note_value(
                    &ctx->expr,
                    repl_flatten_expr_deps(
                        &ctx->expr, i, REPL_EXPR_ROLE_CMD_ARG, k,
                        vars, var_deps, nv,
                        /*structural=*/0, /*missing_ok=*/1));
            repl_flatten_expr_note_emitted(&ctx->expr, tmp.has_vars, i);
        }
        rv = flatten_append_cmd(ctx, &tmp, i, call_src_cmd_idx,
                                root_call_src_cmd_idx, func_scope_mask,
                                has_local_vars ? vars : NULL,
                                has_local_vars ? nv : 0);
    } else if (!has_local_vars && !src_cmd->has_vars) {
        rv = flatten_append_cmd(ctx, src_cmd, i, call_src_cmd_idx,
                                root_call_src_cmd_idx, func_scope_mask,
                                NULL, 0);
    }
    prof_accum_end(PROF_FLATTEN_REPARSE);
    return rv;
}

/* CMD_VAR_ASSIGN: re-evaluate the RHS against current bindings, update the
 * predefined-var slot, and append the resolved assignment. Returns 1 on
 * success (caller advances), 0 if the flat buffer overflowed (caller aborts).
 * Extracted out of flatten_range so the PROF_FLATTEN_ASSIGN probe is one
 * begin/accum_end pair per call and flatten_range stays under its size cap. */
/* Resolve a scalar assignment's destination against the live frame.
 *
 * Returns the scope-array slot for a function-scoped target, or -1 when the
 * target is the predef slot the source command carries. The persisted
 * `var_idx` is a commit-time storage *hint*, not the lexical authority: a
 * later legal edit — inserting a local over an existing global — must
 * retarget older assignment rows without rewriting every one of them. So
 * the LHS is re-derived from source text and resolved here on every visit.
 *
 * `*unwritable_out` reports a first match that is a PARAM or LOOP. Commit
 * and the reverse binder guards are supposed to make that state
 * unreachable; flatten refuses to mutate the binding regardless.
 *
 * Only meaningful inside a frame, so nv == 0 (the whole top level) costs
 * nothing. */
static int flatten_resolve_assign_target(const char *src_text,
                                         const ExprVar *vars,
                                         const ReplVisibleVarKind *var_kinds,
                                         int nv, int *unwritable_out) {
    char lhs[REPL_PREDEF_NAME_MAX] = "";

    *unwritable_out = 0;
    if (nv <= 0 || !vars ||
        !repl_extract_assignment_parts(src_text ? src_text : "",
                                       lhs, sizeof(lhs), NULL, 0))
        return -1;

    for (int v = 0; v < nv; v++) {
        if (strcmp(vars[v].name, lhs) != 0)
            continue;
        /* First match decides — never skip a matching PARAM/LOOP looking
         * for an outer LOCAL, or a shadowed binding would become
         * assignable. */
        if (var_kinds && var_kinds[v] != REPL_VISIBLE_VAR_LOCAL) {
            *unwritable_out = 1;
            return -1;
        }
        return v;
    }
    return -1;
}

/* CMD_VAR_ASSIGN: re-evaluate the RHS against current bindings, update the
 * predefined-var slot, and append the resolved assignment. Returns 1 on
 * success (caller advances), 0 if the flat buffer overflowed (caller aborts).
 * Extracted out of flatten_range so the PROF_FLATTEN_ASSIGN probe is one
 * begin/accum_end pair per call and flatten_range stays under its size cap. */
static int flatten_var_assign(FlattenContext *ctx, const GLCmd *src_cmd, int i,
                              ExprVar *vars,
                              ReplExprDepMask *var_deps,
                              const ReplVisibleVarKind *var_kinds, int nv,
                              int call_src_cmd_idx,
                              int root_call_src_cmd_idx,
                              unsigned int func_scope_mask) {
    prof_begin(PROF_FLATTEN_VAR_ASSIGN);
    int rv = 1;
    int var_idx = src_cmd->var_idx; /* predef var slot */
    float value = src_cmd->args[0];
    char rhs[MAX_LINE_LEN] = "";
    int local_rhs_vars = 0;
    /* Assignment dataflow: the destination slot's mask becomes the RHS's
     * mask (a warm eval yields it directly; build/text visits take one
     * dep-only pass below). A constant RHS is 0 — `n = 5` correctly makes
     * a later slider change to n a routing no-op, since any reflatten
     * re-bakes n = 5 over it. */
    ReplExprDepMask rhs_deps = 0;
    int warm = repl_flatten_expr_line_ready(&ctx->expr, i);
    const char *src_text = flatten_src_text(ctx->text, i);
    int unwritable = 0;
    int local_slot = flatten_resolve_assign_target(src_text, vars, var_kinds,
                                                   nv, &unwritable);
    /* A local's value lives in a per-call frame that rebake_one_cmd cannot
     * reconstruct: it evaluates each command against a *frozen*
     * FlatCmdLocalVars snapshot and writes back only through predef slots,
     * so nothing would carry a local's new value into later commands'
     * snapshots. Reporting the RHS structurally forces a full reflatten
     * instead of a value-only rebake. Assignment is the only way a value
     * enters a local (locals take no initializer), so this one call covers
     * the whole dataflow. Keyed on the *resolved* target, not the persisted
     * var_idx, so a pre-existing global assignment retargeted by a newly
     * inserted local takes the structural path too. */
    int structural = (local_slot >= 0);

    if (unwritable) {
        char lhs[REPL_PREDEF_NAME_MAX] = "";
        char msg[REPL_DIAG_TEXT_MAX];
        repl_extract_assignment_parts(src_text ? src_text : "",
                                      lhs, sizeof(lhs), NULL, 0);
        snprintf(msg, sizeof(msg),
                 "cannot assign to '%s' here - it is a parameter or loop variable",
                 lhs);
        prof_accum_end(PROF_FLATTEN_VAR_ASSIGN);
        flatten_fail(ctx, msg);
        return 0;
    }

    if (warm) {
        /* Warm path. A READY line without an RHS program mirrors the text
         * branch's extract-failure case: keep the baked args[0]. */
        ReplFlattenExprValue v = repl_flatten_expr_eval(
            &ctx->expr, i, REPL_EXPR_ROLE_ASSIGN_RHS, 0,
            vars, var_deps, nv, structural);
        if (v.found) {
            value = v.value;
            rhs_deps = v.deps;
            if (vars && nv > 0)
                local_rhs_vars = v.used_local;
        }
    } else if (repl_extract_assignment_parts(src_text, NULL, 0,
                                             rhs, sizeof(rhs)) && rhs[0]) {
        char repl_rhs[MAX_LINE_LEN];
        const char *eval_rhs = rhs;
        int building = repl_flatten_expr_build_begin(&ctx->expr, i, vars, nv);
        /* Editor/import source is canonical REPL text. Only the forced
         * differential reference retains the old defensive translation. */
        if (ctx->force_reparse) {
            repl_eval_c_expr_to_repl(rhs, repl_rhs, sizeof(repl_rhs));
            eval_rhs = repl_rhs;
        }
        if (building) {
            /* Compile the exact text evaluated below. */
            repl_flatten_expr_capture_span(
                &ctx->expr, REPL_EXPR_ROLE_ASSIGN_RHS, 0,
                eval_rhs, eval_rhs + strlen(eval_rhs));
            repl_flatten_expr_build_finish(&ctx->expr, i, 1);
        }
        ExprCtx expr_ctx = { eval_rhs, vars, nv, NULL, 0 };
        value = repl_eval_expr(&expr_ctx);
        if (vars && nv > 0)
            local_rhs_vars = input_has_expr_vars(rhs, vars, nv);
    } else {
        /* No evaluable RHS: freeze that verdict so later visits take the
         * warm keep-baked branch instead of re-extracting every time. */
        if (repl_flatten_expr_build_begin(&ctx->expr, i, vars, nv))
            repl_flatten_expr_build_finish(&ctx->expr, i, 1);
    }
    if (!warm)
        rhs_deps = repl_flatten_expr_deps(
            &ctx->expr, i, REPL_EXPR_ROLE_ASSIGN_RHS, 0,
            vars, var_deps, nv,
            structural, /*missing_ok=*/1);
    if (local_slot >= 0) {
        vars[local_slot].value = value;
        if (var_deps)
            var_deps[local_slot] = rhs_deps;
        repl_flatten_expr_note_structural(&ctx->expr, rhs_deps);
    } else {
        if (var_idx >= 0 && var_idx < g_num_predef_vars)
            g_predef_vars_mut[var_idx].value = value;
        if (var_idx >= 0 && var_idx < MAX_PREDEF_VARS)
            repl_flatten_expr_set_predef_deps(&ctx->expr, var_idx, rhs_deps);
        repl_flatten_expr_note_value(&ctx->expr, rhs_deps);
    }
    {
        GLCmd tmp = *src_cmd;
        tmp.args[0] = value;
        /* Normalize the emitted target to whatever actually got written,
         * so the flat stream never carries a stale storage claim. */
        tmp.var_idx = (local_slot >= 0) ? REPL_VAR_IDX_LOCAL : var_idx;
        tmp.has_vars = src_cmd->has_vars || local_rhs_vars;
        repl_flatten_expr_note_emitted(&ctx->expr, tmp.has_vars, i);
        if (!flatten_append_cmd(ctx, &tmp, i, call_src_cmd_idx,
                                root_call_src_cmd_idx, func_scope_mask,
                                vars, nv))
            rv = 0;
    }
    prof_accum_end(PROF_FLATTEN_VAR_ASSIGN);
    return rv;
}

/* A[i] = expr scratch-array assignment: re-evaluate index + RHS, write the
 * scratch cell, and append the resolved command. Returns 1 on success, 0 if
 * the index is out of range or the flat buffer overflowed (caller aborts). */
static int flatten_scratch_assign(FlattenContext *ctx, const GLCmd *src_cmd,
                                  int i, ExprVar *vars,
                                  const ReplExprDepMask *var_deps, int nv,
                                  int call_src_cmd_idx,
                                  int root_call_src_cmd_idx,
                                  unsigned int func_scope_mask) {
    prof_begin(PROF_FLATTEN_SCRATCH_ASSIGN);
    int rv = 1;
    int array_idx = (int)src_cmd->args[0];
    int elem_idx = (int)src_cmd->args[1];
    float value = src_cmd->args[2];
    char name[REPL_PREDEF_NAME_MAX] = "";
    char index_expr[MAX_LINE_LEN] = "";
    char rhs[MAX_LINE_LEN] = "";
    int local_index_vars = 0;
    int local_rhs_vars = 0;
    /* Index and RHS are both value positions: a rebake re-evaluates them
     * and rewrites the cell in stream order, so scratch reads downstream
     * see the updated value without any per-cell dependency metadata. */
    ReplExprDepMask assign_deps = 0;
    int warm = repl_flatten_expr_line_ready(&ctx->expr, i);
    const char *src_text = flatten_src_text(ctx->text, i);

    if (warm) {
        /* Warm path. Programs absent on a READY line mean the target
         * extraction failed when the line built — keep the baked args,
         * like the text branch below. */
        ReplFlattenExprValue vi = repl_flatten_expr_eval(
            &ctx->expr, i, REPL_EXPR_ROLE_SCRATCH_INDEX, 0,
            vars, var_deps, nv, /*structural=*/0);
        ReplFlattenExprValue vr = repl_flatten_expr_eval(
            &ctx->expr, i, REPL_EXPR_ROLE_SCRATCH_RHS, 0,
            vars, var_deps, nv, /*structural=*/0);
        if (vi.found && vr.found) {
            elem_idx = (int)vi.value;
            value = vr.value;
            assign_deps = vi.deps | vr.deps;
            if (vars && nv > 0) {
                local_index_vars = vi.used_local;
                local_rhs_vars = vr.used_local;
            }
        }
    } else if (repl_extract_assignment_target_parts(src_text,
                                            name, sizeof(name),
                                            index_expr, sizeof(index_expr),
                                            rhs, sizeof(rhs))) {
        char repl_index[MAX_LINE_LEN];
        char repl_rhs[MAX_LINE_LEN];
        int building = repl_flatten_expr_build_begin(&ctx->expr, i, vars, nv);
        repl_eval_c_expr_to_repl(index_expr, repl_index, sizeof(repl_index));
        repl_eval_c_expr_to_repl(rhs, repl_rhs, sizeof(repl_rhs));
        if (building) {
            /* Compile the translated index + RHS — the exact texts
             * evaluated below. */
            repl_flatten_expr_capture_span(
                &ctx->expr, REPL_EXPR_ROLE_SCRATCH_INDEX, 0,
                repl_index, repl_index + strlen(repl_index));
            repl_flatten_expr_capture_span(
                &ctx->expr, REPL_EXPR_ROLE_SCRATCH_RHS, 0,
                repl_rhs, repl_rhs + strlen(repl_rhs));
            repl_flatten_expr_build_finish(&ctx->expr, i, 1);
        }

        ExprCtx index_ctx = { repl_index, vars, nv, NULL, 0 };
        ExprCtx rhs_ctx = { repl_rhs, vars, nv, NULL, 0 };
        elem_idx = (int)repl_eval_expr(&index_ctx);
        value = repl_eval_expr(&rhs_ctx);
        if (vars && nv > 0) {
            local_index_vars = input_has_expr_vars(index_expr, vars, nv);
            local_rhs_vars = input_has_expr_vars(rhs, vars, nv);
        }
    } else {
        /* Extraction failed: freeze the keep-baked verdict. */
        if (repl_flatten_expr_build_begin(&ctx->expr, i, vars, nv))
            repl_flatten_expr_build_finish(&ctx->expr, i, 1);
    }
    if (!warm)
        assign_deps =
            repl_flatten_expr_deps(
                &ctx->expr, i, REPL_EXPR_ROLE_SCRATCH_INDEX, 0,
                vars, var_deps, nv,
                /*structural=*/0, /*missing_ok=*/1) |
            repl_flatten_expr_deps(
                &ctx->expr, i, REPL_EXPR_ROLE_SCRATCH_RHS, 0,
                vars, var_deps, nv, 0, 1);
    repl_flatten_expr_note_value(&ctx->expr, assign_deps);

    if (elem_idx < 0 || elem_idx >= REPL_SCRATCH_ARRAY_LEN) {
        char msg[REPL_DIAG_TEXT_MAX];
        snprintf(msg, sizeof(msg),
                 "scratch array index out of range: %d", elem_idx);
        flatten_fail(ctx, msg);
        prof_accum_end(PROF_FLATTEN_SCRATCH_ASSIGN);
        return 0;
    }

    repl_eval_scratch_set(array_idx, elem_idx, value);
    {
        GLCmd tmp = *src_cmd;
        tmp.args[0] = (float)array_idx;
        tmp.args[1] = (float)elem_idx;
        tmp.args[2] = value;
        tmp.num_args = 3;
        tmp.has_vars = src_cmd->has_vars || local_index_vars || local_rhs_vars;
        repl_flatten_expr_note_emitted(&ctx->expr, tmp.has_vars, i);
        if (!flatten_append_cmd(ctx, &tmp, i, call_src_cmd_idx,
                                root_call_src_cmd_idx, func_scope_mask,
                                vars, nv))
            rv = 0;
    }
    prof_accum_end(PROF_FLATTEN_SCRATCH_ASSIGN);
    return rv;
}

/* Recursively expand source_commands[start..end_idx) into the destination
 * flat buffer named by FlattenContext. For-loops are unrolled, function calls
 * are inlined, and if-blocks with loop-variable conditions are evaluated.
 * `vars`/`nv` carry loop-variable and function-parameter bindings from
 * enclosing scopes; `var_deps` (parallel to `vars`, NULL ⇒ all zero) carries
 * each binding's predef dependency mask for phase-3 dep propagation. */
static void flatten_range(FlattenContext *ctx,
                          int start, int end_idx,
                          ExprVar *vars, ReplExprDepMask *var_deps,
                          const ReplVisibleVarKind *var_kinds, int nv,
                          int call_src_cmd_idx, int root_call_src_cmd_idx,
                          unsigned int func_scope_mask) {
    int i = start;
    while (i < end_idx && i < ctx->source_count) {
        const GLCmd *src_cmd = &ctx->source_cmds[i];

        if (ctx->abort) return;
        if (--ctx->visit_budget < 0) {
            flatten_fail(ctx, "Recursive expansion exceeded visit budget");
            return;
        }
        if (!src_cmd->valid) { i++; continue; }

        if (src_cmd->type == CMD_FOR_BEGIN) {
            int loop_end = flatten_repl_source_scope_find_block_end(ctx, i);
            flatten_for_loop(ctx, src_cmd, i, vars, var_deps, var_kinds, nv,
                             call_src_cmd_idx, root_call_src_cmd_idx,
                             func_scope_mask, loop_end);
            i = (loop_end < ctx->source_count) ? loop_end + 1 : ctx->source_count;
            continue;
        }

        if (src_cmd->type == CMD_FOR_END) { i++; continue; }

        if (src_cmd->type == CMD_FUNC_DEF) {
            int func_end = flatten_repl_source_scope_find_block_end(ctx, i);
            i = (func_end < ctx->source_count) ? func_end + 1 : ctx->source_count;
            continue;
        }
        if (src_cmd->type == CMD_FUNC_END) { i++; continue; }

        if (src_cmd->type == CMD_CALL) {
            flatten_call(ctx, src_cmd, i, vars, var_deps, nv,
                         root_call_src_cmd_idx, func_scope_mask);
            i++;
            continue;
        }

        if (src_cmd->type == CMD_IF_BEGIN) {
            int if_end = flatten_repl_source_scope_find_block_end(ctx, i);
            flatten_if_block(ctx, src_cmd, i, vars, var_deps, var_kinds, nv,
                             call_src_cmd_idx, root_call_src_cmd_idx,
                             func_scope_mask, if_end);
            i = (if_end < ctx->source_count) ? if_end + 1 : ctx->source_count;
            continue;
        }

        if (flatten_cmd_is_source_only_cond_marker(src_cmd->type)) { i++; continue; }

        if ((src_cmd->type == CMD_GOTO_LABEL || src_cmd->type == CMD_GOTO) &&
            func_scope_mask != 0) {
            flatten_fail(ctx, "goto and labels are not supported inside functions");
            return;
        }

        if (src_cmd->type == CMD_COMMENT) { i++; continue; }

        if (src_cmd->type == CMD_EMPTY) { i++; continue; }

        if (src_cmd->type == CMD_VAR_DECLARE) { i++; continue; }

        /* Variable assignments: update predefined var and pass through */
        if (src_cmd->type == CMD_VAR_ASSIGN) {
            if (!flatten_var_assign(ctx, src_cmd, i, vars, var_deps,
                                    var_kinds, nv, call_src_cmd_idx,
                                    root_call_src_cmd_idx, func_scope_mask))
                return;
            i++;
            continue;
        }

        if (src_cmd->type == CMD_SCRATCH_ASSIGN) {
            if (!flatten_scratch_assign(ctx, src_cmd, i, vars, var_deps, nv,
                                        call_src_cmd_idx, root_call_src_cmd_idx,
                                        func_scope_mask))
                return;
            i++;
            continue;
        }

        if (!flatten_reparse_line(ctx, src_cmd, i, vars, var_deps, nv,
                                  call_src_cmd_idx, root_call_src_cmd_idx,
                                  func_scope_mask))
            return;
        i++;
    }
}

/* Walk the *flat* program — the post-expansion command stream — so
 * that glEnable(GL_LIGHTING) inside `if(0) { }`, `for(i, 0, 0) { }`,
 * an unreferenced funcN body, or a goto-skipped region does not count.
 * Walking the source array (the pre-#10 behavior) treated those as
 * effective and lit scenes that the executor would render unlit. */
static int flatten_flat_lighting_enabled(const GLCmd *flat_cmds,
                                         int flat_count) {
    int user_lighting_enabled = 0;

    for (int i = 0; i < flat_count; i++) {
        if (!flat_cmds[i].valid) continue;
        if (flat_cmds[i].type == CMD_ENABLE &&
            (GLenum)flat_cmds[i].args[0] == GL_LIGHTING)
            user_lighting_enabled = 1;
        else if (flat_cmds[i].type == CMD_DISABLE &&
                 (GLenum)flat_cmds[i].args[0] == GL_LIGHTING)
            user_lighting_enabled = 0;
    }
    return user_lighting_enabled;
}

int repl_flat_clears_stencil(const GLCmd *flat_cmds, int flat_count) {
    int i;

    if (!flat_cmds || flat_count <= 0)
        return 0;
    for (i = 0; i < flat_count; i++) {
        if (!flat_cmds[i].valid || flat_cmds[i].type != CMD_CLEAR)
            continue;
        if (((GLbitfield)flat_cmds[i].args[0] & GL_STENCIL_BUFFER_BIT) != 0)
            return 1;
    }
    return 0;
}

int repl_flatten_program(const ReplFlattenOptions *options,
                         ReplFlattenResult *result) {
    ReplFlattenResult local_result;
    FlattenContext ctx = {
        .source_cmds = options ? options->source_cmds : NULL,
        .source_count = options ? options->source_cmd_count : 0,
        .flat_cmds = options ? options->flat_cmds : NULL,
        .flat_local_vars = options ? options->flat_local_vars : NULL,
        .flat_capacity = options ? options->flat_capacity : 0,
        .flat_count = 0,
        .capacity_exceeded = 0,
        .text = options ? options->text : (SourceTextView){0},
        .func_aliases = options ? options->func_aliases : (ReplFuncAliasView){0},
        .max_call_depth = options && options->max_call_depth > 0
                        ? options->max_call_depth : MAX_FLATTEN_CALL_DEPTH,
        .force_reparse = options ? options->force_reparse : 0,
        .call_depth = 0,
        .abort = 0,
        .visit_budget = options && options->visit_budget > 0
                      ? options->visit_budget : MAX_FLATTEN_VISIT_BUDGET
    };

    repl_flatten_expr_init(&ctx.expr,
                           options ? options->expr_cache : NULL,
                           ctx.force_reparse);

    if (!result)
        result = &local_result;
    memset(result, 0, sizeof(*result));

    if (ctx.source_count < 0 || ctx.flat_capacity < 0 ||
        (ctx.source_count > 0 && !ctx.source_cmds) ||
        (ctx.flat_capacity > 0 && !ctx.flat_cmds)) {
        repl_copy_string_fits(result->status, sizeof(result->status),
                              "Invalid flatten program options");
        return 0;
    }

    repl_source_scope_view_bind(&ctx.source_scope,
                                ctx.source_cmds,
                                ctx.source_count);

    /* Pre-index funcN definitions so CMD_CALL handlers can jump straight
     * to the body without scanning source_cmds[] per call. First match
     * wins: the pre-cbe73d2 linear scan broke on its first match, and
     * any other choice would silently change which body binds to a slot
     * when a malformed file slips two CMD_FUNC_DEFs through the
     * compile-time duplicate check (compile.c:1775 / commit.c:654). */
    for (int s = 0; s < REPL_FUNC_SLOT_COUNT; s++)
        ctx.func_def_idx[s] = -1;
    for (int k = 0; k < ctx.source_count; k++) {
        if (ctx.source_cmds[k].type != CMD_FUNC_DEF) continue;
        int fn = (int)ctx.source_cmds[k].args[0];
        if (fn >= 0 && fn < REPL_FUNC_SLOT_COUNT &&
            ctx.func_def_idx[fn] < 0)
            ctx.func_def_idx[fn] = k;
    }

    /* Accumulate the eval-heavy leaf phases (GL-command reparse, scalar
     * assignment, scratch-array assignment) across the whole recursive
     * expansion (for-loops/calls re-enter flatten_range many times), then
     * commit once so the profile panel shows per-flatten totals under the
     * PROF_FLATTEN parent. The structural remainder (loop/if/call iteration)
     * is the unattributed difference, as elsewhere in the panel. */
    if (!g_flat_refresh_profile_frame.active) {
        prof_accum_reset(PROF_FLATTEN_REPARSE);
        prof_accum_reset(PROF_FLATTEN_VAR_ASSIGN);
        prof_accum_reset(PROF_FLATTEN_SCRATCH_ASSIGN);
    }
    flatten_range(&ctx, 0, ctx.source_count, NULL, NULL, NULL, 0, -1, -1, 0);
    if (!g_flat_refresh_profile_frame.active) {
        prof_accum_commit(PROF_FLATTEN_REPARSE);
        prof_accum_commit(PROF_FLATTEN_VAR_ASSIGN);
        prof_accum_commit(PROF_FLATTEN_SCRATCH_ASSIGN);
    }
    if (ctx.abort) {
        ctx.flat_count = 0;
    } else if (ctx.capacity_exceeded) {
        snprintf(ctx.status, sizeof(ctx.status),
                 "Flattened command limit reached: %d commands need capacity %d",
                 ctx.flat_count, ctx.flat_capacity);
    }

    result->ok = !ctx.abort && !ctx.capacity_exceeded;
    result->flat_cmd_count = result->ok ? ctx.flat_count : 0;
    result->required_flat_capacity = ctx.capacity_exceeded && !ctx.abort
                                   ? ctx.flat_count
                                   : result->flat_cmd_count;
    result->user_lighting_enabled = result->ok
        ? flatten_flat_lighting_enabled(ctx.flat_cmds, ctx.flat_count)
        : 0;
    if (!result->ok) {
        /* A failed expansion (depth/budget/capacity overflow) leaves an empty
         * flat program; report all-bits so any predef change retries a full
         * flatten rather than being routed away from the failure. */
        result->structural_dep_mask = REPL_EXPR_DEP_ALL;
        result->value_dep_mask = REPL_EXPR_DEP_ALL;
        result->rebake_ok = 0;
    } else {
        result->structural_dep_mask =
            repl_flatten_expr_structural_deps(&ctx.expr);
        result->value_dep_mask = repl_flatten_expr_value_deps(&ctx.expr);
        result->rebake_ok = repl_flatten_expr_rebake_ok(&ctx.expr);
    }
    repl_copy_string_fits(result->status, sizeof(result->status), ctx.status);
    return result->ok;
}

/* ---- In-place rebake (flatten plan phase 3b) --------------------------- */

/* Re-bake one flat command's baked values in place. `line` is its owning
 * source line (src_cmd_idx). Assignments update the live predef/scratch
 * tables so later commands in the walk read the new values, mirroring the
 * full flatten's stream-order threading. Returns 1 on success. */
static int rebake_one_cmd(const ReplRebakeOptions *o, int k,
                          ReplRebakeResult *result) {
    GLCmd *cmd = &o->flat_cmds[k];
    const FlatCmdLocalVars *locals =
        o->flat_local_vars ? &o->flat_local_vars[k] : NULL;
    int line = cmd->src_cmd_idx;
    int ok = 1;

    if (!cmd->valid)
        return 1;

    /* Eligibility was derived by the last full flatten, but a source/cache
     * invalidation can occur before this walk. Never mistake a stale line's
     * missing programs for optional baked constants. */
    if (cmd->has_vars &&
        !repl_flatten_expr_rebake_line_ready(o->expr_cache, line)) {
        snprintf(result->status, sizeof(result->status),
                 "Expression cache line %d is not ready", line);
        return 0;
    }

    if (cmd->type == CMD_VAR_ASSIGN) {
        float value = cmd->args[0];
        if (cmd->has_vars)
            ok = repl_flatten_expr_rebake_eval(
                o->expr_cache, line, REPL_EXPR_ROLE_ASSIGN_RHS, 0,
                locals ? locals->vars : NULL, locals ? locals->num_vars : 0,
                &value);
        if (cmd->has_vars && !ok) {
            /* A has_vars assignment with no RHS program on a READY line is
             * the "no evaluable RHS" case the full flatten also leaves at
             * the baked args[0] — keep it, don't fail. */
            return 1;
        }
        if (cmd->var_idx >= 0 && cmd->var_idx < g_num_predef_vars)
            g_predef_vars_mut[cmd->var_idx].value = value;
        cmd->args[0] = value;
        return 1;
    }

    if (cmd->type == CMD_SCRATCH_ASSIGN) {
        float idx_f = cmd->args[1];
        float value = cmd->args[2];
        int ok_idx = 1;
        int ok_rhs = 1;
        if (cmd->has_vars) {
            ok_idx = repl_flatten_expr_rebake_eval(
                o->expr_cache, line, REPL_EXPR_ROLE_SCRATCH_INDEX, 0,
                locals ? locals->vars : NULL, locals ? locals->num_vars : 0,
                &idx_f);
            ok_rhs = repl_flatten_expr_rebake_eval(
                o->expr_cache, line, REPL_EXPR_ROLE_SCRATCH_RHS, 0,
                locals ? locals->vars : NULL, locals ? locals->num_vars : 0,
                &value);
        }
        int elem_idx;
        if (!ok_idx || !ok_rhs)
            return 1;   /* extraction-failure case: keep baked args */
        elem_idx = (int)idx_f;
        if (elem_idx < 0 || elem_idx >= REPL_SCRATCH_ARRAY_LEN) {
            snprintf(result->status, sizeof(result->status),
                     "scratch array index out of range: %d", elem_idx);
            return 0;
        }
        repl_eval_scratch_set((int)cmd->args[0], elem_idx, value);
        cmd->args[1] = (float)elem_idx;
        cmd->args[2] = value;
        return 1;
    }

    if (!cmd->has_vars)
        return 1;   /* constant non-assignment commands need no work */

    /* Literal-form glMultMatrixf: the cells are the command, and they live
     * in the payload rather than args[]. */
    if (flatten_cmd_has_matrix_slots(cmd)) {
        for (int k = 0; k < REPL_MATRIX_CELL_COUNT; k++) {
            float v = 0.0f;
            if (repl_flatten_expr_rebake_eval(
                    o->expr_cache, line, REPL_EXPR_ROLE_CMD_ARG, k,
                    locals ? locals->vars : NULL,
                    locals ? locals->num_vars : 0, &v))
                cmd->payload.matrix.m[k] = v;
        }
        return 1;
    }

    /* Generic GL command: re-evaluate the expression-backed arg slots.
     * Slots without a program (enum tokens, omitted defaults) keep their
     * baked value — the same rule flatten_reparse_line's warm path uses. */
    for (int a = 0; a < cmd->num_args && a < 8; a++) {
        float v = 0.0f;
        int slot_ok = repl_flatten_expr_rebake_eval(
            o->expr_cache, line, REPL_EXPR_ROLE_CMD_ARG, a,
            locals ? locals->vars : NULL, locals ? locals->num_vars : 0,
            &v);
        if (slot_ok)
            cmd->args[a] = v;
    }
    if (cmd->type == CMD_CLEAR_COLOR) {
        for (int ci = 0; ci < 3; ci++)
            if (cmd->args[ci] > REPL_CLEAR_COLOR_MAX_V)
                cmd->args[ci] = REPL_CLEAR_COLOR_MAX_V;
    }
    if (cmd->type == CMD_STENCIL_FUNC) {
        int ref;
        (void)repl_stencil_clamp_ref(cmd->args[1], &ref);
        cmd->args[1] = (float)ref;
    }
    if (cmd->type == CMD_CLEAR_STENCIL) {
        int clear_value;
        (void)repl_stencil_clamp_ref(cmd->args[0], &clear_value);
        cmd->args[0] = (float)clear_value;
    }
    return 1;
}

int repl_flatten_rebake_program(const ReplRebakeOptions *options,
                                ReplRebakeResult *result) {
    ReplRebakeResult local_result;

    if (!result)
        result = &local_result;
    memset(result, 0, sizeof(*result));

    if (!options || !options->expr_cache || options->flat_count < 0 ||
        (options->flat_count > 0 && !options->flat_cmds)) {
        repl_copy_string_fits(result->status, sizeof(result->status),
                              "Invalid rebake options");
        return 0;
    }

    for (int k = 0; k < options->flat_count; k++) {
        if (!rebake_one_cmd(options, k, result)) {
            result->ok = 0;
            return 0;
        }
    }
    result->ok = 1;
    return 1;
}

/* Diagnostic/reference switch for the live pipeline. Read once because this
 * sits on the frame path; changing the environment of a running process is
 * not a supported cache transition. Private flatten callers can independently
 * disable the cache by passing ReplFlattenOptions.expr_cache = NULL. */
static ReplExprCache *flatten_live_expr_cache(void) {
    static int initialized = 0;
    static int disabled = 0;

    if (!initialized) {
        const char *env = getenv("GLR_NO_FLATTEN_CACHE");
        disabled = env && env[0];
        initialized = 1;
    }
    return disabled ? NULL : repl_expr_cache_live();
}

static int flatten_rebake_live(void) {
    ReplFlatProgramState *flat_program = repl_state_flat_program_writable();
    float base_predef[MAX_PREDEF_VARS] = { 0 };
    float base_scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]
        = { { 0.0f } };
    ReplRebakeOptions options;
    ReplRebakeResult result;
    int rv;

    if (!repl_state_flat_program_rebake_ok())
        return 0;   /* not every has_vars command has compiled programs */

    /* Snapshot the pre-rebake tables so a mid-walk failure can be rolled
     * back cleanly before the caller escalates to a full flatten. */
    repl_copy_predef_values(base_predef, MAX_PREDEF_VARS);
    repl_eval_copy_scratch_arrays(base_scratch);

    options = (ReplRebakeOptions){
        .flat_cmds = flat_program->cmds,
        .flat_local_vars = flat_program->local_vars,
        .flat_count = flat_program->cmd_count,
        .expr_cache = flatten_live_expr_cache(),
    };
    /* One bracket around the complete compiled-expression walk keeps this
     * diagnostic useful without adding a clock read around every argument.
     * The parent also includes baseline copies, rollback, and dirty routing. */
    prof_begin(PROF_REBAKE_EVAL);
    rv = repl_flatten_rebake_program(&options, &result);
    flat_refresh_profile_end(PROF_REBAKE_EVAL);

    if (!rv) {
        repl_restore_predef_values(base_predef, MAX_PREDEF_VARS);
        repl_eval_restore_scratch_arrays(base_scratch);
        return 0;
    }
    repl_state_flat_program_clear_args_dirty();
    return 1;
}

void repl_flatten_commands(int edit_line_idx) {
    ReplFlatProgramState *flat_program = repl_state_flat_program_writable();
    ReplFlattenOptions options = {
        .source_cmds = repl_state_document_cmds(),
        .source_cmd_count = repl_state_document_count(),
        .flat_cmds = flat_program->cmds,
        .flat_local_vars = flat_program->local_vars,
        .flat_capacity = flat_program->capacity,
        .text = source_document_view(),
        .func_aliases = repl_func_alias_view(),
        .max_call_depth = MAX_FLATTEN_CALL_DEPTH,
        .visit_budget = MAX_FLATTEN_VISIT_BUDGET,
        .expr_cache = flatten_live_expr_cache()
    };
    ReplFlattenResult result;

    repl_flatten_program(&options, &result);
    repl_state_flat_program_set_count(result.flat_cmd_count);
    flat_program->overflow_cmd_count =
        result.required_flat_capacity > flat_program->capacity
            ? result.required_flat_capacity
            : 0;
    repl_state_flat_program_set_user_lighting_enabled(
        result.user_lighting_enabled);
    /* Refresh the dependency-routing state: a full flatten re-derives both
     * masks and subsumes any pending args-only dirt. */
    repl_state_flat_program_set_dep_state(result.structural_dep_mask,
                                          result.value_dep_mask,
                                          result.rebake_ok);
    if (result.status[0])
        repl_set_status_error(result.status);

    repl_flatten_refresh_current_block_highlight(edit_line_idx);
}

static ReplFlatRefreshKind flatten_refresh_live(int edit_line_idx,
                                                int require_full,
                                                int require_values) {
    if (!require_full && !require_values)
        return REPL_FLAT_REFRESH_NONE;

    if (!require_full) {
        int rebaked;
        prof_begin(PROF_REBAKE);
        rebaked = flatten_rebake_live();
        flat_refresh_profile_end(PROF_REBAKE);
        if (g_flat_refresh_profile_frame.active)
            g_flat_refresh_profile_frame.rebake_sampled = 1;
        if (rebaked)
            return REPL_FLAT_REFRESH_REBAKE;
    }

    /* A failed rebake restored the live value tables. Rebuilding here,
     * before returning through the public boundary, also overwrites any
     * command arguments changed earlier in that failed walk. */
    prof_begin(PROF_FLATTEN);
    repl_flatten_commands(edit_line_idx);
    repl_state_flat_program_clear_dirty();
    flat_refresh_profile_end(PROF_FLATTEN);
    if (g_flat_refresh_profile_frame.active)
        g_flat_refresh_profile_frame.flatten_sampled = 1;
    return REPL_FLAT_REFRESH_FULL;
}

ReplFlatRefreshKind repl_refresh_flat_program(int edit_line_idx) {
    return flatten_refresh_live(
        edit_line_idx,
        repl_state_flat_program_dirty(),
        repl_state_flat_program_args_dirty_mask() != 0);
}

ReplFlatRefreshKind repl_refresh_flat_program_for_deps(
    int edit_line_idx, ReplExprDepMask changed_deps) {
    ReplExprDepMask structural =
        repl_state_flat_program_structural_dep_mask();
    ReplExprDepMask values = repl_state_flat_program_value_dep_mask();

    if (changed_deps == 0 || ((structural | values) & changed_deps) == 0)
        return REPL_FLAT_REFRESH_NONE;
    return flatten_refresh_live(
        edit_line_idx,
        (structural & changed_deps) != 0 ||
            !repl_state_flat_program_rebake_ok(),
        (values & changed_deps) != 0);
}

void repl_ensure_flat_program_with_live_vars(int edit_line_idx) {
    /* Full dirty wins (source edit / structural value change); otherwise a
     * value-only change re-bakes in place; otherwise nothing. Both the
     * full flatten and the rebake mutate the live predef/scratch tables as
     * they thread assignments, so both run inside a save/restore of the
     * caller's live values. A rebake that fails restores the baseline
     * itself and returns 0 — we fall through to the full flatten. */
    if (!repl_state_flat_program_dirty() &&
        !repl_state_flat_program_args_dirty_mask())
        return;

    float live_predef_vals[MAX_PREDEF_VARS] = { 0 };
    float live_scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN] = { { 0.0f } };
    repl_copy_predef_values(live_predef_vals, MAX_PREDEF_VARS);
    repl_eval_copy_scratch_arrays(live_scratch_arrays);

    repl_refresh_flat_program(edit_line_idx);

    repl_restore_predef_values(live_predef_vals, MAX_PREDEF_VARS);
    repl_eval_restore_scratch_arrays(live_scratch_arrays);
}
