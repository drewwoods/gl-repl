/*
 * glr_tour_snapshot.h - whole-app baseline capture/restore for tour rewind.
 *
 * A guided tour's transport controls (Back / Done-restart, see
 * src/app/glr_pointer_script.h) rewind to the exact state the tour began in by
 * restoring one baseline and fast-replaying a prefix of events on top of it -
 * NOT by snapshotting every event. This module owns that single baseline: it
 * composes the focused per-owner captures (repl checkpoint, editor session,
 * undo history, scene catalog, glr/ui/replay/tutorial/variable-panel/
 * help-session by-value states, and the camera / view-transition / menu-bar /
 * color-picker / overlay-layout runtime snapshots) into one opaque object.
 *
 * The snapshot deliberately excludes derived state: the flat program (rebuilt
 * from the document), renderer resources, profiling histories, GL objects,
 * audio engine internals, and controller frame caches are not baseline state.
 * The flat program is left dirty by restore and rebuilds on the next frame.
 *
 * Large components (the ~10 MB scene catalog, the undo history) are separately
 * heap-allocated; the whole GlrTourSnapshot is heap-allocated too, so nothing
 * large lands on the stack. A partially-constructed snapshot is fully cleaned
 * up on allocation failure.
 *
 * Restore covers the scene catalog, glr state, REPL checkpoint, editor
 * session, undo history, camera and view transition, peer states, and
 * UI/menu/overlay state. The pointer engine releases held buttons and clears
 * in-flight glide/typing state before calling this. Then
 * glr_ctrl_after_tour_restore() re-syncs derived chrome and refreshes export
 * strings.
 */
#ifndef GLR_TOUR_SNAPSHOT_H
#define GLR_TOUR_SNAPSHOT_H

typedef struct GlrTourSnapshot GlrTourSnapshot;

/* Capture the current whole-app baseline. Returns a heap-allocated snapshot,
 * or NULL on allocation failure (the live app is left untouched and nothing
 * leaks). Destroy it with glr_tour_snapshot_destroy(). */
GlrTourSnapshot *glr_tour_snapshot_capture(void);

/* Restore owner state from `snapshot`. Leaves the flat program
 * dirty (rebuilt next frame). Returns 1 on success, 0 for a NULL snapshot.
 * The caller owns the pointer-engine reset before and the controller chrome
 * re-sync after (see the header comment). */
int glr_tour_snapshot_restore(const GlrTourSnapshot *snapshot);

/* Free a snapshot (and its heap-allocated sub-components). NULL-safe. */
void glr_tour_snapshot_destroy(GlrTourSnapshot *snapshot);

#endif /* GLR_TOUR_SNAPSHOT_H */
