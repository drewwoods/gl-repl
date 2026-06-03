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

#include "editor/input.h"   /* EditorInputDispatchEffects (by-value seam param) */

struct UiRenderSnapshot;    /* opaque here — only a pointer crosses the seam */

/* View-mode transition tuning.
 *
 * GLR_VIEW_CAMERA_TO_2D_DECAY is a faster per-ease decay used only for the
 * 3D->2D camera flattening leg before projection blending starts. */
#ifndef GLR_VIEW_CAMERA_TO_2D_DECAY
#define GLR_VIEW_CAMERA_TO_2D_DECAY 0.85f
#endif

/* ---- Input router seams (src/app/glr_ctrl_router.c <-> glr_ctrl.c) ----
 *
 * The router lives in glr_ctrl_router.c and calls back into glr_ctrl.c for the
 * cached drag snapshot and the post-routing effect apply; glr_ctrl.c's frame
 * tick calls back into the router to run a pending SIGINT-requested quit. */
const struct UiRenderSnapshot *glr_ctrl_drag_hit_test_snapshot(void);
void glr_ctrl_apply_input_effects(EditorInputDispatchEffects effects);
void glr_ctrl_router_run_pending_quit(void);

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
