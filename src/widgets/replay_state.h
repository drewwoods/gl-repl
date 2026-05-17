/*
 * replay_state.h - Replay peer subsystem storage and accessors.
 *
 * Owns the transient playback state that should not live in the REPL document
 * model: replay on/off, machine state, program counter, playback speed, source
 * line tracking, and annotation expansion mode. The main replay logic in
 * src/widgets/replay.c writes this state; controller/UI code reads it for
 * buttons, shortcuts, and per-frame snapshots.
 *
 * Snapshot/restore is separate because tests, undo-like full-world captures,
 * and frame snapshot builders need to copy replay state alongside REPL/editor/UI
 * peers. Storage lives in a file-static here rather than in ReplRuntimeState,
 * which reflects the replay-peer split introduced in Phase F / Phase J7.
 */
#ifndef REPLAY_STATE_H
#define REPLAY_STATE_H

#include "repl/state_views.h"

/* Copy or reset the full replay runtime snapshot. Used by app-wide capture /
 * restore paths and by replay reset flows. */
void                     replay_state_capture(ReplReplayRuntimeState *snapshot);
void                     replay_state_restore(const ReplReplayRuntimeState *snapshot);
void                     replay_state_reset(void);

/* Snapshot-build accessor: returns the full ReplReplayRuntimeState by
 * value. Use the narrow accessors below for individual fields;
 * replay_state_view exists primarily for the per-frame UiRenderSnapshot
 * fill in glr_ctrl_build_ui_snapshot, where the controller copies
 * the entire struct into the snapshot exactly once. */
ReplReplayRuntimeState   replay_state_view(void);

/* Mutable accessor: still required for the few writers that update
 * multiple fields atomically (config-toggle pointers, repl_replay's
 * REPLAY_STATE macro). New writers should prefer narrow handlers. */
ReplReplayRuntimeState  *replay_state_mut(void);

/* --- Narrow read accessors ---
 *
 * Single-field queries for callers that only need one replay attribute. They
 * keep most readers from depending on the full ReplReplayRuntimeState layout.
 * Phase G added these to reduce unnecessary whole-struct traffic.
 */
int    replay_active(void);          /* .active */
int    replay_machine_state(void);   /* .state — REPLAY_OFF/PLAYING/PAUSED/DONE */
int    replay_pc(void);              /* .pc — current program counter */
int    replay_mode(void);            /* .mode — REPLAY_MODE_VERTEX/POLYGON */
float  replay_speed(void);           /* .speed — playback steps/sec */
int    replay_src_line(void);        /* .src_line_idx — source line of current cmd */
int    replay_total_flat(void);      /* .total_flat_cmds — captured at start */
int    replay_expand_args(void);     /* .expand_args — annotation expansion toggle */

/* --- Handler API ---
 *
 * Small routing surface for controller/editor input code. UI hit handling calls
 * replay_handle_pin_clicked(); keyboard dispatch forwards to replay_handle_key()
 * / replay_handle_special() while replay is active. Implementations delegate to
 * the main replay state machine in replay.c.
 */

/* Toggle replay state on a Replay-pin button click: starts replay
 * when stopped, pauses when playing, resumes when paused, restarts
 * from the beginning when DONE. */
void replay_handle_pin_clicked(void);

/* Forward an ASCII keystroke to the replay state machine while
 * playback is active. Returns 1 if the key was consumed, 0 otherwise. */
int  replay_handle_key(unsigned char key);

/* Forward a GLUT special key (arrows, F-keys, etc.) to replay while
 * playback is active. Returns 1 if consumed. */
int  replay_handle_special(int key);

#endif /* REPLAY_STATE_H */
