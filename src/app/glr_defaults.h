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
 * the box" values. Pure rendering enums themselves (SceneGridTheme,
 * SceneAxesTheme, SceneGridMajorIdx, SceneGridExtentIdx) live in src/scene/themes.h;
 * the panel-layout enum lives in src/ui/core/layout.h. Including those
 * here is intentional — only callers that need these defaults pay
 * the transitive cost, keeping config.h dependency-free.
 *
 * Concept: compile-time defaults. User-toggleable runtime settings
 * (wireframe / grid theme / etc.) live on glr_config.h and the
 * g_cfg_items[] descriptor table in glr_actions.c — different
 * concept, do not conflate.
 */
#ifndef GLR_DEFAULTS_H
#define GLR_DEFAULTS_H

#include "app/glr_config.h"  /* GlrConfigKey (used by GlrExampleTagDefault) */

#define CFG_DEFAULT_WIREFRAME         0
#define CFG_DEFAULT_GRID_THEME        GRID_THEME_XZRULER
#define CFG_DEFAULT_GRID_MAJOR_IDX    GRID_MAJOR_1
#define CFG_DEFAULT_GRID_EXTENT_IDX   GRID_EXTENT_FAR
#define CFG_DEFAULT_AXES_THEME        AXES_THEME_OFF
#define CFG_DEFAULT_VERTEX_LABELS     1
#define CFG_DEFAULT_VERTEX_INDICES    1
#define CFG_DEFAULT_NORMAL_VECTORS    0
#define CFG_DEFAULT_VERTEX_OUTLINES   1
#define CFG_DEFAULT_VERTEX_POINTS     1
#define CFG_DEFAULT_VERTEX_GUIDES     1
#define CFG_DEFAULT_XFORM_GUIDE_MODE  0
#define CFG_DEFAULT_LIGHT_INDICATORS  1
#define CFG_DEFAULT_BACKDROP_MODE     SCENE_BACKDROP_OFF
#define CFG_DEFAULT_ORTHO_MODE        0   /* 0 = 3D perspective, 1 = 2D ortho */
#define CFG_DEFAULT_CAMERA_ROTATE     0
#define CFG_DEFAULT_VARIABLE_PANEL    1
#define CFG_DEFAULT_WRAP_AT_COMMA     1
#define CFG_DEFAULT_CODE_PANEL_LAYOUT CODE_PANEL_LAYOUT_TOP
#define CFG_DEFAULT_SYNTAX_HIGHLIGHT  0   /* off by default */
#define CFG_DEFAULT_CODE_FOCUS        1   /* focus view by default */

/* Render-side config defaults the REPL state initializer feeds into
 * the scene config. These are controller-policy defaults, not
 * scene-internal — the scene module accepts whatever value the caller
 * passes. */
#define CFG_DEFAULT_MULTISAMPLE       1
#define CFG_DEFAULT_LINE_SMOOTH       0
#define CFG_DEFAULT_ATTENUATE_POINTS  1

/* Tag-keyed presentation defaults applied during example loading,
 * layered between the global reset and the example's own leading
 * `@cfg` metadata. Each entry is a (tag, GlrConfigKey, value) tuple;
 * the controller applies them via glr_config_set() so the typed
 * config machinery — not a parser path — owns the change.
 *
 * Precedence: example `@cfg` > tag default > global default.
 *
 * Initialize a table with the GLR_EXAMPLE_TAG_DEFAULTS macro:
 *   static const GlrExampleTagDefault entries[] = GLR_EXAMPLE_TAG_DEFAULTS;
 * The macro is guarded by `#ifndef` so a translation unit can pre-
 * define its own version (e.g., a test injecting a synthetic policy
 * via a unity-include of glr_ctrl.c) before this header is included.
 *
 * If two entries that match the same example's tag mask target the
 * same GlrConfigKey, the later entry wins and the controller logs a
 * one-line warning to stderr. Order the table so the preferred entry
 * is last among colliding rows. */
typedef struct {
    int          tag_idx;  /* REPL_EXAMPLE_TAG_* */
    GlrConfigKey key;
    int          value;
} GlrExampleTagDefault;

#ifndef GLR_EXAMPLE_TAG_DEFAULTS
#define GLR_EXAMPLE_TAG_DEFAULTS {                                         \
    { .tag_idx = REPL_EXAMPLE_TAG_2D,                                      \
      .key     = GLR_CONFIG_GRID_THEME,                                    \
      .value   = GRID_THEME_PLANES },                                      \
}
#endif

#endif /* GLR_DEFAULTS_H */
