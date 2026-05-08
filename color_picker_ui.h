/*
 * color_picker_ui.h - Floating color-picker renderer + hit-test.
 *
 * Pure UI layer over the `color_picker.h` peer. Receives state via
 * `ColorPickerView`; performs no live REPL/editor reads, no parser
 * or commit calls, no _mut() accessors.
 *
 * The narrow signatures below replace the snapshot-shaped API used
 * during the J-phase split: callers pass exactly the values the
 * renderer/hit-test need.
 *
 * The companion guard `check-color-picker-ui-isolation.sh` audits
 * `color_picker_ui*.c` for forbidden mutators / state reads.
 */
#ifndef COLOR_PICKER_UI_H
#define COLOR_PICKER_UI_H

#include "color_picker.h"
#include "ui/editor.h"
#include "ui/hit.h"

/* Width of inline color-swatch boxes (shown in code panel). */
#define UI_COLOR_SWATCH_W 12

/* Render the floating picker overlay. Reads everything it needs from
 * `view`; does nothing when view->open is 0. viewport_w / _h are used
 * for the gl2d projection setup (matches the picker's window-space
 * coordinate model). */
void color_picker_ui_render(const ColorPickerView *view,
                            int viewport_w, int viewport_h);

/* Render an inline color swatch from a controller-pushed transformer
 * entry. `active_line` is the open picker's source-cmd index (-1 when
 * closed) so the renderer can highlight the swatch whose picker is
 * currently active without consulting peer state. */
void color_picker_ui_render_swatch(const EditorTransformer *t,
                                   int sx, int sy,
                                   int active_line);

/* Pure hit-test: classify (mx, my) as a UiHit for the floating picker.
 * Returns UI_HIT_COLOR_SWATCH when the pointer lands on the SV / hue /
 * alpha rect of the open picker; UI_HIT_NONE otherwise. viewport_h is
 * needed to translate GLUT screen-y to OpenGL y-up before comparing
 * against view->rects. */
UiHit color_picker_ui_hit_test(const ColorPickerView *view,
                               int mx, int my, int viewport_h);

#endif /* COLOR_PICKER_UI_H */
