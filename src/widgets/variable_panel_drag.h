/*
 * variable_panel_drag.h - Small shared types for variable-panel drag updates.
 *
 * The drag handlers themselves live on `variable_panel_state.h`; this header is
 * just the neutral place for the per-call value-change payload they exchange.
 * Keeping the typedef here avoids duplicate definitions while letting the peer
 * state header and drag implementation share the same shape.
 */
#ifndef VARIABLE_PANEL_DRAG_H
#define VARIABLE_PANEL_DRAG_H

/* Per-motion value request emitted by the drag math. `name` identifies the
 * target predefined variable; `value` is the requested new numeric value. */
typedef struct VariablePanelValueChange_s {
    char  name[16];
    float value;
} VariablePanelValueChange;

#endif /* VARIABLE_PANEL_DRAG_H */
