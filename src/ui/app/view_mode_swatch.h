/*
 * view_mode_swatch.h - Animated 2D/3D view-mode toggle swatch.
 *
 * A small clickable cell pinned in the code-panel menu bar, immediately
 * left of the Replay button. It shows the current projection mode ("2D" /
 * "3D") in the Replay button's font/color, and animates the 2D<->3D
 * transition driven by the controller's existing projection blend
 * (glr_ctrl_view_projection_mix):
 *
 *   - 3D->2D: a cross-fade - "3D" dissolves toward the menu-bar background
 *     color while "2D" fades in (flat 2D, no cube).
 *   - 2D->3D: a lit cube rotates to reveal a face textured with "3D";
 *     the texture's background IS the menu-bar surface color, so the
 *     camera-facing face lights flat and dissolves into the bar.
 *
 * menu_bar.c owns layout/hit/route + the cell hover-bg and left divider
 * (the shared pin-button chrome); this module owns only the cell content
 * and its animation/texture state. Lives in ui/app (it knows view-mode
 * concepts); reads colors through the theme tokens, never hardcoded.
 */
#ifndef UI_VIEW_MODE_SWATCH_H
#define UI_VIEW_MODE_SWATCH_H

/* The visual the swatch shows, a pure function of (ortho_mode, mix). */
typedef enum {
    UI_VIEW_SWATCH_FLAT_3D = 0, /* settled 3D: flat "3D" text          */
    UI_VIEW_SWATCH_FLAT_2D,     /* settled 2D: flat "2D" text          */
    UI_VIEW_SWATCH_CROSSFADE,   /* 3D->2D: "3D"->bg, "2D" fades in     */
    UI_VIEW_SWATCH_CUBE         /* 2D->3D: lit cube reveals "3D" face  */
} UiViewSwatchMode;

/* Pure selector (unit-testable, no GL): classify the swatch visual and
 * write the 0..1 transition progress into *out_t (CROSSFADE: 2D fade-in;
 * CUBE: 3D reveal). ortho_mode is Render3dViewMode (0 = 3D, non-0 = 2D);
 * projection_mix is the blend in [0,1] (0 = ortho/2D, 1 = persp/3D). */
UiViewSwatchMode ui_view_mode_swatch_state(int ortho_mode,
                                           float projection_mix,
                                           float *out_t);

/* Preferred cell width in px (fits the wider of "2D"/"3D" + padding). */
int ui_view_mode_swatch_label_width(void);

/* Render the cell content (text / cross-fade / lit cube) into the menu-bar
 * cell, given in bottom-up window pixels. The caller (menu_bar.c) has
 * already drawn the cell hover bg and left divider. Expects the menu bar's
 * 2D ortho + GL_BLEND active; fully brackets any 3D/texture state it uses
 * so it can't leak into the rest of the 2D UI pass. */
void ui_view_mode_swatch_render(int cell_x, int cell_y, int cell_w,
                                int cell_h, int ortho_mode,
                                float projection_mix);

#endif /* UI_VIEW_MODE_SWATCH_H */
