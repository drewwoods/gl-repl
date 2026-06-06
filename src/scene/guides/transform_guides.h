/*
 * transform_guides.h - Replay/edit-time transform guide rendering.
 *
 * These helpers visualize pending transform commands near the current replay or
 * cursor context. The controller builds a SceneGuideSnapshot, calls
 * scene_transform_guides_prepare() once per frame, then lets the renderer draw any
 * guide that applies while walking the flat program.
 */
#ifndef SCENE_TRANSFORM_GUIDES_H
#define SCENE_TRANSFORM_GUIDES_H

#include "guides_shared.h"

/* Analyze the current guide snapshot and fill `plan` with the transform-guide
 * state needed for this frame. Returns 1 when a guide pass should run, 0 when
 * there is nothing to draw. */
int scene_transform_guides_prepare(const SceneGuideSnapshot *snapshot,
                                   SceneTransformGuidePlan *plan);

/* Render any transform guide associated with `flat_cmd_idx`. `snapshot` and
 * `plan` come from the current frame; `cam_view` is the current camera-aligned
 * view matrix used to keep guide geometry stable in screen space. */
void scene_transform_guides_render_if_due(const SceneGuideSnapshot *snapshot,
                                          SceneTransformGuidePlan *plan,
                                          int flat_cmd_idx,
                                          const float cam_view[16]);

#endif /* SCENE_TRANSFORM_GUIDES_H */