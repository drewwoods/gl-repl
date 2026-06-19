/*
 * grid.h - Themeable reference-grid renderer.
 *
 * Draws the scene's optional reference grid using the effective theme, major
 * spacing, extent, and transition state already prepared in SceneFrameRenderContext.
 * The controller chooses those settings; this module is responsible only for
 * rendering the requested grid style.
 */
#ifndef SCENE_GRID_H
#define SCENE_GRID_H

#include "render_types.h"
#include "scene_transition.h"   /* SceneXnReveal */

/* Grid show/hide fade durations (seconds). Owned by the grid module, not the
 * generic transition machine: the grid's reveal curve (scene_grid_reveal)
 * reads these, scaled by each theme's g_grid_reveal[].time multiplier. The
 * fade-in is the leisurely "draw-in" window; the fade-out is quicker so
 * cycling themes doesn't feel sticky. Tune to taste. */
#ifndef GRID_FADE_IN_SECS
#define GRID_FADE_IN_SECS  0.25f
#endif
#ifndef GRID_FADE_OUT_SECS
#define GRID_FADE_OUT_SECS 0.20f
#endif

/* The grid's transition curve plugin (see SceneXnReveal): maps elapsed fade
 * time to opacity using GRID_FADE_*_SECS and the per-theme time multiplier,
 * and inverts it for reversal continuity. The controller binds this into the
 * grid's SceneXnState at scene_xn_init, then only feeds the machine dt. */
extern const SceneXnReveal scene_grid_reveal;

/* Render the grid floor for the current frame. `frame_ctx` carries the camera,
 * theme, transition opacity, and spacing tables the grid code needs. */
void scene_grid_render(const SceneFrameRenderContext *frame_ctx);

/* True for the themes whose own fog is incompatible with the
 * synthesized clear-color recede (currently OCEAN's EXP2 atmosphere).
 * FROZEN is not here: its glacial mist is under-ice-only, so the
 * steady above-ground frame emits no fog and the theme hides like a
 * fog-less one. Under
 * the FOG transition style these fall back to the plain alpha FADE.
 * The FAR extent is intentionally NOT here: its distance fog is the
 * same LINEAR/clear-color model as the recede, so it composes without
 * a pop. Pure — safe to call from tests. */
int scene_grid_theme_uses_fog(SceneGridTheme grid_theme);

/* True for the generic table-driven line themes that dissolve their
 * per-vertex alpha to the backdrop at the extent rim (and sweep the
 * front inward while hiding) instead of fogging to the GL clear color.
 * That is the standard line themes plus XZ Ruler and Star Chart; the
 * custom environment themes and Radar own their own atmosphere/fog and
 * are excluded.
 * Pure — safe to call from tests. */
int scene_grid_theme_uses_edge_fade(SceneGridTheme grid_theme);

#endif /* SCENE_GRID_H */
