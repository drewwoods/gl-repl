/*
 * variable_panel_drag.h - Variable slider drag transaction (canonical
 *                         entry points live alongside the peer in
 *                         variable_panel.h).
 *
 * Phase J7: the public drag API lives on variable_panel.h:
 *   variable_panel_drag_active() / _active_var() / _log_mode()
 *   variable_panel_handle_drag_begin() / _motion() / _reset()
 *
 * This header is currently a placeholder for the per-call value
 * struct typedef referenced by the canonical handler signature.
 * variable_panel_drag.c remains the implementation file (linear /
 * log scaling logic).
 */
#ifndef VARIABLE_PANEL_DRAG_H
#define VARIABLE_PANEL_DRAG_H

/* Sole definition of the per-call value-change struct.
 * variable_panel_state.h includes this header rather than re-typedefing
 * it — a duplicate typedef is a C11 feature that
 * -std=c99 -pedantic-errors rejects. */
typedef struct VariablePanelValueChange_s {
    char  name[16];
    float value;
} VariablePanelValueChange;

#endif /* VARIABLE_PANEL_DRAG_H */
