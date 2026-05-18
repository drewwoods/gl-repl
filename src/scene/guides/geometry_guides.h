/*
 * scene_geometry_guides.h - Cursor-context geometry guide rendering.
 *
 * Draws the geometry guides associated with the current edit row: vertex
 * markers, primitive context, and the other immediate cues the scene overlay
 * can derive from SceneGuideSnapshot. The controller prepares that snapshot;
 * this renderer stays read-only over it.
 */
#ifndef SCENE_GEOMETRY_GUIDES_H
#define SCENE_GEOMETRY_GUIDES_H

#include "guides_shared.h"

/* Render geometry guides for the cursor's current context. Called during the
 * scene's guide overlay phase when geometry guides are enabled. */
void geometry_guides_render_for_cursor(const SceneGuideSnapshot *snapshot);

#endif /* SCENE_GEOMETRY_GUIDES_H */