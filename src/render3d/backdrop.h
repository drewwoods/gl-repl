/*
 * backdrop.h - Scene backdrop renderer.
 *
 * Draws the optional background environment behind the rest of the scene. The
 * current backdrop mode comes from SceneFrameRenderContext; this module renders the
 * selected backdrop style without owning any higher-level config or controller
 * state.
 */
#ifndef SCENE_BACKDROP_H
#define SCENE_BACKDROP_H

#include "render_types.h"

/* Render the backdrop for the current frame. Called at the deepest background
 * stage before grid, user geometry, and overlays. */
void scene_backdrop_render(const SceneFrameRenderContext *frame_ctx);

/* Configure + enable any backdrop-owned environment lights (GL_LIGHT4+,
 * above the REPL's user-facing GL_LIGHT0..3). Called in the pass setup
 * phase, after scene_lights_setup, so lit user geometry sees them.
 * No-op for backdrops without lights. */
void scene_backdrop_setup_lights(const SceneFrameRenderContext *frame_ctx);

#endif /* SCENE_BACKDROP_H */
