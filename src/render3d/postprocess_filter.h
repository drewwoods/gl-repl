/*
 * postprocess_filter.h - Scene-viewport post-processing.
 *
 * Self-contained scene-layer effect invoked at the very end of
 * render3d_draw_scene(), after the accumulation resolve, on the fully
 * resolved scene image. Pure fixed-function GL (no shaders, no FBOs):
 * the resolved scene rect is copied into one texture and redrawn.
 *
 * The selected mode flows through Render3dRenderConfig, so the effect works
 * for standalone callers as well as the full application. Config menus and
 * persistence stay outside this module. The effect only touches the scene
 * viewport rect; the code panel and other 2D UI stay crisp.
 */
#ifndef RENDER3D_POSTPROCESS_FILTER_H
#define RENDER3D_POSTPROCESS_FILTER_H

#include "gl_includes.h"   /* GLint for the 2D-bracket matrix-mode snapshot */
#include "themes.h"        /* Render3dPostFilterMode */

/* Human-readable name for status text. Out-of-range -> "Off". */
const char *render3d_postprocess_filter_mode_name(Render3dPostFilterMode mode);

/* Invalidate the cached texture handle / dimensions. Deletes the live
 * texture if a context still owns it. Called from render3d_init_gl()
 * so a fresh GL context never reuses a stale texture name. */
void render3d_postprocess_filter_reset(void);

/* Apply the selected filter to the scene rect (GL bottom-left window
 * coords: the same rect render3d_draw_scene() rendered into via
 * glViewport). No-op for RENDER3D_POST_FILTER_OFF or invalid rects. */
void render3d_postprocess_filter_render(Render3dPostFilterMode mode,
                                     int sx, int sy, int sw, int sh);

/* Scene-layer screen-space 2D bracket shared by the post filters and by
 * whatever composites over the scene rect through the buffer hooks -
 * today the buffer-viz quads (src/render3d/ must not depend on
 * ui/gl_2d.h, and neither may a subscriber reach for it). begin
 * pushes GL_ALL_ATTRIB_BITS + all three matrix stacks, sets a pixel
 * ortho over the rect with texturing on (GL_REPLACE) and depth/light/
 * blend off; end pops it all back. The matrix mode is snapshot/restored
 * through the out-param (it is not covered by glPushAttrib), so the
 * pair isn't coupled by a file-static. */
void render3d_post_2d_begin(int sx, int sy, int sw, int sh,
                            GLint *saved_matrix_mode_out);
void render3d_post_2d_end(GLint saved_matrix_mode);

#endif /* RENDER3D_POSTPROCESS_FILTER_H */
