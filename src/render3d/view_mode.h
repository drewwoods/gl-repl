/*
 * view_mode.h - Projection view mode enum (2D ortho / 3D perspective).
 *
 * Consumed by the app-side presentation state and subsystems that need to
 * know whether the scene is currently in 2D or 3D mode.
 *
 * The X-macro list mirrors src/scene/themes.h so the cfg-symbol string
 * table in src/app/glr_actions.c can be derived from the same source.
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

/* Orthographic/perspective projection blend duration, in seconds, for callers
 * that animate between Render3dViewMode values. */
#ifndef GLR_VIEW_PROJECTION_TRANSITION_SECS
#define GLR_VIEW_PROJECTION_TRANSITION_SECS 0.75f
#endif

#endif /* RENDER3D_VIEW_MODE_H */
