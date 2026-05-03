/*
 * repl_layout.h - pure window layout geometry.
 *
 * Geometry queries for scene and code-panel rectangles. No GL or rendering
 * state; callers include this header when they only need window layout math.
 */
#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

void repl_layout_code_panel_rect(int *x, int *y, int *w, int *h);
void repl_layout_scene_rect(int *x, int *y, int *w, int *h);

#endif /* UI_LAYOUT_H */
