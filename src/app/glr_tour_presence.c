/*
 * glr_tour_presence.c - see glr_tour_presence.h.
 *
 * Three phases driven by one edge-detected input (is a controlled tour
 * running?) and one counter. Everything else is arithmetic over that counter,
 * so the whole module is testable under GL stubs without a frame ever being
 * drawn.
 */
#include "app/glr_tour_presence.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static GlrTourPresencePhase g_phase = GLR_TOUR_PRESENCE_OFF;
static int   g_phase_frame = 0;   /* frames elapsed inside g_phase          */
static int   g_breath_frame = 0;  /* free-running, so the breath never jumps */
static char  g_name[GLR_TOUR_PRESENCE_NAME_CAP] = "";

static float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Cosine ease over [0,1] — the same shape the pointer script uses for glides,
 * so the border's arrival matches the cursor's motion rather than snapping. */
static float ease(float u) {
    return 0.5f - 0.5f * cosf(clamp01(u) * (float)M_PI);
}

static void enter_phase(GlrTourPresencePhase phase) {
    g_phase = phase;
    g_phase_frame = 0;
}

void glr_tour_presence_reset(void) {
    g_phase = GLR_TOUR_PRESENCE_OFF;
    g_phase_frame = 0;
    g_breath_frame = 0;
    g_name[0] = '\0';
}

/* Title-card opacity: ease in, hold, ease out, so it is gone before the tour's
 * first caption lands. Zero outside the intro. */
static float card_alpha_for(int frame) {
    float a = 1.0f;
    int left = GLR_TOUR_PRESENCE_INTRO_FRAMES - frame;
    if (frame < GLR_TOUR_PRESENCE_CARD_IN_FRAMES)
        a *= ease((float)frame / (float)GLR_TOUR_PRESENCE_CARD_IN_FRAMES);
    if (left < GLR_TOUR_PRESENCE_CARD_OUT_FRAMES)
        a *= ease((float)left / (float)GLR_TOUR_PRESENCE_CARD_OUT_FRAMES);
    return clamp01(a);
}

GlrTourPresenceView glr_tour_presence_tick(int tour_active,
                                           const char *tour_name) {
    GlrTourPresenceView v;
    float breathe;

    /* Phase transitions on the edges of `tour_active`. A tour starting while
     * one is already on screen (Tours menu during a tour) restarts the intro,
     * because the name being announced changed. */
    if (tour_active) {
        if (g_phase == GLR_TOUR_PRESENCE_OFF ||
            g_phase == GLR_TOUR_PRESENCE_OUTRO) {
            enter_phase(GLR_TOUR_PRESENCE_INTRO);
            g_breath_frame = 0;
        }
        /* Latched, not borrowed: the outro reads this after the pointer script
         * has already dropped its metadata pointers. */
        if (tour_name && strncmp(g_name, tour_name, sizeof(g_name) - 1) != 0) {
            strncpy(g_name, tour_name, sizeof(g_name) - 1);
            g_name[sizeof(g_name) - 1] = '\0';
            if (g_phase == GLR_TOUR_PRESENCE_ACTIVE)
                enter_phase(GLR_TOUR_PRESENCE_INTRO);
        }
        if (g_phase == GLR_TOUR_PRESENCE_INTRO &&
            g_phase_frame >= GLR_TOUR_PRESENCE_INTRO_FRAMES)
            enter_phase(GLR_TOUR_PRESENCE_ACTIVE);
    } else {
        if (g_phase == GLR_TOUR_PRESENCE_INTRO ||
            g_phase == GLR_TOUR_PRESENCE_ACTIVE)
            enter_phase(GLR_TOUR_PRESENCE_OUTRO);
        if (g_phase == GLR_TOUR_PRESENCE_OUTRO &&
            g_phase_frame >= GLR_TOUR_PRESENCE_OUTRO_FRAMES)
            enter_phase(GLR_TOUR_PRESENCE_OFF);
    }

    v.phase = g_phase;
    v.name = g_name;
    v.border_alpha = 0.0f;
    v.band_scale = 1.0f;
    v.card_alpha = 0.0f;

    breathe = 0.5f + 0.5f * sinf((float)g_breath_frame *
                                 (float)(2.0 * M_PI) /
                                 (float)GLR_TOUR_PRESENCE_BREATHE_FRAMES);
    v.border_alpha = GLR_TOUR_PRESENCE_ALPHA_MIN +
                     (GLR_TOUR_PRESENCE_ALPHA_MAX -
                      GLR_TOUR_PRESENCE_ALPHA_MIN) * breathe;

    switch (g_phase) {
    case GLR_TOUR_PRESENCE_INTRO:
        /* The border arrives over the card's own ease-in, so the frame and the
         * announcement read as one event rather than two. */
        v.border_alpha *= ease((float)g_phase_frame /
                               (float)GLR_TOUR_PRESENCE_CARD_IN_FRAMES);
        v.card_alpha = card_alpha_for(g_phase_frame);
        break;
    case GLR_TOUR_PRESENCE_ACTIVE:
        break;
    case GLR_TOUR_PRESENCE_OUTRO: {
        /* Collapse: the band thins to nothing as it fades, so leaving a tour
         * is something you see happen rather than a frame that is suddenly
         * missing furniture. */
        float u = clamp01((float)g_phase_frame /
                          (float)GLR_TOUR_PRESENCE_OUTRO_FRAMES);
        v.band_scale = 1.0f - u;
        v.border_alpha *= 1.0f - ease(u);
        break;
    }
    case GLR_TOUR_PRESENCE_OFF:
    default:
        v.border_alpha = 0.0f;
        break;
    }

    if (g_phase != GLR_TOUR_PRESENCE_OFF) {
        g_phase_frame++;
        g_breath_frame++;
    }
    return v;
}
