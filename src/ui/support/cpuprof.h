/*
 * ui_cpuprof.h - CPU/GPU profiling overlay panel.
 *
 * Renders a compact overlay showing per-frame time breakdown across major
 * sections (flattening, rendering, UI, etc.). Each row shows three smoothed
 * exponential averages: CPU wall time, GPU elapsed time (for the
 * GPU-bracketed subset, when timer queries are available — "--" otherwise),
 * and Max, the worse of the two. Used for real-time performance monitoring
 * and bottleneck detection. GPU windows overlap on pipelined/tile-deferred
 * GPUs, so the GPU column is a relative-hotspot signal, not an additive
 * budget — see the NOTE in support/gpuprof.h.
 *
 * Profiling infrastructure: The CPU measurement is in
 * src/support/cpuprof.c / src/support/cpuprof.h, which uses platform timers
 * to bracket code sections via prof_begin/prof_end; the GPU measurement is
 * in src/support/gpuprof.c / src/support/gpuprof.h (asynchronous
 * GL_TIME_ELAPSED timer queries over the same sections). This module reads
 * those measurements and renders them as an overlay (top-left corner by
 * default, position is configurable).
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

/* Compute-profile detail levels (Ctrl+W cycle). The FPS plot is a separate
 * floating panel (ui_fps_panel_*) shown at every non-OFF level; the section
 * listing panel joins it from SECTIONS up. */
typedef enum {
	PROFILE_PANEL_OFF = 0,
	PROFILE_PANEL_PLOT,      /* FPS plot panel only */
	PROFILE_PANEL_SECTIONS,  /* plot + top-level section listing */
	PROFILE_PANEL_DETAILS,   /* plot + full nested section listing */
	PROFILE_PANEL_MODE_COUNT
} UiProfilePanelMode;

/* Panel width in pixels (label column + three 72px value columns). Public
 * so sibling panels (e.g. ui_memory_panel) can shift left of this panel
 * for side-by-side layout. */
#define PROFILE_PANEL_W  384

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

/* --- FPS plot panel ---
 *
 * A separate floating panel (its own overlay-layout slot) graphing FPS over
 * the last 10 s / 1 min / 10 min as three overlaid series, memory-panel
 * style. Data comes from the prof_fps_* history in support/cpuprof.c (fed
 * by prof_frame_tick); visible at every Compute-profile level except OFF. */
typedef struct {
    int window_w, window_h;
    int visible;            /* profile mode != PROFILE_PANEL_OFF */
    int panel_x, panel_y;   /* resolved position, controller-baked */
} UiFpsPanelView;

void ui_fps_panel_render(const UiFpsPanelView *view);
int  ui_fps_panel_width(void);
int  ui_fps_panel_height(void);

#endif /* UI_CPUPROF_H */