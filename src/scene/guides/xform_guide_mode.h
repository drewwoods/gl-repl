/*
 * xform_guide_mode.h - Transform guide display mode enum.
 *
 * Cycles the transform-guide overlay across off, world-aligned, and
 * local-frame alignment. Consumed by transform_guides.c and the app-side
 * presentation state; OFF replaces the old separate show/hide toggle.
 *
 * The X-macro list mirrors src/scene/themes.h so the cfg-symbol string
 * table in src/app/glr_actions.c can be derived from the same source.
 */
#ifndef SCENE_XFORM_GUIDE_MODE_H
#define SCENE_XFORM_GUIDE_MODE_H

#define SCENE_XFORM_GUIDE_LIST(X) \
    X(OFF)                        \
    X(WORLD)                      \
    X(FRAME)

typedef enum {
#define SCENE_XFORM_GUIDE_ENUM_ENTRY(name) SCENE_XFORM_GUIDE_##name,
    SCENE_XFORM_GUIDE_LIST(SCENE_XFORM_GUIDE_ENUM_ENTRY)
#undef SCENE_XFORM_GUIDE_ENUM_ENTRY
    SCENE_XFORM_GUIDE_COUNT
} SceneXformGuideMode;

#endif /* SCENE_XFORM_GUIDE_MODE_H */
