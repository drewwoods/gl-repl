/*
 * src/repl/eval.c - Expression evaluator, REPL<->C translators, for-loop parsers.
 *
 * Responsibilities:
 *  - Recursive-descent float evaluator for REPL expressions with support for
 *    variables (predef table `g_predef_vars` plus per-call loop locals),
 *    arithmetic, comparison, and logical operators, and the built-in math
 *    funcs listed in s_reserved_idents[] below.
 *  - Bidirectional text translation between REPL syntax (sin, PI, a%b) and
 *    C syntax (sinf, M_PI, fmodf(a,b)) for save/load round-tripping.
 *  - Header parsing for REPL `for(var, start, end[, step])` and imported
 *    C-style `for (float i = 0; i < N; i++)` loops.
 *
 * Intentionally isolated from GL and from the command/editor layers so the
 * evaluator has its own unit tests (see tests/test_eval.c).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "repl/eval.h"

#include <stdarg.h>

/* ========================================================================= */
/* Predefined variables                                                       */
/* ========================================================================= */

static ExprVar g_fallback_predef_vars[MAX_PREDEF_VARS];
static int     g_fallback_num_predef_vars = 0;
static ExprVar *g_active_predef_vars = g_fallback_predef_vars;
static int     *g_active_num_predef_vars = &g_fallback_num_predef_vars;
static float   g_fallback_scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
static float   (*g_active_scratch_arrays)[REPL_SCRATCH_ARRAY_LEN] =
    g_fallback_scratch_arrays;
static char    g_fallback_func_aliases[REPL_FUNC_SLOT_COUNT][REPL_FUNC_NAME_MAX];
static char    (*g_active_func_aliases)[REPL_FUNC_NAME_MAX] =
    g_fallback_func_aliases;

void repl_eval_bind_predef_storage(ExprVar *vars, int *count_ptr) {
    if (vars && count_ptr) {
        g_active_predef_vars = vars;
        g_active_num_predef_vars = count_ptr;
    } else {
        g_active_predef_vars = g_fallback_predef_vars;
        g_active_num_predef_vars = &g_fallback_num_predef_vars;
    }
}

ExprVar *repl_eval_predef_vars_mut(void) {
    return g_active_predef_vars;
}

int *repl_eval_predef_count_mut(void) {
    return g_active_num_predef_vars;
}

const ExprVar *repl_eval_predef_vars(void) {
    return g_active_predef_vars;
}

int repl_eval_predef_count(void) {
    return *g_active_num_predef_vars;
}

ReplPredefView repl_eval_predef_view(void) {
    return (ReplPredefView){
        .vars  = repl_eval_predef_vars(),
        .count = repl_eval_predef_count(),
    };
}

void repl_eval_bind_scratch_storage(
    float arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]) {
    g_active_scratch_arrays = arrays ? arrays : g_fallback_scratch_arrays;
}

void repl_func_alias_bind_storage(
    char arrays[REPL_FUNC_SLOT_COUNT][REPL_FUNC_NAME_MAX]) {
    g_active_func_aliases = arrays ? arrays : g_fallback_func_aliases;
}

void repl_func_alias_clear_all(void) {
    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT; slot++)
        g_active_func_aliases[slot][0] = '\0';
}

const char *repl_func_alias_get(int slot) {
    if (slot < 0 || slot >= REPL_FUNC_SLOT_COUNT) return NULL;
    return g_active_func_aliases[slot][0] ? g_active_func_aliases[slot] : NULL;
}

int repl_func_alias_lookup_slot(const char *name) {
    if (!name || !*name) return -1;
    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT; slot++) {
        if (g_active_func_aliases[slot][0] &&
            strcmp(g_active_func_aliases[slot], name) == 0)
            return slot;
    }
    return -1;
}

ReplFuncAliasView repl_func_alias_view(void) {
    ReplFuncAliasView view = { g_active_func_aliases, REPL_FUNC_SLOT_COUNT };
    return view;
}

int repl_func_alias_name_is_valid(const char *name) {
    if (!name || !*name) return 0;
    /* Must be a C identifier. */
    if (!repl_eval_is_ident_start((unsigned char)name[0])) return 0;
    int len = 0;
    for (const char *p = name; *p; p++, len++) {
        if (!repl_eval_is_ident_continue((unsigned char)*p)) return 0;
    }
    if (len >= REPL_FUNC_NAME_MAX) return 0;
    /* Reject the bare slot names - those are the underlying form. */
    if (len == 5 && strncmp(name, "func", 4) == 0 &&
        name[4] >= '0' && name[4] <= '9')
        return 0;
    /* Reject the language-reserved set (predef vars `t`, constants
     * PI/TAU, math funcs sin/cos/..., scratch A/B/C, type keywords). */
    if (repl_eval_is_reserved_ident(name)) return 0;
    /* Reject control-flow keywords. These have their own commit
     * handlers (editor_try_commit_for_loop, editor_try_commit_if_block) and would
     * otherwise hijack their syntax if a user typed `if(...)` after
     * accidentally declaring an alias by the same name. */
    static const char *const control_flow_kw[] = {
        "if", "for", "goto", "else", NULL
    };
    for (const char *const *kw = control_flow_kw; *kw; kw++)
        if (strcmp(name, *kw) == 0) return 0;
    /* Reject if a predef var with this name is registered (would
     * shadow an existing scalar). Inverse check (predef declare
     * after alias) is enforced by declare_predef_var via the
     * reserved-ident check + this collision-free guarantee. */
    if (repl_eval_find_predef_var_idx(name) >= 0) return 0;
    return 1;
}

int repl_func_alias_first_free_slot(void) {
    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT; slot++) {
        if (!g_active_func_aliases[slot][0])
            return slot;
    }
    return -1;
}

int repl_func_alias_set(int slot, const char *name) {
    if (slot < 0 || slot >= REPL_FUNC_SLOT_COUNT) return 0;
    if (!name || !*name) {
        g_active_func_aliases[slot][0] = '\0';
        return 1;
    }
    if (!repl_func_alias_name_is_valid(name)) return 0;
    int existing = repl_func_alias_lookup_slot(name);
    if (existing >= 0 && existing != slot) return 0;
    snprintf(g_active_func_aliases[slot], REPL_FUNC_NAME_MAX, "%s", name);
    return 1;
}

void repl_eval_reset_scratch_arrays(void) {
    memset(g_active_scratch_arrays, 0, sizeof(g_fallback_scratch_arrays));
}

int repl_eval_scratch_array_index(const char *name) {
    if (!name)
        return -1;
    if (strcmp(name, "A") == 0)
        return 0;
    if (strcmp(name, "B") == 0)
        return 1;
    if (strcmp(name, "C") == 0)
        return 2;
    return -1;
}

/* A/B/C scratch arrays are single uppercase letters; map the char
 * directly instead of rebuilding a 1-char string at each call site. */
static int scratch_char_index(char c) {
    char nm[2] = { c, '\0' };
    return repl_eval_scratch_array_index(nm);
}

static int scratch_elem_in_range(int elem_idx) {
    return elem_idx >= 0 && elem_idx < REPL_SCRATCH_ARRAY_LEN;
}

int repl_eval_scratch_get(int array_idx, int elem_idx, float *out) {
    if (array_idx < 0 || array_idx >= REPL_SCRATCH_ARRAY_COUNT ||
        !scratch_elem_in_range(elem_idx) || !out)
        return 0;
    *out = g_active_scratch_arrays[array_idx][elem_idx];
    return 1;
}

int repl_eval_scratch_set(int array_idx, int elem_idx, float value) {
    if (array_idx < 0 || array_idx >= REPL_SCRATCH_ARRAY_COUNT ||
        !scratch_elem_in_range(elem_idx))
        return 0;
    g_active_scratch_arrays[array_idx][elem_idx] = value;
    return 1;
}

void repl_eval_copy_scratch_arrays(
    float dst[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]) {
    if (!dst)
        return;
    memcpy(dst, g_active_scratch_arrays, sizeof(g_fallback_scratch_arrays));
}

void repl_eval_restore_scratch_arrays(
    const float src[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN]) {
    if (!src)
        return;
    memcpy(g_active_scratch_arrays, src, sizeof(g_fallback_scratch_arrays));
}

void repl_eval_copy_predef_vars(float dst_vals[MAX_PREDEF_VARS],
                                char dst_names[MAX_PREDEF_VARS][REPL_PREDEF_NAME_MAX],
                                int *dst_count) {
    if (dst_count)
        *dst_count = g_num_predef_vars;
    for (int i = 0; i < g_num_predef_vars; i++) {
        if (dst_vals)
            dst_vals[i] = g_predef_vars[i].value;
        if (dst_names)
            memcpy(dst_names[i], g_predef_vars[i].name, REPL_PREDEF_NAME_MAX);
    }
}

void repl_eval_restore_predef_vars(const float src_vals[MAX_PREDEF_VARS],
                                   const char src_names[MAX_PREDEF_VARS][REPL_PREDEF_NAME_MAX],
                                   int src_count) {
    g_num_predef_vars_mut = src_count;
    for (int i = 0; i < src_count; i++) {
        if (src_vals)
            g_predef_vars_mut[i].value = src_vals[i];
        if (src_names)
            memcpy(g_predef_vars_mut[i].name, src_names[i], REPL_PREDEF_NAME_MAX);
    }
}

void repl_eval_capture_predef_snapshot(ReplPredefSnapshot *dst) {
    if (!dst)
        return;
    memset(dst, 0, sizeof(*dst));
    repl_eval_copy_predef_vars(dst->vals, dst->names, &dst->count);
}

/* Non-destructive restore: assign saved values to the live predef
 * slots whose names match the snapshot. The live table shape
 * (count, names, slot order) is UNCHANGED. Saved names that no
 * longer exist in the live table are silently dropped; live names
 * not present in the snapshot keep their current values.
 *
 * Used by the fade-batch render path (#3 bug fix) - that path wants
 * to revert values to the replay-start baseline before each batch,
 * but it sits inside a frame-level values-only save/restore that
 * doesn't carry a count. Reshaping the live table inside the frame
 * would leave the frame-end restore unable to repopulate slots the
 * fade restore dropped. The by-name variant keeps live state's
 * shape intact while still recovering the user's data even when a
 * mid-replay workspace switch / scene load reshaped the predef
 * table relative to replay_start. */
void repl_eval_restore_predef_values_by_name(
    const float src_vals[MAX_PREDEF_VARS],
    const char src_names[MAX_PREDEF_VARS][REPL_PREDEF_NAME_MAX],
    int src_count) {
    if (!src_vals || !src_names) return;
    for (int i = 0; i < src_count; i++) {
        const char *name = src_names[i];
        if (!name[0]) continue;
        int idx = repl_eval_find_predef_var_idx(name);
        if (idx >= 0)
            g_predef_vars_mut[idx].value = src_vals[i];
    }
}

void repl_eval_restore_predef_values_by_snapshot(const ReplPredefSnapshot *src) {
    if (!src)
        return;
    repl_eval_restore_predef_values_by_name(src->vals, src->names, src->count);
}

/* Values-only snapshot of the live predef table, used by the controller's
 * per-frame baseline-save and by the replay peer's start/stop bracketing.
 * Names + count are NOT preserved here - callers that need the full table
 * use repl_eval_copy_predef_vars / _restore_predef_vars above. */
void repl_copy_predef_values(float *dst, int max_vals) {
    int n;

    if (!dst || max_vals <= 0)
        return;

    n = g_num_predef_vars < max_vals ? g_num_predef_vars : max_vals;
    for (int i = 0; i < n; i++)
        dst[i] = g_predef_vars[i].value;
}

void repl_restore_predef_values(const float *src, int max_vals) {
    int n;

    if (!src || max_vals <= 0)
        return;

    n = g_num_predef_vars < max_vals ? g_num_predef_vars : max_vals;
    for (int i = 0; i < n; i++)
        g_predef_vars_mut[i].value = src[i];
}

static const char *skip_numeric_literal(const char *s) {
    char *end = NULL;
    (void)strtof(s, &end);
    if (end == s)
        return s;
    if (*end == 'f' || *end == 'F')
        end++;
    return end;
}

const char *repl_eval_eat_identifier(const char *p, const char **out_start) {
    if (!p || !repl_eval_is_ident_start((unsigned char)*p))
        return NULL;
    if (out_start)
        *out_start = p;
    do {
        p++;
    } while (*p && repl_eval_is_ident_continue((unsigned char)*p));
    return p;
}

typedef float (*ExprBuiltinFn)(const float *args);

typedef struct {
    const char    *name;
    const char    *c_name;
    int            arity_min;
    int            arity_max;
    ExprBuiltinFn  eval;
} ExprBuiltin;

static float expr_rand01(float seed, float iter);
static float expr_rand_signed(float seed, float iter);

static float builtin_sin(const float *args)   { return sinf(args[0]); }
static float builtin_cos(const float *args)   { return cosf(args[0]); }
static float builtin_tan(const float *args)   { return tanf(args[0]); }
/* asin/acos clamp their argument the way sqrt absolute-values its own: the
 * evaluator stays total, so a value that drifts a hair outside [-1, 1]
 * during live editing yields +-PI/2 (or 0 / PI) instead of NaN. */
static float builtin_asin(const float *args)  {
    float x = args[0] < -1.0f ? -1.0f : (args[0] > 1.0f ? 1.0f : args[0]);
    return asinf(x);
}
static float builtin_acos(const float *args)  {
    float x = args[0] < -1.0f ? -1.0f : (args[0] > 1.0f ? 1.0f : args[0]);
    return acosf(x);
}
static float builtin_atan(const float *args)  { return atanf(args[0]); }
static float builtin_atan2(const float *args) { return atan2f(args[0], args[1]); }
static float builtin_sqrt(const float *args)  { return sqrtf(fabsf(args[0])); }
static float builtin_abs(const float *args)   { return fabsf(args[0]); }
static float builtin_pow(const float *args)   { return powf(args[0], args[1]); }
static float builtin_min(const float *args)   { return args[0] < args[1] ? args[0] : args[1]; }
static float builtin_max(const float *args)   { return args[0] > args[1] ? args[0] : args[1]; }
static float builtin_floor(const float *args) { return floorf(args[0]); }
static float builtin_ceil(const float *args)  { return ceilf(args[0]); }
static float builtin_round(const float *args) { return roundf(args[0]); }
static float builtin_fmod(const float *args)  { return fmodf(args[0], args[1]); }
static float builtin_rem(const float *args)   { return remainderf(args[0], args[1]); }
/* Shaping helpers. These have no libm twin, so the exporter emits matching
 * repl_*f helpers (write_shape_helpers in export_prologue.c) - keep the two
 * bodies identical or a scene diverges between the REPL and its export. */
static float builtin_clamp(const float *args) {
    /* Bounds crossed (hi < lo) is a user typo, not a mode: lo wins, which is
     * what the naive two-compare form does. */
    if (args[0] < args[1]) return args[1];
    if (args[0] > args[2]) return args[2];
    return args[0];
}
static float builtin_lerp(const float *args)  {
    /* Deliberately unclamped: s outside [0, 1] extrapolates, which is how
     * overshoot easings are written. Clamp s yourself for a hard stop. */
    return args[0] + (args[1] - args[0]) * args[2];
}
static float builtin_smoothstep(const float *args) {
    float e0 = args[0], span = args[1] - args[0], u;
    /* A zero-width edge pair would divide by ~0; degenerate to the step
     * function it is the limit of rather than returning 0. */
    if (fabsf(span) < 1e-9f)
        return args[2] < e0 ? 0.0f : 1.0f;
    u = (args[2] - e0) / span;
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    return u * u * (3.0f - 2.0f * u);
}
static float builtin_sign(const float *args)  {
    /* Exactly 0 (and NaN) return 0 - not copysignf's signed zero. */
    if (args[0] > 0.0f) return 1.0f;
    if (args[0] < 0.0f) return -1.0f;
    return 0.0f;
}
static float builtin_log(const float *args)   { return log10f(args[0]); }
static float builtin_ln(const float *args)    { return logf(args[0]); }
static float builtin_rand(const float *args)  { return expr_rand01(args[0], args[1]); }
static float builtin_rand2(const float *args) { return expr_rand_signed(args[0], args[1]); }

static const ExprBuiltin k_expr_builtins[] = {
    { "sin",   "sinf",       1, 1, builtin_sin   },
    { "cos",   "cosf",       1, 1, builtin_cos   },
    { "tan",   "tanf",       1, 1, builtin_tan   },
    { "asin",  "asinf",      1, 1, builtin_asin  },
    { "acos",  "acosf",      1, 1, builtin_acos  },
    { "atan",  "atanf",      1, 1, builtin_atan  },
    { "atan2", "atan2f",     2, 2, builtin_atan2 },
    { "sqrt",  "sqrtf",      1, 1, builtin_sqrt  },
    { "abs",   "fabsf",      1, 1, builtin_abs   },
    { "pow",   "powf",       2, 2, builtin_pow   },
    { "log",   "log10f",     1, 1, builtin_log   },
    { "ln",    "logf",       1, 1, builtin_ln    },
    { "min",   "fminf",      2, 2, builtin_min   },
    { "max",   "fmaxf",      2, 2, builtin_max   },
    { "clamp", "repl_clampf", 3, 3, builtin_clamp },
    { "lerp",  "repl_lerpf", 3, 3, builtin_lerp  },
    { "smoothstep", "repl_smoothstepf", 3, 3, builtin_smoothstep },
    { "sign",  "repl_signf", 1, 1, builtin_sign  },
    { "floor", "floorf",     1, 1, builtin_floor },
    { "ceil",  "ceilf",      1, 1, builtin_ceil  },
    { "round", "roundf",     1, 1, builtin_round },
    { "fmod",  "fmodf",      2, 2, builtin_fmod  },
    { "rem",   "remainderf", 2, 2, builtin_rem   },
    { "rand",  "repl_randf", 1, 2, builtin_rand  },
    { "rand2", "repl_rand2f", 1, 2, builtin_rand2 },
};

static const char *const k_reserved_identifiers[] = {
    "t", "PI", "TAU", "e", "NAN", "INFINITY",
    "nan", "inf", "infinity",
    "float", "var", "A", "B", "C", NULL
};

static const ExprBuiltin *find_expr_builtin(const char *name) {
    for (size_t builtin_idx = 0;
         builtin_idx < sizeof(k_expr_builtins) / sizeof(k_expr_builtins[0]);
         builtin_idx++) {
        if (strcmp(name, k_expr_builtins[builtin_idx].name) == 0)
            return &k_expr_builtins[builtin_idx];
    }
    return NULL;
}

static int repl_eval_named_constant_value(const char *name, float *out) {
    if (!name)
        return 0;
    if (strcmp(name, "PI") == 0) {
        if (out) *out = (float)M_PI;
        return 1;
    }
    if (strcmp(name, "TAU") == 0) {
        if (out) *out = (float)(2.0 * M_PI);
        return 1;
    }
    if (strcmp(name, "e") == 0) {
        if (out) *out = (float)M_E;
        return 1;
    }
    if (strcmp(name, "NAN") == 0 || strcmp(name, "nan") == 0) {
        if (out) *out = NAN;
        return 1;
    }
    if (strcmp(name, "INFINITY") == 0 ||
        strcmp(name, "inf") == 0 ||
        strcmp(name, "infinity") == 0) {
        if (out) *out = INFINITY;
        return 1;
    }
    return 0;
}

int repl_eval_is_builtin_function(const char *name) {
    return name && find_expr_builtin(name) != NULL;
}

int repl_eval_builtin_count(void) {
    return (int)(sizeof(k_expr_builtins) / sizeof(k_expr_builtins[0]));
}

ReplEvalBuiltinView repl_eval_builtin_at(int idx) {
    ReplEvalBuiltinView view = { NULL, 0, 0, NULL };
    if (idx < 0 || idx >= repl_eval_builtin_count())
        return view;
    view.name = k_expr_builtins[idx].name;
    view.arity_min = k_expr_builtins[idx].arity_min;
    view.arity_max = k_expr_builtins[idx].arity_max;
    view.eval = k_expr_builtins[idx].eval;
    return view;
}

int repl_eval_builtin_index_of(const char *name) {
    if (!name)
        return -1;
    for (int i = 0; i < repl_eval_builtin_count(); i++) {
        if (strcmp(name, k_expr_builtins[i].name) == 0)
            return i;
    }
    return -1;
}

int repl_eval_named_constant(const char *name, float *out) {
    return repl_eval_named_constant_value(name, out);
}

void repl_eval_init_predef_vars(void) {
    g_num_predef_vars_mut = 1;
    strncpy(g_predef_vars_mut[0].name, "t", sizeof(g_predef_vars_mut[0].name) - 1);
    g_predef_vars_mut[0].name[sizeof(g_predef_vars_mut[0].name) - 1] = '\0';
    g_predef_vars_mut[0].value = 0.0f;
}

int repl_eval_find_predef_var_idx_in(const ExprVar *vars, int count,
                                     const char *name) {
    if (!vars || !name)
        return -1;
    for (int i = 0; i < count; i++)
        if (strcmp(name, vars[i].name) == 0)
            return i;
    return -1;
}

int repl_eval_find_predef_var_idx(const char *name) {
    return repl_eval_find_predef_var_idx_in(g_predef_vars, g_num_predef_vars, name);
}

static void expr_write_err(ExprCtx *ctx, const char *fmt, ...) {
    va_list ap;

    if (!ctx || !ctx->err || ctx->err_sz <= 0)
        return;

    va_start(ap, fmt);
    vsnprintf(ctx->err, (size_t)ctx->err_sz, fmt, ap);
    va_end(ap);
}

static const char *skip_ws_ptr(const char *p) {
    while (p && *p && isspace((unsigned char)*p))
        p++;
    return p;
}

static const char *find_matching_square(const char *open, const char *limit) {
    int depth = 0;

    if (!open || *open != '[')
        return NULL;

    for (const char *p = open; *p && (!limit || p < limit); p++) {
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

static int expr_is_plain_integer_literal(const char *src) {
    const char *p = skip_ws_ptr(src);

    if (!p || !*p)
        return 0;

    if (*p == '+' || *p == '-')
        p++;

    if (!isdigit((unsigned char)*p))
        return 0;

    while (isdigit((unsigned char)*p))
        p++;

    p = skip_ws_ptr(p);
    return *p == '\0';
}

static void expr_rewrite_scratch_subscripts_to_c(const char *src,
                                                 char *dst,
                                                 int dst_sz) {
    if (!src || !dst || dst_sz <= 0)
        return;

    const char *p = src;
    char *out = dst;
    char *end = dst + dst_sz - 1;

    while (*p && out < end) {
        const char *id_start = NULL;
        const char *id_end = repl_eval_eat_identifier(p, &id_start);
        if (id_end) {
            int id_len = (int)(id_end - id_start);
            int is_scratch_name = id_len == 1 &&
                                  scratch_char_index(*id_start) >= 0;

            if (is_scratch_name && *id_end == '[') {
                const char *close = find_matching_square(id_end, NULL);
                if (close) {
                    int inner_len = (int)(close - (id_end + 1));
                    char inner[MAX_LINE_LEN];
                    char inner_c[MAX_LINE_LEN];
                    size_t avail;
                    int wrote;

                    if (inner_len >= (int)sizeof(inner))
                        inner_len = (int)sizeof(inner) - 1;
                    memcpy(inner, id_end + 1, (size_t)inner_len);
                    inner[inner_len] = '\0';

                    expr_rewrite_scratch_subscripts_to_c(inner, inner_c, sizeof(inner_c));

                    if (out + id_len >= end)
                        break;
                    memcpy(out, id_start, (size_t)id_len);
                    out += id_len;

                    avail = (size_t)(end - out + 1);
                    wrote = expr_is_plain_integer_literal(inner_c)
                        ? snprintf(out, avail, "[%s]", inner_c)
                        : snprintf(out, avail, "[(int)(%s)]", inner_c);
                    if (wrote < 0)
                        break;
                    if ((size_t)wrote >= avail) {
                        out = end;
                        break;
                    }
                    out += wrote;
                    p = close + 1;
                    continue;
                }
            }

            if (out + id_len >= end)
                break;
            memcpy(out, id_start, (size_t)id_len);
            out += id_len;
            p = id_end;
            continue;
        }

        *out++ = *p++;
    }

    *out = '\0';
}

static void expr_rewrite_scratch_subscripts_to_repl(const char *src,
                                                    char *dst,
                                                    int dst_sz) {
    if (!src || !dst || dst_sz <= 0)
        return;

    const char *p = src;
    char *out = dst;
    char *end = dst + dst_sz - 1;

    while (*p && out < end) {
        const char *id_start = NULL;
        const char *id_end = repl_eval_eat_identifier(p, &id_start);
        if (id_end) {
            int id_len = (int)(id_end - id_start);
            int is_scratch_name = id_len == 1 &&
                                  scratch_char_index(*id_start) >= 0;

            if (is_scratch_name && *id_end == '[') {
                const char *close = find_matching_square(id_end, NULL);
                if (close) {
                    int inner_len = (int)(close - (id_end + 1));
                    const char *inner_src = id_end + 1;
                    char inner[MAX_LINE_LEN];
                    char inner_repl[MAX_LINE_LEN];
                    size_t avail;
                    int wrote;

                    if (inner_len >= 7 &&
                        strncmp(inner_src, "(int)(", 6) == 0 &&
                        inner_src[inner_len - 1] == ')') {
                        inner_src += 6;
                        inner_len -= 7;
                    }

                    if (inner_len < 0)
                        inner_len = 0;
                    if (inner_len >= (int)sizeof(inner))
                        inner_len = (int)sizeof(inner) - 1;
                    memcpy(inner, inner_src, (size_t)inner_len);
                    inner[inner_len] = '\0';

                    expr_rewrite_scratch_subscripts_to_repl(inner, inner_repl, sizeof(inner_repl));

                    if (out + id_len >= end)
                        break;
                    memcpy(out, id_start, (size_t)id_len);
                    out += id_len;

                    avail = (size_t)(end - out + 1);
                    wrote = snprintf(out, avail, "[%s]", inner_repl);
                    if (wrote < 0)
                        break;
                    if ((size_t)wrote >= avail) {
                        out = end;
                        break;
                    }
                    out += wrote;
                    p = close + 1;
                    continue;
                }
            }

            if (out + id_len >= end)
                break;
            memcpy(out, id_start, (size_t)id_len);
            out += id_len;
            p = id_end;
            continue;
        }

        *out++ = *p++;
    }

    *out = '\0';
}

static const char *expr_walk_next_ident(const char **s_inout, const char *end,
                                        char *name_out, size_t name_sz,
                                        const char **q_out, int *too_long) {
    const char *s = *s_inout;
    *too_long = 0;

    while (s && *s && (!end || s < end)) {
        if (s[0] == '/' && (!end || s + 1 < end) && s[1] == '/') {
            *s_inout = s;
            return NULL;
        }

        if (isdigit((unsigned char)*s) ||
            (*s == '.' && (!end || s + 1 < end) && isdigit((unsigned char)s[1]))) {
            const char *next = skip_numeric_literal(s);
            if (next != s) {
                s = next;
                continue;
            }
        }

        const char *start = NULL;
        const char *ident_end = repl_eval_eat_identifier(s, &start);
        if (!ident_end) {
            s++;
            continue;
        }

        int len = (int)(ident_end - start);
        if (len >= (int)name_sz) {
            *too_long = 1;
            *s_inout = ident_end;
            return start;
        }

        memcpy(name_out, start, (size_t)len);
        name_out[len] = '\0';

        *s_inout = ident_end;
        if (q_out) {
            *q_out = skip_ws_ptr(ident_end);
        }
        return start;
    }

    *s_inout = s;
    return NULL;
}

static int expr_range_has_runtime_values(
        const ReplExprIdentValidationConfig *cfg) {
    const char *s = cfg ? cfg->src : NULL;
    const char *end = cfg ? cfg->end : NULL;
    const ExprVar *vars = cfg ? cfg->vars : NULL;
    int num_vars = (cfg && cfg->vars) ? cfg->num_vars : 0;
    char name[REPL_PREDEF_NAME_MAX];
    const char *q = NULL;
    int too_long = 0;

    if (!s)
        return 0;

    while (expr_walk_next_ident(&s, end, name, sizeof(name), &q, &too_long)) {
        if (too_long)
            return 1;

        if (repl_eval_named_constant_value(name, NULL))
            continue;

        if (repl_eval_scratch_array_index(name) >= 0)
            return 1;
        if (*q == '(')
            continue;

        for (int var_idx = 0; var_idx < num_vars; var_idx++) {
            if (strcmp(name, vars[var_idx].name) == 0)
                return 1;
        }
        if (repl_eval_find_predef_var_idx_in(cfg->predef.vars,
                                             cfg->predef.count, name) >= 0)
            return 1;
    }

    return 0;
}

/* Commit-time identifier validation for the expression span [src, end)
 * (end == NULL means whole string): every identifier must resolve to a
 * named constant, a builtin function call, a scratch array (with an
 * in-range, recursively-validated index when the index is itself
 * compile-time evaluable), a visible loop/param var, or a predef var.
 * Also rejects unbalanced parens up front, since eval_expr itself is
 * forgiving about a missing ')'. Pure check - evaluates nothing into
 * state; on failure writes a user-facing message into `err`. */
static int validate_expression_idents_range(
        const ReplExprIdentValidationConfig *cfg) {
    const char *src = cfg ? cfg->src : NULL;
    const char *end = cfg ? cfg->end : NULL;
    const ExprVar *vars = cfg ? cfg->vars : NULL;
    int num_vars = (cfg && cfg->vars) ? cfg->num_vars : 0;
    char *err = cfg ? cfg->err : NULL;
    int errsz = cfg ? cfg->errsz : 0;

    if (!src)
        return 0;

    /* Reject unbalanced parens up front. The recursive-descent eval
     * tolerates a missing ')' on a function call (`max(1, 2` returns
     * 2 silently), so without this check broken input slips through
     * validation and commits as if it had been closed. */
    int paren_depth = 0;
    for (const char *p = src; *p && (!end || p < end); p++) {
        if (p[0] == '/' && p[1] == '/' && (!end || p + 1 < end))
            break;
        if (*p == '(') {
            paren_depth++;
        } else if (*p == ')') {
            if (paren_depth == 0) {
                if (err)
                    snprintf(err, (size_t)errsz, "unexpected ')'");
                return 0;
            }
            paren_depth--;
        }
    }
    if (paren_depth != 0) {
        if (err)
            snprintf(err, (size_t)errsz, "missing ')'");
        return 0;
    }

    const char *s = src;
    char name[REPL_PREDEF_NAME_MAX];
    const char *q = NULL;
    int too_long = 0;

    while (expr_walk_next_ident(&s, end, name, sizeof(name), &q, &too_long)) {
        if (too_long) {
            if (err)
                snprintf(err, (size_t)errsz, "identifier too long");
            return 0;
        }

        if (repl_eval_named_constant_value(name, NULL))
            continue;

        int scratch_idx = repl_eval_scratch_array_index(name);
        if (scratch_idx >= 0) {
            if (*q != '[') {
                if (err)
                    snprintf(err, (size_t)errsz,
                             "scratch array '%s' requires an index", name);
                return 0;
            }

            const char *close = find_matching_square(q, end);
            if (!close) {
                if (err)
                    snprintf(err, (size_t)errsz,
                             "missing ']' for scratch array '%s'", name);
                return 0;
            }

            if (!validate_expression_idents_range(
                    &(ReplExprIdentValidationConfig){
                        .src = q + 1,
                        .end = close,
                        .vars = vars,
                        .num_vars = num_vars,
                        .predef = cfg->predef,
                        .err = err,
                        .errsz = errsz,
                    }))
                return 0;

            if (!expr_range_has_runtime_values(
                    &(ReplExprIdentValidationConfig){
                        .src = q + 1,
                        .end = close,
                        .vars = vars,
                        .num_vars = num_vars,
                        .predef = cfg->predef,
                    })) {
                char idx_expr[MAX_LINE_LEN];
                int idx_len = (int)(close - (q + 1));
                if (idx_len >= (int)sizeof(idx_expr))
                    idx_len = (int)sizeof(idx_expr) - 1;
                memcpy(idx_expr, q + 1, (size_t)idx_len);
                idx_expr[idx_len] = '\0';

                ExprCtx idx_ctx = {
                    .p = idx_expr,
                    .vars = vars,
                    .num_vars = num_vars,
                    .predef_vars = cfg->predef.vars,
                    .predef_count = cfg->predef.count,
                };
                int elem_idx = (int)repl_eval_expr(&idx_ctx);
                if (!scratch_elem_in_range(elem_idx)) {
                    if (err)
                        snprintf(err, (size_t)errsz,
                                 "scratch array index out of range: %d", elem_idx);
                    return 0;
                }
            }

            s = close + 1;
            continue;
        }

        if (*q == '[') {
            if (err)
                snprintf(err, (size_t)errsz, "unknown array '%s'", name);
            return 0;
        }

        if (*q == '(')
            continue;

        int found = 0;
        for (int var_idx = 0; var_idx < num_vars; var_idx++) {
            if (strcmp(name, vars[var_idx].name) == 0) {
                found = 1;
                break;
            }
        }
        if (found)
            continue;

        if (repl_eval_find_predef_var_idx_in(cfg->predef.vars,
                                             cfg->predef.count, name) >= 0)
            continue;

        if (err)
            snprintf(err, (size_t)errsz, "undeclared variable '%s'", name);
        return 0;
    }

    return 1;
}

int repl_eval_is_reserved_ident(const char *name) {
    if (repl_eval_is_builtin_function(name))
        return 1;
    for (const char *const *reserved = k_reserved_identifiers;
         *reserved;
         reserved++) {
        if (strcmp(name, *reserved) == 0)
            return 1;
    }
    return 0;
}

int repl_eval_declare_predef_var_with_value(const char *name, float value, char *err, int errsz) {
    if (!name || !name[0]) {
        if (err) snprintf(err, errsz, "empty variable name");
        return -1;
    }
    if (!repl_eval_is_ident_start((unsigned char)name[0])) {
        if (err) snprintf(err, errsz, "invalid identifier '%s'", name);
        return -1;
    }
    for (const char *p = name; *p; p++) {
        if (!repl_eval_is_ident_continue((unsigned char)*p)) {
            if (err) snprintf(err, errsz, "invalid identifier '%s'", name);
            return -1;
        }
    }
    if (strlen(name) >= sizeof(g_predef_vars[0].name)) {
        if (err) snprintf(err, errsz, "name '%s' too long (max %d chars)",
                          name, (int)sizeof(g_predef_vars[0].name) - 1);
        return -1;
    }
    if (repl_eval_is_reserved_ident(name)) {
        if (err) snprintf(err, errsz, "'%s' is reserved", name);
        return -1;
    }
    if (repl_eval_find_predef_var_idx(name) >= 0) {
        if (err) snprintf(err, errsz, "'%s' already declared", name);
        return -1;
    }
    if (repl_func_alias_lookup_slot(name) >= 0) {
        if (err) snprintf(err, errsz,
                          "'%s' is in use as a function name", name);
        return -1;
    }
    if (g_num_predef_vars >= MAX_PREDEF_VARS) {
        if (err) snprintf(err, errsz, "variable table full (max %d)", MAX_PREDEF_VARS);
        return -1;
    }
    strncpy(g_predef_vars_mut[g_num_predef_vars].name, name,
            sizeof(g_predef_vars_mut[0].name) - 1);
    g_predef_vars_mut[g_num_predef_vars].name[sizeof(g_predef_vars_mut[0].name) - 1] = '\0';
    g_predef_vars_mut[g_num_predef_vars].value = value;
    int idx = g_num_predef_vars;
    g_num_predef_vars_mut++;
    return idx;
}

int repl_eval_declare_predef_var(const char *name, char *err, int errsz) {
    int idx = repl_eval_declare_predef_var_with_value(name, 0.0f, err, errsz);
    return idx >= 0 ? 1 : 0;
}

void repl_eval_undeclare_predef_var(const char *name) {
    int idx = repl_eval_find_predef_var_idx(name);
    if (idx < 0) return;
    for (int i = idx; i < g_num_predef_vars - 1; i++)
        g_predef_vars_mut[i] = g_predef_vars_mut[i + 1];
    g_num_predef_vars_mut--;
    memset(&g_predef_vars_mut[g_num_predef_vars], 0, sizeof(ExprVar));
}

int repl_eval_source_uses_ident(const char *src, const char *name) {
    int nlen = (int)strlen(name);
    const char *s = src;
    while (*s) {
        /* Stop at inline `//` - the rest of the line is a comment,
         * not real code. Mirrors the expression validator at
         * validate_expression_idents_range. */
        if (s[0] == '/' && s[1] == '/')
            break;
        if (isdigit((unsigned char)*s) ||
            (*s == '.' && isdigit((unsigned char)s[1]))) {
            const char *end = skip_numeric_literal(s);
            if (end != s) { s = end; continue; }
        }
        const char *start = NULL;
        const char *ident_end = repl_eval_eat_identifier(s, &start);
        if (!ident_end) { s++; continue; }
        s = ident_end;
        int len = (int)(s - start);
        if (len == nlen && strncmp(start, name, nlen) == 0)
            return 1;
    }
    return 0;
}

int repl_eval_validate_expression_idents(
        const ReplExprIdentValidationConfig *cfg) {
    return validate_expression_idents_range(cfg);
}

int repl_eval_input_has_predef_vars(const char *s) {
    while (*s) {
        if (s[0] == '/' && s[1] == '/')
            break;
        const char *start = NULL;
        const char *ident_end = repl_eval_eat_identifier(s, &start);
        if (!ident_end) { s++; continue; }
        s = ident_end;
        int len = (int)(s - start);
        if (len == 1 && (*start == 'A' || *start == 'B' || *start == 'C'))
            return 1;
        for (int pv = 0; pv < g_num_predef_vars; pv++) {
            int nlen = (int)strlen(g_predef_vars[pv].name);
            if (nlen == len && strncmp(start, g_predef_vars[pv].name, len) == 0)
                return 1;
        }
    }
    return 0;
}

/* ========================================================================= */
/* Expression evaluator - recursive descent with variables                    */
/* ========================================================================= */
/*
 * Grammar, loosest to tightest precedence:
 *   expr       := comparison ( ("&&" | "||") comparison )*
 *   comparison := additive ( ("<" | ">" | "<=" | ">=" | "==" | "!=") additive )*
 *   additive   := term ( ("+" | "-") term )*
 *   term       := primary ( ("*" | "/" | "%") primary )*
 *   primary    := number | "(" expr ")" | "-" primary | "+" primary | "!" primary
 *               | identifier [ "(" arg-list ")" ]
 *
 * Unknown identifiers and unrecognized function calls return 0.0f rather than
 * raise an error; validation happens upstream in validate_expression_idents().
 */

static void expr_skip_ws(ExprCtx *ctx) {
    while (*ctx->p && isspace((unsigned char)*ctx->p)) ctx->p++;
}

/* GLSL-classic fract(sin(...)*k) hash. The 0.5 phase offset on the seed
 * keeps seed == 0 off the sin() zero crossing: without it rand(0, 0) == 0
 * exactly and rand(0, iter) collapses to a pure 1-D sweep (its uniformity
 * is the worst of any argument pattern). See `randdist` in tests/test_eval.c
 * for the distribution table this was tuned against. */
#define EXPR_RAND_SEED_OFFSET 0.5f

static float expr_rand01(float seed, float iter) {
    float h = sinf((seed + EXPR_RAND_SEED_OFFSET) * 12.9898f + iter * 78.233f) * 43758.5453f;
    float frac = h - floorf(h);
    if (frac < 0.0f) frac += 1.0f;
    return frac;
}

/* Like expr_rand01 but maps to [-1, 1]. Same hash so identical
 * (seed, iter) pairs produce correlated values across the two
 * functions, which keeps `rand` and `rand2` deterministic together
 * inside a single expression. */
static float expr_rand_signed(float seed, float iter) {
    return expr_rand01(seed, iter) * 2.0f - 1.0f;
}

/* The `primary` production of the recursive-descent grammar - the leaf
 * dispatcher under repl_eval_expr's operator-precedence chain. In match
 * order: unary -/+/!, parenthesised subexpression, numeric literal,
 * then identifier resolution (named constant → scratch-array subscript
 * → loop/param var → predef var → builtin function call). Errors report
 * through expr_write_err and yield 0.0f so evaluation always completes. */
static float eval_primary(ExprCtx *ctx) {
    expr_skip_ws(ctx);

    /* Unary minus / plus / logical not */
    if (*ctx->p == '-') { ctx->p++; return -eval_primary(ctx); }
    if (*ctx->p == '+') { ctx->p++; return eval_primary(ctx); }
    if (*ctx->p == '!') { ctx->p++; return eval_primary(ctx) != 0.0f ? 0.0f : 1.0f; }

    /* Parenthesised expression */
    if (*ctx->p == '(') {
        ctx->p++;
        float v = repl_eval_expr(ctx);
        expr_skip_ws(ctx);
        if (*ctx->p == ')') ctx->p++;
        return v;
    }

    /* Number literal */
    if (isdigit((unsigned char)*ctx->p) || *ctx->p == '.') {
        char *end;
        float v = strtof(ctx->p, &end);
        ctx->p = end;
        if (*ctx->p == 'f' || *ctx->p == 'F')
            ctx->p++;
        return v;
    }

    /* Identifier: constant, variable, or function */
    {
        const char *name_start = NULL;
        const char *name_end = repl_eval_eat_identifier(ctx->p, &name_start);
        if (name_end) {
        char name[32];
        int name_len = (int)(name_end - name_start);
        ctx->p = name_end;
        if (name_len >= (int)sizeof(name))
            return 0.0f;
        memcpy(name, name_start, (size_t)name_len);
        name[name_len] = '\0';
        const char *q = skip_ws_ptr(ctx->p);

        /* Constants */
        {
            float constant_value = 0.0f;
            if (repl_eval_named_constant_value(name, &constant_value))
                return constant_value;
        }

        {
            int scratch_idx = repl_eval_scratch_array_index(name);
            if (scratch_idx >= 0) {
                if (*q != '[') {
                    ctx->p = q;
                    expr_write_err(ctx, "scratch array '%s' requires an index", name);
                    return 0.0f;
                }

                ctx->p = q + 1;
                float idx_value = repl_eval_expr(ctx);
                expr_skip_ws(ctx);
                if (*ctx->p != ']') {
                    expr_write_err(ctx, "missing ']' for scratch array '%s'", name);
                    return 0.0f;
                }
                ctx->p++;

                int elem_idx = (int)idx_value;
                float value = 0.0f;
                if (!repl_eval_scratch_get(scratch_idx, elem_idx, &value)) {
                    expr_write_err(ctx, "scratch array index out of range: %d", elem_idx);
                    return 0.0f;
                }
                return value;
            }
            if (*q == '[') {
                ctx->p = q;
                expr_write_err(ctx, "unknown array '%s'", name);
                return 0.0f;
            }
        }

        /* Variables (loop vars take precedence) */
        if (ctx->vars) {
            for (int i = 0; i < ctx->num_vars; i++)
                if (strcmp(name, ctx->vars[i].name) == 0)
                    return ctx->vars[i].value;
        }

        /* Predefined variables (checked after loop vars). Resolve against the
         * context's predef view when one is supplied (compile evaluating
         * against its ReplCompileContext snapshot); otherwise the live table. */
        {
            const ExprVar *pv = ctx->predef_vars ? ctx->predef_vars : g_predef_vars;
            int pc = ctx->predef_vars ? ctx->predef_count : g_num_predef_vars;
            for (int i = 0; i < pc; i++)
                if (strcmp(name, pv[i].name) == 0)
                    return pv[i].value;
        }

        /* Functions (consume opening paren) */
        if (*q == '(') {
            const ExprBuiltin *builtin = find_expr_builtin(name);
            float args[REPL_EXPR_BUILTIN_ARGS_MAX] = { 0.0f };
            int arg_count = 0;
            int arg_overflow = 0;

            ctx->p = q + 1;
            expr_skip_ws(ctx);
            if (*ctx->p != ')') {
                for (;;) {
                    float arg = repl_eval_expr(ctx);
                    if (arg_count < (int)(sizeof(args) / sizeof(args[0])))
                        args[arg_count] = arg;
                    else
                        arg_overflow = 1;
                    arg_count++;

                    expr_skip_ws(ctx);
                    if (*ctx->p != ',')
                        break;
                    ctx->p++;
                    expr_skip_ws(ctx);
                }
            }
            expr_skip_ws(ctx);
            if (*ctx->p == ')') {
                ctx->p++;
            } else {
                expr_write_err(ctx, "missing ')' for '%s'", name);
                return 0.0f;
            }

            if (!builtin)
                return 0.0f;
            if (arg_overflow ||
                arg_count < builtin->arity_min ||
                arg_count > builtin->arity_max) {
                expr_write_err(ctx, "unsupported arity for '%s'", name);
                return 0.0f;
            }
            return builtin->eval(args);
        }

        return 0.0f;   /* unknown identifier */
        }
    }

    return 0.0f;
}

static float eval_term(ExprCtx *ctx) {
    float v = eval_primary(ctx);
    for (;;) {
        expr_skip_ws(ctx);
        /* Divide/modulo by ~0 yields 0 rather than inf/NaN: the
         * evaluator is kept total so a transient bad denominator
         * during live editing can't poison the whole frame. */
        if (*ctx->p == '*') { ctx->p++; v *= eval_primary(ctx); }
        else if (*ctx->p == '/') {
            ctx->p++;
            float d = eval_primary(ctx);
            v = (fabsf(d) > 1e-12f) ? v / d : 0.0f;
        }
        else if (*ctx->p == '%') {
            ctx->p++;
            float d = eval_primary(ctx);
            v = (fabsf(d) > 1e-12f) ? fmodf(v, d) : 0.0f;
        }
        else break;
    }
    return v;
}

static float eval_additive(ExprCtx *ctx) {
    float v = eval_term(ctx);
    for (;;) {
        expr_skip_ws(ctx);
        if (*ctx->p == '+') { ctx->p++; v += eval_term(ctx); }
        else if (*ctx->p == '-') { ctx->p++; v -= eval_term(ctx); }
        else break;
    }
    return v;
}

static float eval_comparison(ExprCtx *ctx) {
    float v = eval_additive(ctx);
    for (;;) {
        expr_skip_ws(ctx);
        if (ctx->p[0] == '>' && ctx->p[1] == '=') {
            ctx->p += 2; v = (v >= eval_additive(ctx)) ? 1.0f : 0.0f;
        } else if (ctx->p[0] == '<' && ctx->p[1] == '=') {
            ctx->p += 2; v = (v <= eval_additive(ctx)) ? 1.0f : 0.0f;
        } else if (ctx->p[0] == '=' && ctx->p[1] == '=') {
            /* float == / != compare within a 1e-6 epsilon, not bitwise */
            ctx->p += 2; v = (fabsf(v - eval_additive(ctx)) < 1e-6f) ? 1.0f : 0.0f;
        } else if (ctx->p[0] == '!' && ctx->p[1] == '=') {
            ctx->p += 2; v = (fabsf(v - eval_additive(ctx)) >= 1e-6f) ? 1.0f : 0.0f;
        } else if (*ctx->p == '>' && ctx->p[1] != '=') {
            ctx->p++; v = (v > eval_additive(ctx)) ? 1.0f : 0.0f;
        } else if (*ctx->p == '<' && ctx->p[1] != '=') {
            ctx->p++; v = (v < eval_additive(ctx)) ? 1.0f : 0.0f;
        } else break;
    }
    return v;
}

float repl_eval_expr(ExprCtx *ctx) {
    float v = eval_comparison(ctx);
    for (;;) {
        expr_skip_ws(ctx);
        if (ctx->p[0] == '&' && ctx->p[1] == '&') {
            ctx->p += 2;
            float r = eval_comparison(ctx);
            v = (v != 0.0f && r != 0.0f) ? 1.0f : 0.0f;
        } else if (ctx->p[0] == '|' && ctx->p[1] == '|') {
            ctx->p += 2;
            float r = eval_comparison(ctx);
            v = (v != 0.0f || r != 0.0f) ? 1.0f : 0.0f;
        } else break;
    }
    return v;
}

int repl_eval_parse_exprs(const char *s, float *out, int max,
                const ExprVar *vars, int num_vars) {
    int n = 0;
    const char *p = s;
    while (*p && n < max) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p || *p == ')') break;
        ExprCtx ctx = { p, vars, num_vars, NULL, 0 };
        out[n] = repl_eval_expr(&ctx);
        if (ctx.p == p) break;   /* no progress */
        n++;
        p = ctx.p;
    }
    return n;
}

const char *repl_scan_next_arg_delim(const char *s) {
    int depth = 0;
    while (*s && (depth > 0 || (*s != ',' && *s != ')'))) {
        if (*s == '(') depth++;
        else if (*s == ')') depth--;
        s++;
    }
    return s;
}

const char *repl_scan_to_matching_paren(const char *p) {
    while (*p) {
        p = repl_scan_next_arg_delim(p);
        if (*p == ')' || *p == '\0') break;
        /* *p == ',' - internal top-level comma; skip and continue
         * scanning. The matching close lies past further commas. */
        p++;
    }
    return p;
}

const char *repl_line_trailing_comment(const char *s) {
    int in_str = 0;
    if (!s) return NULL;
    for (const char *p = s; *p; p++) {
        if (in_str) {
            if (*p == '\\' && p[1]) { p++; continue; }  /* skip escaped char */
            if (*p == '"') in_str = 0;
            continue;
        }
        if (*p == '"') { in_str = 1; continue; }
        if (p[0] == '/' && p[1] == '/') return p;
    }
    return NULL;
}

/* Scan `line`'s trailing comment for a whole-token `tag` (length `tag_len`):
 * preceded by comment start, whitespace, or '/', followed by end or
 * whitespace. Rejects supersets like "@tuned=5". */
static int line_comment_has_tag(const char *line, const char *tag,
                                size_t tag_len) {
    const char *cmt = repl_line_trailing_comment(line);
    if (!cmt)
        return 0;
    for (const char *p = cmt; (p = strstr(p, tag)) != NULL; p += tag_len) {
        char prev = (p == cmt) ? ' ' : p[-1];
        char next = p[tag_len];
        if (isspace((unsigned char)prev) || prev == '/') {
            if (next == '\0' || isspace((unsigned char)next))
                return 1;
        }
    }
    return 0;
}

int repl_eval_line_has_tune_tag(const char *line) {
    return line_comment_has_tag(line, "@tune", 5);
}

int repl_eval_line_has_config_tag(const char *line) {
    return line_comment_has_tag(line, "@config", 7);
}

void repl_append_trailing_comment(char *dst, size_t dst_sz, const char *source) {
    const char *cmt = repl_line_trailing_comment(source);
    if (!dst || dst_sz == 0 || !cmt)
        return;
    /* Idempotent: if `dst` already carries a trailing comment, leave it.
     * Lets multiple regeneration sites call this without double-appending
     * (e.g. the parser already attached it to the canonical text). */
    if (repl_line_trailing_comment(dst))
        return;

    /* Trim the comment's own trailing whitespace. */
    const char *cmt_end = cmt + strlen(cmt);
    while (cmt_end > cmt && isspace((unsigned char)cmt_end[-1]))
        cmt_end--;
    if (cmt_end == cmt)
        return;

    size_t cur = strnlen(dst, dst_sz);
    if (cur >= dst_sz - 1)
        return;  /* dst already full */

    /* A single separator space ONLY when dst already has content - a
     * comment-only line (empty dst, e.g. repl_eval_expr_to_c translating
     * a `// ...` line whose code part is empty) must not gain a leading
     * space. Then the comment span, all bounds-checked. */
    if (cur > 0)
        dst[cur++] = ' ';
    for (const char *p = cmt; p < cmt_end && cur < dst_sz - 1; p++)
        dst[cur++] = *p;
    dst[cur] = '\0';
}

/* ========================================================================= */
/* Inline numeric swatch helpers                                              */
/* ========================================================================= */

/* Strict float-literal check ([+-]digits[.digits][e[+-]digits], whole
 * span) - gates the inline numeric swatch so it only attaches to a
 * literal argument, never an expression. */
static int is_pure_numeric_literal(const char *s, int len) {
    int i = 0;
    if (i < len && (s[i] == '+' || s[i] == '-')) i++;
    if (i >= len) return 0;
    int has_digit = 0;
    while (i < len && (s[i] >= '0' && s[i] <= '9')) { has_digit = 1; i++; }
    if (i < len && s[i] == '.') {
        i++;
        while (i < len && (s[i] >= '0' && s[i] <= '9')) { has_digit = 1; i++; }
    }
    if (!has_digit) return 0;
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < len && (s[i] == '+' || s[i] == '-')) i++;
        if (i >= len || s[i] < '0' || s[i] > '9') return 0;
        while (i < len && (s[i] >= '0' && s[i] <= '9')) i++;
    }
    return i == len;
}

static int is_swatch_numeric_scan_char(char c) {
    return isdigit((unsigned char)c) || c == '.' || c == 'e' || c == 'E';
}

static int swatch_sign_has_unary_context(const char *src, int sign_pos) {
    int i = sign_pos - 1;
    while (i >= 0 && isspace((unsigned char)src[i]))
        i--;
    if (i < 0)
        return 1;
    switch (src[i]) {
    case '=': case '+': case '-': case '*': case '/': case '%':
    case '(': case ',': case '^':
        return 1;
    default:
        return 0;
    }
}

static int swatch_cursor_char_can_seed(const char *src, int idx, int code_len) {
    char c;
    if (idx < 0 || idx >= code_len)
        return 0;
    c = src[idx];
    if (isdigit((unsigned char)c) || c == 'e' || c == 'E')
        return 1;
    if (c == '.') {
        return (idx + 1 < code_len && isdigit((unsigned char)src[idx + 1])) ||
               (idx > 0 && isdigit((unsigned char)src[idx - 1]));
    }
    if (c == '+' || c == '-') {
        if (idx > 0 && (src[idx - 1] == 'e' || src[idx - 1] == 'E'))
            return 1;
        return swatch_sign_has_unary_context(src, idx) &&
               idx + 1 < code_len &&
               (isdigit((unsigned char)src[idx + 1]) || src[idx + 1] == '.');
    }
    return 0;
}

static int swatch_numeric_span_has_token_boundaries(const char *src,
                                                    int lo,
                                                    int hi,
                                                    int code_len) {
    if (lo > 0 && repl_eval_is_ident_continue((unsigned char)src[lo - 1]))
        return 0;
    if (hi < code_len &&
        repl_eval_is_ident_continue((unsigned char)src[hi]))
        return 0;
    return 1;
}

static int swatch_numeric_result_from_span(const char *src,
                                           int lo,
                                           int hi,
                                           ReplNumericArgAtCursor *out) {
    char literal[MAX_LINE_LEN];
    int span_len = hi - lo;
    ExprCtx ctx;

    if (!out || span_len <= 0 || span_len >= (int)sizeof(literal))
        return 0;
    if (!is_pure_numeric_literal(src + lo, span_len))
        return 0;

    memcpy(literal, src + lo, (size_t)span_len);
    literal[span_len] = '\0';
    ctx.p = literal;
    ctx.vars = NULL;
    ctx.num_vars = 0;
    ctx.err = NULL;
    ctx.err_sz = 0;

    out->found = 1;
    out->arg_start = lo;
    out->arg_end = hi;
    out->value = repl_eval_expr(&ctx);
    return 1;
}

static int swatch_direct_numeric_arg_at_cursor(const char *src,
                                               int cursor,
                                               int code_len,
                                               ReplNumericArgAtCursor *out) {
    int seed = -1;
    int lo, hi;

    if (cursor > code_len)
        return 0;

    if (cursor < code_len && swatch_cursor_char_can_seed(src, cursor, code_len))
        seed = cursor;
    else if (cursor > 0 &&
             swatch_cursor_char_can_seed(src, cursor - 1, code_len))
        seed = cursor - 1;
    if (seed < 0)
        return 0;

    lo = seed;
    while (lo > 0) {
        char c = src[lo - 1];
        if (is_swatch_numeric_scan_char(c)) {
            lo--;
            continue;
        }
        if ((c == '+' || c == '-') && lo >= 2 &&
            (src[lo - 2] == 'e' || src[lo - 2] == 'E')) {
            lo--;
            continue;
        }
        break;
    }
    if (lo > 0 && (src[lo - 1] == '+' || src[lo - 1] == '-') &&
        swatch_sign_has_unary_context(src, lo - 1))
        lo--;

    hi = seed + 1;
    while (hi < code_len) {
        char c = src[hi];
        if (is_swatch_numeric_scan_char(c)) {
            hi++;
            continue;
        }
        if ((c == '+' || c == '-') && hi > lo &&
            (src[hi - 1] == 'e' || src[hi - 1] == 'E')) {
            hi++;
            continue;
        }
        break;
    }

    if (!swatch_numeric_span_has_token_boundaries(src, lo, hi, code_len))
        return 0;
    return swatch_numeric_result_from_span(src, lo, hi, out);
}

ReplNumericArgAtCursor repl_eval_numeric_arg_at_cursor(const char *src,
                                                       int cursor) {
    ReplNumericArgAtCursor result = { 0, 0, 0, 0.0f };
    const char *comment;
    int code_len;
    if (!src || cursor < 0) return result;
    int len = (int)strlen(src);
    if (cursor > len) return result;
    comment = repl_line_trailing_comment(src);
    code_len = comment ? (int)(comment - src) : len;

    if (swatch_direct_numeric_arg_at_cursor(src, cursor, code_len, &result))
        return result;
    if (cursor > code_len)
        return result;

    /* Find innermost enclosing '(' by scanning left. */
    int depth = 0;
    int paren_pos = -1;
    int i;
    for (i = cursor - 1; i >= 0; i--) {
        if (src[i] == ')') depth++;
        else if (src[i] == '(') {
            if (depth == 0) { paren_pos = i; break; }
            depth--;
        }
    }
    if (paren_pos < 0) return result;

    /* Walk top-level slots from paren_pos+1 using repl_scan_next_arg_delim. */
    const char *base = src;
    const char *p = base + paren_pos + 1;
    while ((int)(p - base) < code_len && *p && *p != ')') {
        const char *slot_start = p;
        const char *delim = repl_scan_next_arg_delim(p);
        int s_off = (int)(slot_start - base);
        int e_off = (int)(delim - base);

        if (e_off > code_len)
            e_off = code_len;

        if (cursor >= s_off && cursor <= e_off) {
            /* Trim whitespace. */
            int lo = s_off, hi = e_off;
            while (lo < hi && isspace((unsigned char)base[lo])) lo++;
            while (hi > lo && isspace((unsigned char)base[hi - 1])) hi--;
            if (lo >= hi) return result;

            if (!swatch_numeric_result_from_span(base, lo, hi, &result))
                return result;
            return result;
        }

        if (*delim == ',') p = delim + 1;
        else break;
    }
    return result;
}

float repl_eval_swatch_step(float value) {
    float mag = fabsf(value);
    float exp10 = (mag < 10.0f) ? 0.0f : floorf(log10f(mag));
    return 0.05f * powf(10.0f, exp10);
}

void repl_eval_format_swatch_number(float v, char *out, int out_sz) {
    if (out_sz <= 0) return;
    snprintf(out, (size_t)out_sz, "%.6g", (double)v);
}

/* ========================================================================= */
/* Expression translation: REPL <-> C                                         */
/* ========================================================================= */

/* Translate one REPL expression to compilable C for export. Three
 * sequential rewrite passes over the code (a trailing `// ...` comment
 * is split off first and re-attached untouched at the end):
 *   1. identifier mapping - REPL constants/builtins to their C names
 *      (PI -> M_PI, sin -> sinf, ...), other identifiers verbatim
 *   2. `LHS % RHS` -> `fmodf(LHS, RHS)` (REPL `%` is float-modulo;
 *      C's operator is integer-only), with paren/call-aware operand
 *      scanning in both directions
 *   3. scratch-array subscripts to their exported C array names
 * Inverse of repl_eval_c_expr_to_repl below. */
void repl_eval_expr_to_c(const char *in, char *out, int out_sz) {
    static const struct { const char *from; const char *to; } k_const_expr_to_c[] = {
        { "TAU", "(2*M_PI)" },
        { "PI",  "M_PI" },
        { "e",   "M_E" },
        { "NAN", "NAN" },
        { "nan", "NAN" },
        { "INFINITY", "INFINITY" },
        { "inf", "INFINITY" },
        { "infinity", "INFINITY" }
    };
    const char *p = in;
    char *dst = out;
    char *end = out + out_sz - 1;

    /* Translate only the code; a trailing `// ...` comment is appended raw
     * at the very end so the three rewrite passes below never mangle words
     * inside it (e.g. an identifier in the comment matching PI / max / a
     * scratch-array subscript). */
    const char *expr_end = repl_line_trailing_comment(in);
    if (!expr_end) expr_end = in + strlen(in);

    while (p < expr_end && dst < end) {
        const char *id_start = NULL;
        const char *id_end = repl_eval_eat_identifier(p, &id_start);
        if (id_end) {
            int id_len = (int)(id_end - id_start);
            int found = 0;

            for (size_t i = 0; i < sizeof(k_const_expr_to_c) / sizeof(k_const_expr_to_c[0]); i++) {
                if ((int)strlen(k_const_expr_to_c[i].from) == id_len &&
                    strncmp(id_start, k_const_expr_to_c[i].from, id_len) == 0) {
                    int rlen = (int)strlen(k_const_expr_to_c[i].to);
                    if (dst + rlen < end) {
                        memcpy(dst, k_const_expr_to_c[i].to, rlen);
                        dst += rlen;
                    }
                    found = 1;
                    break;
                }
            }

            if (!found) {
                for (size_t i = 0; i < sizeof(k_expr_builtins) / sizeof(k_expr_builtins[0]); i++) {
                    if ((int)strlen(k_expr_builtins[i].name) == id_len &&
                        strncmp(id_start, k_expr_builtins[i].name, id_len) == 0) {
                        int rlen = (int)strlen(k_expr_builtins[i].c_name);
                        if (dst + rlen < end) {
                            memcpy(dst, k_expr_builtins[i].c_name, rlen);
                            dst += rlen;
                        }
                        found = 1;
                        break;
                    }
                }
            }

            if (!found) {
                if (dst + id_len < end) {
                    memcpy(dst, id_start, id_len);
                    dst += id_len;
                }
            }
            p = id_end;
        } else {
            *dst++ = *p++;
        }
    }
    *dst = '\0';

    /* Second pass: rewrite `LHS % RHS` into `fmodf(LHS, RHS)`. C's `%`
     * operator is integer-only, but REPL uses float semantics (matches
     * eval_expr in this file). We operate on the already-translated buffer
     * in-place via a temp copy. Operands may be parenthesized expressions,
     * identifier+call (e.g. `func(x)`), or plain ident/number tokens. */
    {
        char src[MAX_LINE_LEN];
        strncpy(src, out, sizeof(src) - 1);
        src[sizeof(src) - 1] = '\0';
        char *o = out;
        char *oend = out + out_sz - 1;
        int n = (int)strlen(src);
        int ix = 0;
        while (ix < n && o < oend) {
            if (src[ix] != '%') { *o++ = src[ix++]; continue; }
            /* Find LHS in already-written out buffer */
            char *lhs_end = o;
            while (lhs_end > out && isspace((unsigned char)lhs_end[-1])) lhs_end--;
            char *lhs_start = lhs_end;
            if (lhs_start > out && lhs_start[-1] == ')') {
                int depth = 1;
                lhs_start--;
                while (lhs_start > out && depth > 0) {
                    lhs_start--;
                    if (*lhs_start == ')') depth++;
                    else if (*lhs_start == '(') depth--;
                }
                while (lhs_start > out &&
                       repl_eval_is_ident_continue((unsigned char)lhs_start[-1]))
                    lhs_start--;
            } else {
                while (lhs_start > out &&
                       (repl_eval_is_ident_continue((unsigned char)lhs_start[-1]) ||
                        lhs_start[-1] == '.'))
                    lhs_start--;
            }
            int lhs_len = (int)(lhs_end - lhs_start);
            char lhs[MAX_LINE_LEN];
            if (lhs_len < 0) lhs_len = 0;
            if (lhs_len >= (int)sizeof(lhs)) lhs_len = sizeof(lhs) - 1;
            memcpy(lhs, lhs_start, lhs_len);
            lhs[lhs_len] = '\0';

            /* Advance past '%' and scan RHS */
            ix++;
            while (ix < n && isspace((unsigned char)src[ix])) ix++;
            int rhs_start = ix;
            if (ix < n && (src[ix] == '-' || src[ix] == '+')) ix++;
            if (ix < n && src[ix] == '(') {
                int depth = 1; ix++;
                while (ix < n && depth > 0) {
                    if (src[ix] == '(') depth++;
                    else if (src[ix] == ')') depth--;
                    ix++;
                }
            } else {
                while (ix < n && (isalnum((unsigned char)src[ix]) ||
                                  src[ix] == '_' || src[ix] == '.'))
                    ix++;
                if (ix < n && src[ix] == '(') {
                    int depth = 1; ix++;
                    while (ix < n && depth > 0) {
                        if (src[ix] == '(') depth++;
                        else if (src[ix] == ')') depth--;
                        ix++;
                    }
                }
            }
            int rhs_len = ix - rhs_start;
            char rhs[MAX_LINE_LEN];
            if (rhs_len < 0) rhs_len = 0;
            if (rhs_len >= (int)sizeof(rhs)) rhs_len = sizeof(rhs) - 1;
            memcpy(rhs, src + rhs_start, rhs_len);
            rhs[rhs_len] = '\0';

            /* Rewind output to lhs_start and emit fmodf(lhs, rhs) */
            o = lhs_start;
            int w = snprintf(o, (size_t)(oend - o), "fmodf(%s, %s)", lhs, rhs);
            if (w > 0 && o + w < oend) o += w;
            else o = oend;
        }
        *o = '\0';
    }

    {
        char scratch_buf[MAX_LINE_LEN];
        expr_rewrite_scratch_subscripts_to_c(out, scratch_buf, sizeof(scratch_buf));
        snprintf(out, (size_t)out_sz, "%s", scratch_buf);
    }

    /* Re-attach the raw trailing comment (untouched by the passes above).
     * Trim any code-side trailing whitespace first so the join is a single
     * space rather than whatever spacing preceded the original `//`. */
    if (expr_end < in + strlen(in)) {
        size_t n = strnlen(out, (size_t)out_sz);
        while (n > 0 && isspace((unsigned char)out[n - 1])) out[--n] = '\0';
        repl_append_trailing_comment(out, (size_t)out_sz, in);
    }
}

/* Inverse of repl_eval_expr_to_c: translate a C expression from an
 * imported export file back to REPL spelling. `(2*M_PI)` collapses to
 * TAU by substring pass first (it isn't a single identifier), then an
 * identifier-aware pass maps C names back (M_PI -> PI, sinf -> sin),
 * then scratch-array subscripts return to A/B/C[] form. fmodf is left
 * as-is - the REPL grammar accepts it as the fmod builtin. */
void repl_eval_c_expr_to_repl(const char *in, char *out, int out_sz) {
    if (!in || !out || out_sz <= 0)
        return;

    /* First pass: substring replacement for (2*M_PI) -> TAU */
    char tmp[MAX_LINE_LEN];
    snprintf(tmp, sizeof(tmp), "%s", in);

    {
        char buf[MAX_LINE_LEN];
        char *dst = buf;
        char *bend = buf + sizeof(buf) - 1;
        const char *p = tmp;
        while (*p && dst < bend) {
            if (strncmp(p, "(2*M_PI)", 8) == 0) {
                if (dst + 3 < bend) { memcpy(dst, "TAU", 3); dst += 3; }
                p += 8;
            } else {
                *dst++ = *p++;
            }
        }
        *dst = '\0';
        snprintf(tmp, sizeof(tmp), "%s", buf);
    }

    /* Identifier-aware replacement: sinf->sin, M_PI->PI, M_E->e, etc. */
    static const struct { const char *from; const char *to; } k_const_c_to_expr[] = {
        { "M_PI", "PI" },
        { "M_E",  "e"  },
        { "NAN", "NAN" },
        { "nan", "NAN" },
        { "INFINITY", "INFINITY" },
        { "inf", "INFINITY" },
        { "infinity", "INFINITY" }
    };
    const char *p = tmp;
    char *dst = out;
    char *end = out + out_sz - 1;

    while (*p && dst < end) {
        const char *id_start = NULL;
        const char *id_end = repl_eval_eat_identifier(p, &id_start);
        if (id_end) {
            int id_len = (int)(id_end - id_start);
            int found = 0;

            for (size_t i = 0; i < sizeof(k_const_c_to_expr) / sizeof(k_const_c_to_expr[0]); i++) {
                if ((int)strlen(k_const_c_to_expr[i].from) == id_len &&
                    strncmp(id_start, k_const_c_to_expr[i].from, id_len) == 0) {
                    int rlen = (int)strlen(k_const_c_to_expr[i].to);
                    if (dst + rlen < end) {
                        memcpy(dst, k_const_c_to_expr[i].to, rlen);
                        dst += rlen;
                    }
                    found = 1;
                    break;
                }
            }

            if (!found) {
                for (size_t i = 0; i < sizeof(k_expr_builtins) / sizeof(k_expr_builtins[0]); i++) {
                    if ((int)strlen(k_expr_builtins[i].c_name) == id_len &&
                        strncmp(id_start, k_expr_builtins[i].c_name, id_len) == 0) {
                        int rlen = (int)strlen(k_expr_builtins[i].name);
                        if (dst + rlen < end) {
                            memcpy(dst, k_expr_builtins[i].name, rlen);
                            dst += rlen;
                        }
                        found = 1;
                        break;
                    }
                }
            }

            if (!found) {
                if (dst + id_len < end) {
                    memcpy(dst, id_start, id_len);
                    dst += id_len;
                }
            }
            p = id_end;
        } else {
            *dst++ = *p++;
        }
    }
    *dst = '\0';

    {
        char scratch_buf[MAX_LINE_LEN];
        expr_rewrite_scratch_subscripts_to_repl(out, scratch_buf, sizeof(scratch_buf));
        snprintf(out, (size_t)out_sz, "%s", scratch_buf);
    }
}

/* ========================================================================= */
/* For-loop header parsers                                                    */
/* ========================================================================= */

/* Parse a REPL loop header `for(var, start, end[, step])`, evaluating
 * the bound expressions against the visible vars. On success fills
 * var_name / start / end / step (default 1) and points *body_start
 * just past the ')'. Returns 0 on any token mismatch. */
int repl_eval_parse_for_header(const ReplForHeaderParseConfig *cfg) {
    const char *p = cfg ? cfg->input : NULL;
    char *var_name = cfg ? cfg->var_name : NULL;
    int var_sz = cfg ? cfg->var_sz : 0;
    const ExprVar *vars = cfg ? cfg->vars : NULL;
    int num_vars = (cfg && cfg->vars) ? cfg->num_vars : 0;

    if (!cfg || !p || !var_name || var_sz <= 0 ||
        !cfg->start || !cfg->end || !cfg->step)
        return 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "for(", 4) != 0 && strncmp(p, "for (", 5) != 0)
        return 0;

    while (*p && *p != '(') p++;
    if (!*p) return 0;
    p++;

    /* Variable name */
    while (*p && isspace((unsigned char)*p)) p++;
    int name_len = 0;
    while (*p && repl_eval_is_ident_continue((unsigned char)*p) && name_len < var_sz - 1)
        var_name[name_len++] = *p++;
    var_name[name_len] = '\0';
    if (name_len == 0) return 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ',') return 0;
    p++;

    /* Start and end expressions share the same ExprCtx; we only need to
     * re-seat ctx.p at each argument boundary since eval_expr advances it.
     * Each bound's capture span is [seat point, post-eval position] - the
     * exact text the evaluator consumed. */
    ExprCtx ctx = {
        .p = p,
        .vars = vars,
        .num_vars = num_vars,
        .predef_vars = cfg->predef_vars,
        .predef_count = cfg->predef_count,
    };
    *cfg->start = repl_eval_expr(&ctx);
    if (cfg->capture && cfg->capture->fn)
        cfg->capture->fn(cfg->capture->user_data,
                         REPL_EXPR_ROLE_LOOP_START, 0, p, ctx.p);
    p = ctx.p;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ',') return 0;
    p++;

    ctx.p = p;
    *cfg->end = repl_eval_expr(&ctx);
    if (cfg->capture && cfg->capture->fn)
        cfg->capture->fn(cfg->capture->user_data,
                         REPL_EXPR_ROLE_LOOP_END, 0, p, ctx.p);
    p = ctx.p;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Optional step (defaults to 1). */
    *cfg->step = 1.0f;
    if (*p == ',') {
        p++;
        ctx.p = p;
        *cfg->step = repl_eval_expr(&ctx);
        if (cfg->capture && cfg->capture->fn)
            cfg->capture->fn(cfg->capture->user_data,
                             REPL_EXPR_ROLE_LOOP_STEP, 0, p, ctx.p);
        p = ctx.p;
        while (*p && isspace((unsigned char)*p)) p++;
    }

    if (*p != ')') return 0;
    p++;

    if (cfg->body_start) *cfg->body_start = p;
    return 1;
}

/* Import-side translation of a numeric C for-header
 * `for ([type] i = START; i <op> END; i++/--/+=/-= STEP)` into REPL
 * loop parameters. The token walk is strictly linear (each clause is
 * one validate-and-advance step); the semantic work is mapping C's
 * comparison forms onto the REPL's half-open ascending loop model:
 * inclusive bounds (<=, >=) nudge END by one step, and a `>` condition
 * forces a negative step so the loop terminates. Bounds are evaluated
 * to floats here (no var context - imported headers are literal). */
int repl_eval_parse_c_for_header(const char *input, char *var_name, int var_sz,
                       float *start, float *end, float *step) {
    const char *p = input;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "for", 3) != 0) return 0;
    p += 3;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '(') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Skip optional type: float, int, double */
    if (strncmp(p, "float ", 6) == 0) p += 6;
    else if (strncmp(p, "int ", 4) == 0) p += 4;
    else if (strncmp(p, "double ", 7) == 0) p += 7;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Variable name */
    int name_len = 0;
    while (*p && repl_eval_is_ident_continue((unsigned char)*p) && name_len < var_sz - 1)
        var_name[name_len++] = *p++;
    var_name[name_len] = '\0';
    if (name_len == 0) return 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=') return 0;
    p++;

    /* Start value */
    ExprCtx ctx = { p, NULL, 0, NULL, 0 };
    *start = repl_eval_expr(&ctx);
    p = ctx.p;
    if (*p == 'f' || *p == 'F') p++;   /* skip C float suffix */
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ';') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Skip the loop variable in the condition (we don't validate it matches
     * the declared one - REPL trusts the imported C is well-formed). */
    while (*p && repl_eval_is_ident_continue((unsigned char)*p)) p++;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Comparison operator:
     *   is_greater=1  when condition is `>` or `>=` (loop counts down)
     *   include_end=1 when condition is `<=` or `>=` (inclusive bound) */
    int include_end = 0;
    int is_greater = 0;
    if (*p == '<') { p++; is_greater = 0; }
    else if (*p == '>') { p++; is_greater = 1; }
    else return 0;
    if (*p == '=') { p++; include_end = 1; }

    /* End value. REPL loops are half-open, so convert inclusive C bounds by
     * nudging the limit by one step in the iteration direction. */
    ctx.p = p;
    *end = repl_eval_expr(&ctx);
    if (include_end && !is_greater) *end += 1.0f;
    if (include_end && is_greater)  *end -= 1.0f;
    p = ctx.p;
    if (*p == 'f' || *p == 'F') p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ';') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Increment: skip variable name, then decode the step form. */
    while (*p && repl_eval_is_ident_continue((unsigned char)*p)) p++;
    while (*p && isspace((unsigned char)*p)) p++;

    *step = 1.0f;
    if (*p == '+' && *(p+1) == '+') {
        *step = 1.0f;
        p += 2;
    } else if (*p == '-' && *(p+1) == '-') {
        *step = -1.0f;
        p += 2;
    } else if (*p == '+' && *(p+1) == '=') {
        p += 2;
        ctx.p = p;
        *step = repl_eval_expr(&ctx);
        p = ctx.p;
        if (*p == 'f' || *p == 'F') p++;
    } else if (*p == '-' && *(p+1) == '=') {
        p += 2;
        ctx.p = p;
        *step = -repl_eval_expr(&ctx);
        p = ctx.p;
        if (*p == 'f' || *p == 'F') p++;
    } else {
        return 0;
    }
    /* A `>` condition with a positive step would never terminate - flip sign
     * so it counts down toward the bound. This helps termination when the
     * loop runs, but does not guarantee any iterations. */
    if (is_greater && *step > 0) *step = -*step;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ')') return 0;

    return 1;
}
