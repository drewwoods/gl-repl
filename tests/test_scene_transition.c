/*
 * test_scene_transition.c - pure grid/axes fade state machine.
 * Maps to plans/active/grid-axes-transitions.md rules.
 */
#include "scene/scene_transition.h"
#include "support/test_harness.h"
#include <stdio.h>

static TestHarness g_h = TEST_HARNESS_INIT;
#define AT(label, cond) TEST_ASSERT_TRUE(&g_h, label, cond)
#define AF(label, g, e) do { float _d=(g)-(e); \
    TEST_ASSERT_TRUE(&g_h, label, (_d<0?-_d:_d) < 1e-4f); } while (0)

int main(void) {
    SceneXnState s;

    /* rule 8: init = snap, fully shown, no animation. */
    scene_xn_init(&s, 8);
    AT("init theme", s.current == 8);
    AT("init STEADY", s.phase == SCENE_XN_STEADY);
    AF("init opacity 1", s.opacity, 1.0f);

    /* rule 5: same-value set while STEADY = no-op. */
    scene_xn_set(&s, 8);
    AT("same-value no-op STEADY", s.phase == SCENE_XN_STEADY);
    AF("same-value opacity unchanged", s.opacity, 1.0f);

    /* different theme -> FADE_OUT; current stays until opacity 0. */
    scene_xn_set(&s, 3);
    AT("set diff -> FADE_OUT", s.phase == SCENE_XN_FADE_OUT);
    AT("current still old during OUT", s.current == 8);
    scene_xn_tick(&s, 0.10f, 0.30f, 0.20f);   /* 0.20s out: ->0.5 */
    AF("out half", s.opacity, 0.5f);
    AT("current still old mid-OUT", s.current == 8);

    /* rule 2: rapid cycle skips ephemeral themes (3 never drawn). */
    scene_xn_set(&s, 5);
    AT("retarget keeps FADE_OUT", s.phase == SCENE_XN_FADE_OUT);
    scene_xn_tick(&s, 0.20f, 0.30f, 0.20f);   /* reaches 0 -> adopt 5 */
    AT("adopt latest at 0 (skip 3)", s.current == 5);
    AT("now FADE_IN", s.phase == SCENE_XN_FADE_IN);
    AF("opacity 0 at crossing", s.opacity, 0.0f);
    scene_xn_tick(&s, 0.30f, 0.30f, 0.20f);   /* 0.30s in -> 1.0 */
    AF("fade-in complete", s.opacity, 1.0f);
    AT("STEADY after fade-in", s.phase == SCENE_XN_STEADY);

    /* F1: return to current mid-FADE_OUT reverses (free). */
    scene_xn_set(&s, 7);
    scene_xn_tick(&s, 0.10f, 0.30f, 0.20f);   /* OUT -> 0.5 */
    AT("FADE_OUT before reverse", s.phase == SCENE_XN_FADE_OUT);
    scene_xn_set(&s, 5);                       /* back to current */
    AT("reverse -> FADE_IN", s.phase == SCENE_XN_FADE_IN);
    AT("current unchanged on reverse", s.current == 5);
    AF("reverse resumes from current opacity", s.opacity, 0.5f);
    scene_xn_tick(&s, 0.15f, 0.30f, 0.20f);   /* 0.15/0.30 -> +0.5 =1 */
    AF("reverse completes", s.opacity, 1.0f);
    AT("STEADY after reverse", s.phase == SCENE_XN_STEADY);

    /* rule 6: off needs no special case (off=0; OUT-of-off visually
     * instant since nothing drawn, machine still sequences). */
    scene_xn_init(&s, 0);                      /* seeded to off */
    scene_xn_set(&s, 4);
    AT("off->theme FADE_OUT first", s.phase == SCENE_XN_FADE_OUT);
    scene_xn_tick(&s, 1.0f, 0.30f, 0.20f);
    AT("adopts theme", s.current == 4);
    AT("FADE_IN of theme", s.phase == SCENE_XN_FADE_IN);

    return test_harness_report(&g_h, "scene_transition");
}
