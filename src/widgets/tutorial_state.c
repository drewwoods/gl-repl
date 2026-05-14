#include "widgets/tutorial_state.h"

static TutorialRuntimeState g_tutorial_state;

static void tutorial_state_init_defaults(TutorialRuntimeState *s) {
    s->active = 0;
    s->tutorial_idx = -1;
    s->step = 0;
    s->locked_line_count = 0;
    s->fade_line_idx = -1;
    s->fade_start_t = 0.0f;
    s->fade_duration = 0.5f;
    s->expected_commit_line = -1;
    s->pending.step_idx = -1;
    s->pending.commit_line = -1;
    s->pending.doc_count_before = -1;
    for (int i = 0; i < TUTORIAL_LOCKED_LINE_MAX; i++)
        s->committed_line_for_step[i] = -1;
    s->last_result.kind = TUT_MATCH_OK;
    s->last_result.arg_index = -1;
    s->last_result.message[0] = '\0';
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor))
static void tutorial_state_module_init(void) {
    tutorial_state_init_defaults(&g_tutorial_state);
}
#endif

TutorialRuntimeState tutorial_state_view(void) {
    return g_tutorial_state;
}

TutorialRuntimeState *tutorial_state_mut(void) {
    return &g_tutorial_state;
}

void tutorial_state_reset(void) {
    tutorial_state_init_defaults(&g_tutorial_state);
}

int tutorial_active(void) {
    return g_tutorial_state.active;
}
