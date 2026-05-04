/*
 * ui_layout.h - pure window layout geometry.
 *
 * Geometry queries for scene and code-panel rectangles. No GL or rendering
 * state; callers include this header when they only need window layout math.
 */
#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

typedef enum {
	CODE_PANEL_LAYOUT_LEFT = 0,
	CODE_PANEL_LAYOUT_TOP,
	CODE_PANEL_LAYOUT_BOTTOM,
	CODE_PANEL_LAYOUT_HIDDEN,
	CODE_PANEL_LAYOUT_COUNT
} CodePanelLayout;

#define CFG_DEFAULT_PANEL_FRAC 0.45f

void repl_layout_code_panel_rect(int *x, int *y, int *w, int *h);
void repl_layout_scene_rect(int *x, int *y, int *w, int *h);

#endif /* UI_LAYOUT_H */
