/*
 * repl_variable_panel.h -- Floating variable slider panel.
 *
 * Renders a read-only HUD of declared predefined variables with a
 * shared log-scale slider per row.  Mutation of variable values is
 * driven by repl_var_drag.c; this module only renders and reports
 * geometry/hits.
 */
#ifndef UI_VARIABLE_PANEL_H
#define UI_VARIABLE_PANEL_H

void ui_variable_panel_render(void);
void ui_variable_panel_rect(int *px, int *py, int *pw, int *ph);
int  ui_variable_panel_hit(int gx, int gy, int *out_row);

#endif /* UI_VARIABLE_PANEL_H */
