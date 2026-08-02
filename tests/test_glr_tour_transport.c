/*
 * test_glr_tour_transport.c - controlled-tour transport state machine.
 *
 * Drives glr_pointer_script_start_tour() + glr_pointer_script_frame() and the
 * Space/arrow/speed transport handlers, observing glr_pointer_script_tour_view().
 * Covers: baseline-pending -> playing, pause freezes virtual time, speed ladder
 * + persistence, immediate Right step (paused / at Done), Space restart from
 * Done, timestamped-tour rejection, comment/blank exclusion + physical source
 * lines, filename metadata, backstep from Done / between events / in-flight,
 * 32-event seek chunking, baseline reconstruction of a REPL commit, immediate
 * reconstructed camera easing, and application/camera tick suppression while
 * a multi-frame seek is active.
 *
 * Runs under GL stubs (make test-stubs) and links CORE_TEST_OBJS.
 */
#include "app/glr_pointer_script.h"
#include "app/glr_ctrl.h"
#include "app/glr_camera.h"        /* glr_camera_mut (drag reconstruction) */
#include "app/glr_config.h"        /* GLR_CONFIG_ORTHO_MODE */
#include "app/glr_tour_presence.h" /* ambient presence phase machine */
#include "render3d/view_mode.h"
#include "ui/app/state.h"
#include "ui/subsystems/tour_hud.h" /* tour_hud_panel_width (clamp guard) */
#include "repl/state_owners.h"
#include "gl_includes.h"           /* GLUT_KEY_LEFT / GLUT_KEY_RIGHT / buttons */

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

static void settle_view_transition(void) {
    for (int i = 0; i < 500 && glr_ctrl_view_transition_active(); i++)
        glr_ctrl_tick();
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

static void test_hud_defaults_compact_and_survives_backstep(void) {
    const char *lines[] = {
        "move 100 100", "move 200 200", "move 300 300"
    };
    GlrTourPlaybackView v;

    start_tour(lines, 3);
    v = glr_pointer_script_tour_view();
    ASSERT_INT("new tour HUD defaults compact", v.hud_expanded, 0);
    ASSERT_INT("HUD toggle consumed during tour",
               glr_pointer_script_toggle_tour_hud(), 1);
    ASSERT_INT("HUD expands",
               glr_pointer_script_tour_view().hud_expanded, 1);

    frames(1);  /* capture baseline */
    ASSERT_TRUE("tour reaches Done", run_until_state(GLR_TOUR_DONE, 30));
    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT);
    ASSERT_INT("HUD stays expanded when Back restores baseline",
               glr_pointer_script_tour_view().hud_expanded, 1);
    ASSERT_TRUE("backstep settles", run_until_state(GLR_TOUR_PAUSED, 20));
    ASSERT_INT("HUD stays expanded after prefix replay",
               glr_pointer_script_tour_view().hud_expanded, 1);

    ASSERT_INT("collapse toggle consumed",
               glr_pointer_script_toggle_tour_hud(), 1);
    ASSERT_INT("HUD collapses",
               glr_pointer_script_tour_view().hud_expanded, 0);
    glr_pointer_script_stop();
    ASSERT_INT("HUD toggle ignored without tour",
               glr_pointer_script_toggle_tour_hud(), 0);

    start_tour(lines, 3);
    ASSERT_INT("next tour starts compact",
               glr_pointer_script_tour_view().hud_expanded, 0);
    glr_pointer_script_stop();
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
    glr_camera_set(12.0f, 24.0f, 8.0f, 1.0f, 2.0f, 3.0f, 0.0f);
    glr_camera_ease_to(30.0f, 60.0f, 5.0f, 0.0f, 0.0f, 0.0f);
    ASSERT_INT("restart closes camera reconstruction scope",
               glr_camera_target_active(), 1);
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

static void test_seek_scene_camera_ease_is_immediate(void) {
    const char *lines[] = {
        "chord ctrl+shift c",  /* Reset camera: normally eased. */
        "move 200 200",
    };

    start_tour(lines, 2);
    glr_camera_set(55.0f, 120.0f, 11.0f, 3.0f, 4.0f, 5.0f, 0.0f);
    frames(1);   /* capture the custom camera in the baseline */
    ASSERT_TRUE("camera-reset tour reaches Done",
                run_until_state(GLR_TOUR_DONE, 20));
    ASSERT_INT("normal playback leaves reset-camera ease active",
               glr_camera_target_active(), 1);
    ASSERT_FLOAT("normal playback still shows pre-ease camera",
                 glr_camera().rx, 55.0f);

    /* Done -> Back targets one completed event, so prefix reconstruction
     * replays Reset camera. Its ease must resolve before this frame renders. */
    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT);
    ASSERT_INT("camera backstep enters Seeking",
               glr_pointer_script_tour_view().state, GLR_TOUR_SEEKING);
    glr_pointer_script_frame();

    ASSERT_INT("camera backstep lands Paused",
               glr_pointer_script_tour_view().state, GLR_TOUR_PAUSED);
    ASSERT_INT("reconstructed reset-camera ease is complete",
               glr_camera_target_active(), 0);
    ASSERT_FLOAT("reconstructed camera snaps to default rx",
                 glr_camera().rx, 20.0f);
    ASSERT_FLOAT("reconstructed camera snaps to default ry",
                 glr_camera().ry, 30.0f);
    ASSERT_FLOAT("reconstructed camera snaps to default distance",
                 glr_camera().dist, 5.0f);
}

static void test_seek_suppresses_application_and_camera_ticks(void) {
    int n = build_many(40);
    start_tour(g_many, n);
    repl_state_variables_mut()->anim_time = 3.0f;
    repl_state_variables_mut()->time_playing = 1;
    glr_camera_mut()->auto_rotate = 1;
    frames(1);   /* capture the time/camera baseline */
    ASSERT_TRUE("long tour reaches Done",
                run_until_state(GLR_TOUR_DONE, 120));

    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT); /* target 39 */
    ASSERT_INT("seek suppresses tick immediately",
               glr_pointer_script_tour_suppresses_app_tick(), 1);

    float frozen_time = repl_state_variables().anim_time;
    float frozen_ry = glr_camera().ry;
    glr_ctrl_tick();
    ASSERT_FLOAT("time frozen before first seek chunk",
                 repl_state_variables().anim_time, frozen_time);
    ASSERT_FLOAT("camera frozen before first seek chunk",
                 glr_camera().ry, frozen_ry);

    glr_pointer_script_frame();   /* first 32 events */
    ASSERT_INT("tick remains suppressed between seek chunks",
               glr_pointer_script_tour_suppresses_app_tick(), 1);
    glr_ctrl_tick();
    ASSERT_FLOAT("time frozen between seek chunks",
                 repl_state_variables().anim_time, frozen_time);
    ASSERT_FLOAT("camera frozen between seek chunks",
                 glr_camera().ry, frozen_ry);

    glr_pointer_script_frame();   /* final 7 events -> Paused */
    ASSERT_INT("seek completion frame still suppresses tick",
               glr_pointer_script_tour_suppresses_app_tick(), 1);
    glr_ctrl_tick();
    ASSERT_FLOAT("time frozen on seek completion frame",
                 repl_state_variables().anim_time, frozen_time);
    ASSERT_FLOAT("camera frozen on seek completion frame",
                 glr_camera().ry, frozen_ry);

    glr_pointer_script_frame();   /* present completed seek; Paused */
    ASSERT_INT("following paused frame releases tick suppression",
               glr_pointer_script_tour_suppresses_app_tick(), 0);
    glr_ctrl_tick();
    ASSERT_TRUE("application time resumes after seek",
                repl_state_variables().anim_time > frozen_time);
    ASSERT_TRUE("camera auto-rotation resumes after seek",
                glr_camera().ry != frozen_ry);
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

static void test_right_while_playing_forces_complete_and_pauses(void) {
    int n = build_many(5);
    start_tour(g_many, n);
    frames(2);   /* Playing, event 0 in flight */
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("event 0 in flight", v.current_event, 0);
    ASSERT_INT("none completed yet", v.completed_events, 0);

    ASSERT_INT("Right consumed while Playing",
               glr_pointer_script_handle_tour_special(GLUT_KEY_RIGHT), 1);
    v = glr_pointer_script_tour_view();
    ASSERT_INT("in-flight event forced complete", v.completed_events, 1);
    ASSERT_INT("no in-flight event after step", v.current_event, -1);
    ASSERT_INT("paused after step", v.state, GLR_TOUR_PAUSED);
}

/* A normal (non-stepped) ring is an authored beat: the tour stays Playing for
 * the whole PS_WAIT_RING duration, then enters Done. */
static void test_normal_ring_delays_done(void) {
    const char *lines[] = { "ring 400 400 0.5" };   /* 30 virtual frames */
    start_tour(lines, 1);
    frames(2);   /* baseline, then the ring fires */
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("ring in flight, still Playing", v.state, GLR_TOUR_PLAYING);
    ASSERT_INT("ring not yet counted complete", v.completed_events, 0);

    frames(12);  /* well within the 30-frame ring duration */
    ASSERT_INT("still Playing mid-ring",
               glr_pointer_script_tour_view().state, GLR_TOUR_PLAYING);

    ASSERT_TRUE("ring expiry enters Done", run_until_state(GLR_TOUR_DONE, 60));
    ASSERT_INT("ring counted once at Done",
               glr_pointer_script_tour_view().completed_events, 1);
}

/* A ring forced by Right (or fast seek) counts complete immediately, so a
 * trailing ring enters Done at once instead of waiting its duration; a backstep
 * then lands on total-1 with the ring un-counted. */
static void test_stepped_ring_immediate_done_and_backstep(void) {
    const char *lines[] = { "move 100 100", "ring 400 400 5" }; /* 300-frame ring */
    start_tour(lines, 2);
    frames(2);   /* baseline; event 0 (move) fires */
    /* Step to the ring and force it: two Rights (move, then ring). */
    glr_pointer_script_handle_tour_special(GLUT_KEY_RIGHT);   /* completes move */
    glr_pointer_script_handle_tour_special(GLUT_KEY_RIGHT);   /* next: ring, forced */
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("stepped ring enters Done immediately", v.state, GLR_TOUR_DONE);
    ASSERT_INT("both events counted", v.completed_events, 2);

    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT);    /* target = total-1 = 1 */
    ASSERT_TRUE("seek settles", run_until_state(GLR_TOUR_PAUSED, 20));
    ASSERT_INT("backstep counting agrees (ring un-counted)",
               glr_pointer_script_tour_view().completed_events, 1);
}

/* A normal click blocks until its synthesized release frame; it is not counted
 * complete on the fire frame. */
static void test_normal_click_waits_release(void) {
    const char *lines[] = { "click scene:0.5,0.5" };
    start_tour(lines, 1);
    frames(2);   /* baseline; the click fires (press, release scheduled) */
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("click in flight, still Playing", v.state, GLR_TOUR_PLAYING);
    ASSERT_INT("click not counted on the fire frame", v.completed_events, 0);
    ASSERT_INT("click owns pointer motion until release",
               glr_pointer_script_owns_pointer_motion(), 1);

    frames(3);   /* still before the ~6-frame release */
    ASSERT_INT("still waiting for release",
               glr_pointer_script_tour_view().completed_events, 0);

    ASSERT_TRUE("click completes and enters Done",
                run_until_state(GLR_TOUR_DONE, 20));
    ASSERT_INT("click counted once", glr_pointer_script_tour_view().completed_events, 1);
    ASSERT_INT("click releases pointer-motion ownership",
               glr_pointer_script_owns_pointer_motion(), 0);
}

/* Modified clicks accept the chord modifier grammar ahead of an optional
 * point and retain the normal synthesized-release timing. */
static void test_modified_clicks(void) {
    const char *lines[] = {
        "click shift scene:0.5,0.5",
        "rightclick ctrl+shift scene:0.6,0.5"
    };
    start_tour(lines, 2);
    ASSERT_INT("modified clicks parse", glr_pointer_script_tour_view().active, 1);
    ASSERT_INT("modified click event count",
               glr_pointer_script_tour_view().total_events, 2);
    ASSERT_TRUE("modified clicks reach Done",
                run_until_state(GLR_TOUR_DONE, 30));
}

/* Space / Left / Right are consumed but no-op while the baseline is still
 * pending, and cannot corrupt the subsequent Playing run. */
static void test_controls_during_baseline_pending_noop(void) {
    int n = build_many(3);
    start_tour(g_many, n);   /* BASELINE_PENDING */
    ASSERT_INT("Space consumed in baseline-pending",
               glr_pointer_script_handle_tour_key(' '), 1);
    ASSERT_INT("Left consumed in baseline-pending",
               glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT), 1);
    ASSERT_INT("Right consumed in baseline-pending",
               glr_pointer_script_handle_tour_special(GLUT_KEY_RIGHT), 1);
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("still baseline-pending", v.state, GLR_TOUR_BASELINE_PENDING);
    ASSERT_INT("nothing completed", v.completed_events, 0);

    frames(1);
    ASSERT_INT("enters Playing cleanly", glr_pointer_script_tour_view().state,
               GLR_TOUR_PLAYING);
    ASSERT_TRUE("plays through to Done", run_until_state(GLR_TOUR_DONE, 20));
}

/* Space / Left / Right are consumed but no-op mid-seek, and the seek still
 * settles to its target boundary. */
static void test_controls_during_seeking_noop(void) {
    int n = build_many(40);
    start_tour(g_many, n);
    run_until_state(GLR_TOUR_DONE, 120);
    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT);   /* target 39 -> SEEKING */
    glr_pointer_script_frame();   /* first 32-event chunk */
    ASSERT_INT("seeking mid-chunk", glr_pointer_script_tour_view().state,
               GLR_TOUR_SEEKING);

    ASSERT_INT("Space consumed while seeking",
               glr_pointer_script_handle_tour_key(' '), 1);
    ASSERT_INT("Left consumed while seeking",
               glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT), 1);
    ASSERT_INT("Right consumed while seeking",
               glr_pointer_script_handle_tour_special(GLUT_KEY_RIGHT), 1);
    ASSERT_INT("still seeking after inputs",
               glr_pointer_script_tour_view().state, GLR_TOUR_SEEKING);

    glr_pointer_script_frame();   /* second chunk settles */
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("seek settled to Paused", v.state, GLR_TOUR_PAUSED);
    ASSERT_INT("reached target 39", v.completed_events, 39);
}

static void test_speed_persists_through_done_restart(void) {
    int n = build_many(3);
    start_tour(g_many, n);
    glr_pointer_script_handle_tour_key('+');
    glr_pointer_script_handle_tour_key('+');   /* 4x */
    run_until_state(GLR_TOUR_DONE, 20);
    ASSERT_FLOAT("speed 4x at Done", glr_pointer_script_tour_view().speed, 4.0f);

    glr_pointer_script_handle_tour_key(' ');   /* restart */
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("restarted to Playing", v.state, GLR_TOUR_PLAYING);
    ASSERT_FLOAT("speed persists through Done restart", v.speed, 4.0f);
}

/* A final `down` reaches Done with no held scripted button remaining. */
static void test_final_down_releases_button_at_done(void) {
    const char *lines[] = { "down scene:0.5,0.5" };
    start_tour(lines, 1);
    frames(2);   /* baseline; the down presses and holds */
    ASSERT_INT("scripted button held after down",
               ui_state_pointer().mouse_button, GLUT_LEFT_BUTTON);

    ASSERT_TRUE("reaches Done", run_until_state(GLR_TOUR_DONE, 20));
    ASSERT_INT("Done released the held button",
               ui_state_pointer().mouse_button, -1);
}

/* A paused boundary after `down` intentionally retains the held button (so a
 * following stepped glide/up reproduces a drag); Escape must release it. */
static void test_escape_from_paused_post_down_releases(void) {
    const char *lines[] = { "down scene:0.5,0.5", "move 100 100" };
    start_tour(lines, 2);
    frames(2);   /* baseline; down in flight */
    glr_pointer_script_handle_tour_special(GLUT_KEY_RIGHT);   /* complete down, pause */
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("paused after the down", v.state, GLR_TOUR_PAUSED);
    ASSERT_INT("held button retained across the paused boundary",
               ui_state_pointer().mouse_button, GLUT_LEFT_BUTTON);

    ASSERT_INT("Esc consumed", glr_pointer_script_handle_tour_key(27), 1);
    ASSERT_INT("tour stopped", glr_pointer_script_tour_view().active, 0);
    ASSERT_INT("Esc released the held button",
               ui_state_pointer().mouse_button, -1);
}

/* Backstep from a paused post-`down` boundary releases the held button before
 * restoring the baseline. */
static void test_backstep_from_paused_post_down_releases(void) {
    const char *lines[] = { "down scene:0.5,0.5", "move 100 100", "move 200 200" };
    start_tour(lines, 3);
    frames(2);   /* baseline; down in flight */
    glr_pointer_script_handle_tour_special(GLUT_KEY_RIGHT);   /* complete down, pause */
    ASSERT_INT("held button retained before backstep",
               ui_state_pointer().mouse_button, GLUT_LEFT_BUTTON);

    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT);    /* target = 1-1 = 0 */
    ASSERT_INT("backstep released the held button before restore",
               ui_state_pointer().mouse_button, -1);
    ASSERT_TRUE("seek settles", run_until_state(GLR_TOUR_PAUSED, 20));
    ASSERT_INT("landed at boundary 0",
               glr_pointer_script_tour_view().completed_events, 0);
}

static void test_escape_stops_tour(void) {
    int n = build_many(3);
    start_tour(g_many, n);
    frames(2);
    ASSERT_INT("Esc consumed", glr_pointer_script_handle_tour_key(27), 1);
    GlrTourPlaybackView v = glr_pointer_script_tour_view();
    ASSERT_INT("tour inactive after Esc", v.active, 0);
    ASSERT_INT("state OFF after Esc", v.state, GLR_TOUR_OFF);
}

/* Backstep restores the camera pose a scripted drag changed. begin_seek
 * restores the baseline synchronously (before the prefix replays), so the pose
 * is back at its captured value immediately after Left. */
static void test_backstep_reconstructs_camera_drag(void) {
    const char *lines[] = {
        "down scene:0.5,0.5", "move 900 400", "up 900 400"
    };
    start_tour(lines, 3);
    frames(1);   /* baseline captured */
    float ry0 = glr_camera_mut()->ry;

    run_until_state(GLR_TOUR_DONE, 40);
    ASSERT_TRUE("drag changed the camera yaw",
                glr_camera_mut()->ry != ry0);

    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT);   /* from Done: restore baseline */
    ASSERT_FLOAT("backstep restored the baseline camera yaw",
                 glr_camera_mut()->ry, ry0);
}

/* Held right-button events use the same motion path as a real camera pan, and
 * the tour baseline still restores the translated orbit target on backstep. */
static void test_right_drag_pans_camera_and_rewinds(void) {
    const char *lines[] = {
        "rightdown scene:0.5,0.5",
        "glide scene:0.75,0.65 0.2",
        "rightup",
        "rightdown shift scene:0.75,0.65",
        "glide scene:0.75,0.35 0.2",
        "rightup"
    };
    start_tour(lines, 6);
    frames(1);   /* baseline captured */
    float tx0 = glr_camera_mut()->tx;
    float ty0 = glr_camera_mut()->ty;
    float tz0 = glr_camera_mut()->tz;

    ASSERT_TRUE("right-drag tour reaches Done",
                run_until_state(GLR_TOUR_DONE, 40));
    ASSERT_TRUE("right drag changed the camera pan target",
                glr_camera_mut()->tx != tx0 || glr_camera_mut()->tz != tz0);
    ASSERT_TRUE("Shift+right drag changed camera pan Y",
                glr_camera_mut()->ty != ty0);

    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT);
    ASSERT_FLOAT("backstep restored camera pan X", glr_camera_mut()->tx, tx0);
    ASSERT_FLOAT("backstep restored camera pan Y", glr_camera_mut()->ty, ty0);
    ASSERT_FLOAT("backstep restored camera pan Z", glr_camera_mut()->tz, tz0);
}

static void test_drag_pointer_motion_ownership(void) {
    const char *lines[] = {
        "rightdown scene:0.5,0.5",
        "rightup"
    };
    start_tour(lines, 2);
    frames(2);   /* baseline; rightdown fires */
    ASSERT_INT("scripted drag owns physical pointer motion",
               glr_pointer_script_owns_pointer_motion(), 1);

    frames(1);   /* rightup fires */
    ASSERT_INT("rightup releases physical pointer motion",
               glr_pointer_script_owns_pointer_motion(), 0);

    start_tour(lines, 2);
    frames(2);
    ASSERT_INT("second drag owns pointer motion",
               glr_pointer_script_owns_pointer_motion(), 1);
    glr_pointer_script_stop();
    ASSERT_INT("stopping a drag releases pointer motion",
               glr_pointer_script_owns_pointer_motion(), 0);
}

static void test_tour_blocks_physical_motion_throughout(void) {
    const char *lines[] = {
        "move scene:0.4,0.4",
        "move scene:0.6,0.6"
    };
    start_tour(lines, 2);
    ASSERT_INT("baseline-pending tour blocks physical motion",
               glr_pointer_script_blocks_physical_motion(), 1);

    frames(1);
    ASSERT_INT("playing tour blocks physical motion",
               glr_pointer_script_blocks_physical_motion(), 1);
    glr_pointer_script_handle_tour_key(' ');
    ASSERT_INT("paused tour blocks physical motion",
               glr_pointer_script_blocks_physical_motion(), 1);
    glr_pointer_script_handle_tour_key(' ');

    ASSERT_TRUE("motion-blocking tour reaches Done",
                run_until_state(GLR_TOUR_DONE, 20));
    ASSERT_INT("Done linger blocks physical motion",
               glr_pointer_script_blocks_physical_motion(), 1);
    frames(1);   /* no final caption: Done auto-closes on the next frame */
    ASSERT_INT("tour close restores physical motion",
               glr_pointer_script_blocks_physical_motion(), 0);
}

static void test_controller_arbitrates_physical_and_scripted_input(void) {
    const char *lines[] = {
        "move scene:0.4,0.4",
        "move scene:0.6,0.6"
    };
    start_tour(lines, 2);

    glr_ctrl_passive_motion(900, 700);
    ASSERT_INT("physical motion is inert during tour (x)",
               ui_state_pointer().mouse_x, 100);
    ASSERT_INT("physical motion is inert during tour (y)",
               ui_state_pointer().mouse_y, 100);

    glr_ctrl_scripted_passive_motion(220, 230);
    ASSERT_INT("scripted motion bypasses tour arbitration (x)",
               ui_state_pointer().mouse_x, 220);
    ASSERT_INT("scripted motion bypasses tour arbitration (y)",
               ui_state_pointer().mouse_y, 230);
    ASSERT_INT("scripted motion keeps tour active",
               glr_pointer_script_tour_active(), 1);

    frames(1);
    glr_ctrl_keyboard(' ', 0, 0);
    ASSERT_INT("physical Space pauses through controller",
               glr_pointer_script_tour_view().state, GLR_TOUR_PAUSED);
    glr_ctrl_keyboard(' ', 0, 0);
    ASSERT_INT("physical Space resumes through controller",
               glr_pointer_script_tour_view().state, GLR_TOUR_PLAYING);

    glr_ctrl_scripted_keyboard(' ', 0, 0);
    ASSERT_INT("scripted key bypasses physical cancellation",
               glr_pointer_script_tour_active(), 1);

    glr_ctrl_keyboard('x', 0, 0);
    ASSERT_INT("ordinary physical key cancels tour in controller",
               glr_pointer_script_tour_active(), 0);
    ASSERT_INT("physical motion routes again after cancellation",
               glr_pointer_script_blocks_physical_motion(), 0);
}

static void test_view_event_ensures_3d_and_reconstructs(void) {
    const char *lines[] = {
        "view 3d # idempotent tour precondition",
        "move scene:0.5,0.5"
    };

    /* Already-3D scenes proceed without starting a transition. */
    start_tour(lines, 2);
    frames(2);   /* baseline; view event fires */
    ASSERT_INT("view 3d keeps a 3D scene in 3D",
               glr_config_get(GLR_CONFIG_ORTHO_MODE), RENDER3D_VIEW_3D);
    ASSERT_INT("view 3d is a no-op when already settled",
               glr_ctrl_view_transition_active(), 0);
    ASSERT_TRUE("already-3D view event reaches Done",
                run_until_state(GLR_TOUR_DONE, 20));

    /* Start from a fully settled 2D scene, matching a tour launched after the
     * user has been working in 2D rather than a raw config-only test state. */
    glr_ctrl_reset_all();
    ui_state_viewport_set_size(1200, 800);
    ui_state_pointer_set(100, 100, -1);
    glr_config_set(GLR_CONFIG_ORTHO_MODE, RENDER3D_VIEW_2D);
    settle_view_transition();
    ASSERT_INT("2D fixture settled",
               glr_ctrl_view_transition_active(), 0);
    glr_pointer_script_start_tour("Test Tour", "test.pointer", lines, 2);
    frames(2);   /* baseline; view event requests 3D */
    ASSERT_INT("view event selects 3D from 2D",
               glr_config_get(GLR_CONFIG_ORTHO_MODE), RENDER3D_VIEW_3D);
    ASSERT_INT("2D to 3D transition is active",
               glr_ctrl_view_transition_active(), 1);

    for (int i = 0; i < 500 &&
         glr_pointer_script_tour_view().state != GLR_TOUR_DONE; i++) {
        frames(1);
        glr_ctrl_tick();
    }
    ASSERT_INT("view event waits for transition before Done",
               glr_pointer_script_tour_view().state, GLR_TOUR_DONE);
    ASSERT_INT("3D transition settled before later events completed",
               glr_ctrl_view_transition_active(), 0);

    /* Backstep to the prefix containing only `view 3d`. Prefix replay cannot
     * tick application time, so it resolves the transition synchronously. */
    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT);
    ASSERT_TRUE("view-event backstep settles",
                run_until_state(GLR_TOUR_PAUSED, 20));
    ASSERT_INT("reconstructed prefix remains 3D",
               glr_config_get(GLR_CONFIG_ORTHO_MODE), RENDER3D_VIEW_3D);
    ASSERT_INT("reconstructed view transition is settled",
               glr_ctrl_view_transition_active(), 0);
}

/* The HUD panel width is the scene width minus margins, NEVER forced to a
 * minimum that would push it past a narrow scene (the overflow bug). This is
 * the render path's actual width function; feedback can't see off-window
 * overflow, so the clamp is guarded here arithmetically. */
static void test_hud_panel_width_clamp(void) {
    const int margins = 2 * TOUR_HUD_MARGIN_X;

    ASSERT_INT("wide scene: full width minus margins",
               tour_hud_panel_width(1200), 1200 - margins);
    /* Regression: a 240px scene must yield 204, NOT a forced ~260 that would
     * overflow the scene. */
    ASSERT_INT("narrow scene: still scene minus margins (no forced minimum)",
               tour_hud_panel_width(240), 240 - margins);
    /* At the visibility floor it renders; one below, it is skipped. */
    ASSERT_INT("at the visibility floor: renders",
               tour_hud_panel_width(margins + TOUR_HUD_MIN_VISIBLE),
               TOUR_HUD_MIN_VISIBLE);
    ASSERT_INT("below the visibility floor: skipped",
               tour_hud_panel_width(margins + TOUR_HUD_MIN_VISIBLE - 1), 0);

    /* Invariant: whenever it renders, the panel never exceeds the scene. */
    for (int sw = 100; sw <= 2000; sw += 7) {
        int w = tour_hud_panel_width(sw);
        if (w > 0)
            ASSERT_TRUE("panel width never exceeds the scene", w <= sw);
    }
}

/* Caption alpha: eases in/out during playback, but a frozen (paused/stepped/
 * backstep-settled) clock renders full opacity so a caption caught mid-fade -
 * or reconstructed at a tiny age on backstep - stays readable instead of hung
 * half-transparent. */
static void test_echo_alpha_frozen_is_full(void) {
    /* Playing: ease-in ramps from 0 at age 0 to full by age 9. */
    ASSERT_FLOAT("age 0 playing eases in from zero",
                 glr_pointer_script_echo_alpha(0, 300, 0), 0.0f);
    ASSERT_FLOAT("age 9 playing fully in",
                 glr_pointer_script_echo_alpha(9, 300, 0), 1.0f);
    ASSERT_FLOAT("mid-life playing full",
                 glr_pointer_script_echo_alpha(100, 300, 0), 1.0f);

    /* Frozen: full opacity regardless of where in the fade it was caught. */
    ASSERT_FLOAT("age 0 frozen is full",
                 glr_pointer_script_echo_alpha(0, 300, 1), 1.0f);
    ASSERT_FLOAT("age 3 frozen is full (backstep landing)",
                 glr_pointer_script_echo_alpha(3, 300, 1), 1.0f);
    ASSERT_FLOAT("near-end frozen is full",
                 glr_pointer_script_echo_alpha(295, 300, 1), 1.0f);
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

/* --- ambient presence layer --------------------------------------------
 *
 * The phase machine in glr_tour_presence.c is what makes a running tour
 * visible in peripheral vision, and every property worth pinning is arithmetic
 * over a frame counter - no drawing required. What it must get right: the
 * intro announces before anything moves, the border keeps breathing while the
 * tour is paused (a paused tour is still a tour you are inside), and the exit
 * collapse OUTLIVES the tour, which is the whole reason this state does not
 * live in the UI band.
 */
static GlrTourPresenceView presence_frames(int tour_active,
                                           const char *name, int n) {
    GlrTourPresenceView v = glr_tour_presence_tick(tour_active, name);
    for (int i = 1; i < n; i++)
        v = glr_tour_presence_tick(tour_active, name);
    return v;
}

static void test_presence_intro_then_active(void) {
    GlrTourPresenceView v;

    glr_tour_presence_reset();
    v = glr_tour_presence_tick(0, NULL);
    ASSERT_INT("no tour: presence off", v.phase, GLR_TOUR_PRESENCE_OFF);
    ASSERT_FLOAT("no tour: border invisible", v.border_alpha, 0.0f);

    v = glr_tour_presence_tick(1, "Start Here");
    ASSERT_INT("tour starts: intro", v.phase, GLR_TOUR_PRESENCE_INTRO);
    ASSERT_STR("intro latches the name", v.name, "Start Here");
    /* Frame zero of the ease: the card has not arrived yet, which is what
     * makes it read as an animation rather than a popup. */
    ASSERT_FLOAT("card starts fully transparent", v.card_alpha, 0.0f);

    v = presence_frames(1, "Start Here", GLR_TOUR_PRESENCE_CARD_IN_FRAMES);
    ASSERT_TRUE("card is up once eased in", v.card_alpha > 0.9f);
    ASSERT_TRUE("border is up once eased in", v.border_alpha > 0.0f);
    ASSERT_INT("still intro mid-card", v.phase, GLR_TOUR_PRESENCE_INTRO);

    v = presence_frames(1, "Start Here", GLR_TOUR_PRESENCE_INTRO_FRAMES);
    ASSERT_INT("intro ends at ACTIVE", v.phase, GLR_TOUR_PRESENCE_ACTIVE);
    ASSERT_FLOAT("card gone in ACTIVE", v.card_alpha, 0.0f);
    ASSERT_TRUE("border persists in ACTIVE", v.border_alpha > 0.0f);
}

static void test_presence_breathes_and_stays_within_range(void) {
    float lo = 2.0f, hi = -1.0f;

    glr_tour_presence_reset();
    presence_frames(1, "Tour", GLR_TOUR_PRESENCE_INTRO_FRAMES + 1);
    /* One full breath cycle in ACTIVE: it must sweep a visible range (a border
     * pinned at one alpha reads as static chrome) without ever going opaque
     * enough to compete with the captions it frames. */
    for (int i = 0; i < GLR_TOUR_PRESENCE_BREATHE_FRAMES; i++) {
        GlrTourPresenceView v = glr_tour_presence_tick(1, "Tour");
        if (v.border_alpha < lo) lo = v.border_alpha;
        if (v.border_alpha > hi) hi = v.border_alpha;
    }
    ASSERT_TRUE("breath never dips below the floor",
                lo >= GLR_TOUR_PRESENCE_ALPHA_MIN - 0.001f);
    ASSERT_TRUE("breath never exceeds the ceiling",
                hi <= GLR_TOUR_PRESENCE_ALPHA_MAX + 0.001f);
    ASSERT_TRUE("breath actually moves", hi - lo > 0.2f);
}

static void test_presence_outro_outlives_the_tour(void) {
    GlrTourPresenceView v;
    float first_outro_alpha;

    glr_tour_presence_reset();
    presence_frames(1, "Camera & Views", GLR_TOUR_PRESENCE_INTRO_FRAMES + 5);

    /* The tour is gone and its metadata pointers with it - pass NULL, exactly
     * as glr_pointer_script_tour_view() reports it after a stop. */
    v = glr_tour_presence_tick(0, NULL);
    ASSERT_INT("stopping enters the outro", v.phase, GLR_TOUR_PRESENCE_OUTRO);
    ASSERT_STR("outro still knows the name", v.name, "Camera & Views");
    ASSERT_TRUE("outro still draws", v.border_alpha > 0.0f);
    first_outro_alpha = v.border_alpha;

    v = presence_frames(0, NULL, GLR_TOUR_PRESENCE_OUTRO_FRAMES / 2);
    ASSERT_TRUE("outro fades", v.border_alpha < first_outro_alpha);
    ASSERT_TRUE("outro collapses the band", v.band_scale < 1.0f);

    v = presence_frames(0, NULL, GLR_TOUR_PRESENCE_OUTRO_FRAMES);
    ASSERT_INT("outro ends at OFF", v.phase, GLR_TOUR_PRESENCE_OFF);
    ASSERT_FLOAT("nothing drawn once off", v.border_alpha, 0.0f);

    v = glr_tour_presence_tick(0, NULL);
    ASSERT_INT("stays off with no tour", v.phase, GLR_TOUR_PRESENCE_OFF);
}

static void test_presence_restarts_intro_for_a_new_tour(void) {
    GlrTourPresenceView v;

    glr_tour_presence_reset();
    presence_frames(1, "Editing Basics", GLR_TOUR_PRESENCE_INTRO_FRAMES + 5);
    ASSERT_INT("settled into ACTIVE",
               glr_tour_presence_tick(1, "Editing Basics").phase,
               GLR_TOUR_PRESENCE_ACTIVE);

    /* Starting a tour from the Tours menu during a tour never passes through
     * inactive, so the name change is the only edge available - and the card
     * has to replay, because the thing being announced changed. */
    v = glr_tour_presence_tick(1, "Getting Help");
    ASSERT_INT("new tour name replays the intro",
               v.phase, GLR_TOUR_PRESENCE_INTRO);
    ASSERT_STR("card announces the new tour", v.name, "Getting Help");
}

static void test_presence_reset_skips_the_outro(void) {
    GlrTourPresenceView v;

    glr_tour_presence_reset();
    presence_frames(1, "Tour", GLR_TOUR_PRESENCE_INTRO_FRAMES + 5);
    glr_tour_presence_reset();
    v = glr_tour_presence_tick(0, NULL);
    ASSERT_INT("reset drops straight to OFF", v.phase, GLR_TOUR_PRESENCE_OFF);
    ASSERT_FLOAT("reset leaves nothing drawn", v.border_alpha, 0.0f);
}

/* The compact strip is what a tour actually runs under (the HUD defaults
 * collapsed), so it - not just the expanded transport line - has to carry the
 * way out. */
static void test_hud_compact_strip_carries_the_exit_key(void) {
    ASSERT_TRUE("compact layout names Esc",
                strstr(TOUR_HUD_COMPACT_FMT, "Esc") != NULL);
    /* The chrome count drives the compact panel's width, so a drift between
     * the format string and the constant silently truncates the hint. */
    ASSERT_INT("chrome char count matches the format",
               TOUR_HUD_COMPACT_CHROME_CHARS,
               (int)strlen(TOUR_HUD_COMPACT_FMT) - 2);
}

int main(void) {
    printf("--- glr_tour_transport tests ---\n");
    test_baseline_pending_then_playing();
    test_pause_freezes_virtual_time();
    test_speed_ladder_and_persistence();
    test_hud_defaults_compact_and_survives_backstep();
    test_speed_affects_advance_rate();
    test_right_step_from_paused();
    test_right_while_playing_forces_complete_and_pauses();
    test_right_at_done_is_noop();
    test_space_restart_from_done();
    test_speed_persists_through_done_restart();
    test_timestamped_tour_rejected();
    test_comments_blanks_and_source_line();
    test_normal_ring_delays_done();
    test_stepped_ring_immediate_done_and_backstep();
    test_normal_click_waits_release();
    test_modified_clicks();
    test_controls_during_baseline_pending_noop();
    test_controls_during_seeking_noop();
    test_final_down_releases_button_at_done();
    test_escape_from_paused_post_down_releases();
    test_backstep_from_paused_post_down_releases();
    test_escape_stops_tour();
    test_backstep_from_done();
    test_backstep_between_events();
    test_backstep_in_flight_returns_to_start_boundary();
    test_seek_processes_at_most_32_per_frame();
    test_seek_scene_camera_ease_is_immediate();
    test_seek_suppresses_application_and_camera_ticks();
    test_backstep_reconstructs_repl_commit();
    test_backstep_reconstructs_camera_drag();
    test_right_drag_pans_camera_and_rewinds();
    test_drag_pointer_motion_ownership();
    test_tour_blocks_physical_motion_throughout();
    test_controller_arbitrates_physical_and_scripted_input();
    test_view_event_ensures_3d_and_reconstructs();
    test_hud_panel_width_clamp();
    test_echo_alpha_frozen_is_full();
    test_view_inactive_after_stop();
    test_presence_intro_then_active();
    test_presence_breathes_and_stays_within_range();
    test_presence_outro_outlives_the_tour();
    test_presence_restarts_intro_for_a_new_tour();
    test_presence_reset_skips_the_outro();
    test_hud_compact_strip_carries_the_exit_key();
    printf("%d / %d tests passed\n", g_harness.passed, g_harness.run);
    return g_harness.passed == g_harness.run ? 0 : 1;
}
