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
    UiStatusHistory         status_history;
    UiHelpState             help;
    UiGlStateInspectorState gl_state_inspector;
    UiCommandDescriptionState command_description;
    UiProfilePanelState     profile_panel;
    UiMemoryPanelState      memory_panel;
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
void             ui_state_status_set_music(const char *message);

/* Recent-message history ring (session-only). Every status message is
 * pushed here by the private set-kind chokepoint; the inline viewer's
 * open/closed toggle rides along on the same struct. Opening the list
 * (set_open(1) or either side of toggle) marks ERROR entries read. */
UiStatusHistory  ui_state_status_history(void);
void             ui_state_status_history_set_open(int open);
void             ui_state_status_history_toggle(void);

/* Help overlay visibility. */
UiHelpState  ui_state_help(void);
UiHelpState *ui_state_help_mut(void);

/* Floating OpenGL-state popup. anchor_px/anchor_py are the opening
 * right-click position in GLUT screen coords (y-down); opening resets
 * the wheel-scroll row offset and collapses the default/source detail
 * columns (the popup always opens narrow). */
UiGlStateInspectorState ui_state_gl_state_inspector(void);
void ui_state_gl_state_inspector_open(int source_line_idx,
                                      int anchor_px, int anchor_py);
void ui_state_gl_state_inspector_close(void);
void ui_state_gl_state_inspector_set_scroll(int scroll_rows);
void ui_state_gl_state_inspector_toggle_details(void);
void ui_state_gl_state_inspector_toggle_setup(void);
/* Pin the second probe point the report is compared against, or clear it back
 * to the OpenGL 2.1 defaults (source_line_idx < 0, or re-pinning the line
 * already pinned - the gesture is a toggle, like the anchor's own). Pinning
 * expands the detail columns: the basis is a column, so a comparison with it
 * hidden would change only the accent colors. */
void ui_state_gl_state_inspector_set_basis(int source_line_idx);

/* Floating right-click GL-command description popup. */
UiCommandDescriptionState ui_state_command_description(void);
void ui_state_command_description_open(int source_line_idx,
                                       int anchor_px, int anchor_py);
void ui_state_command_description_close(void);

/* Profile panel mode. */
UiProfilePanelState  ui_state_profile_panel(void);
UiProfilePanelState *ui_state_profile_panel_mut(void);

/* Memory panel mode. */
UiMemoryPanelState  ui_state_memory_panel(void);
UiMemoryPanelState *ui_state_memory_panel_mut(void);

/* Viewport size. */
UiViewportState  ui_state_viewport(void);
UiViewportState *ui_state_viewport_mut(void);
void               ui_state_viewport_set_size(int window_w, int window_h);

/* Pointer state. */
UiPointerState  ui_state_pointer(void);
UiPointerState *ui_state_pointer_mut(void);
void              ui_state_pointer_set(int mouse_x, int mouse_y, int mouse_button);
void              ui_state_pointer_set_pos(int mouse_x, int mouse_y);

/* Code-panel render chrome: panel divider and per-frame mirrors.
 * The editor-session bits (scroll / scroll_follow_cursor) live on
 * EditorState.scroll, and cursor blink state lives on EditorState.cursor_blink instead. */
UiCodePanelRuntimeState  ui_state_code_panel(void);
UiCodePanelRuntimeState *ui_state_code_panel_mut(void);

/* Camera pose is not part of UiState; use glr_camera.h for orbit/pan/zoom
 * state and helpers, and variable_panel_state.h for variable-panel visibility
 * and drag state. */

#endif /* UI_STATE_H */
