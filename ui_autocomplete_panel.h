/*
 * repl_autocomplete_panel.h -- Rendering side of autocomplete.
 *
 * The match computation, selection state, and parameter hints live
 * in the model-only repl_autocomplete.c.  This panel reads that state
 * and draws the popup; it performs no mutation.
 */
#ifndef UI_AUTOCOMPLETE_PANEL_H
#define UI_AUTOCOMPLETE_PANEL_H

void ui_autocomplete_panel_render(void);

#endif /* UI_AUTOCOMPLETE_PANEL_H */
