/*
 * variable_panel_drag.c -- Variable slider drag-state implementation
 *                    (peer-owned by variable_panel).
 *
 * Implements the variable_panel_handle_drag_* surface: reads / writes
 * peer-owned drag state via variable_panel_drag_mut() and maps pointer
 * motion into requested value changes without mutating REPL/editor
 * state directly.
 *
 * Linear drag: 1 pixel = 0.05 units.
 * Log drag:    200 pixels = one decade (x10 / ÷10), sign preserved.
 *              Near-zero start value falls back to a linear bootstrap
 *              so the slider can walk off zero.
 */
#include <math.h>
#include <stdio.h>
#include "repl/eval.h"
#include "subsystems/variable_panel/variable_panel_drag.h"
#include "subsystems/variable_panel/variable_panel_state.h"

/* Drag-scaling tunables (the prose spec in the file header above is the
 * authority — keep them in sync). VAR_DRAG_ZERO_EPS is the |start|
 * threshold below which log drag falls back to the linear bootstrap. */
#define VAR_DRAG_LINEAR_UNITS_PER_PX  0.05f   /* 1 px = 0.05 units */
#define VAR_DRAG_PX_PER_DECADE        200.0f  /* 200 px = x10 / div10 */
#define VAR_DRAG_ZERO_EPS             1e-6f
#define VAR_DRAG_ZERO_BOOTSTRAP_RATE  0.001f  /* units/px while |start| ~ 0 */

int variable_panel_drag_active(void) {
    return variable_panel_drag().var_idx >= 0;
}

int variable_panel_drag_active_var(void) {
    return variable_panel_drag().var_idx;
}

int variable_panel_drag_log_mode(void) {
    return variable_panel_drag().log_mode;
}

void variable_panel_handle_drag_begin(int row, int log_mode, int x) {
    if (row < 0 || row >= g_num_predef_vars) return;
    VariablePanelDragState *drag = variable_panel_drag_mut();
    drag->var_idx = row;
    drag->log_mode = log_mode ? 1 : 0;
    drag->start_value = g_predef_vars[row].value;
    drag->start_x = x;
    snprintf(drag->name, sizeof(drag->name), "%s", g_predef_vars[row].name);
    drag->undo_snapshot_pushed = 0;
}

void variable_panel_handle_drag_reset(void) {
    VariablePanelDragState *drag = variable_panel_drag_mut();
    drag->var_idx = -1;
    drag->log_mode = 0;
    drag->start_value = 0.0f;
    drag->start_x = 0;
    drag->name[0] = '\0';
    drag->undo_snapshot_pushed = 0;
}

int variable_panel_handle_drag_motion(int x, VariablePanelValueChange *out) {
    VariablePanelDragState *drag = variable_panel_drag_mut();
    float new_val;

    if (out) {
        out->name[0] = '\0';
        out->value = 0.0f;
    }
    if (drag->var_idx < 0)
        return 0;

    if (drag->log_mode) {
        /* Logarithmic drag: ×10 / ÷10 per 200 pixels.
         * Preserves sign; near-zero start falls back to a linear
         * bootstrap because the exponential path needs a non-zero
         * magnitude to scale from. */
        float dx_total = (float)(x - drag->start_x);
        float mag = fabsf(drag->start_value);
        if (mag < VAR_DRAG_ZERO_EPS) {
            /* Bootstrap from zero: treat first pixels as linear, then log. */
            new_val = dx_total * VAR_DRAG_ZERO_BOOTSTRAP_RATE;
        } else {
            float sign = (drag->start_value >= 0.0f) ? 1.0f : -1.0f;
            new_val = sign * mag *
                      expf(dx_total * (logf(10.0f) / VAR_DRAG_PX_PER_DECADE));
        }
    } else {
        float delta = (float)(x - drag->start_x) * VAR_DRAG_LINEAR_UNITS_PER_PX;
        new_val = drag->start_value + delta;
    }

    if (out) {
        const char *name = drag->name[0] ? drag->name : g_predef_vars[drag->var_idx].name;
        snprintf(out->name, sizeof(out->name), "%s", name);
        out->value = new_val;
    }
    return 1;
}
