/*
 * repl_flatten.c -- Source-to-flat command expansion and flat cursor mapping.
 */
#include "sample.h"
#include "repl_core.h"
#include "repl_core_internal.h"

#define MAX_FLATTEN_CALL_DEPTH 64
#define MAX_FLATTEN_VISIT_BUDGET 200000

int g_current_block_begin = -1; /* flat cmd index of cursor's glBegin */
int g_current_block_end   = -1; /* flat cmd index of cursor's glEnd */
static int g_current_block_line = -1; /* g_edit_line used to compute block */

typedef struct {
    const GLCmd      *source_cmds;
    int               source_count;
    GLCmd            *flat_cmds;
    FlatCmdLocalVars *flat_local_vars;
    int               flat_capacity;
    int               flat_count;
    int               max_call_depth;
    int call_depth;
    int abort;
    int visit_budget;
    char status[128];
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

static void flatten_get_for_var_name(const GLCmd *cmd, char *var, int var_sz) {
    const char *p = cmd->source;
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
 *   src_cmd_idx          -- the g_cmds[] line this command came from
 *   call_src_cmd_idx     -- the funcN() call site that triggered expansion
 *                          (-1 if top-level)
 *   root_call_src_cmd_idx-- the outermost call site in nested func calls
 *                          (-1 if top-level)
 *   func_scope_mask      -- bitmask of which func bodies this cmd is inside */
static void flat_cmd_set_provenance(GLCmd *cmd, int src_cmd_idx,
                                    int call_src_cmd_idx,
                                    int root_call_src_cmd_idx,
                                    unsigned int func_scope_mask) {
    cmd->src_cmd_idx = src_cmd_idx;
    cmd->call_src_cmd_idx = call_src_cmd_idx;
    cmd->root_call_src_cmd_idx = root_call_src_cmd_idx;
    cmd->func_scope_mask = func_scope_mask;
}

static int flatten_find_block_end(const FlattenContext *ctx, int begin_idx) {
    int depth = 1;

    for (int j = begin_idx + 1; j < ctx->source_count; j++) {
        CmdType t = ctx->source_cmds[j].type;
        if (t == CMD_FOR_BEGIN || t == CMD_FUNC_DEF || t == CMD_IF_BEGIN)
            depth++;
        else if (t == CMD_FOR_END || t == CMD_FUNC_END || t == CMD_IF_END) {
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
                            root_call_src_cmd_idx, func_scope_mask);

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

/* Determine which flat-command range corresponds to the innermost
 * glBegin/glEnd block containing g_edit_line. The result is stored in
 * g_current_block_begin / g_current_block_end and used by
 * repl_flat_cmd_matches_cursor() to highlight the active geometry
 * batch in the 3D view. */
static void refresh_current_block_highlight(void) {
    g_current_block_begin = -1;
    g_current_block_end   = -1;
    g_current_block_line  = g_edit_line;

    /* Scan g_cmds alongside g_flat_cmds to find the innermost
     * BEGIN/END block (in flat-cmd indices) that contains g_edit_line
     * in source-cmd space. Skips for/func/if structural commands that
     * don't appear in the flat stream. */
    {
        int begin_src = -1, begin_flat = -1;
        int fcur = 0;
        for (int ci = 0; ci < g_num_cmds && fcur < g_num_flat_cmds; ci++) {
            if (!g_cmds[ci].valid) continue;
            if (g_cmds[ci].type == CMD_FUNC_DEF || g_cmds[ci].type == CMD_FUNC_END ||
                g_cmds[ci].type == CMD_FOR_BEGIN || g_cmds[ci].type == CMD_FOR_END ||
                g_cmds[ci].type == CMD_IF_BEGIN  || g_cmds[ci].type == CMD_IF_END  ||
                g_cmds[ci].type == CMD_CALL)
                continue;
            while (fcur < g_num_flat_cmds && !g_flat_cmds[fcur].valid) fcur++;
            if (fcur >= g_num_flat_cmds) break;
            if (g_cmds[ci].type == CMD_BEGIN) {
                if (ci <= g_edit_line) { begin_src = ci; begin_flat = fcur; }
            } else if (g_cmds[ci].type == CMD_END) {
                if (begin_src >= 0 && ci > g_edit_line) {
                    g_current_block_begin = begin_flat;
                    g_current_block_end   = fcur;
                    break;
                } else if (begin_src >= 0 && ci <= g_edit_line) {
                    begin_src = -1; begin_flat = -1;
                }
            }
            fcur++;
        }
    }
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
            int loop_end = flatten_find_block_end(ctx, i);
            char var_name[16];
            float start_val = src_cmd->args[0];
            float end_val   = src_cmd->args[1];
            float step_val  = src_cmd->args[2];
            flatten_get_for_var_name(src_cmd, var_name, sizeof(var_name));

            /* Re-evaluate for-loop bounds from source if they contain variables */
            if (src_cmd->has_vars) {
                const char *unused_body;
                float re_start, re_end, re_step;
                char rv[16];
                if (parse_for_header_with_vars(src_cmd->source, rv, sizeof(rv),
                                               &re_start, &re_end, &re_step,
                                               vars, nv, &unused_body)) {
                    start_val = re_start;
                    end_val   = re_end;
                    step_val  = re_step;
                }
            }

            if (fabsf(step_val) > 1e-9f &&
                !((step_val > 0 && start_val >= end_val) ||
                  (step_val < 0 && start_val <= end_val))) {
                int max_iters = 100000;
                for (float val = start_val;
                     (step_val > 0) ? (val < end_val - 1e-6f) : (val > end_val + 1e-6f);
                     val += step_val) {
                    if (--max_iters < 0) break;
                    if (ctx->abort) return;
                    ExprVar lvars[MAX_EXPR_VARS];
                    int lnv = 0;
                    if (lnv < MAX_EXPR_VARS) {
                        repl_copy_string_fits(lvars[lnv].name,
                                              sizeof(lvars[lnv].name),
                                              var_name);
                        lvars[lnv].value = val;
                        lnv++;
                    }
                    if (vars)
                        for (int v = 0; v < nv && lnv < MAX_EXPR_VARS; v++)
                            lvars[lnv++] = vars[v];
                    flatten_range(ctx, i + 1, loop_end, lvars, lnv,
                                  call_src_cmd_idx, root_call_src_cmd_idx,
                                  func_scope_mask);
                }
            }
            i = (loop_end < ctx->source_count) ? loop_end + 1 : ctx->source_count;
            continue;
        }

        if (src_cmd->type == CMD_FOR_END) { i++; continue; }

        /* Function definitions: skip body (expanded at call sites) */
        if (src_cmd->type == CMD_FUNC_DEF) {
            int func_end = flatten_find_block_end(ctx, i);
            i = (func_end < ctx->source_count) ? func_end + 1 : ctx->source_count;
            continue;
        }
        if (src_cmd->type == CMD_FUNC_END) { i++; continue; }

        /* Function calls: find definition and expand body inline */
        if (src_cmd->type == CMD_CALL) {
            int func_num = (int)src_cmd->args[0];
            if (ctx->call_depth >= ctx->max_call_depth) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "Recursive expansion exceeded depth limit (%d) at func%d",
                         ctx->max_call_depth, func_num);
                flatten_fail(ctx, msg);
                i++;
                continue;
            }

            int def_found = 0;
            for (int k = 0; k < ctx->source_count; k++) {
                const GLCmd *def_cmd = &ctx->source_cmds[k];
                if (def_cmd->type == CMD_FUNC_DEF && (int)def_cmd->args[0] == func_num) {
                    def_found = 1;
                    int body_end = flatten_find_block_end(ctx, k);
                    int def_fn = func_num;
                    int param_count = 0;
                    char param_names[MAX_EXPR_VARS][16];
                    char arg_text[MAX_LINE_LEN];
                    float arg_vals[MAX_EXPR_VARS];
                    int arg_count = 0;

                    if (!parse_repl_func_signature(def_cmd->source, &def_fn,
                                                   param_names, MAX_EXPR_VARS,
                                                   &param_count))
                        break;
                    if (!extract_func_call_args_text(src_cmd->source, NULL,
                                                     arg_text, sizeof(arg_text)))
                        break;
                    if (!parse_expr_list_exact(arg_text, arg_vals, MAX_EXPR_VARS,
                                               vars, nv, &arg_count))
                        break;
                    if (arg_count != param_count) {
                        char msg[128];
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
                    if (func_num >= 0 && func_num < 32)
                        nested_func_mask |= (1u << func_num);
                    int nested_root_call = (root_call_src_cmd_idx >= 0)
                                         ? root_call_src_cmd_idx : i;

                    ctx->call_depth++;
                    flatten_range(ctx, k + 1, body_end, lvars, lnv,
                                  i, nested_root_call, nested_func_mask);
                    if (ctx->call_depth > 0) ctx->call_depth--;
                    break;
                }
            }
            if (!def_found) {
                char msg[64];
                snprintf(msg, sizeof(msg),
                         "func%d not defined", func_num);
                flatten_note_status(ctx, msg);
            }
            i++;
            continue;
        }

        if (src_cmd->type == CMD_IF_BEGIN) {
            int if_end = flatten_find_block_end(ctx, i);
            char cond_text[MAX_LINE_LEN];
            int needs_local_eval = 0;

            if (vars && nv > 0 &&
                repl_extract_paren_payload(src_cmd->source, cond_text, sizeof(cond_text)) &&
                (input_has_expr_vars(cond_text, vars, nv) ||
                 input_has_predef_vars(cond_text))) {
                needs_local_eval = 1;
            }

            if (needs_local_eval) {
                char repl_cond[MAX_LINE_LEN];
                c_expr_to_repl(cond_text, repl_cond, sizeof(repl_cond));
                ExprCtx expr_ctx = { repl_cond, vars, nv };
                float cond = eval_expr(&expr_ctx);
                if (cond != 0.0f)
                    flatten_range(ctx, i + 1, if_end, vars, nv,
                                  call_src_cmd_idx, root_call_src_cmd_idx,
                                  func_scope_mask);
                i = (if_end < ctx->source_count) ? if_end + 1 : ctx->source_count;
                continue;
            }

            if (!flatten_append_cmd(ctx, src_cmd, i, call_src_cmd_idx,
                                    root_call_src_cmd_idx, func_scope_mask,
                                    NULL, 0))
                return;
            i++;
            continue;
        }

        if (src_cmd->type == CMD_IF_END) {
            if (!flatten_append_cmd(ctx, src_cmd, i, call_src_cmd_idx,
                                    root_call_src_cmd_idx, func_scope_mask,
                                    NULL, 0))
                return;
            i++;
            continue;
        }

        if ((src_cmd->type == CMD_LABEL || src_cmd->type == CMD_GOTO) &&
            func_scope_mask != 0) {
            flatten_fail(ctx, "goto and labels are not supported inside functions");
            return;
        }

        /* Comments: pass through to flat array (skipped by execute, kept in save) */
        if (src_cmd->type == CMD_COMMENT) {
            if (!flatten_append_cmd(ctx, src_cmd, i, call_src_cmd_idx,
                                    root_call_src_cmd_idx, func_scope_mask,
                                    NULL, 0))
                return;
            i++;
            continue;
        }

        if (src_cmd->type == CMD_VAR_DECLARE) { i++; continue; }

        /* Variable assignments: update predefined var and pass through */
        if (src_cmd->type == CMD_VAR_ASSIGN) {
            int var_idx = src_cmd->num_args; /* predef var index */
            float value = src_cmd->args[0];
            char rhs[MAX_LINE_LEN] = "";
            int local_rhs_vars = 0;

            if (repl_extract_assignment_parts(src_cmd->source, NULL, 0,
                                              rhs, sizeof(rhs)) && rhs[0]) {
                char repl_rhs[MAX_LINE_LEN];
                c_expr_to_repl(rhs, repl_rhs, sizeof(repl_rhs));
                ExprCtx expr_ctx = { repl_rhs, vars, nv };
                value = eval_expr(&expr_ctx);
                if (vars && nv > 0)
                    local_rhs_vars = input_has_expr_vars(rhs, vars, nv);
            }
            if (var_idx >= 0 && var_idx < g_num_predef_vars)
                g_predef_vars[var_idx].value = value;
            {
                GLCmd tmp = *src_cmd;
                tmp.args[0] = value;
                tmp.has_vars = src_cmd->has_vars || local_rhs_vars;
                if (!flatten_append_cmd(ctx, &tmp, i, call_src_cmd_idx,
                                        root_call_src_cmd_idx, func_scope_mask,
                                        vars, nv))
                    return;
            }
            i++;
            continue;
        }

        if (vars && nv > 0) {
            GLCmd tmp;
            ReplParseContext parse_ctx = { i, vars, nv };
            memset(&tmp, 0, sizeof(tmp));
            if (repl_parse_command_ctx(src_cmd->source, &tmp, &parse_ctx)) {
                tmp.has_vars = src_cmd->has_vars;
                strncpy(tmp.source, src_cmd->source, sizeof(tmp.source) - 1);
                tmp.source[sizeof(tmp.source) - 1] = '\0';
                if (!flatten_append_cmd(ctx, &tmp, i, call_src_cmd_idx,
                                        root_call_src_cmd_idx, func_scope_mask,
                                        vars, nv))
                    return;
            }
        } else if (src_cmd->has_vars) {
            /* Outside loop but has predefined var references: re-evaluate */
            GLCmd tmp;
            ReplParseContext parse_ctx = { i, NULL, 0 };
            memset(&tmp, 0, sizeof(tmp));
            if (repl_parse_command_ctx(src_cmd->source, &tmp, &parse_ctx)) {
                tmp.has_vars = 1;
                strncpy(tmp.source, src_cmd->source, sizeof(tmp.source) - 1);
                tmp.source[sizeof(tmp.source) - 1] = '\0';
                if (!flatten_append_cmd(ctx, &tmp, i, call_src_cmd_idx,
                                        root_call_src_cmd_idx, func_scope_mask,
                                        NULL, 0))
                    return;
            }
        } else {
            if (!flatten_append_cmd(ctx, src_cmd, i, call_src_cmd_idx,
                                    root_call_src_cmd_idx, func_scope_mask,
                                    NULL, 0))
                return;
        }
        i++;
    }
}

static int flatten_source_lighting_enabled(const GLCmd *source_cmds,
                                           int source_count) {
    int user_lighting_enabled = 0;

    for (int i = 0; i < source_count; i++) {
        if (source_cmds[i].valid && source_cmds[i].type == CMD_ENABLE &&
            source_cmds[i].mode == GL_LIGHTING)
            user_lighting_enabled = 1;
        if (source_cmds[i].valid && source_cmds[i].type == CMD_DISABLE &&
            source_cmds[i].mode == GL_LIGHTING)
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

    flatten_range(&ctx, 0, ctx.source_count, NULL, 0, -1, -1, 0);
    if (ctx.abort)
        ctx.flat_count = 0;

    result->ok = !ctx.abort;
    result->flat_cmd_count = ctx.flat_count;
    result->user_lighting_enabled =
        flatten_source_lighting_enabled(ctx.source_cmds, ctx.source_count);
    repl_copy_string_fits(result->status, sizeof(result->status), ctx.status);
    return result->ok;
}

void flatten_commands(void) {
    ReplFlattenOptions options = {
        .source_cmds = g_cmds,
        .source_cmd_count = g_num_cmds,
        .flat_cmds = g_flat_cmds,
        .flat_local_vars = g_flat_cmd_local_vars,
        .flat_capacity = MAX_COMMANDS,
        .max_call_depth = MAX_FLATTEN_CALL_DEPTH,
        .visit_budget = MAX_FLATTEN_VISIT_BUDGET
    };
    ReplFlattenResult result;

    repl_flatten_program(&options, &result);
    g_num_flat_cmds = result.flat_cmd_count;
    g_user_lighting_enabled = result.user_lighting_enabled;
    if (result.status[0])
        set_status(result.status);

    refresh_current_block_highlight();
}

static unsigned int line_func_scope_mask(int line) {
    unsigned int mask = 0;
    int stack[32];
    int depth = 0;

    if (line < 0) return 0;
    if (line >= g_num_cmds) line = g_num_cmds - 1;

    for (int i = 0; i <= line && i < g_num_cmds; i++) {
        if (!g_cmds[i].valid) continue;

        if (g_cmds[i].type == CMD_FUNC_DEF) {
            int fn = (int)g_cmds[i].args[0];
            if (fn >= 0 && fn < 32 && depth < (int)(sizeof(stack) / sizeof(stack[0]))) {
                stack[depth++] = fn;
                mask |= (1u << fn);
            }
            continue;
        }

        if (g_cmds[i].type == CMD_FUNC_END) {
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

int repl_flat_cmd_matches_cursor(int flat_idx) {
    if (flat_idx < 0 || flat_idx >= g_num_flat_cmds) return 0;
    if (g_edit_line < 0 || g_edit_line >= g_num_cmds) return 0;
    if (!g_flat_cmds[flat_idx].valid) return 0;
    if (g_current_block_line != g_edit_line)
        refresh_current_block_highlight();

    GLCmd *cmd = &g_flat_cmds[flat_idx];
    GLCmd *cursor_cmd = &g_cmds[g_edit_line];

    if (cursor_cmd->valid && cursor_cmd->type == CMD_CALL) {
        return cmd->call_src_cmd_idx == g_edit_line ||
               cmd->root_call_src_cmd_idx == g_edit_line;
    }

    {
        unsigned int cursor_func_mask = line_func_scope_mask(g_edit_line);
        if (cursor_func_mask != 0)
            return (cmd->func_scope_mask & cursor_func_mask) != 0;
    }

    if (g_current_block_begin >= 0 && g_current_block_end >= g_current_block_begin)
        return flat_idx >= g_current_block_begin && flat_idx <= g_current_block_end;

    /* Top-level color/normal commands outside glBegin/glEnd still affect later
     * vertices. Match those vertices to the most recent applicable state line
     * so block highlighting also works when the state is set before the block. */
    switch (cursor_cmd->type) {
    case CMD_COLOR3F:
    case CMD_COLOR4F: {
        if (cmd->type == CMD_VERTEX3F) {
            int last_color_src = -1;
            for (int i = 0; i <= flat_idx; i++) {
                if (!g_flat_cmds[i].valid) continue;
                if (g_flat_cmds[i].type == CMD_COLOR3F ||
                    g_flat_cmds[i].type == CMD_COLOR4F)
                    last_color_src = g_flat_cmds[i].src_cmd_idx;
            }
            if (last_color_src == g_edit_line)
                return 1;
        }
        break;
    }
    case CMD_NORMAL3F: {
        if (cmd->type == CMD_VERTEX3F) {
            int last_normal_src = -1;
            for (int i = 0; i <= flat_idx; i++) {
                if (!g_flat_cmds[i].valid) continue;
                if (g_flat_cmds[i].type == CMD_NORMAL3F)
                    last_normal_src = g_flat_cmds[i].src_cmd_idx;
            }
            if (last_normal_src == g_edit_line)
                return 1;
        }
        break;
    }
    case CMD_TESS_COLOR: {
        if (cmd->type == CMD_TESS_VERTEX) {
            int last_tess_color_src = -1;
            for (int i = 0; i <= flat_idx; i++) {
                if (!g_flat_cmds[i].valid) continue;
                if (g_flat_cmds[i].type == CMD_TESS_COLOR)
                    last_tess_color_src = g_flat_cmds[i].src_cmd_idx;
            }
            if (last_tess_color_src == g_edit_line)
                return 1;
        }
        break;
    }
    case CMD_TESS_NORMAL: {
        if (cmd->type == CMD_TESS_VERTEX) {
            int last_tess_normal_src = -1;
            for (int i = 0; i <= flat_idx; i++) {
                if (!g_flat_cmds[i].valid) continue;
                if (g_flat_cmds[i].type == CMD_TESS_NORMAL)
                    last_tess_normal_src = g_flat_cmds[i].src_cmd_idx;
            }
            if (last_tess_normal_src == g_edit_line)
                return 1;
        }
        break;
    }
    default:
        break;
    }

    return cmd->src_cmd_idx == g_edit_line;
}
