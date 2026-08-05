/*
 * tests/test_camera_apply_modes.c - Each caller passes the mode it means.
 *
 * The apply mode carries two independent decisions that used to be side
 * effects of *which parser ran*: whether the transition snaps or eases, and
 * whether the applied pose becomes the scene default. A snap/ease flag alone
 * would lose the second, and a snapshot restore that adopted a scene default
 * silently redefines what "Reset camera" returns to - the bug this plan
 * exists to prevent, reintroduced by its own API. This test exists for that
 * line specifically.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app/glr_camera.h"
#include "app/glr_camera_export.h"
#include "app/glr_ctrl.h"
#include "editor/state.h"
#include "repl/camera_header.h"
#include "repl/example_loader.h"
#include "repl/export.h"
#include "repl/scene_snapshot.h"
#include "support/camera_bridge_stub.h"
#include "support/test_harness.h"

static TestHarness g_harness = TEST_HARNESS_INIT;
#define ASSERT_TRUE(label, cond)      TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, want)  TEST_ASSERT_INT(&g_harness, label, got, want)

static const char *const k_tagged_scene[] = {
    "glTranslatef(0.0f, 0.0f, -12.0f);   // @camera dist",
    "glRotatef(41.0f, 1.0f, 0.0f, 0.0f);   // @camera rx",
    "glRotatef(52.0f, 0.0f, 1.0f, 0.0f);   // @camera ry",
    "glTranslatef(-1.0f, -2.0f, -3.0f);   // @camera pan",
    "glBegin(GL_POINTS);",
    "glVertex3f(0, 0, 0);",
    "glEnd();",
    NULL
};

/* An example / tutorial scaffold eases and adopts the pose as the scene
 * default, so "Reset camera" returns to the example the user is looking at. */
static void test_example_mode(void) {
    printf("--- example load passes EXAMPLE ---\n");

    /* reset_all installs the app's own camera bridge, so the stub goes in
     * after it. */
    glr_ctrl_reset_all();
    camera_bridge_stub_install(NULL);
    repl_load_example_lines(k_tagged_scene);

    ASSERT_INT("the bridge is called exactly once",
               g_camera_bridge_stub.apply_count, 1);
    ASSERT_INT("example load means EXAMPLE",
               (int)g_camera_bridge_stub.mode,
               (int)REPL_CAMERA_APPLY_EXAMPLE);
    ASSERT_TRUE("the resolved pose reaches the bridge",
                fabsf(g_camera_bridge_stub.applied.dist - 12.0f) < 1e-4f &&
                fabsf(g_camera_bridge_stub.applied.rx - 41.0f) < 1e-4f &&
                fabsf(g_camera_bridge_stub.applied.ry - 52.0f) < 1e-4f &&
                fabsf(g_camera_bridge_stub.applied.tx - 1.0f) < 1e-4f);
}

/* A snapshot restore snaps and leaves the scene default alone. */
static void test_restore_mode(void) {
    printf("--- snapshot restore passes RESTORE ---\n");
    ReplCameraPose destination;
    SceneSnapshot *snap = (SceneSnapshot *)malloc(sizeof(SceneSnapshot));

    ASSERT_TRUE("snapshot scratch allocated", snap != NULL);
    if (!snap)
        return;

    destination.dist = 7.5f; destination.rx = 12.0f; destination.ry = 34.0f;
    destination.tx = 0.5f;   destination.ty = 1.5f;  destination.tz = 2.5f;
    glr_ctrl_reset_all();
    camera_bridge_stub_install(&destination);

    scene_snapshot_capture_live(snap);
    ASSERT_INT("capture reads the destination, not the live pose",
               g_camera_bridge_stub.capture_count > 0, 1);
    ASSERT_TRUE("the snapshot carries a pose, not formatted text",
                snap->camera_valid == 1 &&
                fabsf(snap->camera_pose.dist - 7.5f) < 1e-4f);

    g_camera_bridge_stub.apply_count = 0;
    (void)scene_snapshot_apply_live(snap, SCENE_SNAPSHOT_CAMERA_SNAP);
    ASSERT_INT("snapshot restore applies once",
               g_camera_bridge_stub.apply_count, 1);
    ASSERT_INT("snapshot restore means RESTORE - never IMPORT",
               (int)g_camera_bridge_stub.mode,
               (int)REPL_CAMERA_APPLY_RESTORE);

    g_camera_bridge_stub.apply_count = 0;
    (void)scene_snapshot_apply_live(snap, SCENE_SNAPSHOT_CAMERA_EASE);
    ASSERT_INT("an easing snapshot restore means EXAMPLE",
               (int)g_camera_bridge_stub.mode,
               (int)REPL_CAMERA_APPLY_EXAMPLE);
    free(snap);
}

/* A file load snaps and adopts: a loaded file's authored pose is what "Reset
 * camera" returns to, which is exactly what the old state-4 gate bought. */
static void test_import_mode(void) {
    printf("--- file import passes IMPORT ---\n");
    const char *path = "/tmp/test_camera_apply_modes_import.c";
    FILE *f = fopen(path, "w");

    ASSERT_TRUE("import fixture written", f != NULL);
    if (!f)
        return;
    fprintf(f,
        "// @scene-name Modes\n"
        "void display(void) {\n"
        "  glTranslatef(0.0000f, 0.0000f, -6.0000f);   /* @camera dist */\n"
        "  glRotatef(21.0000f, 1.0f, 0.0f, 0.0f);   /* @camera rx */\n"
        "  glRotatef(43.0000f, 0.0f, 1.0f, 0.0f);   /* @camera ry */\n"
        "  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);   /* @camera spin */\n"
        "  glTranslatef(0.0000f, 0.0000f, 0.0000f);   /* @camera pan */\n"
        "  // Snippet start\n"
        "  glVertex3f(0, 0, 0);\n"
        "  // Snippet end\n"
        "}\n");
    fclose(f);

    glr_ctrl_reset_all();
    camera_bridge_stub_install(NULL);
    ASSERT_INT("import succeeds",
               repl_export_load_from_file(path, NULL), 1);
    ASSERT_INT("import applies once", g_camera_bridge_stub.apply_count, 1);
    ASSERT_INT("file load means IMPORT",
               (int)g_camera_bridge_stub.mode, (int)REPL_CAMERA_APPLY_IMPORT);
    ASSERT_TRUE("the yaw comes from the ry row, not the spin hook",
                fabsf(g_camera_bridge_stub.applied.ry - 43.0f) < 1e-4f);
    remove(path);
}

/* The app-side bridge enqueues the external-3D-pose record rather than
 * calling it: the record's precondition is "after the camera modelview is
 * loaded in the display frame", and an example load runs off the action path.
 * Carrying the call inside apply_pose is how the violation stayed invisible. */
static void test_external_3d_pose_is_deferred(void) {
    printf("--- external 3D pose record is enqueued, then drained ---\n");
    float rx = 0.0f, ry = 0.0f, tz = 0.0f;

    glr_camera_export_install_bridge();
    /* Drain anything an earlier test left pending. */
    while (glr_camera_export_take_pending_3d_pose(&rx, &ry, &tz)) {}

    glr_ctrl_reset_all();
    editor_state_edit_line_set(repl_load_example_lines(k_tagged_scene));

    ASSERT_INT("an example load enqueues exactly one record",
               glr_camera_export_take_pending_3d_pose(&rx, &ry, &tz), 1);
    ASSERT_TRUE("the enqueued record carries the example's pose",
                fabsf(rx - 41.0f) < 1e-4f && fabsf(ry - 52.0f) < 1e-4f);
    ASSERT_INT("and it drains exactly once",
               glr_camera_export_take_pending_3d_pose(&rx, &ry, &tz), 0);
}

int main(void) {
    printf("=== camera apply modes ===\n");
    test_example_mode();
    test_restore_mode();
    test_import_mode();
    test_external_3d_pose_is_deferred();
    printf("\n=== Results: ");
    return test_harness_report(&g_harness, "camera_apply_modes");
}
