/*
 * replay_state.c - Replay peer subsystem ownership.
 */
#include "subsystems/replay/replay_state.h"
#include "subsystems/replay/replay.h"

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

int replay_machine_state(void) {
    return g_replay_state.state;
}

int replay_pc(void) {
    return g_replay_state.pc;
}

int replay_mode(void) {
    return g_replay_state.mode;
}

float replay_speed(void) {
    return g_replay_state.speed;
}

int replay_src_line(void) {
    return g_replay_state.src_line_idx;
}

int replay_total_flat(void) {
    return g_replay_state.total_flat_cmds;
}

int replay_expand_args(void) {
    return g_replay_state.expand_args;
}

void replay_handle_pin_clicked(void) {
    replay_toggle_play_pause();
}

int replay_handle_key(unsigned char key) {
    return replay_handle_key_impl(key);
}

int replay_handle_special(int key) {
    return replay_handle_special_key_impl(key);
}
