/*
 * axes.c - axes theme rendering
 */
#include "axes.h"
#include "overlay_xn.h"  /* Render3dOverlayXn + shared resolve helper */
#include "occluded_ghost.h"  /* RENDER3D_OCCLUDED_GHOST_STIPPLE */
#include "render3d_hash.h"   /* render3d_hash01 - Fountain droplet scatter */
#include <math.h>            /* sinf, cosf, fmodf, M_PI (via gl_includes.h) */

/* ---- Axes transition curve plugin (Render3dXnReveal, see render3d_transition.h).
 * A plain linear opacity ramp over AXES_FADE_*_SECS, no per-theme speed
 * (theme ignored). The machine feeds elapsed time and reads opacity back;
 * elapsed_at inverts the ramp for reversal continuity. */
static float axes_reveal_opacity(int theme, Render3dXnPhase phase, float elapsed) {
    (void)theme;
    if (phase == RENDER3D_XN_STEADY) return 1.0f;
    float dur = (phase == RENDER3D_XN_FADE_IN) ? AXES_FADE_IN_SECS
                                            : AXES_FADE_OUT_SECS;
    float p = (dur > 0.0f) ? elapsed / dur : 1.0f;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return (phase == RENDER3D_XN_FADE_IN) ? p : 1.0f - p;
}

static float axes_reveal_elapsed_at(int theme, Render3dXnPhase phase,
                                    float opacity) {
    (void)theme;
    float dur = (phase == RENDER3D_XN_FADE_IN) ? AXES_FADE_IN_SECS
                                            : AXES_FADE_OUT_SECS;
    float p = (phase == RENDER3D_XN_FADE_IN) ? opacity : 1.0f - opacity;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return p * dur;
}

const Render3dXnReveal render3d_axes_reveal = {
    axes_reveal_opacity, axes_reveal_elapsed_at
};

enum {
    RENDER3D_AXIS_X = 0,
    RENDER3D_AXIS_Y = 1,
    RENDER3D_AXIS_Z = 2,
};

typedef struct AxesThemeSpec {
    float len;
    Render3dRgba axis[3];
    Render3dRgba label[3];
} AxesThemeSpec;

/* Per-frame axes draw context. Resolved once at render3d_axes_render entry
 * via render3d_overlay_xn_resolve; every
 * axes_color call multiplies through xn_alpha. The struct is
 * deliberately small - axes don't carry the breath/anim_time
 * GridDrawContext has because each per-theme renderer that needs
 * those takes them as separate args. Unlike GridDrawContext there is
 * no xn_opacity here: grid needs it for the fog-recede pass, but the
 * axes' fog path reads xn.fog_tf straight off the resolve, so the
 * field was write-only. */
typedef struct AxesDrawContext {
    float xn_alpha;
} AxesDrawContext;

/* AXES_THEME_NEON and AXES_THEME_FOUNTAIN are intentionally NOT in
 * g_axes_theme_specs[] below. The static spec carries one flat axis/label
 * color per axis; NEON and FOUNTAIN are per-frame procedural looks that a
 * single Render3dRgba triplet cannot express. They are handled inline in
 * render3d_axes_render's switch; only their axis lengths are plain
 * constants. */
#define AXES_NEON_LEN 2.5f
#define AXES_FOUNTAIN_LEN 2.5f

static void render3d_axes_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void render3d_axes_pop_state(void) {
    glPopAttrib();
}

static Render3dRgba rgba(float r, float g, float b, float a) {
    Render3dRgba c = { r, g, b, a };
    return c;
}

/* Axes in-out transition. Resolved once at render3d_axes_render entry from
 * config.axes_opacity
 * via the shared render3d_overlay_xn_resolve helper (overlay_xn.h),
 * stored on AxesDrawContext, then every color path routes through
 * axes_color so it applies uniformly AFTER each call site's own
 * alpha_scale clamp. 1.0 = shown.
 *
 * Axes sit near the origin where distance fog barely bites, so under
 * the FOG style this mostly amounts to the alpha knee with a faint
 * clear-color haze - an intentionally subtle variant (the strong fog
 * look is the grid's). axes never sets uses_own_fog. */

#if AXES_XN_STYLE == GRID_AXES_XN_FOG
#define AXES_XN_FOG_ALPHA_KNEE 0.30f
#define AXES_XN_FOG_REACH      4.0f   /* nominal axis+label extent */

/* tf = 1 - opacity (0 shown .. 1 hidden). Pull a clear-color linear-fog
 * wall in toward the origin as the axes fade out. Untouched at tf<=0 so
 * a steady, fully-shown axes set is unfogged. */
static void axes_xn_apply_transition_fog(float tf, const float clear_col[4]) {
    if (tf <= 0.0f) return;
    glFogfv(GL_FOG_COLOR, clear_col);
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    float far_end  = AXES_XN_FOG_REACH;
    float near_end = AXES_XN_FOG_REACH * 0.02f;
    float end = far_end + (near_end - far_end) * tf;
    glFogf(GL_FOG_START, end * 0.15f);
    glFogf(GL_FOG_END,   end);
}
#endif

static void axes_color(const AxesDrawContext *ctx,
                       float r, float g, float b, float a) {
    glColor4f(r, g, b, a * ctx->xn_alpha);
}

static void axes_color_rgba(const AxesDrawContext *ctx, Render3dRgba c) {
    axes_color(ctx, c.r, c.g, c.b, c.a);
}

/* Helper: draw an axis label at a 3D position */
static void draw_axis_label(const AxesDrawContext *ctx,
                            float x, float y, float z, char ch,
                            float r, float g, float b) {
    axes_color(ctx, r, g, b, 1.0f);
    glRasterPos3f(x, y, z);
    glutBitmapCharacter(FONT_MONO, ch);
}

static const AxesThemeSpec g_axes_theme_specs[AXES_THEME_COUNT] = {
    [AXES_THEME_CLASSIC] = {
        .len = 2.0f,
        .axis = {
            {0.90f, 0.20f, 0.20f, 1.0f},
            {0.20f, 0.90f, 0.20f, 1.0f},
            {0.20f, 0.20f, 0.90f, 1.0f},
        },
        .label = {
            {0.90f, 0.30f, 0.30f, 1.0f},
            {0.30f, 0.90f, 0.30f, 1.0f},
            {0.30f, 0.30f, 0.90f, 1.0f},
        },
    },
    [AXES_THEME_PULSE] = {
        .len = 3.0f,
        .axis = {
            {0.90f, 0.20f, 0.20f, 0.30f},
            {0.20f, 0.90f, 0.20f, 0.30f},
            {0.20f, 0.20f, 0.90f, 0.30f},
        },
        .label = {
            {0.70f, 0.25f, 0.25f, 1.0f},
            {0.25f, 0.70f, 0.25f, 1.0f},
            {0.25f, 0.25f, 0.70f, 1.0f},
        },
    },
    [AXES_THEME_COMPASS] = {
        .len = 2.5f,
        .axis = {
            {1.0f, 0.30f, 0.30f, 0.85f},
            {0.30f, 1.0f, 0.30f, 0.85f},
            {0.30f, 0.30f, 1.0f, 0.85f},
        },
        .label = {
            {0.90f, 0.30f, 0.30f, 1.0f},
            {0.30f, 0.90f, 0.30f, 1.0f},
            {0.30f, 0.30f, 0.90f, 1.0f},
        },
    },
    [AXES_THEME_GIZMO] = {
        .len = 2.0f,
        .axis = {
            {0.90f, 0.20f, 0.20f, 0.85f},
            {0.20f, 0.90f, 0.20f, 0.85f},
            {0.20f, 0.20f, 0.90f, 0.85f},
        },
        .label = {
            {0.90f, 0.25f, 0.25f, 1.0f},
            {0.25f, 0.90f, 0.25f, 1.0f},
            {0.25f, 0.25f, 0.90f, 1.0f},
        },
    },
    [AXES_THEME_RULER] = {
        .len = 5.0f,
        .axis = {
            {0.88f, 0.30f, 0.22f, 0.90f},
            {0.34f, 0.85f, 0.34f, 0.90f},
            {0.30f, 0.46f, 0.92f, 0.90f},
        },
        .label = {
            {0.92f, 0.42f, 0.34f, 1.0f},
            {0.46f, 0.90f, 0.46f, 1.0f},
            {0.42f, 0.56f, 0.95f, 1.0f},
        },
    },
    [AXES_THEME_ARROW] = {
        .len = 2.5f,
        .axis = {
            {0.92f, 0.22f, 0.22f, 1.0f},
            {0.22f, 0.92f, 0.22f, 1.0f},
            {0.22f, 0.22f, 0.92f, 1.0f},
        },
        .label = {
            {0.95f, 0.35f, 0.35f, 1.0f},
            {0.35f, 0.95f, 0.35f, 1.0f},
            {0.35f, 0.35f, 0.95f, 1.0f},
        },
    },
};

static const AxesThemeSpec *axes_theme_spec(Render3dAxesTheme theme) {
    if (theme <= AXES_THEME_OFF || theme >= AXES_THEME_COUNT)
        return NULL;
    if (g_axes_theme_specs[theme].len <= 0.0f)
        return NULL;
    return &g_axes_theme_specs[theme];
}

static void render3d_axes_apply_quality_config(const Render3dRenderConfig *config) {
    if (config->multisample_enabled) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);
    if (config->line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);
}

static void draw_axis_line_triplet(const AxesDrawContext *ctx,
                                   float len, float width,
                                   const Render3dRgba colors[3],
                                   int direction) {
    float end = len * (float)direction;
    glLineWidth(width);
    glBegin(GL_LINES);
    axes_color_rgba(ctx, colors[RENDER3D_AXIS_X]);
    glVertex3f(0, 0, 0); glVertex3f(end, 0, 0);
    axes_color_rgba(ctx, colors[RENDER3D_AXIS_Y]);
    glVertex3f(0, 0, 0); glVertex3f(0, end, 0);
    axes_color_rgba(ctx, colors[RENDER3D_AXIS_Z]);
    glVertex3f(0, 0, 0); glVertex3f(0, 0, end);
    glEnd();
    glLineWidth(1.0f);
}

static void draw_axis_tip_triplet(const AxesDrawContext *ctx,
                                  float len, float point_size,
                                  const Render3dRgba colors[3],
                                  int direction) {
    float end = len * (float)direction;
    glPointSize(point_size);
    glBegin(GL_POINTS);
    axes_color_rgba(ctx, colors[RENDER3D_AXIS_X]);
    glVertex3f(end, 0, 0);
    axes_color_rgba(ctx, colors[RENDER3D_AXIS_Y]);
    glVertex3f(0, end, 0);
    axes_color_rgba(ctx, colors[RENDER3D_AXIS_Z]);
    glVertex3f(0, 0, end);
    glEnd();
    glPointSize(1.0f);
}

static void draw_axis_label_triplet(const AxesDrawContext *ctx,
                                    float len, float offset,
                                    const Render3dRgba colors[3],
                                    const char labels[3],
                                    int direction) {
    float pos = (len + offset) * (float)direction;
    draw_axis_label(ctx, pos, 0, 0, labels[RENDER3D_AXIS_X],
                    colors[RENDER3D_AXIS_X].r, colors[RENDER3D_AXIS_X].g,
                    colors[RENDER3D_AXIS_X].b);
    draw_axis_label(ctx, 0, pos, 0, labels[RENDER3D_AXIS_Y],
                    colors[RENDER3D_AXIS_Y].r, colors[RENDER3D_AXIS_Y].g,
                    colors[RENDER3D_AXIS_Y].b);
    draw_axis_label(ctx, 0, 0, pos, labels[RENDER3D_AXIS_Z],
                    colors[RENDER3D_AXIS_Z].r, colors[RENDER3D_AXIS_Z].g,
                    colors[RENDER3D_AXIS_Z].b);
}

/* --- per-theme axes renderers ---
 *
 * Each theme has a named static helper, so the dispatcher reads like a
 * table of contents and each renderer owns its own small body. The pattern
 * mirrors grid.c's per-theme functions. */

static void render3d_axes_render_classic_theme(const AxesDrawContext *ctx) {
    const AxesThemeSpec *spec = axes_theme_spec(AXES_THEME_CLASSIC);
    draw_axis_line_triplet(ctx, spec->len, 2.0f, spec->axis, 1);
    draw_axis_label_triplet(ctx, spec->len, 0.15f, spec->label, "XYZ", 1);
}

static void render3d_axes_render_pulse_theme(const AxesDrawContext *ctx,
                                          float anim_time) {
    const AxesThemeSpec *spec = axes_theme_spec(AXES_THEME_PULSE);
    float len = spec->len;
    /* Solid dim axes */
    draw_axis_line_triplet(ctx, len, 1.5f, spec->axis, 1);

    /* Pulsing dot position (loops 0..1) */
    float t = fmodf(anim_time * 0.6f, 1.0f);
    float pos = t * len;
    float glow = sinf(t * (float)M_PI); /* bright in middle, dim at ends */
    glow = glow * 0.8f + 0.2f;

    glPointSize(8.0f);
    glBegin(GL_POINTS);
    axes_color(ctx, 1.0f, 0.3f, 0.3f, glow);
    glVertex3f(pos, 0, 0);
    axes_color(ctx, 0.3f, 1.0f, 0.3f, glow);
    glVertex3f(0, pos, 0);
    axes_color(ctx, 0.3f, 0.3f, 1.0f, glow);
    glVertex3f(0, 0, pos);
    glEnd();
    glPointSize(1.0f);

    /* Bright trail behind the dot */
    glLineWidth(3.0f);
    float trail = 0.6f;
    float t0 = pos - trail;
    if (t0 < 0) t0 = 0;
    glBegin(GL_LINES);
    axes_color(ctx, 1.0f, 0.3f, 0.3f, 0.05f);
    glVertex3f(t0, 0, 0);
    axes_color(ctx, 1.0f, 0.3f, 0.3f, glow * 0.7f);
    glVertex3f(pos, 0, 0);

    axes_color(ctx, 0.3f, 1.0f, 0.3f, 0.05f);
    glVertex3f(0, t0, 0);
    axes_color(ctx, 0.3f, 1.0f, 0.3f, glow * 0.7f);
    glVertex3f(0, pos, 0);

    axes_color(ctx, 0.3f, 0.3f, 1.0f, 0.05f);
    glVertex3f(0, 0, t0);
    axes_color(ctx, 0.3f, 0.3f, 1.0f, glow * 0.7f);
    glVertex3f(0, 0, pos);
    glEnd();
    glLineWidth(1.0f);

    draw_axis_label_triplet(ctx, len, 0.15f, spec->label, "XYZ", 1);
}

/* Procedural theme - see the AXES_NEON_LEN note at the top of this
 * file for why NEON, like FOUNTAIN, doesn't sit in the
 * g_axes_theme_specs table. */
static void render3d_axes_render_neon_theme(const AxesDrawContext *ctx,
                                         float breath, float as) {
    float len = AXES_NEON_LEN;
    float glow = 0.6f + breath * 0.4f;
    Render3dRgba outer[3] = {
        rgba(1.0f, 0.1f, 0.1f, fminf(0.12f * glow * as, 1.0f)),
        rgba(0.1f, 1.0f, 0.1f, fminf(0.12f * glow * as, 1.0f)),
        rgba(0.1f, 0.1f, 1.0f, fminf(0.12f * glow * as, 1.0f)),
    };
    Render3dRgba core[3] = {
        rgba(1.0f, 0.4f, 0.4f, 1.0f * glow),
        rgba(0.4f, 1.0f, 0.4f, 1.0f * glow),
        rgba(0.4f, 0.4f, 1.0f, 1.0f * glow),
    };
    Render3dRgba tips[3] = {
        rgba(1.0f, 0.5f, 0.5f, glow),
        rgba(0.5f, 1.0f, 0.5f, glow),
        rgba(0.5f, 0.5f, 1.0f, glow),
    };

    /* Outer glow (wide, dim) -> core (narrow, bright) -> bright tip dots */
    draw_axis_line_triplet(ctx, len, 6.0f, outer, 1);
    draw_axis_line_triplet(ctx, len, 2.0f, core, 1);
    draw_axis_tip_triplet(ctx, len, 6.0f, tips, 1);

    float la = 0.5f + glow * 0.5f;
    Render3dRgba labels[3] = {
        rgba(1.0f * la, 0.3f * la, 0.3f * la, 1.0f),
        rgba(0.3f * la, 1.0f * la, 0.3f * la, 1.0f),
        rgba(0.3f * la, 0.3f * la, 1.0f * la, 1.0f),
    };
    draw_axis_label_triplet(ctx, len, 0.15f, labels, "XYZ", 1);
}

static void render3d_axes_render_compass_theme(const AxesDrawContext *ctx) {
    const AxesThemeSpec *spec = axes_theme_spec(AXES_THEME_COMPASS);
    float len = spec->len;
    Render3dRgba negative_axes[3] = {
        rgba(1.0f, 0.30f, 0.30f, 0.35f),
        rgba(0.30f, 1.0f, 0.30f, 0.35f),
        rgba(0.30f, 0.30f, 1.0f, 0.35f),
    };
    Render3dRgba positive_tips[3] = {
        rgba(1.0f, 0.4f, 0.4f, 0.9f),
        rgba(0.4f, 1.0f, 0.4f, 0.9f),
        rgba(0.4f, 0.4f, 1.0f, 0.9f),
    };
    Render3dRgba negative_tips[3] = {
        rgba(1.0f, 0.3f, 0.3f, 0.30f),
        rgba(0.3f, 1.0f, 0.3f, 0.30f),
        rgba(0.3f, 0.3f, 1.0f, 0.30f),
    };

    /* Positive axes (solid) */
    draw_axis_line_triplet(ctx, len, 2.0f, spec->axis, 1);

    /* Negative axes (stippled) */
    glEnable(GL_LINE_STIPPLE);
    glLineStipple(2, RENDER3D_OCCLUDED_GHOST_STIPPLE);
    draw_axis_line_triplet(ctx, len, 2.0f, negative_axes, -1);
    glDisable(GL_LINE_STIPPLE);

    /* Arrowheads at positive tips, small dots at negative tips */
    draw_axis_tip_triplet(ctx, len, 7.0f, positive_tips, 1);
    draw_axis_tip_triplet(ctx, len, 4.0f, negative_tips, -1);

    /* Origin sphere-ish dot */
    glPointSize(5.0f);
    glBegin(GL_POINTS);
    axes_color(ctx, 0.9f, 0.9f, 0.9f, 0.6f);
    glVertex3f(0, 0, 0);
    glEnd();
    glPointSize(1.0f);

    draw_axis_label_triplet(ctx, len, 0.15f, spec->label, "XYZ", 1);
    Render3dRgba negative_labels[3] = {
        rgba(0.55f, 0.25f, 0.25f, 1.0f),
        rgba(0.25f, 0.55f, 0.25f, 1.0f),
        rgba(0.25f, 0.25f, 0.55f, 1.0f),
    };
    draw_axis_label_triplet(ctx, len, 0.15f, negative_labels, "xyz", -1);
}

static void render3d_axes_render_gizmo_theme(const AxesDrawContext *ctx,
                                          const Render3dRenderConfig *config,
                                          float as) {
    const AxesThemeSpec *spec = axes_theme_spec(AXES_THEME_GIZMO);
    float len  = spec->len;
    float fill = len / 2.0f;

    /* Camera-facing weight for each vertical plane:
     * XY (z=0) is face-on when camera looks along Z -> weight = cos^2(ry)
     * ZY (x=0) is face-on when camera looks along X -> weight = sin^2(ry) */
    float ry_rad = config->cam_ry * (float)M_PI / 180.0f;
    float cos_ry = cosf(ry_rad), sin_ry = sinf(ry_rad);
    float xy_w = cos_ry * cos_ry;
    float zy_w = sin_ry * sin_ry;

    /* Axis lines (same palette as Classic) */
    draw_axis_line_triplet(ctx, len, 2.0f, spec->axis, 1);

    /* XZ floor plane quadrant - always shown */
    glBegin(GL_QUADS);
    axes_color(ctx, 0.58f, 0.60f, 0.72f, fminf(0.07f * as, 1.0f));
    glVertex3f(0,    0, 0);    glVertex3f(fill, 0, 0);
    glVertex3f(fill, 0, fill); glVertex3f(0,    0, fill);
    glEnd();

    /* XY plane quadrant (z=0) - fades in when looking along Z */
    float xy_a = fminf(0.11f * xy_w * as, 1.0f);
    if (xy_a > 0.004f) {
        glBegin(GL_QUADS);
        axes_color(ctx, 0.80f, 0.50f, 0.25f, xy_a);
        glVertex3f(0,    0,    0); glVertex3f(fill, 0,    0);
        glVertex3f(fill, fill, 0); glVertex3f(0,    fill, 0);
        glEnd();
        glBegin(GL_LINE_LOOP);
        axes_color(ctx, 0.92f, 0.66f, 0.42f, xy_a * 2.2f);
        glVertex3f(0,    0,    0); glVertex3f(fill, 0,    0);
        glVertex3f(fill, fill, 0); glVertex3f(0,    fill, 0);
        glEnd();
    }

    /* ZY plane quadrant (x=0) - fades in when looking along X */
    float zy_a = fminf(0.11f * zy_w * as, 1.0f);
    if (zy_a > 0.004f) {
        glBegin(GL_QUADS);
        axes_color(ctx, 0.32f, 0.58f, 0.88f, zy_a);
        glVertex3f(0, 0,    0);    glVertex3f(0, 0,    fill);
        glVertex3f(0, fill, fill); glVertex3f(0, fill, 0);
        glEnd();
        glBegin(GL_LINE_LOOP);
        axes_color(ctx, 0.48f, 0.72f, 0.95f, zy_a * 2.2f);
        glVertex3f(0, 0,    0);    glVertex3f(0, 0,    fill);
        glVertex3f(0, fill, fill); glVertex3f(0, fill, 0);
        glEnd();
    }

    /* Origin dot */
    glPointSize(5.0f);
    glBegin(GL_POINTS);
    axes_color(ctx, 0.95f, 0.95f, 0.95f, 0.72f);
    glVertex3f(0, 0, 0);
    glEnd();
    glPointSize(1.0f);

    draw_axis_label_triplet(ctx, len, 0.15f, spec->label, "XYZ", 1);
}

/* Solid axes with measurement ticks: a short perpendicular bar at
 * every unit, longer every 5 (mirrors the grid XZ Ruler). */
static void render3d_axes_render_ruler_theme(const AxesDrawContext *ctx) {
    const AxesThemeSpec *spec = axes_theme_spec(AXES_THEME_RULER);
    float len = spec->len;
    draw_axis_line_triplet(ctx, len, 2.0f, spec->axis, 1);

    glBegin(GL_LINES);
    for (int i = 1; i <= (int)len; i++) {
        float t = (i % 5 == 0) ? 0.16f : 0.07f;
        /* X axis: ticks span +/- Z */
        axes_color_rgba(ctx, spec->axis[RENDER3D_AXIS_X]);
        glVertex3f((float)i, 0, -t); glVertex3f((float)i, 0, t);
        /* Y axis: ticks span +/- X */
        axes_color_rgba(ctx, spec->axis[RENDER3D_AXIS_Y]);
        glVertex3f(-t, (float)i, 0); glVertex3f(t, (float)i, 0);
        /* Z axis: ticks span +/- X */
        axes_color_rgba(ctx, spec->axis[RENDER3D_AXIS_Z]);
        glVertex3f(-t, 0, (float)i); glVertex3f(t, 0, (float)i);
    }
    glEnd();

    draw_axis_label_triplet(ctx, len, 0.15f, spec->label, "XYZ", 1);
}

/* -- Arrow theme: solid axis shafts with 3D arrowhead cones.
 * The shaft fades from full color at the origin to ~0.55 alpha at the tip,
 * so the cone reads as the bright focal point.  Cones are drawn as a
 * GL_TRIANGLE_FAN (cheaper than glutSolidCone and avoids lighting state). */

#define ARROW_CONE_RADIUS 0.06f
#define ARROW_CONE_HEIGHT 0.22f
#define ARROW_CONE_SLICES 12

/* Draw a single cone arrowhead at the tip of an axis.
 * axis_idx: 0=X, 1=Y, 2=Z.  Rotates the fan to point along the axis. */
static void draw_axis_cone(const AxesDrawContext *ctx, int axis_idx,
                           float len, Render3dRgba color) {
    glPushMatrix();
    switch (axis_idx) {
    case RENDER3D_AXIS_X:
        glTranslatef(len, 0.0f, 0.0f);
        glRotatef(90.0f, 0.0f, 1.0f, 0.0f);
        break;
    case RENDER3D_AXIS_Y:
        glTranslatef(0.0f, len, 0.0f);
        glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
        break;
    case RENDER3D_AXIS_Z:
        glTranslatef(0.0f, 0.0f, len);
        /* identity rotation - fan already points along +Z */
        break;
    }
    /* Cone tip (apex) */
    axes_color(ctx, color.r, color.g, color.b, color.a);
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(0.0f, 0.0f, ARROW_CONE_HEIGHT);
    /* Base circle */
    axes_color(ctx, color.r * 0.7f, color.g * 0.7f, color.b * 0.7f,
               color.a * 0.85f);
    for (int i = ARROW_CONE_SLICES; i >= 0; i--) {
        float angle = (float)i * 2.0f * (float)M_PI / (float)ARROW_CONE_SLICES;
        glVertex3f(cosf(angle) * ARROW_CONE_RADIUS,
                   sinf(angle) * ARROW_CONE_RADIUS, 0.0f);
    }
    glEnd();
    glPopMatrix();
}

static void render3d_axes_render_arrow_theme(const AxesDrawContext *ctx) {
    const AxesThemeSpec *spec = axes_theme_spec(AXES_THEME_ARROW);
    float len = spec->len;

    /* Gradient shafts: full color at origin, faded toward the tip */
    float tip_alpha = 0.55f;
    glLineWidth(2.5f);
    glBegin(GL_LINES);
    for (int a = 0; a < 3; a++) {
        Render3dRgba c = spec->axis[a];
        axes_color(ctx, c.r, c.g, c.b, c.a);
        glVertex3f(0.0f, 0.0f, 0.0f);   /* all three shafts start here */
        axes_color(ctx, c.r, c.g, c.b, c.a * tip_alpha);
        glVertex3f(a == 0 ? len : 0.0f, /* ...and the tip picks the axis */
                   a == 1 ? len : 0.0f,
                   a == 2 ? len : 0.0f);
    }
    glEnd();
    glLineWidth(1.0f);

    /* 3D arrowhead cones at each tip */
    for (int a = 0; a < 3; a++) {
        draw_axis_cone(ctx, a, len, spec->axis[a]);
    }

    /* Origin dot */
    glPointSize(4.0f);
    glBegin(GL_POINTS);
    axes_color(ctx, 0.92f, 0.92f, 0.92f, 0.70f);
    glVertex3f(0, 0, 0);
    glEnd();
    glPointSize(1.0f);

    draw_axis_label_triplet(ctx, len, 0.25f, spec->label, "XYZ", 1);
}

/* -- Fountain theme: droplet-stream coordinate axes.
 * Each axis gets a dim solid line and a stream of droplets sprayed out
 * of the origin. They decelerate as they climb, spread laterally on the
 * way, and fade out before the tip, then respawn. A leader droplet
 * drives the phase; the rest use golden-ratio offsets for stateless,
 * evenly-spaced distribution. Procedural (per-frame positions driven by
 * anim_time), so like NEON it stays out of the spec table. */

#define FOUNTAIN_DROPLET_COUNT 150
#define FOUNTAIN_STREAM_SPEED  0.10f          /* axis-lengths per second   */
#define FOUNTAIN_PHI           0.6180339887f  /* 1/phi - golden ratio fract */
#define FOUNTAIN_SPRAY_RADIUS  0.05f          /* base lateral spray amp     */
#define FOUNTAIN_SPRAY_MIN     0.6f           /* min per-droplet r scale    */
#define FOUNTAIN_SPRAY_MAX     1.4f           /* max per-droplet r scale    */
#define FOUNTAIN_SWIRL_RATE    2.0f           /* base rad/s around the axis */
#define FOUNTAIN_BASE_ALPHA    0.6f           /* alpha at origin (t=0)      */

static void render3d_axes_render_fountain_theme(const AxesDrawContext *ctx,
                                                float anim_time, float breath,
                                                float as) {
    float len = AXES_FOUNTAIN_LEN;
    float glow = 0.6f + breath * 0.4f;

    /* Dim axis lines */
    Render3dRgba dim_axes[3] = {
        rgba(0.85f, 0.25f, 0.25f, fminf(0.22f * as, 1.0f)),
        rgba(0.25f, 0.85f, 0.25f, fminf(0.22f * as, 1.0f)),
        rgba(0.25f, 0.25f, 0.85f, fminf(0.22f * as, 1.0f)),
    };
    draw_axis_line_triplet(ctx, len, 1.2f, dim_axes, 1);

    /* Droplet stream. One leader droplet sets the base phase; the rest
     * ride golden-ratio offsets, so the distribution is even and
     * deterministic with no state to carry between frames. */
    Render3dRgba spray[3] = {
        rgba(1.0f,  0.45f, 0.45f, 1.0f),
        rgba(0.45f, 1.0f,  0.45f, 1.0f),
        rgba(0.45f, 0.45f, 1.0f,  1.0f),
    };
    float leader_t = fmodf(anim_time * FOUNTAIN_STREAM_SPEED, 1.0f);

    glPointSize(2.0f);
    glBegin(GL_POINTS);
    for (int axis = 0; axis < 3; axis++) {
        /* Phase offset per axis so the three streams don't sync */
        float axis_phase = (float)axis * 0.333f;
        for (int p = 0; p < FOUNTAIN_DROPLET_COUNT; p++) {
            float fp = (float)p, fa = (float)axis;
            float t = fmodf(leader_t + fp * FOUNTAIN_PHI + axis_phase, 1.0f);

            /* Bright at the origin, out by the apex. Quadratic rather
             * than linear so droplets stay visible into the stretch
             * where the spray has actually spread out. Cheap enough to
             * test before hashing, so invisible droplets cost nothing. */
            float alpha = FOUNTAIN_BASE_ALPHA * (1.0f - t * t) * glow * as;
            if (alpha < 0.01f) continue;
            if (alpha > 1.0f) alpha = 1.0f;

            /* Per-droplet spread and swirl, hashed fresh each frame from
             * (axis, index) alone - same stateless model as the golden-
             * ratio phase above and as grid.c's per-vertex hashing. Two
             * independent channels for angle and radius; the swirl takes
             * a third and splits it, scaling by 97 and re-fracting for
             * the direction bit so rotation sense doesn't correlate with
             * speed does not correlate with the angle or radius channels. */
            float h_angle = render3d_hash01(fp * 1.7f + fa * 19.0f, fa * 3.1f +  5.0f);
            float h_rad   = render3d_hash01(fp * 2.3f + fa * 31.0f, fa * 7.9f + 11.0f);
            float h_swirl = render3d_hash01(fp * 3.9f + fa * 43.0f, fa * 5.3f + 17.0f);

            float radius_scale = FOUNTAIN_SPRAY_MIN
                                 + h_rad * (FOUNTAIN_SPRAY_MAX - FOUNTAIN_SPRAY_MIN);
            float rate = FOUNTAIN_SWIRL_RATE * (0.3f + h_swirl * 1.4f) /* 0.3..1.7 */
                         * ((fmodf(h_swirl * 97.0f, 1.0f) < 0.5f) ? 1.0f : -1.0f);

            /* Axial ease-out, so droplets decelerate as they climb the
             * way a real spray slows toward its apex. The lateral spread
             * front-loads via sqrtf, so the stream is already wider than
             * the gap between droplets while they are still bright - a
             * linear ramp puts the widest spread exactly where alpha
             * reaches 0, which reads as a solid line fading out rather
             * than as a spray. */
            float inv   = 1.0f - t;
            float along = (1.0f - inv * inv) * len;
            float r     = FOUNTAIN_SPRAY_RADIUS * radius_scale * sqrtf(t);
            float ang   = h_angle * (float)M_PI * 2.0f + anim_time * rate;
            float u = cosf(ang) * r, v = sinf(ang) * r;

            axes_color(ctx, spray[axis].r, spray[axis].g, spray[axis].b, alpha);
            switch (axis) {
            case RENDER3D_AXIS_X: glVertex3f(along, u, v); break; /* spread in YZ */
            case RENDER3D_AXIS_Y: glVertex3f(u, along, v); break; /* spread in XZ */
            case RENDER3D_AXIS_Z: glVertex3f(u, v, along); break; /* spread in XY */
            }
        }
    }
    glEnd();
    glPointSize(1.0f);

    /* Emitter dot at the origin */
    glPointSize(5.0f);
    glBegin(GL_POINTS);
    axes_color(ctx, 0.95f, 0.92f, 0.80f, fminf((0.6f + breath * 0.3f) * as, 1.0f));
    glVertex3f(0, 0, 0);
    glEnd();
    glPointSize(1.0f);

    /* Labels */
    Render3dRgba lbl[3] = {
        rgba(0.85f, 0.35f, 0.35f, 1.0f),
        rgba(0.35f, 0.85f, 0.35f, 1.0f),
        rgba(0.35f, 0.35f, 0.85f, 1.0f),
    };
    draw_axis_label_triplet(ctx, len, 0.15f, lbl, "XYZ", 1);
}

void render3d_axes_render(const Render3dFrameRenderContext *frame_ctx) {
    const Render3dRenderConfig *config = &frame_ctx->config;
    Render3dAxesTheme axes_theme = (Render3dAxesTheme)config->axes_theme;
    if (axes_theme == AXES_THEME_OFF) return;

    /* Resolve the transition fade via the shared overlay-xn helper.
     * Axes have no fog-owning themes, so uses_own_fog is always 0. */
    Render3dOverlayXn xn = render3d_overlay_xn_resolve(
        config->axes_opacity, AXES_XN_STYLE, 0,
#if AXES_XN_STYLE == GRID_AXES_XN_FOG
        AXES_XN_FOG_ALPHA_KNEE
#else
        1.0f
#endif
    );
    if (!xn.draw) return;
    AxesDrawContext ctx = { .xn_alpha = xn.alpha };

    render3d_axes_push_state();

    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    render3d_axes_apply_quality_config(config);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

#if AXES_XN_STYLE == GRID_AXES_XN_FOG
    axes_xn_apply_transition_fog(xn.fog_tf, config->clear_color);
#endif

    float breath = sinf(config->anim_time * RENDER3D_BREATH_FREQ) * 0.5f + 0.5f; /* 0..1 */
    float as = config->alpha_scale;

    switch (axes_theme) {
    case AXES_THEME_CLASSIC: render3d_axes_render_classic_theme(&ctx);          break;
    case AXES_THEME_PULSE:   render3d_axes_render_pulse_theme(&ctx, config->anim_time); break;
    case AXES_THEME_NEON:    render3d_axes_render_neon_theme(&ctx, breath, as); break;
    case AXES_THEME_COMPASS: render3d_axes_render_compass_theme(&ctx);          break;
    case AXES_THEME_GIZMO:   render3d_axes_render_gizmo_theme(&ctx, config, as); break;
    case AXES_THEME_RULER:   render3d_axes_render_ruler_theme(&ctx);            break;
    case AXES_THEME_ARROW:   render3d_axes_render_arrow_theme(&ctx);            break;
    case AXES_THEME_FOUNTAIN:
        render3d_axes_render_fountain_theme(&ctx, config->anim_time, breath, as);
        break;
    default:                                                                 break;
    }

    /* render3d_axes_pop_state restores depth/blend/lighting state via
     * GL_ALL_ATTRIB_BITS; no manual teardown needed. */
    render3d_axes_pop_state();
}
