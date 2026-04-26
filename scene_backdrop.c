/*
 * scene_backdrop.c - optional 3D backdrop renderers for the REPL scene.
 */
#include "scene_backdrop.h"

#define CITY_BLDG_COUNT   150
#define CITY_RADIUS       42.0f
#define CITY_RING_SPREAD   7.0f
#define CITY_CYCLE_SECS  600.0f

static void scene_backdrop_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void scene_backdrop_pop_state(void) {
    glPopAttrib();
}

static float city_rng(unsigned int s) {
    s = ((s >> 16) ^ s) * 0x45d9f3bu;
    s = ((s >> 16) ^ s) * 0x45d9f3bu;
    s = (s >> 16) ^ s;
    return (float)(s & 0xFFFFu) * (1.0f / 65536.0f);
}

static float city_night_factor(float angle, float anim_time) {
    float tz = angle / (2.0f * (float)M_PI);
    float local_t = fmodf(anim_time / CITY_CYCLE_SECS + tz, 1.0f);
    if (local_t < 0.0f) local_t += 1.0f;
    return 0.5f + 0.5f * cosf(local_t * 2.0f * (float)M_PI);
}

static void draw_cityscape(float anim_time) {
    scene_backdrop_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (int bi = 0; bi < CITY_BLDG_COUNT; bi++) {
        unsigned int base = (unsigned int)bi * 13u;

        float h0 = city_rng(base + 1u);
        float h1 = city_rng(base + 2u);
        float h2 = city_rng(base + 3u);
        float h3 = city_rng(base + 4u);
        float h4 = city_rng(base + 5u);

        float base_ang = ((float)bi / (float)CITY_BLDG_COUNT) * 2.0f * (float)M_PI;
        float jitter = (h0 - 0.5f) * (1.6f * (float)M_PI / (float)CITY_BLDG_COUNT);
        float angle = base_ang + jitter;

        float radius = CITY_RADIUS + (h1 - 0.5f) * CITY_RING_SPREAD;
        float cx = sinf(angle) * radius;
        float cz = cosf(angle) * radius;

        float bw = 0.9f + h2 * 1.8f;
        float bd = 0.55f + h3 * 0.90f;
        float bh = 1.4f + h4 * 6.5f;

        int has_tier2 = (city_rng(base + 6u) > 0.60f);
        float t2_frac_h = 0.35f + city_rng(base + 7u) * 0.30f;
        float t2_frac_w = 0.45f + city_rng(base + 8u) * 0.30f;
        float t2_frac_d = 0.45f + city_rng(base + 9u) * 0.30f;
        float t2_h = bh * t2_frac_h;
        float t2_bw = bw * t2_frac_w;
        float t2_bd = bd * t2_frac_d;

        float tang_x = cosf(angle);
        float tang_z = -sinf(angle);
        float in_x = -sinf(angle);
        float in_z = -cosf(angle);

        float hw = bw * 0.5f;
        float hd = bd * 0.5f;

        float ilx = cx + tang_x*hw + in_x*hd, ilz = cz + tang_z*hw + in_z*hd;
        float irx = cx - tang_x*hw + in_x*hd, irz = cz - tang_z*hw + in_z*hd;
        float olx = cx + tang_x*hw - in_x*hd, olz = cz + tang_z*hw - in_z*hd;
        float orx = cx - tang_x*hw - in_x*hd, orz = cz - tang_z*hw - in_z*hd;

        float y0 = -0.05f;
        float y1 = bh;
        float night = city_night_factor(angle, anim_time);

        float bd_base = 0.07f + night * 0.035f;
        float bd_r = bd_base;
        float bd_g = bd_base;
        float bd_b = bd_base + 0.04f;

        glBegin(GL_QUADS);

        glColor3f(bd_r * 1.25f, bd_g * 1.25f, bd_b * 1.45f);
        glVertex3f(irx, y0, irz); glVertex3f(ilx, y0, ilz);
        glVertex3f(ilx, y1, ilz); glVertex3f(irx, y1, irz);

        glColor3f(bd_r * 0.55f, bd_g * 0.55f, bd_b * 0.60f);
        glVertex3f(olx, y0, olz); glVertex3f(orx, y0, orz);
        glVertex3f(orx, y1, orz); glVertex3f(olx, y1, olz);

        glColor3f(bd_r * 0.80f, bd_g * 0.80f, bd_b * 0.90f);
        glVertex3f(ilx, y0, ilz); glVertex3f(olx, y0, olz);
        glVertex3f(olx, y1, olz); glVertex3f(ilx, y1, ilz);

        glVertex3f(orx, y0, orz); glVertex3f(irx, y0, irz);
        glVertex3f(irx, y1, irz); glVertex3f(orx, y1, orz);

        glColor3f(bd_r * 0.50f, bd_g * 0.50f, bd_b * 0.55f);
        glVertex3f(ilx, y1, ilz); glVertex3f(olx, y1, olz);
        glVertex3f(orx, y1, orz); glVertex3f(irx, y1, irz);

        glEnd();

        if (has_tier2) {
            float hw2 = t2_bw * 0.5f, hd2 = t2_bd * 0.5f;
            float il2x = cx + tang_x*hw2 + in_x*hd2, il2z = cz + tang_z*hw2 + in_z*hd2;
            float ir2x = cx - tang_x*hw2 + in_x*hd2, ir2z = cz - tang_z*hw2 + in_z*hd2;
            float ol2x = cx + tang_x*hw2 - in_x*hd2, ol2z = cz + tang_z*hw2 - in_z*hd2;
            float or2x = cx - tang_x*hw2 - in_x*hd2, or2z = cz - tang_z*hw2 - in_z*hd2;
            float y2a = y1, y2b = y1 + t2_h;

            glBegin(GL_QUADS);
            glColor3f(bd_r * 1.25f, bd_g * 1.25f, bd_b * 1.45f);
            glVertex3f(ir2x, y2a, ir2z); glVertex3f(il2x, y2a, il2z);
            glVertex3f(il2x, y2b, il2z); glVertex3f(ir2x, y2b, ir2z);

            glColor3f(bd_r * 0.55f, bd_g * 0.55f, bd_b * 0.60f);
            glVertex3f(ol2x, y2a, ol2z); glVertex3f(or2x, y2a, or2z);
            glVertex3f(or2x, y2b, or2z); glVertex3f(ol2x, y2b, ol2z);

            glColor3f(bd_r * 0.80f, bd_g * 0.80f, bd_b * 0.90f);
            glVertex3f(il2x, y2a, il2z); glVertex3f(ol2x, y2a, ol2z);
            glVertex3f(ol2x, y2b, ol2z); glVertex3f(il2x, y2b, il2z);
            glVertex3f(or2x, y2a, or2z); glVertex3f(ir2x, y2a, ir2z);
            glVertex3f(ir2x, y2b, ir2z); glVertex3f(or2x, y2b, or2z);

            glColor3f(bd_r * 0.50f, bd_g * 0.50f, bd_b * 0.55f);
            glVertex3f(il2x, y2b, il2z); glVertex3f(ol2x, y2b, ol2z);
            glVertex3f(or2x, y2b, or2z); glVertex3f(ir2x, y2b, ir2z);
            glEnd();
        }

        int wcols = 1 + (int)(bw / 0.65f);
        int wrows = 1 + (int)(bh / 0.60f);
        if (wcols < 1) wcols = 1;
        if (wcols > 9) wcols = 9;
        if (wrows < 2) wrows = 2;
        if (wrows > 14) wrows = 14;

        float cell_w = bw / (float)wcols;
        float cell_h = bh / (float)wrows;
        float win_hw = cell_w * 0.20f;
        float win_hh = cell_h * 0.22f;

        float protrude = 0.04f;
        float face_ox = in_x * (hd + protrude);
        float face_oz = in_z * (hd + protrude);

        float warmth = city_rng(base + 200u);
        float tz = angle / (2.0f * (float)M_PI);
        float bldg_phase = (city_rng(base + 50u) - 0.5f) * 0.06f;

        for (int wc = 0; wc < wcols; wc++) {
            for (int wr = 0; wr < wrows; wr++) {
                unsigned int wid = base + 300u + (unsigned int)(wc * 17 + wr);
                float wrng = city_rng(wid);
                if (wrng < 0.10f) continue;

                float win_phase = (city_rng(wid + 7u) - 0.5f) * 0.12f;
                float lt = fmodf(anim_time / CITY_CYCLE_SECS + tz + bldg_phase + win_phase,
                                 1.0f);
                if (lt < 0.0f) lt += 1.0f;

                float raw = 0.5f + 0.5f * cosf(lt * 2.0f * (float)M_PI);
                float night_sq = raw * raw;
                float thresh = 0.20f + city_rng(wid + 11u) * 0.52f;

                int always_on = (wrng > 0.92f);
                float lit;
                if (always_on) {
                    lit = 0.12f + night_sq * 0.45f;
                } else if (night_sq > thresh) {
                    lit = (night_sq - thresh) / (1.0f - thresh);
                    lit *= 0.70f + city_rng(wid + 1u) * 0.30f;
                } else {
                    lit = 0.0f;
                }

                if (lit < 0.03f) continue;

                float u = ((float)wc + 0.5f) / (float)wcols;
                float v = ((float)wr + 0.5f) / (float)wrows;

                float wx = cx + tang_x * (u - 0.5f) * bw + face_ox;
                float wz = cz + tang_z * (u - 0.5f) * bw + face_oz;
                float wy = y0 + v * bh;

                float wr_c = warmth > 0.5f ?
                             (0.90f + warmth * 0.10f) :
                             (0.85f + warmth * 0.10f);
                float wg_c = warmth > 0.5f ?
                             (0.62f + warmth * 0.20f) :
                             (0.80f + warmth * 0.10f);
                float wb_c = warmth > 0.5f ?
                             (0.10f + (1.0f - warmth) * 0.20f) :
                             (0.70f + (1.0f - warmth) * 0.20f);

                glColor4f(wr_c * lit, wg_c * lit, wb_c * lit, 0.85f * lit + 0.05f);

                glBegin(GL_QUADS);
                glVertex3f(wx - tang_x*win_hw, wy - win_hh, wz - tang_z*win_hw);
                glVertex3f(wx + tang_x*win_hw, wy - win_hh, wz + tang_z*win_hw);
                glVertex3f(wx + tang_x*win_hw, wy + win_hh, wz + tang_z*win_hw);
                glVertex3f(wx - tang_x*win_hw, wy + win_hh, wz - tang_z*win_hw);
                glEnd();
            }
        }
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    scene_backdrop_pop_state();
}

void scene_backdrop_render(const FrameRenderContext *frame_ctx) {
    switch (frame_ctx->config.backdrop_mode) {
    case 1:
        draw_cityscape(frame_ctx->config.anim_time);
        break;
    default:
        break;
    }
}
