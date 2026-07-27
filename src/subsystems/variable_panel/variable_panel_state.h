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
#ifndef VARIABLE_PANEL_STATE_H
#define VARIABLE_PANEL_STATE_H

#include "config.h"
#include "subsystems/variable_panel/variable_panel_drag.h"  /* VariablePanelValueChange */

typedef struct {
    int   visible;
    int   collapsed; /* 1 = only the title bar is shown (mouse-only toggle,
                      * no keymap slot) */
} VariablePanelViewState;

/* Snapshot-side view of the slider-drag transaction. Lets the renderer
 * tell which row is being dragged (and in what mode) without reaching
 * back into the peer's live state. The controller fills this once per
 * frame from variable_panel_drag_active_var() / _drag_coarse(); the
 * renderer reads only from the snapshot. */
typedef struct {
    int active_var;   /* row index being dragged, -1 when idle */
    int coarse;       /* 1 = coarse (right-click, 10x) drag, 0 = normal linear
                       * (only meaningful when active_var >= 0) */
} UiVariableDragView;

/* Variable slider drag transaction: which variable is being dragged,
 * the variable name captured at drag-begin, the starting value, the
 * cursor anchor x in window pixels, and whether the controller has
 * already captured the coalesced undo snapshot for this drag.
 *
 * `value_changed` / `final_value` record what the motion handler actually
 * applied. Motion only updates the live predef value; the declaration row in
 * the code panel is rewritten once, on mouse-up, from `final_value` — so a
 * drag performs no source mutation until it ends. `value_changed` also makes
 * a no-motion click (press + release, no move) a complete no-op. */
typedef struct {
    int   var_idx;
    int   coarse;
    float start_value;
    char  name[REPL_PREDEF_NAME_MAX];
    int   undo_snapshot_pushed;
    int   value_changed;
    float final_value;
} VariablePanelDragState;

/* Composite peer state. The two slices keep their existing value types so the
 * accessors and snapshots can pass them by value without another wrapper API. */
typedef struct {
    VariablePanelViewState view; /* visibility flag */
    VariablePanelDragState drag; /* slider drag transaction */
} VariablePanelState;


/* Lifecycle.
 * Note: variable_panel_state_capture/restore is exclusively a test/verification contract;
 * production code does not capture/restore variable panel state. */
void variable_panel_state_capture(VariablePanelState *snapshot);
void variable_panel_state_restore(const VariablePanelState *snapshot);
void variable_panel_state_reset(void);

/* Read-only / mutable accessors. */
VariablePanelViewState variable_panel_view(void);
VariablePanelViewState *variable_panel_state_mut(void);
VariablePanelDragState variable_panel_drag(void);
VariablePanelDragState *variable_panel_drag_mut(void);

/* Convenience wrappers. */
int  variable_panel_visible(void);
void variable_panel_set_visible(int visible);

/* Collapse state: 1 = only the title bar renders (rows and drag hit-testing
 * are unavailable while collapsed). Mouse-only — toggled by clicking the
 * title bar's chip, no keymap binding. */
int  variable_panel_collapsed(void);
void variable_panel_set_collapsed(int collapsed);
void variable_panel_toggle_collapsed(void);

/* --- Drag transaction handler API ---
 *
 * The controller routes `UI_HIT_VARIABLE_SLIDER` hits and follow-up mouse motion
 * through these handlers. Implementations live in `variable_panel_drag.c`
 * alongside the linear scaling math (normal + coarse).
 */

/* Query whether a drag transaction is currently active. */
int  variable_panel_drag_active(void);

/* Active drag's variable index (into g_predef_vars[]), -1 if none. */
int  variable_panel_drag_active_var(void);

/* 1 if the active drag is the coarse (right-click) scrub, 0 for the normal
 * linear scrub. The coarse mode is the same linear scrub at 10x the rate. */
int  variable_panel_drag_coarse(void);

/* 1 once the current drag has captured its coalesced undo snapshot. */
int  variable_panel_drag_undo_snapshot_pushed(void);

/* Mark the current drag's undo snapshot as captured. */
void variable_panel_drag_mark_undo_snapshot_pushed(void);

/* Record the value a motion event successfully applied to the live variable.
 * Peer-owned so the app router never writes drag fields directly; mouse-up
 * reads it back through variable_panel_drag() to decide whether to persist. */
void variable_panel_drag_note_applied_value(float value);

/* Begin a drag transaction on a variable row. `coarse` selects the
 * right-click 10x scrub; 0 is the normal linear scrub. */
void variable_panel_handle_drag_begin(int row, int coarse, int x);

/* Compute the dragged variable's requested value from the new mouse
 * x-coordinate and return it through `out`. Returns 1 when a request
 * was produced, 0 when no drag is active. */
int  variable_panel_handle_drag_motion(int x, VariablePanelValueChange *out);

/* End the drag transaction. */
void variable_panel_handle_drag_reset(void);

#endif /* VARIABLE_PANEL_STATE_H */
