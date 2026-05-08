/*
 * variable_panel.h - Variable slider panel peer subsystem.
 *
 * Phase F commit 31 entry. The variable panel is a peer alongside
 * editor / replay / scene rather than a slice of EditorState or
 * UiState. It owns the panel's visibility flag and the slider drag
 * transaction, with `imrepl_ctrl` routing UiHits (UI_HIT_VARIABLE_SLIDER)
 * to its handler functions.
 *
 * Storage migration: the visibility flag was on `UiState.variable_panel`
 * and the drag state was on `EditorState.variable_drag`. Both live
 * here. Phase J7 retired the legacy forwarders
 * (`ui_state_variable_panel*`, `editor_state_variable_drag*`,
 * `repl_var_drag_*`) — callers use `variable_panel_view` /
 * `variable_panel_drag` / `variable_panel_handle_drag_*` directly.
 *
 * Snapshot/restore: tests and undo capture this state via
 * `variable_panel_state_capture` / `_restore` alongside the editor /
 * ui captures.
 *
 * Render path: imrepl_ctrl_build_ui_snapshot fills
 * `UiRenderSnapshot.variable_panel` from `variable_panel_view()` and
 * `UiRenderSnapshot.variable_drag` from `variable_panel_drag()`
 * directly. ui_variable_panel.c reads visibility through
 * `variable_panel_visible()` and the drag highlights through the
 * narrow `variable_panel_drag_active_var` / `_log_mode` queries.
 */
#ifndef VARIABLE_PANEL_H
#define VARIABLE_PANEL_H

#include "repl_state_views.h"
#include "editor/state.h"

/* Composite peer state. The two slices are kept as their existing
 * typedefs so legacy accessors can return them by value without
 * reshaping the views layer. */
typedef struct {
    ReplVariablePanelState view;   /* visibility flag */
    ReplVariableDragState  drag;   /* slider drag transaction */
} VariablePanelState;

typedef struct VariablePanelValueChange_s {
    char  name[16];
    float value;
} VariablePanelValueChange;

/* Lifecycle. */
void variable_panel_state_capture(VariablePanelState *snapshot);
void variable_panel_state_restore(const VariablePanelState *snapshot);
void variable_panel_state_reset(void);

/* Read-only / mutable accessors. */
ReplVariablePanelState  variable_panel_view(void);
ReplVariablePanelState *variable_panel_view_mut(void);
ReplVariableDragState   variable_panel_drag(void);
ReplVariableDragState  *variable_panel_drag_mut(void);

/* Convenience wrappers. */
int  variable_panel_visible(void);
void variable_panel_set_visible(int visible);

/* --- Drag transaction handler API ---
 *
 * Phase F commit 32 entry points. `imrepl_ctrl` routes
 * UI_HIT_VARIABLE_SLIDER hits through these handlers; the editor's
 * mouse handler calls them. Implementations live in
 * variable_panel_drag.c alongside the linear / log scaling logic.
 */

/* Query whether a drag transaction is currently active. */
int  variable_panel_drag_active(void);

/* Active drag's variable index (into g_predef_vars[]), -1 if none. */
int  variable_panel_drag_active_var(void);

/* 1 if the active drag is in log mode (exponential scaling), 0 for linear. */
int  variable_panel_drag_log_mode(void);

/* 1 once the current drag has captured its coalesced undo snapshot. */
int  variable_panel_drag_undo_snapshot_pushed(void);

/* Mark the current drag's undo snapshot as captured. */
void variable_panel_drag_mark_undo_snapshot_pushed(void);

/* Begin a drag transaction on a variable row. */
void variable_panel_handle_drag_begin(int row, int log_mode, int x);

/* Compute the dragged variable's requested value from the new mouse
 * x-coordinate and return it through `out`. Returns 1 when a request
 * was produced, 0 when no drag is active. */
int  variable_panel_handle_drag_motion(int x, VariablePanelValueChange *out);

/* End the drag transaction. */
void variable_panel_handle_drag_reset(void);

#endif /* VARIABLE_PANEL_H */
