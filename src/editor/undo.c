/*
 * editor_undo.c -- Editor command snapshots and undo/redo rings.
 *
 * Undo snapshots own the source command buffer, active editor line, and
 * predefined-variable table. Input routing decides when a mutation is about to
 * happen; this module records and restores the state affected by that mutation.
 */
#include "state.h"
#include "undo.h"

#include "input.h"
#include "repl/command_store.h"
#include "repl/core.h"
#include "repl/core_internal.h"
#include "repl/state_owners.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "subsystems/color_picker/color_picker_state.h"

#define REPL_UNDO_DEPTH 32

static EditorUndoSnapshot g_undo_buf[REPL_UNDO_DEPTH];
static int g_undo_head = 0;
static int g_undo_count = 0;
static EditorUndoSnapshot g_redo_buf[REPL_UNDO_DEPTH];
static int g_redo_head = 0;
static int g_redo_count = 0;
static unsigned int g_undo_generation = 0;

static const char *const *undo_snapshot_line_ptrs(const EditorUndoSnapshot *snapshot) {
    static const char *lines[MAX_COMMANDS];

    if (!snapshot)
        return NULL;
    for (int i = 0; i < snapshot->num_cmds && i < MAX_COMMANDS; i++)
        lines[i] = snapshot->editor_lines[i];
    return lines;
}

void editor_undo_snapshot_save(EditorUndoSnapshot *snapshot) {
    EditorBufferView text = editor_buffer_view();
    memcpy(snapshot->cmds, repl_state_document_cmds_mut(), (size_t)repl_state_document_count() * sizeof(GLCmd));
    for (int i = 0; i < repl_state_document_count(); i++)
        repl_copy_string_fits(snapshot->editor_lines[i],
                              MAX_LINE_LEN,
                              editor_buffer_view_line(text, i));
    snapshot->num_cmds = repl_state_document_count();
    snapshot->edit_line = editor_state_edit_line();
    repl_eval_copy_predef_vars(snapshot->predef_vals,
                               snapshot->predef_names,
                               &snapshot->num_predef_vars);
    repl_eval_copy_scratch_arrays(snapshot->scratch_arrays);
    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT; slot++) {
        const char *alias = repl_func_alias_get(slot);
        if (alias)
            snprintf(snapshot->func_aliases[slot], REPL_FUNC_NAME_MAX,
                     "%s", alias);
        else
            snapshot->func_aliases[slot][0] = '\0';
    }
    snapshot->generation = g_undo_generation;
}

void editor_undo_snapshot_restore(const EditorUndoSnapshot *snapshot) {
    if (snapshot->generation != g_undo_generation)
        return;
    ReplCommandStore store = repl_command_store_live();
    if (!repl_command_store_load(&store, snapshot->cmds,
                                 snapshot->num_cmds))
        return;
    /* The store no longer writes the cursor on load, so undo policy
     * is to restore the snapshotted edit-line (implemented in Phase 1
     * of plans/in-review/edit-line-ownership.md). */
    editor_state_edit_line_set(snapshot->edit_line);
    editor_buffer_load_lines(undo_snapshot_line_ptrs(snapshot),
                             snapshot->num_cmds);
    repl_eval_restore_predef_vars(snapshot->predef_vals,
                                  snapshot->predef_names,
                                  snapshot->num_predef_vars);
    repl_eval_restore_scratch_arrays(snapshot->scratch_arrays);
    repl_func_alias_clear_all();
    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT; slot++) {
        if (snapshot->func_aliases[slot][0])
            repl_func_alias_set(slot, snapshot->func_aliases[slot]);
    }
    editor_insert_mode_set(0);
    editor_load_line_to_input(editor_state_edit_line());
    repl_mark_source_dirty();
}

void editor_undo_ring_state_capture(EditorUndoRingState *state) {
    state->undo_head = g_undo_head;
    state->undo_count = g_undo_count;
    state->redo_head = g_redo_head;
    state->redo_count = g_redo_count;
    state->generation = g_undo_generation;
}

void editor_undo_ring_state_restore(const EditorUndoRingState *state) {
    if (state->generation != g_undo_generation) {
        editor_undo_clear();
        return;
    }
    g_undo_head = state->undo_head;
    g_undo_count = state->undo_count;
    g_redo_head = state->redo_head;
    g_redo_count = state->redo_count;
}

void editor_undo_clear(void) {
    g_undo_head = 0;
    g_undo_count = 0;
    g_redo_head = 0;
    g_redo_count = 0;
}

void editor_undo_note_wholesale_replacement(void) {
    editor_undo_clear();
    g_undo_generation++;
}

unsigned int editor_undo_generation(void) {
    return g_undo_generation;
}

void editor_undo_push_snapshot(void) {
    /* First mutation on a loaded example auto-promotes to a user scene,
     * inheriting the example's name. The snapshot captures the post-promotion
     * state so Undo rewinds to the unedited example reference still visible in
     * the Scene menu. */
    repl_promote_example_if_needed();

    editor_undo_snapshot_save(&g_undo_buf[g_undo_head]);
    g_undo_head = (g_undo_head + 1) % REPL_UNDO_DEPTH;
    if (g_undo_count < REPL_UNDO_DEPTH)
        g_undo_count++;
    g_redo_count = 0;
    g_redo_head = 0;
}

void editor_undo_pop_snapshot(void) {
    if (tutorial_active()) {
        repl_set_status_error("Undo disabled during tutorial");
        return;
    }

    if (g_undo_count == 0) {
        repl_set_status("Nothing to undo");
        return;
    }
    {
        int peek = (g_undo_head + REPL_UNDO_DEPTH - 1) % REPL_UNDO_DEPTH;
        if (g_undo_buf[peek].generation != g_undo_generation) {
            editor_undo_clear();
            repl_set_status("Nothing to undo");
            return;
        }
    }
    color_picker_stop();
    editor_undo_snapshot_save(&g_redo_buf[g_redo_head]);
    g_redo_head = (g_redo_head + 1) % REPL_UNDO_DEPTH;
    if (g_redo_count < REPL_UNDO_DEPTH)
        g_redo_count++;
    g_undo_head = (g_undo_head + REPL_UNDO_DEPTH - 1) % REPL_UNDO_DEPTH;
    g_undo_count--;
    editor_undo_snapshot_restore(&g_undo_buf[g_undo_head]);
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "Undo (%d more)", g_undo_count);
        repl_set_status(msg);
    }
}

void editor_undo_do_redo(void) {
    if (tutorial_active()) {
        repl_set_status_error("Undo disabled during tutorial");
        return;
    }

    if (g_redo_count == 0) {
        repl_set_status("Nothing to redo");
        return;
    }
    {
        int peek = (g_redo_head + REPL_UNDO_DEPTH - 1) % REPL_UNDO_DEPTH;
        if (g_redo_buf[peek].generation != g_undo_generation) {
            editor_undo_clear();
            repl_set_status("Nothing to redo");
            return;
        }
    }
    color_picker_stop();
    editor_undo_snapshot_save(&g_undo_buf[g_undo_head]);
    g_undo_head = (g_undo_head + 1) % REPL_UNDO_DEPTH;
    if (g_undo_count < REPL_UNDO_DEPTH)
        g_undo_count++;
    g_redo_head = (g_redo_head + REPL_UNDO_DEPTH - 1) % REPL_UNDO_DEPTH;
    g_redo_count--;
    editor_undo_snapshot_restore(&g_redo_buf[g_redo_head]);
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "Redo (%d more)", g_redo_count);
        repl_set_status(msg);
    }
}
