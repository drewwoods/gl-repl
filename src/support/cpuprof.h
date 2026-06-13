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

/* Smoothed instantaneous FPS (EMA of 1/frame-dt); 0 until two ticks. */
double prof_fps_current(void);

/* The window span in seconds for a PROF_FPS_WIN_* index (0 if invalid). */
double prof_fps_window_secs(int window);

/* Copy up to max_samples of a window's bucket history into out,
 * oldest -> newest. Returns the number of samples written. */
int    prof_fps_history(int window, float *out, int max_samples);

#endif /* CPUPROF_H */