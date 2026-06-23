/*
 * gpuprof.c - GPU time profiling via GL timer queries.
 *
 * Mechanism (see gpuprof.h for the contract):
 *
 * GL_TIME_ELAPSED_EXT queries cannot nest — only one may be active at a
 * time — but prof sections do nest (PROF_RENDER3D wraps _FILL, _HELPERS,
 * ...). So begin/end never map 1:1 onto queries; instead each transition
 * is a boundary, slicing the frame into non-overlapping *segments*. Every
 * segment records the bitmask of sections open while it ran; at harvest a
 * segment's nanoseconds are credited to each open section, which
 * reconstructs correct inclusive times for arbitrary nesting. A side
 * benefit: multiple begin/end pairs per frame (accumulation-AA passes,
 * per-batch loops) just contribute more segments and sum naturally.
 *
 * The same segment structure backs both measurement modes:
 *   - elapsed mode: each segment is one TIME_ELAPSED query (end the
 *     running query at a boundary, start the next); seg_mask[i] is the
 *     mask while query i ran.
 *   - timestamp mode: each boundary is one glQueryCounter(GL_TIMESTAMP)
 *     marker; seg_mask[i] is the mask of the interval STARTING at marker
 *     i (0 between top-level runs = uncredited gap), and interval i's
 *     time is ts[i+1] - ts[i]. Consecutive deltas tile the GPU timeline,
 *     so timestamp-mode sums are additive (see gpuprof.h NOTE).
 *
 * Results are asynchronous: a frame's segments live in one slot of a small
 * ring until the GL reports the slot's *last* query available (queries
 * complete in submission order, so one GL_QUERY_RESULT_AVAILABLE poll per
 * frame covers the whole slot). Only then are the 64-bit results read —
 * by which point they are no longer in flight, so the read never stalls.
 * If the ring fills (GPU several frames behind) new frames go unsampled
 * rather than blocking; an EMA display doesn't miss them.
 */
#include "support/gpuprof.h"

#include "c_compat.h"

/* ========================================================================= */
/* Configuration                                                              */
/* ========================================================================= */

/* Same smoothing/staleness behavior as cpuprof.c so the panel's two time
 * sources track each other. */
#define GPU_PROF_EMA_ALPHA      0.04
#define GPU_PROF_STALE_FRAMES   180

/* Frames that may be awaiting results before sampling pauses. Results are
 * typically ready 1-2 frames after submission. */
#ifndef GPU_PROF_RING_SLOTS
#define GPU_PROF_RING_SLOTS 4
#endif

/* Segments (= query objects) per frame slot. Worst case observed shape is
 * the accumulation loop: ~13 GPU-bracketed scene sections x 2 transitions
 * x 16 passes plus the 2D UI sections — roughly 450. Overflow marks the
 * slot invalid (frame dropped from the GPU column), never UB.
 * Overridable so tests can exercise the overflow path with a tiny cap. */
#ifndef GPU_PROF_MAX_SEGMENTS
#define GPU_PROF_MAX_SEGMENTS 1024
#endif

/* Deepest begin/end nesting tracked. The instrumented tree is depth <= 4
 * today (FRAME_TOTAL > SCENE_3D > HELPERS > GRID); deeper begins mark the
 * frame overflowed but stay balanced. */
#define GPU_PROF_MAX_DEPTH 16

/* GL tokens, defined locally so this TU needs no GL header (mesh_ply.c
 * precedent). Values are fixed by the GL specs. */
#define GPU_PROF_GL_QUERY_RESULT            0x8866u
#define GPU_PROF_GL_QUERY_RESULT_AVAILABLE  0x8867u
#define GPU_PROF_GL_TIME_ELAPSED_EXT        0x88BFu
#define GPU_PROF_GL_TIMESTAMP               0x8E28u

/* Segment masks are one bit per section. */
STATIC_ASSERT(PROF_SECTION_COUNT <= 64, "section mask must fit 64 bits");

/* ========================================================================= */
/* State                                                                      */
/* ========================================================================= */

typedef struct {
    unsigned   query_ids[GPU_PROF_MAX_SEGMENTS];
    GpuProfU64 seg_mask[GPU_PROF_MAX_SEGMENTS]; /* sections open per segment */
    int        seg_count;
    int        overflowed;   /* segment/depth budget blown or unbalanced -> discard */
} GpuProfFrameSlot;

static GpuProfGlFns     g_gpu_fns;
static int              g_gpu_enabled = 0;
static int              g_gpu_use_timestamps = 0;

static GpuProfFrameSlot g_gpu_slots[GPU_PROF_RING_SLOTS];
static int              g_gpu_fifo_head = 0; /* oldest in-flight slot */
static int              g_gpu_fifo_len  = 0; /* in-flight slot count */
static int              g_gpu_cur_slot  = -1; /* this frame's slot, -1 = unsampled */

static int              g_gpu_stack[GPU_PROF_MAX_DEPTH];
static int              g_gpu_depth = 0;      /* logical depth; may exceed MAX */
static GpuProfU64       g_gpu_cur_mask = 0;
static int              g_gpu_query_active = 0;

static double           g_gpu_last_us[PROF_SECTION_COUNT];
static double           g_gpu_avg_us[PROF_SECTION_COUNT];
static int              g_gpu_stale[PROF_SECTION_COUNT];

/* ========================================================================= */
/* Helpers                                                                    */
/* ========================================================================= */

static void gpu_reset_stats(void) {
    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        g_gpu_last_us[section_idx] = 0.0;
        g_gpu_avg_us[section_idx]  = 0.0;
        g_gpu_stale[section_idx]   = GPU_PROF_STALE_FRAMES; /* no data yet */
    }
}

/* Open a new elapsed-mode segment under the current section mask (ends
 * none — callers have already ended any running query). */
static void gpu_start_segment(GpuProfFrameSlot *slot) {
    if (slot->seg_count >= GPU_PROF_MAX_SEGMENTS) {
        slot->overflowed = 1;
        return;
    }
    int seg_idx = slot->seg_count++;
    slot->seg_mask[seg_idx] = g_gpu_cur_mask;
    g_gpu_fns.begin_query(GPU_PROF_GL_TIME_ELAPSED_EXT, slot->query_ids[seg_idx]);
    g_gpu_query_active = 1;
}

/* Timestamp mode: drop one marker at a boundary. The stored mask is the
 * mask of the interval that STARTS here (already updated by the caller);
 * mask 0 marks the gap after a top-level run ends and is never credited. */
static void gpu_ts_mark(GpuProfFrameSlot *slot) {
    if (slot->seg_count >= GPU_PROF_MAX_SEGMENTS) {
        slot->overflowed = 1;
        return;
    }
    int seg_idx = slot->seg_count++;
    slot->seg_mask[seg_idx] = g_gpu_cur_mask;
    g_gpu_fns.query_counter(slot->query_ids[seg_idx], GPU_PROF_GL_TIMESTAMP);
}

static void gpu_end_active_query(void) {
    if (g_gpu_query_active) {
        g_gpu_fns.end_query(GPU_PROF_GL_TIME_ELAPSED_EXT);
        g_gpu_query_active = 0;
    }
}

static void gpu_feed_sample(int section_idx, double us) {
    g_gpu_last_us[section_idx] = us;
    g_gpu_stale[section_idx]   = 0;
    if (g_gpu_avg_us[section_idx] == 0.0)
        g_gpu_avg_us[section_idx] = us;  /* seed with first sample */
    else
        g_gpu_avg_us[section_idx] = GPU_PROF_EMA_ALPHA * us
                                  + (1.0 - GPU_PROF_EMA_ALPHA) * g_gpu_avg_us[section_idx];
}

static void gpu_credit_interval(double *total_us, GpuProfU64 mask,
                                GpuProfU64 ns) {
    for (int bit = 0; mask != 0; bit++, mask >>= 1) {
        if (mask & 1ull)
            total_us[bit] += (double)ns / 1000.0;
    }
}

/* Read a completed slot's results and credit each segment's time to all
 * sections that were open while it ran. Elapsed mode: per-segment query
 * results. Timestamp mode: deltas between consecutive markers, crediting
 * the mask of the interval that started at the earlier marker. */
static void gpu_harvest_slot(const GpuProfFrameSlot *slot) {
    double total_us[PROF_SECTION_COUNT];
    GpuProfU64 opened = 0;

    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++)
        total_us[section_idx] = 0.0;

    if (g_gpu_use_timestamps) {
        GpuProfU64 prev_ts = 0;
        if (slot->seg_count >= 2)
            g_gpu_fns.get_query_ui64v(slot->query_ids[0],
                                      GPU_PROF_GL_QUERY_RESULT, &prev_ts);
        for (int seg_idx = 1; seg_idx < slot->seg_count; seg_idx++) {
            GpuProfU64 cur_ts = prev_ts;
            g_gpu_fns.get_query_ui64v(slot->query_ids[seg_idx],
                                      GPU_PROF_GL_QUERY_RESULT, &cur_ts);
            GpuProfU64 mask = slot->seg_mask[seg_idx - 1];
            if (mask != 0) {
                opened |= mask;
                gpu_credit_interval(total_us, mask,
                                    cur_ts > prev_ts ? cur_ts - prev_ts : 0);
            }
            prev_ts = cur_ts;
        }
    } else {
        for (int seg_idx = 0; seg_idx < slot->seg_count; seg_idx++) {
            GpuProfU64 ns = 0;
            g_gpu_fns.get_query_ui64v(slot->query_ids[seg_idx],
                                      GPU_PROF_GL_QUERY_RESULT, &ns);
            opened |= slot->seg_mask[seg_idx];
            gpu_credit_interval(total_us, slot->seg_mask[seg_idx], ns);
        }
    }

    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        if ((opened >> section_idx) & 1ull)
            gpu_feed_sample(section_idx, total_us[section_idx]);
    }
}

/* ========================================================================= */
/* Public API                                                                 */
/* ========================================================================= */

int gpu_prof_init(const GpuProfGlFns *fns) {
    if (g_gpu_enabled)
        gpu_prof_shutdown();
    if (!fns || !fns->gen_queries || !fns->delete_queries ||
        !fns->begin_query || !fns->end_query ||
        !fns->get_query_iv || !fns->get_query_ui64v)
        return 0;
    g_gpu_fns = *fns;
    g_gpu_use_timestamps = (fns->query_counter != 0);
    for (int slot_idx = 0; slot_idx < GPU_PROF_RING_SLOTS; slot_idx++) {
        g_gpu_fns.gen_queries(GPU_PROF_MAX_SEGMENTS, g_gpu_slots[slot_idx].query_ids);
        g_gpu_slots[slot_idx].seg_count  = 0;
        g_gpu_slots[slot_idx].overflowed = 0;
    }
    gpu_reset_stats();
    g_gpu_fifo_head = 0;
    g_gpu_fifo_len  = 0;
    g_gpu_cur_slot  = -1;
    g_gpu_depth     = 0;
    g_gpu_cur_mask  = 0;
    g_gpu_query_active = 0;
    g_gpu_enabled = 1;
    return 1;
}

void gpu_prof_shutdown(void) {
    if (!g_gpu_enabled) return;
    gpu_end_active_query();
    for (int slot_idx = 0; slot_idx < GPU_PROF_RING_SLOTS; slot_idx++)
        g_gpu_fns.delete_queries(GPU_PROF_MAX_SEGMENTS, g_gpu_slots[slot_idx].query_ids);
    gpu_reset_stats();
    g_gpu_fifo_len = 0;
    g_gpu_cur_slot = -1;
    g_gpu_depth    = 0;
    g_gpu_cur_mask = 0;
    g_gpu_enabled  = 0;
    g_gpu_use_timestamps = 0;
}

int gpu_prof_enabled(void) {
    return g_gpu_enabled;
}

int gpu_prof_uses_timestamps(void) {
    return g_gpu_enabled && g_gpu_use_timestamps;
}

void gpu_prof_frame_begin(void) {
    if (!g_gpu_enabled) return;

    /* Close out the slot the previous frame filled; it becomes in-flight.
     * Unbalanced begins (depth never returned to 0) poison the slot. */
    if (g_gpu_cur_slot >= 0) {
        gpu_end_active_query();
        if (g_gpu_depth != 0)
            g_gpu_slots[g_gpu_cur_slot].overflowed = 1;
        g_gpu_depth    = 0;
        g_gpu_cur_mask = 0;
        g_gpu_fifo_len++;
        g_gpu_cur_slot = -1;
    }

    for (int section_idx = 0; section_idx < PROF_SECTION_COUNT; section_idx++) {
        if (g_gpu_stale[section_idx] < GPU_PROF_STALE_FRAMES)
            g_gpu_stale[section_idx]++;
    }

    /* Harvest in-flight slots oldest-first; stop at the first whose results
     * are not in yet (completion is in submission order). */
    while (g_gpu_fifo_len > 0) {
        GpuProfFrameSlot *slot = &g_gpu_slots[g_gpu_fifo_head];
        if (slot->seg_count > 0) {
            int avail = 0;
            g_gpu_fns.get_query_iv(slot->query_ids[slot->seg_count - 1],
                                   GPU_PROF_GL_QUERY_RESULT_AVAILABLE, &avail);
            if (!avail)
                break;
            if (!slot->overflowed)
                gpu_harvest_slot(slot);
        }
        slot->seg_count  = 0;
        slot->overflowed = 0;
        g_gpu_fifo_head = (g_gpu_fifo_head + 1) % GPU_PROF_RING_SLOTS;
        g_gpu_fifo_len--;
    }

    /* Open this frame's slot if the ring has room; else skip the frame. */
    if (g_gpu_fifo_len < GPU_PROF_RING_SLOTS) {
        g_gpu_cur_slot = (g_gpu_fifo_head + g_gpu_fifo_len) % GPU_PROF_RING_SLOTS;
        g_gpu_slots[g_gpu_cur_slot].seg_count  = 0;
        g_gpu_slots[g_gpu_cur_slot].overflowed = 0;
    }
}

void gpu_prof_begin(ProfSection s) {
    if (!g_gpu_enabled || g_gpu_cur_slot < 0) return;
    if (s < 0 || s >= PROF_SECTION_COUNT) return;
    GpuProfFrameSlot *slot = &g_gpu_slots[g_gpu_cur_slot];

    gpu_end_active_query();
    if (g_gpu_depth < GPU_PROF_MAX_DEPTH) {
        g_gpu_stack[g_gpu_depth] = (int)s;
        g_gpu_cur_mask |= 1ull << s;
    } else {
        slot->overflowed = 1;  /* deeper than tracked; keep depth balanced */
    }
    g_gpu_depth++;
    if (g_gpu_use_timestamps)
        gpu_ts_mark(slot);
    else
        gpu_start_segment(slot);
}

void gpu_prof_end(ProfSection s) {
    if (!g_gpu_enabled || g_gpu_cur_slot < 0) return;
    if (g_gpu_depth == 0) return;

    gpu_end_active_query();
    g_gpu_depth--;
    if (g_gpu_depth < GPU_PROF_MAX_DEPTH) {
        /* Tolerate a mismatched end by unwinding to the named section. */
        if (g_gpu_stack[g_gpu_depth] != (int)s) {
            int unwind_idx = g_gpu_depth;
            while (unwind_idx > 0 && g_gpu_stack[unwind_idx] != (int)s)
                unwind_idx--;
            if (g_gpu_stack[unwind_idx] == (int)s)
                g_gpu_depth = unwind_idx;
        }
        g_gpu_cur_mask = 0;
        for (int stack_idx = 0; stack_idx < g_gpu_depth; stack_idx++)
            g_gpu_cur_mask |= 1ull << g_gpu_stack[stack_idx];
    }
    if (g_gpu_use_timestamps)
        gpu_ts_mark(&g_gpu_slots[g_gpu_cur_slot]);  /* closes the interval
                                                     * even at depth 0 */
    else if (g_gpu_depth > 0)
        gpu_start_segment(&g_gpu_slots[g_gpu_cur_slot]);
}

double gpu_prof_section_last_us(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return 0.0;
    return g_gpu_last_us[s];
}

double gpu_prof_section_avg_us(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return 0.0;
    return g_gpu_avg_us[s];
}

int gpu_prof_section_has_data(ProfSection s) {
    if (!g_gpu_enabled) return 0;
    if (s < 0 || s >= PROF_SECTION_COUNT) return 0;
    return g_gpu_stale[s] < GPU_PROF_STALE_FRAMES;
}
