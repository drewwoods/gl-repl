/*
 * test_glr_perf_hint.c - pure watchdog tests, no GL.
 *
 * Driven entirely through glr_perf_hint_tick(fps, dt_us, &in) with helpers
 * that feed consistent FPS / interval pairs. Each case starts from
 * glr_perf_hint_reset_for_test() so the process-static ceiling and capture
 * latch cannot leak between tests.
 */
#include "app/glr_perf_hint.h"
#include "app/glr_config.h"
#include "render3d/render_types.h"
#include "support/test_harness.h"

#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, g, e)  TEST_ASSERT_INT(&g_harness, label, g, e)

static double dt_for_fps(double fps) {
    return 1000000.0 / fps;
}

static GlrPerfHintInputs in_clean(void) {
    GlrPerfHintInputs in;

    memset(&in, 0, sizeof in);
    return in;
}

static GlrPerfHintInputs in_accum(int passes, int effect) {
    GlrPerfHintInputs in = in_clean();

    in.use_accum = 1;
    in.accum_effect = effect;
    in.accum_passes = passes;
    return in;
}

static GlrPerfHintInputs in_post_fx(int scope) {
    GlrPerfHintInputs in = in_clean();

    in.post_fx_scope = scope;
    return in;
}

static GlrPerfHintInputs in_line_smooth(void) {
    GlrPerfHintInputs in = in_clean();

    in.line_smooth_enabled = 1;
    return in;
}

static void tick_for(double fps, const GlrPerfHintInputs *in, double duration_us) {
    double dt = dt_for_fps(fps);
    double acc = 0.0;

    while (acc < duration_us) {
        glr_perf_hint_tick(fps, dt, in);
        acc += dt;
    }
}

static void warmup_clean_at(double fps) {
    GlrPerfHintInputs in = in_clean();

    tick_for(fps, &in, GLR_PERF_HINT_WARMUP_US + dt_for_fps(fps));
}

static void test_empty_mask_never_trips(void) {
    GlrPerfHintInputs in = in_clean();
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    tick_for(5.0, &in, 10000000.0);
    v = glr_perf_hint_view();
    ASSERT_INT("empty mask stays inactive at 5 fps", v.active, 0);
    ASSERT_INT("empty mask culprit is none", v.culprit, GLR_PERF_CULPRIT_NONE);
}

static void test_first_profiler_tick_ignored(void) {
    GlrPerfHintInputs in = in_accum(8, RENDER3D_ACCUM_EFFECT_BLUR);
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    glr_perf_hint_tick(0.0, 0.0, &in);
    v = glr_perf_hint_view();
    ASSERT_INT("fps=0 dt=0 does not trip", v.active, 0);
    ASSERT_INT("fps=0 still refreshes the mask", v.culprit, GLR_PERF_CULPRIT_ACCUM);
}

static void test_warmup_blocks_trip_and_learns_ceiling(void) {
    GlrPerfHintInputs slow = in_accum(8, RENDER3D_ACCUM_EFFECT_AA);
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);
    /* Just inside a 2 s window of slow frames after a completed warm-up. */
    tick_for(20.0, &slow, GLR_PERF_HINT_TRIP_US - dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("does not trip before 2 s", v.active, 0);

    glr_perf_hint_reset_for_test();
    /* Slow from t=0 with a non-empty mask: warm-up swallows the first 3 s. */
    tick_for(20.0, &slow, GLR_PERF_HINT_WARMUP_US);
    v = glr_perf_hint_view();
    ASSERT_INT("nothing trips inside the first 3 s", v.active, 0);
}

static void test_trips_just_past_two_seconds(void) {
    GlrPerfHintInputs in = in_accum(8, RENDER3D_ACCUM_EFFECT_BLUR);
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);
    tick_for(20.0, &in, GLR_PERF_HINT_TRIP_US - dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("1.95 s of slow does not trip", v.active, 0);

    tick_for(20.0, &in, 2.0 * dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("just past 2 s of slow trips", v.active, 1);
    ASSERT_INT("tripped culprit is accum", v.culprit, GLR_PERF_CULPRIT_ACCUM);
    ASSERT_INT("rounded fps refreshed while up", v.fps, 20);
}

static void test_hysteresis_after_60_ceiling(void) {
    GlrPerfHintInputs in = in_accum(8, RENDER3D_ACCUM_EFFECT_BLUR);
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);
    tick_for(20.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("setup: tripped", v.active, 1);

    tick_for(52.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(52.0));
    v = glr_perf_hint_view();
    ASSERT_INT("52 fps keeps a tripped hint up", v.active, 1);

    tick_for(56.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(56.0));
    v = glr_perf_hint_view();
    ASSERT_INT("56 fps for 2 s clears it", v.active, 0);
}

static void test_display_relative_threshold(void) {
    GlrPerfHintInputs in = in_accum(8, RENDER3D_ACCUM_EFFECT_AA);
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    warmup_clean_at(30.0);
    tick_for(28.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(28.0));
    v = glr_perf_hint_view();
    ASSERT_INT("28 fps on a 30 fps ceiling does not trip", v.active, 0);

    tick_for(20.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("20 fps on a 30 fps ceiling trips", v.active, 1);
}

static void test_ceiling_admission(void) {
    GlrPerfHintInputs slow = in_accum(16, RENDER3D_ACCUM_EFFECT_BLUR);
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    /* Non-empty from the first frame: cannot establish a ceiling, so the
     * absolute 50/55 floors apply. 35 fps is below 50. */
    tick_for(35.0, &slow, GLR_PERF_HINT_WARMUP_US + GLR_PERF_HINT_TRIP_US +
                          dt_for_fps(35.0));
    v = glr_perf_hint_view();
    ASSERT_INT("loaded 35 fps Accum trips against the absolute floor",
               v.active, 1);

    /* Non-empty 35 fps frames must not have parked 35 as the ceiling:
     * trip would then be 28, and 29 fps would stay silent. Against the
     * absolute floor 29 fps still trips. */
    glr_perf_hint_reset();
    tick_for(29.0, &slow, GLR_PERF_HINT_WARMUP_US + GLR_PERF_HINT_TRIP_US +
                          10.0 * dt_for_fps(29.0));
    v = glr_perf_hint_view();
    ASSERT_INT("35 fps non-empty frames did not become the ceiling",
               v.active, 1);
}

static void test_nonempty_cannot_raise_ceiling(void) {
    GlrPerfHintInputs slow = in_accum(8, RENDER3D_ACCUM_EFFECT_AA);
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    warmup_clean_at(30.0);
    /* 80 fps with the mask on must not raise the 30 fps ceiling (trip=24). */
    tick_for(80.0, &slow, 1000000.0);
    tick_for(28.0, &slow, GLR_PERF_HINT_TRIP_US + dt_for_fps(28.0));
    v = glr_perf_hint_view();
    ASSERT_INT("non-empty 80 fps frames cannot raise a 30 fps ceiling",
               v.active, 0);
}

static void test_ceiling_retention_across_reset(void) {
    GlrPerfHintInputs in = in_accum(8, RENDER3D_ACCUM_EFFECT_BLUR);
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);
    tick_for(20.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    ASSERT_INT("setup: tripped before reset", glr_perf_hint_view().active, 1);

    glr_perf_hint_reset();
    warmup_clean_at(60.0);  /* restart warm-up; ceiling must stay 60 */
    /* 49 < 50 (absolute) would trip if the ceiling were lost; 49 > 48
     * (60 * 0.80) must not trip if it was kept. */
    tick_for(49.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(49.0));
    v = glr_perf_hint_view();
    ASSERT_INT("reset keeps the 60 fps ceiling (49 fps does not trip)",
               v.active, 0);
}

static void test_ceiling_retention_next_culprit(void) {
    GlrPerfHintInputs both;
    GlrPerfHintInputs post;
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);

    both = in_accum(8, RENDER3D_ACCUM_EFFECT_BLUR);
    both.post_fx_scope = GLR_POST_FX_SCOPE_FRAME;
    tick_for(20.0, &both, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("setup: accum is the heaviest of two", v.culprit,
               GLR_PERF_CULPRIT_ACCUM);
    ASSERT_INT("setup: culprit_count is 2", v.culprit_count, 2);

    /* [off] drops accum, reset restarts debounce, ceiling stays. */
    glr_perf_hint_reset();
    post = in_post_fx(GLR_POST_FX_SCOPE_FRAME);
    warmup_clean_at(60.0);
    tick_for(49.0, &post, GLR_PERF_HINT_TRIP_US + dt_for_fps(49.0));
    v = glr_perf_hint_view();
    ASSERT_INT("[off] then next culprit keeps the 60 fps ceiling",
               v.active, 0);
}

static void test_discontinuity(void) {
    GlrPerfHintInputs in = in_accum(8, RENDER3D_ACCUM_EFFECT_AA);
    GlrPerfHintInputs clean = in_clean();
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    /* A 5 s gap must not establish a 60 fps ceiling. 48 fps against a
     * learned 60 fps ceiling stays silent (`fps < 48` is false); against
     * the absolute floor (`fps < 50`) it trips. Extra frames absorb
     * 1e6/48 rounding so the 2 s window is actually crossed. */
    glr_perf_hint_tick(60.0, 5000000.0, &clean);
    tick_for(48.0, &in, GLR_PERF_HINT_WARMUP_US + GLR_PERF_HINT_TRIP_US +
                        10.0 * dt_for_fps(48.0));
    v = glr_perf_hint_view();
    ASSERT_INT("5 s dt_us does not count as a clean ceiling sample",
               v.active, 1);

    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);
    tick_for(20.0, &in, 1500000.0);
    glr_perf_hint_tick(20.0, 5000000.0, &in);
    v = glr_perf_hint_view();
    ASSERT_INT("a 5 s gap does not trip", v.active, 0);
    /* Accumulator was zeroed: another 1.5 s of slow is still under 2 s. */
    tick_for(20.0, &in, 1500000.0);
    v = glr_perf_hint_view();
    ASSERT_INT("gap zeros the trip accumulator", v.active, 0);
}

static void test_very_slow_but_valid(void) {
    GlrPerfHintInputs in = in_accum(8, RENDER3D_ACCUM_EFFECT_BLUR);
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);
    tick_for(5.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(5.0));
    v = glr_perf_hint_view();
    ASSERT_INT("5 fps / 200 ms ticks trip after about 2 s", v.active, 1);
}

static void test_hitch_recovery(void) {
    GlrPerfHintInputs in = in_accum(8, RENDER3D_ACCUM_EFFECT_AA);
    GlrPerfHintView v;
    int i;

    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);
    /* 1.7 s of slow intervals with a low EMA. */
    for (i = 0; i < 68; i++)
        glr_perf_hint_tick(40.0, 25000.0, &in);
    v = glr_perf_hint_view();
    ASSERT_INT("1.7 s of slow has not tripped yet", v.active, 0);

    /* Recovered 16.7 ms intervals, EMA still reporting 40. Instantaneous
     * cadence is ~60, so the trip accumulator must reset. */
    for (i = 0; i < 180; i++)
        glr_perf_hint_tick(40.0, 16667.0, &in);
    v = glr_perf_hint_view();
    ASSERT_INT("recovered intervals never trip while EMA lags", v.active, 0);
}

static void test_accum_gates(void) {
    GlrPerfHintInputs in;
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);

    in = in_accum(8, RENDER3D_ACCUM_EFFECT_AA);
    in.use_accum = 0;
    tick_for(20.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("use_accum==0 is not blamed", v.active, 0);
    ASSERT_INT("use_accum==0 culprit is none", v.culprit, GLR_PERF_CULPRIT_NONE);

    in = in_accum(1, RENDER3D_ACCUM_EFFECT_AA);
    tick_for(20.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("accum_passes==1 is not blamed", v.active, 0);

    in = in_accum(8, RENDER3D_ACCUM_EFFECT_OFF);
    tick_for(20.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("accum_effect==OFF is not blamed", v.active, 0);
}

static void test_post_fx_and_ranking(void) {
    GlrPerfHintInputs in;
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);

    in = in_post_fx(GLR_POST_FX_SCOPE_OFF);
    tick_for(20.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    ASSERT_INT("Post FX Off is not blamed", glr_perf_hint_view().active, 0);

    in = in_post_fx(GLR_POST_FX_SCOPE_VIEW_3D);
    tick_for(20.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("VIEW_3D is a culprit", v.culprit, GLR_PERF_CULPRIT_POST_FX_VIEW);

    glr_perf_hint_reset();
    warmup_clean_at(60.0);
    in = in_post_fx(GLR_POST_FX_SCOPE_FRAME);
    tick_for(20.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("FRAME is a distinct culprit", v.culprit,
               GLR_PERF_CULPRIT_POST_FX_FRAME);

    glr_perf_hint_reset();
    warmup_clean_at(60.0);
    in = in_accum(8, RENDER3D_ACCUM_EFFECT_BLUR);
    in.post_fx_scope = GLR_POST_FX_SCOPE_FRAME;
    tick_for(20.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("accum outranks Post FX Frame", v.culprit, GLR_PERF_CULPRIT_ACCUM);
    ASSERT_INT("two settings on -> culprit_count 2", v.culprit_count, 2);

    glr_perf_hint_reset();
    warmup_clean_at(60.0);
    in = in_post_fx(GLR_POST_FX_SCOPE_FRAME);
    in.line_smooth_enabled = 1;
    tick_for(20.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("FRAME outranks line smooth", v.culprit,
               GLR_PERF_CULPRIT_POST_FX_FRAME);
    ASSERT_INT("frame+line-smooth count is 2", v.culprit_count, 2);
}

static void test_line_smooth_culprit(void) {
    GlrPerfHintInputs in = in_line_smooth();
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);
    tick_for(20.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("line smooth trips", v.active, 1);
    ASSERT_INT("line smooth culprit", v.culprit, GLR_PERF_CULPRIT_LINE_SMOOTH);
}

static void test_dismiss_and_rearm(void) {
    GlrPerfHintInputs accum = in_accum(8, RENDER3D_ACCUM_EFFECT_AA);
    GlrPerfHintInputs both;
    GlrPerfHintInputs steeper;
    GlrPerfHintInputs clean = in_clean();
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);
    tick_for(20.0, &accum, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    ASSERT_INT("setup: active before dismiss", glr_perf_hint_view().active, 1);

    glr_perf_hint_dismiss();
    v = glr_perf_hint_view();
    ASSERT_INT("dismiss hides it", v.active, 0);

    tick_for(20.0, &accum, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("same configuration stays hidden while still slow", v.active, 0);

    both = accum;
    both.line_smooth_enabled = 1;
    tick_for(20.0, &both, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("adding another culprit re-arms", v.active, 1);

    glr_perf_hint_dismiss();
    glr_perf_hint_reset();
    warmup_clean_at(60.0);
    tick_for(20.0, &both, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("reset re-arms a dismissed configuration", v.active, 1);

    /* Changing the blamed value (8x -> 16x) is a new configuration. */
    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);
    tick_for(20.0, &accum, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    glr_perf_hint_dismiss();
    steeper = in_accum(16, RENDER3D_ACCUM_EFFECT_AA);
    tick_for(20.0, &steeper, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("changing accum passes re-arms", v.active, 1);

    /* Off then on again: empty mask expires dismiss. */
    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);
    tick_for(20.0, &accum, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    glr_perf_hint_dismiss();
    glr_perf_hint_tick(20.0, dt_for_fps(20.0), &clean);
    tick_for(20.0, &accum, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("turning the setting off then on re-arms", v.active, 1);

    /* Recovered FPS expires dismiss so a later slowdown can warn again. */
    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);
    tick_for(20.0, &accum, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    glr_perf_hint_dismiss();
    tick_for(52.0, &accum, GLR_PERF_HINT_TRIP_US + dt_for_fps(52.0));
    v = glr_perf_hint_view();
    ASSERT_INT("52 fps does not expire dismiss (below release)", v.active, 0);
    tick_for(56.0, &accum, GLR_PERF_HINT_TRIP_US + 10.0 * dt_for_fps(56.0));
    v = glr_perf_hint_view();
    ASSERT_INT("recovered FPS expires dismiss without showing the hint",
               v.active, 0);
    tick_for(20.0, &accum, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("a later slowdown after recovery warns again", v.active, 1);
}

static void test_empty_mask_clears_immediately(void) {
    GlrPerfHintInputs accum = in_accum(8, RENDER3D_ACCUM_EFFECT_BLUR);
    GlrPerfHintInputs clean = in_clean();
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);
    tick_for(20.0, &accum, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    ASSERT_INT("setup: active", glr_perf_hint_view().active, 1);

    glr_perf_hint_tick(20.0, dt_for_fps(20.0), &clean);
    v = glr_perf_hint_view();
    ASSERT_INT("empty mask clears active in that tick", v.active, 0);
    ASSERT_INT("empty mask culprit is none", v.culprit, GLR_PERF_CULPRIT_NONE);

    /* Debounce was zeroed: going slow again needs a full 2 s. */
    tick_for(20.0, &accum, GLR_PERF_HINT_TRIP_US - dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("empty-mask clear also zeros the trip accumulator", v.active, 0);
}

static void test_pointer_script_suppression(void) {
    GlrPerfHintInputs in = in_accum(8, RENDER3D_ACCUM_EFFECT_BLUR);
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    warmup_clean_at(60.0);
    in.pointer_script_active = 1;
    tick_for(20.0, &in, GLR_PERF_HINT_WARMUP_US + GLR_PERF_HINT_TRIP_US +
                        dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("pointer_script_active forces inactive", v.active, 0);

    /* Ending suppression starts a fresh warm-up; accrued slow frames
     * cannot make the hint appear on the next tick. */
    in.pointer_script_active = 0;
    glr_perf_hint_tick(20.0, dt_for_fps(20.0), &in);
    v = glr_perf_hint_view();
    ASSERT_INT("first unsuppressed tick after a script is still inactive",
               v.active, 0);
    tick_for(20.0, &in, GLR_PERF_HINT_WARMUP_US);
    v = glr_perf_hint_view();
    ASSERT_INT("warm-up after a script still blocks a trip", v.active, 0);
}

static void test_capture_session_latch(void) {
    GlrPerfHintInputs in = in_accum(8, RENDER3D_ACCUM_EFFECT_BLUR);
    GlrPerfHintView v;

    glr_perf_hint_reset_for_test();
    glr_perf_hint_set_capture_session(1);
    warmup_clean_at(60.0);
    /* pointer_script_active stays 0 - that is the regression: a still
     * capture with no pointer script must remain suppressed. */
    in.pointer_script_active = 0;
    tick_for(20.0, &in, GLR_PERF_HINT_WARMUP_US + GLR_PERF_HINT_TRIP_US +
                        5000000.0);
    v = glr_perf_hint_view();
    ASSERT_INT("capture-session latch stays inactive with script=0",
               v.active, 0);

    glr_perf_hint_reset();
    warmup_clean_at(60.0);
    tick_for(20.0, &in, GLR_PERF_HINT_TRIP_US + dt_for_fps(20.0));
    v = glr_perf_hint_view();
    ASSERT_INT("reset does not clear the capture-session latch", v.active, 0);
}

int main(void) {
    test_empty_mask_never_trips();
    test_first_profiler_tick_ignored();
    test_warmup_blocks_trip_and_learns_ceiling();
    test_trips_just_past_two_seconds();
    test_hysteresis_after_60_ceiling();
    test_display_relative_threshold();
    test_ceiling_admission();
    test_nonempty_cannot_raise_ceiling();
    test_ceiling_retention_across_reset();
    test_ceiling_retention_next_culprit();
    test_discontinuity();
    test_very_slow_but_valid();
    test_hitch_recovery();
    test_accum_gates();
    test_post_fx_and_ranking();
    test_line_smooth_culprit();
    test_dismiss_and_rearm();
    test_empty_mask_clears_immediately();
    test_pointer_script_suppression();
    test_capture_session_latch();

    return test_harness_report(&g_harness, "glr_perf_hint");
}
