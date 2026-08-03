/*
 * view_mode.h - Projection view mode enum (2D ortho / 3D perspective).
 *
 * Consumed by callers and subsystems that need to know whether the scene is
 * currently in 2D or 3D mode.
 *
 * The X-macro list is kept beside the renderer's other vocabulary so a
 * caller's cfg-symbol table can be derived from the same source.
 */
#ifndef RENDER3D_VIEW_MODE_H
#define RENDER3D_VIEW_MODE_H

#define RENDER3D_VIEW_LIST(X) \
    X(3D)                  \
    X(2D)

typedef enum {
#define RENDER3D_VIEW_ENUM_ENTRY(name) RENDER3D_VIEW_##name,
    RENDER3D_VIEW_LIST(RENDER3D_VIEW_ENUM_ENTRY)
#undef RENDER3D_VIEW_ENUM_ENTRY
    RENDER3D_VIEW_COUNT
} Render3dViewMode;

#endif /* RENDER3D_VIEW_MODE_H */
