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

#include "config.h"  /* REPL_PREDEF_NAME_MAX */

/* Per-motion value request emitted by the drag math. `name` identifies the
 * target predefined variable; `value` is the requested new numeric value. */
typedef struct VariablePanelValueChange_s {
    char  name[REPL_PREDEF_NAME_MAX];
    float value;
} VariablePanelValueChange;

/* Value source the drag handlers read the declared-variable name + value
 * from at drag-begin. Installed by the host (the controller wires a
 * REPL-backed reader) so the peer needn't reach into the REPL eval table
 * directly - that keeps src/subsystems/variable_panel linkable from the
 * standalone variable_panel_demo, which installs its own in-memory reader.
 * Unset = drag-begin is a no-op (no variable to grab). */
typedef struct {
    /* Fill name_out (capped to name_cap) and value_out for declared-variable
     * `row`; return 1 on success, 0 if row is out of range. */
    int (*read_row)(int row, char *name_out, int name_cap, float *value_out);
} VariablePanelValueSource;

void variable_panel_install_value_source(const VariablePanelValueSource *src);

#endif /* VARIABLE_PANEL_DRAG_H */
