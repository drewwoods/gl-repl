/*
 * backdrop.c - optional 3D backdrop renderers for the 3D scene.
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

/* Sky dome radius sits just inside the star dome so the two never z-fight;
 * the sun plane sits inside the dome. */
#define SUNSET_SKY_RADIUS   29.0f
#define SUNSET_SKY_SEGS     48
/* Full sphere like the polar-day dome: the gradient clamps to its dark
 * under-horizon stop below the horizon, so closing the lower hemisphere
 * (PHI_MIN = -90deg) leaves no bottom edge for a look-down camera to
 * find. The doubled ring count keeps the above-horizon gradient finely
 * sampled across the full elevation span. */
#define SUNSET_SKY_RINGS    32
/* Dome elevation range in radians: straight down (-90deg) to zenith. */
#define SUNSET_PHI_MIN     (-0.50f * (float)M_PI)
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

/* --- Nebula backdrop --- */

/* Cloud dome sits just inside the star dome; the starfield (drawn
 * after, no depth writes) reads as shining through the gas. */
#define NEBULA_SKY_RADIUS   28.0f
#define NEBULA_PUFF_COUNT   110
#define NEBULA_PUFF_SEGS    12
/* Half-width of the galactic band, radians of elevation off the
 * band's great circle; puffs scatter inside it triangularly. */
#define NEBULA_BAND_SPREAD  0.38f
#define NEBULA_FLARE_COUNT  6

#define NEBULA_RNG_STRIDE   29u
#define NEBULA_RNG_SEED     0x4EB1u
/* --- Polar Day / Snowfall backdrops --- */

/* Snow flakes live in a camera-centred cylinder (the sky-point state
 * strips the camera translation, so the viewer is always inside the
 * fall volume). Y span starts below the grid plane so flakes don't
 * pop out at eye level on shallow camera angles. */
#define SNOW_COUNT        1500
#define SNOW_RADIUS       16.0f
#define SNOW_Y_MIN        -2.0f
#define SNOW_Y_SPAN       14.0f
#define SNOW_RNG_STRIDE   9u
#define SNOW_RNG_SEED     0x1CEDu

/* Cumulative flake-size band cutoffs as fractions of SNOW_COUNT:
 * small/far (0..55%), medium (55..85%), large/near (85..100%). */
#define SNOW_BAND_CUT_SMALL 0.55f
#define SNOW_BAND_CUT_MED   0.85f

static void render3d_backdrop_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void render3d_backdrop_pop_state(void) {
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

/* --- draw_cityscape stages ---
 *
 * City rendering has four concerns: GL setup
 * (lighting/blend/fog), per-building geometry math, the two tiers of
 * box-quad emission (5 colored faces each), and the 2D grid of
 * window-quads with per-window lit-ness and color rolls. These are split
 * across setup_city_gl_state / draw_building_box / draw_building_windows,
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
    render3d_backdrop_push_state();
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
         * the 0.65/0.60 cell sizes constrain wcols in [2, 5] and
         * wrows in [3, 14]. The upper clamp on wrows is the only
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

    /* Restore the NV fog distance mode explicitly - GL_FOG_DISTANCE_MODE_NV
     * isn't in the Khronos GL_FOG_BIT spec so the outer glPopAttrib may
     * not put it back. Conservative even on drivers that DO save it. */
    if (nv_fog_distance_supported)
        glFogi(GL_FOG_DISTANCE_MODE_NV, saved_nv_fog_mode);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    render3d_backdrop_pop_state();
}

/* Shared GL-state preamble for the point-based sky domes (starry sky,
 * sunset). Pushes backdrop state, sets up unlit blended point-smooth
 * rendering, then:
 *
 * - Overrides prior point attenuation with identity so dome stars
 *   render at fixed sizes (the per-band glPointSize values) regardless
 *   of camera distance. The init bootstrap sets a non-identity
 *   quadratic default (1/distance footprint scaling) for normal scene
 *   point rendering; without this override stars would attenuate
 *   noticeably across the STAR_SKY_RADIUS dome. Gated on the runtime
 *   capability + loaded proc the caller supplied in the config -
 *   unsupported contexts can't have run the init-bootstrap call either,
 *   so the GL default (identity) is already in effect and no reset is
 *   needed.
 * - Strips the camera translation from the modelview (under a
 *   glPushMatrix) so the sky follows rotation only - no zoom pop.
 *
 * Pair every call with backdrop_end_sky_point_state(). */
static void backdrop_begin_sky_point_state(
    int point_parameter_supported,
    void (APIENTRY *point_parameter_proc)(GLenum pname, const GLfloat *params)) {
    render3d_backdrop_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POINT_SMOOTH);
    glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
    glDisable(GL_FOG);

    if (point_parameter_supported && point_parameter_proc)
        point_parameter_proc(GL_POINT_DISTANCE_ATTENUATION, (GLfloat[]){1, 0, 0});

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    {
        GLfloat mv[16];
        glGetFloatv(GL_MODELVIEW_MATRIX, mv);
        mv[12] = 0.0f; mv[13] = 0.0f; mv[14] = 0.0f;
        glLoadMatrixf(mv);
    }
}

static void backdrop_end_sky_point_state(void) {
    glPopMatrix();
    render3d_backdrop_pop_state();
}

static void draw_starry_sky(
    float anim_time,
    int point_parameter_supported,
    void (APIENTRY *point_parameter_proc)(GLenum pname, const GLfloat *params)) {
    backdrop_begin_sky_point_state(point_parameter_supported,
                                   point_parameter_proc);

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

    backdrop_end_sky_point_state();
}

/* Piecewise-linear vertical sky gradient for the sunset dome. h is
 * normalized elevation: 0 = horizon, 1 = zenith, negative = below the
 * horizon. Stops run dark under-horizon -> hot magenta horizon glow ->
 * dusk purple -> near-black night blue at the zenith. */
static void sunset_sky_color(float h, float *r, float *g, float *b) {
    static const struct { float h, r, g, b; } stops[] = {
        { -0.32f, 0.05f, 0.02f, 0.08f },
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
    backdrop_begin_sky_point_state(point_parameter_supported,
                                   point_parameter_proc);

    draw_sunset_sky_dome();
    draw_sunset_stars(anim_time);
    draw_sunset_sun(anim_time);

    backdrop_end_sky_point_state();
}

static void draw_aurora(float anim_time, float alpha_scale, float extent) {
    render3d_backdrop_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    /* Additive blend so overlapping folds brighten like real curtains. */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDisable(GL_FOG);

    const float t      = anim_time;
    const float as     = alpha_scale;
    const int   steps  = 256;

    for (int c = 0; c < 2; c++) {
        float ph       = (float)c * 2.6f;
        float z0       = (c == 0 ? -0.55f : -0.30f) * extent;
        float h_base   = extent * (0.15f + 0.08f * (float)c);
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
            glColor4f(0.18f, 0.95f, 0.50f,
                      fminf(0.30f * (0.45f + 0.55f * fold01) * edge * as,
                            1.0f));
            glVertex3f(x, y_lo, z);
            /* top: violet fringe fading to nothing */
            glColor4f(0.50f, 0.25f, 0.90f, 0.0f);
            glVertex3f(x, y_hi, z);
        }
        glEnd();
    }
    render3d_backdrop_pop_state();
}

/* --- Nebula: deep-space gas clouds along a tilted galactic band ---
 *
 * The companion sky to the Star Chart grid (violet/teal glow over its
 * gold chart marks). Additive-blended soft discs ("puffs") scatter
 * along a tilted great-circle band across the sky dome in three color
 * families (magenta, indigo, teal), each slowly pulsing and drifting
 * along the band; a handful of bright flare stars with diffraction
 * spikes anchor the composition. The base starfield is composed on
 * top by the dispatch (same pattern as CITY_AND_STARS). */

/* Orthonormal band frame: e3 is the band pole (the band's great
 * circle is the plane normal to it), e1/e2 span that plane. Constants
 * chosen so the band crosses the sky diagonally. */
static const float k_nebula_e1[3] = { -0.80f, 0.00f, 0.60f };
static const float k_nebula_e2[3] = {  0.48f, -0.60f, 0.64f };
static const float k_nebula_e3[3] = {  0.36f, 0.80f, 0.48f };

/* Direction on the unit sphere for band coordinate (theta, phi_off):
 * theta runs along the band's great circle, phi_off is elevation off
 * it toward the pole. */
static void nebula_band_dir(float theta, float phi_off, float out[3]) {
    float ct = cosf(theta), st = sinf(theta);
    float cp = cosf(phi_off), sp = sinf(phi_off);
    for (int k = 0; k < 3; k++)
        out[k] = cp * (ct * k_nebula_e1[k] + st * k_nebula_e2[k])
               + sp * k_nebula_e3[k];
}

/* Tangent basis at dome direction d (unit): t1 along the band, t2
 * completing the frame. d never reaches the band pole (|phi_off| is
 * capped well below pi/2), so the cross product stays well-formed. */
static void nebula_tangent_frame(const float d[3], float t1[3], float t2[3]) {
    /* t1 = normalize(e3 x d) */
    t1[0] = k_nebula_e3[1] * d[2] - k_nebula_e3[2] * d[1];
    t1[1] = k_nebula_e3[2] * d[0] - k_nebula_e3[0] * d[2];
    t1[2] = k_nebula_e3[0] * d[1] - k_nebula_e3[1] * d[0];
    float len = sqrtf(t1[0] * t1[0] + t1[1] * t1[1] + t1[2] * t1[2]);
    if (len < 1e-5f) len = 1e-5f;
    for (int k = 0; k < 3; k++) t1[k] /= len;
    /* t2 = d x t1 (unit: d and t1 are unit and orthogonal) */
    t2[0] = d[1] * t1[2] - d[2] * t1[1];
    t2[1] = d[2] * t1[0] - d[0] * t1[2];
    t2[2] = d[0] * t1[1] - d[1] * t1[0];
}

/* One soft gas disc: center-bright triangle fan in the tangent plane,
 * rim alpha 0 so puffs have no border and stack additively. */
static void nebula_draw_puff(const float d[3], float radius,
                             float r, float g, float b, float a) {
    float t1[3], t2[3];
    nebula_tangent_frame(d, t1, t2);
    float cx = d[0] * NEBULA_SKY_RADIUS;
    float cy = d[1] * NEBULA_SKY_RADIUS;
    float cz = d[2] * NEBULA_SKY_RADIUS;
    glBegin(GL_TRIANGLE_FAN);
    glColor4f(r, g, b, a);
    glVertex3f(cx, cy, cz);
    glColor4f(r, g, b, 0.0f);
    for (int s = 0; s <= NEBULA_PUFF_SEGS; s++) {
        float ang = ((float)s / (float)NEBULA_PUFF_SEGS) * 2.0f * (float)M_PI;
        float ca = cosf(ang) * radius, sa = sinf(ang) * radius;
        glVertex3f(cx + t1[0] * ca + t2[0] * sa,
                   cy + t1[1] * ca + t2[1] * sa,
                   cz + t1[2] * ca + t2[2] * sa);
    }
    glEnd();
}

static void nebula_draw_clouds(float anim_time) {
    for (int i = 0; i < NEBULA_PUFF_COUNT; i++) {
        unsigned int base =
            (unsigned int)i * NEBULA_RNG_STRIDE + NEBULA_RNG_SEED;

        /* Along-band position with a very slow per-puff drift. */
        float theta = city_rng(base + 1u) * 2.0f * (float)M_PI
                    + anim_time * (0.004f + city_rng(base + 2u) * 0.008f);
        /* Triangular falloff off the band's spine. */
        float phi_off = (city_rng(base + 3u) + city_rng(base + 4u) - 1.0f)
                      * NEBULA_BAND_SPREAD;
        float d[3];
        nebula_band_dir(theta, phi_off, d);
        /* Skip puffs sunk below the horizon: the grid floor owns that
         * half of the view, and the dome dips only slightly under it. */
        if (d[1] < -0.05f) continue;

        float radius = NEBULA_SKY_RADIUS * (0.10f + city_rng(base + 5u) * 0.16f);

        /* Three cold gas families + per-puff jitter. */
        float roll = city_rng(base + 6u);
        float jit = city_rng(base + 7u) * 0.10f;
        float r, g, b;
        if (roll < 0.40f)      { r = 0.58f + jit; g = 0.14f; b = 0.50f + jit; }
        else if (roll < 0.75f) { r = 0.20f + jit; g = 0.18f; b = 0.58f + jit; }
        else                   { r = 0.08f; g = 0.42f + jit; b = 0.52f + jit; }

        float phase = city_rng(base + 8u) * 2.0f * (float)M_PI;
        float speed = 0.05f + city_rng(base + 9u) * 0.18f;
        float pulse = 0.80f + 0.20f * sinf(anim_time * speed * 2.0f * (float)M_PI
                                           + phase);
        float a = (0.045f + city_rng(base + 10u) * 0.075f) * pulse;

        nebula_draw_puff(d, radius, r, g, b, a);
    }
}

/* A few bright "named" stars inside the band: a point core plus four
 * diffraction spikes fading to nothing, slowly twinkling. */
static void nebula_draw_flares(float anim_time) {
    for (int i = 0; i < NEBULA_FLARE_COUNT; i++) {
        unsigned int base =
            (unsigned int)(i + 600) * NEBULA_RNG_STRIDE + NEBULA_RNG_SEED;

        float theta = city_rng(base + 1u) * 2.0f * (float)M_PI;
        float phi_off = (city_rng(base + 2u) - 0.5f) * NEBULA_BAND_SPREAD;
        float d[3];
        nebula_band_dir(theta, phi_off, d);
        if (d[1] < 0.12f) continue;   /* keep flares clear of the floor */

        float t1[3], t2[3];
        nebula_tangent_frame(d, t1, t2);
        float cx = d[0] * NEBULA_SKY_RADIUS;
        float cy = d[1] * NEBULA_SKY_RADIUS;
        float cz = d[2] * NEBULA_SKY_RADIUS;

        float tw = 0.5f + 0.5f * sinf(anim_time *
                                      (0.25f + city_rng(base + 3u) * 0.45f)
                                      * 2.0f * (float)M_PI
                                      + city_rng(base + 4u) * 6.28318f);
        float a = 0.45f + 0.50f * tw;
        float len = NEBULA_SKY_RADIUS * (0.045f + city_rng(base + 5u) * 0.045f)
                  * (0.8f + 0.4f * tw);
        /* warm-white core with a faint violet cast */
        float r = 0.95f, g = 0.90f, b = 1.00f;

        glBegin(GL_LINES);
        for (int s = 0; s < 4; s++) {
            const float *ax = (s < 2) ? t1 : t2;
            float sgn = (s & 1) ? -1.0f : 1.0f;
            glColor4f(r, g, b, a * 0.85f);
            glVertex3f(cx, cy, cz);
            glColor4f(r, g, b, 0.0f);
            glVertex3f(cx + ax[0] * len * sgn,
                       cy + ax[1] * len * sgn,
                       cz + ax[2] * len * sgn);
        }
        glEnd();

        glPointSize(5.0f);
        glBegin(GL_POINTS);
        glColor4f(r, g, b, a);
        glVertex3f(cx, cy, cz);
        glEnd();
    }
}

static void draw_nebula(
    float anim_time,
    int point_parameter_supported,
    void (APIENTRY *point_parameter_proc)(GLenum pname, const GLfloat *params)) {
    backdrop_begin_sky_point_state(point_parameter_supported,
                                   point_parameter_proc);

    /* Additive blend: overlapping gas brightens like emission nebulae. */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    nebula_draw_clouds(anim_time);
    nebula_draw_flares(anim_time);

    backdrop_end_sky_point_state();
}

/* Piecewise-linear vertical gradient for the polar-day dome: a cold
 * whiteout. The horizon stop is exactly RENDER3D_GLACIAL_TINT - the same
 * colour the Frozen Lake grid fades to - so the grid dissolves into
 * the sky with no seam at the grid extent. Above it the sky cools
 * through powder blue to a steel-blue zenith. Below the eye-level
 * horizon the table clamps to the constant glacial tint and the dome
 * extends well downward (POLAR_PHI_MIN): the dome is camera-centred,
 * so a rim pinned at the horizon leaves a void over the beyond-grid
 * region that grows with camera altitude, and any partial floor leaves
 * a visible bottom edge at steep look-down angles - the haze must
 * close the full lower hemisphere to be edge-free from every camera.
 * The backdrop draws before the grid, so the grid (and its FAR
 * white-out, which is this same tint) renders over it seamlessly. */
static void polar_sky_color(float h, float *r, float *g, float *b) {
    static const struct { float h, r, g, b; } stops[] = {
        {  -1.00f, 0.12f, 0.24f, 0.32f },
        {  0.00f, RENDER3D_GLACIAL_TINT_R, RENDER3D_GLACIAL_TINT_G,
                  RENDER3D_GLACIAL_TINT_B },
        {  0.18f, 0.50f, 0.66f, 0.82f },
        {  0.55f, 0.34f, 0.48f, 0.68f },
        {  1.00f, 0.22f, 0.34f, 0.52f },
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

/* Full sphere: the glacial haze closes straight down (-90 deg), so no
 * bottom edge exists for any look-down angle to find (see
 * polar_sky_color note). Extra rings keep the above-horizon gradient
 * as finely sampled as the sunset dome's despite the doubled span. */
#define POLAR_PHI_MIN (-0.50f * (float)M_PI)
#define POLAR_SKY_RINGS 32

/* Gradient dome sharing the sunset dome's radius/tessellation; same
 * ring/strip walk minus the sunward glow term (a polar sky is
 * directionless) and starting at the horizon instead of below it. */
static void draw_polar_sky_dome(void) {
    for (int ri = 0; ri < POLAR_SKY_RINGS; ri++) {
        float f0 = (float)ri / (float)POLAR_SKY_RINGS;
        float f1 = (float)(ri + 1) / (float)POLAR_SKY_RINGS;
        float phi0 = POLAR_PHI_MIN + (SUNSET_PHI_MAX - POLAR_PHI_MIN) * f0;
        float phi1 = POLAR_PHI_MIN + (SUNSET_PHI_MAX - POLAR_PHI_MIN) * f1;

        glBegin(GL_QUAD_STRIP);
        for (int s = 0; s <= SUNSET_SKY_SEGS; s++) {
            float theta = ((float)s / (float)SUNSET_SKY_SEGS) * 2.0f * (float)M_PI;
            float st = sinf(theta), ct = cosf(theta);

            for (int e = 0; e < 2; e++) {
                float phi = e ? phi0 : phi1;
                float sp = sinf(phi), cp = cosf(phi);

                float r, g, b;
                polar_sky_color(sp, &r, &g, &b);
                glColor4f(r, g, b, 1.0f);
                glVertex3f(cp * st * SUNSET_SKY_RADIUS,
                           sp * SUNSET_SKY_RADIUS,
                           cp * ct * SUNSET_SKY_RADIUS);
            }
        }
        glEnd();
    }
}

/* Falling snow: a camera-centred point cloud in three size bands
 * (small/far first so near flakes composite over them). Each flake's
 * column, fall speed, sway, and alpha come from the deterministic
 * hash; only the fall phase advances with anim_time, so pausing t
 * freezes the snow mid-air. Flakes fade in just after spawning at the
 * top of the span and fade out approaching the bottom, so the wrap
 * never pops. */
static void draw_snowfall(
    float anim_time,
    int point_parameter_supported,
    void (APIENTRY *point_parameter_proc)(GLenum pname, const GLfloat *params)) {
    static const float band_sizes[3] = { 2.0f, 3.0f, 4.5f };
    const int band_cuts[4] = {
        0,
        (int)(SNOW_COUNT * SNOW_BAND_CUT_SMALL),
        (int)(SNOW_COUNT * SNOW_BAND_CUT_MED),
        SNOW_COUNT,
    };

    backdrop_begin_sky_point_state(point_parameter_supported,
                                   point_parameter_proc);

    for (int bi = 0; bi < 3; bi++) {
        glPointSize(band_sizes[bi]);
        glBegin(GL_POINTS);

        for (int i = band_cuts[bi]; i < band_cuts[bi + 1]; i++) {
            unsigned int base = (unsigned int)(i * SNOW_RNG_STRIDE + SNOW_RNG_SEED);

            /* Column position: uniform over the disc (sqrt for area). */
            float ang = city_rng(base + 1u) * 2.0f * (float)M_PI;
            float rad = sqrtf(city_rng(base + 2u)) * SNOW_RADIUS;
            float cx = cosf(ang) * rad;
            float cz = sinf(ang) * rad;

            /* Near flakes (later bands) fall a touch faster - cheap
             * parallax against the slow far-band drift. */
            float speed = (0.7f + city_rng(base + 3u) * 0.7f) *
                          (1.0f + 0.25f * (float)bi);
            float phase = city_rng(base + 4u);
            float fy = phase - anim_time * speed / SNOW_Y_SPAN;
            fy -= floorf(fy);                /* wrap to [0,1): 1=top */
            float y = SNOW_Y_MIN + fy * SNOW_Y_SPAN;

            /* Lateral sway, frozen per flake except the slow drift. */
            float sway_ph = city_rng(base + 5u) * 2.0f * (float)M_PI;
            float sway_amp = 0.25f + city_rng(base + 6u) * 0.35f;
            float sway_spd = 0.4f + city_rng(base + 7u) * 0.5f;
            float sx = cx + sinf(anim_time * sway_spd + sway_ph) * sway_amp;
            float sz = cz + cosf(anim_time * sway_spd * 0.7f + sway_ph) *
                       sway_amp * 0.6f;

            float alpha = 0.45f + city_rng(base + 8u) * 0.35f;
            float fade_in  = (1.0f - fy) * 8.0f;
            float fade_out = fy * 10.0f;
            if (fade_in  < 1.0f) alpha *= fade_in;
            if (fade_out < 1.0f) alpha *= fade_out;

            glColor4f(0.92f, 0.95f, 1.0f, alpha);
            glVertex3f(sx, y, sz);
        }

        glEnd();
    }

    backdrop_end_sky_point_state();
}

/* Sunset environment lights, one row per slot: world-space position
 * (w=0 => directional), then diffuse / ambient / specular. Slots live
 * on GL_LIGHT4..6 - above the caller's user-facing GL_LIGHT0..3 range -
 * so lit geometry picks up the scene's colors without consuming any
 * user slot (fixed-function GL guarantees 8 lights). Intensities stay
 * moderate so an enabled user light still reads as the key. */
typedef struct BackdropEnvLight {
    GLenum  id;
    GLfloat pos[4];
    GLfloat diffuse[4];
    GLfloat ambient[4];
    GLfloat specular[4];
} BackdropEnvLight;

static const BackdropEnvLight k_sunset_lights[] = {
    /* Golden-pink sun key from the disc's direction (low, toward -Z). */
    { GL_LIGHT4,
      .pos      = { 0.0f,  5.0f, -24.0f, 0.0f },
      .diffuse  = { 0.85f, 0.42f, 0.28f, 1.0f },
      .ambient  = { 0.05f, 0.02f, 0.03f, 1.0f },
      .specular = { 1.00f, 0.60f, 0.40f, 1.0f } },
    /* Violet dusk-sky fill from high behind the viewer. */
    { GL_LIGHT5,
      .pos      = {  0.2f,  0.6f,  1.0f, 0.0f },
      .diffuse  = { 0.26f, 0.12f, 0.42f, 1.0f },
      .ambient  = { 0.02f, 0.01f, 0.04f, 1.0f },
      .specular = { 0.15f, 0.08f, 0.25f, 1.0f } },
    /* Hot-pink bounce off the neon floor, from below. */
    { GL_LIGHT6,
      .pos      = {  0.0f, -1.0f,  0.15f, 0.0f },
      .diffuse  = { 0.38f, 0.08f, 0.26f, 1.0f },
      .ambient  = { 0.00f, 0.00f, 0.00f, 1.0f },
      .specular = { 0.20f, 0.04f, 0.14f, 1.0f } },
};

/* Nebula environment lights: cold cosmic palette matched to the gas
 * families above - magenta key from high along the band, teal rim
 * from the opposite low quarter, and a deep-indigo bounce from below
 * standing in for the Star Chart floor's glow. Same GL_LIGHT4..6
 * contract as the sunset rig. */
static const BackdropEnvLight k_nebula_lights[] = {
    /* Magenta nebula key, high toward the band's bright side. */
    { GL_LIGHT4,
      .pos      = {  0.45f,  0.65f, -0.60f, 0.0f },
      .diffuse  = { 0.55f, 0.18f, 0.48f, 1.0f },
      .ambient  = { 0.04f, 0.01f, 0.04f, 1.0f },
      .specular = { 0.65f, 0.30f, 0.60f, 1.0f } },
    /* Teal gas rim from the opposite low quarter. */
    { GL_LIGHT5,
      .pos      = { -0.55f,  0.20f,  0.65f, 0.0f },
      .diffuse  = { 0.10f, 0.34f, 0.40f, 1.0f },
      .ambient  = { 0.01f, 0.02f, 0.03f, 1.0f },
      .specular = { 0.15f, 0.40f, 0.45f, 1.0f } },
    /* Indigo chart-floor bounce, from below. */
    { GL_LIGHT6,
      .pos      = {  0.00f, -1.00f,  0.10f, 0.0f },
      .diffuse  = { 0.14f, 0.12f, 0.34f, 1.0f },
      .ambient  = { 0.00f, 0.00f, 0.00f, 1.0f },
      .specular = { 0.08f, 0.07f, 0.20f, 1.0f } },
};

/* Polar-day environment lights: a cool, diffuse arctic palette matched
 * to the dome's glacial-tint -> steel-blue gradient. The dome is
 * directionless (no sun disc), so the key is zenith-down and neutral-
 * cool rather than warm-directional.  Same GL_LIGHT4..6 contract. */
static const BackdropEnvLight k_polar_day_lights[] = {
    /* Cool white zenith key, straight overhead - the open arctic sky. */
    { GL_LIGHT4,
      .pos      = { 1.0f,  0.2f,  0.0f, 0.0f },
      .diffuse  = { 0.28f, 0.36f, 0.42f, 1.0f },
      .ambient  = { 0.10f, 0.12f, 0.15f, 1.0f },
      .specular = { 0.22f, 0.34f, 0.48f, 1.0f } },
    /* Steel-blue horizon fill from low behind the viewer. */
    { GL_LIGHT5,
      .pos      = { 0.15f, 0.20f,  1.00f, 0.0f },
      .diffuse  = { 0.30f, 0.44f, 0.62f, 1.0f },
      .ambient  = { 0.02f, 0.03f, 0.05f, 1.0f },
      .specular = { 0.22f, 0.34f, 0.48f, 1.0f } },
    /* Pale glacial bounce from below - the ice-sheet reflection. */
    { GL_LIGHT6,
      .pos      = { -1.0f, -0.2f,  0.10f, 0.0f },
      .diffuse  = { 0.28f, 0.36f, 0.42f, 1.0f },
      .ambient  = { 0.00f, 0.00f, 0.00f, 1.0f },
      .specular = { 0.18f, 0.24f, 0.30f, 1.0f } },
    { GL_LIGHT7,
      .pos      = { -0.1f, -0.2f,  -1.0f, 0.0f },
      .diffuse  = { 0.28f, 0.36f, 0.42f, 1.0f },
      .ambient  = { 0.00f, 0.00f, 0.00f, 1.0f },
      .specular = { 0.18f, 0.24f, 0.30f, 1.0f } },
};

static void backdrop_apply_env_lights(const BackdropEnvLight *lights, int n) {
    for (int i = 0; i < n; i++) {
        glLightfv(lights[i].id, GL_POSITION, lights[i].pos);
        glLightfv(lights[i].id, GL_DIFFUSE,  lights[i].diffuse);
        glLightfv(lights[i].id, GL_AMBIENT,  lights[i].ambient);
        glLightfv(lights[i].id, GL_SPECULAR, lights[i].specular);
        glEnable(lights[i].id);
    }
}

/* Backdrop-owned colored lights. Runs in the pass setup phase (after
 * render3d_lights_setup, before user fill) so lit user geometry sees them;
 * unlike the user slots these are configured AND enabled here, since
 * the caller's program cannot reach GL_LIGHT4+. They contribute only once
 * the program enables GL_LIGHTING, and the pass's outer
 * glPushAttrib(GL_ALL_ATTRIB_BITS) bracket pops the enables at frame
 * end, so nothing leaks when the backdrop changes. Positions are
 * world-space: the modelview holds the camera at call time. */
void render3d_backdrop_setup_lights(const Render3dFrameRenderContext *frame_ctx) {
    switch (frame_ctx->config.backdrop_mode) {
    case RENDER3D_BACKDROP_SUNSET:
        backdrop_apply_env_lights(
            k_sunset_lights,
            (int)(sizeof(k_sunset_lights) / sizeof(k_sunset_lights[0])));
        break;
    case RENDER3D_BACKDROP_NEBULA:
        backdrop_apply_env_lights(
            k_nebula_lights,
            (int)(sizeof(k_nebula_lights) / sizeof(k_nebula_lights[0])));
        break;
    case RENDER3D_BACKDROP_POLAR_DAY:
    case RENDER3D_BACKDROP_POLAR_DAY_SNOW:
        backdrop_apply_env_lights(
            k_polar_day_lights,
            (int)(sizeof(k_polar_day_lights) / sizeof(k_polar_day_lights[0])));
        break;
    default:
        break;
    }
}

void render3d_backdrop_render(const Render3dFrameRenderContext *frame_ctx) {
    switch (frame_ctx->config.backdrop_mode) {
    case RENDER3D_BACKDROP_CITYSCAPE:
        draw_cityscape(frame_ctx->config.anim_time,
                       frame_ctx->config.nv_fog_distance_supported);
        break;
    case RENDER3D_BACKDROP_STARS:
        draw_starry_sky(frame_ctx->config.anim_time,
                        frame_ctx->config.point_parameter_supported,
                        frame_ctx->config.point_parameter_proc);
        break;
    case RENDER3D_BACKDROP_CITY_AND_STARS:
        /* Stars first so city geometry writes depth over them. */
        draw_starry_sky(frame_ctx->config.anim_time,
                        frame_ctx->config.point_parameter_supported,
                        frame_ctx->config.point_parameter_proc);
        draw_cityscape(frame_ctx->config.anim_time,
                       frame_ctx->config.nv_fog_distance_supported);
        break;
    case RENDER3D_BACKDROP_SUNSET:
        draw_sunset(frame_ctx->config.anim_time,
                    frame_ctx->config.point_parameter_supported,
                    frame_ctx->config.point_parameter_proc);
        break;
    case RENDER3D_BACKDROP_NEBULA:
        /* Gas first, then the shared starfield shines through on top
         * (no depth writes in either pass - draw order is the layering). */
        draw_nebula(frame_ctx->config.anim_time,
                    frame_ctx->config.point_parameter_supported,
                    frame_ctx->config.point_parameter_proc);
        draw_starry_sky(frame_ctx->config.anim_time,
                        frame_ctx->config.point_parameter_supported,
                        frame_ctx->config.point_parameter_proc);
        break;
    case RENDER3D_BACKDROP_AURORA: {
        int ex_i = frame_ctx->config.grid_extent_idx;
        if (ex_i < 0 || ex_i >= GRID_EXTENT_COUNT) ex_i = GRID_EXTENT_MID;
        float extent = frame_ctx->config.grid_extents[ex_i];
        draw_aurora(frame_ctx->config.anim_time,
                    frame_ctx->config.alpha_scale,
                    extent);
        break;
    }
    case RENDER3D_BACKDROP_POLAR_DAY:
        backdrop_begin_sky_point_state(
            frame_ctx->config.point_parameter_supported,
            frame_ctx->config.point_parameter_proc);
        draw_polar_sky_dome();
        backdrop_end_sky_point_state();
        break;
    case RENDER3D_BACKDROP_POLAR_DAY_SNOW:
        /* Dome first so the flakes composite over the sky. */
        backdrop_begin_sky_point_state(
            frame_ctx->config.point_parameter_supported,
            frame_ctx->config.point_parameter_proc);
        draw_polar_sky_dome();
        backdrop_end_sky_point_state();
        draw_snowfall(frame_ctx->config.anim_time,
                      frame_ctx->config.point_parameter_supported,
                      frame_ctx->config.point_parameter_proc);
        break;
    case RENDER3D_BACKDROP_OFF:
    default:
        break;
    }
}
