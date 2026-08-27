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

/* Vertical value ramp applied down every facade, as multipliers on the
 * building's base color at the ring's tallest point (top) and at street
 * level (bottom). Flat-valued boxes read as one paper cut-out wall: 300
 * buildings all at the same value give the eye nothing to separate the
 * near rank from the far one. Lifting the base (the city's own glow
 * pooling at street level) and dropping the tops toward the sky value
 * turns each box into a silhouette that overlaps its neighbours legibly,
 * with no extra geometry - the five face quads already carry per-vertex
 * color. The ramp is keyed to absolute height, not to each box, so a
 * tier-2 setback continues its tier-1 gradient instead of restarting it. */
#define CITY_RAMP_BOTTOM   1.55f
#define CITY_RAMP_TOP      0.42f
/* Height at which the ramp reaches CITY_RAMP_TOP, and clamps past it.
 * Keyed to the tallest *tier-1* box (H_MIN + H_RANGE) rather than to the
 * tallest box-plus-setback: heights are uniform over that range, so
 * spanning the full stack would leave the short-to-median buildings -
 * which is most of the ring - sitting in the ramp's flat bottom third
 * and still reading as cut-outs. Setbacks clamp at the dark end, which
 * is where a tower top wants to be anyway. */
#define CITY_RAMP_HEIGHT   (CITY_BLDG_H_MIN + CITY_BLDG_H_RANGE)

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

/* Lowest elevation a star may occupy, as a polar angle from the zenith.
 * Just past 90deg, so a level camera sees the field run into the horizon
 * with no visible edge, while a camera tilted down finds no stars
 * littered across the ground plane. The old 0.80*PI span put a third of
 * the field up to 54deg *below* the horizon, where the grid's
 * translucent lines let it show through. */
#define STAR_PHI_MAX      (0.53f * (float)M_PI)

/* Per-band alpha scale, applied on top of the twinkle term. Size alone
 * gave a flat field - a 1.5px star and a 4.5px star both landed near
 * opaque, so the sky read as even noise. Dimming with size restores a
 * magnitude distribution: the many small stars sit back, the few large
 * ones carry the sparkle. */
#define STAR_BAND_ALPHA_SMALL  0.40f
#define STAR_BAND_ALPHA_MED    0.60f
#define STAR_BAND_ALPHA_LARGE  0.80f
#define STAR_BAND_ALPHA_BRIGHT 1.00f

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

/* --- Drones backdrop ---
 *
 * Four scanner drones patrol the user's geometry on GL_LIGHT4..7 - the only
 * backdrop whose lights move. Everything about the rig is expressed in scene
 * radii (Render3dRenderConfig.geometry_bounds_fn), so it frames a 0.5-unit
 * scene and a 40-unit one alike; with no bounds available it falls back to a
 * fixed radius about the origin.
 *
 * Pose is a pure function of anim_time with no retained state. That is what
 * keeps it correct under replay and under accumulation blur, both of which
 * render the same frame several times at sub-step times - a rig that
 * integrated its own position would drift apart from itself between samples.
 */
#define DRONE_COUNT          4      /* GL_LIGHT4..7: the slots above the user's */
#define DRONE_PHASE_COUNT    3
#define DRONE_PHASE_SECS     9.0f
/* Fraction of a phase spent cross-fading into the next one. Both poses are
 * evaluated and lerped across the seam, so no drone ever teleports; the
 * incoming phase is asked for a slightly negative local time, which its
 * pose functions are all smooth over. */
#define DRONE_XFADE_FRAC     0.16f

/* Path geometry, all in multiples of the scene radius. */
#define DRONE_STRAFE_SPAN     5.0f  /* run length, entering and exiting offscreen */
#define DRONE_STRAFE_LANE     0.42f /* lateral spacing within the formation */
/* The formation flies PAST the scene, not through it: the run is displaced
 * sideways and upward by these many scene radii so no drone ever ends up
 * inside the geometry at mid-pass. */
/* The INNER lane is the one that matters: with the formation spread over
 * +-1.5 ranks, the closest lane sits at STANDOFF - 1.5*LANE, and that plus
 * the rise has to clear 1.0 (the corner of the measured box). At 1.40 it did
 * not - the inner drone passed at 0.95 radii, inside a scene that fills its
 * own bounding sphere. */
#define DRONE_STRAFE_STANDOFF 1.85f
#define DRONE_STRAFE_RISE     0.55f
#define DRONE_ORBIT_RADIUS    1.75f
#define DRONE_ORBIT_TURNS     1.25f /* revolutions per orbit phase */
#define DRONE_SPOT_DIST       2.40f /* station distance during the spot phase */

/* Hull: a wireframe hexagonal spindle cage with sensor collar, waist
 * equator, aft thruster ring, and outrigger stabilizer fins. Sized off
 * the scene so it stays readable in the light's own colour. */
#define DRONE_HULL_SIDES      6
#define DRONE_HULL_LEN        0.14f   /* x scene radius, lens to waist */
#define DRONE_HULL_WAIST      0.048f
#define DRONE_LINE_WIDTH      1.4f

/* Beam cone: length as a fraction of the distance to the aim point, and
 * its half-angle in the narrow (spot phase) and wide (transit) states.
 * The drawn cone and floor pool visualize the beam footprint directly. */
#define DRONE_BEAM_SEGS       18
#define DRONE_BEAM_NARROW_DEG 9.0f
#define DRONE_BEAM_WIDE_DEG   15.0f
#define DRONE_BEAM_ALPHA      0.038f /* peak alpha at the lens, spot phase */
#define DRONE_BEAM_TRANSIT_A  0.015f /* ... and while merely in transit */
/* The pool is the beam's footprint, so its radius follows from the same
 * angle - but a beam cast from a shallow angle would smear an ellipse to
 * the horizon, so it is capped at a multiple of the scene radius. */
#define DRONE_POOL_MAX_R      2.0f
#define DRONE_POOL_ALPHA      0.08f  /* peak alpha at the floor pool center, spot phase */
#define DRONE_POOL_TRANSIT_A  0.025f /* ... and while merely in transit */

/* Fixed-function spotlights are evaluated per VERTEX. To prevent sudden
 * vertex lighting cutoffs (especially when the drones zoom out and sweep
 * coarse geometry), GL_SPOT_CUTOFF is relaxed to a wide cone (DRONE_SPOT_CUTOFF_DEG)
 * while high spot exponents provide the smooth falloff: the exponential decay
 * drops naturally to near-zero long before reaching the hard cutoff boundary.
 * The exponent is lerped between wide/transit (45.0) and narrow/spot (120.0)
 * matching the drawn beam's visual footprint. */
#define DRONE_SPOT_CUTOFF_DEG 60.0f
#define DRONE_SPOT_EXP_WIDE   45.0f  /* spot exponent in wide / transit phase */
#define DRONE_SPOT_EXP_SPOT   120.0f /* spot exponent in narrow / spot phase */

/* Attenuation is quadratic in distance, so the coefficient has to scale
 * with the scene or a drone lights a unit scene to white and a 50-unit one
 * not at all. Referenced to the scene radius and tuned so a drone at its
 * spot-phase station (DRONE_SPOT_DIST radii out) still delivers most of its
 * diffuse colour, and one on a close orbit pass reads as brighter. */
#define DRONE_ATTEN_CONSTANT  0.55f
#define DRONE_ATTEN_QUADRATIC 0.16f
/* Diffuse headroom. A drone on a close orbit pass would otherwise clip the
 * surface it sweeps to flat white before the falloff has any range left. */
#define DRONE_DIFFUSE_SCALE   0.80f

#define DRONE_GLOW_POINT_SIZE 7.0f
#define DRONE_POOL_SEGS       28

/* --- Fairies backdrop ---
 *
 * A demonstration piece: every fairy is a plain POSITIONAL light with
 * quadratic attenuation and no spot cone anywhere in this backdrop, so
 * proximity alone shapes what gets lit. With a cone in play you cannot tell
 * the falloff from the cutoff, which is the whole thing this is here to show.
 *
 * One resident at a time arrives from outside, hops between points of
 * interest just off the surface, and leaves; the next comes in a different
 * colour. Passers-by cross on their own schedules - dimmer, never stopping,
 * but carrying real light slots, so a fly-past sweeps a highlight across the
 * scene. Poses are pure functions of anim_time, as the drones' are and for
 * the same reason.
 */
#define FAIRY_VISIT_SECS      13.0f  /* one resident visit, arrival to exit */
#define FAIRY_ARRIVE_FRAC     0.16f  /* fly-in share of the visit */
#define FAIRY_DEPART_FRAC     0.16f  /* fly-out share */
#define FAIRY_HOPS            4      /* points of interest per visit */
#define FAIRY_HOP_SETTLE      0.55f  /* share of a hop spent arrived, not moving */

/* Where the resident holds station: just off the geometry's bounding
 * sphere, close enough that the falloff is steep across the surface. */
/* Station radius, in scene radii. 1.0 is the corner of the measured box, and
 * the flutter is applied on all three axes independently, so it can displace
 * by as much as FAIRY_FLUTTER_AMP * sqrt(3) - this has to clear 1.0 by more
 * than that or the flutter alone parks a hovering fairy inside a boxy
 * scene's corner. A time sweep measures the worst case; keep it above 1.0. */
#define FAIRY_INSPECT_R       1.24f
#define FAIRY_ENTRY_R         5.0f   /* x scene radius, where a visit begins */
/* How strongly points of interest skew toward the viewer. 0 is uniform over
 * the sphere, which spends too much of every visit behind the geometry. */
#define FAIRY_VIEW_BIAS       0.95f

/* Flutter: a fairy never holds still. Two incommensurate frequencies per
 * axis so the wander does not read as a circle. */
#define FAIRY_FLUTTER_AMP     0.10f  /* x scene radius */
#define FAIRY_FLUTTER_HZ_A    1.7f
#define FAIRY_FLUTTER_HZ_B    2.9f

/* Passers-by. Three slots on deliberately incommensurate periods, each
 * lit only for the short window it is actually crossing, so they arrive at
 * irregular intervals rather than on a visible beat. */
#define FAIRY_PASSER_COUNT    3
#define FAIRY_PASSER_ACTIVE   0.30f  /* share of its period a slot is crossing */
#define FAIRY_PASSER_SPAN     6.0f   /* x scene radius, run length */
/* A passer aims straight at the centre and veers off around the geometry,
 * the way a streamline bends round an obstacle in a wind tunnel. A run that
 * merely went somewhere near the scene read as decoration; one that comes
 * AT it and gets out of the way reads as noticing it.
 *
 * AVOID_R is the closest approach at the peak of the veer, in scene radii.
 * AVOID_WIDTH is how far up- and downstream the veer extends, in those same
 * avoidance radii - larger is a longer, lazier curve. */
#define FAIRY_AVOID_R         1.20f
/* Must be at least sqrt(2) for the peak of the veer to be the run's
 * closest approach. Below that, d(along)^2 = along^2 + veer(along)^2 has
 * its minimum off-centre and the run dips nearer the geometry on either
 * side of the crossing than it does at the crossing itself - a sweep found
 * 1.93 radii where the profile promised 2.08. */
#define FAIRY_AVOID_WIDTH     1.65f
/* Radians of run-to-run variation in which way the veer goes, about the
 * screen-plane escape direction. Half a radian either side keeps it clearly
 * visible while stopping every pass from looking like the last. */
#define FAIRY_AVOID_SKEW      1.00f
#define FAIRY_PASSER_BRIGHT   0.42f  /* light + glow, relative to the resident */

/* Light rig. GL_LIGHT4 is the resident; 5..7 are the passers-by. No spot
 * state is set at all - that is the point of this backdrop - so each stays
 * at GL's default omnidirectional cutoff.
 *
 * Attenuation is referenced to the scene radius so the effect reads the
 * same at any scale, and is deliberately much steeper than the drones':
 * their job was to light the scene, this one's is to show falloff. */
#define FAIRY_LIGHT_RESIDENT  GL_LIGHT4
#define FAIRY_ATTEN_CONSTANT  0.28f
#define FAIRY_ATTEN_LINEAR    0.55f
#define FAIRY_ATTEN_QUADRATIC 2.40f
#define FAIRY_DIFFUSE_SCALE   1.85f  /* the falloff eats most of this back */

/* Body: a white-hot core point inside a wider coloured halo, plus a wake of
 * fading sparks sampled from the fairy's own past positions. */
#define FAIRY_CORE_POINT      5.0f
#define FAIRY_HALO_POINT      18.0f
#define FAIRY_HALO_ALPHA      0.44f
#define FAIRY_WAKE_COUNT      9
#define FAIRY_WAKE_DT         0.055f /* seconds between wake samples */
#define FAIRY_WING_SEGS       10
#define FAIRY_WING_R          0.085f /* x scene radius, the soft wing disc */

/* Clip-plane slots to clear. Six is GL's guaranteed minimum and the whole
 * set the REPL's GL_CLIP_PLANEn enum table exposes, so it is every plane a
 * user program can possibly have enabled. */
#define BACKDROP_CLIP_PLANE_SLOTS 6

static void render3d_backdrop_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

/* Backdrop geometry is not the user's geometry, and must not inherit the
 * state the user's program left set.
 *
 * The helper passes run inside the frame's outer glPushAttrib, so whatever
 * the program last established is still live when a backdrop draws: a scene
 * that enabled GL_CULL_FACE renders every one of our fans one-sided (a beam
 * cone seen from the culled side vanishes), and one that set up a clip plane
 * clips our volumes against a plane meant for its own model. Neither is a
 * choice the scene made about the backdrop.
 *
 * Only the enables are cleared - the planes' equations stay put, and the
 * backdrop's own glPushAttrib bracket restores everything for the overlays,
 * which DO deliberately replay the program's clip/cull state. */
static void backdrop_clear_user_volume_state(void) {
    glDisable(GL_CULL_FACE);
    for (int i = 0; i < BACKDROP_CLIP_PLANE_SLOTS; i++)
        glDisable((GLenum)(GL_CLIP_PLANE0 + i));
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

/* CITY_RAMP_BOTTOM..CITY_RAMP_TOP as a function of absolute height,
 * clamped past CITY_RAMP_HEIGHT so an unusually tall setback does not
 * run the ramp negative. */
static float city_height_ramp(float y) {
    float f = y / CITY_RAMP_HEIGHT;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return CITY_RAMP_BOTTOM + (CITY_RAMP_TOP - CITY_RAMP_BOTTOM) * f;
}

/* One five-quad box (inner / outer / 2 sides / roof). bd_r,g,b is the
 * base color (per-building deterministic + night-factor modulated);
 * the five faces multiply the base by per-face tints to fake lighting
 * without enabling GL_LIGHTING, and by the height ramp above so each
 * facade darkens toward its roof. The roof takes the top of the ramp
 * flat - it is a horizontal surface, so a gradient across it would read
 * as a crease rather than as height. */
static void draw_building_box(const CityBoxCorners *c,
                              float y_base, float y_top,
                              float bd_r, float bd_g, float bd_b) {
    float ramp_b = city_height_ramp(y_base);
    float ramp_t = city_height_ramp(y_top);

#define CITY_FACE_COLOR(ramp, tr, tg, tb) \
    glColor3f(bd_r * (tr) * (ramp), bd_g * (tg) * (ramp), bd_b * (tb) * (ramp))

    glBegin(GL_QUADS);

    /* Inner face (camera-side) */
    CITY_FACE_COLOR(ramp_b, 1.25f, 1.25f, 1.45f);
    glVertex3f(c->irx, y_base, c->irz); glVertex3f(c->ilx, y_base, c->ilz);
    CITY_FACE_COLOR(ramp_t, 1.25f, 1.25f, 1.45f);
    glVertex3f(c->ilx, y_top,  c->ilz); glVertex3f(c->irx, y_top,  c->irz);

    /* Outer face */
    CITY_FACE_COLOR(ramp_b, 0.55f, 0.55f, 0.60f);
    glVertex3f(c->olx, y_base, c->olz); glVertex3f(c->orx, y_base, c->orz);
    CITY_FACE_COLOR(ramp_t, 0.55f, 0.55f, 0.60f);
    glVertex3f(c->orx, y_top,  c->orz); glVertex3f(c->olx, y_top,  c->olz);

    /* Side faces */
    CITY_FACE_COLOR(ramp_b, 0.80f, 0.80f, 0.90f);
    glVertex3f(c->ilx, y_base, c->ilz); glVertex3f(c->olx, y_base, c->olz);
    CITY_FACE_COLOR(ramp_t, 0.80f, 0.80f, 0.90f);
    glVertex3f(c->olx, y_top,  c->olz); glVertex3f(c->ilx, y_top,  c->ilz);

    CITY_FACE_COLOR(ramp_b, 0.80f, 0.80f, 0.90f);
    glVertex3f(c->orx, y_base, c->orz); glVertex3f(c->irx, y_base, c->irz);
    CITY_FACE_COLOR(ramp_t, 0.80f, 0.80f, 0.90f);
    glVertex3f(c->irx, y_top,  c->irz); glVertex3f(c->orx, y_top,  c->orz);

    /* Roof */
    CITY_FACE_COLOR(ramp_t, 0.50f, 0.50f, 0.55f);
    glVertex3f(c->ilx, y_top,  c->ilz); glVertex3f(c->olx, y_top,  c->olz);
    glVertex3f(c->orx, y_top,  c->orz); glVertex3f(c->irx, y_top,  c->irz);

    glEnd();

#undef CITY_FACE_COLOR
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

    /* Four point-size bands; cumulative cutoffs are the STAR_BAND_CUT_*
     * above, and band_alpha dims with size so the field carries a
     * magnitude spread rather than one uniform brightness. */
    static const float band_sizes[4] = { 1.5f, 2.0f, 3.0f, 4.5f };
    static const float band_alpha[4] = {
        STAR_BAND_ALPHA_SMALL, STAR_BAND_ALPHA_MED,
        STAR_BAND_ALPHA_LARGE, STAR_BAND_ALPHA_BRIGHT,
    };
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

            /* Spherical coords: theta all-around, phi from the zenith down
             * to STAR_PHI_MAX. Sampling *cos(phi)* uniformly - not phi -
             * is what makes the scatter area-uniform over the dome. Linear
             * phi crowds the pole, which showed up as a dense knot of
             * stars straight overhead thinning out toward the horizon.
             * sin(phi) is non-negative across the whole span, so the
             * sqrt form of the identity is exact here. */
            float theta = city_rng(base + 1u) * 2.0f * (float)M_PI;
            float cp = 1.0f - city_rng(base + 2u) * (1.0f - cosf(STAR_PHI_MAX));
            float sp = sqrtf(1.0f - cp * cp);

            float sx = sp * cosf(theta) * STAR_SKY_RADIUS;
            float sy = cp * STAR_SKY_RADIUS;
            float sz = sp * sinf(theta) * STAR_SKY_RADIUS;

            /* Same three retro-80s hue families as the city windows, but
             * carrying far less chroma and weighted toward the off-white:
             * the windows are the thing meant to read as neon, and a sky
             * of equally saturated blue and magenta competed with them -
             * and with the user's geometry - as confetti rather than
             * settling behind both. Hue survives; saturation does not. */
            float color_roll = city_rng(base + 3u);
            float sr, sg, sb;
            if (color_roll < 0.64f) {
                float w = city_rng(base + 10u);
                sr = 0.88f + w * 0.10f;
                sg = 0.88f + w * 0.06f;
                sb = 0.94f + w * 0.04f;
            } else if (color_roll < 0.85f) {
                float b = city_rng(base + 11u);
                sr = 0.60f + b * 0.14f;
                sg = 0.74f + b * 0.12f;
                sb = 1.0f;
            } else {
                float p = city_rng(base + 12u);
                sr = 0.80f + p * 0.12f;
                sg = 0.62f + p * 0.12f;
                sb = 0.96f + p * 0.04f;
            }

            /* ~35% twinkle slowly; the rest have a faint atmospheric
             * shimmer. The twinkle swing is deliberately shallower than a
             * real one - a star crossing most of its alpha range draws the
             * eye off the scene, which is the opposite of a backdrop's
             * job. */
            float alpha;
            float blink_roll = city_rng(base + 4u);
            if (blink_roll > 0.65f) {
                float phase = city_rng(base + 5u) * 2.0f * (float)M_PI;
                float speed = 0.05f + city_rng(base + 6u) * 0.28f;
                float blink = 0.5f + 0.5f * sinf(anim_time * speed * 2.0f * (float)M_PI + phase);
                alpha = 0.60f + 0.40f * blink;
            } else {
                float phase = city_rng(base + 5u) * 2.0f * (float)M_PI;
                alpha = 0.86f + 0.08f * sinf(anim_time * 2.7f + phase);
            }

            glColor4f(sr, sg, sb, alpha * band_alpha[bi]);
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
        /* h,        r,     g,     b */
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
 * top by the dispatch (same pattern as CITYSCAPE). */

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
        /* h,        r,                       g,                       b */
        {  -1.00f, 0.12f,                   0.24f,                   0.32f                   },
        {   0.00f, RENDER3D_GLACIAL_TINT_R, RENDER3D_GLACIAL_TINT_G, RENDER3D_GLACIAL_TINT_B },
        {   0.18f, 0.50f,                   0.66f,                   0.82f                   },
        {   0.55f, 0.34f,                   0.48f,                   0.68f                   },
        {   1.00f, 0.22f,                   0.34f,                   0.52f                   },
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

/* --- Drones backdrop -------------------------------------------------- */

/* Per-drone body/light colour. Three cool scanners and one warm one, so a
 * scene lit by the rig picks up a temperature difference as they move
 * rather than a uniform wash. */
static const float k_drone_colors[DRONE_COUNT][3] = {
    { 0.62f, 0.84f, 1.00f },  /* cold white-blue */
    { 1.00f, 0.66f, 0.30f },  /* sodium amber */
    { 0.45f, 0.95f, 0.88f },  /* pale cyan */
    { 0.78f, 0.62f, 1.00f },  /* dim violet */
};

/* --- Shared helpers for the bounds-driven backdrops (drones, fairies) ---
 *
 * These are the pieces every backdrop that arranges itself AROUND the
 * user's geometry needs: the measured extent, and a little vector algebra
 * for aiming at it. Nothing above this point reads the bounds hook.
 */

/* Fallback extent, used when no bounds hook is installed (render3d_demo) or
 * it reports nothing drawn. The minimum keeps a degenerate single-point
 * scene from collapsing a rig onto the geometry. */
#define BACKDROP_EXTENT_FALLBACK_R 2.5f
#define BACKDROP_EXTENT_MIN_R      0.75f

/* The measured scene a rig arranges itself around. */
typedef struct BackdropExtent {
    float center[3];
    float radius;
    float floor_y;   /* under the geometry: where cast pools land */
} BackdropExtent;

typedef struct DronePose {
    float pos[3];
    float aim[3];    /* unit vector: where the drone is pointing */
} DronePose;

static float backdrop_vec_len(const float v[3]) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static void backdrop_vec_norm(float v[3]) {
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len < 1e-5f) {
        v[0] = 0.0f; v[1] = -1.0f; v[2] = 0.0f;
        return;
    }
    v[0] /= len; v[1] /= len; v[2] /= len;
}

/* Orthonormal frame around unit d - for anything drawn as a disc or cone
 * facing along a direction. The seed axis is whichever cardinal d is least
 * aligned with, so the cross product never degenerates. */
static void backdrop_frame(const float d[3], float t1[3], float t2[3]) {
    float seed[3] = { 0.0f, 1.0f, 0.0f };
    if (fabsf(d[1]) > 0.9f) {
        seed[0] = 1.0f; seed[1] = 0.0f;
    }
    t1[0] = seed[1] * d[2] - seed[2] * d[1];
    t1[1] = seed[2] * d[0] - seed[0] * d[2];
    t1[2] = seed[0] * d[1] - seed[1] * d[0];
    backdrop_vec_norm(t1);
    t2[0] = d[1] * t1[2] - d[2] * t1[1];
    t2[1] = d[2] * t1[0] - d[0] * t1[2];
    t2[2] = d[0] * t1[1] - d[1] * t1[0];
}

/* Unit vector from the measured scene toward the camera.
 *
 * Derived from the caller's camera fields rather than read back out of the
 * modelview, because a glGetFloatv is a pipeline drain. The controller's
 * modelview is T(0,0,-dist) . Rx . Ry . T(-pan), so the eye sits at
 * pan + Ry(-ry).Rx(-rx).(0,0,dist); the horizontal term matches the
 * (sin ry, 0, -cos ry) look direction grid.c's face-on weighting uses.
 *
 * The subtraction of the extent centre is the part that is easy to leave
 * out and wrong to: the orbit target is where the CAMERA pivots, not where
 * the geometry is. Pan the view away from a scene and the two directions
 * diverge, so a consumer biasing toward "the camera" would bias toward the
 * pivot instead and pick the wrong side of the object. Falls back to the
 * orbit bearing when the eye coincides with the centre, which has no
 * defined direction. */
static void backdrop_camera_dir(const Render3dFrameRenderContext *frame_ctx,
                                const BackdropExtent *ex, float out[3]) {
    const Render3dRenderConfig *cfg = &frame_ctx->config;
    float ry = cfg->cam_ry * (float)M_PI / 180.0f;
    float rx = cfg->cam_rx * (float)M_PI / 180.0f;
    float crx = cosf(rx);
    float bearing[3];
    float eye[3];
    float len;

    bearing[0] = -sinf(ry) * crx;
    bearing[1] =  sinf(rx);
    bearing[2] =  cosf(ry) * crx;

    eye[0] = cfg->cam_tx + bearing[0] * cfg->cam_dist;
    eye[1] = cfg->cam_ty + bearing[1] * cfg->cam_dist;
    eye[2] = cfg->cam_tz + bearing[2] * cfg->cam_dist;

    for (int k = 0; k < 3; k++)
        out[k] = eye[k] - ex->center[k];
    len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (len < 1e-4f) {
        for (int k = 0; k < 3; k++)
            out[k] = bearing[k];
    }
    backdrop_vec_norm(out);
}

/* Resolve the extent a rig should frame. The bounds hook is optional in
 * both directions - a caller may not install one (render3d_demo does not),
 * and an installed one reports 0 when there is nothing to measure - and
 * both cases land on the same fallback. */
static void backdrop_resolve_extent(const Render3dFrameRenderContext *frame_ctx,
                                    BackdropExtent *out) {
    float mn[3], mx[3];
    float half[3];

    out->center[0] = out->center[1] = out->center[2] = 0.0f;
    out->radius = BACKDROP_EXTENT_FALLBACK_R;
    out->floor_y = -BACKDROP_EXTENT_FALLBACK_R;

    if (!frame_ctx->config.geometry_bounds_fn)
        return;
    if (!frame_ctx->config.geometry_bounds_fn(
            frame_ctx->config.geometry_bounds_user_data, mn, mx))
        return;

    for (int k = 0; k < 3; k++) {
        out->center[k] = (mn[k] + mx[k]) * 0.5f;
        half[k] = (mx[k] - mn[k]) * 0.5f;
    }
    out->radius = sqrtf(half[0] * half[0] + half[1] * half[1] +
                        half[2] * half[2]);
    if (out->radius < BACKDROP_EXTENT_MIN_R)
        out->radius = BACKDROP_EXTENT_MIN_R;
    /* The floor sits under the geometry, not on the grid plane: a scene
     * built above or below y = 0 should still get its own. */
    out->floor_y = mn[1] - out->radius * 0.05f;
}

/* Phase 0 - transit. The formation crosses the scene along one axis,
 * entering and leaving well outside it. `slot` turns the axis by a golden
 * angle each repetition so successive passes come from a new direction. */
static void drone_pose_strafe(int i, float u, int slot,
                              const BackdropExtent *sc, DronePose *out) {
    float ang = 2.39996323f * (float)slot;
    float dir[3]  = { cosf(ang), 0.0f, sinf(ang) };
    float side[3] = { -dir[2], 0.0f, dir[0] };
    float rank = (float)i - (DRONE_COUNT - 1) * 0.5f;
    float lane = (DRONE_STRAFE_STANDOFF + rank * DRONE_STRAFE_LANE) * sc->radius;
    /* Staggered along the run so they stream past rather than abreast. */
    float along = (u - 0.5f) * DRONE_STRAFE_SPAN * sc->radius
                  - rank * 0.30f * sc->radius;
    float rise = (DRONE_STRAFE_RISE + 0.14f * (float)(i & 1)) * sc->radius;

    for (int k = 0; k < 3; k++)
        out->pos[k] = sc->center[k] + dir[k] * along + side[k] * lane;
    out->pos[1] += rise;

    /* Tracking the scene as they pass rather than staring straight ahead -
     * a pass that lit nothing would be the one phase with no effect on the
     * geometry. */
    for (int k = 0; k < 3; k++)
        out->aim[k] = sc->center[k] - out->pos[k];
    backdrop_vec_norm(out->aim);
}

/* Phase 1 - orbit. Each drone rides its own tilted ring, evenly spaced in
 * phase, looking inward at the scene centre. */
static void drone_pose_orbit(int i, float u, int slot,
                             const BackdropExtent *sc, DronePose *out) {
    float theta = 2.0f * (float)M_PI *
                  (u * DRONE_ORBIT_TURNS + (float)i / (float)DRONE_COUNT);
    float tilt = 0.22f + 0.20f * (float)i;
    float r = DRONE_ORBIT_RADIUS * sc->radius;
    float ct = cosf(theta), st = sinf(theta);
    /* Ring in XZ, tipped about the X axis so the four planes cross. */
    float x = r * ct;
    float y = r * st * sinf(tilt);
    float z = r * st * cosf(tilt);
    /* Alternate the direction of travel per repetition. */
    if (slot & 1)
        z = -z;

    out->pos[0] = sc->center[0] + x;
    out->pos[1] = sc->center[1] + y;
    out->pos[2] = sc->center[2] + z;

    for (int k = 0; k < 3; k++)
        out->aim[k] = sc->center[k] - out->pos[k];
    backdrop_vec_norm(out->aim);
}

/* Phase 2 - spotlight. The drones hold high stations and sweep their beams
 * across the scene: the phase the GL spotlight state exists for. */
static void drone_pose_spot(int i, float u, int slot,
                            const BackdropExtent *sc, DronePose *out) {
    float az = 2.0f * (float)M_PI * ((float)i / (float)DRONE_COUNT
                                     + 0.18f * u)
               + 0.9f * (float)slot;
    float elev = 0.62f + 0.10f * sinf(2.0f * (float)M_PI * u + (float)i);
    float d = DRONE_SPOT_DIST * sc->radius;
    float ce = cosf(elev), se = sinf(elev);
    /* Each beam wanders over its own lissajous inside the scene box, so
     * the four sweeps cross instead of converging on one bright point. */
    float wx = 0.45f * sc->radius * sinf(1.7f * u * 2.0f * (float)M_PI + (float)i * 1.3f);
    float wz = 0.45f * sc->radius * cosf(1.1f * u * 2.0f * (float)M_PI + (float)i * 2.1f);

    out->pos[0] = sc->center[0] + d * ce * cosf(az);
    out->pos[1] = sc->center[1] + d * se;
    out->pos[2] = sc->center[2] + d * ce * sinf(az);

    out->aim[0] = sc->center[0] + wx - out->pos[0];
    out->aim[1] = sc->center[1] - out->pos[1];
    out->aim[2] = sc->center[2] + wz - out->pos[2];
    backdrop_vec_norm(out->aim);
}

static void drone_pose_for_phase(int phase, int i, float u, int slot,
                                 const BackdropExtent *sc, DronePose *out) {
    switch (phase) {
    case 1:  drone_pose_orbit(i, u, slot, sc, out); break;
    case 2:  drone_pose_spot(i, u, slot, sc, out);  break;
    default: drone_pose_strafe(i, u, slot, sc, out); break;
    }
}

/* Where in the patrol cycle `anim_time` falls: the active phase, its local
 * progress, and how far the cross-fade into the next phase has run
 * (blend == 0 outside the seam). */
typedef struct DronePhase {
    int   phase;
    int   slot;
    float u;
    float blend;   /* 0..1 toward (phase + 1) % DRONE_PHASE_COUNT */
} DronePhase;

static DronePhase drone_phase_at(float anim_time) {
    DronePhase p;
    float tt = (anim_time > 0.0f) ? anim_time : 0.0f;
    float cycles = tt / DRONE_PHASE_SECS;
    float slot_f = floorf(cycles);

    p.slot = (int)slot_f;
    p.u = cycles - slot_f;
    p.phase = p.slot % DRONE_PHASE_COUNT;
    p.blend = 0.0f;
    if (p.u > 1.0f - DRONE_XFADE_FRAC) {
        float s = (p.u - (1.0f - DRONE_XFADE_FRAC)) / DRONE_XFADE_FRAC;
        p.blend = s * s * (3.0f - 2.0f * s);   /* smoothstep */
    }
    return p;
}

/* Full pose for drone `i`, cross-fade resolved. The incoming phase is
 * evaluated at a slightly negative local time - which is exactly its own
 * start, continued backwards - so position and aim are continuous across
 * the seam. */
static void drone_pose(const DronePhase *ph, int i, const BackdropExtent *sc,
                       DronePose *out) {
    DronePose next;
    float da[3], db[3], blended[3];
    float ra, rb;

    drone_pose_for_phase(ph->phase, i, ph->u, ph->slot, sc, out);
    if (ph->blend <= 0.0f)
        return;

    drone_pose_for_phase((ph->phase + 1) % DRONE_PHASE_COUNT, i,
                         ph->u - 1.0f, ph->slot + 1, sc, &next);

    /* Blend the two poses in POLAR form about the scene - bearing and
     * distance interpolated separately - rather than lerping the positions.
     * Straight-line interpolation between two points either side of the
     * geometry is a chord through it, and the outgoing and incoming phases
     * are routinely on opposite sides: a sweep found a drone at 0.10 of the
     * scene radius mid-cross-fade, deep inside the object, while each
     * endpoint pose was comfortably clear. In polar form the blend can
     * never be nearer than the closer of the two endpoints. */
    for (int k = 0; k < 3; k++) {
        da[k] = out->pos[k] - sc->center[k];
        db[k] = next.pos[k] - sc->center[k];
    }
    ra = sqrtf(da[0] * da[0] + da[1] * da[1] + da[2] * da[2]);
    rb = sqrtf(db[0] * db[0] + db[1] * db[1] + db[2] * db[2]);
    backdrop_vec_norm(da);
    backdrop_vec_norm(db);
    for (int k = 0; k < 3; k++)
        blended[k] = da[k] + (db[k] - da[k]) * ph->blend;
    backdrop_vec_norm(blended);

    {
        float r = ra + (rb - ra) * ph->blend;
        for (int k = 0; k < 3; k++)
            out->pos[k] = sc->center[k] + blended[k] * r;
    }

    for (int k = 0; k < 3; k++)
        out->aim[k] += (next.aim[k] - out->aim[k]) * ph->blend;
    backdrop_vec_norm(out->aim);
}

/* How much of the current blend is the spotlight phase: drives the beam
 * cone's width and brightness and the GL spot cutoff together, so the drawn
 * cone and the lit falloff never disagree. */
static float drone_spotness(const DronePhase *ph) {
    float here = (ph->phase == 2) ? 1.0f : 0.0f;
    float there = (((ph->phase + 1) % DRONE_PHASE_COUNT) == 2) ? 1.0f : 0.0f;
    return here + (there - here) * ph->blend;
}

/* Configure GL_LIGHT4..7 as the four drones. Runs in the pass setup phase,
 * BEFORE user geometry, which is the whole reason the pose is a function of
 * time rather than something the render pass computes and stashes: the
 * lights have to be placed before the geometry they light, and the bodies
 * are drawn afterwards from the same function. */
static void drone_setup_lights(const Render3dFrameRenderContext *frame_ctx) {
    BackdropExtent sc;
    DronePhase ph = drone_phase_at(frame_ctx->config.anim_time);
    float spotness = drone_spotness(&ph);
    float spot_exp = DRONE_SPOT_EXP_WIDE +
                     (DRONE_SPOT_EXP_SPOT - DRONE_SPOT_EXP_WIDE) * spotness;
    float quad;

    backdrop_resolve_extent(frame_ctx, &sc);
    /* Quadratic falloff referenced to the scene's own size, so a drone at
     * its orbit radius contributes the same amount whatever that radius
     * happens to be. */
    quad = DRONE_ATTEN_QUADRATIC / (sc.radius * sc.radius);

    for (int i = 0; i < DRONE_COUNT; i++) {
        DronePose pose;
        GLenum id = (GLenum)(GL_LIGHT4 + i);
        GLfloat pos[4];
        GLfloat dir[3];
        GLfloat diffuse[4], ambient[4], specular[4];

        drone_pose(&ph, i, &sc, &pose);

        pos[0] = pose.pos[0]; pos[1] = pose.pos[1]; pos[2] = pose.pos[2];
        pos[3] = 1.0f;        /* positional: attenuation and spot need it */
        dir[0] = pose.aim[0]; dir[1] = pose.aim[1]; dir[2] = pose.aim[2];

        for (int k = 0; k < 3; k++) {
            diffuse[k]  = k_drone_colors[i][k] * DRONE_DIFFUSE_SCALE;
            ambient[k]  = k_drone_colors[i][k] * 0.02f;
            specular[k] = k_drone_colors[i][k];
        }
        diffuse[3] = ambient[3] = specular[3] = 1.0f;

        glLightfv(id, GL_POSITION, pos);
        glLightfv(id, GL_DIFFUSE, diffuse);
        glLightfv(id, GL_AMBIENT, ambient);
        glLightfv(id, GL_SPECULAR, specular);
        glLightfv(id, GL_SPOT_DIRECTION, dir);
        glLightf(id, GL_SPOT_CUTOFF, DRONE_SPOT_CUTOFF_DEG);
        glLightf(id, GL_SPOT_EXPONENT, spot_exp);
        glLightf(id, GL_CONSTANT_ATTENUATION, DRONE_ATTEN_CONSTANT);
        glLightf(id, GL_LINEAR_ATTENUATION, 0.0f);
        glLightf(id, GL_QUADRATIC_ATTENUATION, quad);
        glEnable(id);
    }
}

/* One drone hull: a quadrotor, the shape that reads as a drone on sight
 * at the twenty-odd pixels one of these actually occupies. The rotor
 * deck is framed off WORLD UP rather than off the aim, so the craft hangs
 * level and only the gimbal stalk swings toward the target - a camera
 * craft, not a dart that happens to point somewhere. It is then banked
 * partway toward the aim, which is both what a real quadcopter does and
 * what keeps the rotors reading as discs instead of collapsing to bars
 * when the camera is near their plane. */
#define DRONE_QUAD_BANK 0.42f   /* share of the aim mixed into the deck normal */

static void drone_hull_quad(const DronePose *pose, const BackdropExtent *sc,
                            const float col[3], float alpha_scale) {
    float len = DRONE_HULL_LEN * sc->radius;
    float arm = len * 1.15f;
    float rot_r = len * 0.50f;
    float post = len * 0.22f;
    float body = len * 0.30f;
    float up[3], ax[3], az[3];
    float hub[4][3];
    float lens[3];

    /* Deck normal: world up banked toward the aim. */
    up[0] = -pose->aim[0] * DRONE_QUAD_BANK;
    up[1] = 1.0f - pose->aim[1] * DRONE_QUAD_BANK;
    up[2] = -pose->aim[2] * DRONE_QUAD_BANK;
    backdrop_vec_norm(up);
    backdrop_frame(up, ax, az);

    for (int s = 0; s < 4; s++) {
        float a = ((float)s + 0.5f) / 4.0f * 2.0f * (float)M_PI;
        float ca = cosf(a) * arm, sa = sinf(a) * arm;
        for (int k = 0; k < 3; k++)
            hub[s][k] = pose->pos[k] + ax[k] * ca + az[k] * sa;
    }
    for (int k = 0; k < 3; k++)
        lens[k] = pose->pos[k] + pose->aim[k] * (len * 0.75f);

    /* Arms out to the rotor hubs, motor posts standing off each hub, and
     * the gimbal stalk down to the lens. */
    glLineWidth(DRONE_LINE_WIDTH + 0.5f);
    glColor4f(col[0], col[1], col[2], 0.95f * alpha_scale);
    glBegin(GL_LINES);
    for (int s = 0; s < 4; s++) {
        glVertex3fv(pose->pos);
        glVertex3fv(hub[s]);
        glVertex3f(hub[s][0] - up[0] * post * 0.5f,
                   hub[s][1] - up[1] * post * 0.5f,
                   hub[s][2] - up[2] * post * 0.5f);
        glVertex3f(hub[s][0] + up[0] * post,
                   hub[s][1] + up[1] * post,
                   hub[s][2] + up[2] * post);
    }
    glVertex3fv(pose->pos);
    glVertex3fv(lens);
    glEnd();

    /* Body plate: a small diamond around the hub, in the deck plane. */
    glColor4f(col[0], col[1], col[2], 0.70f * alpha_scale);
    glBegin(GL_LINE_LOOP);
    for (int s = 0; s < 4; s++) {
        float a = (float)s / 4.0f * 2.0f * (float)M_PI;
        float ca = cosf(a) * body, sa = sinf(a) * body;
        glVertex3f(pose->pos[0] + ax[0] * ca + az[0] * sa,
                   pose->pos[1] + ax[1] * ca + az[1] * sa,
                   pose->pos[2] + ax[2] * ca + az[2] * sa);
    }
    glEnd();

    /* Rotor discs, at the top of each motor post. Dim - they read as
     * motion, and holding them back keeps the additive blend from washing
     * the four colours toward a common white. */
    glColor4f(col[0], col[1], col[2], 0.30f * alpha_scale);
    for (int s = 0; s < 4; s++) {
        float c[3];
        for (int k = 0; k < 3; k++)
            c[k] = hub[s][k] + up[k] * post;
        glBegin(GL_LINE_LOOP);
        for (int q = 0; q < 12; q++) {
            float a = (float)q / 12.0f * 2.0f * (float)M_PI;
            float ca = cosf(a) * rot_r, sa = sinf(a) * rot_r;
            glVertex3f(c[0] + ax[0] * ca + az[0] * sa,
                       c[1] + ax[1] * ca + az[1] * sa,
                       c[2] + ax[2] * ca + az[2] * sa);
        }
        glEnd();
    }
    glLineWidth(1.0f);
}

/* The lens: an additive point sprite at the nose. A point rather than a
 * quad because a point is inherently camera-facing - no billboard frame,
 * and no glGetFloatv(GL_MODELVIEW_MATRIX) read-back to build one. */
static void drone_draw_lens(const DronePose *pose, const BackdropExtent *sc,
                            const float col[3], float alpha_scale) {
    float len = DRONE_HULL_LEN * sc->radius;
    glPointSize(DRONE_GLOW_POINT_SIZE);
    glBegin(GL_POINTS);
    glColor4f(col[0], col[1], col[2], alpha_scale);
    glVertex3f(pose->pos[0] + pose->aim[0] * len,
               pose->pos[1] + pose->aim[1] * len,
               pose->pos[2] + pose->aim[2] * len);
    glEnd();
}

/* The visible beam: an open cone from the lens along the aim, bright at the
 * lens and transparent at the far rim. This is what actually reads as a
 * spotlight - GL's own spot term is evaluated per vertex, so on the coarse
 * geometry a typed scene usually has it contributes almost nothing. */
static void drone_draw_beam(const DronePose *pose, const BackdropExtent *sc,
                            const float col[3], float spotness,
                            float alpha_scale) {
    float t1[3], t2[3];
    float apex[3];
    float half_deg = DRONE_BEAM_WIDE_DEG +
                     (DRONE_BEAM_NARROW_DEG - DRONE_BEAM_WIDE_DEG) * spotness;
    float half_rad = half_deg * (float)M_PI / 180.0f;
    float peak = DRONE_BEAM_TRANSIT_A +
                 (DRONE_BEAM_ALPHA - DRONE_BEAM_TRANSIT_A) * spotness;
    float reach = 0.0f;
    float rim;
    float lens = DRONE_HULL_LEN * sc->radius;

    for (int k = 0; k < 3; k++) {
        float d = sc->center[k] - pose->pos[k];
        reach += d * d;
    }
    reach = sqrtf(reach);
    rim = reach * tanf(half_rad);

    backdrop_frame(pose->aim, t1, t2);
    for (int k = 0; k < 3; k++)
        apex[k] = pose->pos[k] + pose->aim[k] * lens;

    glBegin(GL_TRIANGLE_FAN);
    glColor4f(col[0], col[1], col[2], peak * alpha_scale);
    glVertex3fv(apex);
    glColor4f(col[0], col[1], col[2], 0.0f);
    for (int s = 0; s <= DRONE_BEAM_SEGS; s++) {
        float a = (float)s / (float)DRONE_BEAM_SEGS * 2.0f * (float)M_PI;
        float ca = cosf(a) * rim, sa = sinf(a) * rim;
        glVertex3f(apex[0] + pose->aim[0] * reach + t1[0] * ca + t2[0] * sa,
                   apex[1] + pose->aim[1] * reach + t1[1] * ca + t2[1] * sa,
                   apex[2] + pose->aim[2] * reach + t1[2] * ca + t2[2] * sa);
    }
    glEnd();
}

/* Where the beam meets the floor, as a soft disc. Skipped when the drone is
 * not pointing down at it, which is also when the ellipse would stretch to
 * the horizon. */
static void drone_draw_pool(const DronePose *pose, const BackdropExtent *sc,
                            const float col[3], float spotness,
                            float alpha_scale) {
    float dist, r, cx, cz, cy;
    float fade;

    /* A beam angled along the floor rather than down at it produces an
     * ellipse stretching away to the horizon; skip it entirely. */
    if (pose->aim[1] > -0.45f)
        return;
    dist = (sc->floor_y - pose->pos[1]) / pose->aim[1];
    if (dist <= 0.0f)
        return;

    cx = pose->pos[0] + pose->aim[0] * dist;
    cz = pose->pos[2] + pose->aim[2] * dist;
    cy = sc->floor_y;
    r = dist * tanf((DRONE_BEAM_WIDE_DEG +
                     (DRONE_BEAM_NARROW_DEG - DRONE_BEAM_WIDE_DEG) * spotness)
                    * (float)M_PI / 180.0f);
    if (r > DRONE_POOL_MAX_R * sc->radius)
        r = DRONE_POOL_MAX_R * sc->radius;
    /* A pool cast from far away is dimmer, matching the beam's own falloff. */
    fade = sc->radius / (sc->radius + dist);
    float peak_a = DRONE_POOL_TRANSIT_A +
                   (DRONE_POOL_ALPHA - DRONE_POOL_TRANSIT_A) * spotness;

    glBegin(GL_TRIANGLE_FAN);
    glColor4f(col[0], col[1], col[2], peak_a * fade * alpha_scale);
    glVertex3f(cx, cy, cz);
    glColor4f(col[0], col[1], col[2], 0.0f);
    for (int s = 0; s <= DRONE_POOL_SEGS; s++) {
        float a = (float)s / (float)DRONE_POOL_SEGS * 2.0f * (float)M_PI;
        glVertex3f(cx + cosf(a) * r, cy, cz + sinf(a) * r);
    }
    glEnd();
}

static void draw_drones(const Render3dFrameRenderContext *frame_ctx) {
    BackdropExtent sc;
    DronePhase ph = drone_phase_at(frame_ctx->config.anim_time);
    float spotness = drone_spotness(&ph);
    float alpha = frame_ctx->config.alpha_scale;

    backdrop_resolve_extent(frame_ctx, &sc);

    render3d_backdrop_push_state();
    backdrop_clear_user_volume_state();
    glDisable(GL_FOG);
    /* The lens sprites are sized in pixels, so neutralize any point
     * distance attenuation the user's program left set - otherwise a scene
     * that uses glPointParameterfv silently resizes the drones' eyes. */
    if (frame_ctx->config.point_parameter_supported &&
        frame_ctx->config.point_parameter_proc)
        frame_ctx->config.point_parameter_proc(GL_POINT_DISTANCE_ATTENUATION,
                                               (GLfloat[]){1, 0, 0});

    /* Hulls, beams, and pools are wireframe/additive and depth-tested so
     * foreground geometry occludes drones behind it. */
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_POINT_SMOOTH);

    for (int i = 0; i < DRONE_COUNT; i++) {
        DronePose pose;
        drone_pose(&ph, i, &sc, &pose);
        drone_hull_quad(&pose, &sc, k_drone_colors[i], alpha);
        drone_draw_beam(&pose, &sc, k_drone_colors[i], spotness, alpha);
        drone_draw_pool(&pose, &sc, k_drone_colors[i], spotness, alpha);
        drone_draw_lens(&pose, &sc, k_drone_colors[i], alpha);
    }

    render3d_backdrop_pop_state();
}

/* --- Fairies backdrop ------------------------------------------------- */

/* Visitor colours, taken in turn so consecutive visits never repeat. The
 * first is Navi's blue; the rest are the other lights you'd expect to meet.
 * These are the HALO colours - the core is drawn washed toward white. */
static const float k_fairy_colors[][3] = {
    { 0.42f, 0.70f, 1.00f },  /* blue */
    { 1.00f, 0.84f, 0.38f },  /* gold */
    { 0.55f, 1.00f, 0.62f },  /* green */
    { 1.00f, 0.52f, 0.78f },  /* rose */
    { 0.72f, 0.58f, 1.00f },  /* violet */
};
#define FAIRY_COLOR_COUNT ((int)(sizeof(k_fairy_colors) / sizeof(k_fairy_colors[0])))

/* Smooth 0..1 ramp. Used for every fade and every hop, so arrival, hop and
 * departure all share one easing and none of them starts or stops abruptly. */
static float fairy_smoothstep(float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

/* A point on the sphere of radius `r` about the extent's centre, chosen
 * deterministically from `seed`. city_rng is a general bit-mixing hash
 * despite the name - the cityscape just happens to be its first caller.
 *
 * `bias` skews the direction toward a unit vector (0 = uniform). Without it
 * a visitor spends a good share of every visit inspecting the FAR side of
 * the geometry, where it is occluded and the surface it lights faces away -
 * seconds of an empty-looking frame. Skewing rather than clamping to the
 * near hemisphere keeps it free to wander round the edges. */
static void fairy_dir_from_seed(unsigned int seed, const float bias[3],
                                float bias_amt, float out[3]) {
    float u = city_rng(seed) * 2.0f - 1.0f;          /* cos(polar) */
    float phi = city_rng(seed + 1u) * 2.0f * (float)M_PI;
    float s = sqrtf(1.0f - u * u);

    out[0] = s * cosf(phi);
    out[1] = u;
    out[2] = s * sinf(phi);
    if (bias) {
        for (int k = 0; k < 3; k++)
            out[k] += bias[k] * bias_amt;
    }
    backdrop_vec_norm(out);
}

/* The same point, resolved onto the sphere of radius `r` about the centre. */
static void fairy_point_on_sphere(unsigned int seed, const BackdropExtent *ex,
                                  float r, const float bias[3], float bias_amt,
                                  float out[3]) {
    float d[3];
    fairy_dir_from_seed(seed, bias, bias_amt, d);
    for (int k = 0; k < 3; k++)
        out[k] = ex->center[k] + r * d[k];
}

/* Interpolate between two directions ON the sphere rather than through it.
 *
 * This is the difference between a fairy arcing around the geometry to its
 * next vantage point and one cutting the chord - and a chord between two
 * points on the inspect sphere passes arbitrarily close to the centre as
 * they approach opposite sides. A time sweep over the linear version found
 * it inside a unit sphere for a good fraction of every visit.
 *
 * Normalized lerp, not slerp: the angular rate is slightly non-uniform,
 * which is invisible under the flutter, and it costs a normalize instead of
 * two trig calls. Exactly antipodal inputs have no defined arc between them
 * either way; backdrop_vec_norm's fallback keeps the result finite, and the
 * view bias makes the case vanishingly rare. */
static void fairy_dir_lerp(const float a[3], const float b[3], float s,
                           float out[3]) {
    for (int k = 0; k < 3; k++)
        out[k] = a[k] + (b[k] - a[k]) * s;
    backdrop_vec_norm(out);
}

/* The restless part. Added to whatever station the fairy is holding, so it
 * flutters both in transit and while inspecting. */
static void fairy_flutter(float t, unsigned int seed, float amp,
                          float out[3]) {
    float p0 = city_rng(seed) * 6.2831853f;
    float p1 = city_rng(seed + 1u) * 6.2831853f;
    float p2 = city_rng(seed + 2u) * 6.2831853f;
    out[0] = amp * (sinf(t * FAIRY_FLUTTER_HZ_A + p0) * 0.6f +
                    sinf(t * FAIRY_FLUTTER_HZ_B + p1) * 0.4f);
    out[1] = amp * (sinf(t * FAIRY_FLUTTER_HZ_B + p1) * 0.6f +
                    cosf(t * FAIRY_FLUTTER_HZ_A + p2) * 0.4f);
    out[2] = amp * (cosf(t * FAIRY_FLUTTER_HZ_A + p2) * 0.6f +
                    sinf(t * FAIRY_FLUTTER_HZ_B + p0) * 0.4f);
}

/* One fairy, resolved at a moment in time. `bright` folds in every fade -
 * arrival, departure, and a passer's window - and scales both the light and
 * the drawn body, so the two can never disagree. */
typedef struct FairyState {
    float pos[3];
    float color[3];
    float bright;   /* 0 = absent */
} FairyState;

/* The resident visitor at time `t`. Arrival eases in from a point well
 * outside the scene, the middle of the visit hops between points of
 * interest just off the surface, and departure eases back out - each hop
 * and each end sharing one easing curve so the motion never snaps. */
static void fairy_resident_at(float t, const BackdropExtent *ex,
                              const float view[3], FairyState *out) {
    float cycles, slot_f, u;
    int slot;
    unsigned int seed;
    float station[3], flut[3];
    float dir[3], entry[3];
    float radius;

    if (t < 0.0f)
        t = 0.0f;
    cycles = t / FAIRY_VISIT_SECS;
    slot_f = floorf(cycles);
    slot = (int)slot_f;
    u = cycles - slot_f;
    seed = (unsigned int)slot * 977u + 12289u;

    for (int k = 0; k < 3; k++)
        out->color[k] = k_fairy_colors[slot % FAIRY_COLOR_COUNT][k];

    /* The whole visit is expressed as a DIRECTION from the centre plus a
     * RADIUS, and every interpolation happens in those two separately. That
     * is what keeps the fairy outside the geometry at all times: a straight
     * line between two stations is a chord, and a chord cuts through. */
    {
        float hop_span = 1.0f / (float)FAIRY_HOPS;
        float hop_f = u / hop_span;
        int hop = (int)hop_f;
        float hu = hop_f - (float)hop;
        float move = fairy_smoothstep((hu - FAIRY_HOP_SETTLE) /
                                      (1.0f - FAIRY_HOP_SETTLE));
        float a[3], b[3];
        if (hop >= FAIRY_HOPS)
            hop = FAIRY_HOPS - 1;
        fairy_dir_from_seed(seed + (unsigned int)hop * 7u,
                            view, FAIRY_VIEW_BIAS, a);
        fairy_dir_from_seed(seed + (unsigned int)(hop + 1) * 7u,
                            view, FAIRY_VIEW_BIAS, b);
        fairy_dir_lerp(a, b, move, dir);
        radius = ex->radius * FAIRY_INSPECT_R;
    }

    /* Arrival and departure swing round from a far entry bearing while
     * descending to the inspect radius - a spiral in rather than a straight
     * run at the geometry, for the same chord reason. */
    fairy_dir_from_seed(seed + 101u, view, FAIRY_VIEW_BIAS * 0.5f, entry);
    {
        float s = 1.0f;
        if (u < FAIRY_ARRIVE_FRAC)
            s = fairy_smoothstep(u / FAIRY_ARRIVE_FRAC);
        else if (u > 1.0f - FAIRY_DEPART_FRAC)
            s = fairy_smoothstep((1.0f - u) / FAIRY_DEPART_FRAC);
        if (s < 1.0f) {
            float swung[3];
            fairy_dir_lerp(entry, dir, s, swung);
            for (int k = 0; k < 3; k++)
                dir[k] = swung[k];
            radius = ex->radius * (FAIRY_ENTRY_R +
                                   (FAIRY_INSPECT_R - FAIRY_ENTRY_R) * s);
        }
        out->bright = s;
    }

    for (int k = 0; k < 3; k++)
        station[k] = ex->center[k] + dir[k] * radius;

    fairy_flutter(t, seed + 211u, ex->radius * FAIRY_FLUTTER_AMP, flut);
    for (int k = 0; k < 3; k++)
        out->pos[k] = station[k] + flut[k];
}

/* Passer-by `i` at time `t`, or bright = 0 when this slot is between runs.
 * Each slot has its own period, and the periods are deliberately
 * incommensurate so the three never fall into a visible rhythm. */
static void fairy_passer_at(float t, int i, const BackdropExtent *ex,
                            const float view[3], FairyState *out) {
    static const float k_periods[FAIRY_PASSER_COUNT] = { 7.3f, 11.7f, 17.1f };
    float period = k_periods[i];
    float cycles, slot_f, u;
    int slot;
    unsigned int seed;
    float from[3], to[3], dir[3], flut[3];
    float fade;

    out->bright = 0.0f;
    if (t < 0.0f)
        t = 0.0f;
    cycles = t / period;
    slot_f = floorf(cycles);
    slot = (int)slot_f;
    u = cycles - slot_f;
    if (u > FAIRY_PASSER_ACTIVE)
        return;                       /* between runs: this slot is dark */
    u /= FAIRY_PASSER_ACTIVE;         /* 0..1 across the run */

    seed = (unsigned int)slot * 613u + (unsigned int)i * 31397u;
    for (int k = 0; k < 3; k++)
        out->color[k] = k_fairy_colors[(slot + i) % FAIRY_COLOR_COUNT][k];

    /* The run is aimed dead at the centre: `to` is `from` mirrored through
     * it, so undeflected the fairy would fly straight into the geometry. */
    fairy_point_on_sphere(seed, ex,
                          ex->radius * FAIRY_PASSER_SPAN * 0.5f,
                          view, FAIRY_VIEW_BIAS * 0.5f, from);
    for (int k = 0; k < 3; k++) {
        to[k] = ex->center[k] * 2.0f - from[k];
        dir[k] = to[k] - from[k];
        out->pos[k] = from[k] + dir[k] * u;
    }
    backdrop_vec_norm(dir);

    /* The veer. `along` is signed distance past the closest approach, so
     * the deflection peaks as the fairy draws level with the centre and
     * falls off symmetrically either side - which is the shape a streamline
     * actually has. It is applied along one seeded perpendicular per run
     * rather than radially outward: a run aimed exactly at the centre has
     * no radial direction to push along there, and every neighbouring one
     * has a nearly opposite one, so a radial push would snap through 180
     * degrees at the crossing. Choosing the escape side up front is both
     * well-defined everywhere and what something avoiding an obstacle
     * does - it picks a way round and commits.
     *
     * 1/(1+x^2) rather than a compactly supported bump: it is smooth
     * everywhere with no seams to place, and by the ends of the run it has
     * decayed to a few percent of the offset, well under the flutter. */
    {
        float side[3], perp[3], t1[3], t2[3];
        float avoid = ex->radius * FAIRY_AVOID_R;
        float along = 0.0f;
        float x, veer, len, ang, ca, sa;

        for (int k = 0; k < 3; k++)
            along += (out->pos[k] - ex->center[k]) * dir[k];
        x = along / (avoid * FAIRY_AVOID_WIDTH);
        veer = avoid / (1.0f + x * x);

        /* Escape into the screen plane: cross(view, dir) is perpendicular
         * to the travel direction AND to the line of sight, so the veer is
         * the component the viewer can actually see. A perpendicular picked
         * at random is just as valid in world space, but half the time it
         * points nearly along the view axis - the fairy clears the geometry
         * in 3D and still reads as flying straight through it, which is the
         * whole thing this is here to avoid. */
        side[0] = view[1] * dir[2] - view[2] * dir[1];
        side[1] = view[2] * dir[0] - view[0] * dir[2];
        side[2] = view[0] * dir[1] - view[1] * dir[0];
        len = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
        if (len < 0.20f) {
            /* Travel nearly along the line of sight: there is no screen-plane
             * direction to prefer (every veer is equally foreshortened), so
             * any perpendicular will do. */
            backdrop_frame(dir, t1, t2);
            for (int k = 0; k < 3; k++)
                side[k] = t1[k];
        } else {
            for (int k = 0; k < 3; k++)
                side[k] /= len;
        }

        /* Which way round, and how squarely, varies per run - held constant
         * for the whole run, because something avoiding an obstacle picks a
         * way past and commits to it. */
        ang = (city_rng(seed + 131u) - 0.5f) * FAIRY_AVOID_SKEW;
        if (city_rng(seed + 137u) < 0.5f)
            ang += (float)M_PI;
        ca = cosf(ang);
        sa = sinf(ang);
        perp[0] = dir[1] * side[2] - dir[2] * side[1];
        perp[1] = dir[2] * side[0] - dir[0] * side[2];
        perp[2] = dir[0] * side[1] - dir[1] * side[0];
        for (int k = 0; k < 3; k++)
            out->pos[k] += (side[k] * ca + perp[k] * sa) * veer;
    }

    fairy_flutter(t, seed + 7u, ex->radius * FAIRY_FLUTTER_AMP * 0.5f, flut);
    for (int k = 0; k < 3; k++)
        out->pos[k] += flut[k];

    /* Fade in and out at the ends of the run so a passer never pops into
     * or out of existence mid-frame - a hard-edged light appearing is far
     * more noticeable than the motion it was meant to suggest. */
    fade = fairy_smoothstep(u / 0.18f) *
           fairy_smoothstep((1.0f - u) / 0.18f);
    out->bright = fade * FAIRY_PASSER_BRIGHT;
}

/* Configure one fairy's light slot. Positional, attenuated, and pointedly
 * NOT a spotlight: no GL_SPOT_* state is set anywhere in this backdrop, so
 * every slot keeps GL's default omnidirectional cutoff and distance is the
 * only thing shaping what it lights. */
static void fairy_apply_light(GLenum id, const FairyState *f, float radius) {
    GLfloat pos[4];
    GLfloat diffuse[4], ambient[4], specular[4];
    float k = f->bright * FAIRY_DIFFUSE_SCALE;

    if (f->bright <= 0.0f) {
        glDisable(id);
        return;
    }

    pos[0] = f->pos[0]; pos[1] = f->pos[1]; pos[2] = f->pos[2];
    pos[3] = 1.0f;   /* positional - a directional light has no falloff */

    for (int c = 0; c < 3; c++) {
        diffuse[c]  = f->color[c] * k;
        ambient[c]  = f->color[c] * 0.015f * f->bright;
        specular[c] = f->color[c] * k;
    }
    diffuse[3] = ambient[3] = specular[3] = 1.0f;

    glLightfv(id, GL_POSITION, pos);
    glLightfv(id, GL_DIFFUSE, diffuse);
    glLightfv(id, GL_AMBIENT, ambient);
    glLightfv(id, GL_SPECULAR, specular);
    /* The demonstration: 1 / (c + l*d + q*d^2), with the distance terms
     * scaled by the scene so the same curve applies at any size. */
    glLightf(id, GL_CONSTANT_ATTENUATION, FAIRY_ATTEN_CONSTANT);
    glLightf(id, GL_LINEAR_ATTENUATION, FAIRY_ATTEN_LINEAR / radius);
    glLightf(id, GL_QUADRATIC_ATTENUATION,
             FAIRY_ATTEN_QUADRATIC / (radius * radius));
    glEnable(id);
}

static void fairy_setup_lights(const Render3dFrameRenderContext *frame_ctx) {
    BackdropExtent ex;
    FairyState f;
    float view[3];
    float t = frame_ctx->config.anim_time;

    backdrop_resolve_extent(frame_ctx, &ex);
    backdrop_camera_dir(frame_ctx, &ex, view);

    fairy_resident_at(t, &ex, view, &f);
    fairy_apply_light(FAIRY_LIGHT_RESIDENT, &f, ex.radius);

    for (int i = 0; i < FAIRY_PASSER_COUNT; i++) {
        fairy_passer_at(t, i, &ex, view, &f);
        fairy_apply_light((GLenum)(FAIRY_LIGHT_RESIDENT + 1 + i), &f, ex.radius);
    }
}

/* --- Fairy body, trail and dust --------------------------------------- */

#define FAIRY_RIBBON_STEPS   22     /* trail samples - a smear, not beads */
#define FAIRY_RIBBON_DT      0.022f
#define FAIRY_RIBBON_W       0.022f /* x scene radius, width at the body */
#define FAIRY_DUST_COUNT     12
#define FAIRY_DUST_DT        0.045f
#define FAIRY_DUST_FALL      0.490f /* x scene radius per sec^2, dust sink */
#define FAIRY_WING_BEAT_HZ   11.0f

static void fairy_sample_at(float t, int passer_idx, const BackdropExtent *ex,
                            const float view[3], FairyState *out) {
    if (passer_idx < 0)
        fairy_resident_at(t, ex, view, out);
    else
        fairy_passer_at(t, passer_idx, ex, view, out);
}

/* The trail: one continuous camera-facing ribbon through the fairy's own
 * past positions, tapering in width and alpha. Replaces the beaded point
 * wake - at speed those samples separate into a dotted line, which reads as
 * a string of markers rather than as motion. Free to compute for the same
 * reason the old wake was: the pose is a pure function of time, so there is
 * no history to keep. */
static void fairy_draw_ribbon(float t, int passer_idx, const BackdropExtent *ex,
                              const float view[3], float alpha_scale) {
    FairyState prev;
    float half = ex->radius * FAIRY_RIBBON_W;
    int started = 0;

    fairy_sample_at(t, passer_idx, ex, view, &prev);
    if (prev.bright <= 0.0f)
        return;

    glBegin(GL_TRIANGLE_STRIP);
    for (int k = 0; k <= FAIRY_RIBBON_STEPS; k++) {
        float back = t - (float)k * FAIRY_RIBBON_DT;
        float decay = 1.0f - (float)k / (float)(FAIRY_RIBBON_STEPS + 1);
        FairyState f;
        float seg[3], side[3], w;

        if (back < 0.0f)
            break;
        fairy_sample_at(back, passer_idx, ex, view, &f);
        if (f.bright <= 0.0f)
            break;

        /* Ribbon width runs across the trail AND across the view, so the
         * strip is edge-on to nothing. */
        for (int i = 0; i < 3; i++)
            seg[i] = prev.pos[i] - f.pos[i];
        side[0] = seg[1] * view[2] - seg[2] * view[1];
        side[1] = seg[2] * view[0] - seg[0] * view[2];
        side[2] = seg[0] * view[1] - seg[1] * view[0];
        if (backdrop_vec_len(side) < 1e-6f) {
            if (started) break;
            continue;
        }
        backdrop_vec_norm(side);

        w = half * decay * sqrtf(decay);
        glColor4f(f.color[0], f.color[1], f.color[2],
                  0.42f * decay * decay * decay * f.bright * alpha_scale);
        glVertex3f(f.pos[0] + side[0] * w, f.pos[1] + side[1] * w,
                   f.pos[2] + side[2] * w);
        glVertex3f(f.pos[0] - side[0] * w, f.pos[1] - side[1] * w,
                   f.pos[2] - side[2] * w);
        started = 1;
        prev = f;
    }
    glEnd();
}

/* The wings: two beating discs either side of the body, drawn in the
 * plane facing the camera so they are never edge-on. The beat is a pure
 * function of time like everything else here, and the two wings are driven
 * in antiphase so the pair flickers rather than pulsing as one blob. */
static void fairy_draw_wings(const FairyState *f, const BackdropExtent *ex,
                             const float view[3], float t, unsigned int seed,
                             float alpha_scale, float scale) {
    float side[3], upv[3];
    float wr = ex->radius * FAIRY_WING_R * 1.05f * scale;
    float phase = t * FAIRY_WING_BEAT_HZ * 2.0f * (float)M_PI +
                  (float)(seed % 97) * 0.13f;
    const float up_hint[3] = { 0.0f, 1.0f, 0.0f };

    /* Screen-plane axes: across the view, and up within it. */
    side[0] = up_hint[1] * view[2] - up_hint[2] * view[1];
    side[1] = up_hint[2] * view[0] - up_hint[0] * view[2];
    side[2] = up_hint[0] * view[1] - up_hint[1] * view[0];
    if (backdrop_vec_len(side) < 1e-6f)
        return;
    backdrop_vec_norm(side);
    upv[0] = view[1] * side[2] - view[2] * side[1];
    upv[1] = view[2] * side[0] - view[0] * side[2];
    upv[2] = view[0] * side[1] - view[1] * side[0];
    backdrop_vec_norm(upv);

    for (int wing = 0; wing < 2; wing++) {
        float dir = wing ? 1.0f : -1.0f;
        /* Foreshortening of a beating wing: |cos| of its own phase. */
        float beat = fabsf(cosf(phase + (wing ? (float)M_PI * 0.5f : 0.0f)));
        float sx = wr * (0.35f + 0.65f * beat);
        float sy = wr * 1.05f;
        float cx[3];

        for (int k = 0; k < 3; k++)
            cx[k] = f->pos[k] + side[k] * dir * sx * 0.85f + upv[k] * wr * 0.25f;

        glBegin(GL_TRIANGLE_FAN);
        glColor4f(f->color[0], f->color[1], f->color[2],
                  0.20f * f->bright * alpha_scale);
        glVertex3fv(cx);
        glColor4f(f->color[0], f->color[1], f->color[2], 0.0f);
        for (int s = 0; s <= 12; s++) {
            float a = (float)s / 12.0f * 2.0f * (float)M_PI;
            float ca = cosf(a) * sx, sa = sinf(a) * sy;
            glVertex3f(cx[0] + side[0] * ca + upv[0] * sa,
                       cx[1] + side[1] * ca + upv[1] * sa,
                       cx[2] + side[2] * ca + upv[2] * sa);
        }
        glEnd();
    }
}

/* The dust: motes shed from the trail at fixed intervals,
 * each drifting downward and fading over its own age - so the fairy leaves
 * something behind that the ribbon does not, and a hovering one still has
 * visible activity around it. */
static void fairy_draw_dust(float t, int passer_idx, const BackdropExtent *ex,
                            const float view[3], float alpha_scale) {
    glPointSize(3.5f);
    glBegin(GL_POINTS);
    for (int k = 1; k <= FAIRY_DUST_COUNT; k++) {
        float age = (float)k * FAIRY_DUST_DT;
        float back = t - age;
        float decay = 1.0f - (float)k / (float)(FAIRY_DUST_COUNT + 1);
        FairyState f;
        float jx, jz;

        if (back < 0.0f)
            break;
        fairy_sample_at(back, passer_idx, ex, view, &f);
        if (f.bright <= 0.0f)
            continue;

        /* Deterministic per-mote drift, in a cone that WIDENS with age -
         * a mote sitting on the trail reads as a dot on a streamer, so
         * each one has to walk away from where it was shed. */
        {
            float spread = ex->radius * 0.390f * age;
            jx = sinf((float)k * 12.9898f) * spread;
            jz = sinf((float)k * 78.233f) * spread;
        }

        glColor4f(0.30f + f.color[0] * 0.70f, 0.30f + f.color[1] * 0.70f,
                  0.30f + f.color[2] * 0.70f,
                  0.55f * decay * decay * f.bright * alpha_scale);
        glVertex3f(f.pos[0] + jx,
                   f.pos[1] - ex->radius * FAIRY_DUST_FALL * age * age +
                       sinf((float)k * 43.17f) * ex->radius * 0.02f,
                   f.pos[2] + jz);
    }
    glEnd();
}

/* One fairy body: a soft wing disc, a coloured halo point, and a white-hot
 * core point inside it. The two points are camera-facing for free, which is
 * why they are points and not billboarded quads. */
static void fairy_draw_body(const FairyState *f, const BackdropExtent *ex,
                            const float view[3], float alpha_scale,
                            float scale) {
    float a = f->bright * alpha_scale;
    float t1[3], t2[3];
    float wr = ex->radius * FAIRY_WING_R * scale;

    if (a <= 0.0f)
        return;

    /* Wing disc: a soft round bloom the points sit inside, so the fairy
     * still reads as having size when it is close to the camera and the
     * fixed-size point sprites stop growing. */
    backdrop_frame(view, t1, t2);
    glBegin(GL_TRIANGLE_FAN);
    glColor4f(f->color[0], f->color[1], f->color[2], 0.30f * a);
    glVertex3fv(f->pos);
    glColor4f(f->color[0], f->color[1], f->color[2], 0.0f);
    for (int s = 0; s <= FAIRY_WING_SEGS; s++) {
        float ang = (float)s / (float)FAIRY_WING_SEGS * 2.0f * (float)M_PI;
        float ca = cosf(ang) * wr, sa = sinf(ang) * wr;
        glVertex3f(f->pos[0] + t1[0] * ca + t2[0] * sa,
                   f->pos[1] + t1[1] * ca + t2[1] * sa,
                   f->pos[2] + t1[2] * ca + t2[2] * sa);
    }
    glEnd();

    glPointSize(FAIRY_HALO_POINT * scale);
    glBegin(GL_POINTS);
    glColor4f(f->color[0], f->color[1], f->color[2], FAIRY_HALO_ALPHA * a);
    glVertex3fv(f->pos);
    glEnd();

    /* Core washed toward white: a light source photographs as white in the
     * middle whatever colour it casts. */
    glPointSize(FAIRY_CORE_POINT * scale);
    glBegin(GL_POINTS);
    glColor4f(0.55f + f->color[0] * 0.45f,
              0.55f + f->color[1] * 0.45f,
              0.55f + f->color[2] * 0.45f, a);
    glVertex3fv(f->pos);
    glEnd();
}

static void draw_fairies(const Render3dFrameRenderContext *frame_ctx) {
    BackdropExtent ex;
    FairyState f;
    float view[3];
    float t = frame_ctx->config.anim_time;
    float alpha = frame_ctx->config.alpha_scale;

    backdrop_resolve_extent(frame_ctx, &ex);
    backdrop_camera_dir(frame_ctx, &ex, view);

    render3d_backdrop_push_state();
    backdrop_clear_user_volume_state();
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_FOG);
    /* Depth-tested so a fairy behind the geometry is hidden by it - the
     * inspection reads as circling the object, not floating over a picture
     * of it - but writing no depth, so the glows stack additively. */
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glEnable(GL_POINT_SMOOTH);
    glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
    if (frame_ctx->config.point_parameter_supported &&
        frame_ctx->config.point_parameter_proc)
        frame_ctx->config.point_parameter_proc(GL_POINT_DISTANCE_ATTENUATION,
                                               (GLfloat[]){1, 0, 0});

    /* Trails first, so a fairy's own body always draws over what it left. */
    for (int i = -1; i < FAIRY_PASSER_COUNT; i++) {
        fairy_draw_ribbon(t, i, &ex, view, alpha);
        fairy_draw_dust(t, i, &ex, view, alpha);
    }

    fairy_resident_at(t, &ex, view, &f);
    fairy_draw_wings(&f, &ex, view, t, 11u, alpha, 1.0f);
    fairy_draw_body(&f, &ex, view, alpha, 1.0f);
    for (int i = 0; i < FAIRY_PASSER_COUNT; i++) {
        fairy_passer_at(t, i, &ex, view, &f);
        fairy_draw_wings(&f, &ex, view, t, (unsigned int)(i * 31 + 7),
                         alpha, 0.8f);
        fairy_draw_body(&f, &ex, view, alpha, 0.8f);
    }

    render3d_backdrop_pop_state();
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
    { .id = GL_LIGHT4,
      .pos      = { 0.0f,  5.0f, -24.0f, 0.0f },
      .diffuse  = { 0.85f, 0.42f, 0.28f, 1.0f },
      .ambient  = { 0.05f, 0.02f, 0.03f, 1.0f },
      .specular = { 1.00f, 0.60f, 0.40f, 1.0f } },
    /* Violet dusk-sky fill from high behind the viewer. */
    { .id = GL_LIGHT5,
      .pos      = {  0.2f,  0.6f,  1.0f, 0.0f },
      .diffuse  = { 0.26f, 0.12f, 0.42f, 1.0f },
      .ambient  = { 0.02f, 0.01f, 0.04f, 1.0f },
      .specular = { 0.15f, 0.08f, 0.25f, 1.0f } },
    /* Hot-pink bounce off the neon floor, from below. */
    { .id = GL_LIGHT6,
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
    { .id = GL_LIGHT4,
      .pos      = {  0.45f,  0.65f, -0.60f, 0.0f },
      .diffuse  = { 0.55f, 0.18f, 0.48f, 1.0f },
      .ambient  = { 0.04f, 0.01f, 0.04f, 1.0f },
      .specular = { 0.65f, 0.30f, 0.60f, 1.0f } },
    /* Teal gas rim from the opposite low quarter. */
    { .id = GL_LIGHT5,
      .pos      = { -0.55f,  0.20f,  0.65f, 0.0f },
      .diffuse  = { 0.10f, 0.34f, 0.40f, 1.0f },
      .ambient  = { 0.01f, 0.02f, 0.03f, 1.0f },
      .specular = { 0.15f, 0.40f, 0.45f, 1.0f } },
    /* Indigo chart-floor bounce, from below. */
    { .id = GL_LIGHT6,
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
    { .id = GL_LIGHT4,
      .pos      = { 1.0f,  0.2f,  0.0f, 0.0f },
      .diffuse  = { 0.28f, 0.36f, 0.42f, 1.0f },
      .ambient  = { 0.10f, 0.12f, 0.15f, 1.0f },
      .specular = { 0.22f, 0.34f, 0.48f, 1.0f } },
    /* Steel-blue horizon fill from low behind the viewer. */
    { .id = GL_LIGHT5,
      .pos      = { 0.15f, 0.20f,  1.00f, 0.0f },
      .diffuse  = { 0.30f, 0.44f, 0.62f, 1.0f },
      .ambient  = { 0.02f, 0.03f, 0.05f, 1.0f },
      .specular = { 0.22f, 0.34f, 0.48f, 1.0f } },
    /* Pale glacial bounce from below - the ice-sheet reflection. */
    { .id = GL_LIGHT6,
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
        backdrop_apply_env_lights(
            k_polar_day_lights,
            (int)(sizeof(k_polar_day_lights) / sizeof(k_polar_day_lights[0])));
        break;
    /* The one moving rig: positions come from the patrol pose rather than
     * a table, and it is the only backdrop that sets spot state. */
    case RENDER3D_BACKDROP_DRONES:
        drone_setup_lights(frame_ctx);
        break;
    /* Positional, attenuated, no spot cone - see the fairy block. */
    case RENDER3D_BACKDROP_FAIRIES:
        fairy_setup_lights(frame_ctx);
        break;
    default:
        break;
    }
}

void render3d_backdrop_render(const Render3dFrameRenderContext *frame_ctx) {
    switch (frame_ctx->config.backdrop_mode) {
    case RENDER3D_BACKDROP_STARS:
        draw_starry_sky(frame_ctx->config.anim_time,
                        frame_ctx->config.point_parameter_supported,
                        frame_ctx->config.point_parameter_proc);
        break;
    case RENDER3D_BACKDROP_CITYSCAPE:
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
    case RENDER3D_BACKDROP_DRONES:
        draw_drones(frame_ctx);
        break;
    case RENDER3D_BACKDROP_FAIRIES:
        draw_fairies(frame_ctx);
        break;
    case RENDER3D_BACKDROP_OFF:
    default:
        break;
    }
}
