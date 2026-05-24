/*
 * src/repl/apply.c -- Apply a ReplCompiledChange to ReplState command arrays.
 *
 * Pure mutation half of the compile/apply split:
 *   - touches ReplState command arrays only,
 *   - never touches the source-text document (callers drive that
 *     through the editor's editor_buffer_apply_compiled_change OR
 *     the neutral source_document_apply_change port),
 *   - never touches status,
 *   - never pushes an undo entry.
 *
 * Failure mode: cmd-store insert overflow returns 0; the caller
 * decides whether to surface that as a status message. All other
 * paths assume validation already passed in repl_compile.
 */
#include "repl/apply.h"

#include "repl/command_store.h"
#include "repl/eval.h"
#include "repl/state_owners.h"

int repl_apply_can_apply_compiled_change(const ReplCompiledChange *change) {
    if (!change) return 0;

    ReplCommandStore store = repl_command_store_live();
    int doc_count = repl_command_store_count(&store);
    int capacity  = repl_command_store_capacity(&store);

    /* Optional pre-insert delete: validate the delete range against
     * the live document. The deleted count is removed before the
     * insert capacity check below. */
    int delete_count = (change->delete_count > 0) ? change->delete_count : 0;
    if (delete_count > 0) {
        if (change->delete_pos < 0 ||
            change->delete_pos >= doc_count ||
            change->delete_pos + delete_count > doc_count)
            return 0;
    }
    int post_delete_count = doc_count - delete_count;
    int post_delete_capacity_headroom = capacity - post_delete_count;

    switch (change->kind) {
    case REPL_COMPILED_NO_CHANGE:
        /* A delete-only change is legal but unusual; the dedicated
         * DELETE_RANGE kind covers it more clearly. NO_CHANGE +
         * delete_count > 0 is rejected to keep the contract crisp. */
        return delete_count == 0;
    case REPL_COMPILED_INSERT_ONE:
        if (change->pos < 0 || change->pos > post_delete_count) return 0;
        return post_delete_capacity_headroom >= 1;
    case REPL_COMPILED_INSERT_MANY:
        if (change->count <= 0 || change->count > MAX_COMMIT_CMDS) return 0;
        if (change->pos < 0 || change->pos > post_delete_count) return 0;
        return post_delete_capacity_headroom >= change->count;
    case REPL_COMPILED_REPLACE_ONE:
        if (delete_count > 0) return 0;  /* compound replace+delete not supported */
        return change->pos >= 0 && change->pos < doc_count;
    case REPL_COMPILED_DELETE_RANGE: {
        if (delete_count > 0) return 0;  /* use delete_pos/count + INSERT_MANY instead */
        int s = 0, c = 0;
        return repl_command_store_normalize_range(&store, change->pos,
                                                  change->count, &s, &c);
    }
    }
    return 0;
}

int repl_apply_compiled_change(const ReplCompiledChange *change,
                               int *cursor_inout) {
    if (!change) return 0;

    ReplCommandStore store = repl_command_store_live();
    ReplStoreMutOpts insert_opts = {
        .flags        = change->adjust_edit_line
                            ? REPL_COMMAND_STORE_ADJUST_EDIT_LINE : 0,
        .cursor_inout = change->adjust_edit_line ? cursor_inout : NULL,
    };

    /* Optional pre-insert delete fires first so `change->pos` —
     * which is interpreted in post-delete coordinates — refers to
     * the right slot when the insert lands.
     *
     * Pass NULL opts for the pre-delete to preserve the historical
     * behavior where the delete did not shift the cursor.
     * Forwarding the caller pointer to both delete and insert
     * would change the composed cursor delta for in-place replace
     * paths (overwrite replacements, toggle-comment batches)
     * (see plans/in-review/edit-line-ownership.md §1.3). */
    if (change->delete_count > 0) {
        if (!repl_command_store_delete_range(&store, change->delete_pos,
                                             change->delete_count, NULL))
            return 0;
    }

    switch (change->kind) {
    case REPL_COMPILED_NO_CHANGE:
        return 1;
    case REPL_COMPILED_INSERT_ONE:
        return repl_command_store_insert_one(&store, change->pos,
                                             &change->cmds[0], &insert_opts);
    case REPL_COMPILED_INSERT_MANY:
        return repl_command_store_insert_many(&store, change->pos,
                                              change->cmds, change->count,
                                              &insert_opts);
    case REPL_COMPILED_REPLACE_ONE:
        return repl_command_store_replace_one(&store, change->pos,
                                              &change->cmds[0]);
    case REPL_COMPILED_DELETE_RANGE: {
        /* Standalone delete: forward cursor unconditionally; the
         * adjust_edit_line flag historically gates inserts only.
         * Callers that don't want cursor math pass NULL from their
         * own caller site. */
        ReplStoreMutOpts delete_opts = {
            .flags        = 0,
            .cursor_inout = cursor_inout,
        };
        return repl_command_store_delete_range(&store, change->pos,
                                               change->count, &delete_opts);
    }
    }
    return 0;
}

void repl_apply_predef_ops(const ReplCompiledChange *change) {
    if (!change) return;

    /* UNDECLARE first so the var_assign var_idx cascade observes
     * the correct pre-removal slot indices. */
    for (int op_idx = 0; op_idx < change->predef_op_count; op_idx++) {
        const ReplPredefOp *op = &change->predef_ops[op_idx];
        if (op->kind != REPL_PREDEF_OP_UNDECLARE) continue;
        int slot = repl_eval_find_predef_var_idx(op->name);
        if (slot < 0) continue;
        repl_eval_undeclare_predef_var(op->name);
        for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
            if (repl_state_document_cmds()[cmd_idx].type == CMD_VAR_ASSIGN &&
                repl_state_document_cmds()[cmd_idx].var_idx > slot)
                repl_state_document_cmds_mut()[cmd_idx].var_idx--;
        }
    }

    for (int op_idx = 0; op_idx < change->predef_op_count; op_idx++) {
        const ReplPredefOp *op = &change->predef_ops[op_idx];
        if (op->kind == REPL_PREDEF_OP_DECLARE) {
            repl_eval_declare_predef_var(op->name, NULL, 0);
            if (op->has_value) {
                int idx = repl_eval_find_predef_var_idx(op->name);
                if (idx >= 0) g_predef_vars[idx].value = op->value;
            }
        } else if (op->kind == REPL_PREDEF_OP_SET_VALUE && op->has_value) {
            int idx = repl_eval_find_predef_var_idx(op->name);
            if (idx >= 0) g_predef_vars[idx].value = op->value;
        }
    }
}

void repl_apply_scratch_ops(const ReplCompiledChange *change) {
    if (!change)
        return;

    for (int op_idx = 0; op_idx < change->scratch_op_count; op_idx++) {
        const ReplScratchOp *op = &change->scratch_ops[op_idx];
        repl_eval_scratch_set(op->array_idx, op->elem_idx, op->value);
    }
}
