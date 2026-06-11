/*
 * ui_layout.h - pure window layout geometry.
 *
 * Geometry queries for scene and code-panel rectangles. No GL or rendering
 * state; callers include this header when they only need window layout math.
 */
#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

#include "ui/core/layout_utils.h"

#define STATUSBAR_H 22

typedef enum {
	CODE_PANEL_LAYOUT_LEFT = 0,
	CODE_PANEL_LAYOUT_TOP,
	CODE_PANEL_LAYOUT_BOTTOM,
	CODE_PANEL_LAYOUT_HIDDEN,
	CODE_PANEL_LAYOUT_COUNT
} UiCodePanelLayout;

void ui_layout_code_panel_rect(int *x, int *y, int *w, int *h);
void ui_layout_scene_rect(int *x, int *y, int *w, int *h);

/* Shared code-panel chrome / floating-panel geometry helpers. */
void ui_layout_menu_bar_rect(int *x, int *y, int *w, int *h);

/* Currently-installed code-panel layout, clamped to a known enumerator. */
int  ui_layout_code_panel_layout_mode(void);

#endif /* UI_LAYOUT_H */
