#include "widgets/tutorial_state.h"

#define TUTORIAL_STATE_INITIAL                        \
    {                                                \
        .active = 0,                                 \
        .tutorial_idx = -1,                          \
        .step = 0,                                   \
        .locked_line_count = 0,                      \
        .fade_line_idx = -1,                         \
        .fade_start_t = 0.0f,                        \
        .fade_duration = 0.5f,                       \
        .last_result = {                             \
            .kind = TUT_MATCH_OK,                    \
            .arg_index = -1,                         \
            .message = "",                          \
        },                                           \
    }

static TutorialRuntimeState       g_tutorial_state = TUTORIAL_STATE_INITIAL;
static const TutorialRuntimeState g_tutorial_state_defaults = TUTORIAL_STATE_INITIAL;

TutorialRuntimeState tutorial_state_view(void) {
    return g_tutorial_state;
}

TutorialRuntimeState *tutorial_state_mut(void) {
    return &g_tutorial_state;
}

void tutorial_state_reset(void) {
    g_tutorial_state = g_tutorial_state_defaults;
}

int tutorial_active(void) {
    return g_tutorial_state.active;
}
