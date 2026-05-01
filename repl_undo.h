/*
 * repl_undo.h - Undo/redo state snapshots and command history.
 *
 * Implements undo/redo using circular snapshot buffers: one ring for undo
 * history (32 slots) and one for redo history (32 slots). Each snapshot
 * captures the complete editor state at a point in time.
 *
 * Snapshot contents: source command array, command count, cursor position
 * (edit_line), predefined variable state (values and names for user-declared
 * floats). This allows reverting to any prior point with full state recovery,
 * including animations (t value) and user variables (x, y, z, etc.).
 *
 * Lifecycle: repl_undo_push_snapshot() called before any mutation (delete,
 * paste, reformat, etc.) saves current state to the undo ring and clears the
 * redo ring. Ctrl+Z calls repl_undo_pop_snapshot() to restore the previous
 * snapshot and move current state to redo. Ctrl+Y calls repl_undo_do_redo()
 * to restore a redo snapshot and move current state back to undo.
 *
 * Example auto-promotion hook: repl_undo_push_snapshot() is the entrypoint
 * where repl_promote_example_if_needed() fires. If the user is editing a
 * built-in example, a fresh user scene slot is allocated, the current state
 * is copied into it, the slot is named (derived from the example name, with
 * de-duplication), and control flow returns — the user continues editing
 * without interruption. Subsequent mutations accumulate into the user scene.
 *
 * Ring state capture: repl_undo_ring_state_capture() and
 * repl_undo_ring_state_restore() allow tests and tools to query/restore the
 * internal head/count pointers without exposing the full snapshot arrays.
 * Snapshot capture/restore helpers allow external callers (tests, import/export)
 * to manually snapshot and restore state without using the history rings.
 */

#ifndef REPL_UNDO_H
#define REPL_UNDO_H

#include "sample.h"

/* A captured snapshot of complete editor state: source commands, cursor
 * position, and predefined variable state. Used by undo/redo history rings
 * and by import/export to preserve full state across save/load boundaries. */
typedef struct {
    GLCmd cmds[MAX_COMMANDS];
    char  editor_lines[MAX_COMMANDS][MAX_LINE_LEN];
    int   num_cmds;
    int   edit_line;
    float predef_vals[MAX_PREDEF_VARS];
    char  predef_names[MAX_PREDEF_VARS][16];
    int   num_predef_vars;
} ReplUndoSnapshot;

/* Ring state descriptors: exposed for test access to undo/redo ring pointers
 * without revealing the full history buffers. undo_head is the index of the
 * oldest snapshot still in the ring; undo_count is the number of snapshots.
 * Similarly for redo. Used by tests that verify undo/redo ordering. */
typedef struct {
    int undo_head;
    int undo_count;
    int redo_head;
    int redo_count;
} ReplUndoRingState;

/* Save/restore helpers for manual snapshot capture and restore. Used by
 * import/export code to preserve full state without involving the history
 * rings. repl_undo_snapshot_save() captures current editor state;
 * repl_undo_snapshot_restore() reverts to a prior snapshot. */
void repl_undo_snapshot_save(ReplUndoSnapshot *snapshot);
void repl_undo_snapshot_restore(const ReplUndoSnapshot *snapshot);

/* Ring state inspection: capture and restore the internal undo/redo head/count
 * pointers. Used by tests to verify ring state without exposing the history
 * buffers themselves. */
void repl_undo_ring_state_capture(ReplUndoRingState *state);
void repl_undo_ring_state_restore(const ReplUndoRingState *state);

/* Undo/redo operations. repl_undo_push_snapshot() saves current editor state
 * to the undo ring and clears the redo ring; called before any mutation.
 * repl_undo_pop_snapshot() restores the most recent undo snapshot (Ctrl+Z),
 * moving current state to redo. repl_undo_do_redo() restores the most recent
 * redo snapshot (Ctrl+Y), moving current state back to undo. */
void repl_undo_push_snapshot(void);
void repl_undo_pop_snapshot(void);
void repl_undo_do_redo(void);

#endif /* REPL_UNDO_H */
