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

/* Render the grid floor for the current frame. `frame_ctx` carries the camera,
 * theme, transition opacity, and spacing tables the grid code needs. */
void scene_grid_render(const SceneFrameRenderContext *frame_ctx);

/* True for the themes whose own fog is incompatible with the
 * synthesized clear-color recede (EXP2: GRID_THEME_FOG, OCEAN).
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
 * That is the standard themes minus FOG (which owns its EXP2 fog); the
 * custom environment themes own their own atmosphere and are excluded.
 * Pure — safe to call from tests. */
int scene_grid_theme_uses_edge_fade(SceneGridTheme grid_theme);

#endif /* SCENE_GRID_H */
