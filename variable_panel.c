/*
 * variable_panel.c - Variable slider panel peer subsystem.
 */
#include "variable_panel.h"
#include "variable_panel_drag.h"

#define VARIABLE_PANEL_INITIAL                              \
    {                                                       \
        .view = { .visible = 1 },                           \
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

ReplVariablePanelState variable_panel_view(void) {
    return g_variable_panel.view;
}

ReplVariablePanelState *variable_panel_view_mut(void) {
    return &g_variable_panel.view;
}

ReplVariableDragState variable_panel_drag(void) {
    return g_variable_panel.drag;
}

ReplVariableDragState *variable_panel_drag_mut(void) {
    return &g_variable_panel.drag;
}

int variable_panel_visible(void) {
    return g_variable_panel.view.visible;
}

void variable_panel_set_visible(int visible) {
    g_variable_panel.view.visible = visible ? 1 : 0;
}

/* Drag transaction handlers. The drag-state writeback (value writeback,
 * source-line rewrite for matching CMD_VAR_ASSIGN) lives in
 * variable_panel_drag.c; these wrappers expose it under the peer-subsystem
 * namespace. */
int variable_panel_drag_active(void) {
    return repl_var_drag_active();
}

int variable_panel_drag_active_var(void) {
    return repl_var_drag_active_var();
}

int variable_panel_drag_log_mode(void) {
    return repl_var_drag_log_mode();
}

void variable_panel_handle_drag_begin(int row, int log_mode, int x) {
    repl_var_drag_begin(row, log_mode, x);
}

void variable_panel_handle_drag_motion(int x) {
    repl_var_drag_motion(x);
}

void variable_panel_handle_drag_reset(void) {
    repl_var_drag_reset();
}
