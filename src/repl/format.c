/* format.c - minimal REPL formatting helpers used by core.c and tests. */
#include "repl/format.h"
#include <string.h>

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
