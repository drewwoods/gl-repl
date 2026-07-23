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
/* Independent projection-toggle blend (GLR_CONFIG_PROJECTION), eased on the
 * same schedule as g_projection_mix but driven purely by the Projection
 * config — no camera flatten, no control-mode change. The controller combines
 * this with the view-mode blend via min() (ortho wins). Kept separate so the
 * view-mode 2D/3D state machine below stays exactly as it was. */
static float g_projection_toggle_mix = 1.0f; /* 0 = ortho, 1 = perspective */
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
    /* Use the ease *destination*, not the live pose: an example load one
     * frame earlier may have issued an ease_to toward its `// camera` block
     * (e.g. dist=-2.5) that hasn't ticked into the live pose yet. Reading
     * glr_camera() here would carry the previous example's stale dist/tx/ty
     * into 2D (and clobber the example's ease), so the ortho zoom would lock
     * onto the old camera distance. */
    GlrCameraState cam = glr_camera_destination();
    /* Refresh on every new 3D->2D leg, including a reversal while the prior
     * 2D->3D camera ease is still active. The destination is the intended 3D
     * pose in both cases and also reflects an intervening Reset camera action;
     * retaining the older snapshot here would resurrect the pre-reset orbit
     * on the next return to 3D. */
    g_saved_3d_camera = cam;
    g_saved_3d_camera_valid = 1;
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

    /* Carry the ease *destination* (not the partway live pose) for
     * tx/ty/dist: a 3D example loaded mid-blend is still easing toward its
     * own pan/zoom when the projection finishes, so the live pose is only
     * ~98% there. Reading the destination locks onto the example's intended
     * values; with no ease in flight it equals the live pose (no change to
     * the plain interactive toggle). The orbit angle + tz come from the
     * saved snapshot, kept current by glr_ctrl_view_record_external_3d_pose. */
    cam = glr_camera_destination();
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
    /* The saved-3D snapshot is still the authority — pending consumption by
     * start_camera_to_3d — for as long as the controls are in 2D mode:
     * either dwelling in 2D, or mid 2D->3D with the projection still
     * blending (the same condition glr_ctrl_sync_camera_control_mode uses).
     * Loading a *second* example mid-blend must refresh it here; otherwise
     * start_camera_to_3d restores the FIRST example's stale orbit angle when
     * the blend finishes. Once start_camera_to_3d has fired (CAMERA_TO_3D)
     * or we've settled in 3D (IDLE) the live camera is authoritative and a
     * stale report must not smear the snapshot — see
     * test_view_record_external_3d_pose_noop_in_perspective. */
    if (!glr_ctrl_view_controls_are_2d())
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

/* Step *mix toward target at the shared projection-transition rate. Returns
 * 1 when it has reached (or already sat at) the target, 0 while still easing.
 * Shared by the view-mode blend and the independent projection-toggle blend. */
static int glr_ctrl_step_mix_toward(float *mix, float target, float dt) {
    if (GLR_VIEW_PROJECTION_TRANSITION_SECS <= 0.0f) {
        *mix = target;
        return 1;
    }

    float step = dt / GLR_VIEW_PROJECTION_TRANSITION_SECS;
    if (*mix != target) {
        float sign = (*mix < target) ? 1.0f : -1.0f;
        *mix += sign * step;
        if ((sign > 0.0f && *mix >= target) ||
            (sign < 0.0f && *mix <= target)) {
            *mix = target;
            return 1;
        }
        return 0;
    }
    return 1;
}

static int glr_ctrl_step_projection_toward(float target, float dt) {
    return glr_ctrl_step_mix_toward(&g_projection_mix, target, dt);
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

    /* Independent of the phase machine above: ease the projection-toggle
     * blend toward the current Projection config. No camera work — the
     * camera stays free, so this renders ortho from the live orbit angle. */
    glr_ctrl_step_mix_toward(&g_projection_toggle_mix,
                             (glr_state_presentation().projection_mode == PROJ_ORTHO) ? 0.0f : 1.0f,
                             dt);

    glr_ctrl_sync_camera_control_mode();
}

float glr_ctrl_view_projection_mix(void) {
    return smoothstep01(g_projection_mix);
}

float glr_ctrl_projection_toggle_mix(void) {
    return smoothstep01(g_projection_toggle_mix);
}

void glr_ctrl_view_reset(void) {
    g_projection_mix         = 1.0f;
    g_projection_toggle_mix  = 1.0f;
    g_view_mode_target_ortho = 0;
    g_view_xn_phase          = GLR_VIEW_XN_IDLE;
    g_saved_3d_camera_valid  = 0;
}

void glr_ctrl_view_transition_capture(GlrViewTransitionSnapshot *out) {
    if (!out)
        return;
    out->projection_mix         = g_projection_mix;
    out->projection_toggle_mix  = g_projection_toggle_mix;
    out->view_mode_target_ortho = g_view_mode_target_ortho;
    out->phase                  = (int)g_view_xn_phase;
    out->saved_3d_camera        = g_saved_3d_camera;
    out->saved_3d_camera_valid  = g_saved_3d_camera_valid;
}

void glr_ctrl_view_transition_restore(const GlrViewTransitionSnapshot *snap) {
    if (!snap)
        return;
    g_projection_mix         = snap->projection_mix;
    g_projection_toggle_mix  = snap->projection_toggle_mix;
    g_view_mode_target_ortho = snap->view_mode_target_ortho;
    g_view_xn_phase          = (GlrViewTransitionPhase)snap->phase;
    g_saved_3d_camera        = snap->saved_3d_camera;
    g_saved_3d_camera_valid  = snap->saved_3d_camera_valid;
}
