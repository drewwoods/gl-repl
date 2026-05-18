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

#include "snapshot.h"
#include "hit.h"

/* --- Rendering --- */

/* Per-frame render output: values the renderer discovers while drawing
 * that the controller actualizes after the render call. Today this
 * carries the editor cursor's window-pixel position, which the
 * floating autocomplete popup needs to anchor itself under the cursor.
 * The cursor pixel is not durable state — it's recomputed every frame
 * from the same wrap/segment math the renderer is already running, so
 * surfacing it as a per-frame output (rather than a state field
 * mutated mid-render) keeps `ui_*` pure.
 *
 * `cursor_valid` is 0 when the active input row didn't render this
 * frame (code panel hidden, edit row scrolled offscreen). The
 * controller leaves any prior cursor coords undisturbed in that
 * case — the controller simply leaves any prior cursor coords untouched for
 * that frame. */
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

/* Render the scene status banner from the supplied snapshot. */
void ui_panels_render_scene_status(const UiRenderSnapshot *snap);

/* Handle right-click in non-code-panel areas: open Config menu if clicked on
 * menu bar region. Returns 1 if consumed, 0 otherwise. mx, my are window
 * coordinates. */
int  ui_panels_handle_right_press(int mx, int my);

/* Pure hit-test: classify the pointer at (mx, my) as a `UiHit`.
 * glr_ctrl_router_handle_code_panel_hit is the canonical consumer — it
 * dispatches by UiHit.kind to the owning subsystem. `variable_count` is
 * supplied by the controller so UI hit-testing does not read the live
 * REPL variable table. */
UiHit ui_panels_hit_test(const UiRenderSnapshot *snap,
                         int mx, int my, int variable_count);

#endif /* UI_PANELS_H */
