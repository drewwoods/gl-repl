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

#include "support/cpuprof.h"   /* ProfHistogramBin, PROF_HISTOGRAM_BIN_COUNT */

/* Compute-profile surfaces (Ctrl+W cycle). Each level adds one floating panel:
 * FPS shows the frame-rate graph, SECTIONS adds the full collapsible timing
 * tree, and HISTOGRAM adds the section-distribution graph. */
typedef enum {
	PROFILE_PANEL_OFF = 0,
	PROFILE_PANEL_FPS,       /* FPS plot panel only */
	PROFILE_PANEL_SECTIONS,  /* FPS + full collapsible section listing */
	PROFILE_PANEL_HISTOGRAM, /* FPS + section listing + histograms */
	PROFILE_PANEL_MODE_COUNT
} UiProfilePanelMode;

/* Panel width in pixels (label column + three 72px value columns). Public
 * so sibling panels (e.g. ui_memory_panel) can shift left of this panel
 * for side-by-side layout. */
#define PROFILE_PANEL_W  384

/* Session-only presentation state for the section tree. One bit per
 * ProfSection marks a branch whose descendants are hidden; profiling itself
 * continues unchanged while a branch is collapsed. */
typedef unsigned long long UiProfileCollapseMask;

enum {
    UI_PROFILE_PANEL_HIT_NONE = -1,
    UI_PROFILE_PANEL_TOGGLE_ALL = -2
};

/* Narrow per-frame view (the 2D analog of Render3dRenderConfig). The controller
 * resolves the panel's stacked anchor and bakes it into panel_x/panel_y, so
 * the renderer needs nothing from UiRenderSnapshot or ui/app — it links from
 * {support, ui/core} alone (see cpuprof_demo). */
typedef struct {
    int                window_w, window_h;
    UiProfilePanelMode mode;
    UiProfileCollapseMask collapsed_sections;
    int                panel_x, panel_y;   /* resolved bottom-left, controller-baked */
} UiProfilePanelView;

/* Render the CPU profile panel overlay once per frame from the supplied view.
 * Reads measurements from src/support/cpuprof.c and displays per-section CPU
 * times. Renders nothing if the profile panel is disabled. */
void ui_profile_panel_render(const UiProfilePanelView *view);

/* Hit-test an interactive section-tree branch. Returns its ProfSection index,
 * UI_PROFILE_PANEL_TOGGLE_ALL for the header control, or
 * UI_PROFILE_PANEL_HIT_NONE. mx/my use GLUT window coordinates (y down).
 * ui_profile_panel_toggle_mask() is the pure state transition applied by the
 * controller for either result. */
int ui_profile_panel_hit_test(const UiProfilePanelView *view, int mx, int my);
UiProfileCollapseMask ui_profile_panel_toggle_mask(
    UiProfileCollapseMask collapsed_sections, int toggle_target);

/* Panel footprint in pixels, exposed so the controller (which owns
 * sibling-panel stacking) can resolve panel_x/panel_y without reaching into
 * the renderer's geometry. Height depends on the mode's visible row count. */
int  ui_profile_panel_width(void);
int  ui_profile_panel_height(UiProfilePanelMode mode);
int  ui_profile_panel_height_collapsed(UiProfilePanelMode mode,
                                       UiProfileCollapseMask collapsed_sections);

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

/* --- Section histogram panel ---
 *
 * A third floating panel (its own overlay-layout slot) drawing the timing
 * histograms from support/cpuprof.c as one graph: every top-level (depth 0)
 * section overlaid, additively blended, so their distributions can be compared
 * directly instead of through the section listing's single EMA number. Nested
 * detail sections are never plotted — forty overlaid series would be noise —
 * so it is shown only by the final HISTOGRAM mode.
 *
 * Both axes are logarithmic. Time (x) because the bins themselves are
 * log-spaced and the axis is linear in bin index: the sections span four or
 * five decades at once, and a linear time axis would pile every cheap one into
 * a single indistinguishable bar at the origin. Counts (y) because a section
 * that spends nearly every frame in one bin would otherwise flatten every
 * spread-out neighbour to the baseline. */
typedef struct {
    int window_w, window_h;
    int visible;            /* profile mode is PROFILE_PANEL_HISTOGRAM */
    int panel_x, panel_y;   /* resolved position, controller-baked */
} UiHistogramPanelView;

void ui_histogram_panel_render(const UiHistogramPanelView *view);
int  ui_histogram_panel_width(void);
int  ui_histogram_panel_height(void);

/* Hit-test the header's "[reset]" control. Returns 1 when mx/my (GLUT window
 * coordinates, y down) land on it, else 0. Pure classification — the
 * controller routes the hit to prof_histogram_reset(), which clears the
 * cumulative sample histograms (the section listing's EMAs are unaffected). */
int  ui_histogram_panel_hit_test(const UiHistogramPanelView *view,
                                 int mx, int my);

/* The panel's x-axis policy, exposed for tests.
 *
 * Writes the inclusive bin range the plot draws: the occupied bins across
 * every plotted series, widened to a minimum span so a one-bin distribution is
 * still drawable. Returns 0 (leaving the outputs untouched) when no plotted
 * series has any samples.
 *
 * No percentile trim is needed, because the bins are log-spaced: the axis is
 * linear in bin index and so logarithmic in time, and an outlier lands a
 * bounded number of bins to the right rather than 30x further along. It costs
 * the axis a slice of width instead of collapsing every real distribution into
 * the leftmost pixel, so the panel can show the full range including the tail.
 *
 * Reads the live histograms rather than taking them as arguments, so it is not
 * pure; it is separated out because the range policy is the panel's one piece
 * of non-obvious logic. */
int ui_histogram_axis_range(int *lo_bin, int *hi_bin);

#endif /* UI_CPUPROF_H */
