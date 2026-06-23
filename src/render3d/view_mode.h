/*
 * view_mode.h - Projection view mode enum (2D ortho / 3D perspective).
 *
 * Consumed by the app-side presentation state and subsystems that need to
 * know whether the scene is currently in 2D or 3D mode.
 *
 * The X-macro list mirrors src/scene/themes.h so the cfg-symbol string
 * table in src/app/glr_actions.c can be derived from the same source.
 */
#ifndef SCENE_VIEW_MODE_H
#define SCENE_VIEW_MODE_H

#define SCENE_VIEW_LIST(X) \
    X(3D)                  \
    X(2D)

typedef enum {
#define SCENE_VIEW_ENUM_ENTRY(name) SCENE_VIEW_##name,
    SCENE_VIEW_LIST(SCENE_VIEW_ENUM_ENTRY)
#undef SCENE_VIEW_ENUM_ENTRY
    SCENE_VIEW_COUNT
} SceneViewMode;

/* Orthographic/perspective projection blend duration, in seconds, for callers
 * that animate between SceneViewMode values. */
#ifndef GLR_VIEW_PROJECTION_TRANSITION_SECS
#define GLR_VIEW_PROJECTION_TRANSITION_SECS 0.75f
#endif

#endif /* SCENE_VIEW_MODE_H */
