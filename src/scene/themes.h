/*
 * src/scene/themes.h - Shared scene theme enums.
 *
 * This is the vocabulary app-side config code and scene renderers share for
 * grid themes, axes themes, backdrop modes, and the grid spacing/extent
 * indices. The scene module owns the meanings; app/UI code imports the enums
 * to present and store those choices.
 *
 * Label tables and render data keyed by these enums must stay in enum order.
 */
#ifndef SCENE_THEMES_H
#define SCENE_THEMES_H

/* X-macro lists drive the enum *and* any cfg-symbol string table that
 * needs to round-trip the value name (see cfg_grid_theme_symbols et al.
 * in src/app/glr_actions.c). Adding a new theme/backdrop here picks it
 * up everywhere automatically. */
#define GRID_THEME_LIST(X) \
    X(OFF)                 \
    X(CLASSIC)             \
    X(FOG)                 \
    X(TRON)                \
    X(EMBER)               \
    X(FAINT)               \
    X(FOCUS)               \
    X(OCEAN)               \
    X(XZRULER)             \
    X(PLANES)              \
    X(RADAR)

typedef enum {
#define GRID_THEME_ENUM_ENTRY(name) GRID_THEME_##name,
    GRID_THEME_LIST(GRID_THEME_ENUM_ENTRY)
#undef GRID_THEME_ENUM_ENTRY
    GRID_THEME_COUNT
} SceneGridTheme;

#define AXES_THEME_LIST(X) \
    X(OFF)                 \
    X(CLASSIC)             \
    X(PULSE)               \
    X(NEON)                \
    X(COMPASS)             \
    X(GIZMO)               \
    X(RULER)

typedef enum {
#define AXES_THEME_ENUM_ENTRY(name) AXES_THEME_##name,
    AXES_THEME_LIST(AXES_THEME_ENUM_ENTRY)
#undef AXES_THEME_ENUM_ENTRY
    AXES_THEME_COUNT
} SceneAxesTheme;

#define SCENE_BACKDROP_LIST(X) \
    X(OFF)                     \
    X(CITYSCAPE)               \
    X(STARS)                   \
    X(CITY_AND_STARS)

typedef enum {
#define SCENE_BACKDROP_ENUM_ENTRY(name) SCENE_BACKDROP_##name,
    SCENE_BACKDROP_LIST(SCENE_BACKDROP_ENUM_ENTRY)
#undef SCENE_BACKDROP_ENUM_ENTRY
    SCENE_BACKDROP_COUNT
} SceneBackdropMode;

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
