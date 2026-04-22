#ifndef UI_COLOR_PICKER_H
#define UI_COLOR_PICKER_H

#define UI_COLOR_SWATCH_W 12

int  ui_color_picker_active_line(void);
int  ui_color_picker_can_edit_cmd(int cmd_idx);
void ui_color_picker_open(int cmd_idx, int my);
void ui_color_picker_render(void);
void ui_color_picker_render_swatch(int cmd_idx, int sx, int sy);
int  ui_color_picker_press(int mx, int my);
int  ui_color_picker_motion(int mx, int my);
void ui_color_picker_release(void);
int  ui_color_picker_close(void);

#endif /* UI_COLOR_PICKER_H */
