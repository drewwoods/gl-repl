/*
 * repl_apply.h - Apply a ReplCompiledChange to ReplState command arrays.
 *
 * The dual of repl_compile. The pair is wired into the editor commit
 * orchestration so a single transaction drives both halves:
 *
 *   editor_undo_begin
 *   editor_buffer_apply_compiled_change   (EditorState text only)
 *   repl_apply_compiled_change            (ReplState command store only)
 *   editor_undo_commit
 *
 * `repl_apply_compiled_change()` mutates ReplState command arrays
 * only. It does not touch editor text, status, undo entries, or
 * predef-variable registrations. The predef-variable cascade is
 * applied separately through `repl_apply_predef_ops()` so callers
 * can sequence it correctly relative to undo capture.
 *
 * The apply functions assume the change has already been validated
 * by `repl_compile_*()`. Capacity overflow is the only failure mode
 * (returns 0 when the cmd-store can't accept an insert).
 */
#ifndef REPL_APPLY_H
#define REPL_APPLY_H

#include "repl_compile.h"

/* Apply the source-command portion of `change` to ReplState's
 * command array. Returns 1 on success, 0 on capacity failure or
 * out-of-bounds. NO_CHANGE is a no-op success. */
int  repl_apply_compiled_change(const ReplCompiledChange *change);

/* Replay the predef-variable side-effects in `change` against the
 * shared eval table. UNDECLARE entries fire first (and cascade
 * num_args adjustments to CMD_VAR_ASSIGN cmds whose slot index sits
 * above the freed slot); then DECLARE / SET_VALUE entries write the
 * new state. */
void repl_apply_predef_ops(const ReplCompiledChange *change);

#endif /* REPL_APPLY_H */
