/*
 * tests/test_assign_plot.c -- assignment-value capture.
 *
 * Covers the capture side only (the panel renderer is tests/test_ui_assign_plot.c):
 * the rate gate, the two X-axis modes and the flip between them, envelope
 * decimation, the ring scroll, target drift, and the invariant that the
 * statistics see every value even when the plot does not.
 *
 * The clock is a plain parameter to assign_plot_capture(), so every timing
 * case here is exact — there is no fake-clock hook to install.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

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
    ASSERT_INT("one execution per loop iteration", v.exec_count, 8);
    ASSERT_INT("one column per execution", v.col_count, 8);
    ASSERT_INT("every value reached the statistics", (int)v.stats.count, 8);
    /* i * 2 over i in [0, 8) */
    ASSERT_FLOAT("min is the first iteration", v.stats.min, 0.0);
    ASSERT_FLOAT("max is the last iteration", v.stats.max, 14.0);
    ASSERT_FLOAT("mean of 0,2,..,14", v.stats.mean, 7.0);
    ASSERT_FLOAT("first column holds the first value", v.cols[0].lo, 0.0);
    ASSERT_FLOAT("last column holds the last value", v.cols[7].hi, 14.0);
    ASSERT_TRUE("undecimated columns collapse to a point",
                v.cols[3].lo == v.cols[3].hi);
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
    ASSERT_INT("one execution per frame", v.exec_count, 1);
    ASSERT_INT("one column per capture", v.col_count, 5);
    ASSERT_INT("one sample per capture", (int)v.stats.count, 5);
    ASSERT_FLOAT("oldest capture is t=0", v.cols[0].lo, 0.0);
    ASSERT_FLOAT("newest capture is t=4", v.cols[4].hi, 40.0);
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
               v.col_count, ASSIGN_PLOT_COLS);
    ASSERT_INT("statistics keep counting past the cap",
               (int)v.stats.count, overflow);
    ASSERT_FLOAT("newest capture is at the right edge",
                 v.cols[ASSIGN_PLOT_COLS - 1].hi, (float)(overflow - 1));
    ASSERT_FLOAT("oldest surviving capture leads the buffer",
                 v.cols[0].lo, (float)(overflow - ASSIGN_PLOT_COLS));
    ASSERT_FLOAT("statistics still remember the dropped minimum",
                 v.stats.min, 0.0);
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

    ASSERT_INT("all 500 executions were found", v.exec_count, 500);
    ASSERT_INT("columns are capped", v.col_count, ASSIGN_PLOT_COLS);
    ASSERT_INT("statistics saw every execution", (int)v.stats.count, 500);

    for (int i = 0; i < 500; i++) expected_mean += (double)i;
    expected_mean /= 500.0;
    ASSERT_FLOAT("mean is over all 500 values", v.stats.mean, expected_mean);
    ASSERT_FLOAT("min survives decimation", v.stats.min, 0.0);
    ASSERT_FLOAT("max survives decimation", v.stats.max, 499.0);

    /* Columns are envelopes over a monotonic ramp, so they must be ordered
     * and must together span the whole range. */
    ASSERT_FLOAT("first column starts at the first value", v.cols[0].lo, 0.0);
    ASSERT_FLOAT("last column ends at the last value",
                 v.cols[ASSIGN_PLOT_COLS - 1].hi, 499.0);
    {
        int ordered = 1;
        int has_span = 0;
        for (int c = 0; c < v.col_count; c++) {
            if (v.cols[c].hi < v.cols[c].lo) ordered = 0;
            if (c > 0 && v.cols[c].lo < v.cols[c - 1].hi) ordered = 0;
            if (v.cols[c].hi > v.cols[c].lo) has_span = 1;
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
               (int)assign_plot_view().stats.count, 1);
    ASSERT_FLOAT("and it is the first frame's value",
                 assign_plot_view().stats.max, 0.0);
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
               (int)assign_plot_view().stats.count, 3);

    /* One frame short of the next second is still gated out. */
    assign_plot_reset();
    for (int frame = 0; frame < 60; frame++)
        assign_plot_capture((double)frame * (US_PER_SEC / 60.0));
    ASSERT_INT("a sub-second burst captures only once",
               (int)assign_plot_view().stats.count, 1);
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
    ASSERT_INT("four frames captured", (int)assign_plot_view().stats.count, 4);

    assign_plot_set_rate(ASSIGN_PLOT_RATE_FRAME);
    ASSERT_INT("re-selecting a rate clears the window",
               (int)assign_plot_view().stats.count, 0);
    ASSERT_INT("and the plot buffer with it",
               assign_plot_view().col_count, 0);
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
               (int)assign_plot_view().stats.count, 3);
    ASSERT_INT("frame axis", assign_plot_view().x_mode, ASSIGN_PLOT_X_FRAME);

    /* ...then retarget the loop row, which is exec-mode. */
    assign_plot_open(loop_row);
    assign_plot_capture(100.0);
    ASSERT_INT("retargeting switched the axis",
               assign_plot_view().x_mode, ASSIGN_PLOT_X_EXEC);
    ASSERT_INT("and the window is only the new row's executions",
               (int)assign_plot_view().stats.count, 6);
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
    ASSERT_INT("four scratch writes", v.exec_count, 4);
    ASSERT_FLOAT("values come from args[2]", v.stats.max, 9.0);
    ASSERT_FLOAT("and start at zero", v.stats.min, 0.0);
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

    ASSERT_FLOAT("min is negative", v.stats.min, -2.0);
    ASSERT_FLOAT("max is positive", v.stats.max, 2.0);
    ASSERT_FLOAT("mean straddles zero", v.stats.mean, 0.0);
    /* -2,-1,0,1,2 -> sample variance 10/4 = 2.5 */
    ASSERT_FLOAT("sample stddev uses the n-1 divisor",
                 v.stats.stddev, sqrt(2.5));
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
    ASSERT_INT("and no samples", (int)assign_plot_view().stats.count, 0);
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
               (int)assign_plot_view().stats.count, 3);

    /* Close the gate: at t == 0 the `if` is false and the row never runs. */
    repl_state_time_set(0.0f);
    reflatten();
    assign_plot_capture(10.0);

    ASSERT_TRUE("plot stays open", assign_plot_is_open());
    ASSERT_INT("no executions this frame",
               assign_plot_view().exec_count, 0);
    ASSERT_INT("history is untouched",
               (int)assign_plot_view().stats.count, 3);
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
    ASSERT_INT("captured", (int)assign_plot_view().stats.count, 4);

    assign_plot_reset();
    ASSERT_TRUE("still open", assign_plot_is_open());
    ASSERT_INT("still on the same row", assign_plot_source_line(), row);
    ASSERT_INT("rate is kept",
               assign_plot_view().rate, ASSIGN_PLOT_RATE_FRAME);
    ASSERT_INT("samples are dropped",
               (int)assign_plot_view().stats.count, 0);
}

int main(void) {
    printf("--- assign_plot tests ---\n\n");
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
    return test_harness_report(&g_harness, "test_assign_plot");
}
