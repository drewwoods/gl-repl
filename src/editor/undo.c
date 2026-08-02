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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "repl/host_effects.h"
#include "repl/state_notify.h"
#include "repl/command.h"
#include "repl/eval.h"
#include "repl/scenes.h"         /* repl_promote_transient_if_needed */
#include "repl/state_views.h"
#include "repl/util.h"           /* repl_copy_string_fits */
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
    static const char *lines[MAX_EDITOR_COMMANDS];

    if (!snapshot)
        return NULL;
    for (int i = 0; i < snapshot->num_cmds && i < MAX_EDITOR_COMMANDS; i++)
        lines[i] = snapshot->editor_lines[i];
    return lines;
}

void editor_undo_snapshot_save(EditorUndoSnapshot *snapshot) {
    EditorBufferView text = editor_buffer_view();
    memcpy(snapshot->cmds, repl_state_document_cmds(), (size_t)repl_state_document_count() * sizeof(GLCmd));
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
     * of docs/plans/done/edit-line-ownership.md). */
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

struct EditorUndoHistorySnapshot {
    int                 undo_count;
    int                 redo_count;
    unsigned int        generation;
    EditorUndoSnapshot *undo;   /* [undo_count], oldest-to-newest */
    EditorUndoSnapshot *redo;   /* [redo_count], oldest-to-newest */
};

/* Copy `count` live ring entries oldest-to-newest out of `ring`, whose next
 * write slot is `head`. Returns a heap array (caller frees), or NULL on OOM
 * or when count == 0. */
static EditorUndoSnapshot *undo_history_copy_ring(const EditorUndoSnapshot *ring,
                                                  int head, int count) {
    if (count <= 0)
        return NULL;
    EditorUndoSnapshot *out =
        (EditorUndoSnapshot *)malloc((size_t)count * sizeof(EditorUndoSnapshot));
    if (!out)
        return NULL;
    for (int i = 0; i < count; i++) {
        int idx = ((head - count + i) % REPL_UNDO_DEPTH + REPL_UNDO_DEPTH)
                  % REPL_UNDO_DEPTH;
        out[i] = ring[idx];
    }
    return out;
}

EditorUndoHistorySnapshot *editor_undo_history_capture(void) {
    EditorUndoHistorySnapshot *s =
        (EditorUndoHistorySnapshot *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->undo_count = g_undo_count;
    s->redo_count = g_redo_count;
    s->generation = g_undo_generation;

    if (s->undo_count > 0) {
        s->undo = undo_history_copy_ring(g_undo_buf, g_undo_head, s->undo_count);
        if (!s->undo) {
            editor_undo_history_destroy(s);
            return NULL;
        }
    }
    if (s->redo_count > 0) {
        s->redo = undo_history_copy_ring(g_redo_buf, g_redo_head, s->redo_count);
        if (!s->redo) {
            editor_undo_history_destroy(s);
            return NULL;
        }
    }
    return s;
}

int editor_undo_history_restore(const EditorUndoHistorySnapshot *snapshot) {
    if (!snapshot)
        return 0;

    editor_undo_clear();
    int un = snapshot->undo_count;
    if (un > REPL_UNDO_DEPTH) un = REPL_UNDO_DEPTH;
    for (int i = 0; i < un; i++)
        g_undo_buf[i] = snapshot->undo[i];
    g_undo_count = un;
    g_undo_head = un % REPL_UNDO_DEPTH;

    int rn = snapshot->redo_count;
    if (rn > REPL_UNDO_DEPTH) rn = REPL_UNDO_DEPTH;
    for (int i = 0; i < rn; i++)
        g_redo_buf[i] = snapshot->redo[i];
    g_redo_count = rn;
    g_redo_head = rn % REPL_UNDO_DEPTH;

    g_undo_generation = snapshot->generation;
    return 1;
}

void editor_undo_history_destroy(EditorUndoHistorySnapshot *snapshot) {
    if (!snapshot)
        return;
    free(snapshot->undo);
    free(snapshot->redo);
    free(snapshot);
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

int editor_undo_can_undo(void) {
    int peek;

    if (tutorial_active() || g_undo_count == 0)
        return 0;
    peek = (g_undo_head + REPL_UNDO_DEPTH - 1) % REPL_UNDO_DEPTH;
    return g_undo_buf[peek].generation == g_undo_generation;
}

int editor_undo_can_redo(void) {
    int peek;

    if (tutorial_active() || g_redo_count == 0)
        return 0;
    peek = (g_redo_head + REPL_UNDO_DEPTH - 1) % REPL_UNDO_DEPTH;
    return g_redo_buf[peek].generation == g_undo_generation;
}

void editor_undo_push_snapshot(void) {
    /* First mutation on a promotable TRANSIENT document auto-promotes it to a
     * user scene, inheriting the origin's name - a loaded example, or the
     * retained result of a completed/stopped tutorial. (An ACTIVE tutorial is
     * never promotable: its own step commits arrive here, and promoting would
     * tear the lesson down at step 0.) The snapshot captures the
     * post-promotion state so Undo rewinds to the unedited reference still
     * visible in the Scene menu. */
    repl_promote_transient_if_needed();

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
