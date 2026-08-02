/*
 * variable_panel_drag.c -- Variable slider drag-state implementation
 *                    (peer-owned by variable_panel).
 *
 * Implements the variable_panel_handle_drag_* surface: reads / writes
 * peer-owned drag state via variable_panel_drag_mut() and maps pointer
 * motion into requested value changes without mutating REPL/editor
 * state directly.
 *
 * Linear drag:  1 pixel = VAR_DRAG_LINEAR_UNITS_PER_PX units (0.1).
 * Coarse drag:  the right-click "fast" scrub is the same linear scrub at
 *               GLR_ADJUST_COARSE_SCALE (10x) the rate, i.e. 1.0 units/px.
 */
#include <stdio.h>
#include "subsystems/variable_panel/variable_panel_drag.h"
#include "subsystems/variable_panel/variable_panel_state.h"

/* Drag-scaling tunable (the prose spec in the file header above is the
 * authority - keep it in sync). The coarse (right-click) scrub multiplies
 * this by GLR_ADJUST_COARSE_SCALE from config.h. */
#define VAR_DRAG_LINEAR_UNITS_PER_PX  0.10f   /* 1 px = 0.1 units */

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

int variable_panel_drag_coarse(void) {
    return variable_panel_drag().coarse;
}

void variable_panel_handle_drag_begin(int row, int coarse, int x) {
    char  name[REPL_PREDEF_NAME_MAX] = {0};
    float value = 0.0f;
    /* Bridge fills name + value and bounds-checks the row; unset source or
     * an out-of-range row leaves the drag idle. */
    if (!g_value_source || !g_value_source->read_row ||
        !g_value_source->read_row(row, name, (int)sizeof(name), &value))
        return;
    VariablePanelDragState *drag = variable_panel_drag_mut();
    drag->var_idx = row;
    drag->coarse = coarse ? 1 : 0;
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
    drag->coarse = 0;
    drag->start_value = 0.0f;
    g_drag_start_x = 0;
    drag->name[0] = '\0';
    drag->undo_snapshot_pushed = 0;
    drag->value_changed = 0;
    drag->final_value = 0.0f;
}

/* Map a horizontal drag delta to a value. Both drag modes are linear; the
 * right-click "coarse" mode scrubs at GLR_ADJUST_COARSE_SCALE (10x) the
 * units-per-pixel of the plain left-click drag. */
static float drag_linear_value(float start_value, int dx, int coarse) {
    float upx = VAR_DRAG_LINEAR_UNITS_PER_PX;
    if (coarse)
        upx *= GLR_ADJUST_COARSE_SCALE;
    return start_value + (float)dx * upx;
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
    new_val = drag_linear_value(drag->start_value, dx, drag->coarse);

    if (out) {
        /* drag->name was captured from the value source at drag-begin. */
        snprintf(out->name, sizeof(out->name), "%s", drag->name);
        out->value = new_val;
    }
    return 1;
}
