/* render3d_transition.c - see render3d_transition.h. Pure; no GL/globals.
 *
 * The machine is a clock: it tracks `elapsed` seconds into the current fade
 * phase and reads opacity back from the bound Render3dXnReveal. It never names a
 * duration - the reveal owns that - so the caller only feeds dt. */
#include "render3d_transition.h"

float render3d_xn_opacity(const Render3dXnState *s) {
    if (!s || s->phase == RENDER3D_XN_STEADY || !s->reveal)
        return 1.0f;                       /* STEADY (or unbound) == fully shown */
    return s->reveal->opacity(s->current, s->phase, s->elapsed);
}

/* Switch to `new_phase` while keeping the on-screen opacity continuous: read
 * the opacity the current phase shows now, then seed the clock so `new_phase`
 * starts at that same opacity. Used by the reversal / direction-flip paths. */
static void render3d_xn_reseat(Render3dXnState *s, Render3dXnPhase new_phase) {
    float op = render3d_xn_opacity(s);
    s->phase = new_phase;
    s->elapsed = s->reveal ? s->reveal->elapsed_at(s->current, new_phase, op)
                           : 0.0f;
}

void render3d_xn_init(Render3dXnState *s, int theme, const Render3dXnReveal *reveal) {
    if (!s) return;
    s->current = theme;
    s->next    = theme;
    s->phase   = RENDER3D_XN_STEADY;
    s->elapsed = 0.0f;
    s->reveal  = reveal;
}

void render3d_xn_set(Render3dXnState *s, int theme) {
    if (!s) return;
    s->next = theme;
    if (theme == s->current) {
        /* Returning to the current theme: reverse an in-progress fade-out
         * (opacity-continuous). Otherwise nothing to do (already shown /
         * showing). */
        if (s->phase == RENDER3D_XN_FADE_OUT)
            render3d_xn_reseat(s, RENDER3D_XN_FADE_IN);
    } else {
        /* A different theme: fade the current one out first. `current` only
         * advances at the opacity-0 crossing (skip-ephemeral). */
        if (s->phase == RENDER3D_XN_FADE_IN)
            render3d_xn_reseat(s, RENDER3D_XN_FADE_OUT);   /* flip mid-fade, no snap */
        else if (s->phase == RENDER3D_XN_STEADY) {
            s->phase   = RENDER3D_XN_FADE_OUT;          /* from opacity 1 */
            s->elapsed = 0.0f;
        }
        /* Already FADE_OUT: keep the clock running; only `next` changed. */
    }
}

void render3d_xn_show(Render3dXnState *s, int theme) {
    if (!s) return;
    s->current = theme;
    s->next    = theme;
    s->phase   = RENDER3D_XN_FADE_IN;
    s->elapsed = 0.0f;
}

void render3d_xn_tick(Render3dXnState *s, float dt) {
    if (!s || s->phase == RENDER3D_XN_STEADY) return;

    s->elapsed += dt;
    float op = render3d_xn_opacity(s);

    if (s->phase == RENDER3D_XN_FADE_IN) {
        if (op >= 1.0f) {
            s->phase   = RENDER3D_XN_STEADY;
            s->elapsed = 0.0f;
        }
    } else { /* RENDER3D_XN_FADE_OUT */
        if (op <= 0.0f) {
            s->elapsed = 0.0f;
            if (s->next != s->current) {
                s->current = s->next;          /* adopt the latest request */
                s->phase   = RENDER3D_XN_FADE_IN;
            } else {
                s->phase   = RENDER3D_XN_STEADY;  /* hidden, nothing pending */
            }
        }
    }
}
