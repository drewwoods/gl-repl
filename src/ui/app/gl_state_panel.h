/*
 * src/ui/app/gl_state_panel.h - Floating OpenGL-state popup table.
 *
 * Pure renderer + hit-test over a controller-built view: draws the
 * right-click OpenGL-state report as a floating four-column table
 * (state name, current value, OpenGL 2.1 default, latest change source)
 * anchored near the click position, wheel-scrollable when the report is
 * taller than the window (flyout-style right-edge scrollbar hint). The fold
 * itself lives in src/repl/gl_state_inspector.c; open/close/scroll chrome
 * lives on UiState (ui_state_gl_state_inspector*).
 */
#ifndef UI_GL_STATE_PANEL_H
#define UI_GL_STATE_PANEL_H

#include "repl/gl_state_inspector.h"

/* Narrow per-frame view built by the controller
 * (glr_ctrl_build_gl_state_panel_view). anchor_px/anchor_py are the
 * popup's preferred top-left in y-up OpenGL window coords (the
 * controller flips the stored GLUT click y); the renderer clamps the
 * popup into the window. scroll_rows is the first visible report row
 * (clamped by render/hit against the solved row capacity). `report`
 * stays valid for the frame — it points at controller-owned storage
 * rebuilt each frame from the flat program. */
typedef struct {
    int visible;
    int window_w, window_h;
    int anchor_px, anchor_py;
    int scroll_rows;
    const ReplGlStateReport *report;
} UiGlStatePanelView;

/* Render the popup table once per frame. No-op when view->visible is 0.
 * Rows that differ from the OpenGL 2.1 default draw their current value
 * in the warning accent; explicit writes of the default value draw in
 * the OK accent so touched-ness stays visible either way. */
void ui_gl_state_panel_render(const UiGlStatePanelView *view);

/* Pure hit-test: 1 when (mx, my) — GLUT screen coords, y-down — lands
 * inside the open popup's frame; 0 otherwise (including when the popup
 * is closed). The router uses this for click-away dismiss and to scope
 * wheel scrolling to the popup. */
int ui_gl_state_panel_hit_test(const UiGlStatePanelView *view,
                               int mx, int my);

/* Largest valid scroll_rows for the view's solved geometry (0 when the
 * whole report fits). The router clamps wheel scrolling against this. */
int ui_gl_state_panel_max_scroll(const UiGlStatePanelView *view);

#endif /* UI_GL_STATE_PANEL_H */
