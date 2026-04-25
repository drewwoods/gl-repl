/*
 * scene_backdrop.h - Backdrop rendering (procedural cityscape or solid color).
 *
 * Renders a background environment for the scene: either a procedural cityscape
 * (buildings, skyline) or a simple solid color. Backdrop is rendered in the
 * background (behind grid and all geometry) using orthogonal projection so it
 * doesn't move with camera. Provides visual context and prevents an empty
 * black void behind the geometry.
 *
 * Backdrop modes (toggled via config REPL_CONFIG_BACKDROP):
 *   - Off: No backdrop, transparent/black background
 *   - Cityscape: Procedural buildings with skyline and horizon
 *   - Solid color: Simple colored plane (e.g., light gray, white)
 *   - Gradient: Gradient from sky color to horizon color
 *
 * Cityscape rendering: Deterministic procedural generation of buildings with
 * varying heights, widths, colors. Generation is seeded so the same scene
 * always produces the same backdrop (useful for consistent snapshots/exports).
 * Buildings are rendered as simple rectangles in orthogonal projection.
 *
 * Integration: Rendered in the deepest background layer (first in the rendering
 * order). Uses orthogonal projection so buildings don't scale with perspective.
 * Backdrop is optional (can be disabled) and doesn't interfere with the user's
 * geometry or grid.
 *
 * Performance: Procedural generation is fast (simple rectangle drawing); backdrop
 * adds negligible overhead to frame time.
 */
#ifndef SCENE_BACKDROP_H
#define SCENE_BACKDROP_H

/* Render the backdrop environment once per frame. Draws either a procedural
 * cityscape or solid color in the background (behind grid, geometry, overlays).
 * Uses orthogonal projection so backdrop doesn't move with camera pan/zoom.
 * Backdrop mode is controlled by config (REPL_CONFIG_BACKDROP toggle). Called
 * early in frame rendering (deepest background layer). */
void scene_backdrop_render(void);

#endif /* SCENE_BACKDROP_H */
