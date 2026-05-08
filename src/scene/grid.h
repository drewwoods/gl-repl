/*
 * scene_grid.h - Grid floor rendering with themeable appearance.
 *
 * Renders a reference grid on the floor plane (typically Z=0) as a visual
 * reference for scale and alignment. Grid is themeable: multiple visual styles
 * are available (simple lines, major/minor spacing, colored layers, etc.)
 * Toggled via F3 key or config menu to cycle through available themes.
 *
 * Theme system: Grid themes are defined as GridTheme enum in sample.h, with
 * each theme having a spec (GridThemeSpec) in scene_grid.c that defines:
 *   - Line colors (major vs. minor lines)
 *   - Spacing (grid cell size, major line spacing)
 *   - Extent (how many grid cells to draw)
 *   - Rendering style (plain lines, subdivided, etc.)
 *
 * Examples: Blank (no grid), simple line grid, major/minor lines with
 * different colors, major spacing options (1 unit, 10 units, 100 units),
 * extent options (10x10 cells, 100x100 cells, etc.).
 *
 * Configuration: Grid appearance is controlled by config items:
 *   - REPL_CONFIG_GRID_THEME: which theme (grid style)
 *   - REPL_CONFIG_GRID_MAJOR: major line spacing
 *   - REPL_CONFIG_GRID_EXTENT: grid size/extent
 *
 * Rendering: Drawn in the scene's background layer (behind user geometry but
 * in front of backdrop). Uses orthogonal projection for lines so spacing is
 * consistent regardless of camera distance. Grid origin is at (0, 0, 0) and
 * extends symmetrically in X and Y.
 */
#ifndef SCENE_GRID_H
#define SCENE_GRID_H

#include "render_types.h"

/* Render the grid floor. Draws a reference grid on the Z=0 plane with the
 * currently selected theme and configuration. frame_ctx provides render context
 * (camera, viewport, theme/config state). Called during grid rendering phase
 * of the frame (background layer, before user geometry, after backdrop).
 * Grid appearance is controlled by config items (grid theme, major spacing,
 * extent). */
void scene_grid_render(const FrameRenderContext *frame_ctx);

#endif /* SCENE_GRID_H */
