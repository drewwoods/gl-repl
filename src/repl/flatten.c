/*
 * src/repl/flatten.c -- Source-to-flat command expansion and flat cursor mapping.
 */
#include "repl/core.h"
#include "repl/core_internal.h"
#include "repl/parser.h"
#include "repl/state_owners.h"
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
    SourceTextView  text;             /* editor source-text view for inline expansion */
    int               max_call_depth;
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

    if (ctx->flat_count >= ctx->flat_capacity) {
        flatten_fail(ctx, "Flattened command limit reached");
        return 0;
    }

    flat_cmd_idx = ctx->flat_count++;
    ctx->flat_cmds[flat_cmd_idx] = *cmd;
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

static int flatten_source_cmd_is_flat_omitted(CmdType type) {
    return repl_cmd_is_block_head(type) ||
           repl_cmd_is_block_end(type) ||
           type == CMD_CALL ||
           type == CMD_COMMENT ||
           type == CMD_EMPTY ||
           type == CMD_VAR_DECLARE;
}

/* Determine which flat-command range corresponds to the innermost
 * glBegin/glEnd block containing edit_line_idx. The result is stored
 * in g_current_block_begin / g_current_block_end and used by
 * repl_flat_cmd_matches_cursor() to highlight the active geometry
 * batch in the 3D view.
 *
 * edit_line_idx is supplied by the caller (β: REPL pipeline does not
 * call editor_state_*), implemented in phase 3.6.2 (see the
 * edit-line-ownership plan doc). */
void repl_flatten_refresh_current_block_highlight(int edit_line_idx) {
    const GLCmd *flat_cmds = repl_state_flat_program_cmds();
    int flat_cmd_count = repl_state_flat_program_count();
    int current_block_begin = -1;
    int current_block_end = -1;

    /* Scan repl_state_document_cmds() alongside g_flat_cmds to find the
     * innermost BEGIN/END block (in flat-cmd indices) that contains
     * edit_line_idx in source-cmd space. Skips structural and
     * document-only rows that don't appear in the flat stream. */
    {
        int begin_src = -1, begin_flat = -1;
        int fcur = 0;
        const GLCmd *document_cmds = repl_state_document_cmds();
        for (int ci = 0; ci < repl_state_document_count() && fcur < flat_cmd_count; ci++) {
            if (!document_cmds[ci].valid) continue;
            CmdType ct = document_cmds[ci].type;
            if (flatten_source_cmd_is_flat_omitted(ct))
                continue;
            while (fcur < flat_cmd_count && !flat_cmds[fcur].valid) fcur++;
            if (fcur >= flat_cmd_count) break;
            if (document_cmds[ci].type == CMD_BEGIN) {
                if (ci <= edit_line_idx) { begin_src = ci; begin_flat = fcur; }
            } else if (document_cmds[ci].type == CMD_END) {
                if (begin_src >= 0 && ci > edit_line_idx) {
                    current_block_begin = begin_flat;
                    current_block_end = fcur;
                    break;
                } else if (begin_src >= 0 && ci <= edit_line_idx) {
                    begin_src = -1; begin_flat = -1;
                }
            }
            fcur++;
        }
    }

    repl_state_flat_program_set_current_block(current_block_begin,
                                              current_block_end,
                                              edit_line_idx);
}

static void flatten_range(FlattenContext *ctx,
                          int start, int end_idx, ExprVar *vars, int nv,
                          int call_src_cmd_idx, int root_call_src_cmd_idx,
                          unsigned int func_scope_mask);

static void flatten_for_loop(FlattenContext *ctx,
                             const GLCmd *src_cmd, int i,
                             ExprVar *vars, int nv,
                             int call_src_cmd_idx, int root_call_src_cmd_idx,
                             unsigned int func_scope_mask,
                             int loop_end) {
    char var_name[REPL_PREDEF_NAME_MAX];
    float start_val = src_cmd->args[0];
    float end_val   = src_cmd->args[1];
    float step_val  = src_cmd->args[2];
    const char *src_text = flatten_src_text(ctx->text, i);
    flatten_get_for_var_name(ctx->text, i, var_name, sizeof(var_name));

    if (src_cmd->has_vars) {
        const char *unused_body;
        float re_start, re_end, re_step;
        char rv[16];
        if (repl_eval_parse_for_header_with_vars(src_text, rv, sizeof(rv),
                                       &re_start, &re_end, &re_step,
                                       vars, nv, &unused_body)) {
            start_val = re_start;
            end_val   = re_end;
            step_val  = re_step;
        }
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
        int lnv = 0;
        repl_copy_string_fits(lvars[lnv].name,
                              sizeof(lvars[lnv].name),
                              var_name);
        lvars[lnv].value = val;
        lnv++;
        if (vars)
            for (int v = 0; v < nv && lnv < MAX_EXPR_VARS; v++)
                lvars[lnv++] = vars[v];
        flatten_range(ctx, i + 1, loop_end, lvars, lnv,
                      call_src_cmd_idx, root_call_src_cmd_idx,
                      func_scope_mask);
    }
}

static void flatten_call(FlattenContext *ctx,
                         const GLCmd *src_cmd, int i,
                         ExprVar *vars, int nv,
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
        int arg_count = 0;
        const char *def_text = flatten_src_text(ctx->text, k);
        const char *call_text = flatten_src_text(ctx->text, i);

        if (!parse_repl_func_signature(def_text, &def_fn,
                                       param_names, MAX_EXPR_VARS,
                                       &param_count))
            break;
        if (!extract_func_call_args_text(call_text, NULL,
                                         arg_text, sizeof(arg_text)))
            break;
        if (!parse_expr_list_exact(arg_text, arg_vals, MAX_EXPR_VARS,
                                   vars, nv, &arg_count))
            break;
        if (arg_count != param_count) {
            char msg[REPL_DIAG_TEXT_MAX];
            snprintf(msg, sizeof(msg),
                     "func%d expects %d args, got %d",
                     func_num, param_count, arg_count);
            flatten_note_status(ctx, msg);
            break;
        }

        ExprVar lvars[MAX_EXPR_VARS];
        int lnv = 0;
        for (int p = 0; p < param_count && lnv < MAX_EXPR_VARS; p++) {
            repl_copy_string_fits(lvars[lnv].name,
                                  sizeof(lvars[lnv].name),
                                  param_names[p]);
            lvars[lnv].value = arg_vals[p];
            lnv++;
        }
        for (int v = 0; vars && v < nv && lnv < MAX_EXPR_VARS; v++)
            lvars[lnv++] = vars[v];

        unsigned int nested_func_mask = func_scope_mask;
        if (func_num >= 0 && func_num < FUNC_SCOPE_MASK_BITS)
            nested_func_mask |= (1u << func_num);
        int nested_root_call = (root_call_src_cmd_idx >= 0)
                             ? root_call_src_cmd_idx : i;

        ctx->call_depth++;
        flatten_range(ctx, k + 1, body_end, lvars, lnv,
                      i, nested_root_call, nested_func_mask);
        if (ctx->call_depth > 0) ctx->call_depth--;
    } while (0);
}

static void flatten_if_block(FlattenContext *ctx,
                             const GLCmd *src_cmd, int i,
                             ExprVar *vars, int nv,
                             int call_src_cmd_idx, int root_call_src_cmd_idx,
                             unsigned int func_scope_mask,
                             int if_end) {
    /* Flatten-time eval decides whether to unroll the body into the
     * flat program. The executor re-evaluates per iteration so goto
     * loops back into the body see updated vars — same kernel, two
     * different times; see repl_eval_if_condition's doc. */
    float cond = repl_eval_if_condition(flatten_src_text(ctx->text, i),
                                        vars, nv, src_cmd->args[0]);

    if (cond != 0.0f)
        flatten_range(ctx, i + 1, if_end, vars, nv,
                      call_src_cmd_idx, root_call_src_cmd_idx,
                      func_scope_mask);
}

/* Re-parse a source line into the flat buffer, evaluating expressions
 * against the current variable bindings.  Three paths converge here:
 *   1. local vars present  → pass vars to the parser, keep src has_vars
 *   2. no local vars but src has predefined-var refs → re-eval, force has_vars=1
 *   3. no vars at all      → re-parse for fresh args, clear has_vars
 * On parse failure in path 3, the original src_cmd is copied through
 * unchanged (the line was already invalid at commit time).
 * Returns 1 on success, 0 if the flat buffer overflowed (caller should
 * return immediately). Single-exit so the PROF_FLATTEN_REPARSE probe is
 * one begin/accum_end pair spanning the whole reparse. */
static int flatten_reparse_line(FlattenContext *ctx,
                                const GLCmd *src_cmd, int i,
                                ExprVar *vars, int nv,
                                int call_src_cmd_idx,
                                int root_call_src_cmd_idx,
                                unsigned int func_scope_mask) {
    char parse_err[REPL_STATUS_TEXT_MAX];
    parse_err[0] = '\0';

    int has_local_vars = (vars && nv > 0);
    ReplParseContext parse_ctx = {
        .source_line_idx = i,
        .vars = has_local_vars ? vars : NULL,
        .num_vars = has_local_vars ? nv : 0,
        .err_buf = parse_err,
        .err_sz  = (int)sizeof(parse_err),
        /* Flatten only reads tmp_pl.cmd; the canonical text rendering
         * (per-arg %g/snprintf) is pure waste on every frame. */
        .skip_text = 1,
    };
    const char *text = flatten_src_text(ctx->text, i);
    ReplParsedLine tmp_pl;
    int rv = 1;

    prof_begin(PROF_FLATTEN_REPARSE);
    if (repl_parser_parse_command_ctx(text, &tmp_pl, &parse_ctx)) {
        GLCmd tmp = tmp_pl.cmd;
        if (has_local_vars)
            tmp.has_vars = src_cmd->has_vars;
        else if (src_cmd->has_vars)
            tmp.has_vars = 1;
        else {
            tmp.has_vars = 0;
            tmp.is_auto = src_cmd->is_auto;
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
static int flatten_var_assign(FlattenContext *ctx, const GLCmd *src_cmd, int i,
                              ExprVar *vars, int nv, int call_src_cmd_idx,
                              int root_call_src_cmd_idx,
                              unsigned int func_scope_mask) {
    prof_begin(PROF_FLATTEN_VAR_ASSIGN);
    int rv = 1;
    int var_idx = src_cmd->var_idx; /* predef var slot */
    float value = src_cmd->args[0];
    char rhs[MAX_LINE_LEN] = "";
    int local_rhs_vars = 0;
    const char *src_text = flatten_src_text(ctx->text, i);

    if (repl_extract_assignment_parts(src_text, NULL, 0,
                                      rhs, sizeof(rhs)) && rhs[0]) {
        char repl_rhs[MAX_LINE_LEN];
        repl_eval_c_expr_to_repl(rhs, repl_rhs, sizeof(repl_rhs));
        ExprCtx expr_ctx = { repl_rhs, vars, nv, NULL, 0 };
        value = repl_eval_expr(&expr_ctx);
        if (vars && nv > 0)
            local_rhs_vars = input_has_expr_vars(rhs, vars, nv);
    }
    if (var_idx >= 0 && var_idx < g_num_predef_vars)
        g_predef_vars_mut[var_idx].value = value;
    {
        GLCmd tmp = *src_cmd;
        tmp.args[0] = value;
        tmp.has_vars = src_cmd->has_vars || local_rhs_vars;
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
                                  int i, ExprVar *vars, int nv,
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
    const char *src_text = flatten_src_text(ctx->text, i);

    if (repl_extract_assignment_target_parts(src_text,
                                            name, sizeof(name),
                                            index_expr, sizeof(index_expr),
                                            rhs, sizeof(rhs))) {
        char repl_index[MAX_LINE_LEN];
        char repl_rhs[MAX_LINE_LEN];
        repl_eval_c_expr_to_repl(index_expr, repl_index, sizeof(repl_index));
        repl_eval_c_expr_to_repl(rhs, repl_rhs, sizeof(repl_rhs));

        ExprCtx index_ctx = { repl_index, vars, nv, NULL, 0 };
        ExprCtx rhs_ctx = { repl_rhs, vars, nv, NULL, 0 };
        elem_idx = (int)repl_eval_expr(&index_ctx);
        value = repl_eval_expr(&rhs_ctx);
        if (vars && nv > 0) {
            local_index_vars = input_has_expr_vars(index_expr, vars, nv);
            local_rhs_vars = input_has_expr_vars(rhs, vars, nv);
        }
    }

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
 * enclosing scopes. */
static void flatten_range(FlattenContext *ctx,
                          int start, int end_idx, ExprVar *vars, int nv,
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
            flatten_for_loop(ctx, src_cmd, i, vars, nv,
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
            flatten_call(ctx, src_cmd, i, vars, nv,
                         root_call_src_cmd_idx, func_scope_mask);
            i++;
            continue;
        }

        if (src_cmd->type == CMD_IF_BEGIN) {
            int if_end = flatten_repl_source_scope_find_block_end(ctx, i);
            flatten_if_block(ctx, src_cmd, i, vars, nv,
                             call_src_cmd_idx, root_call_src_cmd_idx,
                             func_scope_mask, if_end);
            i = (if_end < ctx->source_count) ? if_end + 1 : ctx->source_count;
            continue;
        }

        if (src_cmd->type == CMD_IF_END) {
            i++;
            continue;
        }

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
            if (!flatten_var_assign(ctx, src_cmd, i, vars, nv,
                                    call_src_cmd_idx, root_call_src_cmd_idx,
                                    func_scope_mask))
                return;
            i++;
            continue;
        }

        if (src_cmd->type == CMD_SCRATCH_ASSIGN) {
            if (!flatten_scratch_assign(ctx, src_cmd, i, vars, nv,
                                        call_src_cmd_idx, root_call_src_cmd_idx,
                                        func_scope_mask))
                return;
            i++;
            continue;
        }

        if (!flatten_reparse_line(ctx, src_cmd, i, vars, nv,
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
        .text = options ? options->text : (SourceTextView){0},
        .max_call_depth = options && options->max_call_depth > 0
                        ? options->max_call_depth : MAX_FLATTEN_CALL_DEPTH,
        .call_depth = 0,
        .abort = 0,
        .visit_budget = options && options->visit_budget > 0
                      ? options->visit_budget : MAX_FLATTEN_VISIT_BUDGET
    };

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
    prof_accum_reset(PROF_FLATTEN_REPARSE);
    prof_accum_reset(PROF_FLATTEN_VAR_ASSIGN);
    prof_accum_reset(PROF_FLATTEN_SCRATCH_ASSIGN);
    flatten_range(&ctx, 0, ctx.source_count, NULL, 0, -1, -1, 0);
    prof_accum_commit(PROF_FLATTEN_REPARSE);
    prof_accum_commit(PROF_FLATTEN_VAR_ASSIGN);
    prof_accum_commit(PROF_FLATTEN_SCRATCH_ASSIGN);
    if (ctx.abort)
        ctx.flat_count = 0;

    result->ok = !ctx.abort;
    result->flat_cmd_count = ctx.flat_count;
    result->user_lighting_enabled =
        flatten_flat_lighting_enabled(ctx.flat_cmds, ctx.flat_count);
    repl_copy_string_fits(result->status, sizeof(result->status), ctx.status);
    return result->ok;
}

void repl_flatten_commands(int edit_line_idx) {
    ReplFlatProgramState *flat_program = repl_state_flat_program_mut();
    ReplFlattenOptions options = {
        .source_cmds = repl_state_document_cmds(),
        .source_cmd_count = repl_state_document_count(),
        .flat_cmds = flat_program->cmds,
        .flat_local_vars = flat_program->local_vars,
        .flat_capacity = flat_program->capacity,
        .text = source_document_view(),
        .max_call_depth = MAX_FLATTEN_CALL_DEPTH,
        .visit_budget = MAX_FLATTEN_VISIT_BUDGET
    };
    ReplFlattenResult result;

    repl_flatten_program(&options, &result);
    repl_state_flat_program_set_count(result.flat_cmd_count);
    repl_state_flat_program_set_user_lighting_enabled(
        result.user_lighting_enabled);
    if (result.status[0])
        repl_set_status_error(result.status);

    repl_flatten_refresh_current_block_highlight(edit_line_idx);
}

void repl_ensure_flat_program_with_live_vars(int edit_line_idx) {
    if (repl_state_flat_program_dirty()) {
        float live_predef_vals[MAX_PREDEF_VARS] = { 0 };
        float live_scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN] = { { 0.0f } };
        repl_copy_predef_values(live_predef_vals, MAX_PREDEF_VARS);
        repl_eval_copy_scratch_arrays(live_scratch_arrays);
        repl_flatten_commands(edit_line_idx);
        repl_state_flat_program_clear_dirty();
        repl_restore_predef_values(live_predef_vals, MAX_PREDEF_VARS);
        repl_eval_restore_scratch_arrays(live_scratch_arrays);
    }
}

static unsigned int line_func_scope_mask(int line) {
    unsigned int mask = 0;
    int stack[FUNC_SCOPE_MASK_BITS];
    int depth = 0;
    const GLCmd *document_cmds = repl_state_document_cmds();

    if (line < 0) return 0;
    if (line >= repl_state_document_count()) line = repl_state_document_count() - 1;

    for (int i = 0; i <= line && i < repl_state_document_count(); i++) {
        if (!document_cmds[i].valid) continue;

        if (document_cmds[i].type == CMD_FUNC_DEF) {
            int fn = (int)document_cmds[i].args[0];
            if (fn >= 0 && fn < FUNC_SCOPE_MASK_BITS &&
                depth < (int)(sizeof(stack) / sizeof(stack[0]))) {
                stack[depth++] = fn;
                mask |= (1u << fn);
            }
            continue;
        }

        if (document_cmds[i].type == CMD_FUNC_END) {
            if (i == line)
                return mask;
            if (depth > 0) {
                int fn = stack[--depth];
                mask &= ~(1u << fn);
            }
        }
    }

    return mask;
}

/* edit_line_idx is supplied by the caller (β: REPL pipeline does
 * not call editor_state_*), implemented in phase 3.6.2 (see the
 * edit-line-ownership plan doc). */
int repl_flat_cmd_matches_cursor(int flat_idx, int edit_line_idx) {
    const GLCmd *flat_cmds = repl_state_flat_program_cmds();
    int flat_cmd_count = repl_state_flat_program_count();
    int current_block_begin = repl_state_flat_program_current_block_begin();
    int current_block_end = repl_state_flat_program_current_block_end();
    int current_block_line = repl_state_flat_program_current_block_source_line();

    if (flat_idx < 0 || flat_idx >= flat_cmd_count) return 0;
    if (edit_line_idx < 0 || edit_line_idx >= repl_state_document_count()) return 0;
    if (!flat_cmds[flat_idx].valid) return 0;
    if (current_block_line != edit_line_idx)
        repl_flatten_refresh_current_block_highlight(edit_line_idx);

    const GLCmd *cmd = &flat_cmds[flat_idx];
    const GLCmd *cursor_cmd = &repl_state_document_cmds()[edit_line_idx];

    if (cursor_cmd->valid && cursor_cmd->type == CMD_CALL) {
        return cmd->call_src_cmd_idx == edit_line_idx ||
               cmd->root_call_src_cmd_idx == edit_line_idx;
    }

    {
        unsigned int cursor_func_mask = line_func_scope_mask(edit_line_idx);
        if (cursor_func_mask != 0)
            return (cmd->func_scope_mask & cursor_func_mask) != 0;
    }

    if (current_block_begin >= 0 && current_block_end >= current_block_begin)
        return flat_idx >= current_block_begin && flat_idx <= current_block_end;

    /* Top-level color/normal commands outside glBegin/glEnd still affect later
     * vertices. Match those vertices to the most recent applicable state line
     * so block highlighting also works when the state is set before the block. */
    switch (cursor_cmd->type) {
    case CMD_COLOR3F:
    case CMD_COLOR4F: {
        /* glColor3f / glColor4f feed glVertex3f and glVertex2f. Tess
         * vertices have their own state line (CMD_TESS_COLOR) handled
         * by a sibling case below, so CMD_TESS_VERTEX is intentionally
         * excluded here. */
        if (cmd->type == CMD_VERTEX3F || cmd->type == CMD_VERTEX2F) {
            int last_color_src = -1;
            for (int i = 0; i <= flat_idx; i++) {
                if (!flat_cmds[i].valid) continue;
                if (flat_cmds[i].type == CMD_COLOR3F ||
                    flat_cmds[i].type == CMD_COLOR4F)
                    last_color_src = flat_cmds[i].src_cmd_idx;
            }
            if (last_color_src == edit_line_idx)
                return 1;
        }
        break;
    }
    case CMD_NORMAL3F: {
        /* glNormal3f feeds glVertex3f and glVertex2f. Tess vertices
         * have their own state line (CMD_TESS_NORMAL) handled by a
         * sibling case below, so CMD_TESS_VERTEX is intentionally
         * excluded here. */
        if (cmd->type == CMD_VERTEX3F || cmd->type == CMD_VERTEX2F) {
            int last_normal_src = -1;
            for (int i = 0; i <= flat_idx; i++) {
                if (!flat_cmds[i].valid) continue;
                if (flat_cmds[i].type == CMD_NORMAL3F)
                    last_normal_src = flat_cmds[i].src_cmd_idx;
            }
            if (last_normal_src == edit_line_idx)
                return 1;
        }
        break;
    }
    case CMD_TESS_COLOR: {
        /* CMD_TESS_COLOR feeds tess vertices only; immediate-mode
         * vertices (CMD_VERTEX3F / CMD_VERTEX2F) are fed by glColor3f
         * / glColor4f handled in the COLOR3F / COLOR4F cases above. */
        if (cmd->type == CMD_TESS_VERTEX) {
            int last_tess_color_src = -1;
            for (int i = 0; i <= flat_idx; i++) {
                if (!flat_cmds[i].valid) continue;
                if (flat_cmds[i].type == CMD_TESS_COLOR)
                    last_tess_color_src = flat_cmds[i].src_cmd_idx;
            }
            if (last_tess_color_src == edit_line_idx)
                return 1;
        }
        break;
    }
    case CMD_TESS_NORMAL: {
        /* CMD_TESS_NORMAL feeds tess vertices only; immediate-mode
         * vertices are fed by glNormal3f handled in the NORMAL3F case
         * above. */
        if (cmd->type == CMD_TESS_VERTEX) {
            int last_tess_normal_src = -1;
            for (int i = 0; i <= flat_idx; i++) {
                if (!flat_cmds[i].valid) continue;
                if (flat_cmds[i].type == CMD_TESS_NORMAL)
                    last_tess_normal_src = flat_cmds[i].src_cmd_idx;
            }
            if (last_tess_normal_src == edit_line_idx)
                return 1;
        }
        break;
    }
    default:
        break;
    }

    return cmd->src_cmd_idx == edit_line_idx;
}
