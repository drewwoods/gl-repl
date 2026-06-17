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

#include "gl_includes.h"      /* GLUT_*BUTTON constants, M_PI fallback */
#include <math.h>

/* Orbit/pan momentum decay per frame. Deliberately independent of
 * GLR_CAMERA_TARGET_DECAY and CAM_DECAY_ZOOM in glr_camera.h; they are
 * different knobs. */
#define CAM_DECAY 0.88f
#define CAM_TARGET_ANGLE_EPS 0.01f
#define CAM_TARGET_POS_EPS 0.001f
#define CAM_MOMENTUM_THRESHOLD 1.0f
#define CAM_RX_MIN (-89.0f)
#define CAM_RX_MAX 89.0f
#define CAM_DIST_MIN 0.5f
#define CAM_DIST_MAX 50.0f
#define CAM_AUTO_ROTATE_SPEED 0.3f

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
/* Per-ease decay override: each new ease resets to GLR_CAMERA_TARGET_DECAY;
 * callers that want to run faster/slower than the global default (e.g. the
 * 3D->2D view-mode transition) call glr_camera_set_target_decay AFTER
 * glr_camera_ease_to to override for that ease only. */
static float                 g_camera_target_decay = GLR_CAMERA_TARGET_DECAY;
static GlrCameraControlMode  g_control_mode    = GLR_CAMERA_CONTROL_3D;

static GlrCameraState       g_scene_camera_default;
static int                   g_scene_camera_default_set = 0;

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
    float k = 1.0f - g_camera_target_decay;
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
    glr_camera_set_target_decay(GLR_CAMERA_TARGET_DECAY);
    reset_velocities();
    if (target_deltas_within_epsilon(&g_camera))
        snap_to_target();
}

static void camera_rotate_relative(float rx_deg, float ry_deg,
                                   float dx, float dy, float dz,
                                   float *out_x, float *out_y, float *out_z) {
    float rx = rx_deg * (float)M_PI / 180.0f;
    float ry = ry_deg * (float)M_PI / 180.0f;
    float cry = cosf(ry), sry = sinf(ry);
    float crx = cosf(rx), srx = sinf(rx);

    /* Matches glr_camera_load_modelview's matrix order:
     * glRotatef(rx, X) then glRotatef(ry, Y), so vertices see Y then X. */
    float x1 = cry * dx + sry * dz;
    float y1 = dy;
    float z1 = -sry * dx + cry * dz;

    if (out_x) *out_x = x1;
    if (out_y) *out_y = crx * y1 - srx * z1;
    if (out_z) *out_z = srx * y1 + crx * z1;
}

/* Fit a captured world-space bbox against the camera target orientation.
 * The earlier sphere fit was deliberately conservative, but it wasted the
 * horizontal room in wide top/bottom code-panel layouts. Projecting the 8
 * bbox corners into the intended camera orientation lets aspect ratio do the
 * right thing while still accounting for near/far depth inside the box. */
int glr_camera_ease_fit_bounds(const float bounds_min[3],
                               const float bounds_max[3],
                               float fovy_deg,
                               float aspect,
                               float margin) {
    if (!bounds_min || !bounds_max || fovy_deg <= 0.0f || aspect <= 0.0f)
        return 0;

    GlrCameraState target = g_camera_target_active ? g_camera_target : g_camera;
    float center[3];
    float largest_half = 0.0f;
    for (int a = 0; a < 3; a++) {
        if (bounds_max[a] < bounds_min[a])
            return 0;
        center[a] = 0.5f * (bounds_min[a] + bounds_max[a]);
        float half = 0.5f * (bounds_max[a] - bounds_min[a]);
        if (half > largest_half)
            largest_half = half;
    }
    if (largest_half <= 0.0f)
        return 0;

    float tan_y = tanf(fovy_deg * (float)M_PI / 360.0f);
    if (tan_y <= 0.0f)
        return 0;
    float tan_x = tan_y * aspect;
    if (tan_x <= 0.0f)
        return 0;
    if (margin <= 0.0f)
        margin = 1.0f;

    float dist = 0.0f;
    for (int mask = 0; mask < 8; mask++) {
        float wx = (mask & 1) ? bounds_max[0] : bounds_min[0];
        float wy = (mask & 2) ? bounds_max[1] : bounds_min[1];
        float wz = (mask & 4) ? bounds_max[2] : bounds_min[2];
        float vx, vy, vz;
        camera_rotate_relative(target.rx, target.ry,
                               wx - center[0],
                               wy - center[1],
                               wz - center[2],
                               &vx, &vy, &vz);
        float req_x = vz + fabsf(vx) / tan_x;
        float req_y = vz + fabsf(vy) / tan_y;
        if (req_x > dist) dist = req_x;
        if (req_y > dist) dist = req_y;
    }

    if (dist <= 0.0f)
        return 0;
    dist *= margin;

    target.dist = dist;
    target.tx = center[0];
    target.ty = center[1];
    target.tz = center[2];
    glr_camera_set_scene_default(&target);
    glr_camera_ease_to(target.rx, target.ry, target.dist,
                       target.tx, target.ty, target.tz);
    return 1;
}

/* Override the per-ease decay for the current target. Call AFTER
 * glr_camera_ease_to (which resets to the global default). Values in
 * [0, 1): lower = faster (more of the remaining distance applied per
 * frame); higher = slower; 0.0f snaps to the target on the next tick.
 * Out-of-range values are silently clamped to keep the ease
 * well-behaved. */
void glr_camera_set_target_decay(float decay) {
    if (decay < 0.0f) decay = 0.0f;
    if (decay > 0.999f) decay = 0.999f;
    g_camera_target_decay = decay;
}

void glr_camera_reset_default(void) {
    cancel_target_ease();
    reset_velocities();
    g_control_mode = GLR_CAMERA_CONTROL_3D;
    g_camera = g_camera_defaults;
    g_scene_camera_default_set = 0;
}

/* Pure GL modelview load. Audit #11: this used to live in
 * src/scene/render.c as scene_apply_camera; scene called it,
 * scene readme insisted "scene does not own camera state", and the
 * scene renderer refused to invoke it on its own. Moved here so
 * the function and the camera type both sit with the app's camera
 * owner. scene_render_3d_scene now documents the precondition
 * (modelview populated) without naming a specific helper. */
GlrCameraPose glr_camera_pose_from_state(const GlrCameraState *state) {
    GlrCameraPose pose = {0};

    if (!state)
        return pose;

    pose.rx = state->rx;
    pose.ry = state->ry;
    pose.dist = state->dist;
    pose.tx = state->tx;
    pose.ty = state->ty;
    pose.tz = state->tz;
    return pose;
}

void glr_camera_load_modelview(const GlrCameraPose *pose) {
    if (!pose) return;
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -pose->dist);
    glRotatef(pose->rx, 1, 0, 0);
    glRotatef(pose->ry, 0, 1, 0);
    glTranslatef(-pose->tx, -pose->ty, -pose->tz);
}

GlrCameraPose glr_camera_pose_lerp(const GlrCameraPose *a,
                                   const GlrCameraPose *b, float f) {
    GlrCameraPose o = {0};
    if (!a || !b)
        return o;
    o.rx   = a->rx   + shortest_angle_delta(a->rx, b->rx) * f;
    o.ry   = a->ry   + shortest_angle_delta(a->ry, b->ry) * f;
    o.dist = a->dist + (b->dist - a->dist) * f;
    o.tx   = a->tx   + (b->tx - a->tx) * f;
    o.ty   = a->ty   + (b->ty - a->ty) * f;
    o.tz   = a->tz   + (b->tz - a->tz) * f;
    return o;
}

int glr_camera_pose_changed(const GlrCameraPose *prev,
                            const GlrCameraPose *cur) {
    if (!prev || !cur)
        return 0;
    return fabsf(shortest_angle_delta(prev->rx, cur->rx)) > CAM_TARGET_ANGLE_EPS ||
           fabsf(shortest_angle_delta(prev->ry, cur->ry)) > CAM_TARGET_ANGLE_EPS ||
           fabsf(cur->dist - prev->dist) > CAM_TARGET_POS_EPS ||
           fabsf(cur->tx - prev->tx) > CAM_TARGET_POS_EPS ||
           fabsf(cur->ty - prev->ty) > CAM_TARGET_POS_EPS ||
           fabsf(cur->tz - prev->tz) > CAM_TARGET_POS_EPS;
}

void glr_camera_focus_origin(void) {
    glr_camera_ease_to(g_camera.rx, g_camera.ry, g_camera.dist,
                       0.0f, 0.0f, 0.0f);
}

void glr_camera_ease_to_default(void) {
    const GlrCameraState *d = g_scene_camera_default_set
                                  ? &g_scene_camera_default
                                  : &g_camera_defaults;
    g_control_mode = GLR_CAMERA_CONTROL_3D;
    glr_camera_ease_to(d->rx, d->ry, d->dist, d->tx, d->ty, d->tz);
}

void glr_camera_capture(GlrCameraState *out) {
    if (out) *out = g_camera;
}

void glr_camera_restore(const GlrCameraState *snap) {
    if (snap) {
        cancel_target_ease();
        g_camera = *snap;
        reset_velocities();
        g_pointer_button = -1;
    }
}

void glr_camera_set_scene_default(const GlrCameraState *pose) {
    if (pose) {
        g_scene_camera_default = *pose;
        g_scene_camera_default_set = 1;
    }
}

void glr_camera_clear_scene_default(void) {
    g_scene_camera_default_set = 0;
}

/* ---- Controls ------------------------------------------------------- */

/*
 * Resets pointer, modifier, and velocity states to defaults (clears momentum).
 * Note that the active camera control mode (2D vs 3D) is preserved;
 * use glr_camera_reset_default() for a full state reset.
 */
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

    /* Intentional double-decay: drag paths damp the carry velocity here
     * and glr_camera_tick() damps it again once per frame so active
     * interaction feels snappier than free momentum. */

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
        c->ry += CAM_AUTO_ROTATE_SPEED;
        c->ry = fmodf(c->ry, 360.0f);
    }
}
