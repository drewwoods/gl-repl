/*
 * memprof.h - Process memory profiling instrumentation.
 *
 * Tracks current resident set size (RSS) and virtual size (VSZ) of the
 * running process, holds a ring of historical samples taken every
 * MEMPROF_PUSH_INTERVAL_S seconds, and provides a pure formatter for
 * byte counts. Mirrors src/support/prof.{c,h} in conventions: file-scope
 * statics with `g_` prefix, no GL/UI deps, platform-conditional reader.
 */
#ifndef MEMPROF_H
#define MEMPROF_H

typedef struct {
    unsigned long long rss_bytes;
    unsigned long long vsz_bytes;
} MemSample;

/* Capture baseline using the module's monotonic clock as t0.
 * Safe to call multiple times - only first call takes. */
void memprof_init(void);

/* Per-frame entry point. Refreshes the cached "current" reading and
 * pushes a history sample when the wall-clock interval has elapsed.
 * The module owns its own monotonic clock; no parameter. */
void memprof_frame_tick(void);

/* --- Test seams (also safe to use in production, but exist to make the
 *     module deterministic under test). --- */

/* Init with an explicit t0, instead of querying the monotonic clock.
 * memprof_init() is implemented as memprof_init_at(memprof_now_s()).
 * Tests use this to control sample timestamps from a known origin. */
void memprof_init_at(double t0_seconds);

/* Drive the cadence with a virtual clock. The production frame-tick
 * goes through this with the real clock argument. Tests can call it
 * directly with monotonically-increasing virtual times. */
void memprof_frame_tick_at(double now_seconds);

/* Clear ALL module state: initialized flag, baseline, current, t0,
 * last-push timestamp, ring contents, count, head, injected reader.
 * Must be called in every test's setup (or teardown) to avoid state
 * leaking across cases when tests share the same process. */
void memprof_reset(void);

/* Inject a synthetic reader for tests so cadence/ring-wrap assertions
 * can compare exact MemSample values. NULL restores the platform reader.
 * The injected reader is cleared by memprof_reset(). */
typedef int (*MemprofReaderFn)(MemSample *out);
void memprof_set_reader(MemprofReaderFn reader);

/* --- Live state accessors. --- */

/* Cached current reading (always fresh - refreshed in every frame_tick). */
MemSample memprof_current(void);
MemSample memprof_baseline(void);

/* History ring: oldest-first traversal for the renderer.
 * sample_seconds_out is seconds-since-init for the i-th stored sample. */
int  memprof_history_count(void);
int  memprof_history_capacity(void);
void memprof_history_get(int i, MemSample *out, double *sample_seconds_out);

/* t_rel (seconds-since-t0) of the newest pushed sample. Used by the
 * renderer to right-align the newest sample to "now" without jiggle.
 * Returns 0.0 when history is empty. */
double memprof_history_latest_t(void);

/* Sweep helpers for graph auto-scale (cheap; capacity <= 1024). */
unsigned long long memprof_history_max_rss(void);
unsigned long long memprof_history_min_rss(void);
unsigned long long memprof_history_max_vsz(void);

/* --- Pure formatter (no platform deps; tested in test_memprof.c
 *     without pulling in any UI/GL code). --- */

/* Format byte count as "999 KB" / "1.4 MB" / "2.33 GB"; 0 -> "--".
 * Writes into buf and returns buf for chained use. */
const char *memprof_format_bytes(char *buf, int buf_sz,
                                 unsigned long long bytes);

#endif /* MEMPROF_H */
