/*
 * src/app/glr_extedit.c - External-editor sync (`--watch`).
 *
 * The contract, the state machine and every decision letter cited below are
 * documented in glr_extedit.h; this file is the mechanism. Frame-time
 * controller band, not boot band (`check-app-boot-band`): the poll runs every
 * frame, so the host display callback has to be able to reach it.
 */
#include "app/glr_extedit.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "app/glr_assign_plot_bridge.h"
#include "app/glr_camera.h"
#include "app/glr_ctrl.h"
#include "app/glr_pointer_script.h"
#include "editor/input.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "repl/scene_load.h"
#include "repl/scenes.h"
#include "repl/host_effects.h"   /* repl_set_status / _error */
#include "repl/state_owners.h"
#include "subsystems/color_picker/color_picker_state.h"
#include "subsystems/replay/replay.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "ui/app/menu_bar.h"
#include "ui/app/state.h"

/* Room for the path this follows. Sized to match the scene slot's own
 * source_path (SCENE_GLR_ORIGIN_PATH_MAX) - a path that does not fit there
 * never becomes a binding, so a longer buffer here could not be reached. */
#define GLR_EXTEDIT_PATH_MAX SCENE_GLR_ORIGIN_PATH_MAX

/* A file identity that survives an editor's safe write. `time_t` seconds are
 * not enough: a same-second final save would be missed *indefinitely* (D8). */
typedef struct {
    int                valid;
    unsigned long long mtime_ns;
    unsigned long long ino;
    unsigned long long size;
} GlrExtEditChangeToken;

static int                   g_enabled;
static char                  g_bound[GLR_EXTEDIT_PATH_MAX];
static GlrExtEditChangeToken g_observed;
static unsigned long long    g_applied_content;
static int                   g_applied_valid;
static unsigned long long    g_suppressed_content;
static int                   g_suppressed_valid;
static int                   g_pending;
static unsigned long long    g_pending_content;
/* The live document as it stood when the gate shut. Comparing it again when
 * the gate opens is what separates "the row was abandoned" from "the user
 * committed something" without a single hook in the input router. */
static unsigned long long    g_doc_fp_at_defer;
static int                   g_have_defer_fp;
static int                   g_reported_missing;
static int                   g_was_in_lesson;
static GlrExtEditStats       g_stats;

/* ----- tokens ------------------------------------------------------------ */

static unsigned long long hash_init(void) {
    return 1469598103934665603ULL;  /* FNV-1a 64 offset basis */
}

static unsigned long long hash_bytes(unsigned long long h,
                                     const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned long long)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static GlrExtEditChangeToken change_token_from_stat(const struct stat *st) {
    GlrExtEditChangeToken t;
    t.valid = 1;
#if defined(__APPLE__)
    t.mtime_ns = (unsigned long long)st->st_mtimespec.tv_sec * 1000000000ULL +
                 (unsigned long long)st->st_mtimespec.tv_nsec;
#elif defined(st_mtime) || defined(__linux__) || defined(_POSIX_C_SOURCE)
    t.mtime_ns = (unsigned long long)st->st_mtim.tv_sec * 1000000000ULL +
                 (unsigned long long)st->st_mtim.tv_nsec;
#else
    t.mtime_ns = (unsigned long long)st->st_mtime * 1000000000ULL;
#endif
    t.ino  = (unsigned long long)st->st_ino;
    t.size = (unsigned long long)st->st_size;
    return t;
}

static int change_token_equal(const GlrExtEditChangeToken *a,
                              const GlrExtEditChangeToken *b) {
    return a->valid && b->valid &&
           a->mtime_ns == b->mtime_ns &&
           a->ino == b->ino &&
           a->size == b->size;
}

/* Hash the scene payload. Today that is the whole file: there is no transport
 * metadata in a `.glr` or an exported `.c`. Stage 2.5's `.wip` sidecar rewrites
 * a trailing `// @cursor <line> <col>` on every cursor motion, and hashing THAT
 * would defeat its own purpose - the cursor-only fast path would never fire -
 * so the sidecar reader must strip it before reaching here. */
static int hash_file_content(const char *path, unsigned long long *out) {
    unsigned char buf[4096];
    unsigned long long h = hash_init();
    size_t n;
    FILE *f = fopen(path, "rb");

    if (!f)
        return 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        h = hash_bytes(h, buf, n);
    if (ferror(f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    *out = h;
    return 1;
}

static unsigned long long document_fingerprint(void) {
    unsigned long long h = hash_init();
    int count = repl_state_document_count();

    h = hash_bytes(h, &count, sizeof(count));
    for (int i = 0; i < count; i++) {
        const char *line = editor_buffer_line(i);
        if (line)
            h = hash_bytes(h, line, strlen(line));
        h = hash_bytes(h, "\n", 1);
    }
    return h;
}

/* ----- state ------------------------------------------------------------- */

static void forget_binding_state(void) {
    g_bound[0]           = '\0';
    g_observed.valid     = 0;
    g_applied_valid      = 0;
    g_suppressed_valid   = 0;
    g_pending            = 0;
    g_have_defer_fp      = 0;
    g_reported_missing   = 0;
}

void glr_extedit_set_enabled(int enabled) {
    g_enabled = enabled ? 1 : 0;
    forget_binding_state();
    g_was_in_lesson = 0;
    memset(&g_stats, 0, sizeof(g_stats));
}

int glr_extedit_enabled(void) {
    return g_enabled;
}

const char *glr_extedit_bound_path(void) {
    return g_bound[0] ? g_bound : NULL;
}

GlrExtEditStats glr_extedit_stats(void) {
    return g_stats;
}

/* Adopt `path` as the binding and stamp what is on disk *now* as both observed
 * and applied - without reloading. The live document already is that scene
 * (this is reached at startup and on every scene switch), so a reload would be
 * a pointless wholesale replacement that clears the undo ring and the input
 * row for nothing. */
static void rebind(const char *path) {
    struct stat st;
    unsigned long long content;

    forget_binding_state();
    snprintf(g_bound, sizeof(g_bound), "%s", path);
    if (stat(g_bound, &st) != 0)
        return;
    g_observed = change_token_from_stat(&st);
    g_stats.reads++;
    if (!hash_file_content(g_bound, &content)) {
        g_observed.valid = 0;
        return;
    }
    g_applied_content = content;
    g_applied_valid   = 1;
}

static void report_missing_once(const char *path) {
    char msg[GLR_EXTEDIT_PATH_MAX + 64];

    if (g_reported_missing)
        return;   /* once, not once per frame */
    g_reported_missing = 1;
    g_pending = 0;
    g_have_defer_fp = 0;
    /* The last good scene stays live and the watcher keeps polling, so the
     * binding resumes by itself when the path reappears - which is exactly
     * what an editor's temp-file-and-rename save looks like if the stat lands
     * in the middle of it. */
    snprintf(msg, sizeof(msg), "Watch: cannot read %s (keeping current scene)",
             path);
    repl_set_status_error(msg);
}

/* ----- D7: what a successful reload invalidates --------------------------- */

/* Deliberately NOT glr_ctrl_reset_transients(): that calls
 * glr_camera_controls_reset() and glr_camera_clear_scene_default(), which
 * clobbers exactly the camera state D3 preserves. This is the narrower cousin
 * the plan calls for - settle the ease so the pose is the outgoing scene's
 * intended one, then leave it alone.
 *
 * Not on this list, and each for a reason:
 *   search        the query is preserved on purpose; it re-scans against the
 *                 new document on the next frame like any other edit.
 *   variable panel it is a per-frame view over the live predef table, so it
 *                 rebuilds itself; there is nothing cached to invalidate.
 *   undo          owned by the caller, because the policy depends on scene
 *                 identity (D4) and has to bracket the reload, not follow it.
 */
static void glr_extedit_notify_reloaded(void) {
    /* Flat identity and replay_exec_limit do not survive a reflatten: the
     * limit indexes a flat program that no longer exists. */
    replay_stop();
    glr_ctrl_invalidate_depth_snapshot();
    color_picker_stop();
    ui_menu_bar_close();
    ui_state_command_description_close();
    glr_ctrl_router_reset_code_panel_drag();
    editor_state_selection_clear();
    editor_input_anchor_clear();
    editor_state_autocomplete_clear();
    glr_camera_settle_target();
    /* Force the `// @plot` re-resolve. Keyed through this notification rather
     * than editor_undo_generation(), which a watched reload does not bump -
     * and the actual rescan runs on the frame path, after the flat refresh,
     * because the series-compatibility check reads the flat program. */
    glr_assign_plot_invalidate_tag_sync();
}

/* ----- reload ------------------------------------------------------------- */

static void apply_reload(const char *path) {
    ReplSceneLoadOpts opts;
    EditorUndoHistorySnapshot *history = NULL;
    int is_user_scene = repl_active_user_scene() >= 0;
    int ok;

    repl_scene_load_opts_init(&opts, repl_scene_format_from_path(path));
    /* The loader stays strictly atomic: any rejected line fails the whole
     * import and the live document is untouched. */
    opts.policy = REPL_SCENE_LOAD_POLICY_ATOMIC;
    /* D3: an external text edit is geometry, not a presentation reset. */
    opts.apply_cfg    = 0;
    opts.camera_apply = REPL_CAMERA_APPLY_NONE;

    /* D4. The history capture is heap-backed on purpose: with a full ring the
     * push below has already overwritten the oldest entry, and restoring
     * head/count indices cannot bring it back. */
    if (is_user_scene) {
        history = editor_undo_history_capture();
        editor_undo_push_snapshot();
    }

    ok = repl_reload_active_scene_from_path(path, &opts);

    if (!ok) {
        if (history)
            (void)editor_undo_history_restore(history);
        g_stats.failures++;
    } else {
        g_applied_content = g_pending_content;
        g_applied_valid   = 1;
        g_suppressed_valid = 0;
        g_stats.reloads++;
        /* The input row was clean - deferral guarantees it - but it still
         * holds the OLD document's row text. */
        editor_insert_mode_set(0);
        editor_input_clear();
        editor_state_edit_line_clamp();
        glr_extedit_notify_reloaded();
    }
    editor_undo_history_destroy(history);

    g_pending       = 0;
    g_have_defer_fp = 0;
}

/* ----- poll --------------------------------------------------------------- */

/* A tutorial or a guided tour is driving the document itself; two writers on
 * one document is not a thing this can arbitrate. */
static int lesson_running(void) {
    return tutorial_active() || glr_pointer_script_tour_active();
}

static int gate_is_shut(void) {
    return lesson_running() || editor_input_has_uncommitted_change();
}

/* Should the binding be left exactly as it is, whatever the active scene now
 * resolves to?
 *
 * `tutorial_start` parks the document in a slot-less transient scene, so the
 * bound path momentarily resolves to nothing - and re-resolving there would
 * drop the binding at precisely the moment D7 needs it, leaving the "external
 * change waiting" status with no file to name. The pin outlasts the lesson
 * itself, because the document a completed or stopped tutorial leaves behind
 * is the same slot-less transient. It lifts when that document is promoted
 * into a real slot, and unbinding is then the honest answer: the promoted
 * scene is no longer the file on disk. */
static int binding_is_pinned(void) {
    return lesson_running() || repl_state_tutorial_origin_idx() >= 0;
}

static void dismiss_pending(const char *why) {
    char msg[128];

    if (!g_pending)
        return;
    /* Into `suppressed`, NOT `applied`: the live document is not this payload,
     * and calling it applied would make a later update reason about a document
     * that was never loaded. */
    g_suppressed_content = g_pending_content;
    g_suppressed_valid   = 1;
    g_pending            = 0;
    g_have_defer_fp      = 0;
    g_stats.dismissals++;
    snprintf(msg, sizeof(msg), "Watch: %s (local document kept)", why);
    repl_set_status(msg);
}

void glr_extedit_poll(void) {
#if defined(__EMSCRIPTEN__)
    /* No external editor in a browser tab, and no filesystem worth watching.
     * The TU stays non-empty for the C99 rule and the flag is ignored. */
    return;
#else
    const char *path;
    struct stat st;
    GlrExtEditChangeToken token;
    int in_lesson;

    if (!g_enabled)
        return;

    if (!binding_is_pinned()) {
        path = repl_active_scene_bound_path();
        if (!path || !path[0]) {
            if (g_bound[0])
                forget_binding_state();   /* a built-in example has no file */
            return;
        }
        if (strcmp(path, g_bound) != 0) {
            rebind(path);
            return;
        }
    }
    if (!g_bound[0])
        return;

    /* D7: at the end of a lesson, clear the pending version AND stamp what is
     * on disk as observed. Ignoring activity without stamping it would make
     * the first poll afterwards see that old movement as new and apply it -
     * defeating the dismiss-on-end rule the ignoring exists to serve. */
    in_lesson = lesson_running();
    if (g_was_in_lesson && !in_lesson) {
        dismiss_pending("external change discarded at lesson end");
        if (stat(g_bound, &st) == 0)
            g_observed = change_token_from_stat(&st);
    }
    g_was_in_lesson = in_lesson;

    if (stat(g_bound, &st) != 0) {
        report_missing_once(g_bound);
        return;
    }
    g_reported_missing = 0;

    token = change_token_from_stat(&st);
    if (!change_token_equal(&token, &g_observed)) {
        unsigned long long content;

        /* Stamped before the read can fail, so a file that cannot be parsed is
         * not re-read every frame until the user fixes it. */
        g_observed = token;
        g_stats.reads++;
        if (!hash_file_content(g_bound, &content)) {
            report_missing_once(g_bound);
            return;
        }
        if (g_applied_valid && content == g_applied_content)
            return;   /* our own write, or a touch that changed no bytes */
        if (g_suppressed_valid && content == g_suppressed_content)
            return;   /* the payload a local commit already outvoted */
        g_pending         = 1;
        g_pending_content = content;
        g_have_defer_fp   = 0;
    }

    if (!g_pending)
        return;

    if (gate_is_shut()) {
        if (!g_have_defer_fp) {
            g_doc_fp_at_defer = document_fingerprint();
            g_have_defer_fp   = 1;
            g_stats.deferrals++;
            repl_set_status("Watch: newer version on disk, waiting for this line");
        }
        return;
    }

    /* The gate opened. Which way it opened is decided by what the document
     * did, not by which key was pressed (see glr_extedit.h): an unchanged
     * document means the row was abandoned, so the pending version applies; a
     * changed one means the user committed, and applying would destroy exactly
     * what they just typed. */
    if (g_have_defer_fp && document_fingerprint() != g_doc_fp_at_defer) {
        dismiss_pending("kept your edit over the external one");
        return;
    }

    apply_reload(g_bound);
#endif
}

void glr_extedit_note_saved(void) {
    glr_extedit_note_wrote(repl_active_scene_bound_path());
}

void glr_extedit_note_wrote(const char *path) {
#if defined(__EMSCRIPTEN__)
    (void)path;
#else
    struct stat st;
    unsigned long long content;

    if (!g_enabled || !path || !path[0] || !g_bound[0])
        return;
    if (strcmp(path, g_bound) != 0)
        return;
    /* Stamped from the bytes on disk *now*, at the instant after our own
     * write, rather than by setting a "expect one change" flag for the next
     * poll - a flag would swallow an external save that landed in the same
     * frame. */
    if (stat(g_bound, &st) != 0)
        return;
    if (!hash_file_content(g_bound, &content))
        return;
    g_observed        = change_token_from_stat(&st);
    g_applied_content = content;
    g_applied_valid   = 1;
    /* A save resolves any argument about who wins: the file and the document
     * now agree, so nothing is pending and nothing needs suppressing. */
    g_pending          = 0;
    g_have_defer_fp    = 0;
    g_suppressed_valid = 0;
#endif
}
