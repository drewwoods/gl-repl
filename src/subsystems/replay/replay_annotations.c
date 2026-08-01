/*
 * src/subsystems/replay/replay_annotations.c -- Code-panel replay variable annotations.
 */
#include <stdio.h>
#include <string.h>
#include "repl/flatten.h"
#include "source_document.h"
#include "repl/command.h"
#include "repl/eval.h"
#include "repl/text_helpers.h"
#include "repl/control_flow.h"
#include "repl/parser.h"
#include "repl/source_scope.h"
#include "subsystems/replay/replay.h"
#include "repl/state_views.h"
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
static int   s_replay_flat_map[MAX_EDITOR_COMMANDS];         /* src_cmd_idx → flat_idx */
static int   s_replay_current_flat_idx = -1;          /* flat cmd for src_line */
/* Predef-variable snapshots: one per source command, taken at the flat_idx
 * stored in s_replay_flat_map[src].  Built by a single O(replay_pc) forward
 * simulation so each snapshot reflects the correct variable state BEFORE
 * executing that specific flat command. */
static ReplPredefSnapshot s_replay_predef_snap[MAX_EDITOR_COMMANDS];
static float s_replay_scratch_snap[MAX_EDITOR_COMMANDS][REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
static int   s_replay_predef_snap_valid[MAX_EDITOR_COMMANDS];

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
    const ReplPredefSnapshot *predef,
    const float scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN],
    float *out_value);
typedef void (*ReplayBeforeStepFn)(int pc, const GLCmd *cmd,
                                   const ReplPredefSnapshot *predef,
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

/* Rewrite every scratch-array read in `source` to its simulated value
 * at this replay step: each `A[idx]` becomes the numeric element value,
 * with the index expression evaluated against the simulated runtime
 * state (bracket-aware, so nested subscripts survive). An index that
 * fails to evaluate or lands out of range copies through verbatim, as
 * do all non-scratch identifiers. The caller passes only the RHS of a
 * scratch assignment, which is what keeps the LHS target subscript
 * unsubstituted in the displayed line. */
static void replay_subst_scratch_reads(
    int flat_idx,
    const char *source,
    char *out,
    int out_size,
    const ReplPredefSnapshot *predef,
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
                                                    predef, scratch_arrays,
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
    const GLCmd *flat_cmds = repl_state_flat_program_cmds();
    const GLCmd *candidate;
    const GLCmd *current;

    if (flat_idx < 0 || current_flat_idx < 0)
        return 1;

    candidate = &flat_cmds[flat_idx];
    current = &flat_cmds[current_flat_idx];

    /* Match the invocation's stable provenance, never its mutable local
     * values. Local snapshots are effective state at each command, so two
     * rows in the same call are expected to differ after an assignment.
     * Temporal disambiguation for repeated calls at one source site comes
     * from find_replay_assignment_flat_cmd's backward scan: the nearest
     * matching execution is the current invocation once that row has run,
     * and the previous invocation before it has. */
    return candidate->func_scope_mask == current->func_scope_mask &&
           candidate->call_depth == current->call_depth &&
           candidate->call_src_cmd_idx == current->call_src_cmd_idx &&
           candidate->root_call_src_cmd_idx == current->root_call_src_cmd_idx;
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

static int build_visible_vars_from_predef_snapshot(int flat_idx,
                                                   const ReplPredefSnapshot *predef,
                                                   ExprVar *out, int max_out) {
    int nv = 0;

    if (!out || max_out <= 0)
        return 0;

    if (flat_idx >= 0 && flat_idx < repl_state_flat_program_count()) {
        const FlatCmdLocalVars *lcvars = &repl_state_flat_program_local_vars()[flat_idx];
        for (int i = 0; i < lcvars->num_vars && nv < max_out; i++)
            out[nv++] = lcvars->vars[i];
    }

    if (predef) {
        for (int i = 0; i < predef->count && nv < max_out; i++) {
            if (visible_var_index(out, nv, predef->names[i]) >= 0)
                continue;
            strncpy(out[nv].name, predef->names[i],
                    sizeof(out[nv].name) - 1);
            out[nv].name[sizeof(out[nv].name) - 1] = '\0';
            out[nv].value = predef->vals[i];
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
    const ReplPredefSnapshot *predef,
    const float scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN],
    float *out_value) {
    char repl_expr[MAX_LINE_LEN];
    ExprVar vars[MAX_PREDEF_VARS + MAX_EXPR_VARS];
    float saved_scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    int nv;
    ExprCtx ctx = {0};

    if (!expr || !out_value)
        return 0;

    repl_eval_c_expr_to_repl(expr, repl_expr, sizeof(repl_expr));
    nv = build_visible_vars_from_predef_snapshot(flat_idx, predef,
                                                 vars,
                                                 (int)(sizeof(vars) / sizeof(vars[0])));
    if (scratch_arrays) {
        repl_eval_copy_scratch_arrays(saved_scratch);
        repl_eval_restore_scratch_arrays(scratch_arrays);
    }
    ctx.p = repl_expr;
    ctx.vars = vars;
    ctx.num_vars = nv;
    if (predef) {
        ctx.predef_vars = vars;
        ctx.predef_count = nv;
    }
    *out_value = repl_eval_expr(&ctx);
    if (scratch_arrays)
        repl_eval_restore_scratch_arrays(saved_scratch);
    return 1;
}

static void replay_init_sim_state(ReplPredefSnapshot *predef,
                                  float scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]) {
    ReplayRuntimeState replay = replay_state_view();

    if (!predef)
        return;

    if (replay.active) {
        replay_copy_baseline_predef_snapshot(predef);
        if (scratch)
            replay_copy_baseline_scratch_arrays(scratch);
    } else {
        repl_eval_capture_predef_snapshot(predef);
        if (scratch)
            repl_eval_copy_scratch_arrays(scratch);
    }
}

/* Re-derive the variable / scratch-array state the executor would hold
 * just before flat pc `target_pc`, without touching GL or live state: a
 * dry-run walk over the flat program from a baseline snapshot, applying
 * only the state-mutating semantics — var assigns, scratch assigns,
 * if-blocks (skipping false bodies), and goto jumps (with the same loop
 * limit as the executor). Geometry/state commands are skipped over.
 * This is what lets the paused-replay annotations show the values a
 * command actually executed with, since the live predef table has long
 * since moved on. `before_step` (optional) observes every visited pc,
 * including ones inside skipped if-bodies. Returns 0 on bad args or a
 * goto loop overrun, 1 otherwise. */
static int replay_simulate_runtime_until(
    int target_pc,
    ReplPredefSnapshot *predef,
    float scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN],
    ReplayBeforeStepFn before_step) {
    const GLCmd *flat_cmds = repl_state_flat_program_cmds();
    int pc = 0;
    int goto_count = 0;

    if (!predef)
        return 0;

    replay_init_sim_state(predef, scratch);

    if (target_pc < 0)
        target_pc = 0;
    if (target_pc > repl_state_flat_program_count())
        target_pc = repl_state_flat_program_count();

    while (pc < target_pc) {
        if (before_step)
            before_step(pc, &flat_cmds[pc], predef, scratch);

        if (!flat_cmds[pc].valid) {
            pc++;
            continue;
        }

        switch (flat_cmds[pc].type) {
        case CMD_VAR_ASSIGN: {
            int vi = flat_cmds[pc].var_idx;
            float value = flat_cmds[pc].args[0];
            if (vi >= 0 && vi < predef->count)
                predef->vals[vi] = value;
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
                    replay_eval_expr_with_state(pc, cond_text, predef,
                                                scratch, &cond);
                }
            }
            if (cond == 0.0f) {
                int depth = 1;
                while (depth > 0 && ++pc < target_pc) {
                    if (before_step)
                        before_step(pc, &flat_cmds[pc], predef, scratch);
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
    const ReplPredefSnapshot *predef,
    const float scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]) {
    if (!cmd)
        return;

    if (cmd->src_cmd_idx >= 0 && cmd->src_cmd_idx < repl_state_document_count() &&
        s_replay_flat_map[cmd->src_cmd_idx] == pc &&
        !s_replay_predef_snap_valid[cmd->src_cmd_idx]) {
        if (predef)
            s_replay_predef_snap[cmd->src_cmd_idx] = *predef;
        memcpy(s_replay_scratch_snap[cmd->src_cmd_idx], scratch,
               sizeof(s_replay_scratch_snap[cmd->src_cmd_idx]));
        s_replay_predef_snap_valid[cmd->src_cmd_idx] = 1;
    }
}

static int replay_copy_runtime_state_before_flat_cmd(
    int target_pc,
    ReplPredefSnapshot *out_predef,
    float out_scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]) {
    /* Mirror executor.c CMD_VAR_ASSIGN / CMD_SCRATCH_ASSIGN by applying the
     * precomputed flat args directly; replay doesn't re-flatten here. */
    return replay_simulate_runtime_until(target_pc, out_predef,
                                         out_scratch, NULL);
}

static int replay_load_runtime_state_for(
    int cmd_idx,
    int flat_idx,
    ReplPredefSnapshot *predef,
    float scratch_vals[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]) {
    ReplayRuntimeState replay = replay_state_view();

    if (s_replay_cache_pc == replay.pc &&
        cmd_idx >= 0 && cmd_idx < repl_state_document_count() &&
        s_replay_predef_snap_valid[cmd_idx] &&
        s_replay_flat_map[cmd_idx] == flat_idx) {
        if (predef)
            *predef = s_replay_predef_snap[cmd_idx];
        memcpy(scratch_vals, s_replay_scratch_snap[cmd_idx],
               sizeof(s_replay_scratch_snap[cmd_idx]));
        return 1;
    }

    return replay_copy_runtime_state_before_flat_cmd(flat_idx,
                                                     predef,
                                                     scratch_vals);
}

/* Single-pass forward simulation that builds predef snapshots for every
 * source command whose flat_idx appears in s_replay_flat_map.  Each snapshot
 * captures the predef variable state BEFORE the command at that flat_idx
 * executes, matching replay_copy_predef_values_before_flat_cmd semantics.
 * Total cost: O(replay_pc), called once per frame during cache rebuild. */
static void replay_build_predef_snapshots(void) {
    ReplayRuntimeState replay = replay_state_view();
    ReplPredefSnapshot predef;
    float scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    int target_pc = replay.pc;

    memset(&predef, 0, sizeof(predef));
    memset(s_replay_predef_snap_valid, 0, sizeof(int) * (size_t)repl_state_document_count());

    (void)replay_simulate_runtime_until(target_pc, &predef,
                                        scratch, replay_snapshot_if_mapped);
}

static int build_replay_assignment_inline_comment(int cmd_idx, int flat_idx,
                                                  char *out, int out_size) {
    ReplPredefSnapshot predef;
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
    memset(&predef, 0, sizeof(predef));

    if (!replay_load_runtime_state_for(cmd_idx, flat_idx,
                                       &predef, scratch_vals)) {
        return 0;
    }

    nv = build_visible_vars_from_predef_snapshot(flat_idx, &predef,
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
            const GLCmd *flat_cmd = &repl_state_flat_program_cmds()[flat_idx];
            int local_target = flat_cmd->var_idx == REPL_VAR_IDX_LOCAL;
            float value = flat_cmd->args[0];

            if (local_target) {
                int target_idx = visible_var_index(visible_vars, nv, name);
                if (target_idx < 0)
                    return 0;
                /* FlatCmdLocalVars is deliberately post-write: downstream
                 * commands must observe this assignment. Substitute the
                 * target's captured pre-write value for this RHS only. */
                visible_vars[target_idx].value =
                    flat_cmd->payload.assign.prev_local_value;
            }
            subst_visible_vars(rhs, rhs_subst, sizeof(rhs_subst),
                               NULL, 0, visible_vars, nv);
            if (!local_target) {
                replay_eval_expr_with_state(flat_idx, rhs, &predef,
                                            scratch_vals, &value);
            }

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
                                   &predef, scratch_vals);
        subst_visible_vars(rhs_with_scratch, rhs_subst, sizeof(rhs_subst),
                           NULL, 0, visible_vars, nv);
        if (index_expr[0])
            replay_eval_expr_with_state(flat_idx, index_expr, &predef,
                                        scratch_vals, &resolved_index);
        replay_eval_expr_with_state(flat_idx, rhs, &predef, scratch_vals,
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

/* Resolve a function-definition header to " // half = 5.5, cells = 13, ..."
 * showing the current invocation's parameter values, but only while the
 * replay position is executing inside this function's body — the analogue
 * of the for-loop iteration readout. The values are read off the current
 * flat command's local vars, where flatten bound each param to its
 * evaluated argument (flatten_call); under recursion that is the innermost
 * live frame. parse_repl_func_signature yields the param names for both
 * bare `funcN(...)` and aliased `NAME(...)` headers. Returns 1 when a
 * comment was written. */
static int build_replay_funcdef_inline_comment(int cmd_idx, char *out, int out_size) {
    ReplayRuntimeState replay = replay_state_view();
    const FlatCmdLocalVars *local_vars = repl_state_flat_program_local_vars();
    char param_names[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
    int fn = -1;
    int param_count = 0;
    int func_end;
    int cur_flat;
    int wrote = 0;
    int oi;

    if (!out || out_size <= 0)
        return 0;
    out[0] = '\0';

    if (!parse_repl_func_signature(replay_document_text(cmd_idx), &fn,
                                   param_names, MAX_EXPR_VARS, &param_count) ||
        param_count <= 0)
        return 0;

    func_end = repl_source_scope_find_block_end(cmd_idx);
    cur_flat = replay_current_flat_cmd();

    /* Only while the replay position is inside this function's body. */
    if (cur_flat < 0 || replay.src_line_idx <= cmd_idx ||
        replay.src_line_idx >= func_end)
        return 0;

    oi = snprintf(out, out_size, " // ");
    for (int p = 0; p < param_count && oi < out_size - 1; p++) {
        int vi = visible_var_index(local_vars[cur_flat].vars,
                                   local_vars[cur_flat].num_vars,
                                   param_names[p]);
        if (vi < 0)
            continue;
        oi += snprintf(out + oi, out_size - oi, "%s%s = %g",
                       wrote ? ", " : "", param_names[p],
                       local_vars[cur_flat].vars[vi].value);
        wrote = 1;
    }
    if (!wrote) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

static int expr_text_has_ident(const char *s) {
    for (; s && *s; s++)
        if (isalpha((unsigned char)*s) || *s == '_')
            return 1;
    return 0;
}

/* Resolve a for-loop header to " // i = <iter>, n = <limit>".
 *   - iteration: the loop var's current value, shown only while the current
 *     replay position sits inside this loop's body (so it is genuinely live).
 *   - limit: the end bound, shown when it is a var-bearing expression,
 *     resolved against the current step's runtime snapshot. A literal bound
 *     (e.g. `for(i, 0, 10)`) adds no limit — the text already shows it.
 * Returns 1 when a comment was written. */
static int build_replay_for_inline_comment(int cmd_idx, char *out, int out_size) {
    ReplayRuntimeState replay = replay_state_view();
    const FlatCmdLocalVars *local_vars = repl_state_flat_program_local_vars();
    char var_name[REPL_PREDEF_NAME_MAX];
    char args_text[MAX_LINE_LEN];
    char bounds[3][MAX_LINE_LEN];
    int nbounds;
    int cur_flat;
    int have_iter = 0;
    int have_limit = 0;
    float iter_val = 0.0f;
    float limit_val = 0.0f;
    const char *end_expr = NULL;
    int oi;
    const FlatCmdActiveLoop *active_loop = NULL;

    if (!out || out_size <= 0)
        return 0;
    out[0] = '\0';

    if (!extract_for_args_text(replay_document_text(cmd_idx),
                               var_name, sizeof(var_name),
                               args_text, sizeof(args_text)) ||
        !var_name[0])
        return 0;

    cur_flat = replay_current_flat_cmd();

    /* The flat snapshot carries dynamic loop ancestry separately from lexical
     * locals, so an iterator remains annotatable while replay is executing in
     * a function called from its body. Scan backward for recursion: the same
     * source loop can be active in several call frames and the innermost one
     * is the value relevant to the current command. */
    if (cur_flat >= 0) {
        const FlatCmdLocalVars *locals = &local_vars[cur_flat];
        for (int i = locals->num_active_loops - 1; i >= 0; i--) {
            if (locals->active_loops[i].source_cmd_idx == cmd_idx) {
                active_loop = &locals->active_loops[i];
                break;
            }
        }
    }
    if (active_loop) {
        iter_val = active_loop->iter_value;
        have_iter = 1;
    }

    /* Limit: resolve the end bound when it references a variable. */
    nbounds = split_top_level_args(args_text, bounds, 3);
    if (nbounds >= 2)
        end_expr = skip_leading_ws(bounds[1]);
    if (active_loop && end_expr && expr_text_has_ident(end_expr)) {
        limit_val = active_loop->end_value;
        have_limit = 1;
    } else if (cur_flat >= 0 && end_expr && expr_text_has_ident(end_expr)) {
        ReplPredefSnapshot predef;
        float scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
        memset(&predef, 0, sizeof(predef));
        if (replay_load_runtime_state_for(replay.src_line_idx, cur_flat,
                                          &predef, scratch) &&
            replay_eval_expr_with_state(cur_flat, end_expr, &predef, scratch,
                                        &limit_val))
            have_limit = 1;
    }

    if (!have_iter && !have_limit)
        return 0;

    oi = snprintf(out, out_size, " // ");
    if (have_iter)
        oi += snprintf(out + oi, out_size - oi, "%s = %g", var_name, iter_val);
    if (have_limit && oi < out_size - 1)
        oi += snprintf(out + oi, out_size - oi, "%s%s = %g",
                       have_iter ? ", " : "", end_expr, limit_val);
    return 1;
}

int replay_code_panel_get_command_display_text(SourceTextView text,
                                                    int cmd_idx,
                                                    char *out, int out_size) {
    ReplayRuntimeState replay = replay_state_view();
    s_replay_text_view = text;
    char comment[REPL_REPLAY_COMMENT_BUF];
    CmdType type;
    int has_vars;

    if (!out || out_size <= 0)
        return 0;
    out[0] = '\0';

    if (cmd_idx < 0 || cmd_idx >= repl_state_document_count())
        return 0;

    const char *base = replay_visible_text(cmd_idx);

    snprintf(out, out_size, "%s", base);

    if (!replay.active || !replay.expand_args)
        return 1;

    type = repl_state_document_cmds()[cmd_idx].type;
    has_vars = repl_state_document_cmds()[cmd_idx].has_vars;
    comment[0] = '\0';

    if (type == CMD_VAR_ASSIGN || type == CMD_SCRATCH_ASSIGN) {
        int flat_idx;
        if (!has_vars)
            return 1;
        flat_idx = find_replay_assignment_flat_cmd(cmd_idx);
        if (flat_idx < 0)
            return 1;
        build_replay_assignment_inline_comment(cmd_idx, flat_idx,
                                               comment, sizeof(comment));
    } else if (type == CMD_FUNC_DEF) {
        /* No has_vars gate: the parameter values are per-invocation runtime
         * state, shown only while replay is executing inside the body. The
         * call site itself is intentionally left un-annotated. */
        build_replay_funcdef_inline_comment(cmd_idx, comment, sizeof(comment));
    } else if (type == CMD_FOR_BEGIN) {
        /* No has_vars gate: the loop variable is dynamic even when the
         * bounds are literals, so the iteration readout is still useful. */
        build_replay_for_inline_comment(cmd_idx, comment, sizeof(comment));
    } else if (replay.expand_args == REPLAY_EXPAND_EXPANDED) {
        /* Expanded is the strictly-one-line mode: every command whose args
         * have an evaluated form gets it appended as a `//` comment, and
         * replay_annotations_refresh_output emits no virtual rows at all.
         * No command-type gate here — format_evaluated_cmd answers "is
         * there an evaluated form?" for the whole set (vertex / color /
         * normal, their gluVertex / gluColor / gluNormal twins, glutSolid*
         * shapes, transforms, glClearColor), and returns 0 for the rest,
         * which then render unannotated rather than splitting the row. */
        char evaluated[REPL_REPLAY_ANNOTATION_TEXT_MAX];
        int flat_idx;
        if (!has_vars)
            return 1;
        flat_idx = replay_annotation_flat_cmd_for_source(cmd_idx);
        if (flat_idx < 0)
            return 1;
        if (format_evaluated_cmd(&repl_state_flat_program_cmds()[flat_idx],
                                 skip_leading_ws(base),
                                 evaluated, sizeof(evaluated))) {
            snprintf(comment, sizeof(comment), " // %s",
                     skip_leading_ws(evaluated));
        }
    } else {
        return 1;
    }

    if (comment[0])
        snprintf(out, out_size, "%s%s", base, comment);

    return 1;
}

static int replay_build_subst_annotation(int cmd_idx, int flat_idx,
                                              char *subst, int subst_size,
                                              char *var_comment, int comment_size) {
    ReplPredefSnapshot predef;
    float scratch_vals[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    ExprVar visible_vars[MAX_PREDEF_VARS + MAX_EXPR_VARS];
    int nv;

    if (!subst || subst_size <= 0)
        return 0;
    subst[0] = '\0';
    if (var_comment && comment_size > 0)
        var_comment[0] = '\0';
    memset(&predef, 0, sizeof(predef));

    if (!replay_load_runtime_state_for(cmd_idx, flat_idx,
                                       &predef, scratch_vals)) {
        return 0;
    }

    nv = build_visible_vars_from_predef_snapshot(flat_idx, &predef,
                                                 visible_vars,
                                                 (int)(sizeof(visible_vars) / sizeof(visible_vars[0])));
    return subst_visible_vars(replay_document_text(cmd_idx), subst, subst_size,
                              var_comment, comment_size,
                              visible_vars, nv);
}

static int replay_build_eval_annotation(int cmd_idx, int flat_idx,
                                             char *eval_buf, int eval_size) {
    ReplPredefSnapshot predef;
    float scratch_vals[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    float saved_scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    ExprVar visible_vars[MAX_PREDEF_VARS + MAX_EXPR_VARS];
    int nv;

    if (!eval_buf || eval_size <= 0)
        return 0;
    eval_buf[0] = '\0';
    memset(&predef, 0, sizeof(predef));

    if (!replay_load_runtime_state_for(cmd_idx, flat_idx,
                                       &predef, scratch_vals)) {
        return 0;
    }

    nv = build_visible_vars_from_predef_snapshot(flat_idx, &predef,
                                                 visible_vars,
                                                 (int)(sizeof(visible_vars) / sizeof(visible_vars[0])));
    /* Replay annotation re-parses each step's source for display.
     * Errors here are dropped — the command was already validated at
     * commit time, and a parse failure during annotation just means
     * the step renders without the evaluated-text overlay. */
    char annotation_parse_err[REPL_STATUS_TEXT_MAX]; /* deliberately unread */
    annotation_parse_err[0] = '\0';
    ReplSourceScopeView source_scope;
    repl_source_scope_view_bind(&source_scope,
                                repl_state_document_cmds(),
                                repl_state_document_count());
    ReplParseContext parse_ctx = {
        .source_line_idx = cmd_idx,
        .vars = visible_vars, .num_vars = nv,
        .err_buf = annotation_parse_err,
        .err_sz  = (int)sizeof(annotation_parse_err),
        .func_aliases = repl_func_alias_view(),
        .source_scope = &source_scope,
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
    case CMD_TESS_COLOR:       *nargs_out = 4; return "gluColor(%g, %g, %g, %g);";
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
    /* Virtual rows are Verbose's alone. Expanded keeps every readout inline
     * on the source row (replay_code_panel_get_command_display_text), so
     * Verbose is the only mode in which one source line becomes several. */
    if (!replay.active || replay.expand_args != REPLAY_EXPAND_VERBOSE)
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
