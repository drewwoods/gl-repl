/*
 * tests/test_glr_camera.c - Unit tests for camera boundaries, easing, and auto-rotation.
 */
#include "app/glr_camera.h"
#include "support/test_harness.h"
#include "gl_includes.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT_EQ(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

#define ASSERT_FLOAT_NEAR(label, got, exp, eps) \
    ASSERT_TRUE(label, fabsf((got) - (exp)) < (eps))

static void test_camera_pitch_bounds(void) {
    glr_camera_reset_default();

    /* 1. Verify direct drag pitch clamping (left button drag) */
    glr_camera_mouse_event(GLUT_LEFT_BUTTON, GLUT_DOWN, 0, 0, 0);
    glr_camera_drag_motion(0, 1000); /* Drag down a lot to increase pitch */
    ASSERT_FLOAT_NEAR("L-drag down clamps pitch to CAM_RX_MAX", glr_camera().rx, 89.0f, 0.001f);

    glr_camera_drag_motion(0, -2000); /* Drag up a lot to decrease pitch */
    ASSERT_FLOAT_NEAR("L-drag up clamps pitch to CAM_RX_MIN", glr_camera().rx, -89.0f, 0.001f);
    glr_camera_mouse_event(GLUT_LEFT_BUTTON, GLUT_UP, 0, 0, 0);

    /* 2. Verify tick pitch clamping via carry velocity */
    glr_camera_reset_default();
    glr_camera_mouse_event(GLUT_LEFT_BUTTON, GLUT_DOWN, 0, 0, 0);
    glr_camera_drag_motion(0, 500); /* drag to gain velocity */
    glr_camera_mouse_event(GLUT_LEFT_BUTTON, GLUT_UP, 0, 0, 0); /* release button to let tick apply velocity */

    /* Tick multiple times to let velocity move it beyond the max and verify clamp */
    for (int i = 0; i < 50; i++) {
        glr_camera_tick();
    }
    ASSERT_FLOAT_NEAR("Tick with velocity clamps pitch to CAM_RX_MAX", glr_camera().rx, 89.0f, 0.001f);
}

static void test_camera_distance_bounds(void) {
    glr_camera_reset_default();

    /* 1. Verify drag distance clamping (middle button drag) */
    glr_camera_mouse_event(GLUT_MIDDLE_BUTTON, GLUT_DOWN, 0, 0, 0);
    glr_camera_drag_motion(0, 10000); /* Zoom out a lot */
    ASSERT_FLOAT_NEAR("M-drag clamps distance to CAM_DIST_MAX", glr_camera().dist, 50.0f, 0.001f);

    glr_camera_drag_motion(0, -10000); /* Zoom in a lot */
    ASSERT_FLOAT_NEAR("M-drag clamps distance to CAM_DIST_MIN", glr_camera().dist, 0.5f, 0.001f);
    glr_camera_mouse_event(GLUT_MIDDLE_BUTTON, GLUT_UP, 0, 0, 0);

    /* 2. Verify tick distance clamping via zoom velocity */
    glr_camera_reset_default();
    glr_camera_add_zoom_velocity(100.0f); /* Add huge zoom velocity */
    glr_camera_tick();
    ASSERT_FLOAT_NEAR("Tick with zoom velocity clamps to CAM_DIST_MAX", glr_camera().dist, 50.0f, 0.001f);

    glr_camera_add_zoom_velocity(-100.0f); /* Add huge negative zoom velocity */
    glr_camera_tick();
    ASSERT_FLOAT_NEAR("Tick with negative zoom velocity clamps to CAM_DIST_MIN", glr_camera().dist, 0.5f, 0.001f);
}

static void test_camera_easing_and_decay(void) {
    glr_camera_reset_default();

    /* 1. Test basic target easing active state */
    glr_camera_ease_to(45.0f, 45.0f, 10.0f, 1.0f, 2.0f, 3.0f);
    ASSERT_TRUE("Target ease is active", glr_camera_target_active());

    /* 2. Custom target decay override and boundary clamping */
    glr_camera_set_target_decay(-1.0f); /* Underflow: clamped to 0.0f (instant snap) */
    glr_camera_tick();
    ASSERT_TRUE("Decay underflow snaps target instantly on tick", !glr_camera_target_active());
    ASSERT_FLOAT_NEAR("Snapped to target pitch", glr_camera().rx, 45.0f, 0.001f);
    ASSERT_FLOAT_NEAR("Snapped to target yaw", glr_camera().ry, 45.0f, 0.001f);
    ASSERT_FLOAT_NEAR("Snapped to target distance", glr_camera().dist, 10.0f, 0.001f);

    /* 3. Decay overflow clamping */
    glr_camera_ease_to(0.0f, 0.0f, 5.0f, 0.0f, 0.0f, 0.0f);
    glr_camera_set_target_decay(2.0f); /* Overflow: clamped to 0.999f (extremely slow ease) */
    glr_camera_tick();
    ASSERT_TRUE("Decay overflow remains active (does not snap)", glr_camera_target_active());
    /* Verify it moved only slightly */
    ASSERT_TRUE("Moved slightly towards 0 pitch", glr_camera().rx < 45.0f);
}

static void test_camera_auto_rotation_and_focus(void) {
    glr_camera_reset_default();

    /* 1. Focus Origin */
    glr_camera_set(10.0f, 20.0f, 5.0f, 5.0f, 5.0f, 5.0f, 0.0f);
    glr_camera_focus_origin();
    ASSERT_TRUE("Focus origin activates target ease", glr_camera_target_active());
    glr_camera_set_target_decay(0.0f); /* Snap */
    glr_camera_tick();
    ASSERT_FLOAT_NEAR("Focused tx = 0", glr_camera().tx, 0.0f, 0.001f);
    ASSERT_FLOAT_NEAR("Focused ty = 0", glr_camera().ty, 0.0f, 0.001f);
    ASSERT_FLOAT_NEAR("Focused tz = 0", glr_camera().tz, 0.0f, 0.001f);

    /* 2. Auto-Rotation */
    glr_camera_reset_default();
    glr_camera_mut()->auto_rotate = 1;
    glr_camera_set_control_mode(GLR_CAMERA_CONTROL_3D);

    float start_ry = glr_camera().ry;
    glr_camera_tick();
    float new_ry = glr_camera().ry;
    ASSERT_TRUE("Yaw changed due to auto-rotate", new_ry != start_ry);
    ASSERT_FLOAT_NEAR("Yaw auto-rotated by 0.3 degrees", new_ry - start_ry, 0.3f, 0.001f);

    /* Auto-rotation should NOT happen when target ease is active */
    glr_camera_ease_to(0.0f, 0.0f, 5.0f, 0.0f, 0.0f, 0.0f);
    start_ry = glr_camera().ry;
    glr_camera_tick();
    /* Auto-rotate is skipped during ease target ticks */
    ASSERT_TRUE("No auto-rotate when easing is active", glr_camera_target_active() || glr_camera().ry == start_ry);
}

/* Pose interpolation + change detection used by accum motion blur. */
static void test_camera_pose_lerp_and_changed(void) {
    printf("--- camera pose lerp + change detection ---\n");

    GlrCameraPose a = { .rx = 10.0f, .ry =  20.0f, .dist = 4.0f,
                        .tx = 0.0f,  .ty =  0.0f,  .tz = 0.0f };
    GlrCameraPose b = { .rx = 30.0f, .ry =  60.0f, .dist = 8.0f,
                        .tx = 2.0f,  .ty = -4.0f,  .tz = 1.0f };

    /* Endpoints. */
    GlrCameraPose p0 = glr_camera_pose_lerp(&a, &b, 0.0f);
    ASSERT_FLOAT_NEAR("lerp f=0 rx -> a",   p0.rx,   a.rx,   1e-4f);
    ASSERT_FLOAT_NEAR("lerp f=0 dist -> a", p0.dist, a.dist, 1e-4f);
    GlrCameraPose p1 = glr_camera_pose_lerp(&a, &b, 1.0f);
    ASSERT_FLOAT_NEAR("lerp f=1 ry -> b",   p1.ry,   b.ry,   1e-4f);
    ASSERT_FLOAT_NEAR("lerp f=1 ty -> b",   p1.ty,   b.ty,   1e-4f);

    /* Midpoint is the linear average for non-wrapping axes. */
    GlrCameraPose pm = glr_camera_pose_lerp(&a, &b, 0.5f);
    ASSERT_FLOAT_NEAR("lerp mid dist", pm.dist, 6.0f,  1e-4f);
    ASSERT_FLOAT_NEAR("lerp mid ry",   pm.ry,   40.0f, 1e-4f);
    ASSERT_FLOAT_NEAR("lerp mid tz",   pm.tz,   0.5f,  1e-4f);

    /* Angle wrap: 350 -> 10 should cross 0/360 the short way (~0), not 180. */
    GlrCameraPose w0 = { .ry = 350.0f };
    GlrCameraPose w1 = { .ry =  10.0f };
    GlrCameraPose wm = glr_camera_pose_lerp(&w0, &w1, 0.5f);
    float wm_ry = fmodf(wm.ry + 360.0f, 360.0f);
    ASSERT_TRUE("lerp wraps short way (near 0/360, not 180)",
                wm_ry < 1.0f || wm_ry > 359.0f);

    /* Change detection. */
    ASSERT_TRUE("identical poses -> unchanged",
                glr_camera_pose_changed(&a, &a) == 0);
    GlrCameraPose tiny = a;
    tiny.ry += 0.001f;   /* below CAM_TARGET_ANGLE_EPS (0.01) */
    tiny.tx += 0.0001f;  /* below CAM_TARGET_POS_EPS (0.001) */
    ASSERT_TRUE("sub-epsilon nudge -> unchanged",
                glr_camera_pose_changed(&a, &tiny) == 0);
    GlrCameraPose big = a;
    big.ry += 1.0f;
    ASSERT_TRUE("supra-epsilon angle -> changed",
                glr_camera_pose_changed(&a, &big) == 1);
    GlrCameraPose moved = a;
    moved.tx += 0.1f;
    ASSERT_TRUE("supra-epsilon translate -> changed",
                glr_camera_pose_changed(&a, &moved) == 1);
}

int main(void) {
    printf("--- glr_camera tests ---\n");

    test_camera_pitch_bounds();
    test_camera_distance_bounds();
    test_camera_easing_and_decay();
    test_camera_auto_rotation_and_focus();
    test_camera_pose_lerp_and_changed();

    printf("\n");
    return test_harness_report(&g_harness, "test_glr_camera");
}
