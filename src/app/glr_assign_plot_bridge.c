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

#include "repl/command.h"       /* GLCmd, CMD_VAR_ASSIGN, CMD_SCRATCH_ASSIGN */
#include "repl/state_views.h"   /* flat-program + document reads */
#include "subsystems/assign_plot/assign_plot.h"  /* AssignPlotHostBridge */

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
