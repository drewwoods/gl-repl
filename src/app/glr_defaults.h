/*
 * glr_defaults.h - Controller-side scene / presentation defaults.
 *
 * Compile-time defaults for scene-presentation state that examples
 * are allowed to override via leading `// @cfg` metadata. Reset
 * helpers and tests reuse these to stay aligned with the global
 * definitions in src/repl/core.c without drifting.
 *
 * This is gl-repl app-shell logic — it knits together scene-render
 * enums and the editor's panel layout into a single set of "out of
 * the box" values. Pure rendering enums themselves (GridTheme,
 * AxesTheme, GridMajorIdx, GridExtentIdx) live in src/scene/themes.h;
 * the panel-layout enum lives in src/ui/layout.h. Including those
 * here is intentional — only callers that need these defaults pay
 * the transitive cost, keeping config.h dependency-free.
 *
 * Concept: compile-time defaults. User-toggleable runtime settings
 * (wireframe / grid theme / etc.) live on repl_config.h and the
 * g_cfg_items[] descriptor table in glr_actions.c — different
 * concept, do not conflate.
 */
#ifndef GLR_DEFAULTS_H
#define GLR_DEFAULTS_H

#include "scene/themes.h"   /* GRID_MAJOR_*, GRID_EXTENT_* enum values */
#include "ui/layout.h"      /* CODE_PANEL_LAYOUT_* enum values */

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
#define CFG_DEFAULT_VARIABLE_PANEL    1
#define CFG_DEFAULT_WRAP_AT_COMMA     1
#define CFG_DEFAULT_CODE_PANEL_LAYOUT CODE_PANEL_LAYOUT_TOP

/* Render-side config defaults the REPL state initializer feeds into
 * the scene config. These are controller-policy defaults, not
 * scene-internal — the scene module accepts whatever value the caller
 * passes. */
#define CFG_DEFAULT_MULTISAMPLE       1
#define CFG_DEFAULT_LINE_SMOOTH       0
#define CFG_DEFAULT_ATTENUATE_POINTS  1

#endif /* GLR_DEFAULTS_H */
