#ifndef REPLAY_RENDER_H
#define REPLAY_RENDER_H

#include "subsystems/replay/replay_state.h"  /* ReplayFadePlan */

void replay_render_fade_batches(const ReplayFadePlan *plan);
void replay_render_tess_preview(const ReplayFadePlan *plan);
void replay_render_post_fill(void *user_data);

#endif /* REPLAY_RENDER_H */
