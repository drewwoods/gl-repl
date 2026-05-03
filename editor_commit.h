/*
 * editor_commit.h - Editor-side orchestration for compile/apply commits.
 *
 * Phase C contract:
 *
 *   editor_commit_apply_compiled_change(change)
 *       The editor commit orchestration entry. Assumes the change
 *       has already been validated by repl_compile_*(). Applies
 *       predef-var ops, the editor-buffer half, and the cmd-store
 *       half together — the canonical transaction shape:
 *
 *           editor_undo_begin                  (snapshot pre-state)
 *           repl_apply_predef_ops(change)      (declare/undeclare/set)
 *           editor_buffer_apply_compiled_change(change)
 *           repl_apply_compiled_change(change)
 *           editor_undo_commit                 (no-op today; the
 *                                                undo ring captures
 *                                                a single snapshot
 *                                                pre-mutation, which
 *                                                already covers all
 *                                                three halves)
 *
 *       Returns 1 on success, 0 on cmd-store capacity failure. On
 *       capacity failure no editor-buffer or cmd-store mutation has
 *       landed (the cmd-store call is the last thing tried), but
 *       predef-var ops have already replayed; callers are expected
 *       to either treat capacity failure as terminal (no recovery
 *       needed) or to restore via the undo entry pushed at entry.
 *
 *       Phase C commit 21 keeps the existing repl_undo_push_snapshot
 *       capture point at the ;-key / Enter / feed_line dispatch
 *       sites; this helper is the building block Phase D's
 *       editor_commit.c wires into the editor input handlers
 *       directly, replacing those higher-level undo pushes with a
 *       per-commit transaction.
 */
#ifndef EDITOR_COMMIT_H
#define EDITOR_COMMIT_H

struct ReplCompiledChange_s;

/* Apply a compiled change to ReplState + EditorState atomically.
 * Returns 1 on success, 0 on cmd-store capacity failure. Does not
 * call set_status; callers surface diagnostics. */
int editor_commit_apply_compiled_change(const struct ReplCompiledChange_s *change);

#endif /* EDITOR_COMMIT_H */
