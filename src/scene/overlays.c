/*
 * scene_overlays.c - tiny per-vertex GL primitives the controller's overlay
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

void scene_draw_vertex_number_label(int vertex_idx,
                                    float vx, float vy, float vz) {
    char label[16];
    snprintf(label, sizeof(label), " v%d", vertex_idx);
    glRasterPos3f(vx, vy, vz);
    for (const char *c = label; *c; c++)
        glutBitmapCharacter(FONT_MONO, (unsigned char)*c);
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
