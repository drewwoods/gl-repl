/*
 * ui_variable_panel.h - Floating variable slider panel (renderer + hit-test).
 *
 * Renders a floating panel listing every declared predefined variable
 * (float x, y, z, …) with current values. Each variable gets a draggable
 * slider row for interactive manipulation. Panel layout is compacted —
 * unused rows are skipped. Values are displayed both numerically and as
 * a slider position (linear or logarithmic scale).
 *
 * Target contract (Phase E onward):
 *
 *   UI renders and reports `UiHit` (UI_HIT_VARIABLE_SLIDER with the
 *   variable row in `item_idx`). `imrepl_ctrl` routes the hit to the
 *   variable_panel peer subsystem (Phase F), which owns the visibility
 *   flag and drag transaction. The renderer reads only — it does not
 *   own input dispatch or mutation.
 *
 * Hit-test: ui_variable_panel_hit_test() returns UI_HIT_VARIABLE_SLIDER
 * with item_idx = row index when the panel is visible and the pointer
 * lands on a row.
 *
 * Geometry: Panel floats in the top-right area of the viewport,
 * non-modal (doesn't block interaction with the scene or code panel).
 * Each row has a fixed height with the name label on the left and a
 * draggable slider region on the right. Color coding highlights the
 * active drag row (via variable_panel_drag.c queries).
 *
 * Value mutation lives outside this module. Today variable_panel_drag.c
 * implements drag transactions and repl_editor.c forwards mouse events
 * — those will collapse into the variable_panel peer in Phase F.
 *
 * Visibility: Panel can be toggled on/off via the GLR_CONFIG_VARIABLE_PANEL
 * config item (F-key shortcut). When off, rendering and hit-testing
 * are no-ops.
 */
#ifndef UI_VARIABLE_PANEL_H
#define UI_VARIABLE_PANEL_H

#include "hit.h"
#include "snapshot.h"

/* Render the variable panel with all declared variables and current values.
 * Reads only from the supplied snapshot. Rows are compacted (unused slots
 * skipped). Called once per frame if the variable panel is enabled. */
void ui_variable_panel_render(const UiRenderSnapshot *snap);

/* Query the variable panel's bounding rectangle (in window/screen coordinates).
 * Outputs panel position (px, py) and size (pw, ph). Used by ui_panels.c for
 * layout, hit-testing, and determining input priority (e.g., mouse motion over
 * the panel should go to variable drag, not camera). */
void ui_variable_panel_rect(int *px, int *py, int *pw, int *ph);

/* Hit-test a click in the variable panel. gx, gy are window/screen coordinates.
 * Returns the variable row index (0 = first declared variable) if a row was
 * clicked, or -1 if the click was outside the panel or between rows. out_row
 * is filled with the row index on success. Called by ui_panels.c on mouse
 * clicks; imrepl_ctrl then calls variable_panel_handle_drag_begin() with the
 * row index to start dragging. */
int  ui_variable_panel_hit(int gx, int gy, int *out_row);

/* Pure hit-test: classify (mx, my) as a UiHit for the variable panel.
 *
 * Phase E commit 29 entry. Returns UI_HIT_VARIABLE_SLIDER if the pointer
 * lands on a slider row; item_idx carries the row index. Returns UI_HIT_NONE
 * if the panel is hidden or the pointer is outside it. Reads layout / state
 * only; never mutates. */
UiHit ui_variable_panel_hit_test(int mx, int my);

#endif /* UI_VARIABLE_PANEL_H */
