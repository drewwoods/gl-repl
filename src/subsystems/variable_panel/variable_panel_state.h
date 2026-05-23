/*
 * variable_panel_state.h - Variable-panel peer state and drag handlers.
 *
 * Owns the variable panel's persistent peer state: the visibility flag for the
 * floating slider panel and the active drag transaction for a slider row. The
 * UI layer renders and hit-tests that state; the controller routes slider hits
 * and mouse motion into the handler API below.
 *
 * Snapshot/restore is separate because full-world capture paths copy this peer
 * alongside REPL/editor/UI state. Render snapshot assembly pulls
 * `variable_panel_view()` and `variable_panel_drag()` directly into
 * `UiRenderSnapshot`, while UI code typically uses the narrower visibility and
 * active-drag queries.
 *
 * The current public surface is the `variable_panel_*` family declared here
 * plus the small value-change type in `variable_panel_drag.h` (the peer split
 * replaced older UiState/EditorState storage and retired the legacy
 * forwarders).
 */
#ifndef VARIABLE_PANEL_H
#define VARIABLE_PANEL_H

#include "editor/state.h"
#include "ui/app/state_types.h"
#include "subsystems/variable_panel/variable_panel_drag.h"  /* VariablePanelValueChange */

/* Composite peer state. The two slices keep their existing value types so the
 * accessors and snapshots can pass them by value without another wrapper API. */
typedef struct {
    UiVariablePanelState view;   /* visibility flag */
    EditorVariableDragState  drag;   /* slider drag transaction */
} VariablePanelState;


/* Lifecycle. */
void variable_panel_state_capture(VariablePanelState *snapshot);
void variable_panel_state_restore(const VariablePanelState *snapshot);
void variable_panel_state_reset(void);

/* Read-only / mutable accessors. */
UiVariablePanelState  variable_panel_view(void);
UiVariablePanelState *variable_panel_view_mut(void);
EditorVariableDragState   variable_panel_drag(void);
EditorVariableDragState  *variable_panel_drag_mut(void);

/* Convenience wrappers. */
int  variable_panel_visible(void);
void variable_panel_set_visible(int visible);

/* --- Drag transaction handler API ---
 *
 * The controller routes `UI_HIT_VARIABLE_SLIDER` hits and follow-up mouse motion
 * through these handlers. Implementations live in `variable_panel_drag.c`
 * alongside the linear/log scaling math.
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
