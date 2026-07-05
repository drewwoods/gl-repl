/*
 * postprocess_surface.h - Reusable tessellated warp surface for the
 * post-process filters.
 *
 * A post-process filter historically redraws its captured scene texture
 * on a single screen-aligned quad. This module replaces that quad with a
 * tessellated cols x rows grid whose vertices are displaced by a barrel
 * (CRT bulge) and/or an animated sine ripple, while the texcoords stay
 * linear. The mesh is the whole trick: linear texture sampling over
 * non-linear geometry is what curves the image.
 *
 * The surface is deliberately independent of any one effect's texture
 * work: a filter binds its own texture and sets its own 2D GL state, then
 * asks this module to emit the warped grid. So a future filter can be
 * swapped from a flat quad to this surface without touching this code,
 * and `bulge` is a flat<->barrel morph knob (0 = a flat quad-grid, ramp
 * up to bulge the image toward the viewer).
 *
 * Pure fixed-function GL (glBegin/glTexCoord2f/glVertex2f). No shaders,
 * no FBOs, and it owns no textures or matrices — the caller sets those up.
 */
#ifndef RENDER3D_POSTPROCESS_SURFACE_H
#define RENDER3D_POSTPROCESS_SURFACE_H

/* Warp parameters for one rect. Positions are in the same bottom-left
 * pixel space the filter's 2D ortho uses ([0,sw] x [0,sh]). */
typedef struct Render3dPostSurface {
    int   sw;      /* rect width  in pixels */
    int   sh;      /* rect height in pixels */
    float t;       /* animation time (seconds); drives the ripple */
    float bulge;   /* barrel strength; 0 = flat quad, ~0.1 = gentle CRT */
    float wobble;  /* ripple amplitude in pixels; 0 = no wave */
} Render3dPostSurface;

/* Map a linear grid coordinate (u,v), each in [0,1], to its warped pixel
 * position. Both the textured mesh and any overlay (e.g. scanlines) that
 * must ride the same surface go through this one function, so they stay
 * registered to each other. */
void render3d_post_surface_point(const Render3dPostSurface *s,
                                 float u, float v,
                                 float *out_x, float *out_y);

/* Emit a cols x rows tessellated textured grid over the rect: linear
 * texcoords into [0,umax] x [0,vmax], positions from
 * render3d_post_surface_point. The caller has already bound the source
 * texture and set the tex-env / blend state (GL_REPLACE, blending off is
 * the usual choice). cols/rows are clamped to >= 1. */
void render3d_post_surface_draw_textured(const Render3dPostSurface *s,
                                         int cols, int rows,
                                         float umax, float vmax);

/* Emit horizontal scanlines every `spacing_px` device rows, each line
 * tessellated across `cols` segments and bent onto the surface so the
 * lines curve with the bulge/ripple. The caller sets color, line width,
 * and blend mode (the multiplicative CRT mask, typically). */
void render3d_post_surface_draw_scanlines(const Render3dPostSurface *s,
                                          int cols, int spacing_px);

#endif /* RENDER3D_POSTPROCESS_SURFACE_H */
