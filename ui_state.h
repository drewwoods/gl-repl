#ifndef UI_STATE_H
#define UI_STATE_H

/* EDITOR_OWNERSHIP_TODO(phase-5): break this include. ui_state.h
 * shouldn't depend on repl_state's view header — it inverts the
 * ownership relationship the three-layer contract is trying to
 * establish. The dependency exists today because ReplStatusState /
 * ReplHelpState / ReplVariablePanelState / ReplProfilePanelState /
 * ReplViewportState / ReplPointerState typedefs still live in
 * repl_state_views.h. The Phase 5 rename relocates the typedefs to a
 * ui-owned header (likely a new ui_state_types.h) and this include
 * goes away. The ratchet at scripts/check-editor-ownership-budget.sh
 * forbids the include count from growing in the meantime. */
#include "repl_state_views.h"  /* ReplStatusState, ReplHelpState, etc. */

/* UiState owns transient chrome, viewport, pointer, status text,
 * visibility flags, and the 3D-viewport session per the three-layer
 * ownership contract in MODULES.md and
 * feature/editor-owns-text-completion.md.
 *
 * Phase 1 progress:
 *   commit  3: scaffold (placeholder struct).
 *   commit  8: status / help / variable_panel / profile_panel /
 *              viewport / pointer slices.
 *   commit 12: code_panel chrome (panel divider, cursor blink, cursor px/py).
 *
 * Slices still pending: camera (Phase A commit 13).
 *
 * Naming asymmetry vs. EditorState: the new canonical accessors here
 * use the `ui_state_*` prefix, but the legacy `repl_state_*` names
 * remain alive as one-line forwarders defined in repl_state.c. The
 * forwarders exist because `check-controller-boundaries` forbids most
 * `repl_*.c` callers from including `ui_state.h`; the legacy names
 * stay in repl_state_owners.h and resolve at link time to the
 * forwarders. The Phase 4-equivalent input-routing rework eliminates
 * the direct-mutation call sites that need the legacy names; the
 * forwarders go away then.
 */

typedef struct {
    ReplStatusState           status;
    ReplHelpState             help;
    ReplVariablePanelState    variable_panel;
    ReplProfilePanelState     profile_panel;
    ReplViewportState         viewport;
    ReplPointerState          pointer;
    ReplCodePanelRuntimeState code_panel;
} UiState;

void ui_state_capture(UiState *snapshot);
void ui_state_restore(const UiState *snapshot);
void ui_state_reset(void);

/* Status slice. */
ReplStatusState  ui_state_status(void);
ReplStatusState *ui_state_status_mut(void);
void             ui_state_status_set(const char *message);
void             ui_state_status_clear(void);
void             ui_state_status_tick(void);

/* Help overlay visibility. */
ReplHelpState  ui_state_help(void);
ReplHelpState *ui_state_help_mut(void);
void           ui_state_help_reset(void);

/* Variable panel visibility. */
ReplVariablePanelState  ui_state_variable_panel(void);
ReplVariablePanelState *ui_state_variable_panel_mut(void);

/* Profile panel mode. */
ReplProfilePanelState  ui_state_profile_panel(void);
ReplProfilePanelState *ui_state_profile_panel_mut(void);

/* Viewport size. */
ReplViewportState  ui_state_viewport(void);
ReplViewportState *ui_state_viewport_mut(void);
void               ui_state_viewport_set_size(int window_w, int window_h);

/* Pointer state. */
ReplPointerState  ui_state_pointer(void);
ReplPointerState *ui_state_pointer_mut(void);
void              ui_state_pointer_set(int mouse_x, int mouse_y, int mouse_button);
void              ui_state_pointer_set_pos(int mouse_x, int mouse_y);
void              ui_state_pointer_set_button(int mouse_button);

/* Code-panel render chrome: panel divider, cursor blink, cursor px/py.
 * The editor-session bits (scroll / scroll_follow_cursor) live on
 * EditorState.scroll instead. */
ReplCodePanelRuntimeState  ui_state_code_panel(void);
ReplCodePanelRuntimeState *ui_state_code_panel_mut(void);
void                       ui_state_code_panel_reset(void);

#endif /* UI_STATE_H */
