/*
 * src/repl/core_internal.h - Shared internal parse / normalize helpers.
 *
 * This is the small internal surface that multiple REPL TUs and tests still
 * share: parse-and-normalize entry points, expression/string extractors,
 * canonical-text builders, and visible-variable collection. It no longer owns
 * scene loading, export, editor-input shims, or controller hooks.
 *
 * The old catch-all header was shrunk to this parse-focused subset.
 * The pieces that moved out now live in (implemented in Phase 5 of
 * feature/source-document-port.md):
 *
 *   src/repl/util.h            static inline format / copy helpers
 *   src/repl/scenes.h          scene promotion / capture / reset
 *   src/repl/executor.h        repl_apply_state_cmd
 *   src/repl/command_spec.h    cmd_type_name (alias)
 *   src/repl/example_loader.h  repl_load_example_lines_for_test
 *   src/repl/export.h          code-panel debug dumps
 *   replay.h               bench fade hooks
 *   src/editor/input.h     editor_feed_line, editor_load_line_to_input, modifier
 *                          provider typedef, editor transient reset
 *   glr_completion.h       glr_completion_register_provider,
 *                          glr_completion_accept_autocomplete
 *
 * What remains: the normalize / commit pipeline entry points and the
 * parse / extract / format helpers callers use to build canonical
 * source text. Plus visible-var collection (used by parse callers).
 * No editor headers; no app/controller/editor-input hooks.
 */
#ifndef REPL_CORE_INTERNAL_H
#define REPL_CORE_INTERNAL_H

#include "repl/command.h"    /* GLCmd */
#include "repl/eval.h"       /* ExprVar */
#include "repl/scenes.h"     /* re-exported for legacy callers */
#include "repl/util.h"       /* re-exported for legacy callers */
#include "source_document.h" /* SourceTextView document view */

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

/* Hard cap on goto jumps while walking one flat program, shared by the
 * executor and the replay-annotation walker. A program that exceeds this
 * is treated as a runaway goto loop and bailed out with a status message. */
#define REPL_GOTO_LOOP_LIMIT 100000

/* ---- Visible-var collection ------------------------------------------- */

/* Populate `vars` with every loop/function-local visible at source line
 * `pos`. Returns the count (capped at max_vars). If total_out is non-NULL,
 * receives the uncapped total (for truncation detection at commit sites). */
int  collect_visible_vars(int pos, ExprVar *vars, int max_vars, int *total_out);

#endif /* REPL_CORE_INTERNAL_H */
