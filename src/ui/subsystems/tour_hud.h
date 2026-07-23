/*
 * tour_hud.h - controlled-tour transport HUD (top-of-scene overlay).
 *
 * Feature-owned UI under the `tour_ui_*` prefix, the transport counterpart of
 * replay_ui_hud.c. It legitimately knows controlled-tour concepts (playback
 * state, speed, step/total, source line) and reads ONLY from the per-frame
 * UiRenderSnapshot's `tour` slice (GlrTourPlaybackView) — never live
 * pointer-script state. Display-only: no hit-testing, no state mutation.
 *
 * Rendered at the TOP of the scene viewport, separate from the bottom-mounted
 * REPL replay HUD, so a tour demonstrating replay can show both at once. Drawn
 * before compositor post-processing; the pointer/cursor overlay still composits
 * on top afterward (gl_repl.c), so the cursor stays visually above the HUD.
 */
#ifndef TOUR_UI_HUD_H
#define TOUR_UI_HUD_H

/* HUD footprint, shared with any helper that positions relative to it. */
#define TOUR_HUD_MARGIN_X     18
#define TOUR_HUD_MARGIN_Y     18   /* gap from the scene's top edge */
/* The panel always spans the available scene width (minus margins) — it is
 * never forced wider than the scene, so a narrow scene (left code panel, 90%
 * panel fraction) can't push it into the code panel. Below this width the HUD
 * is too cramped to read, so it is skipped entirely. */
#define TOUR_HUD_MIN_VISIBLE  120
#define TOUR_HUD_HEIGHT       56
#define TOUR_HUD_PROGRESS_Y   22
#define TOUR_HUD_PROGRESS_H    6
#define TOUR_HUD_TEXT_PAD_X   10
#define TOUR_HUD_TEXT_LINE1_Y 36
#define TOUR_HUD_TEXT_LINE2_Y  4

/* Render the controlled-tour HUD once per frame. Reads snap->tour and the
 * scene rect from ui_layout_scene_rect. No-op when snap->tour.active is 0
 * (env-capture scripts and the no-tour idle both report inactive). */
struct UiRenderSnapshot;
void tour_ui_hud_render(const struct UiRenderSnapshot *snap);

#endif /* TOUR_UI_HUD_H */
