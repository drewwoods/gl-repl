/*
 * assign_plot.c - Value capture for the assignment plot.
 */
#include "subsystems/assign_plot/assign_plot.h"

#include <string.h>

/* One second between captures at ASSIGN_PLOT_RATE_1HZ. */
#define ASSIGN_PLOT_1HZ_PERIOD_US 1000000.0

/* One plotted row's buffer. Columns are kept in display order (oldest first)
 * rather than as a ring, so the view can hand the renderer a plain array. In
 * X_FRAME mode a full buffer shifts down by one on append: 1.5 KB of memmove
 * per series at most once per frame, which is far cheaper than making every
 * reader understand ring wrap. */
typedef struct {
    int              source_line_idx;
    AssignPlotColumn cols[ASSIGN_PLOT_COLS];
    int              col_count;
    int              exec_count;
    RunStats         stats;
} AssignPlotSeries;

static int    g_open = 0;
static int    g_rate = ASSIGN_PLOT_RATE_1HZ;
static int    g_x_mode = ASSIGN_PLOT_X_EXEC;
static int    g_captured = 0;
static double g_last_capture_us = 0.0;
static int    g_expanded = 0;
static int    g_y_log = 0;
static int    g_live_capture = 0;

static AssignPlotSeries g_series[MAX_ASSIGN_PLOT_SERIES];
static int              g_series_count = 0;

static const AssignPlotHostBridge *g_host = NULL;

void assign_plot_install_host(const AssignPlotHostBridge *host) {
    g_host = host;
}

/* Bridge reads, with the no-host case answered once here rather than at each
 * of the four scan sites. Without a host the trace is empty and no row is
 * plottable, which makes an uninstalled plot inert rather than crashing. */
static int assign_plot_trace_len(void) {
    return g_host ? g_host->trace_len() : 0;
}

static int assign_plot_trace_at(int idx, AssignPlotSample *out) {
    return g_host ? g_host->trace_at(idx, out) : 0;
}

static void assign_plot_series_clear_samples(AssignPlotSeries *s) {
    memset(s->cols, 0, sizeof(s->cols));
    s->col_count = 0;
    s->exec_count = 0;
    runstats_clear(&s->stats);
}

/* Drop every series' history. The plot-wide capture bookkeeping goes with it:
 * "captured" and the rate clock describe the window the buffers cover. */
static void assign_plot_clear_samples(void) {
    /* The `i < MAX_ASSIGN_PLOT_SERIES` half is redundant - the only append
     * guards on it and the prune loop can only shrink the count - but the
     * invariant is whole-file, and GCC 15 can't carry it through the memset
     * inlined from assign_plot_series_clear_samples: without a bound it can
     * see here, it reports the clear as running one series past g_series[]
     * (-Warray-bounds). Cheap to state, so state it. */
    for (int i = 0; i < g_series_count && i < MAX_ASSIGN_PLOT_SERIES; i++)
        assign_plot_series_clear_samples(&g_series[i]);
    g_captured = 0;
    g_last_capture_us = 0.0;
}

/* Index of the series plotting `source_line_idx`, or -1. */
static int assign_plot_series_index(int source_line_idx) {
    for (int i = 0; i < g_series_count; i++)
        if (g_series[i].source_line_idx == source_line_idx)
            return i;
    return -1;
}

/* A plotted row must still exist and still be an assignment. It does not have
 * to be the *same* assignment: if the row is edited the series follows what is
 * now there, and the panel's legend (rebuilt by the controller from the live
 * row each frame) says so. Anything else - the row deleted, or replaced by a
 * command that assigns nothing - drops that series rather than silently
 * plotting a neighbour. The host answers what "assignment" means, including
 * the bounds check. */
static int assign_plot_row_is_live(int source_line_idx) {
    if (source_line_idx < 0) return 0;
    return g_host ? g_host->row_is_plottable(source_line_idx) : 0;
}

/* Drop dead series, keeping the rest in order. Closes the plot when the last
 * one goes. Returns 1 if anything was removed. */
static int assign_plot_prune_dead_series(void) {
    int kept = 0;
    int removed = 0;

    for (int i = 0; i < g_series_count; i++) {
        if (!assign_plot_row_is_live(g_series[i].source_line_idx)) {
            removed = 1;
            continue;
        }
        if (kept != i)
            g_series[kept] = g_series[i];
        kept++;
    }
    g_series_count = kept;
    if (g_series_count == 0) {
        g_open = 0;
        g_captured = 0;
        g_last_capture_us = 0.0;
    }
    return removed;
}

static int assign_plot_rate_allows(double now_us) {
    switch (g_rate) {
        case ASSIGN_PLOT_RATE_ONCE:
            return !g_captured;
        case ASSIGN_PLOT_RATE_1HZ:
            /* Live capture overrides the clock but not the one-shot above: a
             * marker drawn over a second-old trace points at the right
             * execution ordinal of the wrong frame's values. */
            return g_live_capture
                || !g_captured
                || (now_us - g_last_capture_us) >= ASSIGN_PLOT_1HZ_PERIOD_US;
        case ASSIGN_PLOT_RATE_FRAME:
        default:
            return 1;
    }
}

/* Executions of `source_line_idx` in the current trace. The whole trace on
 * purpose - see the header on why replay's clamp is not applied. */
static int assign_plot_count_executions(int source_line_idx) {
    int n = assign_plot_trace_len();
    int found = 0;
    for (int i = 0; i < n; i++) {
        AssignPlotSample s;
        if (assign_plot_trace_at(i, &s) && s.source_line_idx == source_line_idx)
            found++;
    }
    return found;
}

/* Second pass for one series: fold this frame's executions into `cols`
 * columns, and every value into the statistics. `total` comes from
 * assign_plot_count_executions() so the column a value belongs to can be
 * computed without buffering the values first.
 *
 * With cols <= total the column index is non-decreasing in `seen` and advances
 * by at most one per value, so columns fill strictly left to right with no
 * gaps - which is what lets `col != prev_col` stand in for "first value in
 * this column" and why no per-column seen flag is needed. */
static void assign_plot_fill_exec_columns(AssignPlotSeries *s,
                                          int total, int cols) {
    int n = assign_plot_trace_len();
    int seen = 0;
    int prev_col = -1;

    memset(s->cols, 0, sizeof(s->cols));

    for (int i = 0; i < n && seen < total; i++) {
        AssignPlotSample sample;
        float value;
        int col;
        if (!assign_plot_trace_at(i, &sample)) continue;
        if (sample.source_line_idx != s->source_line_idx) continue;
        value = sample.value;

        /* 64-bit product: `total` can reach MAX_FLAT_COMMANDS today, which
         * overflows nothing, but the widening keeps this correct if either
         * cap grows. */
        col = (int)(((long long)seen * (long long)cols) / (long long)total);
        if (col < 0) col = 0;
        if (col > cols - 1) col = cols - 1;

        if (col != prev_col) {
            s->cols[col].lo = value;
            s->cols[col].hi = value;
            s->cols[col].valid = 1;
            prev_col = col;
        } else {
            if (value < s->cols[col].lo) s->cols[col].lo = value;
            if (value > s->cols[col].hi) s->cols[col].hi = value;
        }
        runstats_record(&s->stats, (double)value);
        seen++;
    }
    s->col_count = cols;
}

/* X_FRAME append: one column per capture, scrolling once the buffer is full.
 * `valid` 0 marks a capture this series had no value for - the column still
 * has to exist so column N means capture N for every series. */
static void assign_plot_append_frame_column(AssignPlotSeries *s,
                                            float lo, float hi, int valid) {
    if (s->col_count >= ASSIGN_PLOT_COLS) {
        memmove(&s->cols[0], &s->cols[1],
                (size_t)(ASSIGN_PLOT_COLS - 1) * sizeof(s->cols[0]));
        s->col_count = ASSIGN_PLOT_COLS - 1;
    }
    s->cols[s->col_count].lo = valid ? lo : 0.0f;
    s->cols[s->col_count].hi = valid ? hi : 0.0f;
    s->cols[s->col_count].valid = valid;
    s->col_count++;
}

/* The min/max envelope of everything this series produced this frame.
 * The primary series always contributes exactly one value in X_FRAME mode;
 * a secondary series may have run more than once (an edit can make a row
 * disagree with the mode after it was admitted), and folding those into one
 * column keeps every series' history aligned on the shared capture axis. */
static int assign_plot_frame_envelope(AssignPlotSeries *s,
                                      float *out_lo, float *out_hi) {
    int n = assign_plot_trace_len();
    int found = 0;

    for (int i = 0; i < n; i++) {
        AssignPlotSample sample;
        float value;
        if (!assign_plot_trace_at(i, &sample)) continue;
        if (sample.source_line_idx != s->source_line_idx) continue;
        value = sample.value;
        if (!found || value < *out_lo) *out_lo = value;
        if (!found || value > *out_hi) *out_hi = value;
        runstats_record(&s->stats, (double)value);
        found = 1;
    }
    return found;
}

void assign_plot_open(int source_line_idx) {
    if (source_line_idx < 0) return;
    if (g_open && g_series_count == 1
        && g_series[0].source_line_idx == source_line_idx)
        return;
    g_open = 1;
    g_series_count = 1;
    g_series[0].source_line_idx = source_line_idx;
    g_x_mode = ASSIGN_PLOT_X_EXEC;
    assign_plot_clear_samples();
}

void assign_plot_close(void) {
    g_open = 0;
    g_series_count = 0;
    g_captured = 0;
    g_last_capture_us = 0.0;
}

void assign_plot_toggle(int source_line_idx) {
    if (g_open && g_series_count == 1
        && g_series[0].source_line_idx == source_line_idx) {
        assign_plot_close();
        return;
    }
    assign_plot_open(source_line_idx);
}

/* Can a row join the current plot's X axis? What has to match is the *shape*
 * of the two rows' execution: a row that runs once per frame belongs on the
 * capture axis and a row that runs many times belongs on the execution axis,
 * and no axis holds both.
 *
 * The comparison is against the primary's live execution count, taken from the
 * flat program as it stands at click time, rather than against g_x_mode. The
 * mode is only derived during a capture, so before the first one it still
 * holds the default from assign_plot_open() and describes nothing - reading it
 * there refuses every second top-level row. Comparing the two rows directly
 * also does the right thing when the primary has just been edited into (or out
 * of) a loop: the pair is admitted or refused on what the program does now,
 * and the next capture flips the mode to match.
 *
 * In order:
 *
 *   - nothing plotted yet, or the candidate has never executed: nothing to
 *     contradict, so admit it;
 *   - the primary ran this frame: admit only if both are once-per-frame or
 *     both are many-per-frame;
 *   - the primary did not run and nothing has been captured: no evidence
 *     either way, so admit;
 *   - otherwise fall back to the mode the last capture derived - X_FRAME
 *     wants a row that runs once, X_EXEC one that runs at least twice.
 */
static int assign_plot_row_fits_x_mode(int source_line_idx) {
    int execs, primary_execs;
    if (g_series_count == 0) return 1;
    execs = assign_plot_count_executions(source_line_idx);
    if (execs == 0) return 1;

    primary_execs = assign_plot_count_executions(g_series[0].source_line_idx);
    if (primary_execs > 0)
        return (primary_execs == 1) == (execs == 1);
    if (!g_captured) return 1;
    return (g_x_mode == ASSIGN_PLOT_X_FRAME) ? (execs == 1) : (execs >= 2);
}

int assign_plot_toggle_series(int source_line_idx) {
    int existing;

    if (source_line_idx < 0) return ASSIGN_PLOT_SERIES_INCOMPATIBLE;

    if (!g_open || g_series_count == 0) {
        assign_plot_open(source_line_idx);
        return ASSIGN_PLOT_SERIES_ADDED;
    }

    existing = assign_plot_series_index(source_line_idx);
    if (existing >= 0) {
        /* Removing the primary hands the X axis to whatever is left, so the
         * buffers no longer describe a consistent axis - clear them and let
         * the next capture re-derive the mode. */
        for (int i = existing; i + 1 < g_series_count; i++)
            g_series[i] = g_series[i + 1];
        g_series_count--;
        if (g_series_count == 0)
            assign_plot_close();
        else if (existing == 0)
            assign_plot_clear_samples();
        return ASSIGN_PLOT_SERIES_REMOVED;
    }

    if (g_series_count >= MAX_ASSIGN_PLOT_SERIES)
        return ASSIGN_PLOT_SERIES_FULL;
    if (!assign_plot_row_fits_x_mode(source_line_idx))
        return ASSIGN_PLOT_SERIES_INCOMPATIBLE;

    /* The new series normally starts empty rather than resetting the plot: the
     * point of adding one is to compare it against history already on screen.
     *
     * On the capture axis that history has to be accounted for, though -
     * column N means capture N for every series, and a series that simply
     * started shorter would be stretched across the full width and read as
     * aligned with values it never saw. It is back-filled with gap columns
     * instead: no data before it was added, drawn as the absence it is. */
    assign_plot_series_clear_samples(&g_series[g_series_count]);
    g_series[g_series_count].source_line_idx = source_line_idx;
    g_series_count++;
    if (g_rate == ASSIGN_PLOT_RATE_ONCE && g_captured) {
        /* A one-shot must describe the same frame for every series. */
        assign_plot_clear_samples();
    } else if (g_x_mode == ASSIGN_PLOT_X_FRAME) {
        int backfill = g_series[0].col_count;
        for (int i = 0; i < backfill; i++)
            assign_plot_append_frame_column(&g_series[g_series_count - 1],
                                            0.0f, 0.0f, 0);
    }
    return ASSIGN_PLOT_SERIES_ADDED;
}

int assign_plot_is_open(void) {
    return g_open;
}

int assign_plot_series_count(void) {
    return g_open ? g_series_count : 0;
}

int assign_plot_source_line(void) {
    return (g_open && g_series_count > 0) ? g_series[0].source_line_idx : -1;
}

int assign_plot_has_series(int source_line_idx) {
    return g_open && assign_plot_series_index(source_line_idx) >= 0;
}

void assign_plot_set_rate(int rate) {
    if (rate < 0 || rate >= ASSIGN_PLOT_RATE_COUNT) return;
    g_rate = rate;
    assign_plot_clear_samples();
}

void assign_plot_cycle_rate(int dir) {
    int step = (dir < 0) ? (ASSIGN_PLOT_RATE_COUNT - 1) : 1;
    assign_plot_set_rate((g_rate + step) % ASSIGN_PLOT_RATE_COUNT);
}

/* Presentation only: deliberately no assign_plot_clear_samples(). Zooming the
 * panel or switching the Y axis to log re-draws the values already captured;
 * throwing them away would make both chips destructive, which is the opposite
 * of what "look at this more closely" should do. */
void assign_plot_toggle_expanded(void) {
    g_expanded = !g_expanded;
}

void assign_plot_toggle_y_log(void) {
    g_y_log = !g_y_log;
}

int assign_plot_is_expanded(void) {
    return g_expanded;
}

void assign_plot_reset(void) {
    assign_plot_clear_samples();
}

void assign_plot_capture(double now_us) {
    int total, cols;
    int any_executed = 0;
    int all_executed = 1;

    /* The gate that makes this feature free when nobody is looking: no flat
     * scan, no clock arithmetic, nothing. */
    if (!g_open) return;

    if (assign_plot_prune_dead_series() && !g_open)
        return;
    if (!assign_plot_rate_allows(now_us)) return;

    /* The primary series fixes the axis for the whole plot. */
    total = assign_plot_count_executions(g_series[0].source_line_idx);

    if (total >= 2) {
        if (g_x_mode != ASSIGN_PLOT_X_EXEC) {
            g_x_mode = ASSIGN_PLOT_X_EXEC;
            assign_plot_clear_samples();
        }
    } else if (total == 1) {
        if (g_x_mode != ASSIGN_PLOT_X_FRAME) {
            g_x_mode = ASSIGN_PLOT_X_FRAME;
            assign_plot_clear_samples();
        }
    }
    /* total == 0: the primary did not run this frame (a false `if`, a `goto`
     * that jumped over it). Leave the axis mode alone - a row that runs
     * intermittently should keep the history it has, and flipping the axis on
     * an empty frame would throw it away. Secondary series are still sampled
     * below, so the plot does not stall on the primary's silence. */

    if (g_x_mode == ASSIGN_PLOT_X_EXEC) {
        for (int i = 0; i < g_series_count; i++) {
            int execs = (i == 0) ? total
                      : assign_plot_count_executions(g_series[i].source_line_idx);
            g_series[i].exec_count = execs;
            if (execs <= 0) { all_executed = 0; continue; }
            cols = (execs < ASSIGN_PLOT_COLS) ? execs : ASSIGN_PLOT_COLS;
            assign_plot_fill_exec_columns(&g_series[i], execs, cols);
        }
    } else {
        float lo[MAX_ASSIGN_PLOT_SERIES], hi[MAX_ASSIGN_PLOT_SERIES];
        int   got[MAX_ASSIGN_PLOT_SERIES];

        for (int i = 0; i < g_series_count; i++) {
            lo[i] = hi[i] = 0.0f;
            got[i] = assign_plot_frame_envelope(&g_series[i], &lo[i], &hi[i]);
            g_series[i].exec_count =
                (i == 0) ? total
                         : assign_plot_count_executions(g_series[i].source_line_idx);
            if (got[i]) any_executed = 1;
            else        all_executed = 0;
        }
        /* Append for every series or none: a partial append would slide the
         * capture axis out from under the series that were skipped. When
         * nothing ran at all, the buffers are left exactly as they were. */
        if (any_executed) {
            /* An envelope spanning several executions is folded into the one
             * column this capture owns. */
            for (int i = 0; i < g_series_count; i++)
                assign_plot_append_frame_column(&g_series[i], lo[i], hi[i],
                                                got[i]);
        }
    }

    /* A one-shot is meant to be a frame you can read across: every series
     * frozen from the same execution of the program. A frame where some row
     * has not run yet (a guard still closed, a `goto` still jumping it) cannot
     * be that, so the shot is not spent on it - the partial attempt is thrown
     * away and the next frame is tried instead. Without this the shot lands on
     * whatever frame happened to be first and freezes an incomplete
     * comparison, which no later frame can repair.
     *
     * The cost is that a plot whose rows never all run in one frame keeps
     * scanning, at the price of the FRAME rate. That is the honest state to be
     * in - the panel says "(collecting)" - and it resolves itself the moment a
     * complete frame arrives. */
    if (g_rate == ASSIGN_PLOT_RATE_ONCE && !all_executed) {
        assign_plot_clear_samples();   /* also leaves g_captured clear: still armed */
        return;
    }

    g_captured = 1;
    g_last_capture_us = now_us;
}

void assign_plot_set_live_capture(int on) {
    g_live_capture = on ? 1 : 0;
}

int assign_plot_exec_progress(int exec_limit,
                              float out_frac[MAX_ASSIGN_PLOT_SERIES]) {
    int n;
    int total[MAX_ASSIGN_PLOT_SERIES];
    int done[MAX_ASSIGN_PLOT_SERIES];
    int any = 0;

    if (!out_frac) return 0;
    for (int s = 0; s < MAX_ASSIGN_PLOT_SERIES; s++)
        out_frac[s] = -1.0f;

    /* X_FRAME has no within-frame axis to place a marker on; see the header. */
    if (!g_open || !g_host || g_x_mode != ASSIGN_PLOT_X_EXEC) return 0;
    /* A one-shot is exempt from the live-capture override, so its columns are
     * some earlier frame's. A marker over them would be a position in this
     * frame drawn on another one's values - the same lie the override exists
     * to prevent, so the frozen case simply has no marker. */
    if (g_rate == ASSIGN_PLOT_RATE_ONCE) return 0;

    memset(total, 0, sizeof(total));
    memset(done, 0, sizeof(done));

    /* One pass for every series rather than one pass each: the series list is
     * at most MAX_ASSIGN_PLOT_SERIES long, so the inner walk is cheaper than
     * re-reading the trace four times. */
    n = assign_plot_trace_len();
    for (int i = 0; i < n; i++) {
        AssignPlotSample sample;
        if (!assign_plot_trace_at(i, &sample)) continue;
        for (int s = 0; s < g_series_count; s++) {
            if (sample.source_line_idx != g_series[s].source_line_idx) continue;
            total[s]++;
            if (exec_limit < 0 || i < exec_limit) done[s]++;
            break;
        }
    }

    for (int s = 0; s < g_series_count; s++) {
        int last;
        /* A series that has not run yet gets no marker rather than one pinned
         * to the left edge, which would read as "the PC is at its first
         * execution". A single-execution series has no span to place one on. */
        if (total[s] < 2 || done[s] <= 0) continue;
        /* The marker sits on the last execution that has *run*, and the trace
         * puts execution i at i/(total-1) of the plot width. */
        last = done[s] - 1;
        if (last > total[s] - 1) last = total[s] - 1;
        out_frac[s] = (float)last / (float)(total[s] - 1);
        any = 1;
    }
    return any;
}

AssignPlotView assign_plot_view(void) {
    AssignPlotView v;
    memset(&v, 0, sizeof(v));
    v.open         = g_open;
    v.rate         = g_rate;
    v.x_mode       = g_x_mode;
    v.captured     = g_captured;
    v.expanded     = g_expanded;
    v.y_log        = g_y_log;
    v.series_count = g_open ? g_series_count : 0;
    for (int i = 0; i < v.series_count; i++) {
        v.series[i].source_line_idx = g_series[i].source_line_idx;
        v.series[i].exec_count      = g_series[i].exec_count;
        v.series[i].cols            = g_series[i].cols;
        v.series[i].col_count       = g_series[i].col_count;
        runstats_read(&g_series[i].stats, &v.series[i].stats);
    }
    return v;
}

void assign_plot_reset_all(void) {
    g_open = 0;
    g_series_count = 0;
    g_rate = ASSIGN_PLOT_RATE_1HZ;
    g_x_mode = ASSIGN_PLOT_X_EXEC;
    g_expanded = 0;
    g_y_log = 0;
    g_captured = 0;
    g_live_capture = 0;
    g_last_capture_us = 0.0;
    for (int i = 0; i < MAX_ASSIGN_PLOT_SERIES; i++) {
        g_series[i].source_line_idx = -1;
        assign_plot_series_clear_samples(&g_series[i]);
    }
}
