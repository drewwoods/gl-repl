/*
 * ui_metrics.h - Shared UI geometry, font, and spacing constants.
 *
 * This header intentionally keeps only layout/spacing metrics. Palette tokens
 * live in `theme.h`.
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

/* Scene tab strip height. MUST equal LINE_H: the visible-row count is
 * integer division by LINE_H, so only an exact-LINE_H reserve drops the
 * count by exactly one when the band is present. Keeps the band
 * visually uniform with the menu bar too (implemented per
 * tabbed-code-panel plan §3). */
#define TAB_STRIP_H LINE_H

/* UI colors live in theme.h. Use `ui_clr(UI_TOK_*)` / `ui_clr_a(...)` for palette
 * lookups instead of adding color constants here. */

#endif /* UI_METRICS_H */
