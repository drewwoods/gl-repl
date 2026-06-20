#ifndef REPLAY_INTERNAL_H
#define REPLAY_INTERNAL_H

#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_state.h"
#include "repl/core_internal.h"
#include "repl/eval.h"
#include "repl/pipeline.h"
#include "repl/core.h"
#include "repl/state_views.h"

/* Batch lifetime cap (seconds). Pairs with the replay peer's
 * fade_speed (replay_state.c, alpha/sec): See REPLAY_STATE_INITIAL
 * and ReplayRuntimeState::fade_speed.
 */
#define REPLAY_FADE_DURATION   0.50f
/* A fresh fade batch starts one ~60 Hz frame "aged" rather than at 0
 * so it fades in smoothly instead of popping at full opacity. */
#define REPLAY_FADE_INITIAL_AGE GLR_FRAME_DT_SECS
/* Replay step-rate clamp (steps/sec) and the per-keystroke multipliers
 * (0.67 ~= 1/1.5, so +/- are inverse steps). */
#define REPLAY_SPEED_MIN       0.5f
#define REPLAY_SPEED_MAX       200.0f
#define REPLAY_SPEED_STEP_UP   1.5f
#define REPLAY_SPEED_STEP_DOWN 0.67f

static inline int replay_advance_tess_depth(CmdType type, int depth) {
    switch (type) {
    case CMD_TESS_BEGIN_POLYGON: return 1;
    case CMD_TESS_BEGIN_CONTOUR: return (depth == 1) ? 2 : depth;
    case CMD_TESS_END:
        if (depth == 2) return 1;
        if (depth == 1) return 0;
        return depth;
    default: return depth;
    }
}

void replay_clear_fade_batches(void);
void replay_push_fade_batch(int old_pc, int new_pc);
void replay_clamp_fade_batches(int max_pc);

void replay_set_src_line(int src_line);

#endif /* REPLAY_INTERNAL_H */
