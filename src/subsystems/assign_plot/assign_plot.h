/*
 * assign_plot.h - Value capture for the assignment plot (right-click a
 *                 `var = expr;` row to see what it actually computes).
 *
 * A debugging surface. Right-clicking an assignment row targets it here; every
 * capture reads the values that row produced and folds them into a small plot
 * buffer plus running min/max/mean/stddev. `src/ui/support/assign_plot.c`
 * draws the result.
 *
 * --- Why this needs no executor hook ---
 *
 * Flatten already does the work. Each execution instance of an assignment
 * becomes its own flat command with the evaluated RHS baked into args[0]
 * (args[2] for a scratch assign) — the executor only applies it, it never
 * re-evaluates (see the comment on CMD_VAR_ASSIGN in src/repl/executor.c).
 * So a capture is a read-only scan of the flat program the frame was going to
 * build anyway: no instrumentation in the hot path, no new bookkeeping, and
 * nothing at all to pay when the panel is closed. assign_plot_capture()
 * returns on its first line unless a row is targeted.
 *
 * The scan deliberately walks the *full* flat count rather than
 * replay_exec_limit(), so scrubbing a replay does not truncate the plot: the
 * question being asked is "what does this row do over a frame", not "what has
 * run so far".
 *
 * --- The two X axes ---
 *
 * A row inside a loop executes many times per frame, so the natural X axis is
 * the execution index within the captured frame (ASSIGN_PLOT_X_EXEC). A
 * top-level row executes exactly once, which would plot a single point — there
 * the X axis becomes successive captures (ASSIGN_PLOT_X_FRAME) and the buffer
 * is a scrolling time series. The mode is re-derived on every capture from the
 * execution count; a flip clears the buffer and the statistics, because the two
 * axes describe different things and averaging across the change would be a
 * lie. A capture that finds zero executions leaves the mode alone and appends
 * nothing.
 *
 * --- Decimation vs. statistics ---
 *
 * The plot is impressionistic by design: a frame with more executions than
 * ASSIGN_PLOT_COLS folds into per-column min/max envelopes, so the shape and
 * the extremes survive but individual samples do not. The statistics are fed
 * from *every* value, never from the columns, so min/max/mean/stddev stay
 * exact no matter how hard the plot is decimated.
 */
#ifndef ASSIGN_PLOT_H
#define ASSIGN_PLOT_H

#include "support/runstats.h"

/* Plot columns. Sized to about one column per pixel of the panel's plot well,
 * which is the point at which more resolution stops being visible. */
#define ASSIGN_PLOT_COLS 192

/* Capture rate. Cycled by the panel's own chip — mouse-only, deliberately no
 * keymap slot and no config key: this is a per-plot property, not a global
 * setting, and it should not appear in exported @cfg headers. */
typedef enum {
    ASSIGN_PLOT_RATE_ONCE = 0,  /* one capture, then frozen                 */
    ASSIGN_PLOT_RATE_1HZ,       /* at most one capture per second (default) */
    ASSIGN_PLOT_RATE_FRAME,     /* every frame                              */
    ASSIGN_PLOT_RATE_COUNT
} AssignPlotRate;

typedef enum {
    ASSIGN_PLOT_X_EXEC = 0,  /* X = execution index within the captured frame */
    ASSIGN_PLOT_X_FRAME      /* X = successive captures (time series)         */
} AssignPlotXMode;

/* One plot column: the min/max envelope of the values that fell into it. When
 * a frame has no more executions than there are columns (and always in
 * X_FRAME mode) each column holds a single value and lo == hi, so the envelope
 * collapses and the plotted line is exact. */
typedef struct {
    float lo, hi;
} AssignPlotColumn;

/* Flat per-frame view for the renderer. Columns are in display order, oldest
 * first — the ring is normalized here so no caller has to know about it. */
typedef struct {
    int open;
    int source_line_idx;
    int rate;             /* AssignPlotRate  */
    int x_mode;           /* AssignPlotXMode */
    int captured;         /* at least one capture has landed */
    int exec_count;       /* executions found by the most recent capture */
    const AssignPlotColumn *cols;
    int col_count;
    RunStatsSummary stats;
} AssignPlotView;

/* Target a source row (an index into the committed document). Resets the
 * buffer and statistics. Targeting the row that is already open is a no-op, so
 * callers that want toggle behavior should use assign_plot_toggle(). */
void assign_plot_open(int source_line_idx);

/* Untarget. Safe to call when already closed. */
void assign_plot_close(void);

/* Open `source_line_idx`, or close if that row is the one already open. */
void assign_plot_toggle(int source_line_idx);

int  assign_plot_is_open(void);
int  assign_plot_source_line(void);

/* Rate control. Both reset the buffer and statistics: a rate change redefines
 * the window the numbers describe. `dir` is +1 forward / -1 backward. */
void assign_plot_set_rate(int rate);
void assign_plot_cycle_rate(int dir);

/* Drop the buffer and statistics, keeping the target and rate. Re-arms
 * ASSIGN_PLOT_RATE_ONCE for one more capture. */
void assign_plot_reset(void);

/* Capture if the rate gate allows. `now_us` is supplied by the caller rather
 * than read from a clock here, which keeps this module free of any timing
 * dependency and makes the gate exactly reproducible under test.
 *
 * Closes the plot if the targeted row is gone or is no longer an assignment.
 * No-op — and no flat-program scan — when nothing is targeted. */
void assign_plot_capture(double now_us);

AssignPlotView assign_plot_view(void);

/* Full reset, for tests and for the standalone drivers. */
void assign_plot_reset_all(void);

#endif /* ASSIGN_PLOT_H */
