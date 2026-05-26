/*
 * memprof.c - Process memory profiling instrumentation.
 */
#define _POSIX_C_SOURCE 200809L
#include "support/memprof.h"

#include <stdio.h>
#include <time.h>

#if defined(__APPLE__)
#  include <mach/mach.h>
#  include <mach/mach_time.h>
#  include <mach/task.h>
#elif defined(__linux__)
#  include <unistd.h>
#elif defined(_WIN32)
   /* Future: <windows.h> + <psapi.h>; GetProcessMemoryInfo. */
#endif

/* ========================================================================= */
/* Configuration                                                              */
/* ========================================================================= */

#define MEMPROF_HISTORY_CAP        1024
#define MEMPROF_PUSH_INTERVAL_S    5.0
/* Total span = 1024 * 5 = 5120s ~= 85 min */

/* ========================================================================= */
/* State                                                                      */
/* ========================================================================= */

static int             g_memprof_initialized = 0;
static MemSample       g_memprof_baseline    = {0};
static MemSample       g_memprof_current     = {0};
static double          g_memprof_t0_seconds  = 0.0;
static double          g_memprof_last_push_s = 0.0; /* relative to t0 */
static int             g_memprof_count       = 0;
static int             g_memprof_head        = 0;
static MemSample       g_memprof_ring[MEMPROF_HISTORY_CAP];
static double          g_memprof_ring_t[MEMPROF_HISTORY_CAP];
static MemprofReaderFn g_memprof_reader      = NULL; /* NULL -> memprof_read */

/* ========================================================================= */
/* Reader (platform-conditional)                                              */
/* ========================================================================= */

static int memprof_read(MemSample *out) {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                                 (task_info_t)&info, &count);
    if (kr != KERN_SUCCESS) { out->rss_bytes = 0; return 0; }
    out->rss_bytes = (unsigned long long)info.resident_size;
    return 1;
#elif defined(__linux__)
    /* Open/read/close every call (once per frame, ~60 Hz). This is
     * intentional: procfs reads are ~1 us, and holding the FD open
     * would prevent the kernel from refreshing the snapshot on each
     * read on some kernels. Don't "optimize" by caching the FD.
     *
     * /proc/self/statm format: "size resident shared text lib data dt".
     * We only need the second field (resident); the leading size is
     * VSZ which we deliberately don't expose (macOS virtual_size is
     * dominated by huge unmapped reservations, so VSZ is not portable
     * useful — RSS is the comparable signal). */
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) { out->rss_bytes = 0; return 0; }
    unsigned long size_p = 0, rss_p = 0;
    int n = fscanf(f, "%lu %lu", &size_p, &rss_p);
    fclose(f);
    if (n != 2) { out->rss_bytes = 0; return 0; }
    (void)size_p;
    long pagesz = sysconf(_SC_PAGESIZE);
    if (pagesz <= 0) pagesz = 4096;
    out->rss_bytes = (unsigned long long)rss_p * (unsigned long long)pagesz;
    return 1;
#else
    out->rss_bytes = 0;
    return 0;
#endif
}

/* ========================================================================= */
/* Monotonic clock                                                            */
/* ========================================================================= */

#if defined(__APPLE__)
static double memprof_now_s(void) {
    static mach_timebase_info_data_t tb = {0, 0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    uint64_t ticks = mach_absolute_time();
    double nsec = (double)ticks * (double)tb.numer / (double)tb.denom;
    return nsec / 1e9;
}
#elif defined(_WIN32)
/* v1 stub for Windows: the platform reader returns zeros and no real
 * sample cadence runs. Returning 0.0 means t_rel stays 0 and pushes
 * never fire, which matches the "no data" path the panel renders. */
static double memprof_now_s(void) { return 0.0; }
#else
static double memprof_now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

/* ========================================================================= */
/* Public API                                                                 */
/* ========================================================================= */

void memprof_init(void) {
    memprof_init_at(memprof_now_s());
}

void memprof_frame_tick(void) {
    memprof_frame_tick_at(memprof_now_s());
}

void memprof_set_reader(MemprofReaderFn reader) {
    g_memprof_reader = reader;
}

void memprof_init_at(double t0_seconds) {
    if (g_memprof_initialized) return;
    g_memprof_t0_seconds  = t0_seconds;
    g_memprof_last_push_s = 0.0;
    MemprofReaderFn reader = g_memprof_reader ? g_memprof_reader : memprof_read;
    reader(&g_memprof_baseline);
    g_memprof_current = g_memprof_baseline;
    g_memprof_initialized = 1;
}

void memprof_reset(void) {
    g_memprof_initialized = 0;
    g_memprof_t0_seconds  = 0.0;
    g_memprof_last_push_s = 0.0;
    g_memprof_count       = 0;
    g_memprof_head        = 0;
    g_memprof_baseline.rss_bytes = 0;
    g_memprof_current.rss_bytes  = 0;
    g_memprof_reader = NULL;
    /* Ring contents left as-is; count=0 makes them unreachable. */
}

void memprof_frame_tick_at(double now_seconds) {
    if (!g_memprof_initialized) return;
    double t_rel = now_seconds - g_memprof_t0_seconds;
    MemprofReaderFn reader = g_memprof_reader ? g_memprof_reader : memprof_read;
    reader(&g_memprof_current);
    if (t_rel - g_memprof_last_push_s >= MEMPROF_PUSH_INTERVAL_S) {
        g_memprof_ring[g_memprof_head]   = g_memprof_current;
        g_memprof_ring_t[g_memprof_head] = t_rel;
        g_memprof_head = (g_memprof_head + 1) % MEMPROF_HISTORY_CAP;
        if (g_memprof_count < MEMPROF_HISTORY_CAP) g_memprof_count++;
        g_memprof_last_push_s = t_rel;
    }
}

MemSample memprof_current(void)  { return g_memprof_current; }
MemSample memprof_baseline(void) { return g_memprof_baseline; }

int memprof_history_count(void)    { return g_memprof_count; }
int memprof_history_capacity(void) { return MEMPROF_HISTORY_CAP; }

void memprof_history_get(int i, MemSample *out, double *sample_seconds_out) {
    if (i < 0 || i >= g_memprof_count) {
        if (out) out->rss_bytes = 0;
        if (sample_seconds_out) *sample_seconds_out = 0.0;
        return;
    }
    /* Oldest-first: head points at the slot the NEXT push will write,
     * which (when the ring is full) is the oldest live entry. When
     * partially filled, head still equals count (writes have not wrapped
     * yet), and (head - count) mod cap = 0 = the actual oldest slot. */
    int oldest = (g_memprof_head - g_memprof_count + MEMPROF_HISTORY_CAP)
                 % MEMPROF_HISTORY_CAP;
    int slot = (oldest + i) % MEMPROF_HISTORY_CAP;
    if (out) *out = g_memprof_ring[slot];
    if (sample_seconds_out) *sample_seconds_out = g_memprof_ring_t[slot];
}

double memprof_history_latest_t(void) {
    if (g_memprof_count <= 0) return 0.0;
    int newest = (g_memprof_head - 1 + MEMPROF_HISTORY_CAP)
                 % MEMPROF_HISTORY_CAP;
    return g_memprof_ring_t[newest];
}

unsigned long long memprof_history_max_rss(void) {
    unsigned long long m = 0;
    for (int i = 0; i < g_memprof_count; i++) {
        int oldest = (g_memprof_head - g_memprof_count + MEMPROF_HISTORY_CAP)
                     % MEMPROF_HISTORY_CAP;
        int slot = (oldest + i) % MEMPROF_HISTORY_CAP;
        if (g_memprof_ring[slot].rss_bytes > m) m = g_memprof_ring[slot].rss_bytes;
    }
    return m;
}

unsigned long long memprof_history_min_rss(void) {
    if (g_memprof_count <= 0) return 0;
    int oldest = (g_memprof_head - g_memprof_count + MEMPROF_HISTORY_CAP)
                 % MEMPROF_HISTORY_CAP;
    unsigned long long m = g_memprof_ring[oldest].rss_bytes;
    for (int i = 1; i < g_memprof_count; i++) {
        int slot = (oldest + i) % MEMPROF_HISTORY_CAP;
        if (g_memprof_ring[slot].rss_bytes < m) m = g_memprof_ring[slot].rss_bytes;
    }
    return m;
}

const char *memprof_format_bytes(char *buf, int buf_sz,
                                 unsigned long long b) {
    if (!buf || buf_sz <= 0) return buf;
    if (b == 0)
        snprintf(buf, (size_t)buf_sz, "--");
    else if (b < 1024ULL * 1024ULL)
        snprintf(buf, (size_t)buf_sz, "%llu KB", b / 1024ULL);
    else if (b < 1024ULL * 1024ULL * 1024ULL)
        snprintf(buf, (size_t)buf_sz, "%.1f MB", (double)b / (1024.0 * 1024.0));
    else
        snprintf(buf, (size_t)buf_sz, "%.2f GB",
                 (double)b / (1024.0 * 1024.0 * 1024.0));
    return buf;
}
