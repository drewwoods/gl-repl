/*
 * replay_state.c - Replay peer subsystem ownership.
 */
#include "replay_state.h"
#include "repl_replay.h"
#include "sample.h"

#define REPLAY_STATE_INITIAL                              \
    {                                                     \
        .active          = 0,                             \
        .state           = REPLAY_OFF,                    \
        .pc              = 0,                             \
        .mode            = REPLAY_MODE_VERTEX,            \
        .speed           = 4.0f,                          \
        .accum           = 0.0f,                          \
        .fade_speed      = 2.0f,                          \
        .src_line_idx    = -1,                            \
        .total_flat_cmds = 0,                             \
        .expand_args     = 1,                             \
    }

static ReplReplayRuntimeState       g_replay_state = REPLAY_STATE_INITIAL;
static const ReplReplayRuntimeState g_replay_state_defaults = REPLAY_STATE_INITIAL;

void replay_state_capture(ReplReplayRuntimeState *snapshot) {
    if (!snapshot) return;
    *snapshot = g_replay_state;
}

void replay_state_restore(const ReplReplayRuntimeState *snapshot) {
    if (!snapshot) return;
    g_replay_state = *snapshot;
}

void replay_state_reset(void) {
    g_replay_state = g_replay_state_defaults;
}

ReplReplayRuntimeState replay_state_view(void) {
    return g_replay_state;
}

ReplReplayRuntimeState *replay_state_mut(void) {
    return &g_replay_state;
}

int replay_active(void) {
    return g_replay_state.active;
}

void replay_handle_pin_clicked(void) {
    repl_replay_toggle_play_pause();
}

int replay_handle_key(unsigned char key) {
    return repl_replay_handle_key(key);
}

int replay_handle_special(int key) {
    return repl_replay_handle_special_key(key);
}
