/*
 * src/repl/replace.c - Atomic whole-document rebuild from source text.
 *
 * See replace.h for why replace-all cannot go through the incremental
 * commit path. The shape of the transaction here mirrors
 * load_example_lines() in example_loader.c — reset the live document,
 * replay lines through repl_load_apply_line, reset the editor input sink
 * — with a SceneSnapshot standing in for the example's "reload the
 * example on failure" fallback, plus the two carry-overs (predef values,
 * is_auto) that a fresh example load has no reason to preserve.
 */
#include "repl/replace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "repl/command.h"
#include "repl/command_store.h"
#include "repl/eval.h"
#include "repl/host_effects.h"
#include "repl/load.h"
#include "repl/scene_snapshot.h"
#include "repl/state_notify.h"
#include "repl/state_owners.h"
#include "repl/state_views.h"
#include "source_document.h"

static void rebuild_result_init(ReplDocumentRebuildResult *out) {
    if (!out)
        return;
    out->failed_line = -1;
    out->err[0] = '\0';
}

/* Same live-state reset the example loader performs, minus the
 * presentation/cfg parts: a replace edits the current scene, so camera,
 * cfg, scene name, and workspace binding all stay put. */
static void rebuild_reset_live(void) {
    ReplCommandStore store = repl_command_store_live();

    repl_command_store_load(&store, NULL, 0);
    source_document_clear();
    repl_state_flat_program_set_count(0);
    repl_dispatch_input_reset();
    repl_eval_init_predef_vars();
    repl_func_alias_clear_all();
}

/* Carry live variable values across the rebuild. Values are matched by
 * name; the one name the substitution may have changed is carried by the
 * caller-supplied rename pair. Variables that are new (or whose old value
 * is gone) keep whatever the reloaded declaration produced. */
static void rebuild_restore_predef_values(const SceneSnapshot *before,
                                          const char *rename_from,
                                          const char *rename_to) {
    ExprVar *live;
    int live_count;

    repl_eval_restore_predef_values_by_name(before->predef_vals,
                                            before->predef_names,
                                            before->num_predef_vars);

    if (!rename_from || !rename_from[0] || !rename_to || !rename_to[0])
        return;
    if (strcmp(rename_from, rename_to) == 0)
        return;

    live = g_predef_vars_mut;
    live_count = g_num_predef_vars;
    for (int i = 0; i < live_count; i++) {
        if (strcmp(live[i].name, rename_to) != 0)
            continue;
        for (int j = 0; j < before->num_predef_vars; j++) {
            if (strcmp(before->predef_names[j], rename_from) == 0) {
                live[i].value = before->predef_vals[j];
                break;
            }
        }
        break;
    }
}

/* Re-stamp is_auto on auto-generated normals. A text substitution never
 * inserts or deletes rows, so when the rebuilt document has the same row
 * count the mapping is the identity; anything else means the loader
 * reshaped the document and the flags are dropped rather than guessed
 * (autonormal_refresh then adopts the rows as user normals, which is
 * inert — it never inserts a second normal in front of an existing one). */
static void rebuild_restore_auto_normals(const SceneSnapshot *before) {
    ReplCommandStore store = repl_command_store_live();
    int count = repl_state_document_count();

    if (count != before->num_cmds)
        return;

    for (int i = 0; i < count; i++) {
        GLCmd cmd;

        if (!before->cmds[i].is_auto)
            continue;
        if (before->cmds[i].type != CMD_NORMAL3F)
            continue;

        cmd = repl_state_document_cmds()[i];
        if (cmd.type != CMD_NORMAL3F || cmd.is_auto)
            continue;
        cmd.is_auto = 1;
        repl_command_store_replace_one(&store, i, &cmd);
    }
}

int repl_document_rebuild(const ReplDocumentRebuild *req,
                          ReplDocumentRebuildResult *out) {
    SceneSnapshot *before;
    int cursor = 0;
    int saved_edit_line;
    int ok = 1;

    rebuild_result_init(out);
    if (!req || !req->lines || req->count < 0)
        return 0;
    if (req->count > MAX_EDITOR_COMMANDS) {
        if (out)
            snprintf(out->err, sizeof(out->err),
                     "document too long (max %d lines)", MAX_EDITOR_COMMANDS);
        return 0;
    }

    /* SceneSnapshot is far too large for the stack (a full command array
     * plus a full text buffer). */
    before = (SceneSnapshot *)malloc(sizeof(*before));
    if (!before) {
        if (out)
            snprintf(out->err, sizeof(out->err), "out of memory");
        return 0;
    }

    scene_snapshot_capture_live(before);
    saved_edit_line = repl_dispatch_edit_line_get();

    rebuild_reset_live();

    for (int i = 0; i < req->count; i++) {
        char err[REPL_DIAG_TEXT_MAX] = "";
        const char *line = req->lines[i] ? req->lines[i] : "";

        if (repl_load_apply_line(line, err, sizeof(err), &cursor))
            continue;

        if (out) {
            out->failed_line = i;
            snprintf(out->err, sizeof(out->err), "%s",
                     err[0] ? err : "line rejected");
        }
        ok = 0;
        break;
    }

    if (!ok) {
        scene_snapshot_apply_live(before, SCENE_SNAPSHOT_CAMERA_SNAP);
        repl_dispatch_input_reset();
        repl_dispatch_edit_line_set(saved_edit_line);
        repl_state_mark_flat_dirty();
        repl_mark_source_dirty();
        free(before);
        return 0;
    }

    rebuild_restore_predef_values(before, req->rename_from, req->rename_to);
    rebuild_restore_auto_normals(before);

    /* The document is the same length, so the caller's cursor is still
     * meaningful; clamp it to the rebuilt document rather than dumping
     * the user at line 0. */
    if (saved_edit_line > repl_state_document_count())
        saved_edit_line = repl_state_document_count();
    if (saved_edit_line < 0)
        saved_edit_line = 0;
    repl_dispatch_edit_line_set(saved_edit_line);

    repl_state_mark_flat_dirty();
    repl_mark_source_dirty();
    free(before);
    return 1;
}
