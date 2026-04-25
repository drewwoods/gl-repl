/*
 * repl_replay_annotations.c -- Code-panel replay variable annotations.
 */
#include "sample.h"
#include "repl_core.h"
#include "repl_core_internal.h"
#include "repl_parser.h"
#include "repl_replay.h"
#include "repl_state.h"
#include "repl_replay_annotations.h"

/* ========================================================================= */
/* Replay variable display helpers                                            */
/* ========================================================================= */

/* Per-frame replay annotation cache.
 * Rebuilt once at the start of render_code_panel to avoid O(N × replay_pc)
 * work when annotating variable assignments during replay. */
static int   s_replay_cache_pc = -2;                 /* replay_pc when built */
static int   s_replay_flat_map[MAX_COMMANDS];         /* src_cmd_idx → flat_idx */
static int   s_replay_current_flat_idx = -1;          /* flat cmd for src_line */
/* Predef-variable snapshots: one per source command, taken at the flat_idx
 * stored in s_replay_flat_map[src].  Built by a single O(replay_pc) forward
 * simulation so each snapshot reflects the correct variable state BEFORE
 * executing that specific flat command. */
static float s_replay_predef_snap[MAX_COMMANDS][MAX_PREDEF_VARS];
static int   s_replay_predef_snap_valid[MAX_COMMANDS];

static void replay_build_predef_snapshots(void);

static void repl_replay_annotations_rebuild_cache(void) {
    const ReplReplayRuntimeState *replay = repl_state_replay();
    memset(s_replay_flat_map, 0xff, sizeof(int) * (size_t)repl_state_document_count());

    /* Backward pass: first match per src_cmd_idx = most recent execution */
    for (int j = *replay->pc - 1; j >= 0; j--) {
        int src = repl_state_flat_program_cmds_mut()[j].src_cmd_idx;
        if (src >= 0 && src < repl_state_document_count() &&
            s_replay_flat_map[src] == -1 && repl_state_flat_program_cmds_mut()[j].valid)
            s_replay_flat_map[src] = j;
    }

    s_replay_current_flat_idx = (*replay->src_line_idx >= 0 &&
                                 *replay->src_line_idx < repl_state_document_count())
                                ? s_replay_flat_map[*replay->src_line_idx] : -1;

    /* Single forward pass builds per-src predef snapshots */
    replay_build_predef_snapshots();

    s_replay_cache_pc = *replay->pc;
}

static void repl_replay_annotations_invalidate(void) {
    s_replay_cache_pc = -2;
}

/* Find the most recent flat command for a source line, at or before replay_pc */
int repl_replay_annotation_flat_cmd_for_source(int src_line) {
    const ReplReplayRuntimeState *replay = repl_state_replay();

    if (*replay->pc <= 0) return -1;
    /* Use per-frame cache when available */
    if (s_replay_cache_pc == *replay->pc &&
        src_line >= 0 && src_line < repl_state_document_count())
        return s_replay_flat_map[src_line];
    for (int j = *replay->pc - 1; j >= 0; j--) {
        if (repl_state_flat_program_cmds_mut()[j].src_cmd_idx == src_line && repl_state_flat_program_cmds_mut()[j].valid)
            return j;
    }
    return -1;
}

static int replay_current_flat_cmd(void) {
    const ReplReplayRuntimeState *replay = repl_state_replay();

    if (*replay->src_line_idx < 0)
        return -1;
    if (s_replay_cache_pc == *replay->pc)
        return s_replay_current_flat_idx;
    return repl_replay_annotation_flat_cmd_for_source(*replay->src_line_idx);
}

static int format_evaluated_cmd(const GLCmd *cmd, const char *orig_source,
                                char *out, int out_size);

static const char *skip_leading_ws(const char *s) {
    while (s && *s && isspace((unsigned char)*s))
        s++;
    return s ? s : "";
}

static const char *skip_numeric_literal(const char *s) {
    const char *p = s;
    int saw_digits = 0;

    if (!p)
        return s;

    if (*p == '.') {
        if (!isdigit((unsigned char)p[1]))
            return s;
        p++;
    }

    while (isdigit((unsigned char)*p)) {
        p++;
        saw_digits = 1;
    }

    if (*p == '.') {
        p++;
        while (isdigit((unsigned char)*p)) {
            p++;
            saw_digits = 1;
        }
    }

    if (!saw_digits)
        return s;

    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        if (*q == '+' || *q == '-')
            q++;
        if (isdigit((unsigned char)*q)) {
            p = q + 1;
            while (isdigit((unsigned char)*p))
                p++;
        }
    }

    return p;
}

static int expr_has_visible_vars(const char *s, const ExprVar *vars, int num_vars) {
    while (s && *s) {
        const char *start;
        int len;
        const char *num_end = skip_numeric_literal(s);

        if (num_end != s) {
            s = num_end;
            continue;
        }

        if (!isalpha((unsigned char)*s) && *s != '_') {
            s++;
            continue;
        }

        start = s;
        while (*s && (isalnum((unsigned char)*s) || *s == '_'))
            s++;
        len = (int)(s - start);

        for (int i = 0; i < num_vars; i++) {
            int nlen = (int)strlen(vars[i].name);
            if (nlen == len && strncmp(start, vars[i].name, len) == 0)
                return 1;
        }
    }
    return 0;
}

static int visible_var_index(const ExprVar *vars, int num_vars, const char *name) {
    for (int i = 0; i < num_vars; i++) {
        if (strcmp(vars[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int replay_flat_cmd_context_matches(int flat_idx, int current_flat_idx) {
    const FlatCmdLocalVars *flat_vars;
    const FlatCmdLocalVars *cur_vars;

    if (flat_idx < 0 || current_flat_idx < 0)
        return 1;

    if (repl_state_flat_program_cmds_mut()[flat_idx].func_scope_mask !=
        repl_state_flat_program_cmds_mut()[current_flat_idx].func_scope_mask)
        return 0;

    flat_vars = &repl_state_flat_program_local_vars_mut()[flat_idx];
    cur_vars = &repl_state_flat_program_local_vars_mut()[current_flat_idx];

    for (int i = 0; i < flat_vars->num_vars; i++) {
        int ci = visible_var_index(cur_vars->vars, cur_vars->num_vars,
                                   flat_vars->vars[i].name);
        if (ci >= 0 &&
            fabsf(flat_vars->vars[i].value - cur_vars->vars[ci].value) > 1e-6f)
            return 0;
    }

    return 1;
}

static int find_replay_assignment_flat_cmd(int src_line) {
    const ReplReplayRuntimeState *replay = repl_state_replay();
    int current_flat_idx = replay_current_flat_cmd();

    if (*replay->pc <= 0)
        return -1;

    /* Try cached map: O(1) lookup + context check */
    if (s_replay_cache_pc == *replay->pc &&
        src_line >= 0 && src_line < repl_state_document_count()) {
        int cached = s_replay_flat_map[src_line];
        if (cached >= 0 &&
            (current_flat_idx < 0 ||
             cached == current_flat_idx ||
             replay_flat_cmd_context_matches(cached, current_flat_idx)))
            return cached;
    }

    /* Fallback: full scan (only for context mismatch in function bodies) */
    for (int j = *replay->pc - 1; j >= 0; j--) {
        if (repl_state_flat_program_cmds_mut()[j].src_cmd_idx != src_line || !repl_state_flat_program_cmds_mut()[j].valid)
            continue;
        if (current_flat_idx < 0 ||
            j == current_flat_idx ||
            replay_flat_cmd_context_matches(j, current_flat_idx))
            return j;
    }

    return -1;
}

static int build_visible_vars_from_predef_values(int flat_idx,
                                                 const float *predef_vals,
                                                 ExprVar *out, int max_out) {
    int nv = 0;

    if (!out || max_out <= 0)
        return 0;

    if (flat_idx >= 0 && flat_idx < repl_state_flat_program_count()) {
        const FlatCmdLocalVars *lcvars = &repl_state_flat_program_local_vars_mut()[flat_idx];
        for (int i = 0; i < lcvars->num_vars && nv < max_out; i++)
            out[nv++] = lcvars->vars[i];
    }

    if (predef_vals) {
        for (int i = 0; i < g_num_predef_vars && nv < max_out; i++) {
            if (visible_var_index(out, nv, g_predef_vars[i].name) >= 0)
                continue;
            strncpy(out[nv].name, g_predef_vars[i].name,
                    sizeof(out[nv].name) - 1);
            out[nv].name[sizeof(out[nv].name) - 1] = '\0';
            out[nv].value = predef_vals[i];
            nv++;
        }
    }

    return nv;
}

/* Replace visible variable identifiers with their formatted values.
 * Optionally builds var_comment like " // t = 3.3, n = 12". */
static int subst_visible_vars(const char *source, char *out, int out_size,
                              char *var_comment, int comment_size,
                              const ExprVar *vars, int num_vars) {
    char used_names[MAX_PREDEF_VARS + MAX_EXPR_VARS][16];
    float used_vals[MAX_PREDEF_VARS + MAX_EXPR_VARS];
    int num_used = 0;
    int max_used = MAX_PREDEF_VARS + MAX_EXPR_VARS;

    int oi = 0;
    const char *p = source;
    while (*p && oi < out_size - 20) {
        const char *num_end = skip_numeric_literal(p);

        if (num_end != p) {
            int len = (int)(num_end - p);
            if (oi + len < out_size) {
                memcpy(out + oi, p, (size_t)len);
                oi += len;
            }
            p = num_end;
            continue;
        }

        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *start = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            int len = (int)(p - start);

            int found = 0;
            float found_val = 0.0f;
            char found_name[16] = "";

            for (int v = 0; !found && v < num_vars; v++) {
                int nlen = (int)strlen(vars[v].name);
                if (nlen == len &&
                    strncmp(start, vars[v].name, len) == 0) {
                    found = 1;
                    found_val = vars[v].value;
                    strncpy(found_name, vars[v].name, 15);
                    found_name[15] = '\0';
                }
            }

            if (found) {
                oi += snprintf(out + oi, out_size - oi, "%g", found_val);
                int dup = 0;
                for (int u = 0; u < num_used; u++) {
                    if (strcmp(used_names[u], found_name) == 0) {
                        dup = 1; break;
                    }
                }
                if (!dup && num_used < max_used) {
                    snprintf(used_names[num_used], sizeof(used_names[num_used]), "%.15s",
                             found_name);
                    used_vals[num_used] = found_val;
                    num_used++;
                }
            } else {
                if (oi + len < out_size) {
                    memcpy(out + oi, start, len);
                    oi += len;
                }
            }
        } else {
            out[oi++] = *p++;
        }
    }
    out[oi] = '\0';

    if (var_comment && comment_size > 0) {
        var_comment[0] = '\0';
        if (num_used > 0) {
            int ci = snprintf(var_comment, comment_size, " // ");
            for (int u = 0; u < num_used && ci < comment_size - 20; u++) {
                if (u > 0)
                    ci += snprintf(var_comment + ci, comment_size - ci, ", ");
                ci += snprintf(var_comment + ci, comment_size - ci, "%s = %g",
                               used_names[u], used_vals[u]);
            }
        }
    }

    return num_used;
}

static int replay_eval_expr_with_predefs(int flat_idx, const char *expr,
                                         const float *predef_vals,
                                         float *out_value) {
    char repl_expr[MAX_LINE_LEN];
    ExprVar vars[MAX_PREDEF_VARS + MAX_EXPR_VARS];
    int nv;
    ExprCtx ctx;

    if (!expr || !out_value)
        return 0;

    c_expr_to_repl(expr, repl_expr, sizeof(repl_expr));
    nv = build_visible_vars_from_predef_values(flat_idx, predef_vals,
                                               vars,
                                               (int)(sizeof(vars) / sizeof(vars[0])));
    ctx.p = repl_expr;
    ctx.vars = vars;
    ctx.num_vars = nv;
    *out_value = eval_expr(&ctx);
    return 1;
}

static int replay_copy_predef_values_before_flat_cmd(int target_pc,
                                                     float *out_vals,
                                                     int max_vals) {
    const ReplReplayRuntimeState *replay = repl_state_replay();
    int pc = 0;
    int goto_count = 0;

    if (!out_vals || max_vals < g_num_predef_vars)
        return 0;

    if (*replay->active)
        repl_copy_replay_baseline_predef_values(out_vals, max_vals);
    else
        for (int i = 0; i < g_num_predef_vars && i < max_vals; i++)
            out_vals[i] = g_predef_vars[i].value;

    if (target_pc < 0)
        target_pc = 0;
    if (target_pc > repl_state_flat_program_count())
        target_pc = repl_state_flat_program_count();

    while (pc < target_pc) {
        if (!repl_state_flat_program_cmds_mut()[pc].valid) {
            pc++;
            continue;
        }

        switch (repl_state_flat_program_cmds_mut()[pc].type) {
        case CMD_VAR_ASSIGN: {
            int vi = repl_state_flat_program_cmds_mut()[pc].num_args;
            float value = repl_state_flat_program_cmds_mut()[pc].args[0];
            if (repl_state_flat_program_cmds_mut()[pc].has_vars) {
                char rhs[MAX_LINE_LEN];
                if (repl_extract_assignment_parts(repl_state_flat_program_cmds_mut()[pc].source,
                                                  NULL, 0,
                                                  rhs, sizeof(rhs)) &&
                    rhs[0]) {
                    replay_eval_expr_with_predefs(pc, rhs, out_vals, &value);
                }
            }
            if (vi >= 0 && vi < g_num_predef_vars)
                out_vals[vi] = value;
            break;
        }
        case CMD_IF_BEGIN: {
            float cond = repl_state_flat_program_cmds_mut()[pc].args[0];
            if (repl_state_flat_program_cmds_mut()[pc].has_vars) {
                char cond_text[MAX_LINE_LEN];
                if (repl_extract_paren_payload(repl_state_flat_program_cmds_mut()[pc].source,
                                               cond_text, sizeof(cond_text)) &&
                    cond_text[0]) {
                    replay_eval_expr_with_predefs(pc, cond_text, out_vals, &cond);
                }
            }
            if (cond == 0.0f) {
                int depth = 1;
                while (depth > 0 && ++pc < target_pc) {
                    if (repl_state_flat_program_cmds_mut()[pc].type == CMD_IF_BEGIN) depth++;
                    else if (repl_state_flat_program_cmds_mut()[pc].type == CMD_IF_END) depth--;
                }
            }
            break;
        }
        case CMD_GOTO: {
            char label[64];
            if (!repl_extract_goto_label(repl_state_flat_program_cmds_mut()[pc].source,
                                         label, sizeof(label)))
                break;
            if (goto_count++ > 100000)
                return 0;
            for (int li = 0; li < repl_state_flat_program_count(); li++) {
                char target_label[64];
                if (repl_state_flat_program_cmds_mut()[li].valid &&
                    repl_state_flat_program_cmds_mut()[li].type == CMD_LABEL &&
                    repl_extract_label_name(repl_state_flat_program_cmds_mut()[li].source,
                                            target_label,
                                            sizeof(target_label)) &&
                    strcmp(target_label, label) == 0) {
                    pc = li;
                    goto next_pc;
                }
            }
            break;
        }
        default:
            break;
        }

        pc++;
next_pc:
        ;
    }

    return 1;
}

/* Single-pass forward simulation that builds predef snapshots for every
 * source command whose flat_idx appears in s_replay_flat_map.  Each snapshot
 * captures the predef variable state BEFORE the command at that flat_idx
 * executes, matching replay_copy_predef_values_before_flat_cmd semantics.
 * Total cost: O(replay_pc), called once per frame during cache rebuild. */
static void replay_build_predef_snapshots(void) {
    const ReplReplayRuntimeState *replay = repl_state_replay();
    float vals[MAX_PREDEF_VARS];
    int pc = 0, goto_count = 0;
    int target_pc = *replay->pc;

    memset(s_replay_predef_snap_valid, 0, sizeof(int) * (size_t)repl_state_document_count());

    if (*replay->active)
        repl_copy_replay_baseline_predef_values(vals, MAX_PREDEF_VARS);
    else
        for (int i = 0; i < g_num_predef_vars && i < MAX_PREDEF_VARS; i++)
            vals[i] = g_predef_vars[i].value;

    if (target_pc < 0) target_pc = 0;
    if (target_pc > repl_state_flat_program_count()) target_pc = repl_state_flat_program_count();

    /* Macro: snapshot predef vals for a flat position's source command */
    #define SNAP_IF_MAPPED(flat_pc) do {                                    \
        int _src = repl_state_flat_program_cmds_mut()[flat_pc].src_cmd_idx;                        \
        if (_src >= 0 && _src < repl_state_document_count() &&                               \
            s_replay_flat_map[_src] == (flat_pc) &&                         \
            !s_replay_predef_snap_valid[_src]) {                            \
            memcpy(s_replay_predef_snap[_src], vals,                        \
                   sizeof(float) * (size_t)g_num_predef_vars);              \
            s_replay_predef_snap_valid[_src] = 1;                           \
        }                                                                   \
    } while (0)

    while (pc < target_pc) {
        SNAP_IF_MAPPED(pc);

        if (!repl_state_flat_program_cmds_mut()[pc].valid) { pc++; continue; }

        switch (repl_state_flat_program_cmds_mut()[pc].type) {
        case CMD_VAR_ASSIGN: {
            int vi = repl_state_flat_program_cmds_mut()[pc].num_args;
            float value = repl_state_flat_program_cmds_mut()[pc].args[0];
            if (repl_state_flat_program_cmds_mut()[pc].has_vars) {
                char rhs[MAX_LINE_LEN];
                if (repl_extract_assignment_parts(repl_state_flat_program_cmds_mut()[pc].source,
                                                  NULL, 0,
                                                  rhs, sizeof(rhs)) &&
                    rhs[0])
                    replay_eval_expr_with_predefs(pc, rhs, vals, &value);
            }
            if (vi >= 0 && vi < g_num_predef_vars)
                vals[vi] = value;
            break;
        }
        case CMD_IF_BEGIN: {
            float cond = repl_state_flat_program_cmds_mut()[pc].args[0];
            if (repl_state_flat_program_cmds_mut()[pc].has_vars) {
                char cond_text[MAX_LINE_LEN];
                if (repl_extract_paren_payload(repl_state_flat_program_cmds_mut()[pc].source,
                                               cond_text, sizeof(cond_text)) &&
                    cond_text[0])
                    replay_eval_expr_with_predefs(pc, cond_text, vals, &cond);
            }
            if (cond == 0.0f) {
                int depth = 1;
                while (depth > 0 && ++pc < target_pc) {
                    SNAP_IF_MAPPED(pc);
                    if (repl_state_flat_program_cmds_mut()[pc].type == CMD_IF_BEGIN) depth++;
                    else if (repl_state_flat_program_cmds_mut()[pc].type == CMD_IF_END) depth--;
                }
            }
            break;
        }
        case CMD_GOTO: {
            char label[64];
            if (!repl_extract_goto_label(repl_state_flat_program_cmds_mut()[pc].source,
                                         label, sizeof(label)))
                break;
            if (goto_count++ > 100000)
                goto snap_done;
            for (int li = 0; li < repl_state_flat_program_count(); li++) {
                char target_label[64];
                if (repl_state_flat_program_cmds_mut()[li].valid &&
                    repl_state_flat_program_cmds_mut()[li].type == CMD_LABEL &&
                    repl_extract_label_name(repl_state_flat_program_cmds_mut()[li].source,
                                            target_label,
                                            sizeof(target_label)) &&
                    strcmp(target_label, label) == 0) {
                    pc = li;
                    goto snap_next_pc;
                }
            }
            break;
        }
        default:
            break;
        }

        pc++;
snap_next_pc:
        ;
    }
snap_done:
    ;

    #undef SNAP_IF_MAPPED
}

static int build_replay_assignment_inline_comment(int cmd_idx, int flat_idx,
                                                  char *out, int out_size) {
    const ReplReplayRuntimeState *replay = repl_state_replay();
    float predef_vals[MAX_PREDEF_VARS];
    ExprVar visible_vars[MAX_PREDEF_VARS + MAX_EXPR_VARS];
    char rhs_subst[MAX_LINE_LEN];
    char name[16];
    char rhs[MAX_LINE_LEN];
    int nv;

    if (!out || out_size <= 0)
        return 0;
    out[0] = '\0';

    /* Use per-src predef snapshot when cache covers this cmd/flat pair */
    if (s_replay_cache_pc == *replay->pc &&
        cmd_idx >= 0 && cmd_idx < repl_state_document_count() &&
        s_replay_predef_snap_valid[cmd_idx] &&
        s_replay_flat_map[cmd_idx] == flat_idx)
        memcpy(predef_vals, s_replay_predef_snap[cmd_idx],
               sizeof(float) * (size_t)g_num_predef_vars);
    else if (!replay_copy_predef_values_before_flat_cmd(flat_idx,
                                                        predef_vals,
                                                        MAX_PREDEF_VARS))
        return 0;

    nv = build_visible_vars_from_predef_values(flat_idx, predef_vals,
                                               visible_vars,
                                               (int)(sizeof(visible_vars) / sizeof(visible_vars[0])));
    if (!repl_extract_assignment_parts(repl_state_document_cmds_mut()[cmd_idx].source,
                                       name, sizeof(name),
                                       rhs, sizeof(rhs)) ||
        !name[0] || !rhs[0])
        return 0;

    {
        float value = repl_state_flat_program_cmds_mut()[flat_idx].args[0];
        subst_visible_vars(rhs, rhs_subst, sizeof(rhs_subst),
                           NULL, 0, visible_vars, nv);
        replay_eval_expr_with_predefs(flat_idx, rhs, predef_vals, &value);

        if (rhs_subst[0] &&
            strcmp(skip_leading_ws(rhs_subst), skip_leading_ws(rhs)) != 0 &&
            !expr_has_visible_vars(rhs_subst, visible_vars, nv)) {
            snprintf(out, out_size, " // %s = %s = %g",
                     name, skip_leading_ws(rhs_subst), value);
        } else {
            snprintf(out, out_size, " // %s = %g", name, value);
        }
        return 1;
    }

    return 0;
}

int code_panel_get_command_display_text(int cmd_idx, char *out, int out_size) {
    const ReplReplayRuntimeState *replay = repl_state_replay();
    int flat_idx;
    char comment[MAX_INPUT_LEN];

    if (!out || out_size <= 0)
        return 0;
    out[0] = '\0';

    if (cmd_idx < 0 || cmd_idx >= repl_state_document_count())
        return 0;

    snprintf(out, out_size, "%s", repl_state_document_cmds_mut()[cmd_idx].source);

    if (!*replay->active ||
        !*replay->expand_args ||
        !repl_state_document_cmds_mut()[cmd_idx].has_vars)
        return 1;

    if (repl_state_document_cmds_mut()[cmd_idx].type != CMD_VAR_ASSIGN)
        return 1;

    flat_idx = find_replay_assignment_flat_cmd(cmd_idx);
    if (flat_idx < 0)
        return 1;

    if (build_replay_assignment_inline_comment(cmd_idx, flat_idx,
                                               comment, sizeof(comment)) &&
        comment[0]) {
        snprintf(out, out_size, "%s%s", repl_state_document_cmds_mut()[cmd_idx].source, comment);
    }

    return 1;
}

int repl_replay_build_subst_annotation(int cmd_idx, int flat_idx,
                                         char *subst, int subst_size,
                                         char *var_comment, int comment_size) {
    const ReplReplayRuntimeState *replay = repl_state_replay();
    float predef_vals[MAX_PREDEF_VARS];
    ExprVar visible_vars[MAX_PREDEF_VARS + MAX_EXPR_VARS];
    int nv;

    if (!subst || subst_size <= 0)
        return 0;
    subst[0] = '\0';
    if (var_comment && comment_size > 0)
        var_comment[0] = '\0';

    if (s_replay_cache_pc == *replay->pc &&
        cmd_idx >= 0 && cmd_idx < repl_state_document_count() &&
        s_replay_predef_snap_valid[cmd_idx] &&
        s_replay_flat_map[cmd_idx] == flat_idx)
        memcpy(predef_vals, s_replay_predef_snap[cmd_idx],
               sizeof(float) * (size_t)g_num_predef_vars);
    else if (!replay_copy_predef_values_before_flat_cmd(flat_idx,
                                                        predef_vals,
                                                        MAX_PREDEF_VARS))
        return 0;

    nv = build_visible_vars_from_predef_values(flat_idx, predef_vals,
                                               visible_vars,
                                               (int)(sizeof(visible_vars) / sizeof(visible_vars[0])));
    return subst_visible_vars(repl_state_document_cmds_mut()[cmd_idx].source, subst, subst_size,
                              var_comment, comment_size,
                              visible_vars, nv);
}

int repl_replay_build_eval_annotation(int cmd_idx, int flat_idx,
                                        char *eval_buf, int eval_size) {
    const ReplReplayRuntimeState *replay = repl_state_replay();
    float predef_vals[MAX_PREDEF_VARS];
    ExprVar visible_vars[MAX_PREDEF_VARS + MAX_EXPR_VARS];
    GLCmd eval_cmd;
    int nv;

    if (!eval_buf || eval_size <= 0)
        return 0;
    eval_buf[0] = '\0';

    if (s_replay_cache_pc == *replay->pc &&
        cmd_idx >= 0 && cmd_idx < repl_state_document_count() &&
        s_replay_predef_snap_valid[cmd_idx] &&
        s_replay_flat_map[cmd_idx] == flat_idx)
        memcpy(predef_vals, s_replay_predef_snap[cmd_idx],
               sizeof(float) * (size_t)g_num_predef_vars);
    else if (!replay_copy_predef_values_before_flat_cmd(flat_idx,
                                                        predef_vals,
                                                        MAX_PREDEF_VARS))
        return 0;

    nv = build_visible_vars_from_predef_values(flat_idx, predef_vals,
                                               visible_vars,
                                               (int)(sizeof(visible_vars) / sizeof(visible_vars[0])));
    memset(&eval_cmd, 0, sizeof(eval_cmd));
    ReplParseContext parse_ctx = { cmd_idx, visible_vars, nv, 0 };
    if (!repl_parse_command_ctx(repl_state_document_cmds_mut()[cmd_idx].source,
                                &eval_cmd, &parse_ctx))
        return 0;

    return format_evaluated_cmd(&eval_cmd, repl_state_document_cmds_mut()[cmd_idx].source,
                                eval_buf, eval_size);
}

/* Get format string for a command type's evaluated display */
static const char *eval_fmt_for_type(CmdType type, int *nargs_out) {
    switch (type) {
    case CMD_VERTEX3F:         *nargs_out = 3; return "glVertex3f(%g, %g, %g);";
    case CMD_VERTEX2F:         *nargs_out = 2; return "glVertex2f(%g, %g);";
    case CMD_NORMAL3F:         *nargs_out = 3; return "glNormal3f(%g, %g, %g);";
    case CMD_COLOR3F:          *nargs_out = 3; return "glColor3f(%g, %g, %g);";
    case CMD_COLOR4F:          *nargs_out = 4; return "glColor4f(%g, %g, %g, %g);";
    case CMD_CLEAR_COLOR:      *nargs_out = 4; return "glClearColor(%g, %g, %g, %g);";
    case CMD_TRANSLATE3F:      *nargs_out = 3; return "glTranslatef(%g, %g, %g);";
    case CMD_SCALEF:           *nargs_out = 3; return "glScalef(%g, %g, %g);";
    case CMD_ROTATEF:          *nargs_out = 4; return "glRotatef(%g, %g, %g, %g);";
    case CMD_GLU_SPHERE:       *nargs_out = 3; return "gluSphere(q, %g, %g, %g);";
    case CMD_GLU_CYLINDER:     *nargs_out = 5; return "gluCylinder(q, %g, %g, %g, %g, %g);";
    case CMD_GLU_DISK:         *nargs_out = 4; return "gluDisk(q, %g, %g, %g, %g);";
    case CMD_GLU_PARTIAL_DISK: *nargs_out = 6; return "gluPartialDisk(q, %g, %g, %g, %g, %g, %g);";
    case CMD_GLUT_TORUS:       *nargs_out = 4; return "glutSolidTorus(%g, %g, %g, %g);";
    case CMD_TESS_NORMAL:      *nargs_out = 3; return "gluNormal(%g, %g, %g);";
    case CMD_TESS_VERTEX:      *nargs_out = 3; return "gluVertex(%g, %g, %g);";
    default:                   *nargs_out = 0; return NULL;
    }
}

/* Format a command with evaluated numeric args.
 * Preserves leading whitespace from the original source. */
static int format_evaluated_cmd(const GLCmd *cmd, const char *orig_source,
                                 char *out, int out_size) {
    int indent = 0;
    while (orig_source[indent] &&
           isspace((unsigned char)orig_source[indent]))
        indent++;

    int oi = 0;
    if (indent > 0) {
        if (indent > out_size - 1) indent = out_size - 1;
        memcpy(out, orig_source, indent);
        oi = indent;
    }

    /* VAR_ASSIGN: show "var = value;" */
    if (cmd->type == CMD_VAR_ASSIGN) {
        const char *p = orig_source;
        while (*p && isspace((unsigned char)*p)) p++;
        char vname[16];
        int ni = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_') && ni < 15)
            vname[ni++] = *p++;
        vname[ni] = '\0';
        snprintf(out + oi, out_size - oi, "%s = %g;", vname, cmd->args[0]);
        return 1;
    }

    int nargs;
    const char *fmt = eval_fmt_for_type(cmd->type, &nargs);
    if (!fmt || nargs < 1) return 0;

    switch (nargs) {
    case 2:
        snprintf(out + oi, out_size - oi, fmt,
                 cmd->args[0], cmd->args[1]);
        break;
    case 3:
        snprintf(out + oi, out_size - oi, fmt,
                 cmd->args[0], cmd->args[1], cmd->args[2]);
        break;
    case 4:
        snprintf(out + oi, out_size - oi, fmt,
                 cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3]);
        break;
    case 5:
        snprintf(out + oi, out_size - oi, fmt,
                 cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3],
                 cmd->args[4]);
        break;
    case 6:
        snprintf(out + oi, out_size - oi, fmt,
                 cmd->args[0], cmd->args[1], cmd->args[2], cmd->args[3],
                 cmd->args[4], cmd->args[5]);
        break;
    default:
        return 0;
    }
    return 1;
}


void repl_replay_annotations_prepare(void) {
    const ReplReplayRuntimeState *replay = repl_state_replay();

    if (*replay->active && s_replay_cache_pc != *replay->pc)
        repl_replay_annotations_rebuild_cache();
    else if (!*replay->active)
        repl_replay_annotations_invalidate();
}

int repl_replay_annotation_extra_rows_for_line(int cmd_idx) {
    const ReplReplayRuntimeState *replay = repl_state_replay();

    if (!*replay->active)
        return 0;
    if (!*replay->expand_args)
        return 0;
    if (cmd_idx < 0 || cmd_idx >= repl_state_document_count())
        return 0;
    if (cmd_idx != *replay->src_line_idx)
        return 0;
    if (!repl_state_document_cmds_mut()[cmd_idx].has_vars)
        return 0;
    if (repl_state_document_cmds_mut()[cmd_idx].type == CMD_VAR_ASSIGN)
        return 0;
    return 2;
}
