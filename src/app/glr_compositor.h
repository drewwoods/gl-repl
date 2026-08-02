/*
 * glr_compositor.h - App-level compositor post-processing hook.
 *
 * A seam for running a post-process pass over the ENTIRE composited
 * frame - the 3D scene plus every 2D UI layer - after all drawing is
 * done and before the buffer swap. The controller calls the hook at the
 * tail of glr_ctrl_display_frame().
 *
 * This is the compositor-level counterpart to src/scene/postprocess_filter.c,
 * which runs inside render3d_draw_scene() over the scene viewport rect
 * only and leaves the code panel / UI crisp. The two are independent:
 * the scene filter is owned by the scene layer (and reachable from the
 * standalone render3d_demo), while this hook is an app concern operating on
 * the final framebuffer.
 *
 * The effect vocabulary is shared (Render3dPostFilterMode). The chromatic
 * aberration pass reuses the scene primitive over the full window rect,
 * so it is literally the same effect the scene applies to its viewport,
 * now spanning the whole frame. No shaders, no FBOs - fixed-function GL.
 */
#ifndef GLR_COMPOSITOR_H
#define GLR_COMPOSITOR_H

#include "render3d/postprocess_filter.h"  /* Render3dPostFilterMode */

/* Apply the selected post-process filter to the whole window
 * (0, 0, win_w, win_h), in GL bottom-left window coordinates. No-op for
 * RENDER3D_POST_FILTER_OFF, an out-of-range mode, or non-positive
 * dimensions. Must be called after all of the frame's geometry and 2D
 * UI have been drawn (it samples the back buffer in place) and before
 * glutSwapBuffers(). */
void glr_compositor_postprocess_frame(Render3dPostFilterMode mode,
                                      int win_w, int win_h);

#endif /* GLR_COMPOSITOR_H */
