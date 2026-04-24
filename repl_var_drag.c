/*
 * repl_var_drag.c -- Variable slider drag state and value writeback.
 *
 * The drag transaction still lives here, but the mutable state now
 * lives in repl_state.c and is accessed through repl_state.h.
 *
 * Linear drag: 1 pixel = 0.05 units.
 * Log drag:    200 pixels = one decade (x10 / ÷10), sign preserved.
 *              Near-zero start value falls back to a linear bootstrap
 *              so the slider can walk off zero.
 */
#include "sample.h"
#include "repl_state.h"
#include "repl_var_drag.h"
int repl_var_drag_active(void) {
    return *repl_state_variable_drag()->var_idx >= 0;
}

int repl_var_drag_active_var(void) {
    return *repl_state_variable_drag()->var_idx;
}

int repl_var_drag_log_mode(void) {
    return *repl_state_variable_drag()->log_mode;
}

void repl_var_drag_begin(int row, int log_mode, int x) {
    if (row < 0 || row >= g_num_predef_vars) return;
    ReplVariableDragState *drag = repl_state_variable_drag_mut();
    *drag->var_idx = row;
    *drag->log_mode = log_mode ? 1 : 0;
    *drag->start_value = g_predef_vars[row].value;
    *drag->start_x = x;
}

void repl_var_drag_reset(void) {
    ReplVariableDragState *drag = repl_state_variable_drag_mut();
    *drag->var_idx = -1;
    *drag->log_mode = 0;
    *drag->start_value = 0.0f;
    *drag->start_x = 0;
}

void repl_var_drag_motion(int x) {
    ReplVariableDragState *drag = repl_state_variable_drag_mut();
    if (*drag->var_idx < 0) return;

    float new_val;
    if (*drag->log_mode) {
        /* Logarithmic drag: ×10 / ÷10 per 200 pixels.
         * Preserves sign; near-zero start falls back to linear bootstrap. */
        float dx_total = (float)(x - *drag->start_x);
        float mag = fabsf(*drag->start_value);
        if (mag < 1e-6f) {
            /* Bootstrap from zero: treat first pixels as linear, then log. */
            new_val = dx_total * 0.001f;
        } else {
            float sign = (*drag->start_value >= 0.0f) ? 1.0f : -1.0f;
            new_val = sign * mag * expf(dx_total * (logf(10.0f) / 200.0f));
        }
    } else {
        float delta = (float)(x - *drag->start_x) * 0.05f;
        new_val = *drag->start_value + delta;
    }

    g_predef_vars[*drag->var_idx].value = new_val;

    /* Sync any literal CMD_VAR_ASSIGN for this var so the source line
     * on screen matches the new value (constant assignments only -
     * expressions that reference other vars are left alone). */
    const char *vname = g_predef_vars[*drag->var_idx].name;
    for (int i = 0; i < repl_state_document_count(); i++) {
        if (repl_state_document_cmds_mut()[i].valid && repl_state_document_cmds_mut()[i].type == CMD_VAR_ASSIGN &&
            repl_state_document_cmds_mut()[i].num_args == *drag->var_idx &&
            !repl_state_document_cmds_mut()[i].has_vars) {
            repl_state_document_cmds_mut()[i].args[0] = new_val;
            snprintf(repl_state_document_cmds_mut()[i].source, sizeof(repl_state_document_cmds_mut()[i].source),
                     "  %s = %g;", vname, (double)new_val);
        }
    }
    repl_state_mark_flat_dirty();
}
