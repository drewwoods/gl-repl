/*
 * ui_panels.h — Code panel, autocomplete, help overlay, var panel, config menu
 */
#ifndef UI_PANELS_H
#define UI_PANELS_H

/* Geometry helpers: rects are in OpenGL coordinates (y=0 at bottom). */
void code_panel_rect(int *x, int *y, int *w, int *h);
void scene_rect(int *x, int *y, int *w, int *h);

void render_code_panel(void);
void render_autocomplete(void);
void render_example_dropdown(void);
void render_help(void);
void render_var_panel(void);
void render_config_menu(void);

int  cfg_hit_row(int gx, int gy);
int  var_panel_hit(int gx, int gy, int *out_row);
int  example_dropdown_is_open(void);
void handle_code_panel_click(int mx, int my);
int  handle_code_panel_press(int mx, int my);
int  handle_code_panel_drag(int mx, int my);
void handle_code_panel_release(void);

#endif /* UI_PANELS_H */
