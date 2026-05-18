/*
 * variable_panel_drag.c -- Variable slider drag-state implementation
 *                    (peer-owned by variable_panel).
 *
 * Status: implementation-behind-variable_panel.
 *
 * Phase F commit 31 moved the drag-state bytes into the variable_panel
 * peer module (variable_panel.c). This file is the implementation
 * behind the peer's variable_panel_handle_drag_* surface — it reads /
 * writes via variable_panel_drag_mut() and supplies the value-mapping
 * logic (linear / log scaling) for controller-routed external edits.
 *
 * Phase J7 retired the legacy repl_var_drag_* aliases — the bodies
 * here are exposed under their canonical variable_panel_* /
 * variable_panel_handle_* names directly. The motion path returns a
 * requested value change instead of mutating REPL/editor state.
 *
 * Linear drag: 1 pixel = 0.05 units.
 * Log drag:    200 pixels = one decade (x10 / ÷10), sign preserved.
 *              Near-zero start value falls back to a linear bootstrap
 *              so the slider can walk off zero.
 */
#include "widgets/variable_panel_drag.h"
#include "widgets/variable_panel_state.h"

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
    EditorVariableDragState *drag = variable_panel_drag_mut();
    drag->var_idx = row;
    drag->log_mode = log_mode ? 1 : 0;
    drag->start_value = g_predef_vars[row].value;
    drag->start_x = x;
    snprintf(drag->name, sizeof(drag->name), "%s", g_predef_vars[row].name);
    drag->undo_snapshot_pushed = 0;
}

void variable_panel_handle_drag_reset(void) {
    EditorVariableDragState *drag = variable_panel_drag_mut();
    drag->var_idx = -1;
    drag->log_mode = 0;
    drag->start_value = 0.0f;
    drag->start_x = 0;
    drag->name[0] = '\0';
    drag->undo_snapshot_pushed = 0;
}

int variable_panel_handle_drag_motion(int x, VariablePanelValueChange *out) {
    EditorVariableDragState *drag = variable_panel_drag_mut();
    float new_val;

    if (out) {
        out->name[0] = '\0';
        out->value = 0.0f;
    }
    if (drag->var_idx < 0)
        return 0;

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

    if (out) {
        const char *name = drag->name[0] ? drag->name : g_predef_vars[drag->var_idx].name;
        snprintf(out->name, sizeof(out->name), "%s", name);
        out->value = new_val;
    }
    return 1;
}
