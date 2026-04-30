#ifndef IMREPL_CTRL_H
#define IMREPL_CTRL_H

void imrepl_ctrl_init_gl(void);
void imrepl_ctrl_bootstrap_repl(const char *input_file);
void imrepl_ctrl_set_accum(int enabled);
void imrepl_ctrl_display_frame(void);
void imrepl_ctrl_reshape(int w, int h);
void imrepl_ctrl_keyboard(unsigned char key, int x, int y);
void imrepl_ctrl_special(int key, int x, int y);
void imrepl_ctrl_mouse(int button, int state, int x, int y);
void imrepl_ctrl_motion(int x, int y);
void imrepl_ctrl_passive_motion(int x, int y);
void imrepl_ctrl_mousewheel(int wheel, int direction, int x, int y);
void imrepl_ctrl_timer(int value);

#endif /* IMREPL_CTRL_H */