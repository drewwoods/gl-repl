/*
 * variable_panel_drag.c -- Variable slider drag-state implementation
 *                    (peer-owned by variable_panel).
 *
 * Status: implementation-behind-variable_panel.
 *
 * Phase F commit 31 moved the drag-state bytes into the variable_panel
 * peer module (variable_panel.c). This file is now the implementation
 * behind the peer's variable_panel_handle_drag_* surface — it reads /
 * writes via variable_panel_drag_mut() and supplies the value-mapping
 * logic (linear / log scaling, source-line rewrite).
 *
 * External callers must use variable_panel_handle_* / variable_panel_drag_*.
 * The legacy repl_var_drag_* names are preserved so the existing
 * imrepl_ctrl-routed callers keep compiling, and the drag value-writeback
 * still reaches REPL state directly (g_predef_vars, document_cmds_mut,
 * editor_buffer_set_line, mark_flat_dirty). That cross-subsystem
 * writeback is the remaining transitional debt: it should move into
 * an explicit "set predef variable" peer/editor API in a follow-up
 * commit so this file can rename to variable_panel_drag.c without
 * dragging REPL writes with it.
 *
 * Linear drag: 1 pixel = 0.05 units.
 * Log drag:    200 pixels = one decade (x10 / ÷10), sign preserved.
 *              Near-zero start value falls back to a linear bootstrap
 *              so the slider can walk off zero.
 */
#include "sample.h"
#include "repl_state.h"
#include "variable_panel_drag.h"
#include "variable_panel.h"

int repl_var_drag_active(void) {
    return variable_panel_drag().var_idx >= 0;
}

int repl_var_drag_active_var(void) {
    return variable_panel_drag().var_idx;
}

int repl_var_drag_log_mode(void) {
    return variable_panel_drag().log_mode;
}

void repl_var_drag_begin(int row, int log_mode, int x) {
    if (row < 0 || row >= g_num_predef_vars) return;
    ReplVariableDragState *drag = variable_panel_drag_mut();
    drag->var_idx = row;
    drag->log_mode = log_mode ? 1 : 0;
    drag->start_value = g_predef_vars[row].value;
    drag->start_x = x;
}

void repl_var_drag_reset(void) {
    ReplVariableDragState *drag = variable_panel_drag_mut();
    drag->var_idx = -1;
    drag->log_mode = 0;
    drag->start_value = 0.0f;
    drag->start_x = 0;
}

void repl_var_drag_motion(int x) {
    ReplVariableDragState *drag = variable_panel_drag_mut();
    if (drag->var_idx < 0) return;

    float new_val;
    if (drag->log_mode) {
        /* Logarithmic drag: ×10 / ÷10 per 200 pixels.
         * Preserves sign; near-zero start falls back to linear bootstrap. */
        float dx_total = (float)(x - drag->start_x);
        float mag = fabsf(drag->start_value);
        if (mag < 1e-6f) {
            /* Bootstrap from zero: treat first pixels as linear, then log. */
            new_val = dx_total * 0.001f;
        } else {
            float sign = (drag->start_value >= 0.0f) ? 1.0f : -1.0f;
            new_val = sign * mag * expf(dx_total * (logf(10.0f) / 200.0f));
        }
    } else {
        float delta = (float)(x - drag->start_x) * 0.05f;
        new_val = drag->start_value + delta;
    }

    g_predef_vars[drag->var_idx].value = new_val;

    /* Sync any literal CMD_VAR_ASSIGN for this var so the source line
     * on screen matches the new value (constant assignments only -
     * expressions that reference other vars are left alone). */
    const char *vname = g_predef_vars[drag->var_idx].name;
    for (int i = 0; i < repl_state_document_count(); i++) {
        if (repl_state_document_cmds_mut()[i].valid && repl_state_document_cmds_mut()[i].type == CMD_VAR_ASSIGN &&
            repl_state_document_cmds_mut()[i].num_args == drag->var_idx &&
            !repl_state_document_cmds_mut()[i].has_vars) {
            char new_line[MAX_LINE_LEN];
            repl_state_document_cmds_mut()[i].args[0] = new_val;
            snprintf(new_line, sizeof(new_line), "  %s = %g;", vname, (double)new_val);
            editor_buffer_set_line(i, new_line);
        }
    }
    repl_state_mark_flat_dirty();
}
