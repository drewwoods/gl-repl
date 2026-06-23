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
#ifndef RENDER3D_THEMES_H
#define RENDER3D_THEMES_H

/* X-macro lists drive the enum *and* any cfg-symbol string table that
 * needs to round-trip the value name (see cfg_grid_theme_symbols et al.
 * in src/app/glr_actions.c). Adding a new theme/backdrop here picks it
 * up everywhere automatically. */
#define GRID_THEME_LIST(X) \
    X(OFF, "OFF")          \
    X(CLASSIC, "Classic")  \
    X(TRON, "Tron")        \
    X(EMBER, "Ember")      \
    X(OCEAN, "Ocean")      \
    X(XZRULER, "XZ Ruler") \
    X(PLANES, "Adaptive Planes") \
    X(RADAR, "Radar")      \
    X(AURORA, "Aurora")    \
    X(SYNTHWAVE, "Synthwave") \
    X(FROZEN, "Frozen Lake") \
    X(SOIL, "Tilled Field") \
    X(STARCHART, "Star Chart")

#define GRID_THEME_ENUM_ENTRY(name, str) GRID_THEME_##name,
#define GRID_THEME_NAME_ENTRY(name, str) [GRID_THEME_##name] = str,

typedef enum {
    GRID_THEME_LIST(GRID_THEME_ENUM_ENTRY)
    GRID_THEME_COUNT
} Render3dGridTheme;

#define AXES_THEME_LIST(X) \
    X(OFF, "OFF")          \
    X(CLASSIC, "Classic")  \
    X(PULSE, "Pulse")      \
    X(NEON, "Neon")        \
    X(COMPASS, "Compass")  \
    X(GIZMO, "Gizmo")      \
    X(RULER, "Ruler")

#define AXES_THEME_ENUM_ENTRY(name, str) AXES_THEME_##name,
#define AXES_THEME_NAME_ENTRY(name, str) [AXES_THEME_##name] = str,

typedef enum {
    AXES_THEME_LIST(AXES_THEME_ENUM_ENTRY)
    AXES_THEME_COUNT
} Render3dAxesTheme;

#define RENDER3D_BACKDROP_LIST(X) \
    X(OFF, "Off")              \
    X(CITYSCAPE, "Cityscape")  \
    X(STARS, "Stars")          \
    X(CITY_AND_STARS, "City+Stars") \
    X(SUNSET, "Sunset")        \
    X(AURORA, "Aurora")        \
    X(NEBULA, "Nebula")        \
    X(POLAR_DAY, "Polar Day")  \
    X(SNOWFALL, "Snowfall")    \
    X(POLAR_DAY_SNOW, "Polar Day+Snow")

#define RENDER3D_BACKDROP_ENUM_ENTRY(name, str) RENDER3D_BACKDROP_##name,
#define RENDER3D_BACKDROP_NAME_ENTRY(name, str) [RENDER3D_BACKDROP_##name] = str,

typedef enum {
    RENDER3D_BACKDROP_LIST(RENDER3D_BACKDROP_ENUM_ENTRY)
    RENDER3D_BACKDROP_COUNT
} Render3dBackdropMode;

/* Lighting environment preset. DEFAULT is the classic three-coloured-key
 * + disabled rim layout. HEADLIGHT places light 0 in eye space (at the
 * camera, set with an identity modelview) so the scene self-illuminates
 * as the camera moves. SOLAR puts light 0 at the world origin so user
 * geometry orbits a single central source — useful for solar-system /
 * planet renders. STUDIO is the three-point portrait rig (warm-white key,
 * cool-blue rim, warm-orange fill) plus a green directional accent,
 * mirroring the tools/render3d_demo lighting. NEON is a vibrant saturated
 * triad (magenta key, cyan rim, lime fill) plus a dim warm back light,
 * for showing off colored materials. As with every theme, all four
 * slots ship `.enabled = 0` — a theme only defines each light's
 * position/colors; the program's glEnable(GL_LIGHTn) commands (or an
 * example's @cfg) decide which slots actually light up. */
#define LIGHT_THEME_LIST(X) \
    X(DEFAULT, "Default")   \
    X(HEADLIGHT, "Headlight") \
    X(SOLAR, "Solar")       \
    X(STUDIO, "Studio")     \
    X(NEON, "Neon")

#define LIGHT_THEME_ENUM_ENTRY(name, str) LIGHT_THEME_##name,
#define LIGHT_THEME_NAME_ENTRY(name, str) [LIGHT_THEME_##name] = str,

typedef enum {
    LIGHT_THEME_LIST(LIGHT_THEME_ENUM_ENTRY)
    LIGHT_THEME_COUNT
} Render3dLightTheme;

/* Grid major-tick spacing index. The actual float values live in a table
 * the controller passes through Render3dRenderConfig.grid_major_steps. */
#define GRID_MAJOR_LIST(X) \
    X(1, "1")              \
    X(2, "2")              \
    X(5, "5")              \
    X(10, "10")

#define GRID_MAJOR_ENUM_ENTRY(name, str) GRID_MAJOR_##name,
#define GRID_MAJOR_NAME_ENTRY(name, str) [GRID_MAJOR_##name] = str,

typedef enum {
    GRID_MAJOR_LIST(GRID_MAJOR_ENUM_ENTRY)
    GRID_MAJOR_COUNT
} Render3dGridMajor;

/* Grid half-extent from origin along each axis. Values live in
 * Render3dRenderConfig.grid_extents and must match this enum order. */
#define GRID_EXTENT_LIST(X) \
    X(CLOSE, "Close")       \
    X(MID, "Mid")           \
    X(FAR, "Far")

#define GRID_EXTENT_ENUM_ENTRY(name, str) GRID_EXTENT_##name,
#define GRID_EXTENT_NAME_ENTRY(name, str) [GRID_EXTENT_##name] = str,

typedef enum {
    GRID_EXTENT_LIST(GRID_EXTENT_ENUM_ENTRY)
    GRID_EXTENT_COUNT
} Render3dGridExtent;

/* Grid line-brightness multiplier index. The default grid-line alphas are
 * deliberately faint (minor lines ~0.03..0.12); this scales them so a
 * grid can be dialed up (or down) for contrast against the backdrop. The
 * actual multiplier values live in the controller (glr_ctrl.c), resolved
 * into Render3dRenderConfig.grid_brightness — NORMAL == 1.0 (no change). */
#define GRID_BRIGHTNESS_LIST(X) \
    X(DIM,    "Dim")            \
    X(NORMAL, "Normal")         \
    X(BRIGHT, "Bright")         \
    X(BOLD,   "Bold")

#define GRID_BRIGHTNESS_ENUM_ENTRY(name, str) GRID_BRIGHTNESS_##name,
#define GRID_BRIGHTNESS_NAME_ENTRY(name, str) [GRID_BRIGHTNESS_##name] = str,

typedef enum {
    GRID_BRIGHTNESS_LIST(GRID_BRIGHTNESS_ENUM_ENTRY)
    GRID_BRIGHTNESS_COUNT
} Render3dGridBrightness;

#endif /* RENDER3D_THEMES_H */
