#include "subsystems/tutorial/tutorial_internal.h"
#include <string.h>

static float clamp01(float value) {
    if (value <= 0.0f)
        return 0.0f;
    if (value >= 1.0f)
        return 1.0f;
    return value;
}

/* Time budget per character slot. The animation fills line_len +
 * TUTORIAL_FADE_SETTLE_CHARS slots over fade_duration: each character
 * takes one slot to fade in (alpha 0 → 1, bright white), and the
 * trailing W slots pace its color settling from white back to the
 * base comment color. The last character finishes settling exactly
 * at fade_start_t + fade_duration. */
static float tutorial_fade_slot_step(float fade_duration, int line_len) {
    int safe_len = line_len > 0 ? line_len : 1;
    int total_slots = safe_len + TUTORIAL_FADE_SETTLE_CHARS;
    return fade_duration / (float)total_slots;
}

int tutorial_step_fade_front(int line_idx, int line_len, float now) {
    TutorialRuntimeState state = tutorial_state_view();
    int safe_len;
    float step;
    float elapsed;
    int front;

    if (!state.active || line_idx != state.fade_line_idx)
        return -1;
    if (now >= state.fade_start_t + state.fade_duration)
        return -1;

    safe_len = line_len > 0 ? line_len : 1;
    step = tutorial_fade_slot_step(state.fade_duration, safe_len);
    if (step <= 0.0f)
        return -1;

    elapsed = now - state.fade_start_t;
    if (elapsed <= 0.0f)
        return 0;

    front = (int)(elapsed / step);
    if (front < 0)
        front = 0;
    if (front >= safe_len)
        return -1;
    return front;
}

float tutorial_step_fade_alpha(int line_idx, int char_idx, int line_len, float now) {
    TutorialRuntimeState state = tutorial_state_view();
    int safe_len;
    float step;
    float elapsed;

    if (!state.active || line_idx != state.fade_line_idx)
        return 1.0f;
    if (now >= state.fade_start_t + state.fade_duration)
        return 1.0f;

    safe_len = line_len > 0 ? line_len : 1;
    if (char_idx < 0)
        char_idx = 0;
    if (char_idx >= safe_len)
        char_idx = safe_len - 1;

    step = tutorial_fade_slot_step(state.fade_duration, safe_len);
    if (step <= 0.0f)
        return 1.0f;
    elapsed = (now - state.fade_start_t) - (float)char_idx * step;
    return clamp01(elapsed / step);
}

float tutorial_step_fade_settle(int line_idx, int char_idx, int line_len, float now) {
    TutorialRuntimeState state = tutorial_state_view();
    int safe_len;
    float step;
    float settle_duration;
    float elapsed;

    if (!state.active || line_idx != state.fade_line_idx)
        return 1.0f;
    if (now >= state.fade_start_t + state.fade_duration)
        return 1.0f;

    safe_len = line_len > 0 ? line_len : 1;
    if (char_idx < 0)
        char_idx = 0;
    if (char_idx >= safe_len)
        char_idx = safe_len - 1;

    step = tutorial_fade_slot_step(state.fade_duration, safe_len);
    settle_duration = step * (float)TUTORIAL_FADE_SETTLE_CHARS;
    if (settle_duration <= 0.0f)
        return 1.0f;
    /* Elapsed time since this char finished its reveal slot. */
    elapsed = (now - state.fade_start_t) - (float)(char_idx + 1) * step;
    return clamp01(elapsed / settle_duration);
}

int tutorial_line_is_fading(int line_idx, float now) {
    TutorialRuntimeState state = tutorial_state_view();

    return state.active && line_idx == state.fade_line_idx &&
           now < state.fade_start_t + state.fade_duration;
}
