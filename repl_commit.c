/*
 * repl_commit.c - REPL commit handlers and commit-order helpers.
 *
 * This module owns the syntactic forms that mutate source commands before
 * the general GL parser gets a chance to run: float declarations, variable
 * assignments, structured blocks, and explicit close braces. repl_editor.c
 * remains responsible for deciding when those handlers are invoked.
 */
#include "sample.h"
#include "editor_commit.h"
#include "repl_apply.h"
#include "repl_core_internal.h"
#include "repl_command_store.h"
#include "repl_compile.h"
#include "repl_parser.h"
#include "repl_source_scope.h"
#include "repl_state.h"

static int g_func_decl_resume_delta = 0;

/* function_decl_insert_pos / function_leading_comment_start
 * removed: try_commit_func_def's full migration to compile/apply
 * (Phase D commit 26f) lives in editor_commit.c, where the helpers
 * read from the compile-time context. */

/* apply_func_decl_resume removed: callers (close_brace) now route
 * through editor_commit_apply_plan which consumes the delta as part
 * of the plan's editor post-effects. The remaining static helper is
 * func_def's setter and Enter-while-inserting-at-func-end via
 * repl_commit_resolve_insert_exit_target — both still touch the
 * global directly, slated for Phase D commit 26d. */

int repl_commit_func_decl_resume_delta_peek(void) {
    return g_func_decl_resume_delta;
}

int repl_commit_func_decl_resume_delta_take(void) {
    int delta = g_func_decl_resume_delta;
    g_func_decl_resume_delta = 0;
    return delta;
}

void repl_commit_func_decl_resume_delta_set(int delta) {
    g_func_decl_resume_delta = delta;
}

/* fill_scope_indent removed: was a one-liner wrapper around
 * repl_source_scope_cmd_indent used only by structured-block
 * handlers, all now migrated to editor_commit.c. */

/* fill_scope_close_indent removed: close_brace migrated to
 * editor_compile_close_brace which has its own static
 * close_brace_indent. Other callers (none today; future func/for/if
 * close-brace migrations land in 26c-26e) compute the indent
 * inside the editor compile path. */


int repl_commit_resolve_insert_exit_target(int target) {
    if (!editor_insert_mode() ||
        g_func_decl_resume_delta <= 0 ||
        repl_state_edit_line() < 0 ||
        repl_state_edit_line() >= repl_state_document_count() ||
        repl_state_document_cmds_mut()[repl_state_edit_line()].type != CMD_FUNC_END)
        return target;

    if (target == repl_state_edit_line()) {
        target += g_func_decl_resume_delta;
        if (target > repl_state_document_count())
            target = repl_state_document_count();
    }

    g_func_decl_resume_delta = 0;
    return target;
}


void repl_commit_reset_transients(void) {
    g_func_decl_resume_delta = 0;
}

/* Apply a compiled float-decl change to the live REPL+editor state.
 *
 * Drives the three apply halves in lockstep:
 *   1. predef-var cascade  (repl_apply_predef_ops)
 *   2. command store write (repl_apply_compiled_change)
 *   3. editor-buffer write (editor_buffer_apply_compiled_change)
 *
 * The float-decl-specific cursor / status / dirty-flag housekeeping
 * stays here. Phase C commit 21 lifts that into the editor commit
 * orchestration so this helper can become a thin transaction wrap.
 */
static int apply_float_decl_change(const ReplCompiledChange *change) {
    if (!editor_commit_apply_compiled_change(change)) {
        set_status("Command buffer full!");
        return 1;
    }

    if (change->kind == REPL_COMPILED_REPLACE_ONE) {
        repl_state_edit_line_set(repl_state_edit_line() + 1);
        load_line_to_input(repl_state_edit_line());
    } else if (change->kind == REPL_COMPILED_INSERT_ONE) {
        if (!editor_insert_mode() &&
            repl_state_edit_line() < repl_state_document_count())
            load_line_to_input(repl_state_edit_line());
    }

    if (change->commit_message[0])
        set_status(change->commit_message);
    {
        ReplEditorInputState *inp = editor_state_input_mut();
        inp->input[0] = '\0';
        inp->input_len = 0;
    }
    editor_cursor_pos_set(0);
    mark_normals_dirty();
    return 1;
}

int try_commit_float_decl(void) {
    ReplCompileContext ctx = repl_compile_context_from_live();
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult result = repl_compile_float_decl(
        editor_state_input().input, &ctx, &change, err, sizeof(err));
    if (result == REPL_COMPILE_OK && change.kind == REPL_COMPILED_NO_CHANGE)
        return 0;  /* not a float decl; fall through */
    if (result != REPL_COMPILE_OK) {
        /* Phase C commit 19: the wrapper still surfaces the diagnostic
         * via set_status. Phase C commit 21 routes the diagnostic
         * through return values to the controller / status layer; the
         * compile function itself never touched status. */
        set_status(err);
        return 1;
    }
    return apply_float_decl_change(&change);
}


/* Apply a compiled var-assignment change.
 *
 * Var-assign has a special-case branch: in non-insert mode an
 * assignment whose target is currently a CMD_VAR_DECLARE silently
 * triggers the float-decl overwrite cascade (undeclare + var_assign
 * num_args fixup). The legacy code path encoded that here; the
 * compile function still classifies the change as REPLACE_ONE and
 * leaves the cascade to apply.
 *
 * Phase C commit 21 will move this var-decl-cascade logic into the
 * compile step (so the compile function emits UNDECLARE predef_ops
 * directly and apply just replays them), unifying with the
 * float-decl pattern. For now the helper preserves the legacy
 * behaviour while still routing through the shared apply primitives.
 */
static int apply_var_assign_change(const ReplCompiledChange *change) {
    /* Var-decl-overwrite cascade (legacy non-insert branch). The
     * compile classified the change as REPLACE_ONE; if the existing
     * slot at `pos` is a CMD_VAR_DECLARE the legacy path replays
     * the float-decl-style undeclare cascade here. Phase D folds
     * this into compile_var_assign so the predef-op plan covers it
     * directly. */
    if (change->kind == REPL_COMPILED_REPLACE_ONE &&
        change->pos < repl_state_document_count() &&
        repl_state_document_cmds_mut()[change->pos].type == CMD_VAR_DECLARE) {
        EditorBufferView text = editor_buffer_view();
        const GLCmd *old_decl = &repl_state_document_cmds_mut()[change->pos];
        for (int decl_idx = 0; decl_idx < old_decl->var_decl_count; decl_idx++) {
            const char *nm = old_decl->var_names[decl_idx];
            for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
                if (cmd_idx == change->pos) continue;
                const char *line = editor_buffer_view_line(text, cmd_idx);
                if (repl_eval_source_uses_ident(line ? line : "", nm)) {
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "variable '%s' is in use, cannot overwrite", nm);
                    set_status(buf);
                    return 1;
                }
            }
        }
        for (int decl_idx = 0; decl_idx < old_decl->var_decl_count; decl_idx++) {
            const char *nm = old_decl->var_names[decl_idx];
            int slot = repl_eval_find_predef_var_idx(nm);
            if (slot < 0) continue;
            repl_eval_undeclare_predef_var(nm);
            for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
                if (repl_state_document_cmds_mut()[cmd_idx].type == CMD_VAR_ASSIGN &&
                    repl_state_document_cmds_mut()[cmd_idx].num_args > slot)
                    repl_state_document_cmds_mut()[cmd_idx].num_args--;
            }
        }
    }

    if (!editor_commit_apply_compiled_change(change)) {
        set_status("Command buffer full!");
        return 1;
    }

    /* Cursor housekeeping per the legacy var-assign paths. */
    if (change->kind == REPL_COMPILED_INSERT_ONE) {
        if (change->pos == repl_state_document_count() - 1) {
            /* append branch — keep cursor at end (legacy behaviour). */
            repl_state_edit_line_set(repl_state_document_count());
        } else if (editor_insert_mode()) {
            repl_state_edit_line_set(repl_state_edit_line() + 1);
        }
    } else if (change->kind == REPL_COMPILED_REPLACE_ONE) {
        repl_state_edit_line_set(repl_state_edit_line() + 1);
        load_line_to_input(repl_state_edit_line());
    }

    if (change->commit_message[0])
        set_status(change->commit_message);
    {
        ReplEditorInputState *inp = editor_state_input_mut();
        inp->input[0] = '\0';
        inp->input_len = 0;
    }
    editor_cursor_pos_set(0);
    mark_normals_dirty();
    return 1;
}

int try_assign_variable(void) {
    ReplCompileContext ctx = repl_compile_context_from_live();
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult result = repl_compile_var_assign(
        editor_state_input().input, &ctx, &change, err, sizeof(err));
    if (result == REPL_COMPILE_OK && change.kind == REPL_COMPILED_NO_CHANGE)
        return 0;  /* not an assignment; fall through */
    if (result != REPL_COMPILE_OK) {
        set_status(err);
        return 1;
    }
    return apply_var_assign_change(&change);
}


int try_commit_for_loop(void) {
    /* Phase D commit 26e migration: dispatch through the
     * compile/apply pair. editor_compile_for_loop handles all three
     * branches (header replace, empty body, one-liner body).
     */
    ReplCompileContext ctx = repl_compile_context_from_live();
    EditorCommitPlan plan;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = editor_compile_for_loop(editor_state_input().input,
                                                  &ctx, &plan,
                                                  err, sizeof(err));
    if (r == REPL_COMPILE_OK &&
        plan.change.kind == REPL_COMPILED_NO_CHANGE &&
        !plan.commit_message_valid)
        return 0;  /* not a for-loop input */
    if (r != REPL_COMPILE_OK) {
        set_status(err);
        return 1;
    }

    if (!editor_commit_apply_plan(&plan)) {
        set_status("Command buffer full!");
        return 1;
    }
    mark_normals_dirty();
    return 1;
}

int try_commit_func_def(void) {
    /* Phase D commit 26f: full migration through compile/apply.
     *
     * editor_compile_func_def now handles both branches:
     *   - Overwrite header (cursor on existing CMD_FUNC_DEF in
     *     non-insert mode): REPLACE_ONE plan.
     *   - New func def with leading-comment relocation:
     *     INSERT_MANY plan with delete_pos / delete_count for the
     *     comment range and func_decl_resume_publish for the
     *     post-effect that publishes resume_delta into the
     *     close-brace bookkeeping.
     */
    ReplCompileContext ctx = repl_compile_context_from_live();
    EditorCommitPlan plan;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = editor_compile_func_def(editor_state_input().input,
                                                  &ctx, &plan,
                                                  err, sizeof(err));
    if (r == REPL_COMPILE_OK &&
        plan.change.kind == REPL_COMPILED_NO_CHANGE &&
        !plan.commit_message_valid)
        return 0;  /* not a func decl input */
    if (r != REPL_COMPILE_OK) {
        set_status(err);
        return 1;
    }

    if (!editor_commit_apply_plan(&plan)) {
        set_status("Command buffer full!");
        return 1;
    }
    mark_normals_dirty();
    return 1;
}


int try_commit_if_block(void) {
    /* Phase D commit 26c migration: dispatch through the
     * compile/apply pair. editor_compile_if_block produces the
     * EditorCommitPlan; editor_commit_apply_plan replays both
     * halves atomically.
     */
    ReplCompileContext ctx = repl_compile_context_from_live();
    EditorCommitPlan plan;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = editor_compile_if_block(editor_state_input().input,
                                                  &ctx, &plan,
                                                  err, sizeof(err));
    if (r == REPL_COMPILE_OK &&
        plan.change.kind == REPL_COMPILED_NO_CHANGE &&
        !plan.commit_message_valid)
        return 0;  /* not an if-block input */
    if (r != REPL_COMPILE_OK) {
        set_status(err);
        return 1;
    }

    if (!editor_commit_apply_plan(&plan)) {
        set_status("Command buffer full!");
        return 1;
    }
    mark_normals_dirty();
    return 1;
}

int try_commit_close_brace(void) {
    /* Phase D commit 26b migration: dispatch through the
     * compile/apply pair. The compile step (editor_compile_close_brace)
     * inspects the input and current scope state and produces an
     * EditorCommitPlan; editor_commit_apply_plan replays the REPL
     * change + editor post-effects atomically.
     *
     * Returns 0 (input not recognized as a close-brace) on
     * NO_CHANGE, matching the legacy contract.
     */
    ReplCompileContext ctx = repl_compile_context_from_live();
    EditorCommitPlan plan;
    char err[REPL_STATUS_TEXT_MAX];

    ReplCompileResult r = editor_compile_close_brace(editor_state_input().input,
                                                     &ctx, &plan,
                                                     err, sizeof(err));
    if (r == REPL_COMPILE_OK &&
        plan.change.kind == REPL_COMPILED_NO_CHANGE &&
        !plan.commit_message_valid)
        return 0;  /* not a close-brace context */
    if (r != REPL_COMPILE_OK) {
        set_status(err);
        return 1;
    }

    if (!editor_commit_apply_plan(&plan)) {
        set_status("Command buffer full!");
        return 1;
    }
    mark_normals_dirty();
    return 1;
}

/* Block-structural commit handlers: `}`, `for(`, `funcN`, `if(`.
 * These inspect the start of g_input and are mutually exclusive with
 * var-statement handlers. */
int try_commit_block_structs(void) {
    if (try_commit_close_brace()) return 1;
    if (try_commit_for_loop())    return 1;
    if (try_commit_func_def())    return 1;
    if (try_commit_if_block())    return 1;
    return 0;
}

/* Statement-level commit handlers. float decl MUST precede assign so that
 * `float x` is not misread as an assignment to an identifier named "float". */
int try_commit_var_statements(void) {
    if (try_commit_float_decl())  return 1;
    if (try_assign_variable())    return 1;
    return 0;
}

/* Canonical order: var statements first (float/assign), then block structs.
 * Handlers are mutually exclusive on input prefix, so the relative ordering
 * of the two groups doesn't affect observed behavior. */
int try_commit_any(void) {
    if (try_commit_var_statements()) return 1;
    if (try_commit_block_structs())  return 1;
    return 0;
}

/* Overwrite-mode Enter variant: on successful var-statement commit, enter
 * insert mode and clear the input. Assign additionally publishes the
 * "Insert mode" status and marks normals dirty (float decl does not). */
int try_commit_var_statements_then_insert(void) {
    if (try_commit_float_decl()) {
        editor_insert_mode_set(1);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            inp->input[0] = '\0';
            inp->input_len = 0;
        }
        editor_cursor_pos_set(0);
        clear_autocomplete_state();
        return 1;
    }
    if (try_assign_variable()) {
        editor_insert_mode_set(1);
        {
            ReplEditorInputState *inp = editor_state_input_mut();
            inp->input[0] = '\0';
            inp->input_len = 0;
        }
        editor_cursor_pos_set(0);
        clear_autocomplete_state();
        set_status("Insert mode");
        mark_normals_dirty();
        return 1;
    }
    return 0;
}
