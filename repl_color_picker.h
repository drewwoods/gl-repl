#ifndef REPL_COLOR_PICKER_H
#define REPL_COLOR_PICKER_H

#define REPL_COLOR_SWATCH_W 12

int  repl_color_picker_active_line(void);
int  repl_color_picker_can_edit_cmd(int cmd_idx);
void repl_color_picker_open(int cmd_idx, int my);
void repl_color_picker_render(void);
void repl_color_picker_render_swatch(int cmd_idx, int sx, int sy);
int  repl_color_picker_press(int mx, int my);
int  repl_color_picker_motion(int mx, int my);
void repl_color_picker_release(void);
int  repl_color_picker_close(void);

#endif /* REPL_COLOR_PICKER_H */
