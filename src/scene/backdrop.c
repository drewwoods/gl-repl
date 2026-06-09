/*
 * backdrop.c - optional 3D backdrop renderers for the REPL scene.
 */
#include "backdrop.h"

#include <math.h>     /* sinf, cosf, fmodf, M_PI (via gl_includes.h) */

#define CITY_BLDG_COUNT   300
#define CITY_RADIUS       72.0f
#define CITY_RING_SPREAD   7.0f
#define CITY_CYCLE_SECS  500.0f

#define STAR_COUNT        2080
#define STAR_SKY_RADIUS   30.0f

/* Per-building deterministic RNG: bi * CITY_RNG_STRIDE seeds the hash.
 * Stride MUST exceed the largest per-building offset so seed ranges for
 * different buildings never overlap. Offsets used below: heights 1..5,
 * tier2 6..9, bldg_phase +50, warmth +200, window-base +300, plus
 * window-relative wc*17 + wr (max ~167 for 9*17+14) and per-window
 * offsets +1..+22, giving a max of ~489 from base. 512 is the next
 * power of two above that, leaving headroom for new slots.
 *
 * The previous value (13) collided across buildings: building 0's
 * warmth (base+200) shared a seed with building 15's height random
 * (15*13 + 5 = 200), and similar overlaps cascaded through every
 * detail. */
#define CITY_RNG_STRIDE   512u

/* Linear distance fog, as a fraction of CITY_RADIUS: fog ramps from
 * START_FRAC to END_FRAC of the ring radius so the far buildings recede
 * into the clear color. */
#define CITY_FOG_START_FRAC 0.60f
#define CITY_FOG_END_FRAC   1.45f

/* Building footprint/height = MIN + rng[0,1] * RANGE (world units). */
#define CITY_BLDG_W_MIN   0.9f
#define CITY_BLDG_W_RANGE 1.8f
#define CITY_BLDG_D_MIN   0.55f
#define CITY_BLDG_D_RANGE 0.90f
#define CITY_BLDG_H_MIN   1.4f
#define CITY_BLDG_H_RANGE 6.5f

/* Starfield deterministic RNG: index * STAR_RNG_STRIDE + STAR_RNG_SEED. */
#define STAR_RNG_STRIDE   7u
#define STAR_RNG_SEED     0x5A7Eu

/* Cumulative star-size band cutoffs as fractions of STAR_COUNT:
 * small (0..57%), medium (57..80%), large (80..94%), bright (94..100%). */
#define STAR_BAND_CUT_SMALL 0.57f
#define STAR_BAND_CUT_MED   0.80f
#define STAR_BAND_CUT_LARGE 0.94f

/* --- Sunset backdrop --- */

/* Sky dome radius sits just inside the star dome so the two never
 * z-fight if a future mode composes them; the sun plane sits inside
 * the dome. */
#define SUNSET_SKY_RADIUS   29.0f
#define SUNSET_SKY_SEGS     48
#define SUNSET_SKY_RINGS    16
/* Dome elevation range in radians: slightly below the horizon (so no
 * gap shows under the grid plane at shallow camera angles) to zenith. */
#define SUNSET_PHI_MIN     (-0.06f * (float)M_PI)
#define SUNSET_PHI_MAX      (0.50f * (float)M_PI)

#define SUNSET_SUN_DIST     24.0f
#define SUNSET_SUN_RADIUS    7.5f
#define SUNSET_SUN_ELEV      5.0f
/* Horizontal slices across the sun disc. Must stay well above
 * SUNSET_STRIPE_FREQ so each scanline stripe spans several slices and
 * the gap edges look crisp rather than aliased. */
#define SUNSET_SUN_BANDS     96
/* Scanline gaps: stripes per disc height, downward scroll rate in
 * stripe phase per second, and where (in v, -1=bottom .. 1=top) the
 * gaps start opening. */
#define SUNSET_STRIPE_FREQ   9.0f
#define SUNSET_STRIPE_SCROLL 0.55f
#define SUNSET_GAP_START_V   0.10f

#define SUNSET_STAR_COUNT    260
#define SUNSET_STAR_STRIDE   11u
#define SUNSET_STAR_SEED     0xD05Eu

static void scene_backdrop_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void scene_backdrop_pop_state(void) {
    glPopAttrib();
}

/* xorshift-multiply integer bit-mixing hash (the well-known "hash
 * without modulo" construction). Deterministic per seed; returns a
 * stable value in [0,1) used to scatter per-building / per-window detail. */
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

/* --- draw_cityscape phases ---
 *
 * The 200+ line god-function had four nested concerns: GL setup
 * (lighting/blend/fog), per-building geometry math, the two tiers of
 * box-quad emission (5 colored faces each), and the 2D grid of
 * window-quads with per-window lit-ness and color rolls. Extracted
 * into setup_city_gl_state / draw_building_box / draw_building_windows
 * so the outer loop reads as the seed/scatter/render sequence. */

/* Drawn box: the 8 corners are 4 floor (y=y_base) + 4 roof (y=y_top)
 * combinations of the inner/outer-left/right XZ pairs. The five
 * colored quads (inner face, outer face, two side faces, roof) all
 * share these four XZ corners. Drawn with a single glBegin(GL_QUADS). */
typedef struct CityBoxCorners {
    float ilx, ilz;  /* inner-left  */
    float irx, irz;  /* inner-right */
    float olx, olz;  /* outer-left  */
    float orx, orz;  /* outer-right */
} CityBoxCorners;

static CityBoxCorners city_box_corners(float cx, float cz,
                                       float tang_x, float tang_z,
                                       float in_x, float in_z,
                                       float half_w, float half_d) {
    return (CityBoxCorners){
        .ilx = cx + tang_x*half_w + in_x*half_d,
        .ilz = cz + tang_z*half_w + in_z*half_d,
        .irx = cx - tang_x*half_w + in_x*half_d,
        .irz = cz - tang_z*half_w + in_z*half_d,
        .olx = cx + tang_x*half_w - in_x*half_d,
        .olz = cz + tang_z*half_w - in_z*half_d,
        .orx = cx - tang_x*half_w - in_x*half_d,
        .orz = cz - tang_z*half_w - in_z*half_d,
    };
}

/* One five-quad box (inner / outer / 2 sides / roof). bd_r,g,b is the
 * base color (per-building deterministic + night-factor modulated);
 * the five faces multiply the base by per-face tints to fake lighting
 * without enabling GL_LIGHTING. */
static void draw_building_box(const CityBoxCorners *c,
                              float y_base, float y_top,
                              float bd_r, float bd_g, float bd_b) {
    glBegin(GL_QUADS);

    /* Inner face (camera-side) */
    glColor3f(bd_r * 1.25f, bd_g * 1.25f, bd_b * 1.45f);
    glVertex3f(c->irx, y_base, c->irz); glVertex3f(c->ilx, y_base, c->ilz);
    glVertex3f(c->ilx, y_top,  c->ilz); glVertex3f(c->irx, y_top,  c->irz);

    /* Outer face */
    glColor3f(bd_r * 0.55f, bd_g * 0.55f, bd_b * 0.60f);
    glVertex3f(c->olx, y_base, c->olz); glVertex3f(c->orx, y_base, c->orz);
    glVertex3f(c->orx, y_top,  c->orz); glVertex3f(c->olx, y_top,  c->olz);

    /* Side faces */
    glColor3f(bd_r * 0.80f, bd_g * 0.80f, bd_b * 0.90f);
    glVertex3f(c->ilx, y_base, c->ilz); glVertex3f(c->olx, y_base, c->olz);
    glVertex3f(c->olx, y_top,  c->olz); glVertex3f(c->ilx, y_top,  c->ilz);

    glVertex3f(c->orx, y_base, c->orz); glVertex3f(c->irx, y_base, c->irz);
    glVertex3f(c->irx, y_top,  c->irz); glVertex3f(c->orx, y_top,  c->orz);

    /* Roof */
    glColor3f(bd_r * 0.50f, bd_g * 0.50f, bd_b * 0.55f);
    glVertex3f(c->ilx, y_top,  c->ilz); glVertex3f(c->olx, y_top,  c->olz);
    glVertex3f(c->orx, y_top,  c->orz); glVertex3f(c->irx, y_top,  c->irz);

    glEnd();
}

/* Per-building inputs the window grid needs. */
typedef struct CityWindowGeometry {
    float cx, cz;          /* footprint center */
    float bw, bh;          /* building width, height */
    float y_base;          /* floor Y */
    float tang_x, tang_z;  /* unit vector along the camera-facing facade */
    float face_ox, face_oz;/* offset to push windows slightly outward */
    float angle;           /* polar angle around the city ring */
    unsigned int base;     /* per-building RNG seed offset */
    float bldg_phase;      /* coarse day/night phase */
    float warmth;          /* palette warmth roll */
    int wcols, wrows;      /* window grid resolution */
    float win_hw, win_hh;  /* per-window quad half-dimensions */
} CityWindowGeometry;

static void draw_building_windows(const CityWindowGeometry *g, float anim_time) {
    float tz = g->angle / (2.0f * (float)M_PI);

    for (int wc = 0; wc < g->wcols; wc++) {
        for (int wr = 0; wr < g->wrows; wr++) {
            unsigned int wid = g->base + 300u + (unsigned int)(wc * 17 + wr);
            float wrng = city_rng(wid);
            if (wrng < 0.10f) continue;

            float win_phase = (city_rng(wid + 7u) - 0.5f) * 0.12f;
            float lt = fmodf(anim_time / CITY_CYCLE_SECS + tz + g->bldg_phase + win_phase,
                             1.0f);
            if (lt < 0.0f) lt += 1.0f;

            /* Squaring keeps twilight transitions soft while still giving
             * an emphatic nighttime "on" window band. */
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

            float u = ((float)wc + 0.5f) / (float)g->wcols;
            float v = ((float)wr + 0.5f) / (float)g->wrows;

            float wx = g->cx + g->tang_x * (u - 0.5f) * g->bw + g->face_ox;
            float wz = g->cz + g->tang_z * (u - 0.5f) * g->bw + g->face_oz;
            float wy = g->y_base + v * g->bh;

            /* Retro-80s palette matching the star colors */
            float wr_c, wg_c, wb_c;
            if (g->warmth < 0.45f) {
                /* Off-white, slight cool tint */
                float w = city_rng(wid + 20u);
                wr_c = 0.88f + w * 0.10f;
                wg_c = 0.88f + w * 0.06f;
                wb_c = 0.94f + w * 0.04f;
            } else if (g->warmth < 0.75f) {
                /* Neon blue */
                float b = city_rng(wid + 21u);
                wr_c = 0.22f + b * 0.18f;
                wg_c = 0.52f + b * 0.22f;
                wb_c = 1.0f;
            } else {
                /* Purple / violet */
                float p = city_rng(wid + 22u);
                wr_c = 0.52f + p * 0.22f;
                wg_c = 0.12f + p * 0.14f;
                wb_c = 0.88f + p * 0.10f;
            }

            glColor4f(wr_c * lit, wg_c * lit, wb_c * lit, 0.85f * lit + 0.05f);

            glBegin(GL_QUADS);
            glVertex3f(wx - g->tang_x*g->win_hw, wy - g->win_hh, wz - g->tang_z*g->win_hw);
            glVertex3f(wx + g->tang_x*g->win_hw, wy - g->win_hh, wz + g->tang_z*g->win_hw);
            glVertex3f(wx + g->tang_x*g->win_hw, wy + g->win_hh, wz + g->tang_z*g->win_hw);
            glVertex3f(wx - g->tang_x*g->win_hw, wy + g->win_hh, wz - g->tang_z*g->win_hw);
            glEnd();
        }
    }
}

/* Setup GL state for the cityscape pass and return the previous
 * GL_FOG_DISTANCE_MODE_NV for tail-restore. (The mode isn't in the
 * Khronos GL_FOG_BIT, so glPushAttrib doesn't reliably save it.) */
static GLint setup_city_gl_state(int nv_fog_distance_supported) {
    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    float clear_col[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_col);
    glFogfv(GL_FOG_COLOR, clear_col);
    glEnable(GL_FOG);
    glHint(GL_FOG_HINT, GL_NICEST);
    glFogi(GL_FOG_MODE, GL_LINEAR);

    /* When GL_NV_fog_distance is available, fog by true radial eye
     * distance instead of eye-plane depth so the ring's side buildings
     * stop popping in and out at the fringes as the camera orbits.
     * Snapshot the prior value so the function tail can restore it
     * rather than trusting driver good-citizenship. */
    GLint saved_nv_fog_mode = 0;
    if (nv_fog_distance_supported) {
        glGetIntegerv(GL_FOG_DISTANCE_MODE_NV, &saved_nv_fog_mode);
        glFogi(GL_FOG_DISTANCE_MODE_NV, GL_EYE_RADIAL_NV);
    }
    glFogf(GL_FOG_START, CITY_RADIUS * CITY_FOG_START_FRAC);
    glFogf(GL_FOG_END, CITY_RADIUS * CITY_FOG_END_FRAC);
    return saved_nv_fog_mode;
}

static void draw_cityscape(float anim_time, int nv_fog_distance_supported) {
    scene_backdrop_push_state();
    GLint saved_nv_fog_mode = setup_city_gl_state(nv_fog_distance_supported);

    for (int bi = 0; bi < CITY_BLDG_COUNT; bi++) {
        unsigned int base = (unsigned int)bi * CITY_RNG_STRIDE;

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

        float bw = CITY_BLDG_W_MIN + h2 * CITY_BLDG_W_RANGE;
        float bd = CITY_BLDG_D_MIN + h3 * CITY_BLDG_D_RANGE;
        float bh = CITY_BLDG_H_MIN + h4 * CITY_BLDG_H_RANGE;

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

        CityBoxCorners tier1 = city_box_corners(cx, cz, tang_x, tang_z,
                                                in_x, in_z, hw, hd);

        /* y_base / y_top straddle ground level; named to avoid the
         * POSIX-<math.h> Bessel y0/y1 shadow. */
        float y_base = -0.05f;
        float y_top  = bh;
        float night = city_night_factor(angle, anim_time);

        float bd_base = 0.07f + night * 0.035f;
        float bd_r = bd_base;
        float bd_g = bd_base;
        float bd_b = bd_base + 0.04f;

        draw_building_box(&tier1, y_base, y_top, bd_r, bd_g, bd_b);

        if (has_tier2) {
            CityBoxCorners tier2 = city_box_corners(cx, cz, tang_x, tang_z,
                                                    in_x, in_z,
                                                    t2_bw * 0.5f,
                                                    t2_bd * 0.5f);
            draw_building_box(&tier2, y_top, y_top + t2_h,
                              bd_r, bd_g, bd_b);
        }

        /* Building footprint ranges (CITY_BLDG_W_RANGE / _H_RANGE) and
         * the 0.65/0.60 cell sizes constrain wcols ∈ [2, 5] and
         * wrows ∈ [3, 14]. The upper clamp on wrows is the only
         * boundary-effective one; the others were dead before. */
        int wcols = 1 + (int)(bw / 0.65f);
        int wrows = 1 + (int)(bh / 0.60f);
        if (wrows > 14) wrows = 14;

        float cell_w = bw / (float)wcols;
        float cell_h = bh / (float)wrows;

        float protrude = 0.04f;
        CityWindowGeometry win_geom = {
            .cx = cx, .cz = cz,
            .bw = bw, .bh = bh,
            .y_base = y_base,
            .tang_x = tang_x, .tang_z = tang_z,
            .face_ox = in_x * (hd + protrude),
            .face_oz = in_z * (hd + protrude),
            .angle = angle,
            .base = base,
            /* Per-building palette warmth plus coarse (building) and
             * fine (window) phase offsets spread the day/night cycle
             * so facades do not pulse in lockstep. */
            .bldg_phase = (city_rng(base + 50u) - 0.5f) * 0.06f,
            .warmth = city_rng(base + 200u),
            .wcols = wcols, .wrows = wrows,
            .win_hw = cell_w * 0.20f,
            .win_hh = cell_h * 0.22f,
        };
        draw_building_windows(&win_geom, anim_time);
    }

    /* Restore the NV fog distance mode explicitly — GL_FOG_DISTANCE_MODE_NV
     * isn't in the Khronos GL_FOG_BIT spec so the outer glPopAttrib may
     * not put it back. Conservative even on drivers that DO save it. */
    if (nv_fog_distance_supported)
        glFogi(GL_FOG_DISTANCE_MODE_NV, saved_nv_fog_mode);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    scene_backdrop_pop_state();
}

static void draw_starry_sky(
    float anim_time,
    int point_parameter_supported,
    void (APIENTRY *point_parameter_proc)(GLenum pname, const GLfloat *params)) {
    scene_backdrop_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POINT_SMOOTH);
    glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
    glDisable(GL_FOG);
    /* Override prior point attenuation with identity so stars render at
     * fixed sizes (the per-band glPointSize values below) regardless of
     * camera distance. The init bootstrap sets a non-identity quadratic
     * default (1/distance footprint scaling) for normal scene point rendering;
     * without this override stars would attenuate noticeably across the
     * STAR_SKY_RADIUS dome. Gated on the runtime capability + loaded
     * proc the controller mirrored into the config — unsupported
     * contexts can't have run the init-bootstrap call either, so the GL
     * default (identity) is already in effect and no reset is needed. */
    if (point_parameter_supported && point_parameter_proc)
        point_parameter_proc(GL_POINT_DISTANCE_ATTENUATION, (GLfloat[]){1, 0, 0});

    /* Strip camera translation so stars follow rotation only (no zoom pop). */
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    {
        GLfloat mv[16];
        glGetFloatv(GL_MODELVIEW_MATRIX, mv);
        mv[12] = 0.0f; mv[13] = 0.0f; mv[14] = 0.0f;
        glLoadMatrixf(mv);
    }

    /* Four point-size bands; cumulative cutoffs are the STAR_BAND_CUT_* above. */
    static const float band_sizes[4] = { 1.5f, 2.0f, 3.0f, 4.5f };
    const int band_cuts[5] = {
        0,
        (int)(STAR_COUNT * STAR_BAND_CUT_SMALL),
        (int)(STAR_COUNT * STAR_BAND_CUT_MED),
        (int)(STAR_COUNT * STAR_BAND_CUT_LARGE),
        STAR_COUNT,
    };

    for (int bi = 0; bi < 4; bi++) {
        glPointSize(band_sizes[bi]);
        glBegin(GL_POINTS);

        for (int i = band_cuts[bi]; i < band_cuts[bi + 1]; i++) {
            unsigned int base = (unsigned int)(i * STAR_RNG_STRIDE + STAR_RNG_SEED);

            /* Spherical coords: theta all-around, phi mostly above horizon */
            float theta = city_rng(base + 1u) * 2.0f * (float)M_PI;
            float phi   = city_rng(base + 2u) * 0.80f * (float)M_PI;
            float sp = sinf(phi), cp = cosf(phi);

            float sx = sp * cosf(theta) * STAR_SKY_RADIUS;
            float sy = cp * STAR_SKY_RADIUS;
            float sz = sp * sinf(theta) * STAR_SKY_RADIUS;

            /* Retro-80s palette: off-white, neon blue, purple/violet */
            float color_roll = city_rng(base + 3u);
            float sr, sg, sb;
            if (color_roll < 0.45f) {
                float w = city_rng(base + 10u);
                sr = 0.88f + w * 0.10f;
                sg = 0.88f + w * 0.06f;
                sb = 0.94f + w * 0.04f;
            } else if (color_roll < 0.75f) {
                float b = city_rng(base + 11u);
                sr = 0.22f + b * 0.18f;
                sg = 0.52f + b * 0.22f;
                sb = 1.0f;
            } else {
                float p = city_rng(base + 12u);
                sr = 0.52f + p * 0.22f;
                sg = 0.12f + p * 0.14f;
                sb = 0.88f + p * 0.10f;
            }

            /* ~35% blink slowly; the rest have a faint atmospheric shimmer */
            float alpha;
            float blink_roll = city_rng(base + 4u);
            if (blink_roll > 0.65f) {
                float phase = city_rng(base + 5u) * 2.0f * (float)M_PI;
                float speed = 0.05f + city_rng(base + 6u) * 0.28f;
                float blink = 0.5f + 0.5f * sinf(anim_time * speed * 2.0f * (float)M_PI + phase);
                alpha = 0.38f + 0.62f * blink;
            } else {
                float phase = city_rng(base + 5u) * 2.0f * (float)M_PI;
                alpha = 0.82f + 0.10f * sinf(anim_time * 2.7f + phase);
            }

            glColor4f(sr, sg, sb, alpha);
            glVertex3f(sx, sy, sz);
        }

        glEnd();
    }

    glPopMatrix();
    scene_backdrop_pop_state();
}

/* Piecewise-linear vertical sky gradient for the sunset dome. h is
 * normalized elevation: 0 = horizon, 1 = zenith, negative = below the
 * horizon. Stops run dark under-horizon -> hot magenta horizon glow ->
 * dusk purple -> near-black night blue at the zenith. */
static void sunset_sky_color(float h, float *r, float *g, float *b) {
    static const struct { float h, r, g, b; } stops[] = {
        { -0.12f, 0.05f, 0.02f, 0.08f },
        {  0.00f, 0.46f, 0.10f, 0.34f },
        {  0.16f, 0.28f, 0.08f, 0.33f },
        {  0.45f, 0.09f, 0.05f, 0.19f },
        {  1.00f, 0.02f, 0.02f, 0.09f },
    };
    const int n = (int)(sizeof(stops) / sizeof(stops[0]));

    if (h <= stops[0].h) { *r = stops[0].r; *g = stops[0].g; *b = stops[0].b; return; }
    for (int i = 1; i < n; i++) {
        if (h <= stops[i].h) {
            float f = (h - stops[i - 1].h) / (stops[i].h - stops[i - 1].h);
            *r = stops[i - 1].r + (stops[i].r - stops[i - 1].r) * f;
            *g = stops[i - 1].g + (stops[i].g - stops[i - 1].g) * f;
            *b = stops[i - 1].b + (stops[i].b - stops[i - 1].b) * f;
            return;
        }
    }
    *r = stops[n - 1].r; *g = stops[n - 1].g; *b = stops[n - 1].b;
}

/* Gradient dome. The sun sits at azimuth theta = pi (toward -Z); a warm
 * sunward glow term brightens the horizon around it so the brightest
 * sky hugs the sun instead of ringing the whole horizon uniformly. */
static void draw_sunset_sky_dome(void) {
    for (int ri = 0; ri < SUNSET_SKY_RINGS; ri++) {
        float f0 = (float)ri / (float)SUNSET_SKY_RINGS;
        float f1 = (float)(ri + 1) / (float)SUNSET_SKY_RINGS;
        float phi0 = SUNSET_PHI_MIN + (SUNSET_PHI_MAX - SUNSET_PHI_MIN) * f0;
        float phi1 = SUNSET_PHI_MIN + (SUNSET_PHI_MAX - SUNSET_PHI_MIN) * f1;

        glBegin(GL_QUAD_STRIP);
        for (int s = 0; s <= SUNSET_SKY_SEGS; s++) {
            float theta = ((float)s / (float)SUNSET_SKY_SEGS) * 2.0f * (float)M_PI;
            float st = sinf(theta), ct = cosf(theta);

            for (int e = 0; e < 2; e++) {
                float phi = e ? phi0 : phi1;
                float sp = sinf(phi), cp = cosf(phi);
                float h = sp;

                float r, g, b;
                sunset_sky_color(h, &r, &g, &b);

                /* Sunward horizon glow: horizontal alignment with the
                 * -Z sun azimuth, faded out away from the horizon. */
                float align = -ct;
                if (align > 0.0f) {
                    float vfall = 1.0f - fabsf(h) * 2.6f;
                    if (vfall > 0.0f) {
                        float glow = align * align * align * align * vfall;
                        r += 0.50f * glow;
                        g += 0.16f * glow;
                        b += 0.10f * glow;
                    }
                }

                glColor4f(r, g, b, 1.0f);
                glVertex3f(cp * st * SUNSET_SKY_RADIUS,
                           sp * SUNSET_SKY_RADIUS,
                           cp * ct * SUNSET_SKY_RADIUS);
            }
        }
        glEnd();
    }
}

/* The retro sun: a vertical stack of horizontal slices across the disc,
 * pink at the base ramping to gold at the top, with animated scanline
 * gaps opening across the lower half. Drawn as a flat camera-distant
 * quad toward -Z; with the camera translation stripped the viewer is
 * always at the origin, so the face-on error stays negligible. */
static void draw_sunset_sun(float anim_time) {
    float cy = SUNSET_SUN_ELEV;

    /* Soft additive bloom behind the disc, pulsing gently. */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    {
        float pulse = 0.80f + 0.20f * sinf(anim_time * 0.8f);
        float glow_r = SUNSET_SUN_RADIUS * 2.1f;
        glBegin(GL_TRIANGLE_FAN);
        glColor4f(1.0f, 0.42f, 0.50f, 0.30f * pulse);
        glVertex3f(0.0f, cy, -SUNSET_SUN_DIST);
        glColor4f(1.0f, 0.30f, 0.45f, 0.0f);
        for (int s = 0; s <= 40; s++) {
            float a = ((float)s / 40.0f) * 2.0f * (float)M_PI;
            glVertex3f(cosf(a) * glow_r, cy + sinf(a) * glow_r,
                       -SUNSET_SUN_DIST);
        }
        glEnd();
    }
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBegin(GL_QUADS);
    for (int j = 0; j < SUNSET_SUN_BANDS; j++) {
        float v0 = -1.0f + 2.0f * (float)j / (float)SUNSET_SUN_BANDS;
        float v1 = -1.0f + 2.0f * (float)(j + 1) / (float)SUNSET_SUN_BANDS;
        float vc = 0.5f * (v0 + v1);

        /* Scanline gap roll: stripes scroll downward; the gap fraction
         * of each stripe widens toward the bottom of the disc. */
        if (vc < SUNSET_GAP_START_V) {
            float gap = (SUNSET_GAP_START_V - vc) * 0.42f;
            if (gap > 0.46f) gap = 0.46f;
            float s = vc * SUNSET_STRIPE_FREQ - anim_time * SUNSET_STRIPE_SCROLL;
            float frac = s - floorf(s);
            if (frac < gap) continue;
        }

        float hw0 = SUNSET_SUN_RADIUS * sqrtf(1.0f - v0 * v0);
        float hw1 = SUNSET_SUN_RADIUS * sqrtf(1.0f - v1 * v1);
        float y0_band = cy + v0 * SUNSET_SUN_RADIUS;
        float y1_band = cy + v1 * SUNSET_SUN_RADIUS;

        /* Set into the horizon: drop bands fully below the grid plane
         * and clamp the straddler, so the disc doesn't show through
         * the floor as a phantom reflection. */
        if (y1_band <= 0.0f) continue;
        if (y0_band < 0.0f) y0_band = 0.0f;

        /* Pink base -> golden crown. */
        float cw = (vc + 1.0f) * 0.5f;
        float r = 1.0f;
        float g = 0.22f + 0.66f * cw;
        float b = 0.52f - 0.18f * cw;

        glColor4f(r, g, b, 1.0f);
        glVertex3f(-hw0, y0_band, -SUNSET_SUN_DIST);
        glVertex3f( hw0, y0_band, -SUNSET_SUN_DIST);
        glVertex3f( hw1, y1_band, -SUNSET_SUN_DIST);
        glVertex3f(-hw1, y1_band, -SUNSET_SUN_DIST);
    }
    glEnd();
}

/* Sparse warm stars confined to the dark upper sky so they read against
 * the night-blue zenith, not the horizon glow. */
static void draw_sunset_stars(float anim_time) {
    static const float band_sizes[2] = { 1.8f, 3.2f };
    const int band_cut = (int)(SUNSET_STAR_COUNT * 0.72f);

    for (int bi = 0; bi < 2; bi++) {
        glPointSize(band_sizes[bi]);
        glBegin(GL_POINTS);
        int lo = bi ? band_cut : 0;
        int hi = bi ? SUNSET_STAR_COUNT : band_cut;
        for (int i = lo; i < hi; i++) {
            unsigned int base =
                (unsigned int)i * SUNSET_STAR_STRIDE + SUNSET_STAR_SEED;

            float theta = city_rng(base + 1u) * 2.0f * (float)M_PI;
            /* phi measured from the zenith; cap keeps stars above ~55deg
             * elevation, clear of the gradient's bright band. */
            float phi = city_rng(base + 2u) * 0.36f * (float)M_PI;
            float sp = sinf(phi), cp = cosf(phi);

            /* Warm dusk palette: champagne white / pale gold / soft pink */
            float roll = city_rng(base + 3u);
            float sr, sg, sb;
            if (roll < 0.5f) {
                float w = city_rng(base + 10u);
                sr = 0.92f + w * 0.08f; sg = 0.86f + w * 0.08f; sb = 0.80f + w * 0.10f;
            } else if (roll < 0.8f) {
                float w = city_rng(base + 11u);
                sr = 1.0f; sg = 0.78f + w * 0.12f; sb = 0.42f + w * 0.16f;
            } else {
                float w = city_rng(base + 12u);
                sr = 1.0f; sg = 0.52f + w * 0.16f; sb = 0.62f + w * 0.18f;
            }

            float phase = city_rng(base + 4u) * 2.0f * (float)M_PI;
            float speed = 0.08f + city_rng(base + 5u) * 0.30f;
            float blink = 0.5f + 0.5f * sinf(anim_time * speed * 2.0f * (float)M_PI + phase);
            float alpha = 0.30f + 0.65f * blink;

            glColor4f(sr, sg, sb, alpha);
            glVertex3f(sp * cosf(theta) * (SUNSET_SKY_RADIUS - 1.0f),
                       cp * (SUNSET_SKY_RADIUS - 1.0f),
                       sp * sinf(theta) * (SUNSET_SKY_RADIUS - 1.0f));
        }
        glEnd();
    }
}

static void draw_sunset(
    float anim_time,
    int point_parameter_supported,
    void (APIENTRY *point_parameter_proc)(GLenum pname, const GLfloat *params)) {
    scene_backdrop_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POINT_SMOOTH);
    glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
    glDisable(GL_FOG);
    /* Same fixed-size star rationale as draw_starry_sky: identity
     * attenuation so the dome stars keep their band sizes. */
    if (point_parameter_supported && point_parameter_proc)
        point_parameter_proc(GL_POINT_DISTANCE_ATTENUATION, (GLfloat[]){1, 0, 0});

    /* Strip camera translation so the sky follows rotation only. */
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    {
        GLfloat mv[16];
        glGetFloatv(GL_MODELVIEW_MATRIX, mv);
        mv[12] = 0.0f; mv[13] = 0.0f; mv[14] = 0.0f;
        glLoadMatrixf(mv);
    }

    draw_sunset_sky_dome();
    draw_sunset_stars(anim_time);
    draw_sunset_sun(anim_time);

    glPopMatrix();
    scene_backdrop_pop_state();
}

void scene_backdrop_render(const SceneFrameRenderContext *frame_ctx) {
    switch (frame_ctx->config.backdrop_mode) {
    case SCENE_BACKDROP_CITYSCAPE:
        draw_cityscape(frame_ctx->config.anim_time,
                       frame_ctx->config.nv_fog_distance_supported);
        break;
    case SCENE_BACKDROP_STARS:
        draw_starry_sky(frame_ctx->config.anim_time,
                        frame_ctx->config.point_parameter_supported,
                        frame_ctx->config.point_parameter_proc);
        break;
    case SCENE_BACKDROP_CITY_AND_STARS:
        /* Stars first so city geometry writes depth over them. */
        draw_starry_sky(frame_ctx->config.anim_time,
                        frame_ctx->config.point_parameter_supported,
                        frame_ctx->config.point_parameter_proc);
        draw_cityscape(frame_ctx->config.anim_time,
                       frame_ctx->config.nv_fog_distance_supported);
        break;
    case SCENE_BACKDROP_SUNSET:
        draw_sunset(frame_ctx->config.anim_time,
                    frame_ctx->config.point_parameter_supported,
                    frame_ctx->config.point_parameter_proc);
        break;
    case SCENE_BACKDROP_OFF:
    default:
        break;
    }
}
