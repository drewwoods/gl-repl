/*
 * repl_camera_controls.h - Interactive camera orbit, pan, and zoom.
 *
 * Manages the scene camera as a stateful object with position, rotation, and
 * zoom level. Supports three interaction modes: orbit (rotate around scene
 * center by dragging), pan (translate camera by dragging), and zoom (scroll
 * wheel or keyboard). Zoom velocity implements momentum so that rapid scroll
 * events create smooth animation.
 *
 * Camera state: Internally stored as transform matrices (translate + rotate)
 * that are applied to the GL matrix stack during rendering. The camera
 * position/rotation are read back for export (camera state is preserved across
 * save/load). Time-stepping (repl_camera_tick) applies zoom momentum decay and
 * accumulates zoom velocity into the transform.
 *
 * Interaction model:
 *   - Mouse drag: repl_camera_mouse_event() detects press; repl_camera_drag_motion()
 *     updates camera transform during drag. Modifier keys (Ctrl = pan, Shift = orbit
 *     alt-style) choose interaction mode.
 *   - Mouse wheel: repl_camera_add_zoom_velocity() accumulates zoom; momentum decay
 *     happens in repl_camera_tick().
 *   - Reset: repl_camera_controls_reset() returns to default view.
 *
 * Pointer tracking: repl_camera_pointer_set() updates the stored pointer position
 * for motion deltas during drags. Called by the GLUT mouse callback.
 *
 * Integration: scene_render.c calls repl_camera_tick() once per frame to apply
 * momentum, then reads camera state for matrix stack setup.
 */
#ifndef REPL_CAMERA_CONTROLS_H
#define REPL_CAMERA_CONTROLS_H

/* Reset camera to default position and orientation. Clears any drag state and
 * momentum, returning to the initial view looking at the scene center.
 * Triggered by user action or scene reset. */
void repl_camera_controls_reset(void);

/* Update the stored pointer position (used for computing drag deltas).
 * x, y are window coordinates. Called by the GLUT mouse callback to track
 * mouse position. */
void repl_camera_pointer_set(int x, int y);

/* Handle mouse button event. button is GLUT_LEFT_BUTTON, GLUT_MIDDLE_BUTTON,
 * or GLUT_RIGHT_BUTTON; state is GLUT_DOWN or GLUT_UP. x, y are window
 * coordinates. mods is a bitmask of modifier keys (GLUT_ACTIVE_CTRL,
 * GLUT_ACTIVE_SHIFT). Detects drag begin/end; drag motion is handled by
 * repl_camera_drag_motion(). Called by the GLUT mouse callback. */
void repl_camera_mouse_event(int button, int state, int x, int y, int mods);

/* Add zoom velocity from scroll wheel or keyboard (+ / -). delta is the scroll
 * delta or keyboard multiplier. Accumulates into momentum; the actual zoom
 * application happens in repl_camera_tick(). Called by scroll event handler
 * or keyboard dispatch. */
void repl_camera_add_zoom_velocity(float delta);

/* Update camera during drag. x, y are current window coordinates. Computes
 * delta from stored pointer position and applies camera rotation (orbit) or
 * translation (pan) based on modifier keys. Called repeatedly by the GLUT
 * motion callback while a mouse button is pressed. */
void repl_camera_drag_motion(int x, int y);

/* Per-frame time step. Applies zoom momentum decay and accumulates zoom
 * velocity into the camera transform. Called once per frame by scene_render.c
 * before rendering. Enables smooth momentum-based zoom animation. */
void repl_camera_tick(void);

#endif /* REPL_CAMERA_CONTROLS_H */
