/*
 * ui_memory_panel.h - Process memory overlay panel.
 *
 * Sibling of ui_profile_panel that visualizes process RSS/VSZ over time.
 * Reads samples + baseline from src/support/memprof.c. Three modes:
 * Off / On / Details (v1 Details adds nothing extra over On - room
 * reserved for a future per-allocator breakdown without enum migration).
 *
 * Toggle: Config > "Memory profile" and Ctrl+Shift+M.
 */
#ifndef UI_MEMORY_PANEL_H
#define UI_MEMORY_PANEL_H

typedef enum {
    MEMORY_PANEL_OFF = 0,
    MEMORY_PANEL_ON,
    MEMORY_PANEL_DETAILS,
    MEMORY_PANEL_MODE_COUNT
} UiMemoryPanelMode;

#include "ui/app/snapshot.h"

/* Render the memory profile panel overlay once per frame from the supplied
 * snapshot. Reads samples from src/support/memprof.c and draws three text
 * rows (current/baseline/delta) plus a time-anchored line graph. Renders
 * nothing if the memory panel is disabled. */
void ui_memory_panel_render(const UiRenderSnapshot *snap);

#endif /* UI_MEMORY_PANEL_H */
