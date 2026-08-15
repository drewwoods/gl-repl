/*
 * src/repl/line_scan.c - Statement-boundary lexing. See line_scan.h for why
 * this is shared rather than duplicated.
 */
#include "repl/line_scan.h"

#include <ctype.h>

char repl_scan_code_line(const char *line, int *depth, int *in_block_comment,
                         int *code_len) {
    int in_str = 0, in_chr = 0, last = -1, i = 0;
    int in_block = in_block_comment ? *in_block_comment : 0;

    for (; line[i]; i++) {
        char c = line[i];
        if (in_block) {
            /* Nothing inside `/ * ... * /` is code: no brackets counted, no
             * quotes opened, and `//` does not end anything. */
            if (c == '*' && line[i + 1] == '/') {
                in_block = 0;
                i++;
            }
            continue;
        }
        if (in_str || in_chr) {
            if (c == '\\' && line[i + 1]) { i++; continue; }
            if (in_str && c == '"')  in_str = 0;
            if (in_chr && c == '\'') in_chr = 0;
            last = i;
            continue;
        }
        if (c == '/' && line[i + 1] == '/') break; /* line comment */
        if (c == '/' && line[i + 1] == '*') { in_block = 1; i++; continue; }
        if (c == '"')  { in_str = 1; last = i; continue; }
        if (c == '\'') { in_chr = 1; last = i; continue; }
        if (c == '(' || c == '[') (*depth)++;
        else if ((c == ')' || c == ']') && *depth > 0) (*depth)--;
        if (!isspace((unsigned char)c)) last = i;
    }

    if (in_block_comment)
        *in_block_comment = in_block;
    *code_len = last + 1; /* 0 when the line has no code */
    return last >= 0 ? line[last] : '\0';
}

int repl_is_stmt_terminator(char c) {
    return c == ';' || c == '{' || c == '}' || c == ':';
}
