/*
 * gpuprof.h - GPU elapsed-time profiling via GL timer queries.
 *
 * The asynchronous twin of cpuprof: where prof_begin/prof_end measure CPU
 * wall time around a section's GL calls (mostly driver command-issue time
 * for an immediate-mode app), gpuprof brackets the same ProfSection ids
 * with GL timer queries so the panel can show what the GPU actually spent
 * executing them. Two measurement modes, picked at init by which entry
 * points the host could load:
 *
 *   - timestamp mode (preferred; glQueryCounter(GL_TIMESTAMP), needs
 *     GL_ARB_timer_query / GL 3.3): one marker per section transition;
 *     interval deltas tile the GPU timeline exactly, so per-section times
 *     are additive and never sum past the wall-clock frame.
 *   - elapsed mode (fallback; GL_TIME_ELAPSED_EXT brackets, the only
 *     option from GL_EXT_timer_query - e.g. Apple's GL 2.1): bracket
 *     windows can overlap on pipelined hardware; see the NOTE below.
 *
 * Results are read back asynchronously a few frames later via
 * GL_QUERY_RESULT_AVAILABLE polling - never a glFinish/glGetQueryObject
 * stall - so enabling it does not serialize the pipeline.
 *
 * Like cpuprof this file has NO app dependency (sections are opaque ints
 * from prof_sections.h via support/cpuprof.h's fallback) and NO GL-header
 * dependency: the five-and-a-half entry points it needs are injected as
 * function pointers at init (resolved by the host once a context exists),
 * and the three GL tokens are defined locally in gpuprof.c - the same
 * trick src/support/mesh_ply.c uses for feedback tokens. With no injected
 * functions (gpu_prof_init never called, or a NULL table) every call is a
 * cheap no-op and the read API reports "no data".
 *
 * Two GL realities shape the implementation (see gpuprof.c):
 *   - results lag: a ring of frame slots holds in-flight queries; the
 *     oldest slot is polled once per frame and harvested when ready.
 *   - GL_TIME_ELAPSED queries cannot nest (one active at a time), but
 *     prof sections do; begin/end therefore split the frame into
 *     non-overlapping *segments*, each tagged with the set of open
 *     sections, and a section's time is the sum of its segments.
 */
#ifndef GPUPROF_H
#define GPUPROF_H

/* ProfSection + PROF_SECTION_COUNT (or their standalone fallback). */
#include "support/cpuprof.h"

/* 64-bit query results (GLuint64EXT). Spelled here without GL headers. */
typedef unsigned long long GpuProfU64;

/* GL entry points injected by the host after a context exists. Signatures
 * mirror glGenQueries / glDeleteQueries / glBeginQuery / glEndQuery /
 * glGetQueryObjectiv / glGetQueryObjectui64vEXT / glQueryCounter with GL
 * typedefs spelled out (GLsizei=int, GLuint/GLenum=unsigned). The host
 * casts whatever the proc loader returns - standard GL-loader practice.
 * query_counter is OPTIONAL (NULL = elapsed mode); the host must only
 * fill it when GL_ARB_timer_query / GL 3.3 is actually advertised - proc
 * loaders can return callable junk for unsupported names. */
typedef struct {
    void (*gen_queries)(int n, unsigned *ids);
    void (*delete_queries)(int n, const unsigned *ids);
    void (*begin_query)(unsigned target, unsigned id);
    void (*end_query)(unsigned target);
    void (*get_query_iv)(unsigned id, unsigned pname, int *params);
    void (*get_query_ui64v)(unsigned id, unsigned pname, GpuProfU64 *params);
    void (*query_counter)(unsigned id, unsigned target);  /* optional */
} GpuProfGlFns;

/* Enable GPU profiling with the given entry points (the first six
 * required; query_counter optional and selects timestamp mode when set).
 * Needs a current GL context (query objects are generated here). Returns
 * 1 on success, 0 (and stays disabled) on a NULL table or missing entry.
 * Re-init tears down the existing state first. */
int  gpu_prof_init(const GpuProfGlFns *fns);

/* Nonzero when enabled in timestamp mode (additive interval deltas). */
int  gpu_prof_uses_timestamps(void);

/* Delete query objects and disable; every call no-ops again afterwards. */
void gpu_prof_shutdown(void);

int  gpu_prof_enabled(void);

/* Once per frame, before the first gpu_prof_begin: harvests any in-flight
 * frames whose results became available and opens this frame's slot. If
 * every ring slot is still in flight (GPU far behind) the frame goes
 * unsampled rather than blocking. */
void gpu_prof_frame_begin(void);

/* Bracket a section's GL calls. Nesting is fine (see header comment);
 * multiple begin/end pairs for one section in a frame sum naturally, so
 * accumulation-loop passes need no special API. */
void gpu_prof_begin(ProfSection s);
void gpu_prof_end(ProfSection s);

/* Read-only API for the panel. Values are us of GPU elapsed time, from
 * the most recent fully-harvested frame (typically 1-3 frames old).
 * has_data is 0 for sections never GPU-bracketed, while disabled, or
 * when samples went stale.
 *
 * NOTE on reading the numbers, by mode:
 *
 * Elapsed mode: a query measures the wall window from its bracket's first
 * command starting to its last command finishing on the GPU - not
 * exclusive busy time. On deeply pipelined and tile-deferred GPUs (e.g.
 * Apple Silicon behind GL-on-Metal, where a render pass's fragment work
 * is batched and lands inside every bracket that contributed draws to the
 * pass) the windows of different sections overlap, so sections - and
 * the frame total, which sums every segment - can add up to more than the
 * wall-clock frame time even at a locked 60 FPS. Treat the GPU column as
 * a relative-hotspot signal; the CPU column plus the frame rate remain
 * the budget ground truth.
 *
 * Timestamp mode: interval deltas between consecutive markers tile the
 * GPU timeline, so sums are exact and the frame total is a true window. On a
 * tile-deferred GPU attribution can still shift between sections sharing
 * a render pass (whichever interval contains the pass flush absorbs its
 * fragment time), but totals never over-count. */
double gpu_prof_section_last_us(ProfSection s);
double gpu_prof_section_avg_us(ProfSection s);
int    gpu_prof_section_has_data(ProfSection s);

#endif /* GPUPROF_H */
