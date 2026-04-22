/*
 * repl_variable_panel.h -- Floating variable slider panel.
 *
 * Renders a read-only HUD of declared predefined variables with a
 * shared log-scale slider per row.  Mutation of variable values is
 * driven by the editor's drag handler (see g_drag_var in
 * repl_editor.c); this module only renders and reports geometry/hits.
 */
#ifndef REPL_VARIABLE_PANEL_H
#define REPL_VARIABLE_PANEL_H

void render_var_panel(void);
void var_panel_rect(int *px, int *py, int *pw, int *ph);
int  var_panel_hit(int gx, int gy, int *out_row);

#endif /* REPL_VARIABLE_PANEL_H */
