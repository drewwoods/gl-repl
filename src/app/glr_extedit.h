/*
 * glr_extedit.h - External-editor sync (`--watch`).
 *
 * Lets vim / VS Code / anything be a peer author of the live scene: the
 * external editor saves the bound file, gl-repl re-reads it atomically, and
 * the scene updates without leaving the editor. See
 * docs/plans/active/BYOE.md; the decision letters below refer to its
 * "Decisions" section.
 *
 * Inbound is one `stat()` per frame. Outbound is Ctrl+S *and* an automatic
 * sync: a watched file that gl-repl silently stops matching is worse than no
 * watch at all, because the editor's next save overwrites whatever gl-repl
 * changed and nothing said so. See "OUTBOUND SYNC" below for what counts as a
 * change and the states that hold the write back.
 *
 * The decisions behind all of this are D1-D8 in the plan; what follows is the
 * short form of the ones a caller has to know, plus the traps that are not
 * obvious from the code.
 *
 * BOUND TO (D1) `repl_active_scene_bound_path()` - the same file Ctrl+S
 * writes, never a path of its own. Watching a file gl-repl would not write, or
 * writing one it does not watch, breaks the round trip and leaves the
 * self-write stamp with nothing to suppress. A scene switch therefore re-binds
 * for free, and a built-in example - which has no file - unbinds.
 * `glr_extedit_bind_path()` seeds it outright for `--watch FILE`.
 *
 * TWO-LEVEL GATE (D8) One `stat()` per frame; read + hash only when the change
 * token (nanosecond mtime + inode + size, not `time_t` seconds) moves; parse
 * only when the content hash does.
 *
 * FOUR TOKENS, and the one that is easy to get wrong:
 *   observed    stamped after every read, *before* the parse can fail - or a
 *               malformed file is re-read every frame until it is fixed.
 *   applied     what the live document came from. Stamped from the bytes the
 *               reload actually read, NOT from the parked token: a deferred
 *               version is re-read when the gate opens, so the two can differ,
 *               and stamping the stale one makes the watcher deaf to a later
 *               genuine save of exactly those bytes.
 *   pending     a newer version seen while the gate was shut. Cleared when the
 *               file moves back to `applied` or `suppressed` content, because
 *               it then describes a payload the file no longer holds.
 *   suppressed  a payload the user's own commit beat. Deliberately not
 *               `applied`: the live document is not that payload.
 *
 * DEFERRAL (D5/D7) A reload is a wholesale document replacement, and
 * `EditorUndoSnapshot` excludes the input row - so one landing mid-typing
 * destroys work Ctrl+Z cannot recover. Held while the input row is dirty or a
 * lesson is running. How the wait ends is read off the *document*, not off
 * which key was pressed: unchanged means the row was abandoned, so the pending
 * version applies; changed means the user committed, so it is dismissed.
 *
 * UNDO (D4) keyed on live identity. A user scene: history captured, snapshot
 * pushed, one undoable clobber; the capture is the heap-backed history, not
 * `EditorUndoRingState`, because on a full ring the push has already evicted
 * the oldest entry and head/count cannot bring it back. An example or
 * transient: no push at all - `editor_undo_push_snapshot()` is the transient
 * auto-promotion hook, so pushing would promote an unedited catalog scene into
 * a user slot on the first vim save.
 *
 * OUTBOUND SYNC. gl-repl is a peer author too, and not everything it changes
 * was typed: a variable-panel drag rewrites the declaration row, the color
 * picker rewrites a `glColor` row. Whenever the live document moves away from
 * what the outside world last handed it - a bind, an import, a sidecar update,
 * a save - the watched file is written back out and re-stamped, so the round
 * trip stays closed in both directions.
 *
 * The trigger is the document *text* plus the scene `@cfg` subset, which is
 * exactly what the writer derives from live state. Neither half is a shortcut:
 * a drag applies its value live on every pointer event but rewrites the source
 * once, on release, so one gesture costs one write; and a scene setting is
 * part of the file even though it moves no row, so a toggle that only ever
 * rode out with the next unrelated edit now goes out on its own. Live state
 * that is neither - the camera above all, `t`, a scratch cell with no
 * declaration, and the session-inspection settings (profilers, viz, replay) -
 * is deliberately not a reason to rewrite the user's file, and neither is an
 * empty document.
 *
 * Four states hold the write back, all of them "the document is not what this
 * file should hold": a pinned binding (a lesson owns the document - this also
 * covers a lesson running, which is why the inbound gate is not consulted), a
 * pending inbound version (inbound wins - writing would stamp over an external
 * save that has not landed yet), a live WIP session (the editor's unsaved
 * buffer is the truth then, and a write would drop the parked row, which is
 * not in the document), and a parked row still in play from a stage-2 file
 * reload (same drop, without a sidecar to hold it). Each of those resolves,
 * and the sync fires on the poll after it does. The last-agreed fingerprint is
 * remembered per path, so a scene-switch round trip cannot treat unsaved slot
 * edits as already on disk.
 *
 * A half-typed row is NOT one of them, and used to be. D5's gate is the
 * *inbound* question - a reload would destroy typing undo cannot recover - and
 * outbound it is far too wide: a row being typed is not in the document, so
 * writing the committed document without it takes nothing from a file that
 * never had it, while holding on it parked every later local change behind a
 * line the user may never finish. The one row whose absence the file would
 * feel is the parked one, because that row came *from* the file - and "still
 * in play" is its own predicate (the caret is still on it and the input is
 * still uncommitted), so evolving it by one keystroke keeps holding rather
 * than releasing.
 *
 * ONE INCOMPLETE FINAL ROW (`.glr` only). A file may end with a half-typed
 * command; it lands in the live input row, where the user keeps typing it and
 * gets the edit-guide overlays and autocomplete for free.
 *
 * **This is not a loader concession.** The loader stays zero-tolerance; the
 * *controller* removes the designated physical row before the import, and it
 * has to be that way round: `import.c` accumulates physical lines into a
 * logical statement while brackets are open, so an unfinished
 * `glVertex3f(1,` absorbs the following lines and an atomic loader could only
 * ever report a joined logical statement, never a physical row.
 *
 * The row is recognized by lexical evidence of incompleteness on the last row
 * with *code* - bracket depth not back to zero, or a last code character that
 * is not a statement terminator - using the importer's own scanner
 * (repl/line_scan.h) rather than a lookalike. "Last row with code" is why an
 * ordinary trailing `// note` stays in the document instead of being parked.
 * Only a single trailing row qualifies; see find_incomplete_final_row for why
 * the multi-row case is refused rather than stripped.
 *
 * `.glr` only, because the heuristic is meaningless on exported C: the
 * incomplete authored command sits inside `display()` and is followed by
 * generated wrapper rows, so it is never the final non-empty physical row.
 *
 * Two consequences worth stating. The parked row is deliberately *not* in the
 * document, so an outbound write drops it. And if removing it unbalances what
 * remains - a block delimiter, say - the ATOMIC import fails and the live
 * document is left alone, which is the correct outcome and falls out of the
 * design rather than being special-cased.
 *
 * THE LIVE WIP SIDECAR (stage 2.5) turns "on save" into "as you type". The
 * editor publishes its unsaved buffer to `<file>.wip` on every change and
 * cursor move, and the watcher follows that instead of waiting for `:w`. The
 * scene file itself is still what Ctrl+S writes and still what the binding
 * names; the sidecar is a shadow of the buffer, never a scene of its own.
 *
 * Four things about it that are not obvious from the code:
 *
 *   THE CURSOR ROW IS NAMED, NOT GUESSED. The sidecar's last line is
 *   `// @cursor <row> <col>` (1-based row, 1-based byte column, vim's
 *   `line('.')` / `col('.')`). That lifts stage 2's restriction to a single
 *   *trailing* incomplete row: the half-typed row can be anywhere, because the
 *   editor says which one it is. The row is still only parked when it is
 *   lexically incomplete - parking a finished command would take its geometry
 *   out of the scene while the user looks straight at it.
 *
 *   A CURSOR MOVE MUST NOT RE-IMPORT. The content hash is taken over the
 *   payload *without* the `@cursor` line, so scrolling around in vim costs one
 *   stat and one hash. This is a correctness requirement, not a saving: a
 *   content update costs milliseconds and stage 2.5 pays it per keystroke
 *   (bench/bench_extedit.c). Following the cursor then needs a physical ->
 *   document row map, because the row it names may be one no import ever
 *   resolved; the reader caches one per content import (repl/scene_load.h).
 *
 *   UNDO IS PER SESSION, NOT PER KEYSTROKE. One snapshot when a WIP session
 *   starts, then nothing - 32 slots of vim keystrokes would be worthless, and
 *   vim owns undo of its own typing. Ctrl+Z therefore exits WIP back to the
 *   last gl-repl document. It is detected the same way an abandoned input row
 *   is: the document moved and this module did not move it. The D4 identity
 *   split still applies, so an unedited example gets no snapshot and is not
 *   promoted by someone else's typing.
 *
 *   A LOCAL COMMIT AND CTRL+Z ARE NOT THE SAME EXIT. Both notice that the
 *   document moved without this module moving it, and both remember the last
 *   applied sidecar payload as a suppressed hash so a later CursorMoved of
 *   those bytes cannot look like new content and overwrite the local
 *   document (D5: ignore content *and* cursor until the payload hash
 *   changes). Ctrl+Z also drops the publication that first observed the
 *   undo - the sidecar may have been written before or after the key - and
 *   then follows the same suppression. A commit falls through: same payload
 *   is ignored, a genuinely new one is followed immediately.
 *
 *   THE SIDECAR IS NOT A SCENE. Found at bind time it means the editor died
 *   holding unsaved work, and it is offered for recovery rather than applied -
 *   accepting is a keystroke, declining leaves the file alone and stops
 *   following it until the editor touches it again. When it *disappears*
 *   mid-session (`:q`), what happens splits on identity for the same reason
 *   D4 does: a user scene keeps the text as unsaved local work, and a
 *   transient or unedited example goes back to the file on disk rather than
 *   silently becoming a scene made of discarded editor text.
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

/* Seed the binding with a path the caller names outright, rather than letting
 * it be resolved from the active scene. `--watch FILE` uses this, and needs
 * to: a `--tutorial` / `--tour` on the same command line has already parked
 * the document in a slot-less transient by the time the watcher arms, so there
 * is no active scene to resolve a path from - and the binding would never
 * form. No-op when the watcher is disabled. Afterwards the binding follows
 * scene switches as usual. */
void glr_extedit_bind_path(const char *path);

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
    int reads;        /* file read + hashed (change token moved) */
    int reloads;      /* documents actually replaced */
    int failures;     /* reload attempts the loader rejected */
    int deferrals;    /* times a version was parked behind a shut gate */
    int dismissals;   /* parked versions the local document outvoted */
    int parked_rows;  /* reloads that put an incomplete row in the input */
    int writes;       /* local edits synced back out to the watched file */
    int write_failures; /* sync writes the exporter refused */
    int wip_updates;  /* sidecar content updates that replaced the document */
    int cursor_moves; /* sidecar updates that moved only the caret. The
                       * assertion the fast path needs: this rising while
                       * `reloads` does not is what "a cursor move costs no
                       * import" means, and it is not observable by timing. */
} GlrExtEditStats;

GlrExtEditStats glr_extedit_stats(void);

#endif /* GLR_EXTEDIT_H */
