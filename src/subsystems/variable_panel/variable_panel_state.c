/*
 * variable_panel_state.c - Variable slider panel peer subsystem state.
 */
#include "subsystems/variable_panel/variable_panel_state.h"

#define VARIABLE_PANEL_INITIAL                              \
    {                                                       \
        .view = { .visible = 1 },                           \
        .drag = {                                           \
            .var_idx     = -1,                              \
            .coarse      = 0,                               \
            .start_value = 0.0f,                            \
        },                                                  \
    }

static VariablePanelState       g_variable_panel = VARIABLE_PANEL_INITIAL;
static const VariablePanelState g_variable_panel_defaults = VARIABLE_PANEL_INITIAL;

void variable_panel_state_capture(VariablePanelState *snapshot) {
    if (!snapshot) return;
    *snapshot = g_variable_panel;
}

void variable_panel_state_restore(const VariablePanelState *snapshot) {
    if (!snapshot) return;
    g_variable_panel = *snapshot;
}

void variable_panel_state_reset(void) {
    g_variable_panel = g_variable_panel_defaults;
}

VariablePanelViewState variable_panel_view(void) {
    return g_variable_panel.view;
}

VariablePanelViewState *variable_panel_state_mut(void) {
    return &g_variable_panel.view;
}

VariablePanelDragState variable_panel_drag(void) {
    return g_variable_panel.drag;
}

VariablePanelDragState *variable_panel_drag_mut(void) {
    return &g_variable_panel.drag;
}

int variable_panel_visible(void) {
    return g_variable_panel.view.visible;
}

void variable_panel_set_visible(int visible) {
    g_variable_panel.view.visible = visible ? 1 : 0;
}

/* Drag transaction handlers. The pure-pass-through query / handler
 * wrappers (variable_panel_drag_active / _active_var / _coarse /
 * variable_panel_handle_drag_begin / _motion / _reset) live in
 * variable_panel_drag.c — they sit alongside the value-mapping
 * logic (normal + coarse linear) so the implementation file is self-contained.
 *
 * The two undo-snapshot fields are kept here because they're a pure
 * field accessor on the peer's drag state; no value-mapping logic
 * involved. */
int variable_panel_drag_undo_snapshot_pushed(void) {
    return variable_panel_drag().undo_snapshot_pushed;
}

void variable_panel_drag_mark_undo_snapshot_pushed(void) {
    variable_panel_drag_mut()->undo_snapshot_pushed = 1;
}

void variable_panel_drag_note_applied_value(float value) {
    VariablePanelDragState *drag = variable_panel_drag_mut();
    if (drag->var_idx < 0)
        return;
    drag->value_changed = 1;
    drag->final_value = value;
}
