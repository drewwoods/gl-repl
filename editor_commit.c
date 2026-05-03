/*
 * editor_commit.c -- Editor-side orchestration for compile/apply commits.
 *
 * The orchestration shape is the dual of repl_compile():
 *
 *   editor_commit_apply_compiled_change(change)
 *       preflight repl_apply_can_apply_compiled_change(change)
 *       services.apply_predef_ops(change)         // predef-var cascade
 *       editor_buffer_apply_compiled_change(change)  // EditorState only
 *       services.apply_repl_change(change)        // ReplState only
 *
 * The mutating halves go through the EditorServices table so the
 * editor doesn't reach into `repl_apply_*` directly. Phase D commit
 * 25 grows this into the full `editor_commit_current_input` shape
 * (compile, undo, preflight, apply); for now this helper just
 * threads services through the apply path that already exists.
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
 * (;-key, Enter, feed_line) in repl_editor.c. Phase D commit 25
 * replaces that with a per-commit transaction wrapping this
 * helper.
 */

#include "editor_commit.h"

#include "editor_services.h"
#include "editor_state.h"
#include "repl_apply.h"
#include "repl_compile.h"

int editor_commit_apply_compiled_change(const struct ReplCompiledChange_s *change) {
    if (!change) return 0;

    /* Preflight: if the cmd-store can't accept the change, return
     * 0 before mutating anything. Pure read; doesn't go through
     * services. */
    if (!repl_apply_can_apply_compiled_change(change))
        return 0;

    /* Past the preflight every apply call below succeeds. */
    EditorServices svc = editor_services_default();
    svc.apply_predef_ops(change, svc.user);
    editor_buffer_apply_compiled_change(change);
    svc.apply_repl_change(change, svc.user);
    return 1;
}
