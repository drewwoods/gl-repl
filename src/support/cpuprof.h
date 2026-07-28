/*
 * cpuprof.h - CPU overhead profiling instrumentation.
 *
 * Measures elapsed wall time spent in major per-frame sections.
 * Instrumentation is generic and does not depend on the UI layer.
 *
 * This file has NO app dependency. A section is just an int index; the catalog
 * of named sections (the `ProfSection` enum + `PROF_SECTION_COUNT`) is supplied
 * by the host via prof_sections.h, which the Makefile force-includes into every
 * TU (`-include prof_sections.h`, beside `-include config.h`). When no catalog
 * is provided — e.g. these files lifted into another project unchanged — the
 * fallback below makes ProfSection an opaque int and the timer degenerates to a
 * single section, so cpuprof.{c,h} still compile and link standalone.
 *
 * The per-section *display* (label / nesting depth / is-total flag) is likewise
 * the host's job, returned by prof_section_info(): src/app/glr_prof.c for the
 * gl-repl binary, the driver itself for each standalone demo. The generic timer
 * and the UI panel never hard-code section names.
 */
#ifndef CPUPROF_H
#define CPUPROF_H

#include <stdint.h>

/* Fallback catalog: active only when prof_sections.h was not force-included
 * (PROF_SECTIONS_PROVIDED unset). Keeps the generic timer self-contained. */
#ifndef PROF_SECTIONS_PROVIDED
typedef int ProfSection;
enum { PROF_SECTION_COUNT = 1 };
#endif

/* Per-section display metadata, supplied by the app (not the generic timer).
 *  label    — bare section name for the HUD (no indentation baked in).
 *  depth    — nesting level (0 = top-level); the single source of truth for
 *             nesting. The panel derives the visual indentation from it and
 *             treats depth>0 as a "detail" sub-section hidden outside DETAILS
 *             mode — so restyling the indent never re-classifies a row.
 *  is_total — nonzero for the single whole-frame total row (drawn with a
 *             divider above it and its own warn/crit thresholds).
 *
 * Implemented by src/app/glr_prof.c for the gl-repl binary, and by each
 * standalone demo driver for the sections it instruments. The generic timer
 * and the UI panel never hard-code section names. */
typedef struct {
    const char *label;
    int         depth;
    int         is_total;
} ProfSectionInfo;

ProfSectionInfo prof_section_info(ProfSection s);

/* Begin/end a named CPU-time measurement.
 * prof_begin stores the current process CPU clock; prof_end records the
 * elapsed time and updates the running average for section s. */
void prof_begin(ProfSection s);
void prof_end(ProfSection s);

/* Optional per-section begin/end hooks, fired from prof_begin (before the
 * clock is read) and prof_end / prof_accum_end (after the elapsed time is
 * taken) so hook cost stays out of the section's own CPU measurement.
 * This is the seam the app uses to bracket the same sections with GPU
 * timer queries (src/support/gpuprof.c) without touching any call site —
 * the hook implementation decides which sections it cares about. NULLs
 * uninstall. No-op-cheap when uninstalled; not used by the generic timer
 * itself. */
typedef void (*ProfSectionHookFn)(ProfSection s);
void prof_install_section_hooks(ProfSectionHookFn begin_hook,
                                ProfSectionHookFn end_hook);

/* Accumulation-aware timing helpers for sections called inside a multi-pass
 * loop (e.g. accumulation-buffer AA).
 *
 * Usage:
 *   prof_accum_reset(s);          // once, before the loop
 *   for (...) {
 *       prof_begin(s);
 *       ...work...
 *       prof_accum_end(s);        // adds elapsed to a running total
 *   }
 *   prof_accum_commit(s);         // stores total as last_us, updates EMA
 */
void prof_accum_reset(ProfSection s);
void prof_accum_end(ProfSection s);
void prof_accum_commit(ProfSection s);

/* Mark the start of a new frame so per-frame sections can detect staleness.
 * Also feeds the FPS history below (one tick = one frame). */
void prof_frame_tick(void);

/* Read-only API for HUD rendering. */
double prof_section_last_us(ProfSection s);
double prof_section_avg_us(ProfSection s);
int    prof_section_is_stale(ProfSection s);

/* --- Fixed timing histograms ---
 *
 * Each histogram is 1024 bins spaced *logarithmically* over 1 us .. 1 s, so
 * every bin is the same constant ratio (~1.36%) wide rather than the same
 * number of microseconds. Bin counts are 32-bit and saturate at UINT32_MAX.
 *
 * Log spacing, not linear, because the sections being timed span four or five
 * decades at once: a whole frame runs in milliseconds while the cheap sections
 * around it run in microseconds. Linear bins wide enough to reach a 100 ms
 * stall (~100 us each) drop every sub-100 us section into bin 0, where they
 * are mutually indistinguishable and no plot can pull them apart. Constant
 * *relative* resolution measures a 3 us section as precisely as a 30 ms one,
 * and demotes an outlier from "blows out the whole scale" to "one more bin".
 *
 * Bin 0 also absorbs the underflow (samples below 1 us, i.e. the sections that
 * are effectively free); the last bin also absorbs the overflow (>= 1 s).
 *
 * Section histograms are fed when a section publishes a sample:
 * prof_end() for direct sections and prof_accum_commit() for accumulated
 * sections. The frame-time histogram is fed by prof_frame_tick() using the
 * wall-clock delta between frame ticks, so it captures overall frame cadence
 * rather than just display-callback body time. */
#define PROF_HISTOGRAM_BIN_COUNT 1024
#define PROF_HISTOGRAM_MIN_US    1.0        /* bin 0's nominal lower edge */
#define PROF_HISTOGRAM_MAX_US    1000000.0  /* last bin's upper edge (1 s) */
#define PROF_HISTOGRAM_DECADES   6.0        /* log10(MAX_US / MIN_US)      */
typedef uint32_t ProfHistogramBin;

/* Bin containing `us`, clamped into [0, PROF_HISTOGRAM_BIN_COUNT - 1]. */
int prof_histogram_bin_for_us(double us);

/* The bin's time bounds in us: [lo, hi). bin 0's true lower bound is 0 and
 * the last bin's true upper bound is infinite (see the underflow/overflow
 * note above); these report the nominal log-spaced edges. */
double prof_histogram_bin_lo_us(int bin);
double prof_histogram_bin_hi_us(int bin);

int prof_section_histogram(ProfSection s,
                           ProfHistogramBin *out,
                           int max_bins);
int prof_frame_time_histogram(ProfHistogramBin *out, int max_bins);

/* --- Running statistics, one set per histogram ---
 *
 * Maintained from the same samples that feed the bins, over the same
 * cumulative window (prof_histogram_reset() clears both together), so the
 * numbers always describe exactly the distribution being drawn.
 *
 * They are accumulated from the raw microsecond values, not read back out of
 * the bins: the bins quantize to ~1.36% and fold their two outer bins into
 * open-ended underflow/overflow buckets, so a bin-derived min/max/mean would
 * be approximate at best and simply wrong at the edges. A sub-microsecond
 * section reports its true mean here while sitting entirely in bin 0.
 *
 * variance_us2 / stddev_us are the *sample* statistics (the n-1 divisor):
 * these are an ongoing sample of a process that keeps running, not a closed
 * population. Both are 0 until the second sample arrives.
 *
 * mean/variance use Welford's online update rather than accumulating a sum of
 * squares — over a long run sum_us^2 dwarfs the spread, and the textbook
 * E[x^2] - E[x]^2 form cancels away the significant digits that carry it
 * (a section that runs 16 ms every frame for an hour would report a negative
 * variance). sum_us is kept as its own plain running total, which is exact
 * enough for a total and is the one figure that cannot be recovered from the
 * others.
 *
 * Getters return 1 and fill *out on success; 0 (leaving *out untouched) for a
 * bad section index or a NULL out. With no samples yet, every field is 0. */
typedef struct {
    unsigned long long count;      /* samples recorded since the last reset */
    double min_us, max_us;         /* extremes of the raw samples           */
    double sum_us;                 /* running total (all time in section)   */
    double mean_us;
    double variance_us2;           /* sample variance, in us^2              */
    double stddev_us;              /* sqrt(variance_us2), in us             */
} ProfHistogramStats;

int prof_section_stats(ProfSection s, ProfHistogramStats *out);
int prof_frame_time_stats(ProfHistogramStats *out);

/* Zero every section histogram and the frame-time histogram — bins and
 * running statistics alike — so the next samples start a fresh distribution.
 * Call when the measured workload is
 * replaced wholesale (loading a different example / scene) — the old shape
 * describes different geometry, and its startup outliers would otherwise sit
 * in the bins forever. Leaves the EMAs, staleness and FPS history alone:
 * those are self-correcting over a few frames, the histograms are cumulative
 * and are not. */
void prof_histogram_reset(void);

/* --- FPS history (fed by prof_frame_tick) ---
 *
 * Three fixed time windows, each a ring of PROF_FPS_HISTORY_CAP buckets
 * (window / CAP seconds per bucket; a bucket's value is the average FPS
 * over its span). Rings fill from empty, newest sample last. */
enum {
    PROF_FPS_WIN_10S = 0,
    PROF_FPS_WIN_1M,
    PROF_FPS_WIN_10M,
    PROF_FPS_WIN_COUNT
};
#define PROF_FPS_HISTORY_CAP 120

/* Smoothed instantaneous FPS (1 / EMA(frame-dt)); 0 until two ticks. */
double prof_fps_current(void);


/* Copy up to max_samples of a window's bucket history into out,
 * oldest -> newest. Returns the number of samples written. */
int    prof_fps_history(int window, float *out, int max_samples);

/* Test support: deterministic clock/reset hooks for tests. */
void prof_test_reset(void);
void prof_test_set_now_us(double now_us);
void prof_test_clear_now_us(void);

#endif /* CPUPROF_H */
