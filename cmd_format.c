/*
 * cmd_format.c — Command indentation / depth computation
 *
 * See cmd_format.h for the public API and documentation.
 */
#include "cmd_format.h"
#include <string.h>

int fmt_tess_depth(const FmtCmd *cmds, int n) {
    int depth = 0;
    for (int i = 0; i < n; i++) {
        if (!cmds[i].valid) continue;
        if (cmds[i].type == FMT_TESS_BEGIN_POLYGON ||
            cmds[i].type == FMT_TESS_BEGIN_CONTOUR) depth++;
        else if (cmds[i].type == FMT_TESS_END) depth--;
    }
    return depth > 0 ? depth : 0;
}

int fmt_begin_depth(const FmtCmd *cmds, int n) {
    int depth = 0;
    for (int i = 0; i < n; i++) {
        if (!cmds[i].valid) continue;
        if (cmds[i].type == FMT_GL_BEGIN) depth++;
        else if (cmds[i].type == FMT_GL_END) depth--;
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

void fmt_indent(const FmtCmd *cmds, int n, char *buf, int buf_sz) {
    int td = fmt_tess_depth(cmds, n);
    int bd = fmt_begin_depth(cmds, n);
    fill_spaces(2 + 2 * td + 2 * bd, buf, buf_sz);
}

void fmt_end_indent(const FmtCmd *cmds, int n, char *buf, int buf_sz) {
    int td = fmt_tess_depth(cmds, n);
    fill_spaces(2 + 2 * td, buf, buf_sz);
}

void fmt_tess_end_indent(const FmtCmd *cmds, int n, char *buf, int buf_sz) {
    int td = fmt_tess_depth(cmds, n);
    if (td > 0) td--;
    fill_spaces(2 + 2 * td, buf, buf_sz);
}

void fmt_reindent_expr(const FmtCmd *cmds, int n,
                       const char *raw_expr, char *out, int out_sz) {
    if (out_sz <= 0) return;

    /* Compute the correct indent for this position */
    char ind[64];
    fmt_indent(cmds, n, ind, sizeof(ind));

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
