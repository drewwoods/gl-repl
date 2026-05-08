/*
 * scene_geometry_guides.h - Geometry context guides at cursor position.
 *
 * Renders visual guides showing the immediate geometry context around the
 * currently editing position (cursor line). Guides help users understand what
 * geometry their current edits affect: bounding boxes, vertex clouds, axes
 * showing primitive orientation, etc.
 *
 * Guide types:
 *   - Bounding box: Wireframe box around all vertices in the current editing
 *     context (e.g., all vertices from the current for-loop or function call)
 *   - Vertex cloud: Small points or cross-hairs at each vertex position
 *   - Primitive axes: Arrows showing surface orientation and winding order
 *   - Editing markers: Highlights the specific primitive being edited
 *
 * Cursor context: The "context" is determined by the cursor line:
 *   - If inside a for-loop, guides show geometry from that loop iteration
 *   - If inside a function, guides show the function's geometry
 *   - If on a glVertex/glColor/glNormal line, guides highlight that specific
 *     command's effect
 *
 * Visibility: Guides are optional (can be disabled via config). When enabled,
 * they provide real-time visual feedback about what the current code will
 * produce, helping with debugging and understanding the geometric structure.
 *
 * Integration: Called during editing (both normal and replay mode). Reads the
 * cursor position from editor state and renders guides for that context.
 * Rendered as semi-transparent overlays so they don't fully occlude geometry.
 */
#ifndef SCENE_GEOMETRY_GUIDES_H
#define SCENE_GEOMETRY_GUIDES_H

#include "guides_shared.h"

/* Render geometry guides for the cursor's current context. Shows the immediate
 * geometry that the cursor line(s) will produce: bounding box, vertex positions,
 * primitive orientations, etc. snapshot describes the current scene state
 * (geometry snapshot, cursor position). Called during guide rendering phase
 * if geometry guides are enabled. Provides real-time visual feedback for code
 * editing. */
void geometry_guides_render_for_cursor(const SceneGuideSnapshot *snapshot);

#endif /* SCENE_GEOMETRY_GUIDES_H */
