/*
 * scene_transform_guides.h - Transform visualization guides during replay.
 *
 * Renders edit guides that visualize pending matrix operations (glTranslatef,
 * glRotatef, glScalef, glPushMatrix, glPopMatrix) during replay step-through.
 * Guides show vectors representing translations, rotation axes, scale factors,
 * and matrix stack depth changes, helping users understand how transforms affect
 * subsequent geometry.
 *
 * Guide types:
 *   - Translation vectors: Arrows showing glTranslatef direction/magnitude
 *   - Rotation axes: Lines showing the axis and direction of rotation
 *   - Scale factors: Labeled indicators showing scale amounts
 *   - Matrix stack visualization: Indentation/nesting indicators for Push/Pop
 *
 * Planning: scene_transform_guides_prepare() analyzes the current snapshot of
 * geometry and matrix stack state to determine which guides to render.
 * SceneTransformGuidePlan caches the guide data so render_if_due() can
 * efficiently draw it. This separation allows preparation to happen once and
 * rendering to happen conditionally (only when guides are visible or changed).
 *
 * Rendering: scene_transform_guides_render_if_due() draws guides for a specific
 * flat command (if it's a transform command). Guides are drawn in the camera
 * view coordinate system so they appear stable in the viewport even as the
 * scene rotates/pans. Rendered with transparency so they don't fully occlude
 * geometry.
 *
 * Integration: Called during replay rendering phase (when stepping through
 * commands one by one). Not rendered during normal playback (too much visual
 * clutter). Guides are optional (can be disabled via config).
 */
#ifndef SCENE_TRANSFORM_GUIDES_H
#define SCENE_TRANSFORM_GUIDES_H

#include "guides_shared.h"

/* Prepare transform guides for rendering. Analyzes the geometry snapshot and
 * matrix state to determine which guides are needed. Caches guide data in the
 * plan. Called once per frame before rendering guides. Returns 1 if guides
 * were prepared, 0 if no guides are needed. snapshot describes the current
 * scene state; plan is filled with guide rendering data. */
int scene_transform_guides_prepare(const SceneGuideSnapshot *snapshot,
                                   SceneTransformGuidePlan *plan);

/* Render transform guides for a specific flat command (if it's a transform).
 * Draws vectors/axes/indicators for the transform operation. Rendering only
 * happens if guides are enabled and the command is a transform. snapshot and
 * plan describe the scene state (from prepare()). flat_cmd_idx is the command
 * being rendered. cam_view is the camera view matrix for viewport-aligned
 * rendering. Called during guide rendering phase for each command. */
void scene_transform_guides_render_if_due(const SceneGuideSnapshot *snapshot,
                                          SceneTransformGuidePlan *plan,
                                          int flat_cmd_idx,
                                          const float cam_view[16]);

#endif /* SCENE_TRANSFORM_GUIDES_H */
