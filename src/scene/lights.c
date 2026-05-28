/*
 * lights.c - scene light setup and visible light indicators.
 */
#include "lights.h"
#include "overlays.h"     /* scene_draw_bitmap_text */
#include "palette.h"
#include "config.h"
#include "occluded_ghost.h"  /* SCENE_OCCLUDED_GHOST_STIPPLE */
#include "gl_includes.h"  /* glColor4f, glutBitmapCharacter (avoid transitive deps) */
#include <math.h>
#include <stdio.h>
#include <string.h>       /* memcpy for theme apply */

static void scene_lights_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void scene_lights_pop_state(void) {
    glPopAttrib();
}

void scene_lights_init_global_ambient(void) {
    /* Bucket-2 carve-out: a lighting *coefficient* (glLightModelfv),
     * not a glColor* draw color, so it is intentionally a named local
     * const and NOT a scene/palette.h token. */
    static const GLfloat lm_amb[] = { 0.15f, 0.15f, 0.20f, 1.0f };
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm_amb);
}

/* --- Lighting themes ---------------------------------------------------
 *
 * Each theme defines the position + color of the four lights. Tones
 * are intentionally varied so the indicator overlay and the lit
 * preview both communicate which theme is active at a glance.
 *
 * `.enabled` stays 0 across all themes: the program's glEnable
 * commands decide which slots light up. The bootstrap section in
 * src/repl/export.c emits one glDisable per slot, and the user (or an
 * example's @cfg) re-enables the slots they need.
 *
 * `.pos_is_eye_space` is 1 for slots whose POSITION must be set at
 * identity modelview (currently only HEADLIGHT slot 0); the runtime
 * pushes those positions in scene_render_init_gl, and the exporter
 * routes their POSITION line to init() instead of display(). */
static const SceneLight g_light_themes[LIGHT_THEME_COUNT][MAX_LIGHTS] = {
    /* DEFAULT: warm key + cool fill + orange rim + disabled directional.
     * Matches the historical 4-light layout. */
    [LIGHT_THEME_DEFAULT] = {
        { GL_LIGHT0, 0, 0,
            {  2.0f,  4.0f,  5.0f, 0.0f },
            { 0.80f, 0.80f, 0.75f, 1.0f },
            { 0.10f, 0.10f, 0.12f, 1.0f },
            { 1.00f, 1.00f, 0.95f, 1.0f } },
        { GL_LIGHT1, 0, 0,
            { -3.0f,  2.0f, -2.0f, 1.0f },
            { 0.45f, 0.30f, 0.15f, 1.0f },
            { 0.05f, 0.03f, 0.02f, 1.0f },
            { 0.30f, 0.20f, 0.10f, 1.0f } },
        { GL_LIGHT2, 0, 0,
            {  0.0f, -1.0f,  3.0f, 1.0f },
            { 0.15f, 0.25f, 0.50f, 1.0f },
            { 0.02f, 0.03f, 0.06f, 1.0f },
            { 0.10f, 0.15f, 0.35f, 1.0f } },
        { GL_LIGHT3, 0, 0,
            {  1.0f,  1.0f, -4.0f, 0.0f },
            { 0.35f, 0.35f, 0.40f, 1.0f },
            { 0.05f, 0.05f, 0.06f, 1.0f },
            { 0.20f, 0.20f, 0.25f, 1.0f } },
    },
    /* HEADLIGHT: light 0 rides eye space (pos_is_eye_space=1, set at
     * identity modelview); the rest are disabled fills the user can
     * opt into. */
    [LIGHT_THEME_HEADLIGHT] = {
        { GL_LIGHT0, 0, 1,
            {  0.0f,  0.0f,  0.0f, 1.0f },
            { 0.90f, 0.90f, 0.85f, 1.0f },
            { 0.10f, 0.10f, 0.10f, 1.0f },
            { 0.70f, 0.70f, 0.65f, 1.0f } },
        { GL_LIGHT1, 0, 0,
            { -3.0f,  2.0f, -2.0f, 1.0f },
            { 0.30f, 0.30f, 0.30f, 1.0f },
            { 0.02f, 0.02f, 0.02f, 1.0f },
            { 0.10f, 0.10f, 0.10f, 1.0f } },
        { GL_LIGHT2, 0, 0,
            {  3.0f, -1.0f, -2.0f, 1.0f },
            { 0.30f, 0.30f, 0.30f, 1.0f },
            { 0.02f, 0.02f, 0.02f, 1.0f },
            { 0.10f, 0.10f, 0.10f, 1.0f } },
        { GL_LIGHT3, 0, 0,
            {  0.0f,  1.0f,  0.0f, 0.0f },
            { 0.25f, 0.25f, 0.30f, 1.0f },
            { 0.02f, 0.02f, 0.02f, 1.0f },
            { 0.10f, 0.10f, 0.12f, 1.0f } },
    },
    /* SOLAR: light 0 at the world origin (positional, no attenuation
     * tweaks here so user code orbits a central sun). Lights 1..3 are
     * dim background fill the user can opt into for ambient rim. */
    [LIGHT_THEME_SOLAR] = {
        { GL_LIGHT0, 0, 0,
            {  0.0f,  0.0f,  0.0f, 1.0f },
            { 1.00f, 0.95f, 0.80f, 1.0f },
            { 0.30f, 0.28f, 0.20f, 1.0f },
            { 1.00f, 1.00f, 0.90f, 1.0f } },
        { GL_LIGHT1, 0, 0,
            {  5.0f,  0.0f,  0.0f, 0.0f },
            { 0.10f, 0.10f, 0.15f, 1.0f },
            { 0.01f, 0.01f, 0.02f, 1.0f },
            { 0.05f, 0.05f, 0.08f, 1.0f } },
        { GL_LIGHT2, 0, 0,
            { -5.0f,  0.0f,  0.0f, 0.0f },
            { 0.10f, 0.10f, 0.15f, 1.0f },
            { 0.01f, 0.01f, 0.02f, 1.0f },
            { 0.05f, 0.05f, 0.08f, 1.0f } },
        { GL_LIGHT3, 0, 0,
            {  0.0f,  5.0f,  0.0f, 0.0f },
            { 0.15f, 0.15f, 0.20f, 1.0f },
            { 0.01f, 0.01f, 0.02f, 1.0f },
            { 0.08f, 0.08f, 0.10f, 1.0f } },
    },
};

const char *scene_light_theme_names[] = {
    [LIGHT_THEME_DEFAULT]   = "Default",
    [LIGHT_THEME_HEADLIGHT] = "Headlight",
    [LIGHT_THEME_SOLAR]     = "Solar",
};

void scene_lights_apply_theme(SceneLight out[MAX_LIGHTS], int theme) {
    if (theme < 0 || theme >= LIGHT_THEME_COUNT)
        theme = LIGHT_THEME_DEFAULT;
    memcpy(out, g_light_themes[theme], sizeof(g_light_themes[theme]));
}


/* Set light properties only. User REPL commands still decide whether each
 * light is enabled during command execution. */
void scene_lights_setup(const SceneFrameRenderContext *frame_ctx) {
    for (int i = 0; i < MAX_LIGHTS; i++) {
        const SceneLight *light = &frame_ctx->config.lights[i];
        glDisable(light->id);
        if (light->pos_is_eye_space) {
            /* glLightfv(POSITION) snapshots the current modelview at
             * call time. Eye-space slots want eye coordinates, so push
             * + identity around the write — that's cheaper than the
             * alternative (a separate init-time push coordinated with
             * the cfg cycle handler and the load path) and self-heals
             * if the caller hadn't pre-positioned the matrix stack. */
            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glLoadIdentity();
            glLightfv(light->id, GL_POSITION, light->pos);
            glPopMatrix();
        } else {
            glLightfv(light->id, GL_POSITION, light->pos);
        }
        glLightfv(light->id, GL_DIFFUSE,  light->diffuse);
        glLightfv(light->id, GL_AMBIENT,  light->ambient);
        glLightfv(light->id, GL_SPECULAR, light->specular);
    }
}

/* Camera origin in world coordinates, derived from the camera fields
 * SceneRenderConfig carries. Lets scene_lights_render draw indicators
 * for eye-space slots at the actual camera position instead of (0,0,0).
 * Matches the forward modelview chain in glr_camera_load_modelview:
 *   T(0,0,-cam_dist) * Rx(cam_rx) * Ry(cam_ry) * T(-cam_tx,-cam_ty,-cam_tz). */
static void scene_lights_camera_world_pos(const SceneRenderConfig *cfg,
                                          float *out_x, float *out_y, float *out_z) {
    const float deg = 3.14159265358979323846f / 180.0f;
    float cx = cosf(cfg->cam_rx * deg), sx = sinf(cfg->cam_rx * deg);
    float cy = cosf(cfg->cam_ry * deg), sy = sinf(cfg->cam_ry * deg);
    *out_x = cfg->cam_tx - cfg->cam_dist * cx * sy;
    *out_y = cfg->cam_ty + cfg->cam_dist * sx;
    *out_z = cfg->cam_tz + cfg->cam_dist * cx * cy;
}

void scene_lights_render(const SceneFrameRenderContext *frame_ctx) {
    if (!frame_ctx->config.show_light_indicators) return;

    scene_lights_push_state();
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float breath = sinf(frame_ctx->config.anim_time * 1.2f) * 0.5f + 0.5f;

    float cam_wx = 0.0f, cam_wy = 0.0f, cam_wz = 0.0f;
    scene_lights_camera_world_pos(&frame_ctx->config, &cam_wx, &cam_wy, &cam_wz);

    for (int i = 0; i < MAX_LIGHTS; i++) {
        const SceneLight *light = &frame_ctx->config.lights[i];
        const float *d = light->diffuse;
        const float *p = light->pos;
        int eye_space = light->pos_is_eye_space;
        int is_dir = !eye_space && (p[3] == 0.0f);
        int on = light->enabled;

        float lx, ly, lz;
        if (eye_space) {
            /* pos[] is in eye space; the slot rides the camera, so the
             * world location for the indicator is just the camera's
             * world position plus any eye-space offset baked into pos. */
            lx = cam_wx + p[0];
            ly = cam_wy + p[1];
            lz = cam_wz + p[2];
        } else if (is_dir) {
            float len = sqrtf(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
            if (len < 1e-6f) continue;
            lx = p[0] / len * 3.5f;
            ly = p[1] / len * 3.5f;
            lz = p[2] / len * 3.5f;
        } else {
            lx = p[0];
            ly = p[1];
            lz = p[2];
        }

        if (on) {
            float glow = 0.6f + breath * 0.4f;

            glPointSize(18.0f);
            glBegin(GL_POINTS);
            glColor4f(d[0], d[1], d[2], 0.15f * glow);
            glVertex3f(lx, ly, lz);
            glEnd();

            glPointSize(8.0f);
            glBegin(GL_POINTS);
            glColor4f(d[0], d[1], d[2], 0.7f * glow);
            glVertex3f(lx, ly, lz);
            glEnd();

            glPointSize(3.0f);
            glBegin(GL_POINTS);
            scene_clr_a(SCENE_CLR_LIGHT_CORE, 0.9f * glow);
            glVertex3f(lx, ly, lz);
            glEnd();

            if (is_dir) {
                glEnable(GL_LINE_STIPPLE);
                glLineStipple(2, SCENE_OCCLUDED_GHOST_STIPPLE);
                glLineWidth(1.0f);
                glBegin(GL_LINES);
                glColor4f(d[0], d[1], d[2], 0.35f * glow);
                glVertex3f(lx, ly, lz);
                glColor4f(d[0], d[1], d[2], 0.05f);
                glVertex3f(0, 0, 0);
                glEnd();
                glDisable(GL_LINE_STIPPLE);
            } else {
                /* Star-burst rays along the 6 cardinal axes; hoisted
                 * out of the begin/end so it's clearly data, not part
                 * of the vertex stream. */
                static const float dirs[6][3] = {
                    {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
                };
                float rlen = 0.25f + breath * 0.1f;
                glLineWidth(1.0f);
                glBegin(GL_LINES);
                for (int r = 0; r < 6; r++) {
                    glColor4f(d[0], d[1], d[2], 0.4f * glow);
                    glVertex3f(lx, ly, lz);
                    glColor4f(d[0], d[1], d[2], 0.0f);
                    glVertex3f(lx + dirs[r][0] * rlen,
                               ly + dirs[r][1] * rlen,
                               lz + dirs[r][2] * rlen);
                }
                glEnd();
            }

            char label[8];
            snprintf(label, sizeof(label), " L%d", i);
            glColor4f(d[0] * 0.7f + 0.3f, d[1] * 0.7f + 0.3f,
                      d[2] * 0.7f + 0.3f, 0.8f);
            scene_draw_bitmap_text(FONT_SMALL, lx, ly, lz, label);
        } else {
            glPointSize(6.0f);
            glBegin(GL_POINTS);
            scene_clr_a(SCENE_CLR_LIGHT_OFF_DOT, 0.3f);
            glVertex3f(lx, ly, lz);
            glEnd();

            float xsz = 0.12f;
            glEnable(GL_LINE_STIPPLE);
            glLineStipple(1, SCENE_OCCLUDED_GHOST_STIPPLE);
            glLineWidth(1.0f);
            glBegin(GL_LINES);
            scene_clr_a(SCENE_CLR_LIGHT_OFF_X, 0.45f);
            glVertex3f(lx - xsz, ly - xsz, lz);
            glVertex3f(lx + xsz, ly + xsz, lz);
            glVertex3f(lx - xsz, ly + xsz, lz);
            glVertex3f(lx + xsz, ly - xsz, lz);
            glEnd();
            glDisable(GL_LINE_STIPPLE);

            char label[16];
            snprintf(label, sizeof(label), " L%d off", i);
            scene_clr_a(SCENE_CLR_LIGHT_OFF_LABEL, 0.45f);
            scene_draw_bitmap_text(FONT_SMALL, lx, ly, lz, label);
        }
    }

    /* scene_lights_pop_state restores every bit that was mutated above:
     * point size (GL_POINT_BIT), blend + color (GL_COLOR_BUFFER_BIT /
     * GL_CURRENT_BIT), depth-test enable (GL_DEPTH_BUFFER_BIT), and
     * lighting enable (GL_LIGHTING_BIT). No manual teardown needed. */
    scene_lights_pop_state();
}
