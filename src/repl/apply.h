/*
 * src/repl/apply.h - Apply a ReplCompiledChange to ReplState command arrays.
 *
 * The dual of repl_compile. The pair is wired into two apply paths,
 * each owning its own text-mutation step:
 *
 *   Editor-input commit (src/editor/commit.c):
 *     editor_undo_begin
 *     editor_buffer_apply_compiled_change   (EditorState text)
 *     repl_apply_compiled_change            (ReplState command store)
 *     editor_undo_commit
 *
 *   Lean source-loader (src/repl/load.c):
 *     repl_compiled_change_to_text_change   (translate)
 *     source_document_apply_change          (neutral text port)
 *     repl_apply_compiled_change            (ReplState command store)
 *
 * `repl_apply_compiled_change()` mutates ReplState command arrays
 * only. It does not touch source text, status, undo entries, or
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

#include "repl/compile.h"

/* Pure preflight check: would `repl_apply_compiled_change(change)`
 * succeed against the live cmd-store right now? Returns 1 if the
 * change fits within the cmd-store's capacity and (for delete /
 * replace) within the current cmd-count bounds. Returns 0 if the
 * apply would fail. Reads ReplState; never mutates.
 *
 * `editor_commit_apply_compiled_change` calls this before doing
 * any mutation so a single capacity failure can't leave predef-vars
 * declared with the cmd-store / editor-buffer untouched. */
int  repl_apply_can_apply_compiled_change(const ReplCompiledChange *change);

/* Apply the source-command portion of `change` to ReplState's
 * command array. Returns 1 on success, 0 on capacity failure or
 * out-of-bounds. NO_CHANGE is a no-op success.
 *
 * `cursor_inout` is an optional caller-owned cursor pointer. When
 * non-NULL and `change->adjust_edit_line` is set, INSERT_ONE /
 * INSERT_MANY pass it through to the store so the standard insert
 * math shifts the caller's int. DELETE_RANGE passes it
 * unconditionally (gating is a caller choice — pass NULL when no
 * cursor math is desired). Pre-insert deletes (the optional
 * `delete_count > 0` path that runs before INSERT_*) pass NULL to
 * preserve the historical behavior where the pre-delete did not
 * shift the cursor. REPLACE_ONE has no cursor parameter — replace
 * never shifts.
 *
 * Apply itself never reads or writes editor state. The cursor
 * lives where the caller put it.
 *
 * Callers that drive the full transaction shape (predef-ops +
 * editor-buffer + cmd-store) should preflight with
 * `repl_apply_can_apply_compiled_change()` before any mutation;
 * otherwise a partial commit can leave predef declarations or
 * editor text without their matching cmd-store entry. The
 * preflight + apply pair is wrapped by
 * `editor_commit_apply_compiled_change()`. */
int  repl_apply_compiled_change(const ReplCompiledChange *change,
                                int *cursor_inout);

/* Replay the predef-variable side-effects in `change` against the
 * shared eval table. UNDECLARE entries fire first (and cascade
 * num_args adjustments to CMD_VAR_ASSIGN cmds whose slot index sits
 * above the freed slot); then DECLARE / SET_VALUE entries write the
 * new state. */
void repl_apply_predef_ops(const ReplCompiledChange *change);

/* Replay scratch-array side-effects in `change` against the evaluator's
 * bound scratch storage. */
void repl_apply_scratch_ops(const ReplCompiledChange *change);

#endif /* REPL_APPLY_H */
