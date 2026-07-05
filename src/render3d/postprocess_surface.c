/*
 * postprocess_surface.c - see postprocess_surface.h.
 *
 * The warp is computed once per vertex in render3d_post_surface_point and
 * reused by both the textured-grid emitter and the scanline emitter, so
 * the image and its scanline mask always ride the exact same curved
 * surface. Barrel first (radial outward push), ripple second (a
 * horizontal sine travelling vertically, animated by t).
 */
#include "postprocess_surface.h"

#include "gl_includes.h"

#include <math.h>   /* sinf for the ripple */

/* Ripple spatial frequency (radians across the rect height) and temporal
 * speed (radians/sec). Kept subtle so the sine reads as CRT signal
 * shimmer under the scanlines rather than a full underwater warp — a
 * caller wanting the "underwater" look just raises wobble. */
#define POST_SURFACE_RIPPLE_FREQ   9.0f
#define POST_SURFACE_RIPPLE_SPEED  2.2f

void render3d_post_surface_point(const Render3dPostSurface *s,
                                 float u, float v,
                                 float *out_x, float *out_y) {
    float cx = 0.5f * (float)s->sw;
    float cy = 0.5f * (float)s->sh;
    /* Centred, normalized to [-1,1] with the corners landing at r^2 = 2. */
    float nx = 2.0f * u - 1.0f;
    float ny = 2.0f * v - 1.0f;
    /* Barrel: push each vertex outward from the centre, more at the edges
     * (r^2 term), so the image bulges toward the viewer and overscans the
     * corners. For bulge >= 0 every vertex moves outward, so the warped
     * grid strictly contains the rect — no black gaps. */
    float scale = 1.0f + s->bulge * (nx * nx + ny * ny);
    float px = cx + nx * scale * cx;
    float py = cy + ny * scale * cy;
    /* Ripple: a horizontal sine that travels vertically and animates with
     * time — the wobble that makes a time-driven scene shimmer. */
    if (s->wobble != 0.0f)
        px += s->wobble * sinf(ny * POST_SURFACE_RIPPLE_FREQ
                               + s->t * POST_SURFACE_RIPPLE_SPEED);
    *out_x = px;
    *out_y = py;
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
     * centre and compresses toward the (curved) edges — the physically
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
