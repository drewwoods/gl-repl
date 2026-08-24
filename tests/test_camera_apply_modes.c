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
    "display() {",
    "glTranslatef(0.0f, 0.0f, -12.0f);   // @camera dist",
    "glRotatef(41.0f, 1.0f, 0.0f, 0.0f);   // @camera rx",
    "glRotatef(52.0f, 0.0f, 1.0f, 0.0f);   // @camera ry",
    "glTranslatef(-1.0f, -2.0f, -3.0f);   // @camera pan",
    "glBegin(GL_POINTS);",
    "glVertex3f(0, 0, 0);",
    "glEnd();",
    "}",
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

/* The transform rows are the source of truth, which is the whole argument
 * against a one-line summary directive: a hand edit to an exported .c must
 * survive re-import rather than losing to a directive that disagrees. */
static void test_hand_edited_numbers_survive(void) {
    printf("--- hand-edited camera numbers survive re-import ---\n");
    const char *path = "/tmp/test_camera_hand_edit.c";
    FILE *f;

    glr_ctrl_reset_all();
    camera_bridge_stub_install(NULL);

    f = fopen(path, "w");
    ASSERT_TRUE("hand-edit fixture written", f != NULL);
    if (!f)
        return;
    fprintf(f,
        "void display(void) {\n"
        /* Someone opened the exported file and typed a different pitch. */
        "  glTranslatef(0.0000f, 0.0000f, -3.2500f);   /* @camera dist */\n"
        "  glRotatef(66.5000f, 1.0f, 0.0f, 0.0f);   /* @camera rx */\n"
        "  glRotatef(12.0000f, 0.0f, 1.0f, 0.0f);   /* @camera ry */\n"
        /* ... and left a non-zero g_angle behind, which is non-semantic and
         * must not move the imported pose - but must warn. */
        "  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);   /* @camera spin */\n"
        "  glTranslatef(0.0000f, 0.0000f, 0.0000f);   /* @camera pan */\n"
        "  // Snippet start\n"
        "  glVertex3f(0, 0, 0);\n"
        "  // Snippet end\n"
        "}\n");
    fclose(f);

    ASSERT_INT("hand-edited import succeeds",
               repl_export_load_from_file(path, NULL), 1);
    ASSERT_TRUE("the edited numbers are the imported pose",
                fabsf(g_camera_bridge_stub.applied.dist - 3.25f) < 1e-4f &&
                fabsf(g_camera_bridge_stub.applied.rx - 66.5f) < 1e-4f &&
                fabsf(g_camera_bridge_stub.applied.ry - 12.0f) < 1e-4f);
    remove(path);
}

/* A hand-edited g_angle initializer cannot move the pose - the yaw rides on
 * the ry row - but the disagreement between the compiled C and the imported
 * REPL is worth saying out loud. The shared reader cannot see that line, so
 * the warning is importer-local. */
static void test_nonzero_g_angle_warns(void) {
    printf("--- non-zero g_angle initializer warns ---\n");
    const char *path = "/tmp/test_camera_g_angle.c";
    FILE *f;

    glr_ctrl_reset_all();
    camera_bridge_stub_install(NULL);

    f = fopen(path, "w");
    ASSERT_TRUE("g_angle fixture written", f != NULL);
    if (!f)
        return;
    fprintf(f,
        "static float g_angle = 45.0000f;\n"
        "void display(void) {\n"
        "  glTranslatef(0.0000f, 0.0000f, -8.0000f);   /* @camera dist */\n"
        "  glRotatef(10.0000f, 1.0f, 0.0f, 0.0f);   /* @camera rx */\n"
        "  glRotatef(20.0000f, 0.0f, 1.0f, 0.0f);   /* @camera ry */\n"
        "  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);   /* @camera spin */\n"
        "  glTranslatef(0.0000f, 0.0000f, 0.0000f);   /* @camera pan */\n"
        "  // Snippet start\n"
        "  glVertex3f(0, 0, 0);\n"
        "  // Snippet end\n"
        "}\n");
    fclose(f);

    ASSERT_INT("g_angle import succeeds",
               repl_export_load_from_file(path, NULL), 1);
    ASSERT_TRUE("the yaw still comes from the ry row, not g_angle",
                fabsf(g_camera_bridge_stub.applied.ry - 20.0f) < 1e-4f);
    remove(path);
}

/* A load can land mid-ease. Merging a partial header against the transient
 * interpolated pose would bake an arbitrary frame of an animation into the
 * result, so capture_pose is specified as the *destination* reader. */
static void test_mid_ease_load_uses_destination(void) {
    printf("--- partial header merges against the destination ---\n");
    ReplCameraHeader hdr;
    ReplCameraFinish fin;
    int i;

    glr_ctrl_reset_all();
    glr_camera_export_install_bridge();
    glr_camera_set_orbit(5.0f, 5.0f);
    glr_camera_set_distance(5.0f);
    /* Arm a long ease, then load before it settles. */
    glr_camera_ease_to(60.0f, 70.0f, 20.0f, 0.0f, 0.0f, 0.0f);
    for (i = 0; i < 3; i++)
        glr_camera_tick();
    ASSERT_TRUE("the live pose is genuinely mid-ease",
                fabsf(glr_camera().rx - 60.0f) > 1.0f);

    repl_camera_header_init(&hdr);
    (void)repl_camera_header_offer(
        &hdr, "glTranslatef(0, 0, -9);   // @camera dist", 1);
    fin = repl_camera_header_finish(&hdr, REPL_CAMERA_APPLY_IMPORT);

    ASSERT_TRUE("the merged pose took the ease destination, not the frame",
                fabsf(fin.pose.rx - 60.0f) < 1e-3f &&
                fabsf(fin.pose.ry - 70.0f) < 1e-3f);
    ASSERT_TRUE("the supplied role still wins",
                fabsf(fin.pose.dist - 9.0f) < 1e-3f);
}

/* A scene switched away from before its camera ease finished must hand the
 * incoming scene its *intended* pose, not the interpolation frame the last
 * tick happened to produce. glr_ctrl_reset_transients() - which every scene
 * load runs first - used to cancel the ease in place, so how far the camera
 * got depended only on how fast the user pressed the key, and a scene with no
 * `@camera` rows inherited that arbitrary pose verbatim. */
static void test_quick_scene_switch_settles_outgoing_ease(void) {
    printf("--- a quick scene switch settles the outgoing ease ---\n");
    static const char *const body_only[] = {
        "display() {",
        "glBegin(GL_POINTS);",
        "glVertex3f(1, 1, 1);",
        "glEnd();",
        "}",
        NULL
    };
    int i;

    /* reset_all installs the app's real camera bridge; this test needs the
     * real ease, so no stub goes in after it. */
    glr_ctrl_reset_all();
    glr_ctrl_reset_transients();
    ASSERT_TRUE("the tagged scene loads", repl_load_example_lines(k_tagged_scene) > 0);
    for (i = 0; i < 2; i++)
        glr_camera_tick();
    ASSERT_TRUE("the outgoing scene is genuinely mid-ease",
                glr_camera_target_active() &&
                fabsf(glr_camera().dist - 12.0f) > 1.0f);

    /* Switch scenes before the ease lands. */
    glr_ctrl_reset_transients();
    ASSERT_TRUE("no ease survives the transition", !glr_camera_target_active());
    ASSERT_TRUE("the live pose settled on the outgoing scene's target",
                fabsf(glr_camera().dist - 12.0f) < 1e-3f &&
                fabsf(glr_camera().rx - 41.0f) < 1e-3f &&
                fabsf(glr_camera().ry - 52.0f) < 1e-3f &&
                fabsf(glr_camera().tx - 1.0f) < 1e-3f);

    /* A scene carrying no camera rows inherits the live pose - which is now
     * the previous scene's intended pose, whatever the switch timing was. */
    ASSERT_TRUE("the camera-less scene loads", repl_load_example_lines(body_only) > 0);
    for (i = 0; i < 200; i++)
        glr_camera_tick();
    ASSERT_TRUE("the inherited pose is the intended one, not a partway frame",
                fabsf(glr_camera().dist - 12.0f) < 1e-3f &&
                fabsf(glr_camera().rx - 41.0f) < 1e-3f &&
                fabsf(glr_camera().ry - 52.0f) < 1e-3f);
}

/* The example body cap is a *body* budget. Metadata and camera rows are
 * consumed before a line reaches it, so counting the raw source index would
 * fail a scene merely for carrying @cfg rows. */
static void test_body_budget_excludes_metadata(void) {
    printf("--- example body budget counts body lines only ---\n");
    static const char *lines[EXAMPLE_BODY_LINES_MAX + 16];
    int n = 0;
    int i;

    glr_ctrl_reset_all();
    camera_bridge_stub_install(NULL);

    lines[n++] = "// @cfg axes = 4";
    lines[n++] = "// @cfg grid = GRID_THEME_CLASSIC";
    lines[n++] = "display() {";
    lines[n++] = "glTranslatef(0.0f, 0.0f, -4.0f);   // @camera dist";
    lines[n++] = "glRotatef(1.0f, 1.0f, 0.0f, 0.0f);   // @camera rx";
    lines[n++] = "glRotatef(2.0f, 0.0f, 1.0f, 0.0f);   // @camera ry";
    lines[n++] = "glTranslatef(0.0f, 0.0f, 0.0f);   // @camera pan";
    /* Exactly the budget in body rows, on top of six consumed metadata rows -
     * so the raw index runs past the cap while the body does not. */
    for (i = 0; i < EXAMPLE_BODY_LINES_MAX; i++)
        lines[n++] = "glVertex3f(0, 0, 0);";
    lines[n++] = "}";
    lines[n] = NULL;

    ASSERT_TRUE("a full body plus metadata still loads",
                repl_load_example_lines(lines) > 0);
    ASSERT_INT("the camera was still applied",
               g_camera_bridge_stub.apply_count, 1);
}

int main(void) {
    printf("=== camera apply modes ===\n");
    test_example_mode();
    test_restore_mode();
    test_import_mode();
    test_external_3d_pose_is_deferred();
    test_hand_edited_numbers_survive();
    test_nonzero_g_angle_warns();
    test_mid_ease_load_uses_destination();
    test_quick_scene_switch_settles_outgoing_ease();
    test_body_budget_excludes_metadata();
    printf("\n=== Results: ");
    return test_harness_report(&g_harness, "camera_apply_modes");
}
