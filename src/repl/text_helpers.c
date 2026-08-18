#include "repl/text_helpers.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void trim_in_place(char *s) {
    int start = 0;
    int len = (int)strlen(s);
    while (start < len && isspace((unsigned char)s[start])) start++;
    while (len > start && isspace((unsigned char)s[len - 1])) len--;
    if (start > 0) memmove(s, s + start, (size_t)(len - start));
    s[len - start] = '\0';
}

void repl_canonical_input_view(const char *src,
                               const char **out_start,
                               int *out_len) {
    const char *s = src ? src : "";
    while (*s && isspace((unsigned char)*s)) s++;
    int len = (int)strlen(s);
    while (len > 0 &&
           (s[len - 1] == ';' || isspace((unsigned char)s[len - 1])))
        len--;
    if (out_start) *out_start = s;
    if (out_len)   *out_len   = len;
}

void repl_format_source_float(char *out, int out_sz, float v) {
    if (!out || out_sz <= 0)
        return;
    if (isnan(v)) {
        snprintf(out, (size_t)out_sz, "NAN");
        return;
    }
    if (isinf(v)) {
        snprintf(out, (size_t)out_sz, "%sINFINITY", v < 0.0f ? "-" : "");
        return;
    }
    if (v == 0.0f) {
        /* Preserve IEEE signed zero: canonical parser text is reparsed and
         * compared bit-for-bit with the committed float args. Spell the
         * negative case as a floating literal so generated C preserves it
         * too (`-0` would be integer zero before conversion). */
        snprintf(out, (size_t)out_sz, "%s", signbit(v) ? "-0.0" : "0");
        return;
    }

    for (int prec = 0; prec <= 9; prec++) {
        char candidate[64];
        char *dot;
        char *end = NULL;

        snprintf(candidate, sizeof(candidate), "%.*f", prec, (double)v);
        dot = strchr(candidate, '.');
        if (dot) {
            char *tail = candidate + strlen(candidate) - 1;
            while (tail > dot && *tail == '0') {
                *tail = '\0';
                tail--;
            }
            if (*tail == '.')
                *tail = '\0';
        }
        /* Large finite floats can have exact fixed-point spellings longer
         * than REPL_SOURCE_FLOAT_TEXT_MAX (e.g. 1e38f). Do not validate the
         * full scratch candidate and then silently truncate it into `out`;
         * let the compact %g search below choose an exponent form instead. */
        if ((int)strlen(candidate) < out_sz &&
            strtof(candidate, &end) == v && end && *end == '\0') {
            snprintf(out, (size_t)out_sz, "%s", candidate);
            return;
        }
    }

    for (int prec = 1; prec <= 9; prec++) {
        char candidate[32];
        char *end = NULL;

        snprintf(candidate, sizeof(candidate), "%.*g", prec, (double)v);
        if ((int)strlen(candidate) < out_sz &&
            strtof(candidate, &end) == v && end && *end == '\0') {
            snprintf(out, (size_t)out_sz, "%s", candidate);
            return;
        }
    }

    snprintf(out, (size_t)out_sz, "%.9g", (double)v);
}

/* Scientific fallback that is always REPL_LABEL_FLOAT_WIDTH characters:
 *   |exp| <= 9  →   1.2e3 /  1.2-4
 *   |exp| >= 10 →   1e+38 /  1e-38
 * Positives keep a leading space so they stay aligned with '-'. */
static void format_label_float_sci(char *field, float v) {
    int neg = signbit(v);
    float a = fabsf(v);
    int exp = (int)floorf(log10f(a));
    float m = a / powf(10.0f, (float)exp);
    if (m < 1.0f) {
        m *= 10.0f;
        exp--;
    } else if (m >= 10.0f) {
        m /= 10.0f;
        exp++;
    }

    if (exp > 9 || exp < -9) {
        int d = (int)(m + 0.5f);
        if (d >= 10) {
            d = 1;
            exp++;
        }
        snprintf(field, REPL_LABEL_FLOAT_WIDTH + 1, "%c%de%+03d",
                 neg ? '-' : ' ', d, exp);
        return;
    }

    int md = (int)(m * 10.0f + 0.5f);
    if (md >= 100) {
        md = 10;
        exp++;
        if (exp > 9) {
            snprintf(field, REPL_LABEL_FLOAT_WIDTH + 1, "%c1e%+03d",
                     neg ? '-' : ' ', exp);
            return;
        }
    }
    if (exp >= 0)
        snprintf(field, REPL_LABEL_FLOAT_WIDTH + 1, "%c%d.%de%d",
                 neg ? '-' : ' ', md / 10, md % 10, exp);
    else
        snprintf(field, REPL_LABEL_FLOAT_WIDTH + 1, "%c%d.%d-%d",
                 neg ? '-' : ' ', md / 10, md % 10, -exp);
}

/* One %f field: exactly REPL_LABEL_FLOAT_WIDTH characters. A leading
 * space stands in for '+', so ` 1.250` stays aligned with `-1.250`.
 * Prefer fixed point (` 1.250`, ` 12.34`, ` 100.0`, ` 1000.`);
 * scientific only when that would not fit or would collapse a
 * non-zero value to ±0.000. */
static int format_label_float(char *out, int out_sz, float v) {
    char field[REPL_LABEL_FLOAT_WIDTH + 1];
    int n;

    if (!out || out_sz <= 0)
        return 0;

    if (isnan(v)) {
        memcpy(field, signbit(v) ? "-  nan" : "   nan",
               REPL_LABEL_FLOAT_WIDTH);
        field[REPL_LABEL_FLOAT_WIDTH] = '\0';
    } else if (isinf(v)) {
        memcpy(field, v < 0.0f ? "-  inf" : "   inf",
               REPL_LABEL_FLOAT_WIDTH);
        field[REPL_LABEL_FLOAT_WIDTH] = '\0';
    } else {
        static const char *const fixed[] = {
            "% .3f", "% .2f", "% .1f", "% .0f",
        };
        int accepted = 0;
        int i;
        for (i = 0; i < (int)(sizeof(fixed) / sizeof(fixed[0])); i++) {
            char scratch[32];
            int wrote = snprintf(scratch, sizeof(scratch), fixed[i],
                                 (double)v);
            if (wrote == REPL_LABEL_FLOAT_WIDTH) {
                if (i == 0 && v != 0.0f &&
                    (strcmp(scratch, " 0.000") == 0 ||
                     strcmp(scratch, "-0.000") == 0))
                    break;
                memcpy(field, scratch, REPL_LABEL_FLOAT_WIDTH + 1);
                accepted = 1;
                break;
            }
            if (wrote == REPL_LABEL_FLOAT_WIDTH - 1 &&
                i == (int)(sizeof(fixed) / sizeof(fixed[0])) - 1) {
                /*  1000 ..  9999 is 5 characters; trail a '.' to fill. */
                scratch[REPL_LABEL_FLOAT_WIDTH - 1] = '.';
                scratch[REPL_LABEL_FLOAT_WIDTH] = '\0';
                memcpy(field, scratch, REPL_LABEL_FLOAT_WIDTH + 1);
                accepted = 1;
                break;
            }
        }
        if (!accepted)
            format_label_float_sci(field, v);
    }

    n = REPL_LABEL_FLOAT_WIDTH;
    if (n > out_sz - 1)
        n = out_sz - 1;
    memcpy(out, field, (size_t)n);
    out[n] = '\0';
    return n;
}

int repl_format_label_string(char *out, int out_sz,
                             const char *fmt,
                             const float *args, int num_args) {
    if (!out || out_sz <= 0) return 0;
    out[0] = '\0';
    if (!fmt) return 0;

    int sub_count = num_args > 0 ? num_args : 0;
    int sub_idx = 0;
    int off = 0;

    while (*fmt && off < out_sz - 1) {
        if (fmt[0] == '%' && fmt[1] == 'f' && sub_idx < sub_count && args) {
            off += format_label_float(out + off, out_sz - off,
                                      args[sub_idx]);
            sub_idx++;
            fmt += 2;
        } else if (fmt[0] == '%' && fmt[1] == '%') {
            out[off++] = '%';
            fmt += 2;
        } else {
            out[off++] = *fmt++;
        }
    }
    out[off] = '\0';
    return off;
}

int repl_extract_paren_payload(const char *src, char *out, int out_sz) {
    const char *p = strchr(src, '(');
    if (!p) return 0;
    p++;
    const char *start = p;
    p = repl_scan_to_matching_paren(p);
    if (*p != ')') return 0;
    int n = (int)(p - start);
    if (n > out_sz - 1) n = out_sz - 1;
    memcpy(out, start, (size_t)n);
    out[n] = '\0';
    trim_in_place(out);
    return 1;
}

int extract_for_args_text(const char *src,
                          char *var, int var_sz,
                          char *args, int args_sz) {
    const char *p = strchr(src, '(');
    if (!p) return 0;
    p++;

    while (*p && isspace((unsigned char)*p)) p++;

    int vi = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && vi < var_sz - 1)
        var[vi++] = *p++;
    var[vi] = '\0';
    if (vi == 0) return 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ',') return 0;
    p++;

    const char *start = p;
    p = repl_scan_to_matching_paren(p);
    if (*p != ')') return 0;

    int n = (int)(p - start);
    if (n > args_sz - 1) n = args_sz - 1;
    memcpy(args, start, (size_t)n);
    args[n] = '\0';
    trim_in_place(args);
    return 1;
}

void repl_extract_for_var_name(const char *text, char *var, int var_sz) {
    const char *p = text ? text : "";
    int i = 0;

    if (!var || var_sz <= 0)
        return;

    while (*p && *p != '(') p++;
    if (*p) p++;
    while (*p && isspace((unsigned char)*p)) p++;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && i < var_sz - 1)
        var[i++] = *p++;
    var[i] = '\0';
}

int repl_parse_identifier_list(const char *src, const char *leading_keyword,
                               char names[][REPL_PREDEF_NAME_MAX], int max_names) {
    const char *p = src;
    int count = 0;
    size_t keyword_len = (leading_keyword && leading_keyword[0])
        ? strlen(leading_keyword) : 0;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (keyword_len > 0) {
            if (strncmp(p, leading_keyword, keyword_len) != 0 ||
                !isspace((unsigned char)p[keyword_len]))
                return -1;
            p += keyword_len;
            while (*p && isspace((unsigned char)*p)) p++;
        }
        if (count >= max_names) return -1;
        if (!isalpha((unsigned char)*p) && *p != '_') return -1;

        int ni = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
            if (ni >= REPL_PREDEF_NAME_MAX - 1) return -1;
            names[count][ni++] = *p++;
        }
        names[count][ni] = '\0';
        count++;

        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p != ',') return -1;
        p++;
    }

    return count;
}

int parse_expr_list_exact(const char *src, float *out_vals, int max_vals,
                          ExprVar *vars, int num_vars, int *out_count) {
    const char *p = src;
    int count = 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) {
        if (out_count) *out_count = 0;
        return 1;
    }

    for (;;) {
        ExprCtx ctx = { p, vars, num_vars };
        float value = repl_eval_expr(&ctx);
        if (ctx.p == p) return 0;
        if (count >= max_vals) return 0;
        if (out_vals) out_vals[count] = value;
        count++;

        p = ctx.p;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p != ',') return 0;
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) return 0;
    }

    if (out_count) *out_count = count;
    return 1;
}

const char *repl_scan_decl_float_prefix(const char *p, int *has_static) {
    if (has_static)
        *has_static = 0;
    if (!p)
        return NULL;
    while (*p && isspace((unsigned char)*p)) p++;
    /* Optional canonical `static ` prefix (see format_decl_text). */
    if (strncmp(p, "static", 6) == 0 && isspace((unsigned char)p[6])) {
        if (has_static)
            *has_static = 1;
        p += 6;
        while (*p && isspace((unsigned char)*p)) p++;
    }
    if (strncmp(p, "float", 5) != 0)
        return NULL;
    if (isalnum((unsigned char)p[5]) || p[5] == '_')
        return NULL;
    return p + 5;
}

int repl_scan_func_name_token(const char **p_inout, int *fn,
                              char ident[REPL_FUNC_NAME_MAX]) {
    const char *p = *p_inout;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "func", 4) == 0 &&
        p[4] >= '0' && p[4] <= '9' &&
        !isalnum((unsigned char)p[5]) && p[5] != '_') {
        if (fn) *fn = p[4] - '0';
        *p_inout = p + 5;
        return 1;
    }
    if (!isalpha((unsigned char)*p) && *p != '_') return 0;
    int len = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') &&
           len < REPL_FUNC_NAME_MAX - 1) {
        ident[len++] = *p++;
    }
    if (*p && (isalnum((unsigned char)*p) || *p == '_')) return 0;
    ident[len] = '\0';
    if (len == 0) return 0;
    *p_inout = p;
    return 2;
}

static int repl_parse_func_name_token_with_pending_alias(const char **p_inout,
                                                        int *fn,
                                                        const char *alias_name,
                                                        int alias_slot) {
    const char *p = *p_inout;
    char ident[REPL_FUNC_NAME_MAX];
    int kind = repl_scan_func_name_token(&p, fn, ident);
    if (kind == 0) return 0;
    if (kind == 2) {
        int slot = repl_func_alias_lookup_slot(ident);
        if (slot < 0 &&
            alias_name && alias_name[0] &&
            alias_slot >= 0 && alias_slot < REPL_FUNC_SLOT_COUNT &&
            strcmp(ident, alias_name) == 0) {
            slot = alias_slot;
        }
        if (slot < 0) return 0;
        if (fn) *fn = slot;
    }
    *p_inout = p;
    return 1;
}

int repl_parse_func_name_token(const char **p_inout, int *fn) {
    return repl_parse_func_name_token_with_pending_alias(p_inout, fn, NULL, -1);
}

int parse_repl_func_signature(const char *src, int *fn,
                              char param_names[][REPL_PREDEF_NAME_MAX], int max_params,
                              int *param_count) {
    return parse_repl_func_signature_with_pending_alias(
        src, NULL, -1, fn, param_names, max_params, param_count);
}

int parse_repl_func_signature_with_pending_alias(
                              const char *src, const char *alias_name, int alias_slot,
                              int *fn,
                              char param_names[][REPL_PREDEF_NAME_MAX], int max_params,
                              int *param_count) {
    const char *p = src;
    if (!repl_parse_func_name_token_with_pending_alias(&p, fn,
                                                       alias_name, alias_slot))
        return 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '{' || *p == '\0') {
        if (param_count) *param_count = 0;
        return 1;
    }
    if (*p != '(') return 0;

    const char *payload_start = ++p;
    p = repl_scan_to_matching_paren(p);
    if (*p != ')') return 0;

    char payload[MAX_LINE_LEN];
    int n = (int)(p - payload_start);
    if (n > (int)sizeof(payload) - 1) n = (int)sizeof(payload) - 1;
    memcpy(payload, payload_start, (size_t)n);
    payload[n] = '\0';
    trim_in_place(payload);

    while (*p == ')') p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{' && *p != '\0') return 0;

    if (!payload[0]) {
        if (param_count) *param_count = 0;
        return 1;
    }

    int count = repl_parse_identifier_list(payload, NULL,
                                           param_names, max_params);
    if (count < 0) return 0;
    if (param_count) *param_count = count;
    return 1;
}

int extract_func_call_args_text(const char *src, int *fn,
                                char *args, int args_sz) {
    const char *p = src;
    if (!repl_parse_func_name_token(&p, fn)) return 0;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '(') return 0;
    p++;
    const char *start = p;
    p = repl_scan_to_matching_paren(p);
    if (*p != ')') return 0;

    int n = (int)(p - start);
    if (n > args_sz - 1) n = args_sz - 1;
    memcpy(args, start, (size_t)n);
    args[n] = '\0';
    trim_in_place(args);

    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '\0' && *p != ';') return 0;
    return 1;
}

static void format_func_header_named(char *out, int out_sz, const char *indent,
                                     int fn, const char *alias_name,
                                     char param_names[][REPL_PREDEF_NAME_MAX],
                                     int param_count) {
    int written = (alias_name && alias_name[0])
        ? snprintf(out, out_sz, "%s%s", indent, alias_name)
        : snprintf(out, out_sz, "%sfunc%d", indent, fn);
    if (written < 0 || written >= out_sz) {
        if (out_sz > 0) out[out_sz - 1] = '\0';
        return;
    }
    if (written < out_sz)
        written += snprintf(out + written, out_sz - written, "(");
    for (int param_idx = 0; param_idx < param_count && written < out_sz; param_idx++) {
        written += snprintf(out + written, out_sz - written, "%s%s",
                            param_idx == 0 ? "" : ", ", param_names[param_idx]);
    }
    if (written < out_sz)
        written += snprintf(out + written, out_sz - written, ")");
    if (written < out_sz)
        snprintf(out + written, out_sz - written, " {");
}

void format_func_header(char *out, int out_sz, const char *indent,
                        int fn, char param_names[][REPL_PREDEF_NAME_MAX], int param_count) {
    format_func_header_named(out, out_sz, indent, fn, repl_func_alias_get(fn),
                             param_names, param_count);
}

void format_func_header_with_alias(char *out, int out_sz, const char *indent,
                                   int fn, char param_names[][REPL_PREDEF_NAME_MAX],
                                   int param_count, const char *alias_name) {
    format_func_header_named(out, out_sz, indent, fn, alias_name,
                             param_names, param_count);
}

int input_has_expr_vars(const char *s, ExprVar *vars, int num_vars) {
    while (*s) {
        if (!isalpha((unsigned char)*s) && *s != '_') { s++; continue; }
        const char *start = s;
        while (*s && (isalnum((unsigned char)*s) || *s == '_')) s++;
        int len = (int)(s - start);
        for (int var_idx = 0; var_idx < num_vars; var_idx++) {
            int nlen = (int)strlen(vars[var_idx].name);
            if (nlen == len && strncmp(start, vars[var_idx].name, len) == 0)
                return 1;
        }
    }
    return 0;
}

int input_has_any_visible_vars(const char *s, ExprVar *vars, int num_vars) {
    return repl_eval_input_has_predef_vars(s) || input_has_expr_vars(s, vars, num_vars);
}

int repl_extract_assignment_parts(const char *src,
                                  char *name, int name_sz,
                                  char *rhs, int rhs_sz) {
    char index_expr[MAX_LINE_LEN];

    if (!repl_extract_assignment_target_parts(src,
                                              name, name_sz,
                                              index_expr, sizeof(index_expr),
                                              rhs, rhs_sz))
        return 0;
    return index_expr[0] == '\0';
}

int repl_extract_assignment_target_parts(const char *src,
                                         char *name, int name_sz,
                                         char *index_expr, int index_expr_sz,
                                         char *rhs, int rhs_sz) {
    const char *p = src;
    const char *index_start = NULL;
    const char *index_end = NULL;
    const char *rhs_start;
    const char *rhs_end;
    const char *comment_start;
    int n = 0;

    while (*p && isspace((unsigned char)*p)) p++;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (name && n < name_sz - 1)
            name[n] = *p;
        n++;
        p++;
    }
    if (name && name_sz > 0)
        name[n < name_sz - 1 ? n : name_sz - 1] = '\0';
    if (n == 0)
        return 0;

    if (index_expr && index_expr_sz > 0)
        index_expr[0] = '\0';

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '[') {
        int depth = 1;
        index_start = ++p;
        while (*p && depth > 0) {
            if (*p == '[')
                depth++;
            else if (*p == ']')
                depth--;
            if (depth > 0)
                p++;
        }
        if (depth != 0 || !*p)
            return 0;
        index_end = p;
        p++;

        if (index_expr && index_expr_sz > 0) {
            int idx_len = (int)(index_end - index_start);
            if (idx_len > index_expr_sz - 1)
                idx_len = index_expr_sz - 1;
            memcpy(index_expr, index_start, (size_t)idx_len);
            index_expr[idx_len] = '\0';
            trim_in_place(index_expr);
            if (!index_expr[0])
                return 0;
        }
    }

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=' || p[1] == '=')
        return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p)
        return 0;

    rhs_start = p;
    rhs_end = src + strlen(src);
    comment_start = strstr(rhs_start, "//");
    if (comment_start && comment_start < rhs_end)
        rhs_end = comment_start;
    while (rhs_end > rhs_start && isspace((unsigned char)rhs_end[-1])) rhs_end--;
    if (rhs_end > rhs_start && rhs_end[-1] == ';') rhs_end--;
    while (rhs_end > rhs_start && isspace((unsigned char)rhs_end[-1])) rhs_end--;
    if (rhs_end <= rhs_start)
        return 0;

    if (rhs && rhs_sz > 0) {
        int rn = (int)(rhs_end - rhs_start);
        if (rn > rhs_sz - 1) rn = rhs_sz - 1;
        memcpy(rhs, rhs_start, (size_t)rn);
        rhs[rn] = '\0';
        trim_in_place(rhs);
    }
    return 1;
}

int split_top_level_args(const char *src, char args[][MAX_LINE_LEN], int max_args) {
    const char *p = src;
    int count = 0;

    while (*p) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        if (count >= max_args)
            return -1;

        const char *start = p;
        p = repl_scan_next_arg_delim(p);

        int n = (int)(p - start);
        if (n > MAX_LINE_LEN - 1)
            n = MAX_LINE_LEN - 1;
        memcpy(args[count], start, (size_t)n);
        args[count][n] = '\0';
        trim_in_place(args[count]);
        count++;

        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '\0')
            break;
        return -1;
    }

    return count;
}

int repl_split_scratch_block_rhs(const char *rhs,
                                 ReplScratchBlockCell *cells, int max_cells) {
    const char *p = rhs;
    const char *close;
    int count = 0;

    if (!rhs || !cells || max_cells <= 0)
        return -1;

    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{')
        return -1;
    p++;

    /* Find the closing brace first so trailing garbage is rejected before
     * any cell is handed back - `{1, 2} 3` must not read as two cells.
     * Nested braces have no meaning in the expression grammar, so a second
     * `{` is malformed rather than a nesting level. */
    close = strchr(p, '}');
    if (!close || strchr(p, '{') != NULL)
        return -1;
    {
        const char *after = close + 1;
        while (*after && isspace((unsigned char)*after)) after++;
        if (*after != '\0')
            return -1;
    }

    while (p < close) {
        const char *start;
        const char *end;

        while (p < close && isspace((unsigned char)*p)) p++;
        if (p >= close)
            break;
        if (count >= max_cells)
            return -1;

        start = p;
        p = repl_scan_next_arg_delim(p);
        if (p > close)
            p = close;

        end = p;
        while (end > start && isspace((unsigned char)end[-1])) end--;
        if (end == start)
            return -1;      /* empty cell: `{1, , 3}` */

        cells[count].start = start;
        cells[count].len = (int)(end - start);
        count++;

        while (p < close && isspace((unsigned char)*p)) p++;
        if (p < close && *p == ',') {
            p++;
            /* A trailing comma before `}` leaves nothing to parse; the
             * loop's leading skip lands on `close` and falls out, which
             * would silently accept `{1, 2,}`. Reject it here instead so
             * the cell count always matches what was written. */
            {
                const char *q = p;
                while (q < close && isspace((unsigned char)*q)) q++;
                if (q >= close)
                    return -1;
            }
        }
    }

    return count > 0 ? count : -1;
}

char *repl_scratch_block_cell_text(const ReplScratchBlockCell *c,
                                   char *out, int out_sz) {
    int n;

    if (!out || out_sz <= 0)
        return out;
    if (!c || !c->start) {
        out[0] = '\0';
        return out;
    }
    n = c->len;
    if (n > out_sz - 1)
        n = out_sz - 1;
    memcpy(out, c->start, (size_t)n);
    out[n] = '\0';
    return out;
}

float repl_eval_if_condition_captured(const char *src_text,
                                      const ExprVar *vars, int num_vars,
                                      float fallback,
                                      const ReplExprCaptureSink *capture) {
    char paren_text[MAX_LINE_LEN];
    if (!repl_extract_paren_payload(src_text, paren_text, sizeof(paren_text)))
        return fallback;
    char repl_cond[MAX_LINE_LEN];
    repl_eval_c_expr_to_repl(paren_text, repl_cond, sizeof(repl_cond));
    if (capture && capture->fn)
        capture->fn(capture->user_data, REPL_EXPR_ROLE_CONDITION, 0,
                    repl_cond, repl_cond + strlen(repl_cond));
    ExprCtx ctx = { repl_cond, vars, num_vars, NULL, 0 };
    return repl_eval_expr(&ctx);
}
