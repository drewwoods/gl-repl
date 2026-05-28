/*
 * ui_variable_panel.h - Floating variable slider panel renderer and hit-test.
 *
 * Draws the compact floating panel of declared predefined variables and classifies
 * pointer hits on its slider rows. The panel is a read-only view over current
 * variable values: visibility lives in the variable-panel peer subsystem, and
 * drag/value mutation lives in `subsystems/variable_panel/variable_panel_drag.h` plus controller
 * routing. UI only renders and returns `UiHit`.
 *
 * When visible, each declared variable gets one slider row with its current
 * numeric value and slider position. Unused slots are skipped, and the active
 * drag row is highlighted via the drag-state queries.
 */
#ifndef UI_VARIABLE_PANEL_H
#define UI_VARIABLE_PANEL_H

#include "ui/app/hit.h"
#include "ui/app/snapshot.h"

/* Render the variable panel with all declared variables and current values.
 * Reads only from the supplied snapshot. Rows are compacted (unused slots
 * skipped). Called once per frame if the variable panel is enabled. */
void ui_variable_panel_render(const UiRenderSnapshot *snap);

/* Query the variable panel's bounding rectangle (in window/screen coordinates).
 * Outputs panel position (px, py) and size (pw, ph). The caller supplies the
 * visible variable count so UI code does not read the live REPL variable table. */
void ui_variable_panel_rect_for_count(const UiRenderSnapshot *snap,
                                      int variable_count,
                                      int *px, int *py, int *pw, int *ph);

/* Hit-test a click in the variable panel. gx, gy are window/screen coordinates.
 * Returns the variable row index (0 = first declared variable) if a row was
 * clicked, or -1 if the click was outside the panel or between rows. out_row
 * is filled with the row index on success. Called by ui_panels.c on mouse
 * clicks; glr_ctrl then calls variable_panel_handle_drag_begin() with the
 * row index to start dragging. */
int  ui_variable_panel_hit_for_count(const UiRenderSnapshot *snap,
                                     int gx, int gy, int variable_count,
                                     int *out_row);

/* Pure hit-test: classify (mx, my) as a UiHit for the variable panel. Returns
 * UI_HIT_VARIABLE_SLIDER if the pointer lands on a slider row; item_idx carries
 * the row index. Returns UI_HIT_NONE if the panel is hidden or the pointer is
 * outside it. Reads layout/state only; never mutates. */
UiHit ui_variable_panel_hit_test(const UiRenderSnapshot *snap, int mx, int my, int variable_count);

#endif /* UI_VARIABLE_PANEL_H */