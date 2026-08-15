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
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "app/glr_assign_plot_bridge.h"
#include "app/glr_camera.h"
#include "app/glr_ctrl.h"
#include "app/glr_modal.h"
#include "app/glr_pointer_script.h"
#include "config.h"          /* MAX_LINE_LEN / MAX_INPUT_LEN */
#include "editor/input.h"
#include "editor/search.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "repl/line_scan.h"   /* the importer's own statement-boundary scan */
#include "repl/scene_load.h"
#include "repl/scenes.h"
#include "repl/host_effects.h"   /* repl_set_status / _error */
#include "repl/state_owners.h"
#include "repl/text_helpers.h"   /* parked input is canonical (no leading ws) */
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
/* Text of the row the watcher parked in the input buffer, and whether it is
 * still exactly that. The parked row is excluded from "the input row is dirty"
 * only while the user has not touched it (D5): the first local keystroke there
 * transfers ownership back to them and re-arms normal deferral, or the next
 * save would overwrite typing undo cannot restore. */
static char                  g_parked_row[MAX_INPUT_LEN];
static int                   g_parked_valid;
/* Leading spaces/tabs stripped when the row was parked from sidecar text.
 * vim's col('.') counts those bytes; the live input does not. Kept across a
 * re-park of the already-canonical g_parked_row so a cursor-only return to
 * the hole still maps the published column. */
static int                   g_parked_lead;
static GlrExtEditStats       g_stats;

/* What the live document last came from, remembered *per path* rather than
 * only for the current binding.
 *
 * Binding to a file does not reload it - the document usually already is that
 * file, and reloading would clobber unsaved slot edits for nothing. But that
 * reasoning fails on the way back: F12 away, let vim save the file, F12 back,
 * and stamping the new bytes as "applied" buries the external edit forever.
 * With one entry per bound path, rebind can tell the two apart - unchanged
 * since we last applied it, or moved while we were elsewhere. The set is not
 * bounded by the eight user-scene slots: a runtime `--examples-dir` catalog
 * can expose dozens of file-backed examples, and an editor can visit any
 * number of additional paths in one session. Evicting an entry reads as
 * "never seen", which stamps and does not reload; that is the right answer
 * for a genuinely new file and the wrong one for a history entry forgotten by
 * an arbitrary cap. Keep the session's complete path history instead. */

typedef struct {
    char               path[GLR_EXTEDIT_PATH_MAX];
    unsigned long long content;
    /* A payload the user's own commit beat, remembered per path for the same
     * reason `content` is: switching away and back must not resurrect a
     * version they already rejected. Kept out of the binding state, which
     * rebind() deliberately wipes. */
    unsigned long long suppressed;
    int                suppressed_valid;
    /* This session has followed this path's sidecar at least once. The
     * recovery offer exists for a `.wip` left behind by an editor that died
     * *before* gl-repl started; one we were following ten seconds ago is
     * self-evidently not that, and re-offering it on every scene switch would
     * put a modal in front of an ordinary F12 round trip. Same shape as
     * `content` above, and for the same reason: binding state is wiped on
     * rebind, so the memory has to live per path. */
    int                wip_followed;
    /* A sidecar the user turned down at the recovery prompt, keyed by its
     * **payload** hash - the buffer without the trailing `@cursor` line, the
     * same key `g_wip_payload` and `g_wip_suppressed` use.
     *
     * Same reason `suppressed` exists for the scene file: a decision the user
     * already made must survive a scene switch, or coming back re-asks it.
     * Keying on content rather than on the path alone is what lets a genuinely
     * new payload re-ask; keying on the *payload* rather than the raw bytes is
     * what stops the plugin rewriting `// @cursor` on every cursor motion from
     * counting as one. A leftover from a still-open editor gets its cursor
     * line rewritten constantly, and that is exactly the case this memory
     * exists for. */
    unsigned long long wip_declined;
    int                wip_declined_valid;
    int                valid;
} GlrExtEditSeen;

static GlrExtEditSeen *g_seen;
static size_t          g_seen_count;
static size_t          g_seen_capacity;

/* ----- stage 2.5: the live WIP sidecar ------------------------------------ */

/* `<bound>.wip`, the editor's unsaved buffer. Derived from the binding, never
 * named separately: a sidecar the watcher followed but Ctrl+S would not write
 * over is a second source of truth. */
static char                  g_wip_path[GLR_EXTEDIT_PATH_MAX];
static GlrExtEditChangeToken g_wip_observed;
/* The buffer WITHOUT its trailing `@cursor` line. Splitting the hash this way
 * is what makes a cursor move cost a stat and a hash instead of a reimport. */
static unsigned long long    g_wip_payload;
static int                   g_wip_payload_valid;
/* A payload the user's own commit or undo beat. Distinct from
 * g_wip_payload_valid: clearing the latter is what used to let a later
 * CursorMoved of the same bytes look like new content and reimport. While
 * this matches, both content and cursor updates are ignored (D5). */
static unsigned long long    g_wip_suppressed;
static int                   g_wip_suppressed_valid;
/* The live document as the session's one undo snapshot captured it. Equal
 * after Ctrl+Z; a local commit produces something else. That is how the two
 * exits are told apart without a hook on either key. */
static unsigned long long    g_wip_entry_fp;
static int                   g_wip_have_entry_fp;
/* A session runs from the first content update to the sidecar's disappearance
 * or the user's own edit. It exists to make "push one undo snapshot" and "who
 * owns the document" answerable. */
static int                   g_wip_active;
/* The user declined the recovery offer. Cleared when the editor next touches
 * the file, which is the plan's "ignore it until its change token moves again"
 * - a decline is about this file as it stands, not about following at all. */
static int                   g_wip_hold;
/* A sidecar that was *already there* when the binding formed. The distinction
 * matters and is not observable later: the same file appearing five seconds
 * after gl-repl started is the editor opening, and appearing before it started
 * is the editor having died. Only the second is a recovery. */
static int                   g_wip_recover_offer;
static int                   g_wip_offering;      /* the recovery modal is up */
/* This binding's sidecar has produced a parseable `// @cursor` trailer at
 * least once. The plugin always writes one last, so its sudden absence is the
 * one cheap signal that a publication was read half-written - see
 * wip_poll(). Per binding, not per path: it is a fact about the publisher
 * currently writing the file. */
static int                   g_wip_saw_cursor;
static int                   g_wip_entry_pushed;  /* the one per-session snapshot */
/* The live document as this module last left it. A mismatch means somebody
 * else moved it - Ctrl+Z, or a commit - and the session is over. Same
 * mechanism as the deferral gate, for the same reason: reading the document
 * beats hooking every key that could have changed it. */
static unsigned long long    g_wip_doc_fp;
static int                   g_wip_have_doc_fp;
/* The row map the last content import produced, and the cursor row it was
 * computed against. Grown to the file's physical row count; a cursor-only
 * update indexes it instead of importing. */
static int                  *g_wip_rows;
static int                   g_wip_rows_cap;
static ReplSceneRowMap       g_wip_map;
static int                   g_wip_map_valid;
static unsigned long         g_wip_nonce;

static void forget_binding_state(void);

static void seen_clear(void) {
    free(g_seen);
    g_seen          = NULL;
    g_seen_count    = 0;
    g_seen_capacity = 0;
}

static int seen_reserve(size_t wanted) {
    size_t capacity = g_seen_capacity ? g_seen_capacity : 8;

    if (wanted <= g_seen_capacity)
        return 1;
    while (capacity < wanted) {
        if (capacity > (size_t)-1 / 2) {
            capacity = wanted;
            break;
        }
        capacity *= 2;
    }
    if (capacity > (size_t)-1 / sizeof(*g_seen))
        return 0;

    GlrExtEditSeen *grown = (GlrExtEditSeen *)realloc(
        g_seen, capacity * sizeof(*grown));
    if (!grown)
        return 0;
    g_seen          = grown;
    g_seen_capacity = capacity;
    return 1;
}

static GlrExtEditSeen *seen_find(const char *path) {
    for (size_t i = 0; i < g_seen_count; i++) {
        if (g_seen[i].valid && strcmp(g_seen[i].path, path) == 0)
            return &g_seen[i];
    }
    return NULL;
}

/* Append an entry for `path`, or NULL if the history cannot grow. */
static GlrExtEditSeen *seen_add(const char *path) {
    GlrExtEditSeen *e;

    if (g_seen_count == (size_t)-1 || !seen_reserve(g_seen_count + 1))
        return NULL;
    e = &g_seen[g_seen_count];
    memset(e, 0, sizeof(*e));
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->valid = 1;
    g_seen_count++;
    return e;
}

static int seen_remember(const char *path, unsigned long long content) {
    GlrExtEditSeen *e = seen_find(path);

    if (!e)
        e = seen_add(path);
    if (!e)
        return 0;
    e->content = content;
    /* Applying content settles any argument about this file: a version the
     * user dismissed earlier is no longer the thing being argued over. */
    e->suppressed_valid = 0;
    return 1;
}

/* Remember that the user's own commit beat `content` for this path. Best
 * effort: failing to record it only costs one redundant re-offer later, so it
 * does not stop the watcher the way losing `content` history does. */
static void seen_remember_suppressed(const char *path,
                                     unsigned long long content) {
    GlrExtEditSeen *e = seen_find(path);

    if (!e)
        e = seen_add(path);
    if (!e)
        return;
    e->suppressed       = content;
    e->suppressed_valid = 1;
}

/* Best effort, like seen_remember_suppressed: failing to record it costs one
 * redundant recovery prompt, not correctness. */
static void seen_note_wip_followed(const char *path) {
    GlrExtEditSeen *e = seen_find(path);

    if (!e)
        e = seen_add(path);
    if (e)
        e->wip_followed = 1;
}

static int seen_wip_followed(const char *path) {
    const GlrExtEditSeen *e = seen_find(path);

    return e && e->wip_followed;
}

/* Recorded when the prompt *opens*, not when it is answered: Esc has no
 * callback, so declining is the default and accepting is what clears it. */
static void seen_note_wip_declined(const char *path, unsigned long long content) {
    GlrExtEditSeen *e = seen_find(path);

    if (!e)
        e = seen_add(path);
    if (!e)
        return;
    e->wip_declined       = content;
    e->wip_declined_valid = 1;
}

static void seen_clear_wip_followed(const char *path) {
    GlrExtEditSeen *e = seen_find(path);

    if (e)
        e->wip_followed = 0;
}

static void seen_clear_wip_declined(const char *path) {
    GlrExtEditSeen *e = seen_find(path);

    if (e)
        e->wip_declined_valid = 0;
}

static int seen_wip_declined(const char *path, unsigned long long content) {
    const GlrExtEditSeen *e = seen_find(path);

    return e && e->wip_declined_valid && e->wip_declined == content;
}

static int seen_lookup_suppressed(const char *path, unsigned long long *out) {
    const GlrExtEditSeen *e = seen_find(path);

    if (!e || !e->suppressed_valid)
        return 0;
    *out = e->suppressed;
    return 1;
}

static int seen_lookup(const char *path, unsigned long long *out) {
    const GlrExtEditSeen *e = seen_find(path);

    if (!e)
        return 0;
    *out = e->content;
    return 1;
}

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

/* Hash a line array as if it were the file those lines came from - one
 * trailing newline each. Used for the sidecar payload, where the bytes on disk
 * and the bytes that matter differ by exactly the `@cursor` row. */
static unsigned long long hash_lines(const char *const *lines, int count) {
    unsigned long long h = hash_init();
    int i;

    for (i = 0; i < count; i++) {
        if (lines[i])
            h = hash_bytes(h, lines[i], strlen(lines[i]));
        h = hash_bytes(h, "\n", 1);
    }
    return h;
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

static void rebind(const char *path);
static void wip_forget(void);

static void forget_binding_state(void) {
    g_bound[0]           = '\0';
    g_observed.valid     = 0;
    g_applied_valid      = 0;
    g_suppressed_valid   = 0;
    g_pending            = 0;
    g_have_defer_fp      = 0;
    g_reported_missing   = 0;
    g_parked_valid       = 0;
    g_parked_lead        = 0;
    wip_forget();
}

static void stop_after_seen_history_oom(void) {
    repl_set_status_error("Watch: out of memory remembering file history; "
                          "watching stopped");
    g_enabled = 0;
    forget_binding_state();
    seen_clear();
}

void glr_extedit_set_enabled(int enabled) {
    g_enabled = enabled ? 1 : 0;
    forget_binding_state();
    g_was_in_lesson = 0;
    seen_clear();
    memset(&g_stats, 0, sizeof(g_stats));
#if !defined(__EMSCRIPTEN__)
    /* Bind now rather than on the first poll. Arming is a deliberate act with
     * a document already loaded, and binding eagerly means the answer does not
     * depend on what happens between here and the next frame. */
    if (g_enabled) {
        const char *path = repl_active_scene_bound_path();
        if (path && path[0])
            rebind(path);
    }
#endif
}

void glr_extedit_bind_path(const char *path) {
#if defined(__EMSCRIPTEN__)
    (void)path;
#else
    struct stat st;

    if (!g_enabled || !path || !path[0])
        return;
    /* A positional argument can also be a managed-workspace directory, and a
     * directory is not a scene file: it has no content to hash and binding it
     * would only delay the poll's own resolution to the scene inside. Leave it
     * unbound and let the first poll resolve it properly.
     *
     * A path that does not exist yet is NOT refused. `--watch new.glr` before
     * the file has been created is a reasonable thing to ask for - the editor
     * is about to write it - and rebind() copes: the stat fails, the observed
     * token stays invalid, and the first poll after the file appears sees it
     * as new. */
    if (stat(path, &st) == 0 && !S_ISREG(st.st_mode))
        return;
    rebind(path);
#endif
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
static void wip_bind(void);

static void rebind(const char *path) {
    struct stat st;
    unsigned long long content;

    forget_binding_state();
    snprintf(g_bound, sizeof(g_bound), "%s", path);
    wip_bind();
    if (stat(g_bound, &st) != 0)
        return;
    g_observed = change_token_from_stat(&st);
    g_stats.reads++;
    if (!hash_file_content(g_bound, &content)) {
        g_observed.valid = 0;
        return;
    }
    /* Restore what the user already rejected for this path before deciding
     * whether the file moved: forget_binding_state() above wiped the live
     * copy, and without this a switch away and back re-offers a version they
     * dismissed. */
    if (seen_lookup_suppressed(g_bound, &g_suppressed_content))
        g_suppressed_valid = 1;

    {
        unsigned long long previously_applied;

        if (g_suppressed_valid && content == g_suppressed_content) {
            /* Still the payload they turned down; nothing to offer. */
        } else if (seen_lookup(g_bound, &previously_applied) &&
                   previously_applied != content) {
            /* We have had this file live before, and it is not the bytes we
             * left it at - somebody saved it while we were on another scene.
             * Park it and let the ordinary gate apply it, so coming back to a
             * watched scene converges instead of silently ignoring the edit. */
            g_pending         = 1;
            g_pending_content = content;
            g_have_defer_fp   = 0;
            return;
        }
    }
    if (!seen_remember(g_bound, content)) {
        stop_after_seen_history_oom();
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
    /* D7 preserves the *query* and rescans. Highlights recompute from the live
     * query on their own, but the match count and the F3 ordinal do not - they
     * are cached against a document that no longer exists. */
    if (editor_state_search()->active)
        editor_search_rescan();
    /* Finish the intended ease destination before clearing the gesture state.
     * controls_reset() then releases any mouse drag and momentum without
     * changing the settled pose or the scene camera default. */
    glr_camera_settle_target();
    glr_camera_controls_reset();
    /* Force the `// @plot` re-resolve. Keyed through this notification rather
     * than editor_undo_generation(), which a watched reload does not bump -
     * and the actual rescan runs on the frame path, after the flat refresh,
     * because the series-compatibility check reads the flat program. */
    glr_assign_plot_invalidate_tag_sync();
}

/* ----- stage 2: one incomplete final row ---------------------------------- */

/* A file the watcher read: the raw bytes, the hash of exactly those bytes, and
 * pointers to each physical line within them.
 *
 * One read, one hash, one parse of the same bytes. Hashing separately from
 * loading leaves a window in which a save lands between the two, and the
 * content stamped as "applied" then describes a payload that was never
 * loaded - the same class of bug as stamping a stale parked token, just
 * narrower.
 *
 * Sized from the file rather than from a fixed line cap. The old 2048-line cap
 * was reported as "stage 2 silently stops parking in large files"; that is not
 * quite what it did, and the difference is worth writing down so nobody
 * restores it believing it protected something. MAX_EDITOR_COMMANDS is 1024,
 * so a `.glr` with more physical rows than that cannot load at all - the cap
 * was unreachable for every `.glr` that could. Where it did bite was exported
 * `.c`, whose scaffolding runs long while the document stays small: those
 * files took the fallback path and paid the separate-hash race below. Removing
 * the cap is about that race, not about parking. */
typedef struct {
    char               *bytes;    /* whole file, NUL-terminated and split */
    const char        **ptrs;     /* NULL-terminated, into bytes */
    int                 count;
    unsigned long long  content;  /* hash of the bytes as they were read */
} GlrExtEditFileLines;

/* A refusal, not a budget: past this the watcher hands the path to the loader
 * rather than buffering it. Any scene file near this is generated, and the
 * loader has its own capacity limits well below it. */
#define GLR_EXTEDIT_MAX_BYTES (8u * 1024u * 1024u)

static void file_lines_free(GlrExtEditFileLines *fl) {
    free(fl->bytes);
    free((void *)fl->ptrs);
    fl->bytes = NULL;
    fl->ptrs  = NULL;
    fl->count = 0;
}

/* Split `fl->bytes` in place at newlines, filling fl->ptrs. Returns 0 if any
 * physical line is too long for the importer, which hard-fails such a file -
 * splitting it into two shorter rows here would atomically load a document
 * the path reader refuses outright. */
static int file_lines_split(GlrExtEditFileLines *fl, size_t len) {
    size_t line_start = 0;
    int    cap = 0;

    for (size_t i = 0; i <= len; i++) {
        if (i != len && fl->bytes[i] != '\n')
            continue;
        cap++;
        line_start = i + 1;
    }
    (void)line_start;
    fl->ptrs = (const char **)malloc((size_t)(cap + 1) * sizeof(*fl->ptrs));
    if (!fl->ptrs)
        return 0;

    line_start = 0;
    fl->count  = 0;
    for (size_t i = 0; i <= len; i++) {
        size_t line_len;
        if (i != len && fl->bytes[i] != '\n')
            continue;
        /* A trailing newline ends the last line; it does not start an empty
         * one, which is what the fgets-based reader produced too. */
        if (i == len && i == line_start)
            break;
        fl->bytes[i] = '\0';
        line_len = i - line_start;
        while (line_len > 0 && fl->bytes[line_start + line_len - 1] == '\r')
            fl->bytes[line_start + --line_len] = '\0';
        if (line_len >= (size_t)MAX_LINE_LEN)
            return 0;
        fl->ptrs[fl->count++] = fl->bytes + line_start;
        line_start = i + 1;
    }
    fl->ptrs[fl->count] = NULL;
    return 1;
}

static int file_lines_read(const char *path, GlrExtEditFileLines *fl) {
    FILE  *f;
    long   size;
    size_t len;

    fl->bytes   = NULL;
    fl->ptrs    = NULL;
    fl->count   = 0;
    fl->content = 0;

    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0 || (unsigned long)size > GLR_EXTEDIT_MAX_BYTES) {
        fclose(f);
        return 0;
    }
    fl->bytes = (char *)malloc((size_t)size + 1);
    if (!fl->bytes) {
        fclose(f);
        return 0;
    }
    len = fread(fl->bytes, 1, (size_t)size, f);
    if (ferror(f)) {
        fclose(f);
        file_lines_free(fl);
        return 0;
    }
    fclose(f);
    fl->bytes[len] = '\0';
    /* Hashed here, before the split writes NULs over the newlines, so this is
     * the hash of the file's own bytes - the same value hash_file_content()
     * would produce, and the value the load below is actually built from. */
    fl->content = hash_bytes(hash_init(), fl->bytes, len);
    if (!file_lines_split(fl, len)) {
        file_lines_free(fl);
        return 0;
    }
    return 1;
}

/* Which physical row, if any, is a half-typed command the user is still
 * writing? Returns its index, or -1.
 *
 * Replays the importer's own accumulator over the file, using the *shared*
 * scanner (repl/line_scan.h) rather than a lookalike - the two have to agree
 * or this parks a row the importer would have accepted. A statement is open
 * until the bracket depth is back to zero AND the last code character closes
 * it; rows with no code (blank, comment, directive) are not statements and are
 * skipped, which is what keeps an ordinary trailing `// note` in the document
 * instead of parking it as a half-typed command.
 *
 * The terminator - not "ends in a comma or an operator" - is the test, and it
 * is the whole point: `glVertex3f(1, 2, 3)` with the `;` not yet typed has
 * depth 0 and ends in `)`. The weaker rule would leave it in the file, where
 * `import_finish_load` flushes it at EOF and the parser adds the `;` back
 * while rebuilding canonical text - so the almost-finished command commits
 * silently as a document row and stage 2's payoff never fires for the most
 * common half-typed line there is.
 *
 * Only a *single* trailing row qualifies. An unfinished statement absorbs
 * every physical line after it, so one stray unclosed paren mid-file would
 * otherwise make the whole tail "the incomplete row" and strip it. Refusing
 * the multi-row case leaves ATOMIC to reject the file and say which line, and
 * matches this stage's scope exactly. */
static int find_incomplete_final_row(const char *const *lines, int count) {
    int depth = 0, started = 0, stmt_start = -1, last_code = -1;
    /* Persists across every line, not just the lines of one statement: a
     * `/ * ... * /` span need not close on the line it opened, and these are
     * RAW physical lines - unlike the importer, nothing stripped comments for
     * us first. Without it a trailing C-style comment has no statement
     * terminator and reads as a half-typed command. */
    int in_block_comment = 0;

    for (int i = 0; i < count; i++) {
        int code_len = 0;
        int d = started ? depth : 0;
        char last = repl_scan_code_line(lines[i], &d, &in_block_comment,
                                        &code_len);

        if (code_len == 0)
            continue;                 /* blank / comment / directive */
        if (!started) {
            started    = 1;
            stmt_start = i;
        }
        depth     = d;
        last_code = i;
        if (depth <= 0 && repl_is_stmt_terminator(last))
            started = 0;
    }

    if (!started || stmt_start != last_code)
        return -1;
    return last_code;
}

/* ----- reload ------------------------------------------------------------- */

/* `doc_row` is where the row belongs in the document, or -1 for "past the end"
 * - which is the only answer stage 2 can give, because a saved file says
 * nothing about where the user is and the row it parks is the trailing one.
 * The sidecar knows better and passes the mapped position.
 *
 * Insert mode follows from that. Parked at the end, the input row appends,
 * which is how the code panel already renders the trailing live row. Parked
 * between two rows, a local commit has to *insert* there - overwrite would
 * eat the row below, which is a row the editor still has. */
static void park_incomplete_row(const char *text, int doc_row) {
    const char *canon;
    int canon_len, lead, count;
    char stripped[MAX_INPUT_LEN];

    if (!text)
        text = "";
    /* Editor input, live guides, and autocomplete all require the command
     * prefix at byte 0. Sidecar / .glr rows keep their file indent; strip it
     * the same way editor_load_line_to_input does. vim's col('.') still
     * counts the indent, so remember how much we removed. */
    lead = 0;
    while (text[lead] == ' ' || text[lead] == '\t')
        lead++;
    repl_canonical_input_view(text, &canon, &canon_len);
    if (canon_len >= MAX_INPUT_LEN)
        canon_len = MAX_INPUT_LEN - 1;
    memcpy(stripped, canon, (size_t)canon_len);
    stripped[canon_len] = '\0';
    /* Re-parking the already-canonical copy must keep the original lead, or
     * a cursor-only return to the hole maps the published column as if the
     * indent were still in the buffer. */
    if (lead == 0 && g_parked_valid && strcmp(text, g_parked_row) == 0)
        lead = g_parked_lead;

    count = repl_state_document_count();
    if (doc_row < 0 || doc_row > count)
        doc_row = count;
    /* Order matters: editor_input_set_text() ends by snapping the cursor to
     * end-of-text, so text first. Stage 2 carries no column, and end-of-text
     * is the right answer for a row the user is still typing.
     *
     * No placeholder document row is inserted; the row is deliberately NOT in
     * the document, so an outbound write drops it. */
    editor_insert_mode_set(doc_row < count);
    editor_state_edit_line_set(doc_row);
    editor_input_set_text(stripped);
    snprintf(g_parked_row, sizeof(g_parked_row), "%s", stripped);
    g_parked_lead  = lead;
    g_parked_valid = 1;
}

/* Is the input row dirty in the sense that matters - typing that a reload
 * would destroy and Ctrl+Z could not recover? A parked WIP row is the
 * watcher's own text, so it does not count until the user changes it.
 *
 * Ownership is tracked by text, which D5 sanctions ("the input text (or an
 * input revision)"). The gap a revision number would close - edit the parked
 * row, then retype it character-for-character - costs nothing when it fires:
 * what a reload would overwrite is byte-identical to what it replaces it
 * with. */
static int input_row_is_dirty(void) {
    if (!editor_input_has_uncommitted_change())
        return 0;
    if (g_parked_valid && strcmp(editor_input_text(), g_parked_row) == 0)
        return 0;
    return 1;
}

static void apply_reload(const char *path) {
    ReplSceneLoadOpts opts;
    EditorUndoHistorySnapshot *history = NULL;
    GlrExtEditFileLines fl = { NULL, NULL, 0, 0 };
    char parked[MAX_INPUT_LEN];
    unsigned long long loaded_content = 0;
    int have_loaded_content = 0;
    int have_parked = 0;
    int is_user_scene = repl_active_user_scene() >= 0;
    int history_ok = 1;
    int ok;

    repl_scene_load_opts_init(&opts, repl_scene_format_from_path(path));
    /* The loader stays strictly atomic: any rejected line fails the whole
     * import and the live document is untouched. The incomplete-row allowance
     * below is NOT a loader concession - the controller removes that row
     * first, and the loader never sees it. */
    opts.policy = REPL_SCENE_LOAD_POLICY_ATOMIC;
    /* D3: an external text edit is geometry, not a presentation reset. */
    opts.apply_cfg    = 0;
    opts.camera_apply = REPL_CAMERA_APPLY_NONE;

    /* Read once: the load below is built from these bytes and the applied stamp
     * is the hash of these bytes, so no save can slip between the two.
     *
     * Both formats are read, but only `.glr` gets the incomplete-row search:
     * the heuristic is meaningless on exported C, where the authored command
     * sits inside display() followed by generated wrapper rows and is never
     * the final non-empty physical row. Falling back to the path loader (a
     * file too large to buffer, an over-long line, a read error) reopens the
     * hash/load window, which is why that fallback also re-hashes below. */
    if (file_lines_read(path, &fl)) {
        loaded_content      = fl.content;
        have_loaded_content = 1;
    } else {
        have_loaded_content = hash_file_content(path, &loaded_content);
    }

    if (opts.format == REPL_EXAMPLE_SOURCE_GLR && fl.ptrs) {
        int row = find_incomplete_final_row(fl.ptrs, fl.count);
        if (row >= 0) {
            snprintf(parked, sizeof(parked), "%s", fl.ptrs[row]);
            have_parked = 1;
            /* Remove that ONE row and close the gap. Truncating the list here
             * instead would also throw away whatever follows it - the selected
             * row is the last row with *code*, so a note written underneath a
             * half-typed command is exactly what sits after it. */
            for (int i = row; i < fl.count - 1; i++)
                fl.ptrs[i] = fl.ptrs[i + 1];
            fl.count--;
            fl.ptrs[fl.count] = NULL;
            /* What is left may be nothing at all - a brand new scene whose
             * only content is the command being typed. "No commands loaded" is
             * the right verdict for a file the user asked to open and the
             * wrong one here, because the emptiness is a removal this code
             * just performed. A rejected line still fails the import. */
            opts.allow_empty = 1;
        }
    }

    /* D4. The history capture is heap-backed on purpose: with a full ring the
     * push below has already overwritten the oldest entry, and restoring
     * head/count indices cannot bring it back. */
    if (is_user_scene) {
        history = editor_undo_history_capture();
        if (!history) {
            repl_set_status_error("Watch: out of memory preserving undo history");
            file_lines_free(&fl);
            g_stats.failures++;
            g_pending       = 0;
            g_have_defer_fp = 0;
            return;
        }
        editor_undo_push_snapshot();
    }

    if (fl.ptrs)
        ok = repl_reload_active_scene_from_lines(fl.ptrs, path, &opts);
    else
        ok = repl_reload_active_scene_from_path(path, &opts);
    file_lines_free(&fl);

    if (!ok) {
        if (history)
            (void)editor_undo_history_restore(history);
        g_stats.failures++;
    } else {
        g_applied_content  = have_loaded_content ? loaded_content
                                                 : g_pending_content;
        g_applied_valid    = 1;
        g_suppressed_valid = 0;
        history_ok = seen_remember(path, g_applied_content);
        g_stats.reloads++;
        /* The input row was clean - deferral guarantees it - but it still
         * holds the OLD document's row text. */
        editor_insert_mode_set(0);
        editor_input_clear();
        editor_state_edit_line_clamp();
        g_parked_valid = 0;
        if (have_parked) {
            park_incomplete_row(parked, -1);
            g_stats.parked_rows++;
        }
        glr_extedit_notify_reloaded();
    }
    editor_undo_history_destroy(history);

    g_pending       = 0;
    g_have_defer_fp = 0;
    if (!history_ok)
        stop_after_seen_history_oom();
}

/* ----- stage 2.5: the live WIP sidecar ------------------------------------ */

static int gate_is_shut(void);
static void park_incomplete_row(const char *text, int doc_row);

/* One `// @cursor <row> <col>` line, 1-based row and 1-based byte column, as
 * vim's line('.') / col('.') report them. Returns 0 for anything else, so a
 * buffer that merely mentions the word is not mistaken for the directive. */
static int parse_cursor_directive(const char *line, int *row, int *col) {
    const char *p = line;
    char *end;
    long r, c;

    if (!line)
        return 0;
    while (*p == ' ' || *p == '\t')
        p++;
    if (p[0] != '/' || p[1] != '/')
        return 0;
    p += 2;
    while (*p == ' ' || *p == '\t')
        p++;
    if (strncmp(p, "@cursor", 7) != 0)
        return 0;
    p += 7;
    if (*p != ' ' && *p != '\t')
        return 0;
    r = strtol(p, &end, 10);
    if (end == p)
        return 0;
    p = end;
    c = strtol(p, &end, 10);
    if (end == p)
        return 0;
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0')
        return 0;
    if (r < 1 || c < 1)
        return 0;
    *row = (int)r;
    *col = (int)c;
    return 1;
}

static void wip_map_release(void) {
    free(g_wip_rows);
    g_wip_rows     = NULL;
    g_wip_rows_cap = 0;
    g_wip_map_valid = 0;
    repl_scene_row_map_init(&g_wip_map, NULL, 0, 0);
}

static void wip_forget(void) {
    g_wip_path[0]       = '\0';
    g_wip_observed.valid = 0;
    g_wip_payload_valid  = 0;
    g_wip_suppressed_valid = 0;
    g_wip_have_entry_fp  = 0;
    g_wip_active         = 0;
    g_wip_hold           = 0;
    g_wip_recover_offer  = 0;
    g_wip_offering       = 0;
    g_wip_saw_cursor     = 0;
    g_wip_entry_pushed   = 0;
    g_wip_have_doc_fp    = 0;
    wip_map_release();
}

/* Grow the cached map to hold one entry per physical row, and hand it a fresh
 * nonce. Returns 0 only on allocation failure, where the caller falls back to
 * not following the cursor rather than to following it wrongly. */
static int wip_map_reserve(int rows) {
    if (rows > g_wip_rows_cap) {
        int  want = rows < 256 ? 256 : rows;
        int *grown = (int *)realloc(g_wip_rows, (size_t)want * sizeof(*grown));
        if (!grown)
            return 0;
        g_wip_rows     = grown;
        g_wip_rows_cap = want;
    }
    /* Never zero: zero is the reader's "no hole requested". Bumped per import
     * so a marker left in a file by an earlier one cannot be consumed by a
     * later one. */
    if (++g_wip_nonce == 0)
        g_wip_nonce = 1;
    repl_scene_row_map_init(&g_wip_map, g_wip_rows, g_wip_rows_cap,
                            g_wip_nonce);
    g_wip_map_valid = 0;
    return 1;
}


/* The document as this module last left it, so a later poll can tell "nobody
 * touched it" from "the user undid or committed something". */
static void wip_stamp_document(void) {
    g_wip_doc_fp      = document_fingerprint();
    g_wip_have_doc_fp = 1;
}

/* A WIP session ends. `why` is shown; NULL says nothing (the caller has its
 * own message, or the session simply never started). */
static void wip_end_session(const char *why) {
    g_wip_active        = 0;
    g_wip_entry_pushed  = 0;
    g_wip_have_doc_fp   = 0;
    g_wip_have_entry_fp = 0;
    if (why)
        repl_set_status(why);
}

/* The sidecar, read and split the same way the scene file is, plus the two
 * things only it carries: the cursor it names and the hash of everything
 * else. */
typedef struct {
    GlrExtEditFileLines lines;    /* payload only - the @cursor row is removed */
    unsigned long long  payload;  /* hash of the payload, cursor row excluded */
    int                 cursor_row;   /* 1-based, or 0 when the file has none */
    int                 cursor_col;   /* 1-based byte column */
} GlrExtEditWip;

static void wip_free(GlrExtEditWip *w) {
    file_lines_free(&w->lines);
}

/* Read the sidecar. The payload hash deliberately excludes the trailing
 * `@cursor` line - that exclusion is the whole fast path, and getting it wrong
 * turns every cursor keypress into a document reimport.
 *
 * Only the *last* line is considered, because that is where the publisher puts
 * it: a buffer whose body happens to contain the same text keeps it as
 * ordinary content. */
static int wip_read(const char *path, GlrExtEditWip *w) {
    w->payload    = 0;
    w->cursor_row = 0;
    w->cursor_col = 1;
    if (!file_lines_read(path, &w->lines))
        return 0;
    if (w->lines.count > 0 &&
        parse_cursor_directive(w->lines.ptrs[w->lines.count - 1],
                               &w->cursor_row, &w->cursor_col)) {
        w->lines.count--;
        w->lines.ptrs[w->lines.count] = NULL;
    }
    w->payload = hash_lines(w->lines.ptrs, w->lines.count);
    return 1;
}

/* Is physical row `row` (1-based) a statement the user has not finished?
 *
 * The same evidence stage 2 uses on the trailing row - the importer's own
 * scanner, bracket depth back to zero AND a last code character that closes
 * the statement - applied at a row the editor named instead of one this code
 * guessed. Rows with no code are not statements and are never parked. */
static int row_is_incomplete(const char *const *lines, int row) {
    int depth = 0, code_len = 0, in_block = 0;
    char last;

    last = repl_scan_code_line(lines[row - 1], &depth, &in_block, &code_len);
    if (code_len == 0)
        return 0;
    return !(depth <= 0 && repl_is_stmt_terminator(last));
}

/* vim's col('.') counts leading spaces/tabs. The live input is canonical
 * (editor_load_line_to_input / park_incomplete_row strip them), so the
 * published column has to be mapped off that lead. Exported C is the
 * usual case: snippet-body rows are indented inside draw_scene(). */
static int physical_line_lead(const char *line) {
    int n = 0;
    if (!line)
        return 0;
    while (line[n] == ' ' || line[n] == '\t')
        n++;
    return n;
}

/* The sidecar's payload hash - its bytes without the trailing `@cursor` line.
 * Defined against wip_read() so it cannot drift from the hash the live path
 * compares against; the read is one-off (a bind, or a recovery prompt), not
 * per frame. Returns 0 if the file cannot be read. */
static int wip_payload_hash(const char *path, unsigned long long *out) {
    GlrExtEditWip w;

    if (!wip_read(path, &w))
        return 0;
    *out = w.payload;
    /* Learn the trailer here too. `g_wip_saw_cursor` is per binding, which is
     * right - it is a fact about the publisher currently writing this file -
     * but a rebind wipes it, and without seeding it from the read a bind
     * already does, the *first* publication after every scene switch bypasses
     * the torn-write backstop entirely. That first one is no safer than any
     * other. */
    if (w.cursor_row > 0)
        g_wip_saw_cursor = 1;
    wip_free(&w);
    return 1;
}

static void wip_bind(void) {
    struct stat st;

    wip_forget();
    if (!g_bound[0])
        return;
    if (snprintf(g_wip_path, sizeof(g_wip_path), "%s.wip", g_bound) >=
        (int)sizeof(g_wip_path)) {
        g_wip_path[0] = '\0';   /* no room: this binding gets no sidecar */
        return;
    }
    if (stat(g_wip_path, &st) == 0 && S_ISREG(st.st_mode)) {
        unsigned long long payload;

        if (!seen_wip_followed(g_bound) &&
            wip_payload_hash(g_wip_path, &payload) &&
            seen_wip_declined(g_bound, payload)) {
            /* Already turned down, and the payload is unchanged since. Asking
             * again on every scene switch would make the answer worthless.
             * Hold, and let a genuinely new payload lift it.
             *
             * Restored into the live suppression too, so the continuously-
             * bound path agrees: without it, the first change-token movement
             * lifts the hold and applies the very payload they declined -
             * which a touch that rewrites no bytes is enough to trigger. */
            g_wip_hold             = 1;
            g_wip_observed         = change_token_from_stat(&st);
            g_wip_suppressed       = payload;
            g_wip_suppressed_valid = 1;
        } else if (seen_wip_followed(g_bound)) {
            /* Read for its own sake: nothing here needs the hash, but the
             * trailer observation it seeds is what keeps the torn-write
             * backstop armed across the switch. */
            (void)wip_payload_hash(g_wip_path, &payload);
            /* Coming back to a scene whose editor is still open. Leave the
             * observed token invalid so the next poll reads the sidecar as an
             * ordinary content update and the document converges on whatever
             * the editor now holds - the same answer the scene file's own
             * per-path memory gives on the way back, and for the same reason:
             * the slot's text and the buffer can have diverged while we were
             * elsewhere. */
            g_wip_observed.valid = 0;
        } else {
            (void)wip_payload_hash(g_wip_path, &payload);   /* as above */
            g_wip_recover_offer = 1;
            g_wip_hold          = 1;
            g_wip_observed      = change_token_from_stat(&st);
        }
    }
}

/* Move the live cursor to wherever physical row `row` ended up.
 *
 * Three answers, and the third is why the map records a sentinel rather than
 * guessing: the row is the parked one (put the caret at the typed column), the
 * row has a document row (move the edit line), or the row has no editable row
 * at all - a header, a directive, exported C's scaffolding - and the honest
 * response is to leave the cursor where it is. A placement that actually
 * moves the caret also requests follow-scroll; an unmapped row does not
 * touch the viewport.
 *
 * `phys_line` is the sidecar text of that physical row (indent included)
 * so a mapped placement can convert vim's column the same way a parked
 * one does. NULL means no indent to subtract. */
static void wip_place_cursor(int row, int col, const char *phys_line) {
    int doc_row;

    if (!g_wip_map_valid || row < 1 || row > g_wip_map.count)
        return;
    if (g_wip_map.hole_row == row && g_parked_valid) {
        int len, pos;
        /* Re-park rather than assume the text is still there. The input buffer
         * holds one row at a time, so moving the caret onto an ordinary row
         * below loaded that row over the parked one; coming back has to put it
         * back, or the half-typed command silently becomes whatever row the
         * caret last visited. A locally-edited parked row cannot reach this -
         * the gate is shut while it is dirty. */
        if (strcmp(editor_input_text(), g_parked_row) != 0)
            park_incomplete_row(g_parked_row, g_wip_map.hole_doc_row);
        len = (int)strlen(editor_input_text());
        pos = col - 1 - g_parked_lead;
        if (pos < 0)   pos = 0;
        if (pos > len) pos = len;
        editor_cursor_pos_set(pos);
        editor_scroll_follow_cursor_set(1);
        return;
    }
    doc_row = g_wip_map.doc_row[row - 1];
    if (doc_row == REPL_ROW_MAP_NONE)
        return;
    /* Insert mode off first, and both halves matter. A row that *exists* is
     * being edited in place, not inserted before - and while insert mode is on,
     * any non-empty input counts as uncommitted whatever the row underneath
     * says, so leaving it on after a mid-file park would shut the deferral gate
     * the moment the caret moved to an ordinary row and stop the following
     * entirely. editor_load_line_to_input() does not clear it. */
    editor_insert_mode_set(0);
    editor_state_edit_line_set(doc_row);
    editor_state_edit_line_clamp();
    /* Load the row, exactly as arrowing onto it would. Not cosmetic either: the
     * input buffer is compared against the row under the cursor, and parking an
     * *empty* buffer on an occupied row reads as a pending deletion - which
     * shuts the same gate. Loading also makes the mirror honest: the row the
     * editor says you are on is the row gl-repl offers to edit. */
    editor_load_line_to_input(editor_state_edit_line());
    {
        int len = (int)strlen(editor_input_text());
        int pos = col - 1 - physical_line_lead(phys_line);
        if (pos < 0)   pos = 0;
        if (pos > len) pos = len;
        editor_cursor_pos_set(pos);
    }
    editor_scroll_follow_cursor_set(1);
}

/* One content update: replace the document from the sidecar's payload, parking
 * the named row when it is half-typed.
 *
 * The difference from apply_reload() is entirely in *which* row is removed.
 * Stage 2 guesses - the last row with code - because a saved file says
 * nothing about where the user is. Here the editor says, so the row can be
 * anywhere, and an incomplete row in the middle of a file becomes followable
 * instead of failing the import. It is still only removed when it is
 * incomplete: a finished command belongs in the document, and parking it would
 * take its geometry out of the scene while the user is looking at it. */
static void wip_apply_content(GlrExtEditWip *w) {
    ReplSceneLoadOpts opts;
    EditorUndoHistorySnapshot *history = NULL;
    char marker[MAX_LINE_LEN];
    char parked[MAX_INPUT_LEN];
    int  have_parked = 0;
    int  is_user_scene = repl_active_user_scene() >= 0;
    int  row = w->cursor_row;
    int  ok;

    repl_scene_load_opts_init(&opts, repl_scene_format_from_path(g_bound));
    opts.policy       = REPL_SCENE_LOAD_POLICY_ATOMIC;
    opts.apply_cfg    = 0;
    opts.camera_apply = REPL_CAMERA_APPLY_NONE;
    /* A keystroke is not a load event; see ReplSceneLoadOpts.quiet. */
    opts.quiet        = 1;

    if (!wip_map_reserve(w->lines.count > 0 ? w->lines.count : 1)) {
        repl_set_status_error("Watch: out of memory following the editor");
        return;
    }
    opts.row_map = &g_wip_map;

    if (row < 1 || row > w->lines.count || !row_is_incomplete(w->lines.ptrs,
                                                              row)) {
        /* The cursor is not on a half-typed row. It can still be true that the
         * *trailing* row is one - typed, then navigated away from - and stage
         * 2's heuristic is exactly the answer for that shape. Without this
         * fallback the whole scene would freeze behind a row the user is no
         * longer looking at. */
        row = find_incomplete_final_row(w->lines.ptrs, w->lines.count) + 1;
    }
    if (row >= 1) {
        snprintf(parked, sizeof(parked), "%s", w->lines.ptrs[row - 1]);
        have_parked = 1;
        /* Substituted, not deleted: the reader cannot report where a row it
         * never saw would have gone, and that position is where the input row
         * has to sit. */
        if (repl_scene_cursor_hole_line(marker, (int)sizeof(marker),
                                        g_wip_nonce) > 0) {
            w->lines.ptrs[row - 1] = marker;
            opts.allow_empty = 1;   /* the buffer may be only this row */
        } else {
            have_parked = 0;
        }
    }

    /* D4, once per session rather than once per keystroke: the 32-slot ring
     * is not a place to store somebody's typing. The history capture is the
     * same heap-backed one apply_reload() uses: ATOMIC restores the document
     * on failure, but a push that already evicted the oldest undo entry
     * cannot be undone by putting the document back. */
    if (is_user_scene && !g_wip_entry_pushed) {
        g_wip_entry_fp      = document_fingerprint();
        g_wip_have_entry_fp = 1;
        history = editor_undo_history_capture();
        if (!history) {
            repl_set_status_error("Watch: out of memory preserving undo history");
            g_stats.failures++;
            return;
        }
        editor_undo_push_snapshot();
        g_wip_entry_pushed = 1;
    }

    ok = repl_reload_active_scene_from_lines(w->lines.ptrs, g_bound, &opts);
    if (!ok) {
        if (history) {
            (void)editor_undo_history_restore(history);
            /* The push never landed; the next successful update is still
             * the session's first and must take a fresh snapshot. */
            g_wip_entry_pushed  = 0;
            g_wip_have_entry_fp = 0;
        }
        editor_undo_history_destroy(history);
        g_stats.failures++;
        /* Deliberately still "active" if a session had already started: the
         * editor is mid-thought and the next keystroke usually fixes it. The
         * document was rolled back, so the fingerprint below still describes
         * what is live. */
        wip_stamp_document();
        return;
    }
    editor_undo_history_destroy(history);

    g_wip_map_valid = 1;
    g_wip_active    = 1;
    seen_note_wip_followed(g_bound);
    /* A payload landed while the recovery prompt was still on screen asking
     * whether to follow this file. The editor is demonstrably alive and
     * publishing - which is the resume condition - so following is right, but
     * leaving the question up is not: it now asks about content that is
     * already live, and either answer is meaningless. Retract it, and drop the
     * decline it had pre-recorded, which describes a payload the file has
     * moved off. */
    if (g_wip_offering) {
        g_wip_offering = 0;
        seen_clear_wip_declined(g_bound);
        if (glr_modal_active() &&
            glr_modal_kind() == GLR_MODAL_CONFIRM_WIP_RECOVER)
            glr_modal_cancel();
    }
    g_stats.reloads++;
    g_stats.wip_updates++;
    editor_insert_mode_set(0);
    editor_input_clear();
    editor_state_edit_line_clamp();
    g_parked_valid = 0;
    if (have_parked) {
        park_incomplete_row(parked, g_wip_map.hole_doc_row);
        g_stats.parked_rows++;
    }
    wip_place_cursor(w->cursor_row, w->cursor_col,
                     (w->cursor_row >= 1 && w->cursor_row <= w->lines.count)
                         ? w->lines.ptrs[w->cursor_row - 1] : NULL);
    glr_extedit_notify_reloaded();
    /* The scene file on disk still holds the last saved version, and the
     * document no longer matches it. Forget the applied stamp so a later `:w`
     * of exactly these bytes is not mistaken for something already loaded -
     * and so returning to this scene later re-reads it. */
    g_pending       = 0;
    g_have_defer_fp = 0;
    wip_stamp_document();
}

/* The sidecar vanished - `:q`, `VimLeave`, or someone deleting it.
 *
 * The split is D4's, arrived at from the other side. A user scene owns its
 * document, so the typed text stays as unsaved local work and Ctrl+Z still
 * reaches the pre-session version. A transient or an unedited example does
 * not: D4 deliberately created no slot and pushed no undo entry, so keeping
 * the text would convert discarded editor buffer into the live catalog scene,
 * under an example's identity, with its source gone. That one goes back to the
 * file. */
static void wip_handle_deleted(void) {
    int is_user_scene = repl_active_user_scene() >= 0;

    /* Before the idle early-out below, not after it. Following a path is
     * evidence about one editor, and this is the moment that editor is
     * observed to be gone - whether or not a session was still live when it
     * went. A local commit or a Ctrl+Z ends the session and *deliberately*
     * leaves the mark set, because the editor is still open; if that editor
     * then quits, this is the only place that learns it. Leaving the mark
     * would auto-resume the next editor's leftovers with no prompt, which is
     * exactly what the mark exists to prevent. */
    seen_clear_wip_followed(g_bound);

    if (!g_wip_active) {
        wip_end_session(NULL);
        g_wip_hold = 0;
        return;
    }
    if (is_user_scene) {
        wip_end_session("Watch: editor closed; unsaved WIP text kept");
    } else {
        ReplSceneLoadOpts opts;
        repl_scene_load_opts_init(&opts, repl_scene_format_from_path(g_bound));
        opts.policy       = REPL_SCENE_LOAD_POLICY_ATOMIC;
        opts.apply_cfg    = 0;
        opts.camera_apply = REPL_CAMERA_APPLY_NONE;
        editor_insert_mode_set(0);
        editor_input_clear();
        g_parked_valid = 0;
        if (repl_reload_active_scene_from_path(g_bound, &opts)) {
            g_stats.reloads++;
            editor_state_edit_line_clamp();
            glr_extedit_notify_reloaded();
            wip_end_session("Watch: editor closed; reloaded the saved file");
        } else {
            g_stats.failures++;
            wip_end_session("Watch: editor closed; could not reload the file");
        }
        /* Whatever the file holds is now what the document holds - or the
         * reload failed and the applied stamp is honest either way only if we
         * re-derive it. Cheapest correct answer: let the main-file poll
         * re-read it by dropping the stamp. */
        g_applied_valid = 0;
    }
    wip_map_release();
    g_wip_payload_valid = 0;
    g_wip_hold          = 0;
}

/* Y at the recovery prompt. The sidecar is left exactly as it is; accepting
 * only says "follow it", and the next poll does the reading. */
static int wip_recover_commit(GlrModalKind kind, const char *text, int context) {
    (void)kind;
    (void)text;
    (void)context;
    g_wip_hold     = 0;
    g_wip_offering = 0;
    seen_clear_wip_declined(g_bound);
    g_wip_suppressed_valid = 0;
    /* Force the read: the observed token was stamped when the offer was made,
     * so nothing has "moved" since. */
    g_wip_observed.valid = 0;
    return 1;
}

/* A sidecar that is already there when the watcher binds means the editor died
 * holding unsaved work. Never applied on sight (that would let a stale file
 * from days ago replace the scene the user just opened); offered once.
 *
 * Declining is Esc, and it does NOT delete the file. The plan's table says
 * delete; leaving it is the smaller surprise - gl-repl did not create it, the
 * editor's own VimLeave hook is what removes it, and the load-bearing halves
 * of that row (never auto-apply, then ignore until the token moves) are both
 * honoured by the hold this sets before the prompt opens. */
static void wip_offer_recovery(void) {
    char question[GLR_EXTEDIT_PATH_MAX + 64];
    unsigned long long payload;

    /* Another modal owns the keyboard. Leave the offer standing and ask when
     * it closes: the hold keeps the sidecar unapplied meanwhile, so waiting
     * costs nothing, and the alternative is worse than it looks - the decline
     * recorded below is durable, so treating "could not ask" as one answers
     * the question on the user's behalf and never asks again. */
    if (glr_modal_active())
        return;

    g_wip_recover_offer = 0;
    snprintf(question, sizeof(question),
             "External WIP recovered from %s. Follow it?", g_wip_path);
    if (!glr_modal_begin(GLR_MODAL_CONFIRM_WIP_RECOVER, question, 0,
                         wip_recover_commit))
        return;     /* refused for a reason retrying cannot fix */
    g_wip_offering = 1;

    /* Recorded now the prompt is actually on screen, not when it is answered:
     * Esc has no callback, so declining is the default and accepting is what
     * clears it. A question nobody was asked has no answer worth keeping. */
    if (wip_payload_hash(g_wip_path, &payload)) {
        seen_note_wip_declined(g_bound, payload);
        g_wip_suppressed       = payload;
        g_wip_suppressed_valid = 1;
    }
}

/* One frame of sidecar watching, run after the scene file's own poll. */
static void wip_poll(void) {
    struct stat st;
    GlrExtEditChangeToken token;
    GlrExtEditWip w;

    if (!g_wip_path[0])
        return;

    if (stat(g_wip_path, &st) != 0) {
        if (g_wip_active || g_wip_hold || g_wip_observed.valid) {
            wip_handle_deleted();
            g_wip_observed.valid = 0;
            g_wip_recover_offer  = 0;
            /* A recovery question cannot outlive the sidecar it names. This
             * also distinguishes a later file appearing on the already-bound
             * path: that is an ordinary post-bind publication, not the
             * pre-bind leftover this offer described. */
            if (g_wip_offering && glr_modal_active() &&
                glr_modal_kind() == GLR_MODAL_CONFIRM_WIP_RECOVER)
                glr_modal_cancel();
            g_wip_offering = 0;
        }
        return;
    }

    if (g_wip_recover_offer) {
        wip_offer_recovery();
        return;
    }

    token = change_token_from_stat(&st);
    if (change_token_equal(&token, &g_wip_observed))
        return;
    g_wip_observed = token;

    /* The editor is publishing again, which is exactly the condition a hold
     * waits for: it was set by declining the recovery offer, and declining
     * means "not this file as it stands", not "never". Following resumes with
     * this update rather than the one after - the local-edit case below is
     * where a publication is deliberately dropped, and doing it in both places
     * would cost two. */
    g_wip_hold = 0;

    /* The same gate the scene file waits on: a lesson owns the document, and
     * so does a half-typed local line the user has not finished. */
    if (gate_is_shut())
        return;

    g_stats.reads++;
    if (!wip_read(g_wip_path, &w))
        return;

    /* Somebody else moved the document since the last update - Ctrl+Z, or a
     * commit. The two exits share a suppressed-payload hash (D5) so a later
     * CursorMoved of the dismissed bytes cannot look like new content, but
     * they are not the same pause:
     *
     *   Ctrl+Z   the sidecar may have been written before or after the key,
     *            so this publication is dropped regardless of its payload.
     *            Following resumes on the next publication whose hash is
     *            not the dismissed one.
     *   commit   fall through. Same payload is ignored below; a genuinely
     *            new payload is followed immediately. */
    if (g_wip_active && g_wip_have_doc_fp &&
        document_fingerprint() != g_wip_doc_fp) {
        int is_undo = g_wip_have_entry_fp &&
                      document_fingerprint() == g_wip_entry_fp;

        if (g_wip_payload_valid) {
            g_wip_suppressed       = g_wip_payload;
            g_wip_suppressed_valid = 1;
            g_stats.dismissals++;
        }
        wip_end_session("Watch: kept your edit; live follow paused");
        g_wip_payload_valid = 0;
        if (is_undo) {
            wip_free(&w);
            return;
        }
    }

    /* A publisher that has been appending `// @cursor` and suddenly is not is
     * almost certainly not a publisher at all: it is this file being read
     * between the truncate and the last byte of an in-place write. Adopting
     * that prefix would replace the scene with however much of it had reached
     * the disk - and it would do so *cleanly*, because a prefix of a valid
     * program is usually a valid program, with the cut row parked as an
     * ordinary half-typed one.
     *
     * A heuristic, and only a backstop: truncation that happens to land after
     * a complete trailer is indistinguishable, and the real protection is the
     * plugin's sibling-temp-and-rename (`make check-wip-plugin-atomic`). It is
     * conditional on having seen a trailer before so that an integration which
     * never writes one is not refused outright. */
    if (g_wip_saw_cursor && w.cursor_row == 0) {
        g_stats.failures++;
        repl_set_status_error("Watch: ignored a truncated editor buffer");
        wip_free(&w);
        return;
    }
    if (w.cursor_row > 0)
        g_wip_saw_cursor = 1;

    if (g_wip_suppressed_valid && w.payload == g_wip_suppressed) {
        /* Dismissed payload: ignore both a reimport and a cursor-only
         * placement derived from a map that was never loaded for this
         * document. */
        wip_free(&w);
        return;
    }

    if (g_wip_payload_valid && w.payload == g_wip_payload) {
        /* Cursor-only. No import, no undo entry, no notification: the document
         * did not change, and the caret is the only thing that did. */
        wip_place_cursor(w.cursor_row, w.cursor_col,
                         (w.cursor_row >= 1 && w.cursor_row <= w.lines.count)
                             ? w.lines.ptrs[w.cursor_row - 1] : NULL);
        g_stats.cursor_moves++;
        wip_free(&w);
        wip_stamp_document();
        return;
    }

    wip_apply_content(&w);
    g_wip_payload          = w.payload;
    g_wip_payload_valid    = 1;
    g_wip_suppressed_valid = 0;
    wip_free(&w);
}

/* ----- poll --------------------------------------------------------------- */

/* A tutorial or a guided tour is driving the document itself; two writers on
 * one document is not a thing this can arbitrate. */
static int lesson_running(void) {
    return tutorial_active() || glr_pointer_script_tour_active();
}

static int gate_is_shut(void) {
    return lesson_running() || input_row_is_dirty();
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
    /* Nothing bound yet means there is nothing to protect - and refusing to
     * resolve in that state left `--watch scene.glr --tutorial N` deaf for the
     * whole session, because the lesson was already up when the watcher armed.
     * A pin over an empty binding protects nothing and costs everything. */
    if (!g_bound[0])
        return 0;
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
    seen_remember_suppressed(g_bound, g_pending_content);
    g_pending            = 0;
    g_have_defer_fp      = 0;
    g_stats.dismissals++;
    snprintf(msg, sizeof(msg), "Watch: %s (local document kept)", why);
    repl_set_status(msg);
}

#if !defined(__EMSCRIPTEN__)
/* The scene file's half of a frame: everything `--watch` did before the
 * sidecar existed. Split out so the sidecar runs on every frame this one
 * returns early from - and it returns early a lot. */
static void scene_file_poll(void) {
    const char *path;
    struct stat st;
    GlrExtEditChangeToken token;
    int in_lesson;

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

    /* D7: at the end of a lesson, discard the version parked during it. A
     * payload computed against the pre-lesson document has no business landing
     * on the document the lesson left behind.
     *
     * D7 also asks for the observed token to be stamped here, so old movement
     * is not re-read as new afterwards. That requirement is already met
     * structurally and must NOT be met again by re-stamping from disk: this
     * poll loop reads and stamps `g_observed` on every change *including*
     * during a lesson (it defers rather than ignoring), so the token is always
     * current. Re-stamping would additionally swallow a save that landed
     * between the lesson ending and this poll - a real edit, discarded for
     * having arrived at the wrong millisecond. */
    in_lesson = lesson_running();
    if (g_was_in_lesson && !in_lesson)
        dismiss_pending("external change discarded at lesson end");
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
        /* The file now holds something already accounted for - gl-repl's own
         * write, a touch that changed no bytes, or a payload a local commit
         * dismissed. Either way it overtakes anything still parked: a pending
         * version describes a payload the file has since moved off. */
        if (g_applied_valid && content == g_applied_content) {
            g_pending       = 0;
            g_have_defer_fp = 0;
            return;
        }
        if (g_suppressed_valid && content == g_suppressed_content) {
            g_pending       = 0;
            g_have_defer_fp = 0;
            return;
        }
        /* A `:w` during a live session: the editor wrote out the very buffer
         * the sidecar has already been feeding us, so the document is that
         * payload already (bar the parked row, which is not the file's to
         * hold). Reloading would clear the input row and the caret for
         * nothing. Stamp it as applied and move on. */
        if (g_wip_payload_valid && content == g_wip_payload) {
            g_applied_content = content;
            g_applied_valid   = 1;
            (void)seen_remember(g_bound, content);
            g_pending       = 0;
            g_have_defer_fp = 0;
            return;
        }
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
}
#endif

void glr_extedit_poll(void) {
#if defined(__EMSCRIPTEN__)
    /* No external editor in a browser tab, and no filesystem worth watching.
     * The TU stays non-empty for the C99 rule and the flag is ignored. */
    return;
#else
    if (!g_enabled)
        return;
    scene_file_poll();
    wip_poll();
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
    if (!seen_remember(g_bound, content)) {
        stop_after_seen_history_oom();
        return;
    }
    /* A save resolves any argument about who wins: the file and the document
     * now agree, so nothing is pending and nothing needs suppressing. */
    g_pending          = 0;
    g_have_defer_fp    = 0;
    g_suppressed_valid = 0;
#endif
}
