/*
 * repl_undo.c -- Editor command snapshots and undo/redo rings.
 *
 * Undo snapshots own the source command buffer, active editor line, and
 * predefined-variable table. Input routing decides when a mutation is about to
 * happen; this module records and restores the state affected by that mutation.
 */
#include "repl_undo.h"
#include "repl_command_store.h"
#include "repl_core_internal.h"
#include "repl_state.h"

#define REPL_UNDO_DEPTH 32

static ReplUndoSnapshot g_undo_buf[REPL_UNDO_DEPTH];
static int g_undo_head = 0;
static int g_undo_count = 0;
static ReplUndoSnapshot g_redo_buf[REPL_UNDO_DEPTH];
static int g_redo_head = 0;
static int g_redo_count = 0;

static const char *const *undo_snapshot_line_ptrs(const ReplUndoSnapshot *snapshot) {
    static const char *lines[MAX_COMMANDS];

    if (!snapshot)
        return NULL;
    for (int i = 0; i < snapshot->num_cmds && i < MAX_COMMANDS; i++)
        lines[i] = snapshot->editor_lines[i];
    return lines;
}

void repl_undo_snapshot_save(ReplUndoSnapshot *snapshot) {
    memcpy(snapshot->cmds, repl_state_document_cmds_mut(), (size_t)repl_state_document_count() * sizeof(GLCmd));
    for (int i = 0; i < repl_state_document_count(); i++)
        repl_copy_string_fits(snapshot->editor_lines[i],
                              MAX_LINE_LEN,
                              repl_state_editor_buffer_line(i));
    snapshot->num_cmds = repl_state_document_count();
    snapshot->edit_line = repl_state_edit_line();
    snapshot->num_predef_vars = g_num_predef_vars;
    for (int i = 0; i < g_num_predef_vars; i++) {
        snapshot->predef_vals[i] = g_predef_vars[i].value;
        memcpy(snapshot->predef_names[i], g_predef_vars[i].name, 16);
    }
}

void repl_undo_snapshot_restore(const ReplUndoSnapshot *snapshot) {
    ReplCommandStore store = repl_command_store_live();
    if (!repl_command_store_load(&store, snapshot->cmds,
                                 snapshot->num_cmds,
                                 undo_snapshot_line_ptrs(snapshot),
                                 snapshot->edit_line))
        return;
    g_num_predef_vars = snapshot->num_predef_vars;
    for (int i = 0; i < snapshot->num_predef_vars; i++) {
        g_predef_vars[i].value = snapshot->predef_vals[i];
        memcpy(g_predef_vars[i].name, snapshot->predef_names[i], 16);
    }
    repl_state_insert_mode_set(0);
    load_line_to_input(repl_state_edit_line());
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

void repl_undo_push_snapshot(void) {
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

void repl_undo_pop_snapshot(void) {
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

void repl_undo_do_redo(void) {
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
