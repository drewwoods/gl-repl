/*
 * overlay_xn.h - Pure transition-fade resolver shared by grid + axes.
 *
 * Both renderers used to maintain identical file-static g_xn_opacity /
 * g_xn_alpha statics and identical 3-way knee-resolve blocks at the
 * top of each render call. The static fields were lifted into each
 * file's draw-context struct (audit #3) and the resolve math moved
 * here so it lives in one place — but the helper is INTENTIONALLY
 * pure: it takes opacity + knee + a "this theme owns fog" flag, and
 * returns the resolved {draw, opacity, alpha, fog_tf} bundle without
 * touching any state. Each renderer then drops the result into its
 * draw context and threads it through gl_color analogues.
 *
 * fog_tf is `1 - opacity` — a convenience for synthetic-fog recede
 * passes (grid uses it; axes doesn't, but the field is cheap).
 */
#ifndef SCENE_OVERLAY_XN_H
#define SCENE_OVERLAY_XN_H

/* Style selects how controller-owned opacity becomes a visual:
 *   GRID_AXES_XN_FADE  plain alpha fade
 *   GRID_AXES_XN_FOG   recede into clear-color fog, with an alpha knee
 *                      near opacity 0 for the final vanish. */
#define GRID_AXES_XN_FADE 0
#define GRID_AXES_XN_FOG  1

#ifndef GRID_XN_STYLE
#define GRID_XN_STYLE GRID_AXES_XN_FOG
#endif
#ifndef AXES_XN_STYLE
#define AXES_XN_STYLE GRID_AXES_XN_FADE
#endif

/* Frequency multiplier for shared grid/axes "breathing" glow:
 * sinf(anim_time * SCENE_BREATH_FREQ) * 0.5 + 0.5. */
#ifndef SCENE_BREATH_FREQ
#define SCENE_BREATH_FREQ 0.8f
#endif

typedef struct SceneOverlayXn {
    int   draw;     /* 0 = skip the entire pass; opacity hit zero */
    float opacity;  /* clamped 0..1 */
    float alpha;    /* color-alpha multiplier (post-knee under FOG style) */
    float fog_tf;   /* synthetic-fog transition factor: 1 - opacity */
} SceneOverlayXn;

/* style:    GRID_AXES_XN_FADE → alpha = opacity (no knee)
 *           GRID_AXES_XN_FOG  → fog-knee resolve below
 * uses_own_fog: only meaningful under FOG style — when the theme
 *           draws its own fog (e.g. grid OCEAN/FOG), fall back to
 *           plain alpha = opacity so the synthetic recede doesn't
 *           fight the theme's fog. Axes has no fog-owning themes
 *           and passes 0 here.
 * fog_alpha_knee: opacity at which the knee transitions (under FOG
 *           style only). Per-renderer constant; the resolver doesn't
 *           hard-code it.
 */
static inline SceneOverlayXn
scene_overlay_xn_resolve(float opacity, int style,
                         int uses_own_fog, float fog_alpha_knee) {
    SceneOverlayXn xn = { 0 };
    if (opacity < 0.0f) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;
    xn.opacity = opacity;
    xn.fog_tf  = 1.0f - opacity;

    if (opacity <= 0.0f) {
        xn.draw = 0;
        xn.alpha = 0.0f;
        return xn;
    }
    xn.draw = 1;

    if (style != GRID_AXES_XN_FOG) {
        xn.alpha = opacity;            /* plain FADE */
        return xn;
    }

    /* FOG style: alpha stays at 1 above the knee (fog carries the
     * recede look), then ramps to 0 below it (final vanish). Themes
     * that already own fog get the plain alpha fallback so their fog
     * isn't competing with the synthetic clear-color recede. */
    if (uses_own_fog) {
        xn.alpha = opacity;            /* FADE fallback */
    } else if (opacity >= fog_alpha_knee) {
        xn.alpha = 1.0f;               /* fog does the work */
    } else {
        xn.alpha = opacity / fog_alpha_knee; /* final vanish */
    }
    return xn;
}

#endif /* SCENE_OVERLAY_XN_H */
