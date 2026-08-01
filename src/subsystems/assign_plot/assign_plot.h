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
 * nothing — and at the ONCE rate does not count as the shot, so a row that is
 * still being skipped does not freeze the plot empty.
 *
 * --- Several series on one plot ---
 *
 * Up to MAX_ASSIGN_PLOT_SERIES rows can be plotted together (Shift+right-click
 * adds a row to the open plot; plain right-click retargets it to that row
 * alone). Two things are shared and cannot be per-series:
 *
 *   The X axis. It is derived from the *primary* series — the one the plot was
 *   opened on — and a row is refused at add time when its execution count
 *   disagrees, because a once-per-frame row and a 64-per-frame row have
 *   nothing to put on a common X. The check runs against the flat program at
 *   the moment of the click; a row that has never executed cannot be judged
 *   and is admitted (it simply contributes nothing until it runs).
 *
 *   In X_EXEC mode series may still legitimately differ in execution count
 *   (two loops of different lengths), so each series is spread across the full
 *   width and X reads as normalized progress through that row's executions
 *   rather than as a shared execution index.
 *
 * The Y axis is shared too, and deliberately so: the reason to put two rows on
 * one plot is to compare them. Series of wildly different magnitudes will
 * flatten each other — the per-series statistics under the legend are what
 * carry the real numbers in that case.
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

/* Series the plot can hold at once. Four is the point where the legend still
 * fits the collapsed panel, four hues stay tellable apart, and a shared Y axis
 * still means something. */
#define MAX_ASSIGN_PLOT_SERIES 4

/* Capture rate. Cycled by the panel's own chip — mouse-only, deliberately no
 * keymap slot and no config key: this is a per-plot property, not a global
 * setting, and it should not appear in exported @cfg headers. */
typedef enum {
    /* One capture, then frozen — but only a frame in which *every* series
     * ran. A one-shot is meant to be read across, so it is not spent on a
     * frame where some row is still being skipped; that attempt is discarded
     * and the next frame tried. */
    ASSIGN_PLOT_RATE_ONCE = 0,
    ASSIGN_PLOT_RATE_1HZ,       /* at most one capture per second (default) */
    ASSIGN_PLOT_RATE_FRAME,     /* every frame                              */
    ASSIGN_PLOT_RATE_COUNT
} AssignPlotRate;

typedef enum {
    ASSIGN_PLOT_X_EXEC = 0,  /* X = execution index within the captured frame */
    ASSIGN_PLOT_X_FRAME      /* X = successive captures (time series)         */
} AssignPlotXMode;

/* Presentation flags. Held here beside the rate rather than in ui_state so the
 * panel's size query and its renderer read them out of the one view they
 * already take, and so all three mouse-only chips live in one place.
 *
 * Like the rate they survive open/close — retargeting a row should not undo
 * the zoom the user just asked for — and unlike the rate neither resets the
 * buffer: they change how captured values are drawn, not which values were
 * captured or what window the statistics describe. */

/* One plot column: the min/max envelope of the values that fell into it. When
 * a frame has no more executions than there are columns (and always in
 * X_FRAME mode) each column holds a single value and lo == hi, so the envelope
 * collapses and the plotted line is exact.
 *
 * `valid` is 0 for a column a series has no value for. Only X_FRAME mode
 * produces those, and only with more than one series: every series appends one
 * column per capture so that column N means capture N for all of them, which
 * means a series that did not execute that frame has to hold its place rather
 * than shift its history left against the others. The renderer breaks its line
 * there — a gap in a time series is information, not an absence of it. */
typedef struct {
    float lo, hi;
    int   valid;
} AssignPlotColumn;

/* One plotted row. Columns are in display order, oldest first — the ring is
 * normalized here so no caller has to know about it. */
typedef struct {
    int source_line_idx;
    int exec_count;       /* executions found by the most recent capture */
    const AssignPlotColumn *cols;
    int col_count;
    RunStatsSummary stats;
} AssignPlotSeriesView;

/* Flat per-frame view for the renderer.
 *
 * `series[0]` is the primary: it fixes the X axis mode for the whole plot (see
 * "The two X axes" above), and it is the series whose statistics the panel
 * shows when nothing is hovered. Series are held in the order they were
 * added. */
typedef struct {
    int open;
    int rate;             /* AssignPlotRate  */
    int x_mode;           /* AssignPlotXMode */
    int captured;         /* at least one capture has landed */
    int expanded;         /* double-size panel                            */
    int y_log;            /* log Y requested (see ui/support/assign_plot.h
                           * for when it is honored) */
    int series_count;
    AssignPlotSeriesView series[MAX_ASSIGN_PLOT_SERIES];
} AssignPlotView;

/* Target a source row (an index into the committed document), replacing every
 * series with that one. Resets the buffers and statistics. Retargeting the row
 * that is already the sole series is a no-op, so callers that want toggle
 * behavior should use assign_plot_toggle(). */
void assign_plot_open(int source_line_idx);

/* Untarget everything. Safe to call when already closed. */
void assign_plot_close(void);

/* Open `source_line_idx`, or close if that row is the only one plotted. */
void assign_plot_toggle(int source_line_idx);

/* Outcome of assign_plot_toggle_series(), which the caller reports — a refusal
 * that said nothing would look like a dead gesture. */
typedef enum {
    ASSIGN_PLOT_SERIES_ADDED = 0,
    ASSIGN_PLOT_SERIES_REMOVED,     /* it was already plotted               */
    ASSIGN_PLOT_SERIES_FULL,        /* MAX_ASSIGN_PLOT_SERIES already held  */
    ASSIGN_PLOT_SERIES_INCOMPATIBLE /* its X axis cannot be the plot's      */
} AssignPlotSeriesResult;

/* Add `source_line_idx` as an additional series, or drop it if it is already
 * plotted. Opens the plot on that row when nothing is plotted yet, so the
 * add gesture works from a closed panel. Dropping the last series closes.
 *
 * Adding normally keeps existing history: the new series starts empty and
 * fills from the next capture. A frozen one-shot instead clears and re-arms so
 * its next snapshot describes the same frame for every series. */
int  assign_plot_toggle_series(int source_line_idx);  /* AssignPlotSeriesResult */

int  assign_plot_is_open(void);
int  assign_plot_series_count(void);

/* The primary series' row, or -1 when closed. */
int  assign_plot_source_line(void);

/* Is `source_line_idx` one of the plotted series? */
int  assign_plot_has_series(int source_line_idx);

/* Rate control. Both reset the buffer and statistics: a rate change redefines
 * the window the numbers describe. `dir` is +1 forward / -1 backward. */
void assign_plot_set_rate(int rate);
void assign_plot_cycle_rate(int dir);

/* Presentation toggles. Neither touches the buffer or the statistics. */
void assign_plot_toggle_expanded(void);
void assign_plot_toggle_y_log(void);
int  assign_plot_is_expanded(void);

/* Drop the buffer and statistics, keeping the target and rate. Re-arms
 * ASSIGN_PLOT_RATE_ONCE for one more capture. */
void assign_plot_reset(void);

/* --- Replay coupling ---------------------------------------------------
 *
 * Replay walks a program counter through the flat program while the rest of
 * the app keeps running, so unless the simulation clock is paused the program
 * is re-flattened under the PC every frame. A marker saying "the PC is here"
 * is only true of the frame it was computed from, which means the trace it is
 * drawn over has to be that same frame's.
 *
 * Hence the two calls below, and the division of labor between them: this
 * module knows nothing about replay (it takes a flat index and a flag), and
 * the controller knows nothing about capture rates. */

/* Capture every frame regardless of the rate gate. ASSIGN_PLOT_RATE_ONCE is
 * deliberately exempt: a one-shot is a snapshot the user asked to freeze, and
 * overriding it would destroy the thing being looked at. The panel draws the
 * rate chip inert while this is on, so a 1 Hz setting that is not in force is
 * visible rather than silently ignored. */
void assign_plot_set_live_capture(int on);

/* Where a replay program counter falls within each series' executions.
 * `exec_limit` is the flat index the executor is rendering up to (negative for
 * "no clamp"); `out_frac` receives a 0..1 position along that series' own
 * execution span, or -1 for a series with no marker to draw. Returns 1 if any
 * series produced one.
 *
 * X_FRAME mode produces nothing: there a column is a whole capture, so a
 * position *within* a frame has no column to point at. Neither does
 * ASSIGN_PLOT_RATE_ONCE, whose frozen columns are some earlier frame's — a
 * marker there would put this frame's position on another frame's values,
 * which is exactly what the live-capture override exists to prevent.
 *
 * Counts come from the flat program as it stands now, numerator and
 * denominator both, so the fraction is self-consistent even if the capture
 * that filled the columns saw a different frame. */
int assign_plot_exec_progress(int exec_limit,
                              float out_frac[MAX_ASSIGN_PLOT_SERIES]);

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
