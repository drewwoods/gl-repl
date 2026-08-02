/*
 * ui_panels.h - Code-panel rendering, scene status, and top-level UI hit-test.
 *
 * This is the main 2D panel surface: it renders the code panel and scene status
 * banner, and it classifies pointer locations into a neutral `UiHit` through
 * `ui_panels_hit_test()`. The controller then routes that hit to the owning
 * subsystem (editor, variable panel, replay, scene, color picker, menu). UI does
 * not own input dispatch or mutation.
 *
 * `ui_panels_hit_test()` is pure classification over the frozen snapshot and
 * layout state. It checks floating/priority surfaces in order and fills the
 * per-kind fields documented in `ui/hit.h`. A small right-click wrapper remains
 * because menu-bar right-click is still treated as a Config-menu shortcut rather
 * than going through the same left-click hit surface.
 */
#ifndef UI_PANELS_H
#define UI_PANELS_H

#include "ui/app/snapshot.h"
#include "ui/app/hit.h"

/* --- Rendering --- */

/* Per-frame render output: values the renderer discovers while drawing
 * that the controller actualizes after the render call. Today this
 * carries the editor cursor's window-pixel position, which the
 * floating autocomplete popup needs to anchor itself under the cursor.
 * The cursor pixel is not durable state - it's recomputed every frame
 * from the same wrap/segment math the renderer is already running, so
 * surfacing it as a per-frame output (rather than a state field
 * mutated mid-render) keeps `ui_*` pure.
 *
 * `cursor_valid` is 0 when the active input row didn't render this
 * frame (code panel hidden, edit row scrolled offscreen). The
 * controller leaves any prior cursor coords untouched for that
 * frame. */
typedef struct UiCodePanelOutput {
    int cursor_px;
    int cursor_py;
    int cursor_valid;
} UiCodePanelOutput;

/* Render the code panel from the supplied snapshot. `out` is optional;
 * pass NULL when the caller doesn't need the cursor-pixel discovery
 * (test fixtures that aren't checking autocomplete anchoring). */
void ui_panels_render_code_panel(const UiRenderSnapshot *snap,
                                 UiCodePanelOutput *out);

/* Render the scene status banner from the supplied snapshot. Also draws
 * the persistent bottom "messages" button and, when toggled open, the
 * inline recent-message history list. */
void ui_panels_render_scene_status(const UiRenderSnapshot *snap);

/* Pure geometry for the bottom "messages" button, in OpenGL bottom-left
 * window coordinates. Returns 1 and fills the x, y, w, h out-params on
 * success, 0 when there is no button (empty history, degenerate scene
 * rect, or a modal prompt owns the strip). Shared by the renderer and the
 * hit-test so both agree, and unit-testable on its own. */
int ui_panels_status_history_button_rect(const UiRenderSnapshot *snap,
                                          int *x, int *y, int *w, int *h);

/* Pure hit-test: classify the pointer at (mx, my) as a `UiHit`.
 * glr_ctrl_router_handle_code_panel_hit is the canonical consumer - it
 * dispatches by UiHit.kind to the owning subsystem. `variable_count` is
 * supplied by the controller so UI hit-testing does not read the live
 * REPL variable table. */
UiHit ui_panels_hit_test(const UiRenderSnapshot *snap,
                         int mx, int my, int variable_count);

/* Reverse-render-order hit pass for surfaces painted after the floating
 * OpenGL-state inspector. Returns actionable hits where a front panel has
 * controls, and UI_HIT_OVERLAY_CHROME for otherwise-inert panel pixels, so
 * the router can keep those pixels from falling through to the inspector.
 * UI_HIT_NONE means no later-rendered surface owns the point. */
UiHit ui_panels_hit_test_above_gl_state(const UiRenderSnapshot *snap,
                                        int mx, int my,
                                        int variable_count);

#endif /* UI_PANELS_H */
