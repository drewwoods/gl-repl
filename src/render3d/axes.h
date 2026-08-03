/*
 * axes.h - Themeable coordinate-axes renderer.
 *
 * Draws the optional origin axes using the effective theme and fade state
 * prepared in Render3dFrameRenderContext. The caller decides which theme is active;
 * this module renders that choice and nothing else.
 */
#ifndef RENDER3D_AXES_H
#define RENDER3D_AXES_H

#include "render_types.h"
#include "render3d_transition.h"   /* Render3dXnReveal */

/* Axes show/hide fade durations (seconds), owned by the axes module. Quicker
 * than the grid's - the axes are a couple of lines, not a draw-in. */
#ifndef AXES_FADE_IN_SECS
#define AXES_FADE_IN_SECS  0.15f
#endif
#ifndef AXES_FADE_OUT_SECS
#define AXES_FADE_OUT_SECS 0.10f
#endif

/* The axes' transition curve plugin (see Render3dXnReveal): a plain linear
 * opacity ramp over AXES_FADE_*_SECS (no per-theme speed). The caller binds
 * this into the axes' Render3dXnState at render3d_xn_init. */
extern const Render3dXnReveal render3d_axes_reveal;

/* Render the origin axes for the current frame. `frame_ctx` supplies the
 * selected theme, fade state, and camera-dependent sizing inputs. */
void render3d_axes_render(const Render3dFrameRenderContext *frame_ctx);

#endif /* RENDER3D_AXES_H */
