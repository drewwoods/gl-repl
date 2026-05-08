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

#include "render_types.h"

/* REPL_OUTLINE_POLYGON_OFFSET_{FACTOR,UNITS} live in scene_render_types.h
 * so repl_export.c can write the same depth-bias constants into the
 * exported output.c without having to include a `scene_*` header
 * (which would trip check-controller-boundaries). */

/* Check whether a flat command block (starting at begin_idx) matches the cursor
 * context (for highlighting in polygon outlines). Returns 1 if the block's
 * source code includes the cursor line, 0 otherwise. is_tess indicates whether
 * the block is tessellation-related (gluTessCallback). Used to determine whether
 * to highlight or dim polygon outlines while editing. */
int  scene_overlay_flat_block_matches_cursor(int begin_idx, int is_tess,
                                             const SceneRenderConfig *cfg);

/* Render polygon outlines (wireframe edges of filled triangles/quads), color-coded
 * by matrix stack depth. show_current_poly controls whether to brighten outlines
 * for geometry matching the cursor block (1) or dim them (0). replay_tess_preview
 * enables special highlighting for tessellation callback geometry during replay.
 * Called during the overlay rendering phase of the frame. */
void scene_overlays_render_outlines(const FrameRenderContext *frame_ctx,
                                    int show_current_poly,
                                    int replay_tess_preview);

/* Per-vertex primitive renderers exposed for the controller's overlay
 * orchestration. Each draws ONE label / arrow at a transformed position;
 * iteration of the user's program and applying transforms is the
 * controller's responsibility (it walks the program via
 * repl_walk_user_vertices and calls these primitives at each visit).
 * The controller is also responsible for setting up the surrounding GL
 * state (color, depth disable, push/pop attribs). */
void scene_draw_vertex_number_label(int vertex_idx,
                                    float vx, float vy, float vz);
void scene_draw_normal_vector_arrow(float vx, float vy, float vz,
                                    float nx, float ny, float nz,
                                    float scale);

/* Render semi-transparent vertex point dots at each vertex position.
 * Points are alpha-blended so they don't obscure underlying geometry.
 * Line-mode primitives (GL_LINES, GL_LINE_STRIP, GL_LINE_LOOP) use a smaller
 * point radius to avoid covering the line itself. Also drives geometry and
 * transform guides inline, so they render at the correct model-matrix position.
 * No-ops when both show_vpoints and replay_vertex_points are false. */
void scene_overlays_render_vertex_points(const FrameRenderContext *frame_ctx);

#endif /* SCENE_OVERLAYS_H */
