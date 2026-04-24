/*
 * scene_axes.c — axes theme rendering
 */
#include "sample.h"
#include "scene_axes.h"

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

static void gl_color_rgba(SceneRgba c) {
    glColor4f(c.r, c.g, c.b, c.a);
}

/* Helper: draw an axis label at a 3D position */
static void draw_axis_label(float x, float y, float z, char ch,
                            float r, float g, float b) {
    glColor3f(r, g, b);
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
};

static const AxesThemeSpec *axes_theme_spec(AxesTheme theme) {
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

static void draw_axis_line_triplet(float len, float width,
                                   const SceneRgba colors[3],
                                   int direction) {
    float end = len * (float)direction;
    glLineWidth(width);
    glBegin(GL_LINES);
    gl_color_rgba(colors[SCENE_AXIS_X]);
    glVertex3f(0, 0, 0); glVertex3f(end, 0, 0);
    gl_color_rgba(colors[SCENE_AXIS_Y]);
    glVertex3f(0, 0, 0); glVertex3f(0, end, 0);
    gl_color_rgba(colors[SCENE_AXIS_Z]);
    glVertex3f(0, 0, 0); glVertex3f(0, 0, end);
    glEnd();
    glLineWidth(1.0f);
}

static void draw_axis_tip_triplet(float len, float point_size,
                                  const SceneRgba colors[3],
                                  int direction) {
    float end = len * (float)direction;
    glPointSize(point_size);
    glBegin(GL_POINTS);
    gl_color_rgba(colors[SCENE_AXIS_X]);
    glVertex3f(end, 0, 0);
    gl_color_rgba(colors[SCENE_AXIS_Y]);
    glVertex3f(0, end, 0);
    gl_color_rgba(colors[SCENE_AXIS_Z]);
    glVertex3f(0, 0, end);
    glEnd();
    glPointSize(1.0f);
}

static void draw_axis_label_triplet(float len, float offset,
                                    const SceneRgba colors[3],
                                    const char labels[3],
                                    int direction) {
    float pos = (len + offset) * (float)direction;
    draw_axis_label(pos, 0, 0, labels[SCENE_AXIS_X],
                    colors[SCENE_AXIS_X].r, colors[SCENE_AXIS_X].g,
                    colors[SCENE_AXIS_X].b);
    draw_axis_label(0, pos, 0, labels[SCENE_AXIS_Y],
                    colors[SCENE_AXIS_Y].r, colors[SCENE_AXIS_Y].g,
                    colors[SCENE_AXIS_Y].b);
    draw_axis_label(0, 0, pos, labels[SCENE_AXIS_Z],
                    colors[SCENE_AXIS_Z].r, colors[SCENE_AXIS_Z].g,
                    colors[SCENE_AXIS_Z].b);
}

void scene_axes_render(const FrameRenderContext *frame_ctx) {
    const SceneRenderConfig *config = &frame_ctx->config;
    AxesTheme axes_theme = (AxesTheme)config->axes_theme;
    if (axes_theme == AXES_THEME_OFF) return;

    scene_axes_push_state();

    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    scene_axes_apply_quality_config(config);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float breath = sinf((*repl_state_variables()->anim_time) * 0.8f) * 0.5f + 0.5f; /* 0..1 */
    float as = config->alpha_scale;

    switch (axes_theme) {

    case AXES_THEME_CLASSIC: {
        const AxesThemeSpec *spec = axes_theme_spec(AXES_THEME_CLASSIC);
        draw_axis_line_triplet(spec->len, 2.0f, spec->axis, 1);
        draw_axis_label_triplet(spec->len, 0.15f, spec->label, "XYZ", 1);
        break;
    }

    case AXES_THEME_PULSE: {
        const AxesThemeSpec *spec = axes_theme_spec(AXES_THEME_PULSE);
        float len = spec->len;
        /* Solid dim axes */
        draw_axis_line_triplet(len, 1.5f, spec->axis, 1);

        /* Pulsing dot position (loops 0..1) */
        float t = fmodf((*repl_state_variables()->anim_time) * 0.6f, 1.0f);
        float pos = t * len;
        float glow = sinf(t * (float)M_PI); /* bright in middle, dim at ends */
        glow = glow * 0.8f + 0.2f;

        glPointSize(8.0f);
        glBegin(GL_POINTS);
        glColor4f(1.0f, 0.3f, 0.3f, glow);
        glVertex3f(pos, 0, 0);
        glColor4f(0.3f, 1.0f, 0.3f, glow);
        glVertex3f(0, pos, 0);
        glColor4f(0.3f, 0.3f, 1.0f, glow);
        glVertex3f(0, 0, pos);
        glEnd();
        glPointSize(1.0f);

        /* Bright trail behind the dot */
        glLineWidth(3.0f);
        float trail = 0.6f;
        float t0 = pos - trail;
        if (t0 < 0) t0 = 0;
        glBegin(GL_LINES);
        glColor4f(1.0f, 0.3f, 0.3f, 0.05f);
        glVertex3f(t0, 0, 0);
        glColor4f(1.0f, 0.3f, 0.3f, glow * 0.7f);
        glVertex3f(pos, 0, 0);

        glColor4f(0.3f, 1.0f, 0.3f, 0.05f);
        glVertex3f(0, t0, 0);
        glColor4f(0.3f, 1.0f, 0.3f, glow * 0.7f);
        glVertex3f(0, pos, 0);

        glColor4f(0.3f, 0.3f, 1.0f, 0.05f);
        glVertex3f(0, 0, t0);
        glColor4f(0.3f, 0.3f, 1.0f, glow * 0.7f);
        glVertex3f(0, 0, pos);
        glEnd();
        glLineWidth(1.0f);

        draw_axis_label_triplet(len, 0.15f, spec->label, "XYZ", 1);
        break;
    }

    case AXES_THEME_NEON: {
        float len = 2.5f;
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

        /* Outer glow (wide, dim) */
        draw_axis_line_triplet(len, 6.0f, outer, 1);

        /* Core (narrow, bright) */
        draw_axis_line_triplet(len, 2.0f, core, 1);

        /* Bright tip dots */
        draw_axis_tip_triplet(len, 6.0f, tips, 1);

        float la = 0.5f + glow * 0.5f;
        SceneRgba labels[3] = {
            rgba(1.0f * la, 0.3f * la, 0.3f * la, 1.0f),
            rgba(0.3f * la, 1.0f * la, 0.3f * la, 1.0f),
            rgba(0.3f * la, 0.3f * la, 1.0f * la, 1.0f),
        };
        draw_axis_label_triplet(len, 0.15f, labels, "XYZ", 1);
        break;
    }

    case AXES_THEME_COMPASS: {
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
        draw_axis_line_triplet(len, 2.0f, spec->axis, 1);

        /* Negative axes (stippled) */
        glEnable(GL_LINE_STIPPLE);
        glLineStipple(2, 0xAAAA);
        draw_axis_line_triplet(len, 2.0f, negative_axes, -1);
        glDisable(GL_LINE_STIPPLE);

        /* Arrowheads at positive tips */
        draw_axis_tip_triplet(len, 7.0f, positive_tips, 1);

        /* Small dots at negative tips */
        draw_axis_tip_triplet(len, 4.0f, negative_tips, -1);

        /* Origin sphere-ish dot */
        glPointSize(5.0f);
        glBegin(GL_POINTS);
        glColor4f(0.9f, 0.9f, 0.9f, 0.6f);
        glVertex3f(0, 0, 0);
        glEnd();
        glPointSize(1.0f);

        draw_axis_label_triplet(len, 0.15f, spec->label, "XYZ", 1);
        SceneRgba negative_labels[3] = {
            rgba(0.55f, 0.25f, 0.25f, 1.0f),
            rgba(0.25f, 0.55f, 0.25f, 1.0f),
            rgba(0.25f, 0.25f, 0.55f, 1.0f),
        };
        draw_axis_label_triplet(len, 0.15f, negative_labels, "xyz", -1);
        break;
    }

    case AXES_THEME_GIZMO: {
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
        draw_axis_line_triplet(len, 2.0f, spec->axis, 1);

        /* XZ floor plane quadrant — always shown */
        glBegin(GL_QUADS);
        glColor4f(0.58f, 0.60f, 0.72f, fminf(0.07f * as, 1.0f));
        glVertex3f(0,    0, 0);    glVertex3f(fill, 0, 0);
        glVertex3f(fill, 0, fill); glVertex3f(0,    0, fill);
        glEnd();

        /* XY plane quadrant (z=0) — fades in when looking along Z */
        float xy_a = fminf(0.11f * xy_w * as, 1.0f);
        if (xy_a > 0.004f) {
            glBegin(GL_QUADS);
            glColor4f(0.80f, 0.50f, 0.25f, xy_a);
            glVertex3f(0,    0,    0); glVertex3f(fill, 0,    0);
            glVertex3f(fill, fill, 0); glVertex3f(0,    fill, 0);
            glEnd();
            glBegin(GL_LINE_LOOP);
            glColor4f(0.92f, 0.66f, 0.42f, xy_a * 2.2f);
            glVertex3f(0,    0,    0); glVertex3f(fill, 0,    0);
            glVertex3f(fill, fill, 0); glVertex3f(0,    fill, 0);
            glEnd();
        }

        /* ZY plane quadrant (x=0) — fades in when looking along X */
        float zy_a = fminf(0.11f * zy_w * as, 1.0f);
        if (zy_a > 0.004f) {
            glBegin(GL_QUADS);
            glColor4f(0.32f, 0.58f, 0.88f, zy_a);
            glVertex3f(0, 0,    0);    glVertex3f(0, 0,    fill);
            glVertex3f(0, fill, fill); glVertex3f(0, fill, 0);
            glEnd();
            glBegin(GL_LINE_LOOP);
            glColor4f(0.48f, 0.72f, 0.95f, zy_a * 2.2f);
            glVertex3f(0, 0,    0);    glVertex3f(0, 0,    fill);
            glVertex3f(0, fill, fill); glVertex3f(0, fill, 0);
            glEnd();
        }

        /* Origin dot */
        glPointSize(5.0f);
        glBegin(GL_POINTS);
        glColor4f(0.95f, 0.95f, 0.95f, 0.72f);
        glVertex3f(0, 0, 0);
        glEnd();
        glPointSize(1.0f);

        draw_axis_label_triplet(len, 0.15f, spec->label, "XYZ", 1);
        break;
    }

    default: break;
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    if (repl_state_flat_program_user_lighting_enabled()) glEnable(GL_LIGHTING);
    scene_axes_pop_state();
}
