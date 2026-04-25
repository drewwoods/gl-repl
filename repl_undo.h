#ifndef REPL_UNDO_H
#define REPL_UNDO_H

#include "sample.h"

typedef struct {
    GLCmd cmds[MAX_COMMANDS];
    int   num_cmds;
    int   edit_line;
    float predef_vals[MAX_PREDEF_VARS];
    char  predef_names[MAX_PREDEF_VARS][16];
    int   num_predef_vars;
} ReplUndoSnapshot;

typedef struct {
    int undo_head;
    int undo_count;
    int redo_head;
    int redo_count;
} ReplUndoRingState;

void repl_undo_snapshot_save(ReplUndoSnapshot *snapshot);
void repl_undo_snapshot_restore(const ReplUndoSnapshot *snapshot);
void repl_undo_ring_state_capture(ReplUndoRingState *state);
void repl_undo_ring_state_restore(const ReplUndoRingState *state);

/* Snapshot the current editor state onto the undo stack. Call before any
 * mutation; pushing clears the redo stack. */
void repl_undo_push_snapshot(void);
void repl_undo_pop_snapshot(void);
void repl_undo_do_redo(void);

#endif /* REPL_UNDO_H */
