/*
 * glr_camera.c - Viewport camera state + orbit/pan/zoom controls.
 *
 * Owns the camera struct, its accessors, and the drag/momentum
 * machinery. UiState used to host the camera slot + the
 * ui_state_camera* accessors and `repl_camera_controls.c` did the
 * drag math reaching back into UiState; both halves merged here.
 *
 * Pointer tracking is internal — drag deltas come from the file-static
 * cache. `ui/state.h` (the global `UiPointerState` for snapshot
 * consumers) is updated by callers in glr_ctrl, not by this module,
 * so glr_camera stays free of UI dependencies.
 */
#include "app/glr_camera.h"
#include "app/glr_defaults.h"  /* CFG_DEFAULT_CAMERA_ROTATE */

#include "gl_includes.h"      /* GLUT_*BUTTON constants */
#include "config.h"
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Orbit/pan momentum decay per frame. Deliberately independent of
 * config.h's GLR_CAMERA_TARGET_DECAY (the ease-to-target decay) even
 * though both currently default to 0.88f — they are different knobs. */
#define CAM_DECAY 0.88f
#define CAM_DECAY_ZOOM 0.65f
#define CAM_TARGET_ANGLE_EPS 0.01f
#define CAM_TARGET_POS_EPS 0.001f
#define CAM_MOMENTUM_THRESHOLD 1.0f
#define CAM_RX_MIN (-89.0f)
#define CAM_RX_MAX 89.0f
#define CAM_DIST_MIN 0.5f
#define CAM_DIST_MAX 50.0f

/* Drag-to-motion conversion (pointer-pixel deltas → world/camera units)
 * and the orbit/pan momentum coupling. CAM_ROT_SENSITIVITY (degrees per
 * pixel of left-drag) and CAM_MOMENTUM_COUPLE (fraction of the same delta
 * fed into velocity) are distinct knobs that merely share a default today. */
#define CAM_PAN_SCALE 0.005f          /* world units / pixel, scaled by dist */
#define CAM_ZOOM_SCALE 0.02f          /* dist units / pixel (middle-drag)     */
#define CAM_ROT_SENSITIVITY 0.5f      /* degrees / pixel (left-drag orbit)    */
#define CAM_MOMENTUM_COUPLE 0.5f      /* pan delta → velocity fraction        */
#define CAM_ROT_MOMENTUM_COUPLE 0.25f /* orbit delta → velocity fraction      */

/* Motion-glow gizmo brightness (0..1): full while actively dragging, a
 * lower level while momentum/ease coasts the target, decayed each frame and
 * snapped to 0 below the floor. Only lights up once pan velocity clears the
 * threshold. */
#define CAM_GLOW_ACTIVE 1.0f
#define CAM_GLOW_COAST 0.6f
#define CAM_GLOW_DECAY 0.94f
#define CAM_GLOW_FLOOR 0.005f
#define CAM_GLOW_VEL_THRESHOLD 0.01f

/* Default values for GlrCameraState — the previous home was
 * UI_STATE_INITIAL.camera in src/ui/app/state.c. */
#define GLR_CAMERA_INITIAL                          \
    {                                               \
        .rx          = 20.0f,                       \
        .ry          = 30.0f,                       \
        .dist        = 5.0f,                        \
        .tx          = 0.0f,                        \
        .ty          = 0.0f,                        \
        .tz          = 0.0f,                        \
        .motion_glow = 0.0f,                        \
        .auto_rotate = CFG_DEFAULT_CAMERA_ROTATE,   \
    }

static GlrCameraState       g_camera          = GLR_CAMERA_INITIAL;
static const GlrCameraState g_camera_defaults = GLR_CAMERA_INITIAL;
static GlrCameraState       g_camera_target;
static int                   g_camera_target_active = 0;
static GlrCameraControlMode  g_control_mode    = GLR_CAMERA_CONTROL_3D;

/* Internal pointer cache: where the mouse is + which button is held.
 * Used only for drag-delta computation. Snapshot consumers who need
 * the "global" pointer position read UiState.pointer, which callers
 * (glr_ctrl) keep in sync alongside our pointer_set / mouse_event
 * calls. */
static int g_pointer_x      = 0;
static int g_pointer_y      = 0;
static int g_pointer_button = -1;

static int   g_mouse_mods = 0;
static float g_vel_ry     = 0.0f;
static float g_vel_rx     = 0.0f;
static float g_vel_tx     = 0.0f;
static float g_vel_ty     = 0.0f;
static float g_vel_tz     = 0.0f;
static float g_vel_zoom   = 0.0f;

static float clampf(float value, float lo, float hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static void clamp_camera_pitch(GlrCameraState *c) {
    if (c->rx > CAM_RX_MAX) {
        c->rx = CAM_RX_MAX;
        g_vel_rx = 0.0f;
    }
    if (c->rx < CAM_RX_MIN) {
        c->rx = CAM_RX_MIN;
        g_vel_rx = 0.0f;
    }
}

static void clamp_camera_distance(GlrCameraState *c) {
    if (c->dist < CAM_DIST_MIN) {
        c->dist = CAM_DIST_MIN;
        g_vel_zoom = 0.0f;
    }
    if (c->dist > CAM_DIST_MAX) {
        c->dist = CAM_DIST_MAX;
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

static void cancel_target_ease(void) {
    g_camera_target_active = 0;
}

static void drop_small_velocities(void) {
    g_vel_ry   = fabsf(g_vel_ry)   > CAM_MOMENTUM_THRESHOLD ? g_vel_ry   : 0.0f;
    g_vel_rx   = fabsf(g_vel_rx)   > CAM_MOMENTUM_THRESHOLD ? g_vel_rx   : 0.0f;
    g_vel_tx   = fabsf(g_vel_tx)   > CAM_MOMENTUM_THRESHOLD ? g_vel_tx   : 0.0f;
    g_vel_ty   = fabsf(g_vel_ty)   > CAM_MOMENTUM_THRESHOLD ? g_vel_ty   : 0.0f;
    g_vel_tz   = fabsf(g_vel_tz)   > CAM_MOMENTUM_THRESHOLD ? g_vel_tz   : 0.0f;
    g_vel_zoom = fabsf(g_vel_zoom) > CAM_MOMENTUM_THRESHOLD ? g_vel_zoom : 0.0f;
}

static float shortest_angle_delta(float from, float to) {
    float delta = fmodf(to - from, 360.0f);
    if (delta > 180.0f)
        delta -= 360.0f;
    if (delta < -180.0f)
        delta += 360.0f;
    return delta;
}

static int target_deltas_within_epsilon(const GlrCameraState *c) {
    return fabsf(g_camera_target.rx - c->rx) < CAM_TARGET_ANGLE_EPS &&
           fabsf(shortest_angle_delta(c->ry, g_camera_target.ry)) <
               CAM_TARGET_ANGLE_EPS &&
           fabsf(g_camera_target.dist - c->dist) < CAM_TARGET_POS_EPS &&
           fabsf(g_camera_target.tx - c->tx) < CAM_TARGET_POS_EPS &&
           fabsf(g_camera_target.ty - c->ty) < CAM_TARGET_POS_EPS &&
           fabsf(g_camera_target.tz - c->tz) < CAM_TARGET_POS_EPS;
}

static void snap_to_target(void) {
    g_camera.rx = g_camera_target.rx;
    g_camera.ry = g_camera_target.ry;
    g_camera.dist = g_camera_target.dist;
    g_camera.tx = g_camera_target.tx;
    g_camera.ty = g_camera_target.ty;
    g_camera.tz = g_camera_target.tz;
    cancel_target_ease();
}

static void tick_target_ease(void) {
    GlrCameraState *c = &g_camera;
    float k = 1.0f - GLR_CAMERA_TARGET_DECAY;
    float dx = g_camera_target.tx - c->tx;
    float dy = g_camera_target.ty - c->ty;
    float dz = g_camera_target.tz - c->tz;

    c->rx += (g_camera_target.rx - c->rx) * k;
    c->ry += shortest_angle_delta(c->ry, g_camera_target.ry) * k;
    c->dist += (g_camera_target.dist - c->dist) * k;
    c->tx += dx * k;
    c->ty += dy * k;
    c->tz += dz * k;

    if (fabsf(dx) + fabsf(dy) + fabsf(dz) > CAM_TARGET_POS_EPS &&
        c->motion_glow < CAM_GLOW_COAST)
        c->motion_glow = CAM_GLOW_COAST;

    if (target_deltas_within_epsilon(c))
        snap_to_target();
}

/* ---- Accessors ------------------------------------------------------ */

GlrCameraState glr_camera(void)        { return g_camera; }
GlrCameraState *glr_camera_mut(void)   { return &g_camera; }
GlrCameraControlMode glr_camera_control_mode(void) { return g_control_mode; }
int glr_camera_target_active(void) { return g_camera_target_active; }

void glr_camera_set_control_mode(GlrCameraControlMode mode) {
    if (mode != GLR_CAMERA_CONTROL_2D)
        mode = GLR_CAMERA_CONTROL_3D;
    if (g_control_mode == mode)
        return;
    g_control_mode = mode;
    reset_velocities();
}

void glr_camera_set(float rx, float ry, float dist,
                    float tx, float ty, float tz,
                    float motion_glow) {
    cancel_target_ease();
    g_camera.rx = rx;
    g_camera.ry = ry;
    g_camera.dist = dist;
    g_camera.tx = tx;
    g_camera.ty = ty;
    g_camera.tz = tz;
    g_camera.motion_glow = motion_glow;
}

void glr_camera_set_orbit(float rx, float ry) {
    cancel_target_ease();
    g_camera.rx = rx;
    g_camera.ry = ry;
}

void glr_camera_set_pan(float tx, float ty, float tz) {
    cancel_target_ease();
    g_camera.tx = tx;
    g_camera.ty = ty;
    g_camera.tz = tz;
}

void glr_camera_set_distance(float dist) {
    cancel_target_ease();
    g_camera.dist = dist;
}

void glr_camera_set_motion_glow(float mg)        { g_camera.motion_glow = mg; }

void glr_camera_ease_to(float rx, float ry, float dist,
                        float tx, float ty, float tz) {
    g_camera_target = g_camera;
    g_camera_target.rx = rx;
    g_camera_target.ry = ry;
    g_camera_target.dist = dist;
    g_camera_target.tx = tx;
    g_camera_target.ty = ty;
    g_camera_target.tz = tz;
    g_camera_target_active = 1;
    g_pointer_button = -1;
    reset_velocities();
    if (target_deltas_within_epsilon(&g_camera))
        snap_to_target();
}

void glr_camera_reset_default(void) {
    cancel_target_ease();
    reset_velocities();
    g_control_mode = GLR_CAMERA_CONTROL_3D;
    g_camera = g_camera_defaults;
}

void glr_camera_focus_origin(void) {
    glr_camera_ease_to(g_camera.rx, g_camera.ry, g_camera.dist,
                       0.0f, 0.0f, 0.0f);
}

void glr_camera_ease_to_default(void) {
    g_control_mode = GLR_CAMERA_CONTROL_3D;
    glr_camera_ease_to(g_camera_defaults.rx, g_camera_defaults.ry,
                       g_camera_defaults.dist, g_camera_defaults.tx,
                       g_camera_defaults.ty, g_camera_defaults.tz);
}

void glr_camera_capture(GlrCameraState *out) {
    if (out) *out = g_camera;
}

void glr_camera_restore(const GlrCameraState *snap) {
    if (snap) {
        cancel_target_ease();
        g_camera = *snap;
    }
}

/* ---- Controls ------------------------------------------------------- */

void glr_camera_controls_reset(void) {
    cancel_target_ease();
    g_pointer_button = -1;
    g_mouse_mods = 0;
    reset_velocities();
}

void glr_camera_pointer_set(int x, int y) {
    g_pointer_x = x;
    g_pointer_y = y;
}

void glr_camera_mouse_event(int button, int state, int x, int y, int mods) {
    g_mouse_mods = mods;
    if (state == GLUT_DOWN) {
        cancel_target_ease();
        g_pointer_button = button;
        g_pointer_x = x;
        g_pointer_y = y;
        reset_velocities();
    } else {
        drop_small_velocities();
        g_pointer_button = -1;
    }
}

void glr_camera_add_zoom_velocity(float delta) {
    g_vel_zoom += delta;
}

void glr_camera_drag_motion(int x, int y) {
    GlrCameraState *c = &g_camera;
    int px = g_pointer_x;
    int py = g_pointer_y;
    int btn = g_pointer_button;
    int dx = x - px;
    int dy = y - py;

    if (g_control_mode == GLR_CAMERA_CONTROL_2D &&
        (btn == GLUT_LEFT_BUTTON || btn == GLUT_RIGHT_BUTTON)) {
        float scale = CAM_PAN_SCALE * c->dist;
        float dtx = -(float)dx * scale;
        float dty =  (float)dy * scale;
        c->tx += dtx;
        c->ty += dty;
        g_vel_tx *= CAM_DECAY;
        g_vel_ty *= CAM_DECAY;
        g_vel_tx += dtx * CAM_MOMENTUM_COUPLE;
        g_vel_ty += dty * CAM_MOMENTUM_COUPLE;
        c->motion_glow = CAM_GLOW_ACTIVE;
    } else if (btn == GLUT_LEFT_BUTTON) {
        c->ry += (float)dx * CAM_ROT_SENSITIVITY;
        c->rx += (float)dy * CAM_ROT_SENSITIVITY;
        c->ry = fmodf(c->ry, 360.0f);
        c->rx = clampf(c->rx, CAM_RX_MIN, CAM_RX_MAX);
        g_vel_rx *= CAM_DECAY;
        g_vel_ry *= CAM_DECAY;
        g_vel_ry += (float)dx * CAM_ROT_MOMENTUM_COUPLE;
        g_vel_rx += (float)dy * CAM_ROT_MOMENTUM_COUPLE;
    } else if (btn == GLUT_RIGHT_BUTTON) {
        float scale = CAM_PAN_SCALE * c->dist;
        float fdy = (float)dy;
        if (g_mouse_mods & GLUT_ACTIVE_SHIFT) {
            /* Shift + right-drag: pan the orbit target along world Y. */
            float wdy = -fdy * scale;
            c->ty -= wdy;
            g_vel_ty *= CAM_DECAY;
            g_vel_ty += wdy * CAM_MOMENTUM_COUPLE;
            c->motion_glow = CAM_GLOW_ACTIVE;
        } else {
            /* Pan along world XZ ground plane. */
            float ry_rad = c->ry * (float)M_PI / 180.0f;
            float cry = cosf(ry_rad);
            float sry = sinf(ry_rad);
            float fdx = (float)dx;
            float wdx = (fdx * cry - fdy * sry) * scale;
            float wdz = (fdx * sry + fdy * cry) * scale;
            c->tx -= wdx;
            c->tz -= wdz;
            g_vel_tx *= CAM_DECAY;
            g_vel_tz *= CAM_DECAY;
            g_vel_tx += wdx * CAM_MOMENTUM_COUPLE;
            g_vel_tz += wdz * CAM_MOMENTUM_COUPLE;
            c->motion_glow = CAM_GLOW_ACTIVE;
        }
    } else if (btn == GLUT_MIDDLE_BUTTON) {
        c->dist += (float)dy * CAM_ZOOM_SCALE;
        c->dist = clampf(c->dist, CAM_DIST_MIN, CAM_DIST_MAX);
    }

    g_pointer_x = x;
    g_pointer_y = y;
}

void glr_camera_tick(void) {
    GlrCameraState *c = &g_camera;

    if (g_camera_target_active && g_pointer_button == -1) {
        tick_target_ease();
    } else if (g_pointer_button == -1) {
        c->ry += g_vel_ry;
        c->rx += g_vel_rx;
        c->ry = fmodf(c->ry, 360.0f);
        clamp_camera_pitch(c);
        c->tx += g_vel_tx;
        c->ty += g_vel_ty;
        c->tz += g_vel_tz;
        c->dist += g_vel_zoom;
        clamp_camera_distance(c);
    }

    g_vel_ry *= CAM_DECAY;
    g_vel_rx *= CAM_DECAY;
    g_vel_tx *= CAM_DECAY;
    g_vel_ty *= CAM_DECAY;
    g_vel_tz *= CAM_DECAY;
    g_vel_zoom *= CAM_DECAY_ZOOM;

    /* Gizmo lit while pan momentum carries the target. */
    float pan_vel = fabsf(g_vel_tx) + fabsf(g_vel_ty) + fabsf(g_vel_tz);
    if (pan_vel > CAM_GLOW_VEL_THRESHOLD && c->motion_glow < CAM_GLOW_COAST)
        c->motion_glow = CAM_GLOW_COAST;
    c->motion_glow *= CAM_GLOW_DECAY;
    if (c->motion_glow < CAM_GLOW_FLOOR)
        c->motion_glow = 0.0f;

    if (!g_camera_target_active &&
        g_control_mode == GLR_CAMERA_CONTROL_3D &&
        c->auto_rotate) {
        c->ry += 0.3f;
        c->ry = fmodf(c->ry, 360.0f);
    }
}
