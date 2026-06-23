/*
 * test_scene_transition.c - pure grid/axes fade state machine.
 * Maps to docs/plans/active/grid-axes-transitions.md rules.
 */
#include "render3d/render3d_transition.h"
#include "support/test_harness.h"
#include <stdio.h>

static TestHarness g_h = TEST_HARNESS_INIT;
#define AT(label, cond) TEST_ASSERT_TRUE(&g_h, label, cond)
#define AF(label, g, e) do { float _d=(g)-(e); \
    TEST_ASSERT_TRUE(&g_h, label, (_d<0?-_d:_d) < 1e-4f); } while (0)

/* Stand-in for an overlay's reveal curve: a plain linear ramp, in=0.30s /
 * out=0.20s, no per-theme speed. The machine owns `elapsed`; this maps it to
 * opacity and inverts it for reversal continuity. */
#define TEST_IN  0.30f
#define TEST_OUT 0.20f
static float test_xn_opacity(int theme, Render3dXnPhase phase, float elapsed) {
    (void)theme;
    if (phase == RENDER3D_XN_STEADY) return 1.0f;
    float dur = (phase == RENDER3D_XN_FADE_IN) ? TEST_IN : TEST_OUT;
    float p = (dur > 0.0f) ? elapsed / dur : 1.0f;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return (phase == RENDER3D_XN_FADE_IN) ? p : 1.0f - p;
}
static float test_xn_elapsed_at(int theme, Render3dXnPhase phase, float opacity) {
    (void)theme;
    float dur = (phase == RENDER3D_XN_FADE_IN) ? TEST_IN : TEST_OUT;
    float p = (phase == RENDER3D_XN_FADE_IN) ? opacity : 1.0f - opacity;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return p * dur;
}
static const Render3dXnReveal test_reveal = { test_xn_opacity, test_xn_elapsed_at };

int main(void) {
    Render3dXnState s;

    /* rule 8: init = snap, fully shown, no animation. */
    render3d_xn_init(&s, 8, &test_reveal);
    AT("init theme", s.current == 8);
    AT("init STEADY", s.phase == RENDER3D_XN_STEADY);
    AF("init opacity 1", render3d_xn_opacity(&s), 1.0f);

    /* rule 5: same-value set while STEADY = no-op. */
    render3d_xn_set(&s, 8);
    AT("same-value no-op STEADY", s.phase == RENDER3D_XN_STEADY);
    AF("same-value opacity unchanged", render3d_xn_opacity(&s), 1.0f);

    /* different theme -> FADE_OUT; current stays until opacity 0. */
    render3d_xn_set(&s, 3);
    AT("set diff -> FADE_OUT", s.phase == RENDER3D_XN_FADE_OUT);
    AT("current still old during OUT", s.current == 8);
    render3d_xn_tick(&s, 0.10f);                  /* 0.10/0.20 out -> 0.5 */
    AF("out half", render3d_xn_opacity(&s), 0.5f);
    AT("current still old mid-OUT", s.current == 8);

    /* rule 2: rapid cycle skips ephemeral themes (3 never drawn). */
    render3d_xn_set(&s, 5);
    AT("retarget keeps FADE_OUT", s.phase == RENDER3D_XN_FADE_OUT);
    render3d_xn_tick(&s, 0.20f);                  /* clock 0.30 -> opacity 0 -> adopt 5 */
    AT("adopt latest at 0 (skip 3)", s.current == 5);
    AT("now FADE_IN", s.phase == RENDER3D_XN_FADE_IN);
    AF("opacity 0 at crossing", render3d_xn_opacity(&s), 0.0f);
    render3d_xn_tick(&s, 0.30f);                  /* 0.30s in -> 1.0 */
    AF("fade-in complete", render3d_xn_opacity(&s), 1.0f);
    AT("STEADY after fade-in", s.phase == RENDER3D_XN_STEADY);

    /* F1: return to current mid-FADE_OUT reverses, opacity-continuous. */
    render3d_xn_set(&s, 7);
    render3d_xn_tick(&s, 0.10f);                  /* OUT -> 0.5 */
    AT("FADE_OUT before reverse", s.phase == RENDER3D_XN_FADE_OUT);
    render3d_xn_set(&s, 5);                       /* back to current */
    AT("reverse -> FADE_IN", s.phase == RENDER3D_XN_FADE_IN);
    AT("current unchanged on reverse", s.current == 5);
    AF("reverse resumes from current opacity", render3d_xn_opacity(&s), 0.5f);
    render3d_xn_tick(&s, 0.15f);                  /* 0.15/0.30 -> +0.5 = 1 */
    AF("reverse completes", render3d_xn_opacity(&s), 1.0f);
    AT("STEADY after reverse", s.phase == RENDER3D_XN_STEADY);

    /* rule 6: show-from-off skips the dead OUT. The controller calls
     * render3d_xn_show (not render3d_xn_set) when current == off index, so the new
     * theme fades IN from 0 with NO preceding FADE_OUT. */
    render3d_xn_init(&s, 0, &test_reveal);        /* seeded to off */
    render3d_xn_show(&s, 4);
    AT("show: current jumps to theme", s.current == 4);
    AT("show: next jumps to theme", s.next == 4);
    AT("show: FADE_IN immediately (no OUT)", s.phase == RENDER3D_XN_FADE_IN);
    AF("show: opacity reset to 0", render3d_xn_opacity(&s), 0.0f);
    render3d_xn_tick(&s, 0.15f);                  /* 0.15/0.30 -> 0.5 */
    AF("show: fades in from 0", render3d_xn_opacity(&s), 0.5f);
    AT("show: still FADE_IN mid-ramp", s.phase == RENDER3D_XN_FADE_IN);
    render3d_xn_tick(&s, 0.15f);                  /* -> 1.0 */
    AF("show: fade-in completes", render3d_xn_opacity(&s), 1.0f);
    AT("show: STEADY after fade-in", s.phase == RENDER3D_XN_STEADY);

    /* render3d_xn_show reverse-safety: a normal set back to current while
     * mid show-fade-in is still a no-op (rule 5 / F1). */
    render3d_xn_show(&s, 6);
    render3d_xn_set(&s, 6);
    AT("set==current during show FADE_IN no-op",
       s.phase == RENDER3D_XN_FADE_IN);

    return test_harness_report(&g_h, "render3d_transition");
}
