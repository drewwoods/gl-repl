/*
 * glr_perf_hint.c - see glr_perf_hint.h.
 *
 * Ranking, debounce, display-relative thresholds, and the two suppression
 * sources all live here so the controller only has to tick and snapshot.
 */
#include "app/glr_perf_hint.h"

#include "app/glr_config.h"          /* GLR_POST_FX_SCOPE_* */
#include "render3d/render_types.h"   /* RENDER3D_ACCUM_EFFECT_* */

#include <string.h>

#define MASK_BIT(c) (1 << (c))

static const int k_rank[] = {
    GLR_PERF_CULPRIT_ACCUM,
    GLR_PERF_CULPRIT_POST_FX_FRAME,
    GLR_PERF_CULPRIT_POST_FX_VIEW,
    GLR_PERF_CULPRIT_LINE_SMOOTH,
};

typedef struct {
    int    active;
    int    fps_rounded;
    int    culprit;
    int    culprit_count;
    int    mask;
    int    dismissed;                 /* [x]: hide this configuration */
    GlrPerfHintInputs last_in;        /* last tick's bag, for dismiss */
    GlrPerfHintInputs dismissed_in;   /* configuration [x] hid */
    int    capture_session;
    double warmup_us;
    double trip_acc_us;
    double release_acc_us;
    double ceiling_fps;   /* 0 = no clean sample yet */
} GlrPerfHintState;

static GlrPerfHintState g;

static double hint_min(double a, double b) {
    return a < b ? a : b;
}

static int culprit_mask(const GlrPerfHintInputs *in) {
    int m = 0;

    if (!in)
        return 0;
    if (in->use_accum &&
        in->accum_effect != RENDER3D_ACCUM_EFFECT_OFF &&
        in->accum_passes > 1)
        m |= MASK_BIT(GLR_PERF_CULPRIT_ACCUM);
    if (in->post_fx_scope == GLR_POST_FX_SCOPE_FRAME)
        m |= MASK_BIT(GLR_PERF_CULPRIT_POST_FX_FRAME);
    else if (in->post_fx_scope == GLR_POST_FX_SCOPE_VIEW_3D)
        m |= MASK_BIT(GLR_PERF_CULPRIT_POST_FX_VIEW);
    if (in->line_smooth_enabled)
        m |= MASK_BIT(GLR_PERF_CULPRIT_LINE_SMOOTH);
    return m;
}

static int highest_culprit(int mask) {
    int i;

    for (i = 0; i < (int)(sizeof k_rank / sizeof k_rank[0]); i++) {
        if (mask & MASK_BIT(k_rank[i]))
            return k_rank[i];
    }
    return GLR_PERF_CULPRIT_NONE;
}

static int mask_count(int mask) {
    int i, n = 0;

    for (i = 0; i < (int)(sizeof k_rank / sizeof k_rank[0]); i++) {
        if (mask & MASK_BIT(k_rank[i]))
            n++;
    }
    return n;
}

static int dismiss_inputs_equal(const GlrPerfHintInputs *a,
                                const GlrPerfHintInputs *b) {
    return a->use_accum == b->use_accum &&
           a->accum_effect == b->accum_effect &&
           a->accum_passes == b->accum_passes &&
           a->line_smooth_enabled == b->line_smooth_enabled &&
           a->post_fx_scope == b->post_fx_scope &&
           a->post_fx_effect == b->post_fx_effect;
}

static void apply_mask(int mask) {
    g.mask = mask;
    g.culprit = highest_culprit(mask);
    g.culprit_count = mask_count(mask);
    if (mask == 0) {
        g.active = 0;
        g.trip_acc_us = 0.0;
        g.release_acc_us = 0.0;
        g.dismissed = 0;
    }
}

static void trip_thresholds(double *trip, double *release) {
    if (g.ceiling_fps > 0.0) {
        *trip = hint_min(GLR_PERF_HINT_FPS_TRIP,
                         g.ceiling_fps * GLR_PERF_HINT_TRIP_FRAC);
        *release = hint_min(GLR_PERF_HINT_FPS_CLEAR,
                            g.ceiling_fps * GLR_PERF_HINT_CLEAR_FRAC);
    } else {
        *trip = GLR_PERF_HINT_FPS_TRIP;
        *release = GLR_PERF_HINT_FPS_CLEAR;
    }
}

void glr_perf_hint_tick(double fps, double dt_us, const GlrPerfHintInputs *in) {
    int mask = culprit_mask(in);
    int suppressed = g.capture_session || (in && in->pointer_script_active);
    double trip, release, inst;

    if (in)
        g.last_in = *in;
    apply_mask(mask);

    if (suppressed) {
        g.active = 0;
        g.trip_acc_us = 0.0;
        g.release_acc_us = 0.0;
        g.warmup_us = 0.0;
        return;
    }

    /* Initialization / no sample: refresh the mask, touch no timers. */
    if (fps <= 0.0 || dt_us <= 0.0)
        return;

    if (dt_us > GLR_PERF_HINT_DISCONTINUITY_US) {
        g.trip_acc_us = 0.0;
        g.release_acc_us = 0.0;
        return;
    }

    if (mask == 0 && fps > g.ceiling_fps)
        g.ceiling_fps = fps;

    if (g.warmup_us < GLR_PERF_HINT_WARMUP_US) {
        g.warmup_us += dt_us;
        return;
    }

    if (mask == 0)
        return;

    /* [x] hides this exact configuration while it stays slow. Changing a
     * blamed setting (16x -> 8x, Blur -> AA, ...) re-arms immediately;
     * recovering above the release floor for TRIP_US expires the dismiss
     * so a later slowdown can warn again. */
    if (g.dismissed) {
        if (!in || !dismiss_inputs_equal(&g.dismissed_in, in)) {
            g.dismissed = 0;
            g.release_acc_us = 0.0;
        } else {
            trip_thresholds(&trip, &release);
            inst = 1000000.0 / dt_us;
            if (fps > release && inst > release) {
                g.release_acc_us += dt_us;
                if (g.release_acc_us >= GLR_PERF_HINT_TRIP_US)
                    g.dismissed = 0;
            } else {
                g.release_acc_us = 0.0;
            }
            g.active = 0;
            return;
        }
    }

    trip_thresholds(&trip, &release);
    inst = 1000000.0 / dt_us;

    if (!g.active) {
        if (fps < trip && inst < trip) {
            g.trip_acc_us += dt_us;
            if (g.trip_acc_us >= GLR_PERF_HINT_TRIP_US)
                g.active = 1;
        } else {
            g.trip_acc_us = 0.0;
        }
    } else {
        if (fps > release && inst > release) {
            g.release_acc_us += dt_us;
            if (g.release_acc_us >= GLR_PERF_HINT_TRIP_US) {
                g.active = 0;
                g.trip_acc_us = 0.0;
                g.release_acc_us = 0.0;
            }
        } else {
            g.release_acc_us = 0.0;
        }
    }

    if (g.active)
        g.fps_rounded = (int)(fps + 0.5);
}

GlrPerfHintView glr_perf_hint_view(void) {
    GlrPerfHintView v;

    v.active = g.active;
    v.fps = g.active ? g.fps_rounded : 0;
    v.culprit = g.culprit;
    v.culprit_count = g.culprit_count;
    return v;
}

void glr_perf_hint_dismiss(void) {
    g.dismissed = 1;
    g.dismissed_in = g.last_in;
    g.active = 0;
    g.trip_acc_us = 0.0;
    g.release_acc_us = 0.0;
}

void glr_perf_hint_reset(void) {
    g.active = 0;
    g.fps_rounded = 0;
    g.trip_acc_us = 0.0;
    g.release_acc_us = 0.0;
    g.warmup_us = 0.0;
    g.dismissed = 0;
}

void glr_perf_hint_set_capture_session(int capturing) {
    if (capturing)
        g.capture_session = 1;
}

void glr_perf_hint_reset_for_test(void) {
    memset(&g, 0, sizeof g);
}
