/*
 * src/repl/normalize.c - REPL parse-and-normalize entry points.
 */

#include "repl/normalize.h"

#include "config.h" /* REPL_STATUS_TEXT_MAX */
#include "repl/command_spec.h"
#include "repl/format.h"
#include "repl/host_effects.h"
#include "repl/parser.h"
#include "repl/source_scope.h"
#include "repl/text_helpers.h"

#include <ctype.h>
#include <string.h>

void repl_normalize_from_parsed(const char *parsed_source,
                                const char *raw_expr,
                                int ensure_semicolon,
                                char *out, int out_sz) {
    if (out_sz <= 0) return;
    char tmp[MAX_LINE_LEN];
    repl_format_reindent_from_parsed(parsed_source, raw_expr, tmp, sizeof(tmp));

    int len = (int)strlen(tmp);
    while (len > 0 && isspace((unsigned char)tmp[len - 1]))
        tmp[--len] = '\0';

    if (ensure_semicolon && len > 0) {
        char last = tmp[len - 1];
        if (last != ';' && last != ':' && last != '{' && last != '}') {
            if (len < (int)sizeof(tmp) - 1) {
                tmp[len++] = ';';
                tmp[len] = '\0';
            }
        }
    }

    strncpy(out, tmp, (size_t)out_sz - 1);
    out[out_sz - 1] = '\0';
}

/* Strip leading/trailing whitespace from `raw_expr`, normalize comma
 * spacing (remove space before comma, ensure one space after), optionally
 * append a semicolon, and prepend `indent_spaces` spaces. Used by
 * repl_parse_and_normalize() to produce canonical source text for a command. */
static void normalize_with_indent(const char *raw_expr, int indent_spaces,
                                  int ensure_semicolon, char *out, int out_sz) {
    if (out_sz <= 0) return;

    const char *p = raw_expr;
    while (*p == ' ' || *p == '\t') p++;

    char body[MAX_LINE_LEN];
    size_t body_len = strlen(p);
    if (body_len >= sizeof(body))
        body_len = sizeof(body) - 1;
    memcpy(body, p, body_len);
    body[body_len] = '\0';

    /* Split off a trailing `// ...` so the ';'-normalization and comma
     * spacing below act on the code only; the comment is re-appended from
     * the original line at the end (otherwise the re-added ';' would land
     * after the comment as `... // c;`). */
    {
        const char *bc = repl_line_trailing_comment(body);
        if (bc) body[bc - body] = '\0';
    }

    int len = (int)strlen(body);
    while (len > 0 && isspace((unsigned char)body[len - 1]))
        body[--len] = '\0';
    while (ensure_semicolon && len > 0 && body[len - 1] == ';')
        body[--len] = '\0';
    while (len > 0 && isspace((unsigned char)body[len - 1]))
        body[--len] = '\0';
    if (ensure_semicolon && len > 0 && len < (int)sizeof(body) - 1) {
        body[len++] = ';';
        body[len] = '\0';
    }

    /* Keep expression tokens but normalize comma delimiters for readability. */
    {
        char spaced[MAX_LINE_LEN];
        int si = 0;
        for (int char_idx = 0; body[char_idx] && si < (int)sizeof(spaced) - 1; char_idx++) {
            char c = body[char_idx];
            if (c == ',') {
                while (si > 0 && isspace((unsigned char)spaced[si - 1]))
                    si--;
                spaced[si++] = ',';
                if (si < (int)sizeof(spaced) - 1)
                    spaced[si++] = ' ';
                while (body[char_idx + 1] && isspace((unsigned char)body[char_idx + 1]))
                    char_idx++;
                continue;
            }
            spaced[si++] = c;
        }
        spaced[si] = '\0';
        memcpy(body, spaced, (size_t)si + 1);
    }

    /* Re-attach the original line's trailing comment after the normalized
     * code (a single space separator). No-op if there was none. */
    repl_append_trailing_comment(body, sizeof(body), raw_expr);

    if (indent_spaces < 0) indent_spaces = 0;
    if (indent_spaces > out_sz - 1) indent_spaces = out_sz - 1;
    memset(out, ' ', (size_t)indent_spaces);
    size_t body_copy_len = strlen(body);
    size_t body_cap = (size_t)(out_sz - 1 - indent_spaces);
    if (body_copy_len > body_cap)
        body_copy_len = body_cap;
    memcpy(out + indent_spaces, body, body_copy_len);
    out[indent_spaces + (int)body_copy_len] = '\0';
}

static int parse_and_normalize_impl(const char *line, int pos,
                                    ExprVar *vars, int num_vars,
                                    int preserve_expr, GLCmd *out_cmd,
                                    char *text_out, int text_sz,
                                    int strict_refs,
                                    const ReplSourceScopeView *source_scope,
                                    ReplFuncAliasView func_aliases) {
    /* repl_parse_and_normalize is called from many sites - commit
     * paths, reformatter, tests. The parser never calls set_status
     * itself (it writes diagnostics into ctx->err_buf; enforced by
     * the check-no-set-status-in-repl-parser build guard). This
     * routine owns the scratch err_buf and surfaces the message to
     * the status bar on failure for callers that want it. */
    char normalize_parse_err[REPL_STATUS_TEXT_MAX];
    normalize_parse_err[0] = '\0';
    ReplParseContext parse_ctx = {
        .source_line_idx = pos,
        .vars = vars, .num_vars = num_vars,
        .strict_refs = strict_refs,
        .err_buf = normalize_parse_err,
        .err_sz  = (int)sizeof(normalize_parse_err),
        .func_aliases = func_aliases,
        .source_scope = source_scope,
    };
    ReplParsedLine pl;
    int parsed = repl_parser_parse_command_ctx(line, &pl, &parse_ctx);
    if (!parsed && normalize_parse_err[0])
        repl_set_status_error(normalize_parse_err);

    if (!parsed) return 0;
    *out_cmd = pl.cmd;
    if (preserve_expr) {
        /* pl.text holds the canonical (indented) form; measure its indent */
        int parsed_indent = 0;
        while (pl.text[parsed_indent] == ' ' || pl.text[parsed_indent] == '\t')
            parsed_indent++;

        if (text_out && text_sz > 0)
            normalize_with_indent(line, parsed_indent,
                                  repl_cmd_type_needs_semicolon(out_cmd->type),
                                  text_out, text_sz);
        out_cmd->has_vars = 1;
    } else {
        if (text_out && text_sz > 0) {
            /* pl.text already carries any trailing `// ...` comment -
             * repl_parser_parse_command_ctx re-attaches it to the
             * canonical text, so copying pl.text preserves it. */
            int n = (int)strlen(pl.text);
            if (n >= text_sz) n = text_sz - 1;
            memcpy(text_out, pl.text, (size_t)n);
            text_out[n] = '\0';
        }
    }
    return 1;
}

int repl_parse_and_normalize(const char *line, int pos,
                             ExprVar *vars, int num_vars,
                             int preserve_expr, GLCmd *out_cmd,
                             char *text_out, int text_sz) {
    ReplSourceScopeLiveView live_scope = repl_source_scope_live_view();
    return parse_and_normalize_impl(line, pos, vars, num_vars,
                                    preserve_expr, out_cmd,
                                    text_out, text_sz, 0,
                                    live_scope.scope,
                                    repl_func_alias_view());
}

int repl_parse_and_normalize_strict(const char *line, int pos,
                                    ExprVar *vars, int num_vars,
                                    int preserve_expr, GLCmd *out_cmd,
                                    char *text_out, int text_sz) {
    ReplSourceScopeLiveView live_scope = repl_source_scope_live_view();
    return parse_and_normalize_impl(line, pos, vars, num_vars,
                                    preserve_expr, out_cmd,
                                    text_out, text_sz, 1,
                                    live_scope.scope,
                                    repl_func_alias_view());
}

int repl_parse_and_normalize_strict_with_scope(
        const char *line, int pos,
        ExprVar *vars, int num_vars,
        int preserve_expr, GLCmd *out_cmd,
        char *text_out, int text_sz,
        const ReplSourceScopeView *source_scope,
        ReplFuncAliasView func_aliases) {
    return parse_and_normalize_impl(line, pos, vars, num_vars,
                                    preserve_expr, out_cmd,
                                    text_out, text_sz, 1,
                                    source_scope,
                                    func_aliases);
}
