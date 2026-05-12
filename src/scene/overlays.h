/*
 * scene_overlays.h - Visual overlays for geometry debugging and feedback.
 *
 * Renders optional visual overlays on top of user geometry: polygon outlines
 * (highlighting current editing context), vertex numbers (for reference),
 * normal vectors (showing surface orientation), and vertex position guides
 * (cross-hairs at vertices). These overlays help users understand and debug
 * their geometry in real-time.
 *
 * Overlay types:
 *   - Polygon outlines: Wireframe edges of filled triangles/quads, color-coded
 *     by matrix stack depth (so geometry drawn under different transforms has
 *     distinct outline colors). Used to visualize polygon structure and
 *     transformation context.
 *   - Current block highlight: If a specific block (for-loop, function, etc.)
 *     is selected, outlines are brighter for geometry from that block,
 *     dimmer for others. Helps navigate the code while watching geometry
 *     appear/disappear.
 *   - Vertex numbers: Small text labels showing vertex index at each vertex
 *     position, useful for debugging index order and primitive winding.
 *   - Normal vectors: Colored lines extending from each vertex showing
 *     surface normal direction, color-coded by depth for visual clarity.
 *   - Vertex guides (F2): Cross-hairs at each vertex for precise alignment
 *     and measurement (not implemented in minimal builds).
 *
 * Replay integration: During replay (step-through mode), a special "tess preview"
 * mode highlights tessellation callback geometry being added, helping users
 * understand tessellation output.
 *
 * Visibility: Each overlay is toggleable via config (polygon outlines = F5,
 * vertex numbers = F6, normal vectors = F7, vertex guides = F2). Overlays
 * respect the depth-masking state so they don't interfere with orbit target
 * highlighting or other UI elements.
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

/* Note: outlines and vertex-point overlays no longer live in this header.
 * They moved to imrepl_ctrl.c where they're implemented by re-executing
 * the user's program with glPolygonMode(GL_LINE) / GL_POINT respectively
 * — the gluTessCallback edge-flag handler in src/repl/executor.c keeps
 * triangulation interiors invisible in GL_LINE mode, so no GLCmd
 * iteration is needed. The visual style (currently black with lighting,
 * later stencil-based) is the controller's choice; the scene module is
 * not involved. */

#endif /* SCENE_OVERLAYS_H */
