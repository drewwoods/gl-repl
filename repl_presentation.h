/*
 * repl_presentation.h - Scene presentation enums and shared defaults.
 */
#ifndef REPL_PRESENTATION_H
#define REPL_PRESENTATION_H

#include "ui/layout.h"

/* Grid themes. The label tables in repl_actions.c, scene_grid.c, and the
 * custom render paths in scene_render.c must stay in sync with this enum. */
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
    GRID_THEME_COUNT
} GridTheme;

/* Axes themes. The label table in repl_actions.c and scene_axes.c must stay in
 * sync with this enum. */
typedef enum {
    AXES_THEME_OFF = 0,
    AXES_THEME_CLASSIC,
    AXES_THEME_PULSE,
    AXES_THEME_NEON,
    AXES_THEME_COMPASS,
    AXES_THEME_GIZMO,
    AXES_THEME_COUNT
} AxesTheme;

/* Grid major tick spacing. Values live in g_grid_major_steps[] and
 * must match this enum order. */
typedef enum {
    GRID_MAJOR_1  = 0,
    GRID_MAJOR_2,
    GRID_MAJOR_5,
    GRID_MAJOR_10,
    GRID_MAJOR_COUNT
} GridMajorIdx;

/* Grid half-extent from origin along each axis. Values live in
 * g_grid_extents[] and must match this enum order. */
typedef enum {
    GRID_EXTENT_CLOSE = 0,
    GRID_EXTENT_MID,
    GRID_EXTENT_FAR,
    GRID_EXTENT_COUNT
} GridExtentIdx;

/* Default values for scene-presentation state that examples are allowed to
 * override via leading metadata. Keep these aligned with the global
 * definitions in repl_core.c and reuse them from reset helpers/tests to avoid
 * drift. */
#define CFG_DEFAULT_WIREFRAME         0
#define CFG_DEFAULT_GRID_THEME        8
#define CFG_DEFAULT_GRID_MAJOR_IDX    GRID_MAJOR_1
#define CFG_DEFAULT_GRID_EXTENT_IDX   GRID_EXTENT_FAR
#define CFG_DEFAULT_AXES_THEME        0
#define CFG_DEFAULT_VERTEX_LABELS     1
#define CFG_DEFAULT_VERTEX_INDICES    1
#define CFG_DEFAULT_NORMAL_VECTORS    0
#define CFG_DEFAULT_VERTEX_OUTLINES   1
#define CFG_DEFAULT_VERTEX_POINTS     1
#define CFG_DEFAULT_VERTEX_GUIDES     1
#define CFG_DEFAULT_XFORM_GUIDE_MODE  0
#define CFG_DEFAULT_LIGHT_INDICATORS  1
#define CFG_DEFAULT_BACKDROP_MODE     0
#define CFG_DEFAULT_CAMERA_ROTATE     0
#define CFG_DEFAULT_WRAP_AT_COMMA     1
#define CFG_DEFAULT_CODE_PANEL_LAYOUT CODE_PANEL_LAYOUT_TOP

/* Render-side config defaults — these were briefly housed in
 * scene_render.h, but the reset helpers and ReplRenderState
 * initializer in repl_state.c need them, and a `repl_*.c` file
 * cannot include a `scene_*` header (check-controller-boundaries).
 * Living here keeps them adjacent to the rest of the CFG_DEFAULT_*
 * block and reachable by both the scene renderer (via
 * scene_render_types.h) and the REPL state defaults. */
#define CFG_DEFAULT_MULTISAMPLE       1
#define CFG_DEFAULT_LINE_SMOOTH       0
#define CFG_DEFAULT_ATTENUATE_POINTS  1

#endif /* REPL_PRESENTATION_H */