/*
 * glr_defaults.h - Controller-side render3d / presentation defaults.
 *
 * Compile-time defaults for render3d presentation state that examples
 * are allowed to override via leading `// @cfg` metadata. Reset
 * helpers and tests reuse these to stay aligned with the app-shell defaults
 * without drifting.
 *
 * This is gl-repl app-shell logic - it knits together render3d
 * enums and the editor's panel layout into a single set of "out of
 * the box" values. The owning enums live in src/render3d/themes.h and
 * src/ui/app/layout.h; include them here so the symbolic defaults do
 * not rely on include order elsewhere.
 *
 * Concept: compile-time defaults. User-toggleable runtime settings
 * (wireframe / grid theme / etc.) live on glr_config.h and the
 * g_cfg_items[] descriptor table in glr_actions.c - different
 * concept, do not conflate.
 */
#ifndef GLR_DEFAULTS_H
#define GLR_DEFAULTS_H

#include "app/glr_config.h"  /* GlrConfigKey (used by GlrExampleTagDefault) */
#include "render3d/guides/xform_guide_mode.h"
#include "render3d/themes.h"
#include "render3d/view_mode.h"
#include "render3d/projection_mode.h"
#include "subsystems/edit_overlays/edit_overlays.h"
#include "ui/app/layout.h"

#define CFG_DEFAULT_WIREFRAME         WIREFRAME_OFF
#define CFG_DEFAULT_GRID_THEME        GRID_THEME_XZRULER
#define CFG_DEFAULT_GRID_MAJOR_IDX    GRID_MAJOR_1
#define CFG_DEFAULT_GRID_EXTENT_IDX   GRID_EXTENT_FAR
#define CFG_DEFAULT_GRID_BRIGHTNESS_IDX GRID_BRIGHTNESS_NORMAL
#define CFG_DEFAULT_AXES_THEME        AXES_THEME_OFF
#define CFG_DEFAULT_VERTEX_LABELS     OVERLAY_VERTEX_LABEL_INDEX
#define CFG_DEFAULT_VERTEX_LABEL_PLACEMENT OVERLAY_LABEL_PLACEMENT_DECLUTTERED
/* Overlay scope: 0 = label/highlight the first representative copy of a
 * looped primitive (so the parametric torus shows v0..vN once, not
 * duplicated per ring);
 * 1 = label every unrolled vertex with a globally-unique number;
 * 2 = label all instances at their vertex positions (without decluttering). */
#define CFG_DEFAULT_OVERLAY_SCOPE 0
#define CFG_DEFAULT_NORMAL_VECTORS    0
#ifndef CFG_DEFAULT_VERTEX_OUTLINES
#define CFG_DEFAULT_VERTEX_OUTLINES   1   /* overridable via -D (e.g. web build) */
#endif
#define CFG_DEFAULT_VERTEX_OUTLINE_STYLE VERTEX_OUTLINE_STYLE_DEFAULT
#ifndef CFG_DEFAULT_VERTEX_POINTS
#define CFG_DEFAULT_VERTEX_POINTS     1   /* overridable via -D (e.g. web build) */
#endif
#define CFG_DEFAULT_XFORM_GUIDE_MODE  RENDER3D_XFORM_GUIDE_FRAME
#define CFG_DEFAULT_LIGHT_INDICATORS  0
#define CFG_DEFAULT_LIGHT_THEME       LIGHT_THEME_DEFAULT
#define CFG_DEFAULT_BACKDROP_MODE     RENDER3D_BACKDROP_OFF
#define CFG_DEFAULT_ORTHO_MODE        RENDER3D_VIEW_3D
#define CFG_DEFAULT_PROJECTION        PROJ_PERSPECTIVE
#define CFG_DEFAULT_CAMERA_ROTATE     0
#define CFG_DEFAULT_VARIABLE_PANEL    1
#define CFG_DEFAULT_WRAP_AT_COMMA     1
#define CFG_DEFAULT_CODE_PANEL_LAYOUT CODE_PANEL_LAYOUT_TOP
#define CFG_DEFAULT_SYNTAX_HIGHLIGHT  2   /* 0 = off, 1 = on, 2 = on + drop-shadow */
#define CFG_DEFAULT_CODE_FOCUS        1   /* focus view by default */
#define CFG_DEFAULT_PAREN_MATCH       1   /* matching-paren highlight on */
#define CFG_DEFAULT_PAREN_SCOPE       1   /* in-scope highlight band on */
#define CFG_DEFAULT_DEPTH_VIZ        0   /* BUFFER_VIZ_DEPTH_OFF */
#define CFG_DEFAULT_STENCIL_VIZ      0   /* BUFFER_VIZ_STENCIL_OFF */
#define CFG_DEFAULT_CALL_DEPTH_TINT  0   /* colour-by-call-depth view off */

/* Tutorial-start overrides. Tutorials reset presentation to the values
 * above, then differ on these; a tutorial's own leading `@cfg` still
 * wins over them.
 *
 * Camera distance + grid extent go together. Tutorial geometry is
 * authored in whole units (see src/repl/tutorials.c) - shapes on +/-1 or
 * +/-2, solid sizes and radii of 1 - because decimal coordinates made
 * lesson shapes small enough that the cursor-bound overlays (vertex
 * labels especially) outweighed the primitive being taught. The built-in
 * dist of 5.0 frames only +/-2.07 vertically, so tutorials pull back far
 * enough to hold that vocabulary; the rest of the built-in pose (orbit,
 * target) is unchanged. CLOSE then puts the grid at the scale of the
 * geometry standing on it, where the FAR default reads as a dense carpet.
 *
 * Vertex outlines / points: the knobs are here so a lesson's decorations
 * can be tuned independently of the app default, which is what they
 * currently track. */
#define CFG_DEFAULT_TUTORIAL_CAMERA_DIST     8.0f
#define CFG_DEFAULT_TUTORIAL_GRID_EXTENT_IDX GRID_EXTENT_CLOSE
#define CFG_DEFAULT_TUTORIAL_VERTEX_OUTLINES 1
#define CFG_DEFAULT_TUTORIAL_VERTEX_POINTS   1

/* Backdrop/grid pairing defaults. These rows declare which backdrops own a
 * companion grid; glr_config.c owns the enforcement policy (force the grid
 * while paired, hide paired grids from direct user cycling, restore the prior
 * grid when leaving paired backdrops).
 *
 * Initialize a table with the GLR_BACKDROP_GRID_PAIR_DEFAULTS macro:
 *   static const GlrBackdropGridPair entries[] = GLR_BACKDROP_GRID_PAIR_DEFAULTS;
 */
#ifndef GLR_BACKDROP_GRID_PAIR_DEFAULTS
#define GLR_BACKDROP_GRID_PAIR_DEFAULTS {                                  \
    { .backdrop = RENDER3D_BACKDROP_AURORA,         .grid = GRID_THEME_AURORA }, \
    { .backdrop = RENDER3D_BACKDROP_SUNSET,         .grid = GRID_THEME_SYNTHWAVE }, \
    { .backdrop = RENDER3D_BACKDROP_POLAR_DAY_SNOW, .grid = GRID_THEME_FROZEN }, \
    { .backdrop = RENDER3D_BACKDROP_POLAR_DAY,      .grid = GRID_THEME_FROZEN }, \
    { .backdrop = RENDER3D_BACKDROP_NEBULA,         .grid = GRID_THEME_STARCHART }, \
}
#endif

/* Neutral default clear color used by the controller when the flat
 * program contains no user glClearColor command. Keep the luminance
 * helper in sync with these channel values; overlay alpha scaling uses
 * this luminance to stay calibrated to the default background. */
#define CFG_DEFAULT_CLEAR_R           0.10f
#define CFG_DEFAULT_CLEAR_G           0.10f
#define CFG_DEFAULT_CLEAR_B           0.10f
#define CFG_DEFAULT_CLEAR_A           1.0f
#define CFG_DEFAULT_CLEAR_LUMA \
    (0.2126f * CFG_DEFAULT_CLEAR_R + \
     0.7152f * CFG_DEFAULT_CLEAR_G + \
     0.0722f * CFG_DEFAULT_CLEAR_B)

/* Render-side config defaults the REPL state initializer feeds into
 * the render3d config. These are controller-policy defaults, not
 * render3d-internal - the render3d module accepts whatever value the caller
 * passes. */
#define CFG_DEFAULT_MULTISAMPLE       1
#define CFG_DEFAULT_LINE_SMOOTH       0
#define CFG_DEFAULT_ATTENUATE_POINTS  1
#define CFG_DEFAULT_AUTONORMAL        0  /* REPL_AUTONORMAL_OFF */
#define CFG_DEFAULT_HIGHLIGHT_POLY    POLY_HIGHLIGHT_ON
/* use_accum is the "accum buffer is usable" gate (the --accum /
 * --no-accum switches), NOT the AA on/off toggle. AA renders only when
 * BOTH this field and accum_effect are set: render.c gates on
 *   do_accum = use_accum && accum_effect != OFF
 * so the two are AND-ed, not an override relationship.
 *
 * This macro is only the compile-time initializer for the use_accum
 * field (glr_state.c). It does NOT control the shipped binary: main()
 * in gl_repl.c unconditionally calls glr_ctrl_set_accum(use_accum) at
 * startup, clobbering whatever this default was. Without an explicit
 * --accum / --no-accum that call resolves AUTO against the GL-init
 * probes - off when the context has no accumulation buffer, and off on
 * renderers that emulate one in software (Mesa), else on. So changing
 * this value only affects paths that build state without going through
 * main() (tests, render3d_demo, headless capture). To change the app's
 * default AA behavior, flip CFG_DEFAULT_ACCUM_EFFECT below - that is
 * the user-facing effect selector. */
#define CFG_DEFAULT_USE_ACCUM         1
/* Effect defaults to AA (preserving the historic accum-AA-on-by-default
 * behavior); Blur is strictly opt-in. Passes default to 1. Set this to
 * RENDER3D_ACCUM_EFFECT_OFF to ship with AA off while leaving the accum
 * buffer allocated for runtime Accum effect / Blur use. */
#define CFG_DEFAULT_ACCUM_EFFECT      RENDER3D_ACCUM_EFFECT_AA
#define CFG_DEFAULT_ACCUM_PASSES      1

/* Scissor the accumulation-AA loop (per-pass glClear + glAccum read/return)
 * to the scene viewport instead of the whole window. On paper this is a
 * strict win: glClear/glAccum are bounded by the scissor box, not the
 * viewport, so without it the up-to-16x accum work scans the dead region
 * under the code panel every pass. Yet on macOS (Apple's OpenGL over Metal)
 * it measured *slower* in practice - enabling GL_SCISSOR_TEST around the
 * accum path appears to push the driver off a fast clear/accumulate path
 * (e.g. a tile/fast-clear or whole-surface DMA optimization) that more than
 * pays for the extra pixels covered. So it ships OFF; flip to 1 only on a
 * platform where the scissored path actually benchmarks faster. */
#define CFG_DEFAULT_USE_ACCUM_AA_SCISSORS  0

/* Tag-keyed presentation defaults applied during example loading,
 * layered between the global reset and the example's own leading
 * `@cfg` metadata. Each entry is a (tag, GlrConfigKey, value) tuple;
 * the controller applies them via glr_config_set() so the typed
 * config machinery - not a parser path - owns the change.
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
