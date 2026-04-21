#ifndef REPL_CAMERA_CONTROLS_H
#define REPL_CAMERA_CONTROLS_H

void repl_camera_controls_reset(void);
void repl_camera_pointer_set(int x, int y);
void repl_camera_mouse_event(int button, int state, int x, int y, int mods);
void repl_camera_add_zoom_velocity(float delta);
void repl_camera_drag_motion(int x, int y);
void repl_camera_tick(void);

#endif /* REPL_CAMERA_CONTROLS_H */
