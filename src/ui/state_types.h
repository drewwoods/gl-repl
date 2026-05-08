/*
 * ui/state_types.h - UI-chrome value types.
 *
 * The 2D editor chrome owns a handful of small structs (panel divider,
 * camera viewport pose, status bar, help overlay flag, etc.). They used
 * to live in repl_state_views.h alongside the REPL-domain slices, which
 * inverted ownership: every UI file pulled in a REPL header just to
 * reach a chrome typedef. Hoisting them here lets ui_*.c files include
 * a UI-owned header for UI-owned types.
 *
 * The names still carry the legacy `Repl` prefix because renaming
 * cascades through every member access. Renaming is a separate
 * cleanup; this header is purely a relocation.
 *
 * repl_state_views.h includes this header so existing transitive
 * consumers keep working.
 */
#ifndef UI_STATE_TYPES_H
#define UI_STATE_TYPES_H

#ifndef REPL_STATUS_TEXT_MAX
#define REPL_STATUS_TEXT_MAX 256
#endif

/* Code-panel UI chrome: panel divider, cursor blink + pixel position
 * the renderer uses. The scroll fields used to live here too; Phase 1
 * commit 11 split them out into EditorState.scroll because scroll is
 * an editing-session concern, not a render-chrome one.
 *
 * `layout_mode` and `show_vertex_indices` are per-frame mirrors of
 * ReplPresentationState fields so ui_*.c renderers and hit-tests can
 * read them without crossing the repl_state_*() boundary; the controller
 * refreshes them in imrepl_ctrl_build_ui_snapshot. The source of truth
 * still lives on ReplPresentationState. */
typedef struct {
    float panel_frac;
    int   resizing_panel;
    int   cursor_visible;
    int   blink_tick;
    int   layout_mode;          /* mirror of presentation.code_panel_layout */
    int   show_vertex_indices;  /* mirror of presentation.show_vertex_indices */
} ReplCodePanelRuntimeState;

/* Help-overlay chrome flag. The session-state fields (tab_idx, scroll)
 * moved to editor_help_session.c (Phase G commit 35); the renderer
 * reads them from a separate UiRenderSnapshot.help_session slot. */
typedef struct {
    int visible;
} ReplHelpState;

typedef struct {
    int visible;
} ReplVariablePanelState;

typedef struct {
    int mode;
} ReplProfilePanelState;

typedef struct {
    char text[REPL_STATUS_TEXT_MAX];
    int  ttl;
} ReplStatusState;

typedef struct {
    float rx;
    float ry;
    float dist;
    float tx;
    float ty;
    float tz;
    float motion_glow;
    int   auto_rotate;
} ReplCameraState;

typedef struct {
    int mouse_x;
    int mouse_y;
    int mouse_button;
} ReplPointerState;

typedef struct {
    int window_w;
    int window_h;
} ReplViewportState;

#endif /* UI_STATE_TYPES_H */
