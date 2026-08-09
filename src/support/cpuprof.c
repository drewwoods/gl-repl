/*
 * cpuprof.c - CPU overhead profiling instrumentation.
 */
#define _POSIX_C_SOURCE 200809L
#include "support/cpuprof.h"

#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

/* ========================================================================= */
/* Configuration                                                              */
/* ========================================================================= */

/* EMA smoothing factor.  Higher = more responsive, lower = smoother. */
#define PROF_EMA_ALPHA      0.04

/* After this many frames without a new sample, display "--" instead of the
 * stale value.  At 60 fps this is 3 seconds. */
#define PROF_STALE_FRAMES   180

/* ========================================================================= */
/* State                                                                      */
/* ========================================================================= */

static double g_prof_start[PROF_SECTION_COUNT];
static double g_prof_last_us[PROF_SECTION_COUNT];  /* last measured wall us */
static double g_prof_avg_us[PROF_SECTION_COUNT];   /* EMA in us             */
static int    g_prof_stale[PROF_SECTION_COUNT];    /* frames since last sample */
static double g_prof_accum_pending[PROF_SECTION_COUNT]; /* running total for accum-commit */
static int    g_prof_accum_sampled[PROF_SECTION_COUNT]; /* accum_end ran since reset */
/* One Histogram per section: the bins plus the running statistics over the
 * same samples, kept together by src/support/histogram.c so a sample can never
 * land in the bins without also updating the statistics, and so a clear resets
 * both by construction. */
static Histogram g_prof_hist[PROF_SECTION_COUNT];
static int    g_prof_initialized = 0;

/* --- Nesting guard (see prof_nesting_violations() in the header) ---
 *
 * The catalog's depth column claims a tree, but nothing enforced that a nested
 * row's span actually runs inside its parent's: the two brackets are usually in
 * different translation units, so no compile-time or source-level check can see
 * the relationship. What *is* knowable here is whether the parent is open at the
 * moments the child begins and ends - both, since containment is a claim about
 * the whole span and a child can just as easily outlive its parent as predate
 * it.
 *
 * g_prof_parent is derived from the depth column the host installs: a row's
 * parent is the nearest preceding row one level shallower - the same rule the
 * panel's branch walk uses. The depth column arrives through
 * prof_install_section_depth_fn() rather than being read from
 * prof_section_info() directly, because this TU stays catalog-free: binaries
 * that link it for the timers alone (the demos, the render3d tests) supply no
 * section table at all, and calling into one would leave them unlinkable. No
 * provider installed = no tree claimed = guard inert.
 *
 * Accumulated parents (prof_accum_*) need no exemption: they still prof_begin
 * around each of their legs, so a child that begins inside one sees an open
 * parent, and one that begins in the gap between legs is a real orphan. */
static signed char g_prof_open[PROF_SECTION_COUNT];
static signed char g_prof_flagged[PROF_SECTION_COUNT];
static short  g_prof_parent[PROF_SECTION_COUNT];
static int    g_prof_nesting_violations = 0;
static ProfSection g_prof_first_orphan = (ProfSection)0;
static int    g_prof_have_orphan = 0;
static ProfSectionDepthFn g_prof_depth_fn = 0;

static ProfSectionHookFn g_prof_begin_hook = 0;
static ProfSectionHookFn g_prof_end_hook   = 0;

/* FPS history: per-window bucket accumulators + sample rings, fed by
 * prof_frame_tick. A bucket closes once its window/CAP span has elapsed;
 * its value is frames / actual-span, so an irregular frame cadence (or a
 * long stall spanning several nominal buckets) still yields a correct
 * average for the time it covers. */
#define PROF_FPS_EMA_ALPHA 0.08

static const double k_fps_window_secs[PROF_FPS_WIN_COUNT] = {
    10.0, 60.0, 600.0
};

typedef struct {
    float  ring[PROF_FPS_HISTORY_CAP];
    int    head;             /* next write slot */
    int    count;            /* filled slots (caps at CAP) */
    double bucket_start_us;  /* 0 = bucket not started yet */
    int    bucket_frames;
} ProfFpsWindow;

static ProfFpsWindow g_fps_win[PROF_FPS_WIN_COUNT];
static double g_fps_last_tick_us = 0.0;
static double g_fps_interval_ema_us = 0.0;
static Histogram g_frame_time_hist;

static int g_prof_test_now_enabled = 0;
static double g_prof_test_now_us = 0.0;

/* ========================================================================= */
/* Helpers                                                                    */
/* ========================================================================= */

#ifdef __APPLE__
static double prof_platform_now_us(void) {
    static mach_timebase_info_data_t tb = {0, 0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    /* mach_absolute_time returns ticks; convert to nanoseconds via the
     * timebase, then to microseconds. Division by 1000.0 is in double to
     * keep sub-microsecond resolution for the EMA. */
    uint64_t ticks = mach_absolute_time();
    double nsec = (double)ticks * (double)tb.numer / (double)tb.denom;
    return nsec / 1000.0;
}
#else
static double prof_platform_now_us(void) {
    struct timespec ts;
    /* CLOCK_MONOTONIC is served by the vDSO on Linux, so this is a
     * single register-load worth of overhead rather than a full syscall. */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1000.0;
}
#endif

static double prof_now_us(void) {
    if (g_prof_test_now_enabled)
        return g_prof_test_now_us;
    return prof_platform_now_us();
}

static void prof_resolve_parents(void);

static void init_if_needed(void) {
    if (g_prof_initialized) return;
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        g_prof_start[section_idx]         = 0.0;
        g_prof_last_us[section_idx]       = 0.0;
        g_prof_avg_us[section_idx]        = 0.0;
        g_prof_stale[section_idx]         = PROF_STALE_FRAMES; /* treat as stale until first sample */
        g_prof_accum_pending[section_idx] = 0.0;
        g_prof_accum_sampled[section_idx] = 0;
        g_prof_open[section_idx]          = 0;
        g_prof_flagged[section_idx]       = 0;
    }
    g_prof_initialized = 1;
    prof_resolve_parents();
}

/* Resolve each row's parent: the nearest preceding row exactly one level
 * shallower. A row with no such predecessor (depth 0, or a depth the host left
 * with no parent above it) keeps -1 and is never checked. */
static void prof_resolve_parents(void) {
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        int depth = g_prof_depth_fn ? g_prof_depth_fn((ProfSection)section_idx)
                                   : 0;
        g_prof_parent[section_idx] = -1;
        if (depth <= 0) continue;
        for (int up = section_idx - 1; up >= 0; up--) {
            int up_depth = g_prof_depth_fn((ProfSection)up);
            if (up_depth == depth - 1) {
                g_prof_parent[section_idx] = (short)up;
                break;
            }
            if (up_depth < depth - 1) break;  /* left the branch */
        }
    }
}

void prof_install_section_depth_fn(ProfSectionDepthFn depth_fn) {
    init_if_needed();
    g_prof_depth_fn = depth_fn;
    prof_resolve_parents();
}

/* ========================================================================= */
/* Public API                                                                 */
/* ========================================================================= */

void prof_install_section_hooks(ProfSectionHookFn begin_hook,
                                ProfSectionHookFn end_hook) {
    g_prof_begin_hook = begin_hook;
    g_prof_end_hook   = end_hook;
}

/* Nonzero when s claims a parent that is not currently open. Containment needs
 * this at BOTH ends of the child's span: checking only the start passes
 * begin(parent) begin(child) end(parent) end(child), where the child outlives
 * the parent whose total it is supposed to be part of - the same lie as starting
 * outside it, just from the other side. A parent that closes early is reported
 * against the child, which is the row that renders wrong. */
static int prof_parent_is_open(ProfSection s) {
    int parent = g_prof_parent[s];
    return parent < 0 || g_prof_open[parent];
}

/* One violation per mis-nested span, not per event: g_prof_flagged remembers
 * that the open span already counted, so a child that both begins and ends
 * outside its parent is one bug rather than two. */
static void prof_note_orphan(ProfSection s) {
    g_prof_flagged[s] = 1;
    g_prof_nesting_violations++;
    if (!g_prof_have_orphan) {
        g_prof_first_orphan = s;
        g_prof_have_orphan = 1;
    }
}

void prof_begin(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return;
    init_if_needed();
    /* The indentation claims this span runs inside its parent's; check it while
     * the answer is still knowable. Before the hook and the clock read: the
     * comparison is two array loads, and an orphan is a catalog/call-site bug
     * either way. */
    g_prof_flagged[s] = 0;
    if (!prof_parent_is_open(s))
        prof_note_orphan(s);
    g_prof_open[s] = 1;
    /* Hook fires before the clock read so its cost (e.g. issuing a GPU
     * timer query) is excluded from this section's CPU time. */
    if (g_prof_begin_hook) g_prof_begin_hook(s);
    g_prof_start[s] = prof_now_us();
}

void prof_end(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return;
    init_if_needed();

    double elapsed = prof_now_us() - g_prof_start[s];
    if (elapsed < 0.0) elapsed = 0.0;
    /* Still inside the parent? A parent that closed first leaves this span
     * hanging outside the total it is drawn as part of. */
    if (!g_prof_flagged[s] && !prof_parent_is_open(s))
        prof_note_orphan(s);
    g_prof_open[s] = 0;
    if (g_prof_end_hook) g_prof_end_hook(s);

    g_prof_last_us[s] = elapsed;
    g_prof_stale[s]   = 0;
    histogram_record(&g_prof_hist[s], elapsed);

    if (g_prof_avg_us[s] == 0.0)
        g_prof_avg_us[s] = elapsed;  /* seed with first sample */
    else
        g_prof_avg_us[s] = PROF_EMA_ALPHA * elapsed
                         + (1.0 - PROF_EMA_ALPHA) * g_prof_avg_us[s];
}

void prof_section_record_us(ProfSection s, double us) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return;
    init_if_needed();
    if (us < 0.0) us = 0.0;

    g_prof_last_us[s] = us;
    g_prof_stale[s]   = 0;
    histogram_record(&g_prof_hist[s], us);

    if (g_prof_avg_us[s] == 0.0)
        g_prof_avg_us[s] = us;       /* seed with first sample */
    else
        g_prof_avg_us[s] = PROF_EMA_ALPHA * us
                         + (1.0 - PROF_EMA_ALPHA) * g_prof_avg_us[s];
}

void prof_accum_reset(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return;
    init_if_needed();
    g_prof_accum_pending[s] = 0.0;
    g_prof_accum_sampled[s] = 0;
}

void prof_accum_end(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return;
    init_if_needed();
    double elapsed = prof_now_us() - g_prof_start[s];
    if (elapsed < 0.0) elapsed = 0.0;
    if (!g_prof_flagged[s] && !prof_parent_is_open(s))
        prof_note_orphan(s);
    g_prof_open[s] = 0;
    if (g_prof_end_hook) g_prof_end_hook(s);
    g_prof_accum_pending[s] += elapsed;
    g_prof_accum_sampled[s] = 1;
    g_prof_stale[s] = 0;
}

void prof_accum_commit(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return;
    init_if_needed();
    double total = g_prof_accum_pending[s];
    g_prof_last_us[s] = total;
    if (g_prof_accum_sampled[s]) {
        histogram_record(&g_prof_hist[s], total);
        g_prof_accum_sampled[s] = 0;
    }
    if (g_prof_avg_us[s] == 0.0)
        g_prof_avg_us[s] = total;
    else
        g_prof_avg_us[s] = PROF_EMA_ALPHA * total
                         + (1.0 - PROF_EMA_ALPHA) * g_prof_avg_us[s];
}

void prof_frame_tick(void) {
    init_if_needed();
    /* Increment staleness counters for all sections.  For sections that run
     * during this frame prof_end() will reset their counter back to 0, so the
     * transient increment here is invisible by the time the panel renders. */
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        if (g_prof_stale[section_idx] < PROF_STALE_FRAMES)
            g_prof_stale[section_idx]++;
    }

    /* FPS bookkeeping: one tick = one frame. */
    double now = prof_now_us();
    if (g_fps_last_tick_us > 0.0) {
        double dt = now - g_fps_last_tick_us;
        if (dt > 0.0) {
            histogram_record(&g_frame_time_hist, dt);
            /* Smooth elapsed time, then invert it. Averaging per-frame
             * reciprocals (EMA(1 / dt)) biases FPS upward whenever callback
             * spacing is uneven; 1 / EMA(dt) tracks the actual callback
             * throughput instead. */
            g_fps_interval_ema_us = (g_fps_interval_ema_us == 0.0)
                ? dt
                : PROF_FPS_EMA_ALPHA * dt
                  + (1.0 - PROF_FPS_EMA_ALPHA) * g_fps_interval_ema_us;
        }
        for (int w = 0; w < PROF_FPS_WIN_COUNT; w++) {
            ProfFpsWindow *win = &g_fps_win[w];
            double bucket_us =
                k_fps_window_secs[w] * 1e6 / (double)PROF_FPS_HISTORY_CAP;
            if (win->bucket_start_us == 0.0)
                win->bucket_start_us = g_fps_last_tick_us;
            win->bucket_frames++;
            double span = now - win->bucket_start_us;
            if (span >= bucket_us) {
                win->ring[win->head] =
                    (float)((double)win->bucket_frames * 1e6 / span);
                win->head = (win->head + 1) % PROF_FPS_HISTORY_CAP;
                if (win->count < PROF_FPS_HISTORY_CAP)
                    win->count++;
                win->bucket_start_us = now;
                win->bucket_frames = 0;
            }
        }
    }
    g_fps_last_tick_us = now;
}

double prof_fps_current(void) {
    return g_fps_interval_ema_us > 0.0
         ? 1e6 / g_fps_interval_ema_us
         : 0.0;
}


int prof_fps_history(int window, float *out, int max_samples) {
    if (window < 0 || window >= PROF_FPS_WIN_COUNT || !out || max_samples <= 0)
        return 0;
    const ProfFpsWindow *win = &g_fps_win[window];
    int n = (win->count < max_samples) ? win->count : max_samples;
    for (int i = 0; i < n; i++) {
        int idx = (win->head - n + i + 2 * PROF_FPS_HISTORY_CAP)
                  % PROF_FPS_HISTORY_CAP;
        out[i] = win->ring[idx];
    }
    return n;
}

double prof_section_last_us(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return 0.0;
    return g_prof_last_us[s];
}

double prof_section_avg_us(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return 0.0;
    return g_prof_avg_us[s];
}

int prof_section_is_stale(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return 1;
    return g_prof_stale[s] >= PROF_STALE_FRAMES;
}

int prof_nesting_violations(void) {
    return g_prof_nesting_violations;
}

ProfSection prof_first_nesting_violation(void) {
    return g_prof_first_orphan;
}

int prof_section_sampled_this_frame(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return 0;
    /* prof_frame_tick() bumps the counter and every publisher zeroes it, so
     * zero means "a sample landed since the tick that opened this frame". */
    return g_prof_stale[s] == 0;
}

int prof_section_histogram(ProfSection s,
                           HistogramBin *out,
                           int max_bins) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return 0;
    return histogram_bins(&g_prof_hist[s], out, max_bins);
}

int prof_frame_time_histogram(HistogramBin *out, int max_bins) {
    return histogram_bins(&g_frame_time_hist, out, max_bins);
}

int prof_section_stats(ProfSection s, HistogramStats *out) {
    if (s < 0 || s >= PROF_SECTION_COUNT || !out) return 0;
    histogram_read_stats(&g_prof_hist[s], out);
    return 1;
}

int prof_frame_time_stats(HistogramStats *out) {
    if (!out) return 0;
    histogram_read_stats(&g_frame_time_hist, out);
    return 1;
}

void prof_histogram_reset(void) {
    init_if_needed();
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++)
        histogram_clear(&g_prof_hist[section_idx]);
    histogram_clear(&g_frame_time_hist);
}

void prof_test_reset(void) {
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        g_prof_start[section_idx]         = 0.0;
        g_prof_last_us[section_idx]       = 0.0;
        g_prof_avg_us[section_idx]        = 0.0;
        g_prof_stale[section_idx]         = PROF_STALE_FRAMES;
        g_prof_accum_pending[section_idx] = 0.0;
        g_prof_accum_sampled[section_idx] = 0;
        g_prof_open[section_idx]          = 0;
        g_prof_flagged[section_idx]       = 0;
        histogram_clear(&g_prof_hist[section_idx]);
    }
    /* A test that deliberately mis-nests a pair must not leave the count set
     * for whatever runs next. */
    g_prof_nesting_violations = 0;
    g_prof_have_orphan = 0;
    g_prof_first_orphan = (ProfSection)0;
    for (int win_idx = 0; win_idx < PROF_FPS_WIN_COUNT; win_idx++) {
        g_fps_win[win_idx].head = 0;
        g_fps_win[win_idx].count = 0;
        g_fps_win[win_idx].bucket_start_us = 0.0;
        g_fps_win[win_idx].bucket_frames = 0;
        for (int sample_idx = 0; sample_idx < PROF_FPS_HISTORY_CAP; sample_idx++)
            g_fps_win[win_idx].ring[sample_idx] = 0.0f;
    }
    histogram_clear(&g_frame_time_hist);
    g_fps_last_tick_us = 0.0;
    g_fps_interval_ema_us = 0.0;
    g_prof_initialized = 0;
    g_prof_begin_hook = 0;
    g_prof_end_hook = 0;
    /* Uninstalled, like the hooks: a reset returns a pristine profiler, and a
     * test that wants the nesting guard installs the tree after resetting. */
    g_prof_depth_fn = 0;
    g_prof_test_now_enabled = 0;
    g_prof_test_now_us = 0.0;
}

void prof_test_set_now_us(double now_us) {
    g_prof_test_now_enabled = 1;
    g_prof_test_now_us = now_us;
}

void prof_test_clear_now_us(void) {
    g_prof_test_now_enabled = 0;
}
