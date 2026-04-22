/*
 * repl_autocomplete_panel.h -- Rendering side of autocomplete.
 *
 * The match computation, selection state, and parameter hints live
 * in the model-only repl_autocomplete.c.  This panel reads that state
 * and draws the popup; it performs no mutation.
 */
#ifndef REPL_AUTOCOMPLETE_PANEL_H
#define REPL_AUTOCOMPLETE_PANEL_H

void repl_autocomplete_panel_render(void);

#endif /* REPL_AUTOCOMPLETE_PANEL_H */
