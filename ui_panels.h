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
void render_scene_status(void);

void var_panel_rect(int *x, int *y, int *w, int *h);
int  var_panel_hit(int gx, int gy, int *out_row);
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

/* Test support: color picker state inspection and hit-rect setup without GL. */
#define UI_PANELS_SWATCH_W 12   /* inline swatch width; mirrors SWATCH_W in ui_panels.c */
int  ui_panels_color_picker_is_open(void);
int  ui_panels_open_color_picker(int cmd_idx, int glut_y); /* returns 1 on success */
void ui_panels_setup_color_picker_rects(void);     /* populate rects from g_cp_px/py */
void ui_panels_color_picker_sv_rect(int *out_x, int *out_y, int *out_sz);

/* Inline rename of a user-scene slot.  While active the keyboard dispatcher
 * forwards keystrokes into ui_panels_handle_rename_key/special; commit
 * (Enter) calls repl_user_scene_rename, cancel (Esc) discards. */
int  ui_panels_rename_active(void);
int  ui_panels_begin_rename(int slot);             /* 1 on start, 0 if slot invalid */
int  ui_panels_handle_rename_key(unsigned char key);/* returns 1 if consumed */
int  ui_panels_handle_rename_special(int key);      /* returns 1 if consumed */
void ui_panels_cancel_rename(void);

#endif /* UI_PANELS_H */
