/*
 * scene_lights.c - scene light setup and visible light indicators.
 */
#include "lights.h"
#include "config.h"
#include <math.h>
#include <stdio.h>

static void scene_lights_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void scene_lights_pop_state(void) {
    glPopAttrib();
}

void scene_lights_init_global_ambient(void) {
    GLfloat lm_amb[] = { 0.15f, 0.15f, 0.20f, 1.0f };
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm_amb);
}

/* Set light properties only. User REPL commands still decide whether each
 * light is enabled during command execution. */
void scene_lights_setup(const FrameRenderContext *frame_ctx) {
    for (int i = 0; i < MAX_LIGHTS; i++) {
        const SceneLight *light = &frame_ctx->config.lights[i];
        glDisable(light->id);
        glLightfv(light->id, GL_POSITION, light->pos);
        glLightfv(light->id, GL_DIFFUSE,  light->diffuse);
        glLightfv(light->id, GL_AMBIENT,  light->ambient);
        glLightfv(light->id, GL_SPECULAR, light->specular);
    }
}

void scene_lights_render(const FrameRenderContext *frame_ctx) {
    if (!frame_ctx->config.show_light_indicators) return;
    int g_user_lighting_enabled = frame_ctx->config.user_lighting_enabled;

    scene_lights_push_state();
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float breath = sinf(frame_ctx->config.anim_time * 1.2f) * 0.5f + 0.5f;

    for (int i = 0; i < MAX_LIGHTS; i++) {
        const SceneLight *light = &frame_ctx->config.lights[i];
        float *d = (float *)light->diffuse;
        float *p = (float *)light->pos;
        int is_dir = (p[3] == 0.0f);
        int on = light->enabled;

        float lx, ly, lz;
        if (is_dir) {
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
            glColor4f(1.0f, 1.0f, 1.0f, 0.9f * glow);
            glVertex3f(lx, ly, lz);
            glEnd();

            if (is_dir) {
                glEnable(GL_LINE_STIPPLE);
                glLineStipple(2, 0xAAAA);
                glLineWidth(1.0f);
                glBegin(GL_LINES);
                glColor4f(d[0], d[1], d[2], 0.35f * glow);
                glVertex3f(lx, ly, lz);
                glColor4f(d[0], d[1], d[2], 0.05f);
                glVertex3f(0, 0, 0);
                glEnd();
                glDisable(GL_LINE_STIPPLE);
            } else {
                float rlen = 0.25f + breath * 0.1f;
                glLineWidth(1.0f);
                glBegin(GL_LINES);
                float dirs[][3] = {
                    {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
                };
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
            glRasterPos3f(lx, ly, lz);
            for (const char *c = label; *c; c++)
                glutBitmapCharacter(FONT_SMALL, (unsigned char)*c);
        } else {
            glPointSize(6.0f);
            glBegin(GL_POINTS);
            glColor4f(0.4f, 0.4f, 0.4f, 0.3f);
            glVertex3f(lx, ly, lz);
            glEnd();

            float xsz = 0.12f;
            glEnable(GL_LINE_STIPPLE);
            glLineStipple(1, 0xAAAA);
            glLineWidth(1.0f);
            glBegin(GL_LINES);
            glColor4f(0.7f, 0.2f, 0.2f, 0.45f);
            glVertex3f(lx - xsz, ly - xsz, lz);
            glVertex3f(lx + xsz, ly + xsz, lz);
            glVertex3f(lx - xsz, ly + xsz, lz);
            glVertex3f(lx + xsz, ly - xsz, lz);
            glEnd();
            glDisable(GL_LINE_STIPPLE);

            char label[16];
            snprintf(label, sizeof(label), " L%d off", i);
            glColor4f(0.5f, 0.3f, 0.3f, 0.45f);
            glRasterPos3f(lx, ly, lz);
            for (const char *c = label; *c; c++)
                glutBitmapCharacter(FONT_SMALL, (unsigned char)*c);
        }
    }

    glPointSize(1.0f);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    if (g_user_lighting_enabled) glEnable(GL_LIGHTING);
    scene_lights_pop_state();
}
