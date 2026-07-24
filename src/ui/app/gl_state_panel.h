/*
 * src/ui/app/gl_state_panel.h - Floating OpenGL-state popup table.
 *
 * Pure renderer + hit-test over a controller-built view: draws the
 * right-click OpenGL-state report as a floating table (state name, current
 * value, and — expanded via the clickable header chip, collapsed by
 * default — the OpenGL 2.1 default and latest change source columns)
 * anchored near the click position, wheel-scrollable when the report is
 * taller than the window (flyout-style right-edge scrollbar hint). The fold
 * itself lives in src/repl/gl_state_inspector.c; open/close/scroll/expand
 * chrome lives on UiState (ui_state_gl_state_inspector*).
 */
#ifndef UI_GL_STATE_PANEL_H
#define UI_GL_STATE_PANEL_H

#include "repl/gl_state_inspector.h"

/* Narrow per-frame view built by the controller
 * (glr_ctrl_build_gl_state_panel_view). anchor_px/anchor_py are the
 * popup's preferred top-left in y-up OpenGL window coords (the
 * controller flips the stored GLUT click y); the renderer clamps the
 * popup into the window. scroll_rows is the first visible semantic report row
 * (clamped by render/hit against the solved visual-line capacity; the
 * modelview matrix remains one report row but renders as four lines).
 * details_expanded widens the table with the default/source columns
 * (collapsed the popup shows only state + current, keeping it narrow).
 * setup_expanded folds in the generated init()/display() rows (collapsed
 * the popup shows only what the program itself wrote — see the report's
 * user_row_count partition; the generated group is typically the large
 * majority, so this is what keeps the popup readable).
 * `report` stays valid for the frame — it points at controller-owned
 * storage rebuilt each frame from the flat program. */
typedef struct {
    int visible;
    int window_w, window_h;
    int anchor_px, anchor_py;
    int scroll_rows;
    int details_expanded;
    int setup_expanded;
    /* Parallel to report->rows: the code panel's gutter label for each row's
     * latest-change line, or -1 to fall back to the document index. The
     * controller resolves these because the gutter label counts the derived-C
     * chrome rows that code focus hides, so it is not report-side data. */
    const int *source_gutter_labels;
    const ReplGlStateReport *report;
} UiGlStatePanelView;

/* Render the popup table once per frame. No-op when view->visible is 0.
 * Program-authored rows that differ from the OpenGL 2.1 default draw their
 * current value in the warning accent; explicit writes of the default value
 * draw in the OK accent so touched-ness stays visible either way. Generated
 * setup rows keep that distinction in the muted palette rather than the
 * accents — nearly all of them differ from the GL default, so accenting the
 * group would carry no signal. */
void ui_gl_state_panel_render(const UiGlStatePanelView *view);

/* Pure hit-test: 1 when (mx, my) — GLUT screen coords, y-down — lands
 * inside the open popup's frame; 0 otherwise (including when the popup
 * is closed). The router uses this for click-away dismiss and to scope
 * wheel scrolling to the popup. */
int ui_gl_state_panel_hit_test(const UiGlStatePanelView *view,
                               int mx, int my);

/* Pure hit-test for the header's expand/collapse chip ("[+] ..." /
 * "[-] ..."): 1 when (mx, my) — GLUT screen coords, y-down — lands on
 * the chip's header-row cell. The router flips
 * ui_state_gl_state_inspector_toggle_details() on a left press there
 * (the press is already swallowed by the popup's surface hit-test). */
int ui_gl_state_panel_hit_test_details_toggle(const UiGlStatePanelView *view,
                                              int mx, int my);

/* Pure hit-test for the title row's setup-fold chip ("[+] N from setup" /
 * "[-] N from setup"): 1 when (mx, my) — GLUT screen coords, y-down — lands
 * on the chip cell. The router flips
 * ui_state_gl_state_inspector_toggle_setup() on a left press there. Returns 0
 * when the report has no generated rows to fold. */
int ui_gl_state_panel_hit_test_setup_toggle(const UiGlStatePanelView *view,
                                            int mx, int my);

/* Largest valid scroll_rows for the view's solved geometry (0 when the
 * whole report fits). The router clamps wheel scrolling against this. */
int ui_gl_state_panel_max_scroll(const UiGlStatePanelView *view);

#endif /* UI_GL_STATE_PANEL_H */
