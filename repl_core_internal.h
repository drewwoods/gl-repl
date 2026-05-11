/*
 * repl_core_internal.h - Parse / extract / normalize helpers.
 *
 * Phase 5 of feature/source-document-port.md shrank this header to
 * parse-only internals. The previous catch-all dispersed to:
 *
 *   repl_util.h            static inline format / copy helpers
 *   repl_scenes.h          scene promotion / capture / reset
 *   repl_executor.h        apply_state_cmd
 *   repl_command_spec.h    cmd_type_name (alias)
 *   repl_example_loader.h  repl_load_example_lines_for_test
 *   repl_export.h          code-panel debug dumps
 *   replay.h               bench fade hooks
 *   src/editor/input.h     feed_line, load_line_to_input, modifier
 *                          provider typedef, editor transient reset
 *   glr_completion.h       glr_completion_register_provider,
 *                          accept_autocomplete
 *
 * What remains: the normalize / commit pipeline entry points and the
 * parse / extract / format helpers callers use to build canonical
 * source text. Plus visible-var collection (used by parse callers).
 * No editor headers; no app/controller/editor-input hooks.
 */
#ifndef REPL_CORE_INTERNAL_H
#define REPL_CORE_INTERNAL_H

#include "repl_command.h"    /* GLCmd */
#include "repl_eval.h"       /* ExprVar */
#include "repl_scenes.h"     /* re-exported for legacy callers */
#include "repl_util.h"       /* re-exported for legacy callers */
#include "source_document.h" /* SourceTextView (Phase 1) */

/* ---- Normalize / commit pipeline -------------------------------------- */

/* Parse `line` into `out_cmd` and optionally write the canonical (normalized)
 * source text into text_out (capacity text_sz; pass NULL/0 to ignore).
 * `preserve_expr` keeps the raw argument expressions instead of re-emitting
 * them from evaluated values. */
int  repl_parse_and_normalize(const char *line, int pos,
                              ExprVar *vars, int num_vars,
                              int preserve_expr, GLCmd *out_cmd,
                              char *text_out, int text_sz);

/* Same as repl_parse_and_normalize() but rejects top-level CMD_CALL
 * whose target funcN has no matching CMD_FUNC_DEF.  Used by the commit
 * path so undefined calls surface at typing time like undeclared
 * variables do; reformat/flatten/test paths keep the permissive
 * variant above. */
int  repl_parse_and_normalize_strict(const char *line, int pos,
                                     ExprVar *vars, int num_vars,
                                     int preserve_expr, GLCmd *out_cmd,
                                     char *text_out, int text_sz);

/* ---- Text / expression parsing helpers -------------------------------- */

void trim_in_place(char *s);
int  extract_for_args_text(const char *src,
                           char *var, int var_sz,
                           char *args, int args_sz);
int  parse_expr_list_exact(const char *src, float *out_vals, int max_vals,
                           ExprVar *vars, int num_vars, int *out_count);
int  parse_repl_func_signature(const char *src, int *fn,
                               char param_names[][16], int max_params,
                               int *param_count);
int  extract_func_call_args_text(const char *src, int *fn,
                                 char *args, int args_sz);
void format_func_header(char *out, int out_sz, const char *indent,
                        int fn, char param_names[][16], int param_count);

/* Does `s` reference any variable in the given loop/function-scope array? */
int  input_has_expr_vars(const char *s, ExprVar *vars, int num_vars);
/* Same, but counting predef vars too (i.e. is any var visible at all?). */
int  input_has_any_visible_vars(const char *s, ExprVar *vars, int num_vars);

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
int  repl_extract_assignment_target_parts(const char *src,
                                          char *name, int name_sz,
                                          char *index_expr, int index_expr_sz,
                                          char *rhs, int rhs_sz);

/* ---- Visible-var collection ------------------------------------------- */

/* Populate `vars` with every loop/function-local visible at source line
 * `pos`. Returns the count (capped at max_vars). If total_out is non-NULL,
 * receives the uncapped total (for truncation detection at commit sites). */
int  collect_visible_vars(int pos, ExprVar *vars, int max_vars, int *total_out);

#endif /* REPL_CORE_INTERNAL_H */
