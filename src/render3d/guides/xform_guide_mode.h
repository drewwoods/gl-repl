/*
 * xform_guide_mode.h - Transform guide display mode enum.
 *
 * Cycles the transform-guide overlay across off, world-aligned, and
 * local-frame alignment. Consumed by transform_guides.c and its callers;
 * OFF is the disabled mode.
 *
 * The X-macro list is kept beside the renderer's other vocabulary so a
 * caller's cfg-symbol table can be derived from the same source.
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
