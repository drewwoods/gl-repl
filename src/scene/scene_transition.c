/* scene_transition.c - see scene_transition.h. Pure; no GL/globals.
 *
 * The machine is a clock: it tracks `elapsed` seconds into the current fade
 * phase and reads opacity back from the bound SceneXnReveal. It never names a
 * duration — the reveal owns that — so the controller only feeds dt. */
#include "scene_transition.h"

float scene_xn_opacity(const SceneXnState *s) {
    if (!s || s->phase == SCENE_XN_STEADY || !s->reveal)
        return 1.0f;                       /* STEADY (or unbound) == fully shown */
    return s->reveal->opacity(s->current, s->phase, s->elapsed);
}

/* Switch to `new_phase` while keeping the on-screen opacity continuous: read
 * the opacity the current phase shows now, then seed the clock so `new_phase`
 * starts at that same opacity. Used by the reversal / direction-flip paths. */
static void scene_xn_reseat(SceneXnState *s, SceneXnPhase new_phase) {
    float op = scene_xn_opacity(s);
    s->phase = new_phase;
    s->elapsed = s->reveal ? s->reveal->elapsed_at(s->current, new_phase, op)
                           : 0.0f;
}

void scene_xn_init(SceneXnState *s, int theme, const SceneXnReveal *reveal) {
    if (!s) return;
    s->current = theme;
    s->next    = theme;
    s->phase   = SCENE_XN_STEADY;
    s->elapsed = 0.0f;
    s->reveal  = reveal;
}

void scene_xn_set(SceneXnState *s, int theme) {
    if (!s) return;
    s->next = theme;
    if (theme == s->current) {
        /* Returning to the current theme: reverse an in-progress fade-out
         * (opacity-continuous). Otherwise nothing to do (already shown /
         * showing). */
        if (s->phase == SCENE_XN_FADE_OUT)
            scene_xn_reseat(s, SCENE_XN_FADE_IN);
    } else {
        /* A different theme: fade the current one out first. `current` only
         * advances at the opacity-0 crossing (skip-ephemeral). */
        if (s->phase == SCENE_XN_FADE_IN)
            scene_xn_reseat(s, SCENE_XN_FADE_OUT);   /* flip mid-fade, no snap */
        else if (s->phase == SCENE_XN_STEADY) {
            s->phase   = SCENE_XN_FADE_OUT;          /* from opacity 1 */
            s->elapsed = 0.0f;
        }
        /* Already FADE_OUT: keep the clock running; only `next` changed. */
    }
}

void scene_xn_show(SceneXnState *s, int theme) {
    if (!s) return;
    s->current = theme;
    s->next    = theme;
    s->phase   = SCENE_XN_FADE_IN;
    s->elapsed = 0.0f;
}

void scene_xn_tick(SceneXnState *s, float dt) {
    if (!s || s->phase == SCENE_XN_STEADY) return;

    s->elapsed += dt;
    float op = scene_xn_opacity(s);

    if (s->phase == SCENE_XN_FADE_IN) {
        if (op >= 1.0f) {
            s->phase   = SCENE_XN_STEADY;
            s->elapsed = 0.0f;
        }
    } else { /* SCENE_XN_FADE_OUT */
        if (op <= 0.0f) {
            s->elapsed = 0.0f;
            if (s->next != s->current) {
                s->current = s->next;          /* adopt the latest request */
                s->phase   = SCENE_XN_FADE_IN;
            } else {
                s->phase   = SCENE_XN_STEADY;  /* hidden, nothing pending */
            }
        }
    }
}
