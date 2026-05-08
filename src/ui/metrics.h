/*
 * ui_metrics.h - Shared UI font, spacing, and color constants.
 */
#ifndef UI_METRICS_H
#define UI_METRICS_H

#include <gl_includes.h>

#define LINE_H          18
#define CODE_MARGIN_X   10
#define CODE_MARGIN_Y   8

/* Height of the amber status strip along the bottom of the scene - used by
 * both ui_panels.c (var panel lift, code panel statusbar) and scene_render.c
 * (replay HUD lift) so the HUD clears the strip. */
#define STATUSBAR_H 22

/* Shared UI accent palette - kept here so menu bar and HUD use identical
 * values. #6fb36f is the design-bundle green used for the Replay button,
 * progress fill, and active example. */
#define UI_ACCENT_GREEN_R 0.435f
#define UI_ACCENT_GREEN_G 0.702f
#define UI_ACCENT_GREEN_B 0.435f

#endif /* UI_METRICS_H */