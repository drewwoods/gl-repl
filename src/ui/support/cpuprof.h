/*
 * ui_cpuprof.h - CPU profiling overlay panel.
 *
 * Renders a compact overlay showing per-frame CPU time breakdown across major
 * sections (flattening, rendering, UI, physics, etc.). Displays both the
 * instantaneous last-frame time and a smoothed exponential average for trend
 * analysis. Used for real-time performance monitoring and bottleneck detection.
 *
 * Profiling infrastructure: The underlying measurement is in
 * src/support/cpuprof.c / src/support/cpuprof.h, which uses platform timers
 * (gettimeofday or equivalent) to bracket code sections via
 * prof_begin/prof_end. This module reads those measurements and renders
 * them as an overlay (top-left corner by default, position is
 * configurable).
 *
 * Visibility: Panel can be toggled on/off via Ctrl+W or the config menu
 * (GLR_CONFIG_CPU_PROFILE). When off, rendering is a no-op. When on, the
 * overlay appears alongside other floating panels (variable slider, color
 * picker, etc.) without blocking interaction.
 *
 * Display format: Shows time in milliseconds per frame, with a list of major
 * subsystems (flattening, rendering, UI, overlays, etc.) and their individual
 * times. Smoothing helps with noisy measurements from frame-to-frame variance.
 * Useful for detecting performance regressions or identifying which features
 * are most expensive to compute.
 */
#ifndef UI_CPUPROF_H
#define UI_CPUPROF_H

typedef enum {
	PROFILE_PANEL_OFF = 0,
	PROFILE_PANEL_ON,
	PROFILE_PANEL_DETAILS,
	PROFILE_PANEL_MODE_COUNT
} UiProfilePanelMode;

/* Panel width in pixels. Public so sibling panels (e.g. ui_memory_panel)
 * can shift left of this panel for side-by-side layout. */
#define PROFILE_PANEL_W  320

/* Narrow per-frame view (the 2D analog of SceneRenderConfig). The controller
 * resolves the panel's stacked anchor and bakes it into panel_x/panel_y, so
 * the renderer needs nothing from UiRenderSnapshot or ui/app — it links from
 * {support, ui/core} alone (see cpuprof_demo). */
typedef struct {
    int                window_w, window_h;
    UiProfilePanelMode mode;
    int                panel_x, panel_y;   /* resolved top-left, controller-baked */
} UiProfilePanelView;

/* Render the CPU profile panel overlay once per frame from the supplied view.
 * Reads measurements from src/support/cpuprof.c and displays per-section CPU
 * times. Renders nothing if the profile panel is disabled. */
void ui_profile_panel_render(const UiProfilePanelView *view);

/* Panel footprint in pixels, exposed so the controller (which owns
 * sibling-panel stacking) can resolve panel_x/panel_y without reaching into
 * the renderer's geometry. Height depends on the mode's visible row count. */
int  ui_profile_panel_width(void);
int  ui_profile_panel_height(UiProfilePanelMode mode);

#endif /* UI_CPUPROF_H */