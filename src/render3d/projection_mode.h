/*
 * projection_mode.h - Projection mode enum (perspective / orthographic).
 *
 * Consumed by the app-side presentation state, render3d, and config systems.
 */
#ifndef RENDER3D_PROJECTION_MODE_H
#define RENDER3D_PROJECTION_MODE_H

#define PROJ_LIST(X) \
    X(PERSPECTIVE)   \
    X(ORTHO)

typedef enum {
#define PROJ_ENUM_ENTRY(name) PROJ_##name,
    PROJ_LIST(PROJ_ENUM_ENTRY)
#undef PROJ_ENUM_ENTRY
    PROJ_COUNT
} Render3dProjectionMode;

#endif /* RENDER3D_PROJECTION_MODE_H */
