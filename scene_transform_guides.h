/*
 * scene_transform_guides.h - transform edit-guide planning/rendering API.
 */
#ifndef SCENE_TRANSFORM_GUIDES_H
#define SCENE_TRANSFORM_GUIDES_H

#include "scene_guides_shared.h"

int scene_transform_guides_prepare(const SceneGuideSnapshot *snapshot,
                                   SceneTransformGuidePlan *plan);
void scene_transform_guides_render_if_due(const SceneGuideSnapshot *snapshot,
                                          SceneTransformGuidePlan *plan,
                                          int flat_cmd_idx,
                                          const float cam_view[16]);

#endif /* SCENE_TRANSFORM_GUIDES_H */
