/*
 * repl_eval.c - Expression evaluator, REPL<->C translators, for-loop parsers.
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
#include "repl_eval.h"

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

ReplPredefView repl_eval_predef_view(void) {
    return (ReplPredefView){ .vars = g_predef_vars, .count = g_num_predef_vars };
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

int repl_func_alias_name_is_valid(const char *name) {
    if (!name || !*name) return 0;
    /* Must be a C identifier. */
    if (!isalpha((unsigned char)name[0]) && name[0] != '_') return 0;
    int len = 0;
    for (const char *p = name; *p; p++, len++) {
        if (!isalnum((unsigned char)*p) && *p != '_') return 0;
    }
    if (len >= REPL_FUNC_NAME_MAX) return 0;
    /* Reject the bare slot names — those are the underlying form. */
    if (len == 5 && strncmp(name, "func", 4) == 0 &&
        name[4] >= '0' && name[4] <= '9')
        return 0;
    /* Reject the language-reserved set (predef vars `t`, constants
     * PI/TAU, math funcs sin/cos/..., scratch A/B/C, type keywords). */
    if (repl_eval_is_reserved_ident(name)) return 0;
    /* Reject control-flow keywords. These have their own commit
     * handlers (try_commit_for_loop, try_commit_if_block) and would
     * otherwise hijack their syntax if a user typed `if(...)` after
     * accidentally declaring an alias by the same name. */
    static const char *const control_flow_kw[] = {
        "if", "for", "goto", NULL
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

void repl_func_alias_clear(int slot) {
    if (slot < 0 || slot >= REPL_FUNC_SLOT_COUNT) return;
    g_active_func_aliases[slot][0] = '\0';
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

static const char *skip_numeric_literal(const char *s) {
    char *end = NULL;
    (void)strtof(s, &end);
    if (end == s)
        return s;
    if (*end == 'f' || *end == 'F')
        end++;
    return end;
}

void repl_eval_init_predef_vars(void) {
    g_num_predef_vars = 1;
    strncpy(g_predef_vars[0].name, "t", sizeof(g_predef_vars[0].name) - 1);
    g_predef_vars[0].name[sizeof(g_predef_vars[0].name) - 1] = '\0';
    g_predef_vars[0].value = 0.0f;
}

int repl_eval_find_predef_var_idx(const char *name) {
    for (int i = 0; i < g_num_predef_vars; i++)
        if (strcmp(name, g_predef_vars[i].name) == 0)
            return i;
    return -1;
}

/* Keep in sync with eval_primary()'s function dispatch below */
static const char *s_reserved_idents[] = {
    "t", "PI", "TAU", "float", "var",
    "sin", "cos", "tan", "sqrt", "abs", "pow",
    "min", "max", "floor", "ceil", "fmod", "rem", "rand", "rand2",
    "A", "B", "C",
    NULL
};

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
    const char *p = src;
    char *out = dst;
    char *end = dst + dst_sz - 1;

    if (!src || !dst || dst_sz <= 0)
        return;

    while (*p && out < end) {
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *id_start = p;

            while (*p && (isalnum((unsigned char)*p) || *p == '_'))
                p++;

            int id_len = (int)(p - id_start);
            int is_scratch_name = id_len == 1 &&
                repl_eval_scratch_array_index(*id_start == 'A' ? "A" :
                                              (*id_start == 'B' ? "B" :
                                               (*id_start == 'C' ? "C" : ""))) >= 0;

            if (is_scratch_name && *p == '[') {
                const char *close = find_matching_square(p, NULL);
                if (close) {
                    int inner_len = (int)(close - (p + 1));
                    char inner[MAX_LINE_LEN];
                    char inner_c[MAX_LINE_LEN];
                    size_t avail;
                    int wrote;

                    if (inner_len >= (int)sizeof(inner))
                        inner_len = (int)sizeof(inner) - 1;
                    memcpy(inner, p + 1, (size_t)inner_len);
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
            continue;
        }

        *out++ = *p++;
    }

    *out = '\0';
}

static void expr_rewrite_scratch_subscripts_to_repl(const char *src,
                                                    char *dst,
                                                    int dst_sz) {
    const char *p = src;
    char *out = dst;
    char *end = dst + dst_sz - 1;

    if (!src || !dst || dst_sz <= 0)
        return;

    while (*p && out < end) {
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *id_start = p;

            while (*p && (isalnum((unsigned char)*p) || *p == '_'))
                p++;

            int id_len = (int)(p - id_start);
            int is_scratch_name = id_len == 1 &&
                repl_eval_scratch_array_index(*id_start == 'A' ? "A" :
                                              (*id_start == 'B' ? "B" :
                                               (*id_start == 'C' ? "C" : ""))) >= 0;

            if (is_scratch_name && *p == '[') {
                const char *close = find_matching_square(p, NULL);
                if (close) {
                    int inner_len = (int)(close - (p + 1));
                    const char *inner_src = p + 1;
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
            continue;
        }

        *out++ = *p++;
    }

    *out = '\0';
}

static int expr_range_has_runtime_values(const char *src, const char *end,
                                         const ExprVar *vars, int num_vars) {
    const char *s = src;

    while (s && *s && (!end || s < end)) {
        if (s[0] == '/' && (!end || s + 1 < end) && s[1] == '/')
            break;

        if (isdigit((unsigned char)*s) ||
            (*s == '.' && (!end || s + 1 < end) && isdigit((unsigned char)s[1]))) {
            const char *next = skip_numeric_literal(s);
            if (next != s) {
                s = next;
                continue;
            }
        }

        if (!isalpha((unsigned char)*s) && *s != '_') {
            s++;
            continue;
        }

        const char *start = s;
        while (*s && (!end || s < end) &&
               (isalnum((unsigned char)*s) || *s == '_'))
            s++;

        int len = (int)(s - start);
        char name[16];
        if (len <= 0 || len >= (int)sizeof(name))
            return 1;

        memcpy(name, start, (size_t)len);
        name[len] = '\0';

        if (strcmp(name, "PI") == 0 || strcmp(name, "TAU") == 0)
            continue;

        const char *q = skip_ws_ptr(s);
        if (repl_eval_scratch_array_index(name) >= 0)
            return 1;
        if (*q == '(')
            continue;

        for (int var_idx = 0; var_idx < num_vars; var_idx++) {
            if (strcmp(name, vars[var_idx].name) == 0)
                return 1;
        }
        if (repl_eval_find_predef_var_idx(name) >= 0)
            return 1;
    }

    return 0;
}

static int validate_expression_idents_range(const char *src, const char *end,
                                            const ExprVar *vars, int num_vars,
                                            char *err, int errsz) {
    const char *s = src;

    while (s && *s && (!end || s < end)) {
        if (s[0] == '/' && (!end || s + 1 < end) && s[1] == '/')
            break;

        if (isdigit((unsigned char)*s) ||
            (*s == '.' && (!end || s + 1 < end) && isdigit((unsigned char)s[1]))) {
            const char *next = skip_numeric_literal(s);
            if (next != s) {
                s = next;
                continue;
            }
        }

        if (!isalpha((unsigned char)*s) && *s != '_') {
            s++;
            continue;
        }

        const char *start = s;
        while (*s && (!end || s < end) &&
               (isalnum((unsigned char)*s) || *s == '_'))
            s++;

        int len = (int)(s - start);
        char name[16];
        if (len >= (int)sizeof(name)) {
            if (err)
                snprintf(err, (size_t)errsz, "identifier too long");
            return 0;
        }

        memcpy(name, start, (size_t)len);
        name[len] = '\0';

        if (strcmp(name, "PI") == 0 || strcmp(name, "TAU") == 0)
            continue;

        const char *q = skip_ws_ptr(s);
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

            if (!validate_expression_idents_range(q + 1, close,
                                                  vars, num_vars,
                                                  err, errsz))
                return 0;

            if (!expr_range_has_runtime_values(q + 1, close, vars, num_vars)) {
                char idx_expr[MAX_LINE_LEN];
                int idx_len = (int)(close - (q + 1));
                if (idx_len >= (int)sizeof(idx_expr))
                    idx_len = (int)sizeof(idx_expr) - 1;
                memcpy(idx_expr, q + 1, (size_t)idx_len);
                idx_expr[idx_len] = '\0';

                ExprCtx idx_ctx = { idx_expr, vars, num_vars, NULL, 0 };
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

        if (repl_eval_find_predef_var_idx(name) >= 0)
            continue;

        if (err)
            snprintf(err, (size_t)errsz, "undeclared variable '%s'", name);
        return 0;
    }

    return 1;
}

int repl_eval_is_reserved_ident(const char *name) {
    for (const char **r = s_reserved_idents; *r; r++)
        if (strcmp(name, *r) == 0) return 1;
    return 0;
}

int repl_eval_declare_predef_var(const char *name, char *err, int errsz) {
    if (!name || !name[0]) {
        if (err) snprintf(err, errsz, "empty variable name");
        return 0;
    }
    if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) {
        if (err) snprintf(err, errsz, "invalid identifier '%s'", name);
        return 0;
    }
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_') {
            if (err) snprintf(err, errsz, "invalid identifier '%s'", name);
            return 0;
        }
    }
    if (strlen(name) >= sizeof(g_predef_vars[0].name)) {
        if (err) snprintf(err, errsz, "name '%s' too long (max %d chars)",
                          name, (int)sizeof(g_predef_vars[0].name) - 1);
        return 0;
    }
    if (repl_eval_is_reserved_ident(name)) {
        if (err) snprintf(err, errsz, "'%s' is reserved", name);
        return 0;
    }
    if (repl_eval_find_predef_var_idx(name) >= 0) {
        if (err) snprintf(err, errsz, "'%s' already declared", name);
        return 0;
    }
    if (repl_func_alias_lookup_slot(name) >= 0) {
        if (err) snprintf(err, errsz,
                          "'%s' is in use as a function name", name);
        return 0;
    }
    if (g_num_predef_vars >= MAX_PREDEF_VARS) {
        if (err) snprintf(err, errsz, "variable table full (max %d)", MAX_PREDEF_VARS);
        return 0;
    }
    strncpy(g_predef_vars[g_num_predef_vars].name, name,
            sizeof(g_predef_vars[0].name) - 1);
    g_predef_vars[g_num_predef_vars].name[sizeof(g_predef_vars[0].name) - 1] = '\0';
    g_predef_vars[g_num_predef_vars].value = 0.0f;
    g_num_predef_vars++;
    return 1;
}

void repl_eval_undeclare_predef_var(const char *name) {
    int idx = repl_eval_find_predef_var_idx(name);
    if (idx < 0) return;
    for (int i = idx; i < g_num_predef_vars - 1; i++)
        g_predef_vars[i] = g_predef_vars[i + 1];
    g_num_predef_vars--;
    memset(&g_predef_vars[g_num_predef_vars], 0, sizeof(ExprVar));
}

int repl_eval_source_uses_ident(const char *src, const char *name) {
    int nlen = (int)strlen(name);
    const char *s = src;
    while (*s) {
        /* Stop at inline `//` — the rest of the line is a comment,
         * not real code. Mirrors the expression validator at
         * validate_expression_idents_range. */
        if (s[0] == '/' && s[1] == '/')
            break;
        if (isdigit((unsigned char)*s) ||
            (*s == '.' && isdigit((unsigned char)s[1]))) {
            const char *end = skip_numeric_literal(s);
            if (end != s) { s = end; continue; }
        }
        if (!isalpha((unsigned char)*s) && *s != '_') { s++; continue; }
        const char *start = s;
        while (*s && (isalnum((unsigned char)*s) || *s == '_')) s++;
        int len = (int)(s - start);
        if (len == nlen && strncmp(start, name, nlen) == 0)
            return 1;
    }
    return 0;
}

int repl_eval_validate_expression_idents(const char *src, const ExprVar *vars,
                               int num_vars, char *err, int errsz) {
    return validate_expression_idents_range(src, NULL, vars, num_vars,
                                            err, errsz);
}

int repl_eval_input_has_predef_vars(const char *s) {
    while (*s) {
        if (s[0] == '/' && s[1] == '/')
            break;
        if (!isalpha((unsigned char)*s) && *s != '_') { s++; continue; }
        const char *start = s;
        while (*s && (isalnum((unsigned char)*s) || *s == '_')) s++;
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

static float expr_rand01(float seed, float iter) {
    float h = sinf(seed * 12.9898f + iter * 78.233f) * 43758.5453f;
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
    if (isalpha((unsigned char)*ctx->p) || *ctx->p == '_') {
        char name[32];
        int name_len = 0;
        while ((isalnum((unsigned char)*ctx->p) || *ctx->p == '_') &&
               name_len < (int)sizeof(name) - 1)
            name[name_len++] = *ctx->p++;
        name[name_len] = '\0';
        const char *q = skip_ws_ptr(ctx->p);

        /* Constants */
        if (strcmp(name, "PI") == 0)  return (float)M_PI;
        if (strcmp(name, "TAU") == 0) return (float)(2.0 * M_PI);

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

        /* Predefined variables (checked after loop vars) */
        for (int i = 0; i < g_num_predef_vars; i++)
            if (strcmp(name, g_predef_vars[i].name) == 0)
                return g_predef_vars[i].value;

        /* Functions (consume opening paren) */
        if (*q == '(') {
            float args[3] = { 0.0f, 0.0f, 0.0f };
            int arg_count = 0;

            ctx->p = q + 1;
            expr_skip_ws(ctx);
            if (*ctx->p != ')') {
                for (;;) {
                    float arg = repl_eval_expr(ctx);
                    if (arg_count < (int)(sizeof(args) / sizeof(args[0])))
                        args[arg_count] = arg;
                    arg_count++;

                    expr_skip_ws(ctx);
                    if (*ctx->p != ',')
                        break;
                    ctx->p++;
                    expr_skip_ws(ctx);
                }
            }
            expr_skip_ws(ctx);
            if (*ctx->p == ')')
                ctx->p++;

            if (strcmp(name, "sin") == 0 && arg_count == 1)  return sinf(args[0]);
            if (strcmp(name, "cos") == 0 && arg_count == 1)  return cosf(args[0]);
            if (strcmp(name, "tan") == 0 && arg_count == 1)  return tanf(args[0]);
            if (strcmp(name, "sqrt") == 0 && arg_count == 1) return sqrtf(fabsf(args[0]));
            if (strcmp(name, "abs") == 0 && arg_count == 1)  return fabsf(args[0]);
            if (strcmp(name, "pow") == 0 && arg_count == 2) return powf(args[0], args[1]);
            if (strcmp(name, "min") == 0 && arg_count == 2) return args[0] < args[1] ? args[0] : args[1];
            if (strcmp(name, "max") == 0 && arg_count == 2) return args[0] > args[1] ? args[0] : args[1];
            if (strcmp(name, "floor") == 0 && arg_count == 1) return floorf(args[0]);
            if (strcmp(name, "ceil") == 0 && arg_count == 1)  return ceilf(args[0]);
            if (strcmp(name, "fmod") == 0 && arg_count == 2) return fmodf(args[0], args[1]);
            if (strcmp(name, "rem") == 0 && arg_count == 2) return remainderf(args[0], args[1]);
            if (strcmp(name, "rand") == 0)
                return arg_count >= 2 ? expr_rand01(args[0], args[1])
                                      : (arg_count == 1 ? expr_rand01(args[0], 0.0f) : 0.0f);
            if (strcmp(name, "rand2") == 0)
                return arg_count >= 2 ? expr_rand_signed(args[0], args[1])
                                      : (arg_count == 1 ? expr_rand_signed(args[0], 0.0f) : 0.0f);
        }

        return 0.0f;   /* unknown identifier */
    }

    return 0.0f;
}

static float eval_term(ExprCtx *ctx) {
    float v = eval_primary(ctx);
    for (;;) {
        expr_skip_ws(ctx);
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
                ExprVar *vars, int num_vars) {
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

/* ========================================================================= */
/* Expression translation: REPL <-> C                                         */
/* ========================================================================= */

void repl_eval_expr_to_c(const char *in, char *out, int out_sz) {
    static const struct { const char *from; const char *to; int is_func; } map[] = {
        { "sin",   "sinf",   1 },
        { "cos",   "cosf",   1 },
        { "tan",   "tanf",   1 },
        { "sqrt",  "sqrtf",  1 },
        { "abs",   "fabsf",  1 },
        { "pow",   "powf",   1 },
        { "min",   "fminf",  1 },
        { "max",   "fmaxf",  1 },
        { "floor", "floorf", 1 },
        { "ceil",  "ceilf",  1 },
        { "fmod",  "fmodf",  1 },
        { "rem",   "remainderf", 1 },
        { "rand",  "repl_randf", 1 },
        { "rand2", "repl_rand2f", 1 },
        { "TAU",   "(2*M_PI)", 0 },
        { "PI",    "M_PI",     0 },
    };
    int nmap = (int)(sizeof(map) / sizeof(map[0]));
    const char *p = in;
    char *dst = out;
    char *end = out + out_sz - 1;

    while (*p && dst < end) {
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *id_start = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            int id_len = (int)(p - id_start);
            int found = 0;
            for (int i = 0; i < nmap; i++) {
                if ((int)strlen(map[i].from) == id_len &&
                    strncmp(id_start, map[i].from, id_len) == 0) {
                    int rlen = (int)strlen(map[i].to);
                    if (dst + rlen < end) {
                        memcpy(dst, map[i].to, rlen);
                        dst += rlen;
                    }
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (dst + id_len < end) {
                    memcpy(dst, id_start, id_len);
                    dst += id_len;
                }
            }
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
                       (isalnum((unsigned char)lhs_start[-1]) || lhs_start[-1] == '_'))
                    lhs_start--;
            } else {
                while (lhs_start > out &&
                       (isalnum((unsigned char)lhs_start[-1]) ||
                        lhs_start[-1] == '_' || lhs_start[-1] == '.'))
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
}

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

    /* Identifier-aware replacement: sinf->sin, M_PI->PI, etc. */
    static const struct { const char *from; const char *to; } map[] = {
        { "sinf",   "sin"   },
        { "cosf",   "cos"   },
        { "tanf",   "tan"   },
        { "sqrtf",  "sqrt"  },
        { "fabsf",  "abs"   },
        { "powf",   "pow"   },
        { "fminf",  "min"   },
        { "fmaxf",  "max"   },
        { "floorf", "floor" },
        { "ceilf",  "ceil"  },
        { "fmodf",  "fmod"  },
        { "remainderf", "rem" },
        { "repl_rand2f", "rand2" },
        { "repl_randf", "rand" },
        { "M_PI",   "PI"    },
    };
    int nmap = (int)(sizeof(map) / sizeof(map[0]));
    const char *p = tmp;
    char *dst = out;
    char *end = out + out_sz - 1;

    while (*p && dst < end) {
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *id_start = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            int id_len = (int)(p - id_start);
            int found = 0;
            for (int i = 0; i < nmap; i++) {
                if ((int)strlen(map[i].from) == id_len &&
                    strncmp(id_start, map[i].from, id_len) == 0) {
                    int rlen = (int)strlen(map[i].to);
                    if (dst + rlen < end) {
                        memcpy(dst, map[i].to, rlen);
                        dst += rlen;
                    }
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (dst + id_len < end) {
                    memcpy(dst, id_start, id_len);
                    dst += id_len;
                }
            }
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

int repl_eval_parse_for_header_with_vars(const char *input, char *var_name, int var_sz,
                               float *start, float *end, float *step,
                               ExprVar *vars, int num_vars,
                               const char **body_start) {
    const char *p = input;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "for(", 4) != 0 && strncmp(p, "for (", 5) != 0)
        return 0;

    while (*p && *p != '(') p++;
    if (!*p) return 0;
    p++;

    /* Variable name */
    while (*p && isspace((unsigned char)*p)) p++;
    int name_len = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && name_len < var_sz - 1)
        var_name[name_len++] = *p++;
    var_name[name_len] = '\0';
    if (name_len == 0) return 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ',') return 0;
    p++;

    /* Start and end expressions share the same ExprCtx; we only need to
     * re-seat ctx.p at each argument boundary since eval_expr advances it. */
    ExprCtx ctx = { p, vars, num_vars, NULL, 0 };
    *start = repl_eval_expr(&ctx);
    p = ctx.p;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ',') return 0;
    p++;

    ctx.p = p;
    *end = repl_eval_expr(&ctx);
    p = ctx.p;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Optional step (defaults to 1). */
    *step = 1.0f;
    if (*p == ',') {
        p++;
        ctx.p = p;
        *step = repl_eval_expr(&ctx);
        p = ctx.p;
        while (*p && isspace((unsigned char)*p)) p++;
    }

    if (*p != ')') return 0;
    p++;

    if (body_start) *body_start = p;
    return 1;
}

int repl_eval_parse_for_header(const char *input, char *var_name, int var_sz,
                     float *start, float *end, float *step,
                     const char **body_start) {
    return repl_eval_parse_for_header_with_vars(input, var_name, var_sz,
                                      start, end, step,
                                      NULL, 0, body_start);
}

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
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && name_len < var_sz - 1)
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
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
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
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
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
