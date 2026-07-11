/*
 * accent_palette.h - Shared accent palette: named anchor sets + a
 * semantic role map.
 *
 * Single source of truth for the curated accent family shared by the
 * built-in example scenes, the XZ Ruler grid axes, the color-picker
 * swatch row, and — via the separate brand-mark anchor list below — the
 * startup splash and the SVG logo assets (examples/README.md "Example
 * Color Language"). Three levels:
 *
 *   palette — a named anchor set (PALETTE_DUSK_ANCHORS today).
 *   anchor  — a named color inside a set (PAL_CORAL, PAL_AZURE, ...).
 *   role    — a semantic slot consumers bind to (PAL_ROLE_WARM_KEY).
 *
 * C consumers bind to ROLES — never raw literals, and normally not
 * anchors — so retuning stays local to this header:
 *
 *   - Reassign a role (say, warm key CORAL -> ROSE): edit that one
 *     PAL_ROLE_* line. Consumers rebind on rebuild. Scene-file literals
 *     are anchors, not roles, so they are deliberately unaffected.
 *   - Swap the whole palette: add a PALETTE_<NAME>_ANCHORS list, repoint
 *     PALETTE_ACTIVE_NAME / PALETTE_ACTIVE_ANCHORS, and remap the roles.
 *     `make check-palette` then fails on every scene file whose literals
 *     are no longer anchors — that failure list is the scene-migration
 *     TODO list (scenes are user-visible source text; they cannot
 *     re-theme themselves).
 *
 * Scene files (.glr) cannot #include, so they keep literal triples and
 * are held on-palette by scripts/check/check-palette.py (file-level
 * ratchet baseline in scripts/baselines/palette-coverage.txt), which
 * also cross-checks the examples/README.md table against this header.
 * `make palette-list` prints the anchors as floats + hex for SVG/doc
 * work.
 *
 * Deliberately NOT here: the 2D UI chrome theme (src/ui/core/theme.h,
 * runtime token table), the render3d overlay/guide token palette
 * (src/render3d/palette.h — fixed editor-affordance colors, a separate
 * system this file must not be confused with; its name is also why this
 * header is accent_palette.h — a root palette.h would be shadowed for
 * render3d TUs by quoted-include search order), the grid's derived
 * near-neutral field grays, and the syntax-highlight category colors —
 * see theme.h's three-bucket taxonomy. This header is the static
 * scene/brand accent family only.
 *
 * Like keymap.h: root header (project-specific), zero includes. Macros
 * use the PAL_/PALETTE_ prefix; functions use palette_*.
 */
#ifndef ACCENT_PALETTE_H
#define ACCENT_PALETTE_H

/* ---- Anchor sets ---------------------------------------------------
 * "Dusk": a deep cool ink canvas plus seven mid-bright, lightly
 * desaturated accents. List order is the presentation order (README
 * table rows, color-picker swatch row). */
#define PALETTE_DUSK_ANCHORS(X) \
    X(CANVAS, 0.05f, 0.06f, 0.08f) /* deep cool ink - clear color    */ \
    X(CORAL,  0.98f, 0.46f, 0.36f) /* warm key                       */ \
    X(AMBER,  0.98f, 0.76f, 0.36f) /* gold highlight / control points */ \
    X(ROSE,   0.95f, 0.44f, 0.66f) /* pink bridge                    */ \
    X(VIOLET, 0.62f, 0.52f, 0.95f) /* purple bridge / faint guides   */ \
    X(AZURE,  0.36f, 0.70f, 0.98f) /* cool key                       */ \
    X(TEAL,   0.30f, 0.84f, 0.80f) /* aqua secondary                 */ \
    X(MIST,   0.92f, 0.95f, 0.98f) /* neutral near-white             */

/* ---- Active palette ------------------------------------------------ */
#define PALETTE_ACTIVE_NAME    "Dusk"
#define PALETTE_ACTIVE_ANCHORS PALETTE_DUSK_ANCHORS

/* ---- Brand-mark anchors ---------------------------------------------
 * The adopted identity mark — the open cube. "The logo is what the app
 * renders": the source of truth is the shipped scene
 * examples/scenes/lit-cube.glr, and this list is its color vocabulary
 * at both ends of the render. A separate list from the scene accents,
 * and not switched by PALETTE_ACTIVE_*: the mark is palette-independent
 * brand vocabulary pinned to what the scene paints (today its floor
 * happens to be the Dusk ROSE anchor; its wall material is not).
 *
 * Two tiers, kept in sync by make check-palette:
 *   - _MAT anchors: the raw glColor3f materials lit-cube.glr paints
 *     with (the scene is validated against this list, not just the
 *     Dusk accents — its exterior MIST and CANVAS ink are already
 *     scene anchors). The floor material was re-anchored on Dusk ROSE
 *     (2026-07-11, replacing the pre-Dusk magenta) so the mark's warm
 *     face sits on the family; the wall material stays mark-specific.
 *   - Flat + _LO/_HI anchors: the *rendered* colors those materials
 *     produce under the scene's lighting — the canonical chips the
 *     startup splash draws with (src/app/splash.c) and the gradient
 *     stops the vector twin shades with (docs/images/logo.svg, held to
 *     exactly this hex vocabulary by the checker).
 *
 * Retune the mark here and the splash follows at compile time, while
 * the checker flags the logo scene / SVG wherever they still carry the
 * old values. */
#define PALETTE_MARK_ANCHORS(X) \
    X(MARK_WALL_MAT,  0.25f, 0.675f, 1.0f)   /* lit-cube wall material   */ \
    X(MARK_FLOOR_MAT, 0.95f, 0.44f,  0.66f)  /* lit-cube floor material (= Dusk ROSE) */ \
    X(MARK_WALL,     0.239f, 0.659f, 0.961f) /* interior azure    #3da8f5 */ \
    X(MARK_WALL_LO,  0.169f, 0.588f, 0.925f) /* wall grad stop    #2b96ec */ \
    X(MARK_WALL_HI,  0.353f, 0.706f, 0.965f) /* wall grad stop    #5ab4f6 */ \
    X(MARK_FLOOR,    0.949f, 0.439f, 0.659f) /* interior rose     #f270a8 */ \
    X(MARK_FLOOR_LO, 0.910f, 0.392f, 0.620f) /* floor grad stop   #e8649e */ \
    X(MARK_FLOOR_HI, 0.961f, 0.482f, 0.675f) /* floor grad stop   #f57bac */ \
    X(MARK_TOP,      0.780f, 0.792f, 0.871f) /* exterior top      #c7cade */ \
    X(MARK_TOP_LO,   0.714f, 0.729f, 0.835f) /* top grad stop     #b6bad5 */ \
    X(MARK_TOP_HI,   0.851f, 0.859f, 0.922f) /* top grad stop     #d9dbeb */ \
    X(MARK_SIDE,     0.824f, 0.835f, 0.859f) /* exterior side     #d2d5db */ \
    X(MARK_SIDE_LO,  0.749f, 0.761f, 0.796f) /* side grad stop    #bfc2cb */ \
    X(MARK_SIDE_HI,  0.882f, 0.890f, 0.914f) /* side grad stop    #e1e3e9 */ \
    X(MARK_EDGE,     1.000f, 1.000f, 1.000f) /* edge pass / ink   #ffffff */

/* Anchor ids, in list order: scene accents first, then the brand mark. */
typedef enum {
#define PAL_EXPAND_ENUM(name, r, g, b) PAL_##name,
    PALETTE_ACTIVE_ANCHORS(PAL_EXPAND_ENUM)
    PALETTE_MARK_ANCHORS(PAL_EXPAND_ENUM)
#undef PAL_EXPAND_ENUM
    PAL_ANCHOR_COUNT
} PalAnchor;

/* Scene accents occupy [0, PAL_SCENE_ANCHOR_COUNT). UI that presents
 * "the palette" to scene authors (the color-picker swatch row) uses
 * this count so the brand-mark anchors stay out of scene-editing
 * surfaces. */
#define PAL_EXPAND_COUNT(name, r, g, b) + 1
enum { PAL_SCENE_ANCHOR_COUNT = 0 PALETTE_ACTIVE_ANCHORS(PAL_EXPAND_COUNT) };
#undef PAL_EXPAND_COUNT

/* ---- Role map -------------------------------------------------------
 * The single place to reassign a semantic slot. One line per role; the
 * comment names every consumer so a retune can be sanity-checked. */
#define PAL_ROLE_WARM_KEY PAL_CORAL /* XZ Ruler X axis + ticks; warm ramp end */
#define PAL_ROLE_COOL_KEY PAL_AZURE /* XZ Ruler Z axis + ticks; cool ramp end */

/* ---- Accessors ------------------------------------------------------ */
/* RGB triple for an anchor (or role, since roles alias anchors). Returns
 * a pointer to a static const [3]; out-of-range ids clamp to the last
 * anchor rather than reading wild. */
static inline const float *palette_anchor_rgb(int anchor) {
    static const float k_rgb[PAL_ANCHOR_COUNT][3] = {
#define PAL_EXPAND_RGB(name, r, g, b) { r, g, b },
        PALETTE_ACTIVE_ANCHORS(PAL_EXPAND_RGB)
        PALETTE_MARK_ANCHORS(PAL_EXPAND_RGB)
#undef PAL_EXPAND_RGB
    };
    if (anchor < 0 || anchor >= PAL_ANCHOR_COUNT)
        anchor = PAL_ANCHOR_COUNT - 1;
    return k_rgb[anchor];
}

#endif /* ACCENT_PALETTE_H */
