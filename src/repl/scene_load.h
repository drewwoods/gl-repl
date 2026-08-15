/*
 * src/repl/scene_load.h - Explicit load policy for the scene reader.
 *
 * `src/repl/import.c` used to decide three things implicitly - from a
 * filename suffix, from which TU the caller reached, and from nothing at all.
 * This header makes each an explicit parameter carried in one options bag:
 *
 *   format        the authored format, so canonical-document-order checking
 *                 no longer depends on the *diagnostic label* ending in
 *                 ".glr". A catalog entry named "Torus Knot" used to lose
 *                 order validation silently.
 *   policy        warn-and-continue vs. abort-at-first-bad-line. Both
 *                 behaviors already existed, in different files;
 *                 neither is universally right (see below).
 *   apply_cfg     whether the file's `@cfg` bag reaches live state.
 *   camera_apply  what a resolved `@camera` pose is applied *for*, including
 *                 REPL_CAMERA_APPLY_NONE for "read and diagnose, apply
 *                 nothing".
 *
 * The last two exist for the external-editor watch path
 * (docs/plans/active/BYOE.md, D3): a *text* edit arriving from vim is
 * geometry, not a presentation reset, so a watched reload replaces the
 * program and leaves live cfg and camera alone.
 *
 * Policy choice, in one line each:
 *   TOLERANT  a user's hand-edited file that loses 33 good rows to 24 bad
 *             ones is hostile. This is what a plain File -> Open does.
 *   ATOMIC    a half-loaded document is worse than no load. The reader
 *             restores the live document from a SceneSnapshot taken at
 *             begin-load, so a rejected file leaves no trace.
 *
 * ATOMIC's rollback covers what SceneSnapshot covers: document commands and
 * text, cursor, predef variables, scratch arrays, func aliases, the
 * scene-subset cfg and the camera pose. It does NOT cover presentation cfg
 * outside the scene subset, nor editor input/selection/search state - callers
 * that need those preserved own them (see src/app/glr_extedit.c, which keeps
 * the input row out of the transaction by deferring instead).
 */
#ifndef REPL_SCENE_LOAD_H
#define REPL_SCENE_LOAD_H

#include "repl/camera_header.h"  /* ReplCameraApplyMode */
#include "repl/examples.h"       /* ReplExampleSourceFormat */
#include "repl/export.h"         /* ReplImportResult */

/* Named _POLICY_ rather than the plan's bare REPL_SCENE_LOAD_TOLERANT /
 * _ATOMIC: `ReplSceneLoadStatus` in repl/scenes.h already owns the
 * REPL_SCENE_LOAD_OK / _ERR_* names, and two enums sharing a prefix in one
 * translation unit read as one. */
typedef enum {
    REPL_SCENE_LOAD_POLICY_TOLERANT = 0, /* warn per bad line, keep the rest */
    REPL_SCENE_LOAD_POLICY_ATOMIC        /* first bad line aborts, restore */
} ReplSceneLoadPolicy;

/* --- physical -> document row map (BYOE stage 2.5) -------------------------
 *
 * An external editor reports the cursor as a **physical file row**. Physical
 * row N is not document row N and no constant relates them: headers, `@cfg` /
 * `@camera` / `@declare` directives, block comments, exported C's wrappers and
 * its staged function bodies all occupy file rows that either produce no
 * editable row at all or produce one somewhere else entirely. So the reader
 * records the correspondence for every physical row it walks, and the caller
 * indexes it instead of subtracting.
 *
 * Why a *cached* map and not just "where did the cursor row land": the whole
 * point of the watcher's content token is that a cursor-only update skips the
 * import. By then the cursor may name a row for which no answer was ever
 * computed, and the two ways out without a map are both wrong - reimport on
 * every cursor twitch, or place the cursor by guesswork. The map is filled by
 * one content import and stays valid until the next one.
 *
 * Memory is the caller's: `doc_row` must have room for at least one entry per
 * physical row, and the reader never keeps a pointer to it past return.
 *
 * Entries are written as rows are fed, with no fixup pass, because the
 * importer only ever appends - see import_row_map_note_applied() for why that
 * holds and what would have to change if it stopped.
 */

/* A physical row with no editable document row of its own - the honest answer
 * for a header, a consumed directive, or exported-C scaffolding, and
 * deliberately distinguishable from document row 0. */
#define REPL_ROW_MAP_NONE (-1)

/* Spelled once. The `//` and the nonce are added by
 * repl_scene_cursor_hole_line() below. */
#define REPL_CURSOR_HOLE_MARKER "@cursor-hole"

typedef struct ReplSceneRowMap {
    int *doc_row;       /* [cap], 0-based document row or REPL_ROW_MAP_NONE */
    int  cap;           /* entries available; physical rows past it are dropped */
    int  count;         /* highest 1-based physical row the reader recorded */

    /* Nonzero enables consumption of one `// @cursor-hole <nonce>` line
     * carrying exactly this value: the reader emits no document row for it and
     * reports where it landed below.
     *
     * The nonce is what keeps this from being a new comment syntax users can
     * trip over. A genuine `// @cursor-hole` written by hand carries no
     * matching number and stays an ordinary comment, and every other load path
     * leaves this zero, so an ordinary File -> Open cannot punch a hole in a
     * document no matter what the file says. */
    unsigned long hole_nonce;
    int           hole_row;      /* 1-based physical row of the marker, or -1 */
    int           hole_doc_row;  /* document row the hole landed at, or -1 */
} ReplSceneRowMap;

/* Point a map at caller-owned storage and clear it. Safe to call again to
 * reuse the same buffer for a later load; the reader also re-clears at
 * begin-load, so a map handed to two loads cannot carry the first one's rows
 * into the second. */
void repl_scene_row_map_init(ReplSceneRowMap *map, int *storage, int cap,
                             unsigned long hole_nonce);

/* The one spelling of the transport marker, so the controller that writes it
 * and the reader that consumes it cannot disagree. Writes
 * `// @cursor-hole <nonce>` and returns its length, or 0 if it would not fit.
 */
int repl_scene_cursor_hole_line(char *buf, int buf_size, unsigned long nonce);

/* Tagged so headers that only pass it by pointer - repl/scenes.h - can
 * forward-declare `struct ReplSceneLoadOpts` instead of pulling this header
 * (and export.h behind it) into every one of their consumers. */
typedef struct ReplSceneLoadOpts {
    ReplExampleSourceFormat format;
    ReplSceneLoadPolicy     policy;
    /* 0 = do not hand the parsed `@cfg` bag to the config bridge. The bag is
     * still *cleared*: it accumulates per load, and leaving it behind would
     * let the next non-watch load inherit a stale accumulator. */
    int                     apply_cfg;
    ReplCameraApplyMode     camera_apply;
    /* 1 = a document with no commands is a legitimate result, not a failure.
     *
     * "No commands loaded" is otherwise reported as an error, and rightly so:
     * for a file the user asked to open it means an empty or non-REPL file,
     * and silently landing on a blank document would strand any `@cfg` side
     * effects on the wrong scene. But the external-editor watcher removes a
     * trailing half-typed row *before* the load (see glr_extedit.h), and a
     * brand new scene being typed in vim can consist of nothing else - so
     * "zero commands" there is the expected outcome of a deliberate removal,
     * not a broken file. Only that caller sets it, and only when it actually
     * removed a row.
     *
     * This does NOT weaken ATOMIC: a rejected line still fails the whole
     * import whatever this says. It suppresses one verdict - the count - and
     * nothing else. */
    int                     allow_empty;
    /* 1 = do not announce the load on stderr. A `.glr` save is a load event
     * worth a line; a keystroke arriving through the WIP sidecar is not, and
     * at typing rate the announcement is thousands of lines of scrollback. The
     * status bar still gets its message either way. */
    int                     quiet;
    /* Optional out-channel: where to record the physical -> document row
     * correspondence, and whether to consume a cursor-hole marker. NULL - the
     * default - means neither. See ReplSceneRowMap above. */
    ReplSceneRowMap        *row_map;
} ReplSceneLoadOpts;

/* Suffix sniff, and the only one left in the tree: `.glr` is the authored
 * format, anything else is treated as exported C. Call it where a path is all
 * the caller has (the filesystem adapter); never re-derive a format from a
 * diagnostic label. */
ReplExampleSourceFormat repl_scene_format_from_path(const char *path);

/* Today's importer behavior, spelled out: tolerant, cfg applied, camera
 * applied as IMPORT (snap + adopt as the scene default). */
void repl_scene_load_opts_init(ReplSceneLoadOpts *opts,
                               ReplExampleSourceFormat format);

/* Reader entry points that take the options bag. The repl_export_load_from_*
 * trio in export.h are thin wrappers over these carrying the defaults above,
 * with `format` derived from the source label's suffix - which is exactly the
 * behavior those callers had before the format became explicit.
 *
 * A NULL `opts` means the same thing those wrappers mean: the defaults above,
 * with `format` derived from the path or label. It does NOT mean "exported C",
 * which would quietly drop canonical-order checking on a `.glr` - the dropout
 * the explicit format was introduced to close. */
int repl_scene_load_from_file(const char *filename,
                              const ReplSceneLoadOpts *opts,
                              ReplImportResult *result);
int repl_scene_load_from_lines(const char *const *lines,
                               const char *source_name,
                               const ReplSceneLoadOpts *opts,
                               ReplImportResult *result);

#endif /* REPL_SCENE_LOAD_H */
