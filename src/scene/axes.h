/*
 * axes.h - Themeable coordinate-axes renderer.
 *
 * Draws the optional origin axes using the effective theme and fade state
 * prepared in SceneFrameRenderContext. The controller decides which theme is active;
 * this module renders that choice and nothing else.
 */
#ifndef SCENE_AXES_H
#define SCENE_AXES_H

#include "render_types.h"

/* Render the origin axes for the current frame. `frame_ctx` supplies the
 * selected theme, fade state, and camera-dependent sizing inputs. */
void scene_axes_render(const SceneFrameRenderContext *frame_ctx);

#endif /* SCENE_AXES_H */
