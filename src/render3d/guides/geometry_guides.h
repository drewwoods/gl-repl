/*
 * geometry_guides.h - Cursor-context geometry guide rendering.
 *
 * Draws the geometry guides associated with the current edit row: vertex
 * markers, primitive context, and the other immediate cues the scene overlay
 * can derive from Render3dGuideSnapshot. The controller prepares that snapshot;
 * this renderer stays read-only over it.
 */
#ifndef RENDER3D_GEOMETRY_GUIDES_H
#define RENDER3D_GEOMETRY_GUIDES_H

#include "guides_shared.h"

/* Render geometry guides for the cursor's current context. Called during the
 * scene's guide overlay phase when geometry guides are enabled. */
void render3d_geometry_guides_render_for_cursor(const Render3dGuideSnapshot *snapshot);

#endif /* RENDER3D_GEOMETRY_GUIDES_H */