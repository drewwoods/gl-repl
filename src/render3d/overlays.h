/*
 * overlays.h - Tiny per-vertex overlay primitives.
 *
 * These are the scene module's narrow overlay exports: draw one vertex-number
 * label or one normal-vector arrow at an already-transformed position.
 * Higher-level policy such as deciding whether overlays are enabled, walking
 * the user's program, tracking transforms, or bracketing GL state belongs to
 * the controller.
 */
#ifndef RENDER3D_OVERLAYS_H
#define RENDER3D_OVERLAYS_H

#include "gl_includes.h"  /* GLUT_BITMAP_* font pointer types */

/* Per-vertex primitive renderers exposed for the edit_overlays
 * subsystem's overlay orchestration. Each draws ONE label / arrow at a
 * transformed position; iteration of the user's program and applying
 * transforms is the src/subsystems/edit_overlays/ responsibility (it walks
 * the program via replay_walk_user_vertices and calls these primitives at
 * each visit). That subsystem also sets up the surrounding GL state
 * (color, depth disable, push/pop attribs). */
void render3d_draw_vertex_label_text(float vx, float vy, float vz,
                                  const char *primary_text,
                                  const char *detail_text);
void render3d_draw_normal_vector_arrow(float vx, float vy, float vz,
                                    float nx, float ny, float nz,
                                    float scale);

/* Rich one-at-a-time normal glyph used for the cursor-focused normal and the
 * replay-focused normal. The dense "Normal Vectors" overlay intentionally
 * keeps using render3d_draw_normal_vector_arrow above. The normal is
 * normalized for display; zero-length normals draw only the anchor marker.
 * Text is optional and is placed at the normal endpoint. */
void render3d_draw_focused_normal_glyph(float vx, float vy, float vz,
                                     float nx, float ny, float nz,
                                     float scale, float alpha_scale,
                                     const char *primary_text,
                                     const char *detail_text);

/* Draw `str` at world position (x, y, z) using `font` (e.g. FONT_MONO
 * or FONT_SMALL — both `void *` GLUT bitmap pointers). Combines the
 * glRasterPos3f + per-character glutBitmapCharacter loop the scene
 * module repeats in light indicators, overlay labels, and the orbit
 * gizmo coord readout. Color is the caller's responsibility (set
 * glColor* before calling). */
void render3d_draw_bitmap_text(void *font, float x, float y, float z,
                            const char *str);

/* Outlines and vertex-point overlays are edit_overlays-subsystem passes, not
 * scene primitives. src/subsystems/edit_overlays/ walks authored vertices,
 * redraws generated glutSolid* geometry in polygon line/point mode where
 * needed, and chooses the surrounding GL state; src/scene/ only provides the
 * per-vertex label/arrow helpers above. */

#endif /* RENDER3D_OVERLAYS_H */
