/*
 * backdrop.h - Scene backdrop renderer.
 *
 * Draws the optional background environment behind the rest of the scene. The
 * current backdrop mode comes from Render3dFrameRenderContext; this module renders the
 * selected backdrop style without owning any higher-level config or caller
 * state.
 */
#ifndef RENDER3D_BACKDROP_H
#define RENDER3D_BACKDROP_H

#include "render_types.h"

/* Render the backdrop for the current frame. Called at the deepest background
 * stage before grid, user geometry, and overlays. */
void render3d_backdrop_render(const Render3dFrameRenderContext *frame_ctx);

/* Configure + enable any backdrop-owned environment lights (GL_LIGHT4+,
 * above the caller's user-facing GL_LIGHT0..3). Called in the pass setup
 * phase, after render3d_lights_setup, so lit user geometry sees them.
 * No-op for backdrops without lights. */
void render3d_backdrop_setup_lights(const Render3dFrameRenderContext *frame_ctx);

#endif /* RENDER3D_BACKDROP_H */
