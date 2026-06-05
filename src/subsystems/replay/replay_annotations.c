/*
 * src/subsystems/replay/replay_annotations.c -- Code-panel replay variable annotations.
 */
#include "repl/core_internal.h"
#include "repl/parser.h"
#include "subsystems/replay/replay.h"
#include "repl/state_owners.h"
#include "subsystems/replay/replay_state.h"
#include "subsystems/replay/replay_annotations.h"
#include "config.h"        /* REPL_STATUS_TEXT_MAX */

/* Inline-comment scratch buffer for display-text formatting. Local to
 * this TU — the editor's MAX_INPUT_LEN used to be the source of this
 * value, but Phase 4 of feature/source-document-port.md dropped the
 * editor/state.h include. 1024 preserves the original headroom; final
 * output gets truncated to out_size or to CODE_ANNOTATION_TEXT_MAX
 * downstream. */
#ifndef REPL_REPLAY_COMMENT_BUF
#define REPL_REPLAY_COMMENT_BUF 1024
#endif

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
static float s_replay_scratch_snap[MAX_COMMANDS][REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
static int   s_replay_predef_snap_valid[MAX_COMMANDS];

/* Per-frame editor-text view, set by the public entry points
 * (`replay_annotations_prepare`,
 *  `replay_code_panel_get_command_display_text`). Static helpers
 * read source text through this view instead of calling
 * `editor_buffer_line` globally — that keeps the module's source-text
 * dependency declared at the API boundary as a SourceTextView
 * parameter rather than a hidden global reach-through. The view is
 * refreshed at frame start; the cache invariant matches the frame's
 * snapshot of the editor buffer. */
static SourceTextView s_replay_text_view;

static void replay_build_predef_snapshots(void);
static int replay_eval_expr_with_state(
    int flat_idx, const char *expr,
    const float *predef_vals,
    const float scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN],
    float *out_value);
typedef void (*ReplayBeforeStepFn)(int pc, const GLCmd *cmd,
                                   const float *vals,
                                   const float scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]);

static const char *replay_document_text(int cmd_idx) {
    const char *text = source_text_line(s_replay_text_view, cmd_idx);
    return (text && text[0]) ? text : "";
}

static const char *replay_visible_text(int cmd_idx) {
    if (cmd_idx < 0 || cmd_idx >= repl_state_document_count())
        return "";
    return replay_document_text(cmd_idx);
}

static const char *replay_flat_text(int flat_idx) {
    const GLCmd *flat_cmd;

    if (flat_idx < 0 || flat_idx >= repl_state_flat_program_count())
        return "";

    flat_cmd = &repl_state_flat_program_cmds()[flat_idx];
    if (flat_cmd->src_cmd_idx < 0 || flat_cmd->src_cmd_idx >= repl_state_document_count())
        return "";

    return replay_document_text(flat_cmd->src_cmd_idx);
}

static void replay_annotations_rebuild_cache(void) {
    ReplayRuntimeState replay = replay_state_view();
    const GLCmd *flat_cmds = repl_state_flat_program_cmds();
    memset(s_replay_flat_map, 0xff, sizeof(int) * (size_t)repl_state_document_count());

    /* Backward pass: first match per src_cmd_idx = most recent execution */
    for (int j = replay.pc - 1; j >= 0; j--) {
        int src = flat_cmds[j].src_cmd_idx;
        if (src >= 0 && src < repl_state_document_count() &&
            s_replay_flat_map[src] == -1 && flat_cmds[j].valid)
            s_replay_flat_map[src] = j;
    }

    s_replay_current_flat_idx = (replay.src_line_idx >= 0 &&
                                 replay.src_line_idx < repl_state_document_count())
                                ? s_replay_flat_map[replay.src_line_idx] : -1;

    /* Single forward pass builds per-src predef snapshots */
    replay_build_predef_snapshots();

    s_replay_cache_pc = replay.pc;
}

static void replay_annotations_invalidate(void) {
    s_replay_cache_pc = -2;
}

/* Find the most recent flat command for a source line, at or before replay_pc */
static int replay_annotation_flat_cmd_for_source(int src_line) {
    ReplayRuntimeState replay = replay_state_view();
    const GLCmd *flat_cmds = repl_state_flat_program_cmds();

    if (replay.pc <= 0) return -1;
    /* Use per-frame cache when available */
    if (s_replay_cache_pc == replay.pc &&
        src_line >= 0 && src_line < repl_state_document_count())
        return s_replay_flat_map[src_line];
    for (int j = replay.pc - 1; j >= 0; j--) {
        if (flat_cmds[j].src_cmd_idx == src_line && flat_cmds[j].valid)
            return j;
    }
    return -1;
}

static int replay_current_flat_cmd(void) {
    ReplayRuntimeState replay = replay_state_view();

    if (replay.src_line_idx < 0)
        return -1;
    if (s_replay_cache_pc == replay.pc)
        return s_replay_current_flat_idx;
    return replay_annotation_flat_cmd_for_source(replay.src_line_idx);
}

static int format_evaluated_cmd(const GLCmd *cmd, const char *orig_source,
                                char *out, int out_size);

static const char *skip_leading_ws(const char *s) {
    while (s && *s && isspace((unsigned char)*s))
        s++;
    return s ? s : "";
}

static const char *replay_scratch_name(int array_idx) {
    switch (array_idx) {
    case 0: return "A";
    case 1: return "B";
    case 2: return "C";
    default: return "?";
    }
}

static const char *replay_find_matching_square(const char *open) {
    int depth = 0;

    if (!open || *open != '[')
        return NULL;

    for (const char *p = open; *p; p++) {
        if (*p == '[')
            depth++;
        else if (*p == ']') {
            depth--;
            if (depth == 0)
                return p;
        }
    }
    return NULL;
}

static void replay_subst_scratch_reads(
    int flat_idx,
    const char *source,
    char *out,
    int out_size,
    const float *predef_vals,
    const float scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]) {
    int oi = 0;
    const char *p = source;

    if (!out || out_size <= 0) {
        return;
    }
    if (!source)
        return;

    while (*p && oi < out_size - 1) {
        if (!isalpha((unsigned char)*p) && *p != '_') {
            out[oi++] = *p++;
            continue;
        }

        const char *start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_'))
            p++;

        {
            int len = (int)(p - start);
            char name[REPL_PREDEF_NAME_MAX];
            const char *bracket = skip_leading_ws(p);
            int array_idx;

            if (len <= 0 || len >= (int)sizeof(name)) {
                if (oi + len >= out_size)
                    len = out_size - oi - 1;
                memcpy(out + oi, start, (size_t)len);
                oi += len;
                continue;
            }

            memcpy(name, start, (size_t)len);
            name[len] = '\0';
            array_idx = repl_eval_scratch_array_index(name);
            if (array_idx >= 0 && *bracket == '[') {
                const char *close = replay_find_matching_square(bracket);
                if (close) {
                    char index_expr[MAX_LINE_LEN] = "";
                    float index_value = 0.0f;
                    int expr_len = (int)(close - bracket - 1);
                    if (expr_len > 0) {
                        if (expr_len >= (int)sizeof(index_expr))
                            expr_len = (int)sizeof(index_expr) - 1;
                        memcpy(index_expr, bracket + 1, (size_t)expr_len);
                        index_expr[expr_len] = '\0';
                    }
                    if (replay_eval_expr_with_state(flat_idx, index_expr,
                                                    predef_vals, scratch_arrays,
                                                    &index_value)) {
                        int elem_idx = (int)index_value;
                        if (elem_idx >= 0 && elem_idx < REPL_SCRATCH_ARRAY_LEN) {
                            oi += snprintf(out + oi, out_size - oi, "%g",
                                           scratch_arrays[array_idx][elem_idx]);
                            p = close + 1;
                            continue;
                        }
                    }

                    len = (int)(close + 1 - start);
                    if (oi + len >= out_size)
                        len = out_size - oi - 1;
                    memcpy(out + oi, start, (size_t)len);
                    oi += len;
                    p = close + 1;
                    continue;
                }
            }

            len = (int)(p - start);
            if (oi + len >= out_size)
                len = out_size - oi - 1;
            memcpy(out + oi, start, (size_t)len);
            oi += len;
        }
    }

    out[oi] = '\0';
}

static const char *replay_skip_number(const char *s) {
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
        const char *num_end = replay_skip_number(s);

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
    const GLCmd *flat_cmds = repl_state_flat_program_cmds();

    if (flat_idx < 0 || current_flat_idx < 0)
        return 1;

    if (flat_cmds[flat_idx].func_scope_mask !=
        flat_cmds[current_flat_idx].func_scope_mask)
        return 0;

    flat_vars = &repl_state_flat_program_local_vars()[flat_idx];
    cur_vars = &repl_state_flat_program_local_vars()[current_flat_idx];

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
    ReplayRuntimeState replay = replay_state_view();
    int current_flat_idx = replay_current_flat_cmd();
    const GLCmd *flat_cmds = repl_state_flat_program_cmds();

    if (replay.pc <= 0)
        return -1;

    /* Try cached map: O(1) lookup + context check */
    if (s_replay_cache_pc == replay.pc &&
        src_line >= 0 && src_line < repl_state_document_count()) {
        int cached = s_replay_flat_map[src_line];
        if (cached >= 0 &&
            (current_flat_idx < 0 ||
             cached == current_flat_idx ||
             replay_flat_cmd_context_matches(cached, current_flat_idx)))
            return cached;
    }

    /* Fallback: full scan (only for context mismatch in function bodies) */
    for (int j = replay.pc - 1; j >= 0; j--) {
        if (flat_cmds[j].src_cmd_idx != src_line || !flat_cmds[j].valid)
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
        const FlatCmdLocalVars *lcvars = &repl_state_flat_program_local_vars()[flat_idx];
        for (int i = 0; i < lcvars->num_vars && nv < max_out; i++)
            out[nv++] = lcvars->vars[i];
    }

    /* NOTE (#3 follow-up — annotation simulator latent bug): when
     * `predef_vals` carries the replay-baseline snapshot, the values
     * here are SAVED slot[i] values but the names below come from the
     * CURRENT predef table. A workspace switch / scene load / undo
     * across @declare between replay_start and this annotation render
     * reshapes the live table, so current slot i no longer corresponds
     * to the saved slot i and the simulator pairs mismatched
     * name/value pairs. The fade-plan restore path was the audit's
     * primary triggerable failure and is fixed via the widened
     * snapshot threaded through ReplayFadePlan; doing the same here
     * needs the saved names too — a follow-up. */
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
    char used_names[MAX_PREDEF_VARS + MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
    float used_vals[MAX_PREDEF_VARS + MAX_EXPR_VARS];
    int num_used = 0;
    int max_used = MAX_PREDEF_VARS + MAX_EXPR_VARS;

    int oi = 0;
    const char *p = source;
    while (*p && oi < out_size - 20) {
        const char *num_end = replay_skip_number(p);

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
            char found_name[REPL_PREDEF_NAME_MAX] = "";

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

static int replay_eval_expr_with_state(
    int flat_idx, const char *expr,
    const float *predef_vals,
    const float scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN],
    float *out_value) {
    char repl_expr[MAX_LINE_LEN];
    ExprVar vars[MAX_PREDEF_VARS + MAX_EXPR_VARS];
    float saved_scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    int nv;
    ExprCtx ctx;

    if (!expr || !out_value)
        return 0;

    repl_eval_c_expr_to_repl(expr, repl_expr, sizeof(repl_expr));
    nv = build_visible_vars_from_predef_values(flat_idx, predef_vals,
                                               vars,
                                               (int)(sizeof(vars) / sizeof(vars[0])));
    if (scratch_arrays) {
        repl_eval_copy_scratch_arrays(saved_scratch);
        repl_eval_restore_scratch_arrays(scratch_arrays);
    }
    ctx.p = repl_expr;
    ctx.vars = vars;
    ctx.num_vars = nv;
    *out_value = repl_eval_expr(&ctx);
    if (scratch_arrays)
        repl_eval_restore_scratch_arrays(saved_scratch);
    return 1;
}

static void replay_init_sim_state(float *vals, int max_vals,
                                  float scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]) {
    ReplayRuntimeState replay = replay_state_view();

    if (!vals || max_vals < g_num_predef_vars)
        return;

    if (replay.active) {
        replay_copy_baseline_predef_values(vals, max_vals);
        if (scratch)
            replay_copy_baseline_scratch_arrays(scratch);
    } else {
        for (int i = 0; i < g_num_predef_vars && i < max_vals; i++)
            vals[i] = g_predef_vars[i].value;
        if (scratch)
            repl_eval_copy_scratch_arrays(scratch);
    }
}

static int replay_simulate_runtime_until(
    int target_pc,
    float *vals,
    int max_vals,
    float scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN],
    ReplayBeforeStepFn before_step) {
    const GLCmd *flat_cmds = repl_state_flat_program_cmds();
    int pc = 0;
    int goto_count = 0;

    if (!vals || max_vals < g_num_predef_vars)
        return 0;

    replay_init_sim_state(vals, max_vals, scratch);

    if (target_pc < 0)
        target_pc = 0;
    if (target_pc > repl_state_flat_program_count())
        target_pc = repl_state_flat_program_count();

    while (pc < target_pc) {
        if (before_step)
            before_step(pc, &flat_cmds[pc], vals, scratch);

        if (!flat_cmds[pc].valid) {
            pc++;
            continue;
        }

        switch (flat_cmds[pc].type) {
        case CMD_VAR_ASSIGN: {
            int vi = flat_cmds[pc].var_idx;
            float value = flat_cmds[pc].args[0];
            if (vi >= 0 && vi < g_num_predef_vars)
                vals[vi] = value;
            break;
        }
        case CMD_SCRATCH_ASSIGN: {
            int array_idx = (int)flat_cmds[pc].args[0];
            int elem_idx = (int)flat_cmds[pc].args[1];
            float value = flat_cmds[pc].args[2];

            if (scratch && array_idx >= 0 && array_idx < REPL_SCRATCH_ARRAY_COUNT &&
                elem_idx >= 0 && elem_idx < REPL_SCRATCH_ARRAY_LEN)
                scratch[array_idx][elem_idx] = value;
            break;
        }
        case CMD_IF_BEGIN: {
            float cond = flat_cmds[pc].args[0];
            if (flat_cmds[pc].has_vars) {
                char cond_text[MAX_LINE_LEN];
                if (repl_extract_paren_payload(replay_flat_text(pc),
                                               cond_text, sizeof(cond_text)) &&
                    cond_text[0]) {
                    replay_eval_expr_with_state(pc, cond_text, vals,
                                                scratch, &cond);
                }
            }
            if (cond == 0.0f) {
                int depth = 1;
                while (depth > 0 && ++pc < target_pc) {
                    if (before_step)
                        before_step(pc, &flat_cmds[pc], vals, scratch);
                    if (flat_cmds[pc].type == CMD_IF_BEGIN) depth++;
                    else if (flat_cmds[pc].type == CMD_IF_END) depth--;
                }
            }
            break;
        }
        case CMD_GOTO: {
            char label[REPL_GOTO_LABEL_MAX];
            if (!repl_extract_goto_label(replay_flat_text(pc),
                                         label, sizeof(label)))
                break;
            if (goto_count++ > REPL_GOTO_LOOP_LIMIT)
                return 0;
            for (int li = 0; li < repl_state_flat_program_count(); li++) {
                char target_label[REPL_GOTO_LABEL_MAX];
                if (flat_cmds[li].valid &&
                    flat_cmds[li].type == CMD_GOTO_LABEL &&
                    repl_extract_label_name(replay_flat_text(li),
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

static void replay_snapshot_if_mapped(
    int pc,
    const GLCmd *cmd,
    const float *vals,
    const float scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]) {
    if (!cmd)
        return;

    if (cmd->src_cmd_idx >= 0 && cmd->src_cmd_idx < repl_state_document_count() &&
        s_replay_flat_map[cmd->src_cmd_idx] == pc &&
        !s_replay_predef_snap_valid[cmd->src_cmd_idx]) {
        memcpy(s_replay_predef_snap[cmd->src_cmd_idx], vals,
               sizeof(float) * (size_t)g_num_predef_vars);
        memcpy(s_replay_scratch_snap[cmd->src_cmd_idx], scratch,
               sizeof(s_replay_scratch_snap[cmd->src_cmd_idx]));
        s_replay_predef_snap_valid[cmd->src_cmd_idx] = 1;
    }
}

static int replay_copy_runtime_state_before_flat_cmd(
    int target_pc,
    float *out_vals,
    int max_vals,
    float out_scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]) {
    /* Mirror executor.c CMD_VAR_ASSIGN / CMD_SCRATCH_ASSIGN by applying the
     * precomputed flat args directly; replay doesn't re-flatten here. */
    return replay_simulate_runtime_until(target_pc, out_vals, max_vals,
                                         out_scratch, NULL);
}

static int replay_load_runtime_state_for(
    int cmd_idx,
    int flat_idx,
    float *predef_vals,
    float scratch_vals[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]) {
    ReplayRuntimeState replay = replay_state_view();

    if (s_replay_cache_pc == replay.pc &&
        cmd_idx >= 0 && cmd_idx < repl_state_document_count() &&
        s_replay_predef_snap_valid[cmd_idx] &&
        s_replay_flat_map[cmd_idx] == flat_idx) {
        memcpy(predef_vals, s_replay_predef_snap[cmd_idx],
               sizeof(float) * (size_t)g_num_predef_vars);
        memcpy(scratch_vals, s_replay_scratch_snap[cmd_idx],
               sizeof(s_replay_scratch_snap[cmd_idx]));
        return 1;
    }

    return replay_copy_runtime_state_before_flat_cmd(flat_idx,
                                                     predef_vals,
                                                     MAX_PREDEF_VARS,
                                                     scratch_vals);
}

/* Single-pass forward simulation that builds predef snapshots for every
 * source command whose flat_idx appears in s_replay_flat_map.  Each snapshot
 * captures the predef variable state BEFORE the command at that flat_idx
 * executes, matching replay_copy_predef_values_before_flat_cmd semantics.
 * Total cost: O(replay_pc), called once per frame during cache rebuild. */
static void replay_build_predef_snapshots(void) {
    ReplayRuntimeState replay = replay_state_view();
    /* Zero-initialized so no later read can hit an indeterminate slot if a
     * branch below fills only [0, g_num_predef_vars). */
    float vals[MAX_PREDEF_VARS] = {0};
    float scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    int target_pc = replay.pc;

    memset(s_replay_predef_snap_valid, 0, sizeof(int) * (size_t)repl_state_document_count());

    (void)replay_simulate_runtime_until(target_pc, vals, MAX_PREDEF_VARS,
                                        scratch, replay_snapshot_if_mapped);
}

static int build_replay_assignment_inline_comment(int cmd_idx, int flat_idx,
                                                  char *out, int out_size) {
    /* Zero-init: the fill helper writes only [0, g_num_predef_vars). */
    float predef_vals[MAX_PREDEF_VARS] = {0};
    float scratch_vals[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    ExprVar visible_vars[MAX_PREDEF_VARS + MAX_EXPR_VARS];
    char rhs_subst[MAX_LINE_LEN];
    char name[REPL_PREDEF_NAME_MAX];
    char index_expr[MAX_LINE_LEN];
    char rhs[MAX_LINE_LEN];
    int nv;
    CmdType type;

    if (!out || out_size <= 0)
        return 0;
    out[0] = '\0';

    if (!replay_load_runtime_state_for(cmd_idx, flat_idx,
                                       predef_vals, scratch_vals)) {
        return 0;
    }

    nv = build_visible_vars_from_predef_values(flat_idx, predef_vals,
                                               visible_vars,
                                               (int)(sizeof(visible_vars) / sizeof(visible_vars[0])));
    type = repl_state_document_cmds()[cmd_idx].type;
    if (type == CMD_VAR_ASSIGN) {
        if (!repl_extract_assignment_parts(replay_document_text(cmd_idx),
                                           name, sizeof(name),
                                           rhs, sizeof(rhs)) ||
            !name[0] || !rhs[0])
            return 0;

        {
            float value = repl_state_flat_program_cmds()[flat_idx].args[0];
            subst_visible_vars(rhs, rhs_subst, sizeof(rhs_subst),
                               NULL, 0, visible_vars, nv);
            replay_eval_expr_with_state(flat_idx, rhs, predef_vals, scratch_vals,
                                        &value);

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
    }

    name[0] = '\0';
    index_expr[0] = '\0';
    rhs[0] = '\0';
    if (type != CMD_SCRATCH_ASSIGN ||
        !repl_extract_assignment_target_parts(replay_document_text(cmd_idx),
                                             name, sizeof(name),
                                             index_expr, sizeof(index_expr),
                                             rhs, sizeof(rhs)) ||
        !rhs[0])
        return 0;

    {
        float resolved_index = repl_state_flat_program_cmds()[flat_idx].args[1];
        float value = repl_state_flat_program_cmds()[flat_idx].args[2];
        char rhs_with_scratch[MAX_LINE_LEN];
        replay_subst_scratch_reads(flat_idx, rhs, rhs_with_scratch,
                                   sizeof(rhs_with_scratch),
                                   predef_vals, scratch_vals);
        subst_visible_vars(rhs_with_scratch, rhs_subst, sizeof(rhs_subst),
                           NULL, 0, visible_vars, nv);
        if (index_expr[0])
            replay_eval_expr_with_state(flat_idx, index_expr, predef_vals,
                                        scratch_vals, &resolved_index);
        replay_eval_expr_with_state(flat_idx, rhs, predef_vals, scratch_vals,
                                    &value);

        if (rhs_subst[0] &&
            strcmp(skip_leading_ws(rhs_subst), skip_leading_ws(rhs)) != 0 &&
            !expr_has_visible_vars(rhs_subst, visible_vars, nv)) {
            snprintf(out, out_size, " // %s[%d] = %s = %g",
                     name[0] ? name : replay_scratch_name((int)repl_state_flat_program_cmds()[flat_idx].args[0]),
                     (int)resolved_index,
                     skip_leading_ws(rhs_subst), value);
        } else {
            snprintf(out, out_size, " // %s[%d] = %g",
                     name[0] ? name : replay_scratch_name((int)repl_state_flat_program_cmds()[flat_idx].args[0]),
                     (int)resolved_index, value);
        }
        return 1;
    }
}

int replay_code_panel_get_command_display_text(SourceTextView text,
                                                    int cmd_idx,
                                                    char *out, int out_size) {
    ReplayRuntimeState replay = replay_state_view();
    s_replay_text_view = text;
    int flat_idx;
    char comment[REPL_REPLAY_COMMENT_BUF];

    if (!out || out_size <= 0)
        return 0;
    out[0] = '\0';

    if (cmd_idx < 0 || cmd_idx >= repl_state_document_count())
        return 0;

    const char *base = replay_visible_text(cmd_idx);

    snprintf(out, out_size, "%s", base);

    if (!replay.active ||
        !replay.expand_args ||
        !repl_state_document_cmds()[cmd_idx].has_vars)
        return 1;

    if (repl_state_document_cmds()[cmd_idx].type != CMD_VAR_ASSIGN &&
        repl_state_document_cmds()[cmd_idx].type != CMD_SCRATCH_ASSIGN)
        return 1;

    flat_idx = find_replay_assignment_flat_cmd(cmd_idx);
    if (flat_idx < 0)
        return 1;

    if (build_replay_assignment_inline_comment(cmd_idx, flat_idx,
                                               comment, sizeof(comment)) &&
        comment[0]) {
        snprintf(out, out_size, "%s%s", base, comment);
    }

    return 1;
}

static int replay_build_subst_annotation(int cmd_idx, int flat_idx,
                                              char *subst, int subst_size,
                                              char *var_comment, int comment_size) {
    /* Zero-init: the fill helper writes only [0, g_num_predef_vars). */
    float predef_vals[MAX_PREDEF_VARS] = {0};
    float scratch_vals[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    ExprVar visible_vars[MAX_PREDEF_VARS + MAX_EXPR_VARS];
    int nv;

    if (!subst || subst_size <= 0)
        return 0;
    subst[0] = '\0';
    if (var_comment && comment_size > 0)
        var_comment[0] = '\0';

    if (!replay_load_runtime_state_for(cmd_idx, flat_idx,
                                       predef_vals, scratch_vals)) {
        return 0;
    }

    nv = build_visible_vars_from_predef_values(flat_idx, predef_vals,
                                               visible_vars,
                                               (int)(sizeof(visible_vars) / sizeof(visible_vars[0])));
    return subst_visible_vars(replay_document_text(cmd_idx), subst, subst_size,
                              var_comment, comment_size,
                              visible_vars, nv);
}

static int replay_build_eval_annotation(int cmd_idx, int flat_idx,
                                             char *eval_buf, int eval_size) {
    /* Zero-init: the fill helper writes only [0, g_num_predef_vars). */
    float predef_vals[MAX_PREDEF_VARS] = {0};
    float scratch_vals[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    float saved_scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    ExprVar visible_vars[MAX_PREDEF_VARS + MAX_EXPR_VARS];
    int nv;

    if (!eval_buf || eval_size <= 0)
        return 0;
    eval_buf[0] = '\0';

    if (!replay_load_runtime_state_for(cmd_idx, flat_idx,
                                       predef_vals, scratch_vals)) {
        return 0;
    }

    nv = build_visible_vars_from_predef_values(flat_idx, predef_vals,
                                               visible_vars,
                                               (int)(sizeof(visible_vars) / sizeof(visible_vars[0])));
    /* Replay annotation re-parses each step's source for display.
     * Errors here are dropped — the command was already validated at
     * commit time, and a parse failure during annotation just means
     * the step renders without the evaluated-text overlay. */
    char annotation_parse_err[REPL_STATUS_TEXT_MAX]; /* deliberately unread */
    annotation_parse_err[0] = '\0';
    ReplParseContext parse_ctx = {
        .source_line_idx = cmd_idx,
        .vars = visible_vars, .num_vars = nv,
        .err_buf = annotation_parse_err,
        .err_sz  = (int)sizeof(annotation_parse_err),
    };
    ReplParsedLine eval_pl;
    repl_eval_copy_scratch_arrays(saved_scratch);
    repl_eval_restore_scratch_arrays(scratch_vals);
    if (!repl_parser_parse_command_ctx(replay_document_text(cmd_idx),
                                &eval_pl, &parse_ctx)) {
        repl_eval_restore_scratch_arrays(saved_scratch);
        return 0;
    }
    repl_eval_restore_scratch_arrays(saved_scratch);
    GLCmd eval_cmd = eval_pl.cmd;

    return format_evaluated_cmd(&eval_cmd, replay_document_text(cmd_idx),
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
    case CMD_GLUT_TORUS:       *nargs_out = 4; return "glutSolidTorus(%g, %g, %g, %g);";
    case CMD_GLUT_CUBE:        *nargs_out = 1; return "glutSolidCube(%g);";
    case CMD_GLUT_SPHERE:      *nargs_out = 3; return "glutSolidSphere(%g, %g, %g);";
    case CMD_GLUT_TEAPOT:      *nargs_out = 1; return "glutSolidTeapot(%g);";
    case CMD_GLUT_CONE:        *nargs_out = 4; return "glutSolidCone(%g, %g, %g, %g);";
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
        char vname[REPL_PREDEF_NAME_MAX];
        int ni = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_') && ni < (int)sizeof(vname) - 1)
            vname[ni++] = *p++;
        vname[ni] = '\0';
        snprintf(out + oi, out_size - oi, "%s = %g;", vname, cmd->args[0]);
        return 1;
    }

    if (cmd->type == CMD_SCRATCH_ASSIGN) {
        char name[REPL_PREDEF_NAME_MAX] = "";
        char index_expr[MAX_LINE_LEN] = "";
        char rhs[MAX_LINE_LEN] = "";
        int array_idx = (int)cmd->args[0];
        if (!repl_extract_assignment_target_parts(orig_source,
                                                 name, sizeof(name),
                                                 index_expr, sizeof(index_expr),
                                                 rhs, sizeof(rhs)) ||
            !name[0]) {
            snprintf(name, sizeof(name), "%s", replay_scratch_name(array_idx));
        }
        snprintf(out + oi, out_size - oi, "%s[%d] = %g;",
                 name, (int)cmd->args[1], cmd->args[2]);
        return 1;
    }

    int nargs;
    const char *fmt = eval_fmt_for_type(cmd->type, &nargs);
    if (!fmt || nargs < 1) return 0;

    /* eval_fmt_for_type only ever returns nargs in {0, 1, 2, 3, 4}.
     * Pass cmd->args[0..3] verbatim — snprintf reads up to `nargs`
     * conversions per the format and ignores trailing varargs. */
    switch (nargs) {
    case 1:
        snprintf(out + oi, out_size - oi, fmt, cmd->args[0]);
        break;
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
    default:
        return 0;
    }
    return 1;
}


static void replay_output_append(ReplReplayAnnotationOutput *out,
                                 int after_line_idx,
                                 ReplReplayAnnotationKind kind,
                                 const char *text,
                                 const char *aux) {
    if (!out || out->count >= REPL_REPLAY_ANNOTATION_MAX)
        return;
    ReplReplayAnnotation *row = &out->items[out->count];
    row->after_line_idx = after_line_idx;
    row->kind           = kind;
    if (text)
        snprintf(row->text, sizeof(row->text), "%s", text);
    else
        row->text[0] = '\0';
    if (aux)
        snprintf(row->aux, sizeof(row->aux), "%s", aux);
    else
        row->aux[0] = '\0';
    out->count++;
}

static void replay_annotations_refresh_output(ReplReplayAnnotationOutput *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    ReplayRuntimeState replay = replay_state_view();
    if (!replay.active || !replay.expand_args)
        return;
    int cmd_idx = replay.src_line_idx;
    if (cmd_idx < 0 || cmd_idx >= repl_state_document_count())
        return;
    const GLCmd *cmd = repl_state_document_cmd_at(cmd_idx);
    if (!cmd || !cmd->has_vars || cmd->type == CMD_VAR_ASSIGN ||
        cmd->type == CMD_SCRATCH_ASSIGN)
        return;
    int flat_idx = replay_annotation_flat_cmd_for_source(cmd_idx);
    if (flat_idx < 0)
        return;

    char subst[REPL_REPLAY_ANNOTATION_TEXT_MAX];
    char var_comment[REPL_REPLAY_ANNOTATION_AUX_MAX];
    if (replay_build_subst_annotation(cmd_idx, flat_idx,
                                           subst, sizeof(subst),
                                           var_comment, sizeof(var_comment)) > 0) {
        replay_output_append(out, cmd_idx,
                             REPL_REPLAY_ANNOTATION_KIND_SUBST,
                             subst, var_comment);
    }

    char eval_buf[REPL_REPLAY_ANNOTATION_TEXT_MAX];
    if (replay_build_eval_annotation(cmd_idx, flat_idx,
                                          eval_buf, sizeof(eval_buf))) {
        replay_output_append(out, cmd_idx,
                             REPL_REPLAY_ANNOTATION_KIND_EVAL,
                             eval_buf, NULL);
    }
}

void replay_annotations_prepare(SourceTextView text,
                                     ReplReplayAnnotationOutput *out) {
    ReplayRuntimeState replay = replay_state_view();

    s_replay_text_view = text;

    if (replay.active && s_replay_cache_pc != replay.pc)
        replay_annotations_rebuild_cache();
    else if (!replay.active)
        replay_annotations_invalidate();

    replay_annotations_refresh_output(out);
}
