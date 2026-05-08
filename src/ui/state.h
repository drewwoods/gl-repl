#ifndef UI_STATE_H
#define UI_STATE_H

/* UI-chrome typedefs (Status / Help / VariablePanel / ProfilePanel /
 * Viewport / Pointer / CodePanel / Camera) used to live in
 * repl_state_views.h, which inverted ownership: every UI file pulled
 * in a REPL header just to reach a chrome typedef. They moved to
 * src/ui/state_types.h (Phase H typedef relocation), and ui_state.h
 * now includes the ui-owned header directly. */
#include "state_types.h"

/* UiState owns transient chrome, viewport, pointer, status text,
 * code-panel render chrome, and camera viewport pose.
 *
 * UI is a view/hit-test layer. It should render snapshots and return
 * UiHit results. imrepl_ctrl routes those hits to the owning
 * subsystem (editor, variable panel, replay, scene/viewport). UI
 * should not own editor behavior or mutate editor/REPL state
 * directly.
 *
 * See feature/editor-text-model-controller.md and MODULES.md for the
 * authoritative contract. The forwarder block that previously kept
 * the legacy `repl_state_*` chrome accessors alive was deleted in
 * Phase A commit 14; non-allowlisted repl_*.c callers forward-
 * declare the few accessors they need, and the rest of the tree
 * uses the canonical `ui_state_*` API directly.
 */

typedef struct {
    ReplStatusState           status;
    ReplHelpState             help;
    /* variable_panel visibility lives in variable_panel.c (Phase F
     * commit 31). Callers use variable_panel_view /
     * variable_panel_view_mut directly. */
    ReplProfilePanelState     profile_panel;
    ReplViewportState         viewport;
    ReplPointerState          pointer;
    ReplCodePanelRuntimeState code_panel;
    ReplCameraState           camera;
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

/* Camera viewport pose. The auto_rotate config field sits on the same
 * struct since it is naturally paired with the pose; presentation
 * "reset" paths still write through ui_state_camera_mut(). */
ReplCameraState  ui_state_camera(void);
ReplCameraState *ui_state_camera_mut(void);
ReplCameraState  ui_state_camera_snapshot(void);
void             ui_state_camera_set(float rx, float ry, float dist,
                                     float tx, float ty, float tz,
                                     float motion_glow);
void             ui_state_camera_set_orbit(float rx, float ry);
void             ui_state_camera_set_pan(float tx, float ty, float tz);
void             ui_state_camera_set_distance(float dist);
void             ui_state_camera_set_motion_glow(float motion_glow);
void             ui_state_camera_reset_default(void);

#endif /* UI_STATE_H */
