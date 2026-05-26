/*
 * editor_commit.h - Editor-side orchestration for compile/apply commits.
 *
 * This layer owns the editor half of a commit transaction: ask the REPL
 * compile/apply surface what source-command change should happen, preflight it,
 * capture undo at the mutation boundary, write editor text, apply the REPL
 * change, then replay any editor-only follow-up effects such as cursor moves or
 * input clearing.
 *
 * Typed user input commits through the `editor_try_commit_*` chain
 * (`editor_try_commit_any` walks the canonical ordering); the chain
 * handlers each run compile + preflight + apply internally and
 * surface status through `repl_set_status` / `_set_status_error`.
 * Callers that already hold a precompiled change use
 * `editor_commit_apply_external_change()` for the shared
 * preflight/apply transaction. Structured block commits that need
 * extra editor-side effects use `EditorCommitPlan`, which keeps
 * those effects alongside the REPL change without teaching the REPL
 * pipeline about cursor or input mechanics.
 */
#ifndef EDITOR_COMMIT_H
#define EDITOR_COMMIT_H

#include "repl/compile.h"     /* ReplCompiledChange, ReplCompileContext, ReplCompileResult */

struct EditorServices_s;

/* Apply a precompiled external change atomically. The caller already
 * has a ReplCompiledChange and only needs the shared preflight/apply
 * transaction. When `capture_undo` is non-zero the helper captures
 * one undo snapshot immediately before the first mutation; when zero
 * it leaves undo ownership to the caller.
 *
 * The live dispatch path for typed input uses the
 * `editor_try_commit_*` chain below — each handler runs its own
 * compile + preflight + apply transaction and surfaces status text
 * through `repl_set_status` / `_set_status_error`.
 *
 *   Returns 1 if all three halves (predef-ops, editor buffer,
 *     cmd store) landed successfully.
 *   Returns 0 if the preflight detected the cmd-store can't
 *     accept the change. On a 0 return no mutation occurred —
 *     predef-vars, editor buffer, and cmd-store are all unchanged.
 *
 * Does not call set_status; callers surface diagnostics. */
int editor_commit_apply_external_change(const struct ReplCompiledChange_s *change,
                                        int capture_undo);

/* ---- Editor commit plan: REPL change + editor side-effects ----- */
/*
 * Structured-block commits (close-brace, if-block, func-def,
 * for-loop) need cursor / insert-mode / input-clear / pending-
 * newline side-effects in addition to the REPL source-command
 * change. Putting those on `ReplCompiledChange` would let the
 * REPL pipeline tests learn cursor mechanics and re-mix the
 * compile/apply split. Instead the editor wraps the REPL change
 * with a sibling editor-side struct:
 *
 *   ReplCompiledChange       describes source-command level
 *                            mutations (insert/replace/delete/load).
 *   EditorCommitPostEffects  describes editor cursor/mode/input
 *                            effects to replay after the REPL
 *                            mutation lands.
 *   EditorCommitPlan         wraps both halves plus the success
 *                            commit_message.
 *
 * Editor-side structured compile functions return `EditorCommitPlan`.
 * `editor_commit_apply_plan` drives the
 * canonical transaction: preflight → undo capture → REPL apply →
 * editor-buffer apply → editor post-effects.
 *
 * Pure-REPL handlers (float-decl, var-assign) keep returning
 * `ReplCompiledChange`; their minimal post-mutation housekeeping
 * (clear input, reset cursor pos) is handled inline by the
 * matching `editor_try_commit_*` wrapper. They don't need the plan
 * wrapper.
 */

#define EDITOR_COMMIT_NO_CURSOR_CHANGE      (-1)
#define EDITOR_COMMIT_NO_INSERT_MODE_CHANGE (-1)

typedef struct EditorCommitPostEffects_s {
    /* Desired edit_line after the REPL apply lands. -1 means leave
     * the cursor where it is. */
    int cursor_target;

    /* -1 = no change, 0 = exit insert mode, 1 = enter insert mode. */
    int insert_mode_target;

    /* Drop g_input + reset cursor_pos to 0. */
    int clear_input;

    /* Drop the pending-newline scratch buffer. */
    int clear_pending_newline;

    /* After the cursor target lands, call editor_load_line_to_input(...)
     * to repopulate g_input from the new edit-line's text. */
    int load_line_after_apply;

    /* `apply_func_decl_resume` applies a stashed delta to edit_line
     * after a func-def's close-brace lands. The compile step
     * captures the delta here so apply doesn't rely on the
        * shared func-decl-resume bookkeeping being reread at apply time.
        * For the close-brace path the compile step snapshots the current
        * resume delta here, and apply consumes and clears that bookkeeping. */
    int func_decl_resume_advance;

    /* CmdType-shaped value identifying the block kind. Used by
     * the func-decl-resume guard ("only fire on CMD_FUNC_END") and
     * by status-message formatting. */
    int end_type;

    /* Drop autocomplete model state when the commit should clear any
     * stale popup/ghost preview. */
    int clear_autocomplete;

    /* Publish a value into the func-decl-resume bookkeeping. When
     * `func_decl_resume_publish` is non-zero the apply step writes
     * `func_decl_resume_publish_value` via
     * `editor_commit_func_decl_resume_set` so a subsequent
     * close-brace's compile reads it. The setter encapsulates the
     * shared resume bookkeeping instead of exposing that state as a
     * cross-module protocol. */
    int func_decl_resume_publish;
    int func_decl_resume_publish_value;
} EditorCommitPostEffects;

typedef struct EditorCommitPlan_s {
    ReplCompiledChange      change;
    EditorCommitPostEffects effects;
    int                     commit_message_valid;
    char                    commit_message[REPL_STATUS_TEXT_MAX];
} EditorCommitPlan;

/* Initialize a plan to neutral defaults: NO_CHANGE on the REPL
 * side, "no change" sentinels on every editor effect. Callers
 * fill in the fields they need. */
void editor_commit_plan_init(EditorCommitPlan *plan);

/* Publish a value into the func-decl-resume bookkeeping. Called
 * by editor_commit_apply_plan when
 * `effects.func_decl_resume_publish` is set. Encapsulates the
 * shared resume bookkeeping so callers stop poking at storage
 * directly. */
void editor_commit_func_decl_resume_set(int delta);

/* Apply an EditorCommitPlan atomically:
 *   1. Preflight `plan->change` via repl_apply_can_apply_compiled_change.
 *      If the preflight fails (or the plan is NULL) returns 0
 *      with no mutation.
 *   2. Apply the REPL halves (predef ops + scratch ops +
 *      editor-buffer apply + REPL cmd-store apply).
 *   3. Apply editor post-effects in order: cursor_target →
 *      func-decl-resume → insert_mode_target → clear_input →
 *      clear_pending_newline → load_line_after_apply →
 *      clear_autocomplete → func_decl_resume_publish.
 *   4. If `plan->commit_message_valid`, set the status text.
 *
 * Undo capture is the caller's responsibility — the ;-key / Enter /
 * editor_feed_line dispatch sites push a snapshot before invoking
 * the try_commit_* chain; this helper deliberately does NOT push
 * one (see body comment for rationale).
 *
 * Returns 1 on success, 0 on preflight failure. */
int editor_commit_apply_plan(const EditorCommitPlan *plan);

/* Editor-side compile entry points. These are the structured-block
 * counterparts to repl_compile_*; they return EditorCommitPlan
 * (REPL change + editor effects) rather than bare
 * ReplCompiledChange. */

ReplCompileResult editor_compile_close_brace(const char *input,
                                             const ReplCompileContext *ctx,
                                             EditorCommitPlan *out,
                                             char *err, int err_size);

ReplCompileResult editor_compile_if_block(const char *input,
                                          const ReplCompileContext *ctx,
                                          EditorCommitPlan *out,
                                          char *err, int err_size);

/* Editor-side compile for func definitions.
 *
 * Handles validation plus BOTH outcomes: the header-replace branch
 * (cursor on an existing CMD_FUNC_DEF in non-insert mode) and the
 * new-def-with-comment-relocation branch, the latter via the
 * EditorCommitPlan delete_pos/delete_count fields and the
 * func_decl_resume_publish post-effect. editor_try_commit_func_def
 * is a thin wrapper that just applies the returned plan.
 *
 * Returns:
 *   REPL_COMPILE_OK + REPLACE_ONE   header-replace plan ready
 *   REPL_COMPILE_OK + (insert plan) new-def plan ready (may carry a
 *                                   delete range for comment relocation)
 *   REPL_COMPILE_OK + NO_CHANGE     not a func decl OR a func decl
 *                                   in non-overwrite context (caller
 *                                   should fall through to some other
 *                                   commit route)
 *   REPL_COMPILE_ERROR              syntax / duplicate-name error;
 *                                   `err` filled */
ReplCompileResult editor_compile_func_def(const char *input,
                                          const ReplCompileContext *ctx,
                                          EditorCommitPlan *out,
                                          char *err, int err_size);

/* Editor-side compile for for-loops. Three branches:
 *   - Header replace: REPLACE_ONE; cursor / insert-mode / clear-input /
 *     clear-autocomplete effects.
 *   - Empty body (`{` or end-of-input): INSERT_MANY count=2
 *     (CMD_FOR_BEGIN + CMD_FOR_END); cursor lands inside the block,
 *     insert mode entered.
 *   - One-liner body: INSERT_MANY count=3 (begin + body + end);
 *     cursor lands past the end, insert mode exited, pending newline
 *     cleared.
 *
 * Returns:
 *   REPL_COMPILE_OK + REPLACE_ONE | INSERT_MANY  plan ready
 *   REPL_COMPILE_OK + NO_CHANGE                  not a for-loop input
 *   REPL_COMPILE_ERROR                           syntax / body error;
 *                                                `err` filled */
ReplCompileResult editor_compile_for_loop(const char *input,
                                          const ReplCompileContext *ctx,
                                          EditorCommitPlan *out,
                                          char *err, int err_size);

/* ---- Commit dispatcher chain ----
 *
 * Each `try_commit_*` inspects the live editor input. Returns 1 if
 * it consumed the line (success or handled error), 0 if the input
 * wasn't in its grammar. Ordering inside the higher-order
 * dispatchers matters — `float` must precede `assign` so `float x`
 * isn't misread as an assignment to "float". */

void editor_commit_reset_transients(void);
int  editor_commit_resolve_insert_exit_target(int target);
int  editor_commit_apply_swatch_change(int edit_line, int direction);

/* Func-decl resume bookkeeping: a CMD_FUNC_DEF commit publishes a
 * delta via editor_commit_func_decl_resume_set; the matching
 * close-brace / Enter-out-of-func consumes it. Read+clear with
 * `_take`, read-only inspection with `_peek`. */
int  editor_commit_func_decl_resume_take(void);
int  editor_commit_func_decl_resume_peek(void);

int editor_try_commit_float_decl(void);
int editor_try_commit_assign_variable(void);
int editor_try_commit_for_loop(void);
int editor_try_commit_func_def(void);
int editor_try_commit_if_block(void);
int editor_try_commit_close_brace(void);
int editor_try_commit_var_statements(void);
int editor_try_commit_block_structs(void);
int editor_try_commit_any(void);
int editor_try_commit_var_statements_then_insert(void);

#endif /* EDITOR_COMMIT_H */
