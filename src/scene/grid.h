/*
 * scene_grid.h - Themeable reference-grid renderer.
 *
 * Draws the scene's optional reference grid using the effective theme, major
 * spacing, extent, and transition state already prepared in FrameRenderContext.
 * The controller chooses those settings; this module is responsible only for
 * rendering the requested grid style.
 */
#ifndef SCENE_GRID_H
#define SCENE_GRID_H

#include "render_types.h"

/* Render the grid floor for the current frame. `frame_ctx` carries the camera,
 * theme, transition opacity, and spacing tables the grid code needs. */
void scene_grid_render(const FrameRenderContext *frame_ctx);

/* True for the themes whose own fog is incompatible with the
 * synthesized clear-color recede (EXP2: GRID_THEME_FOG, OCEAN). Under
 * the FOG transition style these fall back to the plain alpha FADE.
 * The FAR extent is intentionally NOT here: its distance fog is the
 * same LINEAR/clear-color model as the recede, so it composes without
 * a pop. Pure — safe to call from tests. `grid_theme` is a GridTheme. */
int scene_grid_theme_uses_fog(int grid_theme);

#endif /* SCENE_GRID_H */
