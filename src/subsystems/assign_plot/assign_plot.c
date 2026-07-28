/*
 * assign_plot.c - Value capture for the assignment plot.
 */
#include "subsystems/assign_plot/assign_plot.h"

#include <string.h>

#include "repl/command.h"
#include "repl/state_views.h"

/* One second between captures at ASSIGN_PLOT_RATE_1HZ. */
#define ASSIGN_PLOT_1HZ_PERIOD_US 1000000.0

static int    g_open = 0;
static int    g_source_line_idx = -1;
static int    g_rate = ASSIGN_PLOT_RATE_1HZ;
static int    g_x_mode = ASSIGN_PLOT_X_EXEC;
static int    g_captured = 0;
static double g_last_capture_us = 0.0;
static int    g_exec_count = 0;

/* Columns are kept in display order (oldest first) rather than as a ring, so
 * the view can hand the renderer a plain array. In X_FRAME mode a full buffer
 * shifts down by one on append: 1.5 KB of memmove at most once per frame, which
 * is far cheaper than making every reader understand ring wrap. */
static AssignPlotColumn g_cols[ASSIGN_PLOT_COLS];
static int              g_col_count = 0;

static RunStats g_stats;

static void assign_plot_clear_samples(void) {
    memset(g_cols, 0, sizeof(g_cols));
    g_col_count = 0;
    g_exec_count = 0;
    g_captured = 0;
    g_last_capture_us = 0.0;
    runstats_clear(&g_stats);
}

/* The value an assignment produced, by command type. Flatten bakes the
 * evaluated RHS in; nothing here re-evaluates anything. */
static int assign_plot_cmd_value(const GLCmd *cmd, float *out_value) {
    if (cmd->type == CMD_VAR_ASSIGN) {
        *out_value = cmd->args[0];
        return 1;
    }
    if (cmd->type == CMD_SCRATCH_ASSIGN) {
        /* args[0] = array, args[1] = element, args[2] = value. */
        *out_value = cmd->args[2];
        return 1;
    }
    return 0;
}

static int assign_plot_type_is_assign(CmdType type) {
    return type == CMD_VAR_ASSIGN || type == CMD_SCRATCH_ASSIGN;
}

/* The targeted row must still exist and still be an assignment. It does not
 * have to be the *same* assignment: if the row is edited the plot follows what
 * is now there, and the panel title (rebuilt by the controller from the live
 * row each frame) says so. Anything else — the row deleted, or replaced by a
 * command that assigns nothing — closes the plot rather than silently plotting
 * a neighbour. */
static int assign_plot_target_is_live(void) {
    const GLCmd *cmd;
    if (g_source_line_idx < 0 || g_source_line_idx >= repl_state_document_count())
        return 0;
    cmd = repl_state_document_cmd_at(g_source_line_idx);
    return cmd && cmd->valid && assign_plot_type_is_assign(cmd->type);
}

static int assign_plot_rate_allows(double now_us) {
    switch (g_rate) {
        case ASSIGN_PLOT_RATE_ONCE:
            return !g_captured;
        case ASSIGN_PLOT_RATE_1HZ:
            return !g_captured
                || (now_us - g_last_capture_us) >= ASSIGN_PLOT_1HZ_PERIOD_US;
        case ASSIGN_PLOT_RATE_FRAME:
        default:
            return 1;
    }
}

/* Executions of the targeted row in the current flat program. The full flat
 * count on purpose — see the header on why replay's clamp is not applied. */
static int assign_plot_count_executions(void) {
    const GLCmd *flat = repl_state_flat_program_cmds();
    int n = repl_state_flat_program_count();
    int found = 0;
    if (!flat) return 0;
    for (int i = 0; i < n; i++) {
        if (flat[i].src_cmd_idx == g_source_line_idx
            && assign_plot_type_is_assign(flat[i].type))
            found++;
    }
    return found;
}

/* Second pass: fold this frame's executions into `cols` columns, and every
 * value into the statistics. `total` comes from assign_plot_count_executions()
 * so the column a value belongs to can be computed without buffering the
 * values first.
 *
 * With cols <= total the column index is non-decreasing in `seen` and advances
 * by at most one per value, so columns fill strictly left to right with no
 * gaps — which is what lets `col != prev_col` stand in for "first value in
 * this column" and why no per-column seen flag is needed. */
static void assign_plot_fill_exec_columns(int total, int cols) {
    const GLCmd *flat = repl_state_flat_program_cmds();
    int n = repl_state_flat_program_count();
    int seen = 0;
    int prev_col = -1;

    memset(g_cols, 0, sizeof(g_cols));

    for (int i = 0; i < n && seen < total; i++) {
        float value;
        int col;
        if (flat[i].src_cmd_idx != g_source_line_idx) continue;
        if (!assign_plot_cmd_value(&flat[i], &value)) continue;

        /* 64-bit product: `total` can reach MAX_FLAT_COMMANDS today, which
         * overflows nothing, but the widening keeps this correct if either
         * cap grows. */
        col = (int)(((long long)seen * (long long)cols) / (long long)total);
        if (col < 0) col = 0;
        if (col > cols - 1) col = cols - 1;

        if (col != prev_col) {
            g_cols[col].lo = value;
            g_cols[col].hi = value;
            prev_col = col;
        } else {
            if (value < g_cols[col].lo) g_cols[col].lo = value;
            if (value > g_cols[col].hi) g_cols[col].hi = value;
        }
        runstats_record(&g_stats, (double)value);
        seen++;
    }
    g_col_count = cols;
}

/* X_FRAME append: one column per capture, scrolling once the buffer is full. */
static void assign_plot_append_frame_column(float value) {
    if (g_col_count >= ASSIGN_PLOT_COLS) {
        memmove(&g_cols[0], &g_cols[1],
                (size_t)(ASSIGN_PLOT_COLS - 1) * sizeof(g_cols[0]));
        g_col_count = ASSIGN_PLOT_COLS - 1;
    }
    g_cols[g_col_count].lo = value;
    g_cols[g_col_count].hi = value;
    g_col_count++;
    runstats_record(&g_stats, (double)value);
}

/* The single value of a row that executed exactly once this frame. */
static int assign_plot_single_value(float *out_value) {
    const GLCmd *flat = repl_state_flat_program_cmds();
    int n = repl_state_flat_program_count();
    for (int i = 0; i < n; i++) {
        if (flat[i].src_cmd_idx == g_source_line_idx
            && assign_plot_cmd_value(&flat[i], out_value))
            return 1;
    }
    return 0;
}

void assign_plot_open(int source_line_idx) {
    if (source_line_idx < 0) return;
    if (g_open && g_source_line_idx == source_line_idx) return;
    g_open = 1;
    g_source_line_idx = source_line_idx;
    g_x_mode = ASSIGN_PLOT_X_EXEC;
    assign_plot_clear_samples();
}

void assign_plot_close(void) {
    g_open = 0;
    g_source_line_idx = -1;
    assign_plot_clear_samples();
}

void assign_plot_toggle(int source_line_idx) {
    if (g_open && g_source_line_idx == source_line_idx) {
        assign_plot_close();
        return;
    }
    assign_plot_open(source_line_idx);
}

int assign_plot_is_open(void) {
    return g_open;
}

int assign_plot_source_line(void) {
    return g_open ? g_source_line_idx : -1;
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

void assign_plot_reset(void) {
    assign_plot_clear_samples();
}

void assign_plot_capture(double now_us) {
    int total, cols;

    /* The gate that makes this feature free when nobody is looking: no flat
     * scan, no clock arithmetic, nothing. */
    if (!g_open) return;

    if (!assign_plot_target_is_live()) {
        assign_plot_close();
        return;
    }
    if (!assign_plot_rate_allows(now_us)) return;

    total = assign_plot_count_executions();

    if (total >= 2) {
        if (g_x_mode != ASSIGN_PLOT_X_EXEC) {
            g_x_mode = ASSIGN_PLOT_X_EXEC;
            assign_plot_clear_samples();
        }
        cols = (total < ASSIGN_PLOT_COLS) ? total : ASSIGN_PLOT_COLS;
        assign_plot_fill_exec_columns(total, cols);
    } else if (total == 1) {
        float value = 0.0f;
        if (g_x_mode != ASSIGN_PLOT_X_FRAME) {
            g_x_mode = ASSIGN_PLOT_X_FRAME;
            assign_plot_clear_samples();
        }
        if (assign_plot_single_value(&value))
            assign_plot_append_frame_column(value);
    }
    /* total == 0: the row did not run this frame (a false `if`, a `goto` that
     * jumped over it). Leave the axis mode and the buffer alone — a row that
     * runs intermittently should keep the history it has, and flipping the
     * axis on an empty frame would throw it away. */

    g_exec_count = total;
    g_captured = 1;
    g_last_capture_us = now_us;
}

AssignPlotView assign_plot_view(void) {
    AssignPlotView v;
    memset(&v, 0, sizeof(v));
    v.open            = g_open;
    v.source_line_idx = g_open ? g_source_line_idx : -1;
    v.rate            = g_rate;
    v.x_mode          = g_x_mode;
    v.captured        = g_captured;
    v.exec_count      = g_exec_count;
    v.cols            = g_cols;
    v.col_count       = g_col_count;
    runstats_read(&g_stats, &v.stats);
    return v;
}

void assign_plot_reset_all(void) {
    g_open = 0;
    g_source_line_idx = -1;
    g_rate = ASSIGN_PLOT_RATE_1HZ;
    g_x_mode = ASSIGN_PLOT_X_EXEC;
    assign_plot_clear_samples();
}
