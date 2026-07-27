/*
 * test_glr_tour_snapshot.c - whole-app tour-baseline round trip.
 *
 * Populates the owner modules the tour baseline captures (scenes, variables,
 * editor input/selection, config, camera target/momentum, replay, tutorial,
 * menus, undo), snapshots the baseline, mutates every owner, restores, and
 * asserts every public view returns to the baseline. Also asserts the flat
 * program is left dirty and rebuilds to a non-empty program, and that a NULL
 * restore is a safe no-op.
 *
 * Runs under GL stubs (make test-stubs) and links CORE_TEST_OBJS.
 */
#include "app/glr_tour_snapshot.h"
#include "app/glr_ctrl.h"
#include "app/glr_state.h"
#include "app/glr_camera.h"
#include "repl/state.h"
#include "repl/state_owners.h"
#include "repl/scenes.h"
#include "repl/eval.h"
#include "repl/example_loader.h"
#include "repl/pipeline.h"
#include "repl/tutorials.h"
#include "subsystems/tutorial/tutorial.h"
#include "editor/state.h"
#include "editor/input.h"
#include "editor/undo.h"
#include "ui/app/state.h"
#include "ui/app/menu_bar.h"
#include "subsystems/replay/replay_state.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "subsystems/variable_panel/variable_panel_state.h"

#include "support/test_harness.h"
#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond)  TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, g, e)   TEST_ASSERT_INT(&g_harness, label, g, e)
#define ASSERT_STR(label, g, e)   TEST_ASSERT_STR(&g_harness, label, g, e)
#define ASSERT_FLOAT(label, g, e) TEST_ASSERT_FLOAT_DEFAULT(&g_harness, label, g, e)

/* Build a small but broadly-populated app state: two user scenes, a user
 * predefined variable, input text + selection, a non-default config toggle, an
 * eased camera with lingering momentum, and poked replay/tutorial/var-panel
 * fields. */
static int g_myvar_idx = -1;

static void populate_state(void) {
    char err[128];

    glr_ctrl_reset_all();

    /* Two user scenes. create_empty seeds a fresh slot and makes it active;
     * the second call flushes the first back into its slot. */
    (void)repl_scenes_create_empty_user_scene();
    (void)repl_scenes_create_empty_user_scene();
    editor_feed_line("glColor3f(1, 0, 0)");
    editor_feed_line("glVertex3f(0, 1, 0)");
    editor_feed_line("glVertex3f(1, 0, 0)");

    /* A user predef variable with a distinctive value. */
    if (repl_eval_find_predef_var_idx("myvar") < 0)
        repl_eval_declare_predef_var("myvar", err, sizeof(err));
    g_myvar_idx = repl_eval_find_predef_var_idx("myvar");
    if (g_myvar_idx >= 0)
        g_predef_vars_mut[g_myvar_idx].value = 3.5f;

    /* Editor input row + selection + scroll. */
    editor_input_set_text("glNormal3f(0, 0, 1)");
    editor_cursor_pos_set(4);
    editor_state_selection_set(1, 2);
    editor_scroll_set(2);

    /* Config toggle. */
    glr_state_presentation_mut()->wireframe = 1;

    /* UI chrome. */
    ui_state_pointer_set(210, 330, 1);
    ui_state_status_set("baseline status");

    /* Camera: distinct pose + an ease target + momentum. */
    glr_camera_set(15.0f, 40.0f, 6.0f, 0.5f, -0.5f, 0.25f, 0.0f);
    glr_camera_ease_to(30.0f, 60.0f, 4.0f, 1.0f, 0.0f, 0.0f);
    glr_camera_add_zoom_velocity(2.5f);

    /* Peer fields. */
    replay_state_mut()->src_line_idx = 7;
    replay_state_mut()->speed = 3.0f;
    tutorial_state_mut()->step = 4;
    variable_panel_drag_mut()->var_idx = 6;

    /* Rebuild the flat program so the baseline has a valid derived program. */
    repl_refresh_flat_program(editor_state_edit_line());
}

/* Mutate every captured owner away from the baseline. */
static void mutate_state(void) {
    (void)repl_scenes_create_empty_user_scene();  /* 3rd scene, new active */

    if (g_myvar_idx >= 0)
        g_predef_vars_mut[g_myvar_idx].value = 99.0f;

    editor_input_set_text("XYZ");
    editor_cursor_pos_set(0);
    editor_state_selection_clear();
    editor_scroll_set(0);

    glr_state_presentation_mut()->wireframe = 0;
    ui_state_pointer_set(1, 1, -1);
    ui_state_status_set("mutated status");

    glr_camera_set(0.0f, 0.0f, 20.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    glr_camera_add_zoom_velocity(-9.0f);

    replay_state_mut()->src_line_idx = 999;
    replay_state_mut()->speed = 1.0f;
    tutorial_state_mut()->step = 0;
    variable_panel_drag_mut()->var_idx = -1;

    ui_menu_bar_open_config(1.0f);   /* open a menu */

    editor_undo_push_snapshot();      /* perturb the undo ring */
}

static void test_snapshot_round_trip(void) {
    populate_state();

    /* Baseline observations (layout-invariant / public). */
    int    base_scene_count  = repl_user_scene_count();
    int    base_active_scene = repl_active_user_scene();
    float  base_myvar        = g_myvar_idx >= 0 ? g_predef_vars_mut[g_myvar_idx].value : 0.0f;
    int    base_doc_count    = repl_state_document_count();
    char   base_line0[256];
    snprintf(base_line0, sizeof(base_line0), "%s", editor_buffer_line(0));
    char   base_input[256];
    snprintf(base_input, sizeof(base_input), "%s", editor_input_text());
    int    base_cursor       = editor_cursor_pos();
    int    base_sel_anchor   = editor_state_selection_anchor();
    int    base_sel_end      = editor_state_selection_end_idx();
    int    base_scroll       = editor_scroll();
    int    base_wireframe    = glr_state_presentation().wireframe;
    UiPointerState base_ptr  = ui_state_pointer();
    GlrCameraState base_cam   = glr_camera();
    int    base_cam_target   = glr_camera_target_active();
    int    base_replay_line  = replay_state_view().src_line_idx;
    float  base_replay_speed = replay_state_view().speed;
    int    base_tut_step     = tutorial_state_view().step;
    int    base_drag_var     = variable_panel_drag().var_idx;

    EditorUndoRingState base_ring;
    editor_undo_ring_state_capture(&base_ring);
    int base_can_undo = editor_undo_can_undo();

    /* Capture the composite baseline. */
    GlrTourSnapshot *snap = glr_tour_snapshot_capture();
    ASSERT_TRUE("snapshot capture non-NULL", snap != NULL);
    if (!snap)
        return;

    /* Move everything off the baseline, then restore. */
    mutate_state();
    ASSERT_INT("mutation changed myvar", g_myvar_idx >= 0 ? (int)g_predef_vars_mut[g_myvar_idx].value : 99, 99);
    ASSERT_INT("restore returns 1", glr_tour_snapshot_restore(snap), 1);

    /* Owner state is back to baseline. */
    ASSERT_INT("scene count restored", repl_user_scene_count(), base_scene_count);
    ASSERT_INT("active scene restored", repl_active_user_scene(), base_active_scene);
    if (g_myvar_idx >= 0)
        ASSERT_FLOAT("myvar restored", g_predef_vars_mut[g_myvar_idx].value, base_myvar);
    ASSERT_INT("doc count restored", repl_state_document_count(), base_doc_count);
    ASSERT_STR("buffer line0 restored", editor_buffer_line(0), base_line0);
    ASSERT_STR("input restored", editor_input_text(), base_input);
    ASSERT_INT("cursor restored", editor_cursor_pos(), base_cursor);
    ASSERT_INT("sel anchor restored", editor_state_selection_anchor(), base_sel_anchor);
    ASSERT_INT("sel end restored", editor_state_selection_end_idx(), base_sel_end);
    ASSERT_INT("scroll restored", editor_scroll(), base_scroll);
    ASSERT_INT("wireframe restored", glr_state_presentation().wireframe, base_wireframe);
    ASSERT_INT("pointer x restored", ui_state_pointer().mouse_x, base_ptr.mouse_x);
    ASSERT_INT("pointer y restored", ui_state_pointer().mouse_y, base_ptr.mouse_y);
    ASSERT_FLOAT("cam rx restored", glr_camera().rx, base_cam.rx);
    ASSERT_FLOAT("cam ry restored", glr_camera().ry, base_cam.ry);
    ASSERT_FLOAT("cam dist restored", glr_camera().dist, base_cam.dist);
    ASSERT_INT("cam target-active restored", glr_camera_target_active(), base_cam_target);
    ASSERT_INT("replay line restored", replay_state_view().src_line_idx, base_replay_line);
    ASSERT_FLOAT("replay speed restored", replay_state_view().speed, base_replay_speed);
    ASSERT_INT("tutorial step restored", tutorial_state_view().step, base_tut_step);
    ASSERT_INT("drag var restored", variable_panel_drag().var_idx, base_drag_var);

    /* Undo history: logical counts + generation + can-undo, layout-invariant. */
    EditorUndoRingState after_ring;
    editor_undo_ring_state_capture(&after_ring);
    ASSERT_INT("undo count restored", after_ring.undo_count, base_ring.undo_count);
    ASSERT_INT("redo count restored", after_ring.redo_count, base_ring.redo_count);
    ASSERT_INT("undo generation restored", (int)after_ring.generation, (int)base_ring.generation);
    ASSERT_INT("can-undo restored", editor_undo_can_undo(), base_can_undo);

    /* Menu closed again (baseline had none open). */
    ASSERT_INT("menu closed after restore", ui_menu_bar_open_state_capture().menu_id, -1);

    /* Flat program is left dirty and rebuilds to a non-empty program. */
    ASSERT_INT("flat dirty after restore", repl_state_flat_program_dirty(), 1);
    repl_refresh_flat_program(editor_state_edit_line());
    ASSERT_TRUE("flat rebuilds non-empty", repl_state_flat_program_count() > 0);

    glr_tour_snapshot_destroy(snap);
}

static void test_null_restore_is_noop(void) {
    populate_state();
    int doc_before = repl_state_document_count();
    ASSERT_INT("NULL restore returns 0", glr_tour_snapshot_restore(NULL), 0);
    ASSERT_INT("NULL restore left doc untouched", repl_state_document_count(), doc_before);
    glr_tour_snapshot_destroy(NULL);   /* NULL-safe */
}

static void test_capture_does_not_flush_active_scene(void) {
    populate_state();
    /* An unsaved edit in the active scene must not be written back into its
     * slot by capture (capture records the catalog as-is). Prove capture is
     * side-effect free on the catalog by comparing the active index and count
     * across the call. */
    int active_before = repl_active_user_scene();
    int count_before  = repl_user_scene_count();
    GlrTourSnapshot *snap = glr_tour_snapshot_capture();
    ASSERT_TRUE("capture non-NULL", snap != NULL);
    ASSERT_INT("active unchanged by capture", repl_active_user_scene(), active_before);
    ASSERT_INT("count unchanged by capture", repl_user_scene_count(), count_before);
    glr_tour_snapshot_destroy(snap);
}

/* A tour captured while a RETAINED POST-TUTORIAL document is live must
 * restore both halves of that identity together: the scene-runtime origin
 * marker (which rides the ReplCheckpointState) and the tutorial runtime's
 * pending cfg baseline (which rides TutorialRuntimeState). Restoring one
 * without the other yields either a document that can no longer promote, or
 * one that promotes with no baseline left to hand the globals back. */
static void test_post_tutorial_origin_round_trip(void) {
    glr_ctrl_reset_all();
    ASSERT_TRUE("catalog has at least one tutorial", repl_tutorial_count() > 0);
    if (repl_tutorial_count() <= 0)
        return;

    tutorial_start(0);
    ASSERT_TRUE("tutorial active after start", tutorial_active());
    tutorial_stop();
    ASSERT_TRUE("tutorial inactive after stop", !tutorial_active());
    ASSERT_INT("post-tutorial origin established",
               repl_state_scenes().tutorial_origin_idx, 0);
    ASSERT_TRUE("cfg baseline pending", tutorial_state_view().baseline_valid);

    GlrTourSnapshot *snap = glr_tour_snapshot_capture();
    ASSERT_TRUE("capture non-NULL", snap != NULL);
    if (!snap) return;

    /* Mutate away — a wholesale replacement discards both halves. */
    repl_load_example(0);
    ASSERT_INT("origin cleared before restore",
               repl_state_scenes().tutorial_origin_idx, -1);
    ASSERT_TRUE("baseline flushed before restore",
                !tutorial_state_view().baseline_valid);

    ASSERT_INT("restore succeeds", glr_tour_snapshot_restore(snap), 1);
    ASSERT_INT("origin restored", repl_state_scenes().tutorial_origin_idx, 0);
    ASSERT_TRUE("baseline restored alongside the origin",
                tutorial_state_view().baseline_valid);
    ASSERT_INT("restored document is still transient", repl_active_user_scene(), -1);
    ASSERT_INT("no example active after restore",
               repl_state_scenes().active_example_idx, -1);

    /* The restored document is promotable again, which is the whole point of
     * carrying the marker through the snapshot. */
    ASSERT_TRUE("restored post-tutorial document promotes",
                repl_promote_transient_if_needed() >= 0);
    ASSERT_INT("promotion clears the restored origin",
               repl_state_scenes().tutorial_origin_idx, -1);

    glr_tour_snapshot_destroy(snap);
}

int main(void) {
    printf("--- glr_tour_snapshot tests ---\n");
    test_snapshot_round_trip();
    test_null_restore_is_noop();
    test_capture_does_not_flush_active_scene();
    test_post_tutorial_origin_round_trip();
    printf("%d / %d tests passed\n", g_harness.passed, g_harness.run);
    return g_harness.passed == g_harness.run ? 0 : 1;
}
