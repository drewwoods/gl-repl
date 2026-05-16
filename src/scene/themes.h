/*
 * src/scene/themes.h - scene rendering theme enums (grid + axes).
 *
 * These describe scene-side rendering modes (grid line theme, axes theme,
 * grid spacing / extent indices). They live in the scene/ tree because
 * the scene module owns the rendering; the REPL controller imports this
 * header to wire its config UI to scene values.
 *
 * The label tables in repl_actions.c, src/scene/grid.c, and the custom
 * render paths in src/scene/grid.c / src/scene/axes.c must stay in sync
 * with these enums.
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
} GridTheme;

typedef enum {
    AXES_THEME_OFF = 0,
    AXES_THEME_CLASSIC,
    AXES_THEME_PULSE,
    AXES_THEME_NEON,
    AXES_THEME_COMPASS,
    AXES_THEME_GIZMO,
    AXES_THEME_RULER,
    AXES_THEME_COUNT
} AxesTheme;

/* Grid major-tick spacing index. The actual float values live in a table
 * the controller passes through SceneRenderConfig.grid_major_steps. */
typedef enum {
    GRID_MAJOR_1  = 0,
    GRID_MAJOR_2,
    GRID_MAJOR_5,
    GRID_MAJOR_10,
    GRID_MAJOR_COUNT
} GridMajorIdx;

/* Grid half-extent from origin along each axis. Values live in
 * SceneRenderConfig.grid_extents and must match this enum order. */
typedef enum {
    GRID_EXTENT_CLOSE = 0,
    GRID_EXTENT_MID,
    GRID_EXTENT_FAR,
    GRID_EXTENT_COUNT
} GridExtentIdx;

#endif /* SCENE_THEMES_H */
