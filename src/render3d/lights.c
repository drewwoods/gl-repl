/*
 * lights.c - scene light setup and visible light indicators.
 */
#include "lights.h"
#include "overlays.h"     /* render3d_draw_bitmap_text */
#include "palette.h"
#include "config.h"
#include "occluded_ghost.h"  /* RENDER3D_OCCLUDED_GHOST_STIPPLE */
#include "gl_includes.h"  /* glColor4f, glutBitmapCharacter (avoid transitive deps) */
#include <math.h>
#include <stdio.h>
#include <string.h>       /* memcpy for theme apply */

static void render3d_lights_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void render3d_lights_pop_state(void) {
    glPopAttrib();
}

void render3d_lights_init_global_ambient(void) {
    /* Bucket-2 carve-out: a lighting *coefficient* (glLightModelfv),
     * not a glColor* draw color, so it is intentionally a named local
     * const and NOT a render3d/palette.h token. */
    static const GLfloat lm_amb[] = { 0.15f, 0.15f, 0.20f, 1.0f };
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm_amb);
}

/* The HEADLIGHT lamp (slot 0, eye-space) sits this far in front of the eye
 * along the view axis (eye-space -Z) instead of exactly at the camera. A
 * lamp *at* the eye would put its indicator at the viewpoint - never on
 * screen - so we nudge it a small, honest distance forward: the GL light
 * genuinely lives here and the indicator is drawn at that same world point
 * (no draw-only fudge). Well clear of the 0.1 near plane; small relative to
 * the default 5.0 camera distance, so it still reads as the camera light. */
#define RENDER3D_HEADLIGHT_EYE_OFFSET 1.0f

/* --- Lighting themes ---------------------------------------------------
 *
 * Each theme defines the position + color of the four lights. Tones
 * are intentionally varied so the indicator overlay and the lit
 * preview both communicate which theme is active at a glance.
 *
 * `.enabled` stays 0 across all themes: the program's glEnable
 * commands decide which slots light up. The bootstrap section in
 * The exporter emits one glDisable per slot, and the upstream program or
 * its configuration re-enables the slots it needs.
 *
 * `.pos_is_eye_space` is 1 for slots whose POSITION must be set at
 * identity modelview (currently only HEADLIGHT slot 0); the runtime uses an
 * identity push per frame, and exported code places those writes in display()
 * before the camera transform. */
static const Render3dLight g_light_themes[LIGHT_THEME_COUNT][MAX_LIGHTS] = {
    /* DEFAULT: warm key + cool fill + orange rim + disabled directional.
     * This is the standard four-light layout. */
    [LIGHT_THEME_DEFAULT] = {
        { .id = GL_LIGHT0,
          .pos      = {  2.0f,  4.0f,  5.0f, 0.0f },
          .diffuse  = { 0.80f, 0.80f, 0.75f, 1.0f },
          .ambient  = { 0.10f, 0.10f, 0.12f, 1.0f },
          .specular = { 1.00f, 1.00f, 0.95f, 1.0f } },
        { .id = GL_LIGHT1,
          .pos      = { -3.0f,  2.0f, -2.0f, 1.0f },
          .diffuse  = { 0.45f, 0.30f, 0.15f, 1.0f },
          .ambient  = { 0.05f, 0.03f, 0.02f, 1.0f },
          .specular = { 0.30f, 0.20f, 0.10f, 1.0f } },
        { .id = GL_LIGHT2,
          .pos      = {  0.0f, -1.0f,  3.0f, 1.0f },
          .diffuse  = { 0.15f, 0.25f, 0.50f, 1.0f },
          .ambient  = { 0.02f, 0.03f, 0.06f, 1.0f },
          .specular = { 0.10f, 0.15f, 0.35f, 1.0f } },
        { .id = GL_LIGHT3,
          .pos      = {  1.0f,  1.0f, -4.0f, 0.0f },
          .diffuse  = { 0.35f, 0.35f, 0.40f, 1.0f },
          .ambient  = { 0.05f, 0.05f, 0.06f, 1.0f },
          .specular = { 0.20f, 0.20f, 0.25f, 1.0f } },
    },
    /* HEADLIGHT: light 0 rides eye space (pos_is_eye_space=1, set at
     * identity modelview), nudged RENDER3D_HEADLIGHT_EYE_OFFSET into the
     * scene (eye -Z) so its indicator clears the viewpoint; the rest are
     * disabled fills the user can opt into. */
    [LIGHT_THEME_HEADLIGHT] = {
        { .id = GL_LIGHT0, .pos_is_eye_space = 1,
          .pos      = {  0.0f,  0.0f, -RENDER3D_HEADLIGHT_EYE_OFFSET, 1.0f },
          .diffuse  = { 0.90f, 0.90f, 0.85f, 1.0f },
          .ambient  = { 0.10f, 0.10f, 0.10f, 1.0f },
          .specular = { 0.70f, 0.70f, 0.65f, 1.0f } },
        { .id = GL_LIGHT1,
          .pos      = { -3.0f,  2.0f, -2.0f, 1.0f },
          .diffuse  = { 0.30f, 0.30f, 0.30f, 1.0f },
          .ambient  = { 0.02f, 0.02f, 0.02f, 1.0f },
          .specular = { 0.10f, 0.10f, 0.10f, 1.0f } },
        { .id = GL_LIGHT2,
          .pos      = {  3.0f, -1.0f, -2.0f, 1.0f },
          .diffuse  = { 0.30f, 0.30f, 0.30f, 1.0f },
          .ambient  = { 0.02f, 0.02f, 0.02f, 1.0f },
          .specular = { 0.10f, 0.10f, 0.10f, 1.0f } },
        { .id = GL_LIGHT3,
          .pos      = {  0.0f,  1.0f,  0.0f, 0.0f },
          .diffuse  = { 0.25f, 0.25f, 0.30f, 1.0f },
          .ambient  = { 0.02f, 0.02f, 0.02f, 1.0f },
          .specular = { 0.10f, 0.10f, 0.12f, 1.0f } },
    },
    /* SOLAR: light 0 at the world origin (positional, no attenuation
     * tweaks here so user code orbits a central sun). Lights 1..3 are
     * dim background fill the user can opt into for ambient rim. */
    [LIGHT_THEME_SOLAR] = {
        { .id = GL_LIGHT0,
          .pos      = {  0.0f,  0.0f,  0.0f, 1.0f },
          .diffuse  = { 1.00f, 0.95f, 0.80f, 1.0f },
          .ambient  = { 0.30f, 0.28f, 0.20f, 1.0f },
          .specular = { 1.00f, 1.00f, 0.90f, 1.0f } },
        { .id = GL_LIGHT1,
          .pos      = {  5.0f,  0.0f,  0.0f, 0.0f },
          .diffuse  = { 0.10f, 0.10f, 0.15f, 1.0f },
          .ambient  = { 0.01f, 0.01f, 0.02f, 1.0f },
          .specular = { 0.05f, 0.05f, 0.08f, 1.0f } },
        { .id = GL_LIGHT2,
          .pos      = { -5.0f,  0.0f,  0.0f, 0.0f },
          .diffuse  = { 0.10f, 0.10f, 0.15f, 1.0f },
          .ambient  = { 0.01f, 0.01f, 0.02f, 1.0f },
          .specular = { 0.05f, 0.05f, 0.08f, 1.0f } },
        { .id = GL_LIGHT3,
          .pos      = {  0.0f,  5.0f,  0.0f, 0.0f },
          .diffuse  = { 0.15f, 0.15f, 0.20f, 1.0f },
          .ambient  = { 0.01f, 0.01f, 0.02f, 1.0f },
          .specular = { 0.08f, 0.08f, 0.10f, 1.0f } },
    },
    /* STUDIO: the three-point portrait rig from tools/render3d_demo.c -
     * warm-white key (upper right), cool-blue rim (upper left), warm-
     * orange fill (lower right), and a green directional accent from
     * below/back. Colors lifted verbatim from render3d_demo's seed_lights. */
    [LIGHT_THEME_STUDIO] = {
        { .id = GL_LIGHT0,
          .pos      = {  5.0f,  6.0f,  4.0f, 1.0f },
          .diffuse  = { 1.00f, 0.95f, 0.85f, 1.0f },
          .ambient  = { 0.15f, 0.15f, 0.18f, 1.0f },
          .specular = { 1.00f, 0.95f, 0.85f, 1.0f } },
        { .id = GL_LIGHT1,
          .pos      = { -4.0f,  3.0f, -3.0f, 1.0f },
          .diffuse  = { 0.40f, 0.60f, 1.00f, 1.0f },
          .ambient  = { 0.00f, 0.00f, 0.00f, 1.0f },
          .specular = { 0.40f, 0.60f, 1.00f, 1.0f } },
        { .id = GL_LIGHT2,
          .pos      = {  4.0f, -1.5f,  2.0f, 1.0f },
          .diffuse  = { 1.00f, 0.55f, 0.20f, 1.0f },
          .ambient  = { 0.00f, 0.00f, 0.00f, 1.0f },
          .specular = { 1.00f, 0.55f, 0.20f, 1.0f } },
        { .id = GL_LIGHT3,
          .pos      = {  0.2f, -1.0f,  0.5f, 0.0f },
          .diffuse  = { 0.55f, 0.95f, 0.55f, 1.0f },
          .ambient  = { 0.00f, 0.00f, 0.00f, 1.0f },
          .specular = { 0.55f, 0.95f, 0.55f, 1.0f } },
    },
    /* NEON: a vibrant saturated triad for colored-material showcases -
     * magenta key (upper right), cyan rim (upper left), lime fill (from
     * below), plus a dim warm-amber back light (directional). Low ambient
     * keeps the hues reading distinctly rather than washing to white. */
    [LIGHT_THEME_NEON] = {
        { .id = GL_LIGHT0,
          .pos      = {  3.0f,  4.0f,  5.0f, 1.0f },
          .diffuse  = { 0.90f, 0.10f, 0.70f, 1.0f },
          .ambient  = { 0.10f, 0.00f, 0.08f, 1.0f },
          .specular = { 1.00f, 0.40f, 0.90f, 1.0f } },
        { .id = GL_LIGHT1,
          .pos      = { -4.0f,  2.0f, -3.0f, 1.0f },
          .diffuse  = { 0.10f, 0.80f, 0.90f, 1.0f },
          .ambient  = { 0.00f, 0.05f, 0.06f, 1.0f },
          .specular = { 0.40f, 0.90f, 1.00f, 1.0f } },
        { .id = GL_LIGHT2,
          .pos      = {  0.0f, -3.0f,  2.0f, 1.0f },
          .diffuse  = { 0.40f, 0.90f, 0.20f, 1.0f },
          .ambient  = { 0.02f, 0.05f, 0.00f, 1.0f },
          .specular = { 0.50f, 1.00f, 0.30f, 1.0f } },
        { .id = GL_LIGHT3,
          .pos      = {  1.0f,  1.0f, -4.0f, 0.0f },
          .diffuse  = { 0.60f, 0.40f, 0.10f, 1.0f },
          .ambient  = { 0.00f, 0.00f, 0.00f, 1.0f },
          .specular = { 0.70f, 0.50f, 0.20f, 1.0f } },
    },
};

const char *render3d_light_theme_names[] = {
    LIGHT_THEME_LIST(LIGHT_THEME_NAME_ENTRY)
};

void render3d_lights_apply_theme(Render3dLight out[MAX_LIGHTS], int theme) {
    if (theme < 0 || theme >= LIGHT_THEME_COUNT)
        theme = LIGHT_THEME_DEFAULT;
    memcpy(out, g_light_themes[theme], sizeof(g_light_themes[theme]));
}


/* Set light properties only. The upstream program still decides whether each
 * light is enabled during command execution. */
void render3d_lights_setup(const Render3dFrameRenderContext *frame_ctx) {
    for (int i = 0; i < MAX_LIGHTS; i++) {
        const Render3dLight *light = &frame_ctx->config.lights[i];
        glDisable(light->id);
        if (light->pos_is_eye_space) {
            /* glLightfv(POSITION) snapshots the current modelview at
             * call time. Eye-space slots want eye coordinates, so push
             * + identity around the write - that's cheaper than the
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

void render3d_lights_render(const Render3dFrameRenderContext *frame_ctx) {
    if (!frame_ctx->config.show_light_indicators) return;

    render3d_lights_push_state();
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float breath = sinf(frame_ctx->config.anim_time * 1.2f) * 0.5f + 0.5f;

    float cam_wx = frame_ctx->camera_world_pos[0];
    float cam_wy = frame_ctx->camera_world_pos[1];
    float cam_wz = frame_ctx->camera_world_pos[2];

    for (int i = 0; i < MAX_LIGHTS; i++) {
        const Render3dLight *light = &frame_ctx->config.lights[i];
        const float *d = light->diffuse;
        const float *p = light->pos;
        int eye_space = light->pos_is_eye_space;
        int is_dir = !eye_space && (p[3] == 0.0f);
        int on = light->enabled;

        float lx, ly, lz;
        if (eye_space) {
            /* pos[] is in eye space; the slot rides the camera, so the
             * world location is the camera's world position plus the
             * eye-space offset rotated into world space along the view
             * axis via the precomputed camera basis. */
            float ox = frame_ctx->camera_basis[0][0] * p[0] +
                       frame_ctx->camera_basis[1][0] * p[1] +
                       frame_ctx->camera_basis[2][0] * p[2];
            float oy = frame_ctx->camera_basis[0][1] * p[0] +
                       frame_ctx->camera_basis[1][1] * p[1] +
                       frame_ctx->camera_basis[2][1] * p[2];
            float oz = frame_ctx->camera_basis[0][2] * p[0] +
                       frame_ctx->camera_basis[1][2] * p[1] +
                       frame_ctx->camera_basis[2][2] * p[2];
            lx = cam_wx + ox;
            ly = cam_wy + oy;
            lz = cam_wz + oz;
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

        if (i == frame_ctx->config.highlight_light_slot) {
            /* Cursor is on this light's glEnable/glDisable(GL_LIGHTn) line:
             * wrap the glyph in a pulsing translucent sphere of the light's
             * own diffuse color so it reads as "this one". Depth test is
             * already off (set above), so it glows over the scene; drawn for
             * on and off lights alike. */
            float hr = 0.28f + breath * 0.12f;
            glColor4f(d[0], d[1], d[2], 0.12f + breath * 0.18f);
            glPushMatrix();
            glTranslatef(lx, ly, lz);
            glutSolidSphere(hr, 16, 12);
            glPopMatrix();
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
            render3d_clr_a(RENDER3D_CLR_LIGHT_CORE, 0.9f * glow);
            glVertex3f(lx, ly, lz);
            glEnd();

            if (is_dir) {
                glEnable(GL_LINE_STIPPLE);
                glLineStipple(2, RENDER3D_OCCLUDED_GHOST_STIPPLE);
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

            if (eye_space) {
                /* Headlight aim ray: a positional point at the eye reads as
                 * a static lamp, so trace the view direction it shines along
                 * (eye -Z rotated to world) a short way into the scene. */
                float fx = -frame_ctx->camera_basis[2][0];
                float fy = -frame_ctx->camera_basis[2][1];
                float fz = -frame_ctx->camera_basis[2][2];
                float aim = 1.2f;
                glLineWidth(1.5f);
                glBegin(GL_LINES);
                glColor4f(d[0], d[1], d[2], 0.5f * glow);
                glVertex3f(lx, ly, lz);
                glColor4f(d[0], d[1], d[2], 0.0f);
                glVertex3f(lx + fx * aim, ly + fy * aim, lz + fz * aim);
                glEnd();
            }

            char label[8];
            snprintf(label, sizeof(label), " L%d", i);
            glColor4f(d[0] * 0.7f + 0.3f, d[1] * 0.7f + 0.3f,
                      d[2] * 0.7f + 0.3f, 0.8f);
            render3d_draw_bitmap_text(FONT_MONO, lx, ly, lz, label);
        } else {
            glPointSize(6.0f);
            glBegin(GL_POINTS);
            render3d_clr_a(RENDER3D_CLR_LIGHT_OFF_DOT, 0.3f);
            glVertex3f(lx, ly, lz);
            glEnd();

            float xsz = 0.12f;
            glEnable(GL_LINE_STIPPLE);
            glLineStipple(1, RENDER3D_OCCLUDED_GHOST_STIPPLE);
            glLineWidth(1.0f);
            glBegin(GL_LINES);
            render3d_clr_a(RENDER3D_CLR_LIGHT_OFF_X, 0.45f);
            glVertex3f(lx - xsz, ly - xsz, lz);
            glVertex3f(lx + xsz, ly + xsz, lz);
            glVertex3f(lx - xsz, ly + xsz, lz);
            glVertex3f(lx + xsz, ly - xsz, lz);
            glEnd();
            glDisable(GL_LINE_STIPPLE);

            char label[16];
            snprintf(label, sizeof(label), " L%d off", i);
            render3d_clr_a(RENDER3D_CLR_LIGHT_OFF_LABEL, 0.45f);
            render3d_draw_bitmap_text(FONT_SMALL, lx, ly, lz, label);
        }
    }

    /* render3d_lights_pop_state restores every bit that was mutated above:
     * point size (GL_POINT_BIT), blend + color (GL_COLOR_BUFFER_BIT /
     * GL_CURRENT_BIT), depth-test enable (GL_DEPTH_BUFFER_BIT), and
     * lighting enable (GL_LIGHTING_BIT). No manual teardown needed. */
    render3d_lights_pop_state();
}
