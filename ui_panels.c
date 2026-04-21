/*
 * ui_panels.c — Code panel, autocomplete, help overlay, variable sliders,
 *               and configuration menu rendering.
 *
 * Extracted from sample.c for maintainability.
 */
#include "sample.h"
#include "repl_actions.h"
#include "repl_core.h"
#include "repl_core_internal.h"
#include "repl_clipboard.h"
#include "repl_keys.h"
#include "profile_panel.h"
#include "ui_panels.h"

/* Compile-time stringify for embedding macro values in string literals */
#define _HELP_STR2(x) #x
#define _HELP_STR(x)  _HELP_STR2(x)

/* Status footer height — design: 22px strip flush against code panel bottom */
/* STATUSBAR_H lives in sample.h now so scene_render.c can lift the
 * replay HUD above the amber status strip.  */

/* ========================================================================= */
/* Layout geometry helpers                                                    */
/* ========================================================================= */

static int code_panel_layout_mode(void) {
    if (g_code_panel_layout < 0 || g_code_panel_layout >= CODE_PANEL_LAYOUT_COUNT)
        return CODE_PANEL_LAYOUT_LEFT;
    return g_code_panel_layout;
}

static int panel_span_px(int total_px) {
    int span = (int)((float)total_px * g_panel_frac);
    if (span < 1) span = 1;
    if (span > total_px) span = total_px;
    return span;
}

void code_panel_rect(int *x, int *y, int *w, int *h) {
    int layout = code_panel_layout_mode();

    if (layout == CODE_PANEL_LAYOUT_HIDDEN) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = 0;
        if (h) *h = 0;
    } else if (layout == CODE_PANEL_LAYOUT_TOP) {
        int panel_h = panel_span_px(g_win_h);
        if (x) *x = 0;
        if (y) *y = g_win_h - panel_h;
        if (w) *w = g_win_w;
        if (h) *h = panel_h;
    } else if (layout == CODE_PANEL_LAYOUT_BOTTOM) {
        int panel_h = panel_span_px(g_win_h);
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = g_win_w;
        if (h) *h = panel_h;
    } else {
        int panel_w = panel_span_px(g_win_w);
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = panel_w;
        if (h) *h = g_win_h;
    }
}

void scene_rect(int *x, int *y, int *w, int *h) {
    int layout = code_panel_layout_mode();

    if (layout == CODE_PANEL_LAYOUT_HIDDEN) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = g_win_w;
        if (h) *h = g_win_h;
    } else if (layout == CODE_PANEL_LAYOUT_TOP) {
        int panel_h = panel_span_px(g_win_h);
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = g_win_w;
        if (h) *h = g_win_h - panel_h;
    } else if (layout == CODE_PANEL_LAYOUT_BOTTOM) {
        int panel_h = panel_span_px(g_win_h);
        if (x) *x = 0;
        if (y) *y = panel_h;
        if (w) *w = g_win_w;
        if (h) *h = g_win_h - panel_h;
    } else {
        int panel_w = panel_span_px(g_win_w);
        if (x) *x = panel_w;
        if (y) *y = 0;
        if (w) *w = g_win_w - panel_w;
        if (h) *h = g_win_h;
    }
}

/* ========================================================================= */
/* Syntax color helpers                                                       */
/* ========================================================================= */

static void color_for_type(CmdType t) {
    switch (t) {
    case CMD_BEGIN:
    case CMD_END:      glColor3f(0.85f, 0.45f, 0.85f); break;
    case CMD_VERTEX3F:
    case CMD_VERTEX2F: glColor3f(0.40f, 0.90f, 0.40f); break;
    case CMD_NORMAL3F:      glColor3f(0.40f, 0.80f, 0.95f); break;
    case CMD_TRANSLATE3F:
    case CMD_SCALEF:
    case CMD_ROTATEF:
    case CMD_PUSH_MATRIX:
    case CMD_POP_MATRIX:    glColor3f(0.95f, 0.65f, 0.40f); break; /* orange */
    case CMD_COLOR_MATERIAL:
    case CMD_MATERIALF:     glColor3f(0.95f, 0.85f, 0.30f); break; /* yellow */
    case CMD_COLOR3F:
    case CMD_COLOR4F:
    case CMD_CLEAR_COLOR: glColor3f(0.95f, 0.85f, 0.30f); break; /* yellow */
    case CMD_ENABLE:
    case CMD_DISABLE:
    case CMD_SHADE_MODEL:
    case CMD_LIGHT_MODEL_I:
    case CMD_FRONT_FACE:
    case CMD_POINT_SIZE:
    case CMD_POINT_PARAMETER_FV:
    case CMD_BLEND_FUNC:
    case CMD_DEPTH_MASK: glColor3f(0.80f, 0.70f, 0.95f); break; /* lavender */
    case CMD_FOR_BEGIN:
    case CMD_FOR_END:  glColor3f(0.95f, 0.60f, 0.30f); break;
    case CMD_FUNC_DEF:
    case CMD_FUNC_END: glColor3f(0.60f, 0.85f, 0.95f); break;
    case CMD_CALL:     glColor3f(0.60f, 0.85f, 0.95f); break;
    case CMD_IF_BEGIN:
    case CMD_IF_END:   glColor3f(0.95f, 0.75f, 0.50f); break;
    case CMD_COMMENT:    glColor3f(0.45f, 0.50f, 0.45f); break;
    case CMD_VAR_ASSIGN:
    case CMD_VAR_DECLARE: glColor3f(0.55f, 0.80f, 0.95f); break;
    case CMD_LABEL:
    case CMD_GOTO:       glColor3f(0.85f, 0.55f, 0.85f); break;
    case CMD_GLU_SPHERE:
    case CMD_GLU_CYLINDER:
    case CMD_GLU_DISK:
    case CMD_GLU_PARTIAL_DISK:
    case CMD_GLUT_TORUS:  glColor3f(0.50f, 0.90f, 0.70f); break;
    case CMD_TESS_BEGIN_POLYGON:
    case CMD_TESS_BEGIN_CONTOUR:
    case CMD_TESS_END:    glColor3f(0.70f, 0.55f, 0.90f); break; /* violet */
    case CMD_TESS_NORMAL: glColor3f(0.40f, 0.80f, 0.95f); break; /* cyan */
    case CMD_TESS_COLOR:  glColor3f(0.95f, 0.85f, 0.30f); break; /* yellow */
    case CMD_TESS_VERTEX: glColor3f(0.40f, 0.90f, 0.40f); break; /* green */
    default:             glColor3f(0.70f, 0.70f, 0.70f); break;
    }
}

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

static void rebuild_replay_annotation_cache(void) {
    memset(s_replay_flat_map, 0xff, sizeof(int) * (size_t)g_num_cmds);

    /* Backward pass: first match per src_cmd_idx = most recent execution */
    for (int j = g_replay_pc - 1; j >= 0; j--) {
        int src = g_flat_cmds[j].src_cmd_idx;
        if (src >= 0 && src < g_num_cmds &&
            s_replay_flat_map[src] == -1 && g_flat_cmds[j].valid)
            s_replay_flat_map[src] = j;
    }

    s_replay_current_flat_idx = (g_replay_src_line >= 0 &&
                                 g_replay_src_line < g_num_cmds)
                                ? s_replay_flat_map[g_replay_src_line] : -1;

    /* Single forward pass builds per-src predef snapshots */
    replay_build_predef_snapshots();

    s_replay_cache_pc = g_replay_pc;
}

static void invalidate_replay_annotation_cache(void) {
    s_replay_cache_pc = -2;
}

/* Find the most recent flat command for a source line, at or before replay_pc */
static int find_replay_flat_cmd(int src_line) {
    if (g_replay_pc <= 0) return -1;
    /* Use per-frame cache when available */
    if (s_replay_cache_pc == g_replay_pc &&
        src_line >= 0 && src_line < g_num_cmds)
        return s_replay_flat_map[src_line];
    for (int j = g_replay_pc - 1; j >= 0; j--) {
        if (g_flat_cmds[j].src_cmd_idx == src_line && g_flat_cmds[j].valid)
            return j;
    }
    return -1;
}

static int replay_current_flat_cmd(void) {
    if (g_replay_src_line < 0)
        return -1;
    if (s_replay_cache_pc == g_replay_pc)
        return s_replay_current_flat_idx;
    return find_replay_flat_cmd(g_replay_src_line);
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

    if (g_flat_cmds[flat_idx].func_scope_mask !=
        g_flat_cmds[current_flat_idx].func_scope_mask)
        return 0;

    flat_vars = &g_flat_cmd_local_vars[flat_idx];
    cur_vars = &g_flat_cmd_local_vars[current_flat_idx];

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
    int current_flat_idx = replay_current_flat_cmd();

    if (g_replay_pc <= 0)
        return -1;

    /* Try cached map: O(1) lookup + context check */
    if (s_replay_cache_pc == g_replay_pc &&
        src_line >= 0 && src_line < g_num_cmds) {
        int cached = s_replay_flat_map[src_line];
        if (cached >= 0 &&
            (current_flat_idx < 0 ||
             cached == current_flat_idx ||
             replay_flat_cmd_context_matches(cached, current_flat_idx)))
            return cached;
    }

    /* Fallback: full scan (only for context mismatch in function bodies) */
    for (int j = g_replay_pc - 1; j >= 0; j--) {
        if (g_flat_cmds[j].src_cmd_idx != src_line || !g_flat_cmds[j].valid)
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

    if (flat_idx >= 0 && flat_idx < g_num_flat_cmds) {
        const FlatCmdLocalVars *lcvars = &g_flat_cmd_local_vars[flat_idx];
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
    int pc = 0;
    int goto_count = 0;

    if (!out_vals || max_vals < g_num_predef_vars)
        return 0;

    if (g_replay_active)
        repl_copy_replay_baseline_predef_values(out_vals, max_vals);
    else
        for (int i = 0; i < g_num_predef_vars && i < max_vals; i++)
            out_vals[i] = g_predef_vars[i].value;

    if (target_pc < 0)
        target_pc = 0;
    if (target_pc > g_num_flat_cmds)
        target_pc = g_num_flat_cmds;

    while (pc < target_pc) {
        if (!g_flat_cmds[pc].valid) {
            pc++;
            continue;
        }

        switch (g_flat_cmds[pc].type) {
        case CMD_VAR_ASSIGN: {
            int vi = g_flat_cmds[pc].num_args;
            float value = g_flat_cmds[pc].args[0];
            if (g_flat_cmds[pc].has_vars) {
                char rhs[MAX_LINE_LEN];
                if (repl_extract_assignment_parts(g_flat_cmds[pc].source,
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
            float cond = g_flat_cmds[pc].args[0];
            if (g_flat_cmds[pc].has_vars) {
                char cond_text[MAX_LINE_LEN];
                if (repl_extract_paren_payload(g_flat_cmds[pc].source,
                                               cond_text, sizeof(cond_text)) &&
                    cond_text[0]) {
                    replay_eval_expr_with_predefs(pc, cond_text, out_vals, &cond);
                }
            }
            if (cond == 0.0f) {
                int depth = 1;
                while (depth > 0 && ++pc < target_pc) {
                    if (g_flat_cmds[pc].type == CMD_IF_BEGIN) depth++;
                    else if (g_flat_cmds[pc].type == CMD_IF_END) depth--;
                }
            }
            break;
        }
        case CMD_GOTO: {
            char label[64];
            if (!repl_extract_goto_label(g_flat_cmds[pc].source,
                                         label, sizeof(label)))
                break;
            if (goto_count++ > 100000)
                return 0;
            for (int li = 0; li < g_num_flat_cmds; li++) {
                char target_label[64];
                if (g_flat_cmds[li].valid &&
                    g_flat_cmds[li].type == CMD_LABEL &&
                    repl_extract_label_name(g_flat_cmds[li].source,
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
    float vals[MAX_PREDEF_VARS];
    int pc = 0, goto_count = 0;
    int target_pc = g_replay_pc;

    memset(s_replay_predef_snap_valid, 0, sizeof(int) * (size_t)g_num_cmds);

    if (g_replay_active)
        repl_copy_replay_baseline_predef_values(vals, MAX_PREDEF_VARS);
    else
        for (int i = 0; i < g_num_predef_vars && i < MAX_PREDEF_VARS; i++)
            vals[i] = g_predef_vars[i].value;

    if (target_pc < 0) target_pc = 0;
    if (target_pc > g_num_flat_cmds) target_pc = g_num_flat_cmds;

    /* Macro: snapshot predef vals for a flat position's source command */
    #define SNAP_IF_MAPPED(flat_pc) do {                                    \
        int _src = g_flat_cmds[flat_pc].src_cmd_idx;                        \
        if (_src >= 0 && _src < g_num_cmds &&                               \
            s_replay_flat_map[_src] == (flat_pc) &&                         \
            !s_replay_predef_snap_valid[_src]) {                            \
            memcpy(s_replay_predef_snap[_src], vals,                        \
                   sizeof(float) * (size_t)g_num_predef_vars);              \
            s_replay_predef_snap_valid[_src] = 1;                           \
        }                                                                   \
    } while (0)

    while (pc < target_pc) {
        SNAP_IF_MAPPED(pc);

        if (!g_flat_cmds[pc].valid) { pc++; continue; }

        switch (g_flat_cmds[pc].type) {
        case CMD_VAR_ASSIGN: {
            int vi = g_flat_cmds[pc].num_args;
            float value = g_flat_cmds[pc].args[0];
            if (g_flat_cmds[pc].has_vars) {
                char rhs[MAX_LINE_LEN];
                if (repl_extract_assignment_parts(g_flat_cmds[pc].source,
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
            float cond = g_flat_cmds[pc].args[0];
            if (g_flat_cmds[pc].has_vars) {
                char cond_text[MAX_LINE_LEN];
                if (repl_extract_paren_payload(g_flat_cmds[pc].source,
                                               cond_text, sizeof(cond_text)) &&
                    cond_text[0])
                    replay_eval_expr_with_predefs(pc, cond_text, vals, &cond);
            }
            if (cond == 0.0f) {
                int depth = 1;
                while (depth > 0 && ++pc < target_pc) {
                    SNAP_IF_MAPPED(pc);
                    if (g_flat_cmds[pc].type == CMD_IF_BEGIN) depth++;
                    else if (g_flat_cmds[pc].type == CMD_IF_END) depth--;
                }
            }
            break;
        }
        case CMD_GOTO: {
            char label[64];
            if (!repl_extract_goto_label(g_flat_cmds[pc].source,
                                         label, sizeof(label)))
                break;
            if (goto_count++ > 100000)
                goto snap_done;
            for (int li = 0; li < g_num_flat_cmds; li++) {
                char target_label[64];
                if (g_flat_cmds[li].valid &&
                    g_flat_cmds[li].type == CMD_LABEL &&
                    repl_extract_label_name(g_flat_cmds[li].source,
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
    if (s_replay_cache_pc == g_replay_pc &&
        cmd_idx >= 0 && cmd_idx < g_num_cmds &&
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
    if (!repl_extract_assignment_parts(g_cmds[cmd_idx].source,
                                       name, sizeof(name),
                                       rhs, sizeof(rhs)) ||
        !name[0] || !rhs[0])
        return 0;

    {
        float value = g_flat_cmds[flat_idx].args[0];
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
    int flat_idx;
    char comment[MAX_INPUT_LEN];

    if (!out || out_size <= 0)
        return 0;
    out[0] = '\0';

    if (cmd_idx < 0 || cmd_idx >= g_num_cmds)
        return 0;

    snprintf(out, out_size, "%s", g_cmds[cmd_idx].source);

    if (!g_replay_active ||
        !g_replay_expand_args ||
        !g_cmds[cmd_idx].has_vars)
        return 1;

    if (g_cmds[cmd_idx].type != CMD_VAR_ASSIGN)
        return 1;

    flat_idx = find_replay_assignment_flat_cmd(cmd_idx);
    if (flat_idx < 0)
        return 1;

    if (build_replay_assignment_inline_comment(cmd_idx, flat_idx,
                                               comment, sizeof(comment)) &&
        comment[0]) {
        snprintf(out, out_size, "%s%s", g_cmds[cmd_idx].source, comment);
    }

    return 1;
}

static int build_replay_subst_annotation(int cmd_idx, int flat_idx,
                                         char *subst, int subst_size,
                                         char *var_comment, int comment_size) {
    float predef_vals[MAX_PREDEF_VARS];
    ExprVar visible_vars[MAX_PREDEF_VARS + MAX_EXPR_VARS];
    int nv;

    if (!subst || subst_size <= 0)
        return 0;
    subst[0] = '\0';
    if (var_comment && comment_size > 0)
        var_comment[0] = '\0';

    if (s_replay_cache_pc == g_replay_pc &&
        cmd_idx >= 0 && cmd_idx < g_num_cmds &&
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
    return subst_visible_vars(g_cmds[cmd_idx].source, subst, subst_size,
                              var_comment, comment_size,
                              visible_vars, nv);
}

static int build_replay_eval_annotation(int cmd_idx, int flat_idx,
                                        char *eval_buf, int eval_size) {
    float predef_vals[MAX_PREDEF_VARS];
    ExprVar visible_vars[MAX_PREDEF_VARS + MAX_EXPR_VARS];
    GLCmd eval_cmd;
    int nv;

    if (!eval_buf || eval_size <= 0)
        return 0;
    eval_buf[0] = '\0';

    if (s_replay_cache_pc == g_replay_pc &&
        cmd_idx >= 0 && cmd_idx < g_num_cmds &&
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
    if (!repl_parse_command_with_vars(g_cmds[cmd_idx].source,
                                      &eval_cmd, visible_vars, nv))
        return 0;

    return format_evaluated_cmd(&eval_cmd, g_cmds[cmd_idx].source,
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

/* ========================================================================= */
/* Code panel                                                                 */
/* ========================================================================= */

typedef struct {
    const char *text;
    int         len;
    int         panel_w;
    int         pos;
    int         x;
    int         cont_x;
    int         done;
} CodeWrapIter;

#define CODE_PANEL_MAX_HANG_INDENT_CHARS 12

static int code_panel_available_chars(int panel_w, int x) {
    int avail_px = panel_w - x - 4;
    if (avail_px < FONT_W)
        return 0;
    return avail_px / FONT_W;
}

static int code_panel_cont_indent_chars(const char *text) {
    const char *src = text ? text : "";
    int leading = 0;

    while (src[leading] && isspace((unsigned char)src[leading]))
        leading++;

    const char *paren = strchr(src, '(');
    if (paren && paren[1] != '\0') {
        int align = (int)(paren - src) + 1;
        int max_align = leading + CODE_PANEL_MAX_HANG_INDENT_CHARS;
        if (align > max_align)
            align = max_align;
        return align;
    }

    return leading + 4;
}

/* Secondary break characters for long lines without commas. Preference order:
 * comma > closing paren > space > operator. Commas are tried first in a
 * separate pass so they always win when present. */
static int is_secondary_break(char c) {
    return c == ')' || c == ' ' || c == '+' || c == '*' || c == '-' || c == '/';
}

static int code_panel_find_wrap_break(const char *text, int start,
                                      int max_chars, int len) {
    int end = start + max_chars - 1;
    int search_start = start;
    if (end >= len)
        end = len - 1;

    while (search_start < len &&
           isspace((unsigned char)text[search_start]))
        search_start++;

    /* Prefer a comma within the window. */
    for (int i = end; i > search_start; i--) {
        if (text[i] == ',')
            return i;
    }

    /* Fall back to secondary break chars within the window. */
    for (int i = end; i > search_start; i--) {
        if (is_secondary_break(text[i]))
            return i;
    }

    /* Last resort: extend past the window to the next comma or break. */
    for (int i = end + 1; i < len; i++) {
        if (text[i] == ',' ||
            (i > search_start && is_secondary_break(text[i])))
            return i;
    }

    return -1;
}

static void code_wrap_iter_init(CodeWrapIter *it, const char *text,
                                int first_x, int panel_w) {
    it->text = text ? text : "";
    it->len = (int)strlen(it->text);
    it->panel_w = panel_w;
    it->pos = 0;
    it->x = first_x;
    it->cont_x = first_x + code_panel_cont_indent_chars(it->text) * FONT_W;
    it->done = 0;
}

static int code_wrap_iter_next(CodeWrapIter *it, int *out_start,
                               int *out_len, int *out_x) {
    if (it->done)
        return 0;

    *out_start = it->pos;
    *out_x = it->x;

    if (it->len == 0) {
        *out_len = 0;
        it->done = 1;
        return 1;
    }

    {
        int width_chars = code_panel_available_chars(it->panel_w, it->x);
        int remaining = it->len - it->pos;

        if (!g_wrap_at_comma || width_chars < 1 || remaining <= width_chars) {
            *out_len = remaining;
            it->done = 1;
            return 1;
        }

        {
            int break_idx = code_panel_find_wrap_break(it->text, it->pos,
                                                       width_chars, it->len);
            if (break_idx < 0) {
                *out_len = remaining;
                it->done = 1;
                return 1;
            }

            *out_len = break_idx - it->pos + 1;
            it->pos = break_idx + 1;
            it->x = it->cont_x;
            return 1;
        }
    }
}

static int code_panel_row_count_for_text(const char *text, int first_x,
                                         int panel_w) {
    CodeWrapIter it;
    int rows = 0;
    int start, len, x;

    code_wrap_iter_init(&it, text, first_x, panel_w);
    while (code_wrap_iter_next(&it, &start, &len, &x))
        rows++;

    return rows > 0 ? rows : 1;
}

static int code_panel_segment_for_row(const char *text, int first_x, int panel_w,
                                      int want_row, int *out_start,
                                      int *out_len, int *out_x) {
    CodeWrapIter it;
    int row = 0;
    int start, len, x;

    code_wrap_iter_init(&it, text, first_x, panel_w);
    while (code_wrap_iter_next(&it, &start, &len, &x)) {
        if (row == want_row) {
            if (out_start) *out_start = start;
            if (out_len) *out_len = len;
            if (out_x) *out_x = x;
            return 1;
        }
        row++;
    }

    return 0;
}

static int code_panel_cursor_row_for_text(const char *text, int first_x,
                                          int panel_w, int cursor_pos,
                                          int *out_seg_start,
                                          int *out_seg_len,
                                          int *out_seg_x) {
    CodeWrapIter it;
    int row = 0;
    int start, len, x;
    int text_len = (int)strlen(text ? text : "");

    if (cursor_pos < 0)
        cursor_pos = 0;
    if (cursor_pos > text_len)
        cursor_pos = text_len;

    code_wrap_iter_init(&it, text, first_x, panel_w);
    while (code_wrap_iter_next(&it, &start, &len, &x)) {
        int next_start = start + len;
        if (next_start >= text_len || cursor_pos < next_start) {
            if (out_seg_start) *out_seg_start = start;
            if (out_seg_len) *out_seg_len = len;
            if (out_seg_x) *out_seg_x = x;
            return row;
        }
        row++;
    }

    if (out_seg_start) *out_seg_start = text_len;
    if (out_seg_len) *out_seg_len = 0;
    if (out_seg_x) *out_seg_x = first_x;
    return 0;
}

static void code_panel_draw_segment(int x, int y, const char *text,
                                    int start, int len, void *font) {
    char buf[MAX_INPUT_LEN];

    if (len <= 0)
        return;
    if (len >= (int)sizeof(buf))
        len = (int)sizeof(buf) - 1;

    memcpy(buf, text + start, (size_t)len);
    buf[len] = '\0';
    draw_string((float)x, (float)y, buf, font);
}

static int code_panel_header_row_count(int panel_w, int text_x) {
    int rows = 0;

    for (int i = 0; i < g_workspace_header_line_count; i++)
        rows += code_panel_row_count_for_text(g_workspace_header_lines[i], text_x, panel_w);
    for (int i = 0; g_header_pre[i]; i++)
        rows += code_panel_row_count_for_text(g_header_pre[i], text_x, panel_w);
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++)
        rows += code_panel_row_count_for_text(g_render_state_lines[i], text_x, panel_w);
    for (int i = 0; i < CAM_LINE_COUNT; i++)
        rows += code_panel_row_count_for_text(g_cam_lines[i], text_x, panel_w);
    for (int i = 0; g_header_post[i]; i++)
        rows += code_panel_row_count_for_text(g_header_post[i], text_x, panel_w);

    return rows;
}

/* Menu bar — styled after Header Wireframes v2.
 * Left: top-level menus (File, Scene, Config).
 * Right: pinned buttons (Search, Replay) — retained in flat form until the
 * right-side redesign lands. */

enum {
    MENU_FILE = REPL_MENU_FILE,
    MENU_SCENE = REPL_MENU_SCENE,
    MENU_CONFIG = REPL_MENU_CONFIG,
    NUM_MENUS = REPL_MENU_COUNT
};

static const char *g_menu_labels[NUM_MENUS] = {
    "File", "Scene", "Config"
};

/* Right-to-left the pins render from highest index first, so PIN_REPLAY sits
 * at the far right matching the design. PIN_SEARCH fills the gap between the
 * last menu on the left and PIN_REPLAY. */
enum { PIN_SEARCH = 0, PIN_REPLAY, NUM_PIN_BTNS };
static const char *g_pin_btn_labels[NUM_PIN_BTNS] = {
    "search...", "Replay"
};

#define PIN_SEARCH_MIN_W 140

static int g_open_menu = -1;      /* index into g_menu_labels; -1 = none */

/* Inline scene rename state.  Keep near g_open_menu so rendering and key
 * handlers can reach it without extra plumbing. Menu actions enter rename
 * through ui_panels_begin_rename(). */
static int  g_rename_slot = -1;
static char g_rename_buf[USER_SCENE_NAME_MAX];
static int  g_rename_len  = 0;

static void rename_refresh_status(void) {
    char msg[160];
    snprintf(msg, sizeof(msg),
             "Rename scene (Enter to save, Esc to cancel): %s", g_rename_buf);
    set_status(msg);
}
static int g_menu_item_hover = -1;
static float g_menu_open_time = -1.0f;   /* g_anim_time when current menu opened */
static float g_search_open_time = -1.0f; /* g_anim_time when search opened */
#define UI_FADE_DURATION 0.18f

int menu_dropdown_is_open(void) {
    return g_open_menu >= 0 &&
           code_panel_layout_mode() != CODE_PANEL_LAYOUT_HIDDEN;
}
int example_dropdown_is_open(void) { return menu_dropdown_is_open(); }


enum {
    FILE_ITEM_EXPORT = REPL_FILE_ITEM_EXPORT,
    FILE_ITEM_IMPORT = REPL_FILE_ITEM_IMPORT,
    FILE_ITEM_SAVE_WORKSPACE = REPL_FILE_ITEM_SAVE_WORKSPACE,
    FILE_ITEM_LOAD_WORKSPACE = REPL_FILE_ITEM_LOAD_WORKSPACE,
    FILE_ITEM_COUNT = REPL_FILE_ITEM_COUNT
};

/* SCENE menu layout:
 *   [0]                      "### EXAMPLES"
 *   [1..e]                   example names  (e = repl_example_count())
 *   [e + SCENE_OFF_DIVIDER]  "---"
 *   [e + SCENE_OFF_HDR]      "### SCENE"
 *   [e + SCENE_OFF_NEW]      "New empty scene"
 *   [e + SCENE_OFF_SAVE]     "Save to output.c"
 *   [e + SCENE_OFF_RENAME]   "Rename active scene"
 *   [e + SCENE_OFF_SCENES ..
 *      e + SCENE_OFF_SCENES + n - 1]  user scene names
 *                                     (n = repl_user_scene_count())
 */
enum {
    SCENE_OFF_DIVIDER = REPL_SCENE_OFF_DIVIDER,
    SCENE_OFF_HDR     = REPL_SCENE_OFF_HDR,
    SCENE_OFF_NEW     = REPL_SCENE_OFF_NEW,
    SCENE_OFF_SAVE    = REPL_SCENE_OFF_SAVE,
    SCENE_OFF_RENAME  = REPL_SCENE_OFF_RENAME,
    SCENE_OFF_SCENES  = REPL_SCENE_OFF_SCENES,
    SCENE_FIXED_COUNT = REPL_SCENE_FIXED_COUNT
};

static int menu_item_count(int menu_id) {
    switch (menu_id) {
    case MENU_FILE:   return FILE_ITEM_COUNT;
    case MENU_SCENE:  return 1 + repl_example_count() + SCENE_FIXED_COUNT
                             + repl_user_scene_count();
    case MENU_CONFIG: return CFG_ITEM_COUNT;
    }
    return 0;
}

static const char *menu_item_label(int menu_id, int i) {
    if (menu_id == MENU_FILE) {
        if (i == FILE_ITEM_EXPORT) return "Export";
        if (i == FILE_ITEM_IMPORT) return "Import";
        if (i == FILE_ITEM_SAVE_WORKSPACE) return "Save Workspace";
        if (i == FILE_ITEM_LOAD_WORKSPACE) return "Load Workspace";
        return NULL;
    }
    if (menu_id == MENU_SCENE) {
        int e = repl_example_count();
        if (i == 0)                                            return "### EXAMPLES";
        if (i >= 1 && i <= e)                                 return repl_example_name(i - 1);
        if (i == e + SCENE_OFF_DIVIDER)                       return "---";
        if (i == e + SCENE_OFF_HDR)                           return "### SCENE";
        if (i == e + SCENE_OFF_NEW)                           return "New empty scene";
        if (i == e + SCENE_OFF_SAVE)                          return "Save to output.c";
        if (i == e + SCENE_OFF_RENAME)                        return "Rename active scene";
        int scene_n = i - (e + SCENE_OFF_SCENES);
        if (scene_n >= 0 && scene_n < repl_user_scene_count()) {
            int slot = repl_scene_menu_slot_for_dense_index(scene_n);
            return (slot >= 0) ? repl_user_scene_name(slot) : NULL;
        }
        return NULL;
    }
    if (menu_id == MENU_CONFIG) {
        if (i >= 0 && i < CFG_ITEM_COUNT) return g_cfg_items[i].label;
        return NULL;
    }
    return NULL;
}

static const char *menu_item_shortcut(int menu_id, int i) {
    if (menu_id == MENU_FILE && i == FILE_ITEM_EXPORT) return "Ctrl+S";
    if (menu_id == MENU_SCENE) {
        int e = repl_example_count();
        if (i == e + SCENE_OFF_SAVE) return "Ctrl+S";
        return NULL;
    }
    if (menu_id == MENU_CONFIG && i >= 0 && i < CFG_ITEM_COUNT && g_cfg_items[i].value != NULL) {
        static char buf[16];
        if (g_cfg_items[i].key_code == 0) return NULL;
        if (g_cfg_items[i].is_special) {
            snprintf(buf, sizeof(buf), "F%d", g_cfg_items[i].key_code - GLUT_KEY_F1 + 1);
            return buf;
        } else {
            if (g_cfg_items[i].key_code > 0 && g_cfg_items[i].key_code <= 26) {
                snprintf(buf, sizeof(buf), "Ctrl+%c", g_cfg_items[i].key_code - 1 + 'a');
                return buf;
            } else if (g_cfg_items[i].key_code == KEY_CTRL_BACKSLASH) {
                return "Ctrl+\\";
            } else {
                snprintf(buf, sizeof(buf), "%c", g_cfg_items[i].key_code);
                return buf;
            }
        }
    }
    (void)i;
    return NULL;
}

static int menu_max_shortcut_px(int menu_id) {
    int n = menu_item_count(menu_id);
    int max_sc = 0;
    for (int i = 0; i < n; i++) {
        const char *sc = menu_item_shortcut(menu_id, i);
        if (sc) {
            int w = (int)strlen(sc) * FONT_SMALL_W;
            if (w > max_sc) max_sc = w;
        }
    }
    return max_sc;
}

#define CFG_STATE_MAX_CHARS 20

/* Fills `out` with the current state label, truncated to CFG_STATE_MAX_CHARS
 * (with a trailing ellipsis). Returns `out`. */
static const char *cfg_state_str(int i, char *out, int out_size) {
    const char *s = "";
    if (i >= 0 && i < CFG_ITEM_COUNT && g_cfg_items[i].value != NULL) {
        if (g_cfg_items[i].state_names)
            s = g_cfg_items[i].state_names[*g_cfg_items[i].value];
        else
            s = *g_cfg_items[i].value ? "ON" : "OFF";
    }
    int n = (int)strlen(s);
    int cap = out_size - 1;
    if (cap > CFG_STATE_MAX_CHARS) cap = CFG_STATE_MAX_CHARS;
    if (n <= cap) {
        memcpy(out, s, (size_t)n);
        out[n] = '\0';
    } else {
        int keep = cap - 3;
        if (keep < 0) keep = 0;
        memcpy(out, s, (size_t)keep);
        int dots = cap - keep;
        for (int k = 0; k < dots; k++) out[keep + k] = '.';
        out[cap] = '\0';
    }
    return out;
}

/* Widest possible state label across all items & all their possible values,
 * clamped to CFG_STATE_MAX_CHARS. Used to keep the menu width stable as
 * values cycle. */
static int cfg_max_state_chars(void) {
    int max_chars = 3;  /* "OFF" */
    for (int i = 0; i < CFG_ITEM_COUNT; i++) {
        if (g_cfg_items[i].value == NULL) continue;
        const char *const *names = g_cfg_items[i].state_names;
        if (!names) continue;
        for (int k = 0; k < g_cfg_items[i].n_states; k++) {
            int n = (int)strlen(names[k]);
            if (n > max_chars) max_chars = n;
        }
    }
    if (max_chars > CFG_STATE_MAX_CHARS) max_chars = CFG_STATE_MAX_CHARS;
    return max_chars;
}

static int menu_item_activate(int menu_id, int i) {
    return repl_action_menu_item_activate(menu_id, i);
}

static void menubar_rects(int menu_x[NUM_MENUS], int menu_w[NUM_MENUS],
                          int pin_x[NUM_PIN_BTNS], int pin_w[NUM_PIN_BTNS],
                          int *row_y, int *row_h) {
    int cp_x, cp_y, cp_w, cp_h;
    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    int panel_top = cp_y + cp_h;
    int by = panel_top - CODE_MARGIN_Y - LINE_H;
    int bh = LINE_H;
    if (row_y) *row_y = by;
    if (row_h) *row_h = bh;

    int x = cp_x + CODE_MARGIN_X;
    for (int i = 0; i < NUM_MENUS; i++) {
        int label_w = (int)strlen(g_menu_labels[i]) * FONT_SMALL_W;
        menu_w[i] = label_w + 18;  /* ~9px padding each side */
        menu_x[i] = x;
        x += menu_w[i];
    }

    int right_edge = cp_x + cp_w - CODE_MARGIN_X;

    /* PIN_REPLAY — width reserves room for the widest state label plus a
     * 12px state icon (triangle / pause-bars) and padding. */
    int replay_label_w = (int)strlen("Replaying") * FONT_SMALL_W;
    pin_w[PIN_REPLAY] = replay_label_w + 12 /* icon */ + 22 /* pads */;
    pin_x[PIN_REPLAY] = right_edge - pin_w[PIN_REPLAY];

    /* PIN_SEARCH — fills the gap between the last menu and PIN_REPLAY. */
    int menus_right = menu_x[NUM_MENUS - 1] + menu_w[NUM_MENUS - 1];
    int search_w = pin_x[PIN_REPLAY] - menus_right;
    if (search_w < PIN_SEARCH_MIN_W) search_w = PIN_SEARCH_MIN_W;
    pin_w[PIN_SEARCH] = search_w;
    pin_x[PIN_SEARCH] = pin_x[PIN_REPLAY] - search_w;
}

static int menubar_menu_hit(int gx, int gy) {
    int menu_x[NUM_MENUS], menu_w[NUM_MENUS];
    int pin_x[NUM_PIN_BTNS], pin_w[NUM_PIN_BTNS];
    int by, bh;
    int ry = g_win_h - gy;
    menubar_rects(menu_x, menu_w, pin_x, pin_w, &by, &bh);
    if (ry < by || ry >= by + bh) return -1;
    for (int i = 0; i < NUM_MENUS; i++)
        if (gx >= menu_x[i] && gx < menu_x[i] + menu_w[i]) return i;
    return -1;
}

static int menubar_pin_hit(int gx, int gy) {
    int menu_x[NUM_MENUS], menu_w[NUM_MENUS];
    int pin_x[NUM_PIN_BTNS], pin_w[NUM_PIN_BTNS];
    int by, bh;
    int ry = g_win_h - gy;
    menubar_rects(menu_x, menu_w, pin_x, pin_w, &by, &bh);
    if (ry < by || ry >= by + bh) return -1;
    for (int i = 0; i < NUM_PIN_BTNS; i++)
        if (gx >= pin_x[i] && gx < pin_x[i] + pin_w[i]) return i;
    return -1;
}

static int menu_dropdown_rect(int *dx, int *dy, int *dw, int *dh) {
    if (g_open_menu < 0) return 0;
    if (code_panel_layout_mode() == CODE_PANEL_LAYOUT_HIDDEN) return 0;
    int menu_x[NUM_MENUS], menu_w[NUM_MENUS];
    int pin_x[NUM_PIN_BTNS], pin_w[NUM_PIN_BTNS];
    int by, bh;
    menubar_rects(menu_x, menu_w, pin_x, pin_w, &by, &bh);
    int n = menu_item_count(g_open_menu);

    int max_lbl = 0, max_state = 0, max_sc = 0;
    for (int i = 0; i < n; i++) {
        const char *lbl = menu_item_label(g_open_menu, i);
        const char *sc  = menu_item_shortcut(g_open_menu, i);
        int lw = (int)(lbl ? strlen(lbl) : 0) * FONT_SMALL_W;
        if (lw > max_lbl) max_lbl = lw;
        if (sc) {
            int cw = (int)strlen(sc) * FONT_SMALL_W;
            if (cw > max_sc) max_sc = cw;
        }
    }
    if (g_open_menu == MENU_CONFIG)
        max_state = cfg_max_state_chars() * FONT_SMALL_W;
    int max_w = max_lbl;
    if (max_state > 0) max_w += max_state + 20;
    if (max_sc > 0)    max_w += max_sc + 16;
    if (max_w < 80) max_w = 80;
    int width  = max_w + 28;
    int rows   = (n > 0) ? n : 1;  /* reserve one row for "(empty)" */
    int height = rows * LINE_H + 8;

    if (dx) *dx = menu_x[g_open_menu];
    if (dy) *dy = by - height;
    if (dw) *dw = width;
    if (dh) *dh = height;
    return 1;
}

static int menu_dropdown_item_hit(int gx, int gy) {
    if (g_open_menu < 0) return -1;
    int n = menu_item_count(g_open_menu);
    if (n == 0) return -1;
    int dx, dy, dw, dh;
    if (!menu_dropdown_rect(&dx, &dy, &dw, &dh)) return -1;
    int ry = g_win_h - gy;
    if (gx < dx || gx >= dx + dw || ry < dy || ry >= dy + dh) return -1;
    int row = (dy + dh - 4 - ry) / LINE_H;
    if (row < 0 || row >= n) return -1;
    const char *lbl = menu_item_label(g_open_menu, row);
    if (!lbl || strncmp(lbl, "###", 3) == 0 || strcmp(lbl, "---") == 0) return -1;
    return row;
}

static int code_panel_footer_row_count(int panel_w, int text_x) {
    int rows = 0;
    char line[MAX_LINE_LEN];

    for (int i = 0; g_footer_pre_init[i]; i++)
        rows += code_panel_row_count_for_text(g_footer_pre_init[i], text_x, panel_w);
    for (int i = 0; i < init_section_line_count(); i++) {
        init_section_line(i, line, sizeof(line));
        rows += code_panel_row_count_for_text(line, text_x, panel_w);
    }
    for (int i = 0; g_footer_post_init[i]; i++)
        rows += code_panel_row_count_for_text(g_footer_post_init[i], text_x, panel_w);

    return rows;
}

static int code_panel_replay_extra_rows_for_line(int cmd_idx) {
    if (!g_replay_active)
        return 0;
    if (!g_replay_expand_args)
        return 0;
    if (cmd_idx < 0 || cmd_idx >= g_num_cmds)
        return 0;
    if (cmd_idx != g_replay_src_line)
        return 0;
    if (!g_cmds[cmd_idx].has_vars)
        return 0;
    if (g_cmds[cmd_idx].type == CMD_VAR_ASSIGN)
        return 0;
    return 2;
}

static int code_panel_leading_ws_chars(const char *text) {
    int n = 0;

    while (text && text[n] && isspace((unsigned char)text[n]))
        n++;
    return n;
}

static int code_panel_active_indent_chars(void) {
    if (g_inserting)
        return cmd_indent_chars(g_edit_line);
    if (g_edit_line >= 0 && g_edit_line < g_num_cmds)
        return code_panel_leading_ws_chars(g_cmds[g_edit_line].source);
    return cmd_indent_chars(g_num_cmds);
}

static int code_panel_command_main_rows(int cmd_idx, int panel_w, int text_x) {
    if (!g_inserting && cmd_idx == g_edit_line) {
        int indent_chars = code_panel_active_indent_chars();
        return code_panel_row_count_for_text(g_input,
                                             text_x + indent_chars * FONT_W,
                                             panel_w);
    }

    {
        char display_text[MAX_INPUT_LEN];
        if (!code_panel_get_command_display_text(cmd_idx, display_text,
                                                 sizeof(display_text)))
            return 1;
        return code_panel_row_count_for_text(display_text, text_x, panel_w);
    }
}

static void code_panel_precompute_layout_rows(int panel_w, int text_x,
                                              int *main_rows,
                                              int *replay_extra_rows) {
    for (int i = 0; i < g_num_cmds; i++) {
        if (main_rows)
            main_rows[i] = code_panel_command_main_rows(i, panel_w, text_x);
        if (replay_extra_rows)
            replay_extra_rows[i] = code_panel_replay_extra_rows_for_line(i);
    }
}

static int code_panel_insert_rows(int panel_w, int text_x);
static int code_panel_newline_rows(int panel_w, int text_x);

static int code_panel_cursor_doc_line_from_layout(int header_rows,
                                                  const int *cmd_main_rows,
                                                  const int *replay_extra_rows,
                                                  int panel_w, int text_x) {
    int cursor_doc_line = header_rows;

    if (g_inserting) {
        for (int i = 0; i < g_edit_line && i < g_num_cmds; i++) {
            cursor_doc_line += cmd_main_rows[i];
            cursor_doc_line += replay_extra_rows[i];
        }
        cursor_doc_line += code_panel_cursor_row_for_text(
            g_input, text_x + code_panel_active_indent_chars() * FONT_W,
            panel_w, g_cursor_pos, NULL, NULL, NULL);
    } else if (g_edit_line < g_num_cmds) {
        for (int i = 0; i < g_edit_line; i++) {
            cursor_doc_line += cmd_main_rows[i];
            cursor_doc_line += replay_extra_rows[i];
        }
        cursor_doc_line += code_panel_cursor_row_for_text(
            g_input, text_x + code_panel_active_indent_chars() * FONT_W,
            panel_w, g_cursor_pos, NULL, NULL, NULL);
    } else {
        for (int i = 0; i < g_num_cmds; i++) {
            cursor_doc_line += cmd_main_rows[i];
            cursor_doc_line += replay_extra_rows[i];
        }
        cursor_doc_line += code_panel_cursor_row_for_text(
            g_input, text_x + code_panel_active_indent_chars() * FONT_W,
            panel_w, g_cursor_pos, NULL, NULL, NULL);
    }

    return cursor_doc_line;
}

static int code_panel_follow_doc_line_from_layout(int cursor_doc_line,
                                                  int header_rows,
                                                  const int *cmd_main_rows,
                                                  const int *replay_extra_rows) {
    int follow_doc_line = cursor_doc_line;

    if (g_replay_active &&
        g_replay_src_line >= 0 && g_replay_src_line < g_num_cmds) {
        follow_doc_line = header_rows;
        for (int i = 0; i < g_replay_src_line; i++) {
            follow_doc_line += cmd_main_rows[i];
            follow_doc_line += replay_extra_rows[i];
        }
        if (replay_extra_rows[g_replay_src_line] > 0) {
            follow_doc_line += cmd_main_rows[g_replay_src_line];
            follow_doc_line += replay_extra_rows[g_replay_src_line] - 1;
        } else if (cmd_main_rows[g_replay_src_line] > 0) {
            follow_doc_line += cmd_main_rows[g_replay_src_line] - 1;
        }
    }

    return follow_doc_line;
}

static int code_panel_visible_lines_for_height(int cp_h) {
    int available = cp_h - CODE_MARGIN_Y - 2 * LINE_H - 3 - STATUSBAR_H;
    if (available < 0)
        return 1;
    return available / LINE_H + 1;
}

static void code_panel_apply_follow_scroll(int total_lines, int visible_lines,
                                           int follow_doc_line) {
    int max_scroll = total_lines - visible_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    if (g_scroll_follow_cursor) {
        if (follow_doc_line < g_scroll)
            g_scroll = follow_doc_line;
        if (follow_doc_line >= g_scroll + visible_lines)
            g_scroll = follow_doc_line - visible_lines + 1;
        if (g_scroll > max_scroll) g_scroll = max_scroll;
        if (g_scroll < 0) g_scroll = 0;
        g_scroll_follow_cursor = 0;
    }
}

int code_panel_apply_scroll_follow_for_test(int *out_follow_doc_line,
                                            int *out_visible_lines) {
    int cmd_main_rows[MAX_COMMANDS];
    int replay_extra_rows[MAX_COMMANDS];
    int cp_x, cp_y, cp_w, cp_h;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    int text_x = idx_x + idx_col_w;
    int visible_lines;
    int header_rows;
    int footer_rows;
    int total_lines;
    int cursor_doc_line;
    int follow_doc_line;

    refresh_workspace_header_lines();
    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    (void)cp_x;
    (void)cp_y;

    visible_lines = code_panel_visible_lines_for_height(cp_h);

    if (g_replay_active &&
        s_replay_cache_pc != g_replay_pc)
        rebuild_replay_annotation_cache();
    else if (!g_replay_active)
        invalidate_replay_annotation_cache();

    header_rows = code_panel_header_row_count(cp_w, text_x);
    footer_rows = code_panel_footer_row_count(cp_w, text_x);
    code_panel_precompute_layout_rows(cp_w, text_x,
                                      cmd_main_rows, replay_extra_rows);

    total_lines = header_rows + footer_rows + code_panel_newline_rows(cp_w, text_x);
    for (int i = 0; i < g_num_cmds; i++) {
        if (g_inserting && i == g_edit_line)
            total_lines += code_panel_insert_rows(cp_w, text_x);
        total_lines += cmd_main_rows[i];
        total_lines += replay_extra_rows[i];
    }

    cursor_doc_line = code_panel_cursor_doc_line_from_layout(
        header_rows, cmd_main_rows, replay_extra_rows, cp_w, text_x);
    follow_doc_line = code_panel_follow_doc_line_from_layout(
        cursor_doc_line, header_rows, cmd_main_rows, replay_extra_rows);

    code_panel_apply_follow_scroll(total_lines, visible_lines, follow_doc_line);

    if (out_follow_doc_line) *out_follow_doc_line = follow_doc_line;
    if (out_visible_lines) *out_visible_lines = visible_lines;
    return follow_doc_line >= g_scroll &&
           follow_doc_line < g_scroll + visible_lines;
}

static int code_panel_insert_rows(int panel_w, int text_x) {
    int indent_chars = code_panel_active_indent_chars();
    return code_panel_row_count_for_text(g_input,
                                         text_x + indent_chars * FONT_W,
                                         panel_w);
}

static int code_panel_newline_rows(int panel_w, int text_x) {
    if (g_edit_line == g_num_cmds) {
        int indent_chars = code_panel_active_indent_chars();
        return code_panel_row_count_for_text(g_input,
                                             text_x + indent_chars * FONT_W,
                                             panel_w);
    }
    return 1;
}

static int code_panel_target_for_doc_line(int doc_line, int panel_w, int text_x,
                                          int *out_target,
                                          int *out_on_insert_line,
                                          int *out_row_offset) {
    int row = doc_line - code_panel_header_row_count(panel_w, text_x);

    if (row < 0)
        return 0;

    for (int i = 0; i <= g_num_cmds; i++) {
        if (g_inserting && i == g_edit_line) {
            int insert_rows = code_panel_insert_rows(panel_w, text_x);
            if (row < insert_rows) {
                if (out_target) *out_target = -1;
                if (out_on_insert_line) *out_on_insert_line = 1;
                if (out_row_offset) *out_row_offset = row;
                return 1;
            }
            row -= insert_rows;
        }

        if (i < g_num_cmds) {
            int main_rows = code_panel_command_main_rows(i, panel_w, text_x);
            if (row < main_rows) {
                if (out_target) *out_target = i;
                if (out_on_insert_line) *out_on_insert_line = 0;
                if (out_row_offset) *out_row_offset = row;
                return 1;
            }
            row -= main_rows;

            {
                int replay_rows = code_panel_replay_extra_rows_for_line(i);
                if (row < replay_rows) {
                    if (out_target) *out_target = i;
                    if (out_on_insert_line) *out_on_insert_line = 0;
                    if (out_row_offset) *out_row_offset = 0;
                    return 1;
                }
                row -= replay_rows;
            }
        } else {
            int newline_rows = code_panel_newline_rows(panel_w, text_x);
            if (row < newline_rows) {
                if (out_target) *out_target = g_num_cmds;
                if (out_on_insert_line) *out_on_insert_line = 0;
                if (out_row_offset) *out_row_offset = row;
                return 1;
            }
            return 0;
        }
    }

    return 0;
}

static void code_panel_draw_search_highlights(const char *text, int search_row_idx,
                                              int seg_start, int seg_len,
                                              int seg_x, int y) {
    int drew = 0;

    if (!g_search_active || g_search_query_len <= 0 || search_row_idx < 0 ||
        !text || seg_len <= 0)
        return;

    for (int pos = repl_search_find_next_in_text(text, g_search_query, 0);
         pos >= 0;
         pos = repl_search_find_next_in_text(text, g_search_query, pos + 1)) {
        int match_end = pos + g_search_query_len;
        int seg_end = seg_start + seg_len;
        int draw_start = pos > seg_start ? pos : seg_start;
        int draw_end = match_end < seg_end ? match_end : seg_end;

        if (draw_start >= draw_end)
            continue;

        if (!drew) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            drew = 1;
        }

        if (search_row_idx == g_search_hit_line && pos == g_search_hit_char)
            glColor4f(0.95f, 0.65f, 0.18f, 0.55f);
        else
            glColor4f(0.25f, 0.45f, 0.85f, 0.30f);

        draw_quad((float)(seg_x + (draw_start - seg_start) * FONT_W),
                  (float)(y - 2),
                  (float)((draw_end - draw_start) * FONT_W),
                  (float)(FONT_H + 4));
    }

    if (drew)
        glDisable(GL_BLEND);
}

static void code_panel_format_search_query(char *out, int out_sz,
                                           int max_chars,
                                           int *out_cursor_col) {
    int start = 0;
    int take = 0;

    if (out_sz <= 0)
        return;

    out[0] = '\0';
    if (out_cursor_col)
        *out_cursor_col = 0;

    if (max_chars <= 0 || g_search_query_len <= 0)
        return;

    if (g_search_query_len > max_chars) {
        start = g_search_cursor_pos - max_chars + 1;
        if (start < 0)
            start = 0;
        if (start > g_search_query_len - max_chars)
            start = g_search_query_len - max_chars;
    }

    take = g_search_query_len - start;
    if (take > max_chars)
        take = max_chars;
    if (take >= out_sz)
        take = out_sz - 1;
    if (take < 0)
        take = 0;

    if (take > 0)
        memcpy(out, g_search_query + start, (size_t)take);
    out[take] = '\0';

    if (out_cursor_col) {
        int col = g_search_cursor_pos - start;
        if (col < 0)
            col = 0;
        if (col > take)
            col = take;
        *out_cursor_col = col;
    }
}

static float ui_fade_alpha(float open_time) {
    if (open_time < 0.0f) return 1.0f;
    float dt = g_anim_time - open_time;
    if (dt >= UI_FADE_DURATION) return 1.0f;
    if (dt <= 0.0f) return 0.0f;
    return dt / UI_FADE_DURATION;
}

/* Draws a simple magnifying-glass icon (circle + handle) at (cx, cy) with
 * given radius, using the current GL color. */
static void draw_search_icon(float cx, float cy, float r) {
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < 20; i++) {
        float a = (float)i * (6.2831853f / 20.0f);
        glVertex2f(cx + cosf(a) * r, cy + sinf(a) * r);
    }
    glEnd();
    float hx0 = cx + cosf(-0.7853982f) * r;
    float hy0 = cy + sinf(-0.7853982f) * r;
    glBegin(GL_LINES);
    glVertex2f(hx0, hy0);
    glVertex2f(hx0 + r * 0.9f, hy0 - r * 0.9f);
    glEnd();
}

static void render_code_panel_search_overlay(int cp_x, int panel_w, int panel_top) {
    char count_buf[32];
    char query_buf[128];
    int cursor_col = 0;
    (void)cp_x; (void)panel_w; (void)panel_top;

    static int prev_active = 0;
    if (g_search_active && !prev_active) g_search_open_time = g_anim_time;
    prev_active = g_search_active;

    if (!g_search_active)
        return;

    /* Anchor on the PIN_SEARCH slot so the search bar sits where the
     * placeholder was — matches the design's inline search affordance. */
    int menu_x[NUM_MENUS], menu_w[NUM_MENUS];
    int pin_x[NUM_PIN_BTNS], pin_w[NUM_PIN_BTNS];
    int by, bh;
    menubar_rects(menu_x, menu_w, pin_x, pin_w, &by, &bh);

    int box_x = pin_x[PIN_SEARCH];
    int box_y = by;
    int box_w = pin_w[PIN_SEARCH];
    int box_h = bh;

    if (g_search_query_len <= 0)
        snprintf(count_buf, sizeof(count_buf), "type to search");
    else if (g_search_match_count <= 0)
        snprintf(count_buf, sizeof(count_buf), "0");
    else
        snprintf(count_buf, sizeof(count_buf), "%d/%d",
                 g_search_hit_ordinal, g_search_match_count);

    int pad_x = 8;
    int icon_r = 5;
    int icon_cx = box_x + pad_x + icon_r;
    int icon_cy = box_y + box_h / 2;
    int text_y  = box_y + (box_h - FONT_SMALL_H) / 2 + 1;
    int count_w = (int)strlen(count_buf) * FONT_SMALL_W;
    int count_x = box_x + box_w - pad_x - count_w;
    int query_x = icon_cx + icon_r + 8;
    int max_query_chars = (count_x - query_x - pad_x) / FONT_SMALL_W;
    if (max_query_chars < 1) max_query_chars = 1;
    code_panel_format_search_query(query_buf, sizeof(query_buf),
                                   max_query_chars, &cursor_col);

    float alpha = ui_fade_alpha(g_search_open_time);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Background: a shade lighter than the menu bar (#262626) so the active
     * search input reads as "focused". */
    glColor4f(0.149f, 0.149f, 0.149f, alpha);
    draw_quad((float)box_x, (float)box_y, (float)box_w, (float)box_h);

    /* Inner border */
    glColor4f(0.298f, 0.329f, 0.392f, alpha); /* #4c5464 */
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)box_x + 0.5f,              (float)box_y + 0.5f);
    glVertex2f((float)(box_x + box_w) - 0.5f,    (float)box_y + 0.5f);
    glVertex2f((float)(box_x + box_w) - 0.5f,    (float)(box_y + box_h) - 0.5f);
    glVertex2f((float)box_x + 0.5f,              (float)(box_y + box_h) - 0.5f);
    glEnd();

    /* Magnifying-glass icon */
    glColor4f(0.667f, 0.706f, 0.784f, alpha); /* #aab3c8 */
    draw_search_icon((float)icon_cx, (float)icon_cy, (float)icon_r);

    /* Query text (or placeholder style when empty) */
    if (g_search_query_len <= 0)
        glColor4f(0.478f, 0.478f, 0.478f, alpha); /* #7a7a7a placeholder */
    else
        glColor4f(0.941f, 0.941f, 0.902f, alpha);
    draw_string((float)query_x, (float)text_y, query_buf, FONT_SMALL);

    /* Count/status on the right */
    if (g_search_query_len > 0 && g_search_match_count <= 0)
        glColor4f(0.851f, 0.424f, 0.310f, alpha); /* accent for "0" */
    else
        glColor4f(0.533f, 0.533f, 0.533f, alpha);
    draw_string((float)count_x, (float)text_y, count_buf, FONT_SMALL);

    if (g_cursor_on && g_search_query_len > 0) {
        int cursor_x = query_x + cursor_col * FONT_SMALL_W;
        glColor4f(0.95f, 0.80f, 0.24f, 0.85f * alpha);
        draw_quad((float)cursor_x, (float)(text_y - 2), 2.0f,
                  (float)(FONT_SMALL_H + 2));
    }

    glDisable(GL_BLEND);
}

static void render_active_input_rows(int panel_w, int text_x, int idx_x,
                                     int visible_lines, int file_line,
                                     int indent_chars, const char *idx_text,
                                     int search_row_idx,
                                     int *io_cur, int *io_line_y) {
    CodeWrapIter wrap_it;
    int wrap_row = 0;
    int wrap_start, wrap_len, wrap_x;
    int input_x = text_x + indent_chars * FONT_W;
    int cursor_seg_start = 0;
    int cursor_seg_len = 0;
    int cursor_seg_x = input_x;
    int cursor_row = code_panel_cursor_row_for_text(g_input, input_x, panel_w,
                                                    g_cursor_pos,
                                                    &cursor_seg_start,
                                                    &cursor_seg_len,
                                                    &cursor_seg_x);
    int cursor_col = g_cursor_pos - cursor_seg_start;

    code_wrap_iter_init(&wrap_it, g_input, input_x, panel_w);
    while (code_wrap_iter_next(&wrap_it, &wrap_start, &wrap_len, &wrap_x)) {
        if (*io_cur >= g_scroll && *io_cur < g_scroll + visible_lines) {
            glColor3f(0.55f, 0.55f, 0.30f);
            if (wrap_row == 0) {
                char ln[16];
                snprintf(ln, sizeof(ln), "%3d", file_line);
                draw_string((float)CODE_MARGIN_X, (float)(*io_line_y), ln, FONT_MONO);
                if (idx_text) {
                    glColor3f(0.45f, 0.50f, 0.65f);
                    draw_string((float)idx_x, (float)(*io_line_y), idx_text, FONT_MONO);
                }
            }

            glEnable(GL_BLEND);
            glColor4f(0.15f, 0.18f, 0.28f, 0.70f);
            draw_quad(0, (float)(*io_line_y - 3), (float)panel_w, (float)LINE_H);
            glDisable(GL_BLEND);

            if (wrap_row == 0 && indent_chars > 0) {
                char spaces[32];
                int draw_indent = indent_chars;
                if (draw_indent > (int)sizeof(spaces) - 1)
                    draw_indent = (int)sizeof(spaces) - 1;
                memset(spaces, ' ', (size_t)draw_indent);
                spaces[draw_indent] = '\0';
                glColor3f(0.30f, 0.30f, 0.38f);
                draw_string((float)text_x, (float)(*io_line_y), spaces, FONT_MONO);
            }

            glColor3f(0.95f, 0.95f, 0.90f);
            code_panel_draw_search_highlights(g_input, search_row_idx,
                                              wrap_start, wrap_len,
                                              wrap_x, *io_line_y);
            code_panel_draw_segment(wrap_x, *io_line_y, g_input,
                                    wrap_start, wrap_len, FONT_MONO);

            if (wrap_row == cursor_row) {
                int cursor_x = wrap_x + cursor_col * FONT_W;
                int hint_x = cursor_x;

                if (g_ac_ghost[0] && g_cursor_pos == g_input_len) {
                    glEnable(GL_BLEND);
                    glColor4f(0.50f, 0.55f, 0.65f, 0.55f);
                    draw_string((float)cursor_x, (float)(*io_line_y),
                                g_ac_ghost, FONT_MONO);
                    glDisable(GL_BLEND);
                    hint_x += (int)strlen(g_ac_ghost) * FONT_W;
                }

                if (g_ac_hint[0] && g_cursor_pos == g_input_len) {
                    glEnable(GL_BLEND);
                    glColor4f(0.56f, 0.62f, 0.72f, 0.38f);
                    draw_string((float)hint_x, (float)(*io_line_y),
                                g_ac_hint, FONT_MONO);
                    glDisable(GL_BLEND);
                }

                if (g_cursor_on && !g_search_active) {
                    glEnable(GL_BLEND);
                    glColor4f(0.90f, 0.80f, 0.25f, 0.85f);
                    draw_quad((float)cursor_x, (float)(*io_line_y - 2),
                              2.0f, (float)(FONT_H + 2));
                    glDisable(GL_BLEND);
                }

                g_cursor_px = cursor_x;
                g_cursor_py = *io_line_y;
            }

            *io_line_y -= LINE_H;
        }

        (*io_cur)++;
        wrap_row++;
    }
}

/* ========================================================================= */
/* Color picker                                                               */
/* ========================================================================= */

#define CP_SV_SZ    150   /* SV square side */
#define CP_HUE_W     18   /* hue bar width  */
#define CP_ALPHA_W   18   /* alpha bar width (COLOR4F only) */
#define CP_GAP        6   /* gap between elements */
#define CP_PREV_H    16   /* preview strip height */
#define SWATCH_W     12   /* inline swatch width in code panel */
/* CP_CLEAR_MAX_V is defined in sample.h */

/* g_cp_line >= 0: picker is open for that cmd index */
static int   g_cp_line     = -1;
static float g_cp_hue      = 0.0f, g_cp_sat = 1.0f, g_cp_val = 1.0f;
static float g_cp_alpha    = 1.0f;
static int   g_cp_px       = 0, g_cp_py = 0;  /* top-left (OpenGL y-up) */
static int   g_cp_drag     = 0;    /* 0=none 1=SV 2=hue 3=alpha */
static int   g_cp_has_alpha= 0;    /* 1 when editing an RGBA color command */

/* Hit-rects in y-up OpenGL coords, updated each frame in render_color_picker */
static int g_cp_sv_x, g_cp_sv_y, g_cp_sv_sz;
static int g_cp_hue_x, g_cp_hue_y, g_cp_hue_h;
static int g_cp_alp_x, g_cp_alp_y, g_cp_alp_h;

static void cp_hsv_to_rgb(float h, float s, float v,
                           float *r, float *g, float *b) {
    int i = (int)(h*6.0f);
    float f=h*6.0f-i, p=v*(1-s), q=v*(1-f*s), t=v*(1-(1-f)*s);
    switch (i%6) {
    case 0:*r=v;*g=t;*b=p;break; case 1:*r=q;*g=v;*b=p;break;
    case 2:*r=p;*g=v;*b=t;break; case 3:*r=p;*g=q;*b=v;break;
    case 4:*r=t;*g=p;*b=v;break; default:*r=v;*g=p;*b=q;break;
    }
}
static void cp_rgb_to_hsv(float r, float g, float b,
                           float *h, float *s, float *v) {
    float mx=r>g?(r>b?r:b):(g>b?g:b), mn=r<g?(r<b?r:b):(g<b?g:b), d=mx-mn;
    *v=mx; *s=(mx!=0.0f)?d/mx:0.0f;
    if (d==0.0f){*h=0.0f;return;}
    if      (mx==r)*h=fmodf((g-b)/d,6.0f)/6.0f;
    else if (mx==g)*h=((b-r)/d+2.0f)/6.0f;
    else           *h=((r-g)/d+4.0f)/6.0f;
    if(*h<0.0f)*h+=1.0f;
}
static void cp_ring(float cx, float cy, float r, int n) {
    glBegin(GL_LINE_LOOP);
    for (int i=0;i<n;i++){float a=(float)i/(float)n*6.28318f;
        glVertex2f(cx+cosf(a)*r, cy+sinf(a)*r);}
    glEnd();
}

static void color_picker_write_cmd(void) {
    if (g_cp_line<0 || g_cp_line>=g_num_cmds) return;
    float r,g,b;
    CmdType cmd_type = g_cmds[g_cp_line].type;
    const char *old_src = g_cmds[g_cp_line].source;
    int indent_len = 0;
    char indent_prefix[sizeof(g_cmds[g_cp_line].source)];
    char new_source[sizeof(g_cmds[g_cp_line].source)];
    int formatted = 0;
    cp_hsv_to_rgb(g_cp_hue, g_cp_sat, g_cp_val, &r, &g, &b);
    while (old_src[indent_len] == ' ' || old_src[indent_len] == '\t')
        indent_len++;
    if ((size_t)indent_len >= sizeof(indent_prefix))
        indent_len = (int)sizeof(indent_prefix) - 1;
    for (int i = 0; i < indent_len; i++)
        indent_prefix[i] = old_src[i];
    indent_prefix[indent_len] = '\0';
    if (g_cp_has_alpha) {
        if (cmd_type == CMD_TESS_COLOR) {
            formatted = repl_format_fits(new_source, sizeof(new_source),
                                         "%sgluColor(%g, %g, %g, %g);",
                                         indent_prefix, r, g, b, g_cp_alpha);
        } else if (cmd_type == CMD_CLEAR_COLOR) {
            float cr=r<CP_CLEAR_MAX_V?r:CP_CLEAR_MAX_V;
            float cg=g<CP_CLEAR_MAX_V?g:CP_CLEAR_MAX_V;
            float cb=b<CP_CLEAR_MAX_V?b:CP_CLEAR_MAX_V;
            formatted = repl_format_fits(new_source, sizeof(new_source),
                                         "%sglClearColor(%g, %g, %g, %g);",
                                         indent_prefix, cr, cg, cb, g_cp_alpha);
            if (!formatted) {
                set_status("Command too long");
                return;
            }
            g_cmds[g_cp_line].args[0]=cr;
            g_cmds[g_cp_line].args[1]=cg;
            g_cmds[g_cp_line].args[2]=cb;
            g_cmds[g_cp_line].args[3]=g_cp_alpha;
            g_cmds[g_cp_line].num_args = 4;
            memcpy(g_cmds[g_cp_line].source, new_source,
                   strlen(new_source) + 1);
            g_flat_dirty = 1;
            return;
        } else {
            formatted = repl_format_fits(new_source, sizeof(new_source),
                                         "%sglColor4f(%g, %g, %g, %g);",
                                         indent_prefix, r, g, b, g_cp_alpha);
        }
    } else {
        if (cmd_type == CMD_TESS_COLOR) {
            formatted = repl_format_fits(new_source, sizeof(new_source),
                                         "%sgluColor(%g, %g, %g);",
                                         indent_prefix, r, g, b);
        } else {
            formatted = repl_format_fits(new_source, sizeof(new_source),
                                         "%sglColor3f(%g, %g, %g);",
                                         indent_prefix, r, g, b);
        }
    }
    if (!formatted) {
        set_status("Command too long");
        return;
    }
    g_cmds[g_cp_line].args[0]=r;
    g_cmds[g_cp_line].args[1]=g;
    g_cmds[g_cp_line].args[2]=b;
    g_cmds[g_cp_line].num_args = g_cp_has_alpha ? 4 : 3;
    if (g_cp_has_alpha)
        g_cmds[g_cp_line].args[3]=g_cp_alpha;
    memcpy(g_cmds[g_cp_line].source, new_source, strlen(new_source) + 1);
    g_flat_dirty = 1;
}

/* Open (or switch) the picker for cmd_idx.  my is GLUT screen y coord. */
static void color_picker_open(int cmd_idx, int my) {
    int cp_x, cp_w;
    code_panel_rect(&cp_x, NULL, &cp_w, NULL);
    g_cp_line      = cmd_idx;
    g_cp_has_alpha = (g_cmds[cmd_idx].type == CMD_COLOR4F ||
                      g_cmds[cmd_idx].type == CMD_TESS_COLOR ||
                      g_cmds[cmd_idx].type == CMD_CLEAR_COLOR);
    g_cp_alpha     = g_cp_has_alpha ? g_cmds[cmd_idx].args[3] : 1.0f;
    cp_rgb_to_hsv(g_cmds[cmd_idx].args[0],
                  g_cmds[cmd_idx].args[1],
                  g_cmds[cmd_idx].args[2],
                  &g_cp_hue, &g_cp_sat, &g_cp_val);
    if (g_cmds[cmd_idx].type == CMD_CLEAR_COLOR &&
        g_cp_val > CP_CLEAR_MAX_V) g_cp_val = CP_CLEAR_MAX_V;
    /* Position to the right of the panel, near the click y */
    int pw = CP_SV_SZ + CP_GAP + CP_HUE_W
           + (g_cp_has_alpha ? CP_GAP + CP_ALPHA_W : 0) + CP_GAP;
    int ph = CP_SV_SZ + CP_GAP + CP_PREV_H + CP_GAP;
    int ppx = cp_x + cp_w + 8;
    if (ppx + pw > g_win_w - 4) ppx = cp_x - pw - 4;
    if (ppx < 4) ppx = 4;
    if (ppx + pw > g_win_w - 4) ppx = g_win_w - pw - 4;
    if (ppx < 4) ppx = 4;
    int ppy = (g_win_h - my) + ph / 2;
    if (ppy > g_win_h - 4) ppy = g_win_h - 4;
    if (ppy - ph < 4)       ppy = ph + 4;
    g_cp_px = ppx;  g_cp_py = ppy;
}

static void render_color_picker(void) {
    if (g_cp_line < 0) return;
    int px = g_cp_px, py = g_cp_py, sz = CP_SV_SZ;
    int pw = sz + CP_GAP + CP_HUE_W
           + (g_cp_has_alpha ? CP_GAP + CP_ALPHA_W : 0) + CP_GAP;
    int ph = sz + CP_GAP + CP_PREV_H + CP_GAP;

    /* Background */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.08f, 0.08f, 0.12f, 0.94f);
    draw_quad((float)(px-CP_GAP), (float)(py-ph), (float)(pw+CP_GAP), (float)(ph+CP_GAP));
    glColor4f(0.30f, 0.30f, 0.50f, 0.80f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(px-CP_GAP,        py-ph);
    glVertex2f(px-CP_GAP+pw+CP_GAP, py-ph);
    glVertex2f(px-CP_GAP+pw+CP_GAP, py+CP_GAP);
    glVertex2f(px-CP_GAP,        py+CP_GAP);
    glEnd();
    glDisable(GL_BLEND);

    /* SV square: white→hue left-right, hue→black top-bottom */
    g_cp_sv_x=px; g_cp_sv_y=py-sz; g_cp_sv_sz=sz;
    float hr,hg,hb; cp_hsv_to_rgb(g_cp_hue,1,1,&hr,&hg,&hb);
    glBegin(GL_QUADS);
    glColor3f(1,1,1);    glVertex2f(px,    py);
    glColor3f(hr,hg,hb); glVertex2f(px+sz, py);
    glColor3f(hr,hg,hb); glVertex2f(px+sz, py-sz);
    glColor3f(1,1,1);    glVertex2f(px,    py-sz);
    glEnd();
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_QUADS);
    glColor4f(0,0,0,0); glVertex2f(px,    py);
    glColor4f(0,0,0,0); glVertex2f(px+sz, py);
    glColor4f(0,0,0,1); glVertex2f(px+sz, py-sz);
    glColor4f(0,0,0,1); glVertex2f(px,    py-sz);
    glEnd();
    glDisable(GL_BLEND);
    /* glClearColor: shade the V > CP_CLEAR_MAX_V zone to show it's off-limits */
    if (g_cp_line >= 0 && g_cp_line < g_num_cmds &&
        g_cmds[g_cp_line].type == CMD_CLEAR_COLOR) {
        float lim_y = py - (1.0f - CP_CLEAR_MAX_V) * (float)sz;
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(0.0f, 0.0f, 0.0f, 0.72f);
        draw_quad((float)px, lim_y, (float)sz, py - lim_y);
        glDisable(GL_BLEND);
        glColor3f(0.85f, 0.85f, 0.50f); glLineWidth(1.5f);
        glBegin(GL_LINES);
        glVertex2f((float)px, lim_y); glVertex2f((float)(px+sz), lim_y);
        glEnd();
        glLineWidth(1.0f);
    }
    /* SV cursor */
    float cx=px+g_cp_sat*sz, cy=py-(1.0f-g_cp_val)*sz;
    glColor3f(1,1,1); glLineWidth(1.5f); cp_ring(cx,cy,5.0f,16);
    glColor3f(0,0,0); glLineWidth(1.0f); cp_ring(cx,cy,6.5f,16);

    /* Hue bar: hue=0 at top, hue=1 at bottom */
    int hx=px+sz+CP_GAP;
    g_cp_hue_x=hx; g_cp_hue_y=py-sz; g_cp_hue_h=sz;
    for (int i=0;i<40;i++) {
        float h1=(float)i/40.0f, h2=(float)(i+1)/40.0f;
        float r1,g1,b1,r2,g2,b2;
        cp_hsv_to_rgb(h1,1,1,&r1,&g1,&b1); cp_hsv_to_rgb(h2,1,1,&r2,&g2,&b2);
        glBegin(GL_QUADS);
        glColor3f(r1,g1,b1); glVertex2f(hx,          py-h1*sz);
        glColor3f(r1,g1,b1); glVertex2f(hx+CP_HUE_W, py-h1*sz);
        glColor3f(r2,g2,b2); glVertex2f(hx+CP_HUE_W, py-h2*sz);
        glColor3f(r2,g2,b2); glVertex2f(hx,          py-h2*sz);
        glEnd();
    }
    glColor3f(1,1,1); glLineWidth(2.0f);
    float hy=py-g_cp_hue*sz;
    glBegin(GL_LINES); glVertex2f(hx-2,hy); glVertex2f(hx+CP_HUE_W+2,hy); glEnd();
    glLineWidth(1.0f);

    /* Alpha bar (COLOR4F only): alpha=1 at top */
    if (g_cp_has_alpha) {
        int ax=hx+CP_HUE_W+CP_GAP;
        g_cp_alp_x=ax; g_cp_alp_y=py-sz; g_cp_alp_h=sz;
        float cr,cg,cb; cp_hsv_to_rgb(g_cp_hue,g_cp_sat,g_cp_val,&cr,&cg,&cb);
        int ck=5;
        for (int iy=0;iy<sz;iy+=ck) for (int ix=0;ix<CP_ALPHA_W;ix+=ck) {
            float gv=((ix/ck+iy/ck)%2)?0.35f:0.55f; glColor3f(gv,gv,gv);
            int tw=(ix+ck<CP_ALPHA_W)?ck:CP_ALPHA_W-ix, th=(iy+ck<sz)?ck:sz-iy;
            draw_quad((float)(ax+ix),(float)(py-iy-th),(float)tw,(float)th);
        }
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        for (int i=0;i<40;i++) {
            float a1=1.0f-(float)i/40.0f, a2=1.0f-(float)(i+1)/40.0f;
            float y1=py-(float)i/40.0f*sz,  y2=py-(float)(i+1)/40.0f*sz;
            glBegin(GL_QUADS);
            glColor4f(cr,cg,cb,a1); glVertex2f(ax,            y1);
            glColor4f(cr,cg,cb,a1); glVertex2f(ax+CP_ALPHA_W, y1);
            glColor4f(cr,cg,cb,a2); glVertex2f(ax+CP_ALPHA_W, y2);
            glColor4f(cr,cg,cb,a2); glVertex2f(ax,            y2);
            glEnd();
        }
        glDisable(GL_BLEND);
        float ay=py-(1.0f-g_cp_alpha)*sz;
        glColor3f(1,1,1); glLineWidth(2.0f);
        glBegin(GL_LINES); glVertex2f(ax-2,ay); glVertex2f(ax+CP_ALPHA_W+2,ay); glEnd();
        glLineWidth(1.0f);
    }

    /* Preview swatch */
    float pr,pg,pb; cp_hsv_to_rgb(g_cp_hue,g_cp_sat,g_cp_val,&pr,&pg,&pb);
    int total_w=sz+CP_GAP+CP_HUE_W+(g_cp_has_alpha?CP_GAP+CP_ALPHA_W:0);
    int swy=py-sz-CP_GAP;
    if (g_cp_has_alpha) {
        int ck=4;
        for (int iy=0;iy<CP_PREV_H;iy+=ck) for (int ix=0;ix<total_w;ix+=ck) {
            float gv=((ix/ck+iy/ck)%2)?0.35f:0.55f; glColor3f(gv,gv,gv);
            int tw=(ix+ck<total_w)?ck:total_w-ix, th=(iy+ck<CP_PREV_H)?ck:CP_PREV_H-iy;
            draw_quad((float)(px+ix),(float)(swy-CP_PREV_H+iy),(float)tw,(float)th);
        }
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(pr,pg,pb,g_cp_alpha);
    } else {
        glColor3f(pr,pg,pb);
    }
    draw_quad((float)px,(float)(swy-CP_PREV_H),(float)total_w,(float)CP_PREV_H);
    if (g_cp_has_alpha) glDisable(GL_BLEND);
    glColor3f(0.4f,0.4f,0.5f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(px,         swy-CP_PREV_H); glVertex2f(px+total_w, swy-CP_PREV_H);
    glVertex2f(px+total_w, swy);           glVertex2f(px,         swy);
    glEnd();
}

static int color_picker_press(int mx, int my) {
    if (g_cp_line < 0) return 0;
    int gl_y = g_win_h - my;

    /* SV square */
    if (mx >= g_cp_sv_x && mx < g_cp_sv_x+g_cp_sv_sz &&
        gl_y >= g_cp_sv_y && gl_y < g_cp_sv_y+g_cp_sv_sz) {
        g_cp_drag = 1;
        g_cp_sat = (float)(mx-g_cp_sv_x)/(float)g_cp_sv_sz;
        g_cp_val = (float)(gl_y-g_cp_sv_y)/(float)g_cp_sv_sz;
        if (g_cp_sat<0)g_cp_sat=0; if (g_cp_sat>1)g_cp_sat=1;
        if (g_cp_val<0)g_cp_val=0; if (g_cp_val>1)g_cp_val=1;
        if (g_cp_line>=0 && g_cp_line<g_num_cmds &&
            g_cmds[g_cp_line].type==CMD_CLEAR_COLOR &&
            g_cp_val>CP_CLEAR_MAX_V) g_cp_val=CP_CLEAR_MAX_V;
        color_picker_write_cmd(); return 1;
    }
    /* Hue bar */
    if (mx >= g_cp_hue_x && mx < g_cp_hue_x+CP_HUE_W &&
        gl_y >= g_cp_hue_y && gl_y < g_cp_hue_y+g_cp_hue_h) {
        g_cp_drag = 2;
        g_cp_hue = 1.0f-(float)(gl_y-g_cp_hue_y)/(float)g_cp_hue_h;
        if (g_cp_hue<0)g_cp_hue=0; if (g_cp_hue>=1)g_cp_hue=0.999f;
        color_picker_write_cmd(); return 1;
    }
    /* Alpha bar */
    if (g_cp_has_alpha &&
        mx >= g_cp_alp_x && mx < g_cp_alp_x+CP_ALPHA_W &&
        gl_y >= g_cp_alp_y && gl_y < g_cp_alp_y+g_cp_alp_h) {
        g_cp_drag = 3;
        g_cp_alpha = (float)(gl_y-g_cp_alp_y)/(float)g_cp_alp_h;
        if (g_cp_alpha<0)g_cp_alpha=0; if (g_cp_alpha>1)g_cp_alpha=1;
        color_picker_write_cmd(); return 1;
    }

    /* Click outside picker: close and let the event fall through */
    g_cp_line = -1;  g_cp_drag = 0;
    return 0;
}

static int color_picker_motion(int mx, int my) {
    if (g_cp_drag == 0) return 0;
    int gl_y = g_win_h - my;
    if (g_cp_drag == 1) {
        g_cp_sat = (float)(mx-g_cp_sv_x)/(float)g_cp_sv_sz;
        g_cp_val = (float)(gl_y-g_cp_sv_y)/(float)g_cp_sv_sz;
        if (g_cp_sat<0)g_cp_sat=0; if (g_cp_sat>1)g_cp_sat=1;
        if (g_cp_val<0)g_cp_val=0; if (g_cp_val>1)g_cp_val=1;
        if (g_cp_line>=0 && g_cp_line<g_num_cmds &&
            g_cmds[g_cp_line].type==CMD_CLEAR_COLOR &&
            g_cp_val>CP_CLEAR_MAX_V) g_cp_val=CP_CLEAR_MAX_V;
    } else if (g_cp_drag == 2) {
        g_cp_hue = 1.0f-(float)(gl_y-g_cp_hue_y)/(float)g_cp_hue_h;
        if (g_cp_hue<0)g_cp_hue=0; if (g_cp_hue>=1)g_cp_hue=0.999f;
    } else if (g_cp_drag == 3) {
        g_cp_alpha = (float)(gl_y-g_cp_alp_y)/(float)g_cp_alp_h;
        if (g_cp_alpha<0)g_cp_alpha=0; if (g_cp_alpha>1)g_cp_alpha=1;
    }
    color_picker_write_cmd();
    return 1;
}

static void color_picker_release(void) { g_cp_drag = 0; }

/* Close the picker.  Returns 1 if it was open (caller should redisplay). */
static int color_picker_close(void) {
    if (g_cp_line < 0) return 0;
    g_cp_line = -1;  g_cp_drag = 0;
    return 1;
}

void render_code_panel(void) {
    prof_begin(PROF_CODE_PANEL_LAYOUT);
    prof_begin(PROF_CODE_PANEL_LAYOUT_GEOM);
    prof_begin(PROF_CODE_PANEL_LAYOUT_GEOM_SETUP);

    int cmd_main_rows[MAX_COMMANDS];
    int replay_extra_rows[MAX_COMMANDS];
    int cp_x, cp_y, cp_w, cp_h;
    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (cp_w <= 0 || cp_h <= 0) {
        prof_end(PROF_CODE_PANEL_LAYOUT_GEOM_SETUP);
        prof_end(PROF_CODE_PANEL_LAYOUT_GEOM);
        prof_end(PROF_CODE_PANEL_LAYOUT);
        return;
    }
    refresh_workspace_header_lines();
    int panel_w = cp_w;
    int panel_top = cp_y + cp_h;  /* y of the panel's top edge (OpenGL coords) */
    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    int text_x = idx_x + idx_col_w;
    int visible_lines = code_panel_visible_lines_for_height(cp_h);

    /* When cursor is on a vertex, find which normal/color lines feed it so
     * we can draw a gutter accent bar on them below. */
    int highlight_normal_idx = -1;
    int highlight_color_idx  = -1;
    if (!g_inserting && g_edit_line < g_num_cmds && g_cmds[g_edit_line].valid) {
        highlight_normal_idx = repl_find_feeding_normal_cmd(g_edit_line);
        highlight_color_idx  = repl_find_feeding_color_cmd(g_edit_line);
    }

    prof_end(PROF_CODE_PANEL_LAYOUT_GEOM_SETUP);
    prof_begin(PROF_CODE_PANEL_LAYOUT_GEOM_PRECOMPUTE);

    /* Build per-frame replay annotation cache before any annotation work */
    if (g_replay_active &&
        s_replay_cache_pc != g_replay_pc)
        rebuild_replay_annotation_cache();
    else if (!g_replay_active)
        invalidate_replay_annotation_cache();

    int header_rows = code_panel_header_row_count(panel_w, text_x);
    int footer_rows = code_panel_footer_row_count(panel_w, text_x);
    code_panel_precompute_layout_rows(panel_w, text_x,
                                      cmd_main_rows, replay_extra_rows);
    prof_end(PROF_CODE_PANEL_LAYOUT_GEOM_PRECOMPUTE);
    prof_begin(PROF_CODE_PANEL_LAYOUT_GEOM_TOTALS);

    int total_lines = header_rows + footer_rows + code_panel_newline_rows(panel_w, text_x);
    for (int i = 0; i < g_num_cmds; i++) {
        if (g_inserting && i == g_edit_line)
            total_lines += code_panel_insert_rows(panel_w, text_x);
        total_lines += cmd_main_rows[i];
        total_lines += replay_extra_rows[i];
    }

    prof_end(PROF_CODE_PANEL_LAYOUT_GEOM_TOTALS);

    prof_end(PROF_CODE_PANEL_LAYOUT_GEOM);
    prof_begin(PROF_CODE_PANEL_LAYOUT_CURSOR);

    int cursor_doc_line = code_panel_cursor_doc_line_from_layout(
        header_rows, cmd_main_rows, replay_extra_rows, panel_w, text_x);

    prof_end(PROF_CODE_PANEL_LAYOUT_CURSOR);
    prof_begin(PROF_CODE_PANEL_LAYOUT_SCROLL);

    int follow_doc_line = code_panel_follow_doc_line_from_layout(
        cursor_doc_line, header_rows, cmd_main_rows, replay_extra_rows);

    /* Only snap to cursor/replay after an edit or replay step; manual scroll
     * can stay off-target. */
    code_panel_apply_follow_scroll(total_lines, visible_lines, follow_doc_line);

    prof_end(PROF_CODE_PANEL_LAYOUT_SCROLL);

    prof_end(PROF_CODE_PANEL_LAYOUT);
    prof_begin(PROF_CODE_PANEL_CHROME);

    begin_2d();

    /* Background */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.06f, 0.06f, 0.10f, 0.92f);
    draw_quad((float)cp_x, (float)cp_y, (float)cp_w, (float)cp_h);

    /* Border: divider between code panel and scene */
    glColor4f(0.30f, 0.30f, 0.50f, 0.80f);
    glBegin(GL_LINES);
    if (code_panel_layout_mode() == CODE_PANEL_LAYOUT_TOP) {
        /* Horizontal divider along bottom of code panel. */
        glVertex2f(0.0f, (float)cp_y);
        glVertex2f((float)g_win_w, (float)cp_y);
    } else if (code_panel_layout_mode() == CODE_PANEL_LAYOUT_BOTTOM) {
        /* Horizontal divider along top of code panel. */
        glVertex2f(0.0f, (float)(cp_y + cp_h));
        glVertex2f((float)g_win_w, (float)(cp_y + cp_h));
    } else {
        /* Vertical divider along right edge of code panel */
        glVertex2f((float)(cp_x + cp_w), 0.0f);
        glVertex2f((float)(cp_x + cp_w), (float)g_win_h);
    }
    glEnd();
    glDisable(GL_BLEND);

    /* Menu bar — design ref: Header Wireframes v2 (now at the very top of
     * the code panel; the old info bar moved into the bottom status strip). */
    {
        int menu_x[NUM_MENUS], menu_w[NUM_MENUS];
        int pin_x[NUM_PIN_BTNS], pin_w[NUM_PIN_BTNS];
        int by, bh;
        menubar_rects(menu_x, menu_w, pin_x, pin_w, &by, &bh);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        /* Full-width strip: #1d1d1d */
        glColor4f(0.114f, 0.114f, 0.114f, 0.98f);
        draw_quad((float)cp_x, (float)by, (float)cp_w, (float)bh);

        int hover_menu = menubar_menu_hit(g_mouse_x, g_mouse_y);
        int hover_pin  = menubar_pin_hit(g_mouse_x, g_mouse_y);

        /* Left-side menu labels */
        for (int i = 0; i < NUM_MENUS; i++) {
            int active = (g_open_menu == i);
            int hover  = (hover_menu == i);
            if (active) {
                glColor4f(0.149f, 0.149f, 0.149f, 1.0f); /* #262626 */
                draw_quad((float)menu_x[i], (float)by, (float)menu_w[i], (float)bh);
            } else if (hover) {
                glColor4f(0.165f, 0.165f, 0.165f, 1.0f); /* #2a2a2a */
                draw_quad((float)menu_x[i], (float)by, (float)menu_w[i], (float)bh);
            }
            if (active || hover)
                glColor3f(1.0f, 1.0f, 1.0f);
            else
                glColor3f(0.847f, 0.847f, 0.847f);       /* #d8d8d8 */
            int tx = menu_x[i] + 9;
            draw_string((float)tx, (float)(by + 3),
                        g_menu_labels[i], FONT_SMALL);
        }

        /* Mask the combined pin area with the menubar bg so a long menu
         * label in a narrow window can't bleed through transparent pin
         * slots (pins must stay visible and take hit-test priority). */
        int pin_block_x = pin_x[PIN_SEARCH];
        int pin_block_w = cp_x + cp_w - CODE_MARGIN_X - pin_block_x;
        glColor4f(0.114f, 0.114f, 0.114f, 1.0f); /* #1d1d1d, fully opaque */
        draw_quad((float)pin_block_x, (float)by, (float)pin_block_w, (float)bh);

        /* Right-side pins: Search | Replay (always rendered on top) */
        for (int i = 0; i < NUM_PIN_BTNS; i++) {
            int hover = (hover_pin == i);
            int active = (i == PIN_REPLAY && g_replay_active);
            if (hover) {
                glColor4f(0.165f, 0.165f, 0.165f, 1.0f);
                draw_quad((float)pin_x[i], (float)by, (float)pin_w[i], (float)bh);
            } else if (active) {
                glColor4f(0.149f, 0.149f, 0.149f, 1.0f);
                draw_quad((float)pin_x[i], (float)by, (float)pin_w[i], (float)bh);
            }
            /* Left separator rule (#2a2a2a) */
            glColor4f(0.165f, 0.165f, 0.165f, 1.0f);
            glBegin(GL_LINES);
            glVertex2f((float)pin_x[i], (float)by);
            glVertex2f((float)pin_x[i], (float)(by + bh));
            glEnd();

            if (i == PIN_SEARCH) {
                if (g_search_active) {
                    /* render_code_panel_search_overlay() fills this slot */
                    continue;
                }
                /* "search..." label in muted gray */
                glColor3f(0.478f, 0.478f, 0.478f); /* #7a7a7a */
                int tx = pin_x[i] + 12;
                draw_string((float)tx, (float)(by + 3),
                            g_pin_btn_labels[i], FONT_SMALL);
            } else if (i == PIN_REPLAY) {
                /* Green accent (#6fb36f), state icon + dynamic label */
                const char *label = "Replay";
                if (g_replay_state == REPLAY_PLAYING) label = "Replaying";
                else if (g_replay_state == REPLAY_PAUSED) label = "Paused";
                else if (g_replay_state == REPLAY_DONE)   label = "Done";

                int icon_x = pin_x[i] + 10;
                int icon_cy = by + bh / 2;
                int icon_sz = 8;

                glColor3f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B);

                if (g_replay_state == REPLAY_PLAYING) {
                    /* Two vertical bars (pause glyph) */
                    float bw = 2.5f, gap = 2.0f;
                    float by0 = (float)icon_cy - (float)icon_sz * 0.5f;
                    float bh0 = (float)icon_sz;
                    draw_quad((float)icon_x,                    by0, bw, bh0);
                    draw_quad((float)icon_x + bw + gap,         by0, bw, bh0);
                } else if (g_replay_state == REPLAY_DONE) {
                    /* Square — run complete */
                    float sx = (float)icon_x;
                    float sy = (float)icon_cy - (float)icon_sz * 0.5f;
                    draw_quad(sx, sy, (float)icon_sz, (float)icon_sz);
                } else {
                    /* Play triangle — stopped (OFF) or paused, click to start */
                    float x0 = (float)icon_x;
                    float cy = (float)icon_cy;
                    glBegin(GL_TRIANGLES);
                    glVertex2f(x0,             cy - (float)icon_sz * 0.5f);
                    glVertex2f(x0,             cy + (float)icon_sz * 0.5f);
                    glVertex2f(x0 + icon_sz,   cy);
                    glEnd();
                }

                int tx = icon_x + 12 + 6;
                draw_string((float)tx, (float)(by + 3), label, FONT_SMALL);
            } else {
                if (hover || active)
                    glColor3f(1.0f, 1.0f, 1.0f);
                else
                    glColor3f(0.847f, 0.847f, 0.847f);
                int tx = pin_x[i] + 9;
                draw_string((float)tx, (float)(by + 3),
                            g_pin_btn_labels[i], FONT_SMALL);
            }
        }

        /* Bottom divider (#000) */
        glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
        glBegin(GL_LINES);
        glVertex2f((float)cp_x,          (float)by);
        glVertex2f((float)(cp_x + cp_w), (float)by);
        glEnd();

        glDisable(GL_BLEND);
    }

    render_code_panel_search_overlay(cp_x, panel_w, panel_top);

    prof_end(PROF_CODE_PANEL_CHROME);
    prof_begin(PROF_CODE_PANEL_LINES);
    prof_begin(PROF_CODE_PANEL_LINES_STATIC);

    /* Code lines begin immediately below the menu bar (row 0). */
    int line_y = panel_top - CODE_MARGIN_Y - 2 * LINE_H;
    int cur = 0;
    int file_line = 1;

    /* Macro for rendering a static line (header/footer) */
    #define RENDER_STATIC_LINE(text, set_color) do {                           \
        CodeWrapIter wrap_it;                                                   \
        int wrap_row = 0;                                                       \
        int wrap_start, wrap_len, wrap_x;                                       \
        code_wrap_iter_init(&wrap_it, text, text_x, panel_w);                   \
        while (code_wrap_iter_next(&wrap_it, &wrap_start, &wrap_len, &wrap_x)) {\
            if (cur >= g_scroll && cur < g_scroll + visible_lines) {            \
                if (wrap_row == 0) {                                             \
                    glColor3f(0.30f, 0.30f, 0.38f);                            \
                    { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);  \
                      draw_string((float)CODE_MARGIN_X, (float)line_y,          \
                                  ln, FONT_MONO); }                             \
                }                                                               \
                set_color;                                                       \
                code_panel_draw_segment(wrap_x, line_y, text,                   \
                                        wrap_start, wrap_len, FONT_MONO);       \
                line_y -= LINE_H;                                                \
            }                                                                    \
            cur++;                                                               \
            wrap_row++;                                                          \
        }                                                                        \
        file_line++;                                                            \
    } while (0)

    /* Workspace state (saved var values + config toggles) */
    for (int i = 0; i < g_workspace_header_line_count; i++) {
        RENDER_STATIC_LINE(g_workspace_header_lines[i], glColor3f(0.45f, 0.55f, 0.42f));
    }
    /* Header pre-lookAt (dimmed) */
    for (int i = 0; g_header_pre[i]; i++) {
        RENDER_STATIC_LINE(g_header_pre[i], glColor3f(0.38f, 0.38f, 0.42f));
    }
    /* Dynamic render-state lines */
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++) {
        RENDER_STATIC_LINE(g_render_state_lines[i], glColor3f(0.50f, 0.45f, 0.55f));
    }
    /* Camera transform lines (dynamic — updated every frame) */
    for (int i = 0; i < CAM_LINE_COUNT; i++) {
        RENDER_STATIC_LINE(g_cam_lines[i], glColor3f(0.50f, 0.45f, 0.55f));
    }
    /* Header post-camera */
    for (int i = 0; g_header_post[i]; i++) {
        RENDER_STATIC_LINE(g_header_post[i], glColor3f(0.38f, 0.38f, 0.42f));
    }

    prof_end(PROF_CODE_PANEL_LINES_STATIC);
    prof_begin(PROF_CODE_PANEL_LINES_BODY);
    prof_begin(PROF_CODE_PANEL_LINES_BODY_CMDS);

    /* Commands + insert line + new-line slot */
    int vnum = 0; /* vertex counter within current glBegin/glEnd block */
    int loop_depth = 0;
    int in_tess_poly = 0;
    int tess_depth = 0;
    int primitive_vnums_exact = 1;
    for (int i = 0; i < g_num_cmds; i++) {
        /* If inserting, render the virtual insert line before command[g_edit_line] */
        if (g_inserting && i == g_edit_line) {
                        render_active_input_rows(panel_w, text_x, idx_x,
                                                                         visible_lines, file_line,
                                                                         code_panel_active_indent_chars(), NULL,
                                                                         g_edit_line,
                                                                         &cur, &line_y);
            file_line++;
        }

        if (i < g_num_cmds) {
            /* Track vertex number for all commands regardless of visibility */
            if (g_cmds[i].valid) {
                if (g_cmds[i].type == CMD_BEGIN) {
                    vnum = 0;
                    primitive_vnums_exact = (loop_depth == 0);
                } else if (g_cmds[i].type == CMD_TESS_BEGIN_POLYGON) {
                    vnum = 0;
                    in_tess_poly = 1;
                    tess_depth = 1;
                    primitive_vnums_exact = (loop_depth == 0);
                }
            }

            int is_edit = (!g_inserting && i == g_edit_line);
            int is_vertex = g_cmds[i].valid && (g_cmds[i].type == CMD_VERTEX3F ||
                                                g_cmds[i].type == CMD_TESS_VERTEX);
            if (is_edit) {
                /* Active editing line */
                char idx_s[16];
                const char *idx_text = NULL;
                if (g_show_indices && is_vertex) {
                    snprintf(idx_s, sizeof(idx_s),
                             primitive_vnums_exact ? "v%d" : "vn", vnum);
                    idx_text = idx_s;
                }
                render_active_input_rows(panel_w, text_x, idx_x,
                                         visible_lines, file_line,
                                         code_panel_active_indent_chars(), idx_text,
                                         g_edit_line,
                                         &cur, &line_y);
                file_line++;
            } else {
                /* Existing command, not being edited */
                CodeWrapIter wrap_it;
                char display_text[MAX_INPUT_LEN];
                int wrap_row = 0;
                int wrap_start, wrap_len, wrap_x;
                int search_row_idx = repl_search_row_for_cmd_index(i);
                code_panel_get_command_display_text(i, display_text,
                                                    sizeof(display_text));
                code_wrap_iter_init(&wrap_it, display_text, text_x, panel_w);
                while (code_wrap_iter_next(&wrap_it, &wrap_start, &wrap_len, &wrap_x)) {
                    if (cur >= g_scroll && cur < g_scroll + visible_lines) {
                        if (g_replay_active &&
                            g_replay_src_line >= 0 && i == g_replay_src_line) {
                            glEnable(GL_BLEND);
                            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                            glColor4f(0.10f, 0.35f, 0.15f, 0.55f);
                            draw_quad(0, (float)(line_y - 3),
                                      (float)panel_w, (float)LINE_H);
                            glColor4f(0.20f, 0.90f, 0.30f, 0.85f);
                            draw_quad(1.0f, (float)(line_y - 3), 3.0f, (float)LINE_H);
                            glDisable(GL_BLEND);
                        }
                        if (sel_active() && i >= sel_lo() && i <= sel_hi()) {
                            glEnable(GL_BLEND);
                            glColor4f(0.20f, 0.30f, 0.50f, 0.55f);
                            draw_quad(0, (float)(line_y - 3),
                                      (float)panel_w, (float)LINE_H);
                            glDisable(GL_BLEND);
                        }
                        if (i == highlight_normal_idx || i == highlight_color_idx) {
                            glEnable(GL_BLEND);
                            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                            if (i == highlight_normal_idx)
                                glColor4f(0.40f, 0.80f, 0.95f, 0.85f);
                            else
                                glColor4f(0.95f, 0.85f, 0.30f, 0.85f);
                            draw_quad(1.0f, (float)(line_y - 3), 3.0f, (float)LINE_H);
                            glDisable(GL_BLEND);
                        }
                        if (wrap_row == 0) {
                            glColor3f(0.30f, 0.30f, 0.38f);
                            { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);
                              draw_string((float)CODE_MARGIN_X, (float)line_y,
                                          ln, FONT_MONO); }
                            if (g_show_indices && is_vertex) {
                                char idx_s[16];
                                snprintf(idx_s, sizeof(idx_s),
                                         primitive_vnums_exact ? "v%d" : "vn", vnum);
                                glColor3f(0.45f, 0.50f, 0.65f);
                                draw_string((float)idx_x, (float)line_y,
                                            idx_s, FONT_MONO);
                            }
                               /* Color swatch for glColor / glClearColor */
                               if ((g_cmds[i].type == CMD_COLOR3F ||
                                   g_cmds[i].type == CMD_COLOR4F ||
                                   g_cmds[i].type == CMD_TESS_COLOR ||
                                   g_cmds[i].type == CMD_CLEAR_COLOR) &&
                                g_cmds[i].valid && !g_cmds[i].has_vars) {
                                int sw = SWATCH_W;
                                int sx = cp_x + cp_w - CODE_MARGIN_X - sw - 2;
                                int sy = line_y + (LINE_H - sw) / 2 - 1;
                                glEnable(GL_BLEND);
                                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                                  float sa = (g_cmds[i].type == CMD_COLOR4F ||
                                            g_cmds[i].type == CMD_TESS_COLOR ||
                                            g_cmds[i].type == CMD_CLEAR_COLOR)
                                           ? g_cmds[i].args[3] : 1.0f;
                                glColor4f(g_cmds[i].args[0], g_cmds[i].args[1],
                                          g_cmds[i].args[2], sa);
                                draw_quad((float)sx, (float)sy, (float)sw, (float)sw);
                                /* Border */
                                glColor4f(0.55f, 0.55f, 0.65f, 0.9f);
                                glBegin(GL_LINE_LOOP);
                                glVertex2f(sx,    sy);    glVertex2f(sx+sw, sy);
                                glVertex2f(sx+sw, sy+sw); glVertex2f(sx,    sy+sw);
                                glEnd();
                                /* Highlight when picker is open for this line */
                                if (g_cp_line == i) {
                                    glColor4f(1.0f, 1.0f, 1.0f, 0.9f);
                                    glBegin(GL_LINE_LOOP);
                                    glVertex2f(sx-1,    sy-1);
                                    glVertex2f(sx+sw+1, sy-1);
                                    glVertex2f(sx+sw+1, sy+sw+1);
                                    glVertex2f(sx-1,    sy+sw+1);
                                    glEnd();
                                }
                                glDisable(GL_BLEND);
                            }
                        }
                        color_for_type(g_cmds[i].type);
                        code_panel_draw_search_highlights(g_cmds[i].source,
                                                          search_row_idx,
                                                          wrap_start, wrap_len,
                                                          wrap_x, line_y);
                        code_panel_draw_segment(wrap_x, line_y, display_text,
                                                wrap_start, wrap_len, FONT_MONO);
                        line_y -= LINE_H;
                    }
                    cur++;
                    wrap_row++;
                }
                file_line++;

                if (g_replay_active &&
                    g_replay_expand_args &&
                    g_replay_src_line >= 0 && i == g_replay_src_line &&
                    g_cmds[i].has_vars &&
                    g_cmds[i].type != CMD_VAR_ASSIGN) {
                    int flat_idx = find_replay_flat_cmd(i);
                    if (flat_idx >= 0) {
                        char subst[MAX_LINE_LEN], var_comment[128];
                        if (build_replay_subst_annotation(i, flat_idx,
                                                          subst, sizeof(subst),
                                                          var_comment, sizeof(var_comment)) > 0) {
                            if (cur >= g_scroll &&
                                cur < g_scroll + visible_lines) {
                                glEnable(GL_BLEND);
                                glBlendFunc(GL_SRC_ALPHA,
                                            GL_ONE_MINUS_SRC_ALPHA);
                                glColor4f(0.10f, 0.25f, 0.15f, 0.35f);
                                draw_quad(0, (float)(line_y - 3),
                                          (float)panel_w, (float)LINE_H);
                                glDisable(GL_BLEND);
                                glColor3f(0.50f, 0.75f, 0.50f);
                                draw_string((float)text_x, (float)line_y,
                                            subst, FONT_MONO);
                                if (var_comment[0]) {
                                    int sw = (int)strlen(subst) * FONT_W;
                                    glColor3f(0.40f, 0.55f, 0.40f);
                                    draw_string((float)(text_x + sw),
                                                (float)line_y,
                                                var_comment, FONT_MONO);
                                }
                                line_y -= LINE_H;
                            }
                            cur++;
                        }

                        {
                            char eval_buf[MAX_LINE_LEN];
                            if (build_replay_eval_annotation(i, flat_idx,
                                                             eval_buf, sizeof(eval_buf))) {
                                if (cur >= g_scroll &&
                                    cur < g_scroll + visible_lines) {
                                    glEnable(GL_BLEND);
                                    glBlendFunc(GL_SRC_ALPHA,
                                                GL_ONE_MINUS_SRC_ALPHA);
                                    glColor4f(0.15f, 0.15f, 0.25f, 0.35f);
                                    draw_quad(0, (float)(line_y - 3),
                                              (float)panel_w, (float)LINE_H);
                                    glDisable(GL_BLEND);
                                    glColor3f(0.50f, 0.60f, 0.80f);
                                    draw_string((float)text_x, (float)line_y,
                                                eval_buf, FONT_MONO);
                                    line_y -= LINE_H;
                                }
                                cur++;
                            }
                        }
                    }
                }
            }

            /* Advance vertex counter after rendering this command */
            if (is_vertex) vnum++;

            if (g_cmds[i].valid) {
                if (g_cmds[i].type == CMD_FOR_BEGIN) {
                    loop_depth++;
                    primitive_vnums_exact = 0;
                } else if (g_cmds[i].type == CMD_FOR_END) {
                    if (loop_depth > 0) loop_depth--;
                } else if (g_cmds[i].type == CMD_END) {
                    primitive_vnums_exact = 1;
                } else if (g_cmds[i].type == CMD_TESS_BEGIN_CONTOUR && in_tess_poly) {
                    tess_depth++;
                } else if (g_cmds[i].type == CMD_TESS_END && in_tess_poly) {
                    if (tess_depth > 0) tess_depth--;
                    if (tess_depth == 0) {
                        in_tess_poly = 0;
                        primitive_vnums_exact = 1;
                    }
                }
            }
        }
    }

    prof_end(PROF_CODE_PANEL_LINES_BODY_CMDS);
    prof_begin(PROF_CODE_PANEL_LINES_BODY_NEWLINE);

    /* New-line slot after the last command */
    {
        int is_edit_nl = (g_edit_line == g_num_cmds);
        if (is_edit_nl) {
            render_active_input_rows(panel_w, text_x, idx_x,
                                     visible_lines, file_line,
                                     code_panel_active_indent_chars(), NULL,
                                     g_edit_line,
                                     &cur, &line_y);
        } else {
            if (cur >= g_scroll && cur < g_scroll + visible_lines) {
                glColor3f(0.30f, 0.30f, 0.38f);
                { char ln[16]; snprintf(ln, sizeof(ln), "%3d", file_line);
                  draw_string(CODE_MARGIN_X, line_y, ln, FONT_MONO); }
                glColor3f(0.28f, 0.28f, 0.35f);
                { char ind_s[32]; int nc = cmd_indent_chars(g_num_cmds);
                  if (nc > 31) nc = 31;
                  memset(ind_s, ' ', nc); ind_s[nc] = '\0';
                  draw_string((float)text_x, (float)line_y, ind_s, FONT_MONO); }
                line_y -= LINE_H;
            }
        }
        file_line++;
        cur++;
    }

    prof_end(PROF_CODE_PANEL_LINES_BODY_NEWLINE);

    prof_end(PROF_CODE_PANEL_LINES_BODY);
    prof_begin(PROF_CODE_PANEL_LINES_FOOTER);

    /* Footer (dimmed) */
    for (int i = 0; g_footer_pre_init[i]; i++) {
        RENDER_STATIC_LINE(g_footer_pre_init[i], glColor3f(0.38f, 0.38f, 0.42f));
    }
    for (int i = 0; i < init_section_line_count(); i++) {
        char line[MAX_LINE_LEN];
        init_section_line(i, line, sizeof(line));
        RENDER_STATIC_LINE(line, glColor3f(0.38f, 0.38f, 0.42f));
    }
    for (int i = 0; g_footer_post_init[i]; i++) {
        RENDER_STATIC_LINE(g_footer_post_init[i], glColor3f(0.38f, 0.38f, 0.42f));
    }

    prof_end(PROF_CODE_PANEL_LINES_FOOTER);

    prof_end(PROF_CODE_PANEL_LINES);
    prof_begin(PROF_CODE_PANEL_OVERLAYS);

    #undef RENDER_STATIC_LINE

    /* Scroll indicator */
    if (total_lines > visible_lines) {
        int bar_h = cp_h - CODE_MARGIN_Y - LINE_H - STATUSBAR_H;
        float frac = (float)visible_lines / (float)total_lines;
        float pos  = (float)g_scroll / (float)total_lines;
        int thumb_h = (int)(bar_h * frac);
        if (thumb_h < 12) thumb_h = 12;
        int thumb_y = panel_top - CODE_MARGIN_Y - LINE_H
                      - (int)(bar_h * pos) - thumb_h;

        glEnable(GL_BLEND);
        glColor4f(0.50f, 0.50f, 0.65f, 0.35f);
        draw_quad((float)(cp_x + cp_w - 6), (float)thumb_y,
                  5.0f, (float)thumb_h);
        glDisable(GL_BLEND);
    }

    /* Bottom status strip — design ref: Header Wireframes v2 statusbar.
     * Always drawn; shows cmd counts, cursor location, AA indicator, and the
     * transient `g_status` message (when set) in amber "err" style. */
    {
        int sy = cp_y;
        int sh = STATUSBAR_H;
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        /* Strip bg (#181818) + top divider (#000) */
        glColor4f(0.094f, 0.094f, 0.094f, 0.98f);
        draw_quad((float)cp_x, (float)sy, (float)cp_w, (float)sh);
        glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
        glBegin(GL_LINES);
        glVertex2f((float)cp_x,          (float)(sy + sh));
        glVertex2f((float)(cp_x + cp_w), (float)(sy + sh));
        glEnd();

        int text_y = sy + (sh - FONT_SMALL_H) / 2 + 1;
        int tx = cp_x + CODE_MARGIN_X;

        /* cmd count */
        char cmds_buf[48];
        snprintf(cmds_buf, sizeof(cmds_buf), "%d/%d cmds",
                 g_num_flat_cmds, MAX_COMMANDS);
        glColor3f(0.878f, 0.878f, 0.878f); /* #e0e0e0 — stronger for counts */
        draw_string((float)tx, (float)text_y, cmds_buf, FONT_SMALL);
        tx += (int)strlen(cmds_buf) * FONT_SMALL_W;

        #define STATUSBAR_SEP() do {                                    \
            tx += 8;                                                    \
            glColor4f(0.20f, 0.20f, 0.20f, 1.0f); /* #333 */            \
            glBegin(GL_LINES);                                          \
            glVertex2f((float)tx, (float)(sy + 4));                     \
            glVertex2f((float)tx, (float)(sy + sh - 4));                \
            glEnd();                                                    \
            tx += 8;                                                    \
        } while (0)

        STATUSBAR_SEP();

        /* Line / insert-mode / begin-mode */
        char ln_buf[64];
        if (g_inserting)
            snprintf(ln_buf, sizeof(ln_buf), "Ln %d [INSERT]", g_edit_line + 1);
        else if (in_begin_block())
            snprintf(ln_buf, sizeof(ln_buf), "Ln %d  %s",
                     g_edit_line + 1, mode_name(current_begin_mode()));
        else
            snprintf(ln_buf, sizeof(ln_buf), "Ln %d", g_edit_line + 1);
        glColor3f(0.627f, 0.627f, 0.627f); /* #a0a0a0 */
        draw_string((float)tx, (float)text_y, ln_buf, FONT_SMALL);
        tx += (int)strlen(ln_buf) * FONT_SMALL_W;

        /* AA indicator */
        if (g_use_accum) {
            STATUSBAR_SEP();
            char aa_buf[24];
            if (g_accum_aa_enabled && g_accum_samples > 1)
                snprintf(aa_buf, sizeof(aa_buf), "AA %dx", g_accum_samples);
            else
                snprintf(aa_buf, sizeof(aa_buf), "AA off");
            draw_string((float)tx, (float)text_y, aa_buf, FONT_SMALL);
            tx += (int)strlen(aa_buf) * FONT_SMALL_W;
        }

        /* The amber g_status message renders at the bottom of the scene panel
         * (see render_scene_status()) — the statusbar has limited width. */

        /* Right-aligned F1 help affordance */
        {
            const char *help_kbd = "F1";
            const char *help_lbl = "help";
            int kbd_w = (int)strlen(help_kbd) * FONT_SMALL_W + 10;
            int lbl_w = (int)strlen(help_lbl) * FONT_SMALL_W;
            int rx = cp_x + cp_w - CODE_MARGIN_X - lbl_w;
            glColor3f(0.627f, 0.627f, 0.627f);
            draw_string((float)rx, (float)text_y, help_lbl, FONT_SMALL);
            int kx = rx - kbd_w - 6;
            int ky = sy + 3;
            int kh = sh - 6;
            glColor4f(0.078f, 0.078f, 0.078f, 1.0f); /* #141414 */
            draw_quad((float)kx, (float)ky, (float)kbd_w, (float)kh);
            glColor4f(0.20f, 0.20f, 0.20f, 1.0f); /* #333 */
            glBegin(GL_LINE_LOOP);
            glVertex2f((float)kx,           (float)ky);
            glVertex2f((float)(kx + kbd_w), (float)ky);
            glVertex2f((float)(kx + kbd_w), (float)(ky + kh));
            glVertex2f((float)kx,           (float)(ky + kh));
            glEnd();
            glColor3f(0.733f, 0.733f, 0.733f); /* #bbb */
            draw_string((float)(kx + 5), (float)(ky + 2), help_kbd, FONT_SMALL);
        }

        #undef STATUSBAR_SEP
        glDisable(GL_BLEND);
    }

    render_color_picker();

    prof_end(PROF_CODE_PANEL_OVERLAYS);

    end_2d();
}

/* ========================================================================= */
/* Autocomplete popup                                                         */
/* ========================================================================= */

void render_autocomplete(void) {
    if (g_ac_count < 1) return;

    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int popup_x = g_cursor_px;
    int popup_y = g_cursor_py - LINE_H - 4;

    /* Calculate popup width from longest match */
    int max_w = 0;
    for (int i = 0; i < g_ac_count; i++) {
        int w = (int)strlen(g_ac_matches[i]) * FONT_W;
        if (w > max_w) max_w = w;
    }
    int popup_w = max_w + 16;
    int popup_h = g_ac_count * LINE_H + 6;

    /* Clamp to code panel width */
    int cp_x, cp_y, cp_w, cp_h;
    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (popup_x + popup_w > cp_x + cp_w - 4)
        popup_x = cp_x + cp_w - popup_w - 4;
    if (popup_x < cp_x + 4) popup_x = cp_x + 4;

    /* Background */
    glColor4f(0.08f, 0.08f, 0.15f, 0.95f);
    draw_quad((float)popup_x, (float)(popup_y - popup_h),
              (float)popup_w, (float)popup_h);

    /* Border */
    glColor4f(0.40f, 0.40f, 0.65f, 0.80f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)popup_x, (float)(popup_y - popup_h));
    glVertex2f((float)(popup_x + popup_w), (float)(popup_y - popup_h));
    glVertex2f((float)(popup_x + popup_w), (float)popup_y);
    glVertex2f((float)popup_x, (float)popup_y);
    glEnd();

    /* Entries */
    int ey = popup_y - LINE_H + 1;
    for (int i = 0; i < g_ac_count; i++) {
        if (i == g_ac_sel) {
            /* Highlight selected */
            glColor4f(0.20f, 0.25f, 0.42f, 0.90f);
            draw_quad((float)(popup_x + 1), (float)(ey - 2),
                      (float)(popup_w - 2), (float)LINE_H);
            glColor3f(1.0f, 1.0f, 0.90f);
        } else {
            glColor3f(0.65f, 0.65f, 0.72f);
        }
        draw_string((float)(popup_x + 8), (float)ey,
                    g_ac_matches[i], FONT_MONO);
        ey -= LINE_H;
    }

    /* Hint text */
    glColor4f(0.45f, 0.45f, 0.55f, 0.70f);
    draw_string((float)(popup_x + 4),
                (float)(popup_y - popup_h - FONT_H - 2),
                "Tab to accept", FONT_SMALL);

    glDisable(GL_BLEND);
    end_2d();
}

void render_example_dropdown(void) {
    if (g_open_menu < 0) return;
    int menu_id = g_open_menu;
    int n  = menu_item_count(menu_id);
    int ne = (menu_id == MENU_SCENE) ? repl_example_count() : -1;

    int dx, dy, dw, dh;
    if (!menu_dropdown_rect(&dx, &dy, &dw, &dh)) return;

    g_menu_item_hover = menu_dropdown_item_hit(g_mouse_x, g_mouse_y);

    float alpha = ui_fade_alpha(g_menu_open_time);

    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Dropdown bg (#222) + border (#3a3a3a) — design ref */
    glColor4f(0.133f, 0.133f, 0.133f, 0.98f * alpha);
    draw_quad((float)dx, (float)dy, (float)dw, (float)dh);
    glColor4f(0.227f, 0.227f, 0.227f, alpha);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)dx,        (float)dy);
    glVertex2f((float)(dx + dw), (float)dy);
    glVertex2f((float)(dx + dw), (float)(dy + dh));
    glVertex2f((float)dx,        (float)(dy + dh));
    glEnd();

    if (n == 0) {
        int ey = dy + dh - LINE_H + 1;
        glColor4f(0.478f, 0.518f, 0.580f, alpha);  /* #7a8494 (header style) */
        draw_string((float)(dx + 14), (float)ey, "(empty)", FONT_SMALL);
        glDisable(GL_BLEND);
        end_2d();
        return;
    }

    int max_sc_px = menu_max_shortcut_px(menu_id);
    int state_right = dx + dw - 14;
    if (max_sc_px > 0) state_right -= max_sc_px + 16;

    int ey = dy + dh - LINE_H + 1;
    for (int i = 0; i < n; i++) {
        const char *lbl = menu_item_label(menu_id, i);
        if (!lbl) continue;

        if (strncmp(lbl, "### ", 4) == 0) {
            glColor4f(0.478f, 0.518f, 0.580f, alpha);  /* #7a8494 (header style) */
            draw_string((float)(dx + 14), (float)ey, lbl + 4, FONT_SMALL);
            ey -= LINE_H;
            continue;
        }

        if (strcmp(lbl, "---") == 0) {
            glColor4f(0.20f, 0.20f, 0.20f, alpha);  /* #333 */
            glBegin(GL_LINES);
            glVertex2f((float)(dx + 6),       (float)(ey + LINE_H / 2 - 2));
            glVertex2f((float)(dx + dw - 6),  (float)(ey + LINE_H / 2 - 2));
            glEnd();
            ey -= LINE_H;
            continue;
        }

        int scene_hit = -1;
        if (menu_id == MENU_SCENE && ne >= 0) {
            int scene_n = i - (ne + SCENE_OFF_SCENES);
            if (scene_n >= 0 && scene_n < repl_user_scene_count())
                scene_hit = repl_scene_menu_slot_for_dense_index(scene_n);
        }
        int is_active_example = (menu_id == MENU_SCENE && ne >= 0 &&
                                 i >= 1 && i <= ne &&
                                 (i - 1) == g_example_idx);
        int is_active_scene   = (scene_hit >= 0 &&
                                 scene_hit == repl_active_user_scene());

        if (i == g_menu_item_hover) {
            glColor4f(0.180f, 0.290f, 0.431f, alpha);  /* #2e4a6e */
            draw_quad((float)(dx + 1), (float)(ey - 2),
                      (float)(dw - 2), (float)LINE_H);
            glColor4f(1.0f, 1.0f, 1.0f, alpha);
        } else if (is_active_example || is_active_scene) {
            glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, alpha);
        } else {
            glColor4f(0.847f, 0.847f, 0.847f, alpha);  /* #d8d8d8 */
        }

        draw_string((float)(dx + 14), (float)ey, lbl, FONT_SMALL);

        const char *sc = menu_item_shortcut(menu_id, i);
        if (sc) {
            int sc_px = (int)strlen(sc) * FONT_SMALL_W;
            glColor4f(0.533f, 0.533f, 0.533f, alpha);  /* #888 */
            draw_string((float)(dx + dw - 14 - sc_px), (float)ey, sc, FONT_SMALL);
        }

        if (menu_id == MENU_CONFIG && g_cfg_items[i].value != NULL) {
            char st_buf[CFG_STATE_MAX_CHARS + 1];
            const char *st = cfg_state_str(i, st_buf, sizeof(st_buf));
            int st_px = (int)strlen(st) * FONT_SMALL_W;
            int val = *g_cfg_items[i].value;
            if (val)
                glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, alpha);
            else
                glColor4f(0.533f, 0.533f, 0.533f, alpha);
            draw_string((float)(state_right - st_px), (float)ey, st, FONT_SMALL);
        }

        ey -= LINE_H;
    }

    glDisable(GL_BLEND);
    end_2d();
}

/* ========================================================================= */
/* Help overlay                                                               */
/* ========================================================================= */

void render_help(void) {
    if (!g_show_help) return;

    /* --- Tab 0: Commands ---
     * '\t' marks the boundary between left column (command) and
     * right column (description).  Lines without '\t' render in
     * a single colour based on indent level. */
    static const char *tab_commands[] = {
        "Supported Commands (type + ;):",
        "  glBegin(MODE)        \tGL_TRIANGLES, GL_TRIANGLE_STRIP, ...",
        "  glEnd()              \tEnd current primitive block",
        "  glVertex3f(x,y,z)    \tSpecify a vertex position",
        "  glVertex2f(x,y)      \tSpecify a 2D vertex (z=0)",
        "  glNormal3f(x,y,z)    \tSpecify a vertex normal",
        "  glColor3f(r,g,b)     \tSpecify vertex color",
        "  glColor4f(r,g,b,a)   \tSpecify color with alpha",
        "  glClearColor(r,g,b,a)\tSet the background clear color",
        "  glTranslatef(x,y,z)  \tTranslate the modelview matrix",
        "  glScalef(sx,sy,sz)   \tScale the modelview matrix",
        "  glRotatef(d,x,y,z)   \tRotate the modelview matrix",
        "  glPushMatrix()       \tPush current matrix onto stack",
        "  glPopMatrix()        \tPop matrix from stack",
        "  glEnable(CAP) / glDisable(CAP)",
        "       \tGL_BLEND, GL_COLOR_MATERIAL, GL_CULL_FACE, GL_DEPTH_TEST",
        "       \tGL_LIGHTING, GL_LINE_SMOOTH, GL_NORMALIZE, GL_POINT_SMOOTH",
        "       \tGL_LIGHT0..GL_LIGHT3",
        "  glShadeModel(MODE)   \tGL_SMOOTH, GL_FLAT",
        "  glFrontFace(MODE)    \tGL_CW, GL_CCW",
        "  glDepthMask(FLAG)    \tGL_TRUE, GL_FALSE (depth-buffer writes)",
        "  glPointSize(size)    \tRasterized point diameter",
        "  glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, const, linear, quadratic)",
        "       \tDistance attenuation: size *= 1/sqrt(const + linear*d + quadratic*d*d)",
        "  glBlendFunc(sfactor, dfactor)",
        "       \tGL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA / GL_ONE",
        "",
        "Lighting / Material:",
        "  glColorMaterial(face, mode)",
        "       \tface: GL_FRONT, GL_BACK, or GL_FRONT_AND_BACK",
        "       \tmode: GL_AMBIENT / GL_AMBIENT_AND_DIFFUSE / GL_DIFFUSE / GL_SPECULAR / GL_EMISSION",
        "  glMaterialf(face, pname, value | {r,g,b,a})",
        "  glLightModeli(pname, param)",
        "       \tpname: GL_LIGHT_MODEL_TWO_SIDE, GL_LIGHT_MODEL_LOCAL_VIEWER",
        "",
        "GLU / GLUT Primitives:",
        "  gluSphere(r, slices, stacks)",
        "  gluCylinder(baseR, topR, h, slices, stacks)",
        "  gluDisk(innerR, outerR, slices, loops)",
        "  gluPartialDisk(innerR, outerR, slices, loops, start, sweep)",
        "  glutSolidTorus(innerR, outerR, nsides, rings)",
        "",
        "GLU Tessellator (concave / complex polygons):",
        "  gluBegin(GLU_POLYGON)  \tStart a tessellated polygon",
        "  gluBegin(GLU_CONTOUR)  \tStart a contour within the polygon",
        "  gluEnd()               \tEnd contour or polygon",
        "  gluNormal(x,y,z)       \tSet per-vertex normal",
        "  gluColor(r,g,b[,a])    \tSet per-vertex color",
        "  gluVertex(x,y,z)       \tAdd vertex to current contour",
        "  Multiple contours in one polygon create holes (opposite winding)",
        "",
        "Math Expressions (use anywhere floats are expected):",
        "  Constants:  PI, TAU       \tFunctions: sin cos tan sqrt abs pow rand",
        "  Operators:  + - * / % ( ) \tAlso: min max floor ceil fmod  rand(seed[,iter])",
        "  Comparison: > < >= <= == !=  Logical: && || !",
        "  Example:    glVertex3f(cos(PI/4), sin(PI/4), 0)",
        "",
        "Variables (declare before use):",
        "  float x, y, z;           \tDeclare variables",
        "  x = 1.5;                 \tAssign a value",
        "  glVertex3f(x, y, z);     \tUse in expressions",
        "  Variables persist across commands and are saved/loaded",
        "",
        "For-Loops:",
        "  for(i, 0, 24) glVertex3f(cos(i*TAU/24), sin(i*TAU/24), 0);",
        "  for(i, 0, N) {           \tMulti-line block:",
        "    glVertex3f(...)         \tend with }",
        "  }",
        "  Nesting supported up to 4 levels",
        "",
        "Functions (func0..func9, up to " _HELP_STR(MAX_FUNC_HINT_PARAMS) " params):",
        "  func0(radius, sides) {   \tDefine with parameters",
        "    for(i, 0, sides) {",
        "      glVertex3f(radius*cos(i*TAU/sides), ...)",
        "    }",
        "  }",
        "  func0(1.5, 6)            \tCall with args",
        "  Recursion works with if(...) guard",
        "  Up to " _HELP_STR(MAX_FUNC_HINT_PARAMS) " parameters per function",
        "",
        "Conditionals:",
        "  if(t > 1) {              \tBody runs when condition is true",
        "    glColor3f(1, 0, 0)",
        "  }",
        "",
        "Labels / Goto (experimental, top-level only):",
        "  :loop                    \tDeclare a jump target",
        "  goto loop                \tJump back; pair with if(...) to exit",
        "",
        "Comments:",
        "  // text                   \tType directly to add a comment line",
        "",
        "Save / Load:",
        "  Click Save C or press Ctrl+S to export output.c",
        "  Reload a saved file:  ./sample output.c",
        "  (Commands between // Snippet start/end are imported)",
        "",
        "Time variable 't':",
        "  Auto-advances from its current value when playing.",
        "  You can pull it back manually, then resume from there.",
        "  Use in any expression: glVertex3f(sin(t), cos(t), 0)",
        "",
        "Accumulation Buffer AA:",
        "  On by default; launch with --noaccum to disable.",
        "  Status shown in info bar (AA:8x / AA:off).",
        "",
        NULL
    };

    /* --- Tab 1: Keys ---
     * Same '\t' convention: left column = key, right = action.
     * The F-Key Toggles section is generated dynamically from g_cfg_items
     * so it always reflects the actual bindings without manual sync. */
    static const char *tab_keys_base[] = {
        "Editing:",
        "  ;                    \tCommit current line",
        "  Enter                \tInsert new line",
        "  Backspace            \tDelete character or selected lines",
        "  Tab / Enter          \tAccept autocomplete suggestion",
        "  Up / Down            \tNavigate lines",
        "  Left / Right         \tMove cursor within line",
        "  Home / Ctrl+A        \tJump to start of line",
        "  End / Ctrl+E         \tJump to end of line",
        "  Shift+Up/Down        \tSelect multiple lines",
        "  Click + drag         \tSelect lines with mouse",
        "  PgUp / PgDn         \tScroll active panel/overlay",
        "",
        "Clipboard & Undo:",
        "  Ctrl+C               \tCopy line/selection",
        "  Ctrl+X               \tCut line/selection",
        "  Ctrl+V               \tPaste before current line",
        "  Ctrl+Z               \tUndo",
        "  Ctrl+Y               \tRedo",
        "",
        "Buffer Operations:",
        "  Ctrl+F               \tSearch source buffer",
        "  Ctrl+D               \tDelete line or selection",
        "  Ctrl+L               \tClear all commands",
        "  Ctrl+\\              \tReformat buffer",
        "  Ctrl+/               \tToggle comment on line",
        "  Ctrl+P               \tDump debug state to stdout",
        "  Ctrl+S               \tSave to output.c",
        "  Ctrl+Q               \tExit and save to temp file",
        "  Escape               \tClear input / close overlay",
        "",
        "Camera:",
        "  Left-drag            \tOrbit",
        "  Right-drag           \tPan (XZ)",
        "  Shift+Right-drag     \tPan (Y)",
        "  Scroll wheel         \tZoom (viewport) / Scroll (code panel)",
        "",
        "Time & Replay:",
        "  Ctrl+T               \tPlay / pause time variable",
        "  Ctrl+Shift+T         \tReset t to 0",
        "  Ctrl+R               \tStart / stop replay",
        "  Ctrl+K               \tJump replay to cursor line (first geometry at/after)",
        "  Space                \tPause / resume replay",
        "  + / -                \tChange replay speed",
        "  m / M                \tToggle polygon / vertex replay mode",
        "  Left / Right         \tStep backward / forward (when paused)",
        "  Esc                  \tStop replay",
        "",
        "Render State:",
        "  Ctrl+B               \tCycle code panel layout",
        "  Ctrl+=               \tIncrease jitter samples",
        "  Ctrl+-               \tDecrease jitter samples",
        "  Ctrl+U               \tToggle GL_MULTISAMPLE",
        "  Ctrl+O               \tCycle grid major tick spacing (1 / 2 / 5 / 10)",
        "  Ctrl+W               \tCycle CPU profile panel",
        "  Ctrl+B               \tToggle Accum AA",
        "",
        "Interface:",
        "  `                    \tOpen Config menu",
        "  Left-click item      \tCycle config entry forward",
        "  Right-click item     \tCycle config entry backward",
        "",
        "Audio:",
        "  Ctrl+Left            \tPrevious track",
        "  Ctrl+Right           \tNext track",
        "",
        "F-Key Toggles:",
        NULL  /* dynamic F-key lines follow */
    };

    /* Build complete tab_keys array: static base + dynamic F-key entries */
    #define HELP_FKEY_MAX 16
    #define HELP_KEYS_MAX 128
    static char      fkey_strbuf[HELP_FKEY_MAX][48];
    static const char *tab_keys[HELP_KEYS_MAX];
    {
        int nk = 0;
        for (int i = 0; tab_keys_base[i] != NULL && nk < HELP_KEYS_MAX - HELP_FKEY_MAX - 4; i++)
            tab_keys[nk++] = tab_keys_base[i];

        /* F1 — not in g_cfg_items */
        snprintf(fkey_strbuf[0], sizeof(fkey_strbuf[0]), "  F1   \tHelp overlay");
        tab_keys[nk++] = fkey_strbuf[0];

        /* F2–F11 — pulled from g_cfg_items by matching key_code (GLUT_KEY_Fn == n) */
        int di = 1;
        for (int fn = 2; fn <= 11 && di < HELP_FKEY_MAX - 1; fn++) {
            for (int ci = 0; ci < CFG_ITEM_COUNT; ci++) {
                if (g_cfg_items[ci].is_special && g_cfg_items[ci].key_code == fn) {
                    snprintf(fkey_strbuf[di], sizeof(fkey_strbuf[di]),
                             "  F%-2d  \t%s", fn, g_cfg_items[ci].label);
                    tab_keys[nk++] = fkey_strbuf[di++];
                    break;
                }
            }
        }

        /* F12 — not in g_cfg_items */
        snprintf(fkey_strbuf[di], sizeof(fkey_strbuf[di]), "  F12  \tCycle examples");
        tab_keys[nk++] = fkey_strbuf[di];

        tab_keys[nk++] = "";
        tab_keys[nk]   = NULL;
    }
    #undef HELP_FKEY_MAX
    #undef HELP_KEYS_MAX

    static const char *tab_labels[] = { "Commands", "Keys" };
    static const char **tabs[]      = { tab_commands, tab_keys };
    #define HELP_NUM_TABS 2

    if (g_help_tab < 0) g_help_tab = 0;
    if (g_help_tab >= HELP_NUM_TABS) g_help_tab = HELP_NUM_TABS - 1;

    const char **text = tabs[g_help_tab];

    /* Count total lines */
    int n_lines = 0;
    while (text[n_lines]) n_lines++;

    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int hx = g_win_w / 6, hy = g_win_h / 12;
    int hw = g_win_w * 2 / 3, hh = g_win_h * 5 / 6;
    int tab_bar_h = LINE_H + 2;
    int title_h   = LINE_H + 4;
    int pad_top   = title_h + tab_bar_h + 6;
    int pad_bot   = 20;
    int content_h = hh - pad_top - pad_bot;
    int visible_lines = content_h / LINE_H;
    if (visible_lines < 1) visible_lines = 1;

    /* Clamp scroll */
    int max_scroll = n_lines - visible_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (g_help_scroll > max_scroll) g_help_scroll = max_scroll;
    if (g_help_scroll < 0) g_help_scroll = 0;

    /* Background — matches config menu #222 */
    glColor4f(0.133f, 0.133f, 0.133f, 0.98f);
    draw_quad((float)hx, (float)hy, (float)hw, (float)hh);

    /* Border — matches config menu #3a3a3a */
    glColor4f(0.227f, 0.227f, 0.227f, 1.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)hx,        (float)hy);
    glVertex2f((float)(hx + hw), (float)hy);
    glVertex2f((float)(hx + hw), (float)(hy + hh));
    glVertex2f((float)hx,        (float)(hy + hh));
    glEnd();

    /* --- Title bar --- */
    {
        int title_y = hy + hh - title_h;
        /* Title bar separator */
        glColor4f(0.20f, 0.20f, 0.20f, 1.0f);
        glBegin(GL_LINES);
        glVertex2f((float)hx,        (float)title_y);
        glVertex2f((float)(hx + hw), (float)title_y);
        glEnd();

        /* Title text — dim, left-aligned like config menu section headers */
        glColor4f(0.478f, 0.518f, 0.580f, 1.0f);
        draw_string((float)(hx + 14), (float)(title_y + 4), "HELP", FONT_SMALL);

        /* Tab switch hint right-aligned */
        const char *nav_hint = "Left/Right: switch tabs";
        int nh_x = hx + hw - (int)strlen(nav_hint) * FONT_SMALL_W - 14;
        glColor4f(0.533f, 0.533f, 0.533f, 0.70f);
        draw_string((float)nh_x, (float)(title_y + 4), nav_hint, FONT_SMALL);
    }

    /* --- Tab bar --- */
    {
        int tab_y  = hy + hh - title_h - tab_bar_h;
        int tab_w  = hw / HELP_NUM_TABS;

        /* Tab bar background */
        glColor4f(0.10f, 0.10f, 0.10f, 1.0f);
        draw_quad((float)hx, (float)tab_y, (float)hw, (float)tab_bar_h);

        for (int t = 0; t < HELP_NUM_TABS; t++) {
            int tx_tab = hx + t * tab_w;
            if (t == g_help_tab) {
                /* Active tab: bottom accent bar + bright label */
                glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, 0.85f);
                draw_quad((float)tx_tab, (float)tab_y, (float)tab_w, 2.0f);
                glColor4f(0.847f, 0.847f, 0.847f, 1.0f);
            } else {
                glColor4f(0.533f, 0.533f, 0.533f, 1.0f);
            }
            int lbl_len = (int)strlen(tab_labels[t]);
            int lbl_x   = tx_tab + (tab_w - lbl_len * FONT_SMALL_W) / 2;
            draw_string((float)lbl_x, (float)(tab_y + 3), tab_labels[t], FONT_SMALL);
        }

        /* Separator line below tab bar */
        glColor4f(0.20f, 0.20f, 0.20f, 1.0f);
        glBegin(GL_LINES);
        glVertex2f((float)hx,        (float)tab_y);
        glVertex2f((float)(hx + hw), (float)tab_y);
        glEnd();
    }

    /* --- Content --- */
    {
        int scissor_x = hx + 1;
        int scissor_y = hy + pad_bot;
        int scissor_w = hw - 2;
        int scissor_h = content_h;
        int have_scissor = (scissor_w > 0 && scissor_h > 0);

        if (scissor_w < 0) scissor_w = 0;
        if (scissor_h < 0) scissor_h = 0;

        if (have_scissor) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(scissor_x, scissor_y, scissor_w, scissor_h);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
    }
    int tx      = hx + 14;
    int ty_start = hy + hh - pad_top - LINE_H + 3;

    /* Compute tab stop from widest left column so all right columns align */
    int tab_stop = 0;
    for (int i = 0; i < n_lines; i++) {
        const char *t = strchr(text[i], '\t');
        if (t) {
            int ln = (int)(t - text[i]);
            if (ln > tab_stop) tab_stop = ln;
        }
    }

    for (int i = g_help_scroll; i < n_lines && i < g_help_scroll + visible_lines + 1; i++) {
        int ty = ty_start - (i - g_help_scroll) * LINE_H;
        if (ty < hy + pad_bot - LINE_H) break;
        if (text[i][0] == '\0') continue;

        /* '\t' marks the left/right column boundary */
        const char *tab = strchr(text[i], '\t');
        if (tab) {
            /* Left column (command / key) — #d8d8d8 */
            char left[256];
            int ln = (int)(tab - text[i]);
            if (ln > 255) ln = 255;
            memcpy(left, text[i], ln);
            left[ln] = '\0';
            glColor4f(0.847f, 0.847f, 0.847f, 1.0f);
            draw_string((float)tx, (float)ty, left, FONT_SMALL);

            /* Right column (description) — aligned to shared tab stop */
            glColor4f(0.533f, 0.533f, 0.533f, 1.0f);
            draw_string((float)(tx + tab_stop * FONT_SMALL_W), (float)ty,
                        tab + 1, FONT_SMALL);
        } else if (text[i][0] != ' ') {
            /* Section header — dim gray-blue like config menu */
            glColor4f(0.478f, 0.518f, 0.580f, 1.0f);
            draw_string((float)tx, (float)ty, text[i], FONT_SMALL);
        } else if (text[i][2] == ' ' && text[i][3] == ' ') {
            /* 4+ space indent — code example, green accent */
            glColor4f(UI_ACCENT_GREEN_R, UI_ACCENT_GREEN_G, UI_ACCENT_GREEN_B, 0.90f);
            draw_string((float)tx, (float)ty, text[i], FONT_SMALL);
        } else {
            /* 2-space indent, no split — light label colour */
            glColor4f(0.847f, 0.847f, 0.847f, 1.0f);
            draw_string((float)tx, (float)ty, text[i], FONT_SMALL);
        }
    }

    glDisable(GL_SCISSOR_TEST);

    /* Scroll indicator (only if content overflows) */
    if (n_lines > visible_lines) {
        int bar_x   = hx + hw - 8;
        int bar_top = hy + hh - pad_top;
        int bar_h   = content_h;
        float frac  = (float)visible_lines / (float)n_lines;
        float pos   = (float)g_help_scroll / (float)n_lines;
        int thumb_h = (int)(bar_h * frac);
        if (thumb_h < 12) thumb_h = 12;
        int thumb_y = bar_top - (int)(bar_h * pos) - thumb_h;

        /* Track — #333 */
        glColor4f(0.20f, 0.20f, 0.20f, 0.60f);
        draw_quad((float)bar_x, (float)(bar_top - bar_h), 4.0f, (float)bar_h);

        /* Thumb — #888 */
        glColor4f(0.533f, 0.533f, 0.533f, 0.80f);
        draw_quad((float)bar_x, (float)thumb_y, 4.0f, (float)thumb_h);

        /* Scroll hint at bottom */
        if (g_help_scroll < max_scroll) {
            char hint[32];
            snprintf(hint, sizeof(hint), "v %d more v",
                     n_lines - g_help_scroll - visible_lines);
            int hint_x = hx + (hw - (int)strlen(hint) * FONT_SMALL_W) / 2;
            glColor4f(0.533f, 0.533f, 0.533f, 0.50f);
            draw_string((float)hint_x, (float)(hy + 4), hint, FONT_SMALL);
        }
    }

    glDisable(GL_BLEND);
    end_2d();

    #undef HELP_NUM_TABS
}

/* ========================================================================= */
/* Variable slider panel (4.8)                                               */
/* ========================================================================= */

/* Compute a shared logarithmic display scale from all variable absolute values.
 * All sliders use the same scale so their handles are normalized relative to
 * each other (a var at 100 shows near the extreme, one at 0.01 still visible). */
static float var_panel_log_scale(void) {
    float max_abs = 0.1f;   /* minimum display range */
    for (int i = 0; i < g_num_predef_vars; i++) {
        float av = fabsf(g_predef_vars[i].value);
        if (av > max_abs) max_abs = av;
    }
    return max_abs * 1.25f; /* 25% headroom so handle doesn't hug the edge */
}

/* Map a value to slider t in [0,1] using a symmetric asinh (log-like) scale.
 * The "knee" epsilon = 5% of scale is the linear region; beyond that the
 * response is logarithmic.  Zero always maps to 0.5 (centre). */
static float val_to_slider_t(float val, float scale) {
    float eps  = scale * 0.05f;               /* linear knee */
    float norm = asinhf(scale / eps);          /* ≈ asinh(20) ≈ 4.0 – fixed for scale */
    float t    = 0.5f + 0.5f * asinhf(val / eps) / norm;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

#define VAR_PANEL_W   200
#define VAR_PANEL_PAD   6
#define VAR_TITLE_H    20
#define VAR_ROW_H      20
#define VAR_PANEL_BASE_Y 8
/* Extra gap above REPLAY_HUD_BOTTOM_Y while replay HUD is active. */
#define VAR_REPLAY_CLEARANCE 10

static float g_var_panel_replay_lift_px = 0.0f;
static float g_var_panel_lift_update_time = -1.0f;
static float g_var_panel_lift_update_target = -1.0f;

static float var_panel_replay_target_lift_px(void) {
    float target = (float)((REPLAY_HUD_BOTTOM_Y + VAR_REPLAY_CLEARANCE)
                         - VAR_PANEL_BASE_Y);
    if (target < 0.0f) target = 0.0f;
    return target;
}

static float var_panel_replay_lift(void) {
    float target = 0.0f;
    if (g_replay_active)
        target = var_panel_replay_target_lift_px();

    if (g_var_panel_lift_update_time == g_anim_time &&
        g_var_panel_lift_update_target == target)
        return g_var_panel_replay_lift_px;
    g_var_panel_lift_update_time = g_anim_time;
    g_var_panel_lift_update_target = target;

    /* Exponential-decay style easing toward target (and back to 0 when replay ends). */
    g_var_panel_replay_lift_px += (target - g_var_panel_replay_lift_px) * 0.22f;
    if (fabsf(target - g_var_panel_replay_lift_px) < 0.25f)
        g_var_panel_replay_lift_px = target;

    return g_var_panel_replay_lift_px;
}

/* Geometry in render coords (y=0 at bottom). */
void var_panel_rect(int *px, int *py, int *pw, int *ph) {
    int sc_x, sc_y, sc_w, sc_h;
    int panel_w, panel_h, panel_x, panel_y;
    int min_y, max_y;

    scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
    panel_w = VAR_PANEL_W;
    panel_h = VAR_TITLE_H + g_num_predef_vars * VAR_ROW_H + 2 * VAR_PANEL_PAD;
    panel_x = sc_x + sc_w - panel_w - 8;
    if (panel_x < sc_x + 4) panel_x = sc_x + 4;

    panel_y = sc_y + VAR_PANEL_BASE_Y + STATUSBAR_H
            + (int)lroundf(var_panel_replay_lift());
    min_y = sc_y + STATUSBAR_H + 4;
    max_y = sc_y + sc_h - panel_h - 4;
    if (max_y >= min_y) {
        if (panel_y < min_y) panel_y = min_y;
        if (panel_y > max_y) panel_y = max_y;
    } else {
        panel_y = code_panel_layout_mode() == CODE_PANEL_LAYOUT_TOP
                ? sc_y + sc_h - panel_h - 4
                : min_y;
    }

    if (px) *px = panel_x;
    if (py) *py = panel_y;
    if (pw) *pw = panel_w;
    if (ph) *ph = panel_h;
}

/* Return 1 if GLUT screen coord (gx, gy) is in the panel; sets *out_row. */
int var_panel_hit(int gx, int gy, int *out_row) {
    int px, py, pw, ph;
    var_panel_rect(&px, &py, &pw, &ph);
    int ry = g_win_h - gy;
    if (gx < px || gx >= px + pw || ry < py || ry >= py + ph) return 0;
    int inner_top = py + ph - VAR_PANEL_PAD - VAR_TITLE_H;
    int row = (inner_top - ry) / VAR_ROW_H;
    if (row < 0 || row >= g_num_predef_vars) return 0;
    if (out_row) *out_row = row;
    return 1;
}

/* Amber status/error strip along the bottom of the scene panel.  The scene
 * is much wider than the code-panel statusbar slot, so long diagnostics
 * (~80 chars) fit here without truncation. */
void render_scene_status(void) {
    if (g_status_ttl <= 0 || !g_status[0]) return;

    int sc_x, sc_y, sc_w, sc_h;
    scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
    if (sc_w <= 0 || sc_h <= 0) return;

    int bar_h = STATUSBAR_H;
    int bar_y = sc_y;

    float alpha = g_status_ttl > 60 ? 1.0f : (float)g_status_ttl / 60.0f;

    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Design ref: amber error banner — bg #3a2a10, top rule #1a1208, text
     * #f0c070, "!" in bordered circle. */
    glColor4f(0.227f, 0.165f, 0.063f, 0.92f * alpha);
    draw_quad((float)sc_x, (float)bar_y, (float)sc_w, (float)bar_h);
    glColor4f(0.102f, 0.071f, 0.031f, alpha);
    glBegin(GL_LINES);
    glVertex2f((float)sc_x,          (float)(bar_y + bar_h));
    glVertex2f((float)(sc_x + sc_w), (float)(bar_y + bar_h));
    glEnd();

    int text_y = bar_y + (bar_h - FONT_SMALL_H) / 2 + 1;

    /* Bordered "!" badge on the left */
    int badge_d = 14;
    int badge_x = sc_x + CODE_MARGIN_X;
    int badge_y = bar_y + (bar_h - badge_d) / 2;
    glColor4f(0.941f, 0.753f, 0.439f, alpha);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < 16; i++) {
        float a = (float)i * (6.2831853f / 16.0f);
        glVertex2f(badge_x + badge_d * 0.5f + cosf(a) * (badge_d * 0.5f),
                   badge_y + badge_d * 0.5f + sinf(a) * (badge_d * 0.5f));
    }
    glEnd();
    draw_string((float)(badge_x + badge_d / 2 - FONT_SMALL_W / 2 + 1),
                (float)text_y, "!", FONT_SMALL);

    int tx = badge_x + badge_d + 8;
    int max_px = sc_x + sc_w - CODE_MARGIN_X - tx;
    int max_chars = max_px / FONT_SMALL_W;
    if (max_chars < 8) max_chars = 8;
    if (max_chars > 255) max_chars = 255;

    char msg[256];
    int n = (int)strlen(g_status);
    if (n > max_chars) {
        snprintf(msg, sizeof(msg), "%.*s...", max_chars - 3, g_status);
    } else {
        snprintf(msg, sizeof(msg), "%s", g_status);
    }
    glColor4f(0.941f, 0.753f, 0.439f, alpha); /* #f0c070 */
    draw_string((float)tx, (float)text_y, msg, FONT_SMALL);

    glDisable(GL_BLEND);
    end_2d();
}

void render_var_panel(void) {
    if (!g_show_var_panel) return;

    int px, py, pw, ph;
    var_panel_rect(&px, &py, &pw, &ph);

    begin_2d();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Background */
    glColor4f(0.05f, 0.05f, 0.10f, 0.88f);
    draw_quad((float)px, (float)py, (float)pw, (float)ph);

    /* Border */
    glColor4f(0.40f, 0.40f, 0.70f, 0.75f);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)px,        (float)py);
    glVertex2f((float)(px + pw), (float)py);
    glVertex2f((float)(px + pw), (float)(py + ph));
    glVertex2f((float)px,        (float)(py + ph));
    glEnd();

    /* Title */
    glColor3f(0.75f, 0.75f, 1.00f);
    draw_string((float)(px + 6),
                (float)(py + ph - VAR_PANEL_PAD - 4),
                "Variables (declared)", FONT_SMALL);

    /* Column offsets within the panel — sized for multi-char var names */
    int max_name_len = 1;
    for (int i = 0; i < g_num_predef_vars; i++) {
        int len = (int)strlen(g_predef_vars[i].name);
        if (len > max_name_len) max_name_len = len;
    }
    int label_w  = max_name_len * FONT_SMALL_W + FONT_SMALL_W;
    if (label_w > pw / 3) label_w = pw / 3;
    int label_x  = px + 6;
    int val_x    = px + 6 + label_w;
    int track_x  = val_x + 66;
    int track_w  = pw - (track_x - px) - 8;
    int handle_w = 10;
    if (track_w < handle_w + 4) track_w = handle_w + 4;

    /* Shared logarithmic scale: all handles normalized relative to each other. */
    float log_scale = var_panel_log_scale();

    int inner_top = py + ph - VAR_PANEL_PAD - VAR_TITLE_H;

    for (int i = 0; i < g_num_predef_vars; i++) {
        int row_y  = inner_top - (i + 1) * VAR_ROW_H;
        int text_y = row_y + 4;
        float val  = g_predef_vars[i].value;

        /* Drag highlight — amber tint for log mode, blue for linear */
        if (g_drag_var == i) {
            if (g_drag_log_mode)
                glColor4f(0.30f, 0.20f, 0.05f, 0.60f);
            else
                glColor4f(0.20f, 0.20f, 0.40f, 0.60f);
            draw_quad((float)(px + 1), (float)row_y,
                      (float)(pw - 2), (float)VAR_ROW_H);
        }

        /* Label */
        glColor3f(0.70f, 0.85f, 0.70f);
        draw_string((float)label_x, (float)text_y,
                    g_predef_vars[i].name, FONT_SMALL);

        /* Value */
        char valstr[16]; snprintf(valstr, sizeof(valstr), "%7.3f", (double)val);
        glColor3f(0.90f, 0.90f, 0.60f);
        draw_string((float)val_x, (float)text_y, valstr, FONT_SMALL);

        /* Slider track */
        glColor4f(0.18f, 0.18f, 0.28f, 0.90f);
        draw_quad((float)track_x, (float)(row_y + 6),
                  (float)track_w, (float)(VAR_ROW_H - 12));

        /* Centre tick (marks zero on the log scale) */
        float cx = (float)track_x + (float)track_w * 0.5f;
        glColor4f(0.35f, 0.35f, 0.50f, 0.70f);
        glBegin(GL_LINES);
        glVertex2f(cx, (float)(row_y + 5));
        glVertex2f(cx, (float)(row_y + VAR_ROW_H - 5));
        glEnd();

        /* Handle — position computed via shared log-normalized scale.
         * Yellow = linear drag, orange = log drag, blue = idle. */
        float t  = val_to_slider_t(val, log_scale);
        float hx = (float)track_x + t * (float)(track_w - handle_w);
        if (g_drag_var == i) {
            if (g_drag_log_mode)
                glColor4f(1.00f, 0.55f, 0.10f, 0.95f);  /* orange: log mode */
            else
                glColor4f(1.00f, 0.80f, 0.20f, 0.95f);  /* yellow: linear mode */
        } else {
            glColor4f(0.55f, 0.70f, 1.00f, 0.90f);      /* blue: idle */
        }
        draw_quad(hx, (float)(row_y + 4),
                  (float)handle_w, (float)(VAR_ROW_H - 8));
    }

    glDisable(GL_BLEND);
    end_2d();
}

/* ========================================================================= */
/* Configuration menu — now hosted inside the MENU_CONFIG dropdown            */
/* ========================================================================= */

int ui_panels_handle_right_press(int mx, int my) {
    if (g_open_menu != MENU_CONFIG) return 0;
    int item = menu_dropdown_item_hit(mx, my);
    if (item < 0) return 0;
    repl_cfg_cycle_row(item, -1);
    return 1;
}

void ui_panels_close_menus(void) {
    g_open_menu = -1;
    g_menu_item_hover = -1;
    color_picker_close();
}

/* ========================================================================= */
/* Inline scene rename                                                        */
/* ========================================================================= */

int ui_panels_rename_active(void) {
    return g_rename_slot >= 0;
}

int ui_panels_begin_rename(int slot) {
    if (slot < 0 || slot >= MAX_USER_SCENES) return 0;
    if (!repl_user_scene_slot_used(slot))    return 0;
    g_rename_slot = slot;
    const char *cur = repl_user_scene_name(slot);
    snprintf(g_rename_buf, sizeof(g_rename_buf), "%s", cur ? cur : "");
    g_rename_len = (int)strlen(g_rename_buf);
    rename_refresh_status();
    return 1;
}

void ui_panels_cancel_rename(void) {
    g_rename_slot = -1;
    g_rename_buf[0] = '\0';
    g_rename_len = 0;
}

static int rename_char_ok(unsigned char c) {
    if (c < 32 || c >= 127) return 0;
    if (c == '/' || c == '\\' || c == ':') return 0;
    return 1;
}

int ui_panels_handle_rename_key(unsigned char key) {
    if (g_rename_slot < 0) return 0;

    if (key == KEY_ESC) {
        ui_panels_cancel_rename();
        return 1;
    }
    if (key == '\r' || key == '\n') {
        /* repl_user_scene_rename trims whitespace and rejects empty
         * names at the API boundary.  A 0 return means reject-and-retry
         * (keep the overlay open); non-zero means success and we close. */
        if (!repl_user_scene_rename(g_rename_slot, g_rename_buf)) {
            set_status("Scene name cannot be empty");
            return 1;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "Renamed to: %s",
                 repl_user_scene_name(g_rename_slot));
        set_status(msg);
        ui_panels_cancel_rename();
        return 1;
    }
    if (key == KEY_BACKSPACE || key == KEY_DELETE) {
        if (g_rename_len > 0) {
            g_rename_buf[--g_rename_len] = '\0';
            rename_refresh_status();
        }
        return 1;
    }
    if (rename_char_ok(key) && g_rename_len < (int)sizeof(g_rename_buf) - 1) {
        g_rename_buf[g_rename_len++] = (char)key;
        g_rename_buf[g_rename_len] = '\0';
        rename_refresh_status();
        return 1;
    }
    /* Swallow everything else so no stray character hits the editor. */
    return 1;
}

int ui_panels_handle_rename_special(int key) {
    if (g_rename_slot < 0) return 0;
    (void)key;
    return 1;  /* swallow all specials while renaming */
}

void ui_panels_open_config(void) {
    if (g_open_menu == MENU_CONFIG) {
        g_open_menu = -1;
        return;
    }
    g_open_menu = MENU_CONFIG;
    g_menu_open_time = g_anim_time;
    g_menu_item_hover = -1;
}

/* ========================================================================= */
/* Code panel click handler                                                   */
/* ========================================================================= */

static int g_code_panel_drag_active = 0;
static int g_code_panel_drag_anchor = -1;
static int g_code_panel_drag_moved = 0;

static int code_panel_hit_test(int mx, int my,
                               int *out_target,
                               int *out_on_insert_line,
                               int *out_row_offset) {
    int cp_x, cp_y, cp_w, cp_h;
    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (cp_w <= 0 || cp_h <= 0) return 0;
    int panel_w = cp_w;
    int panel_top = cp_y + cp_h;
    /* Convert GLUT Y (top=0) to OpenGL Y (bottom=0) */
    int gl_y = g_win_h - my;
    if (mx < cp_x || mx >= cp_x + cp_w) return 0;
    if (gl_y < cp_y || gl_y >= cp_y + cp_h) return 0;

    /* Same layout constants as render_code_panel */
    int line_y_start = panel_top - CODE_MARGIN_Y - 2 * LINE_H;
    int vis = (line_y_start + LINE_H - 3 - gl_y) / LINE_H;
    if (vis < 0) return 0;   /* clicked in header */
    if (vis >= code_panel_visible_lines_for_height(cp_h)) return 0;

    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int doc_line = g_scroll + vis;
    int target;
    int on_insert_line;
    int row_offset;

    if (!code_panel_target_for_doc_line(doc_line, panel_w, text_x,
                                        &target, &on_insert_line,
                                        &row_offset))
        return 0;

    if (out_target) *out_target = target;
    if (out_on_insert_line) *out_on_insert_line = on_insert_line;
    if (out_row_offset) *out_row_offset = row_offset;
    return 1;
}

static int code_panel_drag_target(int mx, int my, int *out_target) {
    (void)mx;
    int cp_x, cp_y, cp_w, cp_h;
    code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (cp_w <= 0 || cp_h <= 0) return 0;
    int panel_w = cp_w;
    int panel_top = cp_y + cp_h;
    int gl_y = g_win_h - my;
    int line_y_start = panel_top - CODE_MARGIN_Y - 2 * LINE_H;
    int vis = (line_y_start + LINE_H - 3 - gl_y) / LINE_H;

    int visible_lines = code_panel_visible_lines_for_height(cp_h);
    if (vis < 0) vis = 0;
    if (vis >= visible_lines) vis = visible_lines - 1;

    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int doc_line = g_scroll + vis;
    int target;
    int on_insert_line;

    if (!code_panel_target_for_doc_line(doc_line, panel_w, text_x,
                                        &target, &on_insert_line, NULL))
        return 0;

    if (on_insert_line) {
        if (g_edit_line < g_num_cmds)
            target = g_edit_line;
        else if (g_num_cmds > 0)
            target = g_num_cmds - 1;
        else
            return 0;
    } else if (target >= g_num_cmds) {
        target = g_num_cmds - 1;
    }

    if (target < 0) target = 0;
    if (target >= g_num_cmds) target = g_num_cmds - 1;
    if (out_target) *out_target = target;
    return g_num_cmds > 0;
}

/* Handle left-click in the code panel: navigate to line + column */
void handle_code_panel_click(int mx, int my) {
    int target, on_insert_line, row_offset;
    if (!code_panel_hit_test(mx, my, &target, &on_insert_line, &row_offset)) return;

    if (!on_insert_line) {
        if (target < 0) target = 0;
        if (target > g_num_cmds) target = g_num_cmds;
        navigate_to_line(target);
    }

    int cp_w;
    code_panel_rect(NULL, NULL, &cp_w, NULL);
    int panel_w = cp_w;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = g_show_indices ? (6 * FONT_W) : 0;
    int text_x = CODE_MARGIN_X + linenum_w + FONT_W + idx_col_w;
    int indent_chars = code_panel_active_indent_chars();
    int seg_start = g_input_len;
    int seg_len = 0;
    int seg_x = text_x + indent_chars * FONT_W;
    int col;

    code_panel_segment_for_row(g_input, seg_x, panel_w, row_offset,
                               &seg_start, &seg_len, &seg_x);

    col = (mx - seg_x + FONT_W / 2) / FONT_W;
    if (col < 0) col = 0;
    if (col > seg_len) col = seg_len;
    g_cursor_pos = seg_start + col;
    if (g_cursor_pos > g_input_len) g_cursor_pos = g_input_len;

    g_cursor_on = 1;
    g_blink_tick = 0;
    clear_autocomplete_state();
    clear_selection();
}

int handle_code_panel_press(int mx, int my) {
    int actions = UI_PANEL_PRESS_NONE;

    /* Color picker floats and may overlap the code panel (e.g. top/bottom
     * layouts).  Give it first crack so its hit rects take priority. */
    if (color_picker_press(mx, my))
        return UI_PANEL_PRESS_CONSUMED;

    /* Pins (Search, Replay) take priority over menu labels and dropdown items
     * so they remain clickable even when a menu label visually overlaps them
     * in a narrow window — matches the render order (pins drawn on top). */
    int pin = menubar_pin_hit(mx, my);
    if (pin >= 0) {
        if (g_open_menu >= 0) g_open_menu = -1;
        switch (pin) {
        case PIN_REPLAY:
            /* Button mirrors its glyph: pause when playing, resume when
             * paused, (re)start when stopped or done. */
            if (g_replay_state == REPLAY_PLAYING) {
                g_replay_state = REPLAY_PAUSED;
            } else if (g_replay_state == REPLAY_PAUSED) {
                g_replay_state = REPLAY_PLAYING;
            } else {
                replay_start();
            }
            break;
        case PIN_SEARCH:
            handle_search_key(KEY_CTRL_F);
            g_search_open_time = g_anim_time;
            break;
        }
        return UI_PANEL_PRESS_CONSUMED;
    }

    /* Menu dropdown (floats over code) */
    if (g_open_menu >= 0) {
        /* Clicking the same top-level menu toggles closed; clicking another
         * switches to it. */
        int over_menu = menubar_menu_hit(mx, my);
        if (over_menu >= 0) {
            if (over_menu == g_open_menu) {
                g_open_menu = -1;
            } else {
                g_open_menu = over_menu;
                g_menu_open_time = g_anim_time;
            }
            return UI_PANEL_PRESS_CONSUMED;
        }
        int item = menu_dropdown_item_hit(mx, my);
        if (item >= 0) {
            int close = menu_item_activate(g_open_menu, item);
            if (close) g_open_menu = -1;
            return UI_PANEL_PRESS_CONSUMED;
        }
        /* Click outside dropdown: dismiss, fall through for code nav */
        g_open_menu = -1;
    }

    int menu = menubar_menu_hit(mx, my);
    if (menu >= 0) {
        g_open_menu = menu;
        g_menu_open_time = g_anim_time;
        return UI_PANEL_PRESS_CONSUMED;
    }

    int target, on_insert_line, row_offset;
    if (!code_panel_hit_test(mx, my, &target, &on_insert_line, &row_offset))
        return UI_PANEL_PRESS_NONE;

    /* Check for swatch click on a color line */
    if (!on_insert_line && row_offset == 0 && target >= 0 && target < g_num_cmds) {
        CmdType ct = g_cmds[target].type;
        if ((ct == CMD_COLOR3F || ct == CMD_COLOR4F || ct == CMD_TESS_COLOR ||
             ct == CMD_CLEAR_COLOR) &&
            g_cmds[target].valid && !g_cmds[target].has_vars) {
            int cp_x2, cp_w2;
            code_panel_rect(&cp_x2, NULL, &cp_w2, NULL);
            int sx = cp_x2 + cp_w2 - CODE_MARGIN_X - SWATCH_W - 2;
            if (mx >= sx && mx < sx + SWATCH_W) {
                if (g_cp_line == target) {
                    g_cp_line = -1;   /* toggle: close picker */
                } else {
                    actions |= UI_PANEL_PRESS_OPENED_COLOR_PICKER;
                    color_picker_open(target, my);
                }
                glutPostRedisplay();
                return actions | UI_PANEL_PRESS_CONSUMED;
            }
        }
    }
    /* Any non-swatch code-panel click closes the picker */
    if (g_cp_line >= 0) {
        g_cp_line = -1;
        g_cp_drag = 0;
    }

    handle_code_panel_click(mx, my);

    g_code_panel_drag_active = 0;
    g_code_panel_drag_anchor = -1;
    g_code_panel_drag_moved = 0;
    if (!on_insert_line && target >= 0 && target < g_num_cmds) {
        g_code_panel_drag_active = 1;
        g_code_panel_drag_anchor = target;
    }
    return actions | UI_PANEL_PRESS_CONSUMED;
}

int handle_code_panel_drag(int mx, int my) {
    int target;
    if (!g_code_panel_drag_active || g_code_panel_drag_anchor < 0) return 0;
    if (!code_panel_drag_target(mx, my, &target)) return 0;

    if (target != g_code_panel_drag_anchor || g_code_panel_drag_moved) {
        g_code_panel_drag_moved = 1;
        repl_selection_start(g_code_panel_drag_anchor);
        repl_selection_set_end(target);
        navigate_to_line(target);
        g_cursor_on = 1;
        g_blink_tick = 0;
    }
    return 1;
}

void handle_code_panel_release(void) {
    g_code_panel_drag_active = 0;
    g_code_panel_drag_anchor = -1;
    g_code_panel_drag_moved = 0;
}

int ui_panels_handle_escape(void) {
    return color_picker_close();
}

int ui_panels_handle_scene_press(int mx, int my) {
    return color_picker_press(mx, my);
}

int ui_panels_handle_motion(int mx, int my) {
    return color_picker_motion(mx, my);
}

void ui_panels_handle_mouse_release(void) {
    color_picker_release();
    handle_code_panel_release();
}
