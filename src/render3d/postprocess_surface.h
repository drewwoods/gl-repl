/*
 * postprocess_surface.h - Reusable tessellated warp surfaces for the
 * post-process filters.
 *
 * A post-process filter historically redraws its captured scene texture
 * on a single screen-aligned quad. This module replaces that quad with a
 * tessellated cols x rows grid whose vertices are displaced by one of a
 * few named surface *types*, while the texcoords stay linear. The mesh is
 * the whole trick: linear texture sampling over non-linear geometry is
 * what curves the image.
 *
 * The types are:
 *   - FLAT   : an identity quad-grid (the flat<->warp morph's zero point).
 *   - BARREL : a CRT bulge — the corners overscan outward, so the image
 *              bulges toward the viewer. Paired with the scanlines filter.
 *   - RIPPLE : an animated sine wobble (an "underwater" warp). Fully
 *              implemented and tested, but reserved for a future filter —
 *              nothing wires it up yet.
 *
 * The surface is deliberately independent of any one effect's texture
 * work: a filter binds its own texture and sets its own 2D GL state, then
 * asks this module to emit the warped grid. So a future filter can be
 * swapped from a flat quad to any of these surfaces without touching this
 * code.
 *
 * Pure fixed-function GL (glBegin/glTexCoord2f/glVertex2f). No shaders,
 * no FBOs, and it owns no textures or matrices — the caller sets those up.
 */
#ifndef RENDER3D_POSTPROCESS_SURFACE_H
#define RENDER3D_POSTPROCESS_SURFACE_H

typedef enum Render3dPostSurfaceKind {
    RENDER3D_POST_SURFACE_FLAT = 0, /* identity quad-grid */
    RENDER3D_POST_SURFACE_BARREL,   /* CRT bulge (scanlines) */
    RENDER3D_POST_SURFACE_RIPPLE    /* animated sine wobble (future underwater) */
} Render3dPostSurfaceKind;

/* One warped surface. Positions are in the same bottom-left pixel space
 * the filter's 2D ortho uses ([0,sw] x [0,sh]). Build one with a
 * constructor below rather than filling it by hand — `strength` and `t`
 * mean different things per kind. */
typedef struct Render3dPostSurface {
    Render3dPostSurfaceKind kind;
    int   sw;       /* rect width  in pixels */
    int   sh;       /* rect height in pixels */
    float t;        /* animation time (seconds); RIPPLE only */
    float strength; /* BARREL: bulge (0 flat, ~0.1 gentle); RIPPLE: amplitude in px */
} Render3dPostSurface;

/* --- Surface-type constructors --- */

/* Identity grid: maps (u,v) straight to (u*sw, v*sh). */
Render3dPostSurface render3d_post_surface_flat(int sw, int sh);

/* CRT bulge: `bulge` is the barrel strength (0 = flat, ~0.1 = a gentle
 * tube; the corners overscan so the warped grid always covers the rect). */
Render3dPostSurface render3d_post_surface_barrel(int sw, int sh, float bulge);

/* Animated sine wobble (underwater), reserved for a future filter:
 * `amplitude_px` is the horizontal wave amplitude in pixels and `t` the
 * animation time driving it. Not paired with any filter yet. */
Render3dPostSurface render3d_post_surface_ripple(int sw, int sh,
                                                 float t, float amplitude_px);

/* Map a linear grid coordinate (u,v), each in [0,1], to its warped pixel
 * position for this surface's kind. Both the textured mesh and any
 * overlay (e.g. scanlines) that must ride the same surface go through
 * this one function, so they stay registered to each other. */
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
 * lines curve with it. The caller sets color, line width, and blend mode
 * (the multiplicative CRT mask, typically). */
void render3d_post_surface_draw_scanlines(const Render3dPostSurface *s,
                                          int cols, int spacing_px);

#endif /* RENDER3D_POSTPROCESS_SURFACE_H */
