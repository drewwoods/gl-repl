/*
 * grid.h - Themeable reference-grid renderer.
 *
 * Draws the scene's optional reference grid using the effective theme, major
 * spacing, extent, and transition state already prepared in Render3dFrameRenderContext.
 * The caller chooses those settings; this module is responsible only for
 * rendering the requested grid style.
 */
#ifndef RENDER3D_GRID_H
#define RENDER3D_GRID_H

#include "render_types.h"
#include "render3d_transition.h"   /* Render3dXnReveal */

/* Grid show/hide fade durations (seconds). Owned by the grid module, not the
 * generic transition machine: the grid's reveal curve (render3d_grid_reveal)
 * reads these, scaled by each theme's g_grid_reveal[].time multiplier. The
 * fade-in is the leisurely "draw-in" window; the fade-out is quicker so
 * cycling themes doesn't feel sticky. Tune to taste. */
#ifndef GRID_FADE_IN_SECS
#define GRID_FADE_IN_SECS  0.25f
#endif
#ifndef GRID_FADE_OUT_SECS
#define GRID_FADE_OUT_SECS 0.20f
#endif

/* The grid's transition curve plugin (see Render3dXnReveal): maps elapsed fade
 * time to opacity using GRID_FADE_*_SECS and the per-theme time multiplier,
 * and inverts it for reversal continuity. The caller binds this into the
 * grid's Render3dXnState at render3d_xn_init, then only feeds the machine dt. */
extern const Render3dXnReveal render3d_grid_reveal;

/* Render the grid floor for the current frame. `frame_ctx` carries the camera,
 * theme, transition opacity, and spacing tables the grid code needs. */
void render3d_grid_render(const Render3dFrameRenderContext *frame_ctx);

/* True for the themes whose own fog is incompatible with the
 * synthesized clear-color recede (currently OCEAN's EXP2 atmosphere).
 * FROZEN is not here: its glacial mist is under-ice-only, so the
 * steady above-ground frame emits no fog and the theme hides like a
 * fog-less one. Under
 * the FOG transition style these fall back to the plain alpha FADE.
 * The FAR extent is intentionally NOT here: its distance fog is the
 * same LINEAR/clear-color model as the recede, so it composes without
 * a pop. Pure - safe to call from tests. */
int render3d_grid_theme_uses_fog(Render3dGridTheme grid_theme);

/* True for the generic table-driven line themes that dissolve their
 * per-vertex alpha to the backdrop at the extent rim (and sweep the
 * front inward while hiding) instead of fogging to the GL clear color.
 * That is the standard line themes plus XZ Ruler and Star Chart; the
 * custom environment themes and Radar own their own atmosphere/fog and
 * are excluded.
 * Pure - safe to call from tests. */
int render3d_grid_theme_uses_edge_fade(Render3dGridTheme grid_theme);

#endif /* RENDER3D_GRID_H */
