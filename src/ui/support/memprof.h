/*
 * ui_memprof.h - Process memory overlay panel.
 *
 * Sibling of ui_profile_panel that visualizes the process memory signal over
 * time: RSS natively, Wasm sbrk position in Emscripten.
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

/* Narrow per-frame view (the 2D analog of Render3dRenderConfig). The
 * controller resolves the panel's stacked anchor and bakes it into
 * panel_x/panel_y, so the renderer needs nothing from UiRenderSnapshot or
 * ui/app — it links cleanly against {support, ui/core} alone. */
typedef struct {
    int               window_w, window_h;
    UiMemoryPanelMode mode;
    int               panel_x, panel_y;   /* resolved top-left, controller-baked */
} UiMemoryPanelView;

/* Render the memory profile panel overlay once per frame from the supplied
 * view. Reads samples from src/support/memprof.c and draws current,
 * baseline, delta, and limit rows plus a time-anchored line graph. Renders
 * nothing if the memory panel is disabled. */
void ui_memory_panel_render(const UiMemoryPanelView *view);

/* Panel footprint in pixels, exposed so the controller (which owns
 * sibling-panel stacking) can resolve panel_x/panel_y without reaching
 * into the renderer's private geometry constants. */
int  ui_memory_panel_width(void);
int  ui_memory_panel_height(void);

#endif /* UI_MEMPROF_H */
