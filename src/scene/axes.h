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
#include "scene_transition.h"   /* SceneXnReveal */

/* Axes show/hide fade durations (seconds), owned by the axes module. Quicker
 * than the grid's — the axes are a couple of lines, not a draw-in. */
#ifndef AXES_FADE_IN_SECS
#define AXES_FADE_IN_SECS  0.15f
#endif
#ifndef AXES_FADE_OUT_SECS
#define AXES_FADE_OUT_SECS 0.10f
#endif

/* The axes' transition curve plugin (see SceneXnReveal): a plain linear
 * opacity ramp over AXES_FADE_*_SECS (no per-theme speed). The controller
 * binds this into the axes' SceneXnState at scene_xn_init. */
extern const SceneXnReveal scene_axes_reveal;

/* Render the origin axes for the current frame. `frame_ctx` supplies the
 * selected theme, fade state, and camera-dependent sizing inputs. */
void scene_axes_render(const SceneFrameRenderContext *frame_ctx);

#endif /* SCENE_AXES_H */
