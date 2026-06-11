/*
 * cpuprof.c - CPU overhead profiling instrumentation.
 */
#define _POSIX_C_SOURCE 200809L
#include "support/cpuprof.h"

#include <time.h>
#include <stdio.h>

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
static int    g_prof_initialized = 0;

static ProfSectionHookFn g_prof_begin_hook = 0;
static ProfSectionHookFn g_prof_end_hook   = 0;

/* ========================================================================= */
/* Helpers                                                                    */
/* ========================================================================= */

#ifdef __APPLE__
static double prof_now_us(void) {
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
static double prof_now_us(void) {
    struct timespec ts;
    /* CLOCK_MONOTONIC is served by the vDSO on Linux, so this is a
     * single register-load worth of overhead rather than a full syscall. */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1000.0;
}
#endif

static void init_if_needed(void) {
    if (g_prof_initialized) return;
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        g_prof_start[section_idx]         = 0.0;
        g_prof_last_us[section_idx]       = 0.0;
        g_prof_avg_us[section_idx]        = 0.0;
        g_prof_stale[section_idx]         = PROF_STALE_FRAMES; /* treat as stale until first sample */
        g_prof_accum_pending[section_idx] = 0.0;
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
}

void prof_accum_end(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return;
    init_if_needed();
    double elapsed = prof_now_us() - g_prof_start[s];
    if (elapsed < 0.0) elapsed = 0.0;
    if (g_prof_end_hook) g_prof_end_hook(s);
    g_prof_accum_pending[s] += elapsed;
    g_prof_stale[s] = 0;
}

void prof_accum_commit(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return;
    init_if_needed();
    double total = g_prof_accum_pending[s];
    g_prof_last_us[s] = total;
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