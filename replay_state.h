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
 * runtime captures. The replay-mode behavior in repl_replay.c is
 * unchanged — it still drives the state machine, just now writing
 * through `replay_state_mut()` rather than `repl_state_replay_mut()`.
 */
#ifndef REPLAY_STATE_H
#define REPLAY_STATE_H

#include "repl_state_views.h"

void                     replay_state_capture(ReplReplayRuntimeState *snapshot);
void                     replay_state_restore(const ReplReplayRuntimeState *snapshot);
void                     replay_state_reset(void);

ReplReplayRuntimeState   replay_state_view(void);
ReplReplayRuntimeState  *replay_state_mut(void);

#endif /* REPLAY_STATE_H */
