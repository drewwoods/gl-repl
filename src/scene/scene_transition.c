/* scene_transition.c - see scene_transition.h. Pure; no GL/globals. */
#include "scene/scene_transition.h"

void scene_xn_init(SceneXnState *s, int theme) {
    if (!s) return;
    s->current = theme;
    s->next    = theme;
    s->phase   = SCENE_XN_STEADY;
    s->opacity = 1.0f;
}

void scene_xn_set(SceneXnState *s, int theme) {
    if (!s) return;
    s->next = theme;
    if (theme == s->current) {
        /* Returning to the current theme: reverse an in-progress
         * fade-out; otherwise nothing to do (already shown / showing). */
        if (s->phase == SCENE_XN_FADE_OUT)
            s->phase = SCENE_XN_FADE_IN;
    } else {
        /* A different theme: fade the current one out first. `current`
         * only advances at the opacity-0 crossing (skip-ephemeral). */
        s->phase = SCENE_XN_FADE_OUT;
    }
}

void scene_xn_show(SceneXnState *s, int theme) {
    if (!s) return;
    s->current = theme;
    s->next    = theme;
    s->phase   = SCENE_XN_FADE_IN;
    s->opacity = 0.0f;
}

void scene_xn_tick(SceneXnState *s, float dt, float in_secs,
                   float out_secs) {
    if (!s) return;

    if (s->phase == SCENE_XN_FADE_IN) {
        s->opacity = (in_secs > 0.0f) ? s->opacity + dt / in_secs : 1.0f;
        if (s->opacity >= 1.0f) {
            s->opacity = 1.0f;
            s->phase   = SCENE_XN_STEADY;
        }
    } else if (s->phase == SCENE_XN_FADE_OUT) {
        s->opacity = (out_secs > 0.0f) ? s->opacity - dt / out_secs : 0.0f;
        if (s->opacity <= 0.0f) {
            s->opacity = 0.0f;
            if (s->next != s->current) {
                s->current = s->next;       /* adopt the latest request */
                s->phase   = SCENE_XN_FADE_IN;
            } else {
                s->phase   = SCENE_XN_STEADY;  /* hidden, nothing pending */
            }
        }
    }
    /* SCENE_XN_STEADY: opacity is stable, nothing to do. */
}
