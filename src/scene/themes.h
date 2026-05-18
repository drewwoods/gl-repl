/*
 * src/scene/themes.h - Shared scene theme enums.
 *
 * This is the vocabulary app-side config code and scene renderers share for
 * grid themes, axes themes, and the grid spacing/extent indices. The scene
 * module owns the meanings; app/UI code imports the enums to present and store
 * those choices.
 *
 * Label tables and render data keyed by these enums must stay in enum order.
 */
#ifndef SCENE_THEMES_H
#define SCENE_THEMES_H

typedef enum {
    GRID_THEME_OFF = 0,
    GRID_THEME_CLASSIC,
    GRID_THEME_FOG,
    GRID_THEME_TRON,
    GRID_THEME_EMBER,
    GRID_THEME_FAINT,
    GRID_THEME_FOCUS,
    GRID_THEME_OCEAN,
    GRID_THEME_XZRULER,
    GRID_THEME_PLANES,
    GRID_THEME_RADAR,
    GRID_THEME_COUNT
} SceneGridTheme;

typedef enum {
    AXES_THEME_OFF = 0,
    AXES_THEME_CLASSIC,
    AXES_THEME_PULSE,
    AXES_THEME_NEON,
    AXES_THEME_COMPASS,
    AXES_THEME_GIZMO,
    AXES_THEME_RULER,
    AXES_THEME_COUNT
} SceneAxesTheme;

/* Grid major-tick spacing index. The actual float values live in a table
 * the controller passes through SceneRenderConfig.grid_major_steps. */
typedef enum {
    GRID_MAJOR_1  = 0,
    GRID_MAJOR_2,
    GRID_MAJOR_5,
    GRID_MAJOR_10,
    GRID_MAJOR_COUNT
} SceneGridMajorIdx;

/* Grid half-extent from origin along each axis. Values live in
 * SceneRenderConfig.grid_extents and must match this enum order. */
typedef enum {
    GRID_EXTENT_CLOSE = 0,
    GRID_EXTENT_MID,
    GRID_EXTENT_FAR,
    GRID_EXTENT_COUNT
} SceneGridExtentIdx;

#endif /* SCENE_THEMES_H */
