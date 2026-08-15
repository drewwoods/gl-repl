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
static GlrExtEditStats       g_stats;

/* What the live document last came from, remembered *per path* rather than
 * only for the current binding.
 *
 * Binding to a file does not reload it - the document usually already is that
 * file, and reloading would clobber unsaved slot edits for nothing. But that
 * reasoning fails on the way back: F12 away, let vim save the file, F12 back,
 * and stamping the new bytes as "applied" buries the external edit forever.
 * With one entry per recently-bound path, rebind can tell the two apart -
 * unchanged since we last applied it, or moved while we were elsewhere.
 *
 * Four entries because the case this exists for is a switch away and back. A
 * path that falls out is treated as never seen, which is the conservative
 * answer: stamp and do not reload. */
#define GLR_EXTEDIT_SEEN_MAX 4

typedef struct {
    char               path[GLR_EXTEDIT_PATH_MAX];
    unsigned long long content;
    int                valid;
} GlrExtEditSeen;

static GlrExtEditSeen g_seen[GLR_EXTEDIT_SEEN_MAX];
static int            g_seen_next;

static void seen_remember(const char *path, unsigned long long content) {
    for (int i = 0; i < GLR_EXTEDIT_SEEN_MAX; i++) {
        if (g_seen[i].valid && strcmp(g_seen[i].path, path) == 0) {
            g_seen[i].content = content;
            return;
        }
    }
    snprintf(g_seen[g_seen_next].path, sizeof(g_seen[g_seen_next].path),
             "%s", path);
    g_seen[g_seen_next].content = content;
    g_seen[g_seen_next].valid   = 1;
    g_seen_next = (g_seen_next + 1) % GLR_EXTEDIT_SEEN_MAX;
}

static int seen_lookup(const char *path, unsigned long long *out) {
    for (int i = 0; i < GLR_EXTEDIT_SEEN_MAX; i++) {
        if (g_seen[i].valid && strcmp(g_seen[i].path, path) == 0) {
            *out = g_seen[i].content;
            return 1;
        }
    }
    return 0;
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

static void forget_binding_state(void) {
    g_bound[0]           = '\0';
    g_observed.valid     = 0;
    g_applied_valid      = 0;
    g_suppressed_valid   = 0;
    g_pending            = 0;
    g_have_defer_fp      = 0;
    g_reported_missing   = 0;
    g_parked_valid       = 0;
}

void glr_extedit_set_enabled(int enabled) {
    g_enabled = enabled ? 1 : 0;
    forget_binding_state();
    g_was_in_lesson = 0;
    memset(&g_seen, 0, sizeof(g_seen));
    g_seen_next = 0;
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
     * unbound and let the first poll resolve it properly. */
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
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
    {
        unsigned long long previously_applied;

        if (seen_lookup(g_bound, &previously_applied) &&
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
    g_applied_content = content;
    g_applied_valid   = 1;
    seen_remember(g_bound, content);
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
    glr_camera_settle_target();
    /* Force the `// @plot` re-resolve. Keyed through this notification rather
     * than editor_undo_generation(), which a watched reload does not bump -
     * and the actual rescan runs on the frame path, after the flat refresh,
     * because the series-compatibility check reads the flat program. */
    glr_assign_plot_invalidate_tag_sync();
}

/* ----- stage 2: one incomplete final row ---------------------------------- */

/* A file the watcher read, as physical lines. Heap-backed rather than static
 * BSS: `--watch` is off by default, and half a megabyte of always-resident
 * line buffer for a feature nobody armed is not a trade worth making. */
typedef struct {
    char        *storage;   /* GLR_EXTEDIT_MAX_LINES * MAX_LINE_LEN */
    const char **ptrs;      /* NULL-terminated, into storage */
    int          count;
} GlrExtEditFileLines;

/* Beyond this the watcher falls back to letting the loader read the path
 * itself: the incomplete-row heuristic is a convenience, and a scene file this
 * long is a generated one nobody is hand-typing the tail of. */
#define GLR_EXTEDIT_MAX_LINES 2048

static void file_lines_free(GlrExtEditFileLines *fl) {
    free(fl->storage);
    free((void *)fl->ptrs);
    fl->storage = NULL;
    fl->ptrs    = NULL;
    fl->count   = 0;
}

static int file_lines_read(const char *path, GlrExtEditFileLines *fl) {
    FILE *f;
    char buf[MAX_LINE_LEN];

    fl->storage = (char *)malloc((size_t)GLR_EXTEDIT_MAX_LINES * MAX_LINE_LEN);
    fl->ptrs    = (const char **)malloc((size_t)(GLR_EXTEDIT_MAX_LINES + 1) *
                                        sizeof(*fl->ptrs));
    fl->count   = 0;
    if (!fl->storage || !fl->ptrs) {
        file_lines_free(fl);
        return 0;
    }

    f = fopen(path, "r");
    if (!f) {
        file_lines_free(fl);
        return 0;
    }
    while (fgets(buf, (int)sizeof(buf), f)) {
        char *dst;
        size_t len = strlen(buf);
        /* An over-long physical line would arrive here as two shorter ones, and
         * the watcher would then atomically load a document the file reader
         * refuses outright ("line too long"). Bail instead: the caller falls
         * back to the path loader, which reports it properly. */
        if (len > 0 && buf[len - 1] != '\n' && buf[len - 1] != '\r' &&
            !feof(f)) {
            fclose(f);
            file_lines_free(fl);
            return 0;
        }
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        if (fl->count >= GLR_EXTEDIT_MAX_LINES) {
            fclose(f);
            file_lines_free(fl);
            return 0;   /* too many lines: same fallback */
        }
        dst = fl->storage + (size_t)fl->count * MAX_LINE_LEN;
        memcpy(dst, buf, len + 1);
        fl->ptrs[fl->count] = dst;
        fl->count++;
    }
    if (ferror(f)) {
        fclose(f);
        file_lines_free(fl);
        return 0;
    }
    fclose(f);
    fl->ptrs[fl->count] = NULL;
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

static void park_incomplete_row(const char *text) {
    /* Order matters: editor_input_set_text() ends by snapping the cursor to
     * end-of-text, so text first. Stage 2 carries no column, and end-of-text
     * is the right answer for a row the user is still typing.
     *
     * `edit_line = document_count` parks past the last row so the input row
     * appends - which is already how the code panel renders the trailing live
     * row. No placeholder document row is inserted; the row is deliberately
     * NOT in the document, so an outbound write drops it. */
    editor_insert_mode_set(0);
    editor_state_edit_line_set(repl_state_document_count());
    editor_input_set_text(text);
    snprintf(g_parked_row, sizeof(g_parked_row), "%s", text);
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
    GlrExtEditFileLines fl = { NULL, NULL, 0 };
    char parked[MAX_INPUT_LEN];
    unsigned long long loaded_content;
    int have_loaded_content;
    int have_parked = 0;
    int is_user_scene = repl_active_user_scene() >= 0;
    int ok;

    /* Hash what is on disk NOW, because that is what the load below reads.
     * D8 says a deferred version is re-read when the gate opens rather than
     * replayed from stale bytes - so the token that was parked may describe a
     * payload the file no longer holds, and stamping it as "applied" would
     * make the watcher deaf to a later, genuine save of those exact bytes. */
    have_loaded_content = hash_file_content(path, &loaded_content);

    repl_scene_load_opts_init(&opts, repl_scene_format_from_path(path));
    /* The loader stays strictly atomic: any rejected line fails the whole
     * import and the live document is untouched. The incomplete-row allowance
     * below is NOT a loader concession - the controller removes that row
     * first, and the loader never sees it. */
    opts.policy = REPL_SCENE_LOAD_POLICY_ATOMIC;
    /* D3: an external text edit is geometry, not a presentation reset. */
    opts.apply_cfg    = 0;
    opts.camera_apply = REPL_CAMERA_APPLY_NONE;

    /* Stage 2 is `.glr` only. The heuristic is meaningless on exported C: the
     * incomplete authored command sits inside display() and is followed by
     * generated wrapper rows, so it is never the final non-empty physical
     * row. */
    if (opts.format == REPL_EXAMPLE_SOURCE_GLR && file_lines_read(path, &fl)) {
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
        seen_remember(path, g_applied_content);
        g_stats.reloads++;
        /* The input row was clean - deferral guarantees it - but it still
         * holds the OLD document's row text. */
        editor_insert_mode_set(0);
        editor_input_clear();
        editor_state_edit_line_clamp();
        g_parked_valid = 0;
        if (have_parked) {
            park_incomplete_row(parked);
            g_stats.parked_rows++;
        }
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
    seen_remember(g_bound, content);
    /* A save resolves any argument about who wins: the file and the document
     * now agree, so nothing is pending and nothing needs suppressing. */
    g_pending          = 0;
    g_have_defer_fp    = 0;
    g_suppressed_valid = 0;
#endif
}
