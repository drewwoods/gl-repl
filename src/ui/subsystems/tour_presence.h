/*
 * tour_presence.h - ambient guided-tour presence layer (whole-window).
 *
 * Feature-owned UI under the `tour_ui_*` prefix, the peripheral counterpart of
 * tour_hud.c: where the HUD is a panel you read, this is a frame you notice.
 * Pure over a GlrTourPresenceView - it holds no state and reads no live tour
 * data, so all of its animation comes from src/app/glr_tour_presence.c.
 *
 * Drawn LAST, in the host band after the compositor post-process, for two
 * reasons: a user's own whole-frame Post FX must not paint over the one piece
 * of chrome that says which mode the app is in, and nothing else may sit on
 * top of it either. It deliberately does NOT route through the Post FX effect
 * enum - that is a user setting the Config menu owns (and that a tour may well
 * be demonstrating), and it is the wrong channel for app state. The two
 * capture-based effects would also read back the frame every frame and smear
 * the code panel, which during a tour is exactly the text being read.
 *
 * Nothing here samples the framebuffer: it is four gradient bands and one
 * card, so the cost is a handful of blended quads plus (during the intro only)
 * two short bitmap strings.
 */
#ifndef TOUR_UI_PRESENCE_H
#define TOUR_UI_PRESENCE_H

#include "app/glr_tour_presence.h"   /* GlrTourPresenceView */

/* Glow band inset from each window edge, and the solid rule at the very edge.
 * Both scale with the view's band_scale so the outro collapses them together. */
#define TOUR_PRESENCE_BAND_PX 16
#define TOUR_PRESENCE_RULE_PX  2

/* Title-card metrics. The card sits above the window's vertical centre so it
 * stays clear of the caption band tours anchor low in the scene. */
#define TOUR_PRESENCE_CARD_PAD_X    16
#define TOUR_PRESENCE_CARD_PAD_Y    12
#define TOUR_PRESENCE_CARD_GAP_Y    10   /* between the title and hint lines */
#define TOUR_PRESENCE_CARD_RIBBON_H  3   /* amber top edge, as on the HUD    */
#define TOUR_PRESENCE_CARD_CENTER_Y  0.56f  /* fraction of window height     */
#define TOUR_PRESENCE_NAME_MAX_CHARS 40

/* Render the presence layer for one frame. No-op when the view is OFF or the
 * window is degenerate. Expects no particular GL state: sets up its own 2D
 * ortho + blending and restores what it changed. */
void tour_ui_presence_render(const GlrTourPresenceView *v,
                             int win_w, int win_h);

#endif /* TOUR_UI_PRESENCE_H */
