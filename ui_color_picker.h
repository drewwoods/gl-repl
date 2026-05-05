/*
 * ui_color_picker.h - Floating color-picker renderer + hit-test.
 *
 * State, lifecycle, and writeback live on the peer (color_picker.h);
 * this header only declares the renderers and the snapshot-shaped
 * hit-test seam consumed by ui_panels.c / imrepl_ctrl.c.
 *
 * The next phase renames this TU to color_picker_ui.c/h, narrows the
 * render signature to take const ColorPickerView * directly (instead
 * of the full snapshot), and adds an isolation guard.
 */
#ifndef UI_COLOR_PICKER_H
#define UI_COLOR_PICKER_H

#include "ui_editor.h"
#include "ui_hit.h"
#include "ui_snapshot.h"

/* Width of inline color swatch boxes (shown in code panel). */
#define UI_COLOR_SWATCH_W 12

/* Render the color picker overlay (RGB/RGBA sliders) once per frame.
 * Reads peer state through color_picker_view(). Renders nothing when
 * the picker is closed. */
void ui_color_picker_render(const UiRenderSnapshot *snap);

/* Render an inline color swatch from a controller-pushed transformer
 * entry. sx, sy are pixel coordinates in the code panel. */
void ui_color_picker_render_swatch(const EditorTransformer *t,
                                   int sx, int sy);

/* Pure hit-test: classify (mx, my) as a UiHit for the floating color
 * picker. Returns UI_HIT_COLOR_SWATCH when the pointer lands on the
 * SV / hue / alpha rect of an open picker; cmd_idx carries the
 * active picker line, item_idx encodes the slider region (1=SV,
 * 2=hue, 3=alpha). UI_HIT_NONE when the picker is closed or the
 * pointer is outside its rects. Reads peer state only; never mutates. */
UiHit ui_color_picker_hit_test(int mx, int my);

#endif /* UI_COLOR_PICKER_H */
