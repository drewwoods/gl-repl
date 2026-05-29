/*
 * glr_ctrl_internal.h - cross-file-internal surface shared between glr_ctrl.c
 * and its carved-out siblings (glr_ctrl_view_transition.c, ...).
 *
 * NOT a public API — these declarations let the controller's frame loop /
 * scene-config / reset paths reach state machines that now live in sibling
 * translation units. The public controller surface stays in glr_ctrl.h.
 */
#ifndef GLR_CTRL_INTERNAL_H
#define GLR_CTRL_INTERNAL_H

/* ---- View-mode 2D/3D transition (src/app/glr_ctrl_view_transition.c) ---- */

/* Advance the projection/camera 2D<->3D transition by dt seconds. Called once
 * per frame from glr_ctrl_tick. */
void  glr_ctrl_tick_view_transition(float dt);

/* Smoothed projection blend in [0,1] (0 = ortho, 1 = perspective), for the
 * scene-config builder. */
float glr_ctrl_view_projection_mix(void);

/* Reset the transition state machine to the perspective default (reset_all). */
void  glr_ctrl_view_reset(void);

/* Push the camera control mode (2D vs 3D) to match the current view state.
 * Called by the camera-mouse routers after a drag. */
void  glr_ctrl_sync_camera_control_mode(void);

#endif /* GLR_CTRL_INTERNAL_H */
