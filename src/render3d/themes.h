/*
 * src/render3d/themes.h - Shared 3D theme enums.
 *
 * This is the renderer-owned vocabulary for grid themes, axes themes, backdrop
 * modes, post-filter modes, and grid spacing/extent indices. Callers import the
 * enums to present and store those choices; the renderer owns their meanings.
 *
 * Naming convention for symbols in src/render3d/:
 * - New enumerators and public constants must use the RENDER3D_* prefix.
 * - Do not introduce GLR_* symbols in this directory (those belong to the app).
 * - The existing GRID_THEME_*, AXES_THEME_*, WIREFRAME_*, and PROJ_* sets are
 *   preserved as the canonical cfg X-macro source.
 *
 * Label tables and render data keyed by these enums must stay in enum order.
 */
#ifndef RENDER3D_THEMES_H
#define RENDER3D_THEMES_H

/* Scene-viewport post-process filter modes. */
typedef enum Render3dPostFilterMode {
    RENDER3D_POST_FILTER_OFF = 0,
    RENDER3D_POST_FILTER_CHROMATIC_ABERRATION,
    RENDER3D_POST_FILTER_VIGNETTE,
    RENDER3D_POST_FILTER_SCANLINES,
    RENDER3D_POST_FILTER_FILM_GRAIN,
    RENDER3D_POST_FILTER_COUNT
} Render3dPostFilterMode;

/* X-macro lists drive the enum *and* any cfg-symbol string table that
 * needs to round-trip the value name. Adding a new theme/backdrop here picks
 * it up everywhere automatically. */
#define GRID_THEME_LIST(X) \
    X(OFF, "OFF")          \
    X(CLASSIC, "Classic")  \
    X(TRON, "Tron")        \
    X(EMBER, "Ember")      \
    X(OCEAN, "Ocean")      \
    X(XZRULER, "XZ Ruler") \
    X(RADAR, "Radar")      \
    X(AURORA, "Aurora")    \
    X(SYNTHWAVE, "Synthwave") \
    X(FROZEN, "Frozen Lake") \
    X(SOIL, "Tilled Field") \
    X(STARCHART, "Star Chart") \
    X(PLANES, "Adaptive Planes") \
    X(SKETCH, "Sketchbook")  \
    X(NEON, "Neon Graph")    \
    X(GRAPHPLANES, "Graph Planes") \
    X(CHECKER, "Checkerboard")

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
    X(RULER, "Ruler")      \
    X(ARROW, "Arrow")      \
    X(FOUNTAIN, "Fountain")

#define AXES_THEME_ENUM_ENTRY(name, str) AXES_THEME_##name,
#define AXES_THEME_NAME_ENTRY(name, str) [AXES_THEME_##name] = str,

typedef enum {
    AXES_THEME_LIST(AXES_THEME_ENUM_ENTRY)
    AXES_THEME_COUNT
} Render3dAxesTheme;

#define RENDER3D_BACKDROP_LIST(X) \
    X(OFF, "Off")              \
    X(STARS, "Stars")          \
    X(CITY_AND_STARS, "City+Stars") \
    X(SUNSET, "Sunset")        \
    X(AURORA, "Aurora")        \
    X(NEBULA, "Nebula")        \
    X(POLAR_DAY, "Polar Day")  \
    X(DRONES, "Drones") \
    X(FAIRIES, "Fairies")

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
 * geometry orbits a single central source - useful for solar-system /
 * planet renders. STUDIO is the three-point portrait rig (warm-white key,
 * cool-blue rim, warm-orange fill) plus a green directional accent,
 * mirroring the tools/render3d_demo lighting. NEON is a vibrant saturated
 * triad (magenta key, cyan rim, lime fill) plus a dim warm back light,
 * for showing off colored materials. As with every theme, all four
 * slots ship `.enabled = 0` - a theme only defines each light's
 * position/colors; the caller's glEnable(GL_LIGHTn) commands or other policy
 * decide which slots actually light up. */
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

/* Grid major-tick spacing index. The actual float values are supplied by the
 * caller through Render3dRenderConfig.grid_major_steps. */
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
 * actual multiplier values are supplied by the caller through
 * Render3dRenderConfig.grid_brightness - NORMAL == 1.0 (no change). */
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
