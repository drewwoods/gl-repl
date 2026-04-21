/*
 * repl_undo.c -- Editor command snapshots and undo/redo rings.
 *
 * Undo snapshots own the source command buffer, active editor line, and
 * predefined-variable table. Input routing decides when a mutation is about to
 * happen; this module records and restores the state affected by that mutation.
 */
#include "repl_undo.h"
#include "repl_core_internal.h"

#define REPL_UNDO_DEPTH 32

static ReplUndoSnapshot g_undo_buf[REPL_UNDO_DEPTH];
static int g_undo_head = 0;
static int g_undo_count = 0;
static ReplUndoSnapshot g_redo_buf[REPL_UNDO_DEPTH];
static int g_redo_head = 0;
static int g_redo_count = 0;

void repl_undo_snapshot_save(ReplUndoSnapshot *snapshot) {
    memcpy(snapshot->cmds, g_cmds, (size_t)g_num_cmds * sizeof(GLCmd));
    snapshot->num_cmds = g_num_cmds;
    snapshot->edit_line = g_edit_line;
    snapshot->num_predef_vars = g_num_predef_vars;
    for (int i = 0; i < g_num_predef_vars; i++) {
        snapshot->predef_vals[i] = g_predef_vars[i].value;
        memcpy(snapshot->predef_names[i], g_predef_vars[i].name, 16);
    }
}

void repl_undo_snapshot_restore(const ReplUndoSnapshot *snapshot) {
    memcpy(g_cmds, snapshot->cmds,
           (size_t)snapshot->num_cmds * sizeof(GLCmd));
    g_num_cmds = snapshot->num_cmds;
    g_edit_line = snapshot->edit_line;
    g_num_predef_vars = snapshot->num_predef_vars;
    for (int i = 0; i < snapshot->num_predef_vars; i++) {
        g_predef_vars[i].value = snapshot->predef_vals[i];
        memcpy(g_predef_vars[i].name, snapshot->predef_names[i], 16);
    }
    g_inserting = 0;
    load_line_to_input(g_edit_line);
    mark_normals_dirty();
}

void repl_undo_ring_state_capture(ReplUndoRingState *state) {
    state->undo_head = g_undo_head;
    state->undo_count = g_undo_count;
    state->redo_head = g_redo_head;
    state->redo_count = g_redo_count;
}

void repl_undo_ring_state_restore(const ReplUndoRingState *state) {
    g_undo_head = state->undo_head;
    g_undo_count = state->undo_count;
    g_redo_head = state->redo_head;
    g_redo_count = state->redo_count;
}

void push_undo_snapshot(void) {
    /* First mutation on a loaded example auto-promotes to a user scene,
     * inheriting the example's name. The snapshot captures the post-promotion
     * state so Undo rewinds to the unedited example reference still visible in
     * the Scene menu. */
    repl_promote_example_if_needed();

    repl_undo_snapshot_save(&g_undo_buf[g_undo_head]);
    g_undo_head = (g_undo_head + 1) % REPL_UNDO_DEPTH;
    if (g_undo_count < REPL_UNDO_DEPTH)
        g_undo_count++;
    g_redo_count = 0;
    g_redo_head = 0;
}

void pop_undo_snapshot(void) {
    if (g_undo_count == 0) {
        set_status("Nothing to undo");
        return;
    }
    repl_undo_snapshot_save(&g_redo_buf[g_redo_head]);
    g_redo_head = (g_redo_head + 1) % REPL_UNDO_DEPTH;
    if (g_redo_count < REPL_UNDO_DEPTH)
        g_redo_count++;
    g_undo_head = (g_undo_head + REPL_UNDO_DEPTH - 1) % REPL_UNDO_DEPTH;
    g_undo_count--;
    repl_undo_snapshot_restore(&g_undo_buf[g_undo_head]);
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "Undo (%d more)", g_undo_count);
        set_status(msg);
    }
}

void do_redo(void) {
    if (g_redo_count == 0) {
        set_status("Nothing to redo");
        return;
    }
    repl_undo_snapshot_save(&g_undo_buf[g_undo_head]);
    g_undo_head = (g_undo_head + 1) % REPL_UNDO_DEPTH;
    if (g_undo_count < REPL_UNDO_DEPTH)
        g_undo_count++;
    g_redo_head = (g_redo_head + REPL_UNDO_DEPTH - 1) % REPL_UNDO_DEPTH;
    g_redo_count--;
    repl_undo_snapshot_restore(&g_redo_buf[g_redo_head]);
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "Redo (%d more)", g_redo_count);
        set_status(msg);
    }
}
