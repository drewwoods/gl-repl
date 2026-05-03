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
 * and the drag state was on `EditorState.variable_drag`. Both move
 * here. The legacy accessors (`ui_state_variable_panel*`,
 * `editor_state_variable_drag*`) are kept as thin forwarders during
 * the migration so call sites keep compiling; they will be removed
 * once Phase F closes.
 *
 * Snapshot/restore: tests and undo capture this state via
 * `variable_panel_state_capture` / `_restore` alongside the editor /
 * ui captures.
 *
 * Render path: ui_variable_panel.c reads visibility through the
 * `UiRenderSnapshot.variable_panel` slice (which the controller still
 * fills from `ui_state_variable_panel()` for now). The drag-state
 * read sites (panel renderer, code-panel highlight) keep using
 * `repl_var_drag_*` query helpers.
 */
#ifndef VARIABLE_PANEL_H
#define VARIABLE_PANEL_H

#include "repl_state_views.h"
#include "editor_state.h"

/* Composite peer state. The two slices are kept as their existing
 * typedefs so legacy accessors can return them by value without
 * reshaping the views layer. */
typedef struct {
    ReplVariablePanelState view;   /* visibility flag */
    ReplVariableDragState  drag;   /* slider drag transaction */
} VariablePanelState;

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
 * mouse handler now calls them instead of touching the legacy
 * repl_var_drag_* surface directly.
 */

/* Query whether a drag transaction is currently active. */
int  variable_panel_drag_active(void);

/* Active drag's variable index (into g_predef_vars[]), -1 if none. */
int  variable_panel_drag_active_var(void);

/* 1 if the active drag is in log mode (exponential scaling), 0 for linear. */
int  variable_panel_drag_log_mode(void);

/* Begin a drag transaction on a variable row. */
void variable_panel_handle_drag_begin(int row, int log_mode, int x);

/* Update the dragged variable from the new mouse x-coordinate. */
void variable_panel_handle_drag_motion(int x);

/* End the drag transaction. */
void variable_panel_handle_drag_reset(void);

#endif /* VARIABLE_PANEL_H */
