/*
 * grid.c - grid theme rendering
 */
#include "grid.h"
#include "overlay_xn.h"  /* SceneOverlayXn + shared resolve helper */
#include <math.h>     /* sinf, cosf, sqrtf, fabsf, fmodf, M_PI (via gl_includes.h) */

#define GRID_LOOP_EPSILON 0.01f
#define GRID_ORIGIN_SKIP_EPSILON 0.01f
#define GRID_PLANE_VISIBILITY_EPSILON 0.01f
#define GRID_FOCUS_CROSSHAIR_HALF_SIZE 0.3f
#define GRID_MINOR_SUBDIVISIONS 5.0f
#define GRID_MAJOR_TOL_FRACTION 0.25f

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
    /* In-out transition (audit #3): formerly file-static g_xn_opacity /
     * g_xn_alpha. Resolved once at scene_grid_render entry via
     * scene_overlay_xn_resolve, then every grid_color call multiplies
     * `a * xn_alpha`. xn_opacity drives the synthetic-fog recede pass
     * under GRID_XN_STYLE == GRID_AXES_XN_FOG. */
    float xn_opacity;
    float xn_alpha;
} GridDrawContext;

typedef struct GridLineColors {
    SceneRgba x_const;
    SceneRgba z_const;
} GridLineColors;

typedef void (*GridLineColorFn)(float v, int is_major,
                                const GridDrawContext *ctx,
                                GridLineColors *out);
typedef SceneRgba (*GridOriginColorFn)(const GridDrawContext *ctx);
typedef void (*GridPassFn)(const GridDrawContext *ctx);

typedef struct GridThemeSpec {
    GridLineColorFn line_color;
    GridOriginColorFn origin_color;
    GridPassFn begin_pass;
    GridPassFn end_pass;
    float origin_line_width;
} GridThemeSpec;

static void scene_grid_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void scene_grid_pop_state(void) {
    glPopAttrib();
}

static SceneRgba rgba(float r, float g, float b, float a) {
    SceneRgba c = { r, g, b, a };
    return c;
}

/* Grid in-out transition (plans/.../grid-axes-transitions.md rule 4).
 * Resolved once at scene_grid_render entry from config.grid_opacity
 * and stored on the GridDrawContext. Every color path routes through
 * grid_color so it applies uniformly, AFTER each call site's own
 * alpha_scale clamp so the controller-owned OUT is the hard ceiling
 * (rule 3). 1.0 = shown.
 *
 * The shared scene_overlay_xn_resolve helper in overlay_xn.h owns the
 * knee math; grid passes its style + GRID_XN_FOG_ALPHA_KNEE and
 * receives back the effective {alpha, opacity, fog_tf, draw}. */

#if GRID_XN_STYLE == GRID_AXES_XN_FOG
#define GRID_XN_FOG_ALPHA_KNEE 0.30f

/* Synthetic recede fog for fog-less themes: a clear-color linear wall
 * pulled in from beyond the grid as the overlay hides (tf = 1 -
 * opacity). tf<=0 -> no fog, continuous with the fogless steady look. */
static void grid_xn_apply_transition_fog(float tf, float extent) {
    if (tf <= 0.0f) return;
    float clear_col[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_col);
    glFogfv(GL_FOG_COLOR, clear_col);
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
    glColor4f(r, g, b, a * ctx->xn_alpha);
}

static void grid_color_rgba(const GridDrawContext *ctx, SceneRgba c) {
    grid_color(ctx, c.r, c.g, c.b, c.a);
}

static void grid_line_colors_same(GridLineColors *out, SceneRgba color) {
    out->x_const = color;
    out->z_const = color;
}

static void draw_grid_line_pair(float v, const GridDrawContext *ctx,
                                GridLineColors colors) {
    grid_color_rgba(ctx, colors.x_const);
    glVertex3f(v, 0, -ctx->extent);
    glVertex3f(v, 0,  ctx->extent);
    grid_color_rgba(ctx, colors.z_const);
    glVertex3f(-ctx->extent, 0, v);
    glVertex3f( ctx->extent, 0, v);
}

static void draw_grid_origin_axes(const GridDrawContext *ctx, SceneRgba color,
                                  float line_width) {
    glDepthMask(GL_TRUE);
    if (line_width != 1.0f)
        glLineWidth(line_width);
    glBegin(GL_LINES);
    grid_color_rgba(ctx, color);
    glVertex3f(-ctx->extent, 0, 0);
    glVertex3f( ctx->extent, 0, 0);
    glVertex3f(0, 0, -ctx->extent);
    glVertex3f(0, 0,  ctx->extent);
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

    SceneRgba origin_c = spec->origin_color(ctx);
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

static SceneRgba grid_classic_origin_color(const GridDrawContext *ctx) {
    (void)ctx;
    return rgba(0.50f, 0.50f, 0.60f, 0.45f);
}

static void grid_fog_begin(const GridDrawContext *ctx) {
    float fog_density = 0.06f + ctx->breath * 0.04f;
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_EXP2);
    glFogf(GL_FOG_DENSITY, fog_density);
}

static void grid_fog_end(const GridDrawContext *ctx) {
    (void)ctx;
    glDisable(GL_FOG);
}

static void grid_fog_line_color(float v, int is_major,
                                const GridDrawContext *ctx,
                                GridLineColors *out) {
    (void)v;
    (void)ctx;
    grid_line_colors_same(out, rgba(0.45f, 0.50f, 0.65f,
                                    is_major ? 0.25f : 0.10f));
}

static SceneRgba grid_fog_origin_color(const GridDrawContext *ctx) {
    (void)ctx;
    return rgba(0.45f, 0.50f, 0.65f, 0.55f);
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

static SceneRgba grid_tron_origin_color(const GridDrawContext *ctx) {
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

static SceneRgba grid_ember_origin_color(const GridDrawContext *ctx) {
    (void)ctx;
    float ripple0 = -sinf(ctx->anim_time * 2.5f) * 0.5f + 0.5f;
    return rgba(0.95f, 0.35f + ripple0 * 0.25f, 0.05f,
                0.7f * (0.6f + ripple0 * 0.4f));
}

static void grid_faint_line_color(float v, int is_major,
                                  const GridDrawContext *ctx,
                                  GridLineColors *out) {
    (void)v;
    (void)ctx;
    grid_line_colors_same(out, rgba(0.50f, 0.50f, 0.60f,
                                    is_major ? 0.07f : 0.03f));
}

static SceneRgba grid_faint_origin_color(const GridDrawContext *ctx) {
    (void)ctx;
    return rgba(0.50f, 0.50f, 0.60f, 0.18f);
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

static SceneRgba grid_aurora_origin_color(const GridDrawContext *ctx) {
    float s0 = sinf(ctx->anim_time * 0.7f) * 0.5f + 0.5f;
    return rgba(0.50f, 0.80f + 0.10f * s0, 0.95f, 0.50f);
}

static void grid_aurora_end_pass(const GridDrawContext *ctx) {
    const float extent = ctx->extent;
    const float t      = ctx->anim_time;
    const float as     = ctx->alpha_scale;
    const int   steps  = 64;

    /* Additive blend so overlapping folds brighten like real curtains.
     * Depth writes are already off for the whole grid pass, and
     * scene_grid_pop_state restores the blend func afterwards. */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    for (int c = 0; c < 2; c++) {
        float ph       = (float)c * 2.6f;
        float z0       = (c == 0 ? -0.55f : -0.30f) * extent;
        float h_base   = extent * (0.35f + 0.08f * (float)c);
        float h_height = extent * (0.30f - 0.06f * (float)c);
        glBegin(GL_TRIANGLE_STRIP);
        for (int i = 0; i <= steps; i++) {
            float x = -extent + 2.0f * extent * (float)i / (float)steps;
            /* meandering path in z, two octaves */
            float z = z0
                + sinf(x * 0.22f + t * 0.18f + ph) * extent * 0.16f
                + sinf(x * 0.06f - t * 0.11f + ph * 1.7f) * extent * 0.09f;
            /* curtain folds: drive both height and brightness */
            float fold = sinf(x * 1.3f + t * 0.9f + ph) * 0.35f
                       + sinf(x * 2.9f - t * 1.4f + ph * 0.6f) * 0.20f;
            float fold01 = fold + 0.5f;
            if (fold01 < 0.0f) fold01 = 0.0f;
            if (fold01 > 1.0f) fold01 = 1.0f;
            /* soft ends so the ribbon has no hard border */
            float ex = x / extent;
            float edge = 1.0f - ex * ex * ex * ex;
            if (edge < 0.0f) edge = 0.0f;
            float y_lo = h_base
                + sinf(x * 0.5f + t * 0.4f + ph) * extent * 0.05f;
            float y_hi = y_lo + h_height * (0.65f + 0.35f * fold01);
            /* base: bright aurora green */
            grid_color(ctx, 0.18f, 0.95f, 0.50f,
                       fminf(0.30f * (0.45f + 0.55f * fold01) * edge * as,
                             1.0f));
            glVertex3f(x, y_lo, z);
            /* top: violet fringe fading to nothing */
            grid_color(ctx, 0.50f, 0.25f, 0.90f, 0.0f);
            glVertex3f(x, y_hi, z);
        }
        glEnd();
    }
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

static SceneRgba grid_synthwave_origin_color(const GridDrawContext *ctx) {
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
    [GRID_THEME_FOG] = {
        grid_fog_line_color, grid_fog_origin_color, grid_fog_begin,
        grid_fog_end, 1.0f
    },
    [GRID_THEME_TRON] = {
        grid_tron_line_color, grid_tron_origin_color, NULL, NULL, 2.0f
    },
    [GRID_THEME_EMBER] = {
        grid_ember_line_color, grid_ember_origin_color, NULL, NULL, 1.0f
    },
    [GRID_THEME_FAINT] = {
        grid_faint_line_color, grid_faint_origin_color, NULL, NULL, 1.0f
    },
    [GRID_THEME_AURORA] = {
        grid_aurora_line_color, grid_aurora_origin_color, NULL,
        grid_aurora_end_pass, 1.0f
    },
    [GRID_THEME_SYNTHWAVE] = {
        grid_synthwave_line_color, grid_synthwave_origin_color, NULL, NULL,
        2.0f
    },
};

static const GridThemeSpec *grid_theme_spec(SceneGridTheme theme) {
    if (theme <= GRID_THEME_OFF || theme >= GRID_THEME_COUNT)
        return NULL;
    if (!g_grid_theme_specs[theme].line_color)
        return NULL;
    return &g_grid_theme_specs[theme];
}

static void scene_grid_apply_quality_config(const SceneRenderConfig *config) {
    if (config->multisample_enabled) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);
    if (config->line_smooth_enabled) glEnable(GL_LINE_SMOOTH);
    else glDisable(GL_LINE_SMOOTH);
}

static void scene_grid_render_focus_theme(const SceneFrameRenderContext *frame_ctx,
                                         const GridDrawContext *grid_ctx) {
    const SceneFocusVertex *focus = &frame_ctx->config.focus;
    float cx = focus->pos[0], cz = focus->pos[2];
    float radius = 3.0f;  /* fade-out radius */
    float as = grid_ctx->alpha_scale;

    glBegin(GL_LINES);
    for (float v = -grid_ctx->extent; v <= grid_ctx->extent + GRID_LOOP_EPSILON;
         v += grid_ctx->step) {
        if (fabsf(v) < GRID_ORIGIN_SKIP_EPSILON) continue;
        int is_major = grid_is_major_line(v, grid_ctx->major,
                                          grid_ctx->major_tol);
        float base = is_major ? 0.18f : 0.06f;

        /* Vertical line at x=v: fade based on distance from cx */
        float dx = v - cx;
        float fx = 1.0f - (dx * dx) / (radius * radius);
        if (fx < 0.0f) fx = 0.0f;
        fx = fx * fx;  /* sharper falloff */
        if (fx > 0.001f) {
            grid_color(grid_ctx, 0.50f, 0.55f, 0.70f, fminf(base * fx * as, 1.0f));
            /* Clamp line Z extent around focus */
            float z0 = cz - radius, z1 = cz + radius;
            if (z0 < -grid_ctx->extent) z0 = -grid_ctx->extent;
            if (z1 > grid_ctx->extent) z1 = grid_ctx->extent;
            glVertex3f(v, 0, z0); glVertex3f(v, 0, z1);
        }

        /* Horizontal line at z=v: fade based on distance from cz */
        float dz = v - cz;
        float fz = 1.0f - (dz * dz) / (radius * radius);
        if (fz < 0.0f) fz = 0.0f;
        fz = fz * fz;
        if (fz > 0.001f) {
            grid_color(grid_ctx, 0.50f, 0.55f, 0.70f, fminf(base * fz * as, 1.0f));
            float x0 = cx - radius, x1 = cx + radius;
            if (x0 < -grid_ctx->extent) x0 = -grid_ctx->extent;
            if (x1 > grid_ctx->extent) x1 = grid_ctx->extent;
            glVertex3f(x0, 0, v); glVertex3f(x1, 0, v);
        }
    }
    glEnd();

    /* Crosshair at focus point */
    if (focus->valid) {
        glLineWidth(1.5f);
        glBegin(GL_LINES);
        grid_color(grid_ctx, 0.80f, 0.85f, 0.95f, fminf(0.25f * as, 1.0f));
        glVertex3f(cx - GRID_FOCUS_CROSSHAIR_HALF_SIZE, 0, cz);
        glVertex3f(cx + GRID_FOCUS_CROSSHAIR_HALF_SIZE, 0, cz);
        glVertex3f(cx, 0, cz - GRID_FOCUS_CROSSHAIR_HALF_SIZE);
        glVertex3f(cx, 0, cz + GRID_FOCUS_CROSSHAIR_HALF_SIZE);
        glEnd();
        glLineWidth(1.0f);
    }
}

static void scene_grid_render_ocean_theme(const GridDrawContext *grid_ctx,
                                          const SceneFrameRenderContext *frame_ctx,
                                          float breath) {
    const float extent = grid_ctx->extent;
    const float major = grid_ctx->major;
    const float major_tol = grid_ctx->major_tol;
    const float step = grid_ctx->step;
    const SceneRenderConfig *config = &frame_ctx->config;
    float camera_rx_rad = config->cam_rx * (float)M_PI / 180.0f;
    float camera_world_y = config->cam_ty + sinf(camera_rx_rad) * config->cam_dist;

    /* Underwater fog - slightly breathing density */
    if (camera_world_y < 0.0f) {
        /* Fill the active scene viewport with a teal tint. Coordinates
         * use scene_w/scene_h (not the full window viewport) so the
         * rect lines up with whatever glViewport scene_render set. */
        grid_color(grid_ctx, 0.05f, 0.25f, 0.35f, 0.75f);
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        gluOrtho2D(0, config->scene_w, 0, config->scene_h);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        /* GL_FOG_BIT is on the push mask because the outer grid pass
         * leaves fog enabled (linear, end ≈ grid extent) — and after
         * fb976f0 it may also have GL_FOG_DISTANCE_MODE_NV =
         * GL_EYE_RADIAL_NV set when the OCEAN theme is the dispatch
         * branch. With identity modelview the rect's eye-space radial
         * distances run 0..sqrt(scene_w² + scene_h²), wildly past
         * fog-end, so without the disable the rect fades to fog colour
         * everywhere except the tiny lower-left near (0,0). See
         * tests/test_scene_underwater_fill_gl.c. */
        glPushAttrib(GL_DEPTH_BUFFER_BIT | GL_LIGHTING_BIT | GL_FOG_BIT);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
        glDisable(GL_FOG);
        glRectf(0, 0, (float)config->scene_w, (float)config->scene_h);
        glPopAttrib();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
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
     * on but depth-write is off (set at the top of scene_grid_render), so the
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
                grid_color(grid_ctx, 0.05f + cr, 0.25f + cg, 0.35f + cr, alpha);
                glVertex3f(sx, y, zz);
            }
        }
        glEnd();
    }
}

static void scene_grid_render_xzruler_theme(const GridDrawContext *grid_ctx) {
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

    /* Origin axes - bright, wider */
    glDepthMask(GL_TRUE);
    glLineWidth(2.0f);
    glBegin(GL_LINES);
    /* X axis (z=0, runs along X) */
    grid_color(grid_ctx, 0.88f, 0.28f, 0.12f, 0.70f);
    glVertex3f(-extent, 0, 0); glVertex3f(extent, 0, 0);
    /* Z axis (x=0, runs along Z) */
    grid_color(grid_ctx, 0.12f, 0.32f, 0.88f, 0.70f);
    glVertex3f(0, 0, -extent); glVertex3f(0, 0, extent);
    glEnd();
    glLineWidth(1.0f);
    glDepthMask(GL_FALSE);

    /* Ruler tick marks at major-line intervals on both axes */
    float tick = 0.06f;
    glBegin(GL_LINES);
    for (float v = -extent; v <= extent + GRID_LOOP_EPSILON; v += major) {
        if (fabsf(v) < GRID_ORIGIN_SKIP_EPSILON) continue;
        float ta = (fabsf(v) <= major * 2.5f) ? 0.48f : 0.22f;
        ta = fminf(ta * as, 1.0f);
        /* Ticks crossing the X axis in the Z direction */
        grid_color(grid_ctx, 0.88f, 0.28f, 0.12f, ta);
        glVertex3f(v, 0, -tick); glVertex3f(v, 0, tick);
        /* Ticks crossing the Z axis in the X direction */
        grid_color(grid_ctx, 0.12f, 0.32f, 0.88f, ta);
        glVertex3f(-tick, 0, v); glVertex3f(tick, 0, v);
    }
    glEnd();
}

static void scene_grid_render_planes_theme(const SceneRenderConfig *config,
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
static void scene_grid_render_radar_theme(const GridDrawContext *grid_ctx) {
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

int scene_grid_theme_uses_fog(SceneGridTheme grid_theme) {
    return grid_theme == GRID_THEME_FOG ||
           grid_theme == GRID_THEME_OCEAN;
}

/* --- scene_grid_render phases ---
 *
 * Splits the 150-line orchestrator into:
 *   - grid_xn_resolve(): per-frame transition fade via the shared
 *     scene_overlay_xn_resolve. Returns a SceneOverlayXn the caller
 *     stamps onto the GridDrawContext.
 *   - grid_setup_blend_depth(): the GL state baseline for grid draws.
 *   - grid_build_draw_context(): clamps extent/major indices and
 *     fills the GridDrawContext.
 *   - grid_apply_far_fog(): the FAR-extent distance fog + the
 *     FOG-transition recede injection for non-far themes.
 *   - grid_dispatch_theme(): the switch over SceneGridTheme.
 *
 * scene_grid_render is now the sequencer of these phases, and the
 * xn fields live on GridDrawContext rather than as file statics
 * (audit #3). */

/* Resolve grid transition fade via the shared overlay-xn helper. The
 * grid's "this theme owns fog" carve-out (EXP2-fog themes — FOG and
 * OCEAN — fall back to plain alpha FADE so their fog isn't competing
 * with the synthetic LINEAR recede) is keyed on
 * scene_grid_theme_uses_fog(). */
static SceneOverlayXn grid_xn_resolve(const SceneRenderConfig *config,
                                      SceneGridTheme grid_theme) {
#if GRID_XN_STYLE == GRID_AXES_XN_FOG
    int uses_fog = scene_grid_theme_uses_fog(grid_theme);
    float knee   = GRID_XN_FOG_ALPHA_KNEE;
#else
    (void)grid_theme;
    int uses_fog = 0;
    float knee   = 1.0f; /* not used in FADE style; keeps the helper pure */
#endif
    return scene_overlay_xn_resolve(config->grid_opacity,
                                    GRID_XN_STYLE,
                                    uses_fog, knee);
}

static void grid_setup_blend_depth(const SceneRenderConfig *config) {
    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    scene_grid_apply_quality_config(config);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static GridDrawContext grid_build_draw_context(const SceneFrameRenderContext *frame_ctx) {
    const SceneRenderConfig *config = &frame_ctx->config;
    float breath = sinf(config->anim_time * SCENE_BREATH_FREQ) * 0.5f + 0.5f;
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
    };
}

/* FAR-extent clear-color distance fog. Under FOG transition style,
 * fog-less themes animate the same fog inward as they hide. The
 * xn parameter carries the resolved fog_tf (= 1 - opacity) so this
 * helper doesn't reach back to the renderer's draw context for the
 * value. */
static void grid_apply_far_fog(const SceneRenderConfig *config,
                               SceneGridTheme grid_theme, float extent,
                               const SceneOverlayXn *xn) {
    int is_far = (config->grid_extent_idx == GRID_EXTENT_FAR);
#if GRID_XN_STYLE == GRID_AXES_XN_FOG
    /* Fog-less, non-FAR themes: recede into a synthesized clear-color
     * fog as the overlay hides. At FAR the recede is driven by the
     * FAR block's own fog (below) instead, so it isn't double-set /
     * overwritten. Fog-owning configs took the FADE fallback above. */
    int uses_fog = scene_grid_theme_uses_fog(grid_theme);
    if (!uses_fog && !is_far)
        grid_xn_apply_transition_fog(xn->fog_tf, extent);
#else
    (void)grid_theme;
#endif

    if (!is_far) return;

    /* Steady: (0.7e .. e). tf=0 is the steady look (no pop); tf=1 is
     * a tight recede wall near the camera. */
    float fog_start = extent * 0.7f;
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
    float clear_col[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_col);
    glFogfv(GL_FOG_COLOR, clear_col);
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
static void grid_dispatch_theme(const SceneFrameRenderContext *frame_ctx,
                                const GridDrawContext *grid_ctx,
                                SceneGridTheme grid_theme,
                                int set_nv_fog) {
    const SceneRenderConfig *config = &frame_ctx->config;
    switch (grid_theme) {

    case GRID_THEME_FOCUS:
        scene_grid_render_focus_theme(frame_ctx, grid_ctx);
        break;

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
        scene_grid_render_ocean_theme(grid_ctx, frame_ctx, grid_ctx->breath);
        break;

    case GRID_THEME_XZRULER:
        scene_grid_render_xzruler_theme(grid_ctx);
        break;

    case GRID_THEME_PLANES:
        scene_grid_render_planes_theme(config, grid_ctx);
        break;

    case GRID_THEME_RADAR:
        /* Same radial-fog opt-in as Ocean (see above): the radar rings
         * read the shared FAR-extent distance fog, which swims at the
         * fringes under the eye-plane default. */
        if (set_nv_fog)
            glFogi(GL_FOG_DISTANCE_MODE_NV, GL_EYE_RADIAL_NV);
        scene_grid_render_radar_theme(grid_ctx);
        break;

    default: {
        /* GRID_THEME_CLASSIC, _FOG, _TRON, _EMBER, _FAINT and any
         * future standard theme: look up its GridThemeSpec and draw
         * through the table-driven path. */
        const GridThemeSpec *spec = grid_theme_spec(grid_theme);
        if (spec)
            draw_grid_standard_theme(grid_ctx, spec);
        break;
    }
    }
}

void scene_grid_render(const SceneFrameRenderContext *frame_ctx) {
    const SceneRenderConfig *config = &frame_ctx->config;
    SceneGridTheme grid_theme = (SceneGridTheme)config->grid_theme;
    if (grid_theme == GRID_THEME_OFF) return;

    SceneOverlayXn xn = grid_xn_resolve(config, grid_theme);
    if (!xn.draw) return;

    scene_grid_push_state();
    grid_setup_blend_depth(config);

    /* Nudge grid slightly below Y=0 to avoid z-fighting with axes */
    glPushMatrix();
    glTranslatef(0, -0.002f, 0);

    GridDrawContext grid_ctx = grid_build_draw_context(frame_ctx);
    grid_ctx.xn_opacity = xn.opacity;
    grid_ctx.xn_alpha   = xn.alpha;
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
    /* scene_grid_pop_state restores GL_ALL_ATTRIB_BITS, covering
     * GL_DEPTH_BUFFER_BIT (depth mask), GL_COLOR_BUFFER_BIT (blend),
     * GL_FOG_BIT, and GL_LIGHTING_BIT — no manual teardown needed. */
    scene_grid_pop_state();
}
