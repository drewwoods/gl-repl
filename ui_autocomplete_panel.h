/*
 * ui_autocomplete_panel.h - Autocomplete popup rendering.
 *
 * Renders a floating autocomplete dropdown popup showing symbol completions
 * and parameter hints. Pure rendering module — no state mutations or match
 * computation. The model (match state, selection, hints) lives in
 * editor_autocomplete.c; this module reads that state and draws the popup.
 *
 * Model-view separation: editor_autocomplete.c maintains the autocomplete model:
 *   - Current match list (GL command names, constants, math functions, etc.)
 *   - Selected match index (highlighted row)
 *   - Ghost suffix to append on Tab accept (completion proposal)
 *   - Parameter hints (function arguments, types, descriptions)
 *
 * This module queries that state via editor_state_autocomplete() (typed facade)
 * and renders the popup without modifying any state. Completion acceptance
 * (Tab key) is handled by repl_editor.c, which calls editor_autocomplete.c's
 * acceptance function.
 *
 * Popup layout: Appears below the cursor position in the code panel. Shows a
 * list of matching symbols (max 8 rows visible); selected row is highlighted
 * with background color. Below the match list, a parameter hint line shows
 * function signatures or descriptions (for hints like "sin(x) → double").
 *
 * Trigger modes: Autocomplete is active in different contexts:
 *   - Function prefix: after "foo(" or similar, showing parameter hints
 *   - GL constant: after "GL_" or similar, showing constant values
 *   - 3D point: after "glVertex3f(" showing x, y, z suggestions
 *   - General symbol: typing an identifier, matching against command/function
 *     names
 *
 * Integration: ui_panels.c (input bridge) checks if autocomplete is active
 * before routing input to code panel. repl_editor.c routes Tab to accept
 * completion, arrow keys to navigate matches, and Escape to dismiss.
 */
#ifndef UI_AUTOCOMPLETE_PANEL_H
#define UI_AUTOCOMPLETE_PANEL_H

#include "ui_snapshot.h"

/* Render the autocomplete popup once per frame from the supplied snapshot.
 * Performs no live REPL state reads. Renders nothing if autocomplete is not
 * active (no matches). */
void ui_autocomplete_panel_render(const UiRenderSnapshot *snap);

#endif /* UI_AUTOCOMPLETE_PANEL_H */
