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
 * is provided - e.g. these files lifted into another project unchanged - the
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
#include "support/histogram.h"

/* Fallback catalog: active only when prof_sections.h was not force-included
 * (PROF_SECTIONS_PROVIDED unset). Keeps the generic timer self-contained. */
#ifndef PROF_SECTIONS_PROVIDED
typedef int ProfSection;
enum { PROF_SECTION_COUNT = 1 };
#endif

/* A catalog-sized set of ProfSection indices.
 *
 * ProfSection itself is an ordinal used to index the profiler's per-section
 * arrays.  Places that need a set of those ordinals (GPU segments, collapsed
 * profile branches, hidden histogram series) use this multiword value rather
 * than treating the enum as a native-word bitmask.  The representation grows
 * automatically when the host catalog crosses a 64-section boundary. */
enum {
    PROF_SECTION_SET_WORD_BITS = 64,
    PROF_SECTION_SET_WORD_COUNT =
        (PROF_SECTION_COUNT + PROF_SECTION_SET_WORD_BITS - 1) /
        PROF_SECTION_SET_WORD_BITS
};

typedef struct {
    uint64_t words[PROF_SECTION_SET_WORD_COUNT];
} ProfSectionSet;

static inline ProfSectionSet prof_section_set_empty(void) {
    ProfSectionSet set = { { 0 } };
    return set;
}

static inline int prof_section_set_is_empty(const ProfSectionSet *set) {
    int word_idx;
    if (!set) return 1;
    for (word_idx = 0; word_idx < PROF_SECTION_SET_WORD_COUNT; word_idx++)
        if (set->words[word_idx] != 0)
            return 0;
    return 1;
}

static inline int prof_section_set_contains(const ProfSectionSet *set,
                                            ProfSection section) {
    unsigned int section_idx;
    unsigned int word_idx;
    unsigned int bit_idx;
    if (!set || section < 0 || section >= PROF_SECTION_COUNT)
        return 0;
    section_idx = (unsigned int)section;
    word_idx = section_idx / PROF_SECTION_SET_WORD_BITS;
    bit_idx = section_idx % PROF_SECTION_SET_WORD_BITS;
    return (set->words[word_idx] & (UINT64_C(1) << bit_idx)) != 0;
}

static inline void prof_section_set_add(ProfSectionSet *set,
                                        ProfSection section) {
    unsigned int section_idx;
    unsigned int word_idx;
    unsigned int bit_idx;
    if (!set || section < 0 || section >= PROF_SECTION_COUNT)
        return;
    section_idx = (unsigned int)section;
    word_idx = section_idx / PROF_SECTION_SET_WORD_BITS;
    bit_idx = section_idx % PROF_SECTION_SET_WORD_BITS;
    set->words[word_idx] |= UINT64_C(1) << bit_idx;
}

static inline void prof_section_set_toggle(ProfSectionSet *set,
                                           ProfSection section) {
    unsigned int section_idx;
    unsigned int word_idx;
    unsigned int bit_idx;
    if (!set || section < 0 || section >= PROF_SECTION_COUNT)
        return;
    section_idx = (unsigned int)section;
    word_idx = section_idx / PROF_SECTION_SET_WORD_BITS;
    bit_idx = section_idx % PROF_SECTION_SET_WORD_BITS;
    set->words[word_idx] ^= UINT64_C(1) << bit_idx;
}

static inline void prof_section_set_union_into(ProfSectionSet *set,
                                               const ProfSectionSet *other) {
    int word_idx;
    if (!set || !other) return;
    for (word_idx = 0; word_idx < PROF_SECTION_SET_WORD_COUNT; word_idx++)
        set->words[word_idx] |= other->words[word_idx];
}

static inline void prof_section_set_remove_all(ProfSectionSet *set,
                                               const ProfSectionSet *removed) {
    int word_idx;
    if (!set || !removed) return;
    for (word_idx = 0; word_idx < PROF_SECTION_SET_WORD_COUNT; word_idx++)
        set->words[word_idx] &= ~removed->words[word_idx];
}

static inline int prof_section_set_contains_all(
        const ProfSectionSet *set, const ProfSectionSet *required) {
    int word_idx;
    if (!set || !required) return 0;
    for (word_idx = 0; word_idx < PROF_SECTION_SET_WORD_COUNT; word_idx++)
        if ((set->words[word_idx] & required->words[word_idx]) !=
            required->words[word_idx])
            return 0;
    return 1;
}

static inline int prof_section_set_equal(const ProfSectionSet *a,
                                         const ProfSectionSet *b) {
    int word_idx;
    if (!a || !b) return a == b;
    for (word_idx = 0; word_idx < PROF_SECTION_SET_WORD_COUNT; word_idx++)
        if (a->words[word_idx] != b->words[word_idx])
            return 0;
    return 1;
}

/* Per-section display metadata, supplied by the app (not the generic timer).
 *  label    - bare section name for the HUD (no indentation baked in).
 *  depth    - nesting level (0 = top-level); the single source of truth for
 *             nesting. The panel derives the visual indentation from it and
 *             treats depth>0 as a "detail" sub-section hidden outside DETAILS
 *             mode - so restyling the indent never re-classifies a row.
 *  is_total - nonzero for a total row (drawn with a divider above it and the
 *             full-budget warn/crit thresholds).
 *  is_slack - nonzero for a row where a *bigger* number is the healthy one:
 *             it measures time the frame is handed rather than time it spends
 *             (gl-repl's Present row - the vsync wait). The panel inverts the
 *             warn/crit coloring for these, so a long wait reads green and a
 *             vanishing one red. Such a row is outside the frame total by
 *             construction; the flag only decides how it is colored.
 *  is_frame_total - nonzero only for the whole-frame total. It keeps the
 *             refresh-boundary tolerance specific to that row: another total
 *             uses the ordinary hard full-budget threshold.
 *
 * Implemented by src/app/glr_prof.c for the gl-repl binary, and by each
 * standalone demo driver for the sections it instruments. The generic timer
 * and the UI panel never hard-code section names. Drivers written before a
 * field was added keep working: a short positional initializer zero-fills the
 * rest, which is the "ordinary work row" default for every flag here. */
typedef struct {
    const char *label;
    int         depth;
    int         is_total;
    int         is_slack;
    int         is_frame_total;
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
 * timer queries (src/support/gpuprof.c) without touching any call site -
 * the hook implementation decides which sections it cares about. NULLs
 * uninstall. No-op-cheap when uninstalled; not used by the generic timer
 * itself. */
typedef void (*ProfSectionHookFn)(ProfSection s);
void prof_install_section_hooks(ProfSectionHookFn begin_hook,
                                ProfSectionHookFn end_hook);

/* Record a sample the caller computed rather than one this module timed:
 * updates last/EMA/histogram/staleness exactly as prof_end() would, with no
 * clock read and no hooks (there is no span to bracket a GPU query around).
 *
 * For a section that is a *difference* between two measured spans and would be
 * wrong to bracket directly - gl-repl's Present, which is the frame total minus
 * the frame's work. Bracketing the swap itself would leave everything between
 * the two spans unattributed; subtracting sweeps it into the row that is meant
 * to absorb it. Prefer prof_begin/prof_end wherever there is a real span. */
void prof_section_record_us(ProfSection s, double us);

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
 * One Histogram per section plus one for frame time. The distribution itself -
 * log-spaced bins, running statistics, and the reasoning behind both - belongs
 * to src/support/histogram.h; what lives here is only *when* a sample is fed.
 *
 * Section histograms are fed when a section publishes a sample:
 * prof_end() for direct sections and prof_accum_commit() for accumulated
 * sections. The frame-time histogram is fed by prof_frame_tick() using the
 * wall-clock delta between frame ticks, so it captures overall frame cadence
 * rather than just display-callback body time.
 *
 * The getters return the number of bins written / 1 on success, and 0 (leaving
 * *out untouched) for a bad section index or a NULL out. */
int prof_section_histogram(ProfSection s,
                           HistogramBin *out,
                           int max_bins);
int prof_frame_time_histogram(HistogramBin *out, int max_bins);

int prof_section_stats(ProfSection s, HistogramStats *out);
int prof_frame_time_stats(HistogramStats *out);

/* Zero every section histogram and the frame-time histogram - bins and
 * running statistics alike - so the next samples start a fresh distribution.
 * Call when the measured workload changes wholesale (loading a different
 * example / scene); a cumulative histogram must not mix different geometry
 * or carry its startup outliers forever. Leaves the EMAs, staleness and FPS
 * history alone:
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
