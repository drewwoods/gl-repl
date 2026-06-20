/*
 * src/repl/text_helpers.h - Text and expression parsing/formatting helpers.
 */
#ifndef REPL_TEXT_HELPERS_H
#define REPL_TEXT_HELPERS_H

#include "config.h"
#include "repl/eval.h" /* ExprVar */

void trim_in_place(char *s);

/* Return the canonical view of `src`: the [start, start+len) substring
 * after skipping leading whitespace and dropping trailing whitespace
 * plus any trailing ';' characters. `*out_start` aliases src (no
 * allocation); `*out_len` is the length of the canonical content (0
 * if empty after trimming). Used by the editor input ↔ committed
 * line comparison sites where the canonical content is the same
 * whether or not the source carries the optional trailing ';'. */
void repl_canonical_input_view(const char *src,
                               const char **out_start,
                               int *out_len);

/* Buffer capacity for one repl_format_source_float result. The shortest
 * exact-round-trip form of a float32 fits comfortably (sign + up to 9
 * significant digits + '.'/exponent); 32 leaves generous headroom. */
#define REPL_SOURCE_FLOAT_TEXT_MAX 32

void repl_format_source_float(char *out, int out_sz, float v);

/* Split a comma-separated identifier list (e.g. a func parameter list
 * `float a, float b` with leading_keyword "float", or a bare `a, b`
 * with leading_keyword NULL/"") into names[]. Returns the count, or -1
 * on anything malformed (missing keyword, non-identifier token,
 * overlong name, more than max_names). An empty list returns 0. */
int  repl_parse_identifier_list(const char *src, const char *leading_keyword,
                                char names[][REPL_PREDEF_NAME_MAX], int max_names);
int  repl_parse_func_name_token(const char **p_inout, int *fn);
int  extract_for_args_text(const char *src,
                           char *var, int var_sz,
                           char *args, int args_sz);
void repl_extract_for_var_name(const char *text, char *var, int var_sz);
int  parse_expr_list_exact(const char *src, float *out_vals, int max_vals,
                           ExprVar *vars, int num_vars, int *out_count);
int  parse_repl_func_signature(const char *src, int *fn,
                               char param_names[][REPL_PREDEF_NAME_MAX], int max_params,
                               int *param_count);
int  parse_repl_func_signature_with_pending_alias(
                               const char *src, const char *alias_name, int alias_slot,
                               int *fn,
                               char param_names[][REPL_PREDEF_NAME_MAX], int max_params,
                               int *param_count);
int  extract_func_call_args_text(const char *src, int *fn,
                                 char *args, int args_sz);
void format_func_header(char *out, int out_sz, const char *indent,
                        int fn, char param_names[][REPL_PREDEF_NAME_MAX], int param_count);
void format_func_header_with_alias(char *out, int out_sz, const char *indent,
                                   int fn, char param_names[][REPL_PREDEF_NAME_MAX],
                                   int param_count, const char *alias_name);

/* Does `s` reference any variable in the given loop/function-scope array? */
int  input_has_expr_vars(const char *s, ExprVar *vars, int num_vars);
/* Same, but counting predef vars too (i.e. is any var visible at all?). */
int  input_has_any_visible_vars(const char *s, ExprVar *vars, int num_vars);

int  repl_extract_paren_payload(const char *src, char *out, int out_sz);
int  repl_extract_label_name(const char *src, char *name, int name_sz);
int  repl_extract_goto_label(const char *src, char *name, int name_sz);
/* Split a plain `name = rhs;` assignment. Thin wrapper over the
 * _target_parts variant below that additionally rejects subscripted
 * (scratch-array) targets. */
int  repl_extract_assignment_parts(const char *src,
                                   char *name, int name_sz,
                                   char *rhs, int rhs_sz);
/* Split `name[index] = rhs;  // comment` into target name, optional
 * bracket index expression (empty string when the target is a plain
 * var), and the rhs text (trailing comment, `;`, and whitespace
 * stripped). Rejects `==` so comparisons don't read as assignments.
 * Purely lexical: no evaluation, no state reads; any out buffer may be
 * NULL to skip that part. Returns 1 on a well-formed assignment. */
int  repl_extract_assignment_target_parts(const char *src,
                                          char *name, int name_sz,
                                          char *index_expr, int index_expr_sz,
                                          char *rhs, int rhs_sz);
int  split_top_level_args(const char *src,
                          char args[][MAX_LINE_LEN], int max_args);

#endif /* REPL_TEXT_HELPERS_H */
