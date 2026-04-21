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
    int call_depth;
    int abort;
    int visit_budget;
} FlattenContext;

static void flatten_fail(FlattenContext *ctx, const char *msg) {
    if (!ctx->abort)
        set_status(msg);
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

/* Recursively expand the source commands in g_cmds[start..end_idx) into
 * the flat command array g_flat_cmds[]. For-loops are unrolled, function
 * calls are inlined, and if-blocks with loop-variable conditions are
 * evaluated. `vars`/`nv` carry loop-variable and function-parameter
 * bindings from enclosing scopes. */
static void flatten_range(FlattenContext *ctx,
                          int start, int end_idx, ExprVar *vars, int nv,
                          int call_src_cmd_idx, int root_call_src_cmd_idx,
                          unsigned int func_scope_mask) {
    int i = start;
    while (i < end_idx && i < g_num_cmds) {
        if (ctx->abort) return;
        if (--ctx->visit_budget < 0) {
            flatten_fail(ctx, "Recursive expansion exceeded visit budget");
            return;
        }
        if (!g_cmds[i].valid) { i++; continue; }

        if (g_cmds[i].type == CMD_FOR_BEGIN) {
            int loop_end = find_block_end(i);
            GLCmd *loop_cmd = &g_cmds[i];
            char var_name[16];
            flatten_get_for_var_name(loop_cmd, var_name, sizeof(var_name));
            float start_val = loop_cmd->args[0];
            float end_val   = loop_cmd->args[1];
            float step_val  = loop_cmd->args[2];

            /* Re-evaluate for-loop bounds from source if they contain variables */
            if (loop_cmd->has_vars) {
                const char *unused_body;
                float re_start, re_end, re_step;
                char rv[16];
                if (parse_for_header_with_vars(loop_cmd->source, rv, sizeof(rv),
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
            i = (loop_end < g_num_cmds) ? loop_end + 1 : g_num_cmds;
            continue;
        }

        if (g_cmds[i].type == CMD_FOR_END) { i++; continue; }

        /* Function definitions: skip body (expanded at call sites) */
        if (g_cmds[i].type == CMD_FUNC_DEF) {
            int func_end = find_block_end(i);
            i = (func_end < g_num_cmds) ? func_end + 1 : g_num_cmds;
            continue;
        }
        if (g_cmds[i].type == CMD_FUNC_END) { i++; continue; }

        /* Function calls: find definition and expand body inline */
        if (g_cmds[i].type == CMD_CALL) {
            int func_num = (int)g_cmds[i].args[0];
            if (ctx->call_depth >= MAX_FLATTEN_CALL_DEPTH) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "Recursive expansion exceeded depth limit (%d) at func%d",
                         MAX_FLATTEN_CALL_DEPTH, func_num);
                flatten_fail(ctx, msg);
                i++;
                continue;
            }

            for (int k = 0; k < g_num_cmds; k++) {
                if (g_cmds[k].type == CMD_FUNC_DEF && (int)g_cmds[k].args[0] == func_num) {
                    int body_end = find_block_end(k);
                    int def_fn = func_num;
                    int param_count = 0;
                    char param_names[MAX_EXPR_VARS][16];
                    char arg_text[MAX_LINE_LEN];
                    float arg_vals[MAX_EXPR_VARS];
                    int arg_count = 0;

                    if (!parse_repl_func_signature(g_cmds[k].source, &def_fn,
                                                   param_names, MAX_EXPR_VARS,
                                                   &param_count))
                        break;
                    if (!extract_func_call_args_text(g_cmds[i].source, NULL,
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
                        set_status(msg);
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
            i++;
            continue;
        }

        if (g_cmds[i].type == CMD_IF_BEGIN) {
            int if_end = find_block_end(i);
            char cond_text[MAX_LINE_LEN];
            int needs_local_eval = 0;

            if (vars && nv > 0 &&
                repl_extract_paren_payload(g_cmds[i].source, cond_text, sizeof(cond_text)) &&
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
                i = (if_end < g_num_cmds) ? if_end + 1 : g_num_cmds;
                continue;
            }

            if (g_num_flat_cmds < MAX_COMMANDS) {
                g_flat_cmds[g_num_flat_cmds] = g_cmds[i];
                flat_cmd_set_provenance(&g_flat_cmds[g_num_flat_cmds],
                                        i, call_src_cmd_idx,
                                        root_call_src_cmd_idx,
                                        func_scope_mask);
                g_num_flat_cmds++;
            } else {
                flatten_fail(ctx, "Flattened command limit reached");
                return;
            }
            i++;
            continue;
        }

        if (g_cmds[i].type == CMD_IF_END) {
            if (g_num_flat_cmds < MAX_COMMANDS) {
                g_flat_cmds[g_num_flat_cmds] = g_cmds[i];
                flat_cmd_set_provenance(&g_flat_cmds[g_num_flat_cmds],
                                        i, call_src_cmd_idx,
                                        root_call_src_cmd_idx,
                                        func_scope_mask);
                g_num_flat_cmds++;
            } else {
                flatten_fail(ctx, "Flattened command limit reached");
                return;
            }
            i++;
            continue;
        }

        if ((g_cmds[i].type == CMD_LABEL || g_cmds[i].type == CMD_GOTO) &&
            func_scope_mask != 0) {
            flatten_fail(ctx, "goto and labels are not supported inside functions");
            return;
        }

        /* Comments: pass through to flat array (skipped by execute, kept in save) */
        if (g_cmds[i].type == CMD_COMMENT) {
            if (g_num_flat_cmds < MAX_COMMANDS) {
                g_flat_cmds[g_num_flat_cmds] = g_cmds[i];
                flat_cmd_set_provenance(&g_flat_cmds[g_num_flat_cmds],
                                        i, call_src_cmd_idx,
                                        root_call_src_cmd_idx,
                                        func_scope_mask);
                g_num_flat_cmds++;
            } else {
                flatten_fail(ctx, "Flattened command limit reached");
                return;
            }
            i++;
            continue;
        }

        if (g_cmds[i].type == CMD_VAR_DECLARE) { i++; continue; }

        /* Variable assignments: update predefined var and pass through */
        if (g_cmds[i].type == CMD_VAR_ASSIGN) {
            int var_idx = g_cmds[i].num_args; /* predef var index */
            float value = g_cmds[i].args[0];
            char rhs[MAX_LINE_LEN] = "";
            int local_rhs_vars = 0;

            if (repl_extract_assignment_parts(g_cmds[i].source, NULL, 0,
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
            if (g_num_flat_cmds < MAX_COMMANDS) {
                GLCmd tmp = g_cmds[i];
                tmp.args[0] = value;
                tmp.has_vars = g_cmds[i].has_vars || local_rhs_vars;
                g_flat_cmds[g_num_flat_cmds] = tmp;
                g_flat_cmd_local_vars[g_num_flat_cmds].num_vars = 0;
                if (vars && nv > 0) {
                    int snap_n = nv < MAX_EXPR_VARS ? nv : MAX_EXPR_VARS;
                    g_flat_cmd_local_vars[g_num_flat_cmds].num_vars = snap_n;
                    memcpy(g_flat_cmd_local_vars[g_num_flat_cmds].vars, vars,
                           (size_t)snap_n * sizeof(ExprVar));
                }
                flat_cmd_set_provenance(&g_flat_cmds[g_num_flat_cmds],
                                        i, call_src_cmd_idx,
                                        root_call_src_cmd_idx,
                                        func_scope_mask);
                g_num_flat_cmds++;
            } else {
                flatten_fail(ctx, "Flattened command limit reached");
                return;
            }
            i++;
            continue;
        }

        /* Regular command */
        if (g_num_flat_cmds >= MAX_COMMANDS) {
            flatten_fail(ctx, "Flattened command limit reached");
            return;
        }

        if (vars && nv > 0) {
            GLCmd tmp;
            memset(&tmp, 0, sizeof(tmp));
            int saved = g_edit_line;
            g_edit_line = g_num_flat_cmds;
            if (repl_parse_command_with_vars(g_cmds[i].source, &tmp, vars, nv)) {
                tmp.has_vars = g_cmds[i].has_vars;
                strncpy(tmp.source, g_cmds[i].source, sizeof(tmp.source) - 1);
                tmp.source[sizeof(tmp.source) - 1] = '\0';
                flat_cmd_set_provenance(&tmp, i, call_src_cmd_idx,
                                        root_call_src_cmd_idx,
                                        func_scope_mask);
                /* Snapshot local vars so replay can show correct substitution */
                int snap_n = nv < MAX_EXPR_VARS ? nv : MAX_EXPR_VARS;
                g_flat_cmd_local_vars[g_num_flat_cmds].num_vars = snap_n;
                memcpy(g_flat_cmd_local_vars[g_num_flat_cmds].vars, vars,
                       (size_t)snap_n * sizeof(ExprVar));
                g_flat_cmds[g_num_flat_cmds++] = tmp;
            }
            g_edit_line = saved;
        } else if (g_cmds[i].has_vars) {
            /* Outside loop but has predefined var references: re-evaluate */
            GLCmd tmp;
            memset(&tmp, 0, sizeof(tmp));
            if (repl_parse_command(g_cmds[i].source, &tmp)) {
                tmp.has_vars = 1;
                strncpy(tmp.source, g_cmds[i].source, sizeof(tmp.source) - 1);
                tmp.source[sizeof(tmp.source) - 1] = '\0';
                flat_cmd_set_provenance(&tmp, i, call_src_cmd_idx,
                                        root_call_src_cmd_idx,
                                        func_scope_mask);
                g_flat_cmd_local_vars[g_num_flat_cmds].num_vars = 0;
                g_flat_cmds[g_num_flat_cmds++] = tmp;
            }
        } else {
            g_flat_cmds[g_num_flat_cmds] = g_cmds[i];
            flat_cmd_set_provenance(&g_flat_cmds[g_num_flat_cmds],
                                    i, call_src_cmd_idx,
                                    root_call_src_cmd_idx,
                                    func_scope_mask);
            g_flat_cmd_local_vars[g_num_flat_cmds].num_vars = 0;
            g_num_flat_cmds++;
        }
        i++;
    }
}

void flatten_commands(void) {
    FlattenContext ctx = {
        .call_depth = 0,
        .abort = 0,
        .visit_budget = MAX_FLATTEN_VISIT_BUDGET
    };

    g_num_flat_cmds = 0;
    flatten_range(&ctx, 0, g_num_cmds, NULL, 0, -1, -1, 0);
    if (ctx.abort)
        g_num_flat_cmds = 0;

    /* Track whether user enabled lighting (for correct default state) */
    g_user_lighting_enabled = 0;
    for (int i = 0; i < g_num_cmds; i++) {
        if (g_cmds[i].valid && g_cmds[i].type == CMD_ENABLE &&
            g_cmds[i].mode == GL_LIGHTING)
            g_user_lighting_enabled = 1;
        if (g_cmds[i].valid && g_cmds[i].type == CMD_DISABLE &&
            g_cmds[i].mode == GL_LIGHTING)
            g_user_lighting_enabled = 0;
    }

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
