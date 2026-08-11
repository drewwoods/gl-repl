/*
 * ui_metrics.h - Shared UI geometry, font, and spacing constants.
 *
 * This header intentionally keeps only layout/spacing metrics. Palette tokens
 * live in `theme.h`.
 */
#ifndef UI_METRICS_H
#define UI_METRICS_H

#include "gl_includes.h"

#define LINE_H          18
#define CODE_MARGIN_X   10
#define CODE_MARGIN_Y   8

/* Half-width of the code-panel resize divider's grab band, in pixels: the
 * band spans [edge - N, edge + N] around the 1px divider line. Single
 * source of truth - the hover cursor (editor_input_point_on_code_panel_divider)
 * and the click classification (ui_text_panel_point_on_divider) must agree
 * pixel-for-pixel, or the resize cursor appears over pixels that don't
 * start a drag. */
#define UI_PANEL_DIVIDER_GRAB_PX 3

/* Left inset from a menu/dropdown/overlay chrome edge to its row text. */
#define MENU_TEXT_INSET_X 14

/* Dropdown / submenu inner padding. The height formula is
 * rows * LINE_H + 2 * DROPDOWN_PAD_Y; ordinal-from-y uses
 * DROPDOWN_PAD_Y as the top inset. */
#define DROPDOWN_PAD_Y    4

/* Total horizontal padding added to the widest label to get
 * dropdown / submenu width. */
#define DROPDOWN_PAD_X   28

/* Horizontal gap between the label column and shortcut column inside
 * a dropdown when shortcuts are present. */
#define DROPDOWN_SC_GAP  16

/* Baseline y-offset for text within the menu bar row. */
#define MENUBAR_TEXT_BASE_Y 3

/* Cap height of the menu bar's FONT_SMALL (the X11 8x13 cell): how far an
 * uppercase glyph reaches above the baseline. Icons drawn beside a label
 * center on MENUBAR_TEXT_BASE_Y + MENUBAR_CAP_H / 2, which is the label's
 * optical center - the bar's own center sits higher than the low-set
 * glyph row. */
#define MENUBAR_CAP_H 9

/* Horizontal padding for a top-level menu label (half each side). */
#define MENU_LABEL_PAD_X 18

#endif /* UI_METRICS_H */
