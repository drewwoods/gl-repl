/*
 * glr_assign_plot_bridge.c - REPL-backed AssignPlotHostBridge.
 *
 * The assignment plot asks two things of the program it is plotting - what did
 * this frame execute, and is a given document row still an assignment - and
 * this is where those are answered against the flat program and the document.
 * Everything the peer used to read out of src/repl (GLCmd, CMD_VAR_ASSIGN,
 * CMD_SCRATCH_ASSIGN, the arg slot each value lives in) is now known only
 * here, which is what keeps src/subsystems/assign_plot linkable on its own.
 *
 * The trace is the flat program itself, unfiltered: assign_plot_exec_progress
 * is handed a replay exec limit measured in flat indices, so the indices this
 * bridge is addressed by have to be those same ones. trace_at() reports a
 * non-assignment by returning 0 rather than by not being there.
 */
#include "app/glr_assign_plot_bridge.h"

#include <stdio.h>

#include "editor/state.h"       /* editor_buffer_line - the row's text */
#include "editor/undo.h"        /* editor_undo_generation */
#include "repl/command.h"       /* GLCmd, CMD_VAR_ASSIGN, CMD_SCRATCH_ASSIGN */
#include "repl/eval.h"          /* repl_eval_line_has_plot_tag */
#include "repl/state_views.h"   /* flat-program + document reads */
#include "subsystems/assign_plot/assign_plot.h"  /* AssignPlotHostBridge */
#include "ui/app/state.h"       /* ui_state_status_set */

/* Which document rows the plot can be pointed at. This is the single gate:
 * right-click targeting, the `// @plot` tag sync, and the per-capture drift
 * pruning all reach the document through row_is_plottable() below, so a type
 * left out of here is unplottable everywhere at once.
 *
 * CMD_SCRATCH_BLOCK_ASSIGN (`A[base] = {e0, ..., eN-1};`) is deliberately
 * absent. A series is identified by its document row and nothing else -
 * AssignPlotSample is {source_line_idx, value} - and a block row produces N
 * values per execution, all carrying that one row index. Adding the type
 * without widening the sample identity would not plot N series; it would
 * interleave N unrelated cells into one trace and describe them as a single
 * row's history, which is wrong data rather than a missing feature. Leaving
 * it out costs nothing: the row simply refuses to be targeted, and a `@plot`
 * tag on it is ignored.
 *
 * Supporting it properly means a sub-index on both AssignPlotSample and the
 * series target key, inside a peer that deliberately links no src/repl (see
 * assign_plot_demo). It is also not obviously wanted: MAX_ASSIGN_PLOT_SERIES
 * is 4, so a 4-cell row would consume the whole plot and could never be
 * overlaid against another row - which is what the plot is for. Plot the
 * cells you care about as `A[k] = expr;` rows instead. */
static int glr_ap_is_assign(CmdType type) {
    return type == CMD_VAR_ASSIGN || type == CMD_SCRATCH_ASSIGN;
}

/* The value an assignment produced, by command type. Flatten bakes the
 * evaluated RHS in; nothing here re-evaluates anything. */
static int glr_ap_cmd_value(const GLCmd *cmd, float *out_value) {
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

static int glr_ap_trace_len(void) {
    return repl_state_flat_program_cmds() ? repl_state_flat_program_count() : 0;
}

static int glr_ap_trace_at(int idx, AssignPlotSample *out) {
    const GLCmd *flat = repl_state_flat_program_cmds();
    if (!flat || idx < 0 || idx >= repl_state_flat_program_count())
        return 0;
    if (!glr_ap_cmd_value(&flat[idx], &out->value))
        return 0;
    out->source_line_idx = flat[idx].src_cmd_idx;
    return 1;
}

static int glr_ap_row_is_plottable(int source_line_idx) {
    const GLCmd *cmd;
    if (source_line_idx < 0 || source_line_idx >= repl_state_document_count())
        return 0;
    cmd = repl_state_document_cmd_at(source_line_idx);
    return cmd && cmd->valid && glr_ap_is_assign(cmd->type);
}

static const AssignPlotHostBridge g_glr_ap_host = {
    glr_ap_trace_len,
    glr_ap_trace_at,
    glr_ap_row_is_plottable,
};

void glr_assign_plot_install_host(void) {
    assign_plot_install_host(&g_glr_ap_host);
}

/* --- `// @plot` tag sync -------------------------------------------------
 *
 * A scene can open the plot on itself: an assignment row whose trailing
 * comment carries `@plot` becomes a series when the document is loaded. The
 * tag rides the row's canonical text (repl_append_trailing_comment in the
 * parser), so it survives commit, reformat, export to C and re-import with no
 * index bookkeeping anywhere - which is exactly why this is a comment tag and
 * not a config slug. A row index in an `@cfg` header would be applied before
 * the document it indexes has been fed, and would rot on the first edit.
 *
 * Keyed on the undo generation, which bumps only on *wholesale* document
 * replacement (example / scene / workspace / file load, reset). Ordinary
 * edits leave it alone, so a manual right-click retarget is never stomped by
 * the tags in the file. The flip side is the rule for a replacement: row
 * identity does not survive one, so the tags decide the whole series set -
 * no tags means the plot closes rather than keeping indices that now address
 * someone else's rows. */
static unsigned int g_glr_ap_synced_gen;
static int          g_glr_ap_have_synced;

void glr_assign_plot_sync_tags(void) {
    unsigned int gen = editor_undo_generation();
    int rows[MAX_ASSIGN_PLOT_SERIES];
    int found = 0, refused = 0, doc_count, i;

    if (g_glr_ap_have_synced && gen == g_glr_ap_synced_gen)
        return;
    g_glr_ap_have_synced = 1;
    g_glr_ap_synced_gen  = gen;

    doc_count = repl_state_document_count();
    for (i = 0; i < doc_count && found < MAX_ASSIGN_PLOT_SERIES; i++) {
        const char *text;
        if (!glr_ap_row_is_plottable(i))
            continue;
        text = editor_buffer_line(i);
        if (text && repl_eval_line_has_plot_tag(text))
            rows[found++] = i;
    }

    if (found == 0) {
        assign_plot_close();
        return;
    }

    /* First tagged row is the primary: it fixes the shared X axis, so the
     * order the series are added in is the document's order. */
    assign_plot_open(rows[0]);
    for (i = 1; i < found; i++) {
        if (assign_plot_toggle_series(rows[i]) == ASSIGN_PLOT_SERIES_INCOMPATIBLE)
            refused++;
    }

    /* A refusal here has no click to report against, and a silently-missing
     * series looks like a broken scene rather than an incompatible pair of
     * rows (a once-per-frame row and one inside a loop have no common X). */
    if (refused > 0) {
        char msg[REPL_STATUS_TEXT_MAX];
        snprintf(msg, sizeof(msg),
                 "assignment plot: %d @plot row%s skipped (x axis differs)",
                 refused, refused == 1 ? "" : "s");
        ui_state_status_set(msg);
    }
}
