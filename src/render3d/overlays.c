/*
 * overlays.c - tiny per-vertex GL primitives the edit_overlays subsystem's
 * overlay orchestration calls. Outlines and vertex-point overlays used to
 * live here as full GLCmd-walking renderers; they moved to
 * src/subsystems/edit_overlays/. The subsystem walks authored vertices
 * directly, and uses glPolygonMode redraws for generated geometry such as
 * glutSolid* outlines / mesh vertex points. The gluTessCallback edge-flag
 * registration in src/repl/executor.c keeps internal triangulation edges
 * suppressed in GL_LINE mode, so the subsystem doesn't need to walk the
 * program for that either.
 */
#include "gl_includes.h"
#include "overlays.h"
#include "config.h"
#include "render3d/palette.h"

#include <math.h>
#include <stdio.h>

#define FOCUSED_NORMAL_SEGMENTS 32
#define FOCUSED_NORMAL_EPS 1e-8f

/* Emit `text` glyph-by-glyph at the current raster position. Useful
 * for chaining a second run of glyphs (e.g. detail text) after the
 * primary call has already set glRasterPos. lights.c and render.c go
 * through render3d_draw_bitmap_text below — they're single-string sites
 * that want the raster_pos set in the same call. */
void render3d_emit_bitmap_text(void *font, const char *text) {
    if (!text) return;
    for (const char *c = text; *c; c++)
        glutBitmapCharacter(font, (unsigned char)*c);
}

void render3d_draw_bitmap_text(void *font, float x, float y, float z,
                            const char *str) {
    glRasterPos3f(x, y, z);
    render3d_emit_bitmap_text(font, str);
}

void render3d_draw_vertex_label_text(float vx, float vy, float vz,
                                  const char *primary_text,
                                  const char *detail_text) {
    if (!primary_text || !primary_text[0]) return;
    glRasterPos3f(vx, vy, vz);
    render3d_emit_bitmap_text(FONT_MONO, primary_text);
    render3d_emit_bitmap_text(FONT_TINY, detail_text);
}

void render3d_draw_normal_vector_arrow(float vx, float vy, float vz,
                                    float nx, float ny, float nz,
                                    float scale) {
    glBegin(GL_LINES);
    glVertex3f(vx, vy, vz);
    glVertex3f(vx + nx * scale, vy + ny * scale, vz + nz * scale);
    glEnd();
    glPointSize(4.0f);
    glBegin(GL_POINTS);
    glVertex3f(vx + nx * scale, vy + ny * scale, vz + nz * scale);
    glEnd();
    glPointSize(1.0f);
}

static float overlay_clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static int overlay_normal_basis(float nx, float ny, float nz,
                                float out_u[3], float out_v[3]) {
    float ref[3];
    float ux, uy, uz, ulen;

    if (fabsf(nz) < 0.9f) {
        ref[0] = 0.0f; ref[1] = 0.0f; ref[2] = 1.0f;
    } else {
        ref[0] = 0.0f; ref[1] = 1.0f; ref[2] = 0.0f;
    }

    ux = ref[1] * nz - ref[2] * ny;
    uy = ref[2] * nx - ref[0] * nz;
    uz = ref[0] * ny - ref[1] * nx;
    ulen = sqrtf(ux * ux + uy * uy + uz * uz);
    if (ulen <= FOCUSED_NORMAL_EPS)
        return 0;

    out_u[0] = ux / ulen;
    out_u[1] = uy / ulen;
    out_u[2] = uz / ulen;
    out_v[0] = ny * out_u[2] - nz * out_u[1];
    out_v[1] = nz * out_u[0] - nx * out_u[2];
    out_v[2] = nx * out_u[1] - ny * out_u[0];
    return 1;
}

static void overlay_circle_point(float cx, float cy, float cz,
                                 const float u[3], const float v[3],
                                 float radius, int idx,
                                 float *out_x, float *out_y, float *out_z) {
    float a = (6.2831853071795864769f * (float)idx) /
              (float)FOCUSED_NORMAL_SEGMENTS;
    float ca = cosf(a);
    float sa = sinf(a);
    *out_x = cx + (u[0] * ca + v[0] * sa) * radius;
    *out_y = cy + (u[1] * ca + v[1] * sa) * radius;
    *out_z = cz + (u[2] * ca + v[2] * sa) * radius;
}

static void overlay_draw_oriented_disk(float cx, float cy, float cz,
                                       const float u[3], const float v[3],
                                       float radius) {
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(cx, cy, cz);
    for (int i = 0; i <= FOCUSED_NORMAL_SEGMENTS; i++) {
        float x, y, z;
        overlay_circle_point(cx, cy, cz, u, v, radius, i, &x, &y, &z);
        glVertex3f(x, y, z);
    }
    glEnd();
}

static void overlay_draw_oriented_ring(float cx, float cy, float cz,
                                       const float u[3], const float v[3],
                                       float radius) {
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < FOCUSED_NORMAL_SEGMENTS; i++) {
        float x, y, z;
        overlay_circle_point(cx, cy, cz, u, v, radius, i, &x, &y, &z);
        glVertex3f(x, y, z);
    }
    glEnd();
}

void render3d_draw_focused_normal_glyph(float vx, float vy, float vz,
                                     float nx, float ny, float nz,
                                     float scale, float alpha_scale,
                                     const char *primary_text,
                                     const char *detail_text) {
    float len = sqrtf(nx * nx + ny * ny + nz * nz);
    float as = alpha_scale;
    float u[3] = { 1.0f, 0.0f, 0.0f };
    float v[3] = { 0.0f, 1.0f, 0.0f };
    float endpoint[3];

    if (as <= 0.0f)
        return;

    if (len > FOCUSED_NORMAL_EPS) {
        nx /= len;
        ny /= len;
        nz /= len;
        overlay_normal_basis(nx, ny, nz, u, v);
    } else {
        nx = 0.0f;
        ny = 0.0f;
        nz = 1.0f;
    }

    endpoint[0] = vx + nx * scale;
    endpoint[1] = vy + ny * scale;
    endpoint[2] = vz + nz * scale;

    render3d_clr_a(RENDER3D_CLR_FOCUSED_NORMAL_PLANE,
                   overlay_clamp01(0.10f * as));
    overlay_draw_oriented_disk(vx, vy, vz, u, v, scale * 0.18f);

    glLineWidth(1.5f);
    render3d_clr_a(RENDER3D_CLR_FOCUSED_NORMAL_PLANE,
                   overlay_clamp01(0.34f * as));
    overlay_draw_oriented_ring(vx, vy, vz, u, v, scale * 0.18f);

    glLineWidth(2.4f);
    render3d_clr_a(RENDER3D_CLR_FOCUSED_NORMAL_ANCHOR,
                   overlay_clamp01(0.85f * as));
    overlay_draw_oriented_ring(vx, vy, vz, u, v, scale * 0.075f);

    if (len > FOCUSED_NORMAL_EPS) {
        glLineWidth(6.0f);
        render3d_clr_a(RENDER3D_CLR_FOCUSED_NORMAL_CORE,
                       overlay_clamp01(0.22f * as));
        glBegin(GL_LINES);
        glVertex3f(vx, vy, vz);
        glVertex3f(endpoint[0], endpoint[1], endpoint[2]);
        glEnd();

        glLineWidth(2.2f);
        render3d_clr_a(RENDER3D_CLR_FOCUSED_NORMAL_CORE,
                       overlay_clamp01(0.95f * as));
        glBegin(GL_LINES);
        glVertex3f(vx, vy, vz);
        glVertex3f(endpoint[0], endpoint[1], endpoint[2]);
        glEnd();

        glPointSize(7.0f);
        glBegin(GL_POINTS);
        glVertex3f(endpoint[0], endpoint[1], endpoint[2]);
        glEnd();
        glPointSize(1.0f);

        if (primary_text && primary_text[0]) {
            render3d_clr_a(RENDER3D_CLR_FOCUSED_NORMAL_ANCHOR,
                           overlay_clamp01(0.95f * as));
            render3d_draw_vertex_label_text(endpoint[0], endpoint[1], endpoint[2],
                                         primary_text,
                                         detail_text ? detail_text : "");
        }
    }

    glLineWidth(1.0f);
}
