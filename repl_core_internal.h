/*
 * repl_core_internal.h - Parse / executor / debug-dump REPL internals.
 *
 * Phase 5 of feature/source-document-port.md shrank this header to
 * parse-and-friends. The previous catch-all was split:
 *
 *   repl_util.h           static inline format / copy helpers
 *   repl_scenes.h         scene promotion / capture / reset
 *   src/editor/input.h    feed_line, load_line_to_input, modifier
 *                         provider typedef, editor transient reset
 *   glr_completion.h      glr_completion_register_provider,
 *                         accept_autocomplete
 *
 * What still lives here: the normalize / commit pipeline entry points,
 * the parse / extract / format helpers callers use to build canonical
 * source text, the executor's state-apply helper, the code-panel debug
 * dumps that produce test fixtures, the bench fade hooks, and a few
 * REPL test entries. None of these need editor headers or expose
 * editor-input plumbing — the file pulls neutral REPL grammar +
 * source-document types only.
 */
#ifndef REPL_CORE_INTERNAL_H
#define REPL_CORE_INTERNAL_H

#include "repl_command.h"    /* GLCmd, CmdType */
#include "repl_export.h"     /* ReplExportLayout (step 7c) */
#include "repl_scenes.h"     /* scene promotion/reset hooks (Phase 5) */
#include "repl_util.h"       /* repl_format_fits / _copy_string_fits (Phase 5) */
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

/* Underlying save/load (no logging / toast side-effects); public wrappers
 * in repl_core.h call these. */
void save_output(const char *filename);
int  load_from_file(const char *filename);

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

/* ---- Code-panel dumps (debug + test fixtures) ------------------------- */

/* repl_dump_code_panel_text / _visual_text declarations moved to
 * repl_export.h (upstream commit 7601660) — glr_debug.c is now a real
 * caller, so the dumps live with the export API where the
 * implementations are.  */

/* ---- Executor helpers ------------------------------------------------- */

int  apply_state_cmd(const GLCmd *cmd, float alpha_scale);

/* ---- Visible-var collection ------------------------------------------- */

/* Populate `vars` with every loop/function-local visible at source line
 * `pos`. Returns the count (capped at max_vars). If total_out is non-NULL,
 * receives the uncapped total (for truncation detection at commit sites). */
int  collect_visible_vars(int pos, ExprVar *vars, int max_vars, int *total_out);

/* ---- Bench helpers (populate replay fade state without stepping) -------
 * These exist solely for bench_repl.c to drive
 * `render_replay_fade_pass()` on a deterministic workload. They bypass
 * the full replay state machine: the caller is responsible for having
 * populated `g_flat_cmds[]` first (e.g. via repl_flatten_commands()). */
int  repl_bench_fade_install(const int *old_pcs, const int *new_pcs,
                             int count, float age);
void repl_bench_fade_clear(void);

/* ---- Test entry: example loading -------------------------------------- */

/* Drive example loading from unit tests without going through the GLUT
 * example dropdown. Production code uses repl_example_loader.c
 * directly. */
void repl_load_example_lines_for_test(const char *const *lines);

/* ---- Cmd-type name helper --------------------------------------------- */

const char *cmd_type_name(CmdType t);

#endif /* REPL_CORE_INTERNAL_H */
