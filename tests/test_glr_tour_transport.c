/*
 * test_glr_tour_transport.c - controlled-tour transport state machine.
 *
 * Drives glr_pointer_script_start_tour() + glr_pointer_script_frame() and the
 * Space/arrow/speed transport handlers, observing glr_pointer_script_tour_view().
 * Covers: baseline-pending -> playing, pause freezes virtual time, speed ladder
 * + persistence, immediate Right step (paused / at Done), Space restart from
 * Done, timestamped-tour rejection, comment/blank exclusion + physical source
 * lines, filename metadata, backstep from Done / between events / in-flight,
 * 32-event seek chunking, and baseline reconstruction of a REPL commit.
 *
 * Runs under GL stubs (make test-stubs) and links CORE_TEST_OBJS.
 */
#include "app/glr_pointer_script.h"
#include "app/glr_ctrl.h"
#include "ui/app/state.h"
#include "repl/state_owners.h"
#include "gl_includes.h"           /* GLUT_KEY_LEFT / GLUT_KEY_RIGHT */

#include "support/test_harness.h"
#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond)  TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, g, e)   TEST_ASSERT_INT(&g_harness, label, g, e)
#define ASSERT_STR(label, g, e)   TEST_ASSERT_STR(&g_harness, label, g, e)
#define ASSERT_FLOAT(label, g, e) TEST_ASSERT_FLOAT_DEFAULT(&g_harness, label, g, e)

static void start_tour(const char *const *lines, int n) {
    glr_ctrl_reset_all();
    ui_state_viewport_set_size(1200, 800);
    ui_state_pointer_set(100, 100, -1);
    glr_pointer_script_start_tour("Test Tour", "test.pointer", lines, n);
}

static void frames(int n) {
    for (int i = 0; i < n; i++)
        glr_pointer_script_frame();
}

/* Advance rendered frames until the tour reaches `want` (or max exhausted).
 * Returns 1 if reached. */
static int run_until_state(GlrTourPlaybackState want, int max_frames) {
    for (int i = 0; i < max_frames; i++) {
        if (glr_pointer_script_tour_view().state == want)
            return 1;
        glr_pointer_script_frame();
    }
    return glr_pointer_script_tour_view().state == want;
}

static const char *g_many[64];
static int build_many(int n) {
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++)
        g_many[i] = "move 100 100";
    return n;
}

/* --- tests -------------------------------------------------------------- */

static void test_baseline_pending_then_playing(void) {
    const char *lines[] = { "move 100 100", "move 200 200" };
    start_tour(lines, 2);

    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("active after start", v.active, 1);
    ASSERT_INT("baseline pending", v.state, GLR_TOUR_BASELINE_PENDING);
    ASSERT_INT("total events", v.total_events, 2);
    ASSERT_FLOAT("default speed 1x", v.speed, 1.0f);
    ASSERT_STR("name in view", v.name, "Test Tour");
    ASSERT_STR("file in view", v.file, "test.pointer");

    frames(1);   /* baseline captured, enters Playing; no event fired yet */
    v = glr_pointer_script_tour_view();
    ASSERT_INT("playing after first frame", v.state, GLR_TOUR_PLAYING);
    ASSERT_INT("no event fired yet", v.completed_events, 0);
    ASSERT_INT("no in-flight event", v.current_event, -1);

    ASSERT_TRUE("reaches Done", run_until_state(GLR_TOUR_DONE, 20));
    ASSERT_INT("all events completed", glr_pointer_script_tour_view().completed_events, 2);
}

static void test_pause_freezes_virtual_time(void) {
    int n = build_many(6);
    start_tour(g_many, n);
    frames(3);   /* into Playing, a couple virtual frames advanced */

    ASSERT_INT("consume Space", glr_pointer_script_handle_tour_key(' '), 1);
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("paused", v.state, GLR_TOUR_PAUSED);
    int frozen_completed = v.completed_events;

    frames(8);   /* paused: no virtual frames advance */
    v = glr_pointer_script_tour_view();
    ASSERT_INT("still paused", v.state, GLR_TOUR_PAUSED);
    ASSERT_INT("completed frozen", v.completed_events, frozen_completed);

    ASSERT_INT("resume Space", glr_pointer_script_handle_tour_key(' '), 1);
    ASSERT_INT("playing again", glr_pointer_script_tour_view().state, GLR_TOUR_PLAYING);
    ASSERT_TRUE("resumes to Done", run_until_state(GLR_TOUR_DONE, 40));
}

static void test_speed_ladder_and_persistence(void) {
    int n = build_many(6);
    start_tour(g_many, n);
    /* Speed adjust is allowed in baseline-pending. Two '+' from 1x -> 4x. */
    glr_pointer_script_handle_tour_key('+');
    glr_pointer_script_handle_tour_key('+');
    ASSERT_FLOAT("speed 4x", glr_pointer_script_tour_view().speed, 4.0f);

    frames(1);   /* Playing */
    glr_pointer_script_handle_tour_key(' ');   /* pause */
    ASSERT_FLOAT("speed persists through pause", glr_pointer_script_tour_view().speed, 4.0f);

    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT);   /* backstep -> seek */
    run_until_state(GLR_TOUR_PAUSED, 20);
    ASSERT_FLOAT("speed persists through seek", glr_pointer_script_tour_view().speed, 4.0f);

    /* Clamp at the top of the ladder (16x). */
    for (int i = 0; i < 10; i++)
        glr_pointer_script_handle_tour_key('+');
    ASSERT_FLOAT("speed clamps at 16x", glr_pointer_script_tour_view().speed, 16.0f);
    /* And at the bottom (0.25x). */
    for (int i = 0; i < 12; i++)
        glr_pointer_script_handle_tour_key('-');
    ASSERT_FLOAT("speed clamps at 0.25x", glr_pointer_script_tour_view().speed, 0.25f);
}

static void test_speed_affects_advance_rate(void) {
    int n = build_many(50);

    start_tour(g_many, n);
    frames(1);          /* Playing at 1x */
    frames(5);          /* 5 virtual frames */
    int c1 = glr_pointer_script_tour_view().completed_events;

    start_tour(g_many, n);
    glr_pointer_script_handle_tour_key('+');
    glr_pointer_script_handle_tour_key('+');   /* 4x */
    frames(1);          /* Playing at 4x */
    frames(5);          /* ~20 virtual frames */
    int c4 = glr_pointer_script_tour_view().completed_events;

    ASSERT_TRUE("4x advances more than 1x", c4 > c1);
    ASSERT_TRUE("4x advances several-fold", c4 >= c1 * 2);
    ASSERT_TRUE("neither reached the end", c4 < n);
}

static void test_right_step_from_paused(void) {
    int n = build_many(5);
    start_tour(g_many, n);
    frames(1);   /* Playing, no event fired */
    glr_pointer_script_handle_tour_key(' ');   /* Paused, current == -1 */

    ASSERT_INT("consume Right", glr_pointer_script_handle_tour_special(GLUT_KEY_RIGHT), 1);
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("stepped one event", v.completed_events, 1);
    ASSERT_INT("still paused", v.state, GLR_TOUR_PAUSED);

    glr_pointer_script_handle_tour_special(GLUT_KEY_RIGHT);
    ASSERT_INT("stepped second event", glr_pointer_script_tour_view().completed_events, 2);
}

static void test_right_at_done_is_noop(void) {
    const char *lines[] = { "move 100 100", "move 200 200" };
    start_tour(lines, 2);
    run_until_state(GLR_TOUR_DONE, 20);

    ASSERT_INT("Right consumed at Done", glr_pointer_script_handle_tour_special(GLUT_KEY_RIGHT), 1);
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("stays Done", v.state, GLR_TOUR_DONE);
    ASSERT_INT("completed unchanged", v.completed_events, 2);
}

static void test_space_restart_from_done(void) {
    int n = build_many(3);
    start_tour(g_many, n);
    run_until_state(GLR_TOUR_DONE, 20);

    glr_pointer_script_handle_tour_key(' ');   /* restart */
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("restart to Playing", v.state, GLR_TOUR_PLAYING);
    ASSERT_INT("restart resets completed", v.completed_events, 0);
    ASSERT_INT("restart resets in-flight", v.current_event, -1);
    ASSERT_TRUE("restart replays to Done", run_until_state(GLR_TOUR_DONE, 20));
}

static void test_timestamped_tour_rejected(void) {
    glr_ctrl_reset_all();
    const char *lines[] = { "0.5 move 100 100" };
    int r = glr_pointer_script_start_tour("T", "f", lines, 1);
    ASSERT_INT("timestamped tour rejected", r, 0);
    ASSERT_INT("no tour active", glr_pointer_script_tour_view().active, 0);
}

static void test_comments_blanks_and_source_line(void) {
    const char *lines[] = {
        "# leading comment",   /* line 1 */
        "",                    /* line 2 */
        "move 100 100",        /* line 3 -> event 0 */
        "# mid comment",       /* line 4 */
        "move 200 200",        /* line 5 -> event 1 */
    };
    start_tour(lines, 5);
    ASSERT_INT("comments/blanks excluded", glr_pointer_script_tour_view().total_events, 2);

    frames(2);   /* Playing, event 0 in flight */
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("event 0 in flight", v.current_event, 0);
    ASSERT_INT("event 0 source line", v.source_line, 3);

    frames(1);   /* event 0 completes, event 1 fires */
    v = glr_pointer_script_tour_view();
    ASSERT_INT("event 1 in flight", v.current_event, 1);
    ASSERT_INT("event 1 source line", v.source_line, 5);
}

static void test_backstep_from_done(void) {
    int n = build_many(3);
    start_tour(g_many, n);
    run_until_state(GLR_TOUR_DONE, 20);

    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT);   /* target = total-1 = 2 */
    ASSERT_TRUE("seek settles to Paused", run_until_state(GLR_TOUR_PAUSED, 20));
    ASSERT_INT("landed on total-1", glr_pointer_script_tour_view().completed_events, 2);
}

static void test_backstep_between_events(void) {
    int n = build_many(5);
    start_tour(g_many, n);
    frames(1);
    glr_pointer_script_handle_tour_special(GLUT_KEY_RIGHT);   /* completed 1, paused */
    glr_pointer_script_handle_tour_special(GLUT_KEY_RIGHT);   /* completed 2, paused, current -1 */
    ASSERT_INT("paused between events", glr_pointer_script_tour_view().current_event, -1);

    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT);    /* target = 2 - 1 = 1 */
    ASSERT_TRUE("seek settles", run_until_state(GLR_TOUR_PAUSED, 20));
    ASSERT_INT("returned one completed event", glr_pointer_script_tour_view().completed_events, 1);
}

static void test_backstep_in_flight_returns_to_start_boundary(void) {
    int n = build_many(5);
    start_tour(g_many, n);
    frames(2);   /* Playing, event 0 in flight, completed 0 */
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("event 0 in flight", v.current_event, 0);
    ASSERT_INT("none completed yet", v.completed_events, 0);

    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT);    /* in flight -> target = completed = 0 */
    ASSERT_TRUE("seek settles", run_until_state(GLR_TOUR_PAUSED, 20));
    v = glr_pointer_script_tour_view();
    ASSERT_INT("returned to boundary before in-flight event", v.completed_events, 0);
    ASSERT_INT("no in-flight event after seek", v.current_event, -1);
}

static void test_seek_processes_at_most_32_per_frame(void) {
    int n = build_many(40);
    start_tour(g_many, n);
    run_until_state(GLR_TOUR_DONE, 120);

    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT);   /* target = 39 */
    ASSERT_INT("seeking after backstep", glr_pointer_script_tour_view().state, GLR_TOUR_SEEKING);

    glr_pointer_script_frame();   /* first chunk: 32 events */
    ASSERT_INT("still seeking after 32", glr_pointer_script_tour_view().state, GLR_TOUR_SEEKING);
    ASSERT_INT("32 events processed in first chunk",
               glr_pointer_script_tour_view().completed_events, 32);

    glr_pointer_script_frame();   /* second chunk: remaining 7 */
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("paused after second chunk", v.state, GLR_TOUR_PAUSED);
    ASSERT_INT("reached target 39", v.completed_events, 39);
}

static void test_backstep_reconstructs_repl_commit(void) {
    const char *lines[] = { "key glVertex3f(1,2,3);" };
    glr_ctrl_reset_all();
    ui_state_viewport_set_size(1200, 800);
    ui_state_pointer_set(100, 100, -1);

    int base_doc = repl_state_document_count();
    glr_pointer_script_start_tour("Commit", "commit.pointer", lines, 1);
    run_until_state(GLR_TOUR_DONE, 40);
    ASSERT_TRUE("commit added a command", repl_state_document_count() > base_doc);

    /* Backstep from Done (target = total-1 = 0): begin_seek restores the
     * baseline synchronously, so the committed command is already gone. */
    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT);
    ASSERT_INT("baseline reconstructs pre-commit document",
               repl_state_document_count(), base_doc);
    ASSERT_TRUE("seek settles to Paused", run_until_state(GLR_TOUR_PAUSED, 20));
    ASSERT_INT("landed at boundary 0", glr_pointer_script_tour_view().completed_events, 0);
}

static void test_view_inactive_after_stop(void) {
    int n = build_many(3);
    start_tour(g_many, n);
    frames(2);
    glr_pointer_script_stop();
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("view inactive after stop", v.active, 0);
    ASSERT_INT("state OFF after stop", v.state, GLR_TOUR_OFF);
    ASSERT_INT("source line -1 with no tour", v.source_line, -1);
}

int main(void) {
    printf("--- glr_tour_transport tests ---\n");
    test_baseline_pending_then_playing();
    test_pause_freezes_virtual_time();
    test_speed_ladder_and_persistence();
    test_speed_affects_advance_rate();
    test_right_step_from_paused();
    test_right_at_done_is_noop();
    test_space_restart_from_done();
    test_timestamped_tour_rejected();
    test_comments_blanks_and_source_line();
    test_backstep_from_done();
    test_backstep_between_events();
    test_backstep_in_flight_returns_to_start_boundary();
    test_seek_processes_at_most_32_per_frame();
    test_backstep_reconstructs_repl_commit();
    test_view_inactive_after_stop();
    printf("%d / %d tests passed\n", g_harness.passed, g_harness.run);
    return g_harness.passed == g_harness.run ? 0 : 1;
}
