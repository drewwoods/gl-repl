/*
 * scene_backdrop.h - Scene backdrop renderer.
 *
 * Draws the optional background environment behind the rest of the scene. The
 * current backdrop mode comes from SceneFrameRenderContext; this module renders the
 * selected backdrop style without owning any higher-level config or controller
 * state.
 */
#ifndef SCENE_BACKDROP_H
#define SCENE_BACKDROP_H

#include "render_types.h"

/* Backdrop style selector. Stored as a plain int in
 * SceneRenderConfig.backdrop_mode (the app's config layer keeps it as an
 * integer cycle); these names give the scene switch its meaning and must
 * stay in sync with backdrop_mode_names[] in src/app/glr_actions.c. */
typedef enum SceneBackdropMode {
    SCENE_BACKDROP_OFF        = 0,
    SCENE_BACKDROP_CITYSCAPE  = 1,
    SCENE_BACKDROP_STARS      = 2,
    SCENE_BACKDROP_CITY_STARS = 3
} SceneBackdropMode;

/* Render the backdrop for the current frame. Called at the deepest background
 * stage before grid, user geometry, and overlays. */
void scene_backdrop_render(const SceneFrameRenderContext *frame_ctx);

#endif /* SCENE_BACKDROP_H */
