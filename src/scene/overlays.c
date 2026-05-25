/*
 * overlays.c - tiny per-vertex GL primitives the controller's overlay
 * orchestration calls. Outlines and vertex-point overlays used to live
 * here as full GLCmd-walking renderers; they moved to src/app/glr_ctrl.c
 * where they're driven by glPolygonMode tricks (re-execute the user's
 * program with GL_LINE / GL_POINT). The gluTessCallback edge-flag
 * registration in src/repl/executor.c keeps internal triangulation edges
 * suppressed in GL_LINE mode, so the controller doesn't need to walk
 * the program either.
 */
#include "gl_includes.h"
#include "overlays.h"
#include "config.h"

#include <stdio.h>

/* Emit `text` glyph-by-glyph at the current raster position. Useful
 * for chaining a second run of glyphs (e.g. detail text) after the
 * primary call has already set glRasterPos. lights.c and render.c go
 * through scene_draw_bitmap_text below — they're single-string sites
 * that want the raster_pos set in the same call. */
void scene_emit_bitmap_text(void *font, const char *text) {
    if (!text) return;
    for (const char *c = text; *c; c++)
        glutBitmapCharacter(font, (unsigned char)*c);
}

void scene_draw_bitmap_text(void *font, float x, float y, float z,
                            const char *str) {
    glRasterPos3f(x, y, z);
    scene_emit_bitmap_text(font, str);
}

void scene_draw_vertex_label_text(float vx, float vy, float vz,
                                  const char *primary_text,
                                  const char *detail_text) {
    if (!primary_text || !primary_text[0]) return;
    glRasterPos3f(vx, vy, vz);
    scene_emit_bitmap_text(FONT_MONO, primary_text);
    scene_emit_bitmap_text(FONT_TINY, detail_text);
}

void scene_draw_normal_vector_arrow(float vx, float vy, float vz,
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
