/*
 * glr_camera.c - Viewport camera state + orbit/pan/zoom controls.
 *
 * Owns the camera struct, its accessors, and the drag/momentum
 * machinery. UiState used to host the camera slot + the
 * ui_state_camera* accessors and `repl_camera_controls.c` did the
 * drag math reaching back into UiState; both halves merged here.
 *
 * Pointer tracking is internal — drag deltas come from the file-static
 * cache. `ui/state.h` (the global `ReplPointerState` for snapshot
 * consumers) is updated by callers in glr_ctrl, not by this module,
 * so glr_camera stays free of UI dependencies.
 */
#include "app/glr_camera.h"
#include "app/glr_defaults.h"  /* CFG_DEFAULT_CAMERA_ROTATE */

#include <gl_includes.h>      /* GLUT_*BUTTON constants */
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CAM_DECAY 0.88f
#define CAM_DECAY_ZOOM 0.65f
#define CAM_MOMENTUM_THRESHOLD 1.0f
#define CAM_RX_MIN (-89.0f)
#define CAM_RX_MAX 89.0f
#define CAM_DIST_MIN 0.5f
#define CAM_DIST_MAX 50.0f

/* Default values for ReplCameraState — the previous home was
 * UI_STATE_INITIAL.camera in src/ui/state.c. */
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

static ReplCameraState       g_camera          = GLR_CAMERA_INITIAL;
static const ReplCameraState g_camera_defaults = GLR_CAMERA_INITIAL;

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

static void clamp_camera_pitch(ReplCameraState *c) {
    if (c->rx > CAM_RX_MAX) {
        c->rx = CAM_RX_MAX;
        g_vel_rx = 0.0f;
    }
    if (c->rx < CAM_RX_MIN) {
        c->rx = CAM_RX_MIN;
        g_vel_rx = 0.0f;
    }
}

static void clamp_camera_distance(ReplCameraState *c) {
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

static void drop_small_velocities(void) {
    g_vel_ry   = fabsf(g_vel_ry)   > CAM_MOMENTUM_THRESHOLD ? g_vel_ry   : 0.0f;
    g_vel_rx   = fabsf(g_vel_rx)   > CAM_MOMENTUM_THRESHOLD ? g_vel_rx   : 0.0f;
    g_vel_tx   = fabsf(g_vel_tx)   > CAM_MOMENTUM_THRESHOLD ? g_vel_tx   : 0.0f;
    g_vel_ty   = fabsf(g_vel_ty)   > CAM_MOMENTUM_THRESHOLD ? g_vel_ty   : 0.0f;
    g_vel_tz   = fabsf(g_vel_tz)   > CAM_MOMENTUM_THRESHOLD ? g_vel_tz   : 0.0f;
    g_vel_zoom = fabsf(g_vel_zoom) > CAM_MOMENTUM_THRESHOLD ? g_vel_zoom : 0.0f;
}

/* ---- Accessors ------------------------------------------------------ */

ReplCameraState glr_camera(void)        { return g_camera; }
ReplCameraState *glr_camera_mut(void)   { return &g_camera; }

void glr_camera_set(float rx, float ry, float dist,
                    float tx, float ty, float tz,
                    float motion_glow) {
    g_camera.rx = rx;
    g_camera.ry = ry;
    g_camera.dist = dist;
    g_camera.tx = tx;
    g_camera.ty = ty;
    g_camera.tz = tz;
    g_camera.motion_glow = motion_glow;
}

void glr_camera_set_orbit(float rx, float ry) {
    g_camera.rx = rx;
    g_camera.ry = ry;
}

void glr_camera_set_pan(float tx, float ty, float tz) {
    g_camera.tx = tx;
    g_camera.ty = ty;
    g_camera.tz = tz;
}

void glr_camera_set_distance(float dist)         { g_camera.dist = dist; }
void glr_camera_set_motion_glow(float mg)        { g_camera.motion_glow = mg; }
void glr_camera_reset_default(void)              { g_camera = g_camera_defaults; }

void glr_camera_capture(ReplCameraState *out) {
    if (out) *out = g_camera;
}

void glr_camera_restore(const ReplCameraState *snap) {
    if (snap) g_camera = *snap;
}

/* ---- Controls ------------------------------------------------------- */

void glr_camera_controls_reset(void) {
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
    ReplCameraState *c = &g_camera;
    int px = g_pointer_x;
    int py = g_pointer_y;
    int btn = g_pointer_button;
    int dx = x - px;
    int dy = y - py;

    if (btn == GLUT_LEFT_BUTTON) {
        c->ry += (float)dx * 0.5f;
        c->rx += (float)dy * 0.5f;
        c->ry = fmodf(c->ry, 360.0f);
        c->rx = clampf(c->rx, CAM_RX_MIN, CAM_RX_MAX);
        g_vel_rx *= CAM_DECAY;
        g_vel_ry *= CAM_DECAY;
        g_vel_ry += (float)dx * 0.25f;
        g_vel_rx += (float)dy * 0.25f;
    } else if (btn == GLUT_RIGHT_BUTTON) {
        float scale = 0.005f * c->dist;
        float fdy = (float)dy;
        if (g_mouse_mods & GLUT_ACTIVE_SHIFT) {
            /* Shift + right-drag: pan the orbit target along world Y. */
            float wdy = -fdy * scale;
            c->ty -= wdy;
            g_vel_ty *= CAM_DECAY;
            g_vel_ty += wdy * 0.5f;
            c->motion_glow = 1.0f;
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
            g_vel_tx += wdx * 0.5f;
            g_vel_tz += wdz * 0.5f;
            c->motion_glow = 1.0f;
        }
    } else if (btn == GLUT_MIDDLE_BUTTON) {
        c->dist += (float)dy * 0.02f;
        c->dist = clampf(c->dist, CAM_DIST_MIN, CAM_DIST_MAX);
    }

    g_pointer_x = x;
    g_pointer_y = y;
}

void glr_camera_tick(void) {
    ReplCameraState *c = &g_camera;

    if (g_pointer_button == -1) {
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
    if (pan_vel > 0.01f && c->motion_glow < 0.6f)
        c->motion_glow = 0.6f;
    c->motion_glow *= 0.94f;
    if (c->motion_glow < 0.005f)
        c->motion_glow = 0.0f;

    if (c->auto_rotate) {
        c->ry += 0.3f;
        c->ry = fmodf(c->ry, 360.0f);
    }
}
