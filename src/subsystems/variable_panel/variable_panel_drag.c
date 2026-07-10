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
#include "subsystems/variable_panel/variable_panel_drag.h"
#include "subsystems/variable_panel/variable_panel_state.h"

/* Drag-scaling tunables (the prose spec in the file header above is the
 * authority — keep them in sync). VAR_DRAG_ZERO_EPS is the |start|
 * threshold below which log drag falls back to the linear bootstrap. */
#define VAR_DRAG_LINEAR_UNITS_PER_PX  0.05f   /* 1 px = 0.05 units */
#define VAR_DRAG_PX_PER_DECADE        200.0f  /* 200 px = x10 / div10 */
#define VAR_DRAG_ZERO_EPS             1e-6f
#define VAR_DRAG_ZERO_BOOTSTRAP_RATE  0.001f  /* units/px while |start| ~ 0 */

static int g_drag_start_x = 0;
static const VariablePanelValueSource *g_value_source = NULL;

void variable_panel_install_value_source(const VariablePanelValueSource *src) {
    g_value_source = src;
}

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
    char  name[REPL_PREDEF_NAME_MAX] = {0};
    float value = 0.0f;
    /* Bridge fills name + value and bounds-checks the row; unset source or
     * an out-of-range row leaves the drag idle. */
    if (!g_value_source || !g_value_source->read_row ||
        !g_value_source->read_row(row, name, (int)sizeof(name), &value))
        return;
    VariablePanelDragState *drag = variable_panel_drag_mut();
    drag->var_idx = row;
    drag->log_mode = log_mode ? 1 : 0;
    drag->start_value = value;
    g_drag_start_x = x;
    snprintf(drag->name, sizeof(drag->name), "%s", name);
    drag->undo_snapshot_pushed = 0;
    drag->value_changed = 0;
    drag->final_value = value;
}

void variable_panel_handle_drag_reset(void) {
    VariablePanelDragState *drag = variable_panel_drag_mut();
    drag->var_idx = -1;
    drag->log_mode = 0;
    drag->start_value = 0.0f;
    g_drag_start_x = 0;
    drag->name[0] = '\0';
    drag->undo_snapshot_pushed = 0;
    drag->value_changed = 0;
    drag->final_value = 0.0f;
}

static float drag_log_value(float start_value, int dx) {
    float dx_total = (float)dx;
    float mag = fabsf(start_value);
    if (mag < VAR_DRAG_ZERO_EPS) {
        /* Bootstrap from zero: treat first pixels as linear, then log. */
        return dx_total * VAR_DRAG_ZERO_BOOTSTRAP_RATE;
    } else {
        float sign = (start_value >= 0.0f) ? 1.0f : -1.0f;
        return sign * mag * expf(dx_total * (logf(10.0f) / VAR_DRAG_PX_PER_DECADE));
    }
}

static float drag_linear_value(float start_value, int dx) {
    float delta = (float)dx * VAR_DRAG_LINEAR_UNITS_PER_PX;
    return start_value + delta;
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

    int dx = x - g_drag_start_x;
    if (drag->log_mode) {
        new_val = drag_log_value(drag->start_value, dx);
    } else {
        new_val = drag_linear_value(drag->start_value, dx);
    }

    if (out) {
        /* drag->name was captured from the value source at drag-begin. */
        snprintf(out->name, sizeof(out->name), "%s", drag->name);
        out->value = new_val;
    }
    return 1;
}
