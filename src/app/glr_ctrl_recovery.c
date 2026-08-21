/*
 * glr_ctrl_recovery.c - quit-time recovery persistence.
 *
 * Carved out of glr_ctrl_router.c: the deferred-quit flag, what counts as
 * user work worth rescuing, and the two writers (single scene file, or a
 * whole recovery workspace) behind Ctrl+Q / File > Quit / SIGINT / window
 * close. File-writing lifecycle, not routing - the router keeps only the
 * Ctrl+Q key handler, and glr_ctrl_tick drives the deferred quit through
 * glr_ctrl_recovery_run_pending_quit (glr_ctrl_internal.h).
 */
#include "app/glr_ctrl.h"
#include "app/glr_ctrl_export.h"     /* glr_ctrl_fill_export_layout */
#include "app/glr_ctrl_internal.h"

#include "app/glr_paths.h"
#include "config.h"                  /* QUIT_RECOVERY_FILE */
#include "repl/export.h"
#include "repl/scenes.h"
#include "repl/state_views.h"
#include "source_document.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* Recovery safeguard: Ctrl+Q, File > Quit, and SIGINT (Ctrl+C), plus
 * Open Workspace (which replaces every in-memory slot), write a recovery
 * copy to a DISTINCT, findable file - never the active scene/workspace. The point
 * is to rescue an unintended exit / forgotten save / discarded scene
 * without silently clobbering the user's real scene; reload it with
 * `./gl-repl recovery.c`. (Not /tmp - the user would never find it; not
 * the scene file - that would defeat the safeguard.) The filename lives
 * in config.h as QUIT_RECOVERY_FILE. */

static volatile sig_atomic_t g_quit_requested = 0;

/* Async-signal-safe: only sets a sig_atomic_t flag. The actual save +
 * exit runs on the normal path in glr_ctrl_tick(). */
void glr_ctrl_request_quit(void) {
    g_quit_requested = 1;
}

/* An unpromoted built-in example is not user work: the live document is
 * byte-for-byte the shipped example, and writing it out would clobber a
 * real rescue copy (or a user scene the user happens to have named
 * recovery.c) with something they can reload from the Examples menu any
 * time. Any edit auto-promotes an example into a user-scene slot
 * (repl_promote_transient_if_needed), so "an example is active and no
 * slot is active" is exactly the untouched case. */
int glr_ctrl_recovery_has_user_work(void) {
    return !(repl_active_user_scene() < 0 &&
             repl_state_active_example_idx() >= 0);
}

/* Write the live scene to the recovery file. Shared by the quit
 * safeguard and Open Workspace. Returns 1 on success, 0 on failure.
 * Callers must check glr_ctrl_recovery_has_user_work() first - this
 * writes unconditionally. */
int glr_ctrl_save_recovery_file(void) {
    ReplExportLayout layout;
    char path[GLR_PATH_MAX];
    if (!glr_paths_app_state_path(QUIT_RECOVERY_FILE, path, sizeof(path)))
        return 0;
    if (!glr_paths_cwd_supports_relative_saves()) {
        char state_dir[GLR_PATH_MAX];
        snprintf(state_dir, sizeof(state_dir), "%s", path);
        char *slash = strrchr(state_dir, '/');
        if (!slash)
            return 0;
        *slash = '\0';
        if (!glr_paths_ensure_dir(state_dir, NULL))
            return 0;
    }
    glr_ctrl_fill_export_layout(&layout);
    return repl_export_save_output(path, source_document_view(),
                                   &layout) ? 1 : 0;
}

/* The live document is not worth rescuing, but the in-memory scene slots
 * die with the process just the same. Mirror what Open Workspace does
 * when it discards the collection (glr_action_open_workspace_path): dump
 * every occupied slot into a findable recovery workspace. Deliberately
 * NOT the user's bound workspace directory even when one is bound -
 * quitting must not write over files they never asked to save - and no
 * promotion, so the example being *looked at* stays out of it. The
 * workspace binding is restored because repl_save_workspace rebinds on
 * success and callers keep running until exit(). Returns 1 if a
 * workspace was written. */
static int glr_ctrl_save_recovery_workspace(char *out_dir, size_t out_sz) {
    ReplExportLayout layout;
    char bound[REPL_WORKSPACE_DIR_MAX];
    const char *current;
    int written;

    if (repl_user_scene_count() <= 0)
        return 0;
    if (!glr_paths_app_state_path("recovery-workspace", out_dir, out_sz))
        return 0;

    current = repl_workspace_dir();
    snprintf(bound, sizeof(bound), "%s", current ? current : "");
    glr_ctrl_fill_export_layout(&layout);
    written = repl_save_workspace(out_dir, &layout);
    repl_set_workspace_dir(bound);
    return written > 0;
}

int glr_ctrl_save_quit_recovery(void) {
    char path[GLR_PATH_MAX];
    if (!glr_ctrl_recovery_has_user_work()) {
        if (!glr_ctrl_save_recovery_workspace(path, sizeof(path)))
            return 0;
        printf("Saved %d unsaved scene(s) to %s (reload: %s %s)\n",
               repl_user_scene_count(), path, glr_ctrl_program_name(), path);
        return 1;
    }
    if (!glr_ctrl_save_recovery_file())
        return 0;
    glr_paths_app_state_path(QUIT_RECOVERY_FILE, path, sizeof(path));
    printf("Saved recovery copy to %s (reload: %s %s)\n",
           path, glr_ctrl_program_name(), path);
    return 1;
}

/* Deferred quit, consumed on the normal frame-tick path. SIGINT only sets
 * the flag, so no stdio/file I/O runs in handler context. Called from
 * glr_ctrl_tick in glr_ctrl.c via glr_ctrl_internal.h. */
void glr_ctrl_recovery_run_pending_quit(void) {
    if (g_quit_requested) {
        glr_ctrl_save_quit_recovery();
        exit(0);
    }
}
