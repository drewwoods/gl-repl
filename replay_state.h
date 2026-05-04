/*
 * replay_state.h - Replay peer subsystem ownership.
 *
 * Phase F commit 33 entry. The replay system is a peer alongside
 * editor / variable_panel / scene rather than a slice of ReplState.
 * It owns the program counter, mode (OFF/PLAYING/PAUSED/DONE),
 * speed, fade-batch ring, and other transient playback state, with
 * `imrepl_ctrl` routing UiHits (UI_HIT_REPLAY_BUTTON) to its handler
 * functions in commit 34.
 *
 * Storage migration: ReplReplayRuntimeState was a member of
 * ReplRuntimeState. Its bytes move into a static here. The legacy
 * `repl_state_replay` / `_mut` / `_reset` accessors are kept as thin
 * forwarders so existing call sites (ui_snapshot build, executor,
 * editor key paths, tests) keep compiling. They will be removed once
 * Phase F closes.
 *
 * Snapshot/restore: tests and undo capture this state via
 * `replay_state_capture` / `_restore` alongside the editor / ui /
 * runtime captures. The replay-mode behavior in replay.c is
 * unchanged — it still drives the state machine, just now writing
 * through `replay_state_mut()` rather than `repl_state_replay_mut()`.
 */
#ifndef REPLAY_STATE_H
#define REPLAY_STATE_H

#include "replay.h"
#include "repl_state_views.h"

void                     replay_state_capture(ReplReplayRuntimeState *snapshot);
void                     replay_state_restore(const ReplReplayRuntimeState *snapshot);
void                     replay_state_reset(void);

/* Snapshot-build accessor: returns the full ReplReplayRuntimeState by
 * value. Use the narrow accessors below for individual fields;
 * replay_state_view exists primarily for the per-frame UiRenderSnapshot
 * fill in imrepl_ctrl_build_ui_snapshot, where the controller copies
 * the entire struct into the snapshot exactly once. */
ReplReplayRuntimeState   replay_state_view(void);

/* Mutable accessor: still required for the few writers that update
 * multiple fields atomically (config-toggle pointers, repl_replay's
 * REPLAY_STATE macro). New writers should prefer narrow handlers. */
ReplReplayRuntimeState  *replay_state_mut(void);

/* --- Narrow read accessors ---
 *
 * Phase G commit 34c. Single-field queries that let callers avoid
 * pulling the entire struct just to read one field. Reduces public
 * exposure of ReplReplayRuntimeState's internal layout.
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
 * Phase F commit 34 entry points. `imrepl_ctrl` routes
 * UI_HIT_REPLAY_BUTTON hits through replay_handle_pin_clicked; the
 * editor's key dispatcher forwards keystrokes via replay_handle_key /
 * replay_handle_special. Implementations wrap the existing
 * repl_replay_* surface in replay.c.
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
