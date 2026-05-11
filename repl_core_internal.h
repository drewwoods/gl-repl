/*
 * repl_core_internal.h - Implementation internals shared across REPL modules.
 *
 * Collects internal APIs used by repl_core.c, src/editor/input.c,
 * repl_executor.c, repl_export.c, repl_parser.c, src/editor/search.c, and the
 * unit test suites. These are NOT part of the public API (repl_core.h); they
 * are domain-specific helpers that tests and sibling modules need. When an
 * internal API stabilizes and becomes broadly useful, graduate it to
 * repl_core.h.
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
 * 4. Source-scope: Block depth, indentation, and scope queries (documented in
 *    repl_source_scope.h). Prefixes cached to avoid re-traversal.
 *
 * 5. Executor helpers: Apply state commands (enable/disable), variable snapshot
 *    save/restore, and fade context setup for replay.
 *
 * 6. Visible-var collection: collect_visible_vars() walks loop/function-scope
 *    ancestors at a source line.
 *
 * 7. Replay/bench: repl_bench_fade_install/clear() for deterministic replay
 *    fade testing (bench_repl.c workload).
 *
 * 8. Test entry: repl_load_example_lines_for_test() drives example loading
 *    from unit tests without going through the GLUT example dropdown.
 *
 * 9. User scene promotion: repl_promote_example_if_needed() creates a user
 *    scene on first edit of an example. Scene state reset/initialization.
 *
 * Editor-input declarations (feed_line, load_line_to_input,
 * repl_editor_reset_transients, delete_cmd_range, repl_clear_all_cmds,
 * ReplModifierProvider) moved to src/editor/input.h. Controller-side
 * autocomplete entry points (accept_autocomplete,
 * glr_completion_register_provider) moved to glr_completion.h.
 */
#ifndef REPL_CORE_INTERNAL_H
#define REPL_CORE_INTERNAL_H

#include <stdarg.h>

#include "repl_export.h"     /* ReplExportLayout (step 7c) */
#include "source_document.h" /* SourceTextView (Phase 1 of feature/source-document-port.md) */

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

void repl_dump_code_panel_text(FILE *out, SourceTextView text);
/* Visual dump consumes the explicit layout struct (step 7c of
 * feature/decouple-repl-from-gl-repl-alt.md) so the REPL pipeline
 * doesn't reach into ui_state / glr_state for panel width or
 * presentation flags. The plain text dump above doesn't need the
 * struct because it doesn't measure or wrap. */
/* ReplExportLayout typedef is in repl_export.h, transitively included
 * through repl_core.h above. */
void repl_dump_code_panel_visual_text(FILE *out, SourceTextView text,
                                      const ReplExportLayout *layout);

/* ---- Source-scope helpers --------------------------------------------- */

/* Depth/cache, indentation, and block-boundary queries live in
 * repl_source_scope.h. */

/* ---- Executor helpers ------------------------------------------------- */

int  apply_state_cmd(const GLCmd *cmd, float alpha_scale);

/* ---- Line feeding / source structure ---------------------------------- */

void load_line_to_input(int idx);
/* Populate `vars` with every loop/function-local visible at source line
 * `pos`. Returns the count (capped at max_vars). If total_out is non-NULL,
 * receives the uncapped total (for truncation detection at commit sites). */
int  collect_visible_vars(int pos, ExprVar *vars, int max_vars, int *total_out);

/* ---- Replay state machine --------------------------------------------- */

/* Replay APIs live in replay.h. */

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

/* repl_advance_time / repl_reset_time_to_zero moved to repl_core.h as
 * public REPL timekeeping APIs. */

/* Called before any mutation: if an example is currently viewed (no active
 * user scene), allocate a scene slot, copy the current editor state into it,
 * and inherit the example's name (de-duplicated). Returns the promoted slot
 * index, or -1 if promotion was a no-op or rejected. */
int repl_promote_example_if_needed(void);
void repl_scenes_save_active_scene_if_any(void);
void repl_scenes_capture_home_if_needed(void);
/* Snapshot the 14 presentation-cfg keys when entering an example from
 * non-example state. Restored on the next user-scene / home transition.
 * Idempotent across consecutive example loads. */
void repl_scenes_capture_pre_example_cfg_if_entering(void);
void repl_scenes_mark_example_active(void);
void repl_scenes_activate_home_slot(void);
void repl_scenes_reset(void);

/* Commit dispatcher chain (try_commit_*, editor_commit_func_decl_resume_*,
 * editor_commit_resolve_insert_exit_target, editor_commit_reset_transients)
 * declarations moved to editor_commit.h (Phase H.5 commit 41). The
 * bodies live in editor_commit.c (moved from repl_commit.c in
 * commit 39). */

#endif /* REPL_CORE_INTERNAL_H */
