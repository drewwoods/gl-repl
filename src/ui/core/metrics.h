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

/* Horizontal padding for a top-level menu label (half each side). */
#define MENU_LABEL_PAD_X 18

#endif /* UI_METRICS_H */
