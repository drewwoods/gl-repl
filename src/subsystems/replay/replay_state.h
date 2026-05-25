/*
 * replay_state.h - Replay peer subsystem storage and accessors.
 *
 * Owns the transient playback state that should not live in the REPL document
 * model: replay on/off, machine state, program counter, playback speed, source
 * line tracking, and annotation expansion mode. The main replay logic in
 * src/subsystems/replay/replay.c writes this state; controller/UI code reads it for
 * buttons, shortcuts, and per-frame snapshots.
 *
 * Snapshot/restore is separate because tests, undo-like full-world captures,
 * and frame snapshot builders need to copy replay state alongside REPL/editor/UI
 * peers. Storage lives in a file-static here rather than in ReplRuntimeState,
 * reflecting the replay peer split that moved replay out of the core REPL
 * runtime bundle.
 */
#ifndef REPLAY_STATE_H
#define REPLAY_STATE_H

#include "repl/state_views.h"

/* Copy or reset the full replay runtime snapshot.
 * Note: replay_state_capture/restore is exclusively a test/verification contract;
 * production code does not capture/restore replay state. */
void                     replay_state_capture(ReplReplayRuntimeState *snapshot);
void                     replay_state_restore(const ReplReplayRuntimeState *snapshot);
void                     replay_state_reset(void);

/* Snapshot-build accessor: returns the full ReplReplayRuntimeState by value.
 * Use the narrow accessors below for individual fields; `replay_state_view()` is
 * mainly for per-frame snapshot assembly and the few callers that genuinely need
 * the whole struct at once. */
ReplReplayRuntimeState   replay_state_view(void);

/* Mutable accessor for the small set of writers that update multiple replay
 * fields together. Most readers should stay on the by-value or narrow accessors. */
ReplReplayRuntimeState  *replay_state_mut(void);

/* --- Narrow read accessors ---
 *
 * Single-field queries for callers that only need one replay attribute. They
 * keep most readers from depending on the full ReplReplayRuntimeState layout.
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



#endif /* REPLAY_STATE_H */
