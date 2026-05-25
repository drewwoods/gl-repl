/*
 * grid.c - grid theme rendering
 */
#include "grid.h"
#include "config.h"   /* GRID_XN_STYLE / GRID_AXES_XN_* */
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
 * Set once at scene_grid_render entry from config.grid_opacity; every
 * color path in this file routes through gl_color so it applies
 * uniformly, AFTER each call site's own alpha_scale clamp so the
 * controller-owned OUT is the hard ceiling (rule 3). 1.0 = shown.
 *
 * g_xn_opacity is the raw machine opacity; g_xn_alpha is the effective
 * color-alpha multiplier, which differs from opacity only under the
 * compile-time GRID_AXES_XN_FOG style (see config.h): there the alpha
 * stays at 1 until opacity drops past a knee, so the fog carries the
 * recede look and the alpha only guarantees a full vanish at the end. */
static float g_xn_opacity = 1.0f;
static float g_xn_alpha   = 1.0f;

/* FOG-style transition (config.h GRID_XN_STYLE). Only fog-less themes
 * recede into a synthesized clear-color fog as they hide. Themes that
 * draw their own fog (GRID_THEME_FOG, OCEAN) or *any* theme at the FAR
 * extent fall back to the plain alpha FADE for the transition (no
 * recede fog) — far simpler, and it sidesteps the discontinuity of
 * trying to blend two fog models. scene_grid_render decides per pass
 * which path applies and sets g_xn_alpha accordingly (alpha knee for
 * the recede, plain opacity for FADE / the FADE fallback). */

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

static void gl_color(float r, float g, float b, float a) {
    glColor4f(r, g, b, a * g_xn_alpha);
}

static void gl_color_rgba(SceneRgba c) {
    gl_color(c.r, c.g, c.b, c.a);
}

static void grid_line_colors_same(GridLineColors *out, SceneRgba color) {
    out->x_const = color;
    out->z_const = color;
}

static void draw_grid_line_pair(float v, const GridDrawContext *ctx,
                                GridLineColors colors) {
    gl_color_rgba(colors.x_const);
    glVertex3f(v, 0, -ctx->extent);
    glVertex3f(v, 0,  ctx->extent);
    gl_color_rgba(colors.z_const);
    glVertex3f(-ctx->extent, 0, v);
    glVertex3f( ctx->extent, 0, v);
}

static void draw_grid_origin_axes(const GridDrawContext *ctx, SceneRgba color,
                                  float line_width) {
    glDepthMask(GL_TRUE);
    if (line_width != 1.0f)
        glLineWidth(line_width);
    glBegin(GL_LINES);
    gl_color_rgba(color);
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
            gl_color(0.50f, 0.55f, 0.70f, fminf(base * fx * as, 1.0f));
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
            gl_color(0.50f, 0.55f, 0.70f, fminf(base * fz * as, 1.0f));
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
        gl_color(0.80f, 0.85f, 0.95f, fminf(0.25f * as, 1.0f));
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
        gl_color(0.05f, 0.25f, 0.35f, 0.75f);
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        gluOrtho2D(0, config->scene_w, 0, config->scene_h);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        /* Push BEFORE mutating depth/lighting so pop restores the
         * caller's state — formerly disable-before-push meant pop
         * restored "disabled" and a manual glEnable(GL_DEPTH_TEST)
         * after the pop clobbered the outer state instead of restoring
         * it. */
        glPushAttrib(GL_DEPTH_BUFFER_BIT | GL_LIGHTING_BIT);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
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
        gl_color(r, g, b, a);
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
        gl_color(r_o, g_o, b_o, a_o);
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
                gl_color(0.05f + cr, 0.25f + cg, 0.35f + cr, alpha);
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
    gl_color(0.88f, 0.28f, 0.12f, 0.70f);
    glVertex3f(-extent, 0, 0); glVertex3f(extent, 0, 0);
    /* Z axis (x=0, runs along Z) */
    gl_color(0.12f, 0.32f, 0.88f, 0.70f);
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
        gl_color(0.88f, 0.28f, 0.12f, ta);
        glVertex3f(v, 0, -tick); glVertex3f(v, 0, tick);
        /* Ticks crossing the Z axis in the X direction */
        gl_color(0.12f, 0.32f, 0.88f, ta);
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
        gl_color(0.50f, 0.52f, 0.65f, a);
        glVertex3f(v,       0, -extent); glVertex3f(v,      0, extent);
        glVertex3f(-extent, 0, v);       glVertex3f(extent, 0, v);
    }
    glEnd();
    /* Floor origin axes - write to depth buffer */
    glDepthMask(GL_TRUE);
    glBegin(GL_LINES);
    gl_color(0.50f, 0.52f, 0.65f, fminf(0.30f * as, 1.0f));
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
            gl_color(0.35f, 0.62f, 0.88f, a);
            glVertex3f(-extent, v, 0); glVertex3f(extent, v, 0);
            gl_color(0.35f, 0.62f, 0.88f, a * 0.75f);
            glVertex3f(v, -extent, 0); glVertex3f(v, extent, 0);
        }
        glEnd();
        /* XY plane origin axes - write to depth buffer */
        glDepthMask(GL_TRUE);
        glBegin(GL_LINES);
        gl_color(0.35f, 0.62f, 0.88f, fminf(0.42f * xy_w * as, 1.0f));
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
            gl_color(0.82f, 0.52f, 0.28f, a);
            glVertex3f(0, v, -extent); glVertex3f(0, v, extent);
            gl_color(0.82f, 0.52f, 0.28f, a * 0.75f);
            glVertex3f(0, -extent, v); glVertex3f(0, extent, v);
        }
        glEnd();
        /* ZY plane origin axes - write to depth buffer */
        glDepthMask(GL_TRUE);
        glBegin(GL_LINES);
        gl_color(0.82f, 0.52f, 0.28f, fminf(0.42f * zy_w * as, 1.0f));
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
        gl_color(GR, GG, GB, fminf(0.06f * as, 1.0f));
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < SEG; i++) {
            float th = (float)i / (float)SEG * TAU;
            glVertex3f(r * cosf(th), 0.0f, r * sinf(th));
        }
        glEnd();
    }

    glLineWidth(1.5f);
    glBegin(GL_LINES);
    gl_color(GR, GG, GB, fminf(0.07f * as, 1.0f));
    glVertex3f(-extent, 0, 0); glVertex3f(extent, 0, 0);
    glVertex3f(0, 0, -extent); glVertex3f(0, 0, extent);
    glEnd();

    /* Expanding ping, very faint, fades as it grows. */
    float pr = fmodf(t * 0.45f, 1.0f) * extent;
    gl_color(GR, GG, GB, fminf((1.0f - pr / extent) * 0.10f * as, 1.0f));
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < SEG; i++) {
        float th = (float)i / (float)SEG * TAU;
        glVertex3f(pr * cosf(th), 0.0f, pr * sinf(th));
    }
    glEnd();

    /* Single faint sweep line. */
    float ang = fmodf(t * 0.8f, TAU);
    glBegin(GL_LINES);
    gl_color(GR, GG, GB, fminf(0.12f * as, 1.0f));
    glVertex3f(0.0f, 0.0f, 0.0f);
    glVertex3f(extent * cosf(ang), 0.0f, extent * sinf(ang));
    glEnd();
}

int scene_grid_theme_uses_fog(SceneGridTheme grid_theme) {
    return grid_theme == GRID_THEME_FOG ||
           grid_theme == GRID_THEME_OCEAN;
}

void scene_grid_render(const SceneFrameRenderContext *frame_ctx) {
    const SceneRenderConfig *config = &frame_ctx->config;
    SceneGridTheme grid_theme = (SceneGridTheme)config->grid_theme;
    if (grid_theme == GRID_THEME_OFF) return;

    /* Transition fade: clamp defensively, then every gl_color in this
     * file multiplies through it (rule 4). Skip drawing entirely once
     * fully faded out so a 0-opacity pass costs nothing. */
    g_xn_opacity = scene_clamp01f(config->grid_opacity);
    if (g_xn_opacity <= 0.0f) return;
#if GRID_XN_STYLE == GRID_AXES_XN_FOG
    /* The EXP2-fog themes (FOG/OCEAN) fall back to the plain alpha
     * FADE: their fog is left as-is and the geometry just alpha-fades,
     * since their EXP2 fog can't blend continuously with the LINEAR
     * recede. Every other theme — including any theme at the FAR
     * extent, whose LINEAR distance fog composes fine — gets the
     * synthesized clear-color recede + alpha knee. */
    int xn_uses_fog = scene_grid_theme_uses_fog(grid_theme);
    if (xn_uses_fog)
        g_xn_alpha = g_xn_opacity;                       /* FADE fallback */
    else if (g_xn_opacity >= GRID_XN_FOG_ALPHA_KNEE)
        g_xn_alpha = 1.0f;                               /* fog does the work */
    else
        g_xn_alpha = g_xn_opacity / GRID_XN_FOG_ALPHA_KNEE;  /* final vanish */
#else
    g_xn_alpha = g_xn_opacity;
#endif

    scene_grid_push_state();

    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    scene_grid_apply_quality_config(config);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Nudge grid slightly below Y=0 to avoid z-fighting with axes */
    glPushMatrix();
    glTranslatef(0, -0.002f, 0);

    float breath = sinf(frame_ctx->config.anim_time * SCENE_BREATH_FREQ) * 0.5f + 0.5f; /* 0..1 */

    /* Configurable extent / major-tick spacing. Minor step is the
     * major cell divided into 5 subdivisions, which keeps the look
     * consistent across the {1, 2, 5, 10} major options. */
    int ex_i = config->grid_extent_idx;
    if (ex_i < 0 || ex_i >= GRID_EXTENT_COUNT) ex_i = GRID_EXTENT_MID;
    int mj_i = config->grid_major_idx;
    if (mj_i < 0 || mj_i >= GRID_MAJOR_COUNT) mj_i = GRID_MAJOR_1;
    float extent = frame_ctx->config.grid_extents[ex_i];
    float major  = frame_ctx->config.grid_major_steps[mj_i];
    float step   = major / GRID_MINOR_SUBDIVISIONS;
    float major_tol = step * GRID_MAJOR_TOL_FRACTION;
    GridDrawContext grid_ctx = {
        .extent = extent,
        .major = major,
        .step = step,
        .major_tol = major_tol,
        .anim_time = frame_ctx->config.anim_time,
        .breath = breath,
        .alpha_scale = config->alpha_scale,
    };

    int is_far = (config->grid_extent_idx == GRID_EXTENT_FAR);

#if GRID_XN_STYLE == GRID_AXES_XN_FOG
    /* Fog-less, non-FAR themes: recede into a synthesized clear-color
     * fog as the overlay hides. At FAR the recede is driven by the
     * FAR block's own fog (below) instead, so it isn't double-set /
     * overwritten. Fog-owning configs took the FADE fallback above. */
    if (!xn_uses_fog && !is_far)
        grid_xn_apply_transition_fog(1.0f - g_xn_opacity, extent);
#endif

    if (is_far) {
        /* Clear-color linear distance fog. Steady: (0.7e .. e). Under
         * the FOG transition style a fog-less theme animates the same
         * fog inward as it hides — same LINEAR model, so tf=0 is
         * exactly the steady look (no pop) and tf=1 is a tight recede
         * wall near the camera. */
        float fog_start = extent * 0.7f;
        float fog_end   = extent;
#if GRID_XN_STYLE == GRID_AXES_XN_FOG
        if (!xn_uses_fog) {
            float tf = 1.0f - g_xn_opacity;       /* 0 shown .. 1 hidden */
            fog_end   = extent + (extent * 0.05f - extent) * tf;
            fog_start = fog_end * 0.7f;
        }
#endif
        float clear_col[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_col);
        glFogfv(GL_FOG_COLOR, clear_col);
        glEnable(GL_FOG);
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogf(GL_FOG_START, fog_start);
        glFogf(GL_FOG_END, fog_end);
    }

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

    /* Custom themes handle their own draw path; the default arm covers
     * every standard theme by spec-table lookup, so adding/removing a
     * GridThemeSpec entry is a one-edit change instead of two parallel
     * lists. */
    switch (grid_theme) {

    case GRID_THEME_FOCUS:
        scene_grid_render_focus_theme(frame_ctx, &grid_ctx);
        break;

    case GRID_THEME_OCEAN:
        /* Opt into radial eye-distance fog when available, so the fog
         * closes in by true distance rather than eye-plane depth and the
         * fringes stop swimming as the camera orbits. */
        if (set_nv_fog)
            glFogi(GL_FOG_DISTANCE_MODE_NV, GL_EYE_RADIAL_NV);
        scene_grid_render_ocean_theme(&grid_ctx, frame_ctx, breath);
        break;

    case GRID_THEME_XZRULER:
        scene_grid_render_xzruler_theme(&grid_ctx);
        break;

    case GRID_THEME_PLANES:
        scene_grid_render_planes_theme(config, &grid_ctx);
        break;

    case GRID_THEME_RADAR:
        /* Same radial-fog opt-in as Ocean (see above): the radar rings
         * read the shared FAR-extent distance fog, which swims at the
         * fringes under the eye-plane default. */
        if (set_nv_fog)
            glFogi(GL_FOG_DISTANCE_MODE_NV, GL_EYE_RADIAL_NV);
        scene_grid_render_radar_theme(&grid_ctx);
        break;

    default: {
        /* GRID_THEME_CLASSIC, _FOG, _TRON, _EMBER, _FAINT and any
         * future standard theme: look up its GridThemeSpec and draw
         * through the table-driven path. */
        const GridThemeSpec *spec = grid_theme_spec(grid_theme);
        if (spec)
            draw_grid_standard_theme(&grid_ctx, spec);
        break;
    }
    }

    if (set_nv_fog)
        glFogi(GL_FOG_DISTANCE_MODE_NV, saved_nv_fog_mode);

    glPopMatrix();
    /* scene_grid_pop_state restores GL_ALL_ATTRIB_BITS, covering
     * GL_DEPTH_BUFFER_BIT (depth mask), GL_COLOR_BUFFER_BIT (blend),
     * GL_FOG_BIT, and GL_LIGHTING_BIT — no manual teardown needed. */
    scene_grid_pop_state();
}
