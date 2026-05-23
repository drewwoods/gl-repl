/*
 * ui_state.h - UI-owned transient chrome state and accessors.
 *
 * Owns the small pieces of 2D chrome that persist across frames: status/help
 * visibility, profile panel mode, viewport size, pointer position, and code-panel
 * render chrome. It does not own editor session state, variable-panel peer
 * state, camera pose, or REPL behavior.
 *
 * The value types live in `state_types.h`, the UI-owned type header extracted
 * from older REPL state declarations so UI modules can include UI types without
 * reaching through REPL headers.
 */
#ifndef UI_STATE_H
#define UI_STATE_H

#include "ui/app/state_types.h"

/* Live UI-owned chrome state captured into UiRenderSnapshot each frame. */

typedef struct {
    UiStatusState           status;
    UiHelpState             help;
    UiProfilePanelState     profile_panel;
    UiViewportState         viewport;
    UiPointerState          pointer;
    UiCodePanelRuntimeState code_panel;
} UiState;

void ui_state_capture(UiState *snapshot);
void ui_state_restore(const UiState *snapshot);
void ui_state_reset(void);

/* Status slice. */
UiStatusState  ui_state_status(void);
UiStatusState *ui_state_status_mut(void);
void             ui_state_status_set(const char *message);
void             ui_state_status_set_error(const char *message);

/* Help overlay visibility. */
UiHelpState  ui_state_help(void);
UiHelpState *ui_state_help_mut(void);

/* Profile panel mode. */
UiProfilePanelState  ui_state_profile_panel(void);
UiProfilePanelState *ui_state_profile_panel_mut(void);

/* Viewport size. */
UiViewportState  ui_state_viewport(void);
UiViewportState *ui_state_viewport_mut(void);
void               ui_state_viewport_set_size(int window_w, int window_h);

/* Pointer state. */
UiPointerState  ui_state_pointer(void);
UiPointerState *ui_state_pointer_mut(void);
void              ui_state_pointer_set(int mouse_x, int mouse_y, int mouse_button);
void              ui_state_pointer_set_pos(int mouse_x, int mouse_y);

/* Code-panel render chrome: panel divider, cursor blink, cursor px/py.
 * The editor-session bits (scroll / scroll_follow_cursor) live on
 * EditorState.scroll instead. */
UiCodePanelRuntimeState  ui_state_code_panel(void);
UiCodePanelRuntimeState *ui_state_code_panel_mut(void);

/* Camera pose is not part of UiState; use glr_camera.h for orbit/pan/zoom
 * state and helpers, and variable_panel_state.h for variable-panel visibility
 * and drag state. */

#endif /* UI_STATE_H */
