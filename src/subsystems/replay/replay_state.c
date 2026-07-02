/*
 * replay_state.c - Replay peer subsystem ownership.
 */
#include "subsystems/replay/replay_state.h"
#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_internal.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define REPLAY_STATE_INITIAL                                    \
    {                                                           \
        .active           = 0,                                  \
        .state            = REPLAY_OFF,                         \
        .pc               = 0,                                  \
        .mode             = REPLAY_MODE_VERTEX,                 \
        .speed            = 4.0f,                               \
        .accum            = 0.0f,                               \
        .fade_speed       = 1 / MAX(0.01, REPLAY_FADE_DURATION),\
        .src_line_idx     = -1,                                 \
        .total_flat_cmds  = 0,                                  \
        .step_begin       = 0,                                  \
        .expand_args      = 1,                                  \
        .normal_display   = REPLAY_NORMAL_DISPLAY_OFF,          \
        .saved_t_playing  = 1,                                  \
        .last_src_line    = -1,                                 \
        .fade_batch_count = 0,                                  \
    }

static ReplayRuntimeState       g_replay_runtime_state = REPLAY_STATE_INITIAL;
static const ReplayRuntimeState g_replay_state_defaults = REPLAY_STATE_INITIAL;

/* Exclusively a test/verification contract; production code does not capture replay state. */
void replay_state_capture(ReplayRuntimeState *snapshot) {
    if (!snapshot) return;
    *snapshot = g_replay_runtime_state;
}

/* Exclusively a test/verification contract; production code does not restore replay state. */
void replay_state_restore(const ReplayRuntimeState *snapshot) {
    if (!snapshot) return;
    g_replay_runtime_state = *snapshot;
}

void replay_state_reset(void) {
    g_replay_runtime_state = g_replay_state_defaults;
}

ReplayRuntimeState replay_state_view(void) {
    return g_replay_runtime_state;
}

const ReplayRuntimeState *replay_state_const(void) {
    return &g_replay_runtime_state;
}

ReplayRuntimeState *replay_state_mut(void) {
    return &g_replay_runtime_state;
}

int replay_active(void) {
    return g_replay_runtime_state.active;
}

int replay_machine_state(void) {
    return g_replay_runtime_state.state;
}

int replay_pc(void) {
    return g_replay_runtime_state.pc;
}

int replay_mode(void) {
    return g_replay_runtime_state.mode;
}

int replay_normal_display(void) {
    return g_replay_runtime_state.normal_display;
}

int replay_src_line(void) {
    return g_replay_runtime_state.src_line_idx;
}

int replay_total_flat(void) {
    if (!g_replay_runtime_state.active)
        return 0;
    return g_replay_runtime_state.total_flat_cmds;
}

void replay_handle_pin_clicked(void) {
    replay_toggle_play_pause();
}
