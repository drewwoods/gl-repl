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

#include "state_types.h"

/* Live UI-owned chrome state captured into UiRenderSnapshot each frame. */

typedef struct {
    ReplStatusState           status;
    ReplHelpState             help;
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

/* Camera pose is not part of UiState; use glr_camera.h for orbit/pan/zoom
 * state and helpers, and variable_panel_state.h for variable-panel visibility
 * and drag state. */

#endif /* UI_STATE_H */
