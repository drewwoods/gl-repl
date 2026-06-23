/*
 * palette.h - Centralized 3D scene color palette.
 *
 * Single source of truth for the scene module's non-theme one-off
 * *draw* colors - the glColor* per-vertex family. Lighting/material
 * *coefficients* (glLightModelfv / glMaterialfv) are out of scope:
 * they are a different GL call and semantic (reflectance, not emitted
 * color) and the render3d_clr accessors cannot express them - those stay
 * bucket-2 named local consts (see below). The scene equivalent of
 * ui/theme.h, but deliberately NOT the
 * UI palette and NOT a multi-row theme matrix: 3D-semantic tokens, one
 * fixed scheme. Header-only (mirrors ui/theme.h and gl_2d.h): every
 * scene TU already reaches sibling src/scene headers, glr_ctrl.c reaches
 * it via the app->scene layering, and `make check-c99` syntax-checks
 * this transitively through $(SRCS).
 *
 * Reuses the established scene color value type Render3dRgba
 * (render_types.h) so tokens interoperate with the axes/grid
 * gl_color_rgba() setters.
 *
 * Three buckets of scene color (mirrors ui/theme.h's policy):
 *   1. Palette token -> render3d_clr(RENDER3D_CLR_*) here.
 *   2. Named constant -> a local static const at the use site (fixed,
 *      non-palette one-offs; not table slots). Current carve-outs:
 *      lights.c lm_amb (GL_LIGHT_MODEL_AMBIENT) and glr_ctrl.c
 *      mspec/mshin (GL_SPECULAR/GL_SHININESS) - lighting/material
 *      coefficients, not draw colors.
 *   3. Left as data/computed -> must NOT follow this palette:
 *        - axes.c / grid.c per-theme spec tables (theme-switchable; a
 *          separate variation axis, like theme.h's k_category_colors).
 *        - backdrop.c procedural sky/window tint multipliers (computed
 *          from a base color).
 *        - lights.c d[]-derived halo/ray colors (computed from the
 *          light's own diffuse).
 *        - transform_guides.c xform_axis_color() (computed per command)
 *          and its axis_rgb[] scale-guide seed (drawn directly and
 *          arithmetically brightened for the arrowhead).
 *
 * Tokens carry alpha 1.0; callers pass the per-draw effective alpha via
 * render3d_clr_a() (matches the dominant `(..., 0.4f * as)` / `glow`
 * call shape). render3d_rgba() returns the Render3dRgba for component-math or
 * wrapper sites (e.g. transform_guides.c routes palette refs through
 * its own tg_color4f() so the ghost-pass alpha multiplier still bites).
 */
#ifndef RENDER3D_PALETTE_H
#define RENDER3D_PALETTE_H

#include "gl_includes.h"
#include "c_compat.h"          /* STATIC_ASSERT (C99/C11 portable) */
#include "render_types.h"      /* Render3dRgba */

typedef enum {
    /* Vertex/normal edit-guide cut planes (geometry_guides.c). */
    RENDER3D_CLR_GUIDE_PLANE_X_FILL = 0,
    RENDER3D_CLR_GUIDE_PLANE_X_EDGE,
    RENDER3D_CLR_GUIDE_PLANE_Y_FILL,
    RENDER3D_CLR_GUIDE_PLANE_Y_EDGE,
    RENDER3D_CLR_GUIDE_PLANE_Z_FILL,
    RENDER3D_CLR_GUIDE_PLANE_Z_EDGE,

    /* Vertex marker + 1-DOF constraint lines (geometry_guides.c). */
    RENDER3D_CLR_GUIDE_VERTEX_MARK,   /* full-vertex point        1.00,0.30,0.30 */
    RENDER3D_CLR_GUIDE_LINE_X,        /* x-free constraint        1.00,0.35,0.35 */
    RENDER3D_CLR_GUIDE_LINE_Y,        /* y-free constraint        0.30,0.90,0.30 */
    RENDER3D_CLR_GUIDE_LINE_Z,        /* z-free constraint        1.00,0.80,0.20 */

    /* Normal-vector guide (geometry_guides.c). */
    RENDER3D_CLR_GUIDE_NORMAL_BASE,   /* actual normal (gray)     0.80,0.80,0.80 */
    RENDER3D_CLR_GUIDE_NORMAL_DOUBLE, /* doubled (green)          0.20,0.95,0.20 */
    RENDER3D_CLR_GUIDE_NORMAL_HALF,   /* halved (red)             0.95,0.20,0.20 */

    /* Transform-guide reference geometry (transform_guides.c, routed
     * through tg_color4f so the ghost/solid pass multiplier applies). */
    RENDER3D_CLR_GUIDE_REF,           /* identity axis ref        0.55 gray */
    RENDER3D_CLR_GUIDE_REF_TICK,      /* 1.0 tick                 0.90 gray */
    RENDER3D_CLR_GUIDE_REF_POINT,     /* 1.0 point                0.95 gray */

    /* Orbit-target crosshair gizmo (render.c). */
    RENDER3D_CLR_ORBIT_GLOW_OUTER,    /* warm amber halo          1.00,0.70,0.25 */
    RENDER3D_CLR_ORBIT_GLOW_MID,      /* bright core              1.00,0.90,0.55 */
    RENDER3D_CLR_ORBIT_GLOW_INNER,    /* center dot               1.00,0.95,0.75 */

    /* Light indicators - fixed colors only (lights.c; the d[]-derived
     * on-light halo/ray colors stay computed, bucket 3). */
    RENDER3D_CLR_LIGHT_CORE,          /* lit core                 1.00,1.00,1.00 */
    RENDER3D_CLR_LIGHT_OFF_DOT,       /* off marker dot           0.40,0.40,0.40 */
    RENDER3D_CLR_LIGHT_OFF_X,         /* off X mark               0.70,0.20,0.20 */
    RENDER3D_CLR_LIGHT_OFF_LABEL,     /* off label text           0.50,0.30,0.30 */

    /* glr_ctrl.c scene-space overlays. */
    RENDER3D_CLR_REPLAY_FADE,         /* fade-batch geometry tint 0.70,0.70,0.80 */
    RENDER3D_CLR_TESS_PREVIEW,        /* tess wire preview        0.30,0.95,0.75 */
    RENDER3D_CLR_VERTEX_LABEL,        /* vertex-number text       1.00,1.00,0.30 */
    RENDER3D_CLR_NORMAL_LABEL,        /* normal-arrow text        0.80,0.80,0.30 */
    RENDER3D_CLR_OUTLINE_ACTIVE,      /* cursor/tess-current edge 0.00,0.90,0.90 */
    RENDER3D_CLR_OUTLINE,             /* tess non-current edge    0.55,0.20,0.70 */
    RENDER3D_CLR_OUTLINE_EDGE,        /* silhouette outline       0.00,0.00,0.00 */
    RENDER3D_CLR_VERTEX_POINT,        /* idle vertex points       0.85,0.85,0.90 */
    RENDER3D_CLR_VERTEX_POINT_REPLAY, /* replay vertex points     1.00,0.88,0.20 */

    /* Hidden-line wireframe effect (render.c). */
    RENDER3D_CLR_WIREFRAME_VISIBLE,   /* visible edges            0.96,0.98,1.00 */
    RENDER3D_CLR_WIREFRAME_HIDDEN,    /* hidden edges             0.30,0.38,0.52 */

    RENDER3D_CLR_COUNT
} Render3dColorToken;

/* Single fixed scheme. Every slot carries alpha 1.0; a designated-init
 * gap leaves a slot zero-filled, so alpha == 0 reliably flags a missing
 * entry (test_scene_palette asserts this). */
static const Render3dRgba g_scene_palette[RENDER3D_CLR_COUNT] = {
    [RENDER3D_CLR_GUIDE_PLANE_X_FILL]  = { 0.90f, 0.65f, 0.60f, 1.0f },
    [RENDER3D_CLR_GUIDE_PLANE_X_EDGE]  = { 0.90f, 0.30f, 0.30f, 1.0f },
    [RENDER3D_CLR_GUIDE_PLANE_Y_FILL]  = { 0.65f, 0.90f, 0.60f, 1.0f },
    [RENDER3D_CLR_GUIDE_PLANE_Y_EDGE]  = { 0.30f, 0.70f, 0.30f, 1.0f },
    [RENDER3D_CLR_GUIDE_PLANE_Z_FILL]  = { 0.60f, 0.65f, 0.90f, 1.0f },
    [RENDER3D_CLR_GUIDE_PLANE_Z_EDGE]  = { 0.30f, 0.30f, 0.80f, 1.0f },

    [RENDER3D_CLR_GUIDE_VERTEX_MARK]   = { 1.00f, 0.30f, 0.30f, 1.0f },
    [RENDER3D_CLR_GUIDE_LINE_X]        = { 1.00f, 0.35f, 0.35f, 1.0f },
    [RENDER3D_CLR_GUIDE_LINE_Y]        = { 0.30f, 0.90f, 0.30f, 1.0f },
    [RENDER3D_CLR_GUIDE_LINE_Z]        = { 1.00f, 0.80f, 0.20f, 1.0f },

    [RENDER3D_CLR_GUIDE_NORMAL_BASE]   = { 0.80f, 0.80f, 0.80f, 1.0f },
    [RENDER3D_CLR_GUIDE_NORMAL_DOUBLE] = { 0.20f, 0.95f, 0.20f, 1.0f },
    [RENDER3D_CLR_GUIDE_NORMAL_HALF]   = { 0.95f, 0.20f, 0.20f, 1.0f },

    [RENDER3D_CLR_GUIDE_REF]           = { 0.55f, 0.55f, 0.55f, 1.0f },
    [RENDER3D_CLR_GUIDE_REF_TICK]      = { 0.90f, 0.90f, 0.90f, 1.0f },
    [RENDER3D_CLR_GUIDE_REF_POINT]     = { 0.95f, 0.95f, 0.95f, 1.0f },

    [RENDER3D_CLR_ORBIT_GLOW_OUTER]    = { 1.00f, 0.70f, 0.25f, 1.0f },
    [RENDER3D_CLR_ORBIT_GLOW_MID]      = { 1.00f, 0.90f, 0.55f, 1.0f },
    [RENDER3D_CLR_ORBIT_GLOW_INNER]    = { 1.00f, 0.95f, 0.75f, 1.0f },

    [RENDER3D_CLR_LIGHT_CORE]          = { 1.00f, 1.00f, 1.00f, 1.0f },
    [RENDER3D_CLR_LIGHT_OFF_DOT]       = { 0.40f, 0.40f, 0.40f, 1.0f },
    [RENDER3D_CLR_LIGHT_OFF_X]         = { 0.70f, 0.20f, 0.20f, 1.0f },
    [RENDER3D_CLR_LIGHT_OFF_LABEL]     = { 0.50f, 0.30f, 0.30f, 1.0f },

    [RENDER3D_CLR_REPLAY_FADE]         = { 0.70f, 0.70f, 0.80f, 1.0f },
    [RENDER3D_CLR_TESS_PREVIEW]        = { 0.30f, 0.95f, 0.75f, 1.0f },
    [RENDER3D_CLR_VERTEX_LABEL]        = { 1.00f, 1.00f, 0.30f, 1.0f },
    [RENDER3D_CLR_NORMAL_LABEL]        = { 0.80f, 0.80f, 0.30f, 1.0f },
    [RENDER3D_CLR_OUTLINE_ACTIVE]      = { 0.00f, 0.90f, 0.90f, 1.0f },
    [RENDER3D_CLR_OUTLINE]             = { 0.55f, 0.20f, 0.70f, 1.0f },
    [RENDER3D_CLR_OUTLINE_EDGE]        = { 0.00f, 0.00f, 0.00f, 1.0f },
    [RENDER3D_CLR_VERTEX_POINT]        = { 0.85f, 0.85f, 0.90f, 1.0f },
    [RENDER3D_CLR_VERTEX_POINT_REPLAY] = { 1.00f, 0.88f, 0.20f, 1.0f },
    [RENDER3D_CLR_WIREFRAME_VISIBLE]   = { 0.96f, 0.98f, 1.00f, 1.0f },
    [RENDER3D_CLR_WIREFRAME_HIDDEN]    = { 0.30f, 0.38f, 0.52f, 1.0f },
};

static inline Render3dRgba render3d_rgba(Render3dColorToken t) {
    return g_scene_palette[t];
}

/* Set the current GL color from a token (alpha = token's own, 1.0). */
static inline void render3d_clr(Render3dColorToken t) {
    const Render3dRgba c = g_scene_palette[t];
    glColor4f(c.r, c.g, c.b, c.a);
}

/* Set the current GL color from a token with an explicit alpha
 * (multiplies the token alpha) - matches the dominant
 * glColor4f(r, g, b, <computed alpha>) scene call shape. */
static inline void render3d_clr_a(Render3dColorToken t, float a) {
    const Render3dRgba c = g_scene_palette[t];
    glColor4f(c.r, c.g, c.b, c.a * a);
}

/* Table-and-enum agreement: bumping RENDER3D_CLR_COUNT now only needs
 * the matching g_scene_palette entry, not a literal-count update.
 * test_scene_palette still verifies each named token has a non-NaN
 * RGBA tuple. */
STATIC_ASSERT(sizeof(g_scene_palette) / sizeof(g_scene_palette[0])
                  == RENDER3D_CLR_COUNT,
              "g_scene_palette length must match RENDER3D_CLR_COUNT");

#endif /* RENDER3D_PALETTE_H */
