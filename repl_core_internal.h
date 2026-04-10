#ifndef REPL_CORE_INTERNAL_H
#define REPL_CORE_INTERNAL_H

#include "repl_core.h"

/*
 * Test-visible internals for the normalization/commit pipeline.
 * These stay outside the public REPL API, but are shared with regression tests
 * so they can exercise the same source-text rules as the runtime.
 */
int  repl_parse_and_normalize(const char *line, int pos,
                              ExprVar *vars, int num_vars,
                              int preserve_expr, GLCmd *out_cmd);
void repl_normalize_from_parsed(const char *parsed_source,
                                const char *raw_expr,
                                int ensure_semicolon,
                                char *out, int out_sz);
int  repl_extract_paren_payload(const char *src, char *out, int out_sz);
int  repl_extract_label_name(const char *src, char *name, int name_sz);
int  repl_extract_goto_label(const char *src, char *name, int name_sz);
int  repl_extract_assignment_parts(const char *src,
                                   char *name, int name_sz,
                                   char *rhs, int rhs_sz);
void repl_dump_code_panel_text(FILE *out);
void repl_dump_code_panel_visual_text(FILE *out);

#endif /* REPL_CORE_INTERNAL_H */
