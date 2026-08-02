/*
 * postprocess_surface.c - see postprocess_surface.h.
 *
 * The warp is computed once per vertex in render3d_post_surface_point,
 * dispatched on the surface kind, and reused by both the textured-grid
 * emitter and the scanline emitter so the image and its scanline mask
 * always ride the exact same curved surface.
 */
#include "postprocess_surface.h"

#include "gl_includes.h"

#include <math.h>   /* sinf for the ripple */

/* Ripple spatial frequency (radians across the rect height) and temporal
 * speed (radians/sec) for the RIPPLE surface. */
#define POST_SURFACE_RIPPLE_FREQ   9.0f
#define POST_SURFACE_RIPPLE_SPEED  2.2f

Render3dPostSurface render3d_post_surface_flat(int sw, int sh) {
    Render3dPostSurface s;
    s.kind = RENDER3D_POST_SURFACE_FLAT;
    s.sw = sw;
    s.sh = sh;
    s.t = 0.0f;
    s.strength = 0.0f;
    return s;
}

Render3dPostSurface render3d_post_surface_barrel(int sw, int sh, float bulge) {
    Render3dPostSurface s;
    s.kind = RENDER3D_POST_SURFACE_BARREL;
    s.sw = sw;
    s.sh = sh;
    s.t = 0.0f;
    s.strength = bulge;
    return s;
}

Render3dPostSurface render3d_post_surface_ripple(int sw, int sh,
                                                 float t, float amplitude_px) {
    Render3dPostSurface s;
    s.kind = RENDER3D_POST_SURFACE_RIPPLE;
    s.sw = sw;
    s.sh = sh;
    s.t = t;
    s.strength = amplitude_px;
    return s;
}

void render3d_post_surface_point(const Render3dPostSurface *s,
                                 float u, float v,
                                 float *out_x, float *out_y) {
    switch (s->kind) {
    case RENDER3D_POST_SURFACE_BARREL: {
        float cx = 0.5f * (float)s->sw;
        float cy = 0.5f * (float)s->sh;
        /* Centred, normalized to [-1,1] with the corners at r^2 = 2. */
        float nx = 2.0f * u - 1.0f;
        float ny = 2.0f * v - 1.0f;
        /* Pull each vertex inward toward the centre, more at the edges
         * (r^2 term), so magnification falls off with radius: the centre
         * stays ~1:1, the edges compress, and the corners pull in the
         * most - a convex bulge toward the viewer (barrel). The
         * rectangular corners fall outside the image, so the caller draws
         * a black fill behind the mesh for the CRT vignette. (The inverse,
         * scale = 1 + k*r^2, would be pincushion - a concave dish.) */
        float scale = 1.0f - s->strength * (nx * nx + ny * ny);
        *out_x = cx + nx * scale * cx;
        *out_y = cy + ny * scale * cy;
        break;
    }
    case RENDER3D_POST_SURFACE_RIPPLE: {
        /* Flat base plus a horizontal sine that travels vertically and
         * animates with time - the underwater wobble. */
        float ny = 2.0f * v - 1.0f;
        *out_x = u * (float)s->sw
                 + s->strength * sinf(ny * POST_SURFACE_RIPPLE_FREQ
                                      + s->t * POST_SURFACE_RIPPLE_SPEED);
        *out_y = v * (float)s->sh;
        break;
    }
    case RENDER3D_POST_SURFACE_FLAT:
    default:
        *out_x = u * (float)s->sw;
        *out_y = v * (float)s->sh;
        break;
    }
}

void render3d_post_surface_draw_textured(const Render3dPostSurface *s,
                                         int cols, int rows,
                                         float umax, float vmax) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    /* One triangle strip per grid-row band; each strip interleaves the
     * band's top (v1) and bottom (v0) vertex rows across the columns. */
    for (int j = 0; j < rows; j++) {
        float v0 = (float)j / (float)rows;
        float v1 = (float)(j + 1) / (float)rows;
        glBegin(GL_TRIANGLE_STRIP);
        for (int i = 0; i <= cols; i++) {
            float u = (float)i / (float)cols;
            float x, y;
            render3d_post_surface_point(s, u, v1, &x, &y);
            glTexCoord2f(u * umax, v1 * vmax); glVertex2f(x, y);
            render3d_post_surface_point(s, u, v0, &x, &y);
            glTexCoord2f(u * umax, v0 * vmax); glVertex2f(x, y);
        }
        glEnd();
    }
}

void render3d_post_surface_draw_scanlines(const Render3dPostSurface *s,
                                          int cols, int spacing_px) {
    if (cols < 1) cols = 1;
    if (spacing_px < 1) spacing_px = 1;
    if (s->sh <= 0) return;
    /* Lines are placed at device-uniform v, then warped through the same
     * surface. On a barrel that means the pitch spreads at the (magnified)
     * centre and compresses toward the (curved) edges - the physically
     * correct look for uniform scanlines on a curved tube. */
    for (int y = 0; y < s->sh; y += spacing_px) {
        float v = ((float)y + 0.5f) / (float)s->sh; /* pixel-row centre */
        glBegin(GL_LINE_STRIP);
        for (int i = 0; i <= cols; i++) {
            float u = (float)i / (float)cols;
            float x, yy;
            render3d_post_surface_point(s, u, v, &x, &yy);
            glVertex2f(x, yy);
        }
        glEnd();
    }
}
