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
#ifndef RENDER3D_XFORM_GUIDE_MODE_H
#define RENDER3D_XFORM_GUIDE_MODE_H

#define RENDER3D_XFORM_GUIDE_LIST(X) \
    X(OFF)                        \
    X(WORLD)                      \
    X(FRAME)

typedef enum {
#define RENDER3D_XFORM_GUIDE_ENUM_ENTRY(name) RENDER3D_XFORM_GUIDE_##name,
    RENDER3D_XFORM_GUIDE_LIST(RENDER3D_XFORM_GUIDE_ENUM_ENTRY)
#undef RENDER3D_XFORM_GUIDE_ENUM_ENTRY
    RENDER3D_XFORM_GUIDE_COUNT
} Render3dXformGuideMode;

#endif /* RENDER3D_XFORM_GUIDE_MODE_H */
