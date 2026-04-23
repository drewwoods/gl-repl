/*
 * repl_camera_controls.c -- Viewport camera drag and momentum controls.
 *
 * Editor/UI code decides whether a mouse event belongs to panels, menus, or
 * the scene. The runtime camera/pointer values live in repl_state.c during
 * Phase 2 migration; this module owns active drag modifiers, inertial
 * velocities, orbit/pan/zoom math, and per-frame momentum decay.
 */
#include "sample.h"
#include "repl_camera_controls.h"

#define CAM_DECAY 0.88f
#define CAM_DECAY_ZOOM 0.65f
#define CAM_MOMENTUM_THRESHOLD 1.0f
#define CAM_RX_MIN (-89.0f)
#define CAM_RX_MAX 89.0f
#define CAM_DIST_MIN 0.5f
#define CAM_DIST_MAX 50.0f

static int   g_mouse_mods = 0;
static float g_vel_ry = 0.0f;
static float g_vel_rx = 0.0f;
static float g_vel_tx = 0.0f;
static float g_vel_ty = 0.0f;
static float g_vel_tz = 0.0f;
static float g_vel_zoom = 0.0f;

static float clampf(float value, float lo, float hi) {
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return value;
}

static void clamp_camera_pitch(void) {
    if (g_cam_rx > CAM_RX_MAX) {
        g_cam_rx = CAM_RX_MAX;
        g_vel_rx = 0.0f;
    }
    if (g_cam_rx < CAM_RX_MIN) {
        g_cam_rx = CAM_RX_MIN;
        g_vel_rx = 0.0f;
    }
}

static void clamp_camera_distance(void) {
    if (g_cam_dist < CAM_DIST_MIN) {
        g_cam_dist = CAM_DIST_MIN;
        g_vel_zoom = 0.0f;
    }
    if (g_cam_dist > CAM_DIST_MAX) {
        g_cam_dist = CAM_DIST_MAX;
        g_vel_zoom = 0.0f;
    }
}

static void reset_velocities(void) {
    g_vel_ry = 0.0f;
    g_vel_rx = 0.0f;
    g_vel_tx = 0.0f;
    g_vel_ty = 0.0f;
    g_vel_tz = 0.0f;
    g_vel_zoom = 0.0f;
}

static void drop_small_velocities(void) {
    g_vel_ry = fabsf(g_vel_ry) > CAM_MOMENTUM_THRESHOLD ? g_vel_ry : 0.0f;
    g_vel_rx = fabsf(g_vel_rx) > CAM_MOMENTUM_THRESHOLD ? g_vel_rx : 0.0f;
    g_vel_tx = fabsf(g_vel_tx) > CAM_MOMENTUM_THRESHOLD ? g_vel_tx : 0.0f;
    g_vel_ty = fabsf(g_vel_ty) > CAM_MOMENTUM_THRESHOLD ? g_vel_ty : 0.0f;
    g_vel_tz = fabsf(g_vel_tz) > CAM_MOMENTUM_THRESHOLD ? g_vel_tz : 0.0f;
    g_vel_zoom = fabsf(g_vel_zoom) > CAM_MOMENTUM_THRESHOLD ? g_vel_zoom : 0.0f;
}

void repl_camera_controls_reset(void) {
    g_mouse_btn = -1;
    g_mouse_mods = 0;
    reset_velocities();
}

void repl_camera_pointer_set(int x, int y) {
    g_mouse_x = x;
    g_mouse_y = y;
}

void repl_camera_mouse_event(int button, int state, int x, int y, int mods) {
    g_mouse_mods = mods;
    if (state == GLUT_DOWN) {
        g_mouse_btn = button;
        repl_camera_pointer_set(x, y);
        reset_velocities();
    } else {
        drop_small_velocities();
        g_mouse_btn = -1;
    }
}

void repl_camera_add_zoom_velocity(float delta) {
    g_vel_zoom += delta;
}

void repl_camera_drag_motion(int x, int y) {
    int dx = x - g_mouse_x;
    int dy = y - g_mouse_y;

    if (g_mouse_btn == GLUT_LEFT_BUTTON) {
        g_cam_ry += (float)dx * 0.5f;
        g_cam_rx += (float)dy * 0.5f;
        g_cam_ry = fmodf(g_cam_ry, 360.0f);
        g_cam_rx = clampf(g_cam_rx, CAM_RX_MIN, CAM_RX_MAX);
        g_vel_rx *= CAM_DECAY;
        g_vel_ry *= CAM_DECAY;
        g_vel_ry += (float)dx * 0.25f;
        g_vel_rx += (float)dy * 0.25f;
    } else if (g_mouse_btn == GLUT_RIGHT_BUTTON) {
        float scale = 0.005f * g_cam_dist;
        float fdy = (float)dy;
        if (g_mouse_mods & GLUT_ACTIVE_SHIFT) {
            /* Shift + right-drag: pan the orbit target along world Y. */
            float wdy = -fdy * scale;
            g_cam_ty -= wdy;
            g_vel_ty *= CAM_DECAY;
            g_vel_ty += wdy * 0.5f;
            g_cam_motion_glow = 1.0f;
        } else {
            /* Pan the orbit target along the world XZ ground plane.
             *
             * Camera transform is Rx(rx).Ry(ry).T(-target), so a view-space
             * direction v maps back to world by Ry(-ry).Rx(-rx).v. Projected
             * onto the ground (Y=0):
             *   right_xz   = (+cos ry, 0, +sin ry)   (screen +X)
             *   forward_xz = (+sin ry, 0, -cos ry)   (screen -Y / mouse-up)
             *
             * Mouse-down (dy < 0) pulls the target back toward the camera,
             * so we subtract forward_xz * dy. World Y is preserved. */
            float ry_rad = g_cam_ry * (float)M_PI / 180.0f;
            float cry = cosf(ry_rad);
            float sry = sinf(ry_rad);
            float fdx = (float)dx;
            float wdx = (fdx * cry - fdy * sry) * scale;
            float wdz = (fdx * sry + fdy * cry) * scale;
            g_cam_tx -= wdx;
            g_cam_tz -= wdz;
            g_vel_tx *= CAM_DECAY;
            g_vel_tz *= CAM_DECAY;
            g_vel_tx += wdx * 0.5f;
            g_vel_tz += wdz * 0.5f;
            g_cam_motion_glow = 1.0f;
        }
    } else if (g_mouse_btn == GLUT_MIDDLE_BUTTON) {
        g_cam_dist += (float)dy * 0.02f;
        g_cam_dist = clampf(g_cam_dist, CAM_DIST_MIN, CAM_DIST_MAX);
    }

    repl_camera_pointer_set(x, y);
}

void repl_camera_tick(void) {
    if (g_mouse_btn == -1) {
        g_cam_ry += g_vel_ry;
        g_cam_rx += g_vel_rx;
        g_cam_ry = fmodf(g_cam_ry, 360.0f);
        clamp_camera_pitch();
        g_cam_tx += g_vel_tx;
        g_cam_ty += g_vel_ty;
        g_cam_tz += g_vel_tz;
        g_cam_dist += g_vel_zoom;
        clamp_camera_distance();
    }

    g_vel_ry *= CAM_DECAY;
    g_vel_rx *= CAM_DECAY;
    g_vel_tx *= CAM_DECAY;
    g_vel_ty *= CAM_DECAY;
    g_vel_tz *= CAM_DECAY;
    g_vel_zoom *= CAM_DECAY_ZOOM;

    /* Gizmo is a pan-only affordance. Keep it lit while pan momentum carries
     * the target, then fade out. Rotate/zoom do not trigger it. */
    float pan_vel = fabsf(g_vel_tx) + fabsf(g_vel_ty) + fabsf(g_vel_tz);
    if (pan_vel > 0.01f && g_cam_motion_glow < 0.6f)
        g_cam_motion_glow = 0.6f;
    g_cam_motion_glow *= 0.94f;
    if (g_cam_motion_glow < 0.005f)
        g_cam_motion_glow = 0.0f;

    if (g_cam_rotate) {
        g_cam_ry += 0.3f;
        g_cam_ry = fmodf(g_cam_ry, 360.0f);
    }
}
