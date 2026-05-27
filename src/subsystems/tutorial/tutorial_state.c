#include "subsystems/tutorial/tutorial_state.h"

static TutorialRuntimeState g_tutorial_state;

static void tutorial_state_init_defaults(TutorialRuntimeState *s) {
    s->active = 0;
    s->tutorial_idx = -1;
    s->step = 0;
    s->locked_line_count = 0;
    s->fade_line_idx = -1;
    s->fade_start_t = 0.0f;
    /* 0.0f means "no fade pending"; the real value is computed per-line
     * in tutorial_emit_instruction_comment from the comment's character
     * length and TUTORIAL_FADE_CHARS_PER_SEC. */
    s->fade_duration = 0.0f;
    s->expected_commit_line = -1;
    s->pending.step_idx = -1;
    s->pending.commit_line = -1;
    s->pending.doc_count_before = -1;
    /* Defensive initialization: step instruction comment line numbers are
     * only meaningful when the tutorial is active. The array is fully
     * re-initialized in tutorial_start() before any step is evaluated or read. */
    for (int i = 0; i < TUTORIAL_MAX_STEPS; i++)
        s->instruction_line_for_step[i] = -1;
    s->in_enter_step = 0;
    repl_config_bag_clear(&s->baseline_bag);
    s->baseline_valid = 0;
}

void tutorial_state_init_explicit(void) {
    tutorial_state_init_defaults(&g_tutorial_state);
}

TutorialRuntimeState tutorial_state_view(void) {
    return g_tutorial_state;
}

TutorialRuntimeState *tutorial_state_mut(void) {
    return &g_tutorial_state;
}

void tutorial_state_reset(void) {
    tutorial_state_init_defaults(&g_tutorial_state);
}

void tutorial_state_reset_except_baseline(void) {
    ReplConfigBag bag = g_tutorial_state.baseline_bag;
    int valid = g_tutorial_state.baseline_valid;
    tutorial_state_init_defaults(&g_tutorial_state);
    g_tutorial_state.baseline_bag = bag;
    g_tutorial_state.baseline_valid = valid;
}

int tutorial_active(void) {
    return g_tutorial_state.active;
}

void tutorial_state_capture(TutorialRuntimeState *snapshot) {
    if (!snapshot) return;
    *snapshot = g_tutorial_state;
}

void tutorial_state_restore(const TutorialRuntimeState *snapshot) {
    if (!snapshot) return;
    g_tutorial_state = *snapshot;
}
