/*
 * grid.c - grid theme rendering
 */
#include "grid.h"
#include "overlay_xn.h"  /* Render3dOverlayXn + shared resolve helper */
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
 * { axis, head } pair in g_grid_reveal[]; all of it is meant to be edited and
 * recompiled. A reveal is { axis, head=0 } for a plain dissolve with no head
 * wave, or { axis, head=1 } for the bright leading-edge draw-head. */
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

/* Per-theme reveal motion. Only the edge-fade line themes animate a reveal;
 * the environment themes (Ocean / Frozen / Soil / Radar) keep their own
 * atmosphere fade, so their entries are unused. Unlisted entries default to
 * { GRID_REVEAL_RADIAL, head=0 } (designated-init zero) — a plain radial
 * dissolve with no head wave; that includes XZ Ruler and Star Chart, whose
 * decorations fade radially, so a quiet radial graticule reveal stays cohesive
 * with them. Edit freely. */
static const GridReveal g_grid_reveal[GRID_THEME_COUNT] = {
    /*                       axis                  head  time                  */
    [GRID_THEME_EMBER]     = { GRID_REVEAL_RADIAL,   1,  7.0f },
    [GRID_THEME_TRON]      = { GRID_REVEAL_SWEEP_X,  1,  7.0f },
    [GRID_THEME_SYNTHWAVE] = { GRID_REVEAL_DIAGONAL, 1,  6.6f },
    [GRID_THEME_STARCHART] = { GRID_REVEAL_RADIAL,   0,  4.0f },
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
    /* In-out transition (audit #3): formerly file-static g_xn_opacity /
     * g_xn_alpha. Resolved once at render3d_grid_render entry via
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
     * g_grid_reveal[theme]; only consulted while a line theme is
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
typedef Render3dRgba (*GridOriginColorFn)(const GridDrawContext *ctx);
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

static void set_fog_to_clear_color() {
    float clear_col[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_col);
    glFogfv(GL_FOG_COLOR, clear_col);
}

/* Grid in-out transition (docs/plans/.../grid-axes-transitions.md rule 4).
 * Resolved once at render3d_grid_render entry from config.grid_opacity
 * and stored on the GridDrawContext. Every color path routes through
 * grid_color so it applies uniformly, AFTER each call site's own
 * alpha_scale clamp so the controller-owned OUT is the hard ceiling
 * (rule 3). 1.0 = shown.
 *
 * The shared render3d_overlay_xn_resolve helper in overlay_xn.h owns the
 * knee math; grid passes its style + GRID_XN_FOG_ALPHA_KNEE and
 * receives back the effective {alpha, opacity, fog_tf, draw}. */

#if GRID_XN_STYLE == GRID_AXES_XN_FOG
#define GRID_XN_FOG_ALPHA_KNEE 0.30f

/* Synthetic recede fog for fog-less themes: a clear-color linear wall
 * pulled in from beyond the grid as the overlay hides (tf = 1 -
 * opacity). tf<=0 -> no fog, continuous with the fogless steady look. */
static void grid_xn_apply_transition_fog(float tf, float extent) {
    if (tf <= 0.0f) return;
    set_fog_to_clear_color();
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

/* Build the per-frame dissolve front and cache fade_end/band on the
 * context. The breakpoint table (ef_bp/ef_mul) is the offset-0 ramp —
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
 * `p` — for short marks / inline axes that don't subdivide. Mirrors the
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
     * where r hits fade_start / fade_end — same for both axes at this
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

static void draw_grid_origin_axes(const GridDrawContext *ctx, Render3dRgba color,
                                  float line_width) {
    glDepthMask(GL_TRUE);
    if (line_width != 1.0f)
        glLineWidth(line_width);
    glBegin(GL_LINES);
    if (grid_reveal_active(ctx)) {
        grid_reveal_line(ctx, color, 1, 0.0f);  /* X axis, runs along X */
        grid_reveal_line(ctx, color, 0, 0.0f);  /* Z axis, runs along Z */
    } else if (!ctx->edge_fade) {
        grid_color_rgba(ctx, color);
        glVertex3f(-ctx->extent, 0, 0);
        glVertex3f( ctx->extent, 0, 0);
        glVertex3f(0, 0, -ctx->extent);
        glVertex3f(0, 0,  ctx->extent);
    } else {
        /* X axis runs along x (z=0); Z axis runs along z (x=0). */
        for (int i = 0; i + 1 < ctx->ef_n; i++) {
            float m0 = ctx->ef_mul[i], m1 = ctx->ef_mul[i + 1];
            float p0 = ctx->ef_bp[i],  p1 = ctx->ef_bp[i + 1];
            grid_color(ctx, color.r, color.g, color.b, color.a * m0);
            glVertex3f(p0, 0, 0);
            grid_color(ctx, color.r, color.g, color.b, color.a * m1);
            glVertex3f(p1, 0, 0);
            grid_color(ctx, color.r, color.g, color.b, color.a * m0);
            glVertex3f(0, 0, p0);
            grid_color(ctx, color.r, color.g, color.b, color.a * m1);
            glVertex3f(0, 0, p1);
        }
    }
    glEnd();
    if (line_width != 1.0f)
        glLineWidth(1.0f);
    glDepthMask(GL_FALSE);
}

static void draw_grid_standard_theme(const GridDrawContext *ctx,
                                     const GridThemeSpec *spec) {
    if (spec->begin_pass)
        spec->begin_pass(ctx);

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

    Render3dRgba origin_c = spec->origin_color(ctx);
    origin_c.a = fminf(origin_c.a * ctx->alpha_scale, 1.0f);
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

static Render3dRgba grid_classic_origin_color(const GridDrawContext *ctx) {
    (void)ctx;
    return rgba(0.50f, 0.50f, 0.60f, 0.45f);
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

static Render3dRgba grid_tron_origin_color(const GridDrawContext *ctx) {
    float glow = 0.7f + ctx->breath * 0.3f;
    return rgba(0.0f, 0.8f, 1.0f, 0.25f * glow);
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

static Render3dRgba grid_ember_origin_color(const GridDrawContext *ctx) {
    (void)ctx;
    float ripple0 = -sinf(ctx->anim_time * 2.5f) * 0.5f + 0.5f;
    return rgba(0.95f, 0.35f + ripple0 * 0.25f, 0.05f,
                0.7f * (0.6f + ripple0 * 0.4f));
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

static Render3dRgba grid_aurora_origin_color(const GridDrawContext *ctx) {
    float s0 = sinf(ctx->anim_time * 0.7f) * 0.5f + 0.5f;
    return rgba(0.50f, 0.80f + 0.10f * s0, 0.95f, 0.50f);
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
    /* Hot bases — the Sunset backdrop's bright horizon glow sits right
     * behind these lines, so they need to punch well above the
     * Tron-level alphas to read as neon rather than silhouette. */
    float base = is_major ? 0.62f : 0.22f;

    out->x_const = rgba(1.0f, 0.18f, 0.72f, base * fade * glow);

    float wave = sinf(v * 1.8f - ctx->anim_time * 2.2f) * 0.5f + 0.5f;
    out->z_const = rgba(1.0f, 0.18f + 0.24f * wave, 0.72f + 0.22f * wave,
                        base * fade * (0.65f + 0.55f * wave) * glow);
}

static Render3dRgba grid_synthwave_origin_color(const GridDrawContext *ctx) {
    /* Cyan accent against the pink field, pulsing with the same breath. */
    float glow = 0.75f + ctx->breath * 0.25f;
    return rgba(0.15f, 0.85f, 1.0f, 0.40f * glow);
}

static void grid_ruler_line_color(float v, int is_major,
                                  const GridDrawContext *ctx,
                                  GridLineColors *out) {
    float dist_frac = fabsf(v) / ctx->extent;
    float fade = 1.0f - dist_frac * dist_frac;
    float a = (is_major ? 0.18f : 0.07f) * fade;
    out->x_const = rgba(0.82f, 0.42f, 0.18f, a);
    out->z_const = rgba(0.22f, 0.42f, 0.88f, a);
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
};

static const GridThemeSpec *grid_theme_spec(Render3dGridTheme theme) {
    if (theme <= GRID_THEME_OFF || theme >= GRID_THEME_COUNT)
        return NULL;
    if (!g_grid_theme_specs[theme].line_color)
        return NULL;
    return &g_grid_theme_specs[theme];
}

static void render3d_grid_apply_quality_config(const Render3dRenderConfig *config) {
    if (config->multisample_enabled) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);
    if (config->line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);
}

/* Camera world-space height; < 0 means the eye is below the grid
 * plane (Ocean's underwater branch, Frozen's under-ice branch). */
static float grid_camera_world_y(const Render3dRenderConfig *config) {
    float camera_rx_rad = config->cam_rx * (float)M_PI / 180.0f;
    return config->cam_ty + sinf(camera_rx_rad) * config->cam_dist;
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
     * leaves fog enabled (linear, end ≈ grid extent) — and after
     * fb976f0 it may also have GL_FOG_DISTANCE_MODE_NV =
     * GL_EYE_RADIAL_NV set when the OCEAN theme is the dispatch
     * branch. With identity modelview the rect's eye-space radial
     * distances run 0..sqrt(render3d_w² + render3d_h²), wildly past
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
    if (grid_camera_world_y(config) < 0.0f) {
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
     * A semi-transparent rippling mesh at Y ≈ 0.  Because the grid pass
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

/* Frozen Lake: a winter pond. Major grid lines are fracture cracks —
 * jittered polylines with a wide faint halo, a bright core, and a
 * dimmer echo sunk below the surface so the sheet reads as thick;
 * minor lines stay faint straight etch marks under the ice. The
 * pseudo-scene layer (the Ocean water-surface analogue) is a
 * translucent blue-white ice sheet just above Y=0 with static
 * frost-heave displacement and a slow sheen drifting across it.
 * Below Y=0 the viewport gets a pale glacial tint (same mechanism as
 * Ocean's underwater fill) — looking up through the ice. */

/* Deterministic position hash in [-1, 1]; stable frame-to-frame so
 * the cracks and frost heave stay frozen in place (no swimming). */
static float grid_pos_hash(float a, float b) {
    float s = sinf(a * 12.9898f + b * 78.233f) * 43758.5453f;
    return (s - floorf(s)) * 2.0f - 1.0f;
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
     * pairing) — but at the FAR extent the generic recede fog is
     * already enabled and keyed to the clear color, which would fade
     * the far field to black; recolor it glacial so the recede whites
     * out into the same tint instead. (LINEAR params stay; non-FAR
     * steady frames remain fog-call-free, which the fog<->predicate
     * test pins.) The tint rect itself is immune to the mist
     * (grid_draw_viewport_tint brackets and disables fog). */
    if (grid_camera_world_y(config) < 0.0f) {
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
     * are skipped here — the crack pass below replaces them. */
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
                          rgba(0.80f, 0.92f, 1.0f, fminf(0.55f * as, 1.0f)),
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
     * inside the mist on purpose — it whites out toward the horizon. */
    grid_fog_end(grid_ctx);
}

/* Tilled Field: a plowed-earth floor where the grid's two directions
 * are deliberately asymmetric. Constant-z major lines ARE the furrows
 * — V-grooves cut into an opaque soil height-field running along X —
 * while constant-x majors stay shallow: pale "planting string" lines
 * laid over the ridges. Minor constant-z lines render as faint rake
 * marks riding the furrow profile. The soil mesh writes depth (it is
 * ground, not a translucent overlay), and a camera below the surface
 * gets a near-black earth filter — underground you see almost
 * nothing, which is the point. */

#define GRID_SOIL_FURROW_HALF_FRAC 0.22f  /* furrow half-width, × major */
#define GRID_SOIL_DEPTH 0.30f             /* furrow depth, world units */

/* Furrow cross-section: smoothstep dip around each constant-z major
 * line. Depends only on z, so any line running along X sits at one
 * constant height — minor rake lines follow the terrain for free. */
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
    if (grid_camera_world_y(config) < 0.0f)
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
            /* Rake marks: constant-z minors only — the along-X
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
     * — lift it to its groove-bottom height so it doesn't vanish. */
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

/* Star Chart: the floor as a midnight observatory map — the
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
                          rgba(0.92f, 0.80f, 0.52f, fminf(0.40f * as, 1.0f)),
                          1.5f);

    grid_chart_draw_links(grid_ctx);
    grid_chart_draw_nodes(grid_ctx, config);
}

static void render3d_grid_render_xzruler_theme(const GridDrawContext *grid_ctx) {
    float extent = grid_ctx->extent;
    float major = grid_ctx->major;
    float step = grid_ctx->step;
    float major_tol = grid_ctx->major_tol;
    float as = grid_ctx->alpha_scale;

    /* Non-origin grid lines with directional colour coding */
    glBegin(GL_LINES);
    for (float v = -extent; v <= extent + GRID_LOOP_EPSILON; v += step) {
        if (fabsf(v) < GRID_ORIGIN_SKIP_EPSILON) continue;
        int is_major = grid_is_major_line(v, major, major_tol);
        GridLineColors colors;
        grid_ruler_line_color(v, is_major, grid_ctx, &colors);
        colors.x_const.a = fminf(colors.x_const.a * as, 1.0f);
        colors.z_const.a = fminf(colors.z_const.a * as, 1.0f);
        draw_grid_line_pair(v, grid_ctx, colors);
    }
    glEnd();

    /* Origin axes - bright, wider. Edge-fade subdivides each axis so it
     * dissolves into the backdrop at the rim like the grid lines. */
    glDepthMask(GL_TRUE);
    glLineWidth(2.0f);
    glBegin(GL_LINES);
    if (grid_ctx->edge_fade) {
        for (int i = 0; i + 1 < grid_ctx->ef_n; i++) {
            float m0 = grid_ctx->ef_mul[i], m1 = grid_ctx->ef_mul[i + 1];
            float p0 = grid_ctx->ef_bp[i],  p1 = grid_ctx->ef_bp[i + 1];
            /* X axis (z=0, along X) */
            grid_color(grid_ctx, 0.88f, 0.28f, 0.12f, 0.70f * m0);
            glVertex3f(p0, 0, 0);
            grid_color(grid_ctx, 0.88f, 0.28f, 0.12f, 0.70f * m1);
            glVertex3f(p1, 0, 0);
            /* Z axis (x=0, along Z) */
            grid_color(grid_ctx, 0.12f, 0.32f, 0.88f, 0.70f * m0);
            glVertex3f(0, 0, p0);
            grid_color(grid_ctx, 0.12f, 0.32f, 0.88f, 0.70f * m1);
            glVertex3f(0, 0, p1);
        }
    } else {
        /* X axis (z=0, runs along X) */
        grid_color(grid_ctx, 0.88f, 0.28f, 0.12f, 0.70f);
        glVertex3f(-extent, 0, 0); glVertex3f(extent, 0, 0);
        /* Z axis (x=0, runs along Z) */
        grid_color(grid_ctx, 0.12f, 0.32f, 0.88f, 0.70f);
        glVertex3f(0, 0, -extent); glVertex3f(0, 0, extent);
    }
    glEnd();
    glLineWidth(1.0f);
    glDepthMask(GL_FALSE);

    /* Ruler tick marks at major-line intervals on both axes */
    float tick = 0.06f;
    glBegin(GL_LINES);
    for (float v = -extent; v <= extent + GRID_LOOP_EPSILON; v += major) {
        if (fabsf(v) < GRID_ORIGIN_SKIP_EPSILON) continue;
        float ta = (fabsf(v) <= major * 2.5f) ? 0.48f : 0.22f;
        ta = fminf(ta * as, 1.0f) * grid_edge_fade_mul(grid_ctx, v);
        /* Ticks crossing the X axis in the Z direction */
        grid_color(grid_ctx, 0.88f, 0.28f, 0.12f, ta);
        glVertex3f(v, 0, -tick); glVertex3f(v, 0, tick);
        /* Ticks crossing the Z axis in the X direction */
        grid_color(grid_ctx, 0.12f, 0.32f, 0.88f, ta);
        glVertex3f(-tick, 0, v); glVertex3f(tick, 0, v);
    }
    glEnd();
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
     * XY plane (z=0, normal Z): face-on weight = cos²(ry)
     * ZY plane (x=0, normal X): face-on weight = sin²(ry)
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
    return grid_theme == GRID_THEME_OCEAN;
}

/* ===========================================================================
 * 2D-oriented grid themes (Sketchbook + Neon Graph)
 *
 * These draw their graticule in the XY plane (z = GRID_2D_Z, just behind the
 * z=0 scene geometry) instead of the XZ ground plane, so they read as a flat,
 * front-facing grid in the 2D ortho view (camera looking down -Z). In 3D they
 * render as a vertical wall at z=0 — acceptable until grids are filtered by
 * view mode. Both use the Dusk scene palette and route line alpha through
 * grid_color() so the show/hide transition fade still applies. Custom themes:
 * no GridThemeSpec entry, so render3d_grid_theme_uses_edge_fade() is false and
 * the radial edge-fade machinery is skipped.
 * ========================================================================= */

#define GRID_2D_Z (-0.02f)   /* sit just behind z=0 user geometry */

/* Smooth, frame-stable wobble for the inked sketch strokes. `u` runs along the
 * stroke, `v` is its fixed coordinate, `seed` separates passes/lines. Returns a
 * small world-space offset (~±0.05) — large enough to read as hand-drawn at
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
                               int segs, float seed) {
    float dx = bx - ax, dy = by - ay;
    float len = sqrtf(dx * dx + dy * dy);
    float nx = (len > 1e-5f) ? -dy / len : 0.0f;
    float ny = (len > 1e-5f) ?  dx / len : 0.0f;
    glBegin(GL_LINE_STRIP);
    for (int k = 0; k <= segs; k++) {
        float u = (float)k / (float)segs;
        float px = ax + dx * u, py = ay + dy * u;
        float env = sinf(u * (float)M_PI);                 /* 0 at ends, 1 mid */
        float w = grid_sketch_wobble(u * len, ax + ay, seed) * env;
        glVertex3f(px + nx * w, py + ny * w, GRID_2D_Z);
    }
    glEnd();
}

/* GLUT stroke (line) glyphs anchored at (x,y) in the XY plane, `scale` world
 * units per font unit. Matches the inked aesthetic (and scales with the sheet
 * on zoom, unlike the fixed-pixel bitmap font). Color + line width are the
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

/* Sketchbook: a bounded hand-drawn "field study" sheet — wobbly ink lines on
 * the dark canvas with cool-ink strokes, column letters (A..) along the
 * bottom, row numbers (1..) down the left, and a titled header. */
static void render3d_grid_render_sketch_theme(const Render3dRenderConfig *config,
                                              const GridDrawContext *grid_ctx) {
    /* Hand-drawn coordinate graph: wobbly ink cell lines snapped to real
     * world coordinates (multiples of the grid `major` step), each labelled
     * with its actual value, so the labels line up with the gridlines.
     *
     * Fit to the live ortho view: glOrtho's projection matrix gives the
     * visible world half-extents directly (half = 1/|proj[diag]|); the view
     * is centred on the camera pan (cam_tx, cam_ty). Clamp the half-extents
     * to a sane band so the perspective (3D) fallback can't explode the loop
     * counts. */
    GLfloat pm[16];
    glGetFloatv(GL_PROJECTION_MATRIX, pm);
    float half_w = (fabsf(pm[0]) > 1e-6f) ? 1.0f / fabsf(pm[0]) : 6.0f;
    float half_h = (fabsf(pm[5]) > 1e-6f) ? 1.0f / fabsf(pm[5]) : 4.0f;
    if (half_h < 1.5f || half_h > 16.0f) half_h = 4.0f;
    if (half_w < 1.5f || half_w > 24.0f) half_w = half_h * 1.4f;
    float cx = config->cam_tx, cy = config->cam_ty;

    float cell = grid_ctx->major;
    if (cell < 0.25f) cell = 0.25f;

    /* Visible world bounds, centred on the camera pan. Gridlines sit on the
     * `cell`-snapped coordinates inside this range; the line spans run the
     * full view edge-to-edge so the graph fills the screen (no inset). */
    float vx0 = cx - half_w, vx1 = cx + half_w;
    float vy0 = cy - half_h, vy1 = cy + half_h;
    float left  = ceilf(vx0 / cell) * cell;
    float right = floorf(vx1 / cell) * cell;
    float bot   = ceilf(vy0 / cell) * cell;
    float top   = floorf(vy1 / cell) * cell;
    if (right < left - 1e-3f || top < bot - 1e-3f) return;

    const float ink_r = 0.74f, ink_g = 0.80f, ink_g2 = 0.92f;
    int vsegs = (int)((vy1 - vy0) / cell * 3.0f) + 3;
    int hsegs = (int)((vx1 - vx0) / cell * 3.0f) + 3;

    /* Vertical cell lines (bold + faint pass); the x=0 axis line is heavier. */
    for (float x = left; x <= right + 1e-3f; x += cell) {
        int axis = (fabsf(x) < cell * 0.25f);
        for (int s = 0; s < 2; s++) {
            grid_color(grid_ctx, ink_r, ink_g, ink_g2,
                       s == 0 ? (axis ? 0.85f : 0.50f) : 0.16f);
            glLineWidth(s == 0 ? (axis ? 2.1f : 1.4f) : 0.9f);
            grid_sketch_stroke(x, vy0, x, vy1, vsegs, x * 4.3f + s * 31.7f);
        }
    }
    /* Horizontal cell lines; the y=0 axis line is heavier. */
    for (float y = bot; y <= top + 1e-3f; y += cell) {
        int axis = (fabsf(y) < cell * 0.25f);
        for (int s = 0; s < 2; s++) {
            grid_color(grid_ctx, ink_r, ink_g, ink_g2,
                       s == 0 ? (axis ? 0.85f : 0.50f) : 0.16f);
            glLineWidth(s == 0 ? (axis ? 2.1f : 1.4f) : 0.9f);
            grid_sketch_stroke(vx0, y, vx1, y, hsegs, y * 5.1f + s * 23.3f + 100.0f);
        }
    }

    /* Coordinate labels: x values along the bottom of the view, y values down
     * the left of the view — each centred on its gridline so it lines up with
     * the value. Inset just enough from the edge that the glyphs don't clip. */
    float ta = fminf(grid_ctx->xn_alpha * grid_ctx->grid_brightness, 1.0f);
    float lbl = fminf(0.0036f, fmaxf(0.0020f, cell * 0.0034f));   /* ~0.3 world units */
    float xlbl_y = vy0 + cell * 0.38f;          /* just above the bottom edge */
    float ylbl_x = vx0 + cell * 0.12f;          /* just right of the left edge */

    glLineWidth(1.3f);
    glColor4f(0.86f, 0.90f, 0.97f, ta);
    /* Start the x labels one cell in from the left edge so the leftmost one
     * never lands on top of the y-axis label column in the bottom-left corner. */
    float xlbl_start = vx0 + cell * 1.05f;
    for (float x = left; x <= right + 1e-3f; x += cell) {
        if (x < xlbl_start) continue;           /* clear the y-label column */
        if (fabsf(x) < cell * 0.25f) continue;  /* skip 0 (shared with y-axis) */
        char b[16];
        snprintf(b, sizeof b, "%g", (double)x);
        float tw = grid_stroke_text_width(lbl, b);
        grid_stroke_text(x - tw * 0.5f, xlbl_y, lbl, b);
    }
    for (float y = bot; y <= top + 1e-3f; y += cell) {
        char b[16];
        snprintf(b, sizeof b, "%g", (double)y);
        grid_stroke_text(ylbl_x, y + cell * 0.10f, lbl, b);
    }
    glLineWidth(1.0f);
}

/* Neon Graph: a glowing graph-paper grid in the XY plane — faint azure minor
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

/* ---- Grid transition curve plugin (Render3dXnReveal, see render3d_transition.h) --
 * The grid owns its fade durations + per-theme speed + opacity shape; the
 * machine just feeds elapsed time and reads opacity back. A linear opacity
 * ramp is intentional — the reveal's spatial shaping (smoothstep wipe, bright
 * head) is applied on top of opacity in render3d_grid_render, so opacity itself
 * stays a plain 0..1 progress that inverts cleanly for reversal. */

/* Per-theme fade-speed multiplier on GRID_FADE_*_SECS: 1.0 = base, >1 slower,
 * <1 snappier. A 0 entry (the unset default) means base speed. */
static float grid_reveal_time_scale(int grid_theme) {
    if (grid_theme <= GRID_THEME_OFF || grid_theme >= GRID_THEME_COUNT)
        return 1.0f;
    float t = g_grid_reveal[grid_theme].time;
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
    /* Pure reference line-grids dissolve their alpha to the backdrop at
     * the rim instead of fogging to the clear color: the table-driven
     * line themes plus the XZ Ruler and Star Chart — custom-path grids
     * that still emit their graticule through draw_grid_line_pair, so
     * they pick up the per-vertex radial fade once they are in this set.
     * The custom *environment* themes (OCEAN / FROZEN / SOIL) own their
     * own atmosphere, and RADAR owns its distance fog, so all of those
     * are out of scope.
     * Pure — safe to call from tests. */
    if (grid_theme == GRID_THEME_XZRULER ||
        grid_theme == GRID_THEME_STARCHART)
        return 1;
    return grid_theme_spec(grid_theme) != NULL;
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
 * render3d_grid_render is now the sequencer of these phases, and the
 * xn fields live on GridDrawContext rather than as file statics
 * (audit #3). */

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
    render3d_grid_apply_quality_config(config);
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
     * fog — at any extent or transition phase. Make sure none is left
     * enabled from a prior pass and bail. */
    if (render3d_grid_theme_uses_edge_fade(grid_theme)) {
        glDisable(GL_FOG);
        return;
    }
    int is_far = (config->grid_extent_idx == GRID_EXTENT_FAR);
#if GRID_XN_STYLE == GRID_AXES_XN_FOG
    /* Fog-less, non-FAR themes: recede into a synthesized clear-color
     * fog as the overlay hides. At FAR the recede is driven by the
     * FAR block's own fog (below) instead, so it isn't double-set /
     * overwritten. Fog-owning configs took the FADE fallback above. */
    int uses_fog = render3d_grid_theme_uses_fog(grid_theme);
    if (!uses_fog && !is_far)
        grid_xn_apply_transition_fog(xn->fog_tf, extent);
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
    set_fog_to_clear_color();
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, fog_start);
    glFogf(GL_FOG_END, fog_end);
}

/* Custom themes handle their own draw path; the default arm covers
 * every standard theme by spec-table lookup, so adding/removing a
 * GridThemeSpec entry is a one-edit change instead of two parallel
 * lists. set_nv_fog is true iff the runtime supports NV fog distance
 * AND the active theme wants radial eye-distance fog (OCEAN, RADAR). */
static void grid_dispatch_theme(const Render3dFrameRenderContext *frame_ctx,
                                const GridDrawContext *grid_ctx,
                                Render3dGridTheme grid_theme,
                                int set_nv_fog) {
    const Render3dRenderConfig *config = &frame_ctx->config;
    switch (grid_theme) {

    case GRID_THEME_OCEAN:
#if 0 /* nv radial fog breaks some of the geometry, since its per vertex and
         some of the grid lines are very long, extending past the fog end,
         making them invisible */

        /* Opt into radial eye-distance fog when available, so the fog
         * closes in by true distance rather than eye-plane depth and the
         * fringes stop swimming as the camera orbits. */
        if (set_nv_fog)
            glFogi(GL_FOG_DISTANCE_MODE_NV, GL_EYE_RADIAL_NV);
#endif
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

    case GRID_THEME_XZRULER:
        render3d_grid_render_xzruler_theme(grid_ctx);
        break;

    case GRID_THEME_SKETCH:
        render3d_grid_render_sketch_theme(config, grid_ctx);
        break;

    case GRID_THEME_NEON:
        render3d_grid_render_neon_theme(grid_ctx);
        break;

    case GRID_THEME_PLANES:
        render3d_grid_render_planes_theme(config, grid_ctx);
        break;

    case GRID_THEME_RADAR:
        /* Same radial-fog opt-in as Ocean (see above): the radar rings
         * read the shared FAR-extent distance fog, which swims at the
         * fringes under the eye-plane default. */
        if (set_nv_fog)
            glFogi(GL_FOG_DISTANCE_MODE_NV, GL_EYE_RADIAL_NV);
        render3d_grid_render_radar_theme(grid_ctx);
        break;

    default: {
        /* GRID_THEME_CLASSIC, _TRON, _EMBER, _AURORA, _SYNTHWAVE and
         * any future standard line theme: look up its GridThemeSpec and
         * draw through the table-driven path. */
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
    grid_ctx.reveal       = g_grid_reveal[grid_theme];

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
     * Snapshot before the OCEAN/RADAR themes mutate it, restore at the
     * tail before pop. */
    GLint saved_nv_fog_mode = 0;
    int set_nv_fog = (config->nv_fog_distance_supported &&
                      (grid_theme == GRID_THEME_OCEAN ||
                       grid_theme == GRID_THEME_RADAR));
    if (set_nv_fog)
        glGetIntegerv(GL_FOG_DISTANCE_MODE_NV, &saved_nv_fog_mode);

    grid_dispatch_theme(frame_ctx, &grid_ctx, grid_theme, set_nv_fog);

    if (set_nv_fog)
        glFogi(GL_FOG_DISTANCE_MODE_NV, saved_nv_fog_mode);

    glPopMatrix();
    /* render3d_grid_pop_state restores GL_ALL_ATTRIB_BITS, covering
     * GL_DEPTH_BUFFER_BIT (depth mask), GL_COLOR_BUFFER_BIT (blend),
     * GL_FOG_BIT, and GL_LIGHTING_BIT — no manual teardown needed. */
    render3d_grid_pop_state();
}
