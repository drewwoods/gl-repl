/*
 * editor_undo.h - Undo/redo state snapshots and command history.
 *
 * Implements undo/redo using circular snapshot buffers: one ring for undo
 * history (32 slots) and one for redo history (32 slots). Each snapshot
 * captures the editor's *document* state at a point in time - not the
 * full session.
 *
 * Snapshot contents (see EditorUndoSnapshot): source command array +
 * count, the per-line editor text buffer, cursor position (edit_line),
 * predefined variable values + names, the A/B/C scratch arrays, and the
 * funcN aliases. That is enough to recover the program and its runtime
 * variables (including the animated `t` and user floats x/y/z).
 *
 * Deliberately NOT snapshotted: input-buffer text, selection, clipboard,
 * search, autocomplete, and scroll position. Those are transient view /
 * editing state; an undo restores the document, not the cursor's
 * in-progress typing or what was selected.
 *
 * Lifecycle: editor_undo_push_snapshot() called before any mutation (delete,
 * paste, reformat, etc.) saves current state to the undo ring and clears the
 * redo ring. Ctrl+Z calls editor_undo_pop_snapshot() to restore the previous
 * snapshot and move current state to redo. Ctrl+Y calls editor_undo_do_redo()
 * to restore a redo snapshot and move current state back to undo.
 *
 * Transient auto-promotion hook: editor_undo_push_snapshot() is the entrypoint
 * where repl_promote_transient_if_needed() fires. If the user is editing a
 * built-in example - or the document a completed/stopped tutorial left behind
 * - a fresh user scene slot is allocated, the current state is copied into it,
 * the slot is named (derived from the example / tutorial name, with
 * de-duplication), and control flow returns - the user continues editing
 * without interruption. Subsequent mutations accumulate into the user scene.
 * An ACTIVE tutorial is deliberately not promotable: its own step commits
 * come through here, and promoting would end the lesson at step 0.
 *
 * Ring state capture: editor_undo_ring_state_capture() and
 * editor_undo_ring_state_restore() allow tests and tools to query/restore the
 * internal head/count pointers without exposing the full snapshot arrays.
 * Snapshot capture/restore helpers allow external callers (tests, import/export)
 * to manually snapshot and restore state without using the history rings.
 */

#ifndef EDITOR_UNDO_H
#define EDITOR_UNDO_H

#include "repl/command.h"
#include "repl/eval.h"

/* A captured snapshot of editor document state: source commands, the
 * per-line text buffer, cursor position, predefined variables, scratch
 * arrays, and funcN aliases (see the header comment for what is
 * intentionally excluded). Used by the undo/redo history rings and by
 * import/export to preserve document state across save/load boundaries.
 *
 * `generation` is stamped at push time from the live counter.
 * Pop/redo refuse to restore a snapshot whose generation differs from
 * the current live generation - that means a wholesale document
 * replacement (scene switch, workspace load) intervened, and the
 * snapshot belongs to a different world. */
typedef struct {
    GLCmd cmds[MAX_EDITOR_COMMANDS];
    char  editor_lines[MAX_EDITOR_COMMANDS][MAX_LINE_LEN];
    int   num_cmds;
    int   edit_line;
    float predef_vals[MAX_PREDEF_VARS];
    float scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    char  predef_names[MAX_PREDEF_VARS][REPL_PREDEF_NAME_MAX];
    int   num_predef_vars;
    char  func_aliases[REPL_FUNC_SLOT_COUNT][REPL_FUNC_NAME_MAX];
    unsigned int generation;
} EditorUndoSnapshot;

/* Ring state descriptors: exposed for test access to undo/redo ring pointers
 * without revealing the full history buffers. undo_head is the next write
 * slot (one past the newest snapshot); a pop steps it back one and restores
 * from there. undo_count is the number of live snapshots. Similarly for
 * redo. Used by tests and by editor input's aborted-navigation rollback path.
 */
typedef struct {
    int undo_head;
    int undo_count;
    int redo_head;
    int redo_count;
    unsigned int generation;
} EditorUndoRingState;

/* Opaque, heap-backed snapshot of the LIVE undo/redo history - the actual
 * ring entries, not just the head/count pointers that EditorUndoRingState
 * carries. Used by the tour baseline (src/app/glr_tour_snapshot.c) so Back /
 * Done-restart can reinstate the exact undo history the user had when the
 * tour began.
 *
 * Capture copies only the live undo and redo entries (g_undo_count +
 * g_redo_count of the 64 fixed slots), in logical oldest-to-newest order, plus
 * the generation counter. Restore reinstates them into a canonical ring layout
 * (entries at slots [0..count), head == count) and restores the generation.
 * Returns NULL on allocation failure; destroy frees the snapshot. */
typedef struct EditorUndoHistorySnapshot EditorUndoHistorySnapshot;

EditorUndoHistorySnapshot *editor_undo_history_capture(void);
int  editor_undo_history_restore(const EditorUndoHistorySnapshot *snapshot);
void editor_undo_history_destroy(EditorUndoHistorySnapshot *snapshot);

/* Save/restore helpers for manual snapshot capture and restore. Used by
 * import/export code and by editor input's aborted-navigation rollback to
 * preserve full state without involving the history rings.
 * editor_undo_snapshot_save() captures current editor state;
 * editor_undo_snapshot_restore() reverts to a prior snapshot.
 * NOTE: editor_undo_snapshot_restore also resets transient editor state:
 * it exits insert mode, loads the restored edit line into the editor input
 * buffer, and marks the source document as dirty. */
void editor_undo_snapshot_save(EditorUndoSnapshot *snapshot);
void editor_undo_snapshot_restore(const EditorUndoSnapshot *snapshot);

/* Ring state inspection: capture and restore the internal undo/redo head/count
 * pointers. Used by tests to verify ring state without exposing the history
 * buffers themselves. */
void editor_undo_ring_state_capture(EditorUndoRingState *state);
void editor_undo_ring_state_restore(const EditorUndoRingState *state);

/* Undo/redo operations. editor_undo_push_snapshot() saves current editor state
 * to the undo ring and clears the redo ring; called before any mutation.
 * editor_undo_pop_snapshot() restores the most recent undo snapshot (Ctrl+Z),
 * moving current state to redo. editor_undo_do_redo() restores the most recent
 * redo snapshot (Ctrl+Y), moving current state back to undo. */
void editor_undo_push_snapshot(void);
void editor_undo_pop_snapshot(void);
void editor_undo_do_redo(void);
int  editor_undo_can_undo(void);
int  editor_undo_can_redo(void);

/* Semantic API for wholesale document replacement (scene switch,
 * example load, workspace load, full app reset).  Clears both undo
 * and redo rings AND bumps the generation counter so any snapshot
 * that somehow survived the clear (e.g. held in a CommitAttemptState)
 * cannot be restored into the new world. Callers that replace the
 * live REPL document must call this instead of editor_undo_clear(). */
void editor_undo_note_wholesale_replacement(void);

/* Current generation counter.  Exposed for tests that verify the
 * cross-generation safety net. */
unsigned int editor_undo_generation(void);

/* Raw ring clear - implementation detail.  Production code should
 * call editor_undo_note_wholesale_replacement() which also bumps the
 * generation counter.  Retained for test scaffolding that needs to
 * reset ring state without changing generations. */
void editor_undo_clear(void);

#endif /* EDITOR_UNDO_H */
