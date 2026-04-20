/*
 * repl_core_internal.h — Cross-TU internals shared among repl_core.c,
 * repl_editor.c, repl_export.c, repl_search.c, and the unit tests.
 *
 * These are intentionally NOT part of the public API in repl_core.h — they
 * are implementation details of the REPL that tests and sibling modules
 * need to reach into directly. When something here stabilizes and outside
 * code depends on it, graduate it to repl_core.h.
 *
 * Organized by subsystem:
 *   - Normalization / commit pipeline
 *   - Text / expression parsing helpers
 *   - Code-panel dumps (debug + visual)
 *   - Autocomplete
 *   - Block-depth / scope queries
 *   - Replay state machine
 *   - Timekeeping
 *   - Search input dispatch
 *   - Line feeding
 *   - Undo / redo
 *   - Commit handler chain (editor.c private handlers exposed to tests)
 */
#ifndef REPL_CORE_INTERNAL_H
#define REPL_CORE_INTERNAL_H

#include <stdarg.h>

#include "repl_core.h"
#include "repl_replay.h"

#if defined(__GNUC__) || defined(__clang__)
#define REPL_PRINTF_LIKE(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define REPL_PRINTF_LIKE(fmt_idx, arg_idx)
#endif

static inline int repl_format_fits(char *out, size_t out_sz,
                                   const char *fmt, ...) REPL_PRINTF_LIKE(3, 4);
static inline int repl_format_fits(char *out, size_t out_sz,
                                   const char *fmt, ...) {
    va_list ap;
    int written;

    if (!out || out_sz == 0)
        return 0;

    va_start(ap, fmt);
    written = vsnprintf(out, out_sz, fmt, ap);
    va_end(ap);

    if (written < 0 || (size_t)written >= out_sz) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

static inline int repl_copy_string_fits(char *dst, size_t dst_sz,
                                        const char *src) {
    size_t len;

    if (!dst || dst_sz == 0)
        return 0;
    if (!src) {
        dst[0] = '\0';
        return 1;
    }

    len = strlen(src);
    if (len >= dst_sz) {
        dst[0] = '\0';
        return 0;
    }

    memcpy(dst, src, len + 1);
    return 1;
}

/* ---- Normalize / commit pipeline -------------------------------------- */

/* Parse `line` into `out_cmd` and also write the canonical (normalized)
 * source text back into out_cmd->source. `preserve_expr` keeps the raw
 * argument expressions instead of re-emitting them from evaluated values. */
int  repl_parse_and_normalize(const char *line, int pos,
                              ExprVar *vars, int num_vars,
                              int preserve_expr, GLCmd *out_cmd);

void update_render_state_strings(void);
void ensure_init_bootstrap_ready(void);
void apply_init_bootstrap(void);

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

/* ---- Code-panel dumps (debug + test fixtures) ------------------------- */

void repl_dump_code_panel_text(FILE *out);
void repl_dump_code_panel_visual_text(FILE *out);

/* ---- Autocomplete ----------------------------------------------------- */

extern const char *g_ac_insert_matches[MAX_AC_MATCHES];
void update_selected_autocomplete_preview(void);
void update_autocomplete(void);
void accept_autocomplete(void);

/* ---- Block-depth / scope queries -------------------------------------- */

void depth_cache_invalidate(void);
int  apply_state_cmd(const GLCmd *cmd, float alpha_scale);
void repl_copy_predef_values(float *dst, int max_vals);
void repl_restore_predef_values(const float *src, int max_vals);
void repl_execute_set_fade_context(float alpha_scale, int skip_geom_before_pc);
void load_line_to_input(int idx);
int  find_block_end(int begin_idx);
int  block_depth_at(int pos);
CmdType nearest_open_block_at(int pos);
/* Populate `vars` with every loop/function-local visible at source line
 * `pos`. Returns the count (capped at max_vars). */
int  collect_visible_vars(int pos, ExprVar *vars, int max_vars);

/* ---- Replay state machine --------------------------------------------- */

/* Replay APIs live in repl_replay.h. */

/* ---- Bench helpers (populate replay fade state without stepping) -------
 * These exist solely for bench_repl.c to drive
 * `execute_replay_fade_batches()` on a deterministic workload. They bypass
 * the full replay state machine: the caller is responsible for having
 * populated `g_flat_cmds[]` first (e.g. via repl_flatten_commands()). */
int  repl_bench_fade_install(const int *old_pcs, const int *new_pcs,
                             int count, float age);
void repl_bench_fade_clear(void);

/* ---- Line feeding (test entry) ---------------------------------------- */

/* Programmatic entry point equivalent to typing `line` and pressing ';'.
 * Used by file loading, example loading, and every test harness. */
int  feed_line(const char *line);
void repl_load_example_lines_for_test(const char *const *lines);

/* ---- Timekeeping ------------------------------------------------------ */

const char *cmd_type_name(CmdType t);
void repl_advance_time(float dt);
void repl_reset_time_to_zero(void);

/* ---- Search input dispatch (implemented in repl_search.c) ------------- */

void search_clear_all(void);
int  handle_search_key(unsigned char key);
int  handle_search_special(int key);

/* ---- Editor input dispatch test hooks --------------------------------- */

typedef int (*ReplModifierProvider)(void);
void repl_set_modifier_provider_for_test(ReplModifierProvider provider);

/* ---- Undo / redo ------------------------------------------------------ */

/* Snapshot the current editor state onto the undo stack. Call BEFORE any
 * mutation; pushing clears the redo stack. */
void push_undo_snapshot(void);
void pop_undo_snapshot(void);
void do_redo(void);

/* Delete cmds[start..start+count) with a status-bar message describing
 * what was removed. Guards against removing a `float` decl whose variable
 * is still referenced elsewhere. */
void delete_cmd_range(int start, int count, const char *what);

/* Clear ALL commands unconditionally (same behaviour as Ctrl+L). */
void repl_clear_all_cmds(void);

/* ---- Commit handler chain (private to repl_editor.c, exposed for tests)
 * Each handler inspects g_input. Returns 1 if it consumed the line
 * (success or handled error), 0 if the input wasn't in its grammar.
 * Ordering matters: see CLAUDE.md "Commit Dispatch Sites". */
int try_commit_float_decl(void);
int try_assign_variable(void);
int try_commit_for_loop(void);
int try_commit_func_def(void);
int try_commit_if_block(void);
int try_commit_close_brace(void);

#endif /* REPL_CORE_INTERNAL_H */
