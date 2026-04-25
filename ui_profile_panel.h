/*
 * ui_profile_panel.h - CPU overhead profiling overlay panel.
 *
 * Measures CPU time spent in the major per-frame sections and renders a
 * compact overlay showing the last measured time and a smoothed average.
 * Toggle with Ctrl+W.
 */
#ifndef UI_PROFILE_PANEL_H
#define UI_PROFILE_PANEL_H

/* Render the profile panel overlay.  Draws nothing when the profile
 * panel mode is PROFILE_PANEL_OFF. */
void render_profile_panel(void);

#endif /* UI_PROFILE_PANEL_H */
