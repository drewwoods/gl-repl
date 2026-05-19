/*
 * format.c - REPL command indentation / depth computation
 *
 * See repl/format.h for the public API and documentation.
 */
#include "repl/format.h"
#include <string.h>

int repl_format_tess_depth(const ReplFmtCmd *cmds, int n) {
    int depth = 0;
    for (int i = 0; i < n; i++) {
        if (!cmds[i].valid) continue;
        if (cmds[i].type == REPL_FMT_TESS_BEGIN_POLYGON ||
            cmds[i].type == REPL_FMT_TESS_BEGIN_CONTOUR) depth++;
        else if (cmds[i].type == REPL_FMT_TESS_END) depth--;
    }
    return depth > 0 ? depth : 0;
}

int repl_format_begin_depth(const ReplFmtCmd *cmds, int n) {
    int depth = 0;
    for (int i = 0; i < n; i++) {
        if (!cmds[i].valid) continue;
        if (cmds[i].type == REPL_FMT_GL_BEGIN) depth++;
        else if (cmds[i].type == REPL_FMT_GL_END) depth--;
    }
    return depth > 0 ? depth : 0;
}

static void fill_spaces(int n_spaces, char *buf, int buf_sz) {
    if (buf_sz <= 0) return;
    if (n_spaces < 0) n_spaces = 0;
    if (n_spaces > buf_sz - 1) n_spaces = buf_sz - 1;
    memset(buf, ' ', n_spaces);
    buf[n_spaces] = '\0';
}

void repl_format_indent(const ReplFmtCmd *cmds, int n, char *buf, int buf_sz) {
    int td = repl_format_tess_depth(cmds, n);
    int bd = repl_format_begin_depth(cmds, n);
    fill_spaces(2 + 2 * td + 2 * bd, buf, buf_sz);
}

void repl_format_end_indent(const ReplFmtCmd *cmds, int n, char *buf, int buf_sz) {
    int td = repl_format_tess_depth(cmds, n);
    fill_spaces(2 + 2 * td, buf, buf_sz);
}

void repl_format_tess_end_indent(const ReplFmtCmd *cmds, int n, char *buf, int buf_sz) {
    int td = repl_format_tess_depth(cmds, n);
    if (td > 0) td--;
    fill_spaces(2 + 2 * td, buf, buf_sz);
}

void repl_format_tess_leaf_indent(const ReplFmtCmd *cmds, int n, char *buf, int buf_sz) {
    int td = repl_format_tess_depth(cmds, n);
    fill_spaces(2 + 2 * td, buf, buf_sz);
}

void repl_format_reindent_expr(const ReplFmtCmd *cmds, int n,
                       const char *raw_expr, char *out, int out_sz) {
    if (out_sz <= 0) return;

    /* Compute the correct indent for this position */
    char ind[64];
    repl_format_indent(cmds, n, ind, sizeof(ind));

    /* Strip leading whitespace from raw_expr */
    while (*raw_expr && (*raw_expr == ' ' || *raw_expr == '\t'))
        raw_expr++;

    int ind_len = (int)strlen(ind);
    int expr_len = (int)strlen(raw_expr);
    int total = ind_len + expr_len;

    if (total >= out_sz) total = out_sz - 1;
    memcpy(out, ind, ind_len < out_sz ? ind_len : out_sz - 1);
    int remaining = out_sz - ind_len - 1;
    if (remaining > 0)
        memcpy(out + ind_len, raw_expr, remaining < expr_len ? remaining : expr_len);
    out[total] = '\0';
}

void repl_format_reindent_from_parsed(const char *parsed_source, const char *raw_expr,
                               char *out, int out_sz) {
    if (out_sz <= 0) return;

    /* Count leading whitespace in parsed_source to extract its indent */
    int ind_len = 0;
    while (parsed_source[ind_len] == ' ' || parsed_source[ind_len] == '\t')
        ind_len++;

    /* Strip leading whitespace from raw_expr */
    while (*raw_expr == ' ' || *raw_expr == '\t')
        raw_expr++;

    int expr_len = (int)strlen(raw_expr);
    int copy_ind  = ind_len  < out_sz - 1 ? ind_len  : out_sz - 1;
    int copy_expr = expr_len < out_sz - 1 - copy_ind ? expr_len : out_sz - 1 - copy_ind;

    memcpy(out, parsed_source, copy_ind);
    memcpy(out + copy_ind, raw_expr, copy_expr);
    out[copy_ind + copy_expr] = '\0';
}
