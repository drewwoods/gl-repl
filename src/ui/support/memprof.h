/*
 * ui_memprof.h - Process memory overlay panel.
 *
 * Sibling of ui_profile_panel that visualizes process RSS over time.
 * Reads samples + baseline from src/support/memprof.c.
 *
 * Toggle: Config > "Memory profile" and Ctrl+Shift+W (matches the
 * CPU profile panel's Ctrl+W with Shift added).
 */
#ifndef UI_MEMPROF_H
#define UI_MEMPROF_H

typedef enum {
    MEMORY_PANEL_OFF = 0,
    MEMORY_PANEL_ON,
    MEMORY_PANEL_MODE_COUNT
} UiMemoryPanelMode;

#include "ui/app/snapshot.h"

/* Render the memory profile panel overlay once per frame from the supplied
 * snapshot. Reads samples from src/support/memprof.c and draws three text
 * rows (current/baseline/delta) plus a time-anchored line graph. Renders
 * nothing if the memory panel is disabled. */
void ui_memory_panel_render(const UiRenderSnapshot *snap);

#endif /* UI_MEMPROF_H */