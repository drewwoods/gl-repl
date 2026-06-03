/*
 * axes.c - axes theme rendering
 */
#include "axes.h"
#include "overlay_xn.h"  /* SceneOverlayXn + shared resolve helper */
#include "occluded_ghost.h"  /* SCENE_OCCLUDED_GHOST_STIPPLE */
#include <math.h>            /* sinf, cosf, fmodf, M_PI (via gl_includes.h) */

enum {
    SCENE_AXIS_X = 0,
    SCENE_AXIS_Y = 1,
    SCENE_AXIS_Z = 2,
};

typedef struct AxesThemeSpec {
    float len;
    SceneRgba axis[3];
    SceneRgba label[3];
} AxesThemeSpec;

/* Per-frame axes draw context (audit #3). Resolved once at
 * scene_axes_render entry via scene_overlay_xn_resolve; every
 * axes_color call multiplies through xn_alpha. The struct is
 * deliberately small — axes don't carry the breath/anim_time
 * GridDrawContext has because each per-theme renderer that needs
 * those takes them as separate args. */
typedef struct AxesDrawContext {
    float xn_opacity;
    float xn_alpha;
} AxesDrawContext;

/* AXES_THEME_NEON is intentionally NOT in g_axes_theme_specs[] below.
 * The static spec carries one flat axis/label color per axis; NEON is a
 * per-frame procedural look (a wide dim halo pass + a narrow bright core
 * pass + glowing tip dots, every channel modulated by the breathing
 * `glow` term) that a single SceneRgba triplet cannot express. It is
 * handled inline in scene_axes_render's switch; only its axis length is
 * a plain constant. */
#define AXES_NEON_LEN 2.5f

static void scene_axes_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void scene_axes_pop_state(void) {
    glPopAttrib();
}

static SceneRgba rgba(float r, float g, float b, float a) {
    SceneRgba c = { r, g, b, a };
    return c;
}

/* Axes in-out transition (plans/.../grid-axes-transitions.md rule 4).
 * Resolved once at scene_axes_render entry from config.axes_opacity
 * via the shared scene_overlay_xn_resolve helper (overlay_xn.h),
 * stored on AxesDrawContext, then every color path routes through
 * axes_color so it applies uniformly AFTER each call site's own
 * alpha_scale clamp (rule 3). 1.0 = shown.
 *
 * Axes sit near the origin where distance fog barely bites, so under
 * the FOG style this mostly amounts to the alpha knee with a faint
 * clear-color haze — an intentionally subtle variant (the strong fog
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

static void axes_color_rgba(const AxesDrawContext *ctx, SceneRgba c) {
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
};

static const AxesThemeSpec *axes_theme_spec(SceneAxesTheme theme) {
    if (theme <= AXES_THEME_OFF || theme >= AXES_THEME_COUNT)
        return NULL;
    if (g_axes_theme_specs[theme].len <= 0.0f)
        return NULL;
    return &g_axes_theme_specs[theme];
}

static void scene_axes_apply_quality_config(const SceneRenderConfig *config) {
    if (config->multisample_enabled) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);
    if (config->line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);
}

static void draw_axis_line_triplet(const AxesDrawContext *ctx,
                                   float len, float width,
                                   const SceneRgba colors[3],
                                   int direction) {
    float end = len * (float)direction;
    glLineWidth(width);
    glBegin(GL_LINES);
    axes_color_rgba(ctx, colors[SCENE_AXIS_X]);
    glVertex3f(0, 0, 0); glVertex3f(end, 0, 0);
    axes_color_rgba(ctx, colors[SCENE_AXIS_Y]);
    glVertex3f(0, 0, 0); glVertex3f(0, end, 0);
    axes_color_rgba(ctx, colors[SCENE_AXIS_Z]);
    glVertex3f(0, 0, 0); glVertex3f(0, 0, end);
    glEnd();
    glLineWidth(1.0f);
}

static void draw_axis_tip_triplet(const AxesDrawContext *ctx,
                                  float len, float point_size,
                                  const SceneRgba colors[3],
                                  int direction) {
    float end = len * (float)direction;
    glPointSize(point_size);
    glBegin(GL_POINTS);
    axes_color_rgba(ctx, colors[SCENE_AXIS_X]);
    glVertex3f(end, 0, 0);
    axes_color_rgba(ctx, colors[SCENE_AXIS_Y]);
    glVertex3f(0, end, 0);
    axes_color_rgba(ctx, colors[SCENE_AXIS_Z]);
    glVertex3f(0, 0, end);
    glEnd();
    glPointSize(1.0f);
}

static void draw_axis_label_triplet(const AxesDrawContext *ctx,
                                    float len, float offset,
                                    const SceneRgba colors[3],
                                    const char labels[3],
                                    int direction) {
    float pos = (len + offset) * (float)direction;
    draw_axis_label(ctx, pos, 0, 0, labels[SCENE_AXIS_X],
                    colors[SCENE_AXIS_X].r, colors[SCENE_AXIS_X].g,
                    colors[SCENE_AXIS_X].b);
    draw_axis_label(ctx, 0, pos, 0, labels[SCENE_AXIS_Y],
                    colors[SCENE_AXIS_Y].r, colors[SCENE_AXIS_Y].g,
                    colors[SCENE_AXIS_Y].b);
    draw_axis_label(ctx, 0, 0, pos, labels[SCENE_AXIS_Z],
                    colors[SCENE_AXIS_Z].r, colors[SCENE_AXIS_Z].g,
                    colors[SCENE_AXIS_Z].b);
}

/* --- per-theme axes renderers ---
 *
 * Each theme used to live as a giant inline switch arm in
 * scene_axes_render. Extracted into named static helpers so the
 * dispatcher reads like a table-of-contents and each renderer owns
 * its own ~30-line body. Pattern mirrors grid.c's per-theme functions
 * (audit #52). */

static void scene_axes_render_classic_theme(const AxesDrawContext *ctx) {
    const AxesThemeSpec *spec = axes_theme_spec(AXES_THEME_CLASSIC);
    draw_axis_line_triplet(ctx, spec->len, 2.0f, spec->axis, 1);
    draw_axis_label_triplet(ctx, spec->len, 0.15f, spec->label, "XYZ", 1);
}

static void scene_axes_render_pulse_theme(const AxesDrawContext *ctx,
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

/* Procedural theme — see the AXES_NEON_LEN note at the top of this
 * file for why NEON is the only theme that doesn't sit in the
 * g_axes_theme_specs table. */
static void scene_axes_render_neon_theme(const AxesDrawContext *ctx,
                                         float breath, float as) {
    float len = AXES_NEON_LEN;
    float glow = 0.6f + breath * 0.4f;
    SceneRgba outer[3] = {
        rgba(1.0f, 0.1f, 0.1f, fminf(0.12f * glow * as, 1.0f)),
        rgba(0.1f, 1.0f, 0.1f, fminf(0.12f * glow * as, 1.0f)),
        rgba(0.1f, 0.1f, 1.0f, fminf(0.12f * glow * as, 1.0f)),
    };
    SceneRgba core[3] = {
        rgba(1.0f, 0.4f, 0.4f, 1.0f * glow),
        rgba(0.4f, 1.0f, 0.4f, 1.0f * glow),
        rgba(0.4f, 0.4f, 1.0f, 1.0f * glow),
    };
    SceneRgba tips[3] = {
        rgba(1.0f, 0.5f, 0.5f, glow),
        rgba(0.5f, 1.0f, 0.5f, glow),
        rgba(0.5f, 0.5f, 1.0f, glow),
    };

    /* Outer glow (wide, dim) → core (narrow, bright) → bright tip dots */
    draw_axis_line_triplet(ctx, len, 6.0f, outer, 1);
    draw_axis_line_triplet(ctx, len, 2.0f, core, 1);
    draw_axis_tip_triplet(ctx, len, 6.0f, tips, 1);

    float la = 0.5f + glow * 0.5f;
    SceneRgba labels[3] = {
        rgba(1.0f * la, 0.3f * la, 0.3f * la, 1.0f),
        rgba(0.3f * la, 1.0f * la, 0.3f * la, 1.0f),
        rgba(0.3f * la, 0.3f * la, 1.0f * la, 1.0f),
    };
    draw_axis_label_triplet(ctx, len, 0.15f, labels, "XYZ", 1);
}

static void scene_axes_render_compass_theme(const AxesDrawContext *ctx) {
    const AxesThemeSpec *spec = axes_theme_spec(AXES_THEME_COMPASS);
    float len = spec->len;
    SceneRgba negative_axes[3] = {
        rgba(1.0f, 0.30f, 0.30f, 0.35f),
        rgba(0.30f, 1.0f, 0.30f, 0.35f),
        rgba(0.30f, 0.30f, 1.0f, 0.35f),
    };
    SceneRgba positive_tips[3] = {
        rgba(1.0f, 0.4f, 0.4f, 0.9f),
        rgba(0.4f, 1.0f, 0.4f, 0.9f),
        rgba(0.4f, 0.4f, 1.0f, 0.9f),
    };
    SceneRgba negative_tips[3] = {
        rgba(1.0f, 0.3f, 0.3f, 0.30f),
        rgba(0.3f, 1.0f, 0.3f, 0.30f),
        rgba(0.3f, 0.3f, 1.0f, 0.30f),
    };

    /* Positive axes (solid) */
    draw_axis_line_triplet(ctx, len, 2.0f, spec->axis, 1);

    /* Negative axes (stippled) */
    glEnable(GL_LINE_STIPPLE);
    glLineStipple(2, SCENE_OCCLUDED_GHOST_STIPPLE);
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
    SceneRgba negative_labels[3] = {
        rgba(0.55f, 0.25f, 0.25f, 1.0f),
        rgba(0.25f, 0.55f, 0.25f, 1.0f),
        rgba(0.25f, 0.25f, 0.55f, 1.0f),
    };
    draw_axis_label_triplet(ctx, len, 0.15f, negative_labels, "xyz", -1);
}

static void scene_axes_render_gizmo_theme(const AxesDrawContext *ctx,
                                          const SceneRenderConfig *config,
                                          float as) {
    const AxesThemeSpec *spec = axes_theme_spec(AXES_THEME_GIZMO);
    float len  = spec->len;
    float fill = len / 2.0f;

    /* Camera-facing weight for each vertical plane:
     * XY (z=0) is face-on when camera looks along Z  → weight = cos²(ry)
     * ZY (x=0) is face-on when camera looks along X  → weight = sin²(ry) */
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
static void scene_axes_render_ruler_theme(const AxesDrawContext *ctx) {
    const AxesThemeSpec *spec = axes_theme_spec(AXES_THEME_RULER);
    float len = spec->len;
    draw_axis_line_triplet(ctx, len, 2.0f, spec->axis, 1);

    glBegin(GL_LINES);
    for (int i = 1; i <= (int)len; i++) {
        float t = (i % 5 == 0) ? 0.16f : 0.07f;
        /* X axis: ticks span ±Z */
        axes_color_rgba(ctx, spec->axis[SCENE_AXIS_X]);
        glVertex3f((float)i, 0, -t); glVertex3f((float)i, 0, t);
        /* Y axis: ticks span ±X */
        axes_color_rgba(ctx, spec->axis[SCENE_AXIS_Y]);
        glVertex3f(-t, (float)i, 0); glVertex3f(t, (float)i, 0);
        /* Z axis: ticks span ±X */
        axes_color_rgba(ctx, spec->axis[SCENE_AXIS_Z]);
        glVertex3f(-t, 0, (float)i); glVertex3f(t, 0, (float)i);
    }
    glEnd();

    draw_axis_label_triplet(ctx, len, 0.15f, spec->label, "XYZ", 1);
}

void scene_axes_render(const SceneFrameRenderContext *frame_ctx) {
    const SceneRenderConfig *config = &frame_ctx->config;
    SceneAxesTheme axes_theme = (SceneAxesTheme)config->axes_theme;
    if (axes_theme == AXES_THEME_OFF) return;

    /* Resolve the transition fade via the shared overlay-xn helper.
     * Axes have no fog-owning themes, so uses_own_fog is always 0. */
    SceneOverlayXn xn = scene_overlay_xn_resolve(
        config->axes_opacity, AXES_XN_STYLE, 0,
#if AXES_XN_STYLE == GRID_AXES_XN_FOG
        AXES_XN_FOG_ALPHA_KNEE
#else
        1.0f
#endif
    );
    if (!xn.draw) return;
    AxesDrawContext ctx = { .xn_opacity = xn.opacity, .xn_alpha = xn.alpha };

    scene_axes_push_state();

    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    scene_axes_apply_quality_config(config);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

#if AXES_XN_STYLE == GRID_AXES_XN_FOG
    axes_xn_apply_transition_fog(xn.fog_tf, config->clear_color);
#endif

    float breath = sinf(config->anim_time * SCENE_BREATH_FREQ) * 0.5f + 0.5f; /* 0..1 */
    float as = config->alpha_scale;

    switch (axes_theme) {
    case AXES_THEME_CLASSIC: scene_axes_render_classic_theme(&ctx);          break;
    case AXES_THEME_PULSE:   scene_axes_render_pulse_theme(&ctx, config->anim_time); break;
    case AXES_THEME_NEON:    scene_axes_render_neon_theme(&ctx, breath, as); break;
    case AXES_THEME_COMPASS: scene_axes_render_compass_theme(&ctx);          break;
    case AXES_THEME_GIZMO:   scene_axes_render_gizmo_theme(&ctx, config, as); break;
    case AXES_THEME_RULER:   scene_axes_render_ruler_theme(&ctx);            break;
    default:                                                                 break;
    }

    /* scene_axes_pop_state restores depth/blend/lighting state via
     * GL_ALL_ATTRIB_BITS; no manual teardown needed. */
    scene_axes_pop_state();
}
