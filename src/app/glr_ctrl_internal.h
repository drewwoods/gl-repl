/*
 * glr_ctrl_internal.h - cross-file-internal surface shared between glr_ctrl.c
 * and its carved-out siblings (glr_ctrl_view_transition.c, ...).
 *
 * NOT a public API - these declarations let the controller's frame loop /
 * scene-config / reset paths reach state machines that now live in sibling
 * translation units. The public controller surface stays in glr_ctrl.h.
 */
#ifndef GLR_CTRL_INTERNAL_H
#define GLR_CTRL_INTERNAL_H

#include "editor/input.h"   /* EditorInputDispatchEffects (by-value seam param) */
#include "app/glr_camera.h" /* GlrCameraState for the view-transition snapshot */

struct UiRenderSnapshot;    /* opaque here - only a pointer crosses the seam */

/* ---- Input router seams (src/app/glr_ctrl_router.c <-> glr_ctrl.c) ----
 *
 * The router lives in glr_ctrl_router.c and calls back into glr_ctrl.c for the
 * cached drag snapshot and the post-routing effect apply. */
const struct UiRenderSnapshot *glr_ctrl_drag_hit_test_snapshot(void);
void glr_ctrl_apply_input_effects(EditorInputDispatchEffects effects);

/* ---- Quit recovery (src/app/glr_ctrl_recovery.c) ----
 *
 * Consume a quit requested by glr_ctrl_request_quit(): save the recovery
 * copy and exit. Driven from glr_ctrl_tick, because SIGINT sets only a
 * sig_atomic_t flag and the save must not run in handler context. */
void glr_ctrl_recovery_run_pending_quit(void);

/* True when source_line_idx names the currently rendered editor input row
 * and that live, not-yet-committed input contains only whitespace. The GL
 * state router uses this to give visually blank input rows the same boundary
 * semantics as committed CMD_EMPTY rows. */
int glr_ctrl_gl_state_live_input_is_blank_at_line(int source_line_idx);

/* ---- View-mode 2D/3D transition (src/app/glr_ctrl_view_transition.c) ---- */

/* Advance the projection/camera 2D<->3D transition by dt seconds. Called once
 * per frame from glr_ctrl_tick. */
void  glr_ctrl_tick_view_transition(float dt);

/* Smoothed projection blend in [0,1] (0 = ortho, 1 = perspective), for the
 * scene-config builder. */
float glr_ctrl_view_projection_mix(void);

/* Smoothed projection-toggle blend in [0,1] (0 = ortho, 1 = perspective),
 * driven by GLR_CONFIG_PROJECTION independently of the 2D/3D view mode (free
 * camera, no flatten). The scene-config builder combines it with
 * glr_ctrl_view_projection_mix() via min() - the scene is orthographic if
 * EITHER blend is below 1. */
float glr_ctrl_projection_toggle_mix(void);

/* Reset the transition state machine to the perspective default (reset_all). */
void  glr_ctrl_view_reset(void);

/* Complete snapshot of the 2D/3D view-transition state machine: both
 * projection mixes, the target ortho mode, the phase, the saved 3D camera, and
 * its validity flag. Used by the tour baseline so Back / Done-restart put the
 * view-mode blend back exactly as the tour began (paired with the camera
 * runtime restore). */
typedef struct {
    float          projection_mix;
    float          projection_toggle_mix;
    int            view_mode_target_ortho;
    int            phase;             /* GlrViewTransitionPhase (file-local enum) */
    GlrCameraState saved_3d_camera;
    int            saved_3d_camera_valid;
} GlrViewTransitionSnapshot;

void glr_ctrl_view_transition_capture(GlrViewTransitionSnapshot *out);
void glr_ctrl_view_transition_restore(const GlrViewTransitionSnapshot *snapshot);

/* Push the camera control mode (2D vs 3D) to match the current view state.
 * Called by the camera-mouse routers after a drag. */
void  glr_ctrl_sync_camera_control_mode(void);

#endif /* GLR_CTRL_INTERNAL_H */
