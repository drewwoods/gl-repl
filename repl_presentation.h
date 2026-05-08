/*
 * repl_presentation.h - REPL controller-side presentation defaults.
 *
 * Pure scene-rendering enums (GridTheme, AxesTheme, GridMajorIdx,
 * GridExtentIdx) live in src/scene/themes.h — they describe scene
 * behavior, not REPL state. This header re-exports them and adds the
 * controller-level defaults that drive the REPL config UI and the
 * example-load reset path.
 */
#ifndef REPL_PRESENTATION_H
#define REPL_PRESENTATION_H

#include "scene/themes.h"
#include "ui/layout.h"

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

/* Render-side config defaults the REPL state initializer feeds into the
 * scene config. These are controller-policy defaults, not scene-internal
 * — the scene module accepts whatever value the caller passes. */
#define CFG_DEFAULT_MULTISAMPLE       1
#define CFG_DEFAULT_LINE_SMOOTH       0
#define CFG_DEFAULT_ATTENUATE_POINTS  1

#endif /* REPL_PRESENTATION_H */