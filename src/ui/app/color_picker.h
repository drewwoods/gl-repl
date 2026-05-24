/*
 * src/ui/app/color_picker.h - Floating color-picker renderer and hit-test.
 *
 * Pure UI layer over the `ColorPickerView` peer snapshot plus controller-pushed
 * transformer entries for inline swatches. The renderer and hit-test perform no
 * live REPL/editor reads, no parse/commit work, and no `_mut()` access.
 *
 * The companion guard `check-color-picker-ui-isolation.sh` audits
 * `src/ui/app/color_picker.c` for forbidden mutators and state reads.
 */
#ifndef UI_COLOR_PICKER_H
#define UI_COLOR_PICKER_H

#include "subsystems/color_picker/color_picker_state.h"
#include "ui/app/editor.h"
#include "ui/app/hit.h"

/* Width of inline color-swatch boxes (shown in code panel). */
#define UI_COLOR_SWATCH_W 12

/* Render the floating picker overlay. Reads everything it needs from
 * `view`; does nothing when view->open is 0. viewport_w / _h are used
 * for the gl2d projection setup (matches the picker's window-space
 * coordinate model). */
void ui_color_picker_render(const ColorPickerView *view,
                            int viewport_w, int viewport_h);

/* Render an inline color swatch from a controller-pushed transformer
 * entry. `active_line` is the open picker's source-cmd index (-1 when
 * closed) so the renderer can highlight the swatch whose picker is
 * currently active without consulting peer state. */
void ui_color_picker_render_swatch(const UiTransformer *t,
                                   int sx, int sy,
                                   int active_line);

/* Pure hit-test: classify (mx, my) as a UiHit for the floating picker.
 * Returns UI_HIT_COLOR_SWATCH when the pointer lands on the SV / hue /
 * alpha rect of the open picker; UI_HIT_NONE otherwise. viewport_h is
 * needed to translate GLUT screen-y to OpenGL y-up before comparing
 * against view->rects. */
UiHit ui_color_picker_hit_test(const ColorPickerView *view,
                               int mx, int my, int viewport_h);

#endif /* UI_COLOR_PICKER_H */
