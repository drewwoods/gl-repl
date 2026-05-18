/*
 * ui/state_types.h - Small UI-chrome value types.
 *
 * Defines the lightweight structs shared by `ui_state.h`, `UiRenderSnapshot`,
 * and UI renderers: code-panel chrome, help/profile/status flags, pointer, and
 * viewport size. These are UI-owned values even though several still carry the
 * historical `Repl` prefix for compatibility.
 *
 * `src/repl/state_views.h` re-exports these types for older transitive
 * consumers, but ownership lives here.
 */
#ifndef UI_STATE_TYPES_H
#define UI_STATE_TYPES_H

#include "config.h"          /* REPL_STATUS_TEXT_MAX */

/* Code-panel render chrome: panel divider, cursor blink, and per-frame mirrors
 * of the presentation flags renderers need.
 *
 * `layout_mode` and `show_vertex_indices` are per-frame mirrors of
 * ReplPresentationState fields so ui_*.c renderers and hit-tests can
 * read them without crossing the repl_state_*() boundary; the controller
 * refreshes them in glr_ctrl_build_ui_snapshot. The source of truth still lives
 * on app-side presentation state. Scroll state intentionally does not live here;
 * it belongs to the editor session, not UI chrome. */
typedef struct {
    float panel_frac;
    int   resizing_panel;
    int   cursor_visible;
    int   blink_tick;
    int   layout_mode;          /* mirror of presentation.code_panel_layout */
    int   show_vertex_indices;  /* mirror of presentation.show_vertex_indices */
    int   wrap_at_comma;        /* mirror of presentation.wrap_at_comma */
    int   syntax_highlight;     /* mirror of presentation.syntax_highlight */
    int   code_focus;           /* mirror of presentation.code_focus */
} UiCodePanelRuntimeState;

/* Help-overlay visibility. Tab selection and scroll live in the separate
 * EditorHelpSession snapshot carried alongside this flag. */
typedef struct {
    int visible;
} UiHelpState;

typedef struct {
    int visible;
} UiVariablePanelState;

typedef struct {
    int mode;
} UiProfilePanelState;

typedef struct {
    char text[REPL_STATUS_TEXT_MAX];
    int  ttl;
} UiStatusState;

/* Camera pose is intentionally not part of the UI chrome types. See
 * glr_camera.h for the app-owned orbit/pan/zoom state. */

typedef struct {
    int mouse_x;
    int mouse_y;
    int mouse_button;
} UiPointerState;

typedef struct {
    int window_w;
    int window_h;
} UiViewportState;

#endif /* UI_STATE_TYPES_H */
