#ifndef REPL_CORE_INTERNAL_H
#define REPL_CORE_INTERNAL_H

#include "repl_core.h"

/* Test-visible internals for normalization/commit pipeline */
int  repl_parse_and_normalize(const char *line, int pos,
                              ExprVar *vars, int num_vars,
                              int preserve_expr, GLCmd *out_cmd);
void repl_normalize_from_parsed(const char *parsed_source,
                                const char *raw_expr,
                                int ensure_semicolon,
                                char *out, int out_sz);
void repl_dump_code_panel_text(FILE *out);
void repl_dump_code_panel_visual_text(FILE *out);

#endif /* REPL_CORE_INTERNAL_H */
