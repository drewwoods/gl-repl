/*
 * scene_overlays.h - Tiny per-vertex overlay primitives.
 *
 * These are the scene module's narrow overlay exports: draw one vertex-number
 * label or one normal-vector arrow at an already-transformed position.
 * Higher-level policy such as deciding whether overlays are enabled, walking
 * the user's program, tracking transforms, or bracketing GL state belongs to
 * the controller.
 */
#ifndef SCENE_OVERLAYS_H
#define SCENE_OVERLAYS_H

/* Per-vertex primitive renderers exposed for the controller's overlay
 * orchestration. Each draws ONE label / arrow at a transformed position;
 * iteration of the user's program and applying transforms is the
 * controller's responsibility (it walks the program via
 * replay_walk_user_vertices and calls these primitives at each visit).
 * The controller is also responsible for setting up the surrounding GL
 * state (color, depth disable, push/pop attribs). */
void scene_draw_vertex_number_label(int vertex_idx,
                                    float vx, float vy, float vz);
void scene_draw_normal_vector_arrow(float vx, float vy, float vz,
                                    float nx, float ny, float nz,
                                    float scale);

/* Outlines and vertex-point overlays are controller-owned passes, not scene
 * primitives. src/app/glr_ctrl.c re-executes the user's geometry in GL_LINE or
 * GL_POINT mode and chooses the surrounding GL state; src/scene/ only provides
 * the per-vertex label/arrow helpers above. */

#endif /* SCENE_OVERLAYS_H */
