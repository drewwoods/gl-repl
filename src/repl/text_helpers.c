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

int repl_comment_alpha_payload_equals(const char *line, const char *word) {
    const char *p = line;

    if (!p || !word)
        return 0;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (p[0] != '/' || p[1] != '/')
        return 0;
    p += 2;

    while (*p) {
        unsigned char c = (unsigned char)*p++;

        if (!isalpha(c))
            continue;
        if (*word == '\0' ||
            tolower(c) != tolower((unsigned char)*word))
            return 0;
        word++;
    }
    return *word == '\0';
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

const char *repl_scan_decl_float_prefix(const char *p) {
    if (!p)
        return NULL;
    while (*p && isspace((unsigned char)*p)) p++;
    /* Optional canonical `static ` prefix (see format_decl_text). */
    if (strncmp(p, "static", 6) == 0 && isspace((unsigned char)p[6])) {
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

int repl_extract_label_name(const char *src, char *name, int name_sz) {
    const char *p = src;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == ':') p++;
    int n = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && n < name_sz - 1)
        name[n++] = *p++;
    name[n] = '\0';
    return n > 0;
}

int repl_extract_goto_label(const char *src, char *name, int name_sz) {
    const char *p = strstr(src, "goto");
    if (!p) p = src;
    else p += 4;
    while (*p && isspace((unsigned char)*p)) p++;
    int n = 0;
    while (*p && *p != ';' && !isspace((unsigned char)*p) &&
           (isalnum((unsigned char)*p) || *p == '_') && n < name_sz - 1)
        name[n++] = *p++;
    name[n] = '\0';
    return n > 0;
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
