#ifndef GLR_CTRL_EXPORT_H
#define GLR_CTRL_EXPORT_H

#include "repl/export.h"

/* Fill a ReplExportLayout from current ui_layout_* / glr_state_* values.
 * The export pipeline reads layout as opaque integers, so callers build
 * the struct through this helper instead of duplicating layout/state
 * reads or reaching into app/UI storage directly. */
void glr_ctrl_fill_export_layout(ReplExportLayout *out);

#endif /* GLR_CTRL_EXPORT_H */
