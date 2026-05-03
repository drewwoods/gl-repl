/*
 * editor_commit.h - Editor-side orchestration for compile/apply commits.
 *
 * Phase C contract:
 *
 *   editor_commit_apply_compiled_change(change)
 *       The editor commit orchestration entry. Assumes the change
 *       has already been validated by repl_compile_*(). The
 *       canonical transaction shape:
 *
 *           preflight (repl_apply_can_apply_compiled_change)
 *               If the cmd-store can't accept the change, return 0
 *               immediately. No predef-var, editor-buffer, or
 *               cmd-store mutation has happened.
 *
 *           repl_apply_predef_ops(change)        (declare/undeclare/set)
 *           editor_buffer_apply_compiled_change(change)
 *           repl_apply_compiled_change(change)
 *
 *       Returns 1 on success, 0 if the preflight rejects the
 *       change. The atomicity invariant: a 0 return means *no*
 *       mutation landed across any of the three halves; a 1 return
 *       means *all three* halves landed. Callers do not have to
 *       roll back on a 0 return.
 *
 *       Does not call set_status. Callers surface diagnostics
 *       through their own return values.
 *
 *       Phase C still rides on repl_undo_push_snapshot() at the
 *       ;-key / Enter / feed_line dispatch sites; this helper is
 *       the building block Phase D wires into the editor input
 *       handlers directly with a per-commit transaction.
 */
#ifndef EDITOR_COMMIT_H
#define EDITOR_COMMIT_H

struct ReplCompiledChange_s;

/* Apply a compiled change atomically:
 *   - Returns 1 if all three halves (predef-ops, editor buffer,
 *     cmd store) landed successfully.
 *   - Returns 0 if the preflight detected the cmd-store can't
 *     accept the change. On a 0 return no mutation occurred —
 *     predef-vars, editor buffer, and cmd-store are all unchanged.
 *
 * Does not call set_status; callers surface diagnostics. */
int editor_commit_apply_compiled_change(const struct ReplCompiledChange_s *change);

#endif /* EDITOR_COMMIT_H */
