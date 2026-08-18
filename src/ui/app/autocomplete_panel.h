/*
 * ui_autocomplete_panel.h - Autocomplete popup renderer.
 *
 * Draws the floating completion list and parameter hint popup from the frozen
 * autocomplete state already carried in `UiRenderSnapshot`. This module does not
 * compute matches, mutate selection, or accept completions; it is the view for
 * the existing autocomplete model.
 *
 * The caller also supplies the same-frame cursor pixel position so the popup can
 * anchor under the active edit cursor without storing layout-derived coordinates
 * as durable state.
 */
#ifndef UI_AUTOCOMPLETE_PANEL_H
#define UI_AUTOCOMPLETE_PANEL_H

#include "ui/app/snapshot.h"

/* Compute bounding rectangle in OpenGL bottom-left coordinates (out_x, out_y as
 * bottom-left origin) for hit testing. Includes the candidate list box and the
 * bottom "Tab to accept" hint caption. */
void ui_autocomplete_panel_rect(const EditorAutocompleteState *ac,
                                int cursor_px, int cursor_py,
                                int *out_x, int *out_y, int *out_w, int *out_h);

/* Returns 1 if (gl_x, gl_y) in bottom-left OpenGL window coordinates lands inside the popup rect. */
int  ui_autocomplete_panel_hit_test(const EditorAutocompleteState *ac,
                                    int cursor_px, int cursor_py,
                                    int gl_x, int gl_y);

/* Render the autocomplete popup once per frame from the supplied snapshot.
 * Performs no live REPL state reads. Renders nothing if autocomplete is not
 * active (no matches).
 *
 * `cursor_px`/`cursor_py` are window-pixel coordinates of the editor
 * cursor - typically the `UiCodePanelOutput.cursor_px/_py` produced by
 * the same-frame `ui_panels_render_code_panel`. They're per-frame
 * transients (not snapshot state) so the autocomplete popup anchors
 * under the live cursor without snapshot staleness across renderers. */
void ui_autocomplete_panel_render(const UiRenderSnapshot *snap,
                                  int cursor_px, int cursor_py);

#endif /* UI_AUTOCOMPLETE_PANEL_H */
