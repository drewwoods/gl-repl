/*
 * glr_extedit.h - External-editor sync (`--watch`).
 *
 * Lets vim / VS Code / anything be a peer author of the live scene: the
 * external editor saves the bound file, gl-repl re-reads it atomically, and
 * the scene updates without leaving the editor. See
 * docs/plans/active/BYOE.md; the decision letters below refer to its
 * "Decisions" section.
 *
 * Outbound is explicit-save only - Ctrl+S stays the only writer, and this
 * module never writes the file. Inbound is one `stat()` per frame.
 *
 * WHAT IT IS BOUND TO (D1). Not a path of its own: the watcher follows
 * `repl_active_scene_bound_path()`, which is the same file Ctrl+S writes.
 * That is deliberate and load-bearing - watching a file gl-repl would not
 * write, or writing one it does not watch, breaks the round trip and leaves
 * the self-write stamp with nothing to suppress. A scene switch (F12, scene
 * tab, File -> Open) therefore re-binds for free, and a built-in example -
 * which has no file - unbinds.
 *
 * THE TWO-LEVEL GATE (D8). Per frame: `stat()`. The file is read and hashed
 * only once the *change token* (nanosecond mtime + inode + size) moves; the
 * document is re-parsed only once the *content token* (a hash of the payload)
 * moves. Nanoseconds rather than `time_t` seconds because a same-second final
 * save would otherwise be missed indefinitely; inode and size because an
 * editor's safe write (temp file + rename) need not move the timestamp at all.
 *
 * ...and three pieces of state, because one token cannot carry it (D8):
 *   observed    the change token of the last successful *read*, stamped even
 *               when the parse then fails - or a malformed file is re-read
 *               and re-parsed every frame until the user fixes it.
 *   applied     the content the live document actually came from. gl-repl's
 *               own writes stamp this (glr_extedit_note_wrote), so a save can
 *               never come back at us as an inbound change.
 *   pending     a newer version seen while the gate is shut. Only the newest
 *               is retained, and the file is re-read when the gate opens
 *               rather than replayed from stale bytes.
 *   suppressed  a payload the user's own commit beat. Distinct from
 *               "applied", because the live document is NOT that payload -
 *               calling it applied would make a later cursor-only update
 *               reason about a document that was never loaded.
 *
 * DEFERRAL (D5/D7). A reload lands as a wholesale document replacement, and
 * `EditorUndoSnapshot` deliberately does not carry the in-progress input row -
 * so a reload arriving mid-typing destroys work Ctrl+Z cannot recover. While
 * the input row is dirty, or a tutorial or guided tour is running, the inbound
 * version is held pending instead.
 *
 * How the wait ends is decided by *what the document did*, not by which key
 * was pressed: if the live document is unchanged when the gate opens, the row
 * was abandoned and the pending version applies; if it moved, the user
 * committed something, and applying would destroy exactly what they typed - so
 * the payload is dismissed into `suppressed` and the local document wins until
 * the external editor saves again. Deriving it from the document rather than
 * from commit/cancel hooks means there is no ordering to get wrong and no
 * router call site to forget.
 *
 * UNDO (D4), keyed on live identity rather than applied uniformly:
 *   user scene       history captured, snapshot pushed, reload; a failure
 *                    restores the history, a success leaves one undoable
 *                    clobber. The capture is the heap-backed history, not
 *                    EditorUndoRingState - with a full ring the push has
 *                    already overwritten the oldest entry and head/count
 *                    cannot bring it back.
 *   example/transient no push at all. `editor_undo_push_snapshot()` is the
 *                    transient auto-promotion hook, so pushing here would
 *                    promote an unedited catalog scene into a user slot on
 *                    the first vim save. The file is the source of truth and
 *                    vim's own undo is the undo.
 */
#ifndef GLR_EXTEDIT_H
#define GLR_EXTEDIT_H

/* Arm or disarm the watcher. Either edge resets every token and clears any
 * pending version, so a test (or a future File-menu toggle) can restart from a
 * known state. Off by default; `--watch` turns it on. There is deliberately no
 * `GlrConfigKey` for this (D6): a config key would rewrite every example
 * golden and could travel in an `@cfg` row, letting imported content enable
 * its own watcher. */
void glr_extedit_set_enabled(int enabled);
int  glr_extedit_enabled(void);

/* The file currently being followed, or NULL when nothing is. Re-resolved
 * every poll, so this is a query, not a setting. */
const char *glr_extedit_bound_path(void);

/* One frame's worth of watching: re-resolve the binding, `stat()`, and - only
 * when something actually moved - read, hash, and reload. Called from the host
 * display callback before the controller's frame, under its own ProfSection.
 * Inert when disabled, unbound, or built for the web. */
void glr_extedit_poll(void);

/* Self-write stamp. Call after *every* successful gl-repl write of a scene
 * file, with the path written; a path that is not the bound one is ignored.
 * Stamps both tokens from the bytes now on disk, closing the window in which
 * our own save would be read back as an external change. */
void glr_extedit_note_wrote(const char *path);

/* The same stamp for the common case: gl-repl just wrote the active scene to
 * wherever `repl_active_scene_bound_path()` resolves. Save Scene and every
 * workspace save use this rather than naming their own target, so the writer
 * and the watcher cannot disagree about which file that is. */
void glr_extedit_note_saved(void);

/* Counters, for tests. The behaviors worth asserting are all negative - a
 * malformed file must not be re-read every frame, a cursor-only change must
 * not re-parse - and a counter is the only way to assert them without timing.
 */
typedef struct {
    int reads;       /* file read + hashed (change token moved) */
    int reloads;     /* documents actually replaced */
    int failures;    /* reload attempts the loader rejected */
    int deferrals;   /* times a version was parked behind a shut gate */
    int dismissals;  /* parked versions the local document outvoted */
} GlrExtEditStats;

GlrExtEditStats glr_extedit_stats(void);

#endif /* GLR_EXTEDIT_H */
