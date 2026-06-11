#include "subsystems/tutorial/tutorial_animation.h"

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

int tutorial_fade_front(const TutorialFadeView *fade, int line_idx, int line_len,
                        float now) {
    int safe_len;
    float step;
    float elapsed;
    int front;

    if (!fade || !fade->active || line_idx != fade->fade_line_idx)
        return -1;
    if (now >= fade->fade_start_t + fade->fade_duration)
        return -1;

    safe_len = line_len > 0 ? line_len : 1;
    step = tutorial_fade_slot_step(fade->fade_duration, safe_len);
    if (step <= 0.0f)
        return -1;

    elapsed = now - fade->fade_start_t;
    if (elapsed <= 0.0f)
        return 0;

    front = (int)(elapsed / step);
    if (front < 0)
        front = 0;
    if (front >= safe_len)
        return -1;
    return front;
}

float tutorial_fade_alpha(const TutorialFadeView *fade, int line_idx, int char_idx,
                          int line_len, float now) {
    int safe_len;
    float step;
    float elapsed;

    if (!fade || !fade->active || line_idx != fade->fade_line_idx)
        return 1.0f;
    if (now >= fade->fade_start_t + fade->fade_duration)
        return 1.0f;

    safe_len = line_len > 0 ? line_len : 1;
    if (char_idx < 0)
        char_idx = 0;
    if (char_idx >= safe_len)
        char_idx = safe_len - 1;

    step = tutorial_fade_slot_step(fade->fade_duration, safe_len);
    if (step <= 0.0f)
        return 1.0f;
    elapsed = (now - fade->fade_start_t) - (float)char_idx * step;
    return clamp01(elapsed / step);
}

float tutorial_fade_settle(const TutorialFadeView *fade, int line_idx, int char_idx,
                           int line_len, float now) {
    int safe_len;
    float step;
    float settle_duration;
    float elapsed;

    if (!fade || !fade->active || line_idx != fade->fade_line_idx)
        return 1.0f;
    if (now >= fade->fade_start_t + fade->fade_duration)
        return 1.0f;

    safe_len = line_len > 0 ? line_len : 1;
    if (char_idx < 0)
        char_idx = 0;
    if (char_idx >= safe_len)
        char_idx = safe_len - 1;

    step = tutorial_fade_slot_step(fade->fade_duration, safe_len);
    settle_duration = step * (float)TUTORIAL_FADE_SETTLE_CHARS;
    if (settle_duration <= 0.0f)
        return 1.0f;
    /* Elapsed time since this char finished its reveal slot. */
    elapsed = (now - fade->fade_start_t) - (float)(char_idx + 1) * step;
    return clamp01(elapsed / settle_duration);
}

int tutorial_fade_line_active(const TutorialFadeView *fade, int line_idx,
                              float now) {
    return fade && fade->active && line_idx == fade->fade_line_idx &&
           now < fade->fade_start_t + fade->fade_duration;
}
