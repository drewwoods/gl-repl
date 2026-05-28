/*
 * overlays.h - Tiny per-vertex overlay primitives.
 *
 * These are the scene module's narrow overlay exports: draw one vertex-number
 * label or one normal-vector arrow at an already-transformed position.
 * Higher-level policy such as deciding whether overlays are enabled, walking
 * the user's program, tracking transforms, or bracketing GL state belongs to
 * the controller.
 */
#ifndef SCENE_OVERLAYS_H
#define SCENE_OVERLAYS_H

#include "gl_includes.h"  /* GLUT_BITMAP_* font pointer types */

/* Per-vertex primitive renderers exposed for the edit_overlays
 * subsystem's overlay orchestration. Each draws ONE label / arrow at a
 * transformed position; iteration of the user's program and applying
 * transforms is the src/subsystems/edit_overlays/ responsibility (it walks
 * the program via replay_walk_user_vertices and calls these primitives at
 * each visit). That subsystem also sets up the surrounding GL state
 * (color, depth disable, push/pop attribs). */
void scene_draw_vertex_label_text(float vx, float vy, float vz,
                                  const char *primary_text,
                                  const char *detail_text);
void scene_draw_normal_vector_arrow(float vx, float vy, float vz,
                                    float nx, float ny, float nz,
                                    float scale);

/* Draw `str` at world position (x, y, z) using `font` (e.g. FONT_MONO
 * or FONT_SMALL — both `void *` GLUT bitmap pointers). Combines the
 * glRasterPos3f + per-character glutBitmapCharacter loop the scene
 * module repeats in light indicators, overlay labels, and the orbit
 * gizmo coord readout. Color is the caller's responsibility (set
 * glColor* before calling). */
void scene_draw_bitmap_text(void *font, float x, float y, float z,
                            const char *str);

/* Outlines and vertex-point overlays are edit_overlays-subsystem passes, not
 * scene primitives. src/subsystems/edit_overlays/ re-executes the user's
 * geometry in GL_LINE or GL_POINT mode and chooses the surrounding GL state;
 * src/scene/ only provides the per-vertex label/arrow helpers above. */

#endif /* SCENE_OVERLAYS_H */
