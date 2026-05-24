/*
 * variable_panel.c - Variable slider panel peer subsystem.
 */
#include "subsystems/variable_panel/variable_panel_state.h"

#define VARIABLE_PANEL_INITIAL                              \
    {                                                       \
        .view = { .visible = 1, .replay_lift_px = 0.0f },   \
        .drag = {                                           \
            .var_idx     = -1,                              \
            .log_mode    = 0,                               \
            .start_value = 0.0f,                            \
            .start_x     = 0,                               \
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

UiVariablePanelState variable_panel_view(void) {
    return g_variable_panel.view;
}

UiVariablePanelState *variable_panel_view_mut(void) {
    return &g_variable_panel.view;
}

EditorVariableDragState variable_panel_drag(void) {
    return g_variable_panel.drag;
}

EditorVariableDragState *variable_panel_drag_mut(void) {
    return &g_variable_panel.drag;
}

int variable_panel_visible(void) {
    return g_variable_panel.view.visible;
}

void variable_panel_set_visible(int visible) {
    g_variable_panel.view.visible = visible ? 1 : 0;
}

/* Drag transaction handlers. The pure-pass-through query / handler
 * wrappers (variable_panel_drag_active / _active_var / _log_mode /
 * variable_panel_handle_drag_begin / _motion / _reset) live in
 * variable_panel_drag.c — they sit alongside the value-mapping
 * logic (linear / log) so the implementation file is self-contained.
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
