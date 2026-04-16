/*
 * repl_eval.c — Expression evaluator, translators, and for-loop parsers
 *
 * Compile standalone test:
 *   gcc -Wall -std=c2x -o test_eval test_eval.c repl_eval.c -lm
 *
 * Or include directly into sample.c via:
 *   #include "repl_eval.c"
 */
#include "repl_eval.h"

/* ========================================================================= */
/* Predefined variables                                                       */
/* ========================================================================= */

ExprVar g_predef_vars[MAX_PREDEF_VARS];
int     g_num_predef_vars = 0;

void init_predef_vars(void) {
    g_num_predef_vars = 1;
    strncpy(g_predef_vars[0].name, "t", sizeof(g_predef_vars[0].name) - 1);
    g_predef_vars[0].name[sizeof(g_predef_vars[0].name) - 1] = '\0';
    g_predef_vars[0].value = 0.0f;
}

int find_predef_var_idx(const char *name) {
    for (int i = 0; i < g_num_predef_vars; i++)
        if (strcmp(name, g_predef_vars[i].name) == 0)
            return i;
    return -1;
}

/* Keep in sync with eval_primary()'s function dispatch below */
static const char *s_reserved_idents[] = {
    "t", "PI", "TAU", "float", "var",
    "sin", "cos", "tan", "sqrt", "abs", "pow",
    "min", "max", "floor", "ceil", "fmod", "rand",
    NULL
};

int is_reserved_ident(const char *name) {
    for (const char **r = s_reserved_idents; *r; r++)
        if (strcmp(name, *r) == 0) return 1;
    return 0;
}

int declare_predef_var(const char *name, char *err, int errsz) {
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
    if (is_reserved_ident(name)) {
        if (err) snprintf(err, errsz, "'%s' is reserved", name);
        return 0;
    }
    if (find_predef_var_idx(name) >= 0) {
        if (err) snprintf(err, errsz, "'%s' already declared", name);
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

void undeclare_predef_var(const char *name) {
    int idx = find_predef_var_idx(name);
    if (idx < 0) return;
    for (int i = idx; i < g_num_predef_vars - 1; i++)
        g_predef_vars[i] = g_predef_vars[i + 1];
    g_num_predef_vars--;
    memset(&g_predef_vars[g_num_predef_vars], 0, sizeof(ExprVar));
}

int source_uses_ident(const char *src, const char *name) {
    int nlen = (int)strlen(name);
    const char *s = src;
    while (*s) {
        if (isdigit((unsigned char)*s) ||
            (*s == '.' && isdigit((unsigned char)s[1]))) {
            char *end;
            (void)strtof(s, &end);
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

int validate_expression_idents(const char *src, const ExprVar *vars,
                               int num_vars, char *err, int errsz) {
    const char *s = src;
    while (*s) {
        /* Stop at inline comments */
        if (s[0] == '/' && s[1] == '/') break;

        /* Skip number literals, including scientific notation like 1e-06. */
        if (isdigit((unsigned char)*s) ||
            (*s == '.' && isdigit((unsigned char)s[1]))) {
            char *end;
            (void)strtof(s, &end);
            if (end != s) {
                s = end;
                continue;
            }
        }
        if (!isalpha((unsigned char)*s) && *s != '_') { s++; continue; }
        const char *start = s;
        while (*s && (isalnum((unsigned char)*s) || *s == '_')) s++;
        int len = (int)(s - start);
        char name[16];
        if (len >= (int)sizeof(name)) {
            if (err) snprintf(err, errsz, "identifier too long");
            return 0;
        }
        memcpy(name, start, len);
        name[len] = '\0';

        if (strcmp(name, "PI") == 0 || strcmp(name, "TAU") == 0) continue;

        /* Skip function calls (identifier followed by '(') */
        const char *q = s;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q == '(') continue;

        /* Check loop/function locals */
        int found = 0;
        for (int i = 0; i < num_vars; i++)
            if (strcmp(name, vars[i].name) == 0) { found = 1; break; }
        if (found) continue;

        if (find_predef_var_idx(name) >= 0) continue;

        if (err) snprintf(err, errsz, "undeclared variable '%s'", name);
        return 0;
    }
    return 1;
}

int input_has_predef_vars(const char *s) {
    while (*s) {
        if (!isalpha((unsigned char)*s) && *s != '_') { s++; continue; }
        const char *start = s;
        while (*s && (isalnum((unsigned char)*s) || *s == '_')) s++;
        int len = (int)(s - start);
        for (int pv = 0; pv < g_num_predef_vars; pv++) {
            int nlen = (int)strlen(g_predef_vars[pv].name);
            if (nlen == len && strncmp(start, g_predef_vars[pv].name, len) == 0)
                return 1;
        }
    }
    return 0;
}

/* ========================================================================= */
/* Expression evaluator — recursive descent with variables                    */
/* ========================================================================= */

static void expr_skip_ws(ExprCtx *ctx) {
    while (*ctx->p && isspace((unsigned char)*ctx->p)) ctx->p++;
}

static float expr_rand01(float seed, float iter) {
    float h = sinf(seed * 12.9898f + iter * 78.233f) * 43758.5453f;
    float frac = h - floorf(h);
    if (frac < 0.0f) frac += 1.0f;
    return frac;
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
        float v = eval_expr(ctx);
        expr_skip_ws(ctx);
        if (*ctx->p == ')') ctx->p++;
        return v;
    }

    /* Number literal */
    if (isdigit((unsigned char)*ctx->p) || *ctx->p == '.') {
        char *end;
        float v = strtof(ctx->p, &end);
        ctx->p = end;
        return v;
    }

    /* Identifier: constant, variable, or function */
    if (isalpha((unsigned char)*ctx->p) || *ctx->p == '_') {
        char name[32];
        int ni = 0;
        while ((isalnum((unsigned char)*ctx->p) || *ctx->p == '_') &&
               ni < (int)sizeof(name) - 1)
            name[ni++] = *ctx->p++;
        name[ni] = '\0';

        /* Constants */
        if (strcmp(name, "PI") == 0)  return (float)M_PI;
        if (strcmp(name, "TAU") == 0) return (float)(2.0 * M_PI);

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
        expr_skip_ws(ctx);
        if (*ctx->p == '(') {
            ctx->p++;
            float a = eval_expr(ctx);
            float b = 0;
            int has_b = 0;
            expr_skip_ws(ctx);
            if (*ctx->p == ',') { ctx->p++; b = eval_expr(ctx); has_b = 1; }
            expr_skip_ws(ctx);
            if (*ctx->p == ')') ctx->p++;

            if (strcmp(name, "sin") == 0)  return sinf(a);
            if (strcmp(name, "cos") == 0)  return cosf(a);
            if (strcmp(name, "tan") == 0)  return tanf(a);
            if (strcmp(name, "sqrt") == 0) return sqrtf(fabsf(a));
            if (strcmp(name, "abs") == 0)  return fabsf(a);
            if (strcmp(name, "pow") == 0 && has_b) return powf(a, b);
            if (strcmp(name, "min") == 0 && has_b) return a < b ? a : b;
            if (strcmp(name, "max") == 0 && has_b) return a > b ? a : b;
            if (strcmp(name, "floor") == 0) return floorf(a);
            if (strcmp(name, "ceil") == 0)  return ceilf(a);
            if (strcmp(name, "fmod") == 0 && has_b) return fmodf(a, b);
            if (strcmp(name, "rand") == 0) return has_b ? expr_rand01(a, b) : expr_rand01(a, 0.0f);
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

float eval_expr(ExprCtx *ctx) {
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

int parse_exprs(const char *s, float *out, int max,
                ExprVar *vars, int num_vars) {
    int n = 0;
    const char *p = s;
    while (*p && n < max) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p || *p == ')') break;
        ExprCtx ctx = { p, vars, num_vars };
        out[n] = eval_expr(&ctx);
        if (ctx.p == p) break;   /* no progress */
        n++;
        p = ctx.p;
    }
    return n;
}

/* ========================================================================= */
/* Expression translation: REPL <-> C                                         */
/* ========================================================================= */

void repl_expr_to_c(const char *in, char *out, int out_sz) {
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
        { "rand",  "repl_randf", 1 },
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
}

void c_expr_to_repl(const char *in, char *out, int out_sz) {
    /* First pass: substring replacement for (2*M_PI) -> TAU */
    char tmp[MAX_LINE_LEN];
    strncpy(tmp, in, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

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
        strncpy(tmp, buf, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
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
}

/* ========================================================================= */
/* For-loop header parsers                                                    */
/* ========================================================================= */

int parse_for_header_with_vars(const char *input, char *var_name, int var_sz,
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
    int ni = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && ni < var_sz - 1)
        var_name[ni++] = *p++;
    var_name[ni] = '\0';
    if (ni == 0) return 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ',') return 0;
    p++;

    /* Start value (expression) */
    ExprCtx ctx = { p, vars, num_vars };
    *start = eval_expr(&ctx);
    p = ctx.p;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ',') return 0;
    p++;

    /* End value (expression) */
    ctx.p = p;
    ctx.vars = vars;
    ctx.num_vars = num_vars;
    *end = eval_expr(&ctx);
    p = ctx.p;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Optional step */
    *step = 1.0f;
    if (*p == ',') {
        p++;
        ctx.p = p;
        ctx.vars = vars;
        ctx.num_vars = num_vars;
        *step = eval_expr(&ctx);
        p = ctx.p;
        while (*p && isspace((unsigned char)*p)) p++;
    }

    if (*p != ')') return 0;
    p++;

    if (body_start) *body_start = p;
    return 1;
}

int parse_for_header(const char *input, char *var_name, int var_sz,
                     float *start, float *end, float *step,
                     const char **body_start) {
    return parse_for_header_with_vars(input, var_name, var_sz,
                                      start, end, step,
                                      NULL, 0, body_start);
}

int parse_c_for_header(const char *input, char *var_name, int var_sz,
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
    int ni = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && ni < var_sz - 1)
        var_name[ni++] = *p++;
    var_name[ni] = '\0';
    if (ni == 0) return 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=') return 0;
    p++;

    /* Start value */
    ExprCtx ctx = { p, NULL, 0 };
    *start = eval_expr(&ctx);
    p = ctx.p;
    if (*p == 'f' || *p == 'F') p++;   /* skip C float suffix */
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ';') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Skip variable name in condition */
    const char *cond_var = p;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
    (void)cond_var;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Comparison operator */
    int eq = 0;
    int gt = 0;
    if (*p == '<') { p++; gt = 0; }
    else if (*p == '>') { p++; gt = 1; }
    else return 0;
    if (*p == '=') { p++; eq = 1; }

    /* End value */
    ctx.p = p;
    *end = eval_expr(&ctx);
    if (eq && !gt) *end += 1.0f;
    if (eq && gt) *end -= 1.0f;
    p = ctx.p;
    if (*p == 'f' || *p == 'F') p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ';') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Increment: skip variable name */
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
        *step = eval_expr(&ctx);
        p = ctx.p;
        if (*p == 'f' || *p == 'F') p++;
    } else if (*p == '-' && *(p+1) == '=') {
        p += 2;
        ctx.p = p;
        *step = -eval_expr(&ctx);
        p = ctx.p;
        if (*p == 'f' || *p == 'F') p++;
    } else {
        return 0;
    }
    if (gt && *step > 0) *step = -*step;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ')') return 0;

    return 1;
}
