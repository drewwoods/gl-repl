/*
 * tests/test_assign_plot.c -- assignment-value capture.
 *
 * Covers the capture side only (the panel renderer is tests/test_ui_assign_plot.c):
 * the rate gate, the two X-axis modes and the flip between them, envelope
 * decimation, the ring scroll, target drift, and the invariant that the
 * statistics see every value even when the plot does not.
 *
 * The clock is a plain parameter to assign_plot_capture(), so every timing
 * case here is exact - there is no fake-clock hook to install.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app/glr_assign_plot_bridge.h"
#include "app/glr_ctrl.h"
#include "editor/input.h"
#include "repl/command.h"
#include "repl/eval.h"
#include "repl/pipeline.h"
#include "repl/state.h"
#include "repl/state_owners.h"
#include "subsystems/assign_plot/assign_plot.h"
#include "support/test_harness.h"

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond)      TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, exp)   TEST_ASSERT_INT(&g_harness, label, got, exp)
#define ASSERT_FLOAT(label, got, exp) \
    TEST_ASSERT_FLOAT(&g_harness, label, got, exp, 1e-4f)

#define US_PER_SEC 1000000.0

/* The primary series. Most cases here plot exactly one row, so naming the
 * series once keeps the assertions about the values rather than the shape of
 * the view. */
#define S0(v) ((v).series[0])

static void load_scene(const char *const *lines) {
    glr_ctrl_reset_all();
    assign_plot_reset_all();
    for (int i = 0; lines[i]; i++)
        editor_feed_line(lines[i]);
    repl_flatten_commands(0);
    repl_state_flat_program_clear_dirty();
}

/* Index of the first source row of `type`. -1 when absent. */
static int find_row(CmdType type) {
    const GLCmd *cmds = repl_state_document_cmds();
    int n = repl_state_document_count();
    for (int i = 0; i < n; i++)
        if (cmds[i].valid && cmds[i].type == type)
            return i;
    return -1;
}

/* Re-flatten as the frame loop would, so a capture sees this frame's values. */
static void reflatten(void) {
    repl_flatten_commands(0);
    repl_state_flat_program_clear_dirty();
}

/* ------------------------------------------------------------------ */

/* A row inside a loop: X is the execution index within the frame. */
static void test_exec_mode_captures_loop_values(void) {
    static const char *const k_scene[] = {
        "float x;",
        "for(i, 0, 8) {",
        "x = i * 2;",
        "}",
        NULL
    };
    int row;
    AssignPlotView v;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    ASSERT_TRUE("scene has an assignment row", row >= 0);

    assign_plot_open(row);
    assign_plot_capture(0.0);
    v = assign_plot_view();

    ASSERT_TRUE("plot is open", v.open);
    ASSERT_INT("x axis is exec index", v.x_mode, ASSIGN_PLOT_X_EXEC);
    ASSERT_INT("one execution per loop iteration", S0(v).exec_count, 8);
    ASSERT_INT("one column per execution", S0(v).col_count, 8);
    ASSERT_INT("every value reached the statistics", (int)S0(v).stats.count, 8);
    /* i * 2 over i in [0, 8) */
    ASSERT_FLOAT("min is the first iteration", S0(v).stats.min, 0.0);
    ASSERT_FLOAT("max is the last iteration", S0(v).stats.max, 14.0);
    ASSERT_FLOAT("mean of 0,2,..,14", S0(v).stats.mean, 7.0);
    ASSERT_FLOAT("first column holds the first value", S0(v).cols[0].lo, 0.0);
    ASSERT_FLOAT("last column holds the last value", S0(v).cols[7].hi, 14.0);
    ASSERT_TRUE("undecimated columns collapse to a point",
                S0(v).cols[3].lo == S0(v).cols[3].hi);
}

/* A top-level row runs once per frame: X becomes successive captures. */
static void test_frame_mode_accumulates_across_captures(void) {
    static const char *const k_scene[] = {
        "float x;",
        "x = t * 10;",
        NULL
    };
    int row;
    AssignPlotView v;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    assign_plot_open(row);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_FRAME);

    for (int frame = 0; frame < 5; frame++) {
        repl_state_time_set((float)frame);
        reflatten();
        assign_plot_capture((double)frame * 1000.0);
    }

    v = assign_plot_view();
    ASSERT_INT("x axis switched to captures", v.x_mode, ASSIGN_PLOT_X_FRAME);
    ASSERT_INT("one execution per frame", S0(v).exec_count, 1);
    ASSERT_INT("one column per capture", S0(v).col_count, 5);
    ASSERT_INT("one sample per capture", (int)S0(v).stats.count, 5);
    ASSERT_FLOAT("oldest capture is t=0", S0(v).cols[0].lo, 0.0);
    ASSERT_FLOAT("newest capture is t=4", S0(v).cols[4].hi, 40.0);
}

/* The buffer scrolls rather than wrapping, so columns stay in display order. */
static void test_frame_mode_ring_scrolls(void) {
    static const char *const k_scene[] = {
        "float x;",
        "x = t;",
        NULL
    };
    int row;
    int overflow = ASSIGN_PLOT_COLS + 20;
    AssignPlotView v;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    assign_plot_open(row);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_FRAME);

    for (int frame = 0; frame < overflow; frame++) {
        repl_state_time_set((float)frame);
        reflatten();
        assign_plot_capture((double)frame);
    }

    v = assign_plot_view();
    ASSERT_INT("column count saturates at the cap",
               S0(v).col_count, ASSIGN_PLOT_COLS);
    ASSERT_INT("statistics keep counting past the cap",
               (int)S0(v).stats.count, overflow);
    ASSERT_FLOAT("newest capture is at the right edge",
                 S0(v).cols[ASSIGN_PLOT_COLS - 1].hi, (float)(overflow - 1));
    ASSERT_FLOAT("oldest surviving capture leads the buffer",
                 S0(v).cols[0].lo, (float)(overflow - ASSIGN_PLOT_COLS));
    ASSERT_FLOAT("statistics still remember the dropped minimum",
                 S0(v).stats.min, 0.0);
}

/* More executions than columns: the plot decimates, the statistics do not. */
static void test_decimation_preserves_extremes_and_stats(void) {
    static const char *const k_scene[] = {
        "float x;",
        "for(i, 0, 500) {",
        "x = i;",
        "}",
        NULL
    };
    int row;
    AssignPlotView v;
    double expected_mean = 0.0;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    assign_plot_open(row);
    assign_plot_capture(0.0);
    v = assign_plot_view();

    ASSERT_INT("all 500 executions were found", S0(v).exec_count, 500);
    ASSERT_INT("columns are capped", S0(v).col_count, ASSIGN_PLOT_COLS);
    ASSERT_INT("statistics saw every execution", (int)S0(v).stats.count, 500);

    for (int i = 0; i < 500; i++) expected_mean += (double)i;
    expected_mean /= 500.0;
    ASSERT_FLOAT("mean is over all 500 values", S0(v).stats.mean, expected_mean);
    ASSERT_FLOAT("min survives decimation", S0(v).stats.min, 0.0);
    ASSERT_FLOAT("max survives decimation", S0(v).stats.max, 499.0);

    /* Columns are envelopes over a monotonic ramp, so they must be ordered
     * and must together span the whole range. */
    ASSERT_FLOAT("first column starts at the first value", S0(v).cols[0].lo, 0.0);
    ASSERT_FLOAT("last column ends at the last value",
                 S0(v).cols[ASSIGN_PLOT_COLS - 1].hi, 499.0);
    {
        int ordered = 1;
        int has_span = 0;
        for (int c = 0; c < S0(v).col_count; c++) {
            if (S0(v).cols[c].hi < S0(v).cols[c].lo) ordered = 0;
            if (c > 0 && S0(v).cols[c].lo < S0(v).cols[c - 1].hi) ordered = 0;
            if (S0(v).cols[c].hi > S0(v).cols[c].lo) has_span = 1;
        }
        ASSERT_TRUE("columns are non-overlapping and ascending", ordered);
        ASSERT_TRUE("decimated columns carry a real min/max span", has_span);
    }
}

/* Rate gate. */
static void test_rate_once_captures_exactly_once(void) {
    static const char *const k_scene[] = {
        "float x;",
        "x = t;",
        NULL
    };
    int row;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    assign_plot_open(row);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_ONCE);

    for (int frame = 0; frame < 10; frame++) {
        repl_state_time_set((float)frame);
        reflatten();
        assign_plot_capture((double)frame * US_PER_SEC);
    }
    ASSERT_INT("ONCE keeps the first capture only",
               (int)S0(assign_plot_view()).stats.count, 1);
    ASSERT_FLOAT("and it is the first frame's value",
                 S0(assign_plot_view()).stats.max, 0.0);
}

static void test_rate_1hz_gates_on_the_clock(void) {
    static const char *const k_scene[] = {
        "float x;",
        "x = t;",
        NULL
    };
    int row;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    assign_plot_open(row);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_1HZ);

    /* 121 frames at 60 fps spans [0 s, 2 s] inclusive, so the capture
     * instants are frame 0 (nothing captured yet), frame 60 and frame 120. */
    for (int frame = 0; frame <= 120; frame++) {
        repl_state_time_set((float)frame);
        reflatten();
        assign_plot_capture((double)frame * (US_PER_SEC / 60.0));
    }
    ASSERT_INT("1 Hz captures once per elapsed second",
               (int)S0(assign_plot_view()).stats.count, 3);

    /* One frame short of the next second is still gated out. */
    assign_plot_reset();
    for (int frame = 0; frame < 60; frame++)
        assign_plot_capture((double)frame * (US_PER_SEC / 60.0));
    ASSERT_INT("a sub-second burst captures only once",
               (int)S0(assign_plot_view()).stats.count, 1);
}

static void test_rate_change_resets_the_window(void) {
    static const char *const k_scene[] = {
        "float x;",
        "x = t;",
        NULL
    };
    int row;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    assign_plot_open(row);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_FRAME);
    for (int frame = 0; frame < 4; frame++) {
        repl_state_time_set((float)frame);
        reflatten();
        assign_plot_capture((double)frame);
    }
    ASSERT_INT("four frames captured", (int)S0(assign_plot_view()).stats.count, 4);

    assign_plot_set_rate(ASSIGN_PLOT_RATE_FRAME);
    ASSERT_INT("re-selecting a rate clears the window",
               (int)S0(assign_plot_view()).stats.count, 0);
    ASSERT_INT("and the plot buffer with it",
               S0(assign_plot_view()).col_count, 0);
}

static void test_cycle_rate_wraps_both_ways(void) {
    assign_plot_reset_all();
    assign_plot_set_rate(ASSIGN_PLOT_RATE_ONCE);
    assign_plot_cycle_rate(1);
    ASSERT_INT("forward: once -> 1 Hz",
               assign_plot_view().rate, ASSIGN_PLOT_RATE_1HZ);
    assign_plot_cycle_rate(1);
    ASSERT_INT("forward: 1 Hz -> frame",
               assign_plot_view().rate, ASSIGN_PLOT_RATE_FRAME);
    assign_plot_cycle_rate(1);
    ASSERT_INT("forward wraps to once",
               assign_plot_view().rate, ASSIGN_PLOT_RATE_ONCE);
    assign_plot_cycle_rate(-1);
    ASSERT_INT("backward wraps to frame",
               assign_plot_view().rate, ASSIGN_PLOT_RATE_FRAME);
}

/* Switching axes must not average two different X meanings together. */
static void test_mode_flip_clears_the_window(void) {
    static const char *const k_scene[] = {
        "float x;",
        "float n;",
        "n = 1;",
        "for(i, 0, 6) {",
        "x = i;",
        "}",
        NULL
    };
    int loop_row = -1, top_row = -1;
    const GLCmd *cmds;
    int count;

    load_scene(k_scene);
    cmds = repl_state_document_cmds();
    count = repl_state_document_count();
    for (int i = 0; i < count; i++) {
        if (!cmds[i].valid || cmds[i].type != CMD_VAR_ASSIGN) continue;
        if (top_row < 0) top_row = i;
        else loop_row = i;
    }
    ASSERT_TRUE("scene has a top-level and a loop assignment",
                top_row >= 0 && loop_row >= 0);

    /* Build up a frame-mode history on the top-level row... */
    assign_plot_open(top_row);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_FRAME);
    for (int frame = 0; frame < 3; frame++)
        assign_plot_capture((double)frame);
    ASSERT_INT("frame mode collected three captures",
               (int)S0(assign_plot_view()).stats.count, 3);
    ASSERT_INT("frame axis", assign_plot_view().x_mode, ASSIGN_PLOT_X_FRAME);

    /* ...then retarget the loop row, which is exec-mode. */
    assign_plot_open(loop_row);
    assign_plot_capture(100.0);
    ASSERT_INT("retargeting switched the axis",
               assign_plot_view().x_mode, ASSIGN_PLOT_X_EXEC);
    ASSERT_INT("and the window is only the new row's executions",
               (int)S0(assign_plot_view()).stats.count, 6);
}

/* A[i] = expr is the other assignment type; its value lives in args[2]. */
static void test_scratch_assign_is_plotted(void) {
    static const char *const k_scene[] = {
        "for(i, 0, 4) {",
        "A[i] = i * 3;",
        "}",
        NULL
    };
    int row;
    AssignPlotView v;

    load_scene(k_scene);
    row = find_row(CMD_SCRATCH_ASSIGN);
    ASSERT_TRUE("scene has a scratch assignment", row >= 0);

    assign_plot_open(row);
    assign_plot_capture(0.0);
    v = assign_plot_view();
    ASSERT_INT("four scratch writes", S0(v).exec_count, 4);
    ASSERT_FLOAT("values come from args[2]", S0(v).stats.max, 9.0);
    ASSERT_FLOAT("and start at zero", S0(v).stats.min, 0.0);
}

/* Signed values are the reason this cannot reuse the duration histogram. */
static void test_negative_values_are_measured_exactly(void) {
    static const char *const k_scene[] = {
        "float x;",
        "for(i, 0, 5) {",
        "x = i - 2;",
        "}",
        NULL
    };
    int row;
    AssignPlotView v;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    assign_plot_open(row);
    assign_plot_capture(0.0);
    v = assign_plot_view();

    ASSERT_FLOAT("min is negative", S0(v).stats.min, -2.0);
    ASSERT_FLOAT("max is positive", S0(v).stats.max, 2.0);
    ASSERT_FLOAT("mean straddles zero", S0(v).stats.mean, 0.0);
    /* -2,-1,0,1,2 -> sample variance 10/4 = 2.5 */
    ASSERT_FLOAT("sample stddev uses the n-1 divisor",
                 S0(v).stats.stddev, sqrt(2.5));
}

/* Target lifecycle. */
static void test_toggle_and_close(void) {
    static const char *const k_scene[] = {
        "float x;",
        "x = t;",
        NULL
    };
    int row;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);

    ASSERT_TRUE("starts closed", !assign_plot_is_open());
    assign_plot_toggle(row);
    ASSERT_TRUE("toggle opens", assign_plot_is_open());
    ASSERT_INT("on the requested row", assign_plot_source_line(), row);
    assign_plot_toggle(row);
    ASSERT_TRUE("toggling the same row closes", !assign_plot_is_open());
    ASSERT_INT("closed reports no line", assign_plot_source_line(), -1);
}

static void test_capture_is_inert_when_closed(void) {
    static const char *const k_scene[] = {
        "float x;",
        "x = t;",
        NULL
    };

    load_scene(k_scene);
    assign_plot_capture(0.0);
    ASSERT_TRUE("no target: nothing captured",
                !assign_plot_view().captured);
    ASSERT_INT("and no samples", (int)S0(assign_plot_view()).stats.count, 0);
}

/* The row is gone, or is no longer an assignment: close rather than plot a
 * neighbour that happens to sit at the same index. */
static void test_target_drift_closes_the_plot(void) {
    static const char *const k_scene[] = {
        "float x;",
        "x = t;",
        NULL
    };
    static const char *const k_other[] = {
        "glColor3f(1, 0, 0);",
        "glVertex3f(0, 0, 0);",
        NULL
    };
    int row;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    assign_plot_open(row);
    assign_plot_capture(0.0);
    ASSERT_TRUE("open before the swap", assign_plot_is_open());

    /* Replace the document wholesale; the same index is now a GL command. */
    load_scene(k_other);
    assign_plot_open(row);   /* re-target, since reset_all closed it */
    assign_plot_capture(0.0);
    ASSERT_TRUE("a non-assignment row closes the plot",
                !assign_plot_is_open());
}

static void test_out_of_range_target_closes(void) {
    static const char *const k_scene[] = {
        "float x;",
        "x = t;",
        NULL
    };

    load_scene(k_scene);
    assign_plot_open(repl_state_document_count() + 5);
    assign_plot_capture(0.0);
    ASSERT_TRUE("a row past the document end closes the plot",
                !assign_plot_is_open());
}

/* A row inside a false `if` runs zero times. That must not throw away the
 * history the row already has, nor flip the axis. */
static void test_zero_executions_preserve_history(void) {
    static const char *const k_scene[] = {
        "float x;",
        "if(t) {",
        "x = t;",
        "}",
        NULL
    };
    int row;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    ASSERT_TRUE("scene has a guarded assignment", row >= 0);

    assign_plot_open(row);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_FRAME);
    for (int frame = 1; frame <= 3; frame++) {
        repl_state_time_set((float)frame);
        reflatten();
        assign_plot_capture((double)frame);
    }
    ASSERT_INT("three captures while the gate is open",
               (int)S0(assign_plot_view()).stats.count, 3);

    /* Close the gate: at t == 0 the `if` is false and the row never runs. */
    repl_state_time_set(0.0f);
    reflatten();
    assign_plot_capture(10.0);

    ASSERT_TRUE("plot stays open", assign_plot_is_open());
    ASSERT_INT("no executions this frame",
               S0(assign_plot_view()).exec_count, 0);
    ASSERT_INT("history is untouched",
               (int)S0(assign_plot_view()).stats.count, 3);
    ASSERT_INT("axis is untouched",
               assign_plot_view().x_mode, ASSIGN_PLOT_X_FRAME);
}

static void test_reset_keeps_target_and_rate(void) {
    static const char *const k_scene[] = {
        "float x;",
        "for(i, 0, 4) {",
        "x = i;",
        "}",
        NULL
    };
    int row;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    assign_plot_open(row);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_FRAME);
    assign_plot_capture(0.0);
    ASSERT_INT("captured", (int)S0(assign_plot_view()).stats.count, 4);

    assign_plot_reset();
    ASSERT_TRUE("still open", assign_plot_is_open());
    ASSERT_INT("still on the same row", assign_plot_source_line(), row);
    ASSERT_INT("rate is kept",
               assign_plot_view().rate, ASSIGN_PLOT_RATE_FRAME);
    ASSERT_INT("samples are dropped",
               (int)S0(assign_plot_view()).stats.count, 0);
}

/* --- several series on one plot --- */

/* Index of the n-th (0-based) source row of `type`. */
static int find_nth_row(CmdType type, int n) {
    const GLCmd *cmds = repl_state_document_cmds();
    int count = repl_state_document_count();
    int seen = 0;
    for (int i = 0; i < count; i++) {
        if (!cmds[i].valid || cmds[i].type != type) continue;
        if (seen++ == n) return i;
    }
    return -1;
}

/* Two rows in the same loop: both fill, each with its own values. */
static void test_second_series_captures_independently(void) {
    static const char *const k_scene[] = {
        "float x;",
        "float y;",
        "for(i, 0, 8) {",
        "x = i * 2;",
        "y = i + 100;",
        "}",
        NULL
    };
    int row_x, row_y;
    AssignPlotView v;

    load_scene(k_scene);
    row_x = find_nth_row(CMD_VAR_ASSIGN, 0);
    row_y = find_nth_row(CMD_VAR_ASSIGN, 1);
    ASSERT_TRUE("scene has two assignment rows", row_x >= 0 && row_y >= 0);

    assign_plot_open(row_x);
    ASSERT_INT("adding the second row is accepted",
               assign_plot_toggle_series(row_y), ASSIGN_PLOT_SERIES_ADDED);
    ASSERT_INT("two series", assign_plot_series_count(), 2);
    ASSERT_INT("the primary is still the row it opened on",
               assign_plot_source_line(), row_x);

    assign_plot_capture(0.0);
    v = assign_plot_view();

    ASSERT_INT("view carries both", v.series_count, 2);
    ASSERT_INT("primary kept its row", v.series[0].source_line_idx, row_x);
    ASSERT_INT("secondary took the other", v.series[1].source_line_idx, row_y);
    ASSERT_INT("both ran eight times", v.series[1].exec_count, 8);
    /* Each series carries its own values and its own statistics - a shared
     * buffer would show one row's numbers under both names. */
    ASSERT_FLOAT("x starts at 0", v.series[0].cols[0].lo, 0.0);
    ASSERT_FLOAT("y starts at 100", v.series[1].cols[0].lo, 100.0);
    ASSERT_FLOAT("x tops out at 14", v.series[0].stats.max, 14.0);
    ASSERT_FLOAT("y tops out at 107", v.series[1].stats.max, 107.0);
}

/* Before the first capture, compatibility comes from the primary row's live
 * execution count rather than the default X mode. */
static void test_top_level_series_can_join_before_first_capture(void) {
    static const char *const k_scene[] = {
        "float x;", "float y;", "x = t;", "y = t * 2;", NULL
    };
    int row_x, row_y;

    load_scene(k_scene);
    row_x = find_nth_row(CMD_VAR_ASSIGN, 0);
    row_y = find_nth_row(CMD_VAR_ASSIGN, 1);
    assign_plot_open(row_x);

    ASSERT_INT("two top-level rows are compatible before capture",
               assign_plot_toggle_series(row_y), ASSIGN_PLOT_SERIES_ADDED);
    ASSERT_INT("both rows joined", assign_plot_series_count(), 2);
}

/* Adding to a frozen one-shot re-arms one coherent snapshot rather than
 * leaving the new series permanently empty. */
static void test_once_rate_rearms_for_series_added_later(void) {
    static const char *const k_scene[] = {
        "float x;", "float y;", "x = t;", "y = t * 2;", NULL
    };
    int row_x, row_y;
    AssignPlotView v;

    load_scene(k_scene);
    row_x = find_nth_row(CMD_VAR_ASSIGN, 0);
    row_y = find_nth_row(CMD_VAR_ASSIGN, 1);
    assign_plot_open(row_x);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_ONCE);
    assign_plot_capture(0.0);
    ASSERT_INT("the first one-shot captured",
               (int)S0(assign_plot_view()).stats.count, 1);

    ASSERT_INT("the second row joins the frozen plot",
               assign_plot_toggle_series(row_y), ASSIGN_PLOT_SERIES_ADDED);
    v = assign_plot_view();
    ASSERT_INT("the old one-shot is cleared", (int)v.series[0].stats.count, 0);
    ASSERT_INT("the new series starts with it", (int)v.series[1].stats.count, 0);

    assign_plot_capture(1.0);
    v = assign_plot_view();
    ASSERT_INT("the primary recaptures once", (int)v.series[0].stats.count, 1);
    ASSERT_INT("the added row captures once", (int)v.series[1].stats.count, 1);
}

/* The point of a one-shot over several rows: every series frozen from the
 * *same* frame, so the numbers can be read against each other. Values that
 * track `t` at different multiples make a cross-frame mix detectable. */
static void test_once_snapshot_is_one_frame(void) {
    static const char *const k_scene[] = {
        "float a;", "float b;", "a = t;", "b = t * 10;", NULL
    };
    int row_a, row_b;
    AssignPlotView v;

    load_scene(k_scene);
    row_a = find_nth_row(CMD_VAR_ASSIGN, 0);
    row_b = find_nth_row(CMD_VAR_ASSIGN, 1);

    repl_state_time_set(1.0f);
    reflatten();
    assign_plot_open(row_a);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_ONCE);
    assign_plot_capture(0.0);
    ASSERT_FLOAT("the first shot froze t=1", S0(assign_plot_view()).cols[0].lo,
                 1.0);

    /* Adding a row re-arms; the next frame is what both series describe. */
    assign_plot_toggle_series(row_b);
    repl_state_time_set(5.0f);
    reflatten();
    assign_plot_capture(1.0);

    v = assign_plot_view();
    ASSERT_FLOAT("the primary re-froze on the new frame",
                 v.series[0].cols[0].lo, 5.0);
    ASSERT_FLOAT("and the added row froze on that same frame",
                 v.series[1].cols[0].lo, 50.0);
    ASSERT_INT("one column each", v.series[0].col_count, 1);
    ASSERT_INT("one column each", v.series[1].col_count, 1);

    /* Frozen for good: later frames do not move either series. */
    repl_state_time_set(9.0f);
    reflatten();
    assign_plot_capture(2.0);
    v = assign_plot_view();
    ASSERT_FLOAT("still the frozen frame", v.series[0].cols[0].lo, 5.0);
    ASSERT_FLOAT("for both series", v.series[1].cols[0].lo, 50.0);
}

/* A one-shot must not be spent on a frame where some row has not run: that
 * freezes an incomplete comparison no later frame can repair. */
static void test_once_waits_for_a_complete_frame(void) {
    static const char *const k_scene[] = {
        "float a;", "float b;", "a = t;", "if(t) {", "b = t * 10;", "}", NULL
    };
    int row_a, row_b;
    AssignPlotView v;

    load_scene(k_scene);
    row_a = find_nth_row(CMD_VAR_ASSIGN, 0);
    row_b = find_nth_row(CMD_VAR_ASSIGN, 1);

    repl_state_time_set(0.0f);   /* guard closed: b does not run */
    reflatten();
    assign_plot_open(row_a);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_ONCE);
    assign_plot_toggle_series(row_b);

    assign_plot_capture(0.0);
    v = assign_plot_view();
    ASSERT_INT("an incomplete frame is not kept", (int)v.series[0].stats.count, 0);
    ASSERT_INT("nor for the silent row", (int)v.series[1].stats.count, 0);
    ASSERT_INT("and the shot is still armed", v.captured, 0);

    repl_state_time_set(3.0f);   /* guard open: both rows run */
    reflatten();
    assign_plot_capture(1.0);
    v = assign_plot_view();
    ASSERT_INT("the complete frame is taken", (int)v.series[0].stats.count, 1);
    ASSERT_INT("for both rows", (int)v.series[1].stats.count, 1);
    ASSERT_FLOAT("and both describe it", v.series[0].cols[0].lo, 3.0);
    ASSERT_FLOAT("from the same frame", v.series[1].cols[0].lo, 30.0);

    /* Now spent: a later frame must not move it. */
    repl_state_time_set(7.0f);
    reflatten();
    assign_plot_capture(2.0);
    ASSERT_FLOAT("the shot is spent", S0(assign_plot_view()).cols[0].lo, 3.0);
}

/* The same rule with one series: a guarded row used to freeze as
 * "(not executed)" on the first frame and never recover. */
static void test_once_single_series_waits_to_run(void) {
    static const char *const k_scene[] = {
        "float a;", "if(t) {", "a = t * 2;", "}", NULL
    };
    int row;

    load_scene(k_scene);
    row = find_nth_row(CMD_VAR_ASSIGN, 0);

    repl_state_time_set(0.0f);
    reflatten();
    assign_plot_open(row);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_ONCE);
    assign_plot_capture(0.0);
    ASSERT_INT("nothing captured while the row is skipped",
               (int)S0(assign_plot_view()).stats.count, 0);

    repl_state_time_set(4.0f);
    reflatten();
    assign_plot_capture(1.0);
    ASSERT_INT("the first frame it runs is the one kept",
               (int)S0(assign_plot_view()).stats.count, 1);
    ASSERT_FLOAT("with that frame's value",
                 S0(assign_plot_view()).cols[0].lo, 8.0);
}

/* Shift+right-click on a plotted row removes it again. */
static void test_toggle_series_removes_and_closes(void) {
    static const char *const k_scene[] = {
        "float x;",
        "float y;",
        "for(i, 0, 4) {",
        "x = i;",
        "y = i;",
        "}",
        NULL
    };
    int row_x, row_y;

    load_scene(k_scene);
    row_x = find_nth_row(CMD_VAR_ASSIGN, 0);
    row_y = find_nth_row(CMD_VAR_ASSIGN, 1);

    assign_plot_open(row_x);
    assign_plot_toggle_series(row_y);
    ASSERT_INT("two series", assign_plot_series_count(), 2);

    ASSERT_INT("toggling a plotted row removes it",
               assign_plot_toggle_series(row_y), ASSIGN_PLOT_SERIES_REMOVED);
    ASSERT_INT("back to one", assign_plot_series_count(), 1);
    ASSERT_TRUE("and it is the primary that stayed",
                assign_plot_has_series(row_x) && !assign_plot_has_series(row_y));

    ASSERT_INT("removing the last one is still a removal",
               assign_plot_toggle_series(row_x), ASSIGN_PLOT_SERIES_REMOVED);
    ASSERT_INT("and closes the plot", assign_plot_is_open(), 0);
    ASSERT_INT("with no series left", assign_plot_series_count(), 0);

    /* From closed, the add gesture opens on that row rather than doing
     * nothing - otherwise the modifier would be a dead key on a closed panel. */
    ASSERT_INT("adding from closed opens",
               assign_plot_toggle_series(row_x), ASSIGN_PLOT_SERIES_ADDED);
    ASSERT_INT("open on one series", assign_plot_series_count(), 1);
}

static void test_series_cap_is_enforced(void) {
    static const char *const k_scene[] = {
        "float a;", "float b;", "float c;", "float d;", "float q;",
        "for(i, 0, 4) {",
        "a = i;", "b = i;", "c = i;", "d = i;", "q = i;",
        "}",
        NULL
    };
    int rows[5];

    load_scene(k_scene);
    for (int i = 0; i < 5; i++) {
        rows[i] = find_nth_row(CMD_VAR_ASSIGN, i);
        ASSERT_TRUE("scene row exists", rows[i] >= 0);
    }

    assign_plot_open(rows[0]);
    for (int i = 1; i < MAX_ASSIGN_PLOT_SERIES; i++)
        ASSERT_INT("filling up to the cap",
                   assign_plot_toggle_series(rows[i]),
                   ASSIGN_PLOT_SERIES_ADDED);
    ASSERT_INT("full", assign_plot_series_count(), MAX_ASSIGN_PLOT_SERIES);

    ASSERT_INT("one more is refused",
               assign_plot_toggle_series(rows[MAX_ASSIGN_PLOT_SERIES]),
               ASSIGN_PLOT_SERIES_FULL);
    ASSERT_INT("and nothing was displaced",
               assign_plot_series_count(), MAX_ASSIGN_PLOT_SERIES);
    ASSERT_TRUE("the refused row is not plotted",
                !assign_plot_has_series(rows[MAX_ASSIGN_PLOT_SERIES]));
}

/* A once-per-frame row and a many-per-frame row cannot share an X axis. */
static void test_incompatible_x_mode_is_refused(void) {
    static const char *const k_scene[] = {
        "float top;",
        "float inner;",
        "top = t * 2;",
        "for(i, 0, 8) {",
        "inner = i;",
        "}",
        NULL
    };
    int row_top, row_inner;

    load_scene(k_scene);
    row_top   = find_nth_row(CMD_VAR_ASSIGN, 0);
    row_inner = find_nth_row(CMD_VAR_ASSIGN, 1);
    ASSERT_TRUE("scene has both shapes", row_top >= 0 && row_inner >= 0);

    /* Primary runs once per frame -> the plot is a capture time series. */
    assign_plot_open(row_top);
    assign_plot_capture(0.0);
    ASSERT_INT("primary put the plot on the capture axis",
               assign_plot_view().x_mode, ASSIGN_PLOT_X_FRAME);
    ASSERT_INT("a loop row is refused",
               assign_plot_toggle_series(row_inner),
               ASSIGN_PLOT_SERIES_INCOMPATIBLE);
    ASSERT_INT("still one series", assign_plot_series_count(), 1);

    /* And the other way round. */
    assign_plot_open(row_inner);
    assign_plot_capture(0.0);
    ASSERT_INT("primary put the plot on the exec axis",
               assign_plot_view().x_mode, ASSIGN_PLOT_X_EXEC);
    ASSERT_INT("a once-per-frame row is refused",
               assign_plot_toggle_series(row_top),
               ASSIGN_PLOT_SERIES_INCOMPATIBLE);
    ASSERT_INT("still one series", assign_plot_series_count(), 1);
}

/* In capture mode every series appends one column per capture, so column N
 * means capture N for all of them - a series that did not run holds its place
 * with an invalid column instead of sliding its history left. */
static void test_frame_mode_keeps_series_aligned(void) {
    static const char *const k_scene[] = {
        "float always;",
        "float sometimes;",
        "always = t;",
        "if(t) {",
        "sometimes = t * 10;",
        "}",
        NULL
    };
    int row_always, row_sometimes;
    AssignPlotView v;

    load_scene(k_scene);
    row_always    = find_nth_row(CMD_VAR_ASSIGN, 0);
    row_sometimes = find_nth_row(CMD_VAR_ASSIGN, 1);
    ASSERT_TRUE("scene has both rows",
                row_always >= 0 && row_sometimes >= 0);

    assign_plot_open(row_always);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_FRAME);
    assign_plot_capture(0.0);
    ASSERT_INT("capture axis", assign_plot_view().x_mode,
               ASSIGN_PLOT_X_FRAME);
    ASSERT_INT("the guarded row joins",
               assign_plot_toggle_series(row_sometimes),
               ASSIGN_PLOT_SERIES_ADDED);

    /* t = 0 closes the `if`; t = 1, 2 open it. */
    for (int frame = 0; frame < 3; frame++) {
        repl_state_time_set((float)frame);
        reflatten();
        assign_plot_capture((double)(frame + 1));
    }

    v = assign_plot_view();
    /* Four captures: the one that established the axis, then t = 0, 1, 2. */
    ASSERT_INT("both series have the same number of columns",
               v.series[0].col_count, v.series[1].col_count);
    ASSERT_TRUE("and there are four of them", v.series[0].col_count == 4);

    ASSERT_INT("the always-run series has no gaps",
               v.series[0].cols[0].valid && v.series[0].cols[1].valid
               && v.series[0].cols[2].valid && v.series[0].cols[3].valid, 1);
    /* Column 0 predates the add: back-filled as a gap so the two series stay
     * on the same capture index rather than the newcomer being stretched. */
    ASSERT_INT("the newcomer has a gap where it did not yet exist",
               v.series[1].cols[0].valid, 0);
    ASSERT_INT("and a gap at the frame its guard was closed",
               v.series[1].cols[1].valid, 0);
    ASSERT_INT("and values where it ran", v.series[1].cols[2].valid, 1);
    ASSERT_FLOAT("which are its own", v.series[1].cols[2].lo, 10.0);
    /* A gap contributes nothing to the statistics - it is a missing sample,
     * not a zero. */
    ASSERT_INT("the gap is not counted as a sample",
               (int)v.series[1].stats.count, 2);
    ASSERT_FLOAT("nor does it drag the minimum to zero",
                 v.series[1].stats.min, 10.0);
}

/* A secondary can be admitted while silent and later run several times. Its
 * frame column is an envelope, but its statistics must still see every value. */
static void test_frame_secondary_stats_include_every_execution(void) {
    static const char *const k_scene[] = {
        "float x;", "float y;", "x = t;", "for(i, 0, t) {", "y = i;", "}",
        NULL
    };
    int row_x, row_y;
    AssignPlotView v;

    load_scene(k_scene);
    row_x = find_nth_row(CMD_VAR_ASSIGN, 0);
    row_y = find_nth_row(CMD_VAR_ASSIGN, 1);
    assign_plot_open(row_x);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_FRAME);
    assign_plot_capture(0.0);
    ASSERT_INT("a silent secondary is admitted",
               assign_plot_toggle_series(row_y), ASSIGN_PLOT_SERIES_ADDED);

    repl_state_time_set(4.0f);
    reflatten();
    assign_plot_capture(1.0);
    v = assign_plot_view();

    ASSERT_INT("the secondary ran four times", v.series[1].exec_count, 4);
    ASSERT_INT("statistics saw all four values",
               (int)v.series[1].stats.count, 4);
    ASSERT_FLOAT("mean includes the interior values",
                 v.series[1].stats.mean, 1.5);
    ASSERT_FLOAT("sample deviation includes the interior values",
                 v.series[1].stats.stddev, sqrt(5.0 / 3.0));
}

/* Deleting one plotted row drops that series and leaves the rest alone. */
static void test_dead_row_drops_only_its_series(void) {
    static const char *const k_scene[] = {
        "float x;",
        "float y;",
        "for(i, 0, 4) {",
        "x = i;",
        "y = i;",
        "}",
        NULL
    };
    int row_x, row_y;

    load_scene(k_scene);
    row_x = find_nth_row(CMD_VAR_ASSIGN, 0);
    row_y = find_nth_row(CMD_VAR_ASSIGN, 1);

    assign_plot_open(row_x);
    assign_plot_toggle_series(row_y);
    assign_plot_capture(0.0);
    ASSERT_INT("two series", assign_plot_series_count(), 2);

    /* Overwrite the second row with something that assigns nothing. */
    editor_navigate_to_line(row_y);
    editor_feed_line("glVertex3f(1, 2, 3);");
    reflatten();
    assign_plot_capture(US_PER_SEC * 10.0);

    ASSERT_TRUE("the plot stays open", assign_plot_is_open());
    ASSERT_INT("down to one series", assign_plot_series_count(), 1);
    ASSERT_INT("and it is the surviving row", assign_plot_source_line(), row_x);
}

/* ------------------------------------------------------------------ *
 * Replay coupling: the PC marker and the live-capture override.
 * ------------------------------------------------------------------ */

/* Flat index just past the `nth` (1-based) execution of `row` - what a replay
 * clamp would be when exactly that many of the row's assignments have run. */
static int flat_limit_after_exec(int row, int nth) {
    const GLCmd *flat = repl_state_flat_program_cmds();
    int count = repl_state_flat_program_count();
    int seen = 0;
    for (int i = 0; i < count; i++) {
        if (flat[i].src_cmd_idx != row) continue;
        if (flat[i].type != CMD_VAR_ASSIGN && flat[i].type != CMD_SCRATCH_ASSIGN)
            continue;
        if (++seen == nth) return i + 1;
    }
    return count;
}

static void test_replay_progress_tracks_the_pc(void) {
    static const char *const k_scene[] = {
        "float x;",
        "for(i, 0, 8) {",
        "x = i * 2;",
        "}",
        NULL
    };
    float frac[MAX_ASSIGN_PLOT_SERIES];
    int row;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    assign_plot_open(row);
    assign_plot_capture(0.0);

    /* The marker sits on the last execution that has run, and the trace puts
     * execution i at i/(n-1) of the width: 4 of 8 done is execution 3 of 7. */
    ASSERT_INT("a marker is produced mid-program",
               assign_plot_exec_progress(flat_limit_after_exec(row, 4), frac, NULL), 1);
    ASSERT_FLOAT("marker is at the fourth of eight executions",
                 frac[0], 3.0f / 7.0f);

    ASSERT_INT("a marker is produced at the end",
               assign_plot_exec_progress(flat_limit_after_exec(row, 8), frac, NULL), 1);
    ASSERT_FLOAT("a finished pass sits at the right edge", frac[0], 1.0f);

    /* Before the row's first execution there is nothing to point at: a marker
     * pinned to the left edge would read as "the first execution has run". */
    ASSERT_INT("no marker before the row has run",
               assign_plot_exec_progress(0, frac, NULL), 0);
    ASSERT_TRUE("unrun series reports no position", frac[0] < 0.0f);

    /* No clamp is the whole frame. */
    ASSERT_INT("an unclamped program is fully executed",
               assign_plot_exec_progress(-1, frac, NULL), 1);
    ASSERT_FLOAT("unclamped sits at the right edge", frac[0], 1.0f);
}

/* The marker names a number, and that number is the one the row computed at
 * the execution the PC has reached - not a column midpoint. */
static void test_replay_progress_reports_the_value_at_the_pc(void) {
    static const char *const k_scene[] = {
        "float x;",
        "for(i, 0, 8) {",
        "x = i * 2;",
        "}",
        NULL
    };
    float frac[MAX_ASSIGN_PLOT_SERIES];
    float value[MAX_ASSIGN_PLOT_SERIES];
    int row;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    assign_plot_open(row);
    assign_plot_capture(0.0);

    /* Four of eight executions done: the last one to have run is i = 3. */
    assign_plot_exec_progress(flat_limit_after_exec(row, 4), frac, value);
    ASSERT_FLOAT("the value is the fourth execution's", value[0], 6.0f);

    /* Values past the clamp have not happened yet as far as this frame's
     * render goes, so the readout must not run ahead of the marker. */
    assign_plot_exec_progress(flat_limit_after_exec(row, 5), frac, value);
    ASSERT_FLOAT("one step on is one execution on", value[0], 8.0f);

    assign_plot_exec_progress(-1, frac, value);
    ASSERT_FLOAT("an unclamped pass reports its last value", value[0], 14.0f);

    /* No marker, no value: a series the PC has not reached has nothing to
     * read off, and a stale number under a missing rule would be worse than
     * none. */
    ASSERT_INT("no marker before the row has run",
               assign_plot_exec_progress(0, frac, value), 0);
    ASSERT_FLOAT("and no value either", value[0], 0.0f);

    /* The values are optional - callers that only place the rules pass NULL. */
    ASSERT_INT("positions alone still work",
               assign_plot_exec_progress(-1, frac, NULL), 1);
}

/* Two rows the same PC reaches at different points report their own values,
 * for the same reason they get their own rules. */
static void test_replay_pc_values_are_per_series(void) {
    static const char *const k_scene[] = {
        "float x;",
        "float y;",
        "for(i, 0, 8) {",
        "x = i * 10;",
        "}",
        "for(j, 0, 4) {",
        "y = j + 100;",
        "}",
        NULL
    };
    const GLCmd *cmds;
    float frac[MAX_ASSIGN_PLOT_SERIES];
    float value[MAX_ASSIGN_PLOT_SERIES];
    int n, first = -1, second = -1;

    load_scene(k_scene);
    cmds = repl_state_document_cmds();
    n = repl_state_document_count();
    for (int i = 0; i < n; i++) {
        if (!cmds[i].valid || cmds[i].type != CMD_VAR_ASSIGN) continue;
        if (first < 0) first = i;
        else if (second < 0) second = i;
    }
    ASSERT_TRUE("scene has two assignment rows", first >= 0 && second >= 0);

    assign_plot_open(first);
    ASSERT_INT("second row joins the plot",
               assign_plot_toggle_series(second), ASSIGN_PLOT_SERIES_ADDED);
    assign_plot_capture(0.0);

    /* Halfway through the second loop: the first has finished at i = 7, the
     * second is at j = 1. */
    assign_plot_exec_progress(flat_limit_after_exec(second, 2), frac, value);
    ASSERT_FLOAT("finished series reports its last value", value[0], 70.0f);
    ASSERT_FLOAT("running series reports its current one", value[1], 101.0f);
}

/* Each series is spread across the full width as its own execution
 * percentage, so one PC lands at a different fraction on each. */
static void test_replay_progress_is_per_series(void) {
    static const char *const k_scene[] = {
        "float x;",
        "float y;",
        "for(i, 0, 8) {",
        "x = i;",
        "}",
        "for(j, 0, 4) {",
        "y = j;",
        "}",
        NULL
    };
    const GLCmd *cmds;
    float frac[MAX_ASSIGN_PLOT_SERIES];
    int n, first = -1, second = -1;

    load_scene(k_scene);
    cmds = repl_state_document_cmds();
    n = repl_state_document_count();
    for (int i = 0; i < n; i++) {
        if (!cmds[i].valid || cmds[i].type != CMD_VAR_ASSIGN) continue;
        if (first < 0) first = i;
        else if (second < 0) second = i;
    }
    ASSERT_TRUE("scene has two assignment rows", first >= 0 && second >= 0);

    assign_plot_open(first);
    ASSERT_INT("second row joins the plot",
               assign_plot_toggle_series(second), ASSIGN_PLOT_SERIES_ADDED);
    assign_plot_capture(0.0);

    /* A PC past the whole first loop but before the second starts: the first
     * series is finished, the second has not begun. */
    ASSERT_INT("markers exist for the first loop",
               assign_plot_exec_progress(flat_limit_after_exec(first, 8), frac, NULL), 1);
    ASSERT_FLOAT("finished series is at its right edge", frac[0], 1.0f);
    ASSERT_TRUE("series that has not run has no marker", frac[1] < 0.0f);

    /* Halfway through the second loop the first is still done, so the two
     * markers sit at different fractions from the same PC. */
    assign_plot_exec_progress(flat_limit_after_exec(second, 2), frac, NULL);
    ASSERT_FLOAT("first series stays finished", frac[0], 1.0f);
    ASSERT_FLOAT("second series is at its own second of four",
                 frac[1], 1.0f / 3.0f);
}

/* X_FRAME columns are whole captures, so a position within one frame has no
 * column to point at. */
static void test_replay_progress_absent_in_frame_mode(void) {
    static const char *const k_scene[] = {
        "float x;",
        "x = t * 10;",
        NULL
    };
    float frac[MAX_ASSIGN_PLOT_SERIES];
    int row;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    assign_plot_open(row);
    assign_plot_capture(0.0);

    ASSERT_INT("capture derived the frame axis",
               assign_plot_view().x_mode, ASSIGN_PLOT_X_FRAME);
    ASSERT_INT("no marker on the captures axis",
               assign_plot_exec_progress(-1, frac, NULL), 0);
    ASSERT_TRUE("and no position for the series", frac[0] < 0.0f);
}

/* A frozen one-shot's columns are some earlier frame's, so there is no
 * this-frame position to draw on them. */
static void test_replay_progress_absent_for_a_one_shot(void) {
    static const char *const k_scene[] = {
        "float x;",
        "for(i, 0, 8) {",
        "x = i;",
        "}",
        NULL
    };
    float frac[MAX_ASSIGN_PLOT_SERIES];
    int row;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    assign_plot_open(row);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_ONCE);
    assign_plot_capture(0.0);

    ASSERT_INT("no marker over a frozen one-shot",
               assign_plot_exec_progress(-1, frac, NULL), 0);
    ASSERT_TRUE("and no position for the series", frac[0] < 0.0f);

    /* The same plot at a live rate does get one, so it is the freeze that
     * suppresses the marker and not the scene. */
    assign_plot_set_rate(ASSIGN_PLOT_RATE_FRAME);
    assign_plot_capture(0.0);
    ASSERT_INT("a live rate is marked",
               assign_plot_exec_progress(-1, frac, NULL), 1);
}

static void test_replay_progress_needs_an_open_plot(void) {
    static const char *const k_scene[] = {
        "float x;",
        "for(i, 0, 8) {",
        "x = i;",
        "}",
        NULL
    };
    float frac[MAX_ASSIGN_PLOT_SERIES];

    load_scene(k_scene);
    ASSERT_INT("a closed plot produces no markers",
               assign_plot_exec_progress(-1, frac, NULL), 0);
    ASSERT_TRUE("and leaves no stale position", frac[0] < 0.0f);
}

/* Live capture defeats the 1 Hz clock: a marker is only truthful over a trace
 * from the frame the PC is walking. */
static void test_live_capture_overrides_the_1hz_gate(void) {
    static const char *const k_scene[] = {
        "float x;",
        "for(i, 0, 8) {",
        "x = i + t;",
        "}",
        NULL
    };
    int row;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    assign_plot_open(row);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_1HZ);

    repl_state_time_set(0.0f);
    reflatten();
    assign_plot_capture(0.0);
    ASSERT_FLOAT("first capture lands", S0(assign_plot_view()).stats.max, 7.0);

    /* A tenth of a second later the gate would normally still be shut. */
    repl_state_time_set(100.0f);
    reflatten();
    assign_plot_capture(US_PER_SEC / 10.0);
    ASSERT_FLOAT("the clock still gates a normal capture",
                 S0(assign_plot_view()).cols[7].hi, 7.0);

    assign_plot_set_live_capture(1);
    assign_plot_capture(US_PER_SEC / 10.0);
    ASSERT_FLOAT("live capture takes this frame's values",
                 S0(assign_plot_view()).cols[7].hi, 107.0);

    /* And the override lifts cleanly - the gate is the clock again. */
    assign_plot_set_live_capture(0);
    repl_state_time_set(200.0f);
    reflatten();
    assign_plot_capture(US_PER_SEC / 10.0 + 1.0);
    ASSERT_FLOAT("the clock gates again once replay stops",
                 S0(assign_plot_view()).cols[7].hi, 107.0);
}

/* A one-shot is a snapshot the user asked to freeze; replay does not get to
 * overwrite it. */
static void test_live_capture_does_not_thaw_a_one_shot(void) {
    static const char *const k_scene[] = {
        "float x;",
        "for(i, 0, 8) {",
        "x = i + t;",
        "}",
        NULL
    };
    int row;

    load_scene(k_scene);
    row = find_row(CMD_VAR_ASSIGN);
    assign_plot_open(row);
    assign_plot_set_rate(ASSIGN_PLOT_RATE_ONCE);

    repl_state_time_set(0.0f);
    reflatten();
    assign_plot_capture(0.0);
    ASSERT_FLOAT("the shot lands", S0(assign_plot_view()).cols[7].hi, 7.0);

    assign_plot_set_live_capture(1);
    for (int frame = 1; frame < 4; frame++) {
        repl_state_time_set((float)frame * 100.0f);
        reflatten();
        assign_plot_capture((double)frame * US_PER_SEC);
    }
    ASSERT_FLOAT("live capture leaves the frozen shot alone",
                 S0(assign_plot_view()).cols[7].hi, 7.0);
    ASSERT_INT("and does not add samples",
               (int)S0(assign_plot_view()).stats.count, 8);
}

int main(void) {
    printf("--- assign_plot tests ---\n\n");
    /* These cases drive the real REPL pipeline, so they need the real host.
     * The controller installs it at startup; nothing here reaches that path. */
    glr_assign_plot_install_host();
    test_exec_mode_captures_loop_values();
    test_frame_mode_accumulates_across_captures();
    test_frame_mode_ring_scrolls();
    test_decimation_preserves_extremes_and_stats();
    test_rate_once_captures_exactly_once();
    test_rate_1hz_gates_on_the_clock();
    test_rate_change_resets_the_window();
    test_cycle_rate_wraps_both_ways();
    test_mode_flip_clears_the_window();
    test_scratch_assign_is_plotted();
    test_negative_values_are_measured_exactly();
    test_toggle_and_close();
    test_capture_is_inert_when_closed();
    test_target_drift_closes_the_plot();
    test_out_of_range_target_closes();
    test_zero_executions_preserve_history();
    test_reset_keeps_target_and_rate();
    test_second_series_captures_independently();
    test_top_level_series_can_join_before_first_capture();
    test_once_rate_rearms_for_series_added_later();
    test_once_snapshot_is_one_frame();
    test_once_waits_for_a_complete_frame();
    test_once_single_series_waits_to_run();
    test_toggle_series_removes_and_closes();
    test_series_cap_is_enforced();
    test_incompatible_x_mode_is_refused();
    test_frame_mode_keeps_series_aligned();
    test_frame_secondary_stats_include_every_execution();
    test_dead_row_drops_only_its_series();
    test_replay_progress_tracks_the_pc();
    test_replay_progress_reports_the_value_at_the_pc();
    test_replay_pc_values_are_per_series();
    test_replay_progress_is_per_series();
    test_replay_progress_absent_in_frame_mode();
    test_replay_progress_absent_for_a_one_shot();
    test_replay_progress_needs_an_open_plot();
    test_live_capture_overrides_the_1hz_gate();
    test_live_capture_does_not_thaw_a_one_shot();
    return test_harness_report(&g_harness, "test_assign_plot");
}
