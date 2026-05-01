/*
 * ui_variable_panel.h - Floating variable slider panel (read-only HUD).
 *
 * Renders a floating panel listing all declared predefined variables (float x,
 * y, z, etc.) with their current values. Each variable gets a draggable slider
 * row for interactive manipulation. Panel layout is compacted to show only the
 * variables in use; unused rows are skipped. Values are displayed both as
 * numeric text and as slider position (linear or logarithmic scale).
 *
 * Geometry: Panel floats in the top-right area of the viewport, non-modal
 * (doesn't block interaction with the scene or code panel). Each variable row
 * has a fixed height with name label on the left and draggable slider region
 * on the right. Color coding highlights the active drag row (via repl_var_drag.c
 * queries).
 *
 * Value mutation: This module is read-only rendering — it does NOT handle
 * dragging or value updates. Mutation is owned by repl_var_drag.c (which
 * implements begin/motion/reset for drag transactions) and repl_editor.c
 * (which calls drag handlers on mouse events). The panel just renders the
 * current state and reports geometry/hit-testing.
 *
 * Visibility: Panel can be toggled on/off via the REPL_CONFIG_VARIABLE_PANEL
 * config item (F-key shortcut). When off, rendering and hit-testing are no-ops.
 *
 * Hit-testing: ui_variable_panel_hit() identifies which variable row was
 * clicked (by y-coordinate in the panel). Returns the row index; repl_editor.c
 * then calls repl_var_drag_begin() to start a drag transaction on that row.
 *
 * Integration: ui_panels.c queries geometry (ui_variable_panel_rect) for
 * layout and hit-testing (ui_variable_panel_hit) for input routing. repl_var_drag.c
 * queries active drag state (repl_var_drag_active_var, log_mode) to highlight
 * the dragged row and signal log-scale mode to the renderer.
 */
#ifndef UI_VARIABLE_PANEL_H
#define UI_VARIABLE_PANEL_H

#include "ui_snapshot.h"

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
 * clicks; repl_editor.c then calls repl_var_drag_begin() with the row index
 * to start dragging. */
int  ui_variable_panel_hit(int gx, int gy, int *out_row);

#endif /* UI_VARIABLE_PANEL_H */
