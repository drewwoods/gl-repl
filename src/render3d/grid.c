/*
 * grid.c - grid theme rendering
 */
#include "grid.h"
#include "overlay_xn.h"  /* Render3dOverlayXn + shared resolve helper */
#include "accent_palette.h"  /* shared accent palette: ruler axis role colors */
#include "render3d_hash.h"   /* render3d_hash01 - frozen decoration placement */
#include <math.h>     /* sinf, cosf, sqrtf, fabsf, fmodf, M_PI (via gl_includes.h) */
#include <stdio.h>    /* snprintf (2D grid label text) */

#define GRID_LOOP_EPSILON 0.01f
#define GRID_ORIGIN_SKIP_EPSILON 0.01f
#define GRID_PLANE_VISIBILITY_EPSILON 0.01f
#define GRID_MINOR_SUBDIVISIONS 5.0f
#define GRID_MAJOR_TOL_FRACTION 0.25f

/* Edge-fade dissolve (replaces the clear-color recede fog for the
 * generic line themes). Each grid line scales its per-vertex alpha to 0
 * by WORLD radial distance from the origin, sqrt(x^2 + z^2): the fade is
 * pinned in world space, so the grid edge is gone no matter where the
 * camera is (drag out to the rim and there are no lines there), and the
 * grid fades into whatever backdrop is behind it rather than to the GL
 * clear color. Radial (not along-axis) is what kills the grazing-horizon
 * smudge: the lines that pile up at the horizon are the far ones (large
 * radius), and a radial fade dims them at their centers where they
 * stack. The front is at `fade_end = extent` steady and sweeps inward
 * during the hide transition (fade_end = extent * opacity), unifying the
 * steady dissolve and the recede animation. Scoped to the table-driven
 * line themes + XZ Ruler + Star Chart via render3d_grid_theme_uses_edge_fade();
 * the custom environment themes and Radar own their atmosphere/fog. */
#define GRID_EDGE_FADE_BAND   0.35f  /* radial ramp width / extent (interior
                                      * solid inside fade_end - band) */
#define GRID_EDGE_FADE_MAX_BP 8      /* breakpoint capacity (uses <= 5) */

/* ---- Grid appear / disappear reveal animation (editable) ------------------
 * While a line theme is mid fade-in/out it can "draw itself in" along a
 * characteristic wipe instead of the plain radial recede, optionally with a
 * bright moving draw-head at the advancing front that settles to the theme's
 * base line color behind it (inspired by the reference plotter / synthwave
 * draw-in styles). This is a transition-only effect: at full opacity the
 * grid renders through the exact edge-fade path below, unchanged. Tune the
 * motion with the GRID_REVEAL_* constants and assign each theme an
 * { .axis, .head } pair in g_grid_theme_traits[]; all of it is meant to be edited and
 * recompiled. A reveal is { .axis } for a plain dissolve with no head
 * wave, or { .axis, .head = 1 } for the bright leading-edge draw-head. */
typedef enum {
    GRID_REVEAL_RADIAL = 0,  /* grow outward from the origin (the classic look) */
    GRID_REVEAL_SWEEP_X,     /* planar wipe along +X  (plotter, left-to-right)  */
    GRID_REVEAL_SWEEP_Z,     /* planar wipe along +Z  (far-to-near drive-in)    */
    GRID_REVEAL_DIAGONAL     /* planar wipe along the X+Z diagonal              */
} GridRevealAxis;

typedef struct {
    GridRevealAxis axis;
    int            head;   /* 1 = bright moving draw-head; 0 = plain dissolve */
    float          time;   /* fade in/out time scale vs GRID_FADE_*_SECS:
                            * 0 (the unset default) or 1 = base speed, >1
                            * slower, <1 snappier. Consumed by the grid's
                            * reveal curve (render3d_grid_reveal). */
} GridReveal;

#define GRID_REVEAL_BAND        0.45f /* soft reveal ramp width (u fraction)      */
#define GRID_REVEAL_HEAD_BAND   0.10f /* bright draw-head width (u fraction)      */
#define GRID_REVEAL_HEAD_GAIN   2.4f  /* rgb brightness multiply at the head peak */
#define GRID_REVEAL_HEAD_ALPHA  0.35f /* additive head-glow alpha at the front    */
#define GRID_REVEAL_SEGS        22    /* per-line subdivisions while wiping        */

/* Per-theme behavioral traits: edge-fade dissolve, distance fog ownership,
 * NV radial fog opt-in, and reveal animation motion. */
typedef struct GridThemeTraits {
    int        uses_edge_fade;
    int        uses_fog;
    int        uses_nv_fog;
    GridReveal reveal;
} GridThemeTraits;

static const GridThemeTraits g_grid_theme_traits[GRID_THEME_COUNT] = {
    [GRID_THEME_CLASSIC]   = { .uses_edge_fade = 1, .reveal = { .axis = GRID_REVEAL_RADIAL } },
    [GRID_THEME_TRON]      = { .uses_edge_fade = 1, .reveal = { .axis = GRID_REVEAL_SWEEP_X,  .head = 1, .time = 7.0f } },
    [GRID_THEME_EMBER]     = { .uses_edge_fade = 1, .reveal = { .axis = GRID_REVEAL_RADIAL,   .head = 1, .time = 7.0f } },
    [GRID_THEME_AURORA]    = { .uses_edge_fade = 1, .reveal = { .axis = GRID_REVEAL_RADIAL } },
    [GRID_THEME_SYNTHWAVE] = { .uses_edge_fade = 1, .reveal = { .axis = GRID_REVEAL_DIAGONAL, .head = 1, .time = 6.6f } },
    [GRID_THEME_XZRULER]   = { .uses_edge_fade = 1, .reveal = { .axis = GRID_REVEAL_RADIAL } },
    [GRID_THEME_STARCHART] = { .uses_edge_fade = 1, .reveal = { .axis = GRID_REVEAL_RADIAL,   .time = 4.0f } },
    [GRID_THEME_OCEAN]     = { .uses_fog = 1 },
    [GRID_THEME_RADAR]     = { .uses_nv_fog = 1 },
};

/* Returns non-zero when v is close enough to a multiple of `major`
 * to be treated as a major line. `tol` is derived from the minor
 * step so the check stays robust as the step changes. */
static int grid_is_major_line(float v, float major, float tol) {
    return (fabsf(fmodf(fabsf(v) + tol, major)) < (tol * 2.0f));
}

typedef struct GridDrawContext {
    float extent;
    float major;
    float step;
    float major_tol;
    float anim_time;
    float breath;
    float alpha_scale; /* boost factor when bg is darker than design point */
    float grid_brightness; /* user grid-line alpha multiplier (Grid brightness
                            * cfg); folded into grid_color for grid LINES, not
                            * the environment-theme surface fills (those use
                            * grid_color_surface). 1.0 = no change. */
    /* In-out transition. Resolved once at render3d_grid_render entry via
     * render3d_overlay_xn_resolve, then every grid_color call multiplies
     * `a * xn_alpha`. xn_opacity drives the synthetic-fog recede pass
     * under GRID_XN_STYLE == GRID_AXES_XN_FOG. */
    float xn_opacity;
    float xn_alpha;
    /* Edge-fade rim dissolve (see GRID_EDGE_FADE_*). When edge_fade is
     * set, the line draws subdivide at ef_bp[] and scale each vertex's
     * alpha by ef_mul[]; the front is rebuilt once per frame from
     * extent + opacity. The breakpoints are along-axis positions and
     * are offset-independent, so they are shared by every line. */
    int   edge_fade;
    float ef_bp[GRID_EDGE_FADE_MAX_BP];
    float ef_mul[GRID_EDGE_FADE_MAX_BP];
    int   ef_n;
    float ef_fade_end;  /* radius where alpha hits 0 (for arbitrary-pos eval) */
    float ef_band;      /* ramp width just inside fade_end */
    /* Transition-time draw-in (see GRID_REVEAL_* up top). Set from
     * g_grid_theme_traits[theme].reveal; only consulted while a line theme is
     * mid-transition (grid_reveal_active). */
    GridReveal reveal;
} GridDrawContext;

typedef struct GridLineColors {
    Render3dRgba x_const;
    Render3dRgba z_const;
} GridLineColors;

typedef void (*GridLineColorFn)(float v, int is_major,
                                const GridDrawContext *ctx,
                                GridLineColors *out);
/* Fills the X-axis / Z-axis origin colors. A pair rather than one color
 * because XZ Ruler codes direction into its meridians (warm key on X, cool
 * key on Z). Themes with one origin color fill both slots via
 * grid_line_colors_same, matching GridLineColors' x_const/z_const naming. */
typedef void (*GridOriginColorFn)(const GridDrawContext *ctx,
                                  GridLineColors *out);
typedef void (*GridPassFn)(const GridDrawContext *ctx);

typedef struct GridThemeSpec {
    GridLineColorFn line_color;
    GridOriginColorFn origin_color;
    GridPassFn begin_pass;
    GridPassFn end_pass;
    float origin_line_width;
} GridThemeSpec;

static void render3d_grid_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void render3d_grid_pop_state(void) {
    glPopAttrib();
}

static Render3dRgba rgba(float r, float g, float b, float a) {
    Render3dRgba c = { r, g, b, a };
    return c;
}

/* Fade into the frame's background color. That is config->presentation_rgba,
 * not a glGetFloatv(GL_COLOR_CLEAR_VALUE) read-back: the helpers run after the
 * user geometry, inside the same glPushAttrib bracket, so live GL holds
 * whatever the program last set - which is the background only when nothing
 * follows the program's glClear. It is also not config->baseline_clear_color,
 * which is only the state a clear starts from. The presentation color is the
 * background the caller says the scene sits on, and the same one the axes
 * recede fog and the caller's own chrome use, so the three cannot disagree. */
static void set_fog_to_presentation_color(const float presentation_rgba[4]) {
    glFogfv(GL_FOG_COLOR, presentation_rgba);
}

/* Grid in-out transition. Resolved once at render3d_grid_render entry from
 * config.grid_opacity
 * and stored on the GridDrawContext. Every color path routes through
 * grid_color so it applies uniformly, AFTER each call site's own
 * alpha_scale clamp so the caller-supplied OUT is the hard ceiling. 1.0 =
 * shown.
 *
 * The shared render3d_overlay_xn_resolve helper in overlay_xn.h owns the
 * knee math; grid passes its style + GRID_XN_FOG_ALPHA_KNEE and
 * receives back the effective {alpha, opacity, fog_tf, draw}. */

#if GRID_XN_STYLE == GRID_AXES_XN_FOG
#define GRID_XN_FOG_ALPHA_KNEE 0.30f

/* Synthetic recede fog for fog-less themes: a background-colored linear wall
 * pulled in from beyond the grid as the overlay hides (tf = 1 -
 * opacity). tf<=0 -> no fog, continuous with the fogless steady look. */
static void grid_xn_apply_transition_fog(float tf, float extent,
                                         const float presentation_rgba[4]) {
    if (tf <= 0.0f) return;
    set_fog_to_presentation_color(presentation_rgba);
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    float far_end  = extent * 1.25f;   /* tf~0: fog past the grid edge */
    float near_end = extent * 0.02f;   /* tf=1: fog wall at the camera */
    float end = far_end + (near_end - far_end) * tf;
    glFogf(GL_FOG_START, end * 0.15f);
    glFogf(GL_FOG_END,   end);
}
#endif

static void grid_color(const GridDrawContext *ctx,
                       float r, float g, float b, float a) {
    /* Grid LINES route here: fold in the transition fade (xn_alpha) and the
     * user grid-line brightness multiplier, clamping since brightness may
     * push a pre-clamped alpha back above 1.0. */
    glColor4f(r, g, b, fminf(a * ctx->xn_alpha * ctx->grid_brightness, 1.0f));
}

/* Environment-theme surface/atmosphere fills (Ocean water, Frozen ice
 * sheet, Soil mesh, and the underwater/glacial/earth viewport tints) route
 * here instead: they take the transition fade but NOT the grid-line
 * brightness multiplier, which is meant for the grid lines, not a theme's
 * opaque scene layers (scaling those would, e.g., turn the underwater tint
 * fully opaque at Bold or wash it out at Dim). */
static void grid_color_surface(const GridDrawContext *ctx,
                               float r, float g, float b, float a) {
    glColor4f(r, g, b, a * ctx->xn_alpha);
}

static void grid_color_rgba(const GridDrawContext *ctx, Render3dRgba c) {
    grid_color(ctx, c.r, c.g, c.b, c.a);
}

static void grid_line_colors_same(GridLineColors *out, Render3dRgba color) {
    out->x_const = color;
    out->z_const = color;
}

/* By-value twin, for passing a single color where an x/z pair is expected
 * (the origin-axes call sites of the custom themes). */
static GridLineColors grid_line_colors_of(Render3dRgba color) {
    GridLineColors out;
    grid_line_colors_same(&out, color);
    return out;
}

/* Build the per-frame dissolve front and cache fade_end/band on the
 * context. The breakpoint table (ef_bp/ef_mul) is the offset-0 ramp -
 * used directly only by the origin axes, which run through the origin so
 * their radius == |along|. The main grid lines compute their own radial
 * breakpoints per offset in draw_grid_line_pair; ticks sample the front
 * pointwise via grid_edge_fade_mul. The offset-0 multiplier is the
 * piecewise-linear clamp((fade_end - |p|) / band, 0, 1), and placing
 * breakpoints at |p| = fade_start / fade_end reproduces it exactly. */
static void grid_edge_fade_build(GridDrawContext *ctx, float fade_end,
                                 float band) {
    float extent = ctx->extent;
    float fade_start = fade_end - band;
    ctx->ef_fade_end = fade_end;
    ctx->ef_band     = band;
    float pos[GRID_EDGE_FADE_MAX_BP];
    int n = 0;
    pos[n++] = -extent;
    if (fade_end   > 0.0f && fade_end   < extent) pos[n++] = -fade_end;
    if (fade_start > 0.0f && fade_start < extent) pos[n++] = -fade_start;
    pos[n++] = 0.0f;
    if (fade_start > 0.0f && fade_start < extent) pos[n++] = fade_start;
    if (fade_end   > 0.0f && fade_end   < extent) pos[n++] = fade_end;
    pos[n++] = extent;

    ctx->ef_n = n;
    for (int i = 0; i < n; i++) {
        float ap = fabsf(pos[i]);
        float m = (band > 1e-6f) ? (fade_end - ap) / band
                                 : (ap < fade_end ? 1.0f : 0.0f);
        if (m < 0.0f) m = 0.0f;
        if (m > 1.0f) m = 1.0f;
        ctx->ef_bp[i]  = pos[i];
        ctx->ef_mul[i] = m;
    }
}

/* Edge-fade alpha multiplier for a single point at along-axis position
 * `p` - for short marks / inline axes that don't subdivide. Mirrors the
 * ramp grid_edge_fade_build samples at its breakpoints. */
static float grid_edge_fade_mul(const GridDrawContext *ctx, float p) {
    if (!ctx->edge_fade) return 1.0f;
    float ap = fabsf(p);
    float m = (ctx->ef_band > 1e-6f) ? (ctx->ef_fade_end - ap) / ctx->ef_band
                                     : (ap < ctx->ef_fade_end ? 1.0f : 0.0f);
    if (m < 0.0f) m = 0.0f;
    if (m > 1.0f) m = 1.0f;
    return m;
}

/* Radial edge-fade multiplier for a vertex at world (off, along) on one
 * of the axis-aligned grid lines: alpha ramps from 1 inside fade_start
 * to 0 at fade_end, keyed on the world radius sqrt(off^2 + along^2). */
static float grid_radial_mul(float fade_end, float band, float off,
                             float along) {
    float r = sqrtf(off * off + along * along);
    float m = (band > 1e-6f) ? (fade_end - r) / band : (r < fade_end ? 1.0f : 0.0f);
    if (m < 0.0f) m = 0.0f;
    if (m > 1.0f) m = 1.0f;
    return m;
}

/* ---- Reveal wipe (see GRID_REVEAL_* up top) ----
 * Active only while an edge-fade line theme is partway through its
 * transition; at steady opacity the exact radial path below runs instead, so
 * the steady look and its tests are untouched. */
static int grid_reveal_active(const GridDrawContext *ctx) {
    return ctx->edge_fade && ctx->xn_opacity > 0.0f && ctx->xn_opacity < 1.0f;
}

/* Normalized reveal coordinate along the active wipe axis (radial reaches
 * ~1.41 at the corners). The front sweeps from u=0 to u=1 as opacity rises. */
static float grid_reveal_u(const GridDrawContext *ctx, float x, float z) {
    float ex = ctx->extent;
    if (ex <= 0.0f) return 0.0f;
    switch (ctx->reveal.axis) {
    case GRID_REVEAL_SWEEP_X:  return (x + ex) / (2.0f * ex);
    case GRID_REVEAL_SWEEP_Z:  return (z + ex) / (2.0f * ex);
    case GRID_REVEAL_DIAGONAL: return (x + z + 2.0f * ex) / (4.0f * ex);
    case GRID_REVEAL_RADIAL:
    default:                   return sqrtf(x * x + z * z) / ex;
    }
}

/* Emit one reveal-modulated vertex: the steady rim radial fade times the
 * wipe alpha, plus the bright draw-head glow at the advancing front. The
 * front overshoots to 1 + band so every u <= 1 is fully lit at opacity 1
 * (continuous with the steady look); the head leaves the slab by then. */
static void grid_reveal_vertex(const GridDrawContext *ctx, Render3dRgba c,
                               float x, float y, float z) {
    float band   = GRID_REVEAL_BAND;
    float front  = ctx->xn_opacity * (1.0f + band);
    float behind = front - grid_reveal_u(ctx, x, z);   /* >0 on revealed side */

    float am = behind / band;
    if (am < 0.0f) am = 0.0f; else if (am > 1.0f) am = 1.0f;
    am = am * am * (3.0f - 2.0f * am);                  /* smoothstep reveal ramp */

    float head = 0.0f;
    if (ctx->reveal.head && behind >= 0.0f) {
        head = 1.0f - behind / GRID_REVEAL_HEAD_BAND;
        if (head < 0.0f) head = 0.0f;
    }

    float rim  = grid_radial_mul(ctx->extent, GRID_EDGE_FADE_BAND * ctx->extent,
                                 x, z);
    float gain = 1.0f + (GRID_REVEAL_HEAD_GAIN - 1.0f) * head;
    float a    = (c.a * am + GRID_REVEAL_HEAD_ALPHA * head) * rim
                 * ctx->grid_brightness;
    glColor4f(fminf(c.r * gain, 1.0f), fminf(c.g * gain, 1.0f),
              fminf(c.b * gain, 1.0f), fminf(a, 1.0f));
    glVertex3f(x, y, z);
}

/* Subdivide one axis-aligned line into GRID_REVEAL_SEGS reveal-shaded
 * segments. axis 0 runs along Z at x=off; axis 1 runs along X at z=off.
 * Emits vertex pairs for the caller's open GL_LINES block. */
static void grid_reveal_line(const GridDrawContext *ctx, Render3dRgba c,
                             int axis, float off) {
    float ex = ctx->extent;
    int n = GRID_REVEAL_SEGS;
    for (int i = 0; i < n; i++) {
        float a0 = -ex + (2.0f * ex) * (float)i / (float)n;
        float a1 = -ex + (2.0f * ex) * (float)(i + 1) / (float)n;
        if (axis == 0) {
            grid_reveal_vertex(ctx, c, off, 0.0f, a0);
            grid_reveal_vertex(ctx, c, off, 0.0f, a1);
        } else {
            grid_reveal_vertex(ctx, c, a0, 0.0f, off);
            grid_reveal_vertex(ctx, c, a1, 0.0f, off);
        }
    }
}

/* ---- Contrast casing (Bright / Bold grid brightness) ----
 *
 * grid_brightness only scales alpha, and alpha converges a blended line
 * toward its own color: over bright geometry a mid-gray Classic line tops
 * out near |line_color - bg| however bold the setting, so raising the
 * multiplier past Bold buys nothing. The fix is the cartographic one - a
 * wider near-black casing drawn under the real line, giving every line its
 * own local dark edge regardless of what is behind it. Same idiom as the
 * Frozen theme's wide-faint-under-bright-core cracks below.
 *
 * The casing is a shadow and deliberately NOT keyed to the backdrop: over
 * the dark backdrops this codebase ships it is invisible (the lines already
 * read there, and alpha_scale already boosts them), and it earns its pixels
 * only where something bright sits behind the grid. It also never applies
 * alpha_scale, whose whole job is the opposite case.
 *
 * Every line gets one: majors, minors and the origin axes. Minors keep the
 * effect honest - a cased major crossing bare minors reads as two different
 * grids - but they are the dense ones (five subdivisions per cell, and the
 * whole graticule again at FAR extent), so they take a narrower casing.
 * Nothing else holds them back: the strength already self-scales with the
 * core's own alpha (see grid_casing_color), so a minor at a quarter of a
 * major's opacity gets a quarter of the shadow without a second constant.
 */
#define GRID_CASING_WIDTH_ADD   2.0f  /* px added to a cased major / axis    */
#define GRID_CASING_MINOR_W_ADD 1.0f  /* ...and to a cased minor line        */
#define GRID_CASING_ALPHA       0.55f /* casing alpha at the top of the ramp */
#define GRID_CASING_RAMP_START  1.5f  /* grid_brightness where it fades in   */
#define GRID_CASING_RAMP_FULL   3.0f  /* ...and reaches GRID_CASING_ALPHA    */

/* Casing strength for the current brightness setting; 0 at or below the
 * ramp start, so Dim (0.25) and Normal (1.2) draw byte-identically to
 * before and only Bright (2.5) / Bold (4.0) pay for the extra pass. */
static float grid_casing_alpha(const GridDrawContext *ctx) {
    float b = ctx->grid_brightness;
    if (b <= GRID_CASING_RAMP_START) return 0.0f;
    float u = (b - GRID_CASING_RAMP_START) /
              (GRID_CASING_RAMP_FULL - GRID_CASING_RAMP_START);
    if (u > 1.0f) u = 1.0f;
    return GRID_CASING_ALPHA * u;
}

/* Context clone for a casing pass: the brightness multiplier is
 * neutralized because the casing alpha is already derived from it, and
 * grid_color / grid_reveal_vertex would otherwise scale it a second time.
 * Everything else - xn_alpha, the edge-fade front, the reveal wipe - is
 * inherited, so the casing dissolves in and out with its line for free. */
static GridDrawContext grid_casing_ctx(const GridDrawContext *ctx) {
    GridDrawContext c = *ctx;
    c.grid_brightness = 1.0f;
    return c;
}

/* Casing color for a line whose core renders at alpha `core_a` (pre-
 * brightness, as the theme's line_color returned it). Scaling by the core's
 * effective opacity is load-bearing: the line themes fade toward the extent,
 * and a uniform-alpha casing would leave a black line hanging out past the
 * rim where its core has already faded to nothing. */
static Render3dRgba grid_casing_color(const GridDrawContext *ctx,
                                      float casing_a, float core_a) {
    return rgba(0.0f, 0.0f, 0.0f,
                casing_a * fminf(core_a * ctx->grid_brightness, 1.0f));
}

/* ---- GRID_CASING_SUBTRACT: destination-dependent casing (experiment) ----
 *
 * Off by default; flip to 1 to A/B it against the black casing on identical
 * geometry. The casing pass switches to GL_FUNC_REVERSE_SUBTRACT and carries
 * the *core line's own color* instead of black, so the framebuffer gets
 * `dst - line_rgb * a`: a complement shadow that is guaranteed to move every
 * channel the line is made of, even against geometry already saturated in
 * that channel (where the alpha-blended black casing only scales toward the
 * clear color and a bright cyan surface can still swallow a cyan Tron line).
 *
 * Two known costs, which are why it is not the default. It shifts hue rather
 * than just darkening - subtracting Synthwave's pink from a white surface
 * leaves a green trace, so a theme's identity color can come back as its
 * complement. And glBlendEquation is GL 1.4 / EXT_blend_subtract; before
 * enabling this for real it needs the same runtime capability gate as
 * nv_fog_distance_supported (config->nv_fog_distance_supported) rather than
 * being assumed present. The blend equation is restored to GL_FUNC_ADD by
 * hand - the surrounding glPushAttrib(GL_ALL_ATTRIB_BITS) covers
 * GL_COLOR_BUFFER_BIT, but this is the same class of not-in-the-original-
 * spec state as GL_FOG_DISTANCE_MODE_NV, so it does not rely on it. */
#ifndef GRID_CASING_SUBTRACT
#define GRID_CASING_SUBTRACT 0   /* override from the build: -DGRID_CASING_SUBTRACT=1 */
#endif

#if GRID_CASING_SUBTRACT
static void grid_contrast_blend_begin(void) {
    glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
}
static void grid_contrast_blend_end(void) {
    glBlendEquation(GL_FUNC_ADD);
}
/* Subtract the core's color, keep the casing's ramped alpha. */
static Render3dRgba grid_contrast_color(Render3dRgba casing, Render3dRgba core) {
    return rgba(core.r, core.g, core.b, casing.a);
}
#else
static void grid_contrast_blend_begin(void) { }
static void grid_contrast_blend_end(void) { }
static Render3dRgba grid_contrast_color(Render3dRgba casing, Render3dRgba core) {
    (void)core;
    return casing;
}
#endif

static void draw_grid_line_pair(float v, const GridDrawContext *ctx,
                                GridLineColors colors) {
    if (!ctx->edge_fade) {
        grid_color_rgba(ctx, colors.x_const);
        glVertex3f(v, 0, -ctx->extent);
        glVertex3f(v, 0,  ctx->extent);
        grid_color_rgba(ctx, colors.z_const);
        glVertex3f(-ctx->extent, 0, v);
        glVertex3f( ctx->extent, 0, v);
        return;
    }
    if (grid_reveal_active(ctx)) {
        grid_reveal_line(ctx, colors.x_const, 0, v);  /* x-const, runs along Z */
        grid_reveal_line(ctx, colors.z_const, 1, v);  /* z-const, runs along X */
        return;
    }
    /* Radial dissolve by world distance from the origin. A line at
     * perpendicular offset v fades where sqrt(v^2 + a^2) crosses the
     * front, so the breakpoints along the running axis are the radii
     * where r hits fade_start / fade_end - same for both axes at this
     * offset. The x-const line runs along z, the z-const line along x. */
    float fe = ctx->ef_fade_end, band = ctx->ef_band, extent = ctx->extent;
    float av = fabsf(v);
    if (av >= fe) return;                       /* whole line past the front */
    float fs = fe - band;
    float a_end = sqrtf(fe * fe - v * v);       /* r == fe here (fe > av) */
    if (a_end > extent) a_end = extent;
    float a_start = (fs > av) ? sqrtf(fs * fs - v * v) : 0.0f;

    float pos[5];
    int n = 0;
    pos[n++] = -a_end;
    if (a_start > 0.0f && a_start < a_end) pos[n++] = -a_start;
    pos[n++] = 0.0f;
    if (a_start > 0.0f && a_start < a_end) pos[n++] = a_start;
    pos[n++] = a_end;

    for (int i = 0; i + 1 < n; i++) {
        float a0 = pos[i], a1 = pos[i + 1];
        float m0 = grid_radial_mul(fe, band, v, a0);
        float m1 = grid_radial_mul(fe, band, v, a1);
        Render3dRgba c = colors.x_const;
        grid_color(ctx, c.r, c.g, c.b, c.a * m0); glVertex3f(v, 0, a0);
        grid_color(ctx, c.r, c.g, c.b, c.a * m1); glVertex3f(v, 0, a1);
        c = colors.z_const;
        grid_color(ctx, c.r, c.g, c.b, c.a * m0); glVertex3f(a0, 0, v);
        grid_color(ctx, c.r, c.g, c.b, c.a * m1); glVertex3f(a1, 0, v);
    }
}

/* Emit the two origin axes - X at colors.x_const, Z at colors.z_const -
 * honoring whichever fade path is live. Split out of draw_grid_origin_axes
 * so the casing pass can reuse it verbatim; the caller owns line width,
 * depth mask and the GL_LINES block. */
static void emit_grid_origin_axes(const GridDrawContext *ctx,
                                  GridLineColors colors) {
    Render3dRgba cx = colors.x_const, cz = colors.z_const;
    if (grid_reveal_active(ctx)) {
        grid_reveal_line(ctx, cx, 1, 0.0f);  /* X axis, runs along X */
        grid_reveal_line(ctx, cz, 0, 0.0f);  /* Z axis, runs along Z */
    } else if (!ctx->edge_fade) {
        grid_color_rgba(ctx, cx);
        glVertex3f(-ctx->extent, 0, 0);
        glVertex3f( ctx->extent, 0, 0);
        grid_color_rgba(ctx, cz);
        glVertex3f(0, 0, -ctx->extent);
        glVertex3f(0, 0,  ctx->extent);
    } else {
        /* X axis runs along x (z=0); Z axis runs along z (x=0). */
        for (int i = 0; i + 1 < ctx->ef_n; i++) {
            float m0 = ctx->ef_mul[i], m1 = ctx->ef_mul[i + 1];
            float p0 = ctx->ef_bp[i],  p1 = ctx->ef_bp[i + 1];
            grid_color(ctx, cx.r, cx.g, cx.b, cx.a * m0);
            glVertex3f(p0, 0, 0);
            grid_color(ctx, cx.r, cx.g, cx.b, cx.a * m1);
            glVertex3f(p1, 0, 0);
            grid_color(ctx, cz.r, cz.g, cz.b, cz.a * m0);
            glVertex3f(0, 0, p0);
            grid_color(ctx, cz.r, cz.g, cz.b, cz.a * m1);
            glVertex3f(0, 0, p1);
        }
    }
}

static void draw_grid_origin_axes(const GridDrawContext *ctx,
                                  GridLineColors colors, float line_width) {
    /* Casing first, and with the depth mask still FALSE: the axes are the
     * one grid pass that writes depth, and a casing that wrote it would sit
     * at exactly the core's depth and fail the core's GL_LESS test. The
     * core below still writes, so the axes keep occluding as before. */
    float casing_a = grid_casing_alpha(ctx);
    if (casing_a > 0.0f) {
        GridDrawContext cctx = grid_casing_ctx(ctx);
        GridLineColors casing;
        casing.x_const = grid_contrast_color(
            grid_casing_color(ctx, casing_a, colors.x_const.a), colors.x_const);
        casing.z_const = grid_contrast_color(
            grid_casing_color(ctx, casing_a, colors.z_const.a), colors.z_const);
        glLineWidth(line_width + GRID_CASING_WIDTH_ADD);
        grid_contrast_blend_begin();
        glBegin(GL_LINES);
        emit_grid_origin_axes(&cctx, casing);
        glEnd();
        grid_contrast_blend_end();
    }

    glDepthMask(GL_TRUE);
    if (line_width != 1.0f)
        glLineWidth(line_width);
    else if (casing_a > 0.0f)
        glLineWidth(1.0f);
    glBegin(GL_LINES);
    emit_grid_origin_axes(ctx, colors);
    glEnd();
    if (line_width != 1.0f)
        glLineWidth(1.0f);
    glDepthMask(GL_FALSE);
}

/* One casing pass over the lines of a single rank (want_major 0/1), at one
 * width. Split by rank because a glLineWidth cannot change inside a
 * GL_LINES block, and majors and minors want different widths.
 *
 * Walks the same v sequence as the core loop rather than stepping by
 * ctx->major, so "which lines are major" is decided by one predicate and a
 * casing can never land half a tolerance off its line. */
static void draw_grid_casing_lines(const GridDrawContext *ctx,
                                   const GridThemeSpec *spec, float casing_a,
                                   int want_major, float width_add) {
    GridDrawContext cctx = grid_casing_ctx(ctx);
    glLineWidth(1.0f + width_add);
    grid_contrast_blend_begin();
    glBegin(GL_LINES);
    for (float v = -ctx->extent; v <= ctx->extent + GRID_LOOP_EPSILON;
         v += ctx->step) {
        if (fabsf(v) < GRID_ORIGIN_SKIP_EPSILON) continue;
        if (grid_is_major_line(v, ctx->major, ctx->major_tol) != want_major)
            continue;
        GridLineColors colors, casing;
        spec->line_color(v, want_major, ctx, &colors);
        casing.x_const = grid_contrast_color(
            grid_casing_color(ctx, casing_a, colors.x_const.a),
            colors.x_const);
        casing.z_const = grid_contrast_color(
            grid_casing_color(ctx, casing_a, colors.z_const.a),
            colors.z_const);
        draw_grid_line_pair(v, &cctx, casing);
    }
    glEnd();
    grid_contrast_blend_end();
    glLineWidth(1.0f);
}

static void draw_grid_standard_theme(const GridDrawContext *ctx,
                                     const GridThemeSpec *spec) {
    if (spec->begin_pass)
        spec->begin_pass(ctx);

    /* Contrast casing under the whole graticule (see GRID_CASING_*). Both
     * ranks run before the core loop, so every casing stays under every
     * line - a minor's shadow can never land on top of a major's core at a
     * crossing. Minors first, so where the two overlap the major's wider,
     * stronger shadow is what survives. */
    float casing_a = grid_casing_alpha(ctx);
    if (casing_a > 0.0f) {
        draw_grid_casing_lines(ctx, spec, casing_a, 0,
                               GRID_CASING_MINOR_W_ADD);
        draw_grid_casing_lines(ctx, spec, casing_a, 1, GRID_CASING_WIDTH_ADD);
    }

    glBegin(GL_LINES);
    for (float v = -ctx->extent; v <= ctx->extent + GRID_LOOP_EPSILON;
         v += ctx->step) {
        if (fabsf(v) < GRID_ORIGIN_SKIP_EPSILON) continue;
        int is_major = grid_is_major_line(v, ctx->major, ctx->major_tol);
        GridLineColors colors;
        spec->line_color(v, is_major, ctx, &colors);
        colors.x_const.a = fminf(colors.x_const.a * ctx->alpha_scale, 1.0f);
        colors.z_const.a = fminf(colors.z_const.a * ctx->alpha_scale, 1.0f);
        draw_grid_line_pair(v, ctx, colors);
    }
    glEnd();

    GridLineColors origin_c;
    spec->origin_color(ctx, &origin_c);
    origin_c.x_const.a = fminf(origin_c.x_const.a * ctx->alpha_scale, 1.0f);
    origin_c.z_const.a = fminf(origin_c.z_const.a * ctx->alpha_scale, 1.0f);
    draw_grid_origin_axes(ctx, origin_c, spec->origin_line_width);

    if (spec->end_pass)
        spec->end_pass(ctx);
}

static void grid_classic_line_color(float v, int is_major,
                                    const GridDrawContext *ctx,
                                    GridLineColors *out) {
    (void)v;
    (void)ctx;
    grid_line_colors_same(out, rgba(0.50f, 0.50f, 0.60f,
                                    is_major ? 0.22f : 0.08f));
}

static void grid_classic_origin_color(const GridDrawContext *ctx,
                                      GridLineColors *out) {
    (void)ctx;
    grid_line_colors_same(out, rgba(0.50f, 0.50f, 0.60f, 0.45f));
}

static void grid_fog_end(const GridDrawContext *ctx) {
    (void)ctx;
    glDisable(GL_FOG);
}

static void grid_tron_line_color(float v, int is_major,
                                 const GridDrawContext *ctx,
                                 GridLineColors *out) {
    float glow = 0.7f + ctx->breath * 0.3f;
    float dist = fabsf(v) / ctx->extent;
    float fade = (1.0f - dist * dist);
    if (fade < 0.0f) fade = 0.0f;
    float base = is_major ? 0.35f : 0.12f;
    grid_line_colors_same(out, rgba(0.05f, 0.55f, 0.95f,
                                    base * fade * glow));
}

static void grid_tron_origin_color(const GridDrawContext *ctx,
                                   GridLineColors *out) {
    float glow = 0.7f + ctx->breath * 0.3f;
    grid_line_colors_same(out, rgba(0.0f, 0.8f, 1.0f, 0.25f * glow));
}

static void grid_ember_line_color(float v, int is_major,
                                  const GridDrawContext *ctx,
                                  GridLineColors *out) {
    float dist = fabsf(v) / ctx->extent;
    float ripple = sinf(dist * 12.0f - ctx->anim_time * 2.5f);
    ripple = ripple * 0.5f + 0.5f;
    float fade = 1.0f - dist;
    if (fade < 0.0f) fade = 0.0f;
    float base = is_major ? 0.30f : 0.10f;
    float a = base * fade * (0.6f + ripple * 0.4f);
    grid_line_colors_same(out, rgba(0.95f, 0.35f + ripple * 0.25f,
                                    0.05f, a));
}

static void grid_ember_origin_color(const GridDrawContext *ctx,
                                    GridLineColors *out) {
    float ripple0 = -sinf(ctx->anim_time * 2.5f) * 0.5f + 0.5f;
    grid_line_colors_same(out, rgba(0.95f, 0.35f + ripple0 * 0.25f, 0.05f,
                                    0.7f * (0.6f + ripple0 * 0.4f)));
}

/* Aurora: an arctic night. The floor is frosty ice-blue lines with a slow
 * green "reflection" band sweeping across them; the pseudo-scene layer (the
 * Ocean water-surface analogue) is a pair of additive-blended aurora
 * curtains waving high over the grid, green at the base fading to a violet
 * fringe. The curtains ride the standard-theme end_pass hook, so the theme
 * dispatches through the GridThemeSpec table like any other. */
static void grid_aurora_line_color(float v, int is_major,
                                   const GridDrawContext *ctx,
                                   GridLineColors *out) {
    float dist = fabsf(v) / ctx->extent;
    float fade = 1.0f - dist * dist;
    if (fade < 0.0f) fade = 0.0f;
    /* travelling reflection band: ice blue -> aurora green as it passes */
    float shimmer = sinf(v * 0.55f + ctx->anim_time * 0.7f) * 0.5f + 0.5f;
    float base = is_major ? 0.26f : 0.10f;
    grid_line_colors_same(out, rgba(0.42f - 0.20f * shimmer,
                                    0.62f + 0.30f * shimmer,
                                    0.88f - 0.18f * shimmer,
                                    base * fade * (0.7f + shimmer * 0.3f)));
}

static void grid_aurora_origin_color(const GridDrawContext *ctx,
                                     GridLineColors *out) {
    float s0 = sinf(ctx->anim_time * 0.7f) * 0.5f + 0.5f;
    grid_line_colors_same(out, rgba(0.50f, 0.80f + 0.10f * s0, 0.95f, 0.50f));
}


/* Synthwave: the floor companion to the Sunset backdrop. Hot neon-pink
 * lines with breath-pulsed glow; the cross (constant-z) lines carry a
 * brightness wave scrolling toward the camera to fake the classic
 * drive-into-the-horizon grid motion, while the rails (constant-x)
 * stay steady so the floor doesn't strobe. */
static void grid_synthwave_line_color(float v, int is_major,
                                      const GridDrawContext *ctx,
                                      GridLineColors *out) {
    float dist = fabsf(v) / ctx->extent;
    float fade = 1.0f - dist * dist;
    if (fade < 0.0f) fade = 0.0f;
    float glow = 0.75f + ctx->breath * 0.25f;
    /* Hot bases - the Sunset backdrop's bright horizon glow sits right
     * behind these lines, so they need to punch well above the
     * Tron-level alphas to read as neon rather than silhouette. */
    float base = is_major ? 0.62f : 0.22f;

    out->x_const = rgba(1.0f, 0.18f, 0.72f, base * fade * glow);

    float wave = sinf(v * 1.8f - ctx->anim_time * 2.2f) * 0.5f + 0.5f;
    out->z_const = rgba(1.0f, 0.18f + 0.24f * wave, 0.72f + 0.22f * wave,
                        base * fade * (0.65f + 0.55f * wave) * glow);
}

static void grid_synthwave_origin_color(const GridDrawContext *ctx,
                                        GridLineColors *out) {
    /* Cyan accent against the pink field, pulsing with the same breath. */
    float glow = 0.75f + ctx->breath * 0.25f;
    grid_line_colors_same(out, rgba(0.15f, 0.85f, 1.0f, 0.40f * glow));
}

static void grid_ruler_line_color(float v, int is_major,
                                  const GridDrawContext *ctx,
                                  GridLineColors *out) {
    float dist_frac = fabsf(v) / ctx->extent;
    float fade = 1.0f - dist_frac * dist_frac;
    float a = (is_major ? 0.18f : 0.07f) * fade;
    /* Near-neutral warm/cool grays: the directional coding survives as a
     * whisper, but hundreds of lines compressing at the horizon do not
     * pile up into a saturated orange/blue wash (accent-palette rework). */
    out->x_const = rgba(0.62f, 0.56f, 0.50f, a);
    out->z_const = rgba(0.50f, 0.56f, 0.66f, a);
}

/* Origin meridians, directionally coded: warm key on X, cool key on Z, from
 * the shared accent palette's role map (accent_palette.h) so the ruler
 * retunes with the examples. This two-color pair is the reason
 * GridOriginColorFn fills a GridLineColors instead of returning one color. */
static void grid_ruler_origin_color(const GridDrawContext *ctx,
                                    GridLineColors *out) {
    (void)ctx;
    const float *ax = palette_anchor_rgb(PAL_ROLE_WARM_KEY);
    const float *az = palette_anchor_rgb(PAL_ROLE_COOL_KEY);
    out->x_const = rgba(ax[0], ax[1], ax[2], 0.60f);
    out->z_const = rgba(az[0], az[1], az[2], 0.60f);
}

/* Ruler tick marks at major-line intervals on both axes, in the same warm/
 * cool coding as the meridians they cross. Rides end_pass after the origin
 * axes, so the ticks follow the axes. */
static void grid_ruler_end_pass(const GridDrawContext *ctx) {
    const float *ax = palette_anchor_rgb(PAL_ROLE_WARM_KEY);
    const float *az = palette_anchor_rgb(PAL_ROLE_COOL_KEY);
    float extent = ctx->extent, major = ctx->major, as = ctx->alpha_scale;
    float tick = 0.06f;
    glBegin(GL_LINES);
    for (float v = -extent; v <= extent + GRID_LOOP_EPSILON; v += major) {
        if (fabsf(v) < GRID_ORIGIN_SKIP_EPSILON) continue;
        float ta = (fabsf(v) <= major * 2.5f) ? 0.44f : 0.20f;
        ta = fminf(ta * as, 1.0f) * grid_edge_fade_mul(ctx, v);
        /* Ticks crossing the X axis in the Z direction - warm key */
        grid_color(ctx, ax[0], ax[1], ax[2], ta);
        glVertex3f(v, 0, -tick); glVertex3f(v, 0, tick);
        /* Ticks crossing the Z axis in the X direction - cool key */
        grid_color(ctx, az[0], az[1], az[2], ta);
        glVertex3f(-tick, 0, v); glVertex3f(tick, 0, v);
    }
    glEnd();
}

static const GridThemeSpec g_grid_theme_specs[GRID_THEME_COUNT] = {
    [GRID_THEME_CLASSIC] = {
        grid_classic_line_color, grid_classic_origin_color, NULL, NULL, 1.0f
    },
    [GRID_THEME_TRON] = {
        grid_tron_line_color, grid_tron_origin_color, NULL, NULL, 2.0f
    },
    [GRID_THEME_EMBER] = {
        grid_ember_line_color, grid_ember_origin_color, NULL, NULL, 1.0f
    },
    [GRID_THEME_AURORA] = {
        grid_aurora_line_color, grid_aurora_origin_color, NULL,
        NULL, 1.0f
    },
    [GRID_THEME_SYNTHWAVE] = {
        grid_synthwave_line_color, grid_synthwave_origin_color, NULL, NULL,
        2.0f
    },
    [GRID_THEME_XZRULER] = {
        grid_ruler_line_color, grid_ruler_origin_color, NULL,
        grid_ruler_end_pass, 2.0f
    },
};

static const GridThemeSpec *grid_theme_spec(Render3dGridTheme theme) {
    if (theme <= GRID_THEME_OFF || theme >= GRID_THEME_COUNT)
        return NULL;
    if (!g_grid_theme_specs[theme].line_color)
        return NULL;
    return &g_grid_theme_specs[theme];
}

/* Fill the active scene viewport with a tint rect (Ocean's underwater
 * teal, Frozen's under-ice glacial blue). Coordinates use
 * render3d_w/render3d_h (not the full window viewport) so the rect lines up
 * with whatever glViewport render3d_render set. */
static void grid_draw_viewport_tint(const GridDrawContext *grid_ctx,
                                    const Render3dRenderConfig *config,
                                    float r, float g, float b, float a) {
    grid_color_surface(grid_ctx, r, g, b, a);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluOrtho2D(0, config->render3d_w, 0, config->render3d_h);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    /* GL_FOG_BIT is on the push mask because the outer grid pass
     * leaves fog enabled (linear, end about grid extent) - and after
     * fb976f0 it may also have GL_FOG_DISTANCE_MODE_NV =
     * GL_EYE_RADIAL_NV set when the OCEAN theme is the dispatch
     * branch. With identity modelview the rect's eye-space radial
     * distances run 0..sqrt(render3d_w^2 + render3d_h^2), wildly past
     * fog-end, so without the disable the rect fades to fog colour
     * everywhere except the tiny lower-left near (0,0). See
     * tests/test_scene_underwater_fill_gl.c. */
    glPushAttrib(GL_DEPTH_BUFFER_BIT | GL_LIGHTING_BIT | GL_FOG_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glRectf(0, 0, (float)config->render3d_w, (float)config->render3d_h);
    glPopAttrib();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

static void render3d_grid_render_ocean_theme(const GridDrawContext *grid_ctx,
                                          const Render3dFrameRenderContext *frame_ctx,
                                          float breath) {
    const float extent = grid_ctx->extent;
    const float major = grid_ctx->major;
    const float major_tol = grid_ctx->major_tol;
    const float step = grid_ctx->step;
    const Render3dRenderConfig *config = &frame_ctx->config;

    /* Underwater fog - slightly breathing density */
    if (frame_ctx->camera_world_pos[1] < 0.0f) {
        grid_draw_viewport_tint(grid_ctx, config, 0.05f, 0.25f, 0.35f, 0.75f);
    } else {
        glEnable(GL_FOG);
        glFogi(GL_FOG_MODE, GL_EXP2);
        glFogf(GL_FOG_DENSITY, 0.045f + breath * 0.015f);
    }

    /* Ocean floor grid with animated caustic highlights */
    float as = grid_ctx->alpha_scale;
    glBegin(GL_LINES);
    for (float v = -extent; v <= extent + GRID_LOOP_EPSILON; v += step) {
        if (fabsf(v) < GRID_ORIGIN_SKIP_EPSILON) continue;
        int is_major  = grid_is_major_line(v, major, major_tol);
        float base_a  = is_major ? 0.55f : 0.28f;

        float c1 = sinf(v * 3.0f + grid_ctx->anim_time * 1.3f);
        float c2 = cosf(v * 2.3f - grid_ctx->anim_time * 0.9f);
        float caustic = (c1 * c2) * 0.5f + 0.5f;   /* 0..1 */
        float a = fminf(base_a * (0.5f + caustic * 0.5f) * as, 1.0f);

        float r = 0.10f + caustic * 0.35f;
        float g = 0.35f + caustic * 0.60f;
        float b = 0.45f + caustic * 0.50f;
        grid_color(grid_ctx, r, g, b, a);
        glVertex3f(v, 0, -extent);  glVertex3f(v, 0, extent);
        glVertex3f(-extent, 0, v);  glVertex3f(extent, 0, v);
    }
    glEnd();
    /* Origin axes - write to depth buffer; colour evaluated at v=0 */
    {
        float c1_o = sinf(grid_ctx->anim_time * 1.3f);
        float c2_o = cosf(-grid_ctx->anim_time * 0.9f);
        float caustic_o = (c1_o * c2_o) * 0.5f + 0.5f;
        float a_o = fminf(0.95f * (0.5f + caustic_o * 0.5f) * as, 1.0f);
        float r_o = 0.10f + caustic_o * 0.35f;
        float g_o = 0.35f + caustic_o * 0.60f;
        float b_o = 0.45f + caustic_o * 0.50f;
        glDepthMask(GL_TRUE);
        glBegin(GL_LINES);
        grid_color(grid_ctx, r_o, g_o, b_o, a_o);
        glVertex3f(-extent, 0, 0); glVertex3f(extent, 0, 0);
        glVertex3f(0, 0, -extent); glVertex3f(0, 0, extent);
        glEnd();
        glDepthMask(GL_FALSE);
    }

    /* Same teardown as the standard fog theme. */
    grid_fog_end(grid_ctx);

    /* ---- Water surface plane ----
     * A semi-transparent rippling mesh at Y about 0. Because the grid pass
     * runs after execute_commands(), this overlay tints everything the user
     * drew below the surface, producing the underwater look. Depth-test is
     * on but depth-write is off (set at the top of render3d_grid_render), so the
     * surface correctly occludes only geometry that sits behind it from the
     * camera's point of view. */
    float surf_step = 0.75f;
    float surf_y    = 0.01f;   /* tiny offset above grid floor */

    for (float sz = -extent; sz < extent - GRID_LOOP_EPSILON; sz += surf_step) {
        glBegin(GL_TRIANGLE_STRIP);
        for (float sx = -extent; sx <= extent + GRID_LOOP_EPSILON;
             sx += surf_step) {
            for (int row = 0; row < 2; row++) {
                float zz = sz + row * surf_step;

                /* Composite wave displacement (3 octaves) */
                float w = sinf(sx * 1.5f + grid_ctx->anim_time * 0.7f) * 0.025f
                        + cosf(zz * 1.8f + grid_ctx->anim_time * 0.5f) * 0.018f
                        + sinf((sx + zz) * 0.8f + grid_ctx->anim_time * 1.0f) * 0.012f;
                float y = surf_y + w;

                /* Smooth edge fade so the surface has no hard border */
                float dx = fabsf(sx) / extent;
                float dz = fabsf(zz) / extent;
                float edge = (1.0f - dx * dx) * (1.0f - dz * dz);
                if (edge < 0.0f) edge = 0.0f;

                float alpha = 0.62f * edge;

                /* Subtle colour variation across surface */
                float cr = sinf(sx * 0.4f + grid_ctx->anim_time * 0.3f) * 0.04f;
                float cg = cosf(zz * 0.3f + grid_ctx->anim_time * 0.25f) * 0.04f;
                grid_color_surface(grid_ctx, 0.05f + cr, 0.25f + cg,
                                   0.35f + cr, alpha);
                glVertex3f(sx, y, zz);
            }
        }
        glEnd();
    }
}

/* Frozen Lake: a winter pond. Major grid lines are fracture cracks -
 * jittered polylines with a wide faint halo, a bright core, and a
 * dimmer echo sunk below the surface so the sheet reads as thick;
 * minor lines stay faint straight etch marks under the ice. The
 * pseudo-scene layer (the Ocean water-surface analogue) is a
 * translucent blue-white ice sheet just above Y=0 with static
 * frost-heave displacement and a slow sheen drifting across it.
 * Below Y=0 the viewport gets a pale glacial tint (same mechanism as
 * Ocean's underwater fill) - looking up through the ice. */

/* Deterministic position hash in [-1, 1]; stable frame-to-frame so
 * the cracks and frost heave stay frozen in place (no swimming). The range
 * adapter maps the shared render3d_hash01 result into [-1, 1]. */
static float grid_pos_hash(float a, float b) {
    return render3d_hash01(a, b) * 2.0f - 1.0f;
}

/* Crack segment count scales with extent so the kinks stay ~1.5 world
 * units apart and read as fracture angles rather than stretching into
 * smooth long waves; capped to bound immediate-mode cost at FAR. */
static int grid_frozen_crack_segs(const GridDrawContext *ctx) {
    int segs = (int)(ctx->extent * 2.0f / 1.5f);
    if (segs < 8) segs = 8;
    if (segs > 40) segs = 40;
    return segs;
}

/* Lateral jitter of crack joint i on major line v. Scaled by
 * sin(pi*f) so both endpoints stay exactly on the grid edge and the
 * crack still reads as that grid line. Shared by the path and spur
 * passes so spurs anchor exactly on the polyline joints. */
static float grid_frozen_joint_off(const GridDrawContext *ctx, float v,
                                   int axis, int i, int segs) {
    float f = (float)i / (float)segs;
    float amp = ctx->major * 0.20f * sinf(f * (float)M_PI);
    return grid_pos_hash(v * 3.7f + (float)axis * 17.0f,
                            (float)i * 5.1f) * amp;
}

/* One fracture polyline replacing a straight major grid line.
 * axis 0 draws the constant-x line (running along Z), axis 1 the
 * constant-z line. y < 0 renders the sub-surface echo pass. */
static void grid_frozen_crack_path(const GridDrawContext *ctx, float v,
                                   int axis, float y, Render3dRgba c) {
    int segs = grid_frozen_crack_segs(ctx);
    glBegin(GL_LINE_STRIP);
    grid_color_rgba(ctx, c);
    for (int i = 0; i <= segs; i++) {
        float f = (float)i / (float)segs;
        float along = -ctx->extent + f * 2.0f * ctx->extent;
        float off = grid_frozen_joint_off(ctx, v, axis, i, segs);
        if (axis == 0) glVertex3f(v + off, y, along);
        else           glVertex3f(along, y, v + off);
    }
    glEnd();
}

/* Short branch spurs off some crack joints, fading to nothing at the
 * tip. Joint selection hashes on (v, i) so the pattern is static. */
static void grid_frozen_crack_spurs(const GridDrawContext *ctx, float v,
                                    int axis, float as) {
    int segs = grid_frozen_crack_segs(ctx);
    glBegin(GL_LINES);
    for (int i = 2; i < segs - 1; i += 3) {
        float pick = grid_pos_hash(v * 9.3f + (float)axis * 23.0f,
                                      (float)i);
        if (pick < 0.25f) continue;
        float f = (float)i / (float)segs;
        float along = -ctx->extent + f * 2.0f * ctx->extent;
        float off = grid_frozen_joint_off(ctx, v, axis, i, segs);
        float len = ctx->major *
                    (0.25f + 0.35f * fabsf(grid_pos_hash(v, (float)i + 41.0f)));
        float dir = (pick > 0.6f) ? 1.0f : -1.0f;
        float dx, dz; /* diagonal away from the crack */
        if (axis == 0) { dx = dir * len * 0.7f; dz = len; }
        else           { dx = len; dz = dir * len * 0.7f; }
        grid_color(ctx, 0.85f, 0.93f, 1.0f, fminf(0.38f * as, 1.0f));
        if (axis == 0) glVertex3f(v + off, 0.0f, along);
        else           glVertex3f(along, 0.0f, v + off);
        grid_color(ctx, 0.85f, 0.93f, 1.0f, 0.0f);
        if (axis == 0) glVertex3f(v + off + dx, 0.0f, along + dz);
        else           glVertex3f(along + dx, 0.0f, v + off + dz);
    }
    glEnd();
}

static void render3d_grid_render_frozen_theme(const GridDrawContext *grid_ctx,
                                           const Render3dFrameRenderContext *frame_ctx) {
    const Render3dRenderConfig *config = &frame_ctx->config;
    const float extent = grid_ctx->extent;
    const float major = grid_ctx->major;
    const float major_tol = grid_ctx->major_tol;
    const float step = grid_ctx->step;
    const float as = grid_ctx->alpha_scale;

    /* Looking up through the ice: pale glacial viewport tint plus a
     * dense EXP2 mist in the same colour, so the grid overhead fades
     * into the murk. Above ground the theme carries no fog of its own
     * (the Polar Day backdrop's glacial horizon is the intended
     * pairing) - but at the FAR extent the generic recede fog is
     * already enabled and keyed to the clear color, which would fade
     * the far field to black; recolor it glacial so the recede whites
     * out into the same tint instead. (LINEAR params stay; non-FAR
     * steady frames remain fog-call-free, which the fog<->predicate
     * test pins.) The tint rect itself is immune to the mist
     * (grid_draw_viewport_tint brackets and disables fog). */
    if (frame_ctx->camera_world_pos[1] < 0.0f) {
        grid_draw_viewport_tint(grid_ctx, config,
                                RENDER3D_GLACIAL_TINT_R, RENDER3D_GLACIAL_TINT_G,
                                RENDER3D_GLACIAL_TINT_B, 0.65f);
        float fog_col[4] = { RENDER3D_GLACIAL_TINT_R, RENDER3D_GLACIAL_TINT_G,
                             RENDER3D_GLACIAL_TINT_B, 1.0f };
        glFogfv(GL_FOG_COLOR, fog_col);
        glFogi(GL_FOG_MODE, GL_EXP2);
        glFogf(GL_FOG_DENSITY, 0.040f + grid_ctx->breath * 0.008f);
        glEnable(GL_FOG);
    } else if (config->grid_extent_idx == GRID_EXTENT_FAR) {
        float fog_col[4] = { RENDER3D_GLACIAL_TINT_R, RENDER3D_GLACIAL_TINT_G,
                             RENDER3D_GLACIAL_TINT_B, 1.0f };
        glFogfv(GL_FOG_COLOR, fog_col);
    }

    /* Minor lines: faint straight etch marks under the ice. Majors
     * are skipped here - the crack pass below replaces them. */
    glBegin(GL_LINES);
    for (float v = -extent; v <= extent + GRID_LOOP_EPSILON; v += step) {
        if (fabsf(v) < GRID_ORIGIN_SKIP_EPSILON) continue;
        if (grid_is_major_line(v, major, major_tol)) continue;
        GridLineColors colors;
        grid_line_colors_same(&colors, rgba(0.55f, 0.70f, 0.82f,
                                            fminf(0.07f * as, 1.0f)));
        draw_grid_line_pair(v, grid_ctx, colors);
    }
    glEnd();

    /* Major lines as fracture cracks: wide faint halo under a bright
     * core, a dim echo sunk into the sheet for thickness, then branch
     * spurs. A slow glint drifts along the cores so the ice isn't
     * completely dead. */
    for (float v = -extent; v <= extent + GRID_LOOP_EPSILON; v += major) {
        if (fabsf(v) < GRID_ORIGIN_SKIP_EPSILON) continue;
        float glint = sinf(v * 0.9f + grid_ctx->anim_time * 0.5f) * 0.5f + 0.5f;
        float core_a = fminf((0.48f + glint * 0.20f) * as, 1.0f);
        for (int axis = 0; axis < 2; axis++) {
            glLineWidth(3.0f);
            grid_frozen_crack_path(grid_ctx, v, axis, 0.0f,
                                   rgba(0.70f, 0.85f, 0.95f,
                                        fminf(0.10f * as, 1.0f)));
            glLineWidth(1.0f);
            grid_frozen_crack_path(grid_ctx, v, axis, 0.0f,
                                   rgba(0.88f, 0.95f, 1.0f, core_a));
            grid_frozen_crack_path(grid_ctx, v, axis, -0.16f,
                                   rgba(0.45f, 0.62f, 0.78f, core_a * 0.35f));
            grid_frozen_crack_spurs(grid_ctx, v, axis, as);
        }
    }

    /* Origin axes: brighter frozen seams (depth-written like every
     * theme's origin pass). */
    draw_grid_origin_axes(grid_ctx,
                          grid_line_colors_of(
                              rgba(0.80f, 0.92f, 1.0f, fminf(0.55f * as, 1.0f))),
                          1.5f);

    /* ---- Ice sheet ----
     * Translucent blue-white plane just above the cracks. Static
     * frost-heave displacement (position hash, not time) keeps it
     * frozen while a slow sheen drifts across. Same depth-test-on /
     * depth-write-off contract as Ocean's water surface, so it tints
     * the user's geometry below the sheet. Step scales with extent to
     * cap strip density at FAR. */
    float surf_step = extent / 33.0f;
    if (surf_step < 0.75f) surf_step = 0.75f;
    float surf_y = 0.02f;
    for (float sz = -extent; sz < extent - GRID_LOOP_EPSILON; sz += surf_step) {
        glBegin(GL_TRIANGLE_STRIP);
        for (float sx = -extent; sx <= extent + GRID_LOOP_EPSILON;
             sx += surf_step) {
            for (int row = 0; row < 2; row++) {
                float zz = sz + row * surf_step;
                float y = surf_y + grid_pos_hash(sx * 0.83f, zz * 0.83f) * 0.012f;

                /* Smooth edge fade so the sheet has no hard border */
                float dx = fabsf(sx) / extent;
                float dz = fabsf(zz) / extent;
                float edge = (1.0f - dx * dx) * (1.0f - dz * dz);
                if (edge < 0.0f) edge = 0.0f;

                float frost = grid_pos_hash(sx * 0.31f + 11.0f,
                                               zz * 0.31f) * 0.5f + 0.5f;
                float sheen = sinf((sx + zz) * 0.18f +
                                   grid_ctx->anim_time * 0.35f) * 0.5f + 0.5f;
                float alpha = (0.20f + frost * 0.20f + sheen * 0.05f) * edge;
                grid_color_surface(grid_ctx,
                           0.60f + frost * 0.16f + sheen * 0.06f,
                           0.72f + frost * 0.12f + sheen * 0.05f,
                           0.86f + frost * 0.06f + sheen * 0.05f,
                           alpha);
                glVertex3f(sx, y, zz);
            }
        }
        glEnd();
    }

    /* Same teardown as the standard fog theme. Unlike Ocean (which
     * drops fog before its water surface) the sheet above renders
     * inside the mist on purpose - it whites out toward the horizon. */
    grid_fog_end(grid_ctx);
}

/* Tilled Field: a plowed-earth floor where the grid's two directions
 * are deliberately asymmetric. Constant-z major lines ARE the furrows
 * - V-grooves cut into an opaque soil height-field running along X -
 * while constant-x majors stay shallow: pale "planting string" lines
 * laid over the ridges. Minor constant-z lines render as faint rake
 * marks riding the furrow profile. The soil mesh writes depth (it is
 * ground, not a translucent overlay), and a camera below the surface
 * gets a near-black earth filter - underground you see almost
 * nothing, which is the point. */

#define GRID_SOIL_FURROW_HALF_FRAC 0.22f  /* furrow half-width, x major */
#define GRID_SOIL_DEPTH 0.30f             /* furrow depth, world units */

/* Furrow cross-section: smoothstep dip around each constant-z major
 * line. Depends only on z, so any line running along X sits at one
 * constant height - minor rake lines follow the terrain for free. */
static float grid_soil_profile(const GridDrawContext *ctx, float z) {
    float w = ctx->major * GRID_SOIL_FURROW_HALF_FRAC;
    float dz = fabsf(remainderf(z, ctx->major));
    if (dz >= w) return 0.0f;
    float f = 1.0f - dz / w;
    f = f * f * (3.0f - 2.0f * f);
    return -GRID_SOIL_DEPTH * f;
}

/* Per-vertex soil colour: depth-shaded brown (sunlit ridge tops, dark
 * groove bottoms) + a slope-keyed fake sun from -Z + two octaves of
 * positional noise (fine clods, broad soil patches). Opaque at heart;
 * only the outer rim edge-fades so the slab has no hard border. */
static void grid_soil_vertex_color(const GridDrawContext *ctx,
                                   float x, float z, float y) {
    float t = -y / GRID_SOIL_DEPTH;
    if (t > 1.0f) t = 1.0f;
    if (t < 0.0f) t = 0.0f;
    float e = ctx->major * 0.05f;
    float slope = (grid_soil_profile(ctx, z + e) -
                   grid_soil_profile(ctx, z - e)) / (2.0f * e);
    float sun = slope * 0.10f;
    if (sun > 0.10f) sun = 0.10f;
    if (sun < -0.10f) sun = -0.10f;
    float clod  = grid_pos_hash(x * 1.31f, z * 1.73f) * 0.035f;
    float patch = grid_pos_hash(x * 0.06f + 7.0f, z * 0.06f) * 0.030f;
    float dx = fabsf(x) / ctx->extent;
    float dzn = fabsf(z) / ctx->extent;
    float edge = (1.0f - dx * dx) * (1.0f - dzn * dzn);
    if (edge < 0.0f) edge = 0.0f;
    float alpha = edge * 1.8f;
    if (alpha > 1.0f) alpha = 1.0f;
    grid_color_surface(ctx,
               0.40f - 0.24f * t + sun + clod + patch,
               0.29f - 0.18f * t + sun * 0.8f + clod + patch,
               0.18f - 0.11f * t + sun * 0.5f + clod * 0.7f + patch,
               alpha);
}

/* One soil strip between two z breakpoints, sampled along X. Heights
 * carry a small positional jitter; colour is continuous across strips
 * because both key on world coordinates. */
static void grid_soil_strip(const GridDrawContext *ctx, float z0, float z1,
                            float x_step) {
    glBegin(GL_TRIANGLE_STRIP);
    for (float sx = -ctx->extent; sx <= ctx->extent + GRID_LOOP_EPSILON;
         sx += x_step) {
        float zz[2];
        zz[0] = z0;
        zz[1] = z1;
        for (int row = 0; row < 2; row++) {
            float y = grid_soil_profile(ctx, zz[row]) +
                      grid_pos_hash(sx * 0.91f, zz[row] * 0.97f) * 0.008f;
            grid_soil_vertex_color(ctx, sx, zz[row], y);
            glVertex3f(sx, y, zz[row]);
        }
    }
    glEnd();
}

static void render3d_grid_render_soil_theme(const GridDrawContext *grid_ctx,
                                         const Render3dFrameRenderContext *frame_ctx) {
    const Render3dRenderConfig *config = &frame_ctx->config;
    const float extent = grid_ctx->extent;
    const float major = grid_ctx->major;
    const float major_tol = grid_ctx->major_tol;
    const float step = grid_ctx->step;
    const float as = grid_ctx->alpha_scale;
    const float w = major * GRID_SOIL_FURROW_HALF_FRAC;

    /* Buried camera: near-black warm earth filter over the viewport. */
    if (frame_ctx->camera_world_pos[1] < 0.0f)
        grid_draw_viewport_tint(grid_ctx, config, 0.09f, 0.06f, 0.04f, 0.92f);

    /* ---- Soil height-field ----
     * Strips run along X between z breakpoints placed at each furrow's
     * profile points (lip, half-slope, centre) plus the flat span to
     * the next furrow, so the V-grooves are exact regardless of grid
     * spacing. Depth-written: this is opaque ground, and later passes
     * (axes, backdrop) must be occluded by it like real terrain. */
    float x_samp = extent / 25.0f;
    if (x_samp < major) x_samp = major;
    glDepthMask(GL_TRUE);
    float c0 = ceilf((-extent - w) / major) * major; /* first centre */
    for (float c = c0; c - w < extent - GRID_LOOP_EPSILON; c += major) {
        float pts[6];
        pts[0] = c - w;
        pts[1] = c - w * 0.5f;
        pts[2] = c;
        pts[3] = c + w * 0.5f;
        pts[4] = c + w;
        pts[5] = c + major - w; /* flat span to the next furrow's lip */
        for (int i = 0; i < 5; i++) {
            float z0 = pts[i], z1 = pts[i + 1];
            if (z0 < -extent) z0 = -extent;
            if (z1 > extent) z1 = extent;
            if (z1 - z0 < GRID_LOOP_EPSILON) continue;
            grid_soil_strip(grid_ctx, z0, z1, x_samp);
        }
    }
    glDepthMask(GL_FALSE);

    /* ---- Line passes over the soil ----
     * Constant-z lines ride the furrow profile (one constant height
     * per line): majors get a dark seam along the groove bottom,
     * minors faint rake marks on the flats. Constant-x majors are the
     * shallow direction: pale planting-string lines floating just
     * above ridge level. */
    glBegin(GL_LINES);
    for (float v = -extent; v <= extent + GRID_LOOP_EPSILON; v += step) {
        if (fabsf(v) < GRID_ORIGIN_SKIP_EPSILON) continue;
        int is_major = grid_is_major_line(v, major, major_tol);
        if (is_major) {
            /* Furrow-bottom seam (constant z, along X) */
            float yb = grid_soil_profile(grid_ctx, v) + 0.02f;
            grid_color(grid_ctx, 0.10f, 0.07f, 0.05f, fminf(0.50f * as, 1.0f));
            glVertex3f(-extent, yb, v);
            glVertex3f( extent, yb, v);
            /* Planting string (constant x, along Z) */
            grid_color(grid_ctx, 0.72f, 0.62f, 0.42f, fminf(0.28f * as, 1.0f));
            glVertex3f(v, 0.03f, -extent);
            glVertex3f(v, 0.03f,  extent);
        } else {
            /* Rake marks: constant-z minors only - the along-X
             * direction dominates a plowed field, so constant-x
             * minors are deliberately absent. */
            float ym = grid_soil_profile(grid_ctx, v) + 0.02f;
            grid_color(grid_ctx, 0.50f, 0.40f, 0.28f, fminf(0.10f * as, 1.0f));
            glVertex3f(-extent, ym, v);
            glVertex3f( extent, ym, v);
        }
    }
    glEnd();

    /* Origin axes: brighter string lines, depth-written like every
     * theme's origin pass. The X axis (constant z=0) sits in a furrow
     * - lift it to its groove-bottom height so it doesn't vanish. */
    {
        float y0 = grid_soil_profile(grid_ctx, 0.0f) + 0.02f;
        glDepthMask(GL_TRUE);
        glLineWidth(1.5f);
        glBegin(GL_LINES);
        grid_color(grid_ctx, 0.85f, 0.72f, 0.48f, fminf(0.55f * as, 1.0f));
        glVertex3f(-extent, y0, 0);
        glVertex3f( extent, y0, 0);
        glVertex3f(0, 0.03f, -extent);
        glVertex3f(0, 0.03f,  extent);
        glEnd();
        glLineWidth(1.0f);
        glDepthMask(GL_FALSE);
    }
}

/* Star Chart: the floor as a midnight observatory map - the
 * companion piece to the Nebula backdrop (warm gold chart marks
 * under its violet sky). Minor lines are faint indigo graticule
 * etch and major lines slightly brighter chart rules; the content
 * lives at the major-line intersections, where a deterministic
 * subset of nodes renders as twinkling star points (three size
 * bands, parchment gold with a few ice-blue strays) and some
 * neighbouring star nodes are joined by faint inked constellation
 * links, so the floor reads as a star atlas rather than graph
 * paper. All placement keys on grid_pos_hash so the chart is
 * static frame-to-frame; only the twinkle animates. */

#define GRID_CHART_NODE_KEEP 0.55f  /* fraction of intersections inked */
#define GRID_CHART_LINK_KEEP 0.30f  /* chance per neighbour edge */

static float grid_chart_roll(float gx, float gz, float salt) {
    return grid_pos_hash(gx * 1.93f + salt, gz * 2.41f + salt * 0.7f)
           * 0.5f + 0.5f;
}

static int grid_chart_node_is_star(float gx, float gz) {
    return grid_chart_roll(gx, gz, 31.0f) < GRID_CHART_NODE_KEEP;
}

/* Radial edge fade shared by nodes and links so the chart dissolves
 * smoothly at the slab border instead of cutting off. */
static float grid_chart_edge_fade(const GridDrawContext *ctx,
                                  float gx, float gz) {
    float d2 = (gx * gx + gz * gz) / (ctx->extent * ctx->extent);
    float fade = 1.0f - d2;
    return (fade < 0.0f) ? 0.0f : fade;
}

/* Constellation links: per star node, roll independently for the +X,
 * +Z, and +X+Z diagonal neighbour; ink the segment only when both
 * endpoints are star nodes so every line connects two chart marks. */
static void grid_chart_draw_links(const GridDrawContext *ctx) {
    const float ex = ctx->extent;
    const float mj = ctx->major;
    glBegin(GL_LINES);
    for (float gx = -ex; gx <= ex + GRID_LOOP_EPSILON; gx += mj) {
        for (float gz = -ex; gz <= ex + GRID_LOOP_EPSILON; gz += mj) {
            if (!grid_chart_node_is_star(gx, gz)) continue;
            static const float dx_tab[3] = { 1.0f, 0.0f, 1.0f };
            static const float dz_tab[3] = { 0.0f, 1.0f, 1.0f };
            for (int e = 0; e < 3; e++) {
                float nx = gx + dx_tab[e] * mj;
                float nz = gz + dz_tab[e] * mj;
                if (nx > ex + GRID_LOOP_EPSILON ||
                    nz > ex + GRID_LOOP_EPSILON) continue;
                if (grid_chart_roll(gx, gz, 57.0f + 13.0f * (float)e) >=
                    GRID_CHART_LINK_KEEP) continue;
                if (!grid_chart_node_is_star(nx, nz)) continue;
                float fade = grid_chart_edge_fade(ctx, gx, gz);
                /* slow shared shimmer so the inked figures breathe */
                float shim = 0.80f + 0.20f *
                    sinf(ctx->anim_time * 0.4f +
                         grid_chart_roll(gx, gz, 5.0f) * 6.28318f);
                float a = fminf(0.11f * fade * shim * ctx->alpha_scale, 1.0f);
                grid_color(ctx, 0.88f, 0.76f, 0.48f, a);
                glVertex3f(gx, 0.0f, gz);
                glVertex3f(nx, 0.0f, nz);
            }
        }
    }
    glEnd();
}

/* Twinkling star nodes in three point-size bands (batched per size,
 * matching the backdrop star domes). Identity point attenuation is
 * forced like the sky domes so far-corner nodes don't collapse under
 * the init bootstrap's quadratic default. */
static void grid_chart_draw_nodes(const GridDrawContext *ctx,
                                  const Render3dRenderConfig *config) {
    static const float band_sizes[3] = { 2.0f, 3.0f, 4.5f };
    const float ex = ctx->extent;
    const float mj = ctx->major;

    glEnable(GL_POINT_SMOOTH);
    glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
    if (config->point_parameter_supported && config->point_parameter_proc)
        config->point_parameter_proc(GL_POINT_DISTANCE_ATTENUATION,
                                     (GLfloat[]){1, 0, 0});

    for (int band = 0; band < 3; band++) {
        glPointSize(band_sizes[band]);
        glBegin(GL_POINTS);
        for (float gx = -ex; gx <= ex + GRID_LOOP_EPSILON; gx += mj) {
            for (float gz = -ex; gz <= ex + GRID_LOOP_EPSILON; gz += mj) {
                if (fabsf(gx) < GRID_ORIGIN_SKIP_EPSILON &&
                    fabsf(gz) < GRID_ORIGIN_SKIP_EPSILON) continue;
                if (!grid_chart_node_is_star(gx, gz)) continue;
                int b = (int)(grid_chart_roll(gx, gz, 71.0f) * 3.0f);
                if (b > 2) b = 2;
                if (b != band) continue;

                float fade = grid_chart_edge_fade(ctx, gx, gz);
                if (fade <= 0.001f) continue;

                /* per-node twinkle: hashed phase + speed */
                float phase = grid_chart_roll(gx, gz, 91.0f) * 6.28318f;
                float speed = 0.4f + grid_chart_roll(gx, gz, 113.0f) * 1.6f;
                float tw = 0.5f + 0.5f * sinf(ctx->anim_time * speed + phase);
                float a = fminf((0.22f + 0.55f * tw) * fade *
                                ctx->alpha_scale, 1.0f);

                if (grid_chart_roll(gx, gz, 137.0f) < 0.70f)
                    grid_color(ctx, 0.95f, 0.85f, 0.58f, a); /* gold */
                else
                    grid_color(ctx, 0.70f, 0.82f, 1.00f, a); /* ice */
                glVertex3f(gx, 0.0f, gz);
            }
        }
        glEnd();
    }
}

static void render3d_grid_render_starchart_theme(const GridDrawContext *grid_ctx,
                                              const Render3dFrameRenderContext *frame_ctx) {
    const Render3dRenderConfig *config = &frame_ctx->config;
    const float extent = grid_ctx->extent;
    const float major = grid_ctx->major;
    const float major_tol = grid_ctx->major_tol;
    const float step = grid_ctx->step;
    const float as = grid_ctx->alpha_scale;

    /* Graticule: deep-indigo etch, majors as brighter chart rules. */
    glBegin(GL_LINES);
    for (float v = -extent; v <= extent + GRID_LOOP_EPSILON; v += step) {
        if (fabsf(v) < GRID_ORIGIN_SKIP_EPSILON) continue;
        int is_major = grid_is_major_line(v, major, major_tol);
        GridLineColors colors;
        if (is_major)
            grid_line_colors_same(&colors, rgba(0.50f, 0.58f, 0.85f,
                                                fminf(0.13f * as, 1.0f)));
        else
            grid_line_colors_same(&colors, rgba(0.30f, 0.36f, 0.62f,
                                                fminf(0.045f * as, 1.0f)));
        draw_grid_line_pair(v, grid_ctx, colors);
    }
    glEnd();

    /* Origin meridians: parchment-gold rules, depth-written like every
     * theme's origin pass. */
    draw_grid_origin_axes(grid_ctx,
                          grid_line_colors_of(
                              rgba(0.92f, 0.80f, 0.52f, fminf(0.40f * as, 1.0f))),
                          1.5f);

    grid_chart_draw_links(grid_ctx);
    grid_chart_draw_nodes(grid_ctx, config);
}

static void render3d_grid_render_planes_theme(const Render3dRenderConfig *config,
                                           const GridDrawContext *grid_ctx) {
    float extent = grid_ctx->extent;
    float major = grid_ctx->major;
    float major_tol = grid_ctx->major_tol;
    float step = grid_ctx->step;
    float as = grid_ctx->alpha_scale;

    /* Determine which vertical plane is most face-on to the camera.
     * Camera horizontal look direction: (sin(ry), 0, -cos(ry))
     * XY plane (z=0, normal Z): face-on weight = cos^2(ry)
     * ZY plane (x=0, normal X): face-on weight = sin^2(ry)
     * These sum to 1, giving a natural blend between the two. */
    float ry_rad = config->cam_ry * (float)M_PI / 180.0f;
    float rx_rad = config->cam_rx * (float)M_PI / 180.0f;
    float cos_ry = cosf(ry_rad), sin_ry = sinf(ry_rad);
    float xy_w = cos_ry * cos_ry;
    float zy_w = sin_ry * sin_ry;

    /* Fade vertical planes out when the camera is near top/bottom view */
    float vert_fade = cosf(rx_rad);
    vert_fade = vert_fade * vert_fade;
    xy_w *= vert_fade;
    zy_w *= vert_fade;

    /* --- Floor grid (XZ plane, always present) --- */
    glBegin(GL_LINES);
    for (float v = -extent; v <= extent + GRID_LOOP_EPSILON; v += step) {
        if (fabsf(v) < GRID_ORIGIN_SKIP_EPSILON) continue;
        int is_major  = grid_is_major_line(v, major, major_tol);
        float a = fminf((is_major ? 0.10f : 0.04f) * as, 1.0f);
        grid_color(grid_ctx, 0.50f, 0.52f, 0.65f, a);
        glVertex3f(v,       0, -extent); glVertex3f(v,      0, extent);
        glVertex3f(-extent, 0, v);       glVertex3f(extent, 0, v);
    }
    glEnd();
    /* Floor origin axes - write to depth buffer */
    glDepthMask(GL_TRUE);
    glBegin(GL_LINES);
    grid_color(grid_ctx, 0.50f, 0.52f, 0.65f, fminf(0.30f * as, 1.0f));
    glVertex3f(-extent, 0, 0); glVertex3f(extent, 0, 0);
    glVertex3f(0, 0, -extent); glVertex3f(0, 0, extent);
    glEnd();
    glDepthMask(GL_FALSE);

    /* --- XY plane (z=0): visible when camera looks along Z axis --- */
    if (xy_w > GRID_PLANE_VISIBILITY_EPSILON) {
        glBegin(GL_LINES);
        for (float v = -extent; v <= extent + GRID_LOOP_EPSILON; v += step) {
            if (fabsf(v) < GRID_ORIGIN_SKIP_EPSILON) continue;
            int is_major  = grid_is_major_line(v, major, major_tol);
            float base = is_major ? 0.14f : 0.05f;
            float a = fminf(base * xy_w * as, 1.0f);
            grid_color(grid_ctx, 0.35f, 0.62f, 0.88f, a);
            glVertex3f(-extent, v, 0); glVertex3f(extent, v, 0);
            grid_color(grid_ctx, 0.35f, 0.62f, 0.88f, a * 0.75f);
            glVertex3f(v, -extent, 0); glVertex3f(v, extent, 0);
        }
        glEnd();
        /* XY plane origin axes - write to depth buffer */
        glDepthMask(GL_TRUE);
        glBegin(GL_LINES);
        grid_color(grid_ctx, 0.35f, 0.62f, 0.88f, fminf(0.42f * xy_w * as, 1.0f));
        glVertex3f(-extent, 0, 0); glVertex3f(extent, 0, 0);  /* X axis */
        glVertex3f(0, -extent, 0); glVertex3f(0, extent, 0);  /* Y axis */
        glEnd();
        glDepthMask(GL_FALSE);
    }

    /* --- ZY plane (x=0): visible when camera looks along X axis --- */
    if (zy_w > GRID_PLANE_VISIBILITY_EPSILON) {
        glBegin(GL_LINES);
        for (float v = -extent; v <= extent + GRID_LOOP_EPSILON; v += step) {
            if (fabsf(v) < GRID_ORIGIN_SKIP_EPSILON) continue;
            int is_major  = grid_is_major_line(v, major, major_tol);
            float base = is_major ? 0.14f : 0.05f;
            float a = fminf(base * zy_w * as, 1.0f);
            grid_color(grid_ctx, 0.82f, 0.52f, 0.28f, a);
            glVertex3f(0, v, -extent); glVertex3f(0, v, extent);
            grid_color(grid_ctx, 0.82f, 0.52f, 0.28f, a * 0.75f);
            glVertex3f(0, -extent, v); glVertex3f(0, extent, v);
        }
        glEnd();
        /* ZY plane origin axes - write to depth buffer */
        glDepthMask(GL_TRUE);
        glBegin(GL_LINES);
        grid_color(grid_ctx, 0.82f, 0.52f, 0.28f, fminf(0.42f * zy_w * as, 1.0f));
        glVertex3f(0, 0, -extent); glVertex3f(0, 0, extent);  /* Z axis */
        glVertex3f(0, -extent, 0); glVertex3f(0, extent, 0);  /* Y axis */
        glEnd();
        glDepthMask(GL_FALSE);
    }
}

/* Radar: faint green range rings + crosshair, a very faint expanding
 * ping ring, and a single faint sweep line rotating on anim_time. */
static void render3d_grid_render_radar_theme(const GridDrawContext *grid_ctx) {
    const float extent = grid_ctx->extent;
    const float major  = grid_ctx->major;
    const float as     = grid_ctx->alpha_scale;
    const float t      = grid_ctx->anim_time;
    const int   SEG    = 72;
    const float TAU    = 2.0f * (float)M_PI;
    const float GR = 0.20f, GG = 0.95f, GB = 0.45f;   /* radar green */

    for (float r = major; r <= extent + GRID_LOOP_EPSILON; r += major) {
        grid_color(grid_ctx, GR, GG, GB, fminf(0.06f * as, 1.0f));
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < SEG; i++) {
            float th = (float)i / (float)SEG * TAU;
            glVertex3f(r * cosf(th), 0.0f, r * sinf(th));
        }
        glEnd();
    }

    glLineWidth(1.5f);
    glBegin(GL_LINES);
    grid_color(grid_ctx, GR, GG, GB, fminf(0.07f * as, 1.0f));
    glVertex3f(-extent, 0, 0); glVertex3f(extent, 0, 0);
    glVertex3f(0, 0, -extent); glVertex3f(0, 0, extent);
    glEnd();

    /* Expanding ping, very faint, fades as it grows. */
    float pr = fmodf(t * 0.45f, 1.0f) * extent;
    grid_color(grid_ctx, GR, GG, GB, fminf((1.0f - pr / extent) * 0.10f * as, 1.0f));
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < SEG; i++) {
        float th = (float)i / (float)SEG * TAU;
        glVertex3f(pr * cosf(th), 0.0f, pr * sinf(th));
    }
    glEnd();

    /* Single faint sweep line. */
    float ang = fmodf(t * 0.8f, TAU);
    glBegin(GL_LINES);
    grid_color(grid_ctx, GR, GG, GB, fminf(0.12f * as, 1.0f));
    glVertex3f(0.0f, 0.0f, 0.0f);
    glVertex3f(extent * cosf(ang), 0.0f, extent * sinf(ang));
    glEnd();
}

int render3d_grid_theme_uses_fog(Render3dGridTheme grid_theme) {
    if (grid_theme < 0 || grid_theme >= GRID_THEME_COUNT) return 0;
    return g_grid_theme_traits[grid_theme].uses_fog;
}

/* ===========================================================================
 * 2D-oriented grid themes (Sketchbook + Neon Graph)
 *
 * These draw their graticule in the XY plane (z = GRID_2D_Z, just behind the
 * z=0 scene geometry) instead of the XZ ground plane, so they read as a flat,
 * front-facing grid in the 2D ortho view (camera looking down -Z). In 3D they
 * render as a vertical wall at z=0 - acceptable until grids are filtered by
 * view mode. Both use the shared scene accent palette and route line alpha through
 * grid_color() so the show/hide transition fade still applies. Custom themes:
 * no GridThemeSpec entry, so render3d_grid_theme_uses_edge_fade() is false and
 * the radial edge-fade machinery is skipped.
 * ========================================================================= */

#define GRID_2D_Z (-0.02f)   /* sit just behind z=0 user geometry */

/* Smooth, frame-stable wobble for the inked sketch strokes. `u` runs along the
 * stroke, `v` is its fixed coordinate, `seed` separates passes/lines. Returns a
 * small world-space offset (+/- 0.05) - large enough to read as hand-drawn at
 * unit cell size, small enough not to break the cell corners (callers taper it
 * to zero at the endpoints). Deterministic in space, so the ink doesn't crawl
 * between frames. */
static float grid_sketch_wobble(float u, float v, float seed) {
    return sinf(u * 1.7f + seed) * 0.022f
         + cosf(v * 2.3f + seed * 1.7f) * 0.018f
         + sinf(u * 3.9f + seed * 0.5f) * 0.011f;
}

/* One wobbly ink stroke between (ax,ay) and (bx,by) in the XY plane. The
 * perpendicular wobble tapers to zero at both ends (sin envelope) so adjacent
 * strokes still meet at the cell corners. */
static void grid_sketch_stroke(float ax, float ay, float bx, float by,
                               int segs, float seed, float wobble_scale,
                               float wobble_freq) {
    float dx = bx - ax, dy = by - ay;
    float len = sqrtf(dx * dx + dy * dy);
    float nx = (len > 1e-5f) ? -dy / len : 0.0f;
    float ny = (len > 1e-5f) ?  dx / len : 0.0f;
    glBegin(GL_LINE_STRIP);
    for (int k = 0; k <= segs; k++) {
        float u = (float)k / (float)segs;
        float px = ax + dx * u, py = ay + dy * u;
        float env = sinf(u * (float)M_PI);                 /* 0 at ends, 1 mid */
        float w = grid_sketch_wobble(u * len * wobble_freq, (ax + ay) * wobble_freq, seed)
                  * env * wobble_scale;
        glVertex3f(px + nx * w, py + ny * w, GRID_2D_Z);
    }
    glEnd();
}

/* GLUT stroke (line) glyphs anchored at (x,y) in the XY plane, `scale` world
 * units per font unit. Matches the inked aesthetic. Color + line width are the
 * caller's. */
static void grid_stroke_text(float x, float y, float scale, const char *str) {
    glPushMatrix();
    glTranslatef(x, y, GRID_2D_Z);
    glScalef(scale, scale, scale);
    for (const char *p = str; *p; p++)
        glutStrokeCharacter(GLUT_STROKE_ROMAN, (int)*p);
    glPopMatrix();
}

static float grid_stroke_text_width(float scale, const char *str) {
    float w = 0.0f;
    for (const char *p = str; *p; p++)
        w += (float)glutStrokeWidth(GLUT_STROKE_ROMAN, (int)*p);
    return w * scale;
}

/* Smallest "nice" multiplier (1, 2, 5, 10, 20, 50, ...) that is >= ratio. Used
 * to coarsen or subdivide Sketchbook and Checkerboard grid steps. */
static float grid_nice_step_mul(float ratio) {
    if (!(ratio > 1.0f)) return 1.0f;
    float p = 1.0f;
    while (p < ratio) {
        if (p * 2.0f >= ratio) return p * 2.0f;
        if (p * 5.0f >= ratio) return p * 5.0f;
        p *= 10.0f;
    }
    return p;
}

/* Sketchbook: a hand-drawn coordinate graph that fills the 2D ortho view -
 * wobbly ink cell lines snapped to real world coordinates, each labelled with
 * its value (subdivides into sub-1 unit decimal ticks when zoomed in). In 3D
 * mode it renders as a vertical wall spanning [-extent, extent]. */
static void render3d_grid_render_sketch_theme(const Render3dRenderConfig *config,
                                              const GridDrawContext *grid_ctx) {
    /* Hand-drawn coordinate graph: wobbly ink cell lines snapped to real
     * world coordinates (multiples/subdivisions of the grid `major` step),
     * labelled with their actual value so the labels line up with the gridlines.
     *
     * Fit to the live ortho view: in the 2D ortho projection glOrtho's matrix
     * gives the visible world half-extents exactly (half = 1/|proj[diag]|) at
     * ANY zoom. In 3D perspective mode it renders as a vertical wall spanning
     * [-extent, extent] in XY, honouring the Grid extent setting. */
    GLfloat pm[16];
    glGetFloatv(GL_PROJECTION_MATRIX, pm);
    int is_ortho = fabsf(pm[15] - 1.0f) < 1e-3f && fabsf(pm[11]) < 1e-3f;
    float half_w, half_h;
    float vx0, vx1, vy0, vy1;
    float vh_px = (config->render3d_h > 0) ? (float)config->render3d_h : 600.0f;

    if (is_ortho && fabsf(pm[0]) > 1e-9f && fabsf(pm[5]) > 1e-9f) {
        half_w = 1.0f / fabsf(pm[0]);
        half_h = 1.0f / fabsf(pm[5]);
        if (!(half_h > 0.05f && half_h < 1.0e5f)) half_h = 4.0f;
        if (!(half_w > 0.05f && half_w < 1.0e5f)) half_w = half_h * 1.4f;
        float cx = config->cam_tx, cy = config->cam_ty;
        vx0 = cx - half_w; vx1 = cx + half_w;
        vy0 = cy - half_h; vy1 = cy + half_h;
    } else {
        /* 3D perspective wall: span [-extent, extent] centred at origin,
         * honouring the Grid extent setting. */
        float ext = grid_ctx->extent;
        if (ext < 2.0f) ext = 2.0f;
        if (ext > 80.0f) ext = 80.0f;
        vx0 = -ext; vx1 = ext;
        vy0 = -ext; vy1 = ext;
        float dist = config->cam_dist > 0.5f ? config->cam_dist : 6.0f;
        half_h = (fabsf(pm[5]) > 1e-4f) ? dist / fabsf(pm[5]) : 4.0f;
        half_w = (fabsf(pm[0]) > 1e-4f) ? dist / fabsf(pm[0]) : 5.6f;
        if (half_h < 0.5f || half_h > 400.0f) half_h = 4.0f;
        if (half_w < 0.5f || half_w > 600.0f) half_w = 5.6f;
    }

    float cell = grid_ctx->major;
    if (cell < 0.25f) cell = 0.25f;

    /* Cell spacing: coarsens (2x, 5x, 10x...) when zoomed out, and subdivides
     * into clean decimal sub-units (0.5, 0.2, 0.1, 0.05...) when zoomed in (2D mode),
     * maintaining a readable ~40-120px cell spacing. */
    float px_per_cell = cell * vh_px / (2.0f * half_h);
    if (px_per_cell < 34.0f && px_per_cell > 1e-3f) {
        cell *= grid_nice_step_mul(34.0f / px_per_cell);
    } else if (px_per_cell > 120.0f && is_ortho) {
        cell /= grid_nice_step_mul(px_per_cell / 120.0f);
    }

    /* Cap the drawn span so an extreme zoom-out can't emit an unbounded number of strokes. */
    const float MAX_HALF = 300.0f * cell;
    if (half_w > MAX_HALF && is_ortho) {
        float cx = config->cam_tx;
        vx0 = cx - MAX_HALF; vx1 = cx + MAX_HALF;
    }
    if (half_h > MAX_HALF && is_ortho) {
        float cy = config->cam_ty;
        vy0 = cy - MAX_HALF; vy1 = cy + MAX_HALF;
    }

    int i_left  = (int)ceilf(vx0 / cell);
    int i_right = (int)floorf(vx1 / cell);
    int i_bot   = (int)ceilf(vy0 / cell);
    int i_top   = (int)floorf(vy1 / cell);
    if (i_right < i_left || i_top < i_bot) return;
    if (i_right - i_left > 400) i_right = i_left + 400;
    if (i_top - i_bot > 400) i_top = i_bot + 400;

    /* Scale wobble amplitude and spatial frequency proportionally with sub-unit cell size
     * so strokes look consistently hand-inked on screen without distorting cell geometry. */
    float wobble_scale = fminf(1.0f, cell);
    float wobble_freq  = (cell < 1.0f && cell > 1e-5f) ? (1.0f / cell) : 1.0f;

    const float ink_r = 0.74f, ink_g = 0.80f, ink_g2 = 0.92f;
    int vsegs = (int)((vy1 - vy0) / cell * 3.0f) + 3;
    int hsegs = (int)((vx1 - vx0) / cell * 3.0f) + 3;
    if (vsegs > 240) vsegs = 240;
    if (hsegs > 240) hsegs = 240;

    /* Vertical cell lines (bold + faint pass); the x=0 axis line is heavier. */
    for (int i = i_left; i <= i_right; i++) {
        float x = (float)i * cell;
        int axis = (i == 0);
        for (int s = 0; s < 2; s++) {
            grid_color(grid_ctx, ink_r, ink_g, ink_g2,
                       s == 0 ? (axis ? 0.85f : 0.50f) : 0.16f);
            glLineWidth(s == 0 ? (axis ? 2.1f : 1.4f) : 0.9f);
            grid_sketch_stroke(x, vy0, x, vy1, vsegs,
                               (float)i * 4.3f + (float)s * 31.7f,
                               wobble_scale, wobble_freq);
        }
    }
    /* Horizontal cell lines; the y=0 axis line is heavier. */
    for (int j = i_bot; j <= i_top; j++) {
        float y = (float)j * cell;
        int axis = (j == 0);
        for (int s = 0; s < 2; s++) {
            grid_color(grid_ctx, ink_r, ink_g, ink_g2,
                       s == 0 ? (axis ? 0.85f : 0.50f) : 0.16f);
            glLineWidth(s == 0 ? (axis ? 2.1f : 1.4f) : 0.9f);
            grid_sketch_stroke(vx0, y, vx1, y, hsegs,
                               (float)j * 5.1f + (float)s * 23.3f + 100.0f,
                               wobble_scale, wobble_freq);
        }
    }

    /* Coordinate labels: x values along the bottom of the view, y values down
     * the left of the view - each centred on its gridline so it lines up with the value.
     * Sized for the hand-drawn sketch look, with font scale and insets capped on zoom-in
     * so numbers stay readable (~22px max screen height) and pinned to margins. */
    float px_world = (2.0f * half_h) / vh_px;
    if (!(px_world > 1e-7f)) px_world = 8.0f / 600.0f;
    float ta = fminf(grid_ctx->xn_alpha * grid_ctx->grid_brightness, 1.0f);
    float nominal_lbl = fminf(0.0036f, fmaxf(0.0020f, cell * 0.0034f));
    float max_lbl = (22.0f / 119.05f) * px_world;
    float lbl = (cell < 1.0f) ? max_lbl : fminf(nominal_lbl, max_lbl);
    float xlbl_y = vy0 + 12.0f * px_world;
    float ylbl_x = vx0 + 14.0f * px_world;
    float y_offset = 4.0f * px_world;
    float xlbl_start = vx0 + 65.0f * px_world;

    glLineWidth(1.3f);
    glColor4f(0.86f, 0.90f, 0.97f, ta);
    for (int i = i_left; i <= i_right; i++) {
        if (i == 0) continue;                       /* skip 0 (shared with y-axis) */
        float x = (float)i * cell;
        double val = (double)i * (double)cell;
        if (fabs(val) < 1e-9) val = 0.0;
        char b[32];
        snprintf(b, sizeof b, "%g", val);
        float tw = grid_stroke_text_width(lbl, b);
        if (x - tw * 0.5f < xlbl_start) continue;   /* clear the y-label column */
        grid_stroke_text(x - tw * 0.5f, xlbl_y, lbl, b);
    }
    for (int j = i_bot; j <= i_top; j++) {
        float y = (float)j * cell;
        double val = (double)j * (double)cell;
        if (fabs(val) < 1e-9) val = 0.0;
        char b[32];
        snprintf(b, sizeof b, "%g", val);
        grid_stroke_text(ylbl_x, y + y_offset, lbl, b);
    }
    glLineWidth(1.0f);
}

/* Neon Graph: a glowing graph-paper grid in the XY plane - faint azure minor
 * lines, brighter violet majors, an additive bloom pass, glowing nodes at the
 * major intersections, and a pulsing coral origin cross. Clean, no text. */
static void render3d_grid_render_neon_theme(const GridDrawContext *grid_ctx) {
    const float major     = grid_ctx->major;
    const float major_tol = grid_ctx->major_tol;
    const float step      = grid_ctx->step;
    const float t         = grid_ctx->anim_time;
    /* Bound the drawn box so FAR extent doesn't emit thousands of lines. */
    const float ext = fminf(grid_ctx->extent, 9.0f);

    /* Two passes: solid core, then an additive bloom on top. */
    for (int pass = 0; pass < 2; pass++) {
        int glow = (pass == 1);
        glBlendFunc(GL_SRC_ALPHA, glow ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
        glLineWidth(glow ? 3.4f : 1.3f);
        glBegin(GL_LINES);
        for (float v = -ext; v <= ext + GRID_LOOP_EPSILON; v += step) {
            if (fabsf(v) < GRID_ORIGIN_SKIP_EPSILON) continue;
            int is_major = grid_is_major_line(v, major, major_tol);
            float a = (is_major ? 0.50f : 0.13f) * (glow ? 0.30f : 1.0f);
            if (is_major) grid_color(grid_ctx, 0.50f, 0.55f, 0.96f, a);  /* violet */
            else          grid_color(grid_ctx, 0.30f, 0.62f, 0.92f, a);  /* azure  */
            glVertex3f(-ext, v, GRID_2D_Z); glVertex3f(ext, v, GRID_2D_Z);
            glVertex3f(v, -ext, GRID_2D_Z); glVertex3f(v, ext, GRID_2D_Z);
        }
        glEnd();
    }

    /* Glowing nodes at the major intersections (additive). */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glEnable(GL_POINT_SMOOTH);
    glPointSize(3.2f);
    glBegin(GL_POINTS);
    for (float vx = -ext; vx <= ext + GRID_LOOP_EPSILON; vx += major)
        for (float vy = -ext; vy <= ext + GRID_LOOP_EPSILON; vy += major) {
            grid_color(grid_ctx, 0.58f, 0.82f, 0.99f, 0.55f);
            glVertex3f(vx, vy, GRID_2D_Z);
        }
    glEnd();

    /* Pulsing origin cross: coral core + additive bloom. */
    float pulse = 0.62f + 0.18f * sinf(t);
    for (int pass = 0; pass < 2; pass++) {
        int glow = (pass == 1);
        glBlendFunc(GL_SRC_ALPHA, glow ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
        glLineWidth(glow ? 5.0f : 2.0f);
        glBegin(GL_LINES);
        grid_color(grid_ctx, 0.98f, 0.56f, 0.36f, (glow ? 0.16f : 0.72f) * pulse);
        glVertex3f(-ext, 0, GRID_2D_Z); glVertex3f(ext, 0, GRID_2D_Z);
        glVertex3f(0, -ext, GRID_2D_Z); glVertex3f(0, ext, GRID_2D_Z);
        glEnd();
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(1.0f);
}

/* ===========================================================================
 * Graph Planes: an adaptive-planes 3D grid with coordinate labels
 *
 * Like Adaptive Planes (three orthogonal grid planes that emphasise whichever
 * the camera faces), but a bounded labelled graph: each plane is a [-R,R] box
 * of `major`-spaced cells, and the plane the camera is *nearly orthogonal* to
 * gets real coordinate labels (GLUT stroke glyphs, billboarded to the camera).
 * Only one plane is labelled at a time - they're chosen by the face weights
 * xy_w / zy_w / xz_w (which sum to 1), and labels only appear above a head-on
 * threshold, fading in as the view squares up. Accent palette: XY azure, ZY
 * amber, XZ floor cool-grey. Custom dispatch arm (no edge-fade).
 * ========================================================================= */

static float gp_smoothstep(float a, float b, float x) {
    float t = (b - a != 0.0f) ? (x - a) / (b - a) : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* Stroke text billboarded by an explicit (right, up) world basis, anchored at
 * (px,py,pz). Lets the labels lie flat against whichever plane is being viewed
 * head-on while staying readable (advance = screen right, height = screen up). */
static void grid_stroke_text_billboard(float px, float py, float pz, float scale,
                                       const float right[3], const float up[3],
                                       const char *str) {
    float nx = right[1] * up[2] - right[2] * up[1];
    float ny = right[2] * up[0] - right[0] * up[2];
    float nz = right[0] * up[1] - right[1] * up[0];
    GLfloat m[16] = {
        right[0] * scale, right[1] * scale, right[2] * scale, 0.0f,
        up[0]    * scale, up[1]    * scale, up[2]    * scale, 0.0f,
        nx,               ny,               nz,               0.0f,
        px,               py,               pz,               1.0f,
    };
    glPushMatrix();
    glMultMatrixf(m);
    for (const char *p = str; *p; p++)
        glutStrokeCharacter(GLUT_STROKE_ROMAN, (int)*p);
    glPopMatrix();
}

/* Draw one bounded grid plane: the component `kaxis` is pinned to 0, the in-plane
 * axes `iaxis`/`jaxis` vary over [-R,R] at `major` spacing. Axis lines (through 0)
 * use `aa`, the rest `la`. `n` is capped so a huge extent can't run away. */
static void graphplane_lines(const GridDrawContext *ctx, int iaxis, int jaxis, int kaxis,
                             float R, float major, float r, float g, float b,
                             float la, float aa) {
    int n = (int)(R / major + 0.5f);
    if (n > 400) n = 400;
    float ext = n * major;
    (void)kaxis;
    glBegin(GL_LINES);
    for (int s = -n; s <= n; s++) {
        float c = s * major;
        float alpha = (s == 0) ? aa : la;
        float p0[3] = { 0, 0, 0 }, p1[3] = { 0, 0, 0 };
        /* line that runs along iaxis at jaxis = c */
        grid_color(ctx, r, g, b, alpha);
        p0[jaxis] = c; p0[iaxis] = -ext; p1[jaxis] = c; p1[iaxis] = ext;
        glVertex3fv(p0); glVertex3fv(p1);
        /* line that runs along jaxis at iaxis = c */
        p0[iaxis] = c; p0[jaxis] = -ext; p1[iaxis] = c; p1[jaxis] = ext;
        glVertex3fv(p0); glVertex3fv(p1);
    }
    glEnd();
}

/* Coordinate labels for the head-on plane (`haxis` = screen-horizontal in-plane
 * axis, `vaxis` = screen-vertical, `kaxis` pinned to 0). The view is centred on
 * the camera focus (`ch`,`cv` = the in-plane components of cam_tx/ty/tz), so
 * under pan the labelled span [ch +/- Lh] x [cv +/- Lv] tracks what's actually visible.
 * h-values run along the screen-bottom edge, v-values down the screen-left edge;
 * Lh/Lv are the visible half-extents, so both axes stay on screen. */
static void graphplane_labels(int haxis, int vaxis, int kaxis,
                              float ch, float cv, float Lh, float Lv, float major,
                              const float right[3], const float up[3],
                              float scale, float alpha, float px_world) {
    int h0 = (int)ceilf((ch - Lh) / major), h1 = (int)floorf((ch + Lh) / major);
    int v0 = (int)ceilf((cv - Lv) / major), v1 = (int)floorf((cv + Lv) / major);
    if (h1 - h0 > 120) h1 = h0 + 120;               /* runaway guard */
    if (v1 - v0 > 120) v1 = v0 + 120;
    /* Anchor both tracks just *inside* the visible edges (text grows toward the
     * centre) so neither is pushed off screen - edges measured from the focus. */
    float bottom_v = cv - copysignf(Lv, up[vaxis]);  /* screen-bottom edge */
    float left_h   = ch - copysignf(Lh, right[haxis]); /* screen-left edge */
    /* Per-label edge fade: a value ramps up over the outermost ~band as it
     * enters the visible span, so panning/zooming/rotating slides numbers in
     * and out smoothly instead of popping them. */
    float band = major * 1.6f;

    /* Insets off the screen edges: anchored in screen pixels so they don't migrate
     * across the view when zoomed in. */
    float h_nudge = fminf(major * 0.10f, 12.0f * px_world);
    float v_nudge_r = fminf(major * 0.14f, 14.0f * px_world);
    float v_nudge_u = fminf(major * 0.12f, 4.0f * px_world);
    float corner_clear = fmaxf(major * 1.8f, 65.0f * px_world);

    for (int s = h0; s <= h1; s++) {                /* h-values along the bottom */
        float c = (float)s * major;
        double val = (double)s * (double)major;
        if (fabs(val) < 1e-9) val = 0.0;
        float ef = gp_smoothstep(0.0f, band, Lh - fabsf(c - ch));
        /* Fade (rather than hard-skip) the h-labels approaching the left-edge
         * v-label column, so the two tracks don't collide in the corner AND a
         * value zooming past the column slides in smoothly instead of popping
         * the instant it clears a hard cutoff. */
        float cf = gp_smoothstep(0.0f, corner_clear, fabsf(c - left_h));
        float a = alpha * ef * cf;
        if (a <= 0.004f) continue;
        glColor4f(0.90f, 0.93f, 0.98f, a);
        char b[32];
        snprintf(b, sizeof b, "%g", val);
        float tw = grid_stroke_text_width(scale, b);
        float p[3] = { 0, 0, 0 };
        p[haxis] = c; p[vaxis] = bottom_v;
        /* centre on the gridline, lift a touch inward off the bottom edge */
        float q0 = p[0] - right[0] * tw * 0.5f + up[0] * h_nudge;
        float q1 = p[1] - right[1] * tw * 0.5f + up[1] * h_nudge;
        float q2 = p[2] - right[2] * tw * 0.5f + up[2] * h_nudge;
        grid_stroke_text_billboard(q0, q1, q2, scale, right, up, b);
    }
    for (int s = v0; s <= v1; s++) {                /* v-values down the left */
        float c = (float)s * major;
        double val = (double)s * (double)major;
        if (fabs(val) < 1e-9) val = 0.0;
        float ef = gp_smoothstep(0.0f, band, Lv - fabsf(c - cv));
        if (ef <= 0.0f) continue;
        glColor4f(0.90f, 0.93f, 0.98f, alpha * ef);
        char b[32];
        snprintf(b, sizeof b, "%g", val);
        float p[3] = { 0, 0, 0 };
        p[vaxis] = c; p[haxis] = left_h;
        /* nudge inward off the left edge, vertically centre on the gridline */
        float q0 = p[0] + right[0] * v_nudge_r - up[0] * v_nudge_u;
        float q1 = p[1] + right[1] * v_nudge_r - up[1] * v_nudge_u;
        float q2 = p[2] + right[2] * v_nudge_r - up[2] * v_nudge_u;
        grid_stroke_text_billboard(q0, q1, q2, scale, right, up, b);
    }
    (void)kaxis;
}

static void render3d_grid_render_graphplanes_theme(const Render3dRenderConfig *config,
                                                   const GridDrawContext *grid_ctx) {
    float ry = config->cam_ry * (float)M_PI / 180.0f;
    float rx = config->cam_rx * (float)M_PI / 180.0f;
    float cry = cosf(ry), sry = sinf(ry), crx = cosf(rx), srx = sinf(rx);
    /* Face weights (sum to 1): how head-on the camera is to each plane. */
    float xy_w = crx * crx * cry * cry;   /* facing XY (look along Z)   */
    float zy_w = crx * crx * sry * sry;   /* facing ZY (look along X)   */
    float xz_w = srx * srx;               /* facing XZ floor (top-down) */

    float major = grid_ctx->major;
    if (major < 0.25f) major = 0.25f;
    /* Planes span the grid extent (so the NEAR/MID/FAR setting drives them). */
    float R = grid_ctx->extent;
    if (R < 2.0f) R = 2.0f;
    if (R > 80.0f) R = 80.0f;

    /* Visible world half-extents at the origin's depth, from the perspective
     * projection (half = cam_dist / |proj[diag]|) or ortho projection. Labels
     * live at these edges - clamped to the grid extent - so both axes stay on
     * screen at any zoom. */
    GLfloat pm[16];
    glGetFloatv(GL_PROJECTION_MATRIX, pm);
    int is_ortho = fabsf(pm[15] - 1.0f) < 1e-3f && fabsf(pm[11]) < 1e-3f;
    float dist = config->cam_dist > 0.5f ? config->cam_dist : 6.0f;
    float vh, vw;
    if (is_ortho && fabsf(pm[0]) > 1e-9f && fabsf(pm[5]) > 1e-9f) {
        vh = 1.0f / fabsf(pm[5]);
        vw = 1.0f / fabsf(pm[0]);
    } else {
        vh = (fabsf(pm[5]) > 1e-4f) ? dist / fabsf(pm[5]) : 4.0f;
        vw = (fabsf(pm[0]) > 1e-4f) ? dist / fabsf(pm[0]) : 6.0f;
    }
    if (vh < 0.05f || vh > 400.0f) vh = 4.0f;
    if (vw < 0.05f || vw > 600.0f) vw = 6.0f;
    float Lh = fminf(R, vw * 0.86f);      /* horizontal label extent / edge */
    float Lv = fminf(R, vh * 0.80f);      /* vertical   label extent / edge */

    float vh_px = (config->render3d_h > 0) ? (float)config->render3d_h : 600.0f;
    float px_world = (2.0f * vh) / vh_px;
    if (!(px_world > 1e-7f)) px_world = 8.0f / 600.0f;

    /* Cell spacing: coarsens (2x, 5x, 10x...) when zoomed out, and subdivides
     * into clean decimal sub-units (0.5, 0.2, 0.1, 0.05...) when zoomed in,
     * maintaining a readable ~40-120px cell spacing. */
    float px_per_cell = major * vh_px / (2.0f * vh);
    if (px_per_cell < 34.0f && px_per_cell > 1e-3f) {
        major *= grid_nice_step_mul(34.0f / px_per_cell);
    } else if (px_per_cell > 120.0f) {
        major /= grid_nice_step_mul(px_per_cell / 120.0f);
    }

    /* Billboard basis: world-space screen-right and screen-up for this pose. */
    float right[3] = { cry, 0.0f, sry };
    float up[3]    = { srx * sry, crx, -srx * cry };

    /* Three planes spanning the extent. A plane edge-on to the camera fades to
     * near-nothing (low base) so it doesn't pile up receding lines; the head-on
     * plane is the bright, readable graph. */
    glLineWidth(1.3f);
    graphplane_lines(grid_ctx, 0, 2, 1, R, major, 0.55f, 0.60f, 0.72f,   /* XZ floor */
                     0.02f + 0.28f * xz_w, 0.06f + 0.55f * xz_w);
    graphplane_lines(grid_ctx, 0, 1, 2, R, major, 0.40f, 0.70f, 0.98f,   /* XY azure */
                     0.02f + 0.34f * xy_w, 0.05f + 0.60f * xy_w);
    graphplane_lines(grid_ctx, 2, 1, 0, R, major, 0.98f, 0.74f, 0.42f,   /* ZY amber */
                     0.02f + 0.34f * zy_w, 0.05f + 0.60f * zy_w);

    /* Labels: only the most head-on plane, fading the whole set in by the
     * orientation weight (THRESH..FULL) and each value in by its distance from
     * the visible edge (in graphplane_labels). Both in-plane axes are labelled
     * (h bottom, v left). */
    const float THRESH = 0.50f;
    const float FULL   = 0.985f;
    float ta_base = fminf(grid_ctx->xn_alpha * grid_ctx->grid_brightness, 1.0f);
    float nominal_lbl = fminf(0.0040f, fmaxf(0.0022f, major * 0.0036f));
    float max_lbl = (22.0f / 119.05f) * px_world;
    float lblscale = (major < 1.0f || is_ortho) ? max_lbl : fminf(nominal_lbl, max_lbl);

    /* Camera focus per world axis - labels centre on it so pan stays correct. */
    float foc[3] = { config->cam_tx, config->cam_ty, config->cam_tz };
    glLineWidth(1.4f);
    if (xy_w > zy_w && xy_w > xz_w && xy_w > THRESH) {           /* XY: x,y */
        graphplane_labels(0, 1, 2, foc[0], foc[1], Lh, Lv, major, right, up,
                          lblscale, ta_base * gp_smoothstep(THRESH, FULL, xy_w), px_world);
    } else if (zy_w > xz_w && zy_w > THRESH) {                   /* ZY: z,y */
        graphplane_labels(2, 1, 0, foc[2], foc[1], Lh, Lv, major, right, up,
                          lblscale, ta_base * gp_smoothstep(THRESH, FULL, zy_w), px_world);
    } else if (xz_w > THRESH) {                                  /* XZ: x,z */
        graphplane_labels(0, 2, 1, foc[0], foc[2], Lh, Lv, major, right, up,
                          lblscale, ta_base * gp_smoothstep(THRESH, FULL, xz_w), px_world);
    }
    glLineWidth(1.0f);
}

/* ===========================================================================
 * Checkerboard: a lit, solid-fill floor with a coordinate label per cell
 *
 * Unlike every other grid theme this one is a *surface*, not a line drawing:
 * major cells alternate between two flat tones and are filled as quads with an
 * upward normal, drawn under the program's own enabled light slots - so the
 * floor takes the scene's key/fill exactly like user geometry does instead of
 * carrying its own baked shading. It stays translucent so geometry behind it
 * still reads - and how translucent is the **Grid brightness** setting, which
 * drives the fill's alpha through grid_checker_fill_alpha (Dim is a ghost of a
 * floor, Bold is solid) as well as the label ink's, the usual way.
 *
 * Each cell carries its own "x,z" label in GLUT stroke glyphs, laid flat in the
 * cell and sized to fit inside it (GRID_CHECKER_LABEL_HEIGHT, shrunk further
 * when the digits would overrun GRID_CHECKER_LABEL_FIT of the cell width), so
 * the type scales with the grid rather than with the viewport. The cells draw
 * under GL_POLYGON_OFFSET_FILL, pushed back just enough that the coplanar
 * strokes sit cleanly on top of the fill instead of z-fighting it.
 *
 * Distance is handled at both ends, and the split is the point:
 *   - near/mid: each label's *alpha* ramps down with its distance from the
 *     camera, so it dissolves into whatever square is under it;
 *   - far: a hard cull. Past GRID_CHECKER_REACH labels are simply not emitted,
 *     because at that range a cell is a few pixels wide and its label is
 *     illegible scribble that reads as noise over the checker - and skipping
 *     them is also what keeps the per-frame glyph count bounded.
 * The alpha ramp reaches zero exactly at the cull radius, so nothing visible is
 * ever dropped and the boundary can't pop. Alpha rather than the LINEAR fog
 * this theme first used: fog fades toward the presentation *background*, which
 * over a checkered floor means far strokes drift toward the sky color and read
 * differently on pale squares than on dark ones. Alpha is uniform over both.
 * The reach is keyed to the *camera distance*, so zooming out keeps a labelled
 * patch of roughly constant screen size around the view centre instead of
 * either vanishing or carpeting the floor.
 * ========================================================================= */

#define GRID_CHECKER_MAX_CELLS    160    /* per axis; runaway guard at FAR     */
#define GRID_CHECKER_LABEL_MAX    700    /* per frame; runaway guard           */
#define GRID_CHECKER_LABEL_HEIGHT 0.115f /* cap height as a fraction of a cell */
#define GRID_CHECKER_LABEL_FIT    0.72f  /* max label width as a cell fraction */
#define GRID_CHECKER_LABEL_DENSITY 0.09f /* label stride ~= cam_dist * this    */
#define GRID_CHECKER_REACH        1.2f   /* cull radius as a multiple of dist  */
#define GRID_CHECKER_FADE_START   0.35f  /* alpha ramp start, fraction of reach*/
#define GRID_CHECKER_INK_WIDTH    1.4f   /* stroke width (px)                  */
#define GRID_CHECKER_POLY_OFFSET  1.0f   /* factor == units, pushes cells back */
#define GRID_STROKE_CAP_HEIGHT    100.0f /* GLUT_STROKE_ROMAN em, font units   */

/* The two square tones. Fed to the lights as GL_AMBIENT_AND_DIFFUSE
 * reflectances via GL_COLOR_MATERIAL, so these are the *unlit* albedo - keep
 * them well below 1.0 or a bright key blows the pale squares out. Per-theme
 * table data, so palette.h bucket 3 (see its header): deliberately not a
 * RENDER3D_CLR_* token. */
static const float k_grid_checker_pale[3] = { 0.72f, 0.71f, 0.67f };
static const float k_grid_checker_dark[3] = { 0.20f, 0.22f, 0.26f };

/* Enable the light slots the *program* has on, so the floor is lit by the
 * same lamps as the user's geometry. render3d_lights_setup already wrote every
 * slot's position/colors under this frame's camera modelview, so re-enabling a
 * slot here needs no repositioning. A program with no lights on would leave the
 * floor flat ambient-black, which reads as a bug rather than as a scene with no
 * lamps - so that case gets a neutral overhead key on slot 0 instead. Both the
 * enables and the fallback's light parameters are restored by the theme's
 * enclosing glPushAttrib(GL_ALL_ATTRIB_BITS) (GL_LIGHTING_BIT). */
static void grid_checker_bind_lights(const Render3dRenderConfig *config) {
    int any = 0;
    for (int i = 0; i < MAX_LIGHTS; i++) {
        if (!config->lights[i].enabled) continue;
        glEnable(config->lights[i].id);
        any = 1;
    }
    if (!any) {
        /* Bucket-2 carve-out (palette.h): lighting coefficients, not draw
         * colors. Directional (w = 0), so the current modelview turns it into
         * a world-fixed key from above/front. */
        static const GLfloat fb_pos[4] = { 0.35f, 0.90f, 0.45f, 0.0f };
        static const GLfloat fb_dif[4] = { 0.85f, 0.85f, 0.82f, 1.0f };
        static const GLfloat fb_amb[4] = { 0.18f, 0.18f, 0.20f, 1.0f };
        glLightfv(GL_LIGHT0, GL_POSITION, fb_pos);
        glLightfv(GL_LIGHT0, GL_DIFFUSE,  fb_dif);
        glLightfv(GL_LIGHT0, GL_AMBIENT,  fb_amb);
        glEnable(GL_LIGHT0);
    }
    /* Two-sided so the floor is still lit when the camera drops below it -
     * the single +Y normal would otherwise face away and leave it black. */
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    glEnable(GL_LIGHTING);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
}

/* Opacity per Grid brightness step (Dim / Normal / Bright / Bold), read from
 * the *setting* (grid_brightness_idx) rather than from the derived
 * grid_brightness multiplier the line themes scale by: those factors run to
 * 4.5, shaped for lifting deliberately-faint lines, and applying them to a fill
 * that starts near 0.5 would clamp the top three steps to flat opaque - three
 * settings, one look. Two columns because the strokes are thin and need more
 * opacity than the fill to stay readable over it at the same step. */
typedef struct GridCheckerOpacity {
    float fill;   /* checker square alpha */
    float ink;    /* label stroke alpha, before the distance ramp */
} GridCheckerOpacity;

static const GridCheckerOpacity k_grid_checker_opacity[GRID_BRIGHTNESS_COUNT] = {
    [GRID_BRIGHTNESS_DIM]    = { .fill = 0.08f, .ink = 0.10f },
    [GRID_BRIGHTNESS_NORMAL] = { .fill = 0.20f, .ink = 0.20f },
    [GRID_BRIGHTNESS_BRIGHT] = { .fill = 0.55f, .ink = 0.60f },
    [GRID_BRIGHTNESS_BOLD]   = { .fill = 0.85f, .ink = 1.00f },
};

static GridCheckerOpacity grid_checker_opacity(const Render3dRenderConfig *config) {
    int i = config->grid_brightness_idx;
    if (i < 0 || i >= GRID_BRIGHTNESS_COUNT) i = GRID_BRIGHTNESS_NORMAL;
    return k_grid_checker_opacity[i];
}

/* The checkerboard itself: one quad per major cell, alternating tone by cell
 * parity. Cell coordinates are integers so the parity is exact (no fmod
 * tolerance) and the pattern stays pinned to the world origin. */
static void grid_checker_cells(const GridDrawContext *ctx, float alpha) {
    float cell = ctx->major;
    int n = (int)(ctx->extent / cell + 0.5f);
    if (n > GRID_CHECKER_MAX_CELLS / 2) n = GRID_CHECKER_MAX_CELLS / 2;
    if (n < 1) return;

    glNormal3f(0.0f, 1.0f, 0.0f);
    glBegin(GL_QUADS);
    for (int i = -n; i < n; i++) {
        float x0 = (float)i * cell, x1 = x0 + cell;
        for (int k = -n; k < n; k++) {
            float z0 = (float)k * cell, z1 = z0 + cell;
            const float *c = ((i + k) & 1) ? k_grid_checker_dark
                                           : k_grid_checker_pale;
            /* grid_color_surface takes the transition fade only - the Grid
             * brightness contribution is already folded into `alpha` by
             * grid_checker_fill_alpha, on its own curve. */
            grid_color_surface(ctx, c[0], c[1], c[2], alpha);
            glVertex3f(x0, 0.0f, z0);   /* CCW seen from +Y, matching the normal */
            glVertex3f(x0, 0.0f, z1);
            glVertex3f(x1, 0.0f, z1);
            glVertex3f(x1, 0.0f, z0);
        }
    }
    glEnd();
}

/* Far cull radius: past this a cell is a handful of pixels wide, so its label
 * is unreadable scribble and is not emitted at all. Also where the fog below
 * has finished fading, so the cull never cuts a still-visible stroke. Keyed to
 * the camera distance so the labelled patch stays a roughly constant share of
 * the screen at any zoom. */
static float grid_checker_label_reach(const GridDrawContext *ctx, float cam_dist) {
    float reach = cam_dist * GRID_CHECKER_REACH + ctx->major * 2.0f;
    return fminf(reach, ctx->extent);
}

/* Which cells carry a label: one *every* cell at normal zoom, thinning to a
 * nice multiple (1/2/5/10...) of the cell only once a cell is too few pixels
 * across for its number to be readable at all. The type is always sized to a
 * single cell (see below), so this decides density, never size. */
static float grid_checker_label_stride(const GridDrawContext *ctx, float cam_dist) {
    float ratio = cam_dist * GRID_CHECKER_LABEL_DENSITY / ctx->major;
    return ctx->major * grid_nice_step_mul(ratio);
}

/* One "x,z" label per cell, laid flat and centred *in* the cell - the value is
 * the cell's own corner, so the number still names its gridlines. The glyph
 * basis puts the text in the XZ plane reading from +Z: advance along +X, cap
 * height along -Z. Type is sized off one cell, never off the label stride: at a
 * thinned stride the labels get sparser, not bigger, so a number always reads
 * as belonging to the square it sits in. A long value ("-15,-10") is scaled
 * down to GRID_CHECKER_LABEL_FIT of the cell rather than bleeding across it. */
static void grid_checker_labels(const GridDrawContext *ctx, const float cam[3],
                                float reach, float stride, float ink) {
    static const float right[3] = { 1.0f, 0.0f,  0.0f };
    static const float up[3]    = { 0.0f, 0.0f, -1.0f };
    float ext  = ctx->extent;
    float cell = ctx->major;
    float base = cell * GRID_CHECKER_LABEL_HEIGHT / GRID_STROKE_CAP_HEIGHT;
    float fit  = cell * GRID_CHECKER_LABEL_FIT;
    /* Distance ramp: full ink out to GRID_CHECKER_FADE_START of the reach, then
     * down to zero exactly at it, so a label has already dissolved by the
     * radius where the cull stops emitting it and the boundary never pops. */
    float fade_from = reach * GRID_CHECKER_FADE_START;
    float fade_span = reach - fade_from;
    if (fade_span < 1e-4f) fade_span = 1e-4f;

    float lo_x = ceilf (fmaxf(cam[0] - reach, -ext) / stride) * stride;
    float hi_x = floorf(fminf(cam[0] + reach,  ext - cell) / stride) * stride;
    float lo_z = ceilf (fmaxf(cam[2] - reach, -ext) / stride) * stride;
    float hi_z = floorf(fminf(cam[2] + reach,  ext - cell) / stride) * stride;

    int drawn = 0;
    for (float gz = lo_z; gz <= hi_z + GRID_LOOP_EPSILON; gz += stride) {
        for (float gx = lo_x; gx <= hi_x + GRID_LOOP_EPSILON; gx += stride) {
            /* Cull on the cell's centre, and as a sphere rather than a box:
             * that matches how the fog (an eye-distance ramp) actually fades
             * them, so nothing is dropped while still visible. */
            float cx = gx + cell * 0.5f, cz = gz + cell * 0.5f;
            float dx = cx - cam[0], dy = cam[1], dz = cz - cam[2];
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > reach * reach) continue;
            if (++drawn > GRID_CHECKER_LABEL_MAX) return;

            float f = (sqrtf(d2) - fade_from) / fade_span;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            f = 1.0f - f * f * (3.0f - 2.0f * f);   /* smoothstep out */
            if (f <= 0.004f) continue;
            /* Not grid_color: that folds in the grid-line brightness multiplier,
             * and `ink` already carries this step's opacity from the table. */
            grid_color_surface(ctx, 0.94f, 0.95f, 0.98f, ink * f);

            char b[32];
            snprintf(b, sizeof b, "%g,%g", (double)gx, (double)gz);
            float scale = base;
            float tw = grid_stroke_text_width(scale, b);
            if (tw > fit) {                    /* shrink to stay inside the cell */
                scale *= fit / tw;
                tw = fit;
            }
            /* Centre in the cell: half the text width back along +X, half the
             * cap height back along -up (= +Z), so the glyph box straddles the
             * cell centre in both axes. */
            grid_stroke_text_billboard(cx - tw * 0.5f, 0.0f,
                                       cz + GRID_STROKE_CAP_HEIGHT * 0.5f * scale,
                                       scale, right, up, b);
        }
    }
}

static void render3d_grid_render_checker_theme(const GridDrawContext *grid_ctx,
                                               const Render3dFrameRenderContext *frame_ctx) {
    const Render3dRenderConfig *config = &frame_ctx->config;
    const float *cam = frame_ctx->camera_world_pos;
    float cam_dist = config->cam_dist > 0.5f ? config->cam_dist : 6.0f;
    float reach    = grid_checker_label_reach(grid_ctx, cam_dist);
    float stride   = grid_checker_label_stride(grid_ctx, cam_dist);
    GridCheckerOpacity op = grid_checker_opacity(config);

    /* ---- 1. The lit checkerboard ----
     * Polygon offset pushes the fill back a hair so the label strokes - exactly
     * coplanar with it - land on top instead of z-fighting. Culling is
     * explicitly off: the floor is a single-normal sheet meant to be visible
     * from below too, and the program's own glCullFace state is still live
     * here. Fog: at FAR the caller's clear-color distance fog is left in place
     * so the rim recedes with it; at any other extent fog would be whatever the
     * program left enabled, so it is cleared first. */
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(GRID_CHECKER_POLY_OFFSET, GRID_CHECKER_POLY_OFFSET);
    if (config->grid_extent_idx == GRID_EXTENT_FAR)
        glEnable(GL_FOG);
    grid_checker_bind_lights(config);
    grid_checker_cells(grid_ctx, op.fill);
    glDisable(GL_LIGHTING);
    glDisable(GL_COLOR_MATERIAL);
    glDisable(GL_POLYGON_OFFSET_FILL);

    /* ---- 2. Cell labels ----
     * The distance falloff is per-label *alpha*, not fog (see the header note),
     * applied inside grid_checker_labels: full ink out to
     * GRID_CHECKER_FADE_START of the reach, smoothstepping to nothing exactly
     * at the radius where the cull takes over. */
    glLineWidth(GRID_CHECKER_INK_WIDTH);
    grid_checker_labels(grid_ctx, cam, reach, stride, op.ink);
    glLineWidth(1.0f);
}

/* ---- Grid transition curve plugin (Render3dXnReveal, see render3d_transition.h) --
 * The grid owns its fade durations + per-theme speed + opacity shape; the
 * machine just feeds elapsed time and reads opacity back. A linear opacity
 * ramp is intentional - the reveal's spatial shaping (smoothstep wipe, bright
 * head) is applied on top of opacity in render3d_grid_render, so opacity itself
 * stays a plain 0..1 progress that inverts cleanly for reversal. */

/* Per-theme fade-speed multiplier on GRID_FADE_*_SECS: 1.0 = base, >1 slower,
 * <1 snappier. A 0 entry (the unset default) means base speed. */
static float grid_reveal_time_scale(int grid_theme) {
    if (grid_theme <= GRID_THEME_OFF || grid_theme >= GRID_THEME_COUNT)
        return 1.0f;
    float t = g_grid_theme_traits[grid_theme].reveal.time;
    return (t > 0.0f) ? t : 1.0f;
}

static float grid_reveal_duration(int grid_theme, Render3dXnPhase phase) {
    float base = (phase == RENDER3D_XN_FADE_IN) ? GRID_FADE_IN_SECS
                                             : GRID_FADE_OUT_SECS;
    return base * grid_reveal_time_scale(grid_theme);
}

static float grid_reveal_opacity(int grid_theme, Render3dXnPhase phase,
                                 float elapsed) {
    if (phase == RENDER3D_XN_STEADY) return 1.0f;
    float dur = grid_reveal_duration(grid_theme, phase);
    float p = (dur > 0.0f) ? elapsed / dur : 1.0f;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return (phase == RENDER3D_XN_FADE_IN) ? p : 1.0f - p;
}

static float grid_reveal_elapsed_at(int grid_theme, Render3dXnPhase phase,
                                    float opacity) {
    float dur = grid_reveal_duration(grid_theme, phase);
    float p = (phase == RENDER3D_XN_FADE_IN) ? opacity : 1.0f - opacity;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return p * dur;
}

const Render3dXnReveal render3d_grid_reveal = {
    grid_reveal_opacity, grid_reveal_elapsed_at
};

int render3d_grid_theme_uses_edge_fade(Render3dGridTheme grid_theme) {
    if (grid_theme < 0 || grid_theme >= GRID_THEME_COUNT) return 0;
    return g_grid_theme_traits[grid_theme].uses_edge_fade;
}

/* --- render3d_grid_render phases ---
 *
 * Splits the 150-line orchestrator into:
 *   - grid_xn_resolve(): per-frame transition fade via the shared
 *     render3d_overlay_xn_resolve. Returns a Render3dOverlayXn the caller
 *     stamps onto the GridDrawContext.
 *   - grid_setup_blend_depth(): the GL state baseline for grid draws.
 *   - grid_build_draw_context(): clamps extent/major indices and
 *     fills the GridDrawContext.
 *   - grid_apply_far_fog(): the FAR-extent distance fog + the
 *     FOG-transition recede injection for non-far themes.
 *   - grid_dispatch_theme(): the switch over Render3dGridTheme.
 *
 * render3d_grid_render sequences these stages, and the xn fields live on
 * GridDrawContext. */

/* Resolve grid transition fade via the shared overlay-xn helper. The
 * grid's "this theme owns fog" carve-out (currently OCEAN only) falls
 * back to plain alpha FADE so the theme's own fog is not competing
 * with the synthetic LINEAR recede. The carve-out is keyed on
 * render3d_grid_theme_uses_fog(). FROZEN is deliberately not in the set:
 * its mist only exists under the ice, so above ground it transitions
 * like any fog-less theme. */
static Render3dOverlayXn grid_xn_resolve(const Render3dRenderConfig *config,
                                      Render3dGridTheme grid_theme) {
#if GRID_XN_STYLE == GRID_AXES_XN_FOG
    int uses_fog = render3d_grid_theme_uses_fog(grid_theme);
    float knee   = GRID_XN_FOG_ALPHA_KNEE;
#else
    (void)grid_theme;
    int uses_fog = 0;
    float knee   = 1.0f; /* not used in FADE style; keeps the helper pure */
#endif
    return render3d_overlay_xn_resolve(config->grid_opacity,
                                    GRID_XN_STYLE,
                                    uses_fog, knee);
}

static void grid_setup_blend_depth(const Render3dRenderConfig *config) {
    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    render3d_apply_quality_config(config);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static GridDrawContext grid_build_draw_context(const Render3dFrameRenderContext *frame_ctx) {
    const Render3dRenderConfig *config = &frame_ctx->config;
    float breath = sinf(config->anim_time * RENDER3D_BREATH_FREQ) * 0.5f + 0.5f;
    /* Configurable extent / major-tick spacing. Minor step is the
     * major cell divided into 5 subdivisions, which keeps the look
     * consistent across the {1, 2, 5, 10} major options. */
    int ex_i = config->grid_extent_idx;
    if (ex_i < 0 || ex_i >= GRID_EXTENT_COUNT) ex_i = GRID_EXTENT_MID;
    int mj_i = config->grid_major_idx;
    if (mj_i < 0 || mj_i >= GRID_MAJOR_COUNT) mj_i = GRID_MAJOR_1;
    float extent = config->grid_extents[ex_i];
    float major  = config->grid_major_steps[mj_i];
    float step   = major / GRID_MINOR_SUBDIVISIONS;
    return (GridDrawContext){
        .extent      = extent,
        .major       = major,
        .step        = step,
        .major_tol   = step * GRID_MAJOR_TOL_FRACTION,
        .anim_time   = config->anim_time,
        .breath      = breath,
        .alpha_scale = config->alpha_scale,
        /* A zero-initialized config (render3d_demo, headless tests that don't
         * set this) must not zero out grid-line alpha, so treat <= 0 as the
         * neutral 1.0 multiplier. */
        .grid_brightness = config->grid_brightness > 0.0f
                                ? config->grid_brightness : 1.0f,
    };
}


/* FAR-extent clear-color distance fog. Under FOG transition style,
 * fog-less themes animate the same fog inward as they hide. The
 * xn parameter carries the resolved fog_tf (= 1 - opacity) so this
 * helper doesn't reach back to the renderer's draw context for the
 * value. */
static void grid_apply_far_fog(const Render3dRenderConfig *config,
                               Render3dGridTheme grid_theme, float extent,
                               const Render3dOverlayXn *xn) {
    /* Edge-fade themes dissolve to the backdrop via per-vertex alpha
     * (steady rim + animated recede), so they never want clear-color
     * fog - at any extent or transition phase. Make sure none is left
     * enabled from a prior pass and bail. */
    if (render3d_grid_theme_uses_edge_fade(grid_theme)) {
        glDisable(GL_FOG);
        return;
    }
    int is_far = (config->grid_extent_idx == GRID_EXTENT_FAR);
#if GRID_XN_STYLE == GRID_AXES_XN_FOG
    /* Fog-less, non-FAR themes: recede into a synthesized background-color
     * fog as the overlay hides. At FAR the recede is driven by the
     * FAR block's own fog (below) instead, so it isn't double-set /
     * overwritten. Fog-owning configs took the FADE fallback above. */
    int uses_fog = render3d_grid_theme_uses_fog(grid_theme);
    if (!uses_fog && !is_far)
        grid_xn_apply_transition_fog(xn->fog_tf, extent,
                                     config->presentation_rgba);
#else
    (void)grid_theme;
#endif

    if (!is_far) return;

    /* Steady: (0.7e .. e). tf=0 is the steady look (no pop); tf=1 is
     * a tight recede wall near the camera. */
    float fog_start = extent * 0.85f;
    float fog_end   = extent;
#if GRID_XN_STYLE == GRID_AXES_XN_FOG
    if (!uses_fog) {
        float tf = xn->fog_tf;
        fog_end   = extent + (extent * 0.05f - extent) * tf;
        fog_start = fog_end * 0.7f;
    }
#else
    (void)xn;
#endif
    set_fog_to_presentation_color(config->presentation_rgba);
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, fog_start);
    glFogf(GL_FOG_END, fog_end);
}

/* Custom themes handle their own draw path; the default arm covers
 * every standard theme by spec-table lookup, so adding/removing a
 * GridThemeSpec entry is a one-edit change instead of two parallel
 * lists. set_nv_fog is true iff the runtime supports NV fog distance
 * AND the active theme wants radial eye-distance fog (RADAR). */
static void grid_dispatch_theme(const Render3dFrameRenderContext *frame_ctx,
                                const GridDrawContext *grid_ctx,
                                Render3dGridTheme grid_theme,
                                int set_nv_fog) {
    const Render3dRenderConfig *config = &frame_ctx->config;
    switch (grid_theme) {

    case GRID_THEME_OCEAN:
        render3d_grid_render_ocean_theme(grid_ctx, frame_ctx, grid_ctx->breath);
        break;

    case GRID_THEME_FROZEN:
        render3d_grid_render_frozen_theme(grid_ctx, frame_ctx);
        break;

    case GRID_THEME_SOIL:
        render3d_grid_render_soil_theme(grid_ctx, frame_ctx);
        break;

    case GRID_THEME_STARCHART:
        render3d_grid_render_starchart_theme(grid_ctx, frame_ctx);
        break;

    case GRID_THEME_SKETCH:
        render3d_grid_render_sketch_theme(config, grid_ctx);
        break;

    case GRID_THEME_NEON:
        render3d_grid_render_neon_theme(grid_ctx);
        break;

    case GRID_THEME_GRAPHPLANES:
        render3d_grid_render_graphplanes_theme(config, grid_ctx);
        break;

    case GRID_THEME_PLANES:
        render3d_grid_render_planes_theme(config, grid_ctx);
        break;

    case GRID_THEME_CHECKER:
        render3d_grid_render_checker_theme(grid_ctx, frame_ctx);
        break;

    case GRID_THEME_RADAR:
        /* Radial-fog opt-in: the radar rings read the shared FAR-extent
         * distance fog, which swims at the fringes under the eye-plane default. */
        if (set_nv_fog)
            glFogi(GL_FOG_DISTANCE_MODE_NV, GL_EYE_RADIAL_NV);
        render3d_grid_render_radar_theme(grid_ctx);
        break;

    default: {
        /* GRID_THEME_CLASSIC, _TRON, _EMBER, _AURORA, _SYNTHWAVE and
         * standard line themes use the GridThemeSpec table-driven path. */
        const GridThemeSpec *spec = grid_theme_spec(grid_theme);
        if (spec)
            draw_grid_standard_theme(grid_ctx, spec);
        break;
    }
    }
}

void render3d_grid_render(const Render3dFrameRenderContext *frame_ctx) {
    const Render3dRenderConfig *config = &frame_ctx->config;
    Render3dGridTheme grid_theme = (Render3dGridTheme)config->grid_theme;
    if (grid_theme == GRID_THEME_OFF) return;

    Render3dOverlayXn xn = grid_xn_resolve(config, grid_theme);
    if (!xn.draw) return;

    render3d_grid_push_state();
    grid_setup_blend_depth(config);

    /* Nudge grid slightly below Y=0 to avoid z-fighting with axes */
    glPushMatrix();
    glTranslatef(0, -0.002f, 0);

    GridDrawContext grid_ctx = grid_build_draw_context(frame_ctx);
    grid_ctx.xn_opacity   = xn.opacity;
    grid_ctx.xn_alpha     = xn.alpha;
    grid_ctx.reveal       = g_grid_theme_traits[grid_theme].reveal;

    /* Edge-fade dissolve: always on for the line themes. The grid
     * dissolves to transparency by world radial distance, reaching 0 at
     * the extent (fade_end = extent steady, swept inward to 0 during a
     * hide transition). The per-vertex alpha owns the whole fade, so the
     * global xn_alpha is pinned to 1. */
    if (render3d_grid_theme_uses_edge_fade(grid_theme)) {
        float extent = grid_ctx.extent;
        grid_ctx.edge_fade = 1;
        grid_ctx.xn_alpha  = 1.0f;
        grid_edge_fade_build(&grid_ctx,
                             extent * xn.opacity,           /* fade_end */
                             GRID_EDGE_FADE_BAND * extent); /* band */
    }

    grid_apply_far_fog(config, grid_theme, grid_ctx.extent, &xn);

    /* GL_FOG_DISTANCE_MODE_NV isn't in the Khronos GL_FOG_BIT spec, so
     * a strict glPushAttrib(GL_ALL_ATTRIB_BITS) may not save/restore it.
     * Snapshot before the RADAR theme mutates it, restore at the
     * tail before pop. */
    GLint saved_nv_fog_mode = 0;
    int set_nv_fog = (config->nv_fog_distance_supported &&
                      g_grid_theme_traits[grid_theme].uses_nv_fog);
    if (set_nv_fog)
        glGetIntegerv(GL_FOG_DISTANCE_MODE_NV, &saved_nv_fog_mode);

    grid_dispatch_theme(frame_ctx, &grid_ctx, grid_theme, set_nv_fog);

    if (set_nv_fog)
        glFogi(GL_FOG_DISTANCE_MODE_NV, saved_nv_fog_mode);

    glPopMatrix();
    /* render3d_grid_pop_state restores GL_ALL_ATTRIB_BITS, covering
     * GL_DEPTH_BUFFER_BIT (depth mask), GL_COLOR_BUFFER_BIT (blend),
     * GL_FOG_BIT, and GL_LIGHTING_BIT - no manual teardown needed. */
    render3d_grid_pop_state();
}
