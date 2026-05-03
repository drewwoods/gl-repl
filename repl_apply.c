/*
 * repl_apply.c -- Apply a ReplCompiledChange to ReplState command arrays.
 *
 * Pure mutation half of the compile/apply split:
 *   - touches ReplState command arrays only,
 *   - never touches the editor buffer (that's
 *     editor_buffer_apply_compiled_change in editor_state.c),
 *   - never touches status,
 *   - never pushes an undo entry.
 *
 * Failure mode: cmd-store insert overflow returns 0; the caller
 * decides whether to surface that as a status message. All other
 * paths assume validation already passed in repl_compile.
 */
#include "repl_apply.h"

#include "repl_command_store.h"
#include "repl_eval.h"
#include "repl_state.h"

int repl_apply_compiled_change(const ReplCompiledChange *change) {
    if (!change) return 0;

    ReplCommandStore store = repl_command_store_live();
    int flags = change->adjust_edit_line ? REPL_COMMAND_STORE_ADJUST_EDIT_LINE : 0;

    switch (change->kind) {
    case REPL_COMPILED_NO_CHANGE:
        return 1;
    case REPL_COMPILED_INSERT_ONE:
        return repl_command_store_insert_one(&store, change->pos,
                                             &change->cmds[0], flags);
    case REPL_COMPILED_INSERT_MANY:
        return repl_command_store_insert_many(&store, change->pos,
                                              change->cmds, change->count,
                                              flags);
    case REPL_COMPILED_REPLACE_ONE:
        return repl_command_store_replace_one(&store, change->pos,
                                              &change->cmds[0]);
    case REPL_COMPILED_DELETE_RANGE:
        return repl_command_store_delete_range(&store, change->pos, change->count);
    case REPL_COMPILED_LOAD_ALL:
        return repl_command_store_load(&store, change->cmds, change->count,
                                       change->pos);
    }
    return 0;
}

void repl_apply_predef_ops(const ReplCompiledChange *change) {
    if (!change) return;

    /* UNDECLARE first so the var_assign num_args cascade observes
     * the correct pre-removal slot indices. */
    for (int op_idx = 0; op_idx < change->predef_op_count; op_idx++) {
        const ReplPredefOp *op = &change->predef_ops[op_idx];
        if (op->kind != REPL_PREDEF_OP_UNDECLARE) continue;
        int slot = repl_eval_find_predef_var_idx(op->name);
        if (slot < 0) continue;
        repl_eval_undeclare_predef_var(op->name);
        for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
            if (repl_state_document_cmds_mut()[cmd_idx].type == CMD_VAR_ASSIGN &&
                repl_state_document_cmds_mut()[cmd_idx].num_args > slot)
                repl_state_document_cmds_mut()[cmd_idx].num_args--;
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
