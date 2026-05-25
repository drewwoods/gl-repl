/*
 * scene_transition.h - Pure show/hide transition machine for scene overlays.
 *
 * The controller keeps one of these per fading overlay such as grid or axes.
 * It owns the requested theme, calls scene_xn_set()/scene_xn_show() when that
 * request changes, and advances the machine with scene_xn_tick(). Renderers
 * then consume {current, opacity, phase} to decide what to draw.
 *
 * The machine is intentionally GL-free and history-light: fade-out keeps
 * `current` authoritative until opacity reaches zero, so rapid cycling drops
 * transient themes and reversing back to the current theme just flips the
 * direction to FADE_IN.
 *
 * API verb cheat-sheet:
 *   scene_xn_init  — snap to theme fully shown, no animation
 *                    (program init, world reset).
 *   scene_xn_set   — request a theme change; reads `current` to decide
 *                    whether to FADE_OUT, reverse to FADE_IN, or no-op.
 *   scene_xn_show  — fade in from invisible (FADE_IN at opacity 0),
 *                    skipping a pointless FADE_OUT of an already-off
 *                    overlay.
 *   scene_xn_tick  — advance the machine by dt seconds; sole driver of
 *                    `opacity` and `phase` changes.
 */
#ifndef SCENE_TRANSITION_H
#define SCENE_TRANSITION_H

typedef enum {
    SCENE_XN_STEADY = 0,   /* no ramp; opacity stable (1 shown / 0 hidden) */
    SCENE_XN_FADE_IN,      /* opacity -> 1 at fade_in_secs  */
    SCENE_XN_FADE_OUT      /* opacity -> 0 at fade_out_secs */
} SceneXnPhase;

typedef struct {
    int          current;  /* theme being drawn / fading (authoritative) */
    int          next;     /* latest requested theme                     */
    SceneXnPhase phase;
    float        opacity;  /* 0..1 */
} SceneXnState;

/* Seed/snap to `theme` fully shown, no animation. Use at program init
 * and on world reset (a zero-init machine would animate the default
 * theme in on frame 1). */
void scene_xn_init(SceneXnState *s, int theme);

/* Request `theme`. Rules: ==current while STEADY/FADE_IN -> no-op;
 * ==current while FADE_OUT -> FADE_IN (reverse); !=current -> FADE_OUT. */
void scene_xn_set(SceneXnState *s, int theme);

/* Show `theme` directly with no preceding FADE_OUT: current/next jump
 * to `theme`, opacity resets to 0, phase = FADE_IN. The machine is
 * theme-index-agnostic; the controller calls this (instead of
 * scene_xn_set) when the current overlay is the "off" index, so
 * show-from-off skips the pointless OUT of an already-invisible
 * overlay. */
void scene_xn_show(SceneXnState *s, int theme);

/* Advance by dt seconds. in_secs/out_secs are the fade durations
 * (<=0 => instant). At FADE_OUT reaching 0, `current` adopts `next`
 * and FADE_IN begins (or STEADY if unchanged). */
void scene_xn_tick(SceneXnState *s, float dt, float in_secs,
                   float out_secs);

#endif /* SCENE_TRANSITION_H */
