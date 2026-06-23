/*
 * glr_ctrl_view_transition.c - 2D/3D view-mode transition state machine.
 *
 * Carved out of glr_ctrl.c: the projection blend + camera-to-2D/3D easing that
 * runs when the View mode toggles. Pure app-layer state (camera + presentation
 * only — no scene/ui/repl/editor), so it links cleanly and keeps glr_ctrl.c
 * smaller. The controller drives it via glr_ctrl_tick_view_transition and reads
 * the blend through glr_ctrl_view_projection_mix (see glr_ctrl_internal.h).
 */
#include "app/glr_ctrl.h"            /* glr_ctrl_view_record_external_3d_pose decl */
#include "app/glr_ctrl_internal.h"
#include "app/glr_camera.h"
#include "app/glr_state.h"
#include "render3d/view_mode.h"          /* GLR_VIEW_PROJECTION_TRANSITION_SECS */

static float g_projection_mix = 1.0f; /* 0 = ortho, 1 = perspective */
typedef enum {
    GLR_VIEW_XN_IDLE = 0,
    GLR_VIEW_XN_CAMERA_TO_2D,
    GLR_VIEW_XN_PROJECTION_TO_2D,
    GLR_VIEW_XN_PROJECTION_TO_3D,
    GLR_VIEW_XN_CAMERA_TO_3D
} GlrViewTransitionPhase;

static int g_view_mode_target_ortho = 0;
static GlrViewTransitionPhase g_view_xn_phase = GLR_VIEW_XN_IDLE;
static GlrCameraState g_saved_3d_camera;
static int g_saved_3d_camera_valid = 0;

static float smoothstep01(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static int glr_ctrl_view_controls_are_2d(void) {
    return g_view_mode_target_ortho ||
           g_view_xn_phase == GLR_VIEW_XN_PROJECTION_TO_3D;
}

void glr_ctrl_sync_camera_control_mode(void) {
    glr_camera_set_control_mode(glr_ctrl_view_controls_are_2d()
                                ? GLR_CAMERA_CONTROL_2D
                                : GLR_CAMERA_CONTROL_3D);
}

static void glr_ctrl_start_camera_to_2d(void) {
    GlrCameraState cam = glr_camera();
    if (!g_saved_3d_camera_valid || g_view_xn_phase == GLR_VIEW_XN_IDLE) {
        g_saved_3d_camera = cam;
        g_saved_3d_camera_valid = 1;
    }
    glr_camera_ease_to(0.0f, 0.0f, cam.dist, cam.tx, cam.ty, 0.0f);
    /* Use a faster decay for this leg only — the orbit flattening
     * shouldn't drag the projection blend that follows it. The override
     * is reset by the next glr_camera_ease_to call (drag, scene load,
     * etc.), so non-view-mode eases keep the global default. */
    glr_camera_set_target_decay(GLR_VIEW_CAMERA_TO_2D_DECAY);
    g_view_xn_phase = GLR_VIEW_XN_CAMERA_TO_2D;
}

static void glr_ctrl_start_camera_to_3d(void) {
    GlrCameraState cam;
    GlrCameraState target;

    if (!g_saved_3d_camera_valid) {
        g_view_xn_phase = GLR_VIEW_XN_IDLE;
        return;
    }

    cam = glr_camera();
    target = g_saved_3d_camera;
    target.tx = cam.tx;
    target.ty = cam.ty;
    target.dist = cam.dist;
    glr_camera_ease_to(target.rx, target.ry, target.dist,
                       target.tx, target.ty, target.tz);
    g_view_xn_phase = GLR_VIEW_XN_CAMERA_TO_3D;
}

static void glr_ctrl_handle_view_mode_target_change(void) {
    int ortho = glr_state_presentation().ortho_mode ? 1 : 0;

    if (ortho == g_view_mode_target_ortho)
        return;

    g_view_mode_target_ortho = ortho;
    if (ortho)
        glr_ctrl_start_camera_to_2d();
    else
        g_view_xn_phase = GLR_VIEW_XN_PROJECTION_TO_3D;
    glr_ctrl_sync_camera_control_mode();
}

void glr_ctrl_view_record_external_3d_pose(float rx, float ry, float tz) {
    if (!g_view_mode_target_ortho)
        return;
    /* Seed the snapshot when we've never been in 3D this session
     * (e.g. workspace loaded with ortho_mode=1): without this, the
     * 2D->3D restoration early-returns and the camera is left wherever
     * the previous example put it. */
    if (!g_saved_3d_camera_valid) {
        g_saved_3d_camera = glr_camera();
        g_saved_3d_camera_valid = 1;
    }
    g_saved_3d_camera.rx = rx;
    g_saved_3d_camera.ry = ry;
    g_saved_3d_camera.tz = tz;
}

static int glr_ctrl_step_projection_toward(float target, float dt) {
    if (GLR_VIEW_PROJECTION_TRANSITION_SECS <= 0.0f) {
        g_projection_mix = target;
        return 1;
    }

    float step = dt / GLR_VIEW_PROJECTION_TRANSITION_SECS;
    if (g_projection_mix != target) {
        float sign = (g_projection_mix < target) ? 1.0f : -1.0f;
        g_projection_mix += sign * step;
        if ((sign > 0.0f && g_projection_mix >= target) ||
            (sign < 0.0f && g_projection_mix <= target)) {
            g_projection_mix = target;
            return 1;
        }
        return 0;
    }
    return 1;
}

void glr_ctrl_tick_view_transition(float dt) {
    int guard;

    glr_ctrl_handle_view_mode_target_change();

    for (guard = 0; guard < 2; guard++) {
        int phase_changed_without_work = 0;

        switch (g_view_xn_phase) {
        case GLR_VIEW_XN_CAMERA_TO_2D:
            if (!glr_camera_target_active()) {
                g_view_xn_phase = GLR_VIEW_XN_PROJECTION_TO_2D;
                phase_changed_without_work = 1;
            }
            break;
        case GLR_VIEW_XN_PROJECTION_TO_2D:
            if (glr_ctrl_step_projection_toward(0.0f, dt))
                g_view_xn_phase = GLR_VIEW_XN_IDLE;
            break;
        case GLR_VIEW_XN_PROJECTION_TO_3D:
            if (glr_ctrl_step_projection_toward(1.0f, dt))
                glr_ctrl_start_camera_to_3d();
            break;
        case GLR_VIEW_XN_CAMERA_TO_3D:
            if (!glr_camera_target_active())
                g_view_xn_phase = GLR_VIEW_XN_IDLE;
            break;
        case GLR_VIEW_XN_IDLE:
        default:
            break;
        }

        if (!phase_changed_without_work)
            break;
    }

    glr_ctrl_sync_camera_control_mode();
}

float glr_ctrl_view_projection_mix(void) {
    return smoothstep01(g_projection_mix);
}

void glr_ctrl_view_reset(void) {
    g_projection_mix         = 1.0f;
    g_view_mode_target_ortho = 0;
    g_view_xn_phase          = GLR_VIEW_XN_IDLE;
    g_saved_3d_camera_valid  = 0;
}
