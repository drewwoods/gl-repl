/*
 * glr_variable_panel_bridge.h - what the variable-panel peer needs from the app.
 *
 * The peer in src/subsystems/variable_panel/ owns the drag transaction (grab
 * row, track pixels, emit a requested value) and nothing else: it is
 * editor/REPL-independent so it links standalone in variable_panel_demo. Every
 * read and write that crosses into the live document goes through here.
 */
#ifndef GLR_VARIABLE_PANEL_BRIDGE_H
#define GLR_VARIABLE_PANEL_BRIDGE_H

#include "subsystems/variable_panel/variable_panel_drag.h"
#include "subsystems/variable_panel/variable_panel_state.h"

/* Point the peer's drag-begin at the REPL eval table for declared-variable
 * names + values. Idempotent; called from glr_ctrl_install_app_services. */
void glr_variable_panel_install_value_source(void);

/* Per-motion: apply a requested value to the live variable, no source
 * rewrite. Also owns the drag's one undo snapshot (pushed on the first
 * motion, at the drag-start value). */
void glr_variable_panel_apply_value_change(
        const VariablePanelValueChange *value_change);

/* 1 if declared-variable `row` sits on a `// @bool`-tagged declaration, so
 * the panel presents it as a checkbox and a press toggles it instead of
 * starting a scrub. Row indices are the eval table's, matching the panel's
 * row order. */
int glr_variable_panel_row_is_bool(int row);

/* Flip a `@bool` row between 1 and 0: applies the live value and rewrites the
 * declaration in one press, since there is no release to settle on. Pushes the
 * gesture's single undo snapshot. No-op for a row that is not bool-tagged. */
void glr_variable_panel_toggle_bool_row(int row);

/* On release: write the settled value back into the variable's declaration
 * row, once. A drag with no motion, or a variable with no declaration row,
 * does no work. */
void glr_variable_panel_persist_drag_value(const VariablePanelDragState *drag);

#endif /* GLR_VARIABLE_PANEL_BRIDGE_H */
