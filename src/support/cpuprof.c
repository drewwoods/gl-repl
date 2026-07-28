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
static double g_prof_last_us[PROF_SECTION_COUNT];  /* last measured wall µs */
static double g_prof_avg_us[PROF_SECTION_COUNT];   /* EMA in µs             */
static int    g_prof_stale[PROF_SECTION_COUNT];    /* frames since last sample */
static double g_prof_accum_pending[PROF_SECTION_COUNT]; /* running total for accum-commit */
static int    g_prof_accum_sampled[PROF_SECTION_COUNT]; /* accum_end ran since reset */
/* One histogram: the bins plus the running statistics over the same samples.
 * Kept in one struct so a sample can never land in the bins without also
 * updating the stats, and so a reset clears both by construction.
 *
 * `mean`/`m2` are Welford's online accumulators (m2 = sum of squared deviations
 * from the running mean); `sum` is a separate plain total. See the ProfHistogram
 * Stats block in cpuprof.h for why the sum-of-squares shortcut is not used. */
typedef struct {
    ProfHistogramBin   bins[PROF_HISTOGRAM_BIN_COUNT];
    unsigned long long count;
    double             min_us, max_us;
    double             sum;
    double             mean;
    double             m2;
} ProfHistogram;

static ProfHistogram g_prof_hist[PROF_SECTION_COUNT];
static int    g_prof_initialized = 0;

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
static ProfHistogram g_frame_time_hist;

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

/* Decades per bin. The whole log-bin layout derives from this one constant. */
#define PROF_HIST_DECADES_PER_BIN \
    (PROF_HISTOGRAM_DECADES / (double)PROF_HISTOGRAM_BIN_COUNT)

int prof_histogram_bin_for_us(double us) {
    /* Underflow (including zero and the negative that a non-monotonic clock
     * could hand us) folds into bin 0; overflow into the last bin. */
    if (!(us > PROF_HISTOGRAM_MIN_US)) return 0;
    if (us >= PROF_HISTOGRAM_MAX_US) return PROF_HISTOGRAM_BIN_COUNT - 1;

    /* The epsilon makes a bin's exact lower edge land in that bin rather than
     * the one below: prof_histogram_bin_lo_us() reaches the edge through
     * pow(), and log10(pow(10, x)) comes back a hair under x, so the floor
     * would drop it. 1e-9 decades is ~1e-9 of a bin — far below any real
     * timing resolution — so it can only ever fix that boundary. */
    int bin = (int)(log10(us / PROF_HISTOGRAM_MIN_US)
                    / PROF_HIST_DECADES_PER_BIN + 1e-9);
    if (bin < 0) bin = 0;
    if (bin > PROF_HISTOGRAM_BIN_COUNT - 1) bin = PROF_HISTOGRAM_BIN_COUNT - 1;
    return bin;
}

double prof_histogram_bin_lo_us(int bin) {
    if (bin <= 0) return PROF_HISTOGRAM_MIN_US;
    if (bin > PROF_HISTOGRAM_BIN_COUNT - 1) bin = PROF_HISTOGRAM_BIN_COUNT - 1;
    return PROF_HISTOGRAM_MIN_US
         * pow(10.0, (double)bin * PROF_HIST_DECADES_PER_BIN);
}

double prof_histogram_bin_hi_us(int bin) {
    if (bin < 0) bin = 0;
    if (bin >= PROF_HISTOGRAM_BIN_COUNT - 1) return PROF_HISTOGRAM_MAX_US;
    return prof_histogram_bin_lo_us(bin + 1);
}

static void prof_histogram_record(ProfHistogram *h, double elapsed_us) {
    int bin = prof_histogram_bin_for_us(elapsed_us);
    double delta, delta2;

    if (h->bins[bin] < UINT32_MAX)
        h->bins[bin]++;

    /* Stats take the raw value, so they stay exact where the bins do not: at
     * the two open-ended edge bins, and below the ~1.36% bin width. The bin
     * saturation above deliberately does not gate this — a saturated bin stops
     * counting, the distribution's statistics should not. */
    if (h->count == 0 || elapsed_us < h->min_us) h->min_us = elapsed_us;
    if (h->count == 0 || elapsed_us > h->max_us) h->max_us = elapsed_us;
    h->sum += elapsed_us;
    h->count++;

    delta   = elapsed_us - h->mean;
    h->mean += delta / (double)h->count;
    delta2  = elapsed_us - h->mean;   /* deviation from the *updated* mean */
    h->m2  += delta * delta2;
}

static void prof_histogram_clear(ProfHistogram *h) {
    for (int bin_idx = 0; bin_idx < PROF_HISTOGRAM_BIN_COUNT; bin_idx++)
        h->bins[bin_idx] = 0;
    h->count  = 0;
    h->min_us = 0.0;
    h->max_us = 0.0;
    h->sum    = 0.0;
    h->mean   = 0.0;
    h->m2     = 0.0;
}

static void prof_histogram_read_stats(const ProfHistogram *h,
                                      ProfHistogramStats *out) {
    out->count        = h->count;
    out->min_us       = h->min_us;
    out->max_us       = h->max_us;
    out->sum_us       = h->sum;
    out->mean_us      = (h->count > 0) ? h->mean : 0.0;
    /* n-1 divisor: an ongoing sample of a running process, not a population.
     * m2 is a sum of squares and so cannot go negative, but a long run of
     * identical samples can leave it at -0.0; max() keeps sqrt() honest. */
    out->variance_us2 = (h->count > 1)
                      ? ((h->m2 > 0.0) ? h->m2 / (double)(h->count - 1) : 0.0)
                      : 0.0;
    out->stddev_us    = sqrt(out->variance_us2);
}

static void init_if_needed(void) {
    if (g_prof_initialized) return;
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        g_prof_start[section_idx]         = 0.0;
        g_prof_last_us[section_idx]       = 0.0;
        g_prof_avg_us[section_idx]        = 0.0;
        g_prof_stale[section_idx]         = PROF_STALE_FRAMES; /* treat as stale until first sample */
        g_prof_accum_pending[section_idx] = 0.0;
        g_prof_accum_sampled[section_idx] = 0;
    }
    g_prof_initialized = 1;
}

/* ========================================================================= */
/* Public API                                                                 */
/* ========================================================================= */

void prof_install_section_hooks(ProfSectionHookFn begin_hook,
                                ProfSectionHookFn end_hook) {
    g_prof_begin_hook = begin_hook;
    g_prof_end_hook   = end_hook;
}

void prof_begin(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return;
    init_if_needed();
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
    if (g_prof_end_hook) g_prof_end_hook(s);

    g_prof_last_us[s] = elapsed;
    g_prof_stale[s]   = 0;
    prof_histogram_record(&g_prof_hist[s], elapsed);

    if (g_prof_avg_us[s] == 0.0)
        g_prof_avg_us[s] = elapsed;  /* seed with first sample */
    else
        g_prof_avg_us[s] = PROF_EMA_ALPHA * elapsed
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
        prof_histogram_record(&g_prof_hist[s], total);
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
            prof_histogram_record(&g_frame_time_hist, dt);
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

int prof_section_histogram(ProfSection s,
                           ProfHistogramBin *out,
                           int max_bins) {
    if (s < 0 || s >= PROF_SECTION_COUNT || !out || max_bins <= 0)
        return 0;
    int n = max_bins < PROF_HISTOGRAM_BIN_COUNT
          ? max_bins
          : PROF_HISTOGRAM_BIN_COUNT;
    for (int i = 0; i < n; i++)
        out[i] = g_prof_hist[s].bins[i];
    return n;
}

int prof_frame_time_histogram(ProfHistogramBin *out, int max_bins) {
    if (!out || max_bins <= 0) return 0;
    int n = max_bins < PROF_HISTOGRAM_BIN_COUNT
          ? max_bins
          : PROF_HISTOGRAM_BIN_COUNT;
    for (int i = 0; i < n; i++)
        out[i] = g_frame_time_hist.bins[i];
    return n;
}

int prof_section_stats(ProfSection s, ProfHistogramStats *out) {
    if (s < 0 || s >= PROF_SECTION_COUNT || !out) return 0;
    prof_histogram_read_stats(&g_prof_hist[s], out);
    return 1;
}

int prof_frame_time_stats(ProfHistogramStats *out) {
    if (!out) return 0;
    prof_histogram_read_stats(&g_frame_time_hist, out);
    return 1;
}

void prof_histogram_reset(void) {
    init_if_needed();
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++)
        prof_histogram_clear(&g_prof_hist[section_idx]);
    prof_histogram_clear(&g_frame_time_hist);
}

void prof_test_reset(void) {
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        g_prof_start[section_idx]         = 0.0;
        g_prof_last_us[section_idx]       = 0.0;
        g_prof_avg_us[section_idx]        = 0.0;
        g_prof_stale[section_idx]         = PROF_STALE_FRAMES;
        g_prof_accum_pending[section_idx] = 0.0;
        g_prof_accum_sampled[section_idx] = 0;
        prof_histogram_clear(&g_prof_hist[section_idx]);
    }
    for (int win_idx = 0; win_idx < PROF_FPS_WIN_COUNT; win_idx++) {
        g_fps_win[win_idx].head = 0;
        g_fps_win[win_idx].count = 0;
        g_fps_win[win_idx].bucket_start_us = 0.0;
        g_fps_win[win_idx].bucket_frames = 0;
        for (int sample_idx = 0; sample_idx < PROF_FPS_HISTORY_CAP; sample_idx++)
            g_fps_win[win_idx].ring[sample_idx] = 0.0f;
    }
    prof_histogram_clear(&g_frame_time_hist);
    g_fps_last_tick_us = 0.0;
    g_fps_interval_ema_us = 0.0;
    g_prof_initialized = 0;
    g_prof_begin_hook = 0;
    g_prof_end_hook = 0;
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
