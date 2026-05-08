/*
 * scene_axes.h - Coordinate axes rendering with themeable appearance.
 *
 * Renders reference coordinate axes (X, Y, Z) at the origin showing direction
 * and orientation. Axes help users orient themselves in 3D space and understand
 * coordinate conventions. Themeable: multiple visual styles available (simple
 * arrows, lines with labels, cones, etc.). Toggled via F4 key or config menu
 * to cycle through themes.
 *
 * Theme system: Axes themes are defined as AxesTheme enum in sample.h, with
 * each theme having a spec (AxesThemeSpec) in scene_axes.c that defines:
 *   - Visual style (arrows, lines, cones, etc.)
 *   - Colors (red for X, green for Y, blue for Z)
 *   - Size and scale
 *   - Label visibility (axis names X/Y/Z)
 *   - Transparency
 *
 * Examples: Off (no axes), simple colored lines, colored cones, labeled arrows,
 * arrowheads, etc.
 *
 * Configuration: Axes appearance is controlled by config:
 *   - REPL_CONFIG_AXES_THEME: which theme (axes style)
 *
 * Rendering: Axes are drawn at the origin (0, 0, 0) and extend along X, Y, Z
 * directions. Rendered after grid but before user geometry (or optionally after,
 * depending on theme). Uses fixed size regardless of camera zoom so axes remain
 * visible at all scales. Color-coded: red=X, green=Y, blue=Z (standard convention).
 */
#ifndef SCENE_AXES_H
#define SCENE_AXES_H

#include "render_types.h"

/* Render the coordinate axes at the origin. Displays X (red), Y (green), and
 * Z (blue) axes with the currently selected theme. frame_ctx provides render
 * context (camera, viewport, theme/config state). Called during axes rendering
 * phase of the frame. Axes appearance is controlled by config (axes theme).
 * Axes help orient users in 3D space and show coordinate conventions. */
void scene_axes_render(const FrameRenderContext *frame_ctx);

#endif /* SCENE_AXES_H */
