/*
 * src/repl/reformat.c - Whole-document REPL source reformatter.
 */
#include "repl/reformat.h"

#include "config.h"
#include "source_document.h"
#include "support/cpuprof.h"
#include "repl/command_store.h"
#include "repl/command.h"
#include "repl/normalize.h"
#include "repl/text_helpers.h"
#include "repl/visible_vars.h"
#include "repl/eval.h"
#include "repl/source_scope.h"
#include "repl/state_notify.h"
#include "repl/state_views.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int reformat_append_text(char *dst, int dst_sz, int *off,
                                const char *text) {
    size_t len;

    if (!dst || dst_sz <= 0 || !off || !text)
        return 0;
    if (*off < 0 || *off >= dst_sz)
        return 0;

    len = strlen(text);
    if ((size_t)*off + len >= (size_t)dst_sz)
        return 0;

    memcpy(dst + *off, text, len);
    *off += (int)len;
    dst[*off] = '\0';
    return 1;
}

static int reformat_append_span(char *dst, int dst_sz, int *off,
                                const char *start, const char *end) {
    size_t len;

    if (!dst || dst_sz <= 0 || !off || !start || !end || end < start)
        return 0;
    if (*off < 0 || *off >= dst_sz)
        return 0;

    len = (size_t)(end - start);
    if ((size_t)*off + len >= (size_t)dst_sz)
        return 0;

    memcpy(dst + *off, start, len);
    *off += (int)len;
    dst[*off] = '\0';
    return 1;
}

/* Re-emit a `float a = expr, b;` declaration line as canonical text:
 * `<indent>static float a = expr, b;` with single-space comma joins,
 * initializer expressions preserved verbatim, and any trailing `// ...`
 * comment re-attached. One left-to-right scan over the name list; every
 * branch is either a token step or a reject - returns 0 on anything that
 * isn't a well-formed decl (caller falls back to rebuilding the text from
 * the parsed payload.decl names, dropping initializer text). */
static int reformat_var_decl_text(const char *orig_text,
                                  const char *indent,
                                  int is_local,
                                  char *out, int out_sz) {
    char buf[MAX_LINE_LEN] = "";
    const char *p = repl_scan_decl_float_prefix(orig_text ? orig_text : "", NULL);
    int decl_count = 0;
    int off = 0;

    if (!p)
        return 0;

    if (!reformat_append_text(buf, sizeof(buf), &off, indent ? indent : ""))
        return 0;
    /* The keyword is storage, not decoration - re-emitting `static` over a
     * function-scoped row would silently promote it to a global. */
    if (!reformat_append_text(buf, sizeof(buf), &off,
                              is_local ? "float " : "static float "))
        return 0;

    while (*p) {
        const char *name_start;

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ';' || (*p == '/' && p[1] == '/'))
            break;

        if (decl_count > 0) {
            if (*p != ',')
                return 0;
            p++;
            while (*p && isspace((unsigned char)*p)) p++;
            if (!reformat_append_text(buf, sizeof(buf), &off, ", "))
                return 0;
        }

        if (!isalpha((unsigned char)*p) && *p != '_')
            return 0;
        name_start = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
        if (!reformat_append_span(buf, sizeof(buf), &off, name_start, p))
            return 0;

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '=' && p[1] != '=') {
            const char *expr_start;
            const char *expr_end;
            int depth = 0;

            p++;
            while (*p && isspace((unsigned char)*p)) p++;
            expr_start = p;
            while (*p) {
                if (*p == '(') {
                    depth++;
                } else if (*p == ')') {
                    if (depth > 0)
                        depth--;
                } else if (depth == 0 && (*p == ',' || *p == ';')) {
                    break;
                } else if (depth == 0 && *p == '/' && p[1] == '/') {
                    break;
                }
                p++;
            }
            expr_end = p;
            while (expr_end > expr_start &&
                   isspace((unsigned char)expr_end[-1]))
                expr_end--;
            if (expr_end == expr_start)
                return 0;
            if (!reformat_append_text(buf, sizeof(buf), &off, " = ") ||
                !reformat_append_span(buf, sizeof(buf), &off,
                                      expr_start, expr_end))
                return 0;
        }

        decl_count++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ';' || (*p == '/' && p[1] == '/'))
            break;
    }

    if (decl_count == 0)
        return 0;

    if (*p == ';')
        p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '\0' && !(p[0] == '/' && p[1] == '/'))
        return 0;

    if (!reformat_append_text(buf, sizeof(buf), &off, ";"))
        return 0;
    if (p[0] == '/' && p[1] == '/') {
        const char *comment_end = p + strlen(p);
        while (comment_end > p && isspace((unsigned char)comment_end[-1]))
            comment_end--;
        if (!reformat_append_text(buf, sizeof(buf), &off, " ") ||
            !reformat_append_span(buf, sizeof(buf), &off, p, comment_end))
            return 0;
    }

    if (!out || out_sz <= 0)
        return 0;
    snprintf(out, (size_t)out_sz, "%s", buf);
    return 1;
}

static void reformat_replace_cmd(ReplCommandStore *store,
                                 int cmd_idx,
                                 const GLCmd *cmd,
                                 const char *text) {
    if (repl_command_store_replace_one(store, cmd_idx, cmd))
        source_document_replace_line(cmd_idx, text);
}

/* The Ctrl+\ whole-document reformatter: re-derive canonical text for
 * every valid source command and write it back through the command
 * store. One independent case per command family - block heads re-emit
 * their header from parsed args (or the original expression text when
 * has_vars), block ends re-align to their opening line's indent, and
 * the default arm round-trips plain GL commands through
 * repl_parse_and_normalize with the line's visible loop/param vars. A
 * case that can't reconstruct the line leaves it untouched rather than
 * guessing. */
void repl_reformat_program(void) {
    prof_begin(PROF_REFORMAT);
    ReplCommandStore store = repl_command_store_live();
    const GLCmd *document_cmds = repl_state_document_cmds();
    /* Source text reads route through a SourceTextView so the
     * dependency is declared at function scope rather than via a
     * scattered global accessor. */
    SourceTextView text = source_document_view();

    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        if (!document_cmds[cmd_idx].valid) continue;

        GLCmd orig = document_cmds[cmd_idx];
        GLCmd fmt = orig;
        /* *3 allows room for indentation, the canonicalized command text,
         * and trailing comments without snprintf truncation warnings. */
        char fmt_text[MAX_LINE_LEN * 3] = "";

        /* Canonical text for this command lives in the editor buffer. */
        const char *orig_text = source_text_line(text, cmd_idx);
        if (!orig_text) orig_text = "";

        char ind_s[REPL_INDENT_TEXT_MAX];
        repl_source_scope_cmd_indent(cmd_idx, ind_s, sizeof(ind_s));

        switch (orig.type) {
        case CMD_FOR_BEGIN: {
            char var[16] = "";
            char args[128] = "";
            if (!extract_for_args_text(orig_text, var, sizeof(var), args, sizeof(args)))
                repl_extract_for_var_name(orig_text, var, sizeof(var));
            if (!var[0]) strncpy(var, "i", sizeof(var) - 1);

            if (orig.has_vars && args[0]) {
                snprintf(fmt_text, sizeof(fmt_text), "%sfor(%s, %s) {", ind_s, var, args);
                fmt.has_vars = 1;
            } else if (orig.args[2] != 1.0f) {
                char start_buf[32];
                char end_buf[32];
                char step_buf[32];
                repl_format_source_float(start_buf, sizeof(start_buf), orig.args[0]);
                repl_format_source_float(end_buf, sizeof(end_buf), orig.args[1]);
                repl_format_source_float(step_buf, sizeof(step_buf), orig.args[2]);
                snprintf(fmt_text, sizeof(fmt_text), "%sfor(%s, %s, %s, %s) {",
                         ind_s, var, start_buf, end_buf, step_buf);
            } else {
                char start_buf[32];
                char end_buf[32];
                repl_format_source_float(start_buf, sizeof(start_buf), orig.args[0]);
                repl_format_source_float(end_buf, sizeof(end_buf), orig.args[1]);
                snprintf(fmt_text, sizeof(fmt_text), "%sfor(%s, %s, %s) {",
                         ind_s, var, start_buf, end_buf);
            }
            reformat_replace_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        case CMD_FOR_END:
        case CMD_FUNC_END:
        case CMD_IF_END: {
            int close_ind;
            char close_s[32];
            repl_source_scope_cmd_indent(cmd_idx, close_s, sizeof(close_s));
            close_ind = (int)strlen(close_s);
            if (close_ind >= 2)
                close_ind -= 2;
            else
                close_ind = 0;
            if (close_ind > (int)sizeof(close_s) - 1) close_ind = (int)sizeof(close_s) - 1;
            memset(close_s, ' ', (size_t)close_ind);
            close_s[close_ind] = '\0';
            snprintf(fmt_text, sizeof(fmt_text), "%s}", close_s);
            reformat_replace_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        case CMD_FUNC_DEF: {
            int fn = (int)orig.args[0];
            int parsed_fn = fn;
            int param_count = 0;
            char param_names[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
            if (parse_repl_func_signature(orig_text, &parsed_fn,
                                          param_names, MAX_EXPR_VARS,
                                          &param_count))
                format_func_header(fmt_text, sizeof(fmt_text), ind_s,
                                   parsed_fn, param_names, param_count);
            else
                snprintf(fmt_text, sizeof(fmt_text), "%sfunc%d {", ind_s, fn);
            reformat_replace_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        case CMD_IF_BEGIN: {
            char cond[MAX_LINE_LEN] = "";
            if (!repl_extract_paren_payload(orig_text, cond, sizeof(cond)))
                snprintf(cond, sizeof(cond), "%g", orig.args[0]);
            snprintf(fmt_text, sizeof(fmt_text), "%sif(%s) {", ind_s, cond);
            reformat_replace_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        case CMD_ELSE_IF: {
            char cond[MAX_LINE_LEN] = "";
            if (!repl_extract_paren_payload(orig_text, cond, sizeof(cond)))
                snprintf(cond, sizeof(cond), "%g", orig.args[0]);
            snprintf(fmt_text, sizeof(fmt_text), "%s} else if(%s) {", ind_s, cond);
            reformat_replace_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        case CMD_ELSE:
            snprintf(fmt_text, sizeof(fmt_text), "%s} else {", ind_s);
            reformat_replace_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        case CMD_VAR_ASSIGN: {
            const char *name = NULL;
            char rhs[MAX_LINE_LEN] = "";
            if (orig.var_idx >= 0 && orig.var_idx < g_num_predef_vars)
                name = g_predef_vars[orig.var_idx].name;
            char fallback[16] = "";
            if (!name) {
                const char *p = orig_text;
                while (*p && isspace((unsigned char)*p)) p++;
                int n = 0;
                while (*p && (isalnum((unsigned char)*p) || *p == '_') &&
                       n < (int)sizeof(fallback) - 1)
                    fallback[n++] = *p++;
                fallback[n] = '\0';
                if (fallback[0]) name = fallback;
            }
            repl_extract_assignment_parts(orig_text, NULL, 0, rhs, sizeof(rhs));
            {
                char comment[MAX_LINE_LEN] = "";
                const char *cp = strstr(orig_text, "//");
                if (cp) snprintf(comment, sizeof(comment), " %s", cp);
                if (name && rhs[0])
                    snprintf(fmt_text, sizeof(fmt_text), "%s%s = %s;%s", ind_s, name, rhs, comment);
                else if (name)
                    snprintf(fmt_text, sizeof(fmt_text), "%s%s = %g;%s", ind_s, name, orig.args[0], comment);
            }
            reformat_replace_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        case CMD_COMMENT: {
            const char *p = orig_text;
            while (*p && isspace((unsigned char)*p)) p++;
            if (p[0] == '/' && p[1] == '/') {
                char suffix[MAX_LINE_LEN];
                p += 2;
                strncpy(suffix, p, sizeof(suffix) - 1);
                suffix[sizeof(suffix) - 1] = '\0';
                int suffix_len = (int)strlen(suffix);
                while (suffix_len > 0 &&
                       isspace((unsigned char)suffix[suffix_len - 1]))
                    suffix[--suffix_len] = '\0';
                snprintf(fmt_text, sizeof(fmt_text), "%s//%s", ind_s, suffix);
            } else {
                snprintf(fmt_text, sizeof(fmt_text), "%s//", ind_s);
            }
            reformat_replace_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        case CMD_VAR_DECLARE: {
            int is_local = (orig.var_idx == REPL_VAR_IDX_LOCAL);
            if (!reformat_var_decl_text(orig_text, ind_s, is_local,
                                        fmt_text, sizeof(fmt_text))) {
                int off = snprintf(fmt_text, sizeof(fmt_text), "%s%sfloat ",
                                   ind_s, is_local ? "" : "static ");
                for (int decl_idx = 0;
                     decl_idx < orig.payload.decl.count && off < (int)sizeof(fmt_text) - 4;
                     decl_idx++) {
                    if (decl_idx > 0)
                        off += snprintf(fmt_text + off, sizeof(fmt_text) - off, ", ");
                    off += snprintf(fmt_text + off, sizeof(fmt_text) - off,
                                    "%s", orig.payload.decl.names[decl_idx]);
                }
                snprintf(fmt_text + off, sizeof(fmt_text) - off, ";");
            }
            reformat_replace_cmd(&store, cmd_idx, &fmt, fmt_text);
            break;
        }
        default: {
            ExprVar vis_vars[MAX_EXPR_VARS];
            int num_vis_vars = collect_visible_vars(cmd_idx, vis_vars, MAX_EXPR_VARS, NULL);
            int preserve_expr = (num_vis_vars > 0) || orig.has_vars;
            GLCmd parsed;
            char parsed_text[MAX_LINE_LEN] = "";
            memset(&parsed, 0, sizeof(parsed));
            if (repl_parse_and_normalize(orig_text, cmd_idx,
                                         num_vis_vars > 0 ? vis_vars : NULL,
                                         num_vis_vars > 0 ? num_vis_vars : 0,
                                         preserve_expr, &parsed,
                                         parsed_text, sizeof(parsed_text)) &&
                parsed.type == orig.type) {
                parsed.is_auto = orig.is_auto;
                parsed.src_cmd_idx = orig.src_cmd_idx;
                if (!preserve_expr) parsed.has_vars = orig.has_vars;
                reformat_replace_cmd(&store, cmd_idx, &parsed, parsed_text);
            }
            break;
        }
        }
    }

    repl_source_scope_depth_cache_invalidate();
    repl_state_mark_source_dirty();

    prof_end(PROF_REFORMAT);
}
