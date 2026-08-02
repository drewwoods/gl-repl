/*
 * glr_tour_presence.h - ambient "a guided tour is running" state machine.
 *
 * A controlled tour used to announce itself with nothing but a 26px compact
 * strip at the top of the scene, which is easy to miss in both directions: at
 * the start you can watch a still frame and conclude nothing happened, and
 * mid-tour you can forget you are in a mode and quit it by reflex. This owns
 * the presence signal that fixes both - an entry title card, a persistent
 * breathing border, and an exit collapse - as pure animation state; the
 * drawing lives in ui/subsystems/tour_presence.c.
 *
 * App band rather than UI band because the state has a lifetime the UI cannot
 * express: the outro must OUTLIVE the tour. glr_pointer_script.c reports
 * active == 0 the instant a tour stops and drops its metadata, so a renderer
 * driven straight off the playback view can only ever cut to black. This
 * module latches the name into its own storage and keeps ticking afterward.
 *
 * The clock is rendered frames, matching the tour overlay's own animation
 * timings, and it never freezes: a paused tour is still a tour you are inside,
 * so the border keeps breathing while the caption fades hold still.
 */
#ifndef GLR_TOUR_PRESENCE_H
#define GLR_TOUR_PRESENCE_H

/* Longest tour name the title card retains (copied, never borrowed - see the
 * lifetime note above). Truncation is the renderer's problem, not this one's;
 * this bound only has to be generous. */
#define GLR_TOUR_PRESENCE_NAME_CAP 64

/* Animation durations, in rendered frames (~60/s). */
#define GLR_TOUR_PRESENCE_INTRO_FRAMES    84   /* title card + border fade-in */
#define GLR_TOUR_PRESENCE_CARD_IN_FRAMES  10   /* card ease-in                */
#define GLR_TOUR_PRESENCE_CARD_OUT_FRAMES 24   /* card ease-out               */
#define GLR_TOUR_PRESENCE_OUTRO_FRAMES    24   /* border collapse on exit     */
#define GLR_TOUR_PRESENCE_BREATHE_FRAMES  180  /* one full breath, ~3 s       */

/* Border opacity floor/ceiling across a breath. Deliberately a narrow, dim
 * band: this sits in peripheral vision for the whole tour, so it has to read
 * as "alive" without competing with the captions it frames. */
#define GLR_TOUR_PRESENCE_ALPHA_MIN 0.30f
#define GLR_TOUR_PRESENCE_ALPHA_MAX 0.62f

typedef enum {
    GLR_TOUR_PRESENCE_OFF = 0,  /* no tour, and none finishing            */
    GLR_TOUR_PRESENCE_INTRO,    /* title card up, border easing in        */
    GLR_TOUR_PRESENCE_ACTIVE,   /* border only, breathing                 */
    GLR_TOUR_PRESENCE_OUTRO     /* tour gone; border collapsing away      */
} GlrTourPresencePhase;

/* Everything the renderer needs, and nothing it has to interpret: the alphas
 * already fold breathing and fades together, so tour_ui_presence_render() is a
 * pure function of this struct. */
typedef struct {
    GlrTourPresencePhase phase;
    float border_alpha;  /* 0..1, breathe * fade - the band's opacity     */
    float band_scale;    /* 1 normally, 1 -> 0 across the outro collapse  */
    float card_alpha;    /* 0..1; nonzero only during the intro           */
    const char *name;    /* latched tour name, never NULL (may be empty)  */
} GlrTourPresenceView;

/* Advance one rendered frame and return the resulting view. `tour_active` is
 * GlrTourPlaybackView.active (controlled tours only - env-driven capture runs
 * report inactive and correctly get no presence layer); `tour_name` is
 * borrowed for the duration of the call only. */
GlrTourPresenceView glr_tour_presence_tick(int tour_active,
                                           const char *tour_name);

/* Drop straight to OFF with no outro. For app resets, where there is no
 * "exit" to convey because the whole document is being replaced. */
void glr_tour_presence_reset(void);

#endif /* GLR_TOUR_PRESENCE_H */
