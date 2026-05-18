/*
 * ui_profile_panel.h - CPU profiling overlay panel.
 *
 * Renders a compact overlay showing per-frame CPU time breakdown across major
 * sections (flattening, rendering, UI, physics, etc.). Displays both the
 * instantaneous last-frame time and a smoothed exponential average for trend
 * analysis. Used for real-time performance monitoring and bottleneck detection.
 *
 * Profiling infrastructure: The underlying measurement is in prof.c/prof.h,
 * which uses platform timers (gettimeofday or equivalent) to bracket code
 * sections via prof_begin/prof_end. This module reads those measurements and
 * renders them as an overlay (top-left corner by default, position is
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
#ifndef UI_PROFILE_PANEL_H
#define UI_PROFILE_PANEL_H

typedef enum {
	PROFILE_PANEL_OFF = 0,
	PROFILE_PANEL_ON,
	PROFILE_PANEL_DETAILS,
	PROFILE_PANEL_MODE_COUNT
} UiProfilePanelMode;

#include "snapshot.h"

/* Render the CPU profile panel overlay once per frame from the supplied
 * snapshot. Reads measurements from prof.c and displays per-section CPU
 * times. Renders nothing if the profile panel is disabled. */
void ui_profile_panel_render(const UiRenderSnapshot *snap);

#endif /* UI_PROFILE_PANEL_H */
