/*
 * ui_panels.h — Code panel, autocomplete/help/var overlays, panel input bridge
 */
#ifndef UI_PANELS_H
#define UI_PANELS_H

/* Geometry helpers: rects are in OpenGL coordinates (y=0 at bottom). */
void code_panel_rect(int *x, int *y, int *w, int *h);
void scene_rect(int *x, int *y, int *w, int *h);

void render_code_panel(void);
void render_example_dropdown(void);
void render_scene_status(void);

void ui_panels_open_config(void);
void ui_panels_close_menus(void);
int  ui_panels_handle_right_press(int mx, int my);  /* returns 1 if consumed */
int  example_dropdown_is_open(void);
int  code_panel_get_command_display_text(int cmd_idx, char *out, int out_size);
int  code_panel_apply_scroll_follow_for_test(int *out_follow_doc_line,
                                             int *out_visible_lines);
void handle_code_panel_click(int mx, int my);
enum {
	UI_PANEL_PRESS_NONE = 0,
	UI_PANEL_PRESS_CONSUMED = 1 << 0,
	UI_PANEL_PRESS_OPENED_COLOR_PICKER = 1 << 1
};
int  handle_code_panel_press(int mx, int my);      /* returns UI_PANEL_PRESS_* bitmask */
int  handle_code_panel_drag(int mx, int my);
void handle_code_panel_release(void);

/* Input bridge helpers so editor code does not touch color-picker internals. */
int  ui_panels_handle_escape(void);                  /* returns 1 if consumed */
int  ui_panels_handle_scene_press(int mx, int my);  /* returns 1 if consumed */
int  ui_panels_handle_motion(int mx, int my);       /* returns 1 if consumed */
void ui_panels_handle_mouse_release(void);

#endif /* UI_PANELS_H */
