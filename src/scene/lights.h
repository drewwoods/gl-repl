/*
 * scene_lights.h - Scene light setup and light-indicator rendering.
 *
 * Owns the fixed-function light setup helpers scene_render.c uses around the
 * main geometry pass. The per-frame inputs come from SceneFrameRenderContext: which
 * lights are enabled, their colors/positions, and whether indicator geometry
 * should be drawn.
 */
#ifndef SCENE_LIGHTS_H
#define SCENE_LIGHTS_H

#include "render_types.h"

/* Initialize the global ambient light model the scene uses as its baseline. */
void scene_lights_init_global_ambient(void);

/* Apply the frame's light state before executing user geometry. */
void scene_lights_setup(const SceneFrameRenderContext *frame_ctx);

/* Render light-indicator overlays after the main geometry pass when enabled. */
void scene_lights_render(const SceneFrameRenderContext *frame_ctx);

#endif /* SCENE_LIGHTS_H */
