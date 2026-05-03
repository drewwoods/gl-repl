/*
 * editor_commit.c -- Editor-side orchestration for compile/apply commits.
 *
 * The orchestration shape is the dual of repl_compile():
 *
 *   editor_commit_apply_compiled_change(change)
 *       preflight repl_apply_can_apply_compiled_change(change)
 *       repl_apply_predef_ops(change)             // predef-var cascade
 *       editor_buffer_apply_compiled_change(change)  // EditorState only
 *       repl_apply_compiled_change(change)        // ReplState only
 *
 * Preflight gives the helper an all-or-nothing atomicity guarantee:
 * if the cmd-store can't accept the change, none of the three
 * halves run, so predef-vars, editor buffer, and cmd-store stay in
 * sync. Without the preflight a capacity failure would leave
 * predef-vars declared and (potentially) editor text written but
 * no cmd-store entry — exactly the partial-commit state the Phase C
 * transaction shape exists to prevent.
 *
 * The undo capture for migrated handlers still rides on
 * repl_undo_push_snapshot() pushed at the dispatch sites
 * (;-key, Enter, feed_line) in repl_editor.c. Phase D's editor_input
 * carve will replace that with a per-commit transaction wrapping
 * this helper.
 */

#include "editor_commit.h"

#include "editor_state.h"
#include "repl_apply.h"
#include "repl_compile.h"

int editor_commit_apply_compiled_change(const struct ReplCompiledChange_s *change) {
    if (!change) return 0;

    /* Preflight: if the cmd-store can't accept the change, return
     * 0 before mutating anything. This is the load-bearing
     * atomicity guarantee — without it, a capacity failure leaves
     * predef-vars and/or editor buffer mutated while cmd-store is
     * still pre-change. */
    if (!repl_apply_can_apply_compiled_change(change))
        return 0;

    /* Past the preflight every apply call below succeeds. */
    repl_apply_predef_ops(change);
    editor_buffer_apply_compiled_change(change);
    repl_apply_compiled_change(change);
    return 1;
}
