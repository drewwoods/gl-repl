/*
 * ui_metrics.h - Shared UI font, spacing, and color constants.
 */
#ifndef UI_METRICS_H
#define UI_METRICS_H

#include <gl_includes.h>

#define FONT_MONO       GLUT_BITMAP_9_BY_15
#define FONT_SMALL      GLUT_BITMAP_8_BY_13
#define FONT_W          9
#define FONT_H          15
#define FONT_SMALL_W    8
#define FONT_SMALL_H    13
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

/* Max brightness (V in HSV) allowed for glClearColor channels.
 * Since max(r,g,b) == V, capping V caps all channels. */
#define CP_CLEAR_MAX_V 0.1f

#endif /* UI_METRICS_H */