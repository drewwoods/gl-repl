/*
 * editor_commit.h - Editor-side orchestration for compile/apply commits.
 *
 * The orchestration shape is the dual of repl_compile():
 *
 *   editor_commit_current_input(services)
 *       1. compile through services
 *       2. on compile failure: return diagnostic via result;
 *          no buffer/store/status/undo mutation
 *       3. on NO_CHANGE: return consumed=0; caller falls through
 *       4. on compile success:
 *            preflight via repl_apply_can_apply_compiled_change
 *            on preflight failure: return capacity_failed=1; no
 *              mutation
 *            on preflight success:
 *              capture undo pre-state (transaction boundary)
 *              services.apply_predef_ops(change)
 *              editor_buffer_apply_compiled_change(change)
 *              services.apply_repl_change(change)
 *              return mutated=1, commit_message_valid populated
 *
 *   editor_commit_apply_compiled_change(change)
 *       Lower-level helper used by legacy try_commit_* handlers
 *       during the migration. Same transaction shape as the steps
 *       inside editor_commit_current_input from preflight onward;
 *       does not run compile or capture undo. Migrating handlers
 *       move from this helper to editor_commit_current_input as
 *       Phase D commits 26a-26e land.
 *
 * The undo capture sits AFTER successful compile + preflight but
 * BEFORE the first mutation — it is a transaction boundary tied to
 * the act of mutating, not to "successful commit" which would risk
 * capturing post-state.
 *
 * Diagnostic / commit-message text uses explicit *_valid flags so
 * empty string is never a signal.
 */
#ifndef EDITOR_COMMIT_H
#define EDITOR_COMMIT_H

#include "repl_state_views.h"  /* REPL_STATUS_TEXT_MAX */

struct ReplCompiledChange_s;
struct EditorServices_s;

/* Result of an editor commit attempt. The booleans say what
 * happened; the strings carry diagnostic / commit-message text.
 * Empty string is NEVER a signal — check the matching `*_valid`
 * flag. */
typedef struct {
    int  consumed;             /* dispatcher recognized the input */
    int  mutated;              /* state changed; undo entry pushed */
    int  capacity_failed;      /* preflight rejected on capacity */
    int  diagnostic_valid;     /* diagnostic[] holds a compile error */
    int  commit_message_valid; /* commit_message[] holds a success msg */
    char diagnostic[REPL_STATUS_TEXT_MAX];
    char commit_message[REPL_STATUS_TEXT_MAX];
} EditorCommitResult;

/* Run one commit attempt against the live editor input.
 *
 * Reads the editor's current input buffer, asks services to
 * compile + apply, and captures undo at the transaction boundary
 * (between successful compile/preflight and the first mutation).
 * Returns an EditorCommitResult describing what happened; never
 * calls set_status itself.
 *
 * Phase D commit 25 lands this entry; commit 26a routes ;-key /
 * Enter / feed_line through it for the already-migrated simple
 * commit paths (float-decl, var-assign). Structured commits
 * continue to use the legacy try_commit_* chain through
 * editor_commit_apply_compiled_change until commits 26b-26e
 * migrate them. */
EditorCommitResult editor_commit_current_input(const struct EditorServices_s *services);

/* Apply a compiled change atomically. Lower-level helper used by
 * legacy try_commit_* handlers during the migration. Same
 * transaction shape as editor_commit_current_input from preflight
 * onward; does not run compile or capture undo.
 *
 *   Returns 1 if all three halves (predef-ops, editor buffer,
 *     cmd store) landed successfully.
 *   Returns 0 if the preflight detected the cmd-store can't
 *     accept the change. On a 0 return no mutation occurred —
 *     predef-vars, editor buffer, and cmd-store are all unchanged.
 *
 * Does not call set_status; callers surface diagnostics. */
int editor_commit_apply_compiled_change(const struct ReplCompiledChange_s *change);

#endif /* EDITOR_COMMIT_H */
