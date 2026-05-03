/*
 * repl_core_internal.h - Implementation internals shared across REPL modules.
 *
 * Collects internal APIs used by repl_core.c, repl_editor.c, repl_executor.c,
 * repl_export.c, repl_parser.c, repl_search.c, and the unit test suites. These
 * are NOT part of the public API (repl_core.h); they are domain-specific helpers
 * that tests and sibling modules need. When an internal API stabilizes and
 * becomes broadly useful, graduate it to repl_core.h.
 *
 * Organization by subsystem (see function declarations below):
 *
 * 1. Normalization / commit pipeline: repl_parse_and_normalize() and the strict
 *    variant validate top-level function calls. Helpers update render and camera
 *    state strings for export consistency.
 *
 * 2. Text / expression parsing: Argument extraction, for-loop parsing, function
 *    signature parsing, variable reference checking, and normalization helpers.
 *
 * 3. Code-panel dumps: Debug output for tests and visual inspection of the code
 *    panel (wrapped text, highlighting, etc.). Used by test suites to verify
 *    rendering logic without running the full UI.
 *
 * 4. Autocomplete: Update and accept logic for symbol completion and parameter
 *    hints. Integrates with repl_autocomplete.c model.
 *
 * 5. Source-scope: Block depth, indentation, and scope queries (documented in
 *    repl_source_scope.h). Prefixes cached to avoid re-traversal.
 *
 * 6. Executor helpers: Apply state commands (enable/disable), variable snapshot
 *    save/restore, and fade context setup for replay.
 *
 * 7. Line feeding: load_line_to_input() and collect_visible_vars() support
 *    editing existing lines and variable scope queries.
 *
 * 8. Replay/bench: repl_bench_fade_install/clear() for deterministic replay
 *    fade testing (bench_repl.c workload).
 *
 * 9. Feed line (test entry): feed_line() is the programmatic equivalent of typing
 *    a command and pressing ';'. Used by all test harnesses and file loading.
 *
 * 10. Timekeeping: Advance and reset the animated 't' variable.
 *
 * 11. Search dispatch: Keyboard routing for the search overlay (repl_search.c).
 *
 * 12. Editor input test hooks: Modifier provider for simulating Ctrl/Shift/Alt
 *     in tests.
 *
 * 13. Command mutations: delete_cmd_range() with guards against orphaning
 *     variable references, repl_clear_all_cmds() for full clear.
 *
 * 14. User scene promotion: repl_promote_example_if_needed() creates a user
 *     scene on first edit of an example. Scene state reset/initialization.
 *
 * 15. Commit handler chain: try_commit_*() functions in declaration order
 *     (float, assign, close-brace, for, func, if, GL command). Each returns 1
 *     if consumed, 0 if not matched. Utilities repl_commit_reset_transients()
 *     and repl_commit_resolve_insert_exit_target().
 */
#ifndef REPL_CORE_INTERNAL_H
#define REPL_CORE_INTERNAL_H

#include <stdarg.h>

#include "editor_state.h"  /* EditorBufferView */
#include "repl_core.h"
#include "repl_replay.h"
#include "repl_search.h"
#include "repl_undo.h"

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

/* Test-only: parse a command with a local variable scope.
 * Accepts provided vars array instead of predef-only. */
int repl_parser_parse_command_with_vars(const char *line, GLCmd *cmd,
                                        ExprVar *vars, int num_vars);

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

void repl_dump_code_panel_text(FILE *out, EditorBufferView text);
void repl_dump_code_panel_visual_text(FILE *out, EditorBufferView text);

/* ---- Autocomplete ----------------------------------------------------- */

void update_selected_autocomplete_preview(void);
void update_autocomplete(void);
void accept_autocomplete(void);

/* ---- Source-scope helpers --------------------------------------------- */

/* Depth/cache, indentation, and block-boundary queries live in
 * repl_source_scope.h. */

/* ---- Executor helpers ------------------------------------------------- */

int  apply_state_cmd(const GLCmd *cmd, float alpha_scale);

/* ---- Line feeding / source structure ---------------------------------- */

void load_line_to_input(int idx);
/* Populate `vars` with every loop/function-local visible at source line
 * `pos`. Returns the count (capped at max_vars). */
int  collect_visible_vars(int pos, ExprVar *vars, int max_vars);

/* ---- Replay state machine --------------------------------------------- */

/* Replay APIs live in repl_replay.h. */

/* ---- Bench helpers (populate replay fade state without stepping) -------
 * These exist solely for bench_repl.c to drive
 * `render_replay_fade_pass()` on a deterministic workload. They bypass
 * the full replay state machine: the caller is responsible for having
 * populated `g_flat_cmds[]` first (e.g. via repl_flatten_commands()). */
int  repl_bench_fade_install(const int *old_pcs, const int *new_pcs,
                             int count, float age);
void repl_bench_fade_clear(void);

/* ---- Line feeding (test entry) ---------------------------------------- */

/* Programmatic entry point equivalent to typing `line` and pressing ';'.
 * Used by file loading, example loading, and every test harness. */
int  feed_line(const char *line);
void repl_editor_reset_transients(void);
void repl_load_example_lines_for_test(const char *const *lines);

/* ---- Timekeeping ------------------------------------------------------ */

const char *cmd_type_name(CmdType t);
void repl_advance_time(float dt);
void repl_reset_time_to_zero(void);

/* ---- Editor input dispatch test hooks --------------------------------- */

typedef int (*ReplModifierProvider)(void);
void repl_set_modifier_provider_for_test(ReplModifierProvider provider);

/* Delete cmds[start..start+count) with a status-bar message describing
 * what was removed. Guards against removing a `float` decl whose variable
 * is still referenced elsewhere. */
void delete_cmd_range(int start, int count, const char *what);

/* Clear ALL commands unconditionally (same behaviour as Ctrl+L). */
void repl_clear_all_cmds(void);

/* Called before any mutation: if an example is currently viewed (no active
 * user scene), allocate a scene slot, copy the current editor state into it,
 * and inherit the example's name (de-duplicated). Returns the promoted slot
 * index, or -1 if promotion was a no-op or rejected. */
int repl_promote_example_if_needed(void);
void repl_scenes_save_active_scene_if_any(void);
void repl_scenes_capture_home_if_needed(void);
void repl_scenes_mark_example_active(void);
void repl_scenes_activate_home_slot(void);
void repl_scenes_reset(void);

/* ---- Commit handler chain (implemented in repl_commit.c)
 * Each handler inspects g_input. Returns 1 if it consumed the line
 * (success or handled error), 0 if the input wasn't in its grammar.
 * Ordering matters: see ARCHITECTURE.md "Structured block commits". */
void repl_commit_reset_transients(void);
int  repl_commit_resolve_insert_exit_target(int target);

/* func-decl resume bookkeeping. Phase D commit 26b lifts these out
 * of static-helper land so editor_commit.c can stash the delta into
 * an EditorCommitPlan's post-effects + apply consumes the global
 * here. Phase D commit 26d (func_def migration) folds the read+set
 * portion into compile too, eliminating the global. */
int  repl_commit_func_decl_resume_delta_take(void);   /* read + clear */
int  repl_commit_func_decl_resume_delta_peek(void);   /* read only */
void repl_commit_func_decl_resume_delta_set(int delta);
int try_commit_float_decl(void);
int try_assign_variable(void);
int try_commit_for_loop(void);
int try_commit_func_def(void);
int try_commit_if_block(void);
int try_commit_close_brace(void);
int try_commit_var_statements(void);
int try_commit_block_structs(void);
int try_commit_any(void);
int try_commit_var_statements_then_insert(void);

#endif /* REPL_CORE_INTERNAL_H */
