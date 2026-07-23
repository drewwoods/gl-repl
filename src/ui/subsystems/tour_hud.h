/*
 * tour_hud.h - controlled-tour transport HUD (top-of-scene overlay).
 *
 * Feature-owned UI under the `tour_ui_*` prefix, the transport counterpart of
 * replay_ui_hud.c. It legitimately knows controlled-tour concepts (playback
 * state, speed, step/total, source line) and reads ONLY from the per-frame
 * UiRenderSnapshot's `tour` slice (GlrTourPlaybackView) — never live
 * pointer-script state. It renders and classifies hits; the controller owns
 * the resulting expand/collapse mutation.
 *
 * Rendered at the TOP of the scene viewport, separate from the bottom-mounted
 * REPL replay HUD, so a tour demonstrating replay can show both at once. Drawn
 * before compositor post-processing; the pointer/cursor overlay still composits
 * on top afterward (gl_repl.c), so the cursor stays visually above the HUD.
 */
#ifndef TOUR_UI_HUD_H
#define TOUR_UI_HUD_H

#include "ui/core/hit.h"

/* Feature-owned hit kind in the reserved subsystem range. */
enum { UI_HIT_TOUR_HUD = UI_HIT_CORE_COUNT + 66 };

/* HUD footprint, shared with any helper that positions relative to it. */
#define TOUR_HUD_MARGIN_X     18
#define TOUR_HUD_MARGIN_Y     18   /* gap from the scene's top edge */
/* Expanded mode spans the available scene width; compact mode is only as wide
 * as "Tour | name | [+]", capped to that same available width. */
#define TOUR_HUD_MIN_VISIBLE       80
#define TOUR_HUD_EXPANDED_HEIGHT   56
#define TOUR_HUD_COMPACT_HEIGHT    26
#define TOUR_HUD_PROGRESS_Y   22
#define TOUR_HUD_PROGRESS_H    6
#define TOUR_HUD_RIBBON_H      3   /* solid amber band along the top edge */
#define TOUR_HUD_TEXT_PAD_X   10
#define TOUR_HUD_TEXT_LINE1_Y 36
#define TOUR_HUD_TEXT_LINE2_Y  4
#define TOUR_HUD_COMPACT_TEXT_Y 7

/* Render the controlled-tour HUD once per frame. Reads snap->tour and the
 * scene rect from ui_layout_scene_rect. No-op when snap->tour.active is 0
 * (env-capture scripts and the no-tour idle both report inactive). */
struct UiRenderSnapshot;
void tour_ui_hud_render(const struct UiRenderSnapshot *snap);

/* Available panel width for a scene `scene_w` px wide: scene minus horizontal
 * margins, never forced wider (returns 0 when too narrow to render). The
 * expanded-mode / cap width used by tour_ui_hud_rect and the single source of
 * truth exposed for the clamp guard test. */
int tour_hud_panel_width(int scene_w);

/* Compute the currently visible compact/expanded panel bounds in OpenGL
 * window coordinates. Returns 0 when inactive or too narrow to render. */
int tour_ui_hud_rect(const struct UiRenderSnapshot *snap,
                     int *x, int *y, int *w, int *h);

/* Pure hit-test over the exact rendered panel. mx/my use GLUT window
 * coordinates (top-down y); returns UI_HIT_TOUR_HUD anywhere in the panel. */
UiHit tour_ui_hud_hit_test(const struct UiRenderSnapshot *snap, int mx, int my);

#endif /* TOUR_UI_HUD_H */
