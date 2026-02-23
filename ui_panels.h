/*
 * ui_panels.h — Code panel, autocomplete, help overlay, var panel, config menu
 */
#ifndef UI_PANELS_H
#define UI_PANELS_H

void render_code_panel(void);
void render_autocomplete(void);
void render_help(void);
void render_var_panel(void);
void render_config_menu(void);

int  cfg_hit_row(int gx, int gy);
int  var_panel_hit(int gx, int gy, int *out_row);
void handle_code_panel_click(int mx, int my);

#endif /* UI_PANELS_H */
