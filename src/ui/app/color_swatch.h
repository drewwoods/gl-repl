/*
 * ui_app_color_swatch.h - Inline code-panel color swatch renderer.
 *
 * Draws the little color square shown inline in the code panel for an
 * editable glColor/gluColor/glClearColor line. It reads the app-owned
 * UiTransformer, so it lives in ui/app rather than the color_picker
 * subsystem (which must stay free of ui/app to link in color_picker_demo).
 */
#ifndef UI_APP_COLOR_SWATCH_H
#define UI_APP_COLOR_SWATCH_H

#include "ui/app/editor.h"   /* UiTransformer, TRANSFORMER_COLOR_PICKER */

/* Width of inline color-swatch boxes (shown in the code panel). */
#define UI_COLOR_SWATCH_W 12

/* Render an inline color swatch from a controller-pushed transformer entry.
 * `active_line` is the open picker's source-cmd index (-1 when closed) so the
 * swatch whose picker is currently active gets a highlight ring. */
void ui_color_picker_render_swatch(const UiTransformer *t,
                                   int sx, int sy,
                                   int active_line);

#endif /* UI_APP_COLOR_SWATCH_H */
