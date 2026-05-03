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
#include "repl_undo.h"

#include <stdio.h>
#include <string.h>

static void result_set_diagnostic(EditorCommitResult *result, const char *msg) {
    if (!result || !msg) return;
    strncpy(result->diagnostic, msg, sizeof(result->diagnostic) - 1);
    result->diagnostic[sizeof(result->diagnostic) - 1] = '\0';
    result->diagnostic_valid = 1;
}

static void result_set_commit_message(EditorCommitResult *result,
                                      const char *msg) {
    if (!result || !msg) return;
    strncpy(result->commit_message, msg, sizeof(result->commit_message) - 1);
    result->commit_message[sizeof(result->commit_message) - 1] = '\0';
    result->commit_message_valid = 1;
}

EditorCommitResult editor_commit_current_input(const struct EditorServices_s *services) {
    EditorCommitResult result = {0};
    if (!services) return result;

    const char *text = editor_input_text();

    ReplCompileContext ctx = services->context(services->user);
    ReplCompiledChange change;
    char err[REPL_STATUS_TEXT_MAX];
    err[0] = '\0';

    ReplCompileResult cr = services->compile(text, &ctx, &change,
                                             err, sizeof(err),
                                             services->user);

    if (cr == REPL_COMPILE_ERROR) {
        /* Diagnostic flows through the result; no mutation. */
        result.consumed = 1;
        result_set_diagnostic(&result, err);
        return result;
    }

    if (change.kind == REPL_COMPILED_NO_CHANGE) {
        /* Caller falls through to the legacy try_commit_* chain
         * (or treats as unrecognized). */
        result.consumed = 0;
        return result;
    }

    /* Preflight before mutation. */
    if (!repl_apply_can_apply_compiled_change(&change)) {
        result.consumed = 1;
        result.capacity_failed = 1;
        return result;
    }

    /* Transaction boundary: compile + preflight succeeded, no
     * mutation has run yet. Capture undo here. */
    repl_undo_push_snapshot();

    /* Past the preflight every apply call below succeeds. */
    services->apply_predef_ops(&change, services->user);
    editor_buffer_apply_compiled_change(&change);
    services->apply_repl_change(&change, services->user);

    result.consumed = 1;
    result.mutated = 1;
    if (change.commit_message[0])
        result_set_commit_message(&result, change.commit_message);
    return result;
}

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
