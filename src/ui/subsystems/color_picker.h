/*
 * src/ui/subsystems/color_picker.h - Floating color-picker renderer and hit-test.
 *
 * Pure UI layer over the `ColorPickerView` peer snapshot. The renderer and
 * hit-test perform no live REPL/editor reads, no parse/commit work, and no
 * `_mut()` access - and depend on nothing in ui/app, so this subsystem links
 * cleanly in the standalone color_picker_demo. The inline code-panel swatch
 * (which needs ui/app's UiTransformer) lives in src/ui/app/color_swatch.c.
 *
 * The companion guard `check-color-picker-ui-isolation.sh` audits
 * `src/ui/subsystems/color_picker.c` for forbidden mutators and state reads.
 */
#ifndef UI_COLOR_PICKER_H
#define UI_COLOR_PICKER_H

#include "subsystems/color_picker/color_picker_state.h"
#include "ui/core/hit.h"

/* Hit kind the floating picker emits. Owned here (not in ui/app/hit.h) as a
 * fixed offset off UI_HIT_CORE_COUNT, in a reserved subsystem range clear of
 * ui/app's app kinds, so the renderer names it without depending on ui/app.
 * UiHit.kind is an int, so the ranges coexist. */
enum { UI_HIT_COLOR_SWATCH = UI_HIT_CORE_COUNT + 65 };

/* Render the floating picker overlay. Reads everything it needs from
 * `view`; does nothing when view->open is 0. viewport_w / _h are used
 * for the gl2d projection setup (matches the picker's window-space
 * coordinate model). */
void ui_color_picker_render(const ColorPickerView *view,
                            int viewport_w, int viewport_h);

/* Pure hit-test: classify (mx, my) as a UiHit for the floating picker.
 * Returns UI_HIT_COLOR_SWATCH when the pointer lands on the SV / hue /
 * alpha rect of the open picker; UI_HIT_NONE otherwise. viewport_h is
 * needed to translate GLUT screen-y to OpenGL y-up before comparing
 * against view->rects. */
UiHit ui_color_picker_hit_test(const ColorPickerView *view,
                               int mx, int my, int viewport_h);

#endif /* UI_COLOR_PICKER_H */