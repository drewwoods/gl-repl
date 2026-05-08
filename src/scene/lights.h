/*
 * scene_lights.h - Scene lighting setup and visualization.
 *
 * Manages GL light state (default ambient, light positions, visibility
 * indicators). Provides fixed default lights (GL_LIGHT0-GL_LIGHT3) positioned
 * around the scene for immediate-mode rendering. Also renders optional light
 * indicator overlays (small spheres at light positions) to visualize lighting
 * setup.
 *
 * Lighting setup: Default scene uses a simple four-light setup (top, front-left,
 * front-right, back) positioned at fixed locations around the scene. User code
 * can enable/disable lights via glEnable(GL_LIGHT0) / glDisable(GL_LIGHT0), etc.,
 * or modify light parameters via glLight* commands. The default configuration
 * provides reasonable global illumination without requiring complex lighting setup.
 *
 * Initialization: scene_lights_init_global_ambient() sets the default global
 * ambient light color (typically a dim gray, e.g., 0.2, 0.2, 0.2). This is
 * called once at startup.
 *
 * Per-frame setup: scene_lights_setup() applies the light state before rendering
 * (called once per frame). This ensures lights are positioned and enabled
 * according to current state.
 *
 * Light indicators: Optional visual overlay showing light positions as small
 * glowing spheres. Enabled via config toggle (REPL_CONFIG_LIGHT_INDICATORS).
 * Useful for understanding how geometry is lit and debugging lighting issues.
 * Rendered after main geometry so it appears on top.
 */
#ifndef SCENE_LIGHTS_H
#define SCENE_LIGHTS_H

#include "render_types.h"

/* Initialize global ambient light color. Sets the default ambient light that
 * illuminates all surfaces (even those not directly lit). Called once at
 * startup before rendering begins. */
void scene_lights_init_global_ambient(void);

/* Set up light state for the frame. Applies light positions, enables/disables
 * lights according to current state, and configures light parameters.
 * Called once per frame before executing user geometry commands. */
void scene_lights_setup(const FrameRenderContext *frame_ctx);

/* Render light indicator overlays (small spheres at light positions). Visualizes
 * where lights are located in the scene for debugging and understanding
 * geometry lighting. Called during overlay rendering phase if light indicators
 * are enabled (REPL_CONFIG_LIGHT_INDICATORS toggle). */
void scene_lights_render(const FrameRenderContext *frame_ctx);

#endif /* SCENE_LIGHTS_H */
